#include "mqtt_client.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <net/if.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>
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
#include <vector>

namespace {
constexpr int kMqttKeepAliveSeconds = 120;
constexpr int kReconnectDelaySeconds = 3;

bool sendAll(int fd, const char* data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, data + sent, len - sent, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        sent += (size_t)n;
    }
    return true;
}

bool recvAll(int fd, char* data, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, data + got, len - got, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (n == 0)
            return false;
        got += (size_t)n;
    }
    return true;
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
                              const std::string& password)
{
    std::string variable;
    appendMqttString(&variable, "MQTT");
    variable.push_back(4);      // MQTT 3.1.1
    unsigned char flags = 0x02; // clean session
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

std::string makeSubscribePacket(const std::string& topic)
{
    static unsigned short packet_id = 1;
    std::string variable;
    appendUint16(&variable, packet_id++);

    std::string payload;
    appendMqttString(&payload, topic);
    payload.push_back(0x00);  // QoS 0

    return makeFixedHeader(0x82, variable.size() + payload.size()) +
           variable + payload;
}

std::string makePublishPacket(const std::string& topic,
                              const std::string& payload)
{
    std::string variable;
    appendMqttString(&variable, topic);
    return makeFixedHeader(0x30, variable.size() + payload.size()) +
           variable + payload;
}

std::string extractJsonObject(const std::string& json, const std::string& key)
{
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos)
        return "";
    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos)
        return "";
    pos = json.find('{', pos + 1);
    if (pos == std::string::npos)
        return "";

    int depth = 0;
    bool in_string = false;
    bool escaped = false;
    for (size_t i = pos; i < json.size(); ++i) {
        const char ch = json[i];
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }

        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            ++depth;
        } else if (ch == '}') {
            --depth;
            if (depth == 0)
                return json.substr(pos, i - pos + 1);
        }
    }
    return "";
}

std::string fileExtensionLower(const std::string& path)
{
    const size_t dot = path.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= path.size())
        return "";
    std::string ext = path.substr(dot + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return (char)std::tolower(c);
    });
    return ext;
}

}  // namespace

MqttClient::MqttClient()
    : enabled_(getEnvBool("MQTT_ENABLED", true)),
      provisioning_enabled_(getEnvBool("MQTT_PROVISIONING_ENABLED", false)),
      local_host_(getEnvOrDefault("MQTT_HOST", "127.0.0.1")),
      local_port_(getEnvInt("MQTT_PORT", 1883)),
      provisioning_host_(getEnvOrDefault(
          "MQTT_PROVISIONING_HOST",
          getEnvOrDefault("MQTT_HOST", "127.0.0.1"))),
      provisioning_port_(getEnvInt(
          "MQTT_PROVISIONING_PORT",
          getEnvInt("MQTT_PORT", 1883))),
      client_id_(getEnvOrDefault("MQTT_CLIENT_ID",
                                 "luckfox-face-" + generateUuid())),
      device_id_(getEnvOrDefault("MQTT_DEVICE_ID", getEth0Mac())),
      mac_(getEth0Mac()),
      serial_number_(getEnvOrDefault("MQTT_SERIAL_NUMBER", "LF-CAM-000001")),
      device_model_(getEnvOrDefault("MQTT_DEVICE_MODEL", "camera_001")),
      firmware_version_(getEnvOrDefault("MQTT_FIRMWARE_VERSION", "1.0.0")),
      status_interval_seconds_(
          std::max(0, getEnvInt("MQTT_STATUS_INTERVAL_SECONDS", 60))),
      device_secret_(getEnvOrDefault("MQTT_DEVICE_SECRET",
                                     "luckfox-dev-secret")),
      credential_path_(getEnvOrDefault("MQTT_CREDENTIAL_PATH",
                                       "/data/device/credential.json")),
      offline_queue_path_(getEnvOrDefault("MQTT_OFFLINE_QUEUE_PATH",
                                          "/data/device/offline_events.jsonl")),
      request_topic_(getEnvOrDefault(
          "MQTT_REQUEST_TOPIC",
          getEnvOrDefault("MQTT_TOPIC_PREFIX", "x86-lite/local/device") +
              "/" + getEnvOrDefault("MQTT_TOPIC_DEVICE", "mac") +
              "/request")),
      response_topic_(getEnvOrDefault(
          "MQTT_RESPONSE_TOPIC",
          getEnvOrDefault("MQTT_TOPIC_PREFIX", "x86-lite/local/device") +
              "/" + getEnvOrDefault("MQTT_TOPIC_DEVICE", "mac") +
              "/response")),
      event_topic_(getEnvOrDefault(
          "MQTT_EVENT_TOPIC",
          getEnvOrDefault("MQTT_TOPIC_PREFIX", "x86-lite/local/device") +
              "/" + getEnvOrDefault("MQTT_TOPIC_DEVICE", "mac") +
              "/event")),
      status_topic_(getEnvOrDefault(
          "MQTT_STATUS_TOPIC",
          getEnvOrDefault("MQTT_TOPIC_PREFIX", "x86-lite/local/device") +
              "/" + getEnvOrDefault("MQTT_TOPIC_DEVICE", "mac") +
              "/status")),
      active_host_(local_host_),
      active_port_(local_port_),
      active_username_(),
      active_password_(),
      active_subscribe_topic_(request_topic_),
      pending_uuid_(),
      provisioning_response_topic_(),
      next_status_check_(std::chrono::steady_clock::now()),
      next_status_publish_(std::chrono::steady_clock::now()),
      include_image_base64_(getEnvBool("MQTT_INCLUDE_IMAGE_BASE64", false)),
      running_(false),
      connected_(false),
      socket_fd_(-1),
      mode_(Mode::LOCAL_ANONYMOUS)
{
    if (provisioning_enabled_) {
        if (loadCredentialFile(credential_path_, &credential_)) {
            mode_ = Mode::PRODUCTION;
            device_id_ = credential_.device_uid;
            active_host_ = credential_.mqtt_host;
            active_port_ = credential_.mqtt_port;
            active_username_ = credential_.mqtt_user;
            active_password_ = credential_.mqtt_pass;
            request_topic_ = credential_.topic_command;
            response_topic_ = credential_.topic_ack;
            event_topic_ = credential_.topic_event;
            status_topic_ = credential_.topic_status;
            active_subscribe_topic_ = request_topic_;
            if (credential_.mqtt_tls) {
                printf("[mqtt] warning: mqtt_tls=true in credential, but this client currently uses plain TCP MQTT\n");
            }
        } else {
            mode_ = Mode::PROVISIONING_REGISTERING;
            active_host_ = provisioning_host_;
            active_port_ = provisioning_port_;
            pending_uuid_ = generateUuid();
            provisioning_response_topic_ =
                getEnvOrDefault("MQTT_PROVISIONING_RESPONSE_TOPIC", "") ;
            if (provisioning_response_topic_.empty()) {
                provisioning_response_topic_ =
                    "provisioning/v1/register/response/" + pending_uuid_;
            }
            active_subscribe_topic_ = provisioning_response_topic_;
        }
    }

    printf("[mqtt] init enabled=%s provisioning=%s broker=%s:%d mode=%d subscribe=%s response=%s event=%s status=%s status_interval=%ds device_id=%s credential=%s\n",
           enabled_ ? "true" : "false",
           provisioning_enabled_ ? "true" : "false",
           active_host_.c_str(), active_port_, (int)mode_,
           active_subscribe_topic_.c_str(), response_topic_.c_str(),
           event_topic_.c_str(), status_topic_.c_str(),
           status_interval_seconds_, device_id_.c_str(),
           credential_path_.c_str());
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
    disconnectBroker();
    if (worker_.joinable())
        worker_.join();
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
    if (!enabled_)
        return false;
    const std::string event = buildRecognitionEvent(payload);
    if (publishJson(event_topic_, event))
        return true;

    enqueueOfflineEvent(event);
    return false;
}

void MqttClient::loop()
{
    auto last_ping = std::chrono::steady_clock::now();

    while (running_) {
        if (socket_fd_ < 0) {
            if (!connectBroker()) {
                sleep(kReconnectDelaySeconds);
                continue;
            }
            if (!subscribeTopic(active_subscribe_topic_)) {
                disconnectBroker();
                sleep(kReconnectDelaySeconds);
                continue;
            }
            afterConnected();
            last_ping = std::chrono::steady_clock::now();
        }

        if (mode_ == Mode::PROVISIONING_PENDING &&
            std::chrono::steady_clock::now() >= next_status_check_) {
            publishCheckRegisterStatus();
        }

        const auto status_now = std::chrono::steady_clock::now();
        if ((mode_ == Mode::LOCAL_ANONYMOUS || mode_ == Mode::PRODUCTION) &&
            status_interval_seconds_ > 0 &&
            status_now >= next_status_publish_) {
            publishDeviceOnline();
            next_status_publish_ =
                status_now + std::chrono::seconds(status_interval_seconds_);
        }

        unsigned char packet_type = 0;
        std::string body;
        if (readPacket(&packet_type, &body)) {
            if ((packet_type & 0xf0) == 0x30)
                handlePublish(body);
            last_ping = std::chrono::steady_clock::now();
            continue;
        }

        const auto now = std::chrono::steady_clock::now();
        if (now - last_ping >= std::chrono::seconds(kMqttKeepAliveSeconds / 2)) {
            if (!sendPacket(std::string("\xC0\x00", 2))) {
                disconnectBroker();
            }
            last_ping = now;
        }
    }
}

bool MqttClient::connectBroker()
{
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", active_port_);

    struct addrinfo* result = nullptr;
    int gai = getaddrinfo(active_host_.c_str(), port_buf, &hints, &result);
    if (gai != 0) {
        printf("[mqtt] getaddrinfo failed: %s\n", gai_strerror(gai));
        return false;
    }

    int fd = -1;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
            break;

        close(fd);
        fd = -1;
    }
    freeaddrinfo(result);

    if (fd < 0) {
        printf("[mqtt] cannot connect to %s:%d\n",
               active_host_.c_str(), active_port_);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        socket_fd_ = fd;
    }

    if (!sendPacket(makeConnectPacket(client_id_, active_username_,
                                      active_password_))) {
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
    printf("[mqtt] connected broker=%s:%d user=%s\n",
           active_host_.c_str(), active_port_,
           active_username_.empty() ? "<anonymous>" : active_username_.c_str());
    return true;
}

void MqttClient::disconnectBroker()
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ >= 0) {
        close(socket_fd_);
        socket_fd_ = -1;
    }
    connected_ = false;
}

bool MqttClient::subscribeTopic(const std::string& topic)
{
    if (topic.empty())
        return false;
    if (!sendPacket(makeSubscribePacket(topic)))
        return false;

    unsigned char packet_type = 0;
    std::string body;
    if (!readPacket(&packet_type, &body) || packet_type != 0x90) {
        printf("[mqtt] SUBACK failed\n");
        return false;
    }
    printf("[mqtt] subscribed: %s\n", topic.c_str());
    return true;
}

void MqttClient::afterConnected()
{
    if (mode_ == Mode::PROVISIONING_REGISTERING) {
        publishRegisterDevice();
    } else if (mode_ == Mode::PROVISIONING_PENDING) {
        publishCheckRegisterStatus();
    } else if (mode_ == Mode::LOCAL_ANONYMOUS || mode_ == Mode::PRODUCTION) {
        publishDeviceOnline();
        next_status_publish_ = std::chrono::steady_clock::now() +
                               std::chrono::seconds(status_interval_seconds_);
        if (mode_ == Mode::PRODUCTION)
            flushOfflineEvents();
    }
}

bool MqttClient::readPacket(unsigned char* packet_type, std::string* body)
{
    int fd;
    {
        std::lock_guard<std::mutex> lock(socket_mutex_);
        fd = socket_fd_;
    }
    if (fd < 0)
        return false;

    unsigned char header = 0;
    ssize_t n = recv(fd, &header, 1, 0);
    if (n <= 0)
        return false;

    size_t multiplier = 1;
    size_t remaining_len = 0;
    unsigned char encoded = 0;
    do {
        if (!recvAll(fd, (char*)&encoded, 1))
            return false;
        remaining_len += (encoded & 127) * multiplier;
        multiplier *= 128;
        if (multiplier > 128 * 128 * 128)
            return false;
    } while ((encoded & 128) != 0);

    body->assign(remaining_len, '\0');
    if (remaining_len > 0 && !recvAll(fd, &(*body)[0], remaining_len))
        return false;

    *packet_type = header;
    return true;
}

bool MqttClient::sendPacket(const std::string& packet)
{
    std::lock_guard<std::mutex> lock(socket_mutex_);
    if (socket_fd_ < 0)
        return false;
    return sendAll(socket_fd_, packet.data(), packet.size());
}

bool MqttClient::publishJson(const std::string& topic,
                             const std::string& payload)
{
    if (!enabled_)
        return false;
    if (socket_fd_ < 0)
        return false;

    if (!sendPacket(makePublishPacket(topic, payload))) {
        printf("[mqtt] publish failed topic=%s\n", topic.c_str());
        disconnectBroker();
        return false;
    }

    printf("[mqtt] published topic=%s bytes=%zu\n",
           topic.c_str(), payload.size());
    return true;
}

void MqttClient::handlePublish(const std::string& body)
{
    if (body.size() < 2)
        return;

    const unsigned int topic_len =
        ((unsigned char)body[0] << 8) | (unsigned char)body[1];
    if (body.size() < 2 + topic_len)
        return;

    const std::string topic = body.substr(2, topic_len);
    const std::string payload = body.substr(2 + topic_len);

    if ((mode_ == Mode::PROVISIONING_REGISTERING ||
         mode_ == Mode::PROVISIONING_PENDING) &&
        topic == provisioning_response_topic_) {
        handleProvisioningResponse(payload);
        return;
    }

    if (topic == request_topic_)
        handleCommandRequest(payload);
}

void MqttClient::handleProvisioningResponse(const std::string& payload)
{
    const std::string uuid = jsonGetString(payload, "uuid");
    const std::string command = jsonGetString(payload, "command");
    const std::string code = jsonGetString(payload, "code");
    const bool status = jsonGetBool(payload, "status", false);

    if (!pending_uuid_.empty() && uuid != pending_uuid_) {
        printf("[mqtt-provision] ignore response uuid=%s expected=%s\n",
               uuid.c_str(), pending_uuid_.c_str());
        return;
    }

    if (!status) {
        printf("[mqtt-provision] failed command=%s code=%s message=%s\n",
               command.c_str(), code.c_str(),
               jsonGetString(payload, "message").c_str());
        if (code == "DEVICE_REJECTED")
            mode_ = Mode::LOCAL_ANONYMOUS;
        return;
    }

    if (code == "WAITING_USER_APPROVAL") {
        mode_ = Mode::PROVISIONING_PENDING;
        const int retry_after =
            jsonGetInt(payload, "retry_after_seconds", 30);
        next_status_check_ = std::chrono::steady_clock::now() +
                             std::chrono::seconds(retry_after);
        printf("[mqtt-provision] waiting for approval, retry_after=%d seconds\n",
               retry_after);
        return;
    }

    if (code != "DEVICE_APPROVED")
        return;

    MqttDeviceCredential credential;
    credential.valid = true;
    credential.device_uid = jsonGetString(payload, "device_uid");
    credential.name = jsonGetString(payload, "name");
    credential.mqtt_host = jsonGetString(payload, "mqtt_host");
    credential.mqtt_port = jsonGetInt(payload, "mqtt_port", 1883);
    credential.mqtt_tls = jsonGetBool(payload, "mqtt_tls", false);
    credential.mqtt_user = jsonGetString(payload, "mqtt_user");
    credential.mqtt_pass = jsonGetString(payload, "mqtt_pass");
    const std::string topics = extractJsonObject(payload, "topics");
    credential.topic_status = jsonGetString(topics, "status");
    credential.topic_telemetry = jsonGetString(topics, "telemetry");
    credential.topic_event = jsonGetString(topics, "event");
    credential.topic_ack = jsonGetString(topics, "ack");
    credential.topic_command = jsonGetString(topics, "command");
    credential.topic_config = jsonGetString(topics, "config");
    credential.topic_ota = jsonGetString(topics, "ota");

    if (credential.device_uid.empty() || credential.mqtt_host.empty() ||
        credential.mqtt_user.empty() || credential.mqtt_pass.empty() ||
        credential.topic_command.empty() || credential.topic_event.empty()) {
        printf("[mqtt-provision] approved response is missing required credential fields\n");
        return;
    }

    if (!saveCredentialFile(credential_path_, credential)) {
        printf("[mqtt-provision] cannot save credential: %s\n",
               credential_path_.c_str());
        return;
    }

    printf("[mqtt-provision] device approved, credential saved: %s\n",
           credential_path_.c_str());
    switchToProduction(credential);
}

void MqttClient::handleCommandRequest(const std::string& payload)
{
    const std::string command = jsonGetString(payload, "command");
    if (command == "delete_face") {
        DeleteFaceRequest request;
        request.uuid = jsonGetString(payload, "uuid");
        request.timestamp = jsonGetString(payload, "timestamp");
        request.device_id = jsonGetString(payload, "device_id");
        request.command = command;
        request.employee_id = jsonGetString(payload, "employee_id");

        DeleteFaceResponse response;
        DeleteHandler handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = delete_handler_;
        }
        if (!handler) {
            response.status = false;
            response.message = "delete handler not ready";
        } else {
            response = handler(request);
        }
        publishJson(response_topic_, buildDeleteResponse(request, response));
        return;
    }

    RegisterFaceRequest request;
    request.uuid = jsonGetString(payload, "uuid");
    request.timestamp = jsonGetString(payload, "timestamp");
    request.device_id = jsonGetString(payload, "device_id");
    request.command = command;
    request.employee_id = jsonGetString(payload, "employee_id");
    request.name = jsonGetString(payload, "name");
    if (request.name.empty())
        request.name = jsonGetString(payload, "person_name");
    request.face_link = jsonGetString(payload, "face_link");
    request.audio_link = jsonGetString(payload, "audio_link");

    RegisterFaceResponse response;
    if (request.command != "register_face") {
        response.status = false;
        response.message = "unsupported command";
    } else {
        RegisterHandler handler;
        {
            std::lock_guard<std::mutex> lock(handler_mutex_);
            handler = register_handler_;
        }
        if (!handler) {
            response.status = false;
            response.message = "register handler not ready";
        } else {
            response = handler(request);
        }
    }

    publishJson(response_topic_, buildRegisterResponse(request, response));
}

void MqttClient::publishRegisterDevice()
{
    if (pending_uuid_.empty())
        pending_uuid_ = generateUuid();
    const std::string payload =
        buildRegisterDeviceRequest(pending_uuid_, "register_device");
    publishJson("provisioning/v1/register/request", payload);
    printf("[mqtt-provision] register_device published uuid=%s reply_to=%s\n",
           pending_uuid_.c_str(), provisioning_response_topic_.c_str());
}

void MqttClient::publishCheckRegisterStatus()
{
    if (pending_uuid_.empty())
        pending_uuid_ = generateUuid();
    const std::string payload =
        buildRegisterDeviceRequest(pending_uuid_, "check_register_status");
    publishJson("provisioning/v1/register/request", payload);
    next_status_check_ = std::chrono::steady_clock::now() +
                         std::chrono::seconds(30);
    printf("[mqtt-provision] check_register_status published uuid=%s\n",
           pending_uuid_.c_str());
}

bool MqttClient::publishDeviceOnline()
{
    if (status_topic_.empty())
        return false;
    return publishJson(status_topic_, buildDeviceOnlineEvent());
}

void MqttClient::flushOfflineEvents()
{
    std::ifstream in(offline_queue_path_.c_str());
    if (!in.good())
        return;

    std::vector<std::string> events;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            events.push_back(line);
    }
    in.close();

    if (events.empty()) {
        unlink(offline_queue_path_.c_str());
        return;
    }

    std::vector<std::string> failed;
    for (const std::string& event : events) {
        if (!publishJson(event_topic_, event))
            failed.push_back(event);
    }

    if (failed.empty()) {
        unlink(offline_queue_path_.c_str());
        printf("[mqtt] offline event queue flushed\n");
        return;
    }

    const std::string tmp = offline_queue_path_ + ".tmp";
    ensureParentDirectory(tmp);
    std::ofstream out(tmp.c_str(), std::ios::out | std::ios::trunc);
    for (const std::string& event : failed)
        out << event << '\n';
    out.close();
    rename(tmp.c_str(), offline_queue_path_.c_str());
}

void MqttClient::enqueueOfflineEvent(const std::string& payload)
{
    if (!ensureParentDirectory(offline_queue_path_))
        return;
    std::ofstream out(offline_queue_path_.c_str(),
                      std::ios::out | std::ios::app);
    if (!out.good()) {
        printf("[mqtt] cannot open offline queue: %s\n",
               offline_queue_path_.c_str());
        return;
    }
    out << payload << '\n';
    printf("[mqtt] event queued offline: %s\n", offline_queue_path_.c_str());
}

void MqttClient::switchToProduction(const MqttDeviceCredential& credential)
{
    credential_ = credential;
    mode_ = Mode::PRODUCTION;
    device_id_ = credential_.device_uid;
    active_host_ = credential_.mqtt_host;
    active_port_ = credential_.mqtt_port;
    active_username_ = credential_.mqtt_user;
    active_password_ = credential_.mqtt_pass;
    request_topic_ = credential_.topic_command;
    response_topic_ = credential_.topic_ack;
    event_topic_ = credential_.topic_event;
    status_topic_ = credential_.topic_status;
    active_subscribe_topic_ = request_topic_;
    if (credential_.mqtt_tls) {
        printf("[mqtt] warning: mqtt_tls=true in credential, but this client currently uses plain TCP MQTT\n");
    }
    disconnectBroker();
}

std::string MqttClient::buildRegisterResponse(
    const RegisterFaceRequest& request,
    const RegisterFaceResponse& response)
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(request.uuid.empty() ? generateUuid() : request.uuid) << "\",";
    out << "\"timestamp\":\"" << jsonEscape(nowIsoUtc()) << "\",";
    out << "\"device_id\":\"" << jsonEscape(device_id_) << "\",";
    out << "\"command\":\""
        << jsonEscape(request.command.empty() ? "register_face"
                                              : request.command)
        << "\",";
    out << "\"message\":\"" << jsonEscape(response.message) << "\",";
    out << "\"status\":" << (response.status ? "true" : "false") << ",";
    out << "\"data\":{";
    out << "\"employee_id\":\"" << jsonEscape(request.employee_id) << "\",";
    out << "\"name\":\"" << jsonEscape(request.name) << "\",";
    out << "\"face_link\":\"" << jsonEscape(request.face_link) << "\",";
    out << "\"audio_link\":\"" << jsonEscape(request.audio_link) << "\"";
    out << "}}";
    return out.str();
}

std::string MqttClient::buildDeleteResponse(
    const DeleteFaceRequest& request,
    const DeleteFaceResponse& response)
{
    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\""
        << jsonEscape(request.uuid.empty() ? generateUuid() : request.uuid)
        << "\",";
    out << "\"timestamp\":\"" << jsonEscape(nowIsoUtc()) << "\",";
    out << "\"device_id\":\"" << jsonEscape(device_id_) << "\",";
    out << "\"command\":\"delete_face\",";
    out << "\"message\":\"" << jsonEscape(response.message) << "\",";
    out << "\"status\":" << (response.status ? "true" : "false") << ",";
    out << "\"data\":{";
    out << "\"employee_id\":\"" << jsonEscape(request.employee_id) << "\"";
    out << "}}";
    return out.str();
}

std::string MqttClient::buildRecognitionEvent(
    const MqttRecognitionPayload& payload)
{
    const std::string image_base64 =
        include_image_base64_ ? fileToBase64(payload.image_path) : "";

    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(generateUuid()) << "\",";
    out << "\"timestamp\":\"" << jsonEscape(nowIsoUtc()) << "\",";
    out << "\"device_id\":\"" << jsonEscape(device_id_) << "\",";
    if (mode_ == Mode::PRODUCTION) {
        out << "\"command\":\"attendance_success\",";
        out << "\"message\":\"attendance verified successfully\",";
    } else {
        out << "\"command\":\"face_recognized\",";
        out << "\"message\":\"recognition success\",";
    }
    out << "\"status\":true,";
    out << "\"data\":{";
    if (mode_ == Mode::PRODUCTION) {
        out << "\"person_id\":\"" << jsonEscape(payload.person_id) << "\",";
        out << "\"person_name\":\"" << jsonEscape(payload.name) << "\",";
        out << "\"employee_id\":\"" << jsonEscape(payload.employee_id) << "\",";
        out << "\"confidence\":" << payload.confidence << ",";
        out << "\"distance\":" << payload.distance << ",";
        out << "\"liveness_score\":" << payload.liveness_score << ",";
        out << "\"camera_id\":\"cam_001\",";
        out << "\"capture_time\":\"" << jsonEscape(nowIsoUtc()) << "\",";
        out << "\"image\":{";
        out << "\"type\":\"" << (include_image_base64_ ? "base64" : "path") << "\",";
        out << "\"format\":\"" << jsonEscape(fileExtensionLower(payload.image_path)) << "\",";
        out << "\"content\":\""
            << jsonEscape(include_image_base64_ ? image_base64
                                                : payload.image_path)
            << "\"}";
    } else {
        out << "\"face_link\":\"" << jsonEscape(payload.image_path) << "\",";
        out << "\"name\":\"" << jsonEscape(payload.name) << "\",";
        out << "\"person_id\":\"" << jsonEscape(payload.person_id) << "\",";
        out << "\"employee_id\":\"" << jsonEscape(payload.employee_id) << "\",";
        out << "\"time\":\"" << jsonEscape(payload.time) << "\",";
        out << "\"confidence\":" << payload.confidence << ",";
        out << "\"distance\":" << payload.distance;
        if (include_image_base64_) {
            out << ",\"image_base64\":\"" << image_base64 << "\"";
        }
    }
    out << "}}";
    return out.str();
}

std::string MqttClient::buildRegisterDeviceRequest(const std::string& uuid,
                                                   const std::string& command)
{
    const std::string timestamp = nowIsoUtc();
    const std::string response_topic =
        provisioning_response_topic_.empty()
            ? "provisioning/v1/register/response/" + uuid
            : provisioning_response_topic_;

    std::ostringstream data;
    data << "{";
    data << "\"serial_number\":\"" << jsonEscape(serial_number_) << "\",";
    data << "\"model\":\"" << jsonEscape(device_model_) << "\",";
    data << "\"type\":\"facial_scanner\",";
    data << "\"hardware\":\"luckfox\",";
    data << "\"mac\":\"" << jsonEscape(mac_) << "\",";
    data << "\"firmware_version\":\"" << jsonEscape(firmware_version_) << "\",";
    data << "\"capabilities\":[\"camera\",\"face_register\",\"face_attendance\",\"mqtt\"],";
    data << "\"reply_to\":\"" << jsonEscape(response_topic) << "\"";
    data << "}";
    const std::string data_json = data.str();

    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(uuid) << "\",";
    out << "\"timestamp\":\"" << jsonEscape(timestamp) << "\",";
    out << "\"device_id\":\"" << jsonEscape(device_id_) << "\",";
    out << "\"command\":\"" << jsonEscape(command) << "\",";
    out << "\"data\":" << data_json << ",";
    out << "\"auth\":" << buildAuthJson(uuid, timestamp, command, data_json);
    out << "}";
    return out.str();
}

std::string MqttClient::buildDeviceOnlineEvent()
{
    struct sysinfo system_info;
    memset(&system_info, 0, sizeof(system_info));
    const long uptime_seconds = sysinfo(&system_info) == 0
                                    ? std::max(0L, system_info.uptime)
                                    : 0L;

    std::ostringstream data;
    data << "{";
    data << "\"device_uid\":\"" << jsonEscape(device_id_) << "\",";
    data << "\"serial_number\":\"" << jsonEscape(serial_number_) << "\",";
    data << "\"model\":\"" << jsonEscape(device_model_) << "\",";
    data << "\"mac\":\"" << jsonEscape(mac_) << "\",";
    data << "\"ip\":\"" << jsonEscape(getIpAddress()) << "\",";
    data << "\"firmware_version\":\"" << jsonEscape(firmware_version_) << "\",";
    data << "\"uptime\":" << uptime_seconds << ",";
    data << "\"state\":\"running\"";
    data << "}";

    std::ostringstream out;
    out << "{";
    out << "\"uuid\":\"" << jsonEscape(generateUuid()) << "\",";
    out << "\"timestamp\":\"" << jsonEscape(nowIsoUtc()) << "\",";
    out << "\"device_id\":\"" << jsonEscape(device_id_) << "\",";
    out << "\"command\":\"device_online\",";
    out << "\"message\":\"device is online\",";
    out << "\"status\":true,";
    out << "\"data\":" << data.str();
    out << "}";
    return out.str();
}

std::string MqttClient::buildAuthJson(const std::string& uuid,
                                      const std::string& timestamp,
                                      const std::string& command,
                                      const std::string& data_json)
{
    const std::string nonce = generateUuid();
    const std::string data_hash = sha256Hex(data_json);
    const std::string canonical = uuid + "|" + timestamp + "|" + device_id_ +
                                  "|" + command + "|" + nonce + "|" +
                                  data_hash;
    const std::string signature = hmacSha256Hex(device_secret_, canonical);

    std::ostringstream out;
    out << "{";
    out << "\"alg\":\"HMAC-SHA256\",";
    out << "\"key_id\":\"" << jsonEscape(serial_number_) << "\",";
    out << "\"nonce\":\"" << jsonEscape(nonce) << "\",";
    out << "\"data_hash\":\"" << jsonEscape(data_hash) << "\",";
    out << "\"signature\":\"" << jsonEscape(signature) << "\"";
    out << "}";
    return out.str();
}

std::string MqttClient::getEnvOrDefault(const char* name,
                                        const std::string& fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return value;
}

bool MqttClient::getEnvBool(const char* name, bool fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

int MqttClient::getEnvInt(const char* name, int fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    char* end = nullptr;
    long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0')
        return fallback;
    return (int)parsed;
}

std::string MqttClient::getEth0Mac()
{
    std::ifstream in("/sys/class/net/eth0/address");
    std::string mac;
    if (std::getline(in, mac) && !mac.empty())
        return mac;
    return "unknown";
}

std::string MqttClient::getIpAddress()
{
    struct ifaddrs* ifaddr = nullptr;
    if (getifaddrs(&ifaddr) != 0)
        return "";

    std::string ip;
    for (struct ifaddrs* ifa = ifaddr; ifa != nullptr; ifa = ifa->ifa_next) {
        if (!ifa->ifa_addr || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;

        char host[NI_MAXHOST];
        int ret = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in),
                              host, sizeof(host), nullptr, 0,
                              NI_NUMERICHOST);
        if (ret == 0) {
            ip = host;
            break;
        }
    }
    freeifaddrs(ifaddr);
    return ip;
}

std::string MqttClient::nowIsoUtc()
{
    time_t t = time(nullptr);
    struct tm tmv;
    gmtime_r(&t, &tmv);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return std::string(buf);
}

std::string MqttClient::generateUuid()
{
    static std::mutex mutex;
    static std::mt19937 rng((unsigned int)time(nullptr));
    std::lock_guard<std::mutex> lock(mutex);

    unsigned char bytes[16];
    for (unsigned char& byte : bytes)
        byte = (unsigned char)(rng() & 0xff);
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;

    char buf[37];
    snprintf(buf, sizeof(buf),
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
             bytes[0], bytes[1], bytes[2], bytes[3],
             bytes[4], bytes[5], bytes[6], bytes[7],
             bytes[8], bytes[9], bytes[10], bytes[11],
             bytes[12], bytes[13], bytes[14], bytes[15]);
    return std::string(buf);
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
                out << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                    << (int)(unsigned char)ch << std::dec;
            } else {
                out << ch;
            }
            break;
        }
    }
    return out.str();
}

std::string MqttClient::jsonGetString(const std::string& json,
                                      const std::string& key)
{
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos)
        return "";

    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos)
        return "";

    ++pos;
    while (pos < json.size() && isspace((unsigned char)json[pos]))
        ++pos;
    if (pos >= json.size() || json[pos] != '"')
        return "";
    ++pos;

    std::string out;
    while (pos < json.size()) {
        char ch = json[pos++];
        if (ch == '"')
            break;
        if (ch == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            default: out.push_back(esc); break;
            }
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

int MqttClient::jsonGetInt(const std::string& json,
                           const std::string& key,
                           int fallback)
{
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos)
        return fallback;
    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos)
        return fallback;
    ++pos;
    while (pos < json.size() && isspace((unsigned char)json[pos]))
        ++pos;

    char* end = nullptr;
    long parsed = strtol(json.c_str() + pos, &end, 10);
    if (!end || end == json.c_str() + pos)
        return fallback;
    return (int)parsed;
}

bool MqttClient::jsonGetBool(const std::string& json,
                             const std::string& key,
                             bool fallback)
{
    const std::string quoted_key = "\"" + key + "\"";
    size_t pos = json.find(quoted_key);
    if (pos == std::string::npos)
        return fallback;
    pos = json.find(':', pos + quoted_key.size());
    if (pos == std::string::npos)
        return fallback;
    ++pos;
    while (pos < json.size() && isspace((unsigned char)json[pos]))
        ++pos;

    if (json.compare(pos, 4, "true") == 0)
        return true;
    if (json.compare(pos, 5, "false") == 0)
        return false;
    return fallback;
}

std::string MqttClient::fileToBase64(const std::string& path)
{
    static const char table[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.good())
        return "";

    std::ostringstream bytes;
    bytes << in.rdbuf();
    const std::string input = bytes.str();

    std::string out;
    out.reserve(((input.size() + 2) / 3) * 4);

    for (size_t i = 0; i < input.size(); i += 3) {
        unsigned int value = (unsigned char)input[i] << 16;
        if (i + 1 < input.size())
            value |= (unsigned char)input[i + 1] << 8;
        if (i + 2 < input.size())
            value |= (unsigned char)input[i + 2];

        out.push_back(table[(value >> 18) & 0x3f]);
        out.push_back(table[(value >> 12) & 0x3f]);
        out.push_back(i + 1 < input.size() ? table[(value >> 6) & 0x3f] : '=');
        out.push_back(i + 2 < input.size() ? table[value & 0x3f] : '=');
    }
    return out;
}

std::string MqttClient::sanitizeTopicPart(const std::string& value)
{
    std::string out = value;
    for (char& ch : out) {
        unsigned char c = (unsigned char)ch;
        if (!isalnum(c) && ch != '_' && ch != '-' && ch != '.')
            ch = '_';
    }
    return out.empty() ? "unknown" : out;
}

bool MqttClient::ensureParentDirectory(const std::string& path)
{
    const size_t slash = path.find_last_of('/');
    if (slash == std::string::npos)
        return true;

    const std::string dir = path.substr(0, slash);
    if (dir.empty())
        return true;

    std::string current;
    if (dir[0] == '/')
        current = "/";

    size_t pos = (dir[0] == '/') ? 1 : 0;
    while (pos <= dir.size()) {
        size_t next = dir.find('/', pos);
        std::string part = dir.substr(pos, next - pos);
        if (!part.empty()) {
            current = (current == "/" || current.empty())
                          ? current + part
                          : current + "/" + part;
            struct stat st;
            if (stat(current.c_str(), &st) != 0) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST)
                    return false;
            } else if (!S_ISDIR(st.st_mode)) {
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
    if (!ensureParentDirectory(path))
        return false;

    const std::string tmp = path + ".tmp";
    std::ofstream out(tmp.c_str(), std::ios::out | std::ios::trunc);
    if (!out.good())
        return false;
    out << content;
    out.close();
    if (!out.good())
        return false;
    chmod(tmp.c_str(), mode);
    if (rename(tmp.c_str(), path.c_str()) != 0)
        return false;
    chmod(path.c_str(), mode);
    return true;
}

bool MqttClient::loadCredentialFile(const std::string& path,
                                    MqttDeviceCredential* credential)
{
    if (!credential)
        return false;

    std::ifstream in(path.c_str());
    if (!in.good())
        return false;
    std::ostringstream ss;
    ss << in.rdbuf();
    const std::string json = ss.str();

    credential->device_uid = jsonGetString(json, "device_uid");
    credential->name = jsonGetString(json, "name");
    credential->mqtt_host = jsonGetString(json, "mqtt_host");
    credential->mqtt_port = jsonGetInt(json, "mqtt_port", 1883);
    credential->mqtt_tls = jsonGetBool(json, "mqtt_tls", false);
    credential->mqtt_user = jsonGetString(json, "mqtt_user");
    credential->mqtt_pass = jsonGetString(json, "mqtt_pass");

    const std::string topics = extractJsonObject(json, "topics");
    credential->topic_status = jsonGetString(topics, "status");
    credential->topic_telemetry = jsonGetString(topics, "telemetry");
    credential->topic_event = jsonGetString(topics, "event");
    credential->topic_ack = jsonGetString(topics, "ack");
    credential->topic_command = jsonGetString(topics, "command");
    credential->topic_config = jsonGetString(topics, "config");
    credential->topic_ota = jsonGetString(topics, "ota");

    credential->valid =
        !credential->device_uid.empty() &&
        !credential->mqtt_host.empty() &&
        !credential->mqtt_user.empty() &&
        !credential->mqtt_pass.empty() &&
        !credential->topic_command.empty() &&
        !credential->topic_event.empty();
    return credential->valid;
}

bool MqttClient::saveCredentialFile(const std::string& path,
                                    const MqttDeviceCredential& credential)
{
    std::ostringstream out;
    out << "{\n";
    out << "  \"device_uid\": \"" << jsonEscape(credential.device_uid) << "\",\n";
    out << "  \"name\": \"" << jsonEscape(credential.name) << "\",\n";
    out << "  \"mqtt_host\": \"" << jsonEscape(credential.mqtt_host) << "\",\n";
    out << "  \"mqtt_port\": " << credential.mqtt_port << ",\n";
    out << "  \"mqtt_tls\": " << (credential.mqtt_tls ? "true" : "false") << ",\n";
    out << "  \"mqtt_user\": \"" << jsonEscape(credential.mqtt_user) << "\",\n";
    out << "  \"mqtt_pass\": \"" << jsonEscape(credential.mqtt_pass) << "\",\n";
    out << "  \"topics\": {\n";
    out << "    \"status\": \"" << jsonEscape(credential.topic_status) << "\",\n";
    out << "    \"telemetry\": \"" << jsonEscape(credential.topic_telemetry) << "\",\n";
    out << "    \"event\": \"" << jsonEscape(credential.topic_event) << "\",\n";
    out << "    \"ack\": \"" << jsonEscape(credential.topic_ack) << "\",\n";
    out << "    \"command\": \"" << jsonEscape(credential.topic_command) << "\",\n";
    out << "    \"config\": \"" << jsonEscape(credential.topic_config) << "\",\n";
    out << "    \"ota\": \"" << jsonEscape(credential.topic_ota) << "\"\n";
    out << "  },\n";
    out << "  \"registered_at\": \"" << jsonEscape(nowIsoUtc()) << "\"\n";
    out << "}\n";
    return writeTextFile(path, out.str(), 0600);
}

std::string MqttClient::sha256Hex(const std::string& value)
{
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)value.data(), value.size(), digest);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned char byte : digest)
        out << std::setw(2) << (int)byte;
    return out.str();
}

std::string MqttClient::hmacSha256Hex(const std::string& key,
                                      const std::string& value)
{
    unsigned int len = EVP_MAX_MD_SIZE;
    unsigned char digest[EVP_MAX_MD_SIZE];
    HMAC(EVP_sha256(),
         key.data(), (int)key.size(),
         (const unsigned char*)value.data(), value.size(),
         digest, &len);

    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (unsigned int i = 0; i < len; ++i)
        out << std::setw(2) << (int)digest[i];
    return out.str();
}
