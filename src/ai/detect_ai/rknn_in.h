#ifndef DETECT_AI_RKNN_IN_H_
#define DETECT_AI_RKNN_IN_H_

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rknn_api.h"

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

// Zero-copy ĐẦU VÀO cho model KHÔNG lượng tử hoá (đầu vào fp16).
//
// Model fp16 nhận tensor fp16, mà engine chỉ có pixel uint8 — nên
// `rknn_inputs_set` phải cấp phát, đổi kiểu và chép 480×480×3 phần tử bằng CPU
// ở MỖI lần suy luận. Đo trên plate_det: **21,1 ms/lần**, tức hơn một phần ba
// tổng thời gian của cả tầng (58,6 ms), và đây là phí thuần chứ không phải
// tính toán. Cấp sẵn bộ nhớ đầu vào rồi tự đổi uint8 → fp16 bằng NEON ghi
// thẳng vào đó: **0,23 ms**, đầu ra giống nhau tới từng bit.
//
// Khác với zero-copy ĐẦU RA của yolov8, ở đây `rknn_set_io_mem` gọi ngay sau
// `rknn_init` là được — đã đối chứng cả hai thứ tự, tổng đầu ra trùng khớp.
//
// Model int8 KHÔNG cần đường này: đầu vào của nó là int8, `rknn_inputs_set`
// nhận thẳng uint8 nên chỉ mất 0,3 ms.
struct RknnZeroCopyIn {
    rknn_tensor_attr attr;
    rknn_tensor_mem* mem = nullptr;
    size_t elems = 0;  // số phần tử uint8 nguồn = W*H*C

    bool armed() const { return mem != nullptr; }
};

// Bật zero-copy đầu vào nếu tensor đầu vào là fp16 NHWC liền mạch. Mọi trường
// hợp khác trả về false và caller giữ nguyên `rknn_inputs_set` — đoán mò layout
// nguy hiểm hơn là chạy đường cũ.
inline bool rknn_in_arm(rknn_context ctx, int width, int height, int channels,
                        const char* tag, RknnZeroCopyIn* zc) {
#if defined(__aarch64__)
    // AI_PPOCR_ZEROCOPY_IN=0 quay về đường cũ — để đối chứng A/B trên CÙNG một
    // mức tải thật, vì đo lúc engine rảnh cho ra số khác hẳn (governor ondemand).
    {
        const char* env = getenv("AI_PPOCR_ZEROCOPY_IN");
        if (env != nullptr && env[0] == '0') return false;
    }
    memset(&zc->attr, 0, sizeof(zc->attr));
    zc->attr.index = 0;
    if (rknn_query(ctx, RKNN_QUERY_NATIVE_INPUT_ATTR, &zc->attr,
                   sizeof(rknn_tensor_attr)) != RKNN_SUCC) {
        return false;
    }
    const size_t n = (size_t)width * height * channels;
    if (zc->attr.type != RKNN_TENSOR_FLOAT16 ||
        zc->attr.fmt != RKNN_TENSOR_NHWC ||
        zc->attr.size_with_stride < n * sizeof(uint16_t)) {
        return false;
    }
    zc->mem = rknn_create_mem(ctx, zc->attr.size_with_stride);
    if (zc->mem == nullptr) return false;
    if (rknn_set_io_mem(ctx, zc->mem, &zc->attr) < 0) {
        rknn_destroy_mem(ctx, zc->mem);
        zc->mem = nullptr;
        return false;
    }
    zc->elems = n;
    printf("%s: zero-copy dau vao BAT (uint8 -> fp16 bang NEON)\n", tag);
    return true;
#else
    (void)ctx; (void)width; (void)height; (void)channels; (void)tag; (void)zc;
    return false;
#endif
}

inline void rknn_in_release(rknn_context ctx, RknnZeroCopyIn* zc) {
    if (zc->mem != nullptr) {
        rknn_destroy_mem(ctx, zc->mem);
        zc->mem = nullptr;
    }
}

// Nạp một ảnh uint8 HWC vào bộ nhớ đầu vào đã gắn. `src_stride` tính bằng BYTE
// của một hàng nguồn (ảnh cắt có thể có stride lớn hơn bề rộng thật).
//
// Chỉ dùng `vcvt_f16_f32` (lệnh FCVTN) — có sẵn ở MỌI aarch64. Phép TOÁN trên
// fp16 (`vcvtq_f16_u16`, `vst1q_f16`) thuộc phần mở rộng ARMv8.2 FP16 và cần
// `-march=...+fp16`, mà dự án không bật cờ đó; đây là chỗ dễ vỡ build nhất nếu
// sau này ai sửa lại.
inline void rknn_in_fill_u8(const RknnZeroCopyIn& zc, const uint8_t* src,
                            int width, int height, int channels,
                            int src_stride) {
#if defined(__aarch64__)
    uint16_t* dst = static_cast<uint16_t*>(zc.mem->virt_addr);
    const size_t row = (size_t)width * channels;
    for (int y = 0; y < height; ++y) {
        const uint8_t* s = src + (size_t)y * src_stride;
        uint16_t* d = dst + (size_t)y * row;
        size_t i = 0;
        for (; i + 16 <= row; i += 16) {
            const uint8x16_t v = vld1q_u8(s + i);
            const uint16x8_t w0 = vmovl_u8(vget_low_u8(v));
            const uint16x8_t w1 = vmovl_u8(vget_high_u8(v));
            const float16x4_t h0 = vcvt_f16_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(w0))));
            const float16x4_t h1 = vcvt_f16_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(w0))));
            const float16x4_t h2 = vcvt_f16_f32(vcvtq_f32_u32(vmovl_u16(vget_low_u16(w1))));
            const float16x4_t h3 = vcvt_f16_f32(vcvtq_f32_u32(vmovl_u16(vget_high_u16(w1))));
            vst1_u16(d + i,      vreinterpret_u16_f16(h0));
            vst1_u16(d + i + 4,  vreinterpret_u16_f16(h1));
            vst1_u16(d + i + 8,  vreinterpret_u16_f16(h2));
            vst1_u16(d + i + 12, vreinterpret_u16_f16(h3));
        }
        for (; i < row; ++i) {
            const float32x4_t f = vdupq_n_f32((float)s[i]);
            d[i] = vget_lane_u16(vreinterpret_u16_f16(vcvt_f16_f32(f)), 0);
        }
    }
#else
    (void)zc; (void)src; (void)width; (void)height; (void)channels; (void)src_stride;
#endif
}

#endif  // DETECT_AI_RKNN_IN_H_
