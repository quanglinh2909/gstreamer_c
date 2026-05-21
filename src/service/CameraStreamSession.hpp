#ifndef test_gstreamer_CameraStreamSession_hpp
#define test_gstreamer_CameraStreamSession_hpp

#include "service/CameraRecordingSession.hpp"
#include "service/StreamTypes.hpp"

#include <gst/gst.h>
#include <gst/rtsp/rtsp.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <chrono>
#include <ctime>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

class CameraStreamSession : public std::enable_shared_from_this<CameraStreamSession> {
public:
    CameraStreamSession(stream::GStreamerConfig config,
                        stream::CameraRuntimeConfig camera,
                        GstRTSPMountPoints* mounts,
                        stream::StreamStatusSink statusSink = {},
                        recording::RecordingSegmentSink segmentSink = {},
                        recording::MotionEventSink motionSink = {})
        : m_config(std::move(config)),
          m_camera(std::move(camera)),
          m_mounts(GST_RTSP_MOUNT_POINTS(g_object_ref(mounts))),
          m_mountPath(stream::mountPathForCamera(m_camera.id)),
          m_motionMountPath(m_mountPath + "/motion"),
          m_statusSink(std::move(statusSink)),
          m_segmentSink(std::move(segmentSink)),
          m_motionSink(std::move(motionSink))
    {
        if (m_camera.hardware.empty()) m_camera.hardware = m_config.defaultHardware;
        normalizeRecordingFlag(m_camera);
        touchLocked();
    }

    ~CameraStreamSession() {
        cleanup();
        if (m_mounts) {
            g_object_unref(m_mounts);
            m_mounts = nullptr;
        }
    }

    void start() {
        stopReconnectTimer();
        stopRecordingLockedFree();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cleanupMountLocked();
            m_state = stream::StreamState::Starting;
            m_lastError.clear();
            touchLocked();
        }
        notifyStatusChanged();

        auto probe = probeCodec();
        if (probe.authError) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codec = stream::StreamCodec::Unknown;
                m_state = stream::StreamState::AuthError;
                m_lastError = probe.error.empty() ? "Authentication failed" : probe.error;
                touchLocked();
            }
            notifyStatusChanged();
            return;
        }

        if (probe.codec == stream::StreamCodec::Unsupported) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_codec = stream::StreamCodec::Unsupported;
                m_state = stream::StreamState::UnsupportedCodec;
                m_lastError = probe.error.empty() ? "Unsupported codec" : probe.error;
                touchLocked();
            }
            notifyStatusChanged();
            return;
        }

        if (probe.codec != stream::StreamCodec::H264 &&
            probe.codec != stream::StreamCodec::H265)
        {
            markTransientError(probe.error.empty() ? "Could not detect video codec" : probe.error);
            return;
        }

        const auto launch = stream::launchStringForCamera(m_config, m_camera, probe.codec);
        if (launch.empty()) {
            markTransientError("Could not build GStreamer launch string");
            return;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto* factory = gst_rtsp_media_factory_new();
            gst_rtsp_media_factory_set_launch(factory, launch.c_str());
            gst_rtsp_media_factory_set_shared(factory, TRUE);
            gst_rtsp_media_factory_set_suspend_mode(factory, GST_RTSP_SUSPEND_MODE_NONE);
            gst_rtsp_media_factory_set_latency(factory, m_config.sourceLatencyMs);
            gst_rtsp_media_factory_set_eos_shutdown(factory, FALSE);

            m_factory = GST_RTSP_MEDIA_FACTORY(g_object_ref(factory));

            auto* weak = new std::weak_ptr<CameraStreamSession>(shared_from_this());
            g_signal_connect_data(factory,
                                  "media-configure",
                                  G_CALLBACK(&CameraStreamSession::onMediaConfigure),
                                  weak,
                                  [](gpointer data, GClosure*) {
                                      delete static_cast<std::weak_ptr<CameraStreamSession>*>(data);
                                  },
                                  GConnectFlags(0));

            gst_rtsp_mount_points_add_factory(m_mounts, m_mountPath.c_str(), factory);

            // Second, on-demand mount: the camera video with the motioncells
            // overlay. Its pipeline is instantiated by gst-rtsp-server only
            // while a client is connected, so it is free when nobody watches.
            const auto motionDebugLaunch = recording::motionDebugLaunchStringForCamera(
                m_config, m_camera, probe.codec,
                CameraRecordingSession::resolveMotionDecoder(m_camera, probe.codec));
            if (!motionDebugLaunch.empty()) {
                auto* motionFactory = gst_rtsp_media_factory_new();
                gst_rtsp_media_factory_set_launch(motionFactory, motionDebugLaunch.c_str());
                gst_rtsp_media_factory_set_shared(motionFactory, TRUE);
                gst_rtsp_media_factory_set_suspend_mode(motionFactory, GST_RTSP_SUSPEND_MODE_NONE);
                m_motionFactory = GST_RTSP_MEDIA_FACTORY(g_object_ref(motionFactory));
                gst_rtsp_mount_points_add_factory(m_mounts, m_motionMountPath.c_str(), motionFactory);
            }

            m_codec = probe.codec;
            m_state = stream::StreamState::Running;
            m_retryCount = 0;
            m_lastError.clear();
            touchLocked();
        }
        startRecording(probe.codec);
        notifyStatusChanged();
    }

    void stop() {
        stopReconnectTimer();
        stopRecordingLockedFree();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cleanupMountLocked();
            m_state = stream::StreamState::Stopped;
            m_lastError.clear();
            touchLocked();
        }
        notifyStatusChanged();
    }

    void restart(stream::CameraRuntimeConfig camera) {
        stopRecordingLockedFree();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_camera = std::move(camera);
            if (m_camera.hardware.empty()) m_camera.hardware = m_config.defaultHardware;
            normalizeRecordingFlag(m_camera);
            m_mountPath = stream::mountPathForCamera(m_camera.id);
            m_motionMountPath = m_mountPath + "/motion";
        }
        start();
    }

    void cleanup() {
        stopReconnectTimer();
        stopRecordingLockedFree();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cleanupMountLocked();
            m_busWatchIds.clear();
            m_state = stream::StreamState::Stopped;
            touchLocked();
        }
        notifyStatusChanged();
    }

    stream::StreamStatusSnapshot snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        stream::StreamStatusSnapshot out;
        out.id = m_camera.id;
        out.state = m_state;
        out.inputRtsp = m_camera.rtsp;
        out.outputRtsp = stream::outputUrlForCamera(m_config, m_camera.id);
        out.codec = m_codec;
        out.hardware = m_camera.hardware.empty() ? m_config.defaultHardware : m_camera.hardware;
        out.recordingEnabled = m_camera.recordingEnabled;
        out.retryCount = m_retryCount;
        out.lastError = m_lastError;
        out.lastChangedAt = m_lastChangedAt;
        return out;
    }

private:
    struct ProbeResult {
        stream::StreamCodec codec = stream::StreamCodec::Unknown;
        std::string error;
        bool authError = false;
    };

    struct ProbeContext {
        GMainLoop* loop = nullptr;
        ProbeResult result;
    };

    static std::string nowIso8601() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    void touchLocked() {
        m_lastChangedAt = nowIso8601();
    }

    void normalizeRecordingFlag(stream::CameraRuntimeConfig& camera) const {
        camera.recordingEnabled =
            camera.recordingEnabled ||
            m_config.recordingEnabled ||
            recording::recordingModeFromString(camera.recordingMode) != recording::RecordingMode::Off;
    }

    void notifyStatusChanged() const {
        if (!m_statusSink) return;
        m_statusSink(snapshot());
    }

    void markRecordingError(const std::string& message) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_lastError = message;
            touchLocked();
        }
        notifyStatusChanged();
    }

    void startRecording(stream::StreamCodec codec) {
        if (recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Off) {
            return;
        }

        auto weak = weak_from_this();
        auto recordingErrorSink = [weak](const recording::RecordingErrorSnapshot& error) {
            if (auto self = weak.lock()) {
                self->markRecordingError(error.message);
            }
        };

        auto session = std::make_shared<CameraRecordingSession>(
            m_config, m_camera, codec, m_segmentSink, m_motionSink, recordingErrorSink);
        if (!session->start()) {
            return;
        }

        std::lock_guard<std::mutex> lock(m_mutex);
        m_recording = std::move(session);
    }

    void stopRecordingLockedFree() {
        std::shared_ptr<CameraRecordingSession> recording;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            recording = std::move(m_recording);
        }
        if (recording) recording->stop();
    }

    ProbeResult probeCodec() const {
        GMainContext* context = g_main_context_new();
        ProbeContext ctx;
        ctx.loop = g_main_loop_new(context, FALSE);

        GstElement* pipeline = gst_pipeline_new(nullptr);
        GstElement* source = gst_element_factory_make("rtspsrc", "src");
        if (!pipeline || !source) {
            if (pipeline) gst_object_unref(pipeline);
            if (source) gst_object_unref(source);
            g_main_loop_unref(ctx.loop);
            g_main_context_unref(context);
            return {stream::StreamCodec::Unknown, "Could not create rtspsrc probe", false};
        }

        g_object_set(source,
                     "location", m_camera.rtsp.c_str(),
                     "latency", m_config.sourceLatencyMs,
                     "protocols", GST_RTSP_LOWER_TRANS_TCP,
                     "drop-on-latency", TRUE,
                     nullptr);

        gst_bin_add(GST_BIN(pipeline), source);
        g_signal_connect(source, "pad-added", G_CALLBACK(&CameraStreamSession::onProbePadAdded), &ctx);

        GstBus* bus = gst_element_get_bus(pipeline);
        GSource* busSource = gst_bus_create_watch(bus);
        g_source_set_callback(busSource,
                              reinterpret_cast<GSourceFunc>(&CameraStreamSession::onProbeBusMessage),
                              &ctx,
                              nullptr);
        g_source_attach(busSource, context);
        gst_object_unref(bus);

        GSource* timeoutSource = g_timeout_source_new(7000);
        g_source_set_callback(timeoutSource, &CameraStreamSession::onProbeTimeout, &ctx, nullptr);
        g_source_attach(timeoutSource, context);

        const auto stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (stateResult == GST_STATE_CHANGE_FAILURE) {
            ctx.result.error = "Could not start RTSP probe";
        } else {
            g_main_loop_run(ctx.loop);
        }

        gst_element_set_state(pipeline, GST_STATE_NULL);
        g_source_destroy(busSource);
        g_source_unref(busSource);
        g_source_destroy(timeoutSource);
        g_source_unref(timeoutSource);
        gst_object_unref(pipeline);
        g_main_loop_unref(ctx.loop);
        g_main_context_unref(context);

        return ctx.result;
    }

    static void onProbePadAdded(GstElement*, GstPad* pad, gpointer userData) {
        auto* ctx = static_cast<ProbeContext*>(userData);
        GstCaps* caps = gst_pad_get_current_caps(pad);
        if (!caps) caps = gst_pad_query_caps(pad, nullptr);
        if (!caps || gst_caps_is_empty(caps)) {
            if (caps) gst_caps_unref(caps);
            return;
        }

        const GstStructure* structure = gst_caps_get_structure(caps, 0);
        const gchar* encoding = gst_structure_get_string(structure, "encoding-name");
        const auto codec = stream::codecFromEncodingName(encoding ? encoding : "");
        if (codec == stream::StreamCodec::H264 || codec == stream::StreamCodec::H265) {
            ctx->result.codec = codec;
            g_main_loop_quit(ctx->loop);
        } else if (codec == stream::StreamCodec::Unsupported) {
            ctx->result.codec = stream::StreamCodec::Unsupported;
            ctx->result.error = std::string("Unsupported codec: ") + (encoding ? encoding : "unknown");
            g_main_loop_quit(ctx->loop);
        }
        gst_caps_unref(caps);
    }

    static gboolean onProbeBusMessage(GstBus*, GstMessage* message, gpointer userData) {
        auto* ctx = static_cast<ProbeContext*>(userData);
        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                const std::string messageText = error && error->message
                    ? error->message
                    : "GStreamer probe error";
                ctx->result.error = messageText;
                ctx->result.authError = stream::isAuthErrorMessage(messageText);
                if (error) g_error_free(error);
                if (debug) g_free(debug);
                g_main_loop_quit(ctx->loop);
                return G_SOURCE_REMOVE;
            }
            case GST_MESSAGE_EOS:
                ctx->result.error = "RTSP probe reached EOS";
                g_main_loop_quit(ctx->loop);
                return G_SOURCE_REMOVE;
            default:
                return G_SOURCE_CONTINUE;
        }
    }

    static gboolean onProbeTimeout(gpointer userData) {
        auto* ctx = static_cast<ProbeContext*>(userData);
        ctx->result.error = "Timed out while probing RTSP codec";
        g_main_loop_quit(ctx->loop);
        return G_SOURCE_REMOVE;
    }

    static void onMediaConfigure(GstRTSPMediaFactory*, GstRTSPMedia* media, gpointer userData) {
        auto* weak = static_cast<std::weak_ptr<CameraStreamSession>*>(userData);
        auto self = weak->lock();
        if (!self) return;

        GstElement* element = gst_rtsp_media_get_element(media);
        if (!element) return;

        GstBus* bus = gst_element_get_bus(element);
        if (bus) {
            auto* busWeak = new std::weak_ptr<CameraStreamSession>(self);
            const guint watchId = gst_bus_add_watch_full(
                bus,
                G_PRIORITY_DEFAULT,
                &CameraStreamSession::onMediaBusMessage,
                busWeak,
                [](gpointer data) {
                    delete static_cast<std::weak_ptr<CameraStreamSession>*>(data);
                });
            self->addBusWatch(watchId);
            gst_object_unref(bus);
        }
        gst_object_unref(element);
    }

    static gboolean onMediaBusMessage(GstBus*, GstMessage* message, gpointer userData) {
        auto* weak = static_cast<std::weak_ptr<CameraStreamSession>*>(userData);
        auto self = weak->lock();
        if (!self) return G_SOURCE_REMOVE;

        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const std::string messageText = error && error->message
                ? error->message
                : "GStreamer runtime error";
            if (stream::isAuthErrorMessage(messageText)) {
                self->markAuthError(messageText);
            } else {
                self->markTransientError(messageText);
            }
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            return G_SOURCE_REMOVE;
        }

        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            self->markTransientError("RTSP media reached EOS");
            return G_SOURCE_REMOVE;
        }

        return G_SOURCE_CONTINUE;
    }

    void addBusWatch(guint watchId) {
        if (watchId == 0) return;
        std::lock_guard<std::mutex> lock(m_mutex);
        m_busWatchIds.push_back(watchId);
    }

    void markAuthError(const std::string& error) {
        stopReconnectTimer();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cleanupMountLocked();
            m_state = stream::StreamState::AuthError;
            m_lastError = error;
            touchLocked();
        }
        notifyStatusChanged();
    }

    void markTransientError(const std::string& error) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            cleanupMountLocked();
            m_state = stream::StreamState::Reconnecting;
            m_lastError = error;
            touchLocked();
            scheduleReconnectLocked();
        }
        notifyStatusChanged();
    }

    void scheduleReconnectLocked() {
        if (m_reconnectSourceId != 0) {
            g_source_remove(m_reconnectSourceId);
            m_reconnectSourceId = 0;
        }

        const auto delay = stream::nextRetryDelayMs(m_config, m_retryCount);
        ++m_retryCount;
        auto* weak = new std::weak_ptr<CameraStreamSession>(shared_from_this());
        m_reconnectSourceId = g_timeout_add_full(
            G_PRIORITY_DEFAULT,
            delay,
            &CameraStreamSession::onReconnectTimeout,
            weak,
            [](gpointer data) {
                delete static_cast<std::weak_ptr<CameraStreamSession>*>(data);
            });
    }

    static gboolean onReconnectTimeout(gpointer userData) {
        auto* weak = static_cast<std::weak_ptr<CameraStreamSession>*>(userData);
        auto self = weak->lock();
        if (!self) return G_SOURCE_REMOVE;

        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            self->m_reconnectSourceId = 0;
        }

        std::thread([self] {
            self->start();
        }).detach();

        return G_SOURCE_REMOVE;
    }

    void stopReconnectTimer() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_reconnectSourceId != 0) {
            g_source_remove(m_reconnectSourceId);
            m_reconnectSourceId = 0;
        }
    }

    void cleanupMountLocked() {
        for (auto id : m_busWatchIds) {
            if (id != 0) g_source_remove(id);
        }
        m_busWatchIds.clear();

        if (m_mounts && m_factory) {
            gst_rtsp_mount_points_remove_factory(m_mounts, m_mountPath.c_str());
        }
        if (m_mounts && m_motionFactory) {
            gst_rtsp_mount_points_remove_factory(m_mounts, m_motionMountPath.c_str());
        }
        if (m_factory) {
            g_object_unref(m_factory);
            m_factory = nullptr;
        }
        if (m_motionFactory) {
            g_object_unref(m_motionFactory);
            m_motionFactory = nullptr;
        }
    }

    stream::GStreamerConfig m_config;
    stream::CameraRuntimeConfig m_camera;
    GstRTSPMountPoints* m_mounts = nullptr;
    GstRTSPMediaFactory* m_factory = nullptr;
    GstRTSPMediaFactory* m_motionFactory = nullptr;
    std::string m_mountPath;
    std::string m_motionMountPath;
    stream::StreamStatusSink m_statusSink;
    recording::RecordingSegmentSink m_segmentSink;
    recording::MotionEventSink m_motionSink;
    std::shared_ptr<CameraRecordingSession> m_recording;

    mutable std::mutex m_mutex;
    stream::StreamState m_state = stream::StreamState::Stopped;
    stream::StreamCodec m_codec = stream::StreamCodec::Unknown;
    uint32_t m_retryCount = 0;
    std::string m_lastError;
    std::string m_lastChangedAt;
    guint m_reconnectSourceId = 0;
    std::vector<guint> m_busWatchIds;
};

#endif
