#ifndef AI_ENGINE_AI_MANAGER_HPP
#define AI_ENGINE_AI_MANAGER_HPP

// In-process AI subsystem, driven entirely by the database (no config file).
//
// AiJobService reads the ai_jobs / cameras tables and calls applyJob /
// removeJob; this class keeps one tee-fed GStreamer pipeline per camera and
// rebuilds just that camera's pipeline when its job set changes. Job workers
// publish results to a Python consumer over a Unix socket.
//
// All public methods are thread-safe (HTTP handler threads call applyJob /
// removeJob concurrently with each other and with startup loading).

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "AiResult.hpp"
#include "Config.hpp"
#include "ResultPublisher.hpp"
#include "pipeline/AiCameraPipeline.hpp"
#include "JpegEncoder.hpp"
#include "RgaConverter.hpp"
#include "pipeline/MotionDetector.hpp"
#include "pipeline/AiJob.hpp"
#include "postprocess.h"
#include "service/CameraSourceRegistry.hpp"

class AiManager {
public:
    AiManager() = default;
    ~AiManager() { stop(); }

    AiManager(const AiManager&) = delete;
    AiManager& operator=(const AiManager&) = delete;

    // Sổ nguồn RTP dùng chung (của GStreamerService). Cho phép pipeline AI BÁM
    // vào kết nối ghi hình / xem live sẵn có thay vì tự mở kết nối RTSP thứ hai.
    // Gọi một lần lúc dựng component (xem AiComponent). Không đặt (nullptr) thì
    // AI tự mở rtspsrc như cũ — hành vi cũ được giữ nguyên làm fallback.
    void setSourceRegistry(std::shared_ptr<stream::CameraSourceRegistry> sources) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_sources = std::move(sources);
    }

    // Nơi nhận ô đã động của MỖI khung được phân tích: (cameraId, "r:c,r:c").
    // Gọi cả khi chuỗi rỗng — bên nhận cần tin "khung này không có gì" để đóng
    // sự kiện đang mở. Đặt một lần lúc dựng component (App.cpp), truyền bằng
    // std::function để AiManager KHÔNG phải include GStreamerService (hai
    // chiều include nhau là vòng).
    void setMotionSink(std::function<void(const std::string&, const std::string&)> sink) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_motionSink = std::move(sink);
    }

    // Bật/tắt dò chuyển động cho một camera. Gọi từ GStreamerService mỗi khi
    // camera khởi động / đổi cấu hình / dừng.
    //
    // Camera KHÔNG có job AI nào vẫn dựng pipeline nếu bật chuyển động: lúc đó
    // pipeline chỉ để giải mã + RGA, và đó vẫn rẻ hơn nhiều so với nhánh
    // GStreamer riêng trước đây (đo được ~25% -> vài %).
    void setCameraMotion(const cfg::Camera& camera, bool enabled,
                         uint32_t gridX, uint32_t gridY) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;

        auto found = m_groups.find(camera.id);
        if (!enabled) {
            if (found == m_groups.end() || !found->second.motionEnabled) return;
            found->second.motionEnabled = false;
            // Không còn job lẫn chuyển động -> bỏ hẳn nhóm, đừng giữ một
            // pipeline giải mã không ai dùng.
            if (found->second.jobConfigs.empty()) {
                teardownLocked(found->second);
                m_groups.erase(found);
                return;
            }
            rebuildLocked(found->second);
            return;
        }

        CameraGroup& group = m_groups[camera.id];
        const bool unchanged = group.motionEnabled &&
                               group.motionGridX == gridX &&
                               group.motionGridY == gridY &&
                               group.camera.uri == camera.uri;
        group.camera = camera;
        group.motionEnabled = true;
        group.motionGridX = gridX;
        group.motionGridY = gridY;
        // Dựng lại pipeline là ngắt luồng vài giây; chỉ làm khi thực sự đổi.
        if (unchanged) return;
        rebuildLocked(group);
    }

    /**
     * JPEG của khung MỚI NHẤT mà bộ dò chuyển động của camera này đã xem.
     * Rỗng khi camera không bật chuyển động hoặc chưa có khung nào.
     *
     * Mã hoá NGAY LÚC GỌI (tức lúc một sự kiện bắt đầu) chứ không encode đều
     * đặn rồi cất sẵn: sự kiện là chuyện thi thoảng, còn encode 1 khung/giây
     * liên tục đã đo được 5,6% CPU mỗi camera — gần như toàn bộ đổ đi.
     */
    std::vector<uint8_t> grabMotionJpeg(const std::string& cameraId) {
        FramePtr frame;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_groups.find(cameraId);
            if (found == m_groups.end() || !found->second.motion) return {};
            frame = found->second.motion->latestFrame();
        }
        if (!frame || frame->rgb.empty()) return {};

        // JpegEncoder nhận NV12; khung của bộ dò là RGB888 ở cỡ inference.
        const int w = frame->inferW;
        const int h = frame->inferH;
        if (w <= 0 || h <= 0) return {};
        std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2);
        if (!rga::rgbToNv12(frame->rgb.data(), w, h, nv12.data())) return {};

        std::vector<uint8_t> jpeg;
        std::lock_guard<std::mutex> lock(m_motionJpegMutex);
        if (!m_motionJpeg.encodeNv12(nv12.data(), w, h, jpeg)) return {};
        return jpeg;
    }

    // Initialises postprocessing and the result publisher. Idempotent.
    // Returns false (AI stays disabled) if the publisher socket fails.
    bool start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started) return true;

        init_post_process();
        m_publisher.reset(new ResultPublisher(kSocketPath));
        if (!m_publisher->start()) {
            std::fprintf(stderr, "[ai] disabled: result publisher failed\n");
            m_publisher.reset();
            deinit_post_process();
            return false;
        }
        m_started = true;
        std::fprintf(stderr, "[ai] started, socket %s\n", kSocketPath);
        return true;
    }

    void stop() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        m_started = false;
        for (auto& entry : m_groups) teardownLocked(entry.second);
        m_groups.clear();
        if (m_publisher) {
            m_publisher->stop();
            m_publisher.reset();
        }
        deinit_post_process();
    }

    // Adds or updates one job and rebuilds its camera's pipeline. A job with
    // enabled == false is kept in the desired set but not run.
    void applyJob(const cfg::Camera& camera, const cfg::AiJob& job) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;

        CameraGroup& group = m_groups[camera.id];
        group.camera = camera;

        bool replaced = false;
        for (auto& jc : group.jobConfigs) {
            if (jc.jobId == job.jobId) {
                jc = job;
                replaced = true;
                break;
            }
        }
        if (!replaced) group.jobConfigs.push_back(job);

        rebuildLocked(group);
    }

    // Returns the most recent full-frame JPEG captured by any AI job
    // running on this camera, but only if it is younger than maxAgeMs.
    // Empty vector when AI is not running for the camera or the latest
    // frame is too stale. Thread-safe; intended for HTTP handlers that
    // want a "free" snapshot piggybacking on the in-process AI pipeline
    // instead of opening a fresh RTSP connection (avoids the second
    // concurrent RTSP session that some cameras refuse with a 502).
    std::vector<uint8_t> getLatestJpeg(const std::string& cameraId,
                                       uint32_t maxAgeMs = 2000) const {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto it = m_latestJpegs.find(cameraId);
        if (it == m_latestJpegs.end() || it->second.jpeg.empty()) {
            return {};
        }
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - it->second.timestamp).count();
        if (static_cast<uint32_t>(age) > maxAgeMs) return {};
        return it->second.jpeg;  // copy
    }

    // Keeps the live debug stream of one job (by id) armed for ttlMs. Driven
    // by the Python MJPEG viewer over HTTP while a client watches; the flag
    // auto-expires so it costs nothing once nobody is looking. Searches every
    // camera group because the HTTP caller only knows the job id. No-op when
    // the job isn't live.
    void armJobDebug(const std::string& jobId, uint32_t ttlMs) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;
        for (auto& entry : m_groups) {
            for (auto& job : entry.second.jobs) {
                if (job && job->jobId() == jobId) {
                    job->armDebug(ttlMs);
                    return;
                }
            }
        }
    }

    // Removes one job and rebuilds (or drops) its camera's pipeline.
    void removeJob(const std::string& cameraId, const std::string& jobId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;

        auto found = m_groups.find(cameraId);
        if (found == m_groups.end()) return;

        CameraGroup& group = found->second;
        for (auto it = group.jobConfigs.begin(); it != group.jobConfigs.end();) {
            if (it->jobId == jobId) {
                it = group.jobConfigs.erase(it);
            } else {
                ++it;
            }
        }

        if (group.jobConfigs.empty()) {
            teardownLocked(group);
            m_groups.erase(found);
        } else {
            rebuildLocked(group);
        }
    }

    // Stops and drops a camera's entire AI runtime — every job worker and its
    // ingest pipeline. Called when the camera itself is deleted; the ai_jobs
    // rows are removed from the database by an ON DELETE CASCADE. No-op if the
    // camera has no AI jobs.
    void removeCamera(const std::string& cameraId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_started) return;

        auto found = m_groups.find(cameraId);
        if (found == m_groups.end()) return;

        teardownLocked(found->second);
        m_groups.erase(found);
    }

private:
    struct CameraGroup {
        cfg::Camera camera;
        std::vector<cfg::AiJob> jobConfigs;        // desired set
        std::vector<std::unique_ptr<AiJob>> jobs;  // live workers
        std::unique_ptr<AiCameraPipeline> pipeline;

        // Chuyển động chạy như một người tiêu thụ khung nữa của cùng pipeline.
        bool motionEnabled = false;
        uint32_t motionGridX = 32;
        uint32_t motionGridY = 32;
        std::unique_ptr<MotionDetector> motion;
    };

    static void stopGroupRuntime(CameraGroup& group) {
        // Stop job workers FIRST: a worker may be mid-RGA crop on a frame whose
        // pixels are backed by the decoder's buffer pool. Tearing the pipeline
        // down first would free that pool under the worker (RGA then blits
        // freed memory -> "RGA_BLIT Invalid argument"). job->stop() joins the
        // worker, so once it returns no frame is in use; then stop the pipeline.
        for (auto& job : group.jobs) job->stop();
        group.jobs.clear();
        if (group.pipeline) {
            group.pipeline->stop();
            group.pipeline.reset();
        }
        // SAU pipeline: nó gọi thẳng vào detector trên thread appsink, huỷ
        // trước khi pipeline dừng là gọi vào vùng nhớ đã giải phóng.
        group.motion.reset();
    }

    void teardownLocked(CameraGroup& group) { stopGroupRuntime(group); }

    // Stops the camera's runtime, then recreates jobs + pipeline from the
    // current desired job set. Called with m_mutex held.
    void rebuildLocked(CameraGroup& group) {
        stopGroupRuntime(group);

        for (const cfg::AiJob& jc : group.jobConfigs) {
            if (!jc.enabled) continue;
            const std::string cameraId = group.camera.id;
            auto job = std::unique_ptr<AiJob>(new AiJob(
                jc, [this, cameraId](AiResult r) {
                    // Stash the latest encoded full frame before the
                    // result is published, so the snapshot endpoint can
                    // serve it without opening another RTSP session.
                    if (!r.fullJpeg.empty()) {
                        cacheLatestJpeg(cameraId, r.fullJpeg);
                    }
                    if (m_publisher) m_publisher->publish(r);
                }));
            if (!job->init()) {
                std::fprintf(stderr, "[ai] job %s init failed, skipped\n",
                             jc.jobId.c_str());
                continue;
            }
            group.jobs.push_back(std::move(job));
        }

        // KHÔNG còn "không job thì thôi": camera chỉ bật chuyển động vẫn cần
        // pipeline này để có khung đã giải mã + đã qua RGA.
        if (group.jobs.empty() && !group.motionEnabled) return;

        std::vector<AiJob*> jobPtrs;
        jobPtrs.reserve(group.jobs.size());
        for (auto& job : group.jobs) jobPtrs.push_back(job.get());

        // Lookup nguồn chung theo camera, gọi ở MỖI lần (dựng lại) pipeline nên
        // reconnect tự bám lại nguồn hiện tại (nguồn chung mới sau restart, hoặc
        // rtspsrc nếu recording tắt). Bắt registry theo giá trị để lambda sống
        // độc lập với vòng đời group.
        auto sources = m_sources;
        const std::string camId = group.camera.id;
        std::function<std::shared_ptr<stream::FrameSource>()> lookup;
        if (sources) {
            lookup = [sources, camId]() -> std::shared_ptr<stream::FrameSource> {
                return sources->lookup(camId);
            };
        }

        std::function<void(const FramePtr&)> motionSink;
        if (group.motionEnabled) {
            auto sink = m_motionSink;
            const std::string cid = group.camera.id;
            group.motion.reset(new MotionDetector(
                cid, group.motionGridX, group.motionGridY,
                [sink, cid](const std::string& cells) {
                    if (sink) sink(cid, cells);
                }));
            MotionDetector* detector = group.motion.get();
            motionSink = [detector](const FramePtr& frame) { detector->submit(frame); };
        }

        // Khung cho dò chuyển động khi không job nào tới hạn: mượn spec của job
        // đầu tiên để khỏi phát sinh thêm một cỡ khung nữa; camera chỉ bật
        // chuyển động thì dùng mặc định của FrameSpec.
        FrameSpec motionSpec;
        if (!group.jobs.empty()) motionSpec = group.jobs.front()->frameSpec();

        group.pipeline.reset(new AiCameraPipeline(
            group.camera, motionSpec, jobPtrs, std::move(lookup),
            std::move(motionSink)));

        for (auto& job : group.jobs) job->start();
        group.pipeline->start();

        std::fprintf(stderr, "[ai] camera %s running %zu job(s)%s\n",
                     group.camera.id.c_str(), group.jobs.size(),
                     group.motionEnabled ? " + chuyen dong" : "");
    }

    struct CachedJpeg {
        std::vector<uint8_t> jpeg;
        std::chrono::steady_clock::time_point timestamp;
    };

    // Called from every AI job's worker thread on every result frame.
    // Separate mutex from m_mutex so snapshot reads (HTTP handlers) and
    // applyJob/removeJob (HTTP handlers too) do not block AI workers.
    void cacheLatestJpeg(const std::string& cameraId,
                         const std::vector<uint8_t>& jpeg) {
        std::lock_guard<std::mutex> lock(m_cacheMutex);
        auto& slot = m_latestJpegs[cameraId];
        slot.jpeg = jpeg;  // copy; ~100KB
        slot.timestamp = std::chrono::steady_clock::now();
    }

    static constexpr const char* kSocketPath = "/tmp/ai_engine.sock";

    std::mutex m_mutex;
    bool m_started = false;
    std::unique_ptr<ResultPublisher> m_publisher;
    std::map<std::string, CameraGroup> m_groups;
    // Sổ nguồn RTP dùng chung để pipeline AI bám kết nối sẵn có (xem
    // setSourceRegistry). nullptr => AI tự mở rtspsrc như cũ.
    std::shared_ptr<stream::CameraSourceRegistry> m_sources;
    // (cameraId, "r:c,r:c") -> CameraRecordingSession. Xem setMotionSink.
    std::function<void(const std::string&, const std::string&)> m_motionSink;
    // Bộ mã hoá JPEG dùng riêng cho ảnh sự kiện chuyển động (khoá riêng để
    // không chặn m_mutex trong lúc encode).
    std::mutex m_motionJpegMutex;
    JpegEncoder m_motionJpeg;

    mutable std::mutex m_cacheMutex;
    std::unordered_map<std::string, CachedJpeg> m_latestJpegs;
};

#endif  // AI_ENGINE_AI_MANAGER_HPP
