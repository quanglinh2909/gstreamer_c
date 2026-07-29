#ifndef test_gstreamer_CameraRtpSource_hpp
#define test_gstreamer_CameraRtpSource_hpp

// Nguồn RTP dùng CHUNG cho MỌI phiên WHEP của MỘT camera.
//
// Vì sao tồn tại: trước đây mỗi phiên WHEP mở một rtspsrc riêng nối vào RTSP
// server nội bộ (rtsp://127.0.0.1:8554). Mỗi phiên vì thế lặp lại nguyên cụm
// jitterbuffer + rtph26Xdepay + h26Xparse trên CÙNG một luồng — ba người xem
// một camera là ba lần y hệt công việc đó, chưa kể chặng loopback TCP và một
// tầng RTSP thứ hai. Đo trên board: mỗi phiên ~15% CPU cho luồng passthrough
// 1080p, phần lớn nằm ở rtpjitterbuffer và các queue.
//
// Lớp này kéo camera ĐÚNG MỘT LẦN, tách thành access unit đã parse, rồi phát
// cho mọi phiên. Mỗi phiên chỉ còn trả tiền cho rtph26Xpay + webrtcbin (mã hoá
// SRTP) của riêng nó — thứ bắt buộc phải khác nhau giữa các trình duyệt (mỗi
// bên một SSRC/khoá SRTP). Toàn bộ jitterbuffer/depay/parse gộp về một chỗ.
//
// Vẫn giữ đúng tính chất "một kết nối tới camera vật lý" mà thiết kế :8554 có:
// camera Dahua/Hikvision giới hạn số luồng đồng thời nên đây là ràng buộc bắt
// buộc, không phải tối ưu. Mười người xem vẫn chỉ một luồng ra khỏi camera.
//
// Vòng đời: các WebRtcSession giữ shared_ptr tới nguồn này; WebRtcService giữ
// weak_ptr để tái dùng cho phiên mới cùng camera. Phiên cuối buông ->
// shared_ptr về 0 -> huỷ -> pipeline về NULL -> đóng kết nối camera.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/FrameSource.hpp"
#include "service/StreamTypes.hpp"

// Namespace `stream` (không phải `webrtc`) vì nguồn này giờ dùng chung cho cả
// xem live (WHEP) LẪN ghi hình (recording) — xem CameraSourceRegistry.
namespace stream {

class CameraRtpSource : public FrameSource {
public:
    CameraRtpSource(std::string cameraId, std::string rtspUrl, std::string codec,
                    stream::GStreamerConfig config)
        : m_cameraId(std::move(cameraId)),
          m_rtspUrl(std::move(rtspUrl)),
          m_codec(std::move(codec)),
          m_config(std::move(config)) {}

    ~CameraRtpSource() override { stop(); }

    CameraRtpSource(const CameraRtpSource&) = delete;
    CameraRtpSource& operator=(const CameraRtpSource&) = delete;

    const std::string& cameraId() const { return m_cameraId; }
    const std::string& codec() const override { return m_codec; }
    bool alive() const override { return m_alive.load(); }

    bool start() {
        const bool h265 = (m_codec == "h265");
        const char* encoding = h265 ? "H265" : "H264";
        const char* depay = h265 ? "rtph265depay" : "rtph264depay";
        const char* parser = h265 ? "h265parse" : "h264parse";
        const char* media = h265 ? "video/x-h265" : "video/x-h264";

        // latency sàn 300ms như nhánh phân phối: camera gửi IDR 4K thành hàng
        // trăm gói dồn cục, sàn thấp làm jitterbuffer vứt đuôi -> IDR hỏng cho
        // TẤT CẢ người xem. Đây là điểm gộp chung nên càng phải ổn định.
        const unsigned latency =
            m_config.sourceLatencyMs < 300 ? 300 : m_config.sourceLatencyMs;

        std::ostringstream launch;
        launch
            << "rtspsrc name=src location=" << stream::quoteLaunchValue(m_rtspUrl)
            << " latency=" << latency << " protocols=tcp"
            // Đã THỬ do-retransmission=false + buffer-mode=none để giảm CPU
            // jitterbuffer trên TCP — ĐO LẠI: KHÔNG giảm CPU (chi phí RTP thật
            // nằm ở xử lý TỪNG GÓI của rtspsrc TCP-interleaved, không phải máy
            // móc sắp-xếp/RTX). Đã revert về mặc định TCP đã kiểm chứng (bền
            // hơn khi mạng nghẽn). Lever thật là UDP nhưng rủi ro rớt gói burst
            // IDR 1080p/4K -> khung xanh (xem snapshot-green-frame-h265).
            << " ! application/x-rtp,media=video,encoding-name=" << encoding
            << " ! " << depay << " ! " << parser << " config-interval=-1"
            // Ép byte-stream/au: rtph26Xpay phía phiên nhận thẳng caps này nên
            // không cần parse lại lần nữa ở mỗi phiên.
            << " ! " << media << ",stream-format=byte-stream,alignment=au"
            << " ! appsink name=out sync=false max-buffers=30 drop=false";

        GError* err = nullptr;
        m_pipeline = gst_parse_launch(launch.str().c_str(), &err);
        if (!m_pipeline || err) {
            g_printerr("[rtpsrc] camera %s: khong dung duoc pipeline nguon: %s\n",
                       m_cameraId.c_str(),
                       err && err->message ? err->message : "loi khong ro");
            if (err) g_error_free(err);
            stop();
            return false;
        }

        m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "out");
        if (!m_appsink) {
            g_printerr("[rtpsrc] camera %s: thieu appsink\n", m_cameraId.c_str());
            stop();
            return false;
        }

        GstAppSinkCallbacks cbs{};
        cbs.new_sample = &CameraRtpSource::onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &cbs, this, nullptr);

        if (GstBus* bus = gst_element_get_bus(m_pipeline)) {
            m_busWatchId = gst_bus_add_watch(bus, &CameraRtpSource::onBusMessage, this);
            gst_object_unref(bus);
        }

        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            g_printerr("[rtpsrc] camera %s: khong PLAYING duoc nguon\n",
                       m_cameraId.c_str());
            stop();
            return false;
        }

        m_alive.store(true);
        g_print("[rtpsrc] camera %s: nguon dung chung bat dau (%s)\n",
                m_cameraId.c_str(), m_codec.c_str());
        return true;
    }

    void stop() {
        m_alive.store(false);
        if (m_busWatchId) {
            g_source_remove(m_busWatchId);
            m_busWatchId = 0;
        }
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
        }
        if (m_appsink) {
            gst_object_unref(m_appsink);
            m_appsink = nullptr;
        }
        if (m_pipeline) {
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
    }

    // Đăng ký một phiên. Phiên mới chỉ bắt đầu nhận từ KEYFRAME kế tiếp: bơm
    // giữa GOP một chuỗi P-frame tham chiếu frame chưa có sẽ ra hình vỡ tới
    // tận IDR sau. Trả về id để huỷ đăng ký trong removeSink.
    uint64_t addSink(Sink sink) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        const uint64_t id = m_nextId++;
        m_consumers.emplace(id, Consumer{std::move(sink), true});
        return id;
    }

    void removeSink(uint64_t id) override {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_consumers.erase(id);
    }

    size_t sinkCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_consumers.size();
    }

private:
    struct Consumer {
        Sink sink;
        bool needKeyframe = true;
    };

    static GstFlowReturn onNewSample(GstAppSink* appsink, gpointer user) {
        auto* self = static_cast<CameraRtpSource*>(user);
        GstSample* sample = gst_app_sink_pull_sample(appsink);
        if (!sample) return GST_FLOW_OK;
        self->deliver(sample);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }

    void deliver(GstSample* sample) {
        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buffer) return;

        const bool keyframe =
            !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        // Gom danh sách sink cần gọi TRONG khoá (còn xử lý cổng keyframe), rồi
        // gọi NGOÀI khoá: sink đẩy sang appsrc của phiên, không nên giữ khoá
        // của nguồn trong lúc đó — removeSink của phiên khác sẽ bị chặn oan.
        std::vector<Sink> targets;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            targets.reserve(m_consumers.size());
            for (auto& item : m_consumers) {
                Consumer& c = item.second;
                if (c.needKeyframe) {
                    if (!keyframe) continue;  // chờ IDR mới mở van
                    c.needKeyframe = false;
                }
                targets.push_back(c.sink);
            }
        }
        for (auto& sink : targets) sink(buffer, caps);
    }

    static gboolean onBusMessage(GstBus*, GstMessage* msg, gpointer user) {
        auto* self = static_cast<CameraRtpSource*>(user);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                g_printerr("[rtpsrc] camera %s: loi nguon: %s\n",
                           self->m_cameraId.c_str(),
                           err && err->message ? err->message : "?");
                if (err) g_error_free(err);
                g_free(dbg);
                // Nguồn chết: đánh dấu để WebRtcService khong tai dung. Các phiên
                // đang bám sẽ hết media -> watchdog dọn -> trình duyệt nối lại ->
                // phiên mới tạo nguồn mới.
                self->m_alive.store(false);
                break;
            }
            case GST_MESSAGE_EOS:
                g_printerr("[rtpsrc] camera %s: nguon EOS\n",
                           self->m_cameraId.c_str());
                self->m_alive.store(false);
                break;
            default:
                break;
        }
        return TRUE;
    }

    std::string m_cameraId;
    std::string m_rtspUrl;
    std::string m_codec;
    stream::GStreamerConfig m_config;

    GstElement* m_pipeline = nullptr;
    GstElement* m_appsink = nullptr;
    guint m_busWatchId = 0;
    std::atomic<bool> m_alive{false};

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, Consumer> m_consumers;
    uint64_t m_nextId = 1;
};

}  // namespace stream

#endif  // test_gstreamer_CameraRtpSource_hpp
