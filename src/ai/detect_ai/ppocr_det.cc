#include "ppocr_det.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <atomic>
#include <vector>

#include "file_utils.h"
#include "image_utils.h"
#include "npu_core.h"
#include "ppocr_dump.h"
#include "rknn_in.h"
#include "rknn_out.h"

// Ngưỡng nhị phân trên bản đồ xác suất (mặc định của PaddleOCR).
static const float kBinaryThresh = 0.3f;
// Bỏ vệt quá nhỏ: nhiễu một vài pixel không bao giờ là chữ đọc được.
static const int kMinArea = 12;
// DBNet học ra vùng chữ ĐÃ CO LẠI nên phải nong ra; 1.6 là mức PaddleOCR dùng.
static const float kUnclipRatio = 1.6f;

int init_ppocr_det_model(const char* model_path, rknn_app_context_t* app_ctx) {
    int model_len = 0;
    char* model = NULL;
    rknn_context ctx = 0;

    model_len = read_data_from_file(model_path, &model);
    if (model_len < 0 || model == NULL) {
        printf("ppocr_det: khong doc duoc model %s\n", model_path);
        return -1;
    }

    int ret = rknn_init(&ctx, model, model_len, 0, NULL);
    free(model);
    if (ret < 0) {
        printf("ppocr_det: rknn_init fail! ret=%d\n", ret);
        return -1;
    }

    npu_set_multicore(ctx, "ppocr_det");

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret != RKNN_SUCC) {
        printf("ppocr_det: rknn_query IN_OUT_NUM fail! ret=%d\n", ret);
        return -1;
    }

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (uint32_t i = 0; i < io_num.n_input; ++i) {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) return -1;
    }
    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (uint32_t i = 0; i < io_num.n_output; ++i) {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret != RKNN_SUCC) return -1;
    }

    app_ctx->rknn_ctx = ctx;
    // Theo model chứ không cứng `false`: bản fp16 và bản int8 dùng chung mã
    // này, chỉ khác kiểu tensor đầu ra.
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
    printf("ppocr_det: input %dx%d\n", app_ctx->model_width, app_ctx->model_height);

    // Model fp16: tự đổi uint8 → fp16 vào bộ nhớ NPU thay vì để rknn_inputs_set
    // làm — 21,1 ms xuống 0,23 ms mỗi lần gọi, đầu ra không đổi một bit.
    if (!app_ctx->is_quant) {
        RknnZeroCopyIn* zc = new RknnZeroCopyIn();
        if (rknn_in_arm(ctx, app_ctx->model_width, app_ctx->model_height,
                        app_ctx->model_channel > 0 ? app_ctx->model_channel : 3,
                        "ppocr_det", zc)) {
            app_ctx->zc_in = zc;
        } else {
            delete zc;
        }
    }
    return 0;
}

int release_ppocr_det_model(rknn_app_context_t* app_ctx) {
    // Nhả TRƯỚC rknn_destroy — rknn_destroy_mem cần context còn sống.
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

namespace {

struct Blob {
    int left, top, right, bottom;
    int area;
    double probSum;
};

// Gom các pixel "có chữ" liền nhau (8 hướng) thành từng vệt, lặp bằng NGĂN XẾP
// chứ không đệ quy: một dòng chữ dài có thể tới hàng chục nghìn pixel, đệ quy
// theo pixel là tràn stack.
//
// `above` là mặt nạ nhị phân dựng sẵn bởi `RknnOutView::markAbove` — vòng quét
// ngưỡng chạy trên cả 230.400 pixel nên phải nằm trong kiểu gốc của tensor để
// vector hoá được; còn `view.value()` chỉ gọi cho pixel THUỘC vệt (vài phần
// trăm số pixel) nên cái switch trong đó không đáng kể.
void collectBlobs(const uint8_t* above, const RknnOutView& view, int w, int h,
                  std::vector<Blob>& out) {
    std::vector<uint8_t> seen((size_t)w * h, 0);
    std::vector<int> stack;
    stack.reserve(1024);

    for (int y0 = 0; y0 < h; ++y0) {
        for (int x0 = 0; x0 < w; ++x0) {
            const size_t start = (size_t)y0 * w + x0;
            if (seen[start] || !above[start]) continue;

            Blob b{x0, y0, x0, y0, 0, 0.0};
            seen[start] = 1;
            stack.clear();
            stack.push_back((int)start);
            while (!stack.empty()) {
                const int idx = stack.back();
                stack.pop_back();
                const int x = idx % w;
                const int y = idx / w;
                ++b.area;
                b.probSum += view.value((size_t)idx);
                if (x < b.left) b.left = x;
                if (x > b.right) b.right = x;
                if (y < b.top) b.top = y;
                if (y > b.bottom) b.bottom = y;
                for (int dy = -1; dy <= 1; ++dy) {
                    for (int dx = -1; dx <= 1; ++dx) {
                        const int nx = x + dx;
                        const int ny = y + dy;
                        if (nx < 0 || ny < 0 || nx >= w || ny >= h) continue;
                        const size_t n = (size_t)ny * w + nx;
                        if (seen[n] || !above[n]) continue;
                        seen[n] = 1;
                        stack.push_back((int)n);
                    }
                }
            }
            if (b.area >= kMinArea) out.push_back(b);
        }
    }
}

// Gộp các vệt rời thành MỘT DÒNG chữ.
//
// DBNet trả về vùng nét chữ đã co lại. Chữ to, nét liền thì các ký tự dính
// nhau thành một vệt dài — đúng một dòng. Nhưng ảnh nhỏ (biển số 128px phóng
// lên) hoặc chữ thưa nét thì mỗi ký tự thành một vệt riêng: đo trên một ảnh
// biển 128×110 ra 20 mảnh, đưa từng mảnh cho rec thì chẳng mảnh nào thành chữ
// nên rơi hết, kết quả là "0 phát hiện".
//
// Điều kiện gộp: chồng nhau theo chiều DỌC quá nửa (cùng một dòng) và cách
// nhau theo chiều NGANG không quá 0,6 lần chiều cao (khoảng cách chữ/từ bình
// thường). Lấy chiều cao làm thước nên tự co giãn theo cỡ chữ.
void mergeIntoLines(std::vector<Blob>& blobs) {
    bool changed = true;
    while (changed) {
        changed = false;
        for (size_t i = 0; i < blobs.size(); ++i) {
            for (size_t j = i + 1; j < blobs.size();) {
                Blob& a = blobs[i];
                Blob& b = blobs[j];
                const int ha = a.bottom - a.top + 1;
                const int hb = b.bottom - b.top + 1;
                const int overlap = (a.bottom < b.bottom ? a.bottom : b.bottom) -
                                    (a.top > b.top ? a.top : b.top) + 1;
                const int minH = ha < hb ? ha : hb;
                const int maxH = ha > hb ? ha : hb;
                const int gap = (a.left > b.left ? a.left : b.left) -
                                (a.right < b.right ? a.right : b.right) - 1;
                // Chỉ gộp hai vệt CAO XẤP XỈ NHAU. Thiếu điều kiện này thì một
                // vệt cao (viền biển số, khung bảng) nuốt sạch chữ quanh nó:
                // đo trên ảnh biển 128×110 ra một hộp [0,0,128,108] ôm cả ảnh.
                const bool sameSize = minH * 2 >= maxH;
                if (sameSize && overlap > minH / 2 && gap <= (int)(0.6f * maxH)) {
                    if (b.left < a.left) a.left = b.left;
                    if (b.top < a.top) a.top = b.top;
                    if (b.right > a.right) a.right = b.right;
                    if (b.bottom > a.bottom) a.bottom = b.bottom;
                    a.area += b.area;
                    a.probSum += b.probSum;
                    blobs.erase(blobs.begin() + (long)j);
                    changed = true;
                    continue;
                }
                ++j;
            }
        }
    }
}

// Bóc thời gian của TỪNG khâu trong tầng det. Bật bằng AI_STAGE_TIMING=1 (dùng
// chung công tắc với StageRunner), in mỗi 100 lần gọi.
//
// Cần thiết vì "model 80 ms" của StageRunner gộp cả NPU lẫn hậu xử lý, mà hai
// thứ đó tối ưu theo hai hướng hoàn toàn khác nhau — và hậu xử lý DBNet (dò
// vệt liên thông + gộp dòng) là O(số vệt²) nên phình theo độ rối của ảnh chứ
// không cố định.
double nowMs() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

void detPhaseReport(double tIn, double tRun, double tGet, double tEnd,
                    size_t rawBlobs) {
    static const bool on = [] {
        const char* s = getenv("AI_STAGE_TIMING");
        return s != nullptr && s[0] != '0';
    }();
    if (!on) return;
    static std::atomic<long> calls{0};
    static std::atomic<long> blobSum{0};
    // Cộng dồn bằng số nguyên phần nghìn ms để khỏi cần khoá.
    static std::atomic<long> aIn{0}, aRun{0}, aPost{0};
    aIn += (long)((tRun - tIn) * 1000);
    aRun += (long)((tGet - tRun) * 1000);
    aPost += (long)((tEnd - tGet) * 1000);
    blobSum += (long)rawBlobs;
    const long n = ++calls;
    if (n % 100 != 0) return;
    printf("[ai time] ppocr_det %ld lan | nap anh %.1f | NPU+lay ket qua %.1f |"
           " hau xu ly %.1f ms/lan | %.0f vet/lan\n",
           n, aIn.load() / 1000.0 / n, aRun.load() / 1000.0 / n,
           aPost.load() / 1000.0 / n, (double)blobSum.load() / n);
    fflush(stdout);
}

}  // namespace

int inference_ppocr_det_model(rknn_app_context_t* app_ctx,
                              image_buffer_t* img,
                              float box_thresh,
                              ppocr_box_t* boxes_out,
                              int* box_count_out) {
    if (!app_ctx || !img || !img->virt_addr || !boxes_out || !box_count_out) {
        return -1;
    }
    *box_count_out = 0;

    const int width = app_ctx->model_width;
    const int height = app_ctx->model_height;
    const int channels = app_ctx->model_channel > 0 ? app_ctx->model_channel : 3;

    static std::atomic<int> dumpCount{0};
    ppocr_dump_input("det", img, width, height, channels, dumpCount);

    const int srcStride =
        img->width_stride > 0 ? img->width_stride * channels : width * channels;

    const double tIn = nowMs();
    int ret = 0;
    if (app_ctx->zc_in != NULL) {
        rknn_in_fill_u8(*app_ctx->zc_in, img->virt_addr, width, height, channels,
                        srcStride);
    } else {
        rknn_input inputs[1];
        memset(inputs, 0, sizeof(inputs));
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].size = (uint32_t)width * height * channels;
        inputs[0].pass_through = 0;
        inputs[0].buf = img->virt_addr;
        ret = rknn_inputs_set(app_ctx->rknn_ctx, app_ctx->io_num.n_input, inputs);
        if (ret < 0) {
            printf("ppocr_det: rknn_inputs_set fail! ret=%d\n", ret);
            return -1;
        }
    }
    const double tRun = nowMs();
    ret = rknn_run(app_ctx->rknn_ctx, nullptr);
    if (ret < 0) {
        printf("ppocr_det: rknn_run fail! ret=%d\n", ret);
        return -1;
    }

    const rknn_tensor_attr& oa = app_ctx->output_attrs[0];
    // Để nguyên kiểu gốc của tensor thay vì bắt librknnrt dequant cả 230.400
    // giá trị sang fp32 — xem rknn_out.h.
    const int wantFloat = rknn_out_want_float(oa.type);

    rknn_output outputs[1];
    memset(outputs, 0, sizeof(outputs));
    outputs[0].index = 0;
    outputs[0].want_float = wantFloat;
    ret = rknn_outputs_get(app_ctx->rknn_ctx, 1, outputs, NULL);
    if (ret < 0) {
        printf("ppocr_det: rknn_outputs_get fail! ret=%d\n", ret);
        return -1;
    }

    // Bản đồ xác suất [1,1,H,W] — lấy hai chiều CUỐI, khỏi phụ thuộc thứ tự.
    const int mapH = oa.n_dims >= 2 ? (int)oa.dims[oa.n_dims - 2] : 0;
    const int mapW = oa.n_dims >= 1 ? (int)oa.dims[oa.n_dims - 1] : 0;
    if (mapW <= 0 || mapH <= 0 || outputs[0].buf == NULL) {
        rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
        return -1;
    }
    const RknnOutView view = rknn_out_view(oa, outputs[0].buf, wantFloat);

    const double tGet = nowMs();

    std::vector<uint8_t> above((size_t)mapW * mapH, 0);
    view.markAbove(above.size(), kBinaryThresh, above.data());

    std::vector<Blob> blobs;
    collectBlobs(above.data(), view, mapW, mapH, blobs);
    const size_t rawBlobs = blobs.size();
    mergeIntoLines(blobs);
    detPhaseReport(tIn, tRun, tGet, nowMs(), rawBlobs);

    const float sx = (float)width / (float)mapW;
    const float sy = (float)height / (float)mapH;
    int count = 0;
    for (const Blob& b : blobs) {
        if (count >= PPOCR_MAX_BOXES) break;
        const float score = (float)(b.probSum / (double)b.area);
        if (score < box_thresh) continue;

        const int bw = b.right - b.left + 1;
        const int bh = b.bottom - b.top + 1;
        if (bw < 3 || bh < 3) continue;

        // Nong hộp ra: khoảng cách nong = diện tích * tỉ lệ / chu vi, đúng công
        // thức unclip của DBNet (tính trên hình chữ nhật bao).
        const double perim = 2.0 * (bw + bh);
        const int grow = perim > 0.0 ? (int)((double)bw * bh * kUnclipRatio / perim) : 0;

        int left = (int)((b.left - grow) * sx);
        int top = (int)((b.top - grow) * sy);
        int right = (int)((b.right + grow + 1) * sx);
        int bottom = (int)((b.bottom + grow + 1) * sy);
        if (left < 0) left = 0;
        if (top < 0) top = 0;
        if (right > width) right = width;
        if (bottom > height) bottom = height;
        if (right <= left || bottom <= top) continue;

        boxes_out[count].left = left;
        boxes_out[count].top = top;
        boxes_out[count].right = right;
        boxes_out[count].bottom = bottom;
        boxes_out[count].score = score;
        ++count;
    }
    *box_count_out = count;

    rknn_outputs_release(app_ctx->rknn_ctx, 1, outputs);
    return 0;
}
