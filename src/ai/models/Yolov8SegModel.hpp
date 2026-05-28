#ifndef AI_ENGINE_YOLOV8_SEG_MODEL_HPP
#define AI_ENGINE_YOLOV8_SEG_MODEL_HPP

// YOLOv8 segmentation — boxes plus a segmentation mask. Usable as model 1 or,
// via the inherited default runStage2(), as model 2.

#include <cstring>

#include "AiModel.hpp"
#include "yolov8.h"
#include "yolov8_seg.h"

class Yolov8SegModel : public AiModel {
public:
    ~Yolov8SegModel() override { release_yolov8_seg_model(&m_ctx); }

    bool load(const std::string& modelPath) override {
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        return init_yolov8_seg_model(modelPath.c_str(), &m_ctx) == 0;
    }

    int inputWidth() const override {
        return m_ctx.model_width > 0 ? m_ctx.model_width : 640;
    }
    int inputHeight() const override {
        return m_ctx.model_height > 0 ? m_ctx.model_height : 640;
    }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override {
        return inference_yolov8_seg_model(&m_ctx, &img, &out) == 0;
    }

private:
    rknn_app_context_t m_ctx{};
};

#endif  // AI_ENGINE_YOLOV8_SEG_MODEL_HPP
