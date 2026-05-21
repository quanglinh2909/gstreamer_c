#ifndef AI_ENGINE_JPEG_ENCODER_HPP
#define AI_ENGINE_JPEG_ENCODER_HPP

// Hardware JPEG encoding via the Rockchip MPP encoder (mppjpegenc GStreamer
// element). Input is always PACKED NV12 (stride == width) so there is no
// stride/meta negotiation to get wrong — callers pack with RGA first.
//
// A fresh one-shot pipeline is built per encode. JPEG output is only produced
// for frames that carry a detection, so this runs rarely; the hardware encoder
// makes the per-call cost a few milliseconds with negligible CPU.

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

namespace jpeg {

inline bool encodeNv12(const uint8_t* data, int width, int height,
                       std::vector<uint8_t>& outJpeg) {
    outJpeg.clear();
    if (!data || width <= 0 || height <= 0) return false;

    const gsize size = static_cast<gsize>(width) * height * 3 / 2;

    GError* err = nullptr;
    GstElement* pipeline = gst_parse_launch(
        "appsrc name=src is-live=false format=time ! "
        "mppjpegenc ! "
        "appsink name=sink sync=false max-buffers=1 drop=false",
        &err);
    if (!pipeline) {
        if (err) {
            std::fprintf(stderr, "jpeg: pipeline build failed: %s\n", err->message);
            g_error_free(err);
        }
        return false;
    }
    if (err) g_error_free(err);

    GstElement* src = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "sink");
    if (!src || !sink) {
        if (src) gst_object_unref(src);
        if (sink) gst_object_unref(sink);
        gst_object_unref(pipeline);
        return false;
    }

    GstCaps* caps = gst_caps_new_simple("video/x-raw",
                                        "format", G_TYPE_STRING, "NV12",
                                        "width", G_TYPE_INT, width,
                                        "height", G_TYPE_INT, height,
                                        "framerate", GST_TYPE_FRACTION, 30, 1,
                                        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(src), caps);
    gst_caps_unref(caps);

    bool ok = false;
    if (gst_element_set_state(pipeline, GST_STATE_PLAYING) !=
        GST_STATE_CHANGE_FAILURE) {
        GstBuffer* buf = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY, const_cast<uint8_t*>(data), size, 0, size,
            nullptr, nullptr);

        if (gst_app_src_push_buffer(GST_APP_SRC(src), buf) == GST_FLOW_OK &&
            gst_app_src_end_of_stream(GST_APP_SRC(src)) == GST_FLOW_OK) {
            GstSample* sample = gst_app_sink_pull_sample(GST_APP_SINK(sink));
            if (sample) {
                GstBuffer* outBuf = gst_sample_get_buffer(sample);
                GstMapInfo info;
                if (outBuf && gst_buffer_map(outBuf, &info, GST_MAP_READ)) {
                    outJpeg.assign(info.data, info.data + info.size);
                    gst_buffer_unmap(outBuf, &info);
                    ok = !outJpeg.empty();
                }
                gst_sample_unref(sample);
            }
        }
    }

    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(src);
    gst_object_unref(sink);
    gst_object_unref(pipeline);
    return ok;
}

}  // namespace jpeg

#endif  // AI_ENGINE_JPEG_ENCODER_HPP
