#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <atomic>
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

typedef struct ssl_ctx_st SSL_CTX;
typedef struct ssl_st SSL;

struct RegisterFaceRequest {
    std::string uuid;
    std::string timestamp;
    std::string device_id;
    std::string command;
    std::string employee_id;
    std::string name;
    std::string face_link;
    std::string audio_link;
};

struct RegisterFaceResponse {
    bool status = false;
    std::string message;
};

struct DeleteFaceRequest {
    std::string uuid;
    std::string timestamp;
    std::string device_id;
    std::string command;
    std::string employee_id;
};

struct DeleteFaceResponse {
    bool status = false;
    std::string message;
};

struct MqttRecognitionPayload {
    std::string person_id;
    std::string name;
    std::string employee_id;
    std::string time;
    std::string image_path;
    float confidence = 0.0f;
    float distance = -1.0f;
    float liveness_score = 0.0f;
};

struct MqttDeviceCredential {
    bool valid = false;
    int credential_version = 0;
    std::string client_id;
    std::string username;
    std::string password;
    std::string topic_root;
};

class MqttClient {
public:
    using RegisterHandler =
        std::function<RegisterFaceResponse(const RegisterFaceRequest&)>;
    using DeleteHandler =
        std::function<DeleteFaceResponse(const DeleteFaceRequest&)>;

    MqttClient();
    ~MqttClient();

    MqttClient(const MqttClient&) = delete;
    MqttClient& operator=(const MqttClient&) = delete;

    bool isEnabled() const;
    bool start();
    void stop();
    void setRegisterHandler(RegisterHandler handler);
    void setDeleteHandler(DeleteHandler handler);
    bool publishRecognition(const MqttRecognitionPayload& payload);

private:
    enum class Mode {
        BOOTSTRAP,
        PRODUCTION
    };

    void loop();
    bool connectBroker();
    void disconnectBroker();
    bool subscribeTopic(const std::string& topic);
    bool subscribeActiveTopics();
    void afterConnected();
    bool readPacket(unsigned char* packet_type, std::string* body);
    bool sendPacket(const std::string& packet);
    bool publishJson(const std::string& topic,
                     const std::string& payload,
                     unsigned short* packet_id = nullptr,
                     bool duplicate = false);
    bool publishJsonAndWaitAck(const std::string& topic,
                               const std::string& payload);
    void handlePublish(unsigned char packet_type, const std::string& body);
    void handleBootstrapResponse(const std::string& payload);
    void handleCommandRequest(const std::string& payload);
    void handleEventAck(const std::string& payload);
    void publishActivationRequest();
    bool publishDeviceOnline();
    void retryPendingEvents();
    bool enqueuePendingEvent(const std::string& payload);
    void acknowledgePendingEvent(const std::string& uuid);
    void loadCommandCache();
    void cacheCommandResponse(const std::string& uuid,
                              const std::string& payload);
    void switchToProduction(const MqttDeviceCredential& credential);

    bool transportWrite(const char* data, size_t len);
    int transportRead(char* data, size_t len);

    std::string buildRegisterResponse(const RegisterFaceRequest& request,
                                      const RegisterFaceResponse& response);
    std::string buildDeleteResponse(const DeleteFaceRequest& request,
                                    const DeleteFaceResponse& response);
    std::string buildRecognitionEvent(const MqttRecognitionPayload& payload);
    std::string buildActivationRequest() const;
    std::string buildActivationAck(int credential_version) const;
    std::string buildDeviceOnlineEvent() const;

    static std::string getEnvOrDefault(const char* name,
                                       const std::string& fallback);
    static bool getEnvBool(const char* name, bool fallback);
    static int getEnvInt(const char* name, int fallback);
    static std::string getEth0Mac();
    static std::string normalizeMac(const std::string& mac);
    static std::string getIpAddress();
    static std::string nowIsoUtc();
    static std::string generateUuid();
    static std::string jsonEscape(const std::string& value);
    static std::string jsonForLog(const std::string& value);
    static std::string jsonGetString(const std::string& json,
                                     const std::string& key);
    static int jsonGetInt(const std::string& json,
                          const std::string& key,
                          int fallback);
    static bool jsonGetBool(const std::string& json,
                            const std::string& key,
                            bool fallback);
    static std::string extractJsonObject(const std::string& json,
                                         const std::string& key);
    static std::string fileToBase64(const std::string& path);
    static bool ensureParentDirectory(const std::string& path);
    static bool writeTextFile(const std::string& path,
                              const std::string& content,
                              int mode);
    static std::string loadOrCreateInstallationId(
        const std::string& configured_id,
        const std::string& path);
    static bool loadCredentialFile(const std::string& path,
                                   MqttDeviceCredential* credential);
    static bool saveCredentialFile(const std::string& path,
                                   const MqttDeviceCredential& credential);

    const bool enabled_;
    const bool debug_payload_;
    const std::string broker_host_;
    const int broker_port_;
    const bool tls_enabled_;
    const bool verify_server_;
    const std::string ca_file_;
    const std::string bootstrap_username_;
    const std::string bootstrap_password_;
    const std::string mac_;
    const std::string normalized_mac_;
    const std::string serial_number_;
    const std::string device_model_;
    const std::string software_version_;
    const int status_interval_seconds_;
    const int retry_interval_seconds_;
    const std::string credential_path_;
    const std::string credential_fallback_path_;
    const std::string installation_id_path_;
    const std::string installation_id_;
    const std::string offline_queue_path_;
    const std::string command_cache_path_;
    const std::string command_cache_fallback_path_;

    std::string active_client_id_;
    std::string active_username_;
    std::string active_password_;
    std::string bootstrap_request_topic_;
    std::string bootstrap_response_topic_;
    std::string bootstrap_ack_topic_;
    std::string request_topic_;
    std::string ack_topic_;
    std::string response_topic_;
    std::string event_topic_;
    std::string status_topic_;
    std::string activation_uuid_;
    std::chrono::steady_clock::time_point next_activation_retry_;
    std::chrono::steady_clock::time_point next_status_publish_;
    std::chrono::steady_clock::time_point next_event_retry_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<unsigned int> next_packet_id_;
    std::thread worker_;
    mutable std::mutex socket_mutex_;
    int socket_fd_;
    SSL_CTX* ssl_ctx_;
    SSL* ssl_;
    std::atomic<Mode> mode_;
    MqttDeviceCredential credential_;

    std::mutex handler_mutex_;
    RegisterHandler register_handler_;
    DeleteHandler delete_handler_;

    std::mutex event_mutex_;
    std::map<std::string, std::string> command_response_cache_;
    std::deque<std::string> command_response_order_;
};

#endif /* MQTT_CLIENT_H */
