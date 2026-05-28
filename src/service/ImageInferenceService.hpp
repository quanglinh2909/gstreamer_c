#ifndef IMAGE_INFERENCE_SERVICE_HPP
#define IMAGE_INFERENCE_SERVICE_HPP

// One-shot HTTP inference. Decodes a JPEG, runs the same stage-1 + optional
// stage-2 pipeline as the live RTSP path, and returns the AiResult serialized
// in the same JSON shape the Python consumer receives.

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include "ai/AiCatalog.hpp"
#include "ai/AiResult.hpp"
#include "ai/FrameTypes.hpp"
#include "ai/RgaConverter.hpp"
#include "ai/ResultPublisher.hpp"
#include "ai/models/AiModel.hpp"
#include "ai/transforms/Transform.hpp"

#include "common.h"
#include "postprocess.h"

struct ImageInferenceRequest {
    std::vector<uint8_t> jpegBytes;
    std::string modelPath;
    std::string modelType;
    std::string modelPath2;
    std::string modelType2;
    std::string transformData;
    float primaryConf = 0.25f;
    float secondaryConf = 0.0f;
};

class ImageInferenceService {
public:
    // Returns JSON string in the same format as RTSP results.
    static std::string run(const ImageInferenceRequest& req) {
        static std::mutex s_mutex;
        std::lock_guard<std::mutex> lock(s_mutex);

        if (req.jpegBytes.empty()) throw std::runtime_error("empty image");
        if (req.modelPath.empty() || req.modelType.empty()) {
            throw std::runtime_error("modelPath and modelType are required");
        }

        cv::Mat bgr = cv::imdecode(req.jpegBytes, cv::IMREAD_COLOR);
        if (bgr.empty()) throw std::runtime_error("JPEG decode failed");

        // RGA requires RGB888 source width stride to be 16-aligned, and NV12
        // chroma is 2x2-subsampled so height must be even. Trim the input so
        // both hold; lose at most 15 px right and 1 px bottom.
        const int origW = bgr.cols & ~15;
        const int origH = bgr.rows & ~1;
        if (origW < 16 || origH < 16) {
            throw std::runtime_error("image too small after alignment");
        }
        if (origW != bgr.cols || origH != bgr.rows) {
            bgr = bgr(cv::Rect(0, 0, origW, origH));
        }

        cv::Mat rgb;
        cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
        if (!rgb.isContinuous()) rgb = rgb.clone();

        std::vector<uint8_t> nv12Buf(static_cast<size_t>(origW) * origH * 3 / 2);
        if (!rga::rgbToNv12(rgb.data, origW, origH, nv12Buf.data())) {
            throw std::runtime_error("RGB->NV12 conversion failed");
        }

        auto model1 = ai::createModel(req.modelType);
        if (!model1) throw std::runtime_error("unknown modelType: " + req.modelType);
        if (!model1->load(req.modelPath)) {
            throw std::runtime_error("failed to load model: " + req.modelPath);
        }

        std::unique_ptr<AiModel> model2;
        Transform* transform = nullptr;
        if (!req.modelPath2.empty() && !req.modelType2.empty()) {
            model2 = ai::createModel(req.modelType2);
            if (!model2) throw std::runtime_error("unknown modelType2: " + req.modelType2);
            if (!model2->load(req.modelPath2)) {
                throw std::runtime_error("failed to load model2: " + req.modelPath2);
            }
            transform = ai::getTransform(req.transformData);
            if (!transform) {
                throw std::runtime_error("unknown transformData: " + req.transformData);
            }
        }

        Frame frame;
        frame.nv12 = nv12Buf.data();
        frame.width = origW;
        frame.height = origH;
        frame.yStride = origW;
        frame.uvStride = origW;
        frame.uvOffset = static_cast<size_t>(origW) * origH;
        frame.inferW = model1->inputWidth();
        frame.inferH = model1->inputHeight();

        if (!rga::letterboxNv12ToRgb(frame, 114)) {
            throw std::runtime_error("letterbox failed");
        }

        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = frame.inferW;
        img.height = frame.inferH;
        img.width_stride = frame.inferW;
        img.height_stride = frame.inferH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = frame.rgb.data();
        img.size = static_cast<int>(frame.rgb.size());
        img.fd = -1;

        object_detect_result_list results;
        std::memset(&results, 0, sizeof(results));
        if (!model1->detect(img, results)) {
            throw std::runtime_error("stage-1 detect failed");
        }

        AiResult res;
        res.origWidth = origW;
        res.origHeight = origH;

        for (int i = 0; i < results.count && i < OBJ_NUMB_MAX_SIZE; ++i) {
            const object_detect_result& d = results.results[i];
            if (d.prop < req.primaryConf) continue;

            Detection det;
            frame.inferToOrig(static_cast<float>(d.box.left),
                              static_cast<float>(d.box.top), &det.x1, &det.y1);
            frame.inferToOrig(static_cast<float>(d.box.right),
                              static_cast<float>(d.box.bottom), &det.x2, &det.y2);
            if (det.x2 <= det.x1 || det.y2 <= det.y1) continue;
            det.score = d.prop;
            det.classId = d.cls_id;

            const int kpCount = d.keypoint_count < AI_POSE_KEYPOINT_NUM
                                    ? d.keypoint_count
                                    : AI_POSE_KEYPOINT_NUM;
            for (int k = 0; k < kpCount; ++k) {
                float ox, oy;
                frame.inferToOrig(d.keypoints[k].x, d.keypoints[k].y, &ox, &oy);
                det.keypoints.push_back(ox);
                det.keypoints.push_back(oy);
                det.keypoints.push_back(d.keypoints[k].score);
            }

            if (model2) {
                bool ok = runStage2(*model2, *transform, frame, det);
                std::fprintf(stderr,
                             "[infer] stage2 transform=%s ok=%d "
                             "embedding=%zu children=%zu\n",
                             transform->id().c_str(), int(ok),
                             det.embedding.size(), det.children.size());
            } else {
                std::fprintf(stderr,
                             "[infer] stage2 SKIPPED (modelPath2='%s' modelType2='%s')\n",
                             req.modelPath2.c_str(), req.modelType2.c_str());
            }

            if (req.secondaryConf > 0.0f && !det.children.empty()) {
                det.children.erase(
                    std::remove_if(det.children.begin(), det.children.end(),
                                   [&](const Detection& c) {
                                       return c.score < req.secondaryConf;
                                   }),
                    det.children.end());
            }

            res.detections.push_back(std::move(det));
        }

        return ResultPublisher::buildJson(res);
    }

private:
    static bool runStage2(AiModel& model2, Transform& transform,
                          const Frame& frame, Detection& det) {
        TransformContext ctx;
        ctx.frame = &frame;
        ctx.det = &det;
        ctx.keypoints = &det.keypoints;
        ctx.targetW = model2.inputWidth();
        ctx.targetH = model2.inputHeight();

        std::vector<uint8_t> stageInput;
        if (!transform.apply(ctx, stageInput)) {
            std::fprintf(stderr, "[infer] transform.apply(%s) returned false\n",
                         transform.id().c_str());
            return false;
        }

        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = ctx.targetW;
        img.height = ctx.targetH;
        img.width_stride = ctx.targetW;
        img.height_stride = ctx.targetH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = stageInput.data();
        img.size = static_cast<int>(stageInput.size());
        img.fd = -1;

        return model2.runStage2(img, det);
    }
};

#endif  // IMAGE_INFERENCE_SERVICE_HPP
