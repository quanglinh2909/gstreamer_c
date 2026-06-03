#ifndef AI_ENGINE_AI_MODEL_HPP
#define AI_ENGINE_AI_MODEL_HPP

// Unified AI model interface — stage-agnostic.
//
// A pipeline job has a model 1 (runs on the full frame) and an optional
// model 2 (runs on each cropped detection). Both stages take an AiModel, so
// any registered model type may be picked as model 1 OR model 2.
//
// A model only has to implement the role(s) that make sense for it:
//   * detect()     — stage-1 role: produce a list of boxes from a frame.
//   * runStage2()  — stage-2 role: enrich the parent detection from a crop.
// The base class defaults the other role, so a model selected for a stage it
// does not implement simply produces nothing instead of crashing.
//
// To add a new model type: implement this interface in src/ai/models/ and
// register it in AiCatalog.hpp.

#include <cstring>
#include <string>
#include <utility>

#include "AiResult.hpp"
#include "common.h"       // image_buffer_t
#include "postprocess.h"  // object_detect_result_list, OBJ_NUMB_MAX_SIZE

// Converts a detector's raw output (in crop-pixel coordinates) into child
// detections attached to the parent. Used by the default stage-2 role so any
// detector model works as model 2 — e.g. an OCR detector on a plate crop.
inline void appendStage2Detections(const object_detect_result_list& results,
                                   Detection& parent) {
    for (int i = 0; i < results.count && i < OBJ_NUMB_MAX_SIZE; ++i) {
        const object_detect_result& d = results.results[i];
        Detection child;
        child.x1 = static_cast<float>(d.box.left);
        child.y1 = static_cast<float>(d.box.top);
        child.x2 = static_cast<float>(d.box.right);
        child.y2 = static_cast<float>(d.box.bottom);
        child.score = d.prop;
        child.classId = d.cls_id;
        const int kpCount = d.keypoint_count < AI_POSE_KEYPOINT_NUM
                                ? d.keypoint_count
                                : AI_POSE_KEYPOINT_NUM;
        for (int k = 0; k < kpCount; ++k) {
            child.keypoints.push_back(d.keypoints[k].x);
            child.keypoints.push_back(d.keypoints[k].y);
            child.keypoints.push_back(d.keypoints[k].score);
        }
        parent.children.push_back(std::move(child));
    }
}

class AiModel {
public:
    virtual ~AiModel() = default;

    // Loads the .rknn model. Returns false on failure.
    virtual bool load(const std::string& modelPath) = 0;

    // Model input size. When this model runs as stage 2 the transform must
    // produce a crop of exactly this size.
    virtual int inputWidth() const = 0;
    virtual int inputHeight() const = 0;

    // When this model runs as stage 2, whether the crop should be tight to the
    // detection box (object fills the input) rather than expanded with context.
    // DETR-style detectors (rf_detect) are trained on a plain resize-to-square
    // of the tight object, so they override this. yolov8 wants context → false.
    virtual bool prefersTightCrop() const { return false; }

    // Stage-1 role: run on the (letterboxed) inference frame and produce a
    // list of detections. Default: this model is not a detector.
    virtual bool detect(image_buffer_t& /*img*/,
                        object_detect_result_list& /*out*/) {
        return false;
    }

    // Stage-2 role: run on a transformed crop and enrich `parent`. The default
    // runs detect() on the crop and attaches the boxes as child detections, so
    // every detector model works as model 2 unchanged. Models that enrich the
    // parent differently (e.g. a face-embedding network) override this.
    virtual bool runStage2(image_buffer_t& img, Detection& parent) {
        object_detect_result_list results;
        std::memset(&results, 0, sizeof(results));
        if (!detect(img, results)) return false;
        appendStage2Detections(results, parent);
        return true;
    }
};

#endif  // AI_ENGINE_AI_MODEL_HPP
