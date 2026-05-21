#ifndef AI_ENGINE_STAGE2_MODEL_HPP
#define AI_ENGINE_STAGE2_MODEL_HPP

// Stage-2 model interface: runs on a transformed crop and enriches the
// detection (e.g. writes a face embedding).
//
// To add a new stage-2 model type: implement this interface in src/ai/models/
// and register it in AiCatalog.hpp.

#include <string>

#include "AiResult.hpp"
#include "common.h"  // image_buffer_t

class Stage2Model {
public:
    virtual ~Stage2Model() = default;

    // Loads the .rknn model. Returns false on failure.
    virtual bool load(const std::string& modelPath) = 0;

    // Model input size — the transform must produce a crop of exactly this
    // size for run().
    virtual int inputWidth() const = 0;
    virtual int inputHeight() const = 0;

    // Runs inference on the transformed RGB888 image and writes the outcome
    // into `det`. Returns false on failure.
    virtual bool run(image_buffer_t& img, Detection& det) = 0;
};

#endif  // AI_ENGINE_STAGE2_MODEL_HPP
