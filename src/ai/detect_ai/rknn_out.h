#ifndef DETECT_AI_RKNN_OUT_H_
#define DETECT_AI_RKNN_OUT_H_

#include <stdint.h>

#include "rknn_api.h"

// Đọc đầu ra RKNN Ở NGUYÊN KIỂU CỦA MODEL (`want_float = 0`).
//
// `want_float = 1` bắt librknnrt chuyển TOÀN BỘ tensor sang fp32 bằng CPU
// trước khi trả về. Với bản đồ xác suất 480×480 của ppocr_det (230.400 giá
// trị) đo được 1,76 ms mỗi lần gọi, so với 0,65 ms khi để nguyên kiểu — mà
// phần lớn giá trị đó chỉ dùng để so với một ngưỡng rồi vứt đi. Cùng lý do
// với đường zero-copy của yolov8 (xem postprocess.cc).
//
// Kiểu gốc phụ thuộc cách build model: fp16 nếu `do_quantization=False`,
// int8/uint8 nếu lượng tử hoá. `RknnOutView` che khác biệt đó đi.
struct RknnOutView {
    const void* buf = nullptr;
    rknn_tensor_type type = RKNN_TENSOR_FLOAT32;
    float scale = 1.0f;
    int32_t zp = 0;

    float value(size_t i) const {
        switch (type) {
#if defined(__aarch64__)
            case RKNN_TENSOR_FLOAT16:
                return static_cast<float>(static_cast<const __fp16*>(buf)[i]);
#endif
            case RKNN_TENSOR_INT8:
                return (static_cast<float>(static_cast<const int8_t*>(buf)[i]) -
                        static_cast<float>(zp)) * scale;
            case RKNN_TENSOR_UINT8:
                return (static_cast<float>(static_cast<const uint8_t*>(buf)[i]) -
                        static_cast<float>(zp)) * scale;
            default:
                return static_cast<const float*>(buf)[i];
        }
    }

    // Đánh dấu phần tử >= `thr` vào `mask` (1 byte mỗi phần tử). Tách riêng
    // khỏi `value()` vì đây là vòng lặp chạy trên TOÀN BỘ tensor: so sánh
    // ngay trong kiểu gốc thì trình dịch vector hoá được, còn gọi `value()`
    // cho từng phần tử thì kẹt lại ở một switch không nội tuyến nổi.
    void markAbove(size_t n, float thr, uint8_t* mask) const {
        switch (type) {
#if defined(__aarch64__)
            case RKNN_TENSOR_FLOAT16: {
                const __fp16* p = static_cast<const __fp16*>(buf);
                const __fp16 t = static_cast<__fp16>(thr);
                for (size_t i = 0; i < n; ++i) mask[i] = p[i] >= t ? 1 : 0;
                return;
            }
#endif
            case RKNN_TENSOR_INT8: {
                // Ngưỡng quy về miền lượng tử để khỏi dequant từng phần tử.
                // Làm tròn LÊN: q >= qt tương đương (q - zp) * scale >= thr.
                const int8_t* p = static_cast<const int8_t*>(buf);
                const float qf = thr / scale + static_cast<float>(zp);
                const int qi = static_cast<int>(qf > 0.0f ? qf + 0.999f : qf);
                if (qi > 127) { for (size_t i = 0; i < n; ++i) mask[i] = 0; return; }
                const int8_t qt = static_cast<int8_t>(qi < -128 ? -128 : qi);
                for (size_t i = 0; i < n; ++i) mask[i] = p[i] >= qt ? 1 : 0;
                return;
            }
            case RKNN_TENSOR_UINT8: {
                const uint8_t* p = static_cast<const uint8_t*>(buf);
                const float qf = thr / scale + static_cast<float>(zp);
                const int qi = static_cast<int>(qf > 0.0f ? qf + 0.999f : qf);
                if (qi > 255) { for (size_t i = 0; i < n; ++i) mask[i] = 0; return; }
                const uint8_t qt = static_cast<uint8_t>(qi < 0 ? 0 : qi);
                for (size_t i = 0; i < n; ++i) mask[i] = p[i] >= qt ? 1 : 0;
                return;
            }
            default: {
                const float* p = static_cast<const float*>(buf);
                for (size_t i = 0; i < n; ++i) mask[i] = p[i] >= thr ? 1 : 0;
                return;
            }
        }
    }
};

// `want_float` cần đặt cho tensor kiểu `t`: 0 nếu `RknnOutView` đọc thẳng
// được, 1 nếu phải nhờ librknnrt đổi (nền không có fp16 dựng sẵn).
inline int rknn_out_want_float(rknn_tensor_type t) {
#if defined(__aarch64__)
    const bool native = (t == RKNN_TENSOR_FLOAT16);
#else
    const bool native = false;
#endif
    return (native || t == RKNN_TENSOR_INT8 || t == RKNN_TENSOR_UINT8 ||
            t == RKNN_TENSOR_FLOAT32)
               ? 0
               : 1;
}

// Dựng view từ attr của model + bộ đệm mà `rknn_outputs_get` trả về.
// `want_float` phải đúng cái đã dùng khi gọi, vì nó quyết định kiểu thật
// của `buf` (fp32 khi bằng 1, kiểu gốc khi bằng 0).
inline RknnOutView rknn_out_view(const rknn_tensor_attr& attr, const void* buf,
                                 int want_float) {
    RknnOutView v;
    v.buf = buf;
    v.type = want_float ? RKNN_TENSOR_FLOAT32 : attr.type;
    v.scale = attr.scale != 0.0f ? attr.scale : 1.0f;
    v.zp = attr.zp;
    return v;
}

#endif  // DETECT_AI_RKNN_OUT_H_
