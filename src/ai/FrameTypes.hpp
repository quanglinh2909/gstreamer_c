#ifndef AI_ENGINE_FRAME_TYPES_HPP
#define AI_ENGINE_FRAME_TYPES_HPP

#include <cstdint>
#include <memory>
#include <vector>

#include <gst/gst.h>

// One decoded camera frame. Created once per frame by the camera decoder and
// shared (shared_ptr) to every AI job queue of that camera — decode once,
// fan out by ref-count.
//
// It carries two views of the same picture:
//   * the full-resolution NV12 buffer, kept alive through the GstSample ref,
//     used for stage-2 crops and for JPEG encoding of saved images;
//   * a letterboxed RGB888 image at inference size (640x640) that every
//     detector consumes directly.
struct Frame {
    uint64_t seq = 0;
    int64_t ptsUs = 0;

    // --- full-res NV12 (zero-copy view into the decoder's GstBuffer) ---
    GstSample* sample = nullptr;
    GstMapInfo map{};
    bool mapped = false;
    uint8_t* nv12 = nullptr;   // == map.data
    int width = 0;
    int height = 0;
    int yStride = 0;           // Y plane row stride in bytes
    int uvStride = 0;          // interleaved UV plane row stride in bytes
    size_t uvOffset = 0;       // byte offset of the UV plane inside nv12

    // --- inference image: letterboxed RGB888 at infer size ---
    int inferW = 0;
    int inferH = 0;
    std::vector<uint8_t> rgb;  // inferW * inferH * 3

    // Rect of the real (un-padded) content inside the inference image. Used to
    // map detection boxes from inference space back to full-res coordinates.
    int contentX = 0;
    int contentY = 0;
    int contentW = 0;
    int contentH = 0;

    Frame() = default;
    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    ~Frame() {
        if (mapped && sample) {
            GstBuffer* buf = gst_sample_get_buffer(sample);
            if (buf) gst_buffer_unmap(buf, &map);
        }
        if (sample) gst_sample_unref(sample);
    }

    // Maps a point from inference space to full-res space.
    void inferToOrig(float ix, float iy, float* ox, float* oy) const {
        const float cw = contentW > 0 ? static_cast<float>(contentW) : static_cast<float>(inferW);
        const float ch = contentH > 0 ? static_cast<float>(contentH) : static_cast<float>(inferH);
        const float sx = cw > 1e-6f ? static_cast<float>(width) / cw : 1.0f;
        const float sy = ch > 1e-6f ? static_cast<float>(height) / ch : 1.0f;
        float x = (ix - static_cast<float>(contentX)) * sx;
        float y = (iy - static_cast<float>(contentY)) * sy;
        if (x < 0.0f) x = 0.0f;
        if (y < 0.0f) y = 0.0f;
        if (x > static_cast<float>(width)) x = static_cast<float>(width);
        if (y > static_cast<float>(height)) y = static_cast<float>(height);
        *ox = x;
        *oy = y;
    }
};

using FramePtr = std::shared_ptr<Frame>;

#endif  // AI_ENGINE_FRAME_TYPES_HPP
