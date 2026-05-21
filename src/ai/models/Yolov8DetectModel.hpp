#ifndef AI_ENGINE_YOLOV8_DETECT_MODEL_HPP
#define AI_ENGINE_YOLOV8_DETECT_MODEL_HPP

// Stage-1 detector: plain YOLOv8 bounding-box detection.

#include <cstring>

#include "Detector.hpp"
#include "yolov8.h"

class Yolov8DetectModel : public Detector {
public:
    ~Yolov8DetectModel() override { release_yolov8_model(&m_ctx); }

    bool load(const std::string& modelPath) override {
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        return init_yolov8_model(modelPath.c_str(), &m_ctx) == 0;
    }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override {
        return inference_yolov8_model(&m_ctx, &img, &out) == 0;
    }

private:
    rknn_app_context_t m_ctx{};
};

#endif  // AI_ENGINE_YOLOV8_DETECT_MODEL_HPP
