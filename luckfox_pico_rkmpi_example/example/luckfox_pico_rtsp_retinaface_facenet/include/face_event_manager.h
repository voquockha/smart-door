#ifndef FACE_EVENT_MANAGER_H
#define FACE_EVENT_MANAGER_H

#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>
#include <atomic>

#include "opencv2/core/core.hpp"

struct Frame {
    cv::Mat image_bgr;

    Frame() = default;
    explicit Frame(const cv::Mat& image) : image_bgr(image) {}
};

struct FaceResult {
    bool recognized = false;
    std::string person_id;
    std::string name;
    float confidence = 0.0f;
};

struct AttendanceJson {
    std::string user_id;
    std::string name;
    std::string time;
    float confidence = 0.0f;
    std::string camera_id;
    std::string image_path;
};

using AttendanceData = AttendanceJson;

class FaceEventManager {
public:
    using AttendanceSuccessCallback =
        std::function<void(const std::string& name, const std::string& time)>;
    using AttendanceDataCallback =
        std::function<void(const AttendanceData& data,
                           const std::string& image_path)>;

    FaceEventManager();
    ~FaceEventManager();

    FaceEventManager(const FaceEventManager&) = delete;
    FaceEventManager& operator=(const FaceEventManager&) = delete;

    void onFrame(Frame frame, FaceResult result);
    void setAttendanceSuccessCallback(AttendanceSuccessCallback callback);
    void setAttendanceDataCallback(AttendanceDataCallback callback);

private:
    template <typename T>
    class ThreadSafeQueue {
    public:
        explicit ThreadSafeQueue(size_t capacity);

        bool push(T item);
        bool popFor(T* out, int timeout_ms);
        void shutdown();

    private:
        const size_t capacity_;
        bool stopped_;
        std::mutex mutex_;
        std::condition_variable cv_;
        std::queue<T> queue_;
    };

    struct WorkItem {
        cv::Mat image_bgr;
        AttendanceJson data;
        std::string metadata_path;
        std::string post_payload;
    };

    bool isValid(FaceResult r);
    bool isCooldown(std::string person_id);
    void markCooldown(std::string person_id);
    void handleEvent(Frame frame, FaceResult r);

    void saveImage(Frame frame, std::string path);
    void saveMetadata(AttendanceJson data, std::string path);
    bool sendToServer(AttendanceJson data);

    void workerLoop();
    void processWorkItem(WorkItem item);
    void retryQueuedEvents();
    void enqueueFailedPost(const std::string& payload);

    static std::string getEnvOrDefault(const char* name,
                                       const std::string& fallback);
    static std::string nowDate();
    static std::string nowTime();
    static std::string compactTime(const std::string& timestamp);
    static std::string sanitizeFilename(const std::string& name);
    static std::string joinPath(const std::string& left,
                                const std::string& right);
    static bool ensureDirectory(const std::string& path);
    static std::string jsonEscape(const std::string& value);
    static std::string metadataJson(const AttendanceJson& data);
    static std::string postJson(const AttendanceJson& data);
    static bool writeTextFile(const std::string& path,
                              const std::string& content);
    static bool httpPost(const std::string& host,
                         int port,
                         const std::string& path,
                         const std::string& payload,
                         int timeout_ms);

    const float confidence_threshold_;
    const int cooldown_seconds_;
    const std::string base_dir_;
    const std::string camera_id_;
    const std::string server_host_;
    const int server_port_;
    const std::string server_path_;
    const std::string queue_dir_;
    const std::string queue_path_;

    ThreadSafeQueue<WorkItem> work_queue_;
    std::thread worker_;
    std::atomic<bool> running_;

    std::mutex cooldown_mutex_;
    std::map<std::string, std::chrono::steady_clock::time_point> cooldown_;

    std::mutex callback_mutex_;
    AttendanceSuccessCallback attendance_success_callback_;
    AttendanceDataCallback attendance_data_callback_;
};

#endif /* FACE_EVENT_MANAGER_H */
