#ifndef AI_ENGINE_JPEG_ENCODER_HPP
#define AI_ENGINE_JPEG_ENCODER_HPP

// Hardware JPEG encoding via the Rockchip MPP encoder (mppjpegenc).
//
// The appsrc ! mppjpegenc ! appsink pipeline is built ONCE and kept PLAYING,
// then reused for every frame. Rebuilding a pipeline (gst_parse_launch + a
// NULL->PLAYING->NULL state cycle) per encode was the single biggest CPU cost
// on the AI path; a persistent pipeline removes it.
//
// Input is always PACKED NV12 (stride == width). Caps are renegotiated only
// when the frame size changes. One JpegEncoder belongs to one AI job worker
// thread; the mutex only guards against accidental concurrent use.

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <vector>

#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>

class JpegEncoder {
public:
    JpegEncoder() = default;
    ~JpegEncoder() { close(); }

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // Encodes a packed NV12 image to JPEG. Returns false on failure.
    bool encodeNv12(const uint8_t* data, int width, int height,
                    std::vector<uint8_t>& outJpeg) {
        outJpeg.clear();
        if (!data || width <= 0 || height <= 0) return false;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_pipeline && !build()) return false;

        // Renegotiate caps only when the frame size actually changes.
        if (width != m_capW || height != m_capH) {
            GstCaps* caps = gst_caps_new_simple(
                "video/x-raw",
                "format", G_TYPE_STRING, "NV12",
                "width", G_TYPE_INT, width,
                "height", G_TYPE_INT, height,
                "framerate", GST_TYPE_FRACTION, 30, 1,
                nullptr);
            gst_app_src_set_caps(GST_APP_SRC(m_src), caps);
            gst_caps_unref(caps);
            m_capW = width;
            m_capH = height;
        }

        // Wrap the caller's buffer without copying — push and pull are strictly
        // serialised below, so `data` stays valid until the encode completes.
        const gsize size = static_cast<gsize>(width) * height * 3 / 2;
        GstBuffer* buf = gst_buffer_new_wrapped_full(
            GST_MEMORY_FLAG_READONLY, const_cast<uint8_t*>(data), size, 0, size,
            nullptr, nullptr);
        GST_BUFFER_PTS(buf) = m_pts;
        GST_BUFFER_DURATION(buf) = GST_SECOND / 30;
        m_pts += GST_SECOND / 30;

        if (gst_app_src_push_buffer(GST_APP_SRC(m_src), buf) != GST_FLOW_OK) {
            return false;
        }

        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(m_sink), 2 * GST_SECOND);
        if (!sample) {
            std::fprintf(stderr, "jpeg: no output within timeout\n");
            return false;
        }

        bool ok = false;
        GstBuffer* outBuf = gst_sample_get_buffer(sample);
        GstMapInfo info;
        if (outBuf && gst_buffer_map(outBuf, &info, GST_MAP_READ)) {
            outJpeg.assign(info.data, info.data + info.size);
            gst_buffer_unmap(outBuf, &info);
            ok = !outJpeg.empty();
        }
        gst_sample_unref(sample);
        return ok;
    }

private:
    bool build() {
        GError* err = nullptr;
        m_pipeline = gst_parse_launch(
            "appsrc name=src is-live=false format=time ! "
            "mppjpegenc ! "
            "appsink name=sink sync=false max-buffers=2 drop=false",
            &err);
        if (!m_pipeline) {
            std::fprintf(stderr, "jpeg: pipeline build failed: %s\n",
                         err ? err->message : "unknown");
            if (err) g_error_free(err);
            return false;
        }
        if (err) g_error_free(err);

        m_src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
        m_sink = gst_bin_get_by_name(GST_BIN(m_pipeline), "sink");
        if (!m_src || !m_sink) {
            close();
            return false;
        }
        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "jpeg: failed to start pipeline\n");
            close();
            return false;
        }
        return true;
    }

    void close() {
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
        if (m_src) {
            gst_object_unref(m_src);
            m_src = nullptr;
        }
        if (m_sink) {
            gst_object_unref(m_sink);
            m_sink = nullptr;
        }
        m_capW = 0;
        m_capH = 0;
    }

    std::mutex m_mutex;
    GstElement* m_pipeline = nullptr;
    GstElement* m_src = nullptr;
    GstElement* m_sink = nullptr;
    int m_capW = 0;
    int m_capH = 0;
    GstClockTime m_pts = 0;
};

#endif  // AI_ENGINE_JPEG_ENCODER_HPP
