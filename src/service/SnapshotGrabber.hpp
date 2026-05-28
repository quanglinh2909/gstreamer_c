#ifndef test_gstreamer_SnapshotGrabber_hpp
#define test_gstreamer_SnapshotGrabber_hpp

// One-shot JPEG snapshot from a camera RTSP URL.
//
// Opens a short-lived
//   rtspsrc ! decodebin ! videoconvert ! jpegenc ! appsink
// pipeline, pulls the first decoded frame, JPEG-encodes it, and tears the
// pipeline down before returning — nothing persists between calls and no
// connection is held open. Blocking: call it from an HTTP handler thread, not
// from a GLib main-loop thread.

#include <cstdint>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

namespace snapshot {

struct GrabResult {
    std::vector<uint8_t> jpeg;
    std::string error;  // empty on success
    bool ok() const { return error.empty() && !jpeg.empty(); }
};

inline GrabResult grabJpeg(const std::string& rtspUrl,
                           uint32_t latencyMs = 200,
                           uint32_t timeoutMs = 12000,
                           int jpegQuality = 85) {
    GrabResult result;
    if (rtspUrl.empty()) {
        result.error = "Camera has no RTSP URL";
        return result;
    }

    // The application/x-rtp,media=video filter drops the audio stream so
    // decodebin only ever sees video; video/x-raw forces a system-memory
    // buffer that videoconvert / jpegenc can consume on any decoder.
    const std::string launch =
        "rtspsrc name=src protocols=tcp drop-on-latency=true latency=" +
        std::to_string(latencyMs) +
        " ! application/x-rtp,media=video"
        " ! decodebin"
        " ! video/x-raw"
        " ! videoconvert"
        " ! jpegenc quality=" + std::to_string(jpegQuality) +
        " ! appsink name=sink max-buffers=1 drop=false sync=false";

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(launch.c_str(), &err);
    if (!pipeline) {
        result.error = err && err->message ? err->message
                                            : "Failed to build snapshot pipeline";
        if (err) g_error_free(err);
        return result;
    }
    if (err) {
        g_error_free(err);
        err = nullptr;
    }

    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!src || !sink) {
        result.error = "Snapshot pipeline missing elements";
        if (src) gst_object_unref(src);
        if (sink) gst_object_unref(sink);
        gst_object_unref(pipeline);
        return result;
    }
    g_object_set(src, "location", rtspUrl.c_str(), nullptr);

    GstBus* bus = gst_element_get_bus(pipeline);

    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) ==
        GST_STATE_CHANGE_FAILURE) {
        result.error = "Could not start snapshot pipeline";
    } else {
        const gint64 deadline =
            g_get_monotonic_time() + static_cast<gint64>(timeoutMs) * 1000;
        GstSample* sample = nullptr;
        while (g_get_monotonic_time() < deadline) {
            // Fail fast on a pipeline error instead of waiting out the timeout.
            GstMessage* msg = gst_bus_pop_filtered(
                bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR |
                                                 GST_MESSAGE_EOS));
            if (msg) {
                if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
                    GError* e = nullptr;
                    gchar* dbg = nullptr;
                    gst_message_parse_error(msg, &e, &dbg);
                    result.error = e && e->message ? e->message
                                                    : "Snapshot pipeline error";
                    if (e) g_error_free(e);
                    if (dbg) g_free(dbg);
                } else {
                    result.error = "Camera stream reached EOS";
                }
                gst_message_unref(msg);
                break;
            }
            sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                  200 * GST_MSECOND);
            if (sample) break;
        }

        if (sample) {
            GstBuffer* buf = gst_sample_get_buffer(sample);
            GstMapInfo info;
            if (buf && gst_buffer_map(buf, &info, GST_MAP_READ)) {
                result.jpeg.assign(info.data, info.data + info.size);
                gst_buffer_unmap(buf, &info);
            }
            gst_sample_unref(sample);
            if (result.jpeg.empty() && result.error.empty()) {
                result.error = "Snapshot produced no data";
            }
        } else if (result.error.empty()) {
            result.error = "Timed out waiting for a camera frame";
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(bus);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return result;
}

}  // namespace snapshot

#endif
