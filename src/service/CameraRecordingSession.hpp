#ifndef test_gstreamer_CameraRecordingSession_hpp
#define test_gstreamer_CameraRecordingSession_hpp

#include "service/RecordingTypes.hpp"
#include "service/StreamTypes.hpp"

#include <gst/gst.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

class CameraRecordingSession : public std::enable_shared_from_this<CameraRecordingSession> {
public:
    CameraRecordingSession(stream::GStreamerConfig config,
                           stream::CameraRuntimeConfig camera,
                           stream::StreamCodec codec,
                           recording::RecordingSegmentSink segmentSink,
                           recording::MotionEventSink motionSink,
                           recording::RecordingErrorSink errorSink)
        : m_config(std::move(config)),
          m_camera(std::move(camera)),
          m_codec(codec),
          m_segmentSink(std::move(segmentSink)),
          m_motionSink(std::move(motionSink)),
          m_errorSink(std::move(errorSink)) {}

    ~CameraRecordingSession() {
        stop();
    }

    bool start() {
        stop();

        if (recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Off) {
            return true;
        }

        std::string error;
        if (startPipeline(/* includeMotionBranch */ true, error)) {
            return true;
        }

        if (canFallbackToRecordOnly()) {
            emitRecordingError("Recording motion branch failed, retrying record-only: " + error);
            std::string fallbackError;
            if (startPipeline(/* includeMotionBranch */ false, fallbackError)) {
                m_fallbackAttempted = true;
                return true;
            }
            emitRecordingError("Recording record-only fallback failed: " + fallbackError);
            return false;
        }

        emitRecordingError("Recording failed: " + error);
        return false;
    }

    bool startPipeline(bool includeMotionBranch, std::string& errorOut) {
        try {
            std::filesystem::create_directories(
                std::filesystem::path(m_config.recordingDir) / m_camera.id);
        } catch (const std::exception& error) {
            errorOut = error.what();
            return false;
        }

        const auto launch =
            recording::recordingLaunchStringForCamera(
                m_config, m_camera, m_codec,
                includeMotionBranch ? resolveMotionDecoder(m_camera, m_codec)
                                    : std::string{},
                includeMotionBranch);
        if (launch.empty()) {
            errorOut = "Could not build recording launch string";
            return false;
        }

        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(launch.c_str(), &error);
        if (!pipeline) {
            errorOut = consumeGError(error);
            return false;
        }
        if (error) {
            errorOut = consumeGError(error);
            gst_object_unref(pipeline);
            return false;
        }

        installFormatLocationHandler(pipeline);

        GstBus* bus = gst_element_get_bus(pipeline);
        guint watchId = 0;
        if (bus) {
            // Hand the watch a heap weak_ptr (freed by the GDestroyNotify) so a
            // bus message dispatched on the GLib loop thread can never touch a
            // CameraRecordingSession that stop()/the destructor freed on another
            // thread — onBusMessage locks it to a shared_ptr for the call.
            auto* weak = new std::weak_ptr<CameraRecordingSession>(weak_from_this());
            watchId = gst_bus_add_watch_full(
                bus,
                G_PRIORITY_DEFAULT,
                &CameraRecordingSession::onBusMessage,
                weak,
                [](gpointer data) {
                    delete static_cast<std::weak_ptr<CameraRecordingSession>*>(data);
                });
            gst_object_unref(bus);
        }

        if (includeMotionBranch && m_camera.motionKeyframeOnly) {
            installKeyframeOnlyProbe(pipeline);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pipeline = pipeline;
            m_busWatchId = watchId;
            m_motionBranchEnabled = includeMotionBranch && shouldUseMotionBranch();
        }

        const auto stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (stateResult == GST_STATE_CHANGE_FAILURE) {
            errorOut = "Failed to set recording pipeline to PLAYING";
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        GstElement* pipeline = nullptr;
        guint watchId = 0;
        std::vector<std::string> pendingFilesToDelete;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pipeline = m_pipeline;
            m_pipeline = nullptr;
            watchId = m_busWatchId;
            m_busWatchId = 0;
            m_openSegments.clear();
            for (const auto& pending : m_pendingSegments) {
                pendingFilesToDelete.push_back(pending.snapshot.path);
            }
            m_pendingSegments.clear();
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_postMotionUntil = {};
            m_motionBranchEnabled = false;
        }

        if (watchId != 0) g_source_remove(watchId);
        if (pipeline) {
            gst_element_send_event(pipeline, gst_event_new_eos());
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        deleteRecordingFiles(pendingFilesToDelete);
    }

    // Picks the first decoder element that exists for this camera's hardware
    // preference. recordingLaunchStringForCamera uses an explicit decoder, so
    // (unlike decodebin) there is no autoplug fallback — the candidate list
    // always ends with the software decoder for a supported codec. Returns ""
    // only for an unsupported codec (empty candidate list), for which the
    // caller's recordingLaunchStringForCamera also produces no launch string.
    static std::string resolveMotionDecoder(const stream::CameraRuntimeConfig& camera,
                                            stream::StreamCodec codec) {
        for (const auto& name : recording::motionDecoderCandidates(camera.hardware, codec)) {
            GstElementFactory* factory = gst_element_factory_find(name.c_str());
            if (factory) {
                gst_object_unref(factory);
                return name;
            }
        }
        return {};
    }

private:
    using Clock = std::chrono::system_clock;

    struct OpenSegment {
        std::string startedAt;
        Clock::time_point startedClock;
        bool hadMotion = false;
    };

    struct PendingSegment {
        recording::RecordingSegmentSnapshot snapshot;
        Clock::time_point startedClock;
        Clock::time_point endedClock;
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

    static std::string nowLocalFileTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    static gchar* formatSegmentLocation(GstElement*, guint, gpointer userData) {
        auto* self = static_cast<CameraRecordingSession*>(userData);
        if (!self) return nullptr;

        const auto path = recording::recordingFilePathForTimestamp(
            self->m_config, self->m_camera, nowLocalFileTimestamp());
        return g_strdup(path.c_str());
    }

    void installFormatLocationHandler(GstElement* pipeline) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "record_sink");
        if (!sink) return;
        g_signal_connect(sink,
                         "format-location",
                         G_CALLBACK(&CameraRecordingSession::formatSegmentLocation),
                         this);
        gst_object_unref(sink);
    }

    static std::string consumeGError(GError* error) {
        if (!error) return "Unknown GStreamer error";
        std::string message = error->message ? error->message : "Unknown GStreamer error";
        g_error_free(error);
        return message;
    }

    // Stateless pad probe: drops encoded P/B frames so the motion decoder only
    // processes IDR keyframes. No userData, so nothing to outlive — the probe
    // is released together with the pipeline.
    static GstPadProbeReturn dropDeltaFramesProbe(GstPad*, GstPadProbeInfo* info, gpointer) {
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
            return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
    }

    static void installKeyframeOnlyProbe(GstElement* pipeline) {
        GstElement* queue = gst_bin_get_by_name(GST_BIN(pipeline), "motion_q");
        if (!queue) return;  // no motion branch in this pipeline
        GstPad* pad = gst_element_get_static_pad(queue, "sink");
        if (pad) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                              &CameraRecordingSession::dropDeltaFramesProbe,
                              nullptr, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(queue);
    }

    static std::string gstMessageErrorText(GstMessage* message) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);

        std::string out = consumeGError(error);
        if (debug) {
            out += " (";
            out += debug;
            out += ")";
            g_free(debug);
        }
        return out;
    }

    bool isMotionMode() const {
        return recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Motion;
    }

    bool shouldUseMotionBranch() const {
        const auto mode = recording::effectiveRecordingMode(m_camera);
        return mode == recording::RecordingMode::Motion || m_camera.motionEnabled;
    }

    bool canFallbackToRecordOnly() const {
        return recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Always &&
               m_camera.motionEnabled &&
               !m_fallbackAttempted;
    }

    void emitRecordingError(const std::string& message) const {
        if (!m_errorSink) return;
        recording::RecordingErrorSnapshot snapshot;
        snapshot.cameraId = m_camera.id;
        snapshot.message = message;
        m_errorSink(snapshot);
    }

    std::chrono::seconds preMotionDuration() const {
        return std::chrono::seconds(std::max<uint32_t>(0, m_camera.preMotionSeconds));
    }

    std::chrono::seconds postMotionDuration() const {
        return std::chrono::seconds(std::max<uint32_t>(0, m_camera.postMotionSeconds));
    }

    static bool timeRangesOverlap(Clock::time_point leftStart,
                                  Clock::time_point leftEnd,
                                  Clock::time_point rightStart,
                                  Clock::time_point rightEnd) {
        return leftStart < rightEnd && rightStart < leftEnd;
    }

    bool isPostMotionActiveLocked(Clock::time_point now) const {
        return m_postMotionUntil != Clock::time_point{} && now <= m_postMotionUntil;
    }

    static void emitSegments(const std::vector<recording::RecordingSegmentSnapshot>& segments,
                             const recording::RecordingSegmentSink& sink) {
        if (!sink) return;
        for (const auto& segment : segments) {
            sink(segment);
        }
    }

    static void deleteRecordingFiles(const std::vector<std::string>& paths) {
        for (const auto& path : paths) {
            if (path.empty()) continue;
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    static gboolean onBusMessage(GstBus*, GstMessage* message, gpointer userData) {
        auto* weak = static_cast<std::weak_ptr<CameraRecordingSession>*>(userData);
        auto self = weak->lock();
        if (!self) return G_SOURCE_REMOVE;

        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ELEMENT:
                self->handleElementMessage(message);
                return G_SOURCE_CONTINUE;
            case GST_MESSAGE_ERROR:
                return self->handleErrorMessage(message);
            case GST_MESSAGE_EOS:
                return G_SOURCE_REMOVE;
            default:
                return G_SOURCE_CONTINUE;
        }
    }

    gboolean handleErrorMessage(GstMessage* message) {
        const auto errorText = gstMessageErrorText(message);
        if (canFallbackToRecordOnly()) {
            restartRecordOnlyAfterError(errorText);
            return G_SOURCE_REMOVE;
        }

        emitRecordingError("Recording pipeline error: " + errorText);
        return G_SOURCE_CONTINUE;
    }

    void restartRecordOnlyAfterError(const std::string& errorText) {
        GstElement* oldPipeline = nullptr;
        std::vector<std::string> pendingFilesToDelete;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            oldPipeline = m_pipeline;
            m_pipeline = nullptr;
            m_busWatchId = 0;
            m_motionBranchEnabled = false;
            m_fallbackAttempted = true;
            m_openSegments.clear();
            for (const auto& pending : m_pendingSegments) {
                pendingFilesToDelete.push_back(pending.snapshot.path);
            }
            m_pendingSegments.clear();
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_postMotionUntil = {};
        }

        if (oldPipeline) {
            gst_element_set_state(oldPipeline, GST_STATE_NULL);
            gst_object_unref(oldPipeline);
        }
        deleteRecordingFiles(pendingFilesToDelete);

        emitRecordingError("Recording motion branch failed, falling back to record-only: " + errorText);
        std::string fallbackError;
        if (!startPipeline(/* includeMotionBranch */ false, fallbackError)) {
            emitRecordingError("Recording record-only fallback failed: " + fallbackError);
        }
    }

    void handleElementMessage(GstMessage* message) {
        const GstStructure* structure = gst_message_get_structure(message);
        if (!structure) return;

        const auto* name = gst_structure_get_name(structure);
        if (!name) return;

        const std::string messageName(name);
        if (messageName == "splitmuxsink-fragment-opened") {
            const gchar* location = gst_structure_get_string(structure, "location");
            if (!location) return;

            std::lock_guard<std::mutex> lock(m_mutex);
            m_openSegments[location] = {
                nowIso8601(),
                Clock::now(),
                m_motionActive || isPostMotionActiveLocked(Clock::now())};
            return;
        }

        if (messageName == "splitmuxsink-fragment-closed") {
            const gchar* location = gst_structure_get_string(structure, "location");
            if (!location) return;
            closeSegment(location);
            return;
        }

        // motioncells posts element messages named "motion" for both the start
        // and the end of a motion event; the field present distinguishes them
        // ("motion_begin" vs "motion_finished") — the name is "motion" in both.
        switch (recording::classifyMotionMessage(
                    messageName,
                    gst_structure_has_field(structure, "motion_begin"),
                    gst_structure_has_field(structure, "motion_finished"))) {
            case recording::MotionMessageKind::Finished:
                finishMotionEvent();
                break;
            case recording::MotionMessageKind::Started:
                startOrUpdateMotionEvent();
                break;
            case recording::MotionMessageKind::None:
                break;
        }
    }

    void closeSegment(const std::string& location) {
        recording::RecordingSegmentSnapshot snapshot;
        Clock::time_point startedClock;
        Clock::time_point endedClock;
        bool motionMode = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_openSegments.find(location);
            if (found == m_openSegments.end()) return;

            endedClock = Clock::now();
            startedClock = found->second.startedClock;
            motionMode = isMotionMode();
            snapshot.cameraId = m_camera.id;
            snapshot.path = location;
            snapshot.startAt = found->second.startedAt;
            snapshot.endAt = nowIso8601();
            snapshot.durationMs = static_cast<int32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    endedClock - startedClock).count());
            snapshot.codec = stream::toString(m_codec);
            snapshot.container = "ts";
            snapshot.recordingMode = m_camera.recordingMode;
            snapshot.hasMotion =
                found->second.hadMotion || m_motionActive || isPostMotionActiveLocked(endedClock);
            m_openSegments.erase(found);
        }

        if (motionMode) {
            handleClosedMotionSegment({snapshot, startedClock, endedClock});
        } else if (m_segmentSink) {
            m_segmentSink(snapshot);
        }
    }

    void handleClosedMotionSegment(PendingSegment segment) {
        std::vector<recording::RecordingSegmentSnapshot> toEmit;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (segment.snapshot.hasMotion || isPostMotionActiveLocked(segment.endedClock)) {
                segment.snapshot.hasMotion = true;
                toEmit.push_back(segment.snapshot);
            } else {
                m_pendingSegments.push_back(std::move(segment));
            }
            drainExpiredPendingLocked(toDelete, now);
        }

        emitSegments(toEmit, m_segmentSink);
        deleteRecordingFiles(toDelete);
    }

    void retainPreMotionSegmentsLocked(std::vector<recording::RecordingSegmentSnapshot>& toEmit,
                                       Clock::time_point now) {
        const auto keepStart = now - preMotionDuration();
        auto item = m_pendingSegments.begin();
        while (item != m_pendingSegments.end()) {
            if (timeRangesOverlap(item->startedClock, item->endedClock, keepStart, now)) {
                item->snapshot.hasMotion = true;
                toEmit.push_back(item->snapshot);
                item = m_pendingSegments.erase(item);
            } else {
                ++item;
            }
        }
    }

    void drainExpiredPendingLocked(std::vector<std::string>& toDelete,
                                   Clock::time_point now) {
        const auto cutoff = now - preMotionDuration();
        while (!m_pendingSegments.empty()) {
            const auto& pending = m_pendingSegments.front();
            if (pending.endedClock >= cutoff) break;
            toDelete.push_back(pending.snapshot.path);
            m_pendingSegments.pop_front();
        }
    }

    void startOrUpdateMotionEvent() {
        std::vector<recording::RecordingSegmentSnapshot> toEmit;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_motionActive) {
                m_motionActive = true;
                m_motionStartedAt = nowIso8601();
            }
            m_postMotionUntil = {};
            retainPreMotionSegmentsLocked(toEmit, now);
            for (auto& item : m_openSegments) {
                item.second.hadMotion = true;
            }
            drainExpiredPendingLocked(toDelete, now);
        }

        emitSegments(toEmit, m_segmentSink);
        deleteRecordingFiles(toDelete);
    }

    void finishMotionEvent() {
        recording::MotionEventSnapshot snapshot;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_motionActive) return;
            snapshot.cameraId = m_camera.id;
            snapshot.startAt = m_motionStartedAt;
            snapshot.endAt = nowIso8601();
            snapshot.maxScore = 1.0;
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_postMotionUntil = now + postMotionDuration();
            drainExpiredPendingLocked(toDelete, now);
        }

        if (m_motionSink) m_motionSink(snapshot);
        deleteRecordingFiles(toDelete);
    }

    stream::GStreamerConfig m_config;
    stream::CameraRuntimeConfig m_camera;
    stream::StreamCodec m_codec = stream::StreamCodec::Unknown;
    recording::RecordingSegmentSink m_segmentSink;
    recording::MotionEventSink m_motionSink;
    recording::RecordingErrorSink m_errorSink;

    mutable std::mutex m_mutex;
    GstElement* m_pipeline = nullptr;
    guint m_busWatchId = 0;
    bool m_motionActive = false;
    bool m_motionBranchEnabled = false;
    bool m_fallbackAttempted = false;
    std::string m_motionStartedAt;
    Clock::time_point m_postMotionUntil;
    std::unordered_map<std::string, OpenSegment> m_openSegments;
    std::deque<PendingSegment> m_pendingSegments;
};

#endif
