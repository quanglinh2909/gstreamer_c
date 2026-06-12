#ifndef AI_ENGINE_RGA_CONVERTER_HPP
#define AI_ENGINE_RGA_CONVERTER_HPP

// Hardware colour-convert / resize / crop via Rockchip RGA (librga im2d).
// Every pixel operation on the AI path goes through here so nothing touches
// the CPU for image scaling.
//
// Buffer hand-off rules (learned the hard way on this board, see
// DmaHeapBuffer.hpp for the kernel-oops and stale-cache details):
//
//   * decoder frames (dmabuf) are given to RGA BY FD, never by mapped
//     pointer — get_user_pages on a dmabuf mmap intermittently feeds the
//     driver a bogus page and the kernel oopses in __clean_dcache_area_poc,
//     sometimes freezing the whole board;
//   * the RGA kernel runs a job either fully in handle mode or fully in
//     raw virtual-address mode — mixing one imported handle with one raw
//     pointer in the same blit is rejected;
//   * CPU-read destinations in handle mode live in dma-heap memory and are
//     cache-invalidated (DMA_BUF_IOCTL_SYNC) after the blit — imported
//     malloc memory gets no per-job cache maintenance and the CPU reads
//     stale lines (posterised images);
//   * synthetic frames (no dmabuf, e.g. HTTP image inference) keep the
//     legacy va→va path: anonymous memory on both sides is what that path
//     has always handled safely.
//
// Set AI_RGA_LEGACY=1 to force the old all-virtualaddr behaviour everywhere
// (emergency escape hatch; brings back the dmabuf-gup oops risk).
//
// API-version note: librga's im2d signatures have changed across releases.
// This targets the variant where improcess() takes fence + opt arguments.
// If the build fails on improcess(), match it to the installed
// /usr/include/rga/im2d_single.h.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <rga/im2d.h>
#include <rga/rga.h>

#include "DmaHeapBuffer.hpp"
#include "FrameTypes.hpp"
#include "RgaLock.hpp"

namespace rga {

inline bool legacyMode() {
    static const bool on = [] {
        const char* v = std::getenv("AI_RGA_LEGACY");
        return v && v[0] == '1';
    }();
    return on;
}

inline int alignDown2(int v) { return v & ~1; }
inline int alignUp2(int v) { return (v + 1) & ~1; }

inline int frameYWStride(const Frame& f) {
    return f.yStride > 0 ? f.yStride : f.width;
}

inline int frameYHStride(const Frame& f) {
    const int yws = frameYWStride(f);
    return (f.uvStride > 0 && f.uvOffset > 0)
               ? static_cast<int>(f.uvOffset / yws)
               : f.height;
}

// Imports the frame's dmabuf into the RGA driver. Called once per frame by
// the camera pipeline; the handle lives in the Frame and is released by its
// destructor. Returns false (frame falls back to the CPU-pointer path) on
// failure.
inline bool importFrameDmabuf(Frame& f) {
    if (legacyMode()) return false;
    if (f.dmaFd < 0 || f.rgaHandle) return f.rgaHandle != 0;
    im_handle_param_t param;
    param.width = static_cast<uint32_t>(frameYWStride(f));
    param.height = static_cast<uint32_t>(frameYHStride(f));
    param.format = RK_FORMAT_YCbCr_420_SP;
    rga_buffer_handle_t handle;
    {
        std::lock_guard<std::mutex> lock(rgaMutex());
        handle = importbuffer_fd(f.dmaFd, &param);
    }
    if (!handle) {
        std::fprintf(stderr,
                     "rga: importbuffer_fd failed (fd=%d %dx%d) — "
                     "falling back to mapped memory\n",
                     f.dmaFd, f.width, f.height);
        return false;
    }
    f.rgaHandle = handle;
    return true;
}

// Handle-mode source descriptor. Only valid when f.rgaHandle != 0.
inline rga_buffer_t frameSrcHandle(const Frame& f) {
    return wrapbuffer_handle(f.rgaHandle, f.width, f.height,
                             RK_FORMAT_YCbCr_420_SP,
                             frameYWStride(f), frameYHStride(f));
}

// Raw-pointer source descriptor (legacy / synthetic-frame mode). Maps the
// buffer lazily. WARNING for dmabuf-backed frames this is the path that can
// oops the kernel — used only when the fd import failed or AI_RGA_LEGACY=1.
inline bool frameSrcVa(const Frame& f, rga_buffer_t& out) {
    const uint8_t* nv12 = f.nv12 ? f.nv12 : f.cpuNv12();
    if (!nv12) return false;
    out = wrapbuffer_virtualaddr(const_cast<uint8_t*>(nv12), f.width, f.height,
                                 RK_FORMAT_YCbCr_420_SP,
                                 frameYWStride(f), frameYHStride(f));
    return true;
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

inline IM_STATUS runBlit(const rga_buffer_t& src, const rga_buffer_t& dst,
                         const im_rect& srect, const im_rect& drect) {
    rga_buffer_t pat = emptyBuffer();
    im_rect prect = makeRect(0, 0, 0, 0);
    std::lock_guard<std::mutex> lock(rgaMutex());
    return improcess(src, dst, pat, srect, drect, prect,
                     0, nullptr, nullptr, IM_SYNC);
}

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

// NV12 (full-res) -> letterboxed RGB888 at frame.inferW x frame.inferH.
// Black/gray bars keep aspect ratio so the detector sees no distortion.
// Fills frame.rgb and the contentX/Y/W/H rect.
inline bool letterboxNv12ToRgb(Frame& f, int padColor) {
    if (f.width <= 0 || f.height <= 0 || f.inferW <= 0 || f.inferH <= 0) {
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

    im_rect srect = makeRect(0, 0, f.width, f.height);
    im_rect drect = makeRect(offX, offY, newW, newH);

    if (f.rgaHandle) {
        thread_local DmaHeapBuffer scratch;
        const size_t bytes = static_cast<size_t>(f.inferW) * f.inferH * 3;
        if (scratch.ensure(f.inferW, f.inferH, RK_FORMAT_RGB_888, bytes)) {
            IM_STATUS st = runBlit(frameSrcHandle(f),
                                   scratch.wrap(f.inferW, f.inferH),
                                   srect, drect);
            if (st != IM_STATUS_SUCCESS) return false;
            scratch.syncForCpuRead();
            // Copy only the content span of each row; the bars in f.rgb
            // already carry padColor and the scratch outside the content
            // rect is stale garbage from previous frames.
            for (int row = offY; row < offY + newH; ++row) {
                const size_t off =
                    (static_cast<size_t>(row) * f.inferW + offX) * 3;
                std::memcpy(f.rgb.data() + off, scratch.data() + off,
                            static_cast<size_t>(newW) * 3);
            }
            return true;
        }
        // dma-heap unavailable: drop to the va path below (risky for dmabuf
        // sources, but better than producing nothing). Warn once.
        static std::once_flag warned;
        std::call_once(warned, [] {
            std::fprintf(stderr,
                         "rga: dma-heap unavailable, using legacy va path "
                         "(known kernel-oops risk on dmabuf frames)\n");
        });
    }

    rga_buffer_t src;
    if (!frameSrcVa(f, src)) return false;
    rga_buffer_t dst = wrapbuffer_virtualaddr(f.rgb.data(), f.inferW, f.inferH,
                                              RK_FORMAT_RGB_888,
                                              f.inferW, f.inferH);
    return runBlit(src, dst, srect, drect) == IM_STATUS_SUCCESS;
}

// Crop a box (full-res coordinates) and resize+convert it to packed RGB888.
// RGA requires RGB888 pixel stride to be 16-aligned, so the blit lands in a
// stride-aligned scratch and rows are copied back into the tight `out`.
inline bool cropNv12ToRgb(const Frame& f, int x, int y, int w, int h,
                          int dstW, int dstH, std::vector<uint8_t>& out) {
    if (dstW <= 0 || dstH <= 0) return false;

    x = std::max(0, alignDown2(x));
    y = std::max(0, alignDown2(y));
    w = alignUp2(std::min(w, f.width - x));
    h = alignUp2(std::min(h, f.height - y));
    if (w <= 0 || h <= 0) return false;

    out.resize(static_cast<size_t>(dstW) * dstH * 3);

    const int alignedDstW = (dstW + 15) & ~15;
    im_rect srect = makeRect(x, y, w, h);
    im_rect drect = makeRect(0, 0, dstW, dstH);

    if (f.rgaHandle) {
        thread_local DmaHeapBuffer scratch;
        const size_t bytes = static_cast<size_t>(alignedDstW) * dstH * 3;
        if (scratch.ensure(alignedDstW, dstH, RK_FORMAT_RGB_888, bytes)) {
            IM_STATUS st = runBlit(frameSrcHandle(f),
                                   scratch.wrap(dstW, dstH), srect, drect);
            if (st != IM_STATUS_SUCCESS) {
                std::fprintf(stderr,
                             "cropNv12ToRgb: improcess failed src=%dx%d "
                             "rect=(%d,%d,%d,%d) dst=%dx%d status=%d msg=%s\n",
                             f.width, f.height, x, y, w, h, dstW, dstH,
                             static_cast<int>(st), imStrError(st));
                return false;
            }
            scratch.syncForCpuRead();
            for (int row = 0; row < dstH; ++row) {
                std::memcpy(
                    out.data() + static_cast<size_t>(row) * dstW * 3,
                    scratch.data() + static_cast<size_t>(row) * alignedDstW * 3,
                    static_cast<size_t>(dstW) * 3);
            }
            return true;
        }
    }

    // Legacy / synthetic-frame path: blit straight into out (via a plain
    // scratch vector only when the stride needs alignment).
    std::vector<uint8_t> scratchVec;
    uint8_t* dstPtr = out.data();
    int dstStridePixels = dstW;
    if (alignedDstW != dstW) {
        scratchVec.resize(static_cast<size_t>(alignedDstW) * dstH * 3);
        dstPtr = scratchVec.data();
        dstStridePixels = alignedDstW;
    }

    rga_buffer_t src;
    if (!frameSrcVa(f, src)) return false;
    rga_buffer_t dst = wrapbuffer_virtualaddr(dstPtr, dstW, dstH,
                                              RK_FORMAT_RGB_888,
                                              dstStridePixels, dstH);
    IM_STATUS st = runBlit(src, dst, srect, drect);
    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr,
                     "cropNv12ToRgb: improcess failed src=%dx%d (ystride=%d) "
                     "rect=(%d,%d,%d,%d) dst=%dx%d stride=%d status=%d msg=%s\n",
                     f.width, f.height, frameYWStride(f), x, y, w, h,
                     dstW, dstH, dstStridePixels,
                     static_cast<int>(st), imStrError(st));
        return false;
    }

    if (dstPtr != out.data()) {
        for (int row = 0; row < dstH; ++row) {
            std::memcpy(out.data() + static_cast<size_t>(row) * dstW * 3,
                        scratchVec.data() + static_cast<size_t>(row) * alignedDstW * 3,
                        static_cast<size_t>(dstW) * 3);
        }
    }
    return true;
}

// A packed NV12 crop ready for the hardware JPEG encoder. `fd >= 0` means the
// pixels live in a dmabuf the encoder must import BY FD (never by ptr — a
// get_user_pages on the mmap faults the codec IOMMU and freezes the board).
// `ptr` is the matching CPU view, used only by the legacy/non-dmabuf path.
// Both reference a per-thread staging buffer valid until the next rga call on
// the same thread.
struct PackedNv12 {
    const uint8_t* ptr = nullptr;
    int fd = -1;
    int w = 0;
    int h = 0;
    explicit operator bool() const { return ptr != nullptr; }
};

// Crop a box (full-res coordinates) to a packed NV12 image of the crop size,
// used to feed the hardware JPEG encoder. The result references a per-thread
// staging buffer — valid until the NEXT rga call on the same thread.
inline PackedNv12 cropNv12ToNv12(const Frame& f, int x, int y, int w, int h) {
    PackedNv12 out;
    x = std::max(0, alignDown2(x));
    y = std::max(0, alignDown2(y));
    w = alignDown2(std::min(w, f.width - x));
    h = alignDown2(std::min(h, f.height - y));
    if (w <= 1 || h <= 1) return out;

    const size_t bytes = static_cast<size_t>(w) * h * 3 / 2;
    im_rect srect = makeRect(x, y, w, h);
    im_rect drect = makeRect(0, 0, w, h);

    if (f.rgaHandle) {
        thread_local DmaHeapBuffer scratch;
        if (scratch.ensure(w, h, RK_FORMAT_YCbCr_420_SP, bytes)) {
            if (runBlit(frameSrcHandle(f), scratch.wrap(w, h), srect,
                        drect) != IM_STATUS_SUCCESS) {
                return out;
            }
            // Invalidate the CPU cache so a software (libjpeg) encoder reads
            // the RGA-written pixels correctly. Cheap (one ioctl) and a no-op
            // cost for the hardware-fd path, which ignores out.ptr.
            scratch.syncForCpuRead();
            out.ptr = scratch.data();
            out.fd = scratch.fd();
            out.w = w;
            out.h = h;
            return out;
        }
    }

    thread_local std::vector<uint8_t> scratchVec;
    scratchVec.resize(bytes);
    rga_buffer_t src;
    if (!frameSrcVa(f, src)) return out;
    rga_buffer_t dst = wrapbuffer_virtualaddr(scratchVec.data(), w, h,
                                              RK_FORMAT_YCbCr_420_SP, w, h);
    if (runBlit(src, dst, srect, drect) != IM_STATUS_SUCCESS) return out;
    out.ptr = scratchVec.data();
    out.fd = -1;  // anonymous memory: encoder must use the CPU-ptr path
    out.w = w;
    out.h = h;
    return out;
}

// Packed RGB888 (w x h) -> tightly-packed NV12 (w x h). Caller MUST pass even
// w and h (NV12 chroma is 2x2-subsampled). Both sides are anonymous CPU
// memory (HTTP image inference), which the va path handles safely.
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
    im_rect r = makeRect(0, 0, w, h);
    IM_STATUS st = runBlit(src, dst, r, r);
    if (st != IM_STATUS_SUCCESS) {
        std::fprintf(stderr, "rgbToNv12: improcess failed (w=%d h=%d) status=%d msg=%s\n",
                     w, h, static_cast<int>(st), imStrError(st));
    }
    return st == IM_STATUS_SUCCESS;
}

}  // namespace rga

#endif  // AI_ENGINE_RGA_CONVERTER_HPP
