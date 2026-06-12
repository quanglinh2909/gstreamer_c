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
#include <gst/allocators/gstdmabuf.h>
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
        // application/x-rtp,media=video filter drops any audio RTP stream
        // the camera also emits — without it, decodebin tries to handle
        // every dynamic pad rtspsrc exposes, and a single failed audio
        // negotiation tears the whole pipeline down with the famously
        // unhelpful "Internal data stream error".
        m_pipeline = gst_parse_launch(
            "rtspsrc name=src latency=200 protocols=tcp drop-on-latency=true ! "
            "application/x-rtp,media=video ! "
            "decodebin ! "
            "appsink name=sink sync=false max-buffers=2 drop=true "
            "enable-last-sample=false",
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
            // Prefer dmabuf-backed NV12 so the decoder frame goes to RGA by
            // fd (see RgaConverter.hpp for why the mapped-pointer route can
            // oops the kernel). Plain NV12 stays as the fallback for
            // software decoders and for AI_RGA_LEGACY=1.
            GstCaps* caps = gst_caps_from_string(
                !rga::legacyMode()
                    ? "video/x-raw(memory:DMABuf),format=NV12; "
                      "video/x-raw,format=NV12"
                    : "video/x-raw,format=NV12");
            gst_app_sink_set_caps(GST_APP_SINK(sink), caps);
            gst_caps_unref(caps);

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

    // Mark the current attempt as healthy. The bus watch flips us back
    // to PLAYING once it sees a buffer flow successfully; until then we
    // assume the connect is still tentative and keep the backoff state.
    void noteHealthy() { m_reconnectAttempts = 0; }

    void teardown() {
        if (m_pipeline) {
            // The bus watch added in buildAndStart() holds a ref on the bus
            // and stays attached to the context until explicitly removed —
            // without this, every reconnect leaked a watch (and a dead
            // pipeline's bus could still dispatch into onBusThunk).
            GstBus* bus = gst_element_get_bus(m_pipeline);
            if (bus) {
                gst_bus_remove_watch(bus);
                gst_object_unref(bus);
            }
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
    }

    void scheduleReconnect() {
        if (!m_running.load()) return;
        teardown();
        // Exponential backoff capped at 30s. Without it a camera that's
        // permanently down (wrong URL, credential change, hardware off)
        // triggers a connect attempt every 2s — floods logs, hammers the
        // camera's RTSP port and burns CPU on pipeline build/teardown.
        // Reset to 2s once a fresh frame proves the link is healthy.
        const guint delays_ms[] = {2000, 5000, 10000, 20000, 30000};
        const size_t idx = m_reconnectAttempts < sizeof(delays_ms) / sizeof(delays_ms[0])
                               ? m_reconnectAttempts
                               : sizeof(delays_ms) / sizeof(delays_ms[0]) - 1;
        ++m_reconnectAttempts;
        // g_timeout_add() would attach to the GLOBAL default context (the
        // RTSP server's loop), so the reconnect — and the pipeline rebuild —
        // would run on a foreign thread, racing this pipeline's own loop.
        // Attach the source to our per-pipeline context instead.
        GSource* src = g_timeout_source_new(delays_ms[idx]);
        g_source_set_callback(src, &AiCameraPipeline::onReconnectThunk, this,
                              nullptr);
        g_source_attach(src, g_main_loop_get_context(m_loop));
        g_source_unref(src);
    }

    static gboolean onReconnectThunk(gpointer user) {
        auto* self = static_cast<AiCameraPipeline*>(user);
        if (self->m_running.load()) {
            std::fprintf(stderr, "[ai cam %s] reconnecting (attempt %u)...\n",
                         self->m_camera.id.c_str(),
                         self->m_reconnectAttempts);
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
                // GStreamer's e->message is famously generic ("Internal
                // data stream error"); the real cause (element name,
                // pad, codec mismatch, ...) is in `dbg`. Logging both
                // makes per-camera failures actually diagnosable.
                std::fprintf(stderr, "[ai cam %s] error: %s | debug: %s\n",
                             self->m_camera.id.c_str(),
                             e && e->message ? e->message : "unknown",
                             dbg ? dbg : "(none)");
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

        // Once stop() is requested, do no RGA / buffer work — the pipeline is
        // about to be torn down and its decoder buffers freed.
        if (!m_running.load()) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        // Decode + letterbox once; fan the shared Frame out to every job.
        FramePtr frame = buildFrame(sample);
        if (frame) {
            // A successful sample means the link is alive — clear any
            // reconnect backoff so a *future* failure starts again at the
            // short 2s interval instead of the capped 30s.
            noteHealthy();
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

        // Zero-copy path: when the decoder hands us a dmabuf (mppvideodec
        // does), import the fd into RGA once per frame. Every later blit
        // (letterbox, stage-2 crops, JPEG pack) reads the decoder buffer
        // directly — no map, no copy, no per-blit page pinning. Frames that
        // are not dmabuf-backed (software decoder) fall back to a plain
        // CPU mapping exactly as before.
        if (gst_buffer_n_memory(buf) == 1) {
            GstMemory* mem = gst_buffer_peek_memory(buf, 0);
            if (mem && gst_is_dmabuf_memory(mem)) {
                frame->dmaFd = gst_dmabuf_memory_get_fd(mem);
            }
        }
        if (frame->dmaFd >= 0) rga::importFrameDmabuf(*frame);
        if (!frame->rgaHandle && !frame->cpuNv12()) {
            return nullptr;  // Frame dtor unrefs the sample
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
    // Only touched from the GLib main-loop thread (build/teardown/reconnect
    // callbacks all run there), so no atomicity required.
    unsigned m_reconnectAttempts = 0;
};

#endif  // AI_ENGINE_AI_CAMERA_PIPELINE_HPP
