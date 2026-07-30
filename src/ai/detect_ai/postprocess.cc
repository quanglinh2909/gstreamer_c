#include "postprocess.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <cstddef>
#include <set>
#include <vector>

#if defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define AI_HAVE_NEON 1
#endif

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

// ---- int8 (quantized) decode ------------------------------------------------
// Đường nóng CPU đo được: quét float 8400 anchor × num_classes + dequant TOÀN
// BỘ output (want_float=1 trong librknnrt) tốn ~13ms CPU mỗi inference. Đường
// int8 này (theo đúng mẫu rknn_model_zoo của Rockchip):
//   * so sánh ngưỡng NGAY trên int8 (ngưỡng được lượng-tử-hoá 1 lần/branch);
//   * chỉ dequant sang float cho số ít anchor vượt ngưỡng (box DFL + score).
//
// ĐO LẠI 29/07/2026 — nhánh score-sum (1×h×w) KHÔNG loại được như kỳ vọng: với
// model 80 lớp, nền cũng cộng dồn tới ~0,25 nên **95,4% anchor lọt qua**
// (2.763.670/2.898.000 mỗi 5 giây). Mẹo đó chỉ ăn với model ít lớp. Cửa loại
// thật sự nằm ở max_score_per_anchor_i8 bên dưới.

inline float deqnt_affine_to_f32(int8_t q, int zp, float scale) {
    return (static_cast<float>(q) - static_cast<float>(zp)) * scale;
}

inline int8_t qnt_f32_to_affine(float f, int zp, float scale) {
    float dst = f / scale + static_cast<float>(zp);
    if (dst > 127.0f) dst = 127.0f;
    if (dst < -128.0f) dst = -128.0f;
    return static_cast<int8_t>(dst);
}

// Điểm cao nhất trong các lớp CHO MỖI ANCHOR, quét theo LỚP.
//
// Tensor score là NCHW: [1, num_classes, grid_h, grid_w] — tức mỗi lớp là một
// mặt phẳng LIỀN MẠCH grid_len byte. Quét theo anchor (`col[c * grid_len]`)
// nhảy 6400 byte mỗi bước nên mỗi lần đọc kéo về một dòng cache rồi vứt: đo
// được 2,76 triệu anchor × 80 lớp = 221 TRIỆU lần đọc trượt cache mỗi 5s,
// chiếm 27-45% CPU toàn hệ. Quét theo lớp đọc TUẦN TỰ hết tensor đúng một lần
// (512KB) và cộng dồn max vào `best` (6,4KB — nằm gọn trong L1).
//
// `best` phải được nạp sẵn ngưỡng int8 trước khi gọi; sau khi chạy, anchor nào
// có best[o] > ngưỡng là anchor đáng xét tiếp (đúng bằng điều kiện
// `best_class >= 0` của vòng lặp cũ).
void max_score_per_anchor_i8(const int8_t* score_tensor, int num_classes,
                             int grid_len, int8_t* best) {
    for (int c = 0; c < num_classes; ++c) {
        const int8_t* row = score_tensor + static_cast<size_t>(c) * grid_len;
        int o = 0;
#ifdef AI_HAVE_NEON
        for (; o + 16 <= grid_len; o += 16) {
            vst1q_s8(best + o, vmaxq_s8(vld1q_s8(row + o), vld1q_s8(best + o)));
        }
#endif
        for (; o < grid_len; ++o) {
            if (row[o] > best[o]) best[o] = row[o];
        }
    }
}

// Đọc một phần tử tensor int8 theo layout.
//   c2 == 0 : NCHW      -> t[c*grid_len + off]
//   c2 != 0 : NC1HWC2   -> t[((c/c2)*grid_len + off)*c2 + c%c2]
// Công thức NC1HWC2 đã kiểm chứng bằng cách de-swizzle rồi so từng byte với
// đường NCHW của rknn_outputs_get: khớp 0 byte sai trên cả 9 tensor.
inline int8_t tensor_at_i8(const int8_t* t, int c, int off, int grid_len, int c2) {
    return c2 ? t[(static_cast<size_t>(c / c2) * grid_len + off) * c2 + (c % c2)]
              : t[static_cast<size_t>(c) * grid_len + off];
}

// Bản NC1HWC2 của max_score_per_anchor_i8. Ở layout này, c2 kênh của CÙNG một
// anchor nằm LIỀN NHAU, nên max theo anchor là một lệnh vmaxvq_s8 trên 16 byte
// liền mạch; đi theo thứ tự (khối, anchor) thì toàn bộ tensor vẫn được đọc
// tuần tự. Khối cuối có thể thừa lane đệm (80 lớp xếp vào 96 chỗ) — chỉ quét
// đúng số lane thật.
void max_score_per_anchor_nc1hwc2(const int8_t* t, int num_classes, int grid_len,
                                  int c2, int8_t* best) {
    for (int base = 0; base < num_classes; base += c2) {
        const int lanes = std::min(c2, num_classes - base);
        const int8_t* blk = t + static_cast<size_t>(base / c2) * grid_len * c2;
        for (int o = 0; o < grid_len; ++o) {
            const int8_t* v = blk + static_cast<size_t>(o) * c2;
            int8_t m;
#ifdef AI_HAVE_NEON
            if (lanes == 16) {
                m = vmaxvq_s8(vld1q_s8(v));
            } else
#endif
            {
                m = v[0];
                for (int k = 1; k < lanes; ++k) {
                    if (v[k] > m) m = v[k];
                }
            }
            if (m > best[o]) best[o] = m;
        }
    }
}

int decode_detect_branch_i8(const rknn_output* outputs,
                            rknn_app_context_t* app_ctx,
                            int box_idx,
                            int score_idx,
                            int score_sum_idx,
                            float threshold,
                            std::vector<float>& boxes,
                            std::vector<float>& scores,
                            std::vector<int>& class_ids) {
    const int8_t* box_tensor = static_cast<const int8_t*>(outputs[box_idx].buf);
    const int8_t* score_tensor = static_cast<const int8_t*>(outputs[score_idx].buf);
    const int8_t* sum_tensor =
        score_sum_idx >= 0 ? static_cast<const int8_t*>(outputs[score_sum_idx].buf)
                           : nullptr;

    const rknn_tensor_attr& box_attr = app_ctx->output_attrs[box_idx];
    const rknn_tensor_attr& score_attr = app_ctx->output_attrs[score_idx];
    // Layout thật của bộ đệm: NULL = NCHW (đường rknn_outputs_get cũ),
    // khác NULL = NC1HWC2 (zero-copy, xem init_yolov8_model).
    const rknn_tensor_attr* nat = app_ctx->native_output_attrs;
    const int box_c2 = nat ? static_cast<int>(nat[box_idx].dims[4]) : 0;
    const int score_c2 = nat ? static_cast<int>(nat[score_idx].dims[4]) : 0;
    const int sum_c2 =
        (nat && score_sum_idx >= 0) ? static_cast<int>(nat[score_sum_idx].dims[4]) : 0;
    const int box_zp = box_attr.zp;
    const float box_scale = box_attr.scale;
    const int score_zp = score_attr.zp;
    const float score_scale = score_attr.scale;
    const int8_t score_thres_i8 = qnt_f32_to_affine(threshold, score_zp, score_scale);
    int8_t sum_thres_i8 = 0;
    if (sum_tensor) {
        const rknn_tensor_attr& sum_attr = app_ctx->output_attrs[score_sum_idx];
        sum_thres_i8 = qnt_f32_to_affine(threshold, sum_attr.zp, sum_attr.scale);
    }

    const int grid_h = get_nchw_h(box_attr);
    const int grid_w = get_nchw_w(box_attr);
    const int dfl_len = get_nchw_c(box_attr) / 4;
    const int num_classes = get_nchw_c(score_attr);
    const int grid_len = grid_h * grid_w;
    const int stride = app_ctx->model_height / grid_h;
    int valid = 0;

    // Lượt 1: điểm cao nhất mỗi anchor, quét TUẦN TỰ theo lớp. Thay cho việc
    // để mỗi anchor tự quét 80 lớp với sải bước grid_len (mỗi lần đọc là một
    // lần trượt cache) — xem max_score_per_anchor_i8.
    thread_local std::vector<int8_t> best_buf;
    if (static_cast<int>(best_buf.size()) < grid_len) best_buf.resize(grid_len);
    int8_t* best = best_buf.data();
    std::fill(best, best + grid_len, score_thres_i8);
    if (score_c2) {
        max_score_per_anchor_nc1hwc2(score_tensor, num_classes, grid_len,
                                     score_c2, best);
    } else {
        max_score_per_anchor_i8(score_tensor, num_classes, grid_len, best);
    }

    for (int offset = 0; offset < grid_len; ++offset) {
        // Loại nhanh: tổng score mọi lớp còn dưới ngưỡng thì không lớp nào
        // vượt được — bỏ anchor bằng một phép so sánh int8. (Với model 80 lớp
        // cửa này gần như không loại được gì — nền cũng cộng dồn tới ngưỡng —
        // nhưng nó rẻ nên vẫn giữ; cửa THẬT là best[] ngay bên dưới.)
        if (sum_tensor &&
            tensor_at_i8(sum_tensor, 0, offset, grid_len, sum_c2) < sum_thres_i8) {
            continue;
        }

        // Cửa thật: không lớp nào vượt ngưỡng thì bỏ, không cần biết lớp nào.
        if (best[offset] <= score_thres_i8) {
            continue;
        }

        // Lượt 2 chỉ chạy cho ~0,6% anchor sống sót nên vòng tìm ĐÚNG LỚP giữ
        // nguyên như cũ — cùng quy tắc chọn lớp (hoà thì lấy lớp nhỏ nhất),
        // tức kết quả phát hiện không đổi một chút nào.
        int best_class = -1;
        int8_t best_score = score_thres_i8;
        for (int c = 0; c < num_classes; ++c) {
            const int8_t s =
                tensor_at_i8(score_tensor, c, offset, grid_len, score_c2);
            if (s > best_score) {
                best_score = s;
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
            raw_box[k] = deqnt_affine_to_f32(
                tensor_at_i8(box_tensor, k, offset, grid_len, box_c2), box_zp,
                box_scale);
        }

        const int x = offset % grid_w;
        const int y = offset / grid_w;
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
        scores.push_back(deqnt_affine_to_f32(best_score, score_zp, score_scale));
        class_ids.push_back(best_class);
        valid++;
    }
    return valid;
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
                    const void* proto_buf,
                    bool proto_i8,
                    const rknn_tensor_attr& proto_attr,
                    int model_w,
                    int model_h,
                    object_detect_result_list* od_results) {
    const float* proto = static_cast<const float*>(proto_buf);
    const int8_t* proto_i8_buf = static_cast<const int8_t*>(proto_buf);
    const int proto_zp = proto_attr.zp;
    const float proto_scale = proto_attr.scale;
    const int seg_channels = proto_attr.dims[1];
    if (seg_channels <= 0) {
        return;
    }

    const int proto_h = proto_attr.dims[2];
    const int proto_w = proto_attr.dims[3];
    if (proto_h <= 0 || proto_w <= 0) {
        return;
    }
    const int proto_plane = proto_h * proto_w;

    od_results->seg_valid = 1;
    for (size_t i = 0; i < kept_classes.size(); ++i) {
        const int x1 = clamp_int(kept_boxes[i * 4 + 0], 0, model_w - 1);
        const int y1 = clamp_int(kept_boxes[i * 4 + 1], 0, model_h - 1);
        const int x2 = std::min(clamp_int(kept_boxes[i * 4 + 2], 0, model_w),
                                AI_SEG_MASK_WIDTH);
        const int y2 = std::min(clamp_int(kept_boxes[i * 4 + 3], 0, model_h),
                                AI_SEG_MASK_HEIGHT);
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }
        const float* coeff = kept_coeffs[i].data();
        const uint8_t want = static_cast<uint8_t>(kept_classes[i] + 1);

        // Duyệt theo Ô PROTO chứ không theo pixel model.
        //
        // sample_proto ánh xạ NEAREST: px = (x/model_w)*proto_w. Với 640→160 thì
        // mỗi ô proto trải đúng ra một khối 4×4 pixel model có CÙNG giá trị —
        // nghĩa là bản cũ tính lại y hệt một tích vô hướng 32 kênh 16 lần cho
        // mỗi ô. Một bbox 300×400 = 120.000 pixel × 32 kênh = 3,8 TRIỆU phép
        // nhân-cộng cho MỘT vật thể. Gom theo ô proto giảm đúng 16 lần.
        //
        // Vòng while dò biên ô nên không giả định tỉ lệ chia hết: nếu model/proto
        // không phải bội số nguyên thì khối chỉ đơn giản là 1×1 và kết quả vẫn
        // trùng với bản cũ.
        int y = y1;
        while (y < y2) {
            const int py = clamp_int(
                (static_cast<float>(y) / model_h) * proto_h, 0, proto_h - 1);
            int yEnd = y + 1;
            while (yEnd < y2 &&
                   clamp_int((static_cast<float>(yEnd) / model_h) * proto_h, 0,
                             proto_h - 1) == py) {
                ++yEnd;
            }

            int x = x1;
            while (x < x2) {
                const int px = clamp_int(
                    (static_cast<float>(x) / model_w) * proto_w, 0, proto_w - 1);
                int xEnd = x + 1;
                while (xEnd < x2 &&
                       clamp_int((static_cast<float>(xEnd) / model_w) * proto_w,
                                 0, proto_w - 1) == px) {
                    ++xEnd;
                }

                float acc = 0.0f;
                if (proto_i8) {
                    // Dequant proto NGAY TRONG vòng tích vô hướng: chỉ những ô
                    // proto nằm trong bbox mới bị chạm, thay vì bắt librknnrt
                    // dequant cả 819.200 giá trị của tensor proto mỗi lần.
                    const int8_t* cell = proto_i8_buf + py * proto_w + px;
                    for (int c = 0; c < seg_channels; ++c) {
                        acc += coeff[c] * deqnt_affine_to_f32(cell[c * proto_plane],
                                                              proto_zp, proto_scale);
                    }
                } else {
                    const float* cell = proto + py * proto_w + px;
                    for (int c = 0; c < seg_channels; ++c) {
                        acc += coeff[c] * cell[c * proto_plane];
                    }
                }
                // sigmoid đơn điệu tăng và sigmoid(0) = 0,5, nên
                // "sigmoid(acc) < 0,5" đúng bằng "acc < 0" — bỏ được MỘT expf
                // cho mỗi pixel (trước đây là 120.000 expf mỗi vật thể).
                if (acc >= 0.0f) {
                    for (int yy = y; yy < yEnd; ++yy) {
                        uint8_t* row = od_results->seg_mask +
                                       static_cast<size_t>(yy) * AI_SEG_MASK_WIDTH;
                        for (int xx = x; xx < xEnd; ++xx) {
                            if (row[xx] == 0) row[xx] = want;
                        }
                    }
                }
                x = xEnd;
            }
            y = yEnd;
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
        // Nhánh score-sum (1×h×w, chỉ có ở export 9-output) cho phép loại
        // anchor bằng một phép so sánh duy nhất.
        const int sum_idx = output_per_branch >= 3 ? box_idx + 2 : -1;
        if (app_ctx->is_quant && !out[box_idx].want_float) {
            // Output còn nguyên int8 (caller đặt want_float=0): decode trên
            // int8, không tốn dequant toàn bộ tensor.
            valid_count += decode_detect_branch_i8(out, app_ctx, box_idx, score_idx, sum_idx, conf_threshold, boxes, scores, class_ids);
        } else {
            valid_count += decode_detect_branch(out, app_ctx, box_idx, score_idx, -1, conf_threshold, boxes, scores, class_ids);
        }
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
    // Đường INT8 (xem post_process_pose cho cùng lý do): seg có 13 tensor đầu
    // ra, tổng 2.306.000 giá trị — want_float=1 bắt librknnrt dequant sạch cả
    // ngần ấy mỗi lần suy luận, đo được 6-11 ms CPU. Giữ int8, chỉ dequant cho
    // ít anchor sống sót và cho những ô proto thật sự nằm trong bbox.
    const bool use_i8 = app_ctx->is_quant && !outputs[0].want_float;
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
        const int8_t* box_i8 = static_cast<const int8_t*>(outputs[box_idx].buf);
        const int8_t* score_i8 = static_cast<const int8_t*>(outputs[score_idx].buf);
        const int8_t* seg_i8 = static_cast<const int8_t*>(outputs[seg_idx].buf);
        const int box_zp = app_ctx->output_attrs[box_idx].zp;
        const float box_scale = app_ctx->output_attrs[box_idx].scale;
        const int score_zp = app_ctx->output_attrs[score_idx].zp;
        const float score_scale = app_ctx->output_attrs[score_idx].scale;
        const int coef_zp = app_ctx->output_attrs[seg_idx].zp;
        const float coef_scale = app_ctx->output_attrs[seg_idx].scale;
        const int grid_h = get_nchw_h(app_ctx->output_attrs[box_idx]);
        const int grid_w = get_nchw_w(app_ctx->output_attrs[box_idx]);
        const int grid_len = grid_h * grid_w;
        const int dfl_len = get_nchw_c(app_ctx->output_attrs[box_idx]) / 4;
        const int num_classes = get_nchw_c(app_ctx->output_attrs[score_idx]);
        const int seg_channels = get_nchw_c(app_ctx->output_attrs[seg_idx]);
        const int stride = app_ctx->model_height / grid_h;

        if (debug_frames_remaining > 0 && !use_i8) {
            debug_log_tensor_stats("seg-score", score_idx, score_tensor, num_classes, grid_len);
            if (aux_score_tensor != nullptr) {
                debug_log_tensor_stats("seg-aux", aux_score_idx,
                                       aux_score_tensor,
                                       get_nchw_c(app_ctx->output_attrs[aux_score_idx]),
                                       grid_len);
            }
        }

        // Cửa loại nhanh trên int8: quét max theo LỚP (liên tục trong bộ nhớ,
        // NEON) đúng như đường detect. Ngưỡng int8 lấy CHẶN DƯỚI (lùi một bước
        // lượng tử) nên không thể loại nhầm anchor; anchor sót lại vẫn được
        // chọn lớp bằng float y hệt bản cũ.
        thread_local std::vector<int8_t> seg_best_buf;
        int8_t* seg_best = nullptr;
        int8_t seg_gate = -128;
        if (use_i8) {
            float q = conf_threshold / score_scale + static_cast<float>(score_zp);
            q = floorf(q) - 1.0f;
            if (q < -128.0f) q = -128.0f;
            if (q > 127.0f) q = 127.0f;
            seg_gate = static_cast<int8_t>(q);
            if (static_cast<int>(seg_best_buf.size()) < grid_len) {
                seg_best_buf.resize(grid_len);
            }
            seg_best = seg_best_buf.data();
            std::fill(seg_best, seg_best + grid_len, seg_gate);
            max_score_per_anchor_i8(score_i8, num_classes, grid_len, seg_best);
        }

        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const int offset = y * grid_w + x;
                if (use_i8 && seg_best[offset] <= seg_gate) {
                    continue;
                }

                int best_class = -1;
                float best_score = 0.0f;
                for (int c = 0; c < num_classes; ++c) {
                    float score =
                        use_i8 ? deqnt_affine_to_f32(score_i8[c * grid_len + offset],
                                                     score_zp, score_scale)
                               : score_tensor[c * grid_len + offset];
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
                    raw_box[k] = use_i8 ? deqnt_affine_to_f32(
                                              box_i8[k * grid_len + offset],
                                              box_zp, box_scale)
                                        : box_tensor[k * grid_len + offset];
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
                    coeff[c] = use_i8 ? deqnt_affine_to_f32(
                                            seg_i8[c * grid_len + offset],
                                            coef_zp, coef_scale)
                                      : seg_tensor[c * grid_len + offset];
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
                   outputs[proto_idx].buf,
                   use_i8,
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

    // Đường INT8: model pose này là 1 lớp nên không có chuyện quét lớp; chi phí
    // thật là librknnrt dequant TOÀN BỘ output khi want_float=1 — 672.000 giá
    // trị mỗi lần suy luận, đo được 3,0 ms CPU. Giữ int8 rồi chỉ dequant cho
    // vài anchor sống sót. Ngưỡng vẫn được kiểm tra LẠI bằng float đúng như bản
    // cũ, nên kết quả không đổi.
    const bool use_i8 = app_ctx->is_quant && !outputs[0].want_float;

    const float* keypoint_tensor = static_cast<const float*>(outputs[3].buf);
    const int8_t* keypoint_tensor_i8 = static_cast<const int8_t*>(outputs[3].buf);
    const int kp_zp = app_ctx->output_attrs[3].zp;
    const float kp_scale = app_ctx->output_attrs[3].scale;
    const auto kp_at = [&](int i) {
        return use_i8 ? deqnt_affine_to_f32(keypoint_tensor_i8[i], kp_zp, kp_scale)
                      : keypoint_tensor[i];
    };
    const int model_keypoint_count = std::min(keypoint_count_from_attr(app_ctx->output_attrs[3]), AI_POSE_KEYPOINT_NUM);
    const int position_count = keypoint_positions(app_ctx->output_attrs[3]);
    std::vector<float> boxes_xywh;
    std::vector<float> scores;
    std::vector<int> class_ids;
    std::vector<int> anchor_indices;
    int anchor_base = 0;

    for (int branch = 0; branch < 3; ++branch) {
        const rknn_tensor_attr& attr = app_ctx->output_attrs[branch];
        const float* tensor = static_cast<const float*>(outputs[branch].buf);
        const int8_t* tensor_i8 = static_cast<const int8_t*>(outputs[branch].buf);
        const int zp = attr.zp;
        const float scale = attr.scale;
        const int grid_h = get_nchw_h(attr);
        const int grid_w = get_nchw_w(attr);
        const int stride = app_ctx->model_height / grid_h;
        const int grid_len = grid_h * grid_w;
        const int loc_len = 64;

        // Chặn dưới trên int8 cho kênh điểm. sigmoid đơn điệu tăng và dequant
        // cũng đơn điệu (scale > 0), nên chỉ anchor có q >= q_lo mới có cơ hội
        // vượt ngưỡng. Trừ thêm một bước để sai số làm tròn không làm mất anchor
        // — anchor sót lại vẫn bị kiểm tra lại bằng float ở ngay dưới.
        int q_lo = -128;
        bool branch_empty = false;
        if (use_i8 && scale > 0.0f && conf_threshold > 0.0f &&
            conf_threshold < 1.0f) {
            const float logit_thr =
                logf(conf_threshold / (1.0f - conf_threshold));
            const float q = logit_thr / scale + static_cast<float>(zp);
            if (q > 127.0f) {
                branch_empty = true;  // không anchor nào đạt nổi ngưỡng
            } else if (q > -128.0f) {
                q_lo = static_cast<int>(floorf(q)) - 1;
            }
        }
        if (branch_empty) {
            anchor_base += grid_len;
            continue;
        }

        for (int y = 0; y < grid_h; ++y) {
            for (int x = 0; x < grid_w; ++x) {
                const int offset = y * grid_w + x;
                float logit;
                if (use_i8) {
                    const int8_t q = tensor_i8[loc_len * grid_len + offset];
                    if (q < q_lo) {
                        continue;
                    }
                    logit = deqnt_affine_to_f32(q, zp, scale);
                } else {
                    logit = tensor[loc_len * grid_len + offset];
                }
                const float score = sigmoid(logit);
                if (score < conf_threshold) {
                    continue;
                }

                float loc[64];
                for (int i = 0; i < loc_len; ++i) {
                    loc[i] = use_i8 ? deqnt_affine_to_f32(
                                          tensor_i8[i * grid_len + offset], zp,
                                          scale)
                                    : tensor[i * grid_len + offset];
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
                out->keypoints[kp].x = clamp_float((kp_at(kp * 3 * position_count + anchor_idx) - letter_box->x_pad) / letter_box->scale,
                                                   0.0f,
                                                   static_cast<float>(app_ctx->model_width));
                out->keypoints[kp].y = clamp_float((kp_at(kp * 3 * position_count + position_count + anchor_idx) - letter_box->y_pad) / letter_box->scale,
                                                   0.0f,
                                                   static_cast<float>(app_ctx->model_height));
                out->keypoints[kp].score = kp_at(kp * 3 * position_count + position_count * 2 + anchor_idx);
            }
        } else {
            out->keypoint_count = 0;
        }
    }
    od_results->count = count;
    return 0;
}
