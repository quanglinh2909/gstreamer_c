#include "yolov8_seg.h"

#include <string.h>

int init_yolov8_seg_model(const char* model_path, rknn_app_context_t* app_ctx) {
    return init_yolov8_model(model_path, app_ctx);
}

int release_yolov8_seg_model(rknn_app_context_t* app_ctx) {
    return release_yolov8_model(app_ctx);
}

int inference_yolov8_seg_model(rknn_app_context_t* app_ctx, image_buffer_t* img, object_detect_result_list* od_results) {
    if (app_ctx == nullptr || img == nullptr || od_results == nullptr) {
        return -1;
    }

    letterbox_t letter_box;
    memset(&letter_box, 0, sizeof(letter_box));
    letter_box.scale = 1.0f;

    rknn_input inputs[1];
    rknn_output outputs[16];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));

    inputs[0].index = 0;
    inputs[0].type = RKNN_TENSOR_UINT8;
    inputs[0].fmt = RKNN_TENSOR_NHWC;
    inputs[0].size = img->size;
    inputs[0].pass_through = 0;
    inputs[0].buf = img->virt_addr;

    int ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0) {
        return ret;
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        return ret;
    }

    for (int i = 0; i < app_ctx->io_num.n_output; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 1;
    }

    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0) {
        return ret;
    }

    ret = post_process_seg(app_ctx, outputs, &letter_box, BOX_THRESH, NMS_THRESH, od_results);
    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);
    return ret;
}
