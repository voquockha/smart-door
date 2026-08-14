#ifndef LOCAL_HTTP_SERVER_H
#define LOCAL_HTTP_SERVER_H

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct HttpRegisterRequest {
    std::string employee_id;
    std::string name;
    std::string image_path;
    std::string audio_path;
};

struct HttpActionResult {
    bool ok = false;
    std::string message;
};

class LocalHttpServer {
public:
    using RegisterHandler = std::function<HttpActionResult(const HttpRegisterRequest&)>;
    using DeleteHandler = std::function<HttpActionResult(const std::string&)>;
    using EmployeesHandler = std::function<std::string()>;
    using StatusHandler = std::function<std::string()>;

    explicit LocalHttpServer(int port = 8080);
    ~LocalHttpServer();
    void setRegisterHandler(RegisterHandler handler);
    void setDeleteHandler(DeleteHandler handler);
    void setEmployeesHandler(EmployeesHandler handler);
    void setStatusHandler(StatusHandler handler);
    bool start();
    void stop();
    void updateFrame(const unsigned char* bgr, int width, int height);
    void updateRecognition(const std::string& employee_id,
                           const std::string& name,
                           const std::string& time,
                           float confidence, float distance);

private:
    void loop();
    void handleClient(int fd);

    int port_;
    int listen_fd_;
    std::atomic<bool> running_;
    std::thread worker_;
    RegisterHandler register_handler_;
    DeleteHandler delete_handler_;
    EmployeesHandler employees_handler_;
    StatusHandler status_handler_;
    std::mutex frame_mutex_;
    std::vector<unsigned char> latest_frame_;
    int frame_width_ = 0;
    int frame_height_ = 0;
    std::mutex recognition_mutex_;
    std::string latest_recognition_ = "null";
    std::vector<std::string> recognition_history_;
};

#endif
