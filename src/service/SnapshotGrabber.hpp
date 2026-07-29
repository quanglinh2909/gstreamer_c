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

#include <algorithm>
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

// Returns a launch fragment for the Rockchip hardware JPEG encoder, or ""
// when mppjpegenc isn't installed (caller falls back to software jpegenc).
//
// The quality property is NOT the same across mppjpegenc builds — both
// report "Version 1.14.4" yet expose different names and scales:
//   BSP/apt build  (/usr/lib/.../libgstrockchipmpp.so)      -> q-factor, 1..99
//   source build   (/usr/local/lib/.../libgstrockchipmpp.so) -> quant,    0..10
// Hardcoding either one makes the snapshot fail on the other machine with
// `no property "..." in element "mppjpegenc0"` (a 502 from the HTTP layer),
// so ask the element class which property it actually has. Setting no
// property at all would also work but silently drops the quality setting.
//
// Probed once per process: the installed plugin cannot change at runtime.
inline std::string mppJpegEncDesc(int jpegQuality) {
    // -1 = not probed yet, 0 = absent, 1 = quant, 2 = q-factor
    static int mode = [] {
        GstElement* e = gst_element_factory_make("mppjpegenc", nullptr);
        if (!e) return 0;
        GObjectClass* klass = G_OBJECT_GET_CLASS(e);
        int m = 3;  // present but no known quality property -> use defaults
        if (g_object_class_find_property(klass, "quant")) {
            m = 1;
        } else if (g_object_class_find_property(klass, "q-factor")) {
            m = 2;
        }
        gst_object_unref(e);
        return m;
    }();

    const int q = std::max(0, std::min(100, jpegQuality));
    switch (mode) {
        case 1:  // quant: 0..10, 10 = best. Keep >= 1 so quality never hits 0.
            return "mppjpegenc quant=" +
                   std::to_string(std::max(1, std::min(10, (q + 5) / 10)));
        case 2:  // q-factor: 1..99, same "higher is better" sense as jpegenc.
            return "mppjpegenc q-factor=" +
                   std::to_string(std::max(1, std::min(99, q)));
        case 3:
            return "mppjpegenc";
        default:
            return "";
    }
}

inline GrabResult grabJpeg(const std::string& rtspUrl,
                           uint32_t latencyMs = 200,
                           uint32_t timeoutMs = 12000,
                           int jpegQuality = 85) {
    GrabResult result;
    if (rtspUrl.empty()) {
        result.error = "Camera has no RTSP URL";
        return result;
    }

    // DÙNG jpegenc PHẦN MỀM cho snapshot một-lần.
    //
    // Trước đây ưu tiên mppjpegenc (phần cứng) cho nhẹ CPU, nhưng nó nhả ẢNH
    // TOÀN XANH LÁ (chroma = 0) — hoặc không ra khung nào — với một số camera,
    // rõ nhất là luồng H265 1080p đi qua mppvideodec (NV12 vào mppjpegenc bị
    // hỏng). Camera "test" H265 720p không lộ vì nó có job AI nên snapshot đi
    // đường RGA khác; camera "Của showroom" H265 1080p KHÔNG có job AI nên rơi
    // vào đây và ra xanh. Đo tận nơi: cùng pipeline, đổi mppjpegenc -> jpegenc
    // là hết xanh. Snapshot là một-lần, thưa, nên chi phí encode phần mềm một
    // khung không đáng kể; đổi lại chạy đúng trên MỌI luồng. (mppJpegEncDesc
    // vẫn giữ lại phòng khi cần, nhưng KHÔNG dùng ở đây.)
    const std::string jpegEncoder = "jpegenc quality=" + std::to_string(jpegQuality);

    // KHÔNG dùng drop-on-latency, và sàn latency 500ms.
    //
    // Camera 1080p/4K gửi khung IDR (H265) thành một BURST hàng trăm gói RTP
    // dồn cục. Với drop-on-latency=true + latency thấp (200ms), jitterbuffer
    // vứt phần đuôi của burst đó -> IDR thiếu gói -> mppvideodec giải mã ra rác:
    // ẢNH TOÀN XANH LÁ (chroma = 0). Camera 720p IDR nhỏ nên lọt, không lộ; đó
    // là lý do chỉ camera H265 1080p KHÔNG có job AI (rơi vào đường grabJpeg
    // này) mới bị. Đo tận nơi: cùng pipeline, bỏ drop-on-latency + latency=500
    // là hết xanh. Nguồn dùng chung CameraRtpSource cũng né đúng bẫy này (sàn
    // 300ms, không drop) — xem comment ở đó.
    const unsigned latency = latencyMs < 500 ? 500 : latencyMs;
    const std::string launch =
        "rtspsrc name=src protocols=tcp latency=" +
        std::to_string(latency) +
        " ! application/x-rtp,media=video"
        " ! decodebin"
        " ! video/x-raw"
        " ! videoconvert"
        " ! " + jpegEncoder +
        " ! appsink name=sink max-buffers=1 drop=false sync=false";

    // gst_parse_launch reports a recoverable failure (an unknown property, a
    // link that could not be made) by returning a *partial* pipeline with err
    // set — elements silently missing. Such a pipeline never produces a frame
    // and never posts a bus error, so it can only fail as a timeout. Treat any
    // err as fatal and surface it while it still says what went wrong.
    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(launch.c_str(), &err);
    if (!pipeline || err) {
        result.error = err && err->message ? err->message
                                            : "Failed to build snapshot pipeline";
        if (err) g_error_free(err);
        if (pipeline) gst_object_unref(pipeline);
        return result;
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
        // BỎ vài khung ĐẦU rồi mới lấy. mppvideodec (rõ nhất với H265) hay nhả
        // MỘT khung "xanh lá" chưa dựng xong ngay khi vừa mở van decode — lấy
        // đúng khung đầu (max-buffers=1) là dễ dính khung xanh đó, snapshot ra
        // toàn màu xanh. Với appsink max-buffers=1 drop=false, mỗi lần pull đẩy
        // decoder tiến đúng một khung, nên vài lần pull là qua hẳn khung xanh
        // (~0,3s @25fps) mà không phụ thuộc thời điểm. Giữ khung warm-up mới
        // nhất làm dự phòng nếu luồng quá ngắn/chậm để có khung sau warm-up.
        GstSample* lastSeen = nullptr;
        int warmup = 8;
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
            GstSample* s = gst_app_sink_try_pull_sample(GST_APP_SINK(sink),
                                                        200 * GST_MSECOND);
            if (!s) continue;
            if (warmup > 0) {
                --warmup;
                if (lastSeen) gst_sample_unref(lastSeen);
                lastSeen = s;  // dự phòng: khung warm-up mới nhất
                continue;
            }
            sample = s;
            break;
        }
        // Không lấy được khung sau warm-up (luồng quá ngắn hoặc EOS sớm) thì
        // dùng khung cuối đã thấy — vẫn hơn báo lỗi; và xoá lỗi EOS nếu thực sự
        // đã có khung để trả.
        if (!sample && lastSeen) {
            sample = lastSeen;
            lastSeen = nullptr;
            result.error.clear();
        }
        if (lastSeen) gst_sample_unref(lastSeen);

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
