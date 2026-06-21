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
#include <time.h>
#include <unistd.h>
#include <vector>
#include <algorithm>

#include "rtsp_demo.h"
#include "luckfox_mpi.h"
#include "retinaface_facenet.h"
#include "face_align.h"
#include "face_db.h"
#include "face_event_manager.h"
#include "face_test_runner.h"
#include "telegram_client.h"
#include "attendance_utils.h"

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
#define FACE_DIST_THRESHOLD 0.95f

// -------------------------------------------------------------------------
// Timing helper
// -------------------------------------------------------------------------
static inline long ts_diff_us(const struct timespec& a,
                               const struct timespec& b)
{
    return (b.tv_sec - a.tv_sec) * 1000000L
         + (b.tv_nsec - a.tv_nsec) / 1000L;
}

static inline float confidence_from_face_distance(float dist)
{
    if (dist >= FACE_DIST_THRESHOLD)
        return 0.0f;

    float normalized = 1.0f - (dist / FACE_DIST_THRESHOLD);
    float confidence = 0.80f + 0.20f * normalized;
    return std::max(0.0f, std::min(confidence, 1.0f));
}

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

// -------------------------------------------------------------------------
// Usage
// -------------------------------------------------------------------------
static void print_usage(const char *prog)
{
    printf("Usage:\n");
    printf("  Register: %s register <retina_model> <facenet_model>"
           " <db_path> <name> <image>\n", prog);
    printf("  Run:      %s run      <retina_model> <facenet_model>"
           " <db_path>\n", prog);
    printf("  Test:     %s test     [image_dir]\n", prog);
}

// -------------------------------------------------------------------------
// compute_embedding: resize image to model input, run RetinaFace, align
// (or letterbox), run FaceNet, return L2-normalised 128-d vector.
// Returns 0 on success, -1 if no face detected.
// -------------------------------------------------------------------------
static int compute_embedding(const cv::Mat&      image,
                              rknn_app_context_t* retina_ctx,
                              rknn_app_context_t* facenet_ctx,
                              float*              out_fp32)
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
        return -1;
    }

    // Use highest-confidence detection (first after NMS / score sort)
    object_detect_result *det = &od.results[0];
    printf("[embed] Face detected  conf=%.2f  box=(%d %d %d %d)\n",
           det->prop,
           det->box.left, det->box.top, det->box.right, det->box.bottom);

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
                  const char *db_path)
{
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

    system("RkLunch-stop.sh");

    const int width        = DISP_WIDTH;
    const int height       = DISP_HEIGHT;
    const int model_width  = MODEL_WIDTH;
    const int model_height = MODEL_HEIGHT;
    const int facenet_width  = FACENET_WIDTH;
    const int facenet_height = FACENET_HEIGHT;

    const float scale_x = (float)width  / (float)model_width;
    const float scale_y = (float)height / (float)model_height;

    printf("[run] USE_FACE_ALIGNMENT=%d  threshold=%.2f\n",
           USE_FACE_ALIGNMENT, (double)FACE_DIST_THRESHOLD);

    // -----------------------------------------------------------------------
    // Init RKNN models
    // -----------------------------------------------------------------------
    rknn_app_context_t app_retinaface_ctx;
    rknn_app_context_t app_facenet_ctx;
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

    float *face_fp32 = (float *)malloc(sizeof(float) * FACE_DB_EMBED_DIM);

    // Per-face result storage (parallel arrays for detection loop + draw loop)
    float face_dists[128];
    float face_confidences[128];
    char  face_ids[128][32];
    char  face_names[128][FACE_DB_NAME_LEN];

    // -----------------------------------------------------------------------
    // VENC frame buffer
    // -----------------------------------------------------------------------
    VENC_STREAM_S stFrame;
    stFrame.pstPack = (VENC_PACK_S *)malloc(sizeof(VENC_PACK_S));
    RK_U32 H264_TimeRef = 0;
    VIDEO_FRAME_INFO_S stViFrame;
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
    FaceEventManager attendance_events;
    attendance_events.setAttendanceSuccessCallback(
        [](const std::string& name, const std::string& time) {
            printf("[attendance] success hook: %s at %s\n",
                   name.c_str(), time.c_str());
        });
    attendance_events.setAttendanceDataCallback(
        [&telegram](const AttendanceData& data,
                    const std::string& image_path) {
            onAttendanceSuccess(telegram, data, image_path);
        });

    // -----------------------------------------------------------------------
    // Main loop
    // -----------------------------------------------------------------------
    struct timespec t_frame_start, t_retina_done, t_align_done, t_facenet_done;

    while (1) {
        clock_gettime(CLOCK_MONOTONIC, &t_frame_start);

        h264_frame.stVFrame.u32TimeRef = H264_TimeRef++;
        h264_frame.stVFrame.u64PTS     = TEST_COMM_GetNowUs();

        s32Ret = RK_MPI_VI_GetChnFrame(0, 0, &stViFrame, -1);
        if (s32Ret == RK_SUCCESS) {
            void *vi_data = RK_MPI_MB_Handle2VirAddr(stViFrame.stVFrame.pMbBlk);

            cv::Mat yuv420sp(height + height / 2, width, CV_8UC1, vi_data);
            cv::Mat bgr(height, width, CV_8UC3);
            cv::cvtColor(yuv420sp, bgr, cv::COLOR_YUV420sp2BGR);

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

            // -----------------------------------------------------------
            // Phase 1: per-face embedding + DB lookup
            // -----------------------------------------------------------
            long align_us   = 0;
            long facenet_us = 0;

            for (int i = 0; i < od_results.count; i++) {
                object_detect_result *det = &(od_results.results[i]);

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
                int sX = (int)((float)det->box.left   * scale_x);
                int sY = (int)((float)det->box.top    * scale_y);
                int eX = (int)((float)det->box.right  * scale_x);
                int eY = (int)((float)det->box.bottom * scale_y);
                sX = std::max(0, std::min(sX, width  - 1));
                sY = std::max(0, std::min(sY, height - 1));
                eX = std::max(0, std::min(eX, width  - 1));
                eY = std::max(0, std::min(eY, height - 1));
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
                float  dist;
                int    idx = face_db_find(&db, face_fp32, &dist);
                face_dists[i] = dist;
                if (idx >= 0 && dist < FACE_DIST_THRESHOLD) {
                    strncpy(face_names[i], db.entries[idx].name,
                            FACE_DB_NAME_LEN - 1);
                    face_names[i][FACE_DB_NAME_LEN - 1] = '\0';
                    face_confidences[i] =
                        confidence_from_face_distance(dist);
                    snprintf(face_ids[i], sizeof(face_ids[i]),
                             "user_%03d", idx + 1);
                } else {
                    strncpy(face_names[i], "UNKNOWN", FACE_DB_NAME_LEN - 1);
                    face_confidences[i] = 0.0f;
                    face_ids[i][0] = '\0';
                }

                if (strcmp(face_names[i], "UNKNOWN") != 0) {
                    FaceResult event_result;
                    event_result.recognized = true;
                    event_result.person_id = face_ids[i];
                    event_result.name = face_names[i];
                    event_result.confidence = face_confidences[i];
                    attendance_events.onFrame(Frame(bgr), event_result);
                }
            }

            // -----------------------------------------------------------
            // Phase 2: draw bounding boxes and labels
            // -----------------------------------------------------------
            for (int i = 0; i < od_results.count; i++) {
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
                cv::Scalar color = matched
                                   ? cv::Scalar(0, 255, 0)
                                   : cv::Scalar(0, 0, 255);

                cv::rectangle(frame,
                              cv::Point(sX, sY), cv::Point(eX, eY),
                              color, 3);

                // Label: "Name (dist)" or "UNKNOWN"
                char label[FACE_DB_NAME_LEN + 16];
                if (matched)
                    snprintf(label, sizeof(label), "%s (%.2f)",
                             face_names[i], face_dists[i]);
                else
                    snprintf(label, sizeof(label), "UNKNOWN");

                printf("[face %d] %s  dist=%.3f\n",
                       i, face_names[i], face_dists[i]);

                cv::putText(frame, label,
                            cv::Point(sX, std::max(0, sY - 8)),
                            cv::FONT_HERSHEY_SIMPLEX, 0.6, color, 2);
            }

            memcpy(enc_data, frame.data, width * height * 3);

            // -----------------------------------------------------------
            // Benchmark
            // -----------------------------------------------------------
            struct timespec t_end;
            clock_gettime(CLOCK_MONOTONIC, &t_end);
            printf("[bench] Retina=%ld us  Align=%ld us  FaceNet=%ld us"
                   "  Total=%ld us  Faces=%d\n",
                   ts_diff_us(t_frame_start, t_retina_done),
                   align_us, facenet_us,
                   ts_diff_us(t_frame_start, t_end),
                   od_results.count);
        }

        // Encode H264
        RK_MPI_VENC_SendFrame(0, &h264_frame, -1);

        // RTSP transmit
        s32Ret = RK_MPI_VENC_GetStream(0, &stFrame, -1);
        if (s32Ret == RK_SUCCESS && g_rtsplive && g_rtsp_session) {
            void *pData = RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
            rtsp_tx_video(g_rtsp_session,
                          (uint8_t *)pData, stFrame.pstPack->u32Len,
                          stFrame.pstPack->u64PTS);
            rtsp_do_event(g_rtsplive);
        }

        s32Ret = RK_MPI_VI_ReleaseChnFrame(0, 0, &stViFrame);
        if (s32Ret != RK_SUCCESS)
            RK_LOGE("RK_MPI_VI_ReleaseChnFrame fail %x", s32Ret);

        s32Ret = RK_MPI_VENC_ReleaseStream(0, &stFrame);
        if (s32Ret != RK_SUCCESS)
            RK_LOGE("RK_MPI_VENC_ReleaseStream fail %x", s32Ret);
    }

    // -----------------------------------------------------------------------
    // Cleanup
    // -----------------------------------------------------------------------
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
    return 0;
}

// =========================================================================
// TEST mode - static image simulation without camera or ML inference
// =========================================================================
static int do_test(const char *image_dir)
{
    TelegramClient telegram;
    FaceEventManager attendance_events;

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

    FaceTestRunner runner(&attendance_events);
    return runner.run(image_dir ? image_dir : "/test_images");
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
        // run <retina> <facenet> <db>  => 5 args
        if (argc != 5) {
            printf("run needs: <retina_model> <facenet_model> <db_path>\n");
            return -1;
        }
        return do_run(argv[2], argv[3], argv[4]);
    }

    if (strcmp(argv[1], "test") == 0) {
        // test [image_dir], defaults to /test_images
        if (argc > 3) {
            printf("test needs: [image_dir]\n");
            return -1;
        }
        return do_test(argc == 3 ? argv[2] : "/test_images");
    }

    print_usage(argv[0]);
    return -1;
}
