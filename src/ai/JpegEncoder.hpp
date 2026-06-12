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
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

class JpegEncoder {
public:
    JpegEncoder() = default;
    ~JpegEncoder() { close(); }

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // Encodes a packed NV12 image to JPEG. Returns false on failure.
    //
    // `fd >= 0`: the pixels live in a dmabuf and are handed to the encoder BY
    // FD (GstDmaBufAllocator). This is the safe path — mppjpegenc imports the
    // dmabuf instead of get_user_pages'ing a mapped pointer, which on this
    // BSP faults the jpege-core IOMMU and hard-freezes the board. `data` is
    // ignored when `fd >= 0`.
    // `fd < 0`: legacy/synthetic path, wraps the CPU `data` pointer (safe
    // only for anonymous memory, never for a dmabuf mmap).
    bool encodeNv12(const uint8_t* data, int width, int height,
                    std::vector<uint8_t>& outJpeg, int fd = -1) {
        outJpeg.clear();
        if ((!data && fd < 0) || width <= 0 || height <= 0) return false;

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

        const gsize size = static_cast<gsize>(width) * height * 3 / 2;
        GstBuffer* buf;
        if (fd >= 0 && m_hwMode) {
            // Import the existing dmabuf by fd. DONT_CLOSE: the fd is owned by
            // the caller's DmaHeapBuffer and reused next frame, so GStreamer
            // must not close it when this buffer is freed.
            if (!m_dmabufAlloc) m_dmabufAlloc = gst_dmabuf_allocator_new();
            GstMemory* mem = gst_dmabuf_allocator_alloc_with_flags(
                m_dmabufAlloc, fd, size, GST_FD_MEMORY_FLAG_DONT_CLOSE);
            buf = gst_buffer_new();
            gst_buffer_append_memory(buf, mem);
            // mppjpegenc needs the plane layout for packed NV12 (stride==w).
            gst_buffer_add_video_meta(buf, GST_VIDEO_FRAME_FLAG_NONE,
                                      GST_VIDEO_FORMAT_NV12, width, height);
        } else {
            // Wrap the caller's CPU buffer without copying — push and pull are
            // strictly serialised below, so `data` stays valid until done.
            buf = gst_buffer_new_wrapped_full(
                GST_MEMORY_FLAG_READONLY, const_cast<uint8_t*>(data), size, 0,
                size, nullptr, nullptr);
        }
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
            // The pushed frame is still queued inside the encoder. If the
            // pipeline were kept, the NEXT pull would return THIS frame's
            // late output — every later JPEG would belong to the previous
            // call — and with max-buffers=2 drop=false the backlog wedges
            // the pipeline into a 2s-timeout-per-frame crawl (observed in
            // production as "1 processed/s" after one timeout). Tear it
            // down; the next encode builds a fresh, in-sync pipeline.
            close();
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
    // The Rockchip hardware JPEG encoder (mppjpegenc) faults the jpege-core
    // IOMMU on this BSP under repeated multi-stream load and hard-freezes the
    // board — observed both with CPU-pointer and dmabuf-fd inputs. So the
    // DEFAULT is the software encoder (libjpeg, pure CPU, no codec IOMMU).
    // Opt back into hardware with AI_JPEG_HW=1 once a kernel/driver fix lands.
    static bool hwEnabled() {
        static const bool on = [] {
            const char* v = std::getenv("AI_JPEG_HW");
            return v && v[0] == '1';
        }();
        return on;
    }

    bool build() {
        GError* err = nullptr;
        m_hwMode = hwEnabled();
        const char* desc = m_hwMode
            ? "appsrc name=src is-live=false format=time ! "
              "mppjpegenc ! "
              "appsink name=sink sync=false max-buffers=2 drop=false"
            // jpegenc takes NV12 natively — no videoconvert needed (it would
            // just burn CPU doing NV12->I420 that jpegenc does internally).
            // libjpeg.so here is libjpeg-turbo (NEON SIMD), so this is already
            // the fast software path.
            : "appsrc name=src is-live=false format=time ! "
              "jpegenc quality=85 ! "
              "appsink name=sink sync=false max-buffers=2 drop=false";
        m_pipeline = gst_parse_launch(desc, &err);
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
        if (m_dmabufAlloc) {
            gst_object_unref(m_dmabufAlloc);
            m_dmabufAlloc = nullptr;
        }
        m_capW = 0;
        m_capH = 0;
    }

    std::mutex m_mutex;
    GstElement* m_pipeline = nullptr;
    GstElement* m_src = nullptr;
    GstElement* m_sink = nullptr;
    GstAllocator* m_dmabufAlloc = nullptr;
    bool m_hwMode = false;
    int m_capW = 0;
    int m_capH = 0;
    GstClockTime m_pts = 0;
};

#endif  // AI_ENGINE_JPEG_ENCODER_HPP
