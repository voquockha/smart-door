#include "mqtt_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509v3.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace {
constexpr int kMqttKeepAliveSeconds = 120;
constexpr int kReconnectDelaySeconds = 3;
constexpr size_t kCommandCacheLimit = 100;

void setSocketTimeout(int fd, int seconds)
{
    struct timeval timeout;
    timeout.tv_sec = seconds;
    timeout.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

void appendUint16(std::string* out, unsigned int value)
{
    out->push_back((char)((value >> 8) & 0xff));
    out->push_back((char)(value & 0xff));
}

void appendMqttString(std::string* out, const std::string& value)
{
    appendUint16(out, (unsigned int)value.size());
    out->append(value);
}

std::string encodeRemainingLength(size_t len)
{
    std::string out;
    do {
        unsigned char encoded = (unsigned char)(len % 128);
        len /= 128;
        if (len > 0)
            encoded |= 128;
        out.push_back((char)encoded);
    } while (len > 0);
    return out;
}

std::string makeFixedHeader(unsigned char packet_type, size_t remaining_len)
{
    std::string out;
    out.push_back((char)packet_type);
    out += encodeRemainingLength(remaining_len);
    return out;
}

std::string makeConnectPacket(const std::string& client_id,
                              const std::string& username,
                              const std::string& password,
                              bool clean_session)
{
    std::string variable;
    appendMqttString(&variable, "MQTT");
    variable.push_back(4);  // MQTT 3.1.1

    unsigned char flags = clean_session ? 0x02 : 0x00;
    if (!password.empty())
        flags |= 0x40;
    if (!username.empty())
        flags |= 0x80;
    variable.push_back((char)flags);
    appendUint16(&variable, kMqttKeepAliveSeconds);

    std::string payload;
    appendMqttString(&payload, client_id);
    if (!username.empty())
        appendMqttString(&payload, username);
    if (!password.empty())
        appendMqttString(&payload, password);

    return makeFixedHeader(0x10, variable.size() + payload.size()) +
           variable + payload;
}

std::string makeSubscribePacket(const std::string& topic,
                                unsigned short packet_id)
{
    std::string variable;
    appendUint16(&variable, packet_id);

    std::string payload;
    appendMqttString(&payload, topic);
    payload.push_back(0x01);  // QoS 1

    return makeFixedHeader(0x82, variable.size() + payload.size()) +
           variable + payload;
}

std::string makePublishPacket(const std::string& topic,
                              const std::string& payload,
                              unsigned short packet_id,
                              bool duplicate)
{
    std::string variable;
    appendMqttString(&variable, topic);
    appendUint16(&variable, packet_id);
    const unsigned char header = duplicate ? 0x3a : 0x32;  // QoS 1
    return makeFixedHeader(header, variable.size() + payload.size()) +
           variable + payload;
}

std::string makePubAckPacket(unsigned short packet_id)
{
    std::string packet("\x40\x02", 2);
    appendUint16(&packet, packet_id);
    return packet;
}

unsigned short readUint16(const std::string& value, size_t pos)
{
    return (unsigned short)(((unsigned char)value[pos] << 8) |
                            (unsigned char)value[pos + 1]);
}

std::string trimTrailingSlash(std::string value)
{
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}
}  // namespace

MqttClient::MqttClient()
    : enabled_(getEnvBool("MQTT_ENABLED", true)),
      debug_payload_(getEnvBool("MQTT_DEBUG_PAYLOAD", false)),
      broker_host_(getEnvOrDefault("MQTT_HOST", "mqtt.x68.space")),
      broker_port_(getEnvInt("MQTT_PORT", 8883)),
      tls_enabled_(getEnvBool("MQTT_TLS_ENABLED", true)),
      verify_server_(getEnvBool("MQTT_TLS_VERIFY_SERVER", false)),
      ca_file_(getEnvOrDefault("MQTT_CA_FILE", "")),
      bootstrap_username_(getEnvOrDefault("MQTT_BOOTSTRAP_USERNAME",
                                           "x68-bootstrap")),
      bootstrap_password_(getEnvOrDefault(
          "MQTT_BOOTSTRAP_PASSWORD",
          "AuVrJ2wAzPOby88HBa+Yn7zzrKcJfuK5nOeVeGLg3p3OhEs7/5s9m68HvjwPndTA")),
      mac_(getEnvOrDefault("MQTT_MAC", getEth0Mac())),
      normalized_mac_(normalizeMac(mac_)),
      serial_number_(getEnvOrDefault("MQTT_SERIAL_NUMBER", "X680001")),
      device_model_(getEnvOrDefault("MQTT_DEVICE_MODEL", "x68-lite")),
      software_version_(getEnvOrDefault("MQTT_SOFTWARE_VERSION",
                                        getEnvOrDefault("MQTT_FIRMWARE_VERSION",
                                                        "1.0.0"))),
      status_interval_seconds_(
          std::max(0, getEnvInt("MQTT_STATUS_INTERVAL_SECONDS", 60))),
      retry_interval_seconds_(
          std::max(5, getEnvInt("MQTT_RETRY_INTERVAL_SECONDS", 30))),
      credential_path_(getEnvOrDefault("MQTT_CREDENTIAL_PATH",
                                       "/data/device/credential.json")),
      credential_fallback_path_(getEnvOrDefault(
          "MQTT_CREDENTIAL_FALLBACK_PATH",
          "/root/x68-device/credential.json")),
      installation_id_path_(getEnvOrDefault(
          "MQTT_INSTALLATION_ID_PATH", "/data/device/installation_id")),
      installation_id_(loadOrCreateInstallationId(
          getEnvOrDefault("MQTT_INSTALLATION_ID", ""),
          installation_id_path_)),
      offline_queue_path_(getEnvOrDefault(
          "MQTT_OFFLINE_QUEUE_PATH", "/data/device/pending_events.jsonl")),
      command_cache_path_(getEnvOrDefault(
          "MQTT_COMMAND_CACHE_PATH", "/data/device/processed_commands.jsonl")),
      command_cache_fallback_path_(getEnvOrDefault(
          "MQTT_COMMAND_CACHE_FALLBACK_PATH",
          "/root/x68-device/processed_commands.jsonl")),
      active_client_id_(installation_id_),
      active_username_(bootstrap_username_),
      active_password_(bootstrap_password_),
      bootstrap_request_topic_("x68-lite/v1/bootstrap/" + installation_id_ +
                               "/request"),
      bootstrap_response_topic_("x68-lite/v1/bootstrap/" + installation_id_ +
                                "/response"),
      bootstrap_ack_topic_("x68-lite/v1/bootstrap/" + installation_id_ +
                           "/ack"),
      activation_uuid_(generateUuid()),
      next_activation_retry_(std::chrono::steady_clock::now()),
      next_status_publish_(std::chrono::steady_clock::now()),
      next_event_retry_(std::chrono::steady_clock::now()),
      running_(false),
      connected_(false),
      next_packet_id_(1),
      socket_fd_(-1),
      ssl_ctx_(nullptr),
      ssl_(nullptr),
      mode_(Mode::BOOTSTRAP)
{
    signal(SIGPIPE, SIG_IGN);
    loadCommandCache();
    if (loadCredentialFile(credential_path_, &credential_)) {
        switchToProduction(credential_);
    } else if (!credential_fallback_path_.empty() &&
               credential_fallback_path_ != credential_path_ &&
               loadCredentialFile(credential_fallback_path_, &credential_)) {
        printf("[mqtt] loaded credential fallback=%s\n",
               credential_fallback_path_.c_str());
        switchToProduction(credential_);
    }

    printf("[mqtt] init enabled=%s broker=%s:%d tls=%s verify_server=%s mode=%s client_id=%s credential=%s fallback=%s\n",
           enabled_ ? "true" : "false", broker_host_.c_str(), broker_port_,
           tls_enabled_ ? "true" : "false",
           verify_server_ ? "true" : "false",
           mode_.load() == Mode::PRODUCTION ? "production" : "bootstrap",
           active_client_id_.c_str(), credential_path_.c_str(),
           credential_fallback_path_.c_str());
}

MqttClient::~MqttClient()
{
    stop();
}

bool MqttClient::isEnabled() const
{
    return enabled_;
}

bool MqttClient::start()
{
    if (!enabled_)
        return false;
    if (running_)
        return true;
    running_ = true;
    worker_ = std::thread(&MqttClient::loop, this);
    return true;
}

void MqttClient::stop()
{
    if (!running_ && !worker_.joinable())
        return;
    running_ = false;
    if (worker_.joinable())
        worker_.join();
    disconnectBroker();
}

void MqttClient::setRegisterHandler(RegisterHandler handler)
{
    std::lock_guard<std::mutex> lock(handler_mutex_);
    register_handler_ = std::move(handler);
}

void MqttClient::setDeleteHandler(DeleteHandler handler)
{
    std::lock_guard<std::mutex> lock(handler_mutex_);
    delete_handler_ = std::move(handler);
}

bool MqttClient::publishRecognition(const MqttRecognitionPayload& payload)
{
    if (!enabled_ || mode_.load(std::memory_order_acquire) != Mode::PRODUCTION)
        return false;

    const std::string event = buildRecognitionEvent(payload);
    if (!enqueuePendingEvent(event))
        return false;
    publishJson(event_topic_, event);
    return true;
}

void MqttClient::loop()
{
    auto last_activity = std::chrono::steady_clock::now();

    while (running_) {
        if (socket_fd_ < 0) {
            if (!connectBroker() || !subscribeActiveTopics()) {
                disconnectBroker();
                for (int i = 0; running_ && i < kReconnectDelaySeconds * 10; ++i)
                    usleep(100000);
                continue;
            }
            afterConnected();
            last_activity = std::chrono::steady_clock::now();
        }

        const auto now = std::chrono::steady_clock::now();
        if (mode_.load() == Mode::BOOTSTRAP && now >= next_activation_retry_)
            publishActivationRequest();

        if (mode_.load() == Mode::PRODUCTION) {
            if (status_interval_seconds_ > 0 && now >= next_status_publish_) {
                publishDeviceOnline();
                next_status_publish_ =
                    now + std::chrono::seconds(status_interval_seconds_);
            }
            if (now >= next_event_retry_) {
                retryPendingEvents();
                next_event_retry_ =
                    now + std::chrono::seconds(retry_interval_seconds_);
            }
        }

        unsigned char packet_type = 0;
        std::string body;
        if (readPacket(&packet_type, &body)) {
            const unsigned char type = packet_type & 0xf0;
            if (type == 0x30)
                handlePublish(packet_type, body);
            last_activity = std::chrono::steady_clock::now();
            continue;
        }

        const auto after_read = std::chrono::steady_clock::now();
        if (!connected_) {
            disconnectBroker();
            continue;
        }
        if (after_read - last_activity >=
            std::chrono::seconds(kMqttKeepAliveSeconds / 2)) {
            if (!sendPacket(std::string("\xc0\x00", 2)))
                disconnectBroker();
            last_activity = after_read;
        }
    }
}

bool MqttClient::connectBroker()
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_buffer[16];
    snprintf(port_buffer, sizeof(port_buffer), "%d", broker_port_);

    struct addrinfo* result = nullptr;
    const int gai =
        getaddrinfo(broker_host_.c_str(), port_buffer, &hints, &result);
    if (gai != 0) {
        printf("[mqtt] cannot resolve broker: %s\n", gai_strerror(gai));
        return false;
    }

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        setSocketTimeout(fd, 10);

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        printf("[mqtt] cannot connect to %s:%d\n",
               broker_host_.c_str(), broker_port_);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_fd_ = fd;
    }

    if (tls_enabled_) {
        ssl_ctx_ = SSL_CTX_new(TLS_client_method());
        if (!ssl_ctx_) {
            printf("[mqtt] cannot create TLS context\n");
            disconnectBroker();
            return false;
        }
        SSL_CTX_set_verify(ssl_ctx_,
                           verify_server_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
                           nullptr);
        if (verify_server_) {
            const bool ca_loaded = ca_file_.empty()
                                       ? SSL_CTX_set_default_verify_paths(ssl_ctx_) == 1
                                       : SSL_CTX_load_verify_locations(
                                             ssl_ctx_, ca_file_.c_str(), nullptr) == 1;
            if (!ca_loaded) {
                printf("[mqtt] cannot load trusted CA; set MQTT_CA_FILE or disable MQTT_TLS_VERIFY_SERVER\n");
                disconnectBroker();
                return false;
            }
        }

        ssl_ = SSL_new(ssl_ctx_);
        if (!ssl_) {
            printf("[mqtt] cannot create TLS session\n");
            disconnectBroker();
            return false;
        }
        SSL_set_fd(ssl_, fd);
        SSL_set_tlsext_host_name(ssl_, broker_host_.c_str());
        if (verify_server_) {
            X509_VERIFY_PARAM* verify_param = SSL_get0_param(ssl_);
            X509_VERIFY_PARAM_set_hostflags(
                verify_param, X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);
            if (X509_VERIFY_PARAM_set1_host(
                    verify_param, broker_host_.c_str(), 0) != 1) {
                printf("[mqtt] cannot configure TLS hostname verification\n");
                disconnectBroker();
                return false;
            }
        }

        const int handshake_result = SSL_connect(ssl_);
        const long verify_result = SSL_get_verify_result(ssl_);
        if (handshake_result != 1 ||
            (verify_server_ && verify_result != X509_V_OK)) {
            const int ssl_error = SSL_get_error(ssl_, handshake_result);
            const unsigned long openssl_error = ERR_get_error();
            char error_buffer[256] = {0};
            if (openssl_error != 0)
                ERR_error_string_n(openssl_error, error_buffer,
                                   sizeof(error_buffer));
            printf("[mqtt] TLS failed ssl_error=%d verify=%ld detail=%s\n",
                   ssl_error, verify_result,
                   error_buffer[0] ? error_buffer : "none");
            disconnectBroker();
            return false;
        }
        if (!verify_server_)
            printf("[mqtt] TLS connected without server certificate verification\n");
    }

    const bool clean_session = mode_.load() == Mode::BOOTSTRAP;
    if (!sendPacket(makeConnectPacket(active_client_id_, active_username_,
                                      active_password_, clean_session))) {
        disconnectBroker();
        return false;
    }

    unsigned char packet_type = 0;
    std::string body;
    if (!readPacket(&packet_type, &body) || packet_type != 0x20 ||
        body.size() < 2 || body[1] != 0x00) {
        printf("[mqtt] CONNACK failed\n");
        disconnectBroker();
        return false;
    }

    connected_ = true;
    setSocketTimeout(fd, 1);
    printf("[mqtt] connected broker=%s:%d mode=%s\n",
           broker_host_.c_str(), broker_port_,
           mode_.load() == Mode::PRODUCTION ? "production" : "bootstrap");
    return true;
}

void MqttClient::disconnectBroker()
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    connected_ = false;
    if (ssl_) {
        SSL_shutdown(ssl_);
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ssl_ctx_) {
        SSL_CTX_free(ssl_ctx_);
        ssl_ctx_ = nullptr;
    }
    if (socket_fd_ >= 0) {
        shutdown(socket_fd_, SHUT_RDWR);
        close(socket_fd_);
        socket_fd_ = -1;
    }
}

bool MqttClient::subscribeTopic(const std::string& topic)
{
    if (topic.empty())
        return false;

    unsigned int raw_id = next_packet_id_.fetch_add(1);
    unsigned short packet_id = (unsigned short)(raw_id & 0xffff);
    if (packet_id == 0)
        packet_id = (unsigned short)(next_packet_id_.fetch_add(1) & 0xffff);

    if (!sendPacket(makeSubscribePacket(topic, packet_id)))
        return false;

    // A production connection keeps its broker session. After the device has
    // been offline, the broker is allowed to deliver queued PUBLISH packets
    // immediately after CONNACK, before replying to this SUBSCRIBE. Do not
    // mistake such a packet for a failed SUBACK or reconnect forever without
    // acknowledging the queued command.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (running_ && std::chrono::steady_clock::now() < deadline) {
        unsigned char packet_type = 0;
        std::string body;
        if (!readPacket(&packet_type, &body))
            continue;

        const unsigned char type = packet_type & 0xf0;
        if (type == 0x30) {
            printf("[mqtt] queued PUBLISH received while waiting for "
                   "SUBACK topic=%s\n", topic.c_str());
            handlePublish(packet_type, body);
            continue;
        }
        if (type != 0x90) {
            printf("[mqtt] ignoring packet type=0x%02x while waiting for "
                   "SUBACK topic=%s\n", type, topic.c_str());
            continue;
        }
        if (body.size() < 3) {
            printf("[mqtt] malformed SUBACK topic=%s bytes=%zu\n",
                   topic.c_str(), body.size());
            return false;
        }

        const unsigned short acknowledged_id = readUint16(body, 0);
        if (acknowledged_id != packet_id) {
            printf("[mqtt] ignoring SUBACK packet_id=%u while waiting for "
                   "packet_id=%u topic=%s\n",
                   acknowledged_id, packet_id, topic.c_str());
            continue;
        }

        const unsigned char return_code = (unsigned char)body[2];
        if (return_code == 0x80) {
            printf("[mqtt] SUBACK denied topic=%s return_code=0x80\n",
                   topic.c_str());
            return false;
        }
        if (return_code > 0x02) {
            printf("[mqtt] invalid SUBACK topic=%s return_code=0x%02x\n",
                   topic.c_str(), return_code);
            return false;
        }

        printf("[mqtt] subscribed topic=%s qos=%u\n",
               topic.c_str(), return_code);
        return true;
    }

    printf("[mqtt] SUBACK timeout topic=%s packet_id=%u\n",
           topic.c_str(), packet_id);
    return false;
}

bool MqttClient::subscribeActiveTopics()
{
    if (mode_.load() == Mode::BOOTSTRAP)
        return subscribeTopic(bootstrap_response_topic_);
    return subscribeTopic(request_topic_) && subscribeTopic(ack_topic_);
}

void MqttClient::afterConnected()
{
    if (mode_.load() == Mode::BOOTSTRAP) {
        publishActivationRequest();
        return;
    }

    publishDeviceOnline();
    next_status_publish_ = std::chrono::steady_clock::now() +
                           std::chrono::seconds(status_interval_seconds_);
    retryPendingEvents();
    next_event_retry_ = std::chrono::steady_clock::now() +
                        std::chrono::seconds(retry_interval_seconds_);
}

bool MqttClient::readPacket(unsigned char* packet_type, std::string* body)
{
    unsigned char header = 0;
    if (transportRead((char*)&header, 1) != 1)
        return false;

    size_t multiplier = 1;
    size_t remaining_length = 0;
    unsigned char encoded = 0;
    do {
        if (transportRead((char*)&encoded, 1) != 1)
            return false;
        remaining_length += (encoded & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128 * 128)
            return false;
    } while ((encoded & 128) != 0);

    body->assign(remaining_length, '\0');
    size_t received = 0;
    while (received < remaining_length) {
        const int count = transportRead(&(*body)[received],
                                        remaining_length - received);
        if (count <= 0)
            return false;
        received += (size_t)count;
    }
    *packet_type = header;
    return true;
}

bool MqttClient::sendPacket(const std::string& packet)
{
    return transportWrite(packet.data(), packet.size());
}

bool MqttClient::publishJson(const std::string& topic,
                             const std::string& payload,
                             unsigned short* published_packet_id,
                             bool duplicate)
{
    if (!enabled_ || !connected_ || topic.empty())
        return false;

    unsigned int raw_id = next_packet_id_.fetch_add(1);
    unsigned short packet_id = (unsigned short)(raw_id & 0xffff);
    if (packet_id == 0)
        packet_id = (unsigned short)(next_packet_id_.fetch_add(1) & 0xffff);

    if (!sendPacket(makePublishPacket(topic, payload, packet_id, duplicate))) {
        printf("[mqtt] publish failed topic=%s\n", topic.c_str());
        connected_ = false;
        return false;
    }
    if (published_packet_id)
        *published_packet_id = packet_id;
    printf("[mqtt] published topic=%s qos=1 bytes=%zu\n",
           topic.c_str(), payload.size());
    if (debug_payload_)
        printf("[mqtt-json] publish topic=%s body=%s\n",
               topic.c_str(), jsonForLog(payload).c_str());
    return true;
}

bool MqttClient::publishJsonAndWaitAck(const std::string& topic,
                                       const std::string& payload)
{
    unsigned short expected_id = 0;
    if (!publishJson(topic, payload, &expected_id))
        return false;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (running_ && std::chrono::steady_clock::now() < deadline) {
        unsigned char packet_type = 0;
        std::string body;
        if (!readPacket(&packet_type, &body))
            continue;
        if ((packet_type & 0xf0) == 0x40 && body.size() >= 2 &&
            readUint16(body, 0) == expected_id)
            return true;
        if ((packet_type & 0xf0) == 0x30)
            handlePublish(packet_type, body);
    }
    return false;
}

void MqttClient::handlePublish(unsigned char packet_type,
                               const std::string& body)
{
    if (body.size() < 2)
        return;
    const unsigned int topic_length = readUint16(body, 0);
    size_t payload_offset = 2 + topic_length;
    if (body.size() < payload_offset)
        return;

    const std::string topic = body.substr(2, topic_length);
    const int qos = (packet_type >> 1) & 0x03;
    if (qos == 1) {
        if (body.size() < payload_offset + 2)
            return;
        const unsigned short packet_id = readUint16(body, payload_offset);
        payload_offset += 2;
        sendPacket(makePubAckPacket(packet_id));
    }
    const std::string payload = body.substr(payload_offset);
    if (debug_payload_)
        printf("[mqtt-json] receive topic=%s body=%s\n",
               topic.c_str(), jsonForLog(payload).c_str());

    if (mode_.load() == Mode::BOOTSTRAP && topic == bootstrap_response_topic_) {
        handleBootstrapResponse(payload);
    } else if (mode_.load() == Mode::PRODUCTION && topic == request_topic_) {
        handleCommandRequest(payload);
    } else if (mode_.load() == Mode::PRODUCTION && topic == ack_topic_) {
        handleEventAck(payload);
    }
}

void MqttClient::handleBootstrapResponse(const std::string& payload)
{
    const std::string uuid = jsonGetString(payload, "uuid");
    const std::string command = jsonGetString(payload, "command");
    const bool status = jsonGetBool(payload, "status", false);
    const std::string message = jsonGetString(payload, "message");

    if (uuid != activation_uuid_ || command != "activate_device") {
        printf("[mqtt-bootstrap] ignored response with unexpected uuid/command\n");
        return;
    }

    if (!status) {
        printf("[mqtt-bootstrap] activation not ready: %s\n", message.c_str());
        next_activation_retry_ = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(retry_interval_seconds_);
        return;
    }

    const std::string data = extractJsonObject(payload, "data");
    MqttDeviceCredential credential;
    credential.credential_version =
        jsonGetInt(data, "credential_version", 0);
    credential.client_id = jsonGetString(data, "client_id");
    if (credential.client_id.empty())
        credential.client_id = normalized_mac_;
    credential.username = jsonGetString(data, "username");
    if (credential.username.empty())
        credential.username = normalized_mac_;
    credential.password = jsonGetString(data, "password");
    credential.topic_root = trimTrailingSlash(jsonGetString(data, "topic_root"));
    if (credential.topic_root.empty())
        credential.topic_root = "x68-lite/v1/device/" + credential.client_id;
    credential.valid = credential.credential_version > 0 &&
                       !credential.client_id.empty() &&
                       !credential.username.empty() &&
                       !credential.password.empty() &&
                       !credential.topic_root.empty();
    if (!credential.valid) {
        printf("[mqtt-bootstrap] approved response is missing credential fields\n");
        return;
    }

    std::string saved_credential_path = credential_path_;
    if (!saveCredentialFile(credential_path_, credential)) {
        printf("[mqtt-bootstrap] cannot save credential file: %s\n",
               credential_path_.c_str());
        saved_credential_path.clear();
        if (!credential_fallback_path_.empty() &&
            credential_fallback_path_ != credential_path_) {
            if (saveCredentialFile(credential_fallback_path_, credential)) {
                saved_credential_path = credential_fallback_path_;
                printf("[mqtt-bootstrap] credential saved to fallback: %s\n",
                       saved_credential_path.c_str());
            } else {
                printf("[mqtt-bootstrap] cannot save credential fallback: %s\n",
                       credential_fallback_path_.c_str());
            }
        }
        if (saved_credential_path.empty())
            return;
    }

    if (!publishJsonAndWaitAck(
            bootstrap_ack_topic_,
            buildActivationAck(credential.credential_version))) {
        printf("[mqtt-bootstrap] credential ACK was not confirmed; will retry\n");
        unlink(saved_credential_path.c_str());
        next_activation_retry_ = std::chrono::steady_clock::now() +
                                 std::chrono::seconds(retry_interval_seconds_);
        return;
    }

    printf("[mqtt-bootstrap] credential saved and acknowledged\n");
    switchToProduction(credential);
    disconnectBroker();
}

void MqttClient::handleCommandRequest(const std::string& payload)
{
    const std::string uuid = jsonGetString(payload, "uuid");
    const std::string command = jsonGetString(payload, "command");
    if (!uuid.empty()) {
        const auto cached = command_response_cache_.find(uuid);
        if (cached != command_response_cache_.end()) {
            publishJson(response_topic_, cached->second);
            return;
        }
    }

    const std::string data = extractJsonObject(payload, "data");
    std::string response_payload;
    if (command == "delete_face") {
        DeleteFaceRequest request;
        request.uuid = uuid;
        request.timestamp = jsonGetString(payload, "timestamp");
        request.command = command;
        request.employee_id = jsonGetString(data, "employee_id");

        DeleteFaceResponse response;
        DeleteHandler handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = delete_handler_;
        }
        if (!handler) {
            response.message = "delete handler not ready";
        } else {
            response = handler(request);
        }
        response_payload = buildDeleteResponse(request, response);
    } else {
        RegisterFaceRequest request;
        request.uuid = uuid;
        request.timestamp = jsonGetString(payload, "timestamp");
        request.command = command;
        request.employee_id = jsonGetString(data, "employee_id");
        request.name = jsonGetString(data, "name");
        request.face_link = jsonGetString(data, "face_link");
        request.audio_link = jsonGetString(data, "audio_link");

        RegisterFaceResponse response;
        if (command != "register_face") {
            response.message = "unsupported command";
        } else {
            RegisterHandler handler;
            {
                std::lock_guard<std::mutex> lock(handler_mutex_);
                handler = register_handler_;
            }
            if (!handler)
                response.message = "register handler not ready";
            else
                response = handler(request);
        }
        response_payload = buildRegisterResponse(request, response);
    }

    if (!uuid.empty()) {
        cacheCommandResponse(uuid, response_payload);
    }
    publishJson(response_topic_, response_payload);
}

void MqttClient::handleEventAck(const std::string& payload)
{
    const std::string uuid = jsonGetString(payload, "uuid");
    if (uuid.empty())
        return;

    if (jsonGetBool(payload, "status", false)) {
        acknowledgePendingEvent(uuid);
    } else {
        printf("[mqtt] event rejected uuid=%s message=%s; drop pending event\n",
               uuid.c_str(), jsonGetString(payload, "message").c_str());
        acknowledgePendingEvent(uuid);
    }
}

void MqttClient::publishActivationRequest()
{
    publishJson(bootstrap_request_topic_, buildActivationRequest());
    next_activation_retry_ = std::chrono::steady_clock::now() +
                             std::chrono::seconds(retry_interval_seconds_);
}

bool MqttClient::publishDeviceOnline()
{
    return publishJson(status_topic_, buildDeviceOnlineEvent());
}

void MqttClient::retryPendingEvents()
{
    std::vector<std::string> events;
    {
        std::lock_guard<std::mutex> lock(event_mutex_);
        std::ifstream input(offline_queue_path_.c_str());
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty())
                events.push_back(line);
        }
    }
    for (const std::string& event : events) {
        if (!publishJson(event_topic_, event))
            break;
    }
}

bool MqttClient::enqueuePendingEvent(const std::string& payload)
{
    std::lock_guard<std::mutex> lock(event_mutex_);
    if (!ensureParentDirectory(offline_queue_path_))
        return false;
    std::ofstream output(offline_queue_path_.c_str(),
                         std::ios::out | std::ios::app);
    if (!output.good()) {
        printf("[mqtt] cannot persist pending event\n");
        return false;
    }
    output << payload << '\n';
    output.close();
    if (!output.good()) {
        printf("[mqtt] cannot flush pending event\n");
        return false;
    }
    chmod(offline_queue_path_.c_str(), 0600);
    return true;
}

void MqttClient::acknowledgePendingEvent(const std::string& uuid)
{
    std::lock_guard<std::mutex> lock(event_mutex_);
    std::ifstream input(offline_queue_path_.c_str());
    if (!input.good())
        return;

    std::vector<std::string> pending;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && jsonGetString(line, "uuid") != uuid)
            pending.push_back(line);
    }
    input.close();

    if (pending.empty()) {
        unlink(offline_queue_path_.c_str());
        return;
    }

    std::ostringstream content;
    for (const std::string& item : pending)
        content << item << '\n';
    writeTextFile(offline_queue_path_, content.str(), 0600);
}

void MqttClient::loadCommandCache()
{
    std::ifstream input(command_cache_path_.c_str());
    if (!input.good() && !command_cache_fallback_path_.empty() &&
        command_cache_fallback_path_ != command_cache_path_) {
        input.clear();
        input.open(command_cache_fallback_path_.c_str());
        if (input.good()) {
            printf("[mqtt] loaded processed command cache fallback=%s\n",
                   command_cache_fallback_path_.c_str());
        }
    }
    std::string payload;
    while (std::getline(input, payload)) {
        const std::string uuid = jsonGetString(payload, "uuid");
        if (uuid.empty())
            continue;
        if (command_response_cache_.find(uuid) == command_response_cache_.end())
            command_response_order_.push_back(uuid);
        command_response_cache_[uuid] = payload;
    }
    while (command_response_order_.size() > kCommandCacheLimit) {
        command_response_cache_.erase(command_response_order_.front());
        command_response_order_.pop_front();
    }
}

void MqttClient::cacheCommandResponse(const std::string& uuid,
                                      const std::string& payload)
{
    if (command_response_cache_.find(uuid) == command_response_cache_.end())
        command_response_order_.push_back(uuid);
    command_response_cache_[uuid] = payload;
    while (command_response_order_.size() > kCommandCacheLimit) {
        command_response_cache_.erase(command_response_order_.front());
        command_response_order_.pop_front();
    }

    std::ostringstream content;
    for (const std::string& cached_uuid : command_response_order_)
        content << command_response_cache_[cached_uuid] << '\n';
    if (!writeTextFile(command_cache_path_, content.str(), 0600)) {
        printf("[mqtt] warning: processed command cache could not be saved: %s\n",
               command_cache_path_.c_str());
        if (!command_cache_fallback_path_.empty() &&
            command_cache_fallback_path_ != command_cache_path_) {
            if (writeTextFile(command_cache_fallback_path_,
                              content.str(),
                              0600)) {
                printf("[mqtt] processed command cache saved to fallback: %s\n",
                       command_cache_fallback_path_.c_str());
            } else {
                printf("[mqtt] warning: processed command cache fallback could not be saved: %s\n",
                       command_cache_fallback_path_.c_str());
            }
        }
    }
}

void MqttClient::switchToProduction(
    const MqttDeviceCredential& credential)
{
    credential_ = credential;
    active_client_id_ = credential.client_id;
    active_username_ = credential.username;
    active_password_ = credential.password;
    const std::string root = trimTrailingSlash(credential.topic_root);
    request_topic_ = root + "/request";
    ack_topic_ = root + "/ack";
    response_topic_ = root + "/response";
    event_topic_ = root + "/event";
    status_topic_ = root + "/status";
    mode_.store(Mode::PRODUCTION, std::memory_order_release);
}

bool MqttClient::transportWrite(const char* data, size_t len)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ < 0)
        return false;

    size_t sent = 0;
    while (sent < len) {
        int count = 0;
        if (tls_enabled_) {
            count = SSL_write(ssl_, data + sent, (int)(len - sent));
            if (count <= 0) {
                const int error = SSL_get_error(ssl_, count);
                if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
                    continue;
                return false;
            }
        } else {
            count = (int)send(socket_fd_, data + sent, len - sent, 0);
            if (count < 0 && errno == EINTR)
                continue;
            if (count <= 0)
                return false;
        }
        sent += (size_t)count;
    }
    return true;
}

int MqttClient::transportRead(char* data, size_t len)
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        fd = socket_fd_;
        if (fd < 0)
            return -1;
        if (tls_enabled_ && ssl_ && SSL_pending(ssl_) > 0) {
            const int count = SSL_read(ssl_, data, (int)len);
            return count > 0 ? count : -1;
        }
    }

    struct pollfd descriptor;
    descriptor.fd = fd;
    descriptor.events = POLLIN;
    descriptor.revents = 0;
    const int ready = poll(&descriptor, 1, 1000);
    if (ready == 0)
        return 0;
    if (ready < 0) {
        if (errno == EINTR)
            return 0;
        return -1;
    }
    if ((descriptor.revents & POLLIN) == 0)
        return -1;

    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ != fd)
        return -1;
    if (tls_enabled_) {
        const int count = SSL_read(ssl_, data, (int)len);
        if (count > 0)
            return count;
        const int error = SSL_get_error(ssl_, count);
        if (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE)
            return 0;
        return -1;
    }

    const int count = (int)recv(socket_fd_, data, len, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
        return 0;
    return count;
}

std::string MqttClient::buildRegisterResponse(
    const RegisterFaceRequest& request,
    const RegisterFaceResponse& response)
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(request.uuid) << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"register_face\",";
    out << "\"status\":" << (response.status ? "true" : "false") << ",";
    out << "\"message\":\"" << jsonEscape(response.message) << "\",";
    out << "\"data\":{\"employee_id\":\""
        << jsonEscape(request.employee_id) << "\"}}";
    return out.str();
}

std::string MqttClient::buildDeleteResponse(
    const DeleteFaceRequest& request,
    const DeleteFaceResponse& response)
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(request.uuid) << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"delete_face\",";
    out << "\"status\":" << (response.status ? "true" : "false") << ",";
    out << "\"message\":\"" << jsonEscape(response.message) << "\",";
    out << "\"data\":{\"employee_id\":\""
        << jsonEscape(request.employee_id) << "\"}}";
    return out.str();
}

std::string MqttClient::buildRecognitionEvent(
    const MqttRecognitionPayload& payload)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{";
    out << "\"uuid\":\"" << generateUuid() << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"face_recognized\",";
    out << "\"data\":{";
    out << "\"employee_id\":\"" << jsonEscape(payload.employee_id) << "\",";
    out << "\"confidence\":" << payload.confidence << ",";
    out << "\"distance\":" << payload.distance << ",";
    out << "\"image_base64\":\""
        << jsonEscape(fileToBase64(payload.image_path)) << "\"}}";
    return out.str();
}

std::string MqttClient::buildActivationRequest() const
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << activation_uuid_ << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"activate_device\",";
    out << "\"data\":{";
    out << "\"installation_id\":\"" << jsonEscape(installation_id_) << "\",";
    out << "\"device_id\":\"" << jsonEscape(normalized_mac_) << "\",";
    out << "\"model\":\"" << jsonEscape(device_model_) << "\",";
    out << "\"serial_number\":\"" << jsonEscape(serial_number_) << "\",";
    out << "\"software_version\":\"" << jsonEscape(software_version_)
        << "\"}}";
    return out.str();
}

std::string MqttClient::buildActivationAck(int credential_version) const
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << activation_uuid_ << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"activate_device\",";
    out << "\"status\":true,";
    out << "\"message\":\"configuration applied\",";
    out << "\"data\":{\"credential_version\":" << credential_version
        << "}}";
    return out.str();
}

std::string MqttClient::buildDeviceOnlineEvent() const
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << generateUuid() << "\",";
    out << "\"timestamp\":\"" << nowIsoUtc() << "\",";
    out << "\"command\":\"device_status\",";
    out << "\"data\":{";
    out << "\"state\":\"online\",";
    out << "\"software_version\":\"" << jsonEscape(software_version_) << "\"";
    out << "}}";
    return out.str();
}

std::string MqttClient::getEnvOrDefault(const char* name,
                                        const std::string& fallback)
{
    const char* value = getenv(name);
    return (!value || value[0] == '\0') ? fallback : std::string(value);
}

bool MqttClient::getEnvBool(const char* name, bool fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return strcmp(value, "1") == 0 || strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 || strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

int MqttClient::getEnvInt(const char* name, int fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    return (!end || *end != '\0') ? fallback : (int)parsed;
}

std::string MqttClient::getEth0Mac()
{
    std::ifstream input("/sys/class/net/eth0/address");
    std::string mac;
    if (std::getline(input, mac) && !mac.empty())
        return mac;
    return "00:00:00:00:00:00";
}

std::string MqttClient::normalizeMac(const std::string& mac)
{
    std::string normalized;
    for (unsigned char ch : mac) {
        if (std::isxdigit(ch))
            normalized.push_back((char)std::tolower(ch));
    }
    return normalized;
}

std::string MqttClient::getIpAddress()
{
    struct ifaddrs* addresses = nullptr;
    if (getifaddrs(&addresses) != 0)
        return "";
    std::string ip;
    for (struct ifaddrs* item = addresses; item; item = item->ifa_next) {
        if (!item->ifa_addr || item->ifa_addr->sa_family != AF_INET ||
            (item->ifa_flags & IFF_LOOPBACK))
            continue;
        char host[NI_MAXHOST];
        if (getnameinfo(item->ifa_addr, sizeof(struct sockaddr_in),
                        host, sizeof(host), nullptr, 0,
                        NI_NUMERICHOST) == 0) {
            ip = host;
            break;
        }
    }
    freeifaddrs(addresses);
    return ip;
}

std::string MqttClient::nowIsoUtc()
{
    const time_t current = time(nullptr);
    struct tm utc;
    gmtime_r(&current, &utc);
    char buffer[32];
    strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

std::string MqttClient::generateUuid()
{
    static std::mutex mutex;
    static std::mt19937 rng(std::random_device{}());
    std::lock_guard<std::mutex> lock(mutex);
    unsigned char bytes[16];
    for (unsigned char& byte : bytes)
        byte = (unsigned char)(rng() & 0xff);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buffer[37];
    snprintf(buffer, sizeof(buffer),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3], bytes[4], bytes[5],
             bytes[6], bytes[7], bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return buffer;
}

std::string MqttClient::jsonEscape(const std::string& value)
{
    std::ostringstream out;
    for (char ch : value) {
        switch (ch) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\b': out << "\\b"; break;
        case '\f': out << "\\f"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if ((unsigned char)ch < 0x20) {
                out << "\\u" << std::hex << std::setw(4)
                    << std::setfill('0') << (int)(unsigned char)ch << std::dec;
            } else {
                out << ch;
            }
        }
    }
    return out.str();
}

std::string MqttClient::jsonForLog(const std::string& value)
{
    std::string out = value;
    const auto replaceStringValue =
        [&out](const std::string& key, const std::string& replacement) {
            const std::string quoted_key = "\"" + key + "\"";
            size_t search_from = 0;
            while (true) {
                size_t pos = out.find(quoted_key, search_from);
                if (pos == std::string::npos)
                    break;
                pos = out.find(':', pos + quoted_key.size());
                if (pos == std::string::npos)
                    break;
                ++pos;
                while (pos < out.size() && isspace((unsigned char)out[pos]))
                    ++pos;
                if (pos >= out.size() || out[pos] != '"') {
                    search_from = pos;
                    continue;
                }
                const size_t value_start = pos + 1;
                size_t value_end = value_start;
                bool escaped = false;
                while (value_end < out.size()) {
                    const char ch = out[value_end];
                    if (escaped) {
                        escaped = false;
                    } else if (ch == '\\') {
                        escaped = true;
                    } else if (ch == '"') {
                        break;
                    }
                    ++value_end;
                }
                if (value_end >= out.size())
                    break;
                out.replace(value_start, value_end - value_start, replacement);
                search_from = value_start + replacement.size();
            }
        };

    replaceStringValue("password", "<redacted>");

    const std::string image_key = "\"image_base64\"";
    size_t pos = out.find(image_key);
    while (pos != std::string::npos) {
        pos = out.find(':', pos + image_key.size());
        if (pos == std::string::npos)
            break;
        ++pos;
        while (pos < out.size() && isspace((unsigned char)out[pos]))
            ++pos;
        if (pos < out.size() && out[pos] == '"') {
            const size_t value_start = pos + 1;
            size_t value_end = out.find('"', value_start);
            if (value_end == std::string::npos)
                break;
            if (value_end - value_start > 80) {
                const std::string shortened =
                    out.substr(value_start, 80) + "...<truncated>";
                out.replace(value_start, value_end - value_start, shortened);
                pos = value_start + shortened.size();
            } else {
                pos = value_end + 1;
            }
        }
        pos = out.find(image_key, pos);
    }
    return out;
}

std::string MqttClient::jsonGetString(const std::string& json,
                                      const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos ||
        (pos = json.find(':', pos + quoted.size())) == std::string::npos)
        return "";
    do { ++pos; } while (pos < json.size() && std::isspace((unsigned char)json[pos]));
    if (pos >= json.size() || json[pos++] != '"')
        return "";

    std::string value;
    while (pos < json.size()) {
        char ch = json[pos++];
        if (ch == '"')
            break;
        if (ch == '\\' && pos < json.size()) {
            const char escaped = json[pos++];
            switch (escaped) {
            case '"': value.push_back('"'); break;
            case '\\': value.push_back('\\'); break;
            case '/': value.push_back('/'); break;
            case 'b': value.push_back('\b'); break;
            case 'f': value.push_back('\f'); break;
            case 'n': value.push_back('\n'); break;
            case 'r': value.push_back('\r'); break;
            case 't': value.push_back('\t'); break;
            default: value.push_back(escaped); break;
            }
        } else {
            value.push_back(ch);
        }
    }
    return value;
}

int MqttClient::jsonGetInt(const std::string& json,
                           const std::string& key,
                           int fallback)
{
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos ||
        (pos = json.find(':', pos + quoted.size())) == std::string::npos)
        return fallback;
    ++pos;
    while (pos < json.size() && std::isspace((unsigned char)json[pos]))
        ++pos;
    char* end = nullptr;
    const long value = strtol(json.c_str() + pos, &end, 10);
    return (!end || end == json.c_str() + pos) ? fallback : (int)value;
}

bool MqttClient::jsonGetBool(const std::string& json,
                             const std::string& key,
                             bool fallback)
{
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos ||
        (pos = json.find(':', pos + quoted.size())) == std::string::npos)
        return fallback;
    ++pos;
    while (pos < json.size() && std::isspace((unsigned char)json[pos]))
        ++pos;
    if (json.compare(pos, 4, "true") == 0)
        return true;
    if (json.compare(pos, 5, "false") == 0)
        return false;
    return fallback;
}

std::string MqttClient::extractJsonObject(const std::string& json,
                                          const std::string& key)
{
    const std::string quoted = "\"" + key + "\"";
    size_t pos = json.find(quoted);
    if (pos == std::string::npos ||
        (pos = json.find(':', pos + quoted.size())) == std::string::npos ||
        (pos = json.find('{', pos + 1)) == std::string::npos)
        return "";

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        const char ch = json[i];
        if (in_string) {
            if (escaped)
                escaped = false;
            else if (ch == '\\')
                escaped = true;
            else if (ch == '"')
                in_string = false;
        } else if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}' && --depth == 0) {
            return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

std::string MqttClient::fileToBase64(const std::string& path)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::ifstream input(path.c_str(), std::ios::binary);
    if (!input.good())
        return "";
    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string bytes = stream.str();
    std::string encoded;
    encoded.reserve(((bytes.size() + 2) / 3) * 4);
    for (size_t i = 0; i < bytes.size(); i += 3) {
        unsigned int value = (unsigned char)bytes[i] << 16;
        if (i + 1 < bytes.size())
            value |= (unsigned char)bytes[i + 1] << 8;
        if (i + 2 < bytes.size())
            value |= (unsigned char)bytes[i + 2];
        encoded.push_back(table[(value >> 18) & 0x3f]);
        encoded.push_back(table[(value >> 12) & 0x3f]);
        encoded.push_back(i + 1 < bytes.size() ? table[(value >> 6) & 0x3f] : '=');
        encoded.push_back(i + 2 < bytes.size() ? table[value & 0x3f] : '=');
    }
    return encoded;
}

bool MqttClient::ensureParentDirectory(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos || slash == 0)
        return true;
    const std::string directory = path.substr(0, slash);
    std::string current = directory[0] == '/' ? "/" : "";
    size_t pos = directory[0] == '/' ? 1 : 0;
    while (pos <= directory.size()) {
        const size_t next = directory.find('/', pos);
        const std::string part = directory.substr(pos, next - pos);
        if (!part.empty()) {
            current = current.empty() || current == "/"
                          ? current + part
                          : current + "/" + part;
            struct stat state;
            if (stat(current.c_str(), &state) != 0) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            } else if (!S_ISDIR(state.st_mode)) {
                return false;
            }
        }
        if (next == std::string::npos)
            break;
        pos = next + 1;
    }
    return true;
}

bool MqttClient::writeTextFile(const std::string& path,
                               const std::string& content,
                               int mode)
{
    if (!ensureParentDirectory(path)) {
        printf("[mqtt] cannot create parent directory for %s: %s\n",
               path.c_str(), strerror(errno));
        return false;
    }
    const std::string temporary = path + ".tmp";
    std::ofstream output(temporary.c_str(), std::ios::out | std::ios::trunc);
    if (!output.good()) {
        printf("[mqtt] cannot open temp file %s for writing: %s\n",
               temporary.c_str(), strerror(errno));
        return false;
    }
    output << content;
    output.close();
    if (!output.good()) {
        printf("[mqtt] cannot flush temp file %s\n", temporary.c_str());
        return false;
    }
    chmod(temporary.c_str(), mode);
    if (rename(temporary.c_str(), path.c_str()) != 0) {
        printf("[mqtt] cannot rename %s to %s: %s\n",
               temporary.c_str(), path.c_str(), strerror(errno));
        return false;
    }
    chmod(path.c_str(), mode);
    return true;
}

std::string MqttClient::loadOrCreateInstallationId(
    const std::string& configured_id,
    const std::string& path)
{
    if (!configured_id.empty())
        return configured_id;
    std::ifstream input(path.c_str());
    std::string id;
    if (std::getline(input, id) && !id.empty())
        return id;
    id = generateUuid();
    if (!writeTextFile(path, id + "\n", 0600))
        printf("[mqtt] warning: installation_id could not be persisted\n");
    return id;
}

bool MqttClient::loadCredentialFile(const std::string& path,
                                    MqttDeviceCredential* credential)
{
    if (!credential)
        return false;
    std::ifstream input(path.c_str());
    if (!input.good())
        return false;
    std::ostringstream stream;
    stream << input.rdbuf();
    const std::string json = stream.str();
    credential->credential_version =
        jsonGetInt(json, "credential_version", 0);
    credential->client_id = jsonGetString(json, "client_id");
    credential->username = jsonGetString(json, "username");
    credential->password = jsonGetString(json, "password");
    credential->topic_root = trimTrailingSlash(jsonGetString(json, "topic_root"));
    credential->valid = credential->credential_version > 0 &&
                        !credential->client_id.empty() &&
                        !credential->username.empty() &&
                        !credential->password.empty() &&
                        !credential->topic_root.empty();
    return credential->valid;
}

bool MqttClient::saveCredentialFile(
    const std::string& path,
    const MqttDeviceCredential& credential)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"credential_version\": " << credential.credential_version << ",\n";
    out << "  \"client_id\": \"" << jsonEscape(credential.client_id) << "\",\n";
    out << "  \"username\": \"" << jsonEscape(credential.username) << "\",\n";
    out << "  \"password\": \"" << jsonEscape(credential.password) << "\",\n";
    out << "  \"topic_root\": \"" << jsonEscape(credential.topic_root) << "\"\n";
    out << "}\n";
    return writeTextFile(path, out.str(), 0600);
}
