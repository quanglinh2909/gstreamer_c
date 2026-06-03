// RF-DETR hybrid detector — see RfDetectModel.hpp.
//
// Pipeline per frame (all in C++, no Python):
//   1. take the letterboxed RGB888 inference frame (e.g. 640x640) the AiJob
//      hands every detector,
//   2. resize it isotropically to the backbone input (e.g. 384x384) and
//      ImageNet-normalize into a float NHWC buffer (matches the reference
//      pipeline_common.preprocess_image / run_backbone.cpp),
//   3. run backbone.rknn on the NPU -> feature map(s),
//   4. feed the feature map(s) into the detection head .onnx on the CPU via
//      ONNX Runtime -> dets[1,Q,4] (cxcywh, normalized) + logits[1,Q,C],
//   5. postprocess (sigmoid -> top-K -> cxcywh->xyxy, scaled to the inference
//      frame) into an object_detect_result_list — the SAME output struct the
//      yolov8 models fill, so AiJob and everything downstream is unchanged.

#include "models/RfDetectModel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <opencv2/opencv.hpp>

#include "rknn_api.h"
#include "common.h"
#include "postprocess.h"  // object_detect_result_list, OBJ_NUMB_MAX_SIZE

#include "onnxruntime_cxx_api.h"

namespace {

// ImageNet normalization — the RKNN backbone carries no baked-in mean/std, so
// we apply it in software exactly like the reference preprocess.
constexpr float kMean[3] = {0.485f, 0.456f, 0.406f};
constexpr float kStd[3] = {0.229f, 0.224f, 0.225f};

// Postprocess constants — match verify_rknn.py defaults.
constexpr int kNumSelect = 300;     // top-K (query,class) pairs before threshold
constexpr float kMinScore = 0.05f;  // floor; AiJob's primaryConf does the real filtering

inline float sigmoid(float x) { return 1.0f / (1.0f + std::exp(-x)); }

// ".../rf_detr_n.rknn" -> ".../rf_detr_n_detect.onnx"
std::string deriveDetectorOnnx(const std::string& rknnPath) {
    const std::string suffix = ".rknn";
    if (rknnPath.size() >= suffix.size() &&
        rknnPath.compare(rknnPath.size() - suffix.size(), suffix.size(),
                         suffix) == 0) {
        return rknnPath.substr(0, rknnPath.size() - suffix.size()) +
               "_detect.onnx";
    }
    return rknnPath + "_detect.onnx";
}

}  // namespace

struct RfDetectModel::Impl {
    // --- backbone (NPU) ---
    rknn_context rknn = 0;
    int inW = 384;
    int inH = 384;
    uint32_t nOut = 0;
    std::vector<std::vector<int64_t>> outShapes;  // per backbone output, want_float shape

    // --- detection head (CPU / ONNX Runtime) ---
    Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "rf_detect"};
    std::unique_ptr<Ort::Session> session;
    std::vector<std::string> inNames;
    std::vector<std::string> outNames;

    ~Impl() {
        session.reset();
        if (rknn) rknn_destroy(rknn);
    }
};

RfDetectModel::RfDetectModel() : m_impl(std::make_unique<Impl>()) {}
RfDetectModel::~RfDetectModel() = default;

int RfDetectModel::inputWidth() const { return m_impl->inW; }
int RfDetectModel::inputHeight() const { return m_impl->inH; }

bool RfDetectModel::load(const std::string& modelPath) {
    Impl& s = *m_impl;

    // ---- 1. backbone .rknn ----
    std::vector<uint8_t> model;
    {
        FILE* fp = std::fopen(modelPath.c_str(), "rb");
        if (!fp) {
            std::fprintf(stderr, "[rf_detect] cannot open backbone: %s\n",
                         modelPath.c_str());
            return false;
        }
        std::fseek(fp, 0, SEEK_END);
        long sz = std::ftell(fp);
        std::fseek(fp, 0, SEEK_SET);
        if (sz <= 0) {
            std::fclose(fp);
            return false;
        }
        model.resize(static_cast<size_t>(sz));
        size_t rd = std::fread(model.data(), 1, model.size(), fp);
        std::fclose(fp);
        if (rd != model.size()) return false;
    }

    int ret = rknn_init(&s.rknn, model.data(),
                        static_cast<uint32_t>(model.size()), 0, nullptr);
    if (ret < 0) {
        std::fprintf(stderr, "[rf_detect] rknn_init failed: %d\n", ret);
        return false;
    }

    rknn_input_output_num io{};
    if (rknn_query(s.rknn, RKNN_QUERY_IN_OUT_NUM, &io, sizeof(io)) != RKNN_SUCC) {
        std::fprintf(stderr, "[rf_detect] rknn_query io num failed\n");
        return false;
    }
    s.nOut = io.n_output;

    rknn_tensor_attr inAttr{};
    inAttr.index = 0;
    if (rknn_query(s.rknn, RKNN_QUERY_INPUT_ATTR, &inAttr, sizeof(inAttr)) !=
        RKNN_SUCC) {
        std::fprintf(stderr, "[rf_detect] rknn_query input attr failed\n");
        return false;
    }
    if (inAttr.fmt == RKNN_TENSOR_NCHW) {  // [N,C,H,W]
        s.inH = static_cast<int>(inAttr.dims[2]);
        s.inW = static_cast<int>(inAttr.dims[3]);
    } else {  // NHWC: [N,H,W,C]
        s.inH = static_cast<int>(inAttr.dims[1]);
        s.inW = static_cast<int>(inAttr.dims[2]);
    }

    // Record each backbone output's shape (as the want_float runtime reports
    // it) so we can wrap the dequantized buffers as ONNX input tensors.
    s.outShapes.resize(s.nOut);
    for (uint32_t i = 0; i < s.nOut; ++i) {
        rknn_tensor_attr a{};
        a.index = i;
        if (rknn_query(s.rknn, RKNN_QUERY_OUTPUT_ATTR, &a, sizeof(a)) !=
            RKNN_SUCC) {
            std::fprintf(stderr, "[rf_detect] rknn_query output attr %u failed\n",
                         i);
            return false;
        }
        std::vector<int64_t>& shape = s.outShapes[i];
        shape.clear();
        for (uint32_t d = 0; d < a.n_dims; ++d) {
            shape.push_back(static_cast<int64_t>(a.dims[d]));
        }
        if (shape.empty()) shape.push_back(1);
    }

    // ---- 2. detection head .onnx (CPU) ----
    const std::string onnxPath = deriveDetectorOnnx(modelPath);
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(2);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        s.session = std::make_unique<Ort::Session>(s.env, onnxPath.c_str(), opts);

        Ort::AllocatorWithDefaultOptions alloc;
        const size_t nIn = s.session->GetInputCount();
        const size_t nOut = s.session->GetOutputCount();
        for (size_t i = 0; i < nIn; ++i) {
            s.inNames.push_back(s.session->GetInputNameAllocated(i, alloc).get());
        }
        for (size_t i = 0; i < nOut; ++i) {
            s.outNames.push_back(
                s.session->GetOutputNameAllocated(i, alloc).get());
        }
        if (s.inNames.empty() || s.outNames.size() < 2) {
            std::fprintf(stderr,
                         "[rf_detect] detector onnx has unexpected IO: in=%zu "
                         "out=%zu\n",
                         s.inNames.size(), s.outNames.size());
            return false;
        }
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[rf_detect] failed to load detector onnx %s: %s\n",
                     onnxPath.c_str(), e.what());
        return false;
    }

    std::fprintf(stderr,
                 "[rf_detect] loaded backbone=%s (%dx%d, %u outputs) head=%s\n",
                 modelPath.c_str(), s.inW, s.inH, s.nOut, onnxPath.c_str());
    return true;
}

bool RfDetectModel::detect(image_buffer_t& img, object_detect_result_list& out) {
    Impl& s = *m_impl;
    std::memset(&out, 0, sizeof(out));
    if (!s.rknn || !s.session) return false;
    if (!img.virt_addr || img.width <= 0 || img.height <= 0) return false;

    // ---- preprocess: RGB888 (inference frame) -> float NHWC, ImageNet-norm ----
    cv::Mat src(img.height, img.width, CV_8UC3, img.virt_addr);  // already RGB
    cv::Mat resized;
    const bool shrinking = (s.inW < img.width) || (s.inH < img.height);
    cv::resize(src, resized, cv::Size(s.inW, s.inH), 0, 0,
               shrinking ? cv::INTER_AREA : cv::INTER_CUBIC);

    std::vector<float> inBuf(static_cast<size_t>(s.inW) * s.inH * 3);
    size_t idx = 0;
    for (int y = 0; y < s.inH; ++y) {
        const uint8_t* row = resized.ptr<uint8_t>(y);
        for (int x = 0; x < s.inW; ++x) {
            for (int c = 0; c < 3; ++c) {
                const float v = row[x * 3 + c] / 255.0f;
                inBuf[idx++] = (v - kMean[c]) / kStd[c];
            }
        }
    }

    // ---- backbone on NPU ----
    rknn_input rin{};
    rin.index = 0;
    rin.type = RKNN_TENSOR_FLOAT32;
    rin.fmt = RKNN_TENSOR_NHWC;  // runtime converts to the model's NCHW layout
    rin.size = static_cast<uint32_t>(inBuf.size() * sizeof(float));
    rin.buf = inBuf.data();
    rin.pass_through = 0;
    if (rknn_inputs_set(s.rknn, 1, &rin) < 0) {
        std::fprintf(stderr, "[rf_detect] rknn_inputs_set failed\n");
        return false;
    }
    if (rknn_run(s.rknn, nullptr) < 0) {
        std::fprintf(stderr, "[rf_detect] rknn_run failed\n");
        return false;
    }

    std::vector<rknn_output> rout(s.nOut);
    std::memset(rout.data(), 0, rout.size() * sizeof(rknn_output));
    for (uint32_t i = 0; i < s.nOut; ++i) {
        rout[i].index = i;
        rout[i].want_float = 1;  // dequantize to float feature map
    }
    if (rknn_outputs_get(s.rknn, s.nOut, rout.data(), nullptr) < 0) {
        std::fprintf(stderr, "[rf_detect] rknn_outputs_get failed\n");
        return false;
    }

    // ---- detection head on CPU: wrap the backbone feature buffers as ONNX
    // input tensors (no copy), run, read dets + logits. The rknn buffers must
    // stay alive until Run() returns, so release them only afterwards. ----
    bool ok = true;
    try {
        Ort::MemoryInfo mem =
            Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

        const size_t nFeed = std::min<size_t>(s.inNames.size(), s.nOut);
        std::vector<Ort::Value> inputs;
        inputs.reserve(nFeed);
        for (size_t i = 0; i < nFeed; ++i) {
            const std::vector<int64_t>& shape = s.outShapes[i];
            size_t count = 1;
            for (int64_t d : shape) count *= static_cast<size_t>(d <= 0 ? 1 : d);
            inputs.push_back(Ort::Value::CreateTensor<float>(
                mem, static_cast<float*>(rout[i].buf), count, shape.data(),
                shape.size()));
        }

        std::vector<const char*> inNames, outNames;
        for (const auto& n : s.inNames) inNames.push_back(n.c_str());
        for (const auto& n : s.outNames) outNames.push_back(n.c_str());

        auto results = s.session->Run(Ort::RunOptions{nullptr}, inNames.data(),
                                      inputs.data(), inputs.size(),
                                      outNames.data(), outNames.size());

        // Identify outputs by trailing dim: 4 -> boxes (cxcywh), else logits.
        const float* dets = nullptr;
        const float* logits = nullptr;
        int numQueries = 0, numClasses = 0;
        for (auto& v : results) {
            auto shape = v.GetTensorTypeAndShapeInfo().GetShape();
            if (shape.empty()) continue;
            const int64_t last = shape.back();
            if (last == 4) {
                dets = v.GetTensorData<float>();
            } else {
                logits = v.GetTensorData<float>();
                numClasses = static_cast<int>(last);
                numQueries = shape.size() >= 2
                                 ? static_cast<int>(shape[shape.size() - 2])
                                 : 0;
            }
        }

        if (dets && logits && numQueries > 0 && numClasses > 0) {
            // top-K over all (query,class) sigmoid scores, like postprocess_onnx.
            struct Cand {
                float score;
                int query;
                int cls;
            };
            std::vector<Cand> cands;
            cands.reserve(static_cast<size_t>(numQueries) * numClasses);
            for (int q = 0; q < numQueries; ++q) {
                const float* lq = logits + static_cast<size_t>(q) * numClasses;
                for (int c = 0; c < numClasses; ++c) {
                    cands.push_back({sigmoid(lq[c]), q, c});
                }
            }
            const int k = std::min<int>(kNumSelect,
                                        static_cast<int>(cands.size()));
            std::partial_sort(
                cands.begin(), cands.begin() + k, cands.end(),
                [](const Cand& a, const Cand& b) { return a.score > b.score; });

            const float fW = static_cast<float>(img.width);
            const float fH = static_cast<float>(img.height);
            int n = 0;
            for (int i = 0; i < k && n < OBJ_NUMB_MAX_SIZE; ++i) {
                const Cand& cd = cands[i];
                if (cd.score < kMinScore) break;  // sorted desc -> rest are lower
                const float* b = dets + static_cast<size_t>(cd.query) * 4;
                const float cx = b[0], cy = b[1];
                const float w = std::max(0.0f, b[2]);
                const float h = std::max(0.0f, b[3]);
                float x1 = (cx - 0.5f * w) * fW;
                float y1 = (cy - 0.5f * h) * fH;
                float x2 = (cx + 0.5f * w) * fW;
                float y2 = (cy + 0.5f * h) * fH;
                x1 = std::clamp(x1, 0.0f, fW);
                y1 = std::clamp(y1, 0.0f, fH);
                x2 = std::clamp(x2, 0.0f, fW);
                y2 = std::clamp(y2, 0.0f, fH);
                if (x2 <= x1 || y2 <= y1) continue;

                object_detect_result& r = out.results[n++];
                r.box.left = static_cast<int>(x1 + 0.5f);
                r.box.top = static_cast<int>(y1 + 0.5f);
                r.box.right = static_cast<int>(x2 + 0.5f);
                r.box.bottom = static_cast<int>(y2 + 0.5f);
                r.prop = cd.score;
                r.cls_id = cd.cls;
                r.keypoint_count = 0;
            }
            out.count = n;
        }
    } catch (const Ort::Exception& e) {
        std::fprintf(stderr, "[rf_detect] detector run failed: %s\n", e.what());
        ok = false;
    }

    rknn_outputs_release(s.rknn, s.nOut, rout.data());
    return ok;
}
