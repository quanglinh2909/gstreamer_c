#include "postprocess.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <vector>

#include "yolov8.h"

namespace {

inline int clamp_int(float val, int min_v, int max_v) {
    if (val < min_v) {
        return min_v;
    }
    if (val > max_v) {
        return max_v;
    }
    return static_cast<int>(val);
}

inline float clamp_float(float val, float min_v, float max_v) {
    if (val < min_v) {
        return min_v;
    }
    if (val > max_v) {
        return max_v;
    }
    return val;
}

inline float sigmoid(float x) {
    return 1.0f / (1.0f + expf(-x));
}

void reset_results(object_detect_result_list* od_results) {
    memset(od_results, 0, sizeof(*od_results));
    od_results->seg_width = AI_SEG_MASK_WIDTH;
    od_results->seg_height = AI_SEG_MASK_HEIGHT;
}

void debug_log_tensor_stats(const char* tag,
                            int tensor_idx,
                            const float* tensor,
                            int channels,
                            int grid_len) {
    if (!tag || !tensor || channels <= 0 || grid_len <= 0) {
        return;
    }

    float min_v = tensor[0];
    float max_v = tensor[0];
    float sum_v = 0.0f;
    int above_01 = 0;
    int above_025 = 0;
    int above_05 = 0;

    for (int i = 0; i < channels * grid_len; ++i) {
        const float v = tensor[i];
        min_v = std::min(min_v, v);
        max_v = std::max(max_v, v);
        sum_v += v;
        if (v > 0.1f) {
            above_01 += 1;
        }
        if (v > 0.25f) {
            above_025 += 1;
        }
        if (v > 0.5f) {
            above_05 += 1;
        }
    }

    fprintf(stdout,
            "[postprocess-debug] %s tensor=%d channels=%d grid=%d min=%.6f max=%.6f mean=%.6f gt0.1=%d gt0.25=%d gt0.5=%d\n",
            tag,
            tensor_idx,
            channels,
            grid_len,
            min_v,
            max_v,
            sum_v / static_cast<float>(channels * grid_len),
            above_01,
            above_025,
            above_05);

    const int sample_count = std::min(grid_len, 5);
    fprintf(stdout, "[postprocess-debug] %s tensor=%d first_values=", tag, tensor_idx);
    for (int i = 0; i < sample_count; ++i) {
        fprintf(stdout, "%s%.6f", (i == 0 ? "" : ","), tensor[i]);
    }
    fprintf(stdout, "\n");
}

float calculate_iou_xywh(const std::vector<float>& boxes, int lhs, int rhs) {
    const float x1a = boxes[lhs * 4 + 0];
    const float y1a = boxes[lhs * 4 + 1];
    const float x2a = x1a + boxes[lhs * 4 + 2];
    const float y2a = y1a + boxes[lhs * 4 + 3];

    const float x1b = boxes[rhs * 4 + 0];
    const float y1b = boxes[rhs * 4 + 1];
    const float x2b = x1b + boxes[rhs * 4 + 2];
    const float y2b = y1b + boxes[rhs * 4 + 3];

    const float inter_x1 = std::max(x1a, x1b);
    const float inter_y1 = std::max(y1a, y1b);
    const float inter_x2 = std::min(x2a, x2b);
    const float inter_y2 = std::min(y2a, y2b);
    const float inter_w = std::max(0.0f, inter_x2 - inter_x1 + 1.0f);
    const float inter_h = std::max(0.0f, inter_y2 - inter_y1 + 1.0f);
    const float inter = inter_w * inter_h;
    const float area_a = (x2a - x1a + 1.0f) * (y2a - y1a + 1.0f);
    const float area_b = (x2b - x1b + 1.0f) * (y2b - y1b + 1.0f);
    const float uni = area_a + area_b - inter;
    if (uni <= 0.0f) {
        return 0.0f;
    }
    return inter / uni;
}

void quick_sort_indice_inverse(std::vector<float>& input, int left, int right, std::vector<int>& indices) {
    if (left >= right) {
        return;
    }
    int low = left;
    int high = right;
    const float key = input[left];
    const int key_index = indices[left];
    while (low < high) {
        while (low < high && input[high] <= key) {
            high--;
        }
        input[low] = input[high];
        indices[low] = indices[high];
        while (low < high && input[low] >= key) {
            low++;
        }
        input[high] = input[low];
        indices[high] = indices[low];
    }
    input[low] = key;
    indices[low] = key_index;
    quick_sort_indice_inverse(input, left, low - 1, indices);
    quick_sort_indice_inverse(input, low + 1, right, indices);
}

void nms_by_class(int valid_count,
                  const std::vector<float>& boxes,
                  const std::vector<int>& class_ids,
                  std::vector<int>& order,
                  int filter_class,
                  float threshold) {
    for (int i = 0; i < valid_count; ++i) {
        const int n = order[i];
        if (n == -1 || class_ids[n] != filter_class) {
            continue;
        }
        for (int j = i + 1; j < valid_count; ++j) {
            const int m = order[j];
            if (m == -1 || class_ids[m] != filter_class) {
                continue;
            }
            if (calculate_iou_xywh(boxes, n, m) > threshold) {
                order[j] = -1;
            }
        }
    }
}

void compute_dfl(const float* tensor, int dfl_len, float* box) {
    for (int b = 0; b < 4; ++b) {
        float exp_sum = 0.0f;
        float acc_sum = 0.0f;
        float tmp[64];
        if (dfl_len > 64) {
            box[b] = 0.0f;
            continue;
        }
        for (int i = 0; i < dfl_len; ++i) {
            tmp[i] = expf(tensor[i + b * dfl_len]);
            exp_sum += tmp[i];
        }
        if (exp_sum <= 1e-6f) {
            box[b] = 0.0f;
            continue;
        }
        for (int i = 0; i < dfl_len; ++i) {
            acc_sum += (tmp[i] / exp_sum) * i;
        }
        box[b] = acc_sum;
    }
}

int get_nchw_h(const rknn_tensor_attr& attr) {
    return attr.dims[2];
}

int get_nchw_w(const rknn_tensor_attr& attr) {
    return attr.dims[3];
}

int get_nchw_c(const rknn_tensor_attr& attr) {
    return attr.dims[1];
}

void fill_box_result(const std::vector<float>& boxes,
                     const std::vector<float>& scores,
                     const std::vector<int>& class_ids,
                     const std::vector<int>& order,
                     const letterbox_t* letter_box,
                     int model_w,
                     int model_h,
                     object_detect_result_list* od_results) {
    int count = 0;
    for (size_t i = 0; i < order.size() && count < OBJ_NUMB_MAX_SIZE; ++i) {
        const int idx = order[i];
        if (idx < 0) {
            continue;
        }
        float x1 = boxes[idx * 4 + 0] - letter_box->x_pad;
        float y1 = boxes[idx * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + boxes[idx * 4 + 2];
        float y2 = y1 + boxes[idx * 4 + 3];

        object_detect_result* out = &od_results->results[count++];
        out->box.left = clamp_int(x1 / letter_box->scale, 0, model_w);
        out->box.top = clamp_int(y1 / letter_box->scale, 0, model_h);
        out->box.right = clamp_int(x2 / letter_box->scale, 0, model_w);
        out->box.bottom = clamp_int(y2 / letter_box->scale, 0, model_h);
        out->prop = scores[idx];
        out->cls_id = class_ids[idx];
        out->tracker_id = -1;
        out->keypoint_count = 0;
    }
    od_results->count = count;
}

int decode_detect_branch(const rknn_output* outputs,
                         rknn_app_context_t* app_ctx,
                         int box_idx,
                         int score_idx,
                         int score_sum_idx,
                         float threshold,
                         std::vector<float>& boxes,
                         std::vector<float>& scores,
                         std::vector<int>& class_ids) {
    const float* box_tensor = static_cast<const float*>(outputs[box_idx].buf);
    const float* score_tensor = static_cast<const float*>(outputs[score_idx].buf);
    (void)score_sum_idx;
    const int grid_h = get_nchw_h(app_ctx->output_attrs[box_idx]);
    const int grid_w = get_nchw_w(app_ctx->output_attrs[box_idx]);
    const int dfl_len = get_nchw_c(app_ctx->output_attrs[box_idx]) / 4;
    const int num_classes = get_nchw_c(app_ctx->output_attrs[score_idx]);
    const int grid_len = grid_h * grid_w;
    const int stride = app_ctx->model_height / grid_h;
    int valid = 0;

    for (int y = 0; y < grid_h; ++y) {
        for (int x = 0; x < grid_w; ++x) {
            const int offset = y * grid_w + x;

            int best_class = -1;
            float best_score = 0.0f;
            for (int c = 0; c < num_classes; ++c) {
                float score = score_tensor[c * grid_len + offset];
                if (score > threshold && score > best_score) {
                    best_score = score;
                    best_class = c;
                }
            }
            if (best_class < 0) {
                continue;
            }

            float raw_box[256];
            if (dfl_len * 4 > 256) {
                continue;
            }
            for (int k = 0; k < dfl_len * 4; ++k) {
                raw_box[k] = box_tensor[k * grid_len + offset];
            }

            float box[4];
            compute_dfl(raw_box, dfl_len, box);
            const float x1 = (-box[0] + x + 0.5f) * stride;
            const float y1 = (-box[1] + y + 0.5f) * stride;
            const float x2 = (box[2] + x + 0.5f) * stride;
            const float y2 = (box[3] + y + 0.5f) * stride;

            boxes.push_back(x1);
            boxes.push_back(y1);
            boxes.push_back(x2 - x1);
            boxes.push_back(y2 - y1);
            scores.push_back(best_score);
            class_ids.push_back(best_class);
            valid++;
        }
    }
    return valid;
}

void sort_and_nms(const std::vector<float>& boxes,
                  const std::vector<float>& scores,
                  const std::vector<int>& class_ids,
                  float nms_threshold,
                  std::vector<int>* order) {
    std::vector<float> sortable_scores = scores;
    order->clear();
    order->reserve(sortable_scores.size());
    for (size_t i = 0; i < sortable_scores.size(); ++i) {
        order->push_back(static_cast<int>(i));
    }
    if (sortable_scores.empty()) {
        return;
    }
    quick_sort_indice_inverse(sortable_scores, 0, static_cast<int>(sortable_scores.size()) - 1, *order);
    std::set<int> unique_classes(class_ids.begin(), class_ids.end());
    for (int class_id : unique_classes) {
        nms_by_class(static_cast<int>(sortable_scores.size()), boxes, class_ids, *order, class_id, nms_threshold);
    }
}

int proto_index(const rknn_tensor_attr& attr, int c, int y, int x) {
    return c * attr.dims[2] * attr.dims[3] + y * attr.dims[3] + x;
}

float sample_proto(const float* proto,
                   const rknn_tensor_attr& proto_attr,
                   int channel,
                   int model_x,
                   int model_y,
                   int model_w,
                   int model_h) {
    const int proto_h = proto_attr.dims[2];
    const int proto_w = proto_attr.dims[3];
    const int px = clamp_int((static_cast<float>(model_x) / model_w) * proto_w, 0, proto_w - 1);
    const int py = clamp_int((static_cast<float>(model_y) / model_h) * proto_h, 0, proto_h - 1);
    return proto[proto_index(proto_attr, channel, py, px)];
}

void paint_seg_mask(const std::vector<float>& kept_boxes,
                    const std::vector<int>& kept_classes,
                    const std::vector<std::vector<float>>& kept_coeffs,
                    const float* proto,
                    const rknn_tensor_attr& proto_attr,
                    int model_w,
                    int model_h,
                    object_detect_result_list* od_results) {
    const int seg_channels = proto_attr.dims[1];
    if (seg_channels <= 0) {
        return;
    }

    od_results->seg_valid = 1;
    for (size_t i = 0; i < kept_classes.size(); ++i) {
        const int x1 = clamp_int(kept_boxes[i * 4 + 0], 0, model_w - 1);
        const int y1 = clamp_int(kept_boxes[i * 4 + 1], 0, model_h - 1);
        const int x2 = clamp_int(kept_boxes[i * 4 + 2], 0, model_w);
        const int y2 = clamp_int(kept_boxes[i * 4 + 3], 0, model_h);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        for (int y = y1; y < y2; ++y) {
            for (int x = x1; x < x2; ++x) {
                float acc = 0.0f;
                for (int c = 0; c < seg_channels; ++c) {
                    acc += kept_coeffs[i][c] * sample_proto(proto, proto_attr, c, x, y, model_w, model_h);
                }
                if (sigmoid(acc) < 0.5f) {
                    continue;
                }
                const int idx = y * AI_SEG_MASK_WIDTH + x;
                if (idx >= 0 && idx < AI_SEG_MASK_SIZE && od_results->seg_mask[idx] == 0) {
                    od_results->seg_mask[idx] = static_cast<uint8_t>(kept_classes[i] + 1);
                }
            }
        }
    }
}

int keypoint_positions(const rknn_tensor_attr& attr) {
    if (attr.n_dims >= 4 && attr.dims[1] == AI_POSE_KEYPOINT_NUM && attr.dims[2] == 3) {
        return attr.dims[3];
    }
    if (attr.n_dims >= 3 && attr.dims[attr.n_dims - 2] == 3) {
        return attr.dims[attr.n_dims - 1];
    }
    if (attr.dims[1] <= 0) {
        return 0;
    }
    return attr.n_elems / attr.dims[1];
}

int keypoint_count_from_attr(const rknn_tensor_attr& attr) {
    if (attr.n_dims >= 4 && attr.dims[2] == 3) {
        return attr.dims[1];
    }
    if (attr.n_dims >= 3 && attr.dims[attr.n_dims - 2] == 3) {
        if (attr.n_dims >= 4) {
            return attr.dims[attr.n_dims - 3];
        }
        return attr.dims[0];
    }
    return AI_POSE_KEYPOINT_NUM;
}

}  // namespace

int init_post_process() {
    return 0;
}

void deinit_post_process() {
}

char* coco_cls_to_name(int cls_id) {
    static char labels[OBJ_CLASS_NUM][16];
    static bool initialized = false;
    if (!initialized) {
        for (int i = 0; i < OBJ_CLASS_NUM; ++i) {
            snprintf(labels[i], sizeof(labels[i]), "cls_%d", i);
        }
        initialized = true;
    }
    if (cls_id < 0 || cls_id >= OBJ_CLASS_NUM) {
        return const_cast<char*>("null");
    }
    return labels[cls_id];
}

int post_process(rknn_app_context_t* app_ctx,
                 void* outputs,
                 letterbox_t* letter_box,
                 float conf_threshold,
                 float nms_threshold,
                 object_detect_result_list* od_results) {
    reset_results(od_results);
    rknn_output* out = static_cast<rknn_output*>(outputs);
    const int output_per_branch = app_ctx->io_num.n_output / 3;
    std::vector<float> boxes;
    std::vector<float> scores;
    std::vector<int> class_ids;
    int valid_count = 0;

    for (int i = 0; i < 3; ++i) {
        const int box_idx = i * output_per_branch;
        const int score_idx = box_idx + 1;
        valid_count += decode_detect_branch(out, app_ctx, box_idx, score_idx, -1, conf_threshold, boxes, scores, class_ids);
    }

    if (valid_count <= 0) {
        return 0;
    }

    std::vector<int> order;
    sort_and_nms(boxes, scores, class_ids, nms_threshold, &order);
    fill_box_result(boxes, scores, class_ids, order, letter_box, app_ctx->model_width, app_ctx->model_height, od_results);
    return 0;
}

int post_process_seg(rknn_app_context_t* app_ctx,
                     rknn_output* outputs,
                     letterbox_t* letter_box,
                     float conf_threshold,
                     float nms_threshold,
                     object_detect_result_list* od_results) {
    reset_results(od_results);
    if (app_ctx->io_num.n_output < 13) {
        return -1;
    }

    const int proto_idx = app_ctx->io_num.n_output - 1;
    const int output_per_branch = (app_ctx->io_num.n_output - 1) / 3;
    static int debug_frames_remaining = 3;
    std::vector<float> boxes_xywh;
    std::vector<float> scores;
    std::vector<int> class_ids;
    std::vector<std::vector<float>> coeffs;

    for (int branch = 0; branch < 3; ++branch) {
        const int box_idx = branch * output_per_branch;
        const int score_idx = box_idx + 1;
        const int aux_score_idx = score_idx + 1;
        const int seg_idx = box_idx + output_per_branch - 1;
        const float* box_tensor = static_cast<const float*>(outputs[box_idx].buf);
        const float* score_tensor = static_cast<const float*>(outputs[score_idx].buf);
        const float* aux_score_tensor =
            (aux_score_idx < seg_idx) ? static_cast<const float*>(outputs[aux_score_idx].buf) : nullptr;
        const float* seg_tensor = static_cast<const float*>(outputs[seg_idx].buf);
        const int grid_h = get_nchw_h(app_ctx->output_attrs[box_idx]);
        const int grid_w = get_nchw_w(app_ctx->output_attrs[box_idx]);
        const int grid_len = grid_h * grid_w;
        const int dfl_len = get_nchw_c(app_ctx->output_attrs[box_idx]) / 4;
        const int num_classes = get_nchw_c(app_ctx->output_attrs[score_idx]);
        const int seg_channels = get_nchw_c(app_ctx->output_attrs[seg_idx]);
        const int stride = app_ctx->model_height / grid_h;

        if (debug_frames_remaining > 0) {
            debug_log_tensor_stats("seg-score", score_idx, score_tensor, num_classes, grid_len);
            if (aux_score_tensor != nullptr) {
                debug_log_tensor_stats("seg-aux", aux_score_idx,
                                       aux_score_tensor,
                                       get_nchw_c(app_ctx->output_attrs[aux_score_idx]),
                                       grid_len);
            }
        }

        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const int offset = y * grid_w + x;

                int best_class = -1;
                float best_score = 0.0f;
                for (int c = 0; c < num_classes; ++c) {
                    float score = score_tensor[c * grid_len + offset];
                    if (score > conf_threshold && score > best_score) {
                        best_score = score;
                        best_class = c;
                    }
                }
                if (best_class < 0) {
                    continue;
                }

                float raw_box[256];
                if (dfl_len * 4 > 256) {
                    continue;
                }
                for (int k = 0; k < dfl_len * 4; ++k) {
                    raw_box[k] = box_tensor[k * grid_len + offset];
                }
                float box[4];
                compute_dfl(raw_box, dfl_len, box);
                const float x1 = (-box[0] + x + 0.5f) * stride;
                const float y1 = (-box[1] + y + 0.5f) * stride;
                const float x2 = (box[2] + x + 0.5f) * stride;
                const float y2 = (box[3] + y + 0.5f) * stride;

                boxes_xywh.push_back(x1);
                boxes_xywh.push_back(y1);
                boxes_xywh.push_back(x2 - x1);
                boxes_xywh.push_back(y2 - y1);
                scores.push_back(best_score);
                class_ids.push_back(best_class);

                std::vector<float> coeff(seg_channels);
                for (int c = 0; c < seg_channels; ++c) {
                    coeff[c] = seg_tensor[c * grid_len + offset];
                }
                coeffs.push_back(coeff);
            }
        }
    }

    if (scores.empty()) {
        if (debug_frames_remaining > 0) {
            fprintf(stdout, "[postprocess-debug] seg selected_boxes=0 conf_threshold=%.3f\n", conf_threshold);
            debug_frames_remaining -= 1;
        }
        return 0;
    }

    std::vector<int> order;
    sort_and_nms(boxes_xywh, scores, class_ids, nms_threshold, &order);

    std::vector<float> kept_boxes_xyxy;
    std::vector<int> kept_classes;
    std::vector<std::vector<float>> kept_coeffs;
    int count = 0;
    for (size_t i = 0; i < order.size() && count < OBJ_NUMB_MAX_SIZE; ++i) {
        const int idx = order[i];
        if (idx < 0) {
            continue;
        }

        float x1 = boxes_xywh[idx * 4 + 0] - letter_box->x_pad;
        float y1 = boxes_xywh[idx * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + boxes_xywh[idx * 4 + 2];
        float y2 = y1 + boxes_xywh[idx * 4 + 3];

        object_detect_result* out = &od_results->results[count++];
        out->box.left = clamp_int(x1 / letter_box->scale, 0, app_ctx->model_width);
        out->box.top = clamp_int(y1 / letter_box->scale, 0, app_ctx->model_height);
        out->box.right = clamp_int(x2 / letter_box->scale, 0, app_ctx->model_width);
        out->box.bottom = clamp_int(y2 / letter_box->scale, 0, app_ctx->model_height);
        out->prop = scores[idx];
        out->cls_id = class_ids[idx];
        out->tracker_id = -1;
        out->keypoint_count = 0;

        kept_boxes_xyxy.push_back(clamp_float(x1 / letter_box->scale, 0.0f, static_cast<float>(app_ctx->model_width)));
        kept_boxes_xyxy.push_back(clamp_float(y1 / letter_box->scale, 0.0f, static_cast<float>(app_ctx->model_height)));
        kept_boxes_xyxy.push_back(clamp_float(x2 / letter_box->scale, 0.0f, static_cast<float>(app_ctx->model_width)));
        kept_boxes_xyxy.push_back(clamp_float(y2 / letter_box->scale, 0.0f, static_cast<float>(app_ctx->model_height)));
        kept_classes.push_back(class_ids[idx]);
        kept_coeffs.push_back(coeffs[idx]);
    }
    od_results->count = count;

    paint_seg_mask(kept_boxes_xyxy,
                   kept_classes,
                   kept_coeffs,
                   static_cast<const float*>(outputs[proto_idx].buf),
                   app_ctx->output_attrs[proto_idx],
                   app_ctx->model_width,
                   app_ctx->model_height,
                   od_results);
    if (debug_frames_remaining > 0) {
        fprintf(stdout,
                "[postprocess-debug] seg selected_boxes=%zu kept_boxes=%d top_score=%.6f conf_threshold=%.3f\n",
                scores.size(),
                od_results->count,
                scores.empty() ? 0.0f : scores.front(),
                conf_threshold);
        debug_frames_remaining -= 1;
    }
    return 0;
}

int post_process_pose(rknn_app_context_t* app_ctx,
                      rknn_output* outputs,
                      letterbox_t* letter_box,
                      float conf_threshold,
                      float nms_threshold,
                      object_detect_result_list* od_results) {
    reset_results(od_results);
    if (app_ctx->io_num.n_output < 4) {
        return -1;
    }

    const float* keypoint_tensor = static_cast<const float*>(outputs[3].buf);
    const int model_keypoint_count = std::min(keypoint_count_from_attr(app_ctx->output_attrs[3]), AI_POSE_KEYPOINT_NUM);
    const int position_count = keypoint_positions(app_ctx->output_attrs[3]);
    std::vector<float> boxes_xywh;
    std::vector<float> scores;
    std::vector<int> class_ids;
    std::vector<int> anchor_indices;
    int anchor_base = 0;

    for (int branch = 0; branch < 3; ++branch) {
        const float* tensor = static_cast<const float*>(outputs[branch].buf);
        const int grid_h = get_nchw_h(app_ctx->output_attrs[branch]);
        const int grid_w = get_nchw_w(app_ctx->output_attrs[branch]);
        const int stride = app_ctx->model_height / grid_h;
        const int grid_len = grid_h * grid_w;
        const int loc_len = 64;

        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const int offset = y * grid_w + x;
                const float logit = tensor[loc_len * grid_len + offset];
                const float score = sigmoid(logit);
                if (score < conf_threshold) {
                    continue;
                }

                float loc[64];
                for (int i = 0; i < loc_len; ++i) {
                    loc[i] = tensor[i * grid_len + offset];
                }
                for (int i = 0; i < loc_len / 16; ++i) {
                    float exp_sum = 0.0f;
                    for (int k = 0; k < 16; ++k) {
                        loc[i * 16 + k] = expf(loc[i * 16 + k]);
                        exp_sum += loc[i * 16 + k];
                    }
                    if (exp_sum <= 1e-6f) {
                        continue;
                    }
                    for (int k = 0; k < 16; ++k) {
                        loc[i * 16 + k] /= exp_sum;
                    }
                }

                float d[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                for (int k = 0; k < 16; ++k) {
                    d[0] += loc[k] * k;
                    d[1] += loc[16 + k] * k;
                    d[2] += loc[32 + k] * k;
                    d[3] += loc[48 + k] * k;
                }

                const float left = (x + 0.5f - d[0]) * stride;
                const float top = (y + 0.5f - d[1]) * stride;
                const float right = (x + 0.5f + d[2]) * stride;
                const float bottom = (y + 0.5f + d[3]) * stride;

                boxes_xywh.push_back(left);
                boxes_xywh.push_back(top);
                boxes_xywh.push_back(right - left);
                boxes_xywh.push_back(bottom - top);
                scores.push_back(score);
                class_ids.push_back(0);
                anchor_indices.push_back(anchor_base + offset);
            }
        }
        anchor_base += grid_len;
    }

    if (scores.empty()) {
        return 0;
    }

    std::vector<int> order;
    sort_and_nms(boxes_xywh, scores, class_ids, nms_threshold, &order);

    int count = 0;
    for (size_t i = 0; i < order.size() && count < OBJ_NUMB_MAX_SIZE; ++i) {
        const int idx = order[i];
        if (idx < 0) {
            continue;
        }

        float x1 = boxes_xywh[idx * 4 + 0] - letter_box->x_pad;
        float y1 = boxes_xywh[idx * 4 + 1] - letter_box->y_pad;
        float x2 = x1 + boxes_xywh[idx * 4 + 2];
        float y2 = y1 + boxes_xywh[idx * 4 + 3];

        object_detect_result* out = &od_results->results[count++];
        out->box.left = clamp_int(x1 / letter_box->scale, 0, app_ctx->model_width);
        out->box.top = clamp_int(y1 / letter_box->scale, 0, app_ctx->model_height);
        out->box.right = clamp_int(x2 / letter_box->scale, 0, app_ctx->model_width);
        out->box.bottom = clamp_int(y2 / letter_box->scale, 0, app_ctx->model_height);
        out->prop = scores[idx];
        out->cls_id = 0;
        out->tracker_id = -1;
        out->keypoint_count = model_keypoint_count;

        const int anchor_idx = anchor_indices[idx];
        if (anchor_idx >= 0 && anchor_idx < position_count) {
            for (int kp = 0; kp < model_keypoint_count; ++kp) {
                out->keypoints[kp].x = clamp_float((keypoint_tensor[kp * 3 * position_count + anchor_idx] - letter_box->x_pad) / letter_box->scale,
                                                   0.0f,
                                                   static_cast<float>(app_ctx->model_width));
                out->keypoints[kp].y = clamp_float((keypoint_tensor[kp * 3 * position_count + position_count + anchor_idx] - letter_box->y_pad) / letter_box->scale,
                                                   0.0f,
                                                   static_cast<float>(app_ctx->model_height));
                out->keypoints[kp].score = keypoint_tensor[kp * 3 * position_count + position_count * 2 + anchor_idx];
            }
        } else {
            out->keypoint_count = 0;
        }
    }
    od_results->count = count;
    return 0;
}
