#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "rknn_api.h"

#define MODEL_PATH "face_detector.rknn"

int main()
{
    printf("=== RKNN SIMPLE TEST START ===\n");

    FILE *fp = fopen(MODEL_PATH, "rb");
    if (!fp)
    {
        printf("Open model failed: %s\n", MODEL_PATH);
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    int model_size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    unsigned char *model_data = (unsigned char *)malloc(model_size);
    fread(model_data, 1, model_size, fp);
    fclose(fp);

    rknn_context ctx;

    int ret = rknn_init(&ctx, model_data, model_size, 0, NULL);
    if (ret < 0)
    {
        printf("rknn_init failed! ret=%d\n", ret);
        return -1;
    }

    printf("rknn_init success\n");

    // query input/output
    rknn_input_output_num io_num;
    rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    printf("input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));

    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
        printf("input[%d] dims=%d type=%d size=%d\n",
               i,
               input_attrs[i].n_dims,
               input_attrs[i].type,
               input_attrs[i].size);
    }

    // tạo input giả (random)
    rknn_input inputs[1];
    memset(inputs, 0, sizeof(inputs));

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].size = input_attrs[0].size;

    unsigned char *fake_data = (unsigned char *)malloc(inputs[0].size);

    for (int i = 0; i < inputs[0].size; i++)
        fake_data[i] = rand() % 255;

    inputs[0].buf = fake_data;
    inputs[0].pass_through = 0;

    rknn_inputs_set(ctx, io_num.n_input, inputs);

    rknn_output outputs[io_num.n_output];
    memset(outputs, 0, sizeof(outputs));

    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 1;
    }

    ret = rknn_run(ctx, NULL);
    if (ret < 0)
    {
        printf("rknn_run failed! ret=%d\n", ret);
        return -1;
    }

    rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);

    printf("=== OUTPUT INFO ===\n");
    for (int i = 0; i < io_num.n_output; i++)
    {
        printf("output[%d] size=%d\n", i, outputs[i].size);
    }

    rknn_outputs_release(ctx, io_num.n_output, outputs);

    rknn_destroy(ctx);

    free(fake_data);
    free(model_data);

    printf("=== DONE SUCCESS ===\n");
    return 0;
}
