#ifndef test_gstreamer_AppSrcBridge_hpp
#define test_gstreamer_AppSrcBridge_hpp

// Cầu nối một chiều: CameraRtpSource (nguồn dùng chung) -> appsrc của MỘT
// người tiêu thụ (phiên WHEP hoặc phiên ghi hình). Đóng gói đúng phần khó:
// đồng bộ giữa thread streaming của nguồn (đang push) và thread tháo dỡ.
//
// Hai bất biến bắt buộc:
//   1) do-timestamp=true trên appsrc (caller tự đặt trong launch string):
//      buffer từ pipeline nguồn mang PTS theo đồng hồ KHÁC — giữ nguyên là
//      rtppay/muxer tính giờ sai, "connected" mà đen hình / segment lỗi.
//   2) make_writable(ref(buffer)) trước khi push: KHÔNG được để appsrc dán PTS
//      lên buffer dùng chung (các consumer khác cũng đang giữ nó) — bản copy
//      nông chia sẻ vùng nhớ ảnh nên rẻ.

#include "service/FrameSource.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>

#include <cstdint>
#include <memory>
#include <mutex>

namespace stream {

class AppSrcBridge {
public:
    ~AppSrcBridge() { detach(); }

    // source: giữ shared_ptr để nguồn sống theo consumer này.
    // appsrc: MƯỢN (caller sở hữu và unref trong pipeline của mình); phải đã ở
    // trong pipeline PLAYING trước khi attach để buffer chảy được ngay.
    void attach(std::shared_ptr<FrameSource> source, GstElement* appsrc) {
        m_source = std::move(source);
        m_appsrc = appsrc;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_enabled = true;
        }
        m_sinkId = m_source->addSink(
            [this](GstBuffer* buffer, GstCaps* caps) { push(buffer, caps); });
    }

    // Cắt nối AN TOÀN. Thứ tự bắt buộc: hạ cờ dưới khoá (đợi push đang chạy
    // xong, chặn push sau) -> removeSink (nguồn ngừng gọi) -> quên appsrc.
    // Gọi được nhiều lần (destructor + stop() của caller).
    void detach() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_enabled = false;
        }
        if (m_source && m_sinkId != 0) {
            m_source->removeSink(m_sinkId);
            m_sinkId = 0;
        }
        m_appsrc = nullptr;
        m_source.reset();
    }

    bool sourceAlive() const { return m_source && m_source->alive(); }

private:
    void push(GstBuffer* buffer, GstCaps* caps) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_enabled || !m_appsrc) return;
        if (!m_capsSet && caps) {
            gst_app_src_set_caps(GST_APP_SRC(m_appsrc), caps);
            m_capsSet = true;
        }
        GstBuffer* out = gst_buffer_make_writable(gst_buffer_ref(buffer));
        // XOÁ dấu thời gian của nguồn TRƯỚC khi đẩy vào appsrc.
        //
        // do-timestamp=true chỉ dán giờ-đến khi buffer CHƯA có PTS; buffer nào
        // đã mang PTS thì appsrc giữ nguyên. Với nguồn live thì vô hại (PTS của
        // rtspsrc chạy sát đồng hồ phiên nên không ai nhận ra), nhưng với nguồn
        // XEM LẠI thì hỏng nặng: PTS là vị trí TRONG FILE, bấm vào giây thứ 10
        // của một đoạn là buffer mang PTS=10s, và nicesink bên trong webrtcbin
        // (đồng bộ theo đồng hồ như mọi basesink) ngồi ôm dữ liệu tới khi đồng
        // hồ phiên chạy đủ 10 giây mới gửi. Triệu chứng: kết nối "connected",
        // engine báo đang phát, trình duyệt nhận ĐÚNG 0 gói suốt chừng ấy giây
        // rồi mới nhận một cục — càng bấm vào đoạn xa đầu file càng đợi lâu.
        GST_BUFFER_PTS(out) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(out) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(out) = GST_CLOCK_TIME_NONE;
        gst_app_src_push_buffer(GST_APP_SRC(m_appsrc), out);
    }

    std::shared_ptr<FrameSource> m_source;
    GstElement* m_appsrc = nullptr;  // mượn
    uint64_t m_sinkId = 0;
    std::mutex m_mutex;
    bool m_enabled = false;
    bool m_capsSet = false;
};

}  // namespace stream

#endif  // test_gstreamer_AppSrcBridge_hpp
