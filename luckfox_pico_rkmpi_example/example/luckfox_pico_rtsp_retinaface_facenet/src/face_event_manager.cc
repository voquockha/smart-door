#include "face_event_manager.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

#include "opencv2/highgui/highgui.hpp"

namespace {
constexpr float kDefaultConfidenceThreshold = 0.80f;
constexpr int kDefaultCooldownSeconds = 10;
constexpr size_t kDefaultQueueCapacity = 32;
constexpr int kHttpTimeoutMs = 3000;
constexpr int kRetryIntervalSeconds = 10;

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
}  // namespace

template <typename T>
FaceEventManager::ThreadSafeQueue<T>::ThreadSafeQueue(size_t capacity)
    : capacity_(capacity), stopped_(false)
{
}

template <typename T>
bool FaceEventManager::ThreadSafeQueue<T>::push(T item)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_ || queue_.size() >= capacity_)
        return false;
    queue_.push(std::move(item));
    cv_.notify_one();
    return true;
}

template <typename T>
bool FaceEventManager::ThreadSafeQueue<T>::popFor(T* out, int timeout_ms)
{
    std::unique_lock<std::mutex> lock(mutex_);
    if (!cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this] {
            return stopped_ || !queue_.empty();
        })) {
        return false;
    }

    if (queue_.empty())
        return false;

    *out = std::move(queue_.front());
    queue_.pop();
    return true;
}

template <typename T>
void FaceEventManager::ThreadSafeQueue<T>::shutdown()
{
    std::lock_guard<std::mutex> lock(mutex_);
    stopped_ = true;
    cv_.notify_all();
}

FaceEventManager::FaceEventManager()
    : confidence_threshold_(kDefaultConfidenceThreshold),
      cooldown_seconds_(kDefaultCooldownSeconds),
      requested_base_dir_(
          getEnvOrDefault("ATTENDANCE_BASE_DIR", "/data/attendance")),
      effective_base_dir_(requested_base_dir_),
      camera_id_(getEnvOrDefault("ATTENDANCE_CAMERA_ID", "cam_01")),
      server_host_(getEnvOrDefault("ATTENDANCE_SERVER_HOST", "127.0.0.1")),
      server_port_(atoi(getEnvOrDefault("ATTENDANCE_SERVER_PORT",
                                        "8080").c_str())),
      server_path_(getEnvOrDefault("ATTENDANCE_SERVER_PATH", "/attendance")),
      work_queue_(kDefaultQueueCapacity),
      running_(true)
{
    if (!ensureDirectory(requested_base_dir_) ||
        !isDirectoryWritable(requested_base_dir_)) {
        effective_base_dir_ = "/tmp/attendance";
        printf("[attendance] storage fallback: %s -> %s\n",
               requested_base_dir_.c_str(), effective_base_dir_.c_str());
        ensureDirectory(effective_base_dir_);
    } else {
        printf("[attendance] storage dir: %s\n", effective_base_dir_.c_str());
    }

    queue_dir_ = joinPath(effective_base_dir_, "queue");
    queue_path_ = joinPath(queue_dir_, "attendance_queue.jsonl");
    ensureDirectory(queue_dir_);
    worker_ = std::thread(&FaceEventManager::workerLoop, this);
}

FaceEventManager::~FaceEventManager()
{
    running_ = false;
    work_queue_.shutdown();
    if (worker_.joinable())
        worker_.join();
}

void FaceEventManager::setAttendanceSuccessCallback(
    AttendanceSuccessCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    attendance_success_callback_ = std::move(callback);
}

void FaceEventManager::setAttendanceDataCallback(AttendanceDataCallback callback)
{
    std::lock_guard<std::mutex> lock(callback_mutex_);
    attendance_data_callback_ = std::move(callback);
}

void FaceEventManager::onFrame(Frame frame, FaceResult result)
{
    if (!isValid(result))
        return;
    if (isCooldown(result.person_id))
        return;

    handleEvent(frame, result);
}

bool FaceEventManager::isValid(FaceResult r)
{
    if (!r.recognized)
        return false;
    if (r.person_id.empty() || r.name.empty())
        return false;
    if (r.name == "UNKNOWN")
        return false;
    return r.confidence >= confidence_threshold_;
}

bool FaceEventManager::isCooldown(std::string person_id)
{
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(cooldown_mutex_);

    auto it = cooldown_.find(person_id);
    if (it == cooldown_.end())
        return false;

    const auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(now - it->second)
            .count();
    return elapsed < cooldown_seconds_;
}

void FaceEventManager::markCooldown(std::string person_id)
{
    std::lock_guard<std::mutex> lock(cooldown_mutex_);
    cooldown_[person_id] = std::chrono::steady_clock::now();
}

void FaceEventManager::handleEvent(Frame frame, FaceResult r)
{
    if (frame.image_bgr.empty()) {
        printf("[attendance] Empty frame, skip event for %s\n", r.name.c_str());
        return;
    }

    AttendanceJson data;
    data.user_id = r.person_id;
    data.name = r.name;
    data.time = nowTime();
    data.confidence = r.confidence;
    data.distance = r.distance;
    data.camera_id = camera_id_;

    const std::string date_dir = joinPath(effective_base_dir_, nowDate());
    const std::string basename =
        sanitizeFilename(r.name) + "_" + compactTime(data.time);

    data.image_path = joinPath(date_dir, basename + ".jpg");

    WorkItem item;
    item.image_bgr = frame.image_bgr.clone();
    item.data = data;
    item.metadata_path = joinPath(date_dir, basename + ".json");
    item.post_payload = postJson(data);

    if (!work_queue_.push(std::move(item))) {
        printf("[attendance] Worker queue full, drop event for %s\n",
               r.name.c_str());
        return;
    }

    markCooldown(r.person_id);
}

bool FaceEventManager::saveImage(Frame frame, std::string path)
{
    if (frame.image_bgr.empty()) {
        printf("[attendance] Cannot save empty image: %s\n", path.c_str());
        return false;
    }

    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(90);

    try {
        if (!cv::imwrite(path, frame.image_bgr, params)) {
            printf("[attendance] Failed to write image: %s\n", path.c_str());
            return false;
        }
    } catch (const cv::Exception& e) {
        printf("[attendance] OpenCV image write error for %s: %s\n",
               path.c_str(), e.what());
        return false;
    }

    printf("[attendance] Image saved: %s\n", path.c_str());
    return true;
}

bool FaceEventManager::saveMetadata(AttendanceJson data, std::string path)
{
    if (!writeTextFile(path, metadataJson(data))) {
        printf("[attendance] Failed to write metadata: %s\n", path.c_str());
        return false;
    }

    printf("[attendance] Metadata saved: %s\n", path.c_str());
    return true;
}

bool FaceEventManager::sendToServer(AttendanceJson data)
{
    return httpPost(server_host_, server_port_, server_path_, postJson(data),
                    kHttpTimeoutMs);
}

void FaceEventManager::workerLoop()
{
    auto last_retry = std::chrono::steady_clock::now()
                    - std::chrono::seconds(kRetryIntervalSeconds);

    while (running_) {
        const auto now = std::chrono::steady_clock::now();
        if (now - last_retry >= std::chrono::seconds(kRetryIntervalSeconds)) {
            retryQueuedEvents();
            last_retry = now;
        }

        WorkItem item;
        if (!work_queue_.popFor(&item, 1000)) {
            continue;
        }

        processWorkItem(std::move(item));
    }

    WorkItem item;
    while (work_queue_.popFor(&item, 0))
        processWorkItem(std::move(item));
}

void FaceEventManager::processWorkItem(WorkItem item)
{
    const std::string date_dir = item.metadata_path.substr(
        0, item.metadata_path.find_last_of('/'));
    if (!ensureDirectory(date_dir) || !isDirectoryWritable(date_dir)) {
        const std::string fallback_base = "/tmp/attendance";
        const std::string fallback_dir = joinPath(fallback_base, nowDate());
        const std::string image_name = item.data.image_path.substr(
            item.data.image_path.find_last_of('/') + 1);
        const std::string json_name = item.metadata_path.substr(
            item.metadata_path.find_last_of('/') + 1);

        ensureDirectory(fallback_dir);
        printf("[attendance] event dir not writable, fallback: %s -> %s\n",
               date_dir.c_str(), fallback_dir.c_str());
        item.data.image_path = joinPath(fallback_dir, image_name);
        item.metadata_path = joinPath(fallback_dir, json_name);
        item.post_payload = postJson(item.data);
    }

    bool image_saved = saveImage(Frame(item.image_bgr), item.data.image_path);
    if (!image_saved) {
        const std::string fallback_base = "/tmp/attendance";
        const std::string fallback_dir = joinPath(fallback_base, nowDate());
        const std::string image_name = item.data.image_path.substr(
            item.data.image_path.find_last_of('/') + 1);
        const std::string json_name = item.metadata_path.substr(
            item.metadata_path.find_last_of('/') + 1);

        ensureDirectory(fallback_dir);
        printf("[attendance] image save fallback: %s\n", fallback_dir.c_str());
        item.data.image_path = joinPath(fallback_dir, image_name);
        item.metadata_path = joinPath(fallback_dir, json_name);
        item.post_payload = postJson(item.data);
        image_saved = saveImage(Frame(item.image_bgr), item.data.image_path);
    }
    saveMetadata(item.data, item.metadata_path);

    if (!sendToServer(item.data)) {
        enqueueFailedPost(item.post_payload);
        printf("[attendance] Server send failed, queued: %s\n",
               item.data.name.c_str());
    } else {
        printf("[attendance] Synced event: %s %s\n",
               item.data.name.c_str(), item.data.time.c_str());
    }

    AttendanceSuccessCallback callback;
    AttendanceDataCallback data_callback;
    {
        std::lock_guard<std::mutex> lock(callback_mutex_);
        callback = attendance_success_callback_;
        data_callback = attendance_data_callback_;
    }
    if (callback)
        callback(item.data.name, item.data.time);
    if (data_callback && image_saved)
        data_callback(item.data, item.data.image_path);
    else if (data_callback)
        printf("[attendance] Skip image callback because evidence image was not saved\n");
}

void FaceEventManager::retryQueuedEvents()
{
    std::ifstream in(queue_path_.c_str());
    if (!in.good())
        return;

    std::vector<std::string> pending;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty())
            pending.push_back(line);
    }
    in.close();

    if (pending.empty()) {
        unlink(queue_path_.c_str());
        return;
    }

    std::vector<std::string> failed;
    for (const std::string& payload : pending) {
        if (!httpPost(server_host_, server_port_, server_path_, payload,
                      kHttpTimeoutMs)) {
            failed.push_back(payload);
        }
    }

    if (failed.empty()) {
        unlink(queue_path_.c_str());
        return;
    }

    const std::string tmp_path = queue_path_ + ".tmp";
    std::ofstream out(tmp_path.c_str(), std::ios::out | std::ios::trunc);
    for (const std::string& payload : failed)
        out << payload << '\n';
    out.close();
    rename(tmp_path.c_str(), queue_path_.c_str());
}

void FaceEventManager::enqueueFailedPost(const std::string& payload)
{
    ensureDirectory(queue_dir_);
    std::ofstream out(queue_path_.c_str(), std::ios::out | std::ios::app);
    if (!out.good()) {
        printf("[attendance] Cannot open queue file: %s\n", queue_path_.c_str());
        return;
    }
    out << payload << '\n';
}

std::string FaceEventManager::getEnvOrDefault(const char* name,
                                              const std::string& fallback)
{
    const char* value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return value;
}

std::string FaceEventManager::nowDate()
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);

    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &tmv);
    return std::string(buf);
}

std::string FaceEventManager::nowTime()
{
    time_t t = time(nullptr);
    struct tm tmv;
    localtime_r(&t, &tmv);

    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tmv);
    return std::string(buf);
}

std::string FaceEventManager::compactTime(const std::string& timestamp)
{
    if (timestamp.size() < 19)
        return "000000";
    std::string out;
    out.reserve(6);
    out.append(timestamp.substr(11, 2));
    out.append(timestamp.substr(14, 2));
    out.append(timestamp.substr(17, 2));
    return out;
}

std::string FaceEventManager::sanitizeFilename(const std::string& name)
{
    std::string out;
    out.reserve(name.size());

    for (char ch : name) {
        unsigned char c = (unsigned char)ch;
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9')) {
            out.push_back((char)c);
        } else if (c == '_' || c == '-') {
            out.push_back((char)c);
        } else if (c == ' ' || c == '\t') {
            continue;
        } else {
            out.push_back('_');
        }
    }

    if (out.empty())
        out = "person";
    return out;
}

std::string FaceEventManager::joinPath(const std::string& left,
                                       const std::string& right)
{
    if (left.empty())
        return right;
    if (right.empty())
        return left;
    if (left[left.size() - 1] == '/')
        return left + right;
    return left + "/" + right;
}

bool FaceEventManager::ensureDirectory(const std::string& path)
{
    if (path.empty())
        return false;

    std::string current;
    if (path[0] == '/')
        current = "/";

    size_t pos = (path[0] == '/') ? 1 : 0;
    while (pos <= path.size()) {
        size_t slash = path.find('/', pos);
        std::string part = path.substr(pos, slash - pos);
        if (!part.empty()) {
            current = (current == "/" || current.empty())
                          ? current + part
                          : current + "/" + part;

            struct stat st;
            if (stat(current.c_str(), &st) != 0) {
                if (mkdir(current.c_str(), 0755) != 0 && errno != EEXIST) {
                    printf("[attendance] mkdir failed %s: %s\n",
                           current.c_str(), strerror(errno));
                    return false;
                }
            } else if (!S_ISDIR(st.st_mode)) {
                printf("[attendance] Path is not directory: %s\n",
                       current.c_str());
                return false;
            }
        }

        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    return true;
}

std::string FaceEventManager::jsonEscape(const std::string& value)
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

std::string FaceEventManager::metadataJson(const AttendanceJson& data)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{\n";
    out << "  \"id\": \"" << jsonEscape(data.user_id) << "\",\n";
    out << "  \"name\": \"" << jsonEscape(data.name) << "\",\n";
    out << "  \"time\": \"" << jsonEscape(data.time) << "\",\n";
    out << "  \"confidence\": " << data.confidence << ",\n";
    out << "  \"distance\": " << data.distance << ",\n";
    out << "  \"camera_id\": \"" << jsonEscape(data.camera_id) << "\",\n";
    out << "  \"image_path\": \"" << jsonEscape(data.image_path) << "\"\n";
    out << "}\n";
    return out.str();
}

std::string FaceEventManager::postJson(const AttendanceJson& data)
{
    std::ostringstream out;
    out << std::fixed << std::setprecision(3);
    out << "{";
    out << "\"user_id\":\"" << jsonEscape(data.user_id) << "\",";
    out << "\"name\":\"" << jsonEscape(data.name) << "\",";
    out << "\"time\":\"" << jsonEscape(data.time) << "\",";
    out << "\"confidence\":" << data.confidence << ",";
    out << "\"distance\":" << data.distance << ",";
    out << "\"image_path\":\"" << jsonEscape(data.image_path) << "\"";
    out << "}";
    return out.str();
}

bool FaceEventManager::writeTextFile(const std::string& path,
                                     const std::string& content)
{
    std::ofstream out(path.c_str(), std::ios::out | std::ios::trunc);
    if (!out.good())
        return false;
    out << content;
    return out.good();
}

bool FaceEventManager::isDirectoryWritable(const std::string& path)
{
    if (path.empty())
        return false;
    if (access(path.c_str(), W_OK) == 0)
        return true;
    return false;
}

bool FaceEventManager::httpPost(const std::string& host,
                                int port,
                                const std::string& path,
                                const std::string& payload,
                                int timeout_ms)
{
    if (host.empty() || port <= 0)
        return false;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%d", port);

    struct addrinfo* result = nullptr;
    int gai = getaddrinfo(host.c_str(), port_buf, &hints, &result);
    if (gai != 0)
        return false;

    bool ok = false;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        int fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0)
            continue;

        struct timeval tv;
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        if (connect(fd, rp->ai_addr, rp->ai_addrlen) != 0) {
            close(fd);
            continue;
        }

        std::ostringstream req;
        req << "POST " << path << " HTTP/1.1\r\n";
        req << "Host: " << host << ":" << port << "\r\n";
        req << "Content-Type: application/json\r\n";
        req << "Content-Length: " << payload.size() << "\r\n";
        req << "Connection: close\r\n\r\n";
        req << payload;

        const std::string request = req.str();
        if (!sendAll(fd, request.data(), request.size())) {
            close(fd);
            continue;
        }

        char response[256];
        ssize_t n = recv(fd, response, sizeof(response) - 1, 0);
        close(fd);
        if (n <= 0)
            continue;

        response[n] = '\0';
        int status = 0;
        if (sscanf(response, "HTTP/%*s %d", &status) == 1 &&
            status >= 200 && status < 300) {
            ok = true;
            break;
        }
    }

    freeaddrinfo(result);
    return ok;
}
