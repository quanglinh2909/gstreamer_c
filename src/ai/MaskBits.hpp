#ifndef AI_ENGINE_MASK_BITS_HPP
#define AI_ENGINE_MASK_BITS_HPP

// Hạ mẫu mask phân vùng của MỘT vật thành lưới bit MASK_GRID×MASK_GRID.
//
// Dùng chung cho CẢ HAI đường chạy model: pipeline RTSP (AiJob) và endpoint
// một-lần POST /inference/run (ImageInferenceService). Trước đây chỉ AiJob có,
// nên endpoint một-lần — vốn hứa trả "đúng JSON mà pipeline live bắn ra" —
// lặng lẽ thiếu hẳn khoá `mask`, và đó cũng là công cụ duy nhất thử được model
// mà không cần chờ vật đi vào khung camera.

#include <algorithm>
#include <cstdint>
#include <vector>

#include "AiResult.hpp"
#include "detect_ai/postprocess.h"

// `results.seg_mask` là ảnh nhãn cho CẢ KHUNG ở không gian MODEL (mỗi pixel =
// cls_id+1, 0 = nền). `d.box` cũng ở không gian MODEL — KHÔNG dùng det.x1/y1
// vì chúng đã đổi sang khung gốc.
inline void fillMaskBits(const object_detect_result_list& results,
                         const object_detect_result& d,
                         Detection& det) {
    const int G = Detection::MASK_GRID;
    const int mw = results.seg_width;
    const int mh = results.seg_height;
    const int bx1 = std::max(0, std::min(mw - 1, d.box.left));
    const int by1 = std::max(0, std::min(mh - 1, d.box.top));
    const int bx2 = std::max(bx1 + 1, std::min(mw, d.box.right));
    const int by2 = std::max(by1 + 1, std::min(mh, d.box.bottom));
    const int bw = bx2 - bx1;
    const int bh = by2 - by1;

    std::vector<uint8_t> bits(static_cast<size_t>(G) * G / 8, 0);
    const uint8_t want = static_cast<uint8_t>(d.cls_id + 1);
    bool any = false;
    for (int gy = 0; gy < G; ++gy) {
        // Tâm ô lưới, tránh lệch nửa ô khi hạ mẫu.
        const int sy = by1 + (2 * gy + 1) * bh / (2 * G);
        for (int gx = 0; gx < G; ++gx) {
            const int sx = bx1 + (2 * gx + 1) * bw / (2 * G);
            const int idx = sy * AI_SEG_MASK_WIDTH + sx;
            if (idx < 0 || idx >= AI_SEG_MASK_SIZE) continue;
            if (results.seg_mask[idx] != want) continue;
            const int bit = gy * G + gx;
            bits[bit >> 3] |= static_cast<uint8_t>(1u << (bit & 7));
            any = true;
        }
    }
    // Không có pixel nào của lớp này trong bbox -> đừng gửi 128 byte số 0.
    if (any) det.maskBits = std::move(bits);
}

#endif  // AI_ENGINE_MASK_BITS_HPP
