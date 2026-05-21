#ifndef AI_ENGINE_AI_JOB_HPP
#define AI_ENGINE_AI_JOB_HPP

// One AI job: a worker thread that pulls the latest frame of its camera from
// a drop-old queue, runs the stage-1 detector, optionally cascades each kept
// detection through a transform + a stage-2 model, then emits an AiResult.
//
// The detector, stage-2 model and transform are all resolved from AiCatalog —
// AiJob itself has no per-model-type or per-transform branching.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AiCatalog.hpp"
#include "AiResult.hpp"
#include "Config.hpp"
#include "FrameQueue.hpp"
#include "FrameTypes.hpp"
#include "JpegEncoder.hpp"
#include "RgaConverter.hpp"
#include "models/Detector.hpp"
#include "models/Stage2Model.hpp"
#include "transforms/Transform.hpp"

#include "common.h"
#include "postprocess.h"

class AiJob {
public:
    using ResultSink = std::function<void(AiResult)>;

    AiJob(cfg::AiJob config, ResultSink sink)
        : m_cfg(std::move(config)), m_sink(std::move(sink)) {}

    ~AiJob() { stop(); }

    AiJob(const AiJob&) = delete;
    AiJob& operator=(const AiJob&) = delete;

    // Resolves and loads the models/transform. Call before start().
    bool init() {
        m_detector = ai::createDetector(m_cfg.model1Type);
        if (!m_detector) {
            std::fprintf(stderr, "[job %s] unknown model_type: %s\n",
                         m_cfg.jobId.c_str(), m_cfg.model1Type.c_str());
            return false;
        }
        if (!m_detector->load(m_cfg.model1Path)) {
            std::fprintf(stderr, "[job %s] failed to load model 1: %s\n",
                         m_cfg.jobId.c_str(), m_cfg.model1Path.c_str());
            return false;
        }

        if (m_cfg.hasModel2()) {
            m_stage2 = ai::createStage2(m_cfg.model2Type);
            if (!m_stage2) {
                std::fprintf(stderr, "[job %s] unknown model_type_2: %s\n",
                             m_cfg.jobId.c_str(), m_cfg.model2Type.c_str());
                return false;
            }
            if (!m_stage2->load(m_cfg.model2Path)) {
                std::fprintf(stderr, "[job %s] failed to load model 2: %s\n",
                             m_cfg.jobId.c_str(), m_cfg.model2Path.c_str());
                return false;
            }
            m_transform = ai::getTransform(m_cfg.transform);
            if (!m_transform) {
                std::fprintf(stderr, "[job %s] unknown transform: %s\n",
                             m_cfg.jobId.c_str(), m_cfg.transform.c_str());
                return false;
            }
        }
        return true;
    }

    void start() {
        if (m_running.exchange(true)) return;
        m_thread = std::thread([this] { run(); });
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        m_queue.close();
        if (m_thread.joinable()) m_thread.join();
        m_detector.reset();
        m_stage2.reset();
    }

    // Called from the camera pipeline thread; never blocks (drop-old queue).
    void submit(FramePtr frame) { m_queue.push(std::move(frame)); }

    const std::string& cameraId() const { return m_cfg.cameraId; }
    const std::string& jobId() const { return m_cfg.jobId; }

private:
    static uint64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void run() {
        FramePtr frame;
        while (m_queue.pop(frame)) {
            if (!frame) continue;
            if (m_cfg.maxFps > 0) {
                const uint64_t now = nowMs();
                const uint64_t minGap = 1000u / static_cast<uint64_t>(m_cfg.maxFps);
                if (now - m_lastProcessMs < minGap) continue;  // drop to cap fps
                m_lastProcessMs = now;
            }
            process(frame);
        }
    }

    void process(const FramePtr& f) {
        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = f->inferW;
        img.height = f->inferH;
        img.width_stride = f->inferW;
        img.height_stride = f->inferH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = f->rgb.data();
        img.size = static_cast<int>(f->rgb.size());
        img.fd = -1;

        object_detect_result_list results;
        std::memset(&results, 0, sizeof(results));
        if (!m_detector->detect(img, results)) return;

        AiResult res;
        res.cameraId = m_cfg.cameraId;
        res.jobId = m_cfg.jobId;
        res.seq = f->seq;
        res.tsUs = f->ptsUs;
        res.origWidth = f->width;
        res.origHeight = f->height;

        for (int i = 0; i < results.count && i < OBJ_NUMB_MAX_SIZE; ++i) {
            const object_detect_result& d = results.results[i];
            if (!m_cfg.classFilter.empty() &&
                m_cfg.classFilter.count(d.cls_id) == 0) {
                continue;
            }
            if (d.prop < m_cfg.primaryConf) continue;

            Detection det;
            f->inferToOrig(static_cast<float>(d.box.left),
                           static_cast<float>(d.box.top), &det.x1, &det.y1);
            f->inferToOrig(static_cast<float>(d.box.right),
                           static_cast<float>(d.box.bottom), &det.x2, &det.y2);
            if (det.x2 <= det.x1 || det.y2 <= det.y1) continue;
            det.score = d.prop;
            det.classId = d.cls_id;

            const int kpCount =
                d.keypoint_count < AI_POSE_KEYPOINT_NUM ? d.keypoint_count
                                                        : AI_POSE_KEYPOINT_NUM;
            for (int k = 0; k < kpCount; ++k) {
                float ox, oy;
                f->inferToOrig(d.keypoints[k].x, d.keypoints[k].y, &ox, &oy);
                det.keypoints.push_back(ox);
                det.keypoints.push_back(oy);
                det.keypoints.push_back(d.keypoints[k].score);
            }

            if (m_stage2) runStage2(*f, det);

            res.detections.push_back(std::move(det));
        }

        if (!res.detections.empty()) encodeImages(*f, res);
        if (m_sink) m_sink(std::move(res));
    }

    // Transform the detection crop, then run the stage-2 model on it.
    void runStage2(const Frame& f, Detection& det) {
        TransformContext ctx;
        ctx.frame = &f;
        ctx.det = &det;
        ctx.keypoints = &det.keypoints;
        ctx.targetW = m_stage2->inputWidth();
        ctx.targetH = m_stage2->inputHeight();

        std::vector<uint8_t> stageInput;
        if (!m_transform->apply(ctx, stageInput)) return;

        image_buffer_t img;
        std::memset(&img, 0, sizeof(img));
        img.width = ctx.targetW;
        img.height = ctx.targetH;
        img.width_stride = ctx.targetW;
        img.height_stride = ctx.targetH;
        img.format = IMAGE_FORMAT_RGB888;
        img.virt_addr = stageInput.data();
        img.size = static_cast<int>(stageInput.size());
        img.fd = -1;

        m_stage2->run(img, det);
    }

    void encodeImages(const Frame& f, AiResult& res) {
        std::vector<uint8_t> packed;
        int pw = 0, ph = 0;
        if (rga::cropNv12ToNv12(f, 0, 0, f.width, f.height, packed, pw, ph)) {
            jpeg::encodeNv12(packed.data(), pw, ph, res.fullJpeg);
        }

        for (auto& det : res.detections) {
            int cx = std::max(0, static_cast<int>(det.x1));
            int cy = std::max(0, static_cast<int>(det.y1));
            int cw = static_cast<int>(det.x2) - cx;
            int ch = static_cast<int>(det.y2) - cy;
            if (cw <= 1 || ch <= 1) continue;
            // Pad small detections out so RGA can crop them (RGA rejects tiny
            // regions); a saved crop with some context is fine.
            rga::expandCropToMin(cx, cy, cw, ch, f.width, f.height);
            std::vector<uint8_t> cropNv12;
            int ow = 0, oh = 0;
            if (!rga::cropNv12ToNv12(f, cx, cy, cw, ch, cropNv12, ow, oh)) {
                continue;
            }
            std::vector<uint8_t> jpegBytes;
            if (jpeg::encodeNv12(cropNv12.data(), ow, oh, jpegBytes)) {
                res.cropJpegs.push_back(std::move(jpegBytes));
                det.cropJpegIndex = static_cast<int>(res.cropJpegs.size()) - 1;
            }
        }
    }

    cfg::AiJob m_cfg;
    ResultSink m_sink;

    std::unique_ptr<Detector> m_detector;
    std::unique_ptr<Stage2Model> m_stage2;
    Transform* m_transform = nullptr;  // owned by AiCatalog (shared singleton)

    BoundedQueue<FramePtr> m_queue{1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    uint64_t m_lastProcessMs = 0;
};

#endif  // AI_ENGINE_AI_JOB_HPP
