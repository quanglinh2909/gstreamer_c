#ifndef AI_ENGINE_AI_CATALOG_HPP
#define AI_ENGINE_AI_CATALOG_HPP

// Central registry for AI model types and stage-2 helpers — the one place
// that knows which concrete classes exist.
//
// Models are stage-agnostic: every registered model type may be picked as
// model 1 (runs on the full frame) or model 2 (runs on each crop). A model
// only acts on the role it implements — see AiModel — so e.g. face_recognition
// picked as model 1 simply yields no detections.
//
// ────────────────────── To add a new type ──────────────────────
//  * new model     : implement AiModel in models/, add one line to
//                     createModel() and modelTypes()
//  * new transform : implement Transform in transforms/, add one line to
//                     transformList()
// Nothing else in the codebase needs to change — AiJob, the REST validation
// and the GET /ai-* endpoints all read from here.

#include <memory>
#include <string>
#include <vector>

#include "models/AiModel.hpp"
#include "transforms/Transform.hpp"

#include "models/FaceRecognitionModel.hpp"
#include "models/RfDetectModel.hpp"
#include "models/Yolov8DetectModel.hpp"
#include "models/Yolov8PoseModel.hpp"
#include "models/Yolov8SegModel.hpp"

#include "transforms/FaceAlignTransform.hpp"
#include "transforms/IdentityTransform.hpp"
#include "transforms/PlateAlignTransform.hpp"

namespace ai {

// --- models ------------------------------------------------------------------
// One factory for both stages: the returned model can be used as model 1 or
// model 2 — AiModel handles whichever role the job assigns it.
inline std::unique_ptr<AiModel> createModel(const std::string& type) {
    if (type == "yolov8_detect")    return std::unique_ptr<AiModel>(new Yolov8DetectModel());
    if (type == "yolov8_pose")      return std::unique_ptr<AiModel>(new Yolov8PoseModel());
    if (type == "yolov8_seg")       return std::unique_ptr<AiModel>(new Yolov8SegModel());
    if (type == "rf_detect")        return std::unique_ptr<AiModel>(new RfDetectModel());
    if (type == "face_recognition") return std::unique_ptr<AiModel>(new FaceRecognitionModel());
    return nullptr;
}

inline std::vector<std::string> modelTypes() {
    return {"yolov8_detect", "yolov8_pose", "yolov8_seg", "rf_detect", "face_recognition"};
}

inline bool isModelType(const std::string& type) {
    return createModel(type) != nullptr;
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

}  // namespace ai

#endif  // AI_ENGINE_AI_CATALOG_HPP
