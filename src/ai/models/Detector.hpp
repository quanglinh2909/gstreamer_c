#ifndef AI_ENGINE_DETECTOR_HPP
#define AI_ENGINE_DETECTOR_HPP

// Stage-1 model interface: runs on the letterboxed inference frame and
// produces a list of detections.
//
// To add a new detector model type: create a class implementing this
// interface in src/ai/models/, then register it in AiCatalog.hpp. Nothing
// else needs to change.

#include <string>

#include "common.h"       // image_buffer_t
#include "postprocess.h"  // object_detect_result_list

class Detector {
public:
    virtual ~Detector() = default;

    // Loads the .rknn model. Returns false on failure.
    virtual bool load(const std::string& modelPath) = 0;

    // Runs inference on an RGB888 inference image. Returns false on failure.
    virtual bool detect(image_buffer_t& img, object_detect_result_list& out) = 0;
};

#endif  // AI_ENGINE_DETECTOR_HPP
