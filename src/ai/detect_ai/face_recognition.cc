#include "face_recognition.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <vector>

#include "file_utils.h"
#include "image_utils.h"

static void dump_tensor_attr(rknn_tensor_attr* attr) {
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[0], attr->dims[1], attr->dims[2], attr->dims[3],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

int init_face_recognition_model(const char* model_path, rknn_app_context_t* app_ctx) {
    int ret;
    int model_len = 0;
    char* model = NULL;
    rknn_context ctx = 0;

    model_len = read_data_from_file(model_path, &model);
    if (model_len < 0 || model == NULL) {
        printf("load_model fail!\n");
        return -1;
    }

    ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("rknn_query fail! ret=%d\n", ret);
        return -1;
    }
    printf("face_recognition input num: %d, output num: %d\n", io_num.n_input, io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    printf("face_recognition input tensors:\n");
    for (int i = 0; i < io_num.n_input; ++i) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    printf("face_recognition output tensors:\n");
    for (int i = 0; i < io_num.n_output; ++i) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]), sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) {
            printf("rknn_query fail! ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(output_attrs[i]));
    }

    app_ctx->rknn_ctx = ctx;
    app_ctx->is_quant = false;
    app_ctx->io_num = io_num;
    app_ctx->input_attrs = (rknn_tensor_attr*)malloc(io_num.n_input * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->input_attrs, input_attrs, io_num.n_input * sizeof(rknn_tensor_attr));
    app_ctx->output_attrs = (rknn_tensor_attr*)malloc(io_num.n_output * sizeof(rknn_tensor_attr));
    memcpy(app_ctx->output_attrs, output_attrs, io_num.n_output * sizeof(rknn_tensor_attr));

    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        app_ctx->model_channel = input_attrs[0].dims[1];
        app_ctx->model_height = input_attrs[0].dims[2];
        app_ctx->model_width = input_attrs[0].dims[3];
    } else {
        app_ctx->model_height = input_attrs[0].dims[1];
        app_ctx->model_width = input_attrs[0].dims[2];
        app_ctx->model_channel = input_attrs[0].dims[3];
    }
    printf("face_recognition input height=%d, width=%d, channel=%d\n",
           app_ctx->model_height, app_ctx->model_width, app_ctx->model_channel);
    return 0;
}

int release_face_recognition_model(rknn_app_context_t* app_ctx) {
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

int inference_face_recognition_model(rknn_app_context_t* app_ctx,
                                     image_buffer_t* img,
                                     face_embedding_result_t* embedding_result) {
    if (!app_ctx || !img || !embedding_result || !img->virt_addr) {
        return -1;
    }

    memset(embedding_result, 0, sizeof(*embedding_result));

    const int width = app_ctx->model_width;
    const int height = app_ctx->model_height;
    const int channels = app_ctx->model_channel > 0 ? app_ctx->model_channel : 3;
    const int src_stride = img->width_stride > 0 ? img->width_stride * channels : img->width * channels;

    std::vector<float> input_buf(static_cast<size_t>(width) * height * channels);
    if (app_ctx->input_attrs[0].fmt == RKNN_TENSOR_NCHW) {
        for (int c = 0; c < channels; ++c) {
            for (int y = 0; y < height; ++y) {
                const uint8_t* src_row = img->virt_addr + y * src_stride;
                for (int x = 0; x < width; ++x) {
                    input_buf[c * width * height + y * width + x] = static_cast<float>(src_row[x * channels + c]);
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

    int ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
    if (ret < 0) {
        printf("face_recognition rknn_inputs_set fail! ret=%d\n", ret);
        return -1;
    }

    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("face_recognition rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    rknn_output outputs[4];
    memset(outputs, 0, sizeof(outputs));
    for (uint32_t i = 0; i < app_ctx->io_num.n_output && i < 4; ++i) {
        outputs[i].index = i;
        outputs[i].want_float = 1;
    }
    ret = rknn_outputs_get(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs, NULL);
    if (ret < 0) {
        printf("face_recognition rknn_outputs_get fail! ret=%d\n", ret);
        return -1;
    }

    const int out_elems = app_ctx->output_attrs[0].n_elems;
    const int copy_dim = out_elems < FACE_EMBEDDING_DIM ? out_elems : FACE_EMBEDDING_DIM;
    const float* out = static_cast<const float*>(outputs[0].buf);
    embedding_result->valid = 1;
    embedding_result->dim = copy_dim;
    for (int i = 0; i < copy_dim; ++i) {
        embedding_result->embedding[i] = out[i];
    }

    rknn_outputs_release(app_ctx->rknn_ctx, app_ctx->io_num.n_output, outputs);
    return 0;
}
