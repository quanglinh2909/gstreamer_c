#ifndef AI_ENGINE_RGA_CONVERTER_HPP
#define AI_ENGINE_RGA_CONVERTER_HPP

// Hardware colour-convert / resize / crop via Rockchip RGA (librga im2d).
// Every pixel operation on the AI path goes through here so nothing touches
// the CPU for image scaling.
//
// API-version note: librga's im2d signatures have changed across releases.
// This targets the variant where improcess() takes fence + opt arguments.
// If the build fails on improcess(), match it to the installed
// /usr/include/rga/im2d_single.h.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include <rga/im2d.h>
#include <rga/rga.h>

#include "FrameTypes.hpp"

namespace rga {

// All RGA blits are serialised through one mutex. librga's virtual-address
// (MMU) path is not reliable when several threads submit blits at once —
// under multiple AI streams it fails with "RGA_BLIT Invalid argument". RGA is
// a single hardware engine anyway, so serialising costs little.
inline std::mutex& rgaMutex() {
    static std::mutex m;
    return m;
}

inline int alignDown2(int v) { return v & ~1; }
inline int alignUp2(int v) { return (v + 1) & ~1; }

// RGA (especially the RGA3 core on RK3588) rejects very small crops. Pads a
// crop rect out to at least minSize in each dimension, kept centred on the
// original box and clamped to the frame, with even coordinates for NV12
// chroma. Mutates x/y/w/h in place.
inline void expandCropToMin(int& x, int& y, int& w, int& h,
                            int frameW, int frameH, int minSize = 128) {
    const int cw = std::min(std::max(w, minSize), frameW);
    const int ch = std::min(std::max(h, minSize), frameH);
    int cx = x + w / 2 - cw / 2;
    int cy = y + h / 2 - ch / 2;
    if (cx < 0) cx = 0;
    if (cy < 0) cy = 0;
    if (cx + cw > frameW) cx = frameW - cw;
    if (cy + ch > frameH) cy = frameH - ch;
    x = alignDown2(std::max(0, cx));
    y = alignDown2(std::max(0, cy));
    w = alignDown2(cw);
    h = alignDown2(ch);
}

inline rga_buffer_t emptyBuffer() {
    rga_buffer_t b;
    std::memset(&b, 0, sizeof(b));
    return b;
}

inline im_rect makeRect(int x, int y, int w, int h) {
    im_rect r;
    r.x = x;
    r.y = y;
    r.width = w;
    r.height = h;
    return r;
}

// NV12 (full-res) -> letterboxed RGB888 at frame.inferW x frame.inferH.
// Black/gray bars keep aspect ratio so the detector sees no distortion.
// Fills frame.rgb and the contentX/Y/W/H rect.
inline bool letterboxNv12ToRgb(Frame& f, int padColor) {
    if (!f.nv12 || f.width <= 0 || f.height <= 0 || f.inferW <= 0 || f.inferH <= 0) {
        return false;
    }

    f.rgb.assign(static_cast<size_t>(f.inferW) * f.inferH * 3,
                 static_cast<uint8_t>(padColor));

    const float scale = std::min(static_cast<float>(f.inferW) / f.width,
                                 static_cast<float>(f.inferH) / f.height);
    int newW = alignDown2(std::max(2, static_cast<int>(f.width * scale)));
    int newH = alignDown2(std::max(2, static_cast<int>(f.height * scale)));
    int offX = alignDown2((f.inferW - newW) / 2);
    int offY = alignDown2((f.inferH - newH) / 2);

    f.contentX = offX;
    f.contentY = offY;
    f.contentW = newW;
    f.contentH = newH;

    const int ywstride = f.yStride > 0 ? f.yStride : f.width;
    const int yhstride = f.uvStride > 0 && f.uvOffset > 0
                             ? static_cast<int>(f.uvOffset / ywstride)
                             : f.height;

    rga_buffer_t src = wrapbuffer_virtualaddr(f.nv12, f.width, f.height,
                                              RK_FORMAT_YCbCr_420_SP,
                                              ywstride, yhstride);
    rga_buffer_t dst = wrapbuffer_virtualaddr(f.rgb.data(), f.inferW, f.inferH,
                                              RK_FORMAT_RGB_888,
                                              f.inferW, f.inferH);
    rga_buffer_t pat = emptyBuffer();

    im_rect srect = makeRect(0, 0, f.width, f.height);
    im_rect drect = makeRect(offX, offY, newW, newH);
    im_rect prect = makeRect(0, 0, 0, 0);

    IM_STATUS st;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        st = improcess(src, dst, pat, srect, drect, prect,
                       0, nullptr, nullptr, IM_SYNC);
    }
    return st == IM_STATUS_SUCCESS;
}

// Crop a box (full-res coordinates) and resize+convert it to packed RGB888.
// RGA requires RGB888 pixel stride to be 16-aligned. When the requested dstW
// is not, we route the blit through a wider scratch buffer, then memcpy each
// row back into the tight `out` the caller expects.
inline bool cropNv12ToRgb(const Frame& f, int x, int y, int w, int h,
                          int dstW, int dstH, std::vector<uint8_t>& out) {
    if (!f.nv12 || dstW <= 0 || dstH <= 0) return false;

    x = std::max(0, alignDown2(x));
    y = std::max(0, alignDown2(y));
    w = alignUp2(std::min(w, f.width - x));
    h = alignUp2(std::min(h, f.height - y));
    if (w <= 0 || h <= 0) return false;

    out.assign(static_cast<size_t>(dstW) * dstH * 3, 0);

    const int ywstride = f.yStride > 0 ? f.yStride : f.width;
    const int yhstride = f.uvStride > 0 && f.uvOffset > 0
                             ? static_cast<int>(f.uvOffset / ywstride)
                             : f.height;

    const int alignedDstW = (dstW + 15) & ~15;
    std::vector<uint8_t> scratch;
    uint8_t* dstPtr = out.data();
    int dstStridePixels = dstW;
    if (alignedDstW != dstW) {
        scratch.assign(static_cast<size_t>(alignedDstW) * dstH * 3, 0);
        dstPtr = scratch.data();
        dstStridePixels = alignedDstW;
    }

    rga_buffer_t src = wrapbuffer_virtualaddr(f.nv12, f.width, f.height,
                                              RK_FORMAT_YCbCr_420_SP,
                                              ywstride, yhstride);
    rga_buffer_t dst = wrapbuffer_virtualaddr(dstPtr, dstW, dstH,
                                              RK_FORMAT_RGB_888,
                                              dstStridePixels, dstH);
    rga_buffer_t pat = emptyBuffer();

    im_rect srect = makeRect(x, y, w, h);
    im_rect drect = makeRect(0, 0, dstW, dstH);
    im_rect prect = makeRect(0, 0, 0, 0);

    IM_STATUS st;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        st = improcess(src, dst, pat, srect, drect, prect,
                       0, nullptr, nullptr, IM_SYNC);
    }
    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr,
                     "cropNv12ToRgb: improcess failed src=%dx%d (ystride=%d) "
                     "rect=(%d,%d,%d,%d) dst=%dx%d stride=%d status=%d msg=%s\n",
                     f.width, f.height, ywstride, x, y, w, h,
                     dstW, dstH, dstStridePixels,
                     static_cast<int>(st), imStrError(st));
        return false;
    }

    if (dstPtr != out.data()) {
        for (int row = 0; row < dstH; ++row) {
            std::memcpy(out.data() + static_cast<size_t>(row) * dstW * 3,
                        scratch.data() + static_cast<size_t>(row) * alignedDstW * 3,
                        static_cast<size_t>(dstW) * 3);
        }
    }
    return true;
}

// Crop a box (full-res coordinates) to a packed NV12 buffer of the crop size.
// Used to feed the hardware JPEG encoder for saved crop images.
inline bool cropNv12ToNv12(const Frame& f, int x, int y, int w, int h,
                           std::vector<uint8_t>& out, int& outW, int& outH) {
    if (!f.nv12) return false;

    x = std::max(0, alignDown2(x));
    y = std::max(0, alignDown2(y));
    w = alignDown2(std::min(w, f.width - x));
    h = alignDown2(std::min(h, f.height - y));
    if (w <= 1 || h <= 1) return false;

    outW = w;
    outH = h;
    out.assign(static_cast<size_t>(w) * h * 3 / 2, 0);

    const int ywstride = f.yStride > 0 ? f.yStride : f.width;
    const int yhstride = f.uvStride > 0 && f.uvOffset > 0
                             ? static_cast<int>(f.uvOffset / ywstride)
                             : f.height;

    rga_buffer_t src = wrapbuffer_virtualaddr(f.nv12, f.width, f.height,
                                              RK_FORMAT_YCbCr_420_SP,
                                              ywstride, yhstride);
    rga_buffer_t dst = wrapbuffer_virtualaddr(out.data(), w, h,
                                              RK_FORMAT_YCbCr_420_SP, w, h);
    rga_buffer_t pat = emptyBuffer();

    im_rect srect = makeRect(x, y, w, h);
    im_rect drect = makeRect(0, 0, w, h);
    im_rect prect = makeRect(0, 0, 0, 0);

    IM_STATUS st;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        st = improcess(src, dst, pat, srect, drect, prect,
                       0, nullptr, nullptr, IM_SYNC);
    }
    return st == IM_STATUS_SUCCESS;
}

// Packed RGB888 (w x h) -> tightly-packed NV12 (w x h). Caller MUST pass even
// w and h (NV12 chroma is 2x2-subsampled). Logs the RGA status on failure so
// the caller knows whether it was an argument problem or a hardware reject.
inline bool rgbToNv12(const uint8_t* rgb, int w, int h, uint8_t* nv12Out) {
    if (!rgb || !nv12Out || w <= 0 || h <= 0 || (w & 1) || (h & 1)) {
        std::fprintf(stderr,
                     "rgbToNv12: invalid args (rgb=%p out=%p w=%d h=%d)\n",
                     rgb, nv12Out, w, h);
        return false;
    }

    rga_buffer_t src = wrapbuffer_virtualaddr(const_cast<uint8_t*>(rgb), w, h,
                                              RK_FORMAT_RGB_888, w, h);
    rga_buffer_t dst = wrapbuffer_virtualaddr(nv12Out, w, h,
                                              RK_FORMAT_YCbCr_420_SP, w, h);
    rga_buffer_t pat = emptyBuffer();

    im_rect r = makeRect(0, 0, w, h);
    im_rect p = makeRect(0, 0, 0, 0);

    IM_STATUS st;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        st = improcess(src, dst, pat, r, r, p, 0, nullptr, nullptr, IM_SYNC);
    }
    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr, "rgbToNv12: improcess failed (w=%d h=%d) status=%d msg=%s\n",
                     w, h, static_cast<int>(st), imStrError(st));
    }
    return st == IM_STATUS_SUCCESS;
}

}  // namespace rga

#endif  // AI_ENGINE_RGA_CONVERTER_HPP
