// RTSP RetinaFace + MobileFaceNet — multi-person recognition with registration.
//
// Two modes:
//
//   REGISTER a face into the database:
//     ./exe register <retina_model> <facenet_model> <db_path> <name> <image>
//
//   RUN the RTSP recognition pipeline:
//     ./exe run      <retina_model> <facenet_model> <db_path>
//
// Set USE_FACE_ALIGNMENT to 1 (default) to use 5-point landmark alignment
// before MobileFaceNet, or 0 to fall back to bounding-box letterbox crop.

//export TELEGRAM_BOT_TOKEN="8889721857:AAG3lbVpMTq0Tc2Cli-evuRMjWAGbwN7jrA"
//export TELEGRAM_CHAT_ID="1074873491"
//export TELEGRAM_ALLOW_INSECURE=1

#define USE_FACE_ALIGNMENT 1

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <vector>
#include <algorithm>
#include <map>
#include <mutex>
#include <set>

#include <curl/curl.h>
#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "retinaface_facenet.h"
#include "face_align.h"
#include "face_db.h"
#include "face_event_manager.h"
#include "face_test_runner.h"
#include "local_http_server.h"
#include "telegram_client.h"
#include "attendance_utils.h"
#include "anti_spoof.h"

#include "opencv2/core/core.hpp"
#include "opencv2/highgui/highgui.hpp"
#include "opencv2/imgproc/imgproc.hpp"

// -------------------------------------------------------------------------
// Tunables
// -------------------------------------------------------------------------
#define DISP_WIDTH          720
#define DISP_HEIGHT         480
#define MODEL_WIDTH         640
#define MODEL_HEIGHT        640
#define FACENET_WIDTH       160
#define FACENET_HEIGHT      160
#define FACE_DIST_DEFAULT_THRESHOLD 0.78f
#define FACE_MATCH_DEFAULT_MARGIN 0.10f
#define FACE_MIN_DEFAULT_SIZE_PIXELS 100
#define FACE_CONFIRM_DEFAULT_FRAMES 3
#define FACE_MULTI_DEFAULT_MAX_PEOPLE 5
#define FACE_SINGLE_DEFAULT_MIN_SIZE_PIXELS 180
#define FACE_SINGLE_DEFAULT_CENTER_TOLERANCE 0.20f
#define ANTI_SPOOF_DEFAULT_MODEL "model/minifasnet_v2_80x80.rknn"
#define ANTI_SPOOF_REAL_THRESHOLD 0.80f

static volatile sig_atomic_t g_stop_requested = 0;

static void request_stop(int)
{
    g_stop_requested = 1;
}

static void install_stop_signal_handlers()
{
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = request_stop;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT, &action, nullptr);
    sigaction(SIGTERM, &action, nullptr);

    // RTSP clients may disconnect while the server is sending a packet.
    // Ignore SIGPIPE so a normal VLC/FFprobe disconnect does not terminate
    // the entire attendance application.
    struct sigaction ignore_pipe;
    memset(&ignore_pipe, 0, sizeof(ignore_pipe));
    ignore_pipe.sa_handler = SIG_IGN;
    sigemptyset(&ignore_pipe.sa_mask);
    sigaction(SIGPIPE, &ignore_pipe, nullptr);
}

// -------------------------------------------------------------------------
// Timing helper
// -------------------------------------------------------------------------
static inline long ts_diff_us(const struct timespec& a,
                               const struct timespec& b)
{
    return (b.tv_sec - a.tv_sec) * 1000000L
         + (b.tv_nsec - a.tv_nsec) / 1000L;
}

static inline float confidence_from_face_distance(float dist,
                                                  float threshold)
{
    if (dist >= threshold)
        return 0.0f;

    float normalized = 1.0f - (dist / threshold);
    float confidence = 0.80f + 0.20f * normalized;
    return std::max(0.0f, std::min(confidence, 1.0f));
}

static float env_float(const char* name, float fallback,
                       float minimum, float maximum)
{
    const char* value = getenv(name);
    if (!value || !*value)
        return fallback;
    char* end = nullptr;
    const float parsed = strtof(value, &end);
    if (!end || *end != '\0' || parsed < minimum || parsed > maximum) {
        printf("[config] invalid %s=%s, use %.2f\n",
               name, value, (double)fallback);
        return fallback;
    }
    return parsed;
}

static int env_int(const char* name, int fallback,
                   int minimum, int maximum)
{
    const char* value = getenv(name);
    if (!value || !*value)
        return fallback;
    char* end = nullptr;
    const long parsed = strtol(value, &end, 10);
    if (!end || *end != '\0' || parsed < minimum || parsed > maximum) {
        printf("[config] invalid %s=%s, use %d\n",
               name, value, fallback);
        return fallback;
    }
    return (int)parsed;
}

struct FaceConfirmationState {
    unsigned long long last_frame = 0;
    int consecutive_frames = 0;
};

enum class FaceAttendanceMode {
    Single,
    Multi
};

static void onAttendanceSuccess(const TelegramClient& telegram,
                                const AttendanceData& data,
                                const std::string& image_path)
{
    std::string caption;
    caption.reserve(160);
    caption += "TransID: ";
    caption += generateTransID();
    caption += "\n";
    caption += u8"Họ và tên: ";
    caption += data.name;
    caption += "\n";
    caption += u8"Có mặt lúc: ";
    caption += formatTimeForTelegram(data.time);

    telegram.sendPhoto(image_path, caption);
}

static bool starts_with(const std::string& value, const char *prefix)
{
    const size_t prefix_len = strlen(prefix);
    return value.size() >= prefix_len &&
           value.compare(0, prefix_len, prefix) == 0;
}

static std::string safe_token(std::string value)
{
    for (char& ch : value) {
        unsigned char c = (unsigned char)ch;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            ch = '_';
        }
    }
    if (value.empty())
        value = "face";
    return value;
}

static bool env_bool(const char *name, bool fallback)
{
    const char *value = getenv(name);
    if (!value || value[0] == '\0')
        return fallback;
    return strcmp(value, "1") == 0 ||
           strcmp(value, "true") == 0 ||
           strcmp(value, "TRUE") == 0 ||
           strcmp(value, "yes") == 0 ||
           strcmp(value, "YES") == 0;
}

static size_t write_file_callback(char *ptr,
                                  size_t size,
                                  size_t nmemb,
                                  void *userdata)
{
    FILE *fp = static_cast<FILE *>(userdata);
    return fwrite(ptr, size, nmemb, fp);
}

static bool ensure_directory(const std::string& path)
{
    if (path.empty())
        return false;

    std::string partial;
    partial.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        partial.push_back(path[i]);
        if (path[i] != '/' || partial.size() == 1)
            continue;
        if (mkdir(partial.c_str(), 0755) != 0 && errno != EEXIST)
            return false;
    }
    if (mkdir(path.c_str(), 0755) != 0 && errno != EEXIST)
        return false;

    struct stat st{};
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static bool download_http_file(const std::string& url,
                               const std::string& path,
                               std::string *error)
{
    FILE *fp = fopen(path.c_str(), "wb");
    if (!fp) {
        *error = "cannot create download file";
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        fclose(fp);
        unlink(path.c_str());
        *error = "curl_easy_init failed";
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "luckfox-face-mqtt/1.0");
    const bool verify_file_tls = env_bool("MQTT_FILE_TLS_VERIFY", false);
    if (env_bool("MQTT_FACE_ALLOW_INSECURE", !verify_file_tls)) {
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 0L);
    }

    const CURLcode code = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);
    fclose(fp);

    if (code != CURLE_OK || http_code < 200 || http_code >= 300) {
        unlink(path.c_str());
        *error = std::string("download failed: ") + curl_easy_strerror(code) +
                 " http=" + std::to_string(http_code);
        return false;
    }
    return true;
}

static bool convert_to_wav(const std::string& input_path,
                           const std::string& output_path,
                           std::string *error)
{
    const char *configured = getenv("FACE_AUDIO_FFMPEG");
    const char *ffmpeg = configured && *configured
        ? configured : "/usr/bin/ffmpeg";
    const pid_t pid = fork();
    if (pid < 0) {
        *error = std::string("cannot start ffmpeg: ") + strerror(errno);
        return false;
    }
    if (pid == 0) {
        execl(ffmpeg, ffmpeg, "-loglevel", "error", "-y",
              "-i", input_path.c_str(), "-vn", "-ac", "1",
              "-ar", "16000", "-c:a", "pcm_s16le",
              output_path.c_str(), (char*)nullptr);
        _exit(127);
    }

    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno == EINTR)
            continue;
        *error = std::string("wait ffmpeg failed: ") + strerror(errno);
        return false;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        *error = "audio conversion failed";
        return false;
    }
    return true;
}

static bool download_employee_audio(const std::string& audio_link,
                                    const std::string& employee_id,
                                    const std::string& request_id,
                                    std::string *audio_path,
                                    std::string *error)
{
    if (!audio_path || !error)
        return false;
    if (audio_link.empty()) {
        *error = "audio_link is empty";
        return false;
    }

    const char *configured_dir = getenv("FACE_AUDIO_DIR");
    const std::string audio_dir = configured_dir && *configured_dir
        ? configured_dir : "/root/kha/audio";
    if (!ensure_directory(audio_dir)) {
        *error = "cannot create face audio directory";
        return false;
    }

    const std::string token = safe_token(employee_id);
    std::string input_path = audio_link;
    bool remove_input = false;
    if (starts_with(audio_link, "http://") ||
        starts_with(audio_link, "https://")) {
        static std::once_flag curl_once;
        std::call_once(curl_once, []() {
            curl_global_init(CURL_GLOBAL_DEFAULT);
        });
        input_path = "/tmp/mqtt_register_audio_" +
                     safe_token(request_id) + ".input";
        if (!download_http_file(audio_link, input_path, error))
            return false;
        remove_input = true;
    } else if (access(input_path.c_str(), R_OK) != 0) {
        *error = "audio source is not readable";
        return false;
    }

    const std::string final_path = audio_dir + "/" + token + ".wav";
    const std::string temp_path = final_path + ".tmp.wav";
    unlink(temp_path.c_str());
    const bool converted = convert_to_wav(input_path, temp_path, error);
    if (remove_input)
        unlink(input_path.c_str());
    if (!converted) {
        unlink(temp_path.c_str());
        return false;
    }
    if (rename(temp_path.c_str(), final_path.c_str()) != 0) {
        unlink(temp_path.c_str());
        *error = std::string("cannot save employee audio: ") +
                 strerror(errno);
        return false;
    }

    *audio_path = final_path;
    return true;
}

// -------------------------------------------------------------------------
// Usage
// -------------------------------------------------------------------------
static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  Register: %s register <retina_model> <facenet_model>"
           " <db_path> <name> <image>\n", prog);
    printf("  Run:      %s run      <retina_model> <facenet_model>"
           " <db_path> [anti_spoof_model]\n", prog);
    printf("  Test:     %s test     <retina_model> <facenet_model>"
           " <db_path> [image_dir]\n", prog);
    printf("  Evaluate: %s detector-eval <detector_model> <image_dir>\n", prog);
    printf("  Telegram: %s telegram-test [message]\n", prog);
}

// -------------------------------------------------------------------------
// compute_embedding: resize image to model input, run RetinaFace, align
// (or letterbox), run FaceNet, return L2-normalised 128-d vector.
// Returns 0 on success, -1 if no face detected.
// -------------------------------------------------------------------------
static int compute_embedding(const cv::Mat&      image,
                              rknn_app_context_t* retina_ctx,
                              rknn_app_context_t* facenet_ctx,
                              float*              out_fp32,
                              std::string*        quality_error = nullptr)
{
    const int mw = MODEL_WIDTH;
    const int mh = MODEL_HEIGHT;
    const int fw = FACENET_WIDTH;
    const int fh = FACENET_HEIGHT;

    // Resize source image to RetinaFace input
    cv::Mat model_bgr(mh, mw, CV_8UC3);
    cv::resize(image, model_bgr, cv::Size(mw, mh), 0, 0, cv::INTER_LINEAR);
    memcpy(retina_ctx->input_mems[0]->virt_addr,
           model_bgr.data, mw * mh * 3);

    object_detect_result_list od;
    memset(&od, 0, sizeof(od));
    inference_retinaface_model(retina_ctx, &od);

    if (od.count == 0) {
        printf("[embed] No face detected\n");
        if (quality_error)
            *quality_error = "no face detected";
        return -1;
    }
    if (od.count != 1) {
        printf("[embed] Enrollment rejected: expected one face, found %d\n",
               od.count);
        if (quality_error)
            *quality_error = "face image must contain exactly one face";
        return -1;
    }

    object_detect_result *det = &od.results[0];
    printf("[embed] Face detected  conf=%.2f  box=(%d %d %d %d)\n",
           det->prop,
           det->box.left, det->box.top, det->box.right, det->box.bottom);

    const int enroll_min_size = env_int(
        "FACE_ENROLL_MIN_SIZE_PIXELS", 120, 60, 500);
    const int box_width = det->box.right - det->box.left;
    const int box_height = det->box.bottom - det->box.top;
    const int enroll_edge_margin = 4;
    if (box_width < enroll_min_size || box_height < enroll_min_size) {
        printf("[embed] Enrollment rejected: face too small %dx%d "
               "(minimum %dx%d)\n",
               box_width, box_height, enroll_min_size, enroll_min_size);
        if (quality_error)
            *quality_error = "face is too small; use a closer photo";
        return -1;
    }
    if (det->box.left <= enroll_edge_margin ||
        det->box.top <= enroll_edge_margin ||
        det->box.right >= mw - enroll_edge_margin ||
        det->box.bottom >= mh - enroll_edge_margin) {
        printf("[embed] Enrollment rejected: face clipped by image edge\n");
        if (quality_error)
            *quality_error = "face is clipped by image edge";
        return -1;
    }

    const float enroll_eye_span =
        std::abs((float)det->point[1].x - (float)det->point[0].x);
    const float enroll_mouth_span =
        std::abs((float)det->point[4].x - (float)det->point[3].x);
    if (enroll_eye_span < box_width * 0.18f ||
        enroll_mouth_span < box_width * 0.14f) {
        printf("[embed] Enrollment rejected: face is not frontal\n");
        if (quality_error)
            *quality_error = "face must look directly at camera";
        return -1;
    }

#if USE_FACE_ALIGNMENT
    std::vector<cv::Point2f> lms;
    lms.reserve(5);
    for (int j = 0; j < 5; j++)
        lms.emplace_back((float)det->point[j].x,
                         (float)det->point[j].y);

    cv::Mat aligned = align_face(model_bgr, lms);   // 112x112
    cv::Mat aligned_rs;
    cv::resize(aligned, aligned_rs, cv::Size(fw, fh));
    memcpy(facenet_ctx->input_mems[0]->virt_addr,
           aligned_rs.data, fw * fh * 3);
#else
    // Bounding-box crop + letterbox from model_bgr
    int sX = det->box.left, sY = det->box.top;
    int eX = det->box.right, eY = det->box.bottom;
    sX = std::max(0, std::min(sX, mw - 1));
    sY = std::max(0, std::min(sY, mh - 1));
    eX = std::max(0, std::min(eX, mw - 1));
    eY = std::max(0, std::min(eY, mh - 1));
    cv::Mat face_crop = model_bgr(cv::Rect(sX, sY, eX-sX, eY-sY));
    cv::Mat facenet_input(fh, fw, CV_8UC3,
                          facenet_ctx->input_mems[0]->virt_addr);
    letterbox(face_crop, facenet_input);
#endif

    int ret = rknn_run(facenet_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("[embed] rknn_run fail ret=%d\n", ret);
        if (quality_error)
            *quality_error = "face embedding inference failed";
        return -1;
    }

    uint8_t *raw = (uint8_t *)(facenet_ctx->output_mems[0]->virt_addr);
    output_normalization(facenet_ctx, raw, out_fp32);
    return 0;
}

// =========================================================================
// REGISTER mode
// =========================================================================
static int do_register(const char *retina_model_path,
                       const char *facenet_model_path,
                       const char *db_path,
                       const char *name,
                       const char *image_path)
{
    printf("[register] Enrolling '%s' from %s\n", name, image_path);

    // Load source image
    cv::Mat image = cv::imread(image_path);
    if (image.empty()) {
        printf("[register] Cannot load image: %s\n", image_path);
        return -1;
    }

    // Init RKNN models (no ISP / MPI needed for static image)
    rknn_app_context_t retina_ctx, facenet_ctx;
    memset(&retina_ctx,  0, sizeof(retina_ctx));
    memset(&facenet_ctx, 0, sizeof(facenet_ctx));

    int ret = init_retinaface_facenet_model(retina_model_path,
                                            facenet_model_path,
                                            &retina_ctx, &facenet_ctx);
    if (ret != 0) {
        printf("[register] init_retinaface_facenet_model fail ret=%d\n", ret);
        return -1;
    }

    // Compute embedding
    float embedding[FACE_DB_EMBED_DIM];
    ret = compute_embedding(image, &retina_ctx, &facenet_ctx, embedding);

    release_facenet_model(&facenet_ctx);
    release_retinaface_model(&retina_ctx);

    if (ret != 0) {
        printf("[register] Enrollment failed: no face detected\n");
        return -1;
    }

    // Load existing DB (or start empty on first use)
    face_db_t db;
    if (face_db_load(&db, db_path) != 0)
        printf("[register] No existing DB at %s, creating new\n", db_path);

    // Add entry
    ret = face_db_add(&db, name, embedding);
    if (ret != 0) {
        printf("[register] DB full, cannot add '%s'\n", name);
        return -1;
    }

    ret = face_db_save(&db, db_path);
    if (ret != 0) {
        printf("[register] Failed to save DB to %s\n", db_path);
        return -1;
    }

    printf("[register] '%s' enrolled successfully. ", name);
    face_db_print(&db);
    return 0;
}

// =========================================================================
// RUN mode  —  full RTSP recognition pipeline
// =========================================================================
static int do_run(const char *retina_model_path,
                  const char *facenet_model_path,
                  const char *db_path,
                  const char *anti_spoof_model_path)
{
    setvbuf(stdout, nullptr, _IOLBF, 0);
    g_stop_requested = 0;
    install_stop_signal_handlers();

    // -----------------------------------------------------------------------
    // Load face database
    // -----------------------------------------------------------------------
    face_db_t db;
    int load_ret = face_db_load(&db, db_path);
    if (load_ret != 0 || db.count == 0) {
        printf("[run] Warning: DB empty or not found at %s\n", db_path);
        printf("[run] Register faces first:  ./exe register ...\n");
        printf("[run] Continuing — all faces will show as UNKNOWN\n");
        db.count = 0;
    } else {
        face_db_print(&db);
    }

    std::mutex db_mutex;
    std::mutex model_mutex;

    // The vendor RkLunch-stop.sh also kills udhcpc. On this device the DHCP
    // deconfig hook then removes eth0's address, default route, and DNS,
    // leaving MQTT offline as soon as the camera application starts. We only
    // need to release the vendor camera owner, so stop rkipc without touching
    // networking.
    printf("[run] stopping vendor camera service without changing network\n");
    system("killall rkipc >/dev/null 2>&1");
    for (int wait_count = 0; wait_count < 20; ++wait_count) {
        if (system("pidof rkipc >/dev/null 2>&1") != 0)
            break;
        usleep(100000);
    }

    const int width        = DISP_WIDTH;
    const int height       = DISP_HEIGHT;
    const int model_width  = MODEL_WIDTH;
    const int model_height = MODEL_HEIGHT;
    const int facenet_width  = FACENET_WIDTH;
    const int facenet_height = FACENET_HEIGHT;

    const float scale_x = (float)width  / (float)model_width;
    const float scale_y = (float)height / (float)model_height;
    const bool bench_log_enabled = env_bool("BENCH_LOG_ENABLED", false);
    const float face_dist_threshold = env_float(
        "FACE_DIST_THRESHOLD", FACE_DIST_DEFAULT_THRESHOLD, 0.40f, 1.20f);
    const float face_match_margin = env_float(
        "FACE_MATCH_MARGIN", FACE_MATCH_DEFAULT_MARGIN, 0.0f, 0.50f);
    const int face_min_size = env_int(
        "FACE_MIN_SIZE_PIXELS", FACE_MIN_DEFAULT_SIZE_PIXELS, 40, 400);
    const int face_confirm_frames = env_int(
        "FACE_CONFIRM_FRAMES", FACE_CONFIRM_DEFAULT_FRAMES, 1, 30);
    const char* configured_face_mode = getenv("FACE_ATTENDANCE_MODE");
    FaceAttendanceMode face_attendance_mode = FaceAttendanceMode::Single;
    if (configured_face_mode && *configured_face_mode) {
        if (strcmp(configured_face_mode, "multi") == 0) {
            face_attendance_mode = FaceAttendanceMode::Multi;
        } else if (strcmp(configured_face_mode, "single") != 0) {
            printf("[config] invalid FACE_ATTENDANCE_MODE=%s, use single\n",
                   configured_face_mode);
        }
    }
    const int face_multi_max_people = env_int(
        "FACE_MULTI_MAX_PEOPLE", FACE_MULTI_DEFAULT_MAX_PEOPLE, 1, 5);
    const int face_single_min_size = env_int(
        "FACE_SINGLE_MIN_SIZE_PIXELS",
        FACE_SINGLE_DEFAULT_MIN_SIZE_PIXELS, 80, 400);
    const float face_single_center_tolerance = env_float(
        "FACE_SINGLE_CENTER_TOLERANCE",
        FACE_SINGLE_DEFAULT_CENTER_TOLERANCE, 0.05f, 0.45f);

    printf("[run] USE_FACE_ALIGNMENT=%d face threshold=%.2f margin=%.2f "
           "min_size=%dpx confirm=%d frames\n",
           USE_FACE_ALIGNMENT, (double)face_dist_threshold,
           (double)face_match_margin, face_min_size, face_confirm_frames);
    printf("[run] attendance face mode=%s multi_max=%d "
           "single_min_size=%dpx single_center_tolerance=%.2f\n",
           face_attendance_mode == FaceAttendanceMode::Single
               ? "single" : "multi",
           face_multi_max_people, face_single_min_size,
           (double)face_single_center_tolerance);

    // -----------------------------------------------------------------------
    // Init RKNN models
    // -----------------------------------------------------------------------
    rknn_app_context_t app_retinaface_ctx;
    rknn_app_context_t app_facenet_ctx;
    AntiSpoofContext anti_spoof_ctx;
    object_detect_result_list od_results;

    memset(&app_retinaface_ctx, 0, sizeof(rknn_app_context_t));
    memset(&app_facenet_ctx,    0, sizeof(rknn_app_context_t));

    int ret = init_retinaface_facenet_model(retina_model_path,
                                            facenet_model_path,
                                            &app_retinaface_ctx,
                                            &app_facenet_ctx);
    if (ret != 0) {
        printf("[run] init_retinaface_facenet_model fail ret=%d\n", ret);
        return -1;
    }

    ret = init_anti_spoof_model(anti_spoof_model_path, &anti_spoof_ctx);
    if (ret != 0) {
        printf("[run] Anti-spoof model is required; refusing to start\n");
        release_facenet_model(&app_facenet_ctx);
        release_retinaface_model(&app_retinaface_ctx);
        return -1;
    }
    const float anti_spoof_threshold = env_float(
        "ANTI_SPOOF_THRESHOLD", ANTI_SPOOF_REAL_THRESHOLD, 0.50f, 0.99f);
    printf("[run] effective anti-spoof threshold=%.2f\n",
           (double)anti_spoof_threshold);

    float *face_fp32 = (float *)malloc(sizeof(float) * FACE_DB_EMBED_DIM);

    // Per-face result storage (parallel arrays for detection loop + draw loop)
    float face_dists[128];
    float face_confidences[128];
    char  face_ids[128][32];
    char  face_names[128][FACE_DB_NAME_LEN];
    bool  face_liveness_verified[128];
    bool  face_identity_confirmed[128];
    bool  face_is_spoof[128];
    bool  face_needs_position[128];
    bool  face_selected[128];
    float face_liveness_scores[128];
    std::string face_instructions[128];
    std::map<std::string, FaceConfirmationState> confirmation_states;
    unsigned long long recognition_frame = 0;

    // -----------------------------------------------------------------------
    // VENC frame buffer
    // -----------------------------------------------------------------------
    VENC_STREAM_S stFrame;
    memset(&stFrame, 0, sizeof(stFrame));
    stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    memset(stFrame.pstPack, 0, sizeof(VENC_PACK_S));
    RK_U32 H264_TimeRef = 0;
    VIDEO_FRAME_INFO_S stViFrame;
    memset(&stViFrame, 0, sizeof(stViFrame));
    RK_S32 s32Ret = 0;

    MB_POOL_CONFIG_S PoolCfg;
    memset(&PoolCfg, 0, sizeof(MB_POOL_CONFIG_S));
    PoolCfg.u64MBSize   = width * height * 3;
    PoolCfg.u32MBCnt    = 1;
    PoolCfg.enAllocType = MB_ALLOC_TYPE_DMA;
    MB_POOL src_Pool = RK_MPI_MB_CreatePool(&PoolCfg);
    printf("[run] Create Pool success!\n");

    MB_BLK src_Blk = RK_MPI_MB_GetMB(src_Pool, width * height * 3, RK_TRUE);

    VIDEO_FRAME_INFO_S h264_frame;
    h264_frame.stVFrame.u32Width      = width;
    h264_frame.stVFrame.u32Height     = height;
    h264_frame.stVFrame.u32VirWidth   = width;
    h264_frame.stVFrame.u32VirHeight  = height;
    h264_frame.stVFrame.enPixelFormat = RK_FMT_RGB888;
    h264_frame.stVFrame.u32FrameFlag  = 160;
    h264_frame.stVFrame.pMbBlk        = src_Blk;

    unsigned char *enc_data = (unsigned char *)RK_MPI_MB_Handle2VirAddr(src_Blk);
    cv::Mat frame(cv::Size(width, height), CV_8UC3, enc_data);

    // FaceNet input wrapper (zero-copy view of RKNN input memory)
    cv::Mat facenet_input(facenet_height, facenet_width, CV_8UC3,
                          app_facenet_ctx.input_mems[0]->virt_addr);

    // -----------------------------------------------------------------------
    // ISP + MPI
    // -----------------------------------------------------------------------
    RK_BOOL multi_sensor = RK_FALSE;
    const char *iq_dir   = "/etc/iqfiles";
    rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
    SAMPLE_COMM_ISP_Init(0, hdr_mode, multi_sensor, iq_dir);
    SAMPLE_COMM_ISP_Run(0);

    if (RK_MPI_SYS_Init() != RK_SUCCESS) {
        RK_LOGE("[run] rk mpi sys init fail!");
        free(face_fp32);
        free(stFrame.pstPack);
        release_anti_spoof_model(&anti_spoof_ctx);
        release_facenet_model(&app_facenet_ctx);
        release_retinaface_model(&app_retinaface_ctx);
        return -1;
    }

    // -----------------------------------------------------------------------
    // RTSP
    // -----------------------------------------------------------------------
    rtsp_demo_handle    g_rtsplive    = NULL;
    rtsp_session_handle g_rtsp_session;
    g_rtsplive     = create_rtsp_demo(554);
    g_rtsp_session = rtsp_new_session(g_rtsplive, "/live/0");
    rtsp_set_video(g_rtsp_session, RTSP_CODEC_ID_VIDEO_H264, NULL, 0);
    rtsp_sync_video_ts(g_rtsp_session, rtsp_get_reltime(), rtsp_get_ntptime());

    // -----------------------------------------------------------------------
    // VI + VENC
    // -----------------------------------------------------------------------
    vi_dev_init();
    vi_chn_init(0, width, height);
    venc_init(0, width, height, RK_VIDEO_ID_AVC);

    printf("[run] Ready — rtsp://<device>:554/live/0\n");

    TelegramClient telegram;
    LocalHttpServer http(8080);
    http.setRegisterHandler(
        [&](const HttpRegisterRequest& request) -> HttpActionResult {
            HttpActionResult response;

            if (request.name.empty()) {
                response.ok = false;
                response.message = "name is empty";
                return response;
            }
            if (request.employee_id.empty()) {
                response.ok = false;
                response.message = "employee_id is empty";
                return response;
            }
            if (request.image_path.empty()) {
                response.ok = false;
                response.message = "face image is required";
                return response;
            }

            cv::Mat image = cv::imread(request.image_path);
            if (image.empty()) {
                response.ok = false;
                response.message = "cannot load face image";
                return response;
            }

            float embedding[FACE_DB_EMBED_DIM];
            std::string embedding_error;
            int embed_ret = 0;
            {
                std::lock_guard<std::mutex> lock(model_mutex);
                embed_ret = compute_embedding(image, &app_retinaface_ctx,
                                              &app_facenet_ctx, embedding,
                                              &embedding_error);
            }
            if (embed_ret != 0) {
                response.ok = false;
                response.message = embedding_error.empty()
                    ? "face enrollment failed" : embedding_error;
                return response;
            }

            std::string employee_audio_path;
            if (!request.audio_path.empty()) {
                std::string error;
                if (!download_employee_audio(request.audio_path,
                                             request.employee_id,
                                             request.employee_id,
                                             &employee_audio_path, &error)) {
                    response.ok = false;
                    response.message = error;
                    return response;
                }
            }

            {
                std::lock_guard<std::mutex> lock(db_mutex);
                int add_ret = face_db_add_with_info(
                    &db, request.name.c_str(), request.employee_id.c_str(),
                    employee_audio_path.c_str(), embedding);
                if (add_ret != 0) {
                    response.ok = false;
                    response.message = "database full";
                    return response;
                }
                if (face_db_save(&db, db_path) != 0) {
                    response.ok = false;
                    response.message = "save database failed";
                    return response;
                }
                face_db_print(&db);
            }

            response.ok = true;
            response.message = "Đăng ký thành công";
            printf("[http-register] registered employee_id=%s\n",
                   request.employee_id.c_str());
            return response;
        });
    http.setDeleteHandler(
        [&](const std::string& employee_id) -> HttpActionResult {
            HttpActionResult response;
            if (employee_id.empty()) {
                response.ok = false;
                response.message = "employee_id is empty";
                return response;
            }

            char audio_path[FACE_DB_AUDIO_PATH_LEN]{};
            {
                std::lock_guard<std::mutex> lock(db_mutex);
                const face_db_t backup = db;
                if (face_db_remove_by_employee_id(
                        &db, employee_id.c_str(), audio_path,
                        sizeof(audio_path)) != 0) {
                    response.ok = false;
                    response.message = "employee not found";
                    return response;
                }
                if (face_db_save(&db, db_path) != 0) {
                    db = backup;
                    response.ok = false;
                    response.message = "save database failed";
                    return response;
                }
                face_db_print(&db);
            }

            bool audio_removed = true;
            if (audio_path[0] != '\0' && unlink(audio_path) != 0 &&
                errno != ENOENT) {
                audio_removed = false;
                printf("[http-delete] cannot remove audio %s: %s\n",
                       audio_path, strerror(errno));
            }

            response.ok = true;
            response.message = audio_removed
                ? "Đã xóa nhân viên" : "Đã xóa; không xóa được audio";
            printf("[http-delete] deleted employee_id=%s audio=%s\n",
                   employee_id.c_str(), audio_path);
            return response;
        });
    http.setEmployeesHandler([&]() -> std::string {
        std::lock_guard<std::mutex> lock(db_mutex);
        std::string json = "[";
        for (int i = 0; i < db.count; ++i) {
            if (i) json += ',';
            json += "{\"employee_id\":\"" +
                    std::string(db.entries[i].employee_id) +
                    "\",\"name\":\"" + std::string(db.entries[i].name) +
                    "\",\"has_audio\":" +
                    (db.entries[i].audio_path[0] ? "true" : "false") + "}";
        }
        return json + "]";
    });
    http.setStatusHandler([&]() -> std::string {
        std::lock_guard<std::mutex> lock(db_mutex);
        return "{\"online\":true,\"employee_count\":" +
               std::to_string(db.count) +
               ",\"http_port\":8080,\"rtsp_url\":"
               "\"rtsp://172.32.0.93:554/live/0\"}";
    });
    if (!http.start())
        printf("[http] failed to start dashboard on port 8080\n");

    FaceEventManager attendance_events;
    attendance_events.setAttendanceSuccessCallback(
        [](const std::string& name, const std::string& time) {
            printf("[attendance] success hook: %s at %s\n",
                   name.c_str(), time.c_str());
        });
    attendance_events.setAttendanceDataCallback(
        [&telegram, &http](const AttendanceData& data,
                           const std::string& image_path) {
            onAttendanceSuccess(telegram, data, image_path);
            http.updateRecognition(data.employee_id, data.name, data.time,
                                   data.confidence, data.distance);
        });

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    struct timespec t_frame_start, t_retina_done, t_align_done, t_facenet_done;
    struct timespec fps_window_start;
    clock_gettime(CLOCK_MONOTONIC, &fps_window_start);
    unsigned int fps_window_frames = 0;
    double rtsp_fps = 0.0;

    // Some vendor media libraries install process-wide signal handlers while
    // initializing. Re-assert the application handler after all camera/NPU
    // setup so SIGTERM from the init service exits the frame loop and reaches
    // the ordered cleanup below.
    install_stop_signal_handlers();

    while (!g_stop_requested) {
        clock_gettime(CLOCK_MONOTONIC, &t_frame_start);

        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS     = TEST_COMM_GetNowUs();

        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, -1);
        bool got_vi_frame = (s32Ret == RK_SUCCESS);
        if (s32Ret == RK_SUCCESS) {
            ++recognition_frame;
            std::set<std::string> identities_seen_this_frame;
            void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);

            cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);
            cv::Mat bgr(height, width, CV_8UC3);
            cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);

            long align_us   = 0;
            long facenet_us = 0;
            long anti_spoof_us = 0;
            int detected_face_count = 0;

            {
                std::lock_guard<std::mutex> model_lock(model_mutex);

            // Resize for RetinaFace
            cv::Mat model_bgr(model_height, model_width, CV_8UC3);
            cv::resize(bgr, model_bgr,
                       cv::Size(model_width, model_height), 0, 0, cv::INTER_LINEAR);
            memcpy(app_retinaface_ctx.input_mems[0]->virt_addr,
                   model_bgr.data, model_width * model_height * 3);

            // -----------------------------------------------------------
            // RetinaFace inference
            // -----------------------------------------------------------
            ret = inference_retinaface_model(&app_retinaface_ctx, &od_results);
            clock_gettime(CLOCK_MONOTONIC, &t_retina_done);

            bgr.copyTo(frame);   // clean frame for drawing

            // Measure the real end-to-end frame rate of frames that reach the
            // RTSP encoding path. Refresh once per second to keep the overlay
            // readable instead of displaying a noisy per-frame estimate.
            ++fps_window_frames;
            struct timespec fps_now;
            clock_gettime(CLOCK_MONOTONIC, &fps_now);
            const double fps_elapsed =
                (fps_now.tv_sec - fps_window_start.tv_sec) +
                (fps_now.tv_nsec - fps_window_start.tv_nsec) / 1000000000.0;
            if (fps_elapsed >= 1.0) {
                rtsp_fps = fps_window_frames / fps_elapsed;
                fps_window_frames = 0;
                fps_window_start = fps_now;
            }

            detected_face_count =
                std::max(0, std::min(od_results.count, 128));
            memset(face_selected, 0, sizeof(face_selected));

            if (face_attendance_mode == FaceAttendanceMode::Single) {
                if (detected_face_count == 1) {
                    face_selected[0] = true;
                } else if (detected_face_count > 1 &&
                           recognition_frame % 30 == 1) {
                    printf("[attendance-mode] single requires exactly one "
                           "face; detected=%d\n", detected_face_count);
                }
            } else {
                std::vector<int> face_order;
                face_order.reserve(detected_face_count);
                for (int i = 0; i < detected_face_count; ++i)
                    face_order.push_back(i);
                std::sort(face_order.begin(), face_order.end(),
                          [&od_results](int lhs, int rhs) {
                    const object_detect_result& a =
                        od_results.results[lhs];
                    const object_detect_result& b =
                        od_results.results[rhs];
                    const long area_a =
                        (long)std::max(0, a.box.right - a.box.left) *
                        (long)std::max(0, a.box.bottom - a.box.top);
                    const long area_b =
                        (long)std::max(0, b.box.right - b.box.left) *
                        (long)std::max(0, b.box.bottom - b.box.top);
                    return area_a > area_b;
                });
                const int selected_count = std::min(
                    (int)face_order.size(), face_multi_max_people);
                for (int i = 0; i < selected_count; ++i)
                    face_selected[face_order[i]] = true;
            }

            // -----------------------------------------------------------
            // Phase 1: per-face embedding + DB lookup
            // -----------------------------------------------------------
            for (int i = 0; i < detected_face_count; i++) {
                object_detect_result *det = &(od_results.results[i]);

                face_dists[i] = 9999.0f;
                face_confidences[i] = 0.0f;
                face_ids[i][0] = '\0';
                strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                face_liveness_verified[i] = false;
                face_identity_confirmed[i] = false;
                face_is_spoof[i] = false;
                face_needs_position[i] = false;
                face_liveness_scores[i] = 0.0f;
                face_instructions[i].clear();

                if (!face_selected[i]) {
                    face_needs_position[i] = true;
                    if (face_attendance_mode ==
                        FaceAttendanceMode::Single) {
                        face_instructions[i] = "ONE PERSON ONLY";
                    } else {
                        char instruction[32];
                        snprintf(instruction, sizeof(instruction),
                                 "MAX %d - WAIT",
                                 face_multi_max_people);
                        face_instructions[i] = instruction;
                    }
                    continue;
                }

                int sX = (int)((float)det->box.left   * scale_x);
                int sY = (int)((float)det->box.top    * scale_y);
                int eX = (int)((float)det->box.right  * scale_x);
                int eY = (int)((float)det->box.bottom * scale_y);
                sX = std::max(0, std::min(sX, width  - 1));
                sY = std::max(0, std::min(sY, height - 1));
                eX = std::max(0, std::min(eX, width  - 1));
                eY = std::max(0, std::min(eY, height - 1));

                // Scale landmarks to display space
                float lm_x[5], lm_y[5];
                for (int j = 0; j < 5; j++) {
                    lm_x[j] = (float)det->point[j].x * scale_x;
                    lm_y[j] = (float)det->point[j].y * scale_y;
                }
                printf("[landmarks %d]  "
                       "L=(%.1f,%.1f)  R=(%.1f,%.1f)  N=(%.1f,%.1f)  "
                       "LM=(%.1f,%.1f)  RM=(%.1f,%.1f)\n",
                       i,
                       lm_x[0], lm_y[0], lm_x[1], lm_y[1],
                       lm_x[2], lm_y[2], lm_x[3], lm_y[3],
                       lm_x[4], lm_y[4]);

                // A clipped face does not contain enough skin/context for a
                // reliable passive anti-spoof decision. Keep it fail-closed,
                // but distinguish positioning from an actual spoof result.
                const int edge_margin = 2;
                // YOLO boxes commonly include forehead/background and are
                // clamped to y=0 even though all facial features are visible.
                // Judge clipping from the five landmarks instead of the
                // expanded bounding box so those faces still reach FaceNet.
                float landmark_min_x = lm_x[0], landmark_max_x = lm_x[0];
                float landmark_min_y = lm_y[0], landmark_max_y = lm_y[0];
                for (int j = 1; j < 5; ++j) {
                    landmark_min_x = std::min(landmark_min_x, lm_x[j]);
                    landmark_max_x = std::max(landmark_max_x, lm_x[j]);
                    landmark_min_y = std::min(landmark_min_y, lm_y[j]);
                    landmark_max_y = std::max(landmark_max_y, lm_y[j]);
                }
                const bool face_clipped =
                    landmark_min_x <= edge_margin ||
                    landmark_min_y <= edge_margin ||
                    landmark_max_x >= width - 1 - edge_margin ||
                    landmark_max_y >= height - 1 - edge_margin;
                if (face_clipped) {
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                    face_needs_position[i] = true;
                    face_instructions[i] = "MOVE TO CENTER";
                    printf("[face-quality %d] clipped by frame edge; "
                           "anti-spoof skipped\n", i);
                    continue;
                }

                const float face_width = (float)std::max(1, eX - sX);
                const float face_height = (float)std::max(1, eY - sY);
                // YOLO returns tighter face boxes than RetinaFace. Applying
                // the old single-mode 180 px guide threshold discarded valid
                // 120-170 px faces before FaceNet could run.
                const int required_face_size = face_min_size;
                if (face_width < required_face_size ||
                    face_height < required_face_size) {
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "UNKNOWN",
                            FACE_DB_NAME_LEN - 1);
                    face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                    face_needs_position[i] = true;
                    face_instructions[i] = "MOVE CLOSER";
                    printf("[face-quality %d] face too small %.0fx%.0f "
                           "(minimum %dx%d); recognition skipped\n",
                           i, (double)face_width, (double)face_height,
                           required_face_size, required_face_size);
                    continue;
                }

                const float eye_span = std::abs(lm_x[1] - lm_x[0]);
                const float mouth_span = std::abs(lm_x[4] - lm_x[3]);
                const bool face_profile =
                    eye_span < face_width * 0.18f ||
                    mouth_span < face_width * 0.14f;
                if (face_profile) {
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                    face_needs_position[i] = true;
                    face_instructions[i] = "LOOK AT CAMERA";
                    printf("[face-quality %d] profile pose eye=%.3f "
                           "mouth=%.3f; anti-spoof skipped\n",
                           i, (double)(eye_span / face_width),
                           (double)(mouth_span / face_width));
                    continue;
                }

                // Passive liveness runs before FaceNet. Spoofed faces never
                // reach identity matching or the attendance event manager.
                struct timespec anti_start, anti_done;
                clock_gettime(CLOCK_MONOTONIC, &anti_start);
                AntiSpoofResult anti_result;
                const cv::Rect detected_box(
                    sX, sY, std::max(0, eX - sX), std::max(0, eY - sY));
                const int anti_ret = inference_anti_spoof_model(
                    &anti_spoof_ctx, bgr, detected_box,
                    anti_spoof_threshold, &anti_result);
                clock_gettime(CLOCK_MONOTONIC, &anti_done);
                anti_spoof_us += ts_diff_us(anti_start, anti_done);
                face_liveness_scores[i] = anti_result.real_score;

                if (anti_ret != 0 || !anti_result.is_real) {
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "SPOOF", FACE_DB_NAME_LEN - 1);
                    face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                    face_is_spoof[i] = true;
                    face_instructions[i] = anti_ret == 0
                        ? "SPOOF BLOCKED" : "ANTI-SPOOF ERROR";
                    printf("[anti-spoof %d] %s real=%.3f probs="
                           "[%.3f %.3f %.3f]\n",
                           i, face_instructions[i].c_str(),
                           (double)anti_result.real_score,
                           (double)anti_result.probabilities[0],
                           (double)anti_result.probabilities[1],
                           (double)anti_result.probabilities[2]);
                    continue;
                }
                face_liveness_verified[i] = true;

                // Alignment / crop timing start
                struct timespec ta;
                clock_gettime(CLOCK_MONOTONIC, &ta);

#if USE_FACE_ALIGNMENT
                std::vector<cv::Point2f> lms;
                lms.reserve(5);
                for (int j = 0; j < 5; j++)
                    lms.emplace_back(lm_x[j], lm_y[j]);
                cv::Mat aligned = align_face(bgr, lms);
                cv::Mat aligned_rs;
                cv::resize(aligned, aligned_rs,
                           cv::Size(facenet_width, facenet_height));
                memcpy(app_facenet_ctx.input_mems[0]->virt_addr,
                       aligned_rs.data, facenet_width * facenet_height * 3);
#else
                int fw = eX - sX, fh = eY - sY;
                if (fw <= 0 || fh <= 0) {
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    continue;
                }
                cv::Mat face_crop = bgr(cv::Rect(sX, sY, fw, fh));
                letterbox(face_crop, facenet_input);
#endif
                clock_gettime(CLOCK_MONOTONIC, &t_align_done);
                align_us += ts_diff_us(ta, t_align_done);

                // FaceNet inference
                ret = rknn_run(app_facenet_ctx.rknn_ctx, nullptr);
                clock_gettime(CLOCK_MONOTONIC, &t_facenet_done);
                facenet_us += ts_diff_us(t_align_done, t_facenet_done);

                if (ret < 0) {
                    printf("[warn] rknn_run facenet ret=%d\n", ret);
                    face_dists[i] = 9999.0f;
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    continue;
                }

                uint8_t *raw =
                    (uint8_t *)(app_facenet_ctx.output_mems[0]->virt_addr);
                output_normalization(&app_facenet_ctx, raw, face_fp32);

                // Match against database
                float  dist = 9999.0f;
                float  second_dist = 9999.0f;
                int    idx = -1;
                std::string employee_id;
                std::string audio_path;
                bool match_is_ambiguous = false;
                {
                    std::lock_guard<std::mutex> db_lock(db_mutex);
                    idx = face_db_find_best_two(
                        &db, face_fp32, &dist, &second_dist);
                    match_is_ambiguous =
                        idx >= 0 && second_dist < 9998.0f &&
                        second_dist - dist < face_match_margin;
                    if (idx >= 0 && dist < face_dist_threshold &&
                        !match_is_ambiguous) {
                        strncpy(face_names[i], db.entries[idx].name,
                                FACE_DB_NAME_LEN - 1);
                        face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                        employee_id = db.entries[idx].employee_id;
                        audio_path = db.entries[idx].audio_path;
                    }
                }
                face_dists[i] = dist;
                if (idx >= 0 && dist < face_dist_threshold &&
                    !match_is_ambiguous) {
                    face_confidences[i] =
                        confidence_from_face_distance(
                            dist, face_dist_threshold);
                    snprintf(face_ids[i], sizeof(face_ids[i]),
                             "user_%03d", idx + 1);
                } else {
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                }

                if (match_is_ambiguous) {
                    face_instructions[i] = "AMBIGUOUS FACE";
                    printf("[face-match %d] ambiguous best=%.3f "
                           "second=%.3f margin=%.3f; rejected\n",
                           i, (double)dist, (double)second_dist,
                           (double)(second_dist - dist));
                } else if (idx >= 0 && dist >= face_dist_threshold) {
                    printf("[face-match %d] rejected dist=%.3f "
                           "threshold=%.3f\n",
                           i, (double)dist,
                           (double)face_dist_threshold);
                }

                if (strcmp(face_names[i], "UNKNOWN") != 0) {
                    const std::string confirmation_key =
                        employee_id.empty()
                            ? std::string(face_ids[i])
                            : employee_id;
                    FaceConfirmationState& state =
                        confirmation_states[confirmation_key];
                    if (identities_seen_this_frame
                            .insert(confirmation_key).second) {
                        if (state.last_frame + 1 == recognition_frame)
                            ++state.consecutive_frames;
                        else
                            state.consecutive_frames = 1;
                        state.last_frame = recognition_frame;
                    }

                    if (state.consecutive_frames < face_confirm_frames) {
                        char instruction[48];
                        snprintf(instruction, sizeof(instruction),
                                 "VERIFYING %d/%d",
                                 state.consecutive_frames,
                                 face_confirm_frames);
                        face_instructions[i] = instruction;
                        continue;
                    }
                    face_identity_confirmed[i] = true;

                    FaceResult event_result;
                    event_result.recognized = true;
                    event_result.person_id = face_ids[i];
                    event_result.name = face_names[i];
                    event_result.employee_id = employee_id;
                    event_result.audio_path = audio_path;
                    event_result.confidence = face_confidences[i];
                    event_result.distance = face_dists[i];
                    event_result.liveness_verified = true;
                    event_result.liveness_score = anti_result.real_score;

                    const AttendanceFrameDecision decision =
                        attendance_events.onFrame(Frame(bgr), event_result);
                    face_liveness_verified[i] = decision.liveness_verified;
                    face_instructions[i] = decision.instruction;
                }
            }

            }

            // -----------------------------------------------------------
            // Phase 2: draw bounding boxes and labels
            // -----------------------------------------------------------
            for (int i = 0; i < detected_face_count; i++) {
                object_detect_result *det = &(od_results.results[i]);

                int sX = (int)((float)det->box.left   * scale_x);
                int sY = (int)((float)det->box.top    * scale_y);
                int eX = (int)((float)det->box.right  * scale_x);
                int eY = (int)((float)det->box.bottom * scale_y);
                sX = std::max(0, std::min(sX, width  - 1));
                sY = std::max(0, std::min(sY, height - 1));
                eX = std::max(0, std::min(eX, width  - 1));
                eY = std::max(0, std::min(eY, height - 1));

                bool matched = (strcmp(face_names[i], "UNKNOWN") != 0);
                matched = matched && !face_is_spoof[i];
                const bool live = matched && face_liveness_verified[i] &&
                                  face_identity_confirmed[i];
                cv::Scalar color;
                if (face_is_spoof[i])
                    color = cv::Scalar(255, 0, 255);
                else if (face_needs_position[i])
                    color = cv::Scalar(0, 215, 255);
                else if (!matched)
                    color = cv::Scalar(0, 0, 255);
                else if (live)
                    color = cv::Scalar(0, 255, 0);
                else
                    color = cv::Scalar(0, 215, 255);

                cv::rectangle(frame,
                              cv::Point(sX, sY), cv::Point(eX, eY),
                              color, 3);

                // Magenta means the passive model blocked a presentation
                // attack. Green means liveness and identity both passed.
                std::string label;
                if (face_is_spoof[i]) {
                    char spoof_label[64];
                    snprintf(spoof_label, sizeof(spoof_label),
                             "%s - real %.2f", face_instructions[i].c_str(),
                             face_liveness_scores[i]);
                    label = spoof_label;
                } else if (face_needs_position[i]) {
                    label = face_instructions[i];
                } else if (matched) {
                    char identity[FACE_DB_NAME_LEN + 16];
                    snprintf(identity, sizeof(identity), "%s (%.2f)",
                             face_names[i], face_dists[i]);
                    label = identity;
                    if (!face_instructions[i].empty()) {
                        label += " - ";
                        label += face_instructions[i];
                    }
                } else {
                    label = "UNKNOWN";
                }

                printf("[face %d] %s  dist=%.3f  liveness=%s %.3f  %s\n",
                       i, face_names[i], face_dists[i],
                       live ? "verified" : "pending",
                       (double)face_liveness_scores[i],
                       face_instructions[i].c_str());

                // The fixed single-person oval/status overlay was removed.
                // Always draw the identity/status above its detected box in
                // both single- and multi-person attendance modes.
                cv::putText(frame, label,
                            cv::Point(sX, std::max(20, sY - 8)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
            }

            char fps_label[32];
            snprintf(fps_label, sizeof(fps_label), "FPS: %.1f", rtsp_fps);
            int fps_baseline = 0;
            const cv::Size fps_size = cv::getTextSize(
                fps_label, cv::FONT_HERSHEY_SIMPLEX, 0.65, 2, &fps_baseline);
            const int fps_x = std::max(8, width - fps_size.width - 16);
            cv::rectangle(frame,
                          cv::Point(fps_x - 7, 7),
                          cv::Point(width - 7, 15 + fps_size.height + fps_baseline),
                          cv::Scalar(0, 0, 0), cv::FILLED);
            cv::putText(frame, fps_label,
                        cv::Point(fps_x, 12 + fps_size.height),
                        cv::FONT_HERSHEY_SIMPLEX, 0.65,
                        cv::Scalar(0, 255, 0), 2, cv::LINE_AA);

            http.updateFrame(frame.data, width, height);
            memcpy(enc_data, frame.data, width * height * 3);

            // -----------------------------------------------------------
            // Benchmark
            // -----------------------------------------------------------
            struct timespec t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            if (bench_log_enabled) {
                printf("[bench] Retina=%ld us  AntiSpoof=%ld us  Align=%ld us  FaceNet=%ld us"
                       "  Total=%ld us  Faces=%d\n",
                       ts_diff_us(t_frame_start, t_retina_done),
                       anti_spoof_us, align_us, facenet_us,
                       ts_diff_us(t_frame_start, t_end),
                       od_results.count);
            }
        } else {
            RK_LOGE("RK_MPI_VI_GetChnFrame fail %x", s32Ret);
        }

        if (got_vi_frame) {
            // Encode H264 only for a valid captured frame.
            s32Ret = RK_MPI_VENC_SendFrame(0, &h264_frame, -1);
            if (s32Ret != RK_SUCCESS)
                RK_LOGE("RK_MPI_VENC_SendFrame fail %x", s32Ret);

            bool got_venc_stream = false;
            if (s32Ret == RK_SUCCESS) {
                memset(stFrame.pstPack, 0, sizeof(VENC_PACK_S));
                s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
                got_venc_stream = (s32Ret == RK_SUCCESS);
                if (got_venc_stream && g_rtsplive && g_rtsp_session) {
                    void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
                    rtsp_tx_video(g_rtsp_session,
                                  (uint8_t *)pData, stFrame.pstPack->u32Len,
                                  stFrame.pstPack->u64PTS);
                    rtsp_do_event(g_rtsplive);
                } else if (!got_venc_stream) {
                    RK_LOGE("RK_MPI_VENC_GetStream fail %x", s32Ret);
                }
            }

            s32Ret = RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
            if (s32Ret != RK_SUCCESS)
                RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);

            if (got_venc_stream) {
                s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
                if (s32Ret != RK_SUCCESS)
                    RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
            }
        }
    }

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------
    printf("[run] stop requested; releasing camera and inference resources\n");
    http.stop();
    free(face_fp32);
    free(stFrame.pstPack);

    RK_MPI_MB_ReleaseMB(src_Blk);
    RK_MPI_MB_DestroyPool(src_Pool);
    RK_MPI_VI_DisableChn(0, 0);
    RK_MPI_VI_DisableDev(0);
    SAMPLE_COMM_ISP_Stop(0);
    RK_MPI_VENC_StopRecvFrame(0);
    RK_MPI_VENC_DestroyChn(0);

    if (g_rtsplive)
        rtsp_del_demo(g_rtsplive);

    RK_MPI_SYS_Exit();

    release_facenet_model(&app_facenet_ctx);
    release_retinaface_model(&app_retinaface_ctx);
    release_anti_spoof_model(&anti_spoof_ctx);
    return 0;
}

// =========================================================================
// TEST mode - static image recognition without camera
// =========================================================================
static int do_test(const char *retina_model_path,
                   const char *facenet_model_path,
                   const char *db_path,
                   const char *image_dir)
{
    face_db_t db;
    int load_ret = face_db_load(&db, db_path);
    if (load_ret != 0 || db.count == 0) {
        printf("[test] DB empty or not found at %s\n", db_path);
        printf("[test] Register faces first: ./exe register ...\n");
        return -1;
    }
    face_db_print(&db);

    rknn_app_context_t retina_ctx;
    rknn_app_context_t facenet_ctx;
    memset(&retina_ctx, 0, sizeof(retina_ctx));
    memset(&facenet_ctx, 0, sizeof(facenet_ctx));

    int ret = init_retinaface_facenet_model(retina_model_path,
                                            facenet_model_path,
                                            &retina_ctx,
                                            &facenet_ctx);
    if (ret != 0) {
        printf("[test] init_retinaface_facenet_model fail ret=%d\n", ret);
        return -1;
    }

    TelegramClient telegram;
    // Static-image test mode intentionally bypasses liveness.  Production
    // camera mode above always uses the default (liveness required).
    FaceEventManager attendance_events(false);

    attendance_events.setAttendanceSuccessCallback(
        [](const std::string& name, const std::string& time) {
            printf("[attendance-test] success hook: %s at %s\n",
                   name.c_str(), time.c_str());
        });
    attendance_events.setAttendanceDataCallback(
        [&telegram](const AttendanceData& data,
                    const std::string& image_path) {
            onAttendanceSuccess(telegram, data, image_path);
        });

    const float face_dist_threshold = env_float(
        "FACE_DIST_THRESHOLD", FACE_DIST_DEFAULT_THRESHOLD, 0.40f, 1.20f);
    const float face_match_margin = env_float(
        "FACE_MATCH_MARGIN", FACE_MATCH_DEFAULT_MARGIN, 0.0f, 0.50f);

    FaceTestRunner runner(&attendance_events);
    int run_ret = runner.run(
        image_dir ? image_dir : "/test_images",
        [&db, &retina_ctx, &facenet_ctx, face_dist_threshold,
         face_match_margin](const cv::Mat& image,
                            const std::string& image_path,
                            FaceResult* result) -> bool {
            if (!result)
                return false;

            float embedding[FACE_DB_EMBED_DIM];
            if (compute_embedding(image, &retina_ctx, &facenet_ctx,
                                  embedding) != 0) {
                return false;
            }

            float dist = 9999.0f;
            float second_dist = 9999.0f;
            int idx = face_db_find_best_two(
                &db, embedding, &dist, &second_dist);
            const bool ambiguous =
                second_dist < 9998.0f &&
                second_dist - dist < face_match_margin;
            if (idx < 0 || dist >= face_dist_threshold || ambiguous) {
                printf("[test] %s -> UNKNOWN best=%.3f second=%.3f%s\n",
                       image_path.c_str(), (double)dist,
                       (double)second_dist,
                       ambiguous ? " ambiguous" : "");
                return false;
            }

            result->recognized = true;
            result->person_id.clear();
            char id_buf[32];
            snprintf(id_buf, sizeof(id_buf), "user_%03d", idx + 1);
            result->person_id = id_buf;
            result->name = db.entries[idx].name;
            result->distance = dist;
            result->confidence = confidence_from_face_distance(
                dist, face_dist_threshold);
            return true;
        });

    release_facenet_model(&facenet_ctx);
    release_retinaface_model(&retina_ctx);
    return run_ret;
}

static int do_telegram_test(const char *message)
{
    TelegramClient telegram;
    const char *default_message = "xin chao Vo Quoc Kha";
    const std::string text = message ? message : default_message;

    printf("[telegram-test] sending message: %s\n", text.c_str());
    return telegram.sendMessage(text) ? 0 : -1;
}

// Evaluate only the deployed detector. Predictions are emitted as normalized
// YOLO boxes so metrics can be calculated off-device against the test labels.
static int do_detector_eval(const char* model_path, const char* image_dir)
{
    rknn_app_context_t detector_ctx;
    memset(&detector_ctx, 0, sizeof(detector_ctx));
    if (init_retinaface_model(model_path, &detector_ctx) != 0) {
        printf("[detector-eval] cannot initialize %s\n", model_path);
        return -1;
    }

    std::vector<cv::String> paths;
    cv::glob(std::string(image_dir) + "/*", paths, false);
    int processed = 0;
    for (const cv::String& path : paths) {
        cv::Mat image = cv::imread(path);
        if (image.empty())
            continue;

        struct timespec start{}, finish{};
        clock_gettime(CLOCK_MONOTONIC, &start);
        const float scale = std::min(
            MODEL_WIDTH / (float)image.cols,
            MODEL_HEIGHT / (float)image.rows);
        const int resized_w = std::max(1, (int)roundf(image.cols * scale));
        const int resized_h = std::max(1, (int)roundf(image.rows * scale));
        const int pad_x = (MODEL_WIDTH - resized_w) / 2;
        const int pad_y = (MODEL_HEIGHT - resized_h) / 2;
        cv::Mat resized_bgr;
        cv::resize(image, resized_bgr, cv::Size(resized_w, resized_h),
                   0, 0, cv::INTER_LINEAR);
        cv::Mat model_bgr(MODEL_HEIGHT, MODEL_WIDTH, CV_8UC3,
                          cv::Scalar(114, 114, 114));
        resized_bgr.copyTo(model_bgr(
            cv::Rect(pad_x, pad_y, resized_w, resized_h)));
        cv::Mat model_rgb;
        cv::cvtColor(model_bgr, model_rgb, cv::COLOR_BGR2RGB);
        memcpy(detector_ctx.input_mems[0]->virt_addr, model_rgb.data,
               MODEL_WIDTH * MODEL_HEIGHT * 3);
        object_detect_result_list detections;
        memset(&detections, 0, sizeof(detections));
        const int ret = inference_retinaface_model(&detector_ctx, &detections);
        clock_gettime(CLOCK_MONOTONIC, &finish);
        if (ret != 0) {
            printf("[detector-eval] inference failed: %s\n", path.c_str());
            continue;
        }

        const double latency_ms =
            (finish.tv_sec - start.tv_sec) * 1000.0 +
            (finish.tv_nsec - start.tv_nsec) / 1000000.0;
        const size_t slash = path.find_last_of("/\\");
        const std::string name = slash == std::string::npos
            ? std::string(path) : std::string(path.substr(slash + 1));
        printf("EVAL,%s,%.6f,%d", name.c_str(), latency_ms,
               detections.count);
        for (int i = 0; i < detections.count; ++i) {
            const object_detect_result& det = detections.results[i];
            const float left = std::max(0.0f,
                (det.box.left - pad_x) / scale);
            const float top = std::max(0.0f,
                (det.box.top - pad_y) / scale);
            const float right = std::min((float)image.cols,
                (det.box.right - pad_x) / scale);
            const float bottom = std::min((float)image.rows,
                (det.box.bottom - pad_y) / scale);
            const float cx = (left + right) / (2.0f * image.cols);
            const float cy = (top + bottom) / (2.0f * image.rows);
            const float width = std::max(0.0f, right - left) / image.cols;
            const float height = std::max(0.0f, bottom - top) / image.rows;
            printf(",%.8f,%.8f,%.8f,%.8f,%.8f",
                   det.prop, cx, cy, width, height);
        }
        printf("\n");
        ++processed;
    }
    release_retinaface_model(&detector_ctx);
    printf("[detector-eval] processed=%d discovered=%zu\n",
           processed, paths.size());
    return processed == (int)paths.size() ? 0 : -1;
}

// =========================================================================
int main(int argc, char *argv[])
// =========================================================================
{
    if (argc < 2) {
        print_usage(argv[0]);
        return -1;
    }

    if (strcmp(argv[1], "register") == 0) {
        // register <retina> <facenet> <db> <name> <image>  => 7 args
        if (argc != 7) {
            printf("register needs: <retina_model> <facenet_model>"
                   " <db_path> <name> <image>\n");
            return -1;
        }
        return do_register(argv[2], argv[3], argv[4], argv[5], argv[6]);
    }

    if (strcmp(argv[1], "run") == 0) {
        // The optional path keeps old launch commands working; installed
        // packages include the default model under ./model.
        if (argc != 5 && argc != 6) {
            printf("run needs: <retina_model> <facenet_model> <db_path>"
                   " [anti_spoof_model]\n");
            return -1;
        }
        return do_run(argv[2], argv[3], argv[4],
                      argc == 6 ? argv[5] : ANTI_SPOOF_DEFAULT_MODEL);
    }

    if (strcmp(argv[1], "test") == 0) {
        // test <retina> <facenet> <db> [image_dir], defaults to /test_images
        if (argc != 5 && argc != 6) {
            printf("test needs: <retina_model> <facenet_model>"
                   " <db_path> [image_dir]\n");
            return -1;
        }
        return do_test(argv[2], argv[3], argv[4],
                       argc == 6 ? argv[5] : "/test_images");
    }

    if (strcmp(argv[1], "detector-eval") == 0) {
        if (argc != 4) {
            printf("detector-eval needs: <detector_model> <image_dir>\n");
            return -1;
        }
        return do_detector_eval(argv[2], argv[3]);
    }

    if (strcmp(argv[1], "telegram-test") == 0) {
        if (argc > 3) {
            printf("telegram-test needs: [message]\n");
            return -1;
        }
        return do_telegram_test(argc == 3 ? argv[2] : nullptr);
    }

    print_usage(argv[0]);
    return -1;
}
