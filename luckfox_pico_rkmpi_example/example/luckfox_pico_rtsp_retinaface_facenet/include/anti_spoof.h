#ifndef ANTI_SPOOF_H
#define ANTI_SPOOF_H

#include <stdint.h>

#include "opencv2/core/core.hpp"
#include "rknn_api.h"

struct AntiSpoofContext {
    rknn_context rknn_ctx = 0;
    rknn_tensor_attr input_attr{};
    rknn_tensor_attr output_attr{};
    rknn_tensor_mem* input_mem = nullptr;
    rknn_tensor_mem* output_mem = nullptr;
    int model_width = 0;
    int model_height = 0;
    int model_channels = 0;
};

struct AntiSpoofResult {
    bool is_real = false;
    float real_score = 0.0f;
    float probabilities[3] = {0.0f, 0.0f, 0.0f};
};

int init_anti_spoof_model(const char* model_path, AntiSpoofContext* ctx);
int inference_anti_spoof_model(AntiSpoofContext* ctx,
                               const cv::Mat& image_bgr,
                               const cv::Rect& face_box,
                               float real_threshold,
                               AntiSpoofResult* result);
int release_anti_spoof_model(AntiSpoofContext* ctx);

#endif  // ANTI_SPOOF_H
