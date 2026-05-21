#ifndef AI_ENGINE_PLATE_ALIGN_TRANSFORM_HPP
#define AI_ENGINE_PLATE_ALIGN_TRANSFORM_HPP

// Stage-2 helper: license-plate alignment before OCR.
//
// TODO: a proper implementation rectifies the plate quad from the YOLOv8-seg
// mask. That needs the segmentation mask in the TransformContext; until then
// this falls back to a plain box crop, which is already enough to exercise
// the cascade end to end. The quad-warp algorithm is available in the
// reference project (test/final/ai/src/plate_align.cc).

#include "RgaConverter.hpp"
#include "Transform.hpp"

class PlateAlignTransform : public Transform {
public:
    std::string id() const override { return "align_plate"; }
    std::string label() const override { return "Plate alignment"; }
    std::string description() const override {
        return "Rectify the license plate before OCR (currently a plain crop)";
    }

    bool apply(const TransformContext& ctx, std::vector<uint8_t>& outRgb) override {
        const Detection& d = *ctx.det;
        int x = std::max(0, static_cast<int>(d.x1));
        int y = std::max(0, static_cast<int>(d.y1));
        int w = static_cast<int>(d.x2 - d.x1);
        int h = static_cast<int>(d.y2 - d.y1);
        if (w <= 1 || h <= 1) return false;
        rga::expandCropToMin(x, y, w, h, ctx.frame->width, ctx.frame->height);
        return rga::cropNv12ToRgb(*ctx.frame, x, y, w, h, ctx.targetW,
                                  ctx.targetH, outRgb);
    }
};

#endif  // AI_ENGINE_PLATE_ALIGN_TRANSFORM_HPP
