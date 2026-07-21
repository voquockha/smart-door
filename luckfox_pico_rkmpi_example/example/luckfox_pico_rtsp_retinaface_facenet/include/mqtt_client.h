#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

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
    std::string device_uid;
    std::string name;
    std::string mqtt_host;
    int mqtt_port = 1883;
    bool mqtt_tls = false;
    std::string mqtt_user;
    std::string mqtt_pass;
    std::string topic_status;
    std::string topic_telemetry;
    std::string topic_event;
    std::string topic_ack;
    std::string topic_command;
    std::string topic_config;
    std::string topic_ota;
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
        LOCAL_ANONYMOUS,
        PROVISIONING_REGISTERING,
        PROVISIONING_PENDING,
        PRODUCTION
    };

    void loop();
    bool connectBroker();
    void disconnectBroker();
    bool subscribeTopic(const std::string& topic);
    void afterConnected();
    bool readPacket(unsigned char* packet_type, std::string* body);
    bool sendPacket(const std::string& packet);
    bool publishJson(const std::string& topic, const std::string& payload);
    void handlePublish(const std::string& body);
    void handleProvisioningResponse(const std::string& payload);
    void handleCommandRequest(const std::string& payload);
    void publishRegisterDevice();
    void publishCheckRegisterStatus();
    bool publishDeviceOnline();
    void flushOfflineEvents();
    void enqueueOfflineEvent(const std::string& payload);
    void switchToProduction(const MqttDeviceCredential& credential);
    std::string buildRegisterResponse(const RegisterFaceRequest& request,
                                      const RegisterFaceResponse& response);
    std::string buildDeleteResponse(const DeleteFaceRequest& request,
                                    const DeleteFaceResponse& response);
    std::string buildRecognitionEvent(const MqttRecognitionPayload& payload);
    std::string buildRegisterDeviceRequest(const std::string& uuid,
                                           const std::string& command);
    std::string buildDeviceOnlineEvent();
    std::string buildAuthJson(const std::string& uuid,
                              const std::string& timestamp,
                              const std::string& command,
                              const std::string& data_json);

    static std::string getEnvOrDefault(const char* name,
                                       const std::string& fallback);
    static bool getEnvBool(const char* name, bool fallback);
    static int getEnvInt(const char* name, int fallback);
    static std::string getEth0Mac();
    static std::string getIpAddress();
    static std::string nowIsoUtc();
    static std::string generateUuid();
    static std::string jsonEscape(const std::string& value);
    static std::string jsonGetString(const std::string& json,
                                     const std::string& key);
    static int jsonGetInt(const std::string& json,
                          const std::string& key,
                          int fallback);
    static bool jsonGetBool(const std::string& json,
                            const std::string& key,
                            bool fallback);
    static std::string fileToBase64(const std::string& path);
    static std::string sanitizeTopicPart(const std::string& value);
    static bool ensureParentDirectory(const std::string& path);
    static bool writeTextFile(const std::string& path,
                              const std::string& content,
                              int mode);
    static bool loadCredentialFile(const std::string& path,
                                   MqttDeviceCredential* credential);
    static bool saveCredentialFile(const std::string& path,
                                   const MqttDeviceCredential& credential);
    static std::string sha256Hex(const std::string& value);
    static std::string hmacSha256Hex(const std::string& key,
                                     const std::string& value);

    const bool enabled_;
    const bool provisioning_enabled_;
    const std::string local_host_;
    const int local_port_;
    const std::string provisioning_host_;
    const int provisioning_port_;
    const std::string client_id_;
    std::string device_id_;
    const std::string mac_;
    const std::string serial_number_;
    const std::string device_model_;
    const std::string firmware_version_;
    const int status_interval_seconds_;
    const std::string device_secret_;
    const std::string credential_path_;
    const std::string offline_queue_path_;
    std::string request_topic_;
    std::string response_topic_;
    std::string event_topic_;
    std::string status_topic_;
    std::string active_host_;
    int active_port_;
    std::string active_username_;
    std::string active_password_;
    std::string active_subscribe_topic_;
    std::string pending_uuid_;
    std::string provisioning_response_topic_;
    std::chrono::steady_clock::time_point next_status_check_;
    std::chrono::steady_clock::time_point next_status_publish_;
    const bool include_image_base64_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::thread worker_;
    mutable std::mutex socket_mutex_;
    int socket_fd_;
    Mode mode_;
    MqttDeviceCredential credential_;

    std::mutex handler_mutex_;
    RegisterHandler register_handler_;
    DeleteHandler delete_handler_;
};

#endif /* MQTT_CLIENT_H */
