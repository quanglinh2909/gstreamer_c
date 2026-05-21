#ifndef AI_ENGINE_AI_CAMERA_PIPELINE_HPP
#define AI_ENGINE_AI_CAMERA_PIPELINE_HPP

// In-process GStreamer pipeline that feeds the AI jobs of one camera.
//
//   rtspsrc ! decodebin ! NV12 ! appsink
//
// Decode and the NV12->RGB letterbox happen exactly once per frame; the
// resulting Frame is shared (shared_ptr, ref-counted) to every AI job of the
// camera. Jobs only ever read the Frame, so the fan-out costs nothing beyond
// a refcount — no extra decode, no extra colour-convert, no extra allocation
// per job.

#include <atomic>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>

#include "AiJob.hpp"
#include "Config.hpp"
#include "FrameTypes.hpp"
#include "RgaConverter.hpp"

class AiCameraPipeline {
public:
    AiCameraPipeline(cfg::Camera camera, int inferW, int inferH, int padColor,
                     std::vector<AiJob*> jobs)
        : m_camera(std::move(camera)),
          m_inferW(inferW),
          m_inferH(inferH),
          m_padColor(padColor),
          m_jobs(std::move(jobs)) {}

    ~AiCameraPipeline() { stop(); }

    AiCameraPipeline(const AiCameraPipeline&) = delete;
    AiCameraPipeline& operator=(const AiCameraPipeline&) = delete;

    void start() {
        if (m_jobs.empty()) return;
        if (m_running.exchange(true)) return;
        m_thread = std::thread([this] { run(); });
    }

    void stop() {
        if (!m_running.exchange(false)) return;
        if (m_loop) g_main_loop_quit(m_loop);
        if (m_thread.joinable()) m_thread.join();
    }

private:
    void run() {
        GMainContext* ctx = g_main_context_new();
        g_main_context_push_thread_default(ctx);
        m_loop = g_main_loop_new(ctx, FALSE);

        buildAndStart();
        g_main_loop_run(m_loop);

        teardown();
        g_main_loop_unref(m_loop);
        m_loop = nullptr;
        g_main_context_pop_thread_default(ctx);
        g_main_context_unref(ctx);
    }

    void buildAndStart() {
        GError* err = nullptr;
        m_pipeline = gst_parse_launch(
            "rtspsrc name=src latency=200 protocols=tcp drop-on-latency=true ! "
            "decodebin ! video/x-raw,format=NV12 ! "
            "appsink name=sink sync=false max-buffers=2 drop=true",
            &err);
        if (!m_pipeline) {
            std::fprintf(stderr, "[ai cam %s] pipeline build failed: %s\n",
                         m_camera.id.c_str(), err ? err->message : "unknown");
            if (err) g_error_free(err);
            return;
        }
        if (err) g_error_free(err);

        GstElement* src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
        if (src) {
            g_object_set(src, "location", m_camera.uri.c_str(), nullptr);
            gst_object_unref(src);
        }

        GstElement* sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
        if (sink) {
            GstAppSinkCallbacks cbs;
            std::memset(&cbs, 0, sizeof(cbs));
            cbs.new_sample = &AiCameraPipeline::onNewSampleThunk;
            gst_app_sink_set_callbacks(GST_APP_SINK(sink), &cbs, this, nullptr);
            gst_object_unref(sink);
        }

        GstBus* bus = gst_element_get_bus(m_pipeline);
        gst_bus_add_watch(bus, &AiCameraPipeline::onBusThunk, this);
        gst_object_unref(bus);

        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "[ai cam %s] failed to start pipeline\n",
                         m_camera.id.c_str());
            scheduleReconnect();
        }
    }

    void teardown() {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
    }

    void scheduleReconnect() {
        if (!m_running.load()) return;
        teardown();
        g_timeout_add(2000, &AiCameraPipeline::onReconnectThunk, this);
    }

    static gboolean onReconnectThunk(gpointer user) {
        auto* self = static_cast<AiCameraPipeline*>(user);
        if (self->m_running.load()) {
            std::fprintf(stderr, "[ai cam %s] reconnecting...\n",
                         self->m_camera.id.c_str());
            self->buildAndStart();
        }
        return G_SOURCE_REMOVE;
    }

    static gboolean onBusThunk(GstBus*, GstMessage* msg, gpointer user) {
        auto* self = static_cast<AiCameraPipeline*>(user);
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR ||
            GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS) {
            if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                GError* e = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &e, &dbg);
                std::fprintf(stderr, "[ai cam %s] error: %s\n",
                             self->m_camera.id.c_str(),
                             e && e->message ? e->message : "unknown");
                if (e) g_error_free(e);
                if (dbg) g_free(dbg);
            }
            self->scheduleReconnect();
        }
        return TRUE;
    }

    static GstFlowReturn onNewSampleThunk(GstAppSink* sink, gpointer user) {
        return static_cast<AiCameraPipeline*>(user)->onNewSample(sink);
    }

    GstFlowReturn onNewSample(GstAppSink* sink) {
        GstSample* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_OK;

        // Decode + letterbox once; fan the shared Frame out to every job.
        FramePtr frame = buildFrame(sample);
        if (frame) {
            for (AiJob* job : m_jobs) job->submit(frame);
        }
        return GST_FLOW_OK;
    }

    // Wraps a decoded NV12 sample in a Frame and RGA-letterboxes it once.
    // Ownership of the sample moves into the Frame. Returns nullptr on failure.
    FramePtr buildFrame(GstSample* sample) {
        GstBuffer* buf = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buf || !caps) {
            gst_sample_unref(sample);
            return nullptr;
        }

        GstVideoInfo vinfo;
        if (!gst_video_info_from_caps(&vinfo, caps)) {
            gst_sample_unref(sample);
            return nullptr;
        }

        auto frame = std::make_shared<Frame>();
        frame->sample = sample;  // ownership moves into Frame
        if (!gst_buffer_map(buf, &frame->map, GST_MAP_READ)) {
            return nullptr;  // Frame dtor unrefs the sample
        }
        frame->mapped = true;
        frame->nv12 = frame->map.data;
        frame->width = GST_VIDEO_INFO_WIDTH(&vinfo);
        frame->height = GST_VIDEO_INFO_HEIGHT(&vinfo);
        frame->yStride = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 0);
        frame->uvStride = GST_VIDEO_INFO_PLANE_STRIDE(&vinfo, 1);
        frame->uvOffset = GST_VIDEO_INFO_PLANE_OFFSET(&vinfo, 1);

        GstVideoMeta* meta = gst_buffer_get_video_meta(buf);
        if (meta) {
            frame->yStride = static_cast<int>(meta->stride[0]);
            frame->uvStride = static_cast<int>(meta->stride[1]);
            frame->uvOffset = meta->offset[1];
        }

        frame->inferW = m_inferW;
        frame->inferH = m_inferH;
        frame->ptsUs = GST_BUFFER_PTS(buf) != GST_CLOCK_TIME_NONE
                           ? static_cast<int64_t>(GST_BUFFER_PTS(buf) / 1000)
                           : 0;
        frame->seq = ++m_seq;

        if (!rga::letterboxNv12ToRgb(*frame, m_padColor)) {
            return nullptr;  // Frame dtor cleans up
        }
        return frame;
    }

    cfg::Camera m_camera;
    int m_inferW;
    int m_inferH;
    int m_padColor;
    std::vector<AiJob*> m_jobs;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    GMainLoop* m_loop = nullptr;
    GstElement* m_pipeline = nullptr;
    std::atomic<uint64_t> m_seq{0};
};

#endif  // AI_ENGINE_AI_CAMERA_PIPELINE_HPP
