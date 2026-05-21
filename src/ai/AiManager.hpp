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

#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <string>
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

private:
    struct CameraGroup {
        cfg::Camera camera;
        std::vector<cfg::AiJob> jobConfigs;        // desired set
        std::vector<std::unique_ptr<AiJob>> jobs;  // live workers
        std::unique_ptr<AiCameraPipeline> pipeline;
    };

    static void stopGroupRuntime(CameraGroup& group) {
        if (group.pipeline) {
            group.pipeline->stop();
            group.pipeline.reset();
        }
        for (auto& job : group.jobs) job->stop();
        group.jobs.clear();
    }

    void teardownLocked(CameraGroup& group) { stopGroupRuntime(group); }

    // Stops the camera's runtime, then recreates jobs + pipeline from the
    // current desired job set. Called with m_mutex held.
    void rebuildLocked(CameraGroup& group) {
        stopGroupRuntime(group);

        for (const cfg::AiJob& jc : group.jobConfigs) {
            if (!jc.enabled) continue;
            auto job = std::unique_ptr<AiJob>(new AiJob(
                jc, [this](AiResult r) {
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

    static constexpr const char* kSocketPath = "/tmp/ai_engine.sock";
    static constexpr int kInferW = 640;
    static constexpr int kInferH = 640;
    static constexpr int kPadColor = 114;

    std::mutex m_mutex;
    bool m_started = false;
    std::unique_ptr<ResultPublisher> m_publisher;
    std::map<std::string, CameraGroup> m_groups;
};

#endif  // AI_ENGINE_AI_MANAGER_HPP
