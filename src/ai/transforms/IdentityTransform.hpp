#ifndef AI_ENGINE_IDENTITY_TRANSFORM_HPP
#define AI_ENGINE_IDENTITY_TRANSFORM_HPP

// The default stage-2 helper: crop the detection box straight from the frame
// and resize it to the model-2 input size. No alignment.

#include "RgaConverter.hpp"
#include "Transform.hpp"

class IdentityTransform : public Transform {
public:
    std::string id() const override { return ""; }
    std::string label() const override { return "None"; }
    std::string description() const override {
        return "Crop the detection box directly, no alignment";
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

#endif  // AI_ENGINE_IDENTITY_TRANSFORM_HPP
