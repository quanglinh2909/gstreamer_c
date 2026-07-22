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
#include "models/AiModel.hpp"
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
        m_model1 = ai::createModel(m_cfg.model1Type);
        if (!m_model1) {
            std::fprintf(stderr, "[job %s] unknown model_type: %s\n",
                         m_cfg.jobId.c_str(), m_cfg.model1Type.c_str());
            return false;
        }
        if (!m_model1->load(m_cfg.model1Path)) {
            std::fprintf(stderr, "[job %s] failed to load model 1: %s\n",
                         m_cfg.jobId.c_str(), m_cfg.model1Path.c_str());
            return false;
        }

        if (m_cfg.hasModel2()) {
            m_model2 = ai::createModel(m_cfg.model2Type);
            if (!m_model2) {
                std::fprintf(stderr, "[job %s] unknown model_type_2: %s\n",
                             m_cfg.jobId.c_str(), m_cfg.model2Type.c_str());
                return false;
            }
            if (!m_model2->load(m_cfg.model2Path)) {
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
        m_pubThread = std::thread([this] { publishLoop(); });
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        m_queue.close();
        if (m_thread.joinable()) m_thread.join();
        // Close the publish queue only after the worker is done producing;
        // pop() drains whatever is still queued, so no result is lost.
        m_pubQueue.close();
        if (m_pubThread.joinable()) m_pubThread.join();
        m_model1.reset();
        m_model2.reset();
    }

    // Called from the camera pipeline thread; never blocks (drop-old queue).
    void submit(FramePtr frame) { m_queue.push(std::move(frame)); }

    // Keep the debug stream alive for ttlMs more. Called repeatedly (every
    // couple of seconds) by the Python MJPEG viewer over HTTP while a client
    // is watching. Auto-expires so a closed tab / crashed viewer simply stops
    // re-arming and the per-frame debug encode goes idle on its own — no
    // stuck-on CPU. Thread-safe and cheap (one atomic store).
    void armDebug(uint32_t ttlMs) {
        m_debugUntilMs.store(nowMs() + ttlMs, std::memory_order_relaxed);
    }

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
        uint64_t fpsWindowStart = nowMs();
        int processedInWindow = 0;
        unsigned long lastDropped = 0;
        while (m_queue.pop(frame)) {
            if (!frame) continue;
            if (m_cfg.maxFps > 0) {
                const uint64_t now = nowMs();
                const uint64_t minGap = 1000u / static_cast<uint64_t>(m_cfg.maxFps);
                if (now - m_lastProcessMs < minGap) continue;  // drop to cap fps
                m_lastProcessMs = now;
            }
            process(frame);

            // Report how many frames this job actually processes per second,
            // and how many the drop-old queue discarded.
            ++processedInWindow;
            const uint64_t elapsed = nowMs() - fpsWindowStart;
            if (elapsed >= 1000) {
                const unsigned long dropped = m_queue.droppedCount();
                std::fprintf(stderr,
                             "[ai fps] job %s: %d processed/s, %lu dropped/s\n",
                             m_cfg.jobId.c_str(), processedInWindow,
                             dropped - lastDropped);
                lastDropped = dropped;
                processedInWindow = 0;
                fpsWindowStart += elapsed;
            }
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
        if (!m_model1->detect(img, results)) return;

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

            if (m_model2) runStage2(*f, det);

            res.detections.push_back(std::move(det));
        }

        // Encode a JPEG when there are detections (every detection frame, so
        // the Python consumer can cut crops) OR when a debug viewer is
        // currently watching this job. The debug-only encode is rate-capped
        // (~5 fps) so a continuous live debug stream stays cheap, and the
        // arming auto-expires so it costs nothing once the viewer leaves.
        const uint64_t now = nowMs();
        const bool hasDet = !res.detections.empty();
        bool encode = hasDet;
        if (!hasDet && now < m_debugUntilMs.load(std::memory_order_relaxed)) {
            if (now - m_lastDebugMs >= kDebugMinGapMs) {
                m_lastDebugMs = now;
                encode = true;
            }
        }

        // Snapshot keepalive. GET /cameras/{id}/snapshot serves the JPEG
        // cached from this pipeline when it is fresh, and only falls back to
        // opening a NEW RTSP session (handshake + wait for an I-frame, ~1-2s,
        // plus a second concurrent session the camera may refuse) when the
        // cache is stale. Previously nothing was encoded on an empty scene, so
        // that fallback was the *normal* path — snapshots were slow exactly
        // when the view was quiet. One cheap encode per interval keeps the
        // cache permanently warm.
        //
        // Self-limiting: a busy scene already encodes every detection frame
        // and keeps `m_lastEncodeMs` current, so this adds nothing there — it
        // only fires when the camera is idle and the board has spare CPU.
        // Cost is one 1080p libjpeg-turbo encode per kSnapshotWarmMs, on the
        // publish thread, so it never slows the detect loop.
        if (!encode && now - m_lastEncodeMs >= kSnapshotWarmMs) {
            encode = true;
        }
        if (encode) m_lastEncodeMs = now;

        // The JPEG encode is SOFTWARE (libjpeg-turbo; mppjpegenc freezes the
        // board, see JpegEncoder) and costs ~a frame period of CPU at 1080p.
        // Doing it inline here made every detection frame pay detect+encode
        // back to back, dropping the processed rate whenever objects were in
        // view. Instead hand the result to the publish thread: detect on
        // frame N overlaps with encode/serialize/send of frame N-1.
        //
        // Backpressure: if the encoder already has kMaxPendingEncodes frames
        // waiting, publish this result META-ONLY rather than queueing more
        // pictures. The tracker needs uninterrupted metadata far more than it
        // needs another JPEG, and each held frame pins a decoder buffer.
        PublishItem item;
        if (encode &&
            m_pendingEncodes.load(std::memory_order_relaxed) < kMaxPendingEncodes) {
            item.ticket = EncodeTicket(&m_pendingEncodes);
            item.frame = f;
        }
        item.res = std::move(res);
        m_pubQueue.push(std::move(item));
    }

    // Runs on m_pubThread: encodes (when a frame is attached) and hands the
    // result to the sink, strictly in FIFO order so the Python consumer never
    // sees frames out of sequence.
    void publishLoop() {
        PublishItem item;
        while (m_pubQueue.pop(item)) {
            if (item.frame) encodeImages(*item.frame, item.res);
            if (m_sink) m_sink(std::move(item.res));
            item = PublishItem{};  // drop the frame ref (decoder buffer) now
        }
    }

    // Transform the detection crop, then run model 2 on it.
    void runStage2(const Frame& f, Detection& det) {
        TransformContext ctx;
        ctx.frame = &f;
        ctx.det = &det;
        ctx.keypoints = &det.keypoints;
        ctx.targetW = m_model2->inputWidth();
        ctx.targetH = m_model2->inputHeight();
        ctx.tightCrop = m_model2->prefersTightCrop();

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

        m_model2->runStage2(img, det);
    }

    // Encodes only the full frame to JPEG. Per-detection crops are deliberately
    // NOT encoded here — the Python consumer cuts them out of this full frame.
    // Skipping the per-crop RGA + JPEG work keeps it off the hot path.
    void encodeImages(const Frame& f, AiResult& res) {
        // Points into a per-thread staging buffer; valid until the next RGA
        // call on this worker thread — encodeNv12 completes synchronously
        // before that can happen. The encoder takes it BY FD (dmabuf) when
        // available so mppjpegenc never get_user_pages a mapped pointer (that
        // faults the jpege IOMMU and freezes the board).
        rga::PackedNv12 packed = rga::cropNv12ToNv12(f, 0, 0, f.width, f.height);
        if (packed) {
            m_jpeg.encodeNv12(packed.ptr, packed.w, packed.h, res.fullJpeg,
                              packed.fd);
        }
    }

    // RAII share of the pending-encode budget. Counting through a guard —
    // not bare fetch_add/sub at the call sites — means a PublishItem dropped
    // by the queue's overflow path (consumer stalled >2s and items piled up)
    // still gives its slot back; a leaked count would silently degrade the
    // job to meta-only forever.
    struct EncodeTicket {
        std::atomic<int>* counter = nullptr;
        EncodeTicket() = default;
        explicit EncodeTicket(std::atomic<int>* c) : counter(c) {
            if (counter) counter->fetch_add(1, std::memory_order_relaxed);
        }
        EncodeTicket(EncodeTicket&& o) noexcept : counter(o.counter) {
            o.counter = nullptr;
        }
        EncodeTicket& operator=(EncodeTicket&& o) noexcept {
            release();
            counter = o.counter;
            o.counter = nullptr;
            return *this;
        }
        ~EncodeTicket() { release(); }
        void release() {
            if (counter) {
                counter->fetch_sub(1, std::memory_order_relaxed);
                counter = nullptr;
            }
        }
    };

    // One unit of work for the publish thread. `frame` is held ONLY when this
    // result still needs its JPEG encoded — meta-only items keep no frame, so
    // the queue never pins more than kMaxPendingEncodes decoder buffers.
    struct PublishItem {
        FramePtr frame;
        AiResult res;
        EncodeTicket ticket;
    };

    cfg::AiJob m_cfg;
    ResultSink m_sink;

    std::unique_ptr<AiModel> m_model1;  // stage 1: runs on the full frame
    std::unique_ptr<AiModel> m_model2;  // stage 2: runs on each crop (optional)
    Transform* m_transform = nullptr;   // owned by AiCatalog (shared singleton)
    JpegEncoder m_jpeg;                // persistent hardware JPEG pipeline

    BoundedQueue<FramePtr> m_queue{1};
    std::thread m_thread;
    std::atomic<bool> m_running{false};
    uint64_t m_lastProcessMs = 0;

    // Publish lane: encode + serialize + socket send, decoupled from detect.
    // Capacity 8 is a stall backstop (SO_SNDTIMEO lets a dead consumer block
    // sends for up to 2s); in steady state the queue holds at most a couple
    // of items because meta-only entries drain in well under a millisecond.
    BoundedQueue<PublishItem> m_pubQueue{8};
    std::thread m_pubThread;
    std::atomic<int> m_pendingEncodes{0};
    static constexpr int kMaxPendingEncodes = 2;

    // Debug-stream gating (see armDebug). When m_debugUntilMs is in the past,
    // no debug-only frames are encoded — zero cost when nobody is watching.
    std::atomic<uint64_t> m_debugUntilMs{0};
    uint64_t m_lastDebugMs = 0;                       // worker-thread only
    static constexpr uint64_t kDebugMinGapMs = 200;   // ~5 fps debug cap

    // Snapshot cache keepalive (see process()). Must stay comfortably under
    // the freshness window CameraService::getSnapshot asks for (2000ms), or
    // requests landing just before a refresh would still miss the cache and
    // pay the slow RTSP path.
    uint64_t m_lastEncodeMs = 0;                          // worker-thread only
    static constexpr uint64_t kSnapshotWarmMs = 1000;
};

#endif  // AI_ENGINE_AI_JOB_HPP
