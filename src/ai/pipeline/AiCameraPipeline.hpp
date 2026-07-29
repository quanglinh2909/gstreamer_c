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
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gst/gst.h>
#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsink.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>

#include "AiJob.hpp"
#include "Config.hpp"
#include "FrameTypes.hpp"
#include "RgaConverter.hpp"
#include "service/AppSrcBridge.hpp"
#include "service/FrameSource.hpp"

class AiCameraPipeline {
public:
    // sourceLookup (tuỳ chọn): trả về nguồn RTP DÙNG CHUNG đang sống của camera
    // (kết nối ghi hình / xem live), hoặc nullptr nếu chưa có. Gọi ở MỖI lần
    // buildAndStart nên reconnect tự bám lại nguồn hiện tại. Rỗng => luôn tự mở
    // rtspsrc như cũ.
    AiCameraPipeline(cfg::Camera camera, int inferW, int inferH, int padColor,
                     std::vector<AiJob*> jobs,
                     std::function<std::shared_ptr<stream::FrameSource>()>
                         sourceLookup = {})
        : m_camera(std::move(camera)),
          m_inferW(inferW),
          m_inferH(inferH),
          m_padColor(padColor),
          m_jobs(std::move(jobs)),
          m_sourceLookup(std::move(sourceLookup)) {}

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

        // Ưu tiên BÁM nguồn RTP DÙNG CHUNG (kết nối ghi hình / xem live đã mở)
        // để KHỎI mở kết nối RTSP thứ hai tới camera: kéo access unit đã parse
        // qua AppSrcBridge rồi giải mã TẠI CHỖ. Một camera vừa ghi vừa chạy AI
        // trước đây tốn HAI kết nối main-stream + hai jitterbuffer; giờ dùng
        // chung một. Nguồn chung do recording tạo với codec ĐÃ DÒ lúc chạy nên
        // luôn đúng codec. Nếu chưa có nguồn chung (camera không ghi/không xem)
        // thì tự mở rtspsrc như cũ.
        m_activeSource = m_sourceLookup ? m_sourceLookup() : nullptr;
        m_usingShared = m_activeSource && m_activeSource->alive();

        // Phần sink dùng chung cho cả hai đường: appsink NV12 (caps + callback
        // gắn bên dưới theo tên "sink").
        const char* sinkPart =
            " ! appsink name=sink sync=false max-buffers=2 drop=true "
            "enable-last-sample=false";

        std::string launch;
        if (m_usingShared) {
            // appsrc bám nguồn chung. do-timestamp=true là bắt buộc cho
            // AppSrcBridge (nó xoá PTS nguồn, để appsrc dán giờ-đến). block=false
            // + max-bytes=0: push không bao giờ chặn thread streaming của nguồn
            // chung (không được để nhánh AI làm khựng ghi hình / người xem).
            // decodebin tự cắm h26Xparse + mppvideodec đúng theo caps nguồn đưa
            // sang — giữ y hệt đường giải mã (dmabuf NV12) mà rtspsrc cho ra.
            launch = std::string(
                         "appsrc name=aisrc is-live=true format=time "
                         "do-timestamp=true max-bytes=0 block=false ! "
                         "decodebin") +
                     sinkPart;
        } else {
            // application/x-rtp,media=video filter drops any audio RTP stream
            // the camera also emits — without it, decodebin tries to handle
            // every dynamic pad rtspsrc exposes, and a single failed audio
            // negotiation tears the whole pipeline down with the famously
            // unhelpful "Internal data stream error".
            // KHÔNG dùng drop-on-latency, latency=500ms. Camera 1080p/4K gửi
            // khung IDR (H265) thành BURST hàng trăm gói RTP dồn cục;
            // drop-on-latency + latency thấp làm jitterbuffer VỨT phần đuôi burst
            // -> IDR thiếu gói -> mppvideodec giải mã ra rác XANH LÁ ở mọi khung
            // (chroma=0). Hậu quả: ảnh sự kiện AI xanh, snapshot (getLatestJpeg)
            // xanh, VÀ đầu vào inference là rác -> phát hiện sai. Camera 720p IDR
            // nhỏ nên không lộ. appsink drop=true max-buffers=2 vẫn giữ luồng bám
            // hiện tại nên bỏ drop ở jitterbuffer không gây tồn đọng. (Cùng bẫy
            // đã sửa ở SnapshotGrabber / CameraRtpSource.)
            launch = std::string(
                         "rtspsrc name=src latency=500 protocols=tcp ! "
                         "application/x-rtp,media=video ! decodebin") +
                     sinkPart;
        }

        m_pipeline = gst_parse_launch(launch.c_str(), &err);
        if (!m_pipeline) {
            std::fprintf(stderr, "[ai cam %s] pipeline build failed: %s\n",
                         m_camera.id.c_str(), err ? err->message : "unknown");
            if (err) g_error_free(err);
            m_activeSource.reset();
            m_usingShared = false;
            return;
        }
        if (err) g_error_free(err);

        // Đường rtspsrc: đặt location. Đường appsrc chung: không có location,
        // AppSrcBridge sẽ bơm dữ liệu sau khi PLAYING.
        if (!m_usingShared) {
            GstElement* src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
            if (src) {
                g_object_set(src, "location", m_camera.uri.c_str(), nullptr);
                gst_object_unref(src);
            }
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

        // Mỗi lần (dựng lại) pipeline, bỏ vài khung ĐẦU của decoder — xem
        // onNewSample: mppvideodec (rõ nhất với H265) hay nhả một khung "xanh
        // lá" chưa dựng xong ngay khi mở van, lọt vào fullJpeg làm snapshot xanh.
        m_warmupRemaining = kWarmupFrames;

        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            std::fprintf(stderr, "[ai cam %s] failed to start pipeline\n",
                         m_camera.id.c_str());
            scheduleReconnect();
            return;
        }

        // Đấu nguồn chung -> appsrc SAU khi PLAYING (yêu cầu của AppSrcBridge).
        // Cầu nối tự đăng ký sink trên nguồn chung và bơm access unit vào đây.
        if (m_usingShared) {
            GstElement* aisrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "aisrc");
            if (aisrc) {
                m_bridge.attach(m_activeSource, aisrc);
                gst_object_unref(aisrc);  // bridge chỉ mượn con trỏ; pipeline giữ
                std::fprintf(stderr,
                             "[ai cam %s] bam nguon RTP dung chung (khong mo "
                             "ket noi RTSP thu hai)\n",
                             m_camera.id.c_str());
            }
        }

        // Watchdog đói-khung: đường appsrc bám nguồn chung KHÔNG tự phát lỗi bus
        // khi nguồn tắt tiếng (appsrc starve không EOS), nên cần đồng hồ riêng
        // phát hiện "lâu không có khung" rồi dựng lại (và tra lại nguồn). Cũng
        // dùng để chuyển TỪ rtspsrc riêng SANG nguồn chung khi recording hồi phục
        // sau blip. Gắn vào context của pipeline này (như onReconnect), KHÔNG
        // phải default context toàn cục.
        m_lastSampleUs.store(g_get_monotonic_time());
        GSource* wd = g_timeout_source_new(kWatchdogMs);
        g_source_set_callback(wd, &AiCameraPipeline::onWatchdogThunk, this,
                              nullptr);
        m_watchdogId = g_source_attach(wd, g_main_loop_get_context(m_loop));
        g_source_unref(wd);
    }

    // Mark the current attempt as healthy. The bus watch flips us back
    // to PLAYING once it sees a buffer flow successfully; until then we
    // assume the connect is still tentative and keep the backoff state.
    void noteHealthy() { m_reconnectAttempts = 0; }

    void teardown() {
        // Cắt cầu nối TRƯỚC: ngừng bơm access unit vào appsrc rồi mới hạ pipeline
        // (không để push chạy vào appsrc đang bị giải phóng). No-op nếu đang là
        // đường rtspsrc. detach() nhả luôn shared_ptr nguồn chung của bridge.
        m_bridge.detach();
        m_activeSource.reset();
        m_usingShared = false;

        // Gỡ watchdog. Nó gắn vào context RIÊNG của pipeline nên g_source_remove
        // (chỉ soi default context) không thấy — phải tra theo id trong context
        // đó rồi destroy. id==0 nghĩa là watchdog đã tự gỡ (đường reconnect).
        if (m_watchdogId && m_loop) {
            GMainContext* ctx = g_main_loop_get_context(m_loop);
            if (GSource* s = g_main_context_find_source_by_id(ctx, m_watchdogId)) {
                g_source_destroy(s);
            }
            m_watchdogId = 0;
        }

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
        // MỘT reconnect trong-chuyến MỘT LÚC. Một pipeline chết phun NHIỀU thông
        // điệp bus liên tiếp ("Could not read", "Internal data stream error",
        // "EOS", "Not found") — onBus gọi scheduleReconnect cho MỖI cái; cộng
        // thêm watchdog cũng có thể gọi. Không có cờ này thì mỗi lần chết đẻ ra
        // NHIỀU timer reconnect, mỗi timer lại dựng pipeline chết khác đẻ tiếp
        // -> bùng nổ cấp số nhân: camera hỏng/đã xoá quay ~25 lần/giây, đốt CPU
        // (đúng thủ phạm làm load cao). Gộp mọi tác nhân về đúng một reconnect.
        if (m_reconnectScheduled) return;
        m_reconnectScheduled = true;
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
        // Mở cổng cho reconnect KẾ TIẾP: từ đây trở đi một lỗi mới (nếu build
        // này lại chết) được phép lên lịch lại. Đặt TRƯỚC buildAndStart để lỗi
        // đồng bộ trong chính buildAndStart vẫn xếp được một reconnect.
        self->m_reconnectScheduled = false;
        if (self->m_running.load()) {
            std::fprintf(stderr, "[ai cam %s] reconnecting (attempt %u)...\n",
                         self->m_camera.id.c_str(),
                         self->m_reconnectAttempts);
            self->buildAndStart();
        }
        return G_SOURCE_REMOVE;
    }

    static gboolean onWatchdogThunk(gpointer user) {
        return static_cast<AiCameraPipeline*>(user)->onWatchdog();
    }

    // Chạy trên thread main-loop của pipeline này (cùng thread build/teardown/
    // reconnect) nên đọc/ghi m_usingShared/m_watchdogId không cần khoá; chỉ
    // m_lastSampleUs là atomic vì onNewSample ghi từ thread appsink.
    gboolean onWatchdog() {
        if (!m_running.load()) {
            m_watchdogId = 0;
            return G_SOURCE_REMOVE;
        }
        const gint64 now = g_get_monotonic_time();

        // 1) Đói khung: nguồn (chung hoặc rtspsrc) tắt tiếng mà KHÔNG phát lỗi
        //    bus. Với đường appsrc bám nguồn chung, nguồn chết chỉ làm appsrc
        //    hết dữ liệu — không có EOS/ERROR nào để onBus bắt — nên đây là
        //    đường DUY NHẤT phát hiện. Dựng lại: teardown nhả nguồn cũ, buildAnd
        //    Start tra lại nguồn hiện tại (có thể là nguồn chung mới, hoặc
        //    rtspsrc nếu recording đã tắt).
        const gint64 last = m_lastSampleUs.load();
        if (last != 0 && now - last > kStaleUs) {
            std::fprintf(stderr,
                         "[ai cam %s] %llds khong co khung -> dung lai\n",
                         m_camera.id.c_str(),
                         static_cast<long long>((now - last) / G_USEC_PER_SEC));
            m_watchdogId = 0;  // báo teardown khỏi gỡ lần nữa
            scheduleReconnect();
            return G_SOURCE_REMOVE;
        }

        // 2) Đang chạy rtspsrc RIÊNG nhưng nguồn CHUNG vừa sẵn sàng (recording
        //    hồi phục sau blip mạng): chuyển sang bám chung để bỏ kết nối RTSP
        //    thứ hai. Chỉ tốn một lần dựng lại; sau đó m_usingShared=true nên
        //    không lặp.
        if (!m_usingShared && m_sourceLookup) {
            auto shared = m_sourceLookup();
            if (shared && shared->alive()) {
                std::fprintf(stderr,
                             "[ai cam %s] nguon chung san sang -> chuyen sang "
                             "bam chung (bo ket noi RTSP rieng)\n",
                             m_camera.id.c_str());
                m_watchdogId = 0;
                scheduleReconnect();
                return G_SOURCE_REMOVE;
            }
        }
        return G_SOURCE_CONTINUE;
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

        // Nhịp cho watchdog đói-khung: có khung (kể cả khung warm-up) = còn sống.
        m_lastSampleUs.store(g_get_monotonic_time());

        // Once stop() is requested, do no RGA / buffer work — the pipeline is
        // about to be torn down and its decoder buffers freed.
        if (!m_running.load()) {
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        // Bỏ vài khung ĐẦU sau mỗi lần (dựng lại) pipeline. mppvideodec (rõ
        // nhất với H265) hay nhả MỘT khung "xanh lá" chưa dựng xong ngay khi mở
        // van decode: khung đó lọt vào fullJpeg (snapshot phục vụ giao diện lấy
        // từ đây qua getLatestJpeg) làm ảnh ra toàn xanh, và cũng là đầu vào
        // rác cho inference. Bỏ ~5 khung (~200ms) là qua hẳn. Vẫn tính là "link
        // sống" để reset backoff kết nối lại.
        if (m_warmupRemaining > 0) {
            --m_warmupRemaining;
            noteHealthy();
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        // Hỏi NHỊP TRƯỚC, tiền-xử-lý SAU. Mỗi job có maxFps riêng (thường 5)
        // trong khi camera nhả 15-25 khung/s, nên phần lớn khung sẽ bị vứt.
        // Trước đây cửa nhịp nằm trong AiJob::run — khung đã bị letterbox
        // (memset 1,2MB + blit RGA + memcpy 1,2MB) rồi mới bị vứt, tức 2/3
        // công tiền-xử-lý là công đổ đi. Giờ chỉ dựng Frame khi có ít nhất một
        // job tới hạn, và chỉ đẩy cho đúng những job đó.
        const uint64_t nowMs =
            static_cast<uint64_t>(g_get_monotonic_time() / 1000);
        m_dueJobs.clear();
        for (AiJob* job : m_jobs) {
            if (job->wantsFrame(nowMs)) m_dueJobs.push_back(job);
        }
        if (m_dueJobs.empty()) {
            // Vẫn là khung tới nơi => link còn sống; chỉ là chưa tới nhịp.
            noteHealthy();
            gst_sample_unref(sample);
            return GST_FLOW_OK;
        }

        // Decode + letterbox once; fan the shared Frame out to every due job.
        FramePtr frame = buildFrame(sample);
        if (frame) {
            // A successful sample means the link is alive — clear any
            // reconnect backoff so a *future* failure starts again at the
            // short 2s interval instead of the capped 30s.
            noteHealthy();
            for (AiJob* job : m_dueJobs) {
                job->noteAccepted(nowMs);
                job->submit(frame);
            }
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
    // Các job tới hạn nhận khung hiện tại (xem onNewSample). Là biến thành viên
    // để khỏi cấp phát vector mỗi khung; chỉ thread appsink dùng.
    std::vector<AiJob*> m_dueJobs;

    std::atomic<bool> m_running{false};
    std::thread m_thread;
    GMainLoop* m_loop = nullptr;
    GstElement* m_pipeline = nullptr;
    std::atomic<uint64_t> m_seq{0};
    // Only touched from the GLib main-loop thread (build/teardown/reconnect
    // callbacks all run there), so no atomicity required.
    unsigned m_reconnectAttempts = 0;
    // Có đúng một reconnect đang chờ hay không (chống bùng nổ timer khi pipeline
    // chết phun nhiều thông điệp lỗi). Cùng thread main-loop nên không cần atomic.
    bool m_reconnectScheduled = false;
    // Số khung decoder cần bỏ sau mỗi lần (dựng lại) pipeline (khung "xanh"
    // đầu của mppvideodec). Cùng thread main-loop nên không cần atomic.
    static constexpr int kWarmupFrames = 5;
    int m_warmupRemaining = kWarmupFrames;

    // Bám nguồn RTP DÙNG CHUNG (nếu có) thay vì tự mở rtspsrc thứ hai.
    std::function<std::shared_ptr<stream::FrameSource>()> m_sourceLookup;
    std::shared_ptr<stream::FrameSource> m_activeSource;  // giữ nguồn chung sống
    stream::AppSrcBridge m_bridge;      // nguồn chung -> appsrc giải mã của AI
    bool m_usingShared = false;         // đường hiện tại (thread main-loop)
    std::atomic<gint64> m_lastSampleUs{0};  // onNewSample (thread appsink) ghi
    guint m_watchdogId = 0;             // thread main-loop
    static constexpr gint64 kStaleUs = 7 * G_USEC_PER_SEC;
    static constexpr guint kWatchdogMs = 2000;
};

#endif  // AI_ENGINE_AI_CAMERA_PIPELINE_HPP
