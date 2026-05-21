#ifndef test_gstreamer_RecordingTypes_hpp
#define test_gstreamer_RecordingTypes_hpp

#include "service/StreamTypes.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace recording {

enum class RecordingMode {
    Off,
    Always,
    Motion
};

struct SegmentTime {
    int64_t startMs = 0;
    int64_t endMs = 0;
};

struct MotionWindow {
    int64_t startMs = 0;
    int64_t endMs = 0;
    int64_t preMotionMs = 10'000;
    int64_t postMotionMs = 20'000;
};

struct RecordingSegmentSnapshot {
    std::string cameraId;
    std::string path;
    std::string startAt;
    std::string endAt;
    int32_t durationMs = 0;
    std::string codec;
    std::string container = "ts";
    std::string recordingMode;
    bool hasMotion = false;
};

struct MotionEventSnapshot {
    std::string cameraId;
    std::string startAt;
    std::string endAt;
    double maxScore = 0.0;
};

struct RecordingErrorSnapshot {
    std::string cameraId;
    std::string message;
};

using RecordingSegmentSink = std::function<void(const RecordingSegmentSnapshot&)>;
using MotionEventSink = std::function<void(const MotionEventSnapshot&)>;
using RecordingErrorSink = std::function<void(const RecordingErrorSnapshot&)>;

inline const char* toString(RecordingMode mode) {
    switch (mode) {
        case RecordingMode::Always: return "always";
        case RecordingMode::Motion: return "motion";
        case RecordingMode::Off:
        default: return "off";
    }
}

inline RecordingMode recordingModeFromString(const std::string& value) {
    const auto normalized = stream::toLower(value);
    if (normalized == "always") return RecordingMode::Always;
    if (normalized == "motion") return RecordingMode::Motion;
    return RecordingMode::Off;
}

inline bool isValidRecordingMode(const std::string& value) {
    const auto normalized = stream::toLower(value);
    return normalized == "off" || normalized == "always" || normalized == "motion";
}

inline RecordingMode effectiveRecordingMode(const stream::CameraRuntimeConfig& camera) {
    const auto mode = recordingModeFromString(camera.recordingMode);
    if (mode == RecordingMode::Off && camera.recordingEnabled) {
        return RecordingMode::Always;
    }
    return mode;
}

inline bool recordingPatchRequiresRuntimeRestart(bool recordingEnabledPresent,
                                                 bool recordingModePresent,
                                                 bool motionEnabledPresent,
                                                 bool motionSensitivityPresent,
                                                 bool motionThresholdPresent,
                                                 bool preMotionSecondsPresent,
                                                 bool postMotionSecondsPresent,
                                                 bool segmentSecondsPresent,
                                                 bool motionKeyframeOnlyPresent) {
    return recordingEnabledPresent ||
           recordingModePresent ||
           motionEnabledPresent ||
           motionSensitivityPresent ||
           motionThresholdPresent ||
           preMotionSecondsPresent ||
           postMotionSecondsPresent ||
           segmentSecondsPresent ||
           motionKeyframeOnlyPresent;
}

enum class MotionMessageKind {
    None,
    Started,
    Finished
};

// motioncells posts element messages named "motion" for both the start and
// the end of a motion event, distinguished by which field is present:
// "motion_begin" on start, "motion_finished" on end. Classify by field — the
// structure name is "motion" in both cases, so the name cannot distinguish them.
inline MotionMessageKind classifyMotionMessage(const std::string& structureName,
                                               bool hasBeginField,
                                               bool hasFinishedField) {
    if (structureName != "motion") return MotionMessageKind::None;
    if (hasFinishedField) return MotionMessageKind::Finished;
    if (hasBeginField) return MotionMessageKind::Started;
    return MotionMessageKind::None;
}

inline bool containsTimestamp(const SegmentTime& segment, int64_t timestampMs) {
    return segment.startMs <= timestampMs && timestampMs < segment.endMs;
}

inline int64_t seekOffsetMs(const SegmentTime& segment, int64_t timestampMs) {
    if (timestampMs <= segment.startMs) return 0;
    if (timestampMs >= segment.endMs) {
        return std::max<int64_t>(0, segment.endMs - segment.startMs);
    }
    return timestampMs - segment.startMs;
}

inline bool rangesOverlap(int64_t leftStartMs,
                          int64_t leftEndMs,
                          int64_t rightStartMs,
                          int64_t rightEndMs) {
    return leftStartMs < rightEndMs && rightStartMs < leftEndMs;
}

inline bool segmentOverlapsMotionWindow(const SegmentTime& segment,
                                        const MotionWindow& motion) {
    const auto keepStart = motion.startMs - std::max<int64_t>(0, motion.preMotionMs);
    const auto keepEnd = motion.endMs + std::max<int64_t>(0, motion.postMotionMs);
    return rangesOverlap(segment.startMs, segment.endMs, keepStart, keepEnd);
}

// Ordered preference list of decoder element names for the motion branch.
// Every list ends with the software decoder so resolution always succeeds.
inline std::vector<std::string> motionDecoderCandidates(const std::string& hardware,
                                                        stream::StreamCodec codec) {
    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if (!h264 && !h265) return {};  // Unknown / Unsupported — no decoder to offer

    const std::string va  = h264 ? "vah264dec"   : "vah265dec";
    const std::string nv  = h264 ? "nvh264dec"   : "nvh265dec";
    const std::string v4l = h264 ? "v4l2h264dec" : "v4l2h265dec";
    const std::string sw  = h264 ? "avdec_h264"  : "avdec_h265";

    const auto mode = stream::toLower(hardware);
    if (mode == "software" || mode == "sw" || mode == "none" || mode == "off") {
        return {sw};
    }
    if (mode == "vaapi" || mode == "va") {
        return {va, sw};
    }
    if (mode == "nvdec" || mode == "nvidia" || mode == "cuda") {
        return {nv, sw};
    }
    if (mode == "v4l2") {
        return {v4l, sw};
    }
    return {va, nv, v4l, sw};  // auto / empty / unknown
}

inline std::string recordingFilePattern(const stream::GStreamerConfig& config,
                                        const stream::CameraRuntimeConfig& camera) {
    std::ostringstream out;
    out << config.recordingDir << "/" << camera.id << "/segment-%010d.ts";
    return out.str();
}

inline std::string recordingFilePathForTimestamp(const stream::GStreamerConfig& config,
                                                 const stream::CameraRuntimeConfig& camera,
                                                 const std::string& timestamp) {
    std::ostringstream out;
    out << config.recordingDir << "/" << camera.id << "/" << timestamp << ".ts";
    return out.str();
}

inline std::string recordingLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                  const stream::CameraRuntimeConfig& camera,
                                                  stream::StreamCodec codec,
                                                  const std::string& motionDecoder,
                                                  bool includeMotionBranch = true) {
    const auto mode = effectiveRecordingMode(camera);
    if (mode == RecordingMode::Off) return {};

    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if (!h264 && !h265) return {};

    const char* encoding = h264 ? "H264" : "H265";
    const char* depay = h264 ? "rtph264depay" : "rtph265depay";
    const char* parser = h264 ? "h264parse" : "h265parse";
    const char* parsedCaps = h264
        ? "video/x-h264,stream-format=byte-stream,alignment=au"
        : "video/x-h265,stream-format=byte-stream,alignment=au";

    const uint64_t segmentNs =
        static_cast<uint64_t>(std::max<uint32_t>(1, camera.segmentSeconds)) * 1000ULL * 1000ULL * 1000ULL;

    // No surrounding "( ... )": this string is parsed with gst_parse_launch,
    // which returns a real GstPipeline only for an unwrapped description.
    // A "( ... )" wrapper would yield a clock-less GstBin that cannot run a
    // live source. (The live-stream launch string is wrapped because it goes
    // to gst_rtsp_media_factory_set_launch, a different API.)
    std::ostringstream launch;
    launch
        << "rtspsrc name=record_src location=" << stream::quoteLaunchValue(camera.rtsp)
        << " latency=" << config.sourceLatencyMs
        << " protocols=tcp"
        << " drop-on-latency=true"
        << " ! application/x-rtp,media=video,encoding-name=" << encoding
        << " ! " << depay
        << " ! " << parser << " config-interval=-1"
        << " ! " << parsedCaps
        << " ! tee name=record_t"
        << " record_t. ! queue"
        << " ! splitmuxsink name=record_sink async-finalize=true"
        << " message-forward=true"
        << " muxer-factory=mpegtsmux"
        << " max-size-time=" << segmentNs
        << " send-keyframe-requests=true"
        << " location=" << stream::quoteLaunchValue(recordingFilePattern(config, camera));

    if (includeMotionBranch && (mode == RecordingMode::Motion || camera.motionEnabled)) {
        launch
            << " record_t. ! queue name=motion_q leaky=downstream max-size-buffers=2"
            << " ! " << motionDecoder
            << " ! videorate drop-only=true"
            << " ! video/x-raw,framerate=5/1"
            << " ! videoscale"
            << " ! video/x-raw,width=320"
            << " ! videoconvert"
            << " ! video/x-raw,format=RGB"
            << " ! motioncells name=motion_detector display=false postallmotion=false"
            << " sensitivity=" << camera.motionSensitivity
            << " threshold=" << camera.motionThreshold
            << " gap=" << camera.postMotionSeconds
            << " ! fakesink sync=false";
    }

    return launch.str();
}

// Builds the launch string for the on-demand motion-debug RTSP mount. The
// string is wrapped in "( ... )" because it is fed to
// gst_rtsp_media_factory_set_launch. motioncells runs at 320px/5fps exactly
// like the recording motion branch (so the operator sees real production
// behaviour); the annotated frames are scaled up to 960px purely for viewing
// and re-encoded to H264. Returns {} for a non-H264/H265 codec or an empty
// decoder name.
inline std::string motionDebugLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                    const stream::CameraRuntimeConfig& camera,
                                                    stream::StreamCodec codec,
                                                    const std::string& motionDecoder) {
    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if ((!h264 && !h265) || motionDecoder.empty()) return {};

    const char* encoding = h264 ? "H264" : "H265";
    const char* depay = h264 ? "rtph264depay" : "rtph265depay";
    const char* parser = h264 ? "h264parse" : "h265parse";

    std::ostringstream launch;
    launch
        << "( rtspsrc name=motion_dbg_src location=" << stream::quoteLaunchValue(camera.rtsp)
        << " latency=" << config.sourceLatencyMs
        << " protocols=tcp"
        << " drop-on-latency=true"
        << " ! application/x-rtp,media=video,encoding-name=" << encoding
        << " ! " << depay
        << " ! " << parser << " config-interval=-1"
        << " ! " << motionDecoder
        << " ! videorate drop-only=true"
        << " ! video/x-raw,framerate=5/1"
        << " ! videoscale"
        << " ! video/x-raw,width=320"
        << " ! videoconvert"
        << " ! video/x-raw,format=RGB"
        << " ! motioncells display=true postallmotion=false"
        << " sensitivity=" << camera.motionSensitivity
        << " threshold=" << camera.motionThreshold
        << " gap=" << camera.postMotionSeconds
        << " ! videoscale"
        << " ! video/x-raw,width=960,height=540"
        << " ! videoconvert"
        << " ! video/x-raw,format=I420"
        << " ! x264enc tune=zerolatency speed-preset=ultrafast"
        << " ! rtph264pay config-interval=1 name=pay0 pt=96 )";
    return launch.str();
}

} // namespace recording

#endif
