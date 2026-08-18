#ifndef AI_ENGINE_IDENTITY_TRANSFORM_HPP
#define AI_ENGINE_IDENTITY_TRANSFORM_HPP

// The default stage-2 helper: crop the detection box straight from the frame
// and resize it to the model-2 input size. No alignment.

#include <cmath>
#include <cstring>

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

    bool apply(TransformContext& ctx, std::vector<uint8_t>& outRgb) override {
        int x = std::max(0, static_cast<int>(ctx.x1));
        int y = std::max(0, static_cast<int>(ctx.y1));
        int w = static_cast<int>(ctx.x2 - ctx.x1);
        int h = static_cast<int>(ctx.y2 - ctx.y1);
        if (w <= 1 || h <= 1) return false;

        // Model đòi GIỮ TỈ LỆ theo chiều cao (PP-OCR rec): dựng ảnh hẹp hơn
        // khung rồi đệm phần thừa bên phải, thay vì kéo cho đầy.
        //
        // Đây KHÔNG phải chuyện làm đẹp: một dòng biển số 70x16 bị kéo thành
        // 320x48 là méo hơn 2 lần theo chiều ngang, và model đọc ra rác. Đo
        // trên 6 dòng biển thật: kéo đầy 0/6 đúng, giữ tỉ lệ 5/6 đúng (xem
        // FramePrep.hpp).
        if (ctx.prep == FramePrep::FitHeight) {
            return fitHeight(ctx, x, y, w, h, outRgb);
        }
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
            ctx.srcX = static_cast<float>(x);
            ctx.srcY = static_cast<float>(y);
            ctx.srcW = static_cast<float>(w);
            ctx.srcH = static_cast<float>(h);
            return cropNv12ToRgbCpu(nv12, fr.width, fr.height, fr.yStride,
                                    fr.uvOffset, fr.uvStride, x, y, w, h,
                                    ctx.targetW, ctx.targetH, outRgb);
        }
        // expandCropToMin nới hộp ra cho vừa giới hạn của RGA, nên vùng nguồn
        // thật là hộp SAU khi nới — báo lại đúng cái đó.
        rga::expandCropToMin(x, y, w, h, ctx.frame->width, ctx.frame->height);
        ctx.srcX = static_cast<float>(x);
        ctx.srcY = static_cast<float>(y);
        ctx.srcW = static_cast<float>(w);
        ctx.srcH = static_cast<float>(h);
        return rga::cropNv12ToRgb(*ctx.frame, x, y, w, h, ctx.targetW,
                                  ctx.targetH, outRgb);
    }

private:
    // Cắt hộp rồi co theo CHIỀU CAO, canh trái, đệm phần thừa bên phải bằng
    // xám 128 — đúng resize_norm_img của PaddleOCR (nó đệm 0 sau khi chuẩn hoá
    // về [-1,1], tức là xám 128 ở ảnh gốc).
    static bool fitHeight(TransformContext& ctx, int x, int y, int w, int h,
                          std::vector<uint8_t>& outRgb) {
        const int outW = ctx.targetW, outH = ctx.targetH;
        if (outW <= 0 || outH <= 0) return false;

        // Bề rộng sau khi co sao cho chiều cao vừa khít khung.
        int scaledW = static_cast<int>(std::lround(
            static_cast<double>(w) * outH / static_cast<double>(h)));
        if (scaledW < 1) scaledW = 1;
        // Dòng chữ dài hơn tỉ lệ khung thì không đệm được nữa — co cho vừa cả
        // hai chiều, đúng như PaddleOCR làm khi vượt max_wh_ratio.
        if (scaledW > outW) scaledW = outW;

        std::vector<uint8_t> scaled;
        const Frame& fr = *ctx.frame;
        bool ok;
        // Đường CPU: dòng chữ chỉ vài chục pixel mà khung rec cao 48 nên tỉ lệ
        // phóng dễ vượt giới hạn của RGA; cropNv12ToRgbCpu không có giới hạn đó.
        const uint8_t* nv12 = fr.cpuNv12();
        if (nv12) {
            ctx.srcX = static_cast<float>(x);
            ctx.srcY = static_cast<float>(y);
            ctx.srcW = static_cast<float>(w);
            ctx.srcH = static_cast<float>(h);
            ok = cropNv12ToRgbCpu(nv12, fr.width, fr.height, fr.yStride,
                                  fr.uvOffset, fr.uvStride, x, y, w, h,
                                  scaledW, outH, scaled);
        } else {
            rga::expandCropToMin(x, y, w, h, fr.width, fr.height);
            ctx.srcX = static_cast<float>(x);
            ctx.srcY = static_cast<float>(y);
            ctx.srcW = static_cast<float>(w);
            ctx.srcH = static_cast<float>(h);
            ok = rga::cropNv12ToRgb(fr, x, y, w, h, scaledW, outH, scaled);
        }
        if (!ok) return false;

        // Nền xám rồi dán ảnh đã co vào mép trái.
        outRgb.assign(static_cast<size_t>(outW) * outH * 3, 128);
        const size_t rowBytes = static_cast<size_t>(scaledW) * 3;
        for (int row = 0; row < outH; ++row) {
            std::memcpy(outRgb.data() + static_cast<size_t>(row) * outW * 3,
                        scaled.data() + static_cast<size_t>(row) * rowBytes,
                        rowBytes);
        }
        // Ảnh phủ đúng vùng đã cắt, phần đệm không tương ứng pixel nào của
        // khung gốc — bộ chạy quy hộp con về khung gốc bằng srcX..srcH nên chỉ
        // cần chúng đúng, đệm bên phải không làm lệch gì.
        return true;
    }
};

#endif  // AI_ENGINE_IDENTITY_TRANSFORM_HPP
