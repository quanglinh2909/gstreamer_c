#ifndef AI_ENGINE_FACE_ALIGN_TRANSFORM_HPP
#define AI_ENGINE_FACE_ALIGN_TRANSFORM_HPP

// Stage-2 helper: 5-point facial-landmark alignment. Crops the face from the
// full-res frame and warps it to the canonical pose expected by the face
// embedding model (ArcFace / AdaFace).

#include <algorithm>
#include <cstdint>
#include <vector>

#include "RgaConverter.hpp"
#include "Transform.hpp"
#include "face_align.h"

class FaceAlignTransform : public Transform {
public:
    std::string id() const override { return "align_face"; }
    std::string label() const override { return "Face alignment"; }
    std::string description() const override {
        return "Align the face by 5 landmarks before the embedding model";
    }

    bool apply(const TransformContext& ctx, std::vector<uint8_t>& outRgb) override {
        // 5 landmarks, each stored as an (x, y, score) triple.
        if (!ctx.keypoints || ctx.keypoints->size() < 15) return false;

        const Frame& f = *ctx.frame;
        const Detection& d = *ctx.det;
        int cx = std::max(0, static_cast<int>(d.x1));
        int cy = std::max(0, static_cast<int>(d.y1));
        int cw = static_cast<int>(d.x2) - cx;
        int ch = static_cast<int>(d.y2) - cy;
        if (cw <= 1 || ch <= 1) return false;
        // Pad small face boxes out — RGA rejects tiny crops; the alignment
        // warp absorbs the extra margin since keypoints stay crop-relative.
        rga::expandCropToMin(cx, cy, cw, ch, f.width, f.height);

        std::vector<uint8_t> crop;
        if (!rga::cropNv12ToRgb(f, cx, cy, cw, ch, cw, ch, crop)) return false;

        FacePoint2f kp[5];
        for (int k = 0; k < 5; ++k) {
            kp[k].x = (*ctx.keypoints)[k * 3 + 0] - static_cast<float>(cx);
            kp[k].y = (*ctx.keypoints)[k * 3 + 1] - static_cast<float>(cy);
        }

        outRgb.assign(static_cast<size_t>(ctx.targetW) * ctx.targetH * 3, 0);
        return align_face_u8c3(crop.data(), cw, ch, cw * 3, kp, outRgb.data(),
                               ctx.targetW, ctx.targetH, ctx.targetW * 3);
    }
};

#endif  // AI_ENGINE_FACE_ALIGN_TRANSFORM_HPP
