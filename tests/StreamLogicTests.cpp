#include "service/StreamTypes.hpp"
#include "service/RecordingTypes.hpp"
#include "service/HlsPlaylist.hpp"
#include "http/ByteRange.hpp"
#include "http/Uuid.hpp"

#include <cassert>
#include <string>
#include <vector>

namespace {

void testCodecDetection() {
    assert(stream::codecFromEncodingName("H264") == stream::StreamCodec::H264);
    assert(stream::codecFromEncodingName("h265") == stream::StreamCodec::H265);
    assert(stream::codecFromEncodingName("HEVC") == stream::StreamCodec::H265);
    assert(stream::codecFromEncodingName("MJPEG") == stream::StreamCodec::Unsupported);
}

void testMountAndOutputUrl() {
    stream::GStreamerConfig config;
    config.publicRtspHost = "10.0.0.5";
    config.rtspPort = 9554;

    assert(stream::mountPathForCamera("abc-123") == "/cameras/abc-123");
    assert(stream::outputUrlForCamera(config, "abc-123") ==
           "rtsp://10.0.0.5:9554/cameras/abc-123");
}

void testRetryDelayCapsAtMaximum() {
    stream::GStreamerConfig config;
    config.retryInitialMs = 1000;
    config.retryMaxMs = 5000;

    assert(stream::nextRetryDelayMs(config, 0) == 1000);
    assert(stream::nextRetryDelayMs(config, 1) == 2000);
    assert(stream::nextRetryDelayMs(config, 2) == 4000);
    assert(stream::nextRetryDelayMs(config, 3) == 5000);
    assert(stream::nextRetryDelayMs(config, 12) == 5000);
}

void testAuthErrorClassification() {
    assert(stream::isAuthErrorMessage("401 Unauthorized"));
    assert(stream::isAuthErrorMessage("Forbidden by RTSP source"));
    assert(stream::isAuthErrorMessage("Authentication failed"));
    assert(!stream::isAuthErrorMessage("Connection timed out"));
}

void testStreamStateToStringIsCoarse() {
    // 'state' is user-facing and collapses to online | offline | error.
    assert(std::string(stream::toString(stream::StreamState::Running)) == "online");
    assert(std::string(stream::toString(stream::StreamState::Stopped)) == "offline");
    assert(std::string(stream::toString(stream::StreamState::Starting)) == "offline");
    assert(std::string(stream::toString(stream::StreamState::Reconnecting)) == "offline");
    assert(std::string(stream::toString(stream::StreamState::AuthError)) == "error");
    assert(std::string(stream::toString(stream::StreamState::UnsupportedCodec)) == "error");
    assert(std::string(stream::toString(stream::StreamState::Error)) == "error");
}

void testLaunchStringUsesPassthroughCodecElements() {
    stream::GStreamerConfig config;
    config.sourceLatencyMs = 150;

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";
    camera.rtsp = "rtsp://user:pass@example.local/stream1";

    const auto h264Launch =
        stream::launchStringForCamera(config, camera, stream::StreamCodec::H264);
    assert(h264Launch.find("decodebin") == std::string::npos);
    assert(h264Launch.find("drop-on-latency=true") != std::string::npos);
    assert(h264Launch.find("rtph264depay") != std::string::npos);
    assert(h264Launch.find("h264parse") != std::string::npos);
    assert(h264Launch.find("rtph264pay config-interval=1 name=pay0") != std::string::npos);
    assert(h264Launch.find("tee name=video_t") == std::string::npos);
    assert(h264Launch.find("max-size-buffers=1") == std::string::npos);

    const auto h265Launch =
        stream::launchStringForCamera(config, camera, stream::StreamCodec::H265);
    assert(h265Launch.find("decodebin") == std::string::npos);
    assert(h265Launch.find("drop-on-latency=true") != std::string::npos);
    assert(h265Launch.find("rtph265depay") != std::string::npos);
    assert(h265Launch.find("h265parse") != std::string::npos);
    assert(h265Launch.find("rtph265pay config-interval=1 name=pay0") != std::string::npos);
    assert(h265Launch.find("tee name=video_t") == std::string::npos);
    assert(h265Launch.find("max-size-buffers=1") == std::string::npos);
}

void testRecordingModeParsing() {
    assert(recording::recordingModeFromString("off") == recording::RecordingMode::Off);
    assert(recording::recordingModeFromString("always") == recording::RecordingMode::Always);
    assert(recording::recordingModeFromString("motion") == recording::RecordingMode::Motion);
    assert(recording::toString(recording::RecordingMode::Motion) == std::string("motion"));
    assert(recording::isValidRecordingMode("not-a-mode") == false);

    stream::CameraRuntimeConfig camera;
    camera.recordingMode = "off";
    camera.recordingEnabled = true;
    assert(recording::effectiveRecordingMode(camera) == recording::RecordingMode::Always);
}

void testRecordingPatchPresenceRequiresRuntimeRestart() {
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, true, false, false, false, false, false, false, false));
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, false, true, false, false, false, false, false, false));
    // The new motionKeyframeOnly flag also forces a runtime restart.
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, false, false, false, false, false, false, false, true));
    assert(!recording::recordingPatchRequiresRuntimeRestart(
        false, false, false, false, false, false, false, false, false));
}

void testMotionRetentionWindowKeepsOverlappingSegments() {
    recording::SegmentTime segment;
    segment.startMs = 95'000;
    segment.endMs = 105'000;

    recording::MotionWindow motion;
    motion.startMs = 110'000;
    motion.endMs = 120'000;
    motion.preMotionMs = 10'000;
    motion.postMotionMs = 20'000;

    assert(recording::segmentOverlapsMotionWindow(segment, motion));

    segment.startMs = 70'000;
    segment.endMs = 80'000;
    assert(!recording::segmentOverlapsMotionWindow(segment, motion));
}

void testSeekOffsetWithinSegment() {
    recording::SegmentTime segment;
    segment.startMs = 1'000;
    segment.endMs = 11'000;

    assert(recording::containsTimestamp(segment, 1'000));
    assert(recording::containsTimestamp(segment, 10'999));
    assert(!recording::containsTimestamp(segment, 11'000));
    assert(recording::seekOffsetMs(segment, 5'250) == 4'250);
}

void testRecordingLaunchStringBuildsExpectedBranches() {
    stream::GStreamerConfig config;
    config.sourceLatencyMs = 100;
    config.recordingDir = "recordings";

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";
    camera.rtsp = "rtsp://example.local/stream1";
    camera.recordingMode = recording::toString(recording::RecordingMode::Always);
    camera.segmentSeconds = 10;

    const auto alwaysLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(alwaysLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(alwaysLaunch.find("message-forward=true") != std::string::npos);
    assert(alwaysLaunch.find("muxer-factory=mpegtsmux") != std::string::npos);
    assert(alwaysLaunch.find("muxer-factory=mp4mux") == std::string::npos);
    assert(alwaysLaunch.find("video/x-h264,stream-format=byte-stream,alignment=au") != std::string::npos);
    assert(alwaysLaunch.find("motioncells") == std::string::npos);
    assert(alwaysLaunch.find("max-size-time=10000000000") != std::string::npos);
    // The recording launch string is fed to gst_parse_launch (not an RTSP
    // media factory). A leading "(" makes gst_parse_launch return a clock-less
    // GstBin instead of a GstPipeline, and the recording never runs.
    assert(!alwaysLaunch.empty() && alwaysLaunch.front() != '(');
    assert(alwaysLaunch.back() != ')');

    camera.motionEnabled = true;
    const auto alwaysWithMotionIndexLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(alwaysWithMotionIndexLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("motioncells name=motion_detector") != std::string::npos);
    // Motion branch uses an explicit decoder, a named queue, and drop-only rate.
    assert(alwaysWithMotionIndexLaunch.find("decodebin") == std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("queue name=motion_q") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("videorate drop-only=true") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("! avdec_h264 !") != std::string::npos);

    const auto fallbackLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264", false);
    assert(fallbackLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(fallbackLaunch.find("motioncells") == std::string::npos);
    assert(fallbackLaunch.find("queue name=motion_q") == std::string::npos);

    camera.motionEnabled = false;
    camera.recordingMode = recording::toString(recording::RecordingMode::Motion);
    const auto motionLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H265, "vah265dec");
    assert(motionLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(motionLaunch.find("motioncells name=motion_detector") != std::string::npos);
    assert(motionLaunch.find("muxer-factory=mpegtsmux") != std::string::npos);
    assert(motionLaunch.find("video/x-h265,stream-format=byte-stream,alignment=au") != std::string::npos);
    assert(motionLaunch.find("video/x-raw,format=RGB") != std::string::npos);
    assert(motionLaunch.find("! vah265dec !") != std::string::npos);
}

void testRecordingFilePathUsesTimestampFilename() {
    stream::GStreamerConfig config;
    config.recordingDir = "recordings";

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";

    const std::string timestamp = "2026-05-20 16:46:07";
    assert(recording::recordingFilePathForTimestamp(config, camera, timestamp) ==
           "recordings/cam-a/2026-05-20 16:46:07.ts");
}

void testHlsPlaylistBuildsVodManifest() {
    std::vector<playback::HlsSegment> segments = {
        {
            "ae4b082b-5a63-4c83-8c1b-f12303f94b3d",
            "2026-05-20 16:46:07+07",
            9998,
        },
        {
            "11111111-2222-3333-4444-555555555555",
            "2026-05-20T16:46:17+07:00",
            10001,
        },
    };

    const auto playlist = playback::buildVodPlaylist(segments);
    assert(playlist.find("#EXTM3U\n") == 0);
    assert(playlist.find("#EXT-X-VERSION:3\n") != std::string::npos);
    assert(playlist.find("#EXT-X-PLAYLIST-TYPE:VOD\n") != std::string::npos);
    assert(playlist.find("#EXT-X-TARGETDURATION:11\n") != std::string::npos);
    assert(playlist.find("#EXT-X-PROGRAM-DATE-TIME:2026-05-20T16:46:07+07:00\n") != std::string::npos);
    assert(playlist.find("#EXTINF:9.998,\n/recording-segments/ae4b082b-5a63-4c83-8c1b-f12303f94b3d/file\n") != std::string::npos);
    assert(playlist.find("#EXTINF:10.001,\n/recording-segments/11111111-2222-3333-4444-555555555555/file\n") != std::string::npos);
    assert(playlist.rfind("#EXT-X-ENDLIST\n") == playlist.size() - std::string("#EXT-X-ENDLIST\n").size());
}

void testMotionDecoderCandidates() {
    using SC = stream::StreamCodec;

    assert((recording::motionDecoderCandidates("software", SC::H264)
            == std::vector<std::string>{"avdec_h264"}));
    assert((recording::motionDecoderCandidates("vaapi", SC::H264)
            == std::vector<std::string>{"vah264dec", "avdec_h264"}));
    assert((recording::motionDecoderCandidates("nvdec", SC::H265)
            == std::vector<std::string>{"nvh265dec", "avdec_h265"}));
    assert((recording::motionDecoderCandidates("v4l2", SC::H264)
            == std::vector<std::string>{"v4l2h264dec", "avdec_h264"}));

    assert((recording::motionDecoderCandidates("auto", SC::H264)
            == std::vector<std::string>{"vah264dec", "nvh264dec", "v4l2h264dec", "avdec_h264"}));

    // Unknown / empty values behave like "auto".
    assert(recording::motionDecoderCandidates("banana", SC::H264)
           == recording::motionDecoderCandidates("auto", SC::H264));
    assert(recording::motionDecoderCandidates("", SC::H265)
           == recording::motionDecoderCandidates("auto", SC::H265));

    // Every list ends with a software decoder so resolution always succeeds.
    assert(recording::motionDecoderCandidates("vaapi", SC::H265).back() == "avdec_h265");

    // Non-H264/H265 codecs have no decoder to offer.
    assert(recording::motionDecoderCandidates("auto", SC::Unknown).empty());
    assert(recording::motionDecoderCandidates("software", SC::Unsupported).empty());
}

void testMotionDebugLaunchString() {
    stream::GStreamerConfig config;
    config.sourceLatencyMs = 100;

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";
    camera.rtsp = "rtsp://example.local/stream1";
    camera.motionSensitivity = 0.5;
    camera.motionThreshold = 0.01;
    camera.postMotionSeconds = 20;

    const auto h264 = recording::motionDebugLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(!h264.empty());
    assert(h264.front() == '(');   // RTSP media-factory launch string
    assert(h264.back() == ')');
    assert(h264.find("motioncells display=true") != std::string::npos);
    assert(h264.find("rtph264depay") != std::string::npos);
    assert(h264.find("! avdec_h264 !") != std::string::npos);
    assert(h264.find("video/x-raw,width=320") != std::string::npos);  // analyse at 320px
    assert(h264.find("x264enc") != std::string::npos);
    assert(h264.find("name=pay0") != std::string::npos);
    // x264enc must be fed a fixed even-dimensioned 4:2:0 frame — an
    // unconstrained input lets it negotiate a 4:4:4 chroma, which conflicts
    // with the baseline H264 profile ("baseline profile doesn't support 4:4:4").
    assert(h264.find("video/x-raw,width=960,height=540") != std::string::npos);
    assert(h264.find("video/x-raw,format=I420 ! x264enc") != std::string::npos);

    const auto h265 = recording::motionDebugLaunchStringForCamera(
        config, camera, stream::StreamCodec::H265, "vah265dec");
    assert(h265.find("rtph265depay") != std::string::npos);
    assert(h265.find("! vah265dec !") != std::string::npos);

    // Unsupported codec or empty decoder -> empty string.
    assert(recording::motionDebugLaunchStringForCamera(
               config, camera, stream::StreamCodec::Unknown, "avdec_h264").empty());
    assert(recording::motionDebugLaunchStringForCamera(
               config, camera, stream::StreamCodec::H264, "").empty());
}

void testParseByteRange() {
    const int64_t size = 1000;  // a pretend 1000-byte file: valid bytes 0..999

    // Missing / non-"bytes=" / unparseable / multi-range -> not present.
    assert(!http::parseByteRange("", size).present);
    assert(!http::parseByteRange("seconds=0-1", size).present);
    assert(!http::parseByteRange("bytes=abc", size).present);
    assert(!http::parseByteRange("bytes=100-abc", size).present);
    assert(!http::parseByteRange("bytes=0-99,200-299", size).present);

    // bytes=0- -> the whole file.
    {
        auto r = http::parseByteRange("bytes=0-", size);
        assert(r.present && r.satisfiable && r.start == 0 && r.end == 999);
    }
    // bytes=100-199 -> an exact span.
    {
        auto r = http::parseByteRange("bytes=100-199", size);
        assert(r.present && r.satisfiable && r.start == 100 && r.end == 199);
    }
    // END past end-of-file -> clamped to the last byte.
    {
        auto r = http::parseByteRange("bytes=900-99999", size);
        assert(r.present && r.satisfiable && r.start == 900 && r.end == 999);
    }
    // Suffix range bytes=-500 -> the last 500 bytes.
    {
        auto r = http::parseByteRange("bytes=-500", size);
        assert(r.present && r.satisfiable && r.start == 500 && r.end == 999);
    }
    // Suffix larger than the file -> the whole file.
    {
        auto r = http::parseByteRange("bytes=-99999", size);
        assert(r.present && r.satisfiable && r.start == 0 && r.end == 999);
    }
    // START past end-of-file -> present but not satisfiable (-> 416).
    {
        auto r = http::parseByteRange("bytes=1000-1100", size);
        assert(r.present && !r.satisfiable);
    }
    // start > end -> present but not satisfiable.
    {
        auto r = http::parseByteRange("bytes=100-50", size);
        assert(r.present && !r.satisfiable);
    }
    // Empty file -> any range unsatisfiable.
    {
        auto r = http::parseByteRange("bytes=0-", 0);
        assert(r.present && !r.satisfiable);
    }
    // bytes=0-0 -> exactly the first byte (start == end boundary).
    {
        auto r = http::parseByteRange("bytes=0-0", size);
        assert(r.present && r.satisfiable && r.start == 0 && r.end == 0);
    }
    // bytes=-0 -> a suffix of zero bytes: present but not satisfiable.
    {
        auto r = http::parseByteRange("bytes=-0", size);
        assert(r.present && !r.satisfiable);
    }
}

void testUuidValidationRejectsMalformedPathIds() {
    assert(http::isUuid("b26e4505-e9ce-4c79-90fc-91b40e656b3c"));
    assert(http::isUuid("B26E4505-E9CE-4C79-90FC-91B40E656B3C"));

    assert(!http::isUuid(""));
    assert(!http::isUuid("b26e4505-e9ce-4c79-90fc-91b40e656b3c%22"));
    assert(!http::isUuid("b26e4505-e9ce-4c79-90fc-91b40e656b3c\""));
    assert(!http::isUuid("not-a-uuid"));
}

void testMotionMessageClassification() {
    using recording::MotionMessageKind;
    // motioncells posts element messages named "motion" for BOTH the start
    // and the end of a motion event — the field present distinguishes them:
    // "motion_begin" on start, "motion_finished" on end.
    assert(recording::classifyMotionMessage("motion", true, false)
           == MotionMessageKind::Started);
    assert(recording::classifyMotionMessage("motion", false, true)
           == MotionMessageKind::Finished);
    // "motion_finished" wins if both fields are somehow present.
    assert(recording::classifyMotionMessage("motion", true, true)
           == MotionMessageKind::Finished);
    // A "motion" structure with neither field, or any non-motion structure.
    assert(recording::classifyMotionMessage("motion", false, false)
           == MotionMessageKind::None);
    assert(recording::classifyMotionMessage("splitmuxsink-fragment-closed", false, true)
           == MotionMessageKind::None);
}

}

int main() {
    testCodecDetection();
    testMountAndOutputUrl();
    testRetryDelayCapsAtMaximum();
    testAuthErrorClassification();
    testStreamStateToStringIsCoarse();
    testLaunchStringUsesPassthroughCodecElements();
    testRecordingModeParsing();
    testRecordingPatchPresenceRequiresRuntimeRestart();
    testMotionRetentionWindowKeepsOverlappingSegments();
    testSeekOffsetWithinSegment();
    testRecordingLaunchStringBuildsExpectedBranches();
    testRecordingFilePathUsesTimestampFilename();
    testHlsPlaylistBuildsVodManifest();
    testMotionDecoderCandidates();
    testMotionDebugLaunchString();
    testMotionMessageClassification();
    testParseByteRange();
    testUuidValidationRejectsMalformedPathIds();
    return 0;
}
