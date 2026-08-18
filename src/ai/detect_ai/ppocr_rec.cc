#include "ppocr_rec.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <vector>

#include "file_utils.h"
#include "image_utils.h"
#include "npu_core.h"
#include "ppocr_dump.h"
#include "rknn_in.h"
#include "rknn_out.h"

int init_ppocr_rec_model(const char* model_path, rknn_app_context_t* app_ctx) {
    int model_len = 0;
    char* model = NULL;
    rknn_context ctx = 0;

    model_len = read_data_from_file(model_path, &model);
    if (model_len < 0 || model == NULL) {
        printf("ppocr_rec: khong doc duoc model %s\n", model_path);
        return -1;
    }

    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("ppocr_rec: rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    npu_set_multicore(ctx, "ppocr_rec");

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("ppocr_rec: rknn_query IN_OUT_NUM fail! ret=%d\n", ret);
        return -1;
    }

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("ppocr_rec: rknn_query INPUT_ATTR fail! ret=%d\n", ret);
            return -1;
        }
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("ppocr_rec: rknn_query OUTPUT_ATTR fail! ret=%d\n", ret);
            return -1;
        }
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->is_quant = (output_attrs[0].qnt_type == RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC &&
                         output_attrs[0].type != RKNN_TENSOR_FLOAT16);
    app_ctx->io_num = io_num;
    app_ctx->input_attrs =
        (rknn_tensor_attr*)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs,
           io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs =
        (rknn_tensor_attr*)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs,
           io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    } else {
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }

    // Output is [1, seq_len, num_classes]; num_classes = 1 blank + dictionary
    // (+ 1 space when the model was trained with use_space_char).
    printf("ppocr_rec: input %dx%d, dau ra [%d, %d, %d]\n",
           app_ctx->model_width, app_ctx->model_height, output_attrs[0].dims[0],
           output_attrs[0].dims[1], output_attrs[0].dims[2]);

    // KHÔNG bật zero-copy đầu vào ở đây, dù mã đã sẵn sàng (`app_ctx->zc_in`).
    // Đo A/B trong cùng một tiến trình, 40 vòng mỗi bên:
    //     det 480×480: nạp 19,35 → 0,62 ms, TỔNG 56,17 → 52,83 ms  (đáng)
    //     rec 320×48 : nạp  1,25 → 0,06 ms, TỔNG 17,38 → 18,28 ms  (LỖ)
    // Bộ nhớ của `rknn_create_mem` không đi qua cache CPU, nên phần tiết kiệm
    // được ở khâu nạp bị `rknn_run` đòi lại khi NPU đọc vào. Với det thì ảnh to
    // nên vẫn lời; với rec thì khâu nạp vốn đã rẻ, đổi lại chỉ tổ chậm hơn.
    // Kết quả đọc thì giống hệt ở cả hai đường (đối chứng chuỗi CTC trên ảnh
    // thật), nên đây thuần tuý là chuyện tốc độ.
    return 0;
}

int release_ppocr_rec_model(rknn_app_context_t* app_ctx) {
    if (app_ctx->zc_in != NULL) {
        rknn_in_release(app_ctx->rknn_ctx, app_ctx->zc_in);
        delete app_ctx->zc_in;
        app_ctx->zc_in = NULL;
    }
    if (app_ctx->input_attrs != NULL) {
        free(app_ctx->input_attrs);
        app_ctx->input_attrs = NULL;
    }
    if (app_ctx->output_attrs != NULL) {
        free(app_ctx->output_attrs);
        app_ctx->output_attrs = NULL;
    }
    if (app_ctx->rknn_ctx != 0) {
        rknn_destroy(app_ctx->rknn_ctx);
        app_ctx->rknn_ctx = 0;
    }
    return 0;
}

int inference_ppocr_rec_model(rknn_app_context_t* app_ctx,
                              image_buffer_t* img,
                              ppocr_char_t* chars_out,
                              int* char_count_out,
                              int* seq_len_out) {
    if (!app_ctx || !img || !img->virt_addr || !chars_out || !char_count_out) {
        return -1;
    }
    *char_count_out = 0;
    if (seq_len_out) *seq_len_out = 0;

    const int width = app_ctx->model_width;
    const int height = app_ctx->model_height;
    const int channels = app_ctx->model_channel > 0 ? app_ctx->model_channel : 3;
    const int src_stride =
        img->width_stride > 0 ? img->width_stride * channels : img->width * channels;

    static std::atomic<int> dumpCount{0};
    ppocr_dump_input("rec", img, width, height, channels, dumpCount);

    int ret = 0;
    if (app_ctx->zc_in != NULL) {
        rknn_in_fill_u8(*app_ctx->zc_in, img->virt_addr, width, height, channels,
                        src_stride);
    } else {
        // Same conversion as face_recognition: hand rknn float32 in 0..255 and
        // let the mean/std baked into the model do the /255. Cheap here — the
        // input is only 48x320x3.
        std::vector<float> input_buf(static_cast<size_t>(width) * height * channels);
        if (app_ctx->input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
            for (int c = 0; c < channels; ++c) {
                for (int y = 0; y < height; ++y) {
                    const uint8_t* src_row = img->virt_addr + y * src_stride;
                    for (int x = 0; x < width; ++x) {
                        input_buf[c * width * height + y * width + x] =
                            static_cast<float>(src_row[x * channels + c]);
                    }
                }
            }
        } else {
            for (int y = 0; y < height; ++y) {
                const uint8_t* src_row = img->virt_addr + y * src_stride;
                float* dst_row = input_buf.data() + y * width * channels;
                for (int x = 0; x < width * channels; ++x) {
                    dst_row[x] = static_cast<float>(src_row[x]);
                }
            }
        }

        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_FLOAT32;
        inputs[0].fmt = app_ctx->input_attrs[0].fmt;
        inputs[0].size = input_buf.size() * sizeof(float);
        inputs[0].pass_through = 0;
        inputs[0].buf = input_buf.data();

        ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
        if (ret < 0) {
            printf("ppocr_rec: rknn_inputs_set fail! ret=%d\n", ret);
            return -1;
        }
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("ppocr_rec: rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    const rknn_tensor_attr& oa = app_ctx->output_attrs[0];
    const int wantFloat = rknn_out_want_float(oa.type);

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].index = 0;
    outputs[0].want_float = wantFloat;
    ret = rknn_outputs_get(app_ctx->rknn_ctx, 1, outputs, NULL);
    if (ret < 0) {
        printf("ppocr_rec: rknn_outputs_get fail! ret=%d\n", ret);
        return -1;
    }

    const int seq_len = oa.n_dims >= 3 ? (int)oa.dims[1] : 0;
    const int num_classes = oa.n_dims >= 3 ? (int)oa.dims[2] : 0;
    if (seq_len <= 0 || num_classes <= 1 || outputs[0].buf == NULL) {
        rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
        return -1;
    }
    // Đầu ra chỉ 40×31 giá trị nên `want_float` gần như không tốn gì ở đây;
    // đi qua RknnOutView là để bản int8 của model chạy được mà không phải
    // dequant riêng — phép lượng tử hoá affine đơn điệu nên argmax không đổi.
    const RknnOutView view = rknn_out_view(oa, outputs[0].buf, wantFloat);
    if (seq_len_out) *seq_len_out = seq_len;

    // Greedy CTC: argmax per timestep, then drop blanks (class 0) and repeats.
    // The head already emits probabilities (softmax is part of the graph), so
    // the max value is usable as a confidence directly.
    int count = 0;
    int prev = -1;
    for (int t = 0; t < seq_len && count < PPOCR_MAX_CHARS; ++t) {
        const size_t base = (size_t)t * num_classes;
        int best = 0;
        float bestVal = view.value(base);
        for (int c = 1; c < num_classes; ++c) {
            const float v = view.value(base + (size_t)c);
            if (v > bestVal) {
                bestVal = v;
                best = c;
            }
        }
        if (best != 0 && best != prev) {
            chars_out[count].index = best - 1;  // class 0 is the CTC blank
            chars_out[count].score = bestVal > 1.0f ? 1.0f : bestVal;
            chars_out[count].step = t;
            ++count;
        }
        prev = best;
    }
    *char_count_out = count;

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
    return 0;
}
