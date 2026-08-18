#ifndef AI_ENGINE_PADDLE_OCR_REC_MODEL_HPP
#define AI_ENGINE_PADDLE_OCR_REC_MODEL_HPP

// PaddleOCR text recogniser (PP-OCR "rec"). Reads ONE line of text and returns
// each character as its own detection, left to right.
//
// Why characters instead of one box with a string: every consumer in this
// project — the JSON on the socket, the plate assembler in Python, the overlay
// on the test page — is built around a list of boxes with a class id. Emitting
// per-character boxes means PaddleOCR drops into all of them unchanged; the
// readable text rides along in Detection::text (see labelFor()).
//
// Works as model 1 (whole uploaded image is one text line) and as model 2 (a
// plate/text crop from a detector). It does NOT find text in a scene — that is
// the job of the separate PP-OCR "det" model.
//
// The x of each character comes from its CTC timestep: the head emits seq_len
// steps across the input width, so step t covers [t*W/seq_len, (t+1)*W/seq_len].
// That is a genuine horizontal position, accurate to one step (8 px at 48x320),
// which is all the plate assembler needs to sort characters and split lines.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "AiModel.hpp"
#include "ppocr_rec.h"

class PaddleOcrRecModel : public AiModel {
public:
    ~PaddleOcrRecModel() override { release_ppocr_rec_model(&m_ctx); }

    bool load(const std::string& modelPath) override {
        std::memset(&m_ctx, 0, sizeof(m_ctx));
        if (init_ppocr_rec_model(modelPath.c_str(), &m_ctx) != 0) return false;
        loadDictionary(modelPath);
        return true;
    }

    int inputWidth() const override {
        return m_ctx.model_width > 0 ? m_ctx.model_width : 320;
    }
    int inputHeight() const override {
        return m_ctx.model_height > 0 ? m_ctx.model_height : 48;
    }

    // Stage 2: the crop must be the text itself, no surrounding context.
    bool prefersTightCrop() const override { return true; }

    // Stage 1: cao đúng 48px, giữ nguyên tỉ lệ, dán sát trái, đệm xám bên phải
    // — đúng resize_norm_img của PaddleOCR (nó đệm 0 SAU khi chuẩn hoá về
    // [-1,1], tức là xám 128 ở thang pixel).
    //
    // Đo trên 6 dòng biển số thật (model biển số tự train): kéo đầy khung 0/6
    // đúng, kiểu này 5/6 (conf 0,97). Letterbox canh giữa cũng chỉ 3/6 vì dòng
    // chữ bị co lại và lệch khỏi mép trái model quen thấy.
    FramePrep framePrep() const override { return FramePrep::FitHeight; }

    // The character behind a class id — this is what makes the result readable.
    std::string labelFor(int classId) const override {
        if (classId < 0 || classId >= static_cast<int>(m_dict.size())) return {};
        return m_dict[static_cast<size_t>(classId)];
    }

    bool detect(image_buffer_t& img, object_detect_result_list& out) override {
        ppocr_char_t chars[PPOCR_MAX_CHARS];
        int count = 0;
        int seqLen = 0;
        if (inference_ppocr_rec_model(&m_ctx, &img, chars, &count, &seqLen) != 0) {
            return false;
        }

        std::memset(&out, 0, sizeof(out));
        const int width = inputWidth();
        const int height = inputHeight();
        const float stepW =
            seqLen > 0 ? static_cast<float>(width) / static_cast<float>(seqLen) : 1.0f;

        int kept = 0;
        for (int i = 0; i < count && kept < OBJ_NUMB_MAX_SIZE; ++i) {
            object_detect_result& d = out.results[kept];
            std::memset(&d, 0, sizeof(d));
            const float x = static_cast<float>(chars[i].step) * stepW;
            d.box.left = static_cast<int>(x);
            d.box.right = static_cast<int>(x + stepW);
            if (d.box.right > width) d.box.right = width;
            d.box.top = 0;
            d.box.bottom = height;
            d.prop = chars[i].score;
            d.cls_id = chars[i].index;
            ++kept;
        }
        out.count = kept;
        return true;
    }

private:
    // Dictionary file: "<model>.rknn" -> "<model>.txt", one character per line,
    // in the order the model was trained with. Falls back to ppocr_keys_v1.txt
    // next to the model (the name Rockchip's model zoo ships for v4).
    void loadDictionary(const std::string& modelPath) {
        m_dict.clear();
        std::string sidecar = modelPath;
        const size_t dot = sidecar.find_last_of('.');
        if (dot != std::string::npos) sidecar.resize(dot);
        sidecar += ".txt";

        if (!readDict(sidecar)) {
            const size_t slash = modelPath.find_last_of('/');
            const std::string dir =
                slash == std::string::npos ? std::string(".") : modelPath.substr(0, slash);
            readDict(dir + "/ppocr_keys_v1.txt");
        }
        if (m_dict.empty()) {
            std::fprintf(stderr,
                         "ppocr_rec: KHONG co tu dien (%s) — ket qua chi co "
                         "classId, khong ra chu\n",
                         sidecar.c_str());
        } else {
            // The trailing space class exists only when the model was trained
            // with use_space_char; harmless to append either way because a
            // class id past the end simply yields an empty label.
            m_dict.push_back(" ");
            std::fprintf(stderr, "ppocr_rec: tu dien %zu ky tu\n", m_dict.size());
        }
    }

    bool readDict(const std::string& path) {
        std::ifstream in(path);
        if (!in.is_open()) return false;
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            m_dict.push_back(line);
        }
        return !m_dict.empty();
    }

    rknn_app_context_t m_ctx{};
    std::vector<std::string> m_dict;
};

#endif  // AI_ENGINE_PADDLE_OCR_REC_MODEL_HPP
