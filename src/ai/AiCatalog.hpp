#ifndef AI_ENGINE_AI_CATALOG_HPP
#define AI_ENGINE_AI_CATALOG_HPP

// Central registry for AI model types and stage-2 helpers — the one place
// that knows which concrete classes exist.
//
// ────────────────────── To add a new type ──────────────────────
//  * new stage-1 detector : implement Detector in models/, add one line to
//                           createDetector() and detectorTypes()
//  * new stage-2 model    : implement Stage2Model in models/, add one line to
//                           createStage2() and stage2Types()
//  * new helper/transform : implement Transform in transforms/, add one line
//                           to transformList()
// Nothing else in the codebase needs to change — AiJob, the REST validation
// and GET /ai-transforms all read from here.

#include <memory>
#include <string>
#include <vector>

#include "models/Detector.hpp"
#include "models/Stage2Model.hpp"
#include "transforms/Transform.hpp"

#include "models/FaceRecognitionModel.hpp"
#include "models/Yolov8DetectModel.hpp"
#include "models/Yolov8PoseModel.hpp"
#include "models/Yolov8SegModel.hpp"

#include "transforms/FaceAlignTransform.hpp"
#include "transforms/IdentityTransform.hpp"
#include "transforms/PlateAlignTransform.hpp"

namespace ai {

// --- stage-1 detectors -------------------------------------------------------
inline std::unique_ptr<Detector> createDetector(const std::string& type) {
    if (type == "yolov8_detect") return std::unique_ptr<Detector>(new Yolov8DetectModel());
    if (type == "yolov8_pose")   return std::unique_ptr<Detector>(new Yolov8PoseModel());
    if (type == "yolov8_seg")    return std::unique_ptr<Detector>(new Yolov8SegModel());
    return nullptr;
}

inline std::vector<std::string> detectorTypes() {
    return {"yolov8_detect", "yolov8_pose", "yolov8_seg"};
}

// --- stage-2 models ----------------------------------------------------------
inline std::unique_ptr<Stage2Model> createStage2(const std::string& type) {
    if (type == "face_recognition") return std::unique_ptr<Stage2Model>(new FaceRecognitionModel());
    return nullptr;
}

inline std::vector<std::string> stage2Types() {
    return {"face_recognition"};
}

// --- stage-2 helpers (transforms) -------------------------------------------
// Stateless singletons shared by every job worker thread.
inline const std::vector<std::unique_ptr<Transform>>& transformList() {
    static const std::vector<std::unique_ptr<Transform>> list = [] {
        std::vector<std::unique_ptr<Transform>> v;
        v.emplace_back(new IdentityTransform());
        v.emplace_back(new FaceAlignTransform());
        v.emplace_back(new PlateAlignTransform());
        return v;
    }();
    return list;
}

inline Transform* getTransform(const std::string& id) {
    for (const auto& transform : transformList()) {
        if (transform->id() == id) return transform.get();
    }
    return nullptr;
}

inline bool isDetectorType(const std::string& type) {
    return createDetector(type) != nullptr;
}

inline bool isStage2Type(const std::string& type) {
    return createStage2(type) != nullptr;
}

}  // namespace ai

#endif  // AI_ENGINE_AI_CATALOG_HPP
