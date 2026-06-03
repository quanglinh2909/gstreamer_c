#ifndef AI_ENGINE_RF_DETECT_MODEL_HPP
#define AI_ENGINE_RF_DETECT_MODEL_HPP

// RF-DETR object detection — a hybrid stage-1 detector.
//
// RF-DETR is a transformer detector that does NOT convert to a single RKNN
// graph: only its CNN backbone maps to the NPU; the transformer head must run
// on the CPU. So this model runs as two stages internally:
//   1. backbone (.rknn) on the NPU  -> a feature map  [1,256,24,24]
//   2. detection head (.onnx) on the CPU via ONNX Runtime -> boxes + logits
//
// Model paths follow a convention: the job's model1Path points at the backbone
// .rknn file, and the detector .onnx is found beside it with the same stem and
// a "_detect.onnx" suffix, e.g.
//   .../rf_detr_n.rknn   ->   .../rf_detr_n_detect.onnx
//
// The implementation is in RfDetectModel.cc (kept out of this header so the
// heavy RKNN / ONNX Runtime / OpenCV includes do not leak into AiCatalog).

#include <memory>
#include <string>

#include "AiModel.hpp"

class RfDetectModel : public AiModel {
public:
    RfDetectModel();
    ~RfDetectModel() override;

    RfDetectModel(const RfDetectModel&) = delete;
    RfDetectModel& operator=(const RfDetectModel&) = delete;

    bool load(const std::string& modelPath) override;

    int inputWidth() const override;
    int inputHeight() const override;

    // RF-DETR is trained on a plain resize-to-square of the tight object crop,
    // so as a stage-2 model it wants a tight crop (no context padding).
    bool prefersTightCrop() const override { return true; }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

#endif  // AI_ENGINE_RF_DETECT_MODEL_HPP
