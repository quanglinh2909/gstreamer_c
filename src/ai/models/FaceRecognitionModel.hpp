#ifndef AI_ENGINE_FACE_RECOGNITION_MODEL_HPP
#define AI_ENGINE_FACE_RECOGNITION_MODEL_HPP

// Stage-2 model: face embedding network (e.g. ArcFace / AdaFace). Consumes an
// aligned face crop and writes a feature vector into the detection.

#include <cstring>

#include "Stage2Model.hpp"
#include "face_recognition.h"

class FaceRecognitionModel : public Stage2Model {
public:
    ~FaceRecognitionModel() override { release_face_recognition_model(&m_ctx); }

    bool load(const std::string& modelPath) override {
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        return init_face_recognition_model(modelPath.c_str(), &m_ctx) == 0;
    }

    int inputWidth() const override {
        return m_ctx.model_width > 0 ? m_ctx.model_width : 112;
    }
    int inputHeight() const override {
        return m_ctx.model_height > 0 ? m_ctx.model_height : 112;
    }

    bool run(image_buffer_t& img, Detection& det) override {
        face_embedding_result_t emb;
        std::memset(&emb, 0, sizeof(emb));
        if (inference_face_recognition_model(&m_ctx, &img, &emb) != 0 ||
            !emb.valid) {
            return false;
        }
        det.embedding.assign(emb.embedding, emb.embedding + emb.dim);
        return true;
    }

private:
    rknn_app_context_t m_ctx{};
};

#endif  // AI_ENGINE_FACE_RECOGNITION_MODEL_HPP
