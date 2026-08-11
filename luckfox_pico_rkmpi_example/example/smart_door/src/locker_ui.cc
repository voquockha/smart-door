#include "locker_ui.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <array>
#include <cmath>
#include <string>
#include <vector>

extern "C" {
#include "lvgl.h"
#include "lv_drivers/display/fbdev.h"
#include "lv_drivers/indev/evdev.h"
}

namespace {

constexpr int kDisplayWidth = 480;
constexpr int kDisplayHeight = 480;
constexpr int kCameraWidth = 440;
constexpr int kCameraHeight = 278;
constexpr int kLockerCount = 8;
constexpr int kEmbeddingDimension = 128;
constexpr int kConfirmationFrames = 3;
constexpr time_t kDefaultRetentionSeconds = 24 * 60 * 60;

const lv_color_t kBackground = lv_color_hex(0xF5F7FA);
const lv_color_t kNavy = lv_color_hex(0x12263F);
const lv_color_t kBlue = lv_color_hex(0x246BFD);
const lv_color_t kGreen = lv_color_hex(0x17A673);
const lv_color_t kMuted = lv_color_hex(0x667085);
const lv_color_t kWhite = lv_color_hex(0xFFFFFF);

struct LockerSlot {
    bool occupied = false;
    std::string person_id;
    std::string display_name;
    time_t created_at = 0;
    std::array<float, kEmbeddingDimension> embedding{};
};

std::array<LockerSlot, kLockerCount> g_slots;
LockerMode g_mode = LockerMode::None;
lv_obj_t *g_screen = nullptr;
lv_obj_t *g_camera_image = nullptr;
lv_obj_t *g_hint_label = nullptr;
lv_img_dsc_t g_camera_descriptor{};
std::vector<lv_color_t> g_camera_pixels;
lv_disp_draw_buf_t g_draw_buffer;
lv_disp_drv_t g_display_driver;
lv_indev_drv_t g_input_driver;
std::vector<lv_color_t> g_buffer_a;
std::vector<lv_color_t> g_buffer_b;
uint32_t g_last_camera_update = 0;
bool g_initialized = false;
bool g_autotest_consumed = false;
bool g_autotest_face_consumed = false;
std::array<float, kEmbeddingDimension> g_deposit_sum{};
int g_deposit_frames = 0;
int g_pickup_candidate = -1;
int g_pickup_frames = 0;
uint32_t g_last_expiry_check = 0;

time_t retention_seconds()
{
    const char *value = getenv("SMART_LOCKER_RETENTION_SECONDS");
    if(!value || !value[0])
        return kDefaultRetentionSeconds;
    char *end = nullptr;
    const long parsed = strtol(value, &end, 10);
    return end && *end == '\0' && parsed >= 60
        ? static_cast<time_t>(parsed) : kDefaultRetentionSeconds;
}

float temporary_face_threshold()
{
    const char *value = getenv("SMART_LOCKER_TEMP_FACE_THRESHOLD");
    if(!value || !value[0])
        return 0.78f;
    char *end = nullptr;
    const float parsed = strtof(value, &end);
    return end && *end == '\0' && parsed >= 0.3f && parsed <= 1.5f
        ? parsed : 0.78f;
}

const char *locker_state_path()
{
    const char *configured = getenv("SMART_LOCKER_STATE_FILE");
    return configured && configured[0] ? configured : "locker_state.db";
}

bool save_locker_state()
{
    const std::string temporary_path =
        std::string(locker_state_path()) + ".tmp";
    FILE *file = fopen(temporary_path.c_str(), "w");
    if(!file) {
        printf("[SMART_LOCKER][STATE] cannot write %s\n",
               temporary_path.c_str());
        return false;
    }
    for(int i = 0; i < kLockerCount; ++i) {
        if(g_slots[i].occupied) {
            fprintf(file, "%d\t%lld\t%s\t%s", i + 1,
                    static_cast<long long>(g_slots[i].created_at),
                    g_slots[i].person_id.c_str(),
                    g_slots[i].display_name.c_str());
            for(float value : g_slots[i].embedding)
                fprintf(file, "\t%.9g", static_cast<double>(value));
            fputc('\n', file);
        }
    }
    bool success = fflush(file) == 0 && fsync(fileno(file)) == 0;
    if(fclose(file) != 0)
        success = false;
    if(success && rename(temporary_path.c_str(), locker_state_path()) != 0) {
        printf("[SMART_LOCKER][STATE] cannot commit %s: %s\n",
               locker_state_path(), strerror(errno));
        success = false;
    }
    if(!success)
        unlink(temporary_path.c_str());
    return success;
}

void load_locker_state()
{
    FILE *file = fopen(locker_state_path(), "r");
    if(!file) {
        printf("[SMART_LOCKER][STATE] new empty state at %s\n",
               locker_state_path());
        return;
    }
    char line[4096];
    const time_t now = time(nullptr);
    int loaded = 0;
    int expired = 0;
    while(fgets(line, sizeof(line), file)) {
        char *save = nullptr;
        char *token = strtok_r(line, "\t\n", &save);
        if(!token)
            continue;
        const int number = atoi(token);
        token = strtok_r(nullptr, "\t\n", &save);
        if(number < 1 || number > kLockerCount || !token)
            continue;
        const time_t created_at =
            static_cast<time_t>(strtoll(token, nullptr, 10));
        char *person_id = strtok_r(nullptr, "\t\n", &save);
        char *display_name = strtok_r(nullptr, "\t\n", &save);
        if(!person_id || !display_name)
            continue;
        if(created_at <= 0 || now - created_at >= retention_seconds()) {
            ++expired;
            continue;
        }
        LockerSlot slot;
        slot.occupied = true;
        slot.created_at = created_at;
        slot.person_id = person_id;
        slot.display_name = display_name;
        bool valid = true;
        for(int i = 0; i < kEmbeddingDimension; ++i) {
            token = strtok_r(nullptr, "\t\n", &save);
            if(!token) {
                valid = false;
                break;
            }
            slot.embedding[i] = strtof(token, nullptr);
        }
        if(valid) {
            g_slots[number - 1] = slot;
            ++loaded;
        }
    }
    fclose(file);
    if(expired)
        (void)save_locker_state();
    printf("[SMART_LOCKER][STATE] loaded=%d expired=%d retention=%llds\n",
           loaded, expired, static_cast<long long>(retention_seconds()));
}

void expire_old_slots()
{
    const time_t now = time(nullptr);
    bool changed = false;
    for(int i = 0; i < kLockerCount; ++i) {
        if(g_slots[i].occupied &&
           now - g_slots[i].created_at >= retention_seconds()) {
            printf("[SMART_LOCKER][EXPIRE] slot=%02d age=%llds\n",
                   i + 1,
                   static_cast<long long>(now - g_slots[i].created_at));
            g_slots[i] = LockerSlot{};
            changed = true;
        }
    }
    if(changed)
        (void)save_locker_state();
}

lv_obj_t *make_label(lv_obj_t *parent, const char *text,
                     int x, int y, int width, lv_color_t color)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, width);
    lv_obj_set_style_text_color(label, color, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    return label;
}

lv_obj_t *make_button(lv_obj_t *parent, const char *text,
                      int x, int y, int width, int height,
                      lv_color_t color, lv_event_cb_t callback,
                      void *user_data)
{
    lv_obj_t *button = lv_btn_create(parent);
    lv_obj_set_pos(button, x, y);
    lv_obj_set_size(button, width, height);
    lv_obj_set_style_radius(button, 18, 0);
    lv_obj_set_style_bg_color(button, color, 0);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(button, 12, 0);
    lv_obj_set_style_shadow_opa(button, LV_OPA_20, 0);
    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED, user_data);

    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, kWhite, 0);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(label);
    return button;
}

void new_screen()
{
    lv_obj_t *old = g_screen;
    g_screen = lv_obj_create(nullptr);
    lv_obj_clear_flag(g_screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(g_screen, kBackground, 0);
    lv_obj_set_style_bg_opa(g_screen, LV_OPA_COVER, 0);
    lv_scr_load(g_screen);
    if(old)
        lv_obj_del_async(old);
    g_camera_image = nullptr;
    g_hint_label = nullptr;
}

void show_home();
void show_camera(LockerMode mode);

void mode_event(lv_event_t *event)
{
    const LockerMode mode = static_cast<LockerMode>(
        reinterpret_cast<intptr_t>(lv_event_get_user_data(event)));
    show_camera(mode);
}

void home_event(lv_event_t *)
{
    show_home();
}

void cancel_event(lv_event_t *)
{
    printf("[SMART_LOCKER] scan cancelled\n");
    show_home();
}

void show_home()
{
    g_mode = LockerMode::None;
    g_deposit_frames = 0;
    g_deposit_sum.fill(0.0f);
    g_pickup_candidate = -1;
    g_pickup_frames = 0;
    new_screen();

    make_label(g_screen, LV_SYMBOL_HOME "  SMART LOCKER",
               24, 28, 432, kBlue);
    make_label(g_screen, "GUI DO / NHAN DO",
               24, 82, 432, kNavy);
    make_label(g_screen, "Chon thao tac de bat dau",
               24, 112, 432, kMuted);

    make_button(g_screen, LV_SYMBOL_UPLOAD "\nGUI DO\nCHECK IN",
                30, 165, 200, 190, kBlue, mode_event,
                reinterpret_cast<void *>(
                    static_cast<intptr_t>(LockerMode::Deposit)));
    make_button(g_screen, LV_SYMBOL_DOWNLOAD "\nNHAN DO\nCHECK OUT",
                250, 165, 200, 190, kGreen, mode_event,
                reinterpret_cast<void *>(
                    static_cast<intptr_t>(LockerMode::Pickup)));

    lv_obj_t *note = make_label(
        g_screen, LV_SYMBOL_EYE_CLOSE "  Camera dang tat",
        50, 407, 380, kMuted);
    lv_obj_set_style_bg_color(note, lv_color_hex(0xE7EDF6), 0);
    lv_obj_set_style_bg_opa(note, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(note, 12, 0);
    lv_obj_set_style_radius(note, 12, 0);

    printf("[SMART_LOCKER] HOME camera_display=OFF\n");
}

void show_camera(LockerMode mode)
{
    g_mode = mode;
    g_deposit_frames = 0;
    g_deposit_sum.fill(0.0f);
    g_pickup_candidate = -1;
    g_pickup_frames = 0;
    g_last_camera_update = 0;
    new_screen();

    const bool deposit = mode == LockerMode::Deposit;
    const lv_color_t accent = deposit ? kBlue : kGreen;
    make_label(g_screen,
               deposit ? LV_SYMBOL_UPLOAD "  GUI DO - NHAN DIEN KHUON MAT"
                       : LV_SYMBOL_DOWNLOAD "  NHAN DO - NHAN DIEN KHUON MAT",
               20, 18, 440, accent);

    g_camera_descriptor.header.always_zero = 0;
    g_camera_descriptor.header.w = kCameraWidth;
    g_camera_descriptor.header.h = kCameraHeight;
    g_camera_descriptor.header.cf = LV_IMG_CF_TRUE_COLOR;
    g_camera_descriptor.data_size =
        kCameraWidth * kCameraHeight * sizeof(lv_color_t);
    g_camera_descriptor.data =
        reinterpret_cast<const uint8_t *>(g_camera_pixels.data());

    g_camera_image = lv_img_create(g_screen);
    lv_img_set_src(g_camera_image, &g_camera_descriptor);
    lv_obj_set_pos(g_camera_image, 20, 55);
    lv_obj_set_style_radius(g_camera_image, 16, 0);
    lv_obj_set_style_clip_corner(g_camera_image, true, 0);
    lv_obj_set_style_border_color(g_camera_image, accent, 0);
    lv_obj_set_style_border_width(g_camera_image, 3, 0);

    lv_obj_t *guide = lv_obj_create(g_screen);
    lv_obj_set_pos(guide, 166, 86);
    lv_obj_set_size(guide, 148, 190);
    lv_obj_set_style_bg_opa(guide, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(guide, kWhite, 0);
    lv_obj_set_style_border_width(guide, 3, 0);
    lv_obj_set_style_radius(guide, 70, 0);

    g_hint_label = make_label(g_screen, "Dat khuon mat vao khung",
                              50, 305, 380, kWhite);
    lv_obj_set_style_bg_color(g_hint_label, lv_color_hex(0x101828), 0);
    lv_obj_set_style_bg_opa(g_hint_label, LV_OPA_70, 0);
    lv_obj_set_style_pad_all(g_hint_label, 8, 0);
    lv_obj_set_style_radius(g_hint_label, 8, 0);

    make_button(g_screen, LV_SYMBOL_CLOSE "  HUY",
                120, 382, 240, 64, kMuted, cancel_event, nullptr);
    printf("[SMART_LOCKER] MODE=%s camera_display=ON recognition=ON\n",
           deposit ? "DEPOSIT" : "PICKUP");
}

int find_empty_slot()
{
    for(int i = 0; i < kLockerCount; ++i) {
        if(!g_slots[i].occupied)
            return i;
    }
    return -1;
}

void show_result(bool success, int locker_number,
                 const char *title, const char *message)
{
    g_mode = LockerMode::None;
    new_screen();
    const lv_color_t accent =
        success ? kGreen : lv_color_hex(0xD92D20);

    make_label(g_screen, success ? LV_SYMBOL_OK : LV_SYMBOL_WARNING,
               190, 62, 100, accent);
    make_label(g_screen, title, 30, 112, 420, accent);

    if(success) {
        char locker_text[32];
        snprintf(locker_text, sizeof(locker_text),
                 "NGAN TU %02d", locker_number);
        lv_obj_t *locker = make_label(g_screen, locker_text,
                                      70, 170, 340, kNavy);
        lv_obj_set_style_bg_color(locker, lv_color_hex(0xE4F7EE), 0);
        lv_obj_set_style_bg_opa(locker, LV_OPA_COVER, 0);
        lv_obj_set_style_pad_all(locker, 28, 0);
        lv_obj_set_style_radius(locker, 16, 0);
    }

    make_label(g_screen, message, 45, success ? 285 : 205, 390, kMuted);
    make_button(g_screen, LV_SYMBOL_HOME "  HOAN TAT",
                100, 372, 280, 68, accent, home_event, nullptr);
}

}  // namespace

uint32_t locker_tick_get()
{
    static uint64_t start_ms = 0;
    timeval now{};
    gettimeofday(&now, nullptr);
    const uint64_t now_ms =
        (static_cast<uint64_t>(now.tv_sec) * 1000000ULL + now.tv_usec) / 1000;
    if(start_ms == 0)
        start_ms = now_ms;
    return static_cast<uint32_t>(now_ms - start_ms);
}

bool locker_ui_init()
{
    if(g_initialized)
        return true;

    lv_init();
    fbdev_init();

    constexpr int draw_pixels = kDisplayWidth * 60;
    g_buffer_a.resize(draw_pixels);
    g_buffer_b.resize(draw_pixels);
    g_camera_pixels.resize(kCameraWidth * kCameraHeight,
                           lv_color_hex(0x101828));

    lv_disp_draw_buf_init(&g_draw_buffer, g_buffer_a.data(),
                          g_buffer_b.data(), draw_pixels);
    lv_disp_drv_init(&g_display_driver);
    g_display_driver.draw_buf = &g_draw_buffer;
    g_display_driver.flush_cb = fbdev_flush;
    g_display_driver.hor_res = kDisplayWidth;
    g_display_driver.ver_res = kDisplayHeight;
    lv_disp_drv_register(&g_display_driver);

    evdev_init();
    lv_indev_drv_init(&g_input_driver);
    g_input_driver.type = LV_INDEV_TYPE_POINTER;
    g_input_driver.read_cb = evdev_read;
    lv_indev_drv_register(&g_input_driver);

    load_locker_state();
    show_home();
    g_initialized = true;
    g_autotest_consumed = false;
    g_autotest_face_consumed = false;
    lv_timer_handler();
    printf("[SMART_LOCKER][LCD] initialized 480x480 fbdev + evdev\n");
    return true;
}

void locker_ui_process()
{
    if(g_initialized) {
        const char *autotest = getenv("SMART_LOCKER_AUTOTEST");
        if(!g_autotest_consumed && autotest && locker_tick_get() > 1500) {
            g_autotest_consumed = true;
            if(strcmp(autotest, "deposit") == 0)
                show_camera(LockerMode::Deposit);
            else if(strcmp(autotest, "pickup") == 0)
                show_camera(LockerMode::Pickup);
        }
        const char *autotest_face = getenv("SMART_LOCKER_AUTOTEST_FACE");
        if(!g_autotest_face_consumed && locker_ui_is_scanning() &&
           autotest_face && strcmp(autotest_face, "1") == 0 &&
           locker_tick_get() > 3000) {
            g_autotest_face_consumed = true;
            std::array<float, kEmbeddingDimension> diagnostic_embedding{};
            for(int i = 0; i < kEmbeddingDimension; ++i)
                diagnostic_embedding[i] =
                    static_cast<float>(i + 1) / 1024.0f;
            for(int frame = 0; frame < kConfirmationFrames; ++frame) {
                locker_ui_process_embedding(diagnostic_embedding.data(),
                                            kEmbeddingDimension, 0.99f);
            }
        }
        const uint32_t now = locker_tick_get();
        if(now - g_last_expiry_check >= 60000) {
            g_last_expiry_check = now;
            expire_old_slots();
        }
        lv_timer_handler();
    }
}

void locker_ui_shutdown()
{
    if(!g_initialized)
        return;
    save_locker_state();
    fbdev_exit();
    g_initialized = false;
}

bool locker_ui_is_scanning()
{
    return g_initialized && g_mode != LockerMode::None;
}

LockerMode locker_ui_mode()
{
    return g_mode;
}

void locker_ui_update_camera(const uint8_t *bgr, int width, int height,
                             int stride)
{
    if(!locker_ui_is_scanning() || !g_camera_image || !bgr ||
       width <= 0 || height <= 0)
        return;

    const uint32_t now = locker_tick_get();
    if(g_last_camera_update && now - g_last_camera_update < 100)
        return;
    g_last_camera_update = now;

    /* Center-crop the 3:2 camera to the nearly-square LCD preview. */
    const int source_width = height * kCameraWidth / kCameraHeight;
    const int crop_width = source_width < width ? source_width : width;
    const int crop_x = (width - crop_width) / 2;
    for(int y = 0; y < kCameraHeight; ++y) {
        const int source_y = y * height / kCameraHeight;
        for(int x = 0; x < kCameraWidth; ++x) {
            const int source_x = crop_x + x * crop_width / kCameraWidth;
            const uint8_t *pixel =
                bgr + source_y * stride + source_x * 3;
            g_camera_pixels[y * kCameraWidth + x] =
                lv_color_make(pixel[2], pixel[1], pixel[0]);
        }
    }
    lv_obj_invalidate(g_camera_image);
}

void locker_ui_set_hint(const char *hint)
{
    if(g_hint_label && locker_ui_is_scanning())
        lv_label_set_text(g_hint_label, hint ? hint : "");
}

void locker_ui_face_rejected(const char *reason)
{
    if(!locker_ui_is_scanning())
        return;
    printf("[SMART_LOCKER][FACE] rejected reason=%s\n",
           reason ? reason : "unknown");
    locker_ui_set_hint("Khong nhan dien duoc - vui long thu lai");
}

bool locker_ui_process_embedding(const float *embedding, int dimension,
                                 float liveness_score)
{
    if(!locker_ui_is_scanning() || !embedding ||
       dimension != kEmbeddingDimension)
        return false;

    if(g_mode == LockerMode::Deposit) {
        for(int i = 0; i < kEmbeddingDimension; ++i)
            g_deposit_sum[i] += embedding[i];
        ++g_deposit_frames;

        char hint[64];
        snprintf(hint, sizeof(hint), "Dang ghi nhan khuon mat %d/%d",
                 g_deposit_frames, kConfirmationFrames);
        locker_ui_set_hint(hint);
        if(g_deposit_frames < kConfirmationFrames)
            return false;

        const int slot_index = find_empty_slot();
        if(slot_index < 0) {
            show_result(false, 0, "TU DA DAY",
                        "Khong con ngan trong. Vui long lien he ho tro.");
            return true;
        }

        LockerSlot& slot = g_slots[slot_index];
        slot.occupied = true;
        slot.created_at = time(nullptr);
        char temporary_id[64];
        snprintf(temporary_id, sizeof(temporary_id), "guest-%lld-%02d",
                 static_cast<long long>(slot.created_at), slot_index + 1);
        slot.person_id = temporary_id;
        slot.display_name = "Khach gui do";
        for(int i = 0; i < kEmbeddingDimension; ++i)
            slot.embedding[i] =
                g_deposit_sum[i] / static_cast<float>(g_deposit_frames);
        if(!save_locker_state()) {
            g_slots[slot_index] = LockerSlot{};
            show_result(false, 0, "LOI LUU DU LIEU",
                        "Khong the luu khuon mat. Vui long thu lai.");
            printf("[SMART_LOCKER][STATE] deposit aborted; lock remains closed\n");
            return true;
        }

        printf("[SMART_LOCKER][TEMP_FACE] SAVED id=%s slot=%02d "
               "liveness=%.3f storage=persistent expires_in=%llds\n",
               slot.person_id.c_str(), slot_index + 1, liveness_score,
               static_cast<long long>(retention_seconds()));
        printf("[SMART_LOCKER][HARDWARE_MOCK] LOCK_OPEN slot=%02d "
               "action=DEPOSIT person=%s\n",
               slot_index + 1, slot.person_id.c_str());
        show_result(true, slot_index + 1, "CUA TU DA MO",
                    "Dat do vao ngan va dong cua tu.");
        return true;
    }

    int best_slot = -1;
    float best_distance = 9999.0f;
    for(int slot_index = 0; slot_index < kLockerCount; ++slot_index) {
        if(!g_slots[slot_index].occupied)
            continue;
        float squared_distance = 0.0f;
        for(int i = 0; i < kEmbeddingDimension; ++i) {
            const float difference =
                embedding[i] - g_slots[slot_index].embedding[i];
            squared_distance += difference * difference;
        }
        const float distance = std::sqrt(squared_distance);
        if(distance < best_distance) {
            best_distance = distance;
            best_slot = slot_index;
        }
    }

    const float threshold = temporary_face_threshold();
    if(best_slot < 0 || best_distance >= threshold) {
        g_pickup_candidate = -1;
        g_pickup_frames = 0;
        locker_ui_set_hint("Khong tim thay do - vui long thu lai");
        printf("[SMART_LOCKER][TEMP_FACE] NO_MATCH best=%.3f threshold=%.3f\n",
               best_distance, threshold);
        return false;
    }

    if(g_pickup_candidate == best_slot)
        ++g_pickup_frames;
    else {
        g_pickup_candidate = best_slot;
        g_pickup_frames = 1;
    }

    char hint[80];
    snprintf(hint, sizeof(hint), "Dang xac minh %d/%d",
             g_pickup_frames, kConfirmationFrames);
    locker_ui_set_hint(hint);
    if(g_pickup_frames < kConfirmationFrames)
        return false;

    const std::string temporary_id = g_slots[best_slot].person_id;
    printf("[SMART_LOCKER][TEMP_FACE] MATCH id=%s slot=%02d "
           "distance=%.3f threshold=%.3f liveness=%.3f\n",
           temporary_id.c_str(), best_slot + 1, best_distance,
           threshold, liveness_score);
    const LockerSlot picked_up_slot = g_slots[best_slot];
    g_slots[best_slot] = LockerSlot{};
    if(!save_locker_state()) {
        g_slots[best_slot] = picked_up_slot;
        show_result(false, 0, "LOI LUU DU LIEU",
                    "Khong the cap nhat du lieu. Vui long thu lai.");
        printf("[SMART_LOCKER][STATE] pickup aborted; lock remains closed\n");
        return true;
    }
    printf("[SMART_LOCKER][HARDWARE_MOCK] LOCK_OPEN slot=%02d "
           "action=PICKUP person=%s\n",
           best_slot + 1, temporary_id.c_str());
    printf("[SMART_LOCKER][TEMP_FACE] DELETED id=%s reason=PICKUP\n",
           temporary_id.c_str());
    show_result(true, best_slot + 1, "CUA TU DA MO",
                "Lay do va dong cua tu sau khi hoan tat.");
    return true;
}
