#include "anti_spoof.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "opencv2/imgproc/imgproc.hpp"

namespace {
constexpr float kCropScale = 2.7f;
constexpr int kClassCount = 3;
constexpr int kRealClass = 1;

cv::Rect expandedFaceBox(const cv::Rect& box, const cv::Size& image_size)
{
    if (box.width <= 0 || box.height <= 0 || image_size.width <= 0 ||
        image_size.height <= 0) {
        return cv::Rect();
    }

    const float scale = std::min(
        kCropScale,
        std::min((float)(image_size.width - 1) / (float)box.width,
                 (float)(image_size.height - 1) / (float)box.height));
    // Match Silent-Face-Anti-Spoofing's CropImage::_get_new_box exactly:
    // preserve the expanded crop size by shifting it back inside the frame,
    // then include both right/bottom endpoints in the resulting ROI.
    const float crop_width = (float)box.width * scale;
    const float crop_height = (float)box.height * scale;
    const float center_x = (float)box.x + (float)box.width / 2.0f;
    const float center_y = (float)box.y + (float)box.height / 2.0f;

    float left = center_x - crop_width / 2.0f;
    float top = center_y - crop_height / 2.0f;
    float right = center_x + crop_width / 2.0f;
    float bottom = center_y + crop_height / 2.0f;

    if (left < 0.0f) {
        right -= left;
        left = 0.0f;
    }
    if (top < 0.0f) {
        bottom -= top;
        top = 0.0f;
    }
    if (right > (float)(image_size.width - 1)) {
        left -= right - (float)image_size.width + 1.0f;
        right = (float)(image_size.width - 1);
    }
    if (bottom > (float)(image_size.height - 1)) {
        top -= bottom - (float)image_size.height + 1.0f;
        bottom = (float)(image_size.height - 1);
    }

    const int x0 = std::max(0, (int)left);
    const int y0 = std::max(0, (int)top);
    const int x1 = std::min(image_size.width - 1, (int)right);
    const int y1 = std::min(image_size.height - 1, (int)bottom);
    return cv::Rect(x0, y0, x1 - x0 + 1, y1 - y0 + 1);
}

float dequantize(int32_t value, const rknn_tensor_attr& attr)
{
    return ((float)value - (float)attr.zp) * attr.scale;
}

void softmax3(const float logits[kClassCount], float probs[kClassCount])
{
    const float max_logit = std::max(logits[0], std::max(logits[1], logits[2]));
    float sum = 0.0f;
    for (int i = 0; i < kClassCount; ++i) {
        probs[i] = std::exp(logits[i] - max_logit);
        sum += probs[i];
    }
    if (sum <= 0.0f)
        return;
    for (int i = 0; i < kClassCount; ++i)
        probs[i] /= sum;
}
}  // namespace

int init_anti_spoof_model(const char* model_path, AntiSpoofContext* ctx)
{
    if (!model_path || !ctx)
        return -1;
    *ctx = AntiSpoofContext();

    int ret = rknn_init(&ctx->rknn_ctx, (char*)model_path, 0, 0, nullptr);
    if (ret < 0) {
        printf("[anti-spoof] rknn_init failed path=%s ret=%d\n",
               model_path, ret);
        return -1;
    }

    rknn_input_output_num io_num{};
    ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_IN_OUT_NUM,
                     &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC || io_num.n_input != 1 || io_num.n_output != 1) {
        printf("[anti-spoof] invalid IO count input=%u output=%u ret=%d\n",
               io_num.n_input, io_num.n_output, ret);
        release_anti_spoof_model(ctx);
        return -1;
    }

    ctx->input_attr.index = 0;
    ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_NATIVE_NHWC_INPUT_ATTR,
                     &ctx->input_attr, sizeof(ctx->input_attr));
    if (ret != RKNN_SUCC) {
        printf("[anti-spoof] query input failed ret=%d\n", ret);
        release_anti_spoof_model(ctx);
        return -1;
    }

    ctx->output_attr.index = 0;
    ret = rknn_query(ctx->rknn_ctx, RKNN_QUERY_NATIVE_NHWC_OUTPUT_ATTR,
                     &ctx->output_attr, sizeof(ctx->output_attr));
    if (ret != RKNN_SUCC || ctx->output_attr.n_elems < kClassCount) {
        printf("[anti-spoof] query output failed elems=%u ret=%d\n",
               ctx->output_attr.n_elems, ret);
        release_anti_spoof_model(ctx);
        return -1;
    }

    // RV1106 zero-copy accepts NHWC uint8 here and performs the model's input
    // quantization internally.
    ctx->input_attr.type = RKNN_TENSOR_UINT8;
    ctx->input_attr.fmt = RKNN_TENSOR_NHWC;
    ctx->input_attr.pass_through = 0;

    if (ctx->input_attr.n_dims != 4) {
        printf("[anti-spoof] unsupported input dims=%u\n",
               ctx->input_attr.n_dims);
        release_anti_spoof_model(ctx);
        return -1;
    }
    ctx->model_height = (int)ctx->input_attr.dims[1];
    ctx->model_width = (int)ctx->input_attr.dims[2];
    ctx->model_channels = (int)ctx->input_attr.dims[3];
    if (ctx->model_width != 80 || ctx->model_height != 80 ||
        ctx->model_channels != 3) {
        printf("[anti-spoof] expected NHWC 80x80x3, got %dx%dx%d\n",
               ctx->model_width, ctx->model_height, ctx->model_channels);
        release_anti_spoof_model(ctx);
        return -1;
    }

    ctx->input_mem = rknn_create_mem(ctx->rknn_ctx,
                                     ctx->input_attr.size_with_stride);
    ctx->output_mem = rknn_create_mem(ctx->rknn_ctx,
                                      ctx->output_attr.size_with_stride);
    if (!ctx->input_mem || !ctx->output_mem) {
        printf("[anti-spoof] memory allocation failed\n");
        release_anti_spoof_model(ctx);
        return -1;
    }
    ret = rknn_set_io_mem(ctx->rknn_ctx, ctx->input_mem, &ctx->input_attr);
    if (ret != RKNN_SUCC) {
        printf("[anti-spoof] bind input failed ret=%d\n", ret);
        release_anti_spoof_model(ctx);
        return -1;
    }
    ret = rknn_set_io_mem(ctx->rknn_ctx, ctx->output_mem, &ctx->output_attr);
    if (ret != RKNN_SUCC) {
        printf("[anti-spoof] bind output failed ret=%d\n", ret);
        release_anti_spoof_model(ctx);
        return -1;
    }

    printf("[anti-spoof] loaded %s input=%dx%dx%d output=%u type=%s\n",
           model_path, ctx->model_width, ctx->model_height,
           ctx->model_channels, ctx->output_attr.n_elems,
           get_type_string(ctx->output_attr.type));
    return 0;
}

int inference_anti_spoof_model(AntiSpoofContext* ctx,
                               const cv::Mat& image_bgr,
                               const cv::Rect& face_box,
                               float real_threshold,
                               AntiSpoofResult* result)
{
    if (!ctx || !ctx->rknn_ctx || !ctx->input_mem || !ctx->output_mem ||
        !result || image_bgr.empty() || image_bgr.type() != CV_8UC3) {
        return -1;
    }
    *result = AntiSpoofResult();

    const cv::Rect crop_box = expandedFaceBox(face_box, image_bgr.size());
    if (crop_box.width <= 0 || crop_box.height <= 0)
        return -1;

    cv::Mat resized;
    cv::resize(image_bgr(crop_box), resized,
               cv::Size(ctx->model_width, ctx->model_height),
               0, 0, cv::INTER_LINEAR);

    memset(ctx->input_mem->virt_addr, 0, ctx->input_attr.size_with_stride);
    const size_t row_bytes =
        (size_t)ctx->model_width * (size_t)ctx->model_channels;
    const size_t stride_bytes = ctx->input_attr.w_stride
        ? (size_t)ctx->input_attr.w_stride * (size_t)ctx->model_channels
        : row_bytes;
    uint8_t* dst = (uint8_t*)ctx->input_mem->virt_addr;
    for (int row = 0; row < ctx->model_height; ++row) {
        memcpy(dst + (size_t)row * stride_bytes,
               resized.ptr(row), row_bytes);
    }

    const int ret = rknn_run(ctx->rknn_ctx, nullptr);
    if (ret != RKNN_SUCC) {
        printf("[anti-spoof] rknn_run failed ret=%d\n", ret);
        return -1;
    }

    float logits[kClassCount]{};
    if (ctx->output_attr.type == RKNN_TENSOR_INT8) {
        const int8_t* values = (const int8_t*)ctx->output_mem->virt_addr;
        for (int i = 0; i < kClassCount; ++i)
            logits[i] = dequantize(values[i], ctx->output_attr);
    } else if (ctx->output_attr.type == RKNN_TENSOR_UINT8) {
        const uint8_t* values = (const uint8_t*)ctx->output_mem->virt_addr;
        for (int i = 0; i < kClassCount; ++i)
            logits[i] = dequantize(values[i], ctx->output_attr);
    } else if (ctx->output_attr.type == RKNN_TENSOR_FLOAT32) {
        const float* values = (const float*)ctx->output_mem->virt_addr;
        for (int i = 0; i < kClassCount; ++i)
            logits[i] = values[i];
    } else {
        printf("[anti-spoof] unsupported output type=%s\n",
               get_type_string(ctx->output_attr.type));
        return -1;
    }

    softmax3(logits, result->probabilities);
    result->real_score = result->probabilities[kRealClass];
    const int predicted_class =
        (int)(std::max_element(result->probabilities,
                               result->probabilities + kClassCount) -
              result->probabilities);
    result->is_real = predicted_class == kRealClass &&
                      result->real_score >= real_threshold;
    return 0;
}

int release_anti_spoof_model(AntiSpoofContext* ctx)
{
    if (!ctx)
        return 0;
    if (ctx->input_mem) {
        rknn_destroy_mem(ctx->rknn_ctx, ctx->input_mem);
        ctx->input_mem = nullptr;
    }
    if (ctx->output_mem) {
        rknn_destroy_mem(ctx->rknn_ctx, ctx->output_mem);
        ctx->output_mem = nullptr;
    }
    if (ctx->rknn_ctx) {
        rknn_destroy(ctx->rknn_ctx);
        ctx->rknn_ctx = 0;
    }
    return 0;
}
