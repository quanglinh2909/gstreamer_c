#ifndef DETECT_AI_PPOCR_DUMP_H_
#define DETECT_AI_PPOCR_DUMP_H_

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "common.h"

// Ghi ĐẦU VÀO THẬT của một tầng PP-OCR ra đĩa để dựng bộ ảnh hiệu chuẩn int8.
//
// Vì sao không lấy đại vài trăm ảnh biển số có sẵn: lượng tử hoá chỉ tốt khi
// bộ hiệu chuẩn trùng phân bố với dữ liệu chạy thật, mà thứ đi vào det/rec
// KHÔNG phải ảnh biển thô — nó đã qua letterbox / fit-height + đệm xám 128 của
// chính pipeline, và với rec thì còn là một DÒNG chữ do det cắt ra. Lấy thẳng
// từ đây là khỏi phải dựng lại chuỗi tiền xử lý ở phía Python.
//
// Mặc định TẮT (không tốn gì khi không đặt biến môi trường):
//   AI_PPOCR_DUMP_DIR=/duong/dan   bật, ghi vào thư mục đó (phải tạo sẵn)
//   AI_PPOCR_DUMP_MAX=400          số ảnh tối đa mỗi tầng
// File ra là raw HWC uint8, tên `<tag>_<so>_<w>x<h>x<c>.bin`.
inline void ppocr_dump_input(const char* tag, const image_buffer_t* img,
                             int width, int height, int channels,
                             std::atomic<int>& counter) {
    static const char* dir = getenv("AI_PPOCR_DUMP_DIR");
    if (dir == nullptr || dir[0] == '\0') return;
    if (img == nullptr || img->virt_addr == nullptr) return;

    static const int maxN = [] {
        const char* s = getenv("AI_PPOCR_DUMP_MAX");
        const int v = s ? atoi(s) : 0;
        return v > 0 ? v : 400;
    }();

    const int n = counter.fetch_add(1);
    if (n >= maxN) return;

    char path[512];
    snprintf(path, sizeof(path), "%s/%s_%05d_%dx%dx%d.bin", dir, tag, n, width,
             height, channels);
    FILE* f = fopen(path, "wb");
    if (f == nullptr) return;
    const int srcStride =
        img->width_stride > 0 ? img->width_stride * channels : width * channels;
    for (int y = 0; y < height; ++y) {
        fwrite(img->virt_addr + (size_t)y * srcStride, 1,
               (size_t)width * channels, f);
    }
    fclose(f);
}

#endif  // DETECT_AI_PPOCR_DUMP_H_
