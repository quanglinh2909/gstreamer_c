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
#include "pipeline/AiJob.hpp"
#include "postprocess.h"

class AiManager {
public:
    AiManager() = default;
    ~AiManager() { stop(); }

    AiManager(const AiManager&) = delete;
    AiManager& operator=(const AiManager&) = delete;

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

        if (group.jobs.empty()) return;

        std::vector<AiJob*> jobPtrs;
        jobPtrs.reserve(group.jobs.size());
        for (auto& job : group.jobs) jobPtrs.push_back(job.get());

        group.pipeline.reset(new AiCameraPipeline(
            group.camera, kInferW, kInferH, kPadColor, jobPtrs));

        for (auto& job : group.jobs) job->start();
        group.pipeline->start();

        std::fprintf(stderr, "[ai] camera %s running %zu job(s)\n",
                     group.camera.id.c_str(), group.jobs.size());
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
    static constexpr int kInferW = 640;
    static constexpr int kInferH = 640;
    static constexpr int kPadColor = 114;

    std::mutex m_mutex;
    bool m_started = false;
    std::unique_ptr<ResultPublisher> m_publisher;
    std::map<std::string, CameraGroup> m_groups;

    mutable std::mutex m_cacheMutex;
    std::unordered_map<std::string, CachedJpeg> m_latestJpegs;
};

#endif  // AI_ENGINE_AI_MANAGER_HPP
