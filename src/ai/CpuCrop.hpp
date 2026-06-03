#ifndef AI_ENGINE_CPU_CROP_HPP
#define AI_ENGINE_CPU_CROP_HPP

// CPU crop of an NV12 frame's sub-rect -> packed RGB888 resized to dstW x dstH.
//
// Unlike the RGA path (RgaConverter::cropNv12ToRgb) this has NO scale-ratio
// limit. RGA can only scale within ~[1/16, 16]x and in practice fails earlier
// on the combined crop+colour-convert+scale, so upscaling a tiny crop (a small
// licence plate, ~30px) to a large square model input (512) errors out. This
// is the fallback used by the tight-crop stage-2 path (rf_detect / DETR), where
// the object must fill the input and crops can be far smaller than dst/16.
//
// Implemented in CpuCrop.cc with OpenCV (kept out of this header so the heavy
// OpenCV include does not leak into the widely-included transform headers).

#include <cstddef>
#include <cstdint>
#include <vector>

bool cropNv12ToRgbCpu(const uint8_t* nv12, int frameW, int frameH,
                      int yStride, std::size_t uvOffset, int uvStride,
                      int x, int y, int w, int h,
                      int dstW, int dstH, std::vector<uint8_t>& out);

#endif  // AI_ENGINE_CPU_CROP_HPP
