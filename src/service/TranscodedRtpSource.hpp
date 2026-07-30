#ifndef test_gstreamer_TranscodedRtpSource_hpp
#define test_gstreamer_TranscodedRtpSource_hpp

// Nguồn H264 DÙNG CHUNG, transcode MỘT LẦN từ một camera H265.
//
// Vì sao tồn tại: trình duyệt (Chrome/Firefox) không nhận H265 qua WebRTC, nên
// camera H265 phải giải mã rồi mã hoá lại sang H264. Trước đây MỖI phiên WHEP
// tự dựng cụm mppvideodec ! mpph264enc của riêng nó — mười người xem một camera
// H265 là mười lần transcode song song, ăn tuyến tính vào VPU/RGA (thứ dùng
// chung với cả AI). Lớp này gom việc đó về đúng MỘT lần: kéo access unit H265
// từ CameraRtpSource dùng chung, chạy qua một pipeline transcode duy nhất, rồi
// phát H264 đã parse cho mọi phiên như CameraRtpSource phát H264 gốc.
//
// Vòng đời: đếm tham chiếu như CameraRtpSource. Các phiên WHEP giữ shared_ptr;
// CameraSourceRegistry giữ weak_ptr để tái dùng. Người xem H265-transcode cuối
// buông -> huỷ -> nhả luôn CameraRtpSource nền (nếu không còn ai ghi/ xem H265
// gốc). Giữ shared_ptr tới nguồn nền nên camera vật lý vẫn chỉ bị kéo một lần.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/AppSrcBridge.hpp"
#include "service/FrameSource.hpp"
#include "service/StreamTypes.hpp"

namespace stream {

class TranscodedRtpSource : public FrameSource {
public:
    TranscodedRtpSource(std::string cameraId,
                        std::shared_ptr<FrameSource> base,
                        stream::GStreamerConfig config)
        : m_cameraId(std::move(cameraId)),
          m_base(std::move(base)),
          m_config(std::move(config)) {}

    ~TranscodedRtpSource() override { stop(); }

    TranscodedRtpSource(const TranscodedRtpSource&) = delete;
    TranscodedRtpSource& operator=(const TranscodedRtpSource&) = delete;

    const std::string& cameraId() const { return m_cameraId; }
    // Luôn là H264: đó là mục đích của lớp này. Phiên WHEP dùng nó như nguồn
    // H264 passthrough, không transcode lại lần nữa.
    const std::string& codec() const override { return m_codec; }
    // Chết nếu pipeline transcode chết HOẶC nguồn nền (camera) chết.
    bool alive() const override {
        return m_alive.load() && m_base && m_base->alive();
    }

    // Bitrate cho mpph264enc, lấy từ bitrate THẬT của camera nguồn.
    //
    // Hệ số 1,6: H264 cần nhiều bit hơn H265 cho cùng chất lượng, nên nhân lên
    // mới không làm xấu hình. Nhờ vậy chất lượng đi theo ĐÚNG từng camera, khác
    // với một con số cố định (camera 0,85 Mbps và camera 3,4 Mbps cần khác nhau).
    //
    // Nguồn nền hầu như luôn đã chạy sẵn (ghi hình giữ nó sống) nên số đo có
    // ngay khi người xem đầu tiên vào. Nếu chưa có thì dùng mức dự phòng thay
    // vì bps=0 — "tự tính" của MPP là cái bẫy đang sửa.
    uint64_t encoderBps() const {
        if (const char* forced = std::getenv("AI_TRANSCODE_BPS")) {
            const long v = atol(forced);
            if (v > 0) return static_cast<uint64_t>(v);
        }
        double factor = 1.6;
        if (const char* f = std::getenv("AI_TRANSCODE_FACTOR")) {
            const double v = g_ascii_strtod(f, nullptr);
            if (v > 0.2 && v < 10.0) factor = v;
        }
        const uint64_t src = m_base ? m_base->bitrateBps() : 0;
        uint64_t bps = src ? static_cast<uint64_t>(src * factor) : 3000000;
        if (bps < 512000) bps = 512000;        // sàn: đừng bết quá
        if (bps > 8000000) bps = 8000000;      // trần: chặn camera bitrate cao
        g_print("[transcode] camera %s: nguon %.2f Mbps -> encoder %.2f Mbps"
                " (he so %.2f)\n",
                m_cameraId.c_str(), src / 1e6, bps / 1e6, factor);
        return bps;
    }

    bool start() {
        if (!m_base || m_base->codec() != "h265") {
            g_printerr("[transcode] camera %s: nguon nen khong phai H265, bo qua\n",
                       m_cameraId.c_str());
            return false;
        }

        // Toàn bộ transcode chạy trên VPU (mppvideodec/mpph264enc), không đụng
        // CPU/NPU. gop=-1 => một keyframe mỗi giây: người xem mới vào có hình
        // trong ~1s kể cả khi camera gốc để GOP hàng chục giây.
        //
        // TUYỆT ĐỐI KHÔNG để bps=0. "Tự tính" của MPP là `w*h*fps/8`, ra
        // 6,48 Mbps cho 1080p25 BẤT KỂ nguồn bao nhiêu. Đo trên hệ này
        // (30/07/2026): 6 camera H265 nguồn tổng 11,3 Mbps bị phát lại thành
        // 31,4 Mbps — phình 1,7-3,7 lần, chiếm 3083/6334 gói mỗi giây tức 49%
        // toàn bộ tải gửi WebRTC, mà chi phí đường gửi là THEO TỪNG GÓI. Kiểm
        // riêng bằng cách transcode một file ghi hình: 23,3 MB -> 46,5 MB, đúng
        // 2,00 lần.
        const uint64_t targetBps = encoderBps();
        std::ostringstream launch;
        launch
            << "appsrc name=in is-live=true format=time do-timestamp=true"
            << " max-bytes=0 block=false"
            << " ! video/x-h265,stream-format=byte-stream,alignment=au"
            << " ! h265parse config-interval=-1"
            << " ! mppvideodec ! mpph264enc gop=-1 rc-mode=vbr"
            << " bps=" << targetBps
            // bps-max nới 1,5 lần để cảnh động (mưa, xe chạy) không bị bết,
            // nhưng vẫn có trần thay vì để encoder tự do như bps=0.
            << " bps-max=" << (targetBps * 3 / 2)
            << " ! h264parse config-interval=-1"
            << " ! video/x-h264,stream-format=byte-stream,alignment=au"
            << " ! appsink name=out sync=false max-buffers=30 drop=false";

        GError* err = nullptr;
        m_pipeline = gst_parse_launch(launch.str().c_str(), &err);
        if (!m_pipeline || err) {
            g_printerr("[transcode] camera %s: khong dung duoc pipeline: %s\n",
                       m_cameraId.c_str(),
                       err && err->message ? err->message : "loi khong ro");
            if (err) g_error_free(err);
            stop();
            return false;
        }

        m_appsink = gst_bin_get_by_name(GST_BIN(m_pipeline), "out");
        m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "in");
        if (!m_appsink || !m_appsrc) {
            g_printerr("[transcode] camera %s: thieu appsrc/appsink\n",
                       m_cameraId.c_str());
            stop();
            return false;
        }

        GstAppSinkCallbacks cbs{};
        cbs.new_sample = &TranscodedRtpSource::onNewSample;
        gst_app_sink_set_callbacks(GST_APP_SINK(m_appsink), &cbs, this, nullptr);

        if (GstBus* bus = gst_element_get_bus(m_pipeline)) {
            m_busWatchId = gst_bus_add_watch(bus, &TranscodedRtpSource::onBusMessage, this);
            gst_object_unref(bus);
        }

        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            g_printerr("[transcode] camera %s: khong PLAYING duoc\n",
                       m_cameraId.c_str());
            stop();
            return false;
        }

        m_alive.store(true);
        // Đấu nguồn nền -> appsrc SAU khi PLAYING (yêu cầu của AppSrcBridge). Cầu
        // nối tự đăng ký sink trên nguồn nền và bơm access unit H265 vào đây.
        m_bridge.attach(m_base, m_appsrc);

        g_print("[transcode] camera %s: nguon H264 dung chung bat dau "
                "(transcode H265 mot lan cho moi nguoi xem)\n",
                m_cameraId.c_str());
        return true;
    }

    void stop() {
        m_alive.store(false);
        // Cắt cầu nối TRƯỚC: ngừng bơm H265 vào appsrc rồi mới hạ pipeline.
        m_bridge.detach();
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
        if (m_appsrc) {
            gst_object_unref(m_appsrc);
            m_appsrc = nullptr;
        }
        if (m_pipeline) {
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
    }

    // Fanout giống hệt CameraRtpSource: phiên mới chỉ nhận từ KEYFRAME kế tiếp
    // (bơm giữa GOP một chuỗi P-frame tham chiếu frame chưa có sẽ vỡ hình).
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
        auto* self = static_cast<TranscodedRtpSource*>(user);
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
        auto* self = static_cast<TranscodedRtpSource*>(user);
        switch (GST_MESSAGE_TYPE(msg)) {
            case GST_MESSAGE_ERROR: {
                GError* err = nullptr;
                gchar* dbg = nullptr;
                gst_message_parse_error(msg, &err, &dbg);
                g_printerr("[transcode] camera %s: loi pipeline: %s\n",
                           self->m_cameraId.c_str(),
                           err && err->message ? err->message : "?");
                if (err) g_error_free(err);
                g_free(dbg);
                // Chết: các phiên đang bám sẽ hết media -> watchdog dọn -> trình
                // duyệt nối lại -> phiên mới dựng nguồn transcode mới.
                self->m_alive.store(false);
                break;
            }
            case GST_MESSAGE_EOS:
                self->m_alive.store(false);
                break;
            default:
                break;
        }
        return TRUE;
    }

    std::string m_cameraId;
    // "h264" cố định — đây là điểm khác biệt với nguồn nền H265.
    std::string m_codec = "h264";
    std::shared_ptr<FrameSource> m_base;  // nguồn H265 dùng chung, giữ sống
    stream::GStreamerConfig m_config;

    GstElement* m_pipeline = nullptr;
    GstElement* m_appsrc = nullptr;   // sở hữu (unref trong stop)
    GstElement* m_appsink = nullptr;  // sở hữu (unref trong stop)
    guint m_busWatchId = 0;
    std::atomic<bool> m_alive{false};

    // Cầu nối nguồn nền H265 -> appsrc của pipeline transcode.
    AppSrcBridge m_bridge;

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, Consumer> m_consumers;
    uint64_t m_nextId = 1;
};

}  // namespace stream

#endif  // test_gstreamer_TranscodedRtpSource_hpp
