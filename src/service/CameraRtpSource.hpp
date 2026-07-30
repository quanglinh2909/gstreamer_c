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
#include <cstdlib>
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
    uint64_t bitrateBps() const override { return m_bitrateBps.load(); }

    // Giao thức vận chuyển RTP của nguồn dùng chung. Mặc định `tcp` — đã kiểm
    // chứng là bền khi mạng nghẽn, và burst IDR 1080p/4K không bị rớt gói (rớt
    // gói giữa burst IDR = khung XANH cho MỌI người dùng nguồn này: ghi hình,
    // AI, người xem live).
    //
    // AI_RTSP_PROTOCOLS=udp có thể đo thử ở lắp đặt khác, nhưng ĐỪNG kỳ vọng
    // giảm CPU: đo A/B trên hệ này (16 camera) ra TCP 160,0% — UDP 160,0%,
    // không chênh một chút nào, mà UDP còn kém ổn định hơn. Xem chú thích ở
    // chuỗi launch bên dưới.
    // Chỉ SETUP luồng video, bỏ audio ngay ở rtspsrc (xem onSelectStream).
    static bool videoOnly() {
        static const bool on = [] {
            const char* e = std::getenv("AI_RTSP_VIDEO_ONLY");
            return !(e && e[0] == '0');
        }();
        return on;
    }

    static const char* rtspProtocols() {
        static const std::string v = [] {
            const char* e = std::getenv("AI_RTSP_PROTOCOLS");
            return std::string(e && *e ? e : "tcp");
        }();
        return v.c_str();
    }

    // ĐỪNG THỬ LẠI: tự nhận RTP interleaved để bỏ rtpbin.
    //
    // Đã dựng đủ (30/07/2026): client RTSP riêng bằng GstRTSPConnection, nhận
    // RTP interleaved trên chính socket TCP, đẩy vào appsrc ! rtph26Xdepay.
    // Tầng vận chuyển ĐÚNG — 60 000+ gói, 0 gói RTP lỗi, 0 lệch seqnum, cùng
    // nhịp access unit và cùng nhịp keyframe như rtspsrc.
    //
    // NHƯNG KHÔNG DÙNG ĐƯỢC: nhánh AI (appsrc ! decodebin, người tiêu thụ DUY
    // NHẤT có giải mã) chết. `mppvideodec` khớp khung ra với khung vào theo dấu
    // thời gian; thiếu rtpjitterbuffer là nó bỏ MỌI khung —
    // "MPP is not able to generate pts" rồi "can't process this frame" (thấy
    // bằng GST_DEBUG=mpp*:4). Đo được 320-928 AU vào, 0 khung ra; ghi hình,
    // snapshot và WebRTC vẫn tốt nên rất dễ tưởng là ổn.
    //
    // Thêm lại RIÊNG rtpjitterbuffer để rải nhịp: vẫn không sửa được AI (5/16
    // job), MÀ thread về đúng 275 — tức PHẦN TIẾT KIỆM NẰM CHÍNH Ở
    // rtpjitterbuffer, không ở bộ máy của rtspsrc. Bỏ nó thì AI chết; giữ nó
    // thì không còn gì để tiết kiệm. Lập luận "TCP không mất gói nên
    // jitterbuffer vô ích" là SAI ở chỗ đó.
    //
    // Đã loại trừ bằng đo (đừng điều tra lại): mất/đảo gói, caps, framerate,
    // SPS/PPS, cờ keyframe, codec, PTS không hợp lệ, dồn cụm (đường tự viết
    // dồn cụm ÍT HƠN rtspsrc), bão hoà CPU, appsrc chặn.

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
            << " latency=" << latency << " protocols=" << rtspProtocols()
            // Đã THỬ do-retransmission=false + buffer-mode=none để giảm CPU
            // jitterbuffer trên TCP — ĐO LẠI: KHÔNG giảm CPU. Đã revert về mặc
            // định TCP đã kiểm chứng (bền hơn khi mạng nghẽn).
            //
            // ĐO A/B 29/07/2026 (16 camera, 60s mỗi mốc sau 100s ổn định):
            // TCP 160,0% — UDP 160,0%. KHÔNG chênh một chút nào, mà UDP còn
            // đẻ 9 cảnh báo/reconnect so với 1. Câu "lever thật là UDP" ở bản
            // chú thích cũ là SAI — chi phí ~38% của nhóm thread `src` không
            // phụ thuộc giao thức vận chuyển. Đừng thử lại.
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

        // BỎ LUỒNG AUDIO NGAY TỪ ĐẦU, đừng để capsfilter phía sau lọc.
        //
        // Capsfilter `media=video` chỉ chặn dữ liệu Ở HẠ NGUỒN — rtspsrc vẫn
        // SETUP luồng audio, vẫn dựng đủ một rtpjitterbuffer + rtpsession +
        // thread RTCP + thread timer cho nó, và vẫn nhận + bóc + xếp lại từng
        // gói audio rồi mới bị bỏ. Đo trên hệ này: 32 thread rtpjitterbuffer và
        // 32 thread rtpsession-rtcp cho 16 camera, vì 4/6 camera kiểm thử có
        // luồng audio trong SDP. Toàn bộ chỗ đó là công đổ đi.
        //
        // `select-stream` được phát TRƯỚC khi cấu hình luồng; trả FALSE thì
        // rtspsrc bỏ hẳn luồng đó (không pad, không jitterbuffer, không nhận
        // gói). Chi phí RTP ở đây là theo TỪNG GÓI nên bỏ ~50 gói/s audio mỗi
        // camera là đáng, chưa kể tiết kiệm hẳn 3 thread mỗi camera.
        // AI_RTSP_VIDEO_ONLY=0 để tắt (cửa thoát nếu có camera lạ khai SDP
        // không có trường `media` và video bị bỏ oan).
        if (videoOnly()) {
            if (GstElement* src = gst_bin_get_by_name(GST_BIN(m_pipeline), "src")) {
                g_signal_connect(src, "select-stream",
                                 G_CALLBACK(&CameraRtpSource::onSelectStream), this);
                gst_object_unref(src);
            }
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

    // Chỉ nhận luồng VIDEO (xem chỗ g_signal_connect trong start()). Trả FALSE
    // là rtspsrc bỏ hẳn luồng, không tốn jitterbuffer/rtpsession/RTCP/thread.
    // Không đọc được `media` thì trả TRUE để giữ đúng hành vi cũ, tránh vô tình
    // bỏ mất video của một camera lạ.
    static gboolean onSelectStream(GstElement*, guint num, GstCaps* caps,
                                   gpointer user) {
        auto* self = static_cast<CameraRtpSource*>(user);
        const GstStructure* s = caps ? gst_caps_get_structure(caps, 0) : nullptr;
        const gchar* media = s ? gst_structure_get_string(s, "media") : nullptr;
        if (!media) return TRUE;
        const gboolean keep = (g_strcmp0(media, "video") == 0) ? TRUE : FALSE;
        if (!keep) {
            g_print("[rtpsrc] camera %s: bo luong %u (media=%s)\n",
                    self ? self->m_cameraId.c_str() : "?", num, media);
        }
        return keep;
    }

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

        // Đo bitrate thật của luồng bằng TRUNG BÌNH LUỸ KẾ (tổng byte / tổng
        // thời gian), công bố khi đã có ít nhất 2 giây dữ liệu. Rẻ: một phép
        // cộng mỗi khung. Đây là số duy nhất cho phép TranscodedRtpSource đặt
        // bitrate encoder theo ĐÚNG camera — xem FrameSource::bitrateBps.
        //
        // KHÔNG dùng cửa sổ tức thời. Đã thử cửa sổ 1 giây và HỎNG: video có
        // GOP nên một giây trúng khung IDR đo ra gấp rưỡi tới gấp đôi mức thật
        // — camera babdd763 ra 4,65 Mbps (thực ~3,3), encoder bị đặt 7,44 Mbps,
        // tức CÒN TỆ HƠN lúc chưa sửa. Trung bình luỹ kế thì càng chạy càng
        // đúng và không bao giờ vọt vì một khung lớn.
        //
        // Ngưỡng 2 giây là thoả hiệp: `bps` của mpph264enc chỉ đặt được lúc
        // dựng pipeline, nên phải có số TRƯỚC khi người xem đầu tiên vào; với
        // cửa sổ 5 giây thì 4/6 camera transcode khởi động khi chưa có số nào.
        const gint64 nowUs = g_get_monotonic_time();
        m_rateBytes += gst_buffer_get_size(buffer);
        if (m_rateSinceUs == 0) {
            m_rateSinceUs = nowUs;
        } else if (nowUs - m_rateSinceUs >= 2 * G_USEC_PER_SEC) {
            m_bitrateBps.store(m_rateBytes * 8 * G_USEC_PER_SEC /
                               static_cast<uint64_t>(nowUs - m_rateSinceUs));
        }

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
    // Đo bitrate: chỉ thread appsink ghi m_rateBytes/m_rateSinceUs; kết quả
    // công bố qua atomic vì TranscodedRtpSource đọc từ thread khác.
    std::atomic<uint64_t> m_bitrateBps{0};
    uint64_t m_rateBytes = 0;
    gint64 m_rateSinceUs = 0;

    mutable std::mutex m_mutex;
    std::unordered_map<uint64_t, Consumer> m_consumers;
    uint64_t m_nextId = 1;
};

}  // namespace stream

#endif  // test_gstreamer_CameraRtpSource_hpp
