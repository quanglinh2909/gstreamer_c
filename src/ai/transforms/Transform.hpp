#ifndef AI_ENGINE_TRANSFORM_HPP
#define AI_ENGINE_TRANSFORM_HPP

// Stage-2 helper interface: turns one detection into the image fed to the
// stage-2 model (crop, alignment warp, etc).
//
// To add a new helper: implement this interface in src/ai/transforms/ and
// register it in AiCatalog.hpp — it then becomes selectable as
// AiJob.transformData and shows up in GET /ai-transforms automatically.
//
// Implementations must be stateless: one shared instance is used by every
// job worker thread.

#include <string>
#include <vector>

#include "AiResult.hpp"
#include "FrameTypes.hpp"

struct TransformContext {
    const Frame* frame;                   // full-res NV12 source
    const Detection* det;                 // detection box in full-res coords
    const std::vector<float>* keypoints;  // flat (x,y,score) triples, full-res
    int targetW;                          // stage-2 model input width
    int targetH;                          // stage-2 model input height
    // When true the helper crops tight to the detection box (no context
    // padding) so the object fills the model input — what DETR-style models
    // (rf_detect) expect. Set by AiJob from the stage-2 model's preference.
    bool tightCrop = false;
};

class Transform {
public:
    virtual ~Transform() = default;

    // Stable id stored in AiJob.transformData ("" = the identity/crop helper).
    virtual std::string id() const = 0;
    virtual std::string label() const = 0;
    virtual std::string description() const = 0;

    // Produces the stage-2 input as packed RGB888 of size targetW x targetH.
    // Returns false to skip this detection (e.g. missing landmarks).
    virtual bool apply(const TransformContext& ctx, std::vector<uint8_t>& outRgb) = 0;
};

#endif  // AI_ENGINE_TRANSFORM_HPP
