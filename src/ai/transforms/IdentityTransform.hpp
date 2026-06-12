#ifndef AI_ENGINE_IDENTITY_TRANSFORM_HPP
#define AI_ENGINE_IDENTITY_TRANSFORM_HPP

// The default stage-2 helper: crop the detection box straight from the frame
// and resize it to the model-2 input size. No alignment.

#include "CpuCrop.hpp"
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
        // DETR-style stage-2 models (rf_detect) want the object to fill the
        // input — crop tight to the box (no context padding), matching the
        // reference rfdetr.predict(crop). RGA can't do this: its ~16x scale
        // limit fails when a small crop (e.g. a 30px plate) is blown up to the
        // 512 model input, so route the tight path through the CPU cropper,
        // which has no scale limit. yolov8 keeps the RGA context crop.
        if (ctx.tightCrop) {
            const Frame& fr = *ctx.frame;
            // cpuNv12() maps the buffer on demand — on the dmabuf path the
            // frame is otherwise never CPU-mapped at all.
            const uint8_t* nv12 = fr.cpuNv12();
            if (!nv12) return false;
            return cropNv12ToRgbCpu(nv12, fr.width, fr.height, fr.yStride,
                                    fr.uvOffset, fr.uvStride, x, y, w, h,
                                    ctx.targetW, ctx.targetH, outRgb);
        }
        rga::expandCropToMin(x, y, w, h, ctx.frame->width, ctx.frame->height);
        return rga::cropNv12ToRgb(*ctx.frame, x, y, w, h, ctx.targetW,
                                  ctx.targetH, outRgb);
    }
};

#endif  // AI_ENGINE_IDENTITY_TRANSFORM_HPP
