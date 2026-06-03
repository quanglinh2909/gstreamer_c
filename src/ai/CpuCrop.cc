// CPU NV12 sub-rect crop -> RGB888, resized to the model input. See CpuCrop.hpp
// for why this exists (RGA's scale-ratio limit fails on small-crop -> big-input
// upscales, e.g. a 30px plate -> 512). No scale limit here, so the crop stays
// tight to the object and fills the model input — matching rfdetr.predict(crop).

#include "CpuCrop.hpp"

#include <algorithm>
#include <cstring>

#include <opencv2/opencv.hpp>

bool cropNv12ToRgbCpu(const uint8_t* nv12, int frameW, int frameH,
                      int yStride, std::size_t uvOffset, int uvStride,
                      int x, int y, int w, int h,
                      int dstW, int dstH, std::vector<uint8_t>& out) {
    if (!nv12 || frameW <= 0 || frameH <= 0 || dstW <= 0 || dstH <= 0) {
        return false;
    }
    if (yStride <= 0) yStride = frameW;
    if (uvStride <= 0) uvStride = frameW;
    if (uvOffset == 0) uvOffset = static_cast<std::size_t>(yStride) * frameH;

    // NV12 chroma is 2x2-subsampled, so the crop origin/size must be even.
    x &= ~1;
    y &= ~1;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    w = std::min(w, frameW - x);
    h = std::min(h, frameH - y);
    w &= ~1;
    h &= ~1;
    if (w <= 1 || h <= 1) return false;

    // Wrap the source planes (no copy) and cut the ROI.
    cv::Mat yPlane(frameH, yStride, CV_8UC1, const_cast<uint8_t*>(nv12));
    cv::Mat uvPlane(frameH / 2, uvStride, CV_8UC1,
                    const_cast<uint8_t*>(nv12 + uvOffset));
    cv::Mat yRoi = yPlane(cv::Rect(x, y, w, h));
    cv::Mat uvRoi = uvPlane(cv::Rect(x, y / 2, w, h / 2));

    // Assemble a contiguous NV12 buffer for the crop: Y (h rows) then the
    // interleaved UV (h/2 rows), both w wide — the layout cvtColor expects.
    cv::Mat nv12crop(h + h / 2, w, CV_8UC1);
    yRoi.copyTo(nv12crop(cv::Rect(0, 0, w, h)));
    uvRoi.copyTo(nv12crop(cv::Rect(0, h, w, h / 2)));

    cv::Mat rgb;
    cv::cvtColor(nv12crop, rgb, cv::COLOR_YUV2RGB_NV12);

    cv::Mat resized;
    const bool shrink = (dstW < w) || (dstH < h);
    cv::resize(rgb, resized, cv::Size(dstW, dstH), 0, 0,
               shrink ? cv::INTER_AREA : cv::INTER_CUBIC);

    out.assign(static_cast<size_t>(dstW) * dstH * 3, 0);
    if (resized.isContinuous()) {
        std::memcpy(out.data(), resized.data, out.size());
    } else {
        for (int r = 0; r < dstH; ++r) {
            std::memcpy(out.data() + static_cast<size_t>(r) * dstW * 3,
                        resized.ptr(r), static_cast<size_t>(dstW) * 3);
        }
    }
    return true;
}
