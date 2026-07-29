#ifndef test_gstreamer_FrameSource_hpp
#define test_gstreamer_FrameSource_hpp

// Nguồn access unit đã parse, cho một hoặc nhiều người tiêu thụ.
//
// Có đúng hai hiện thực và chúng khác nhau ở CHỖ LẤY dữ liệu, không khác ở
// cách giao:
//   - stream::CameraRtpSource   — kéo camera qua RTSP, thời gian thực, DÙNG
//                                 CHUNG cho mọi phiên xem/ghi của camera đó;
//   - stream::PlaybackSource    — đọc file .ts đã ghi trong máy, có seek / đổi
//                                 tốc độ / tạm dừng, RIÊNG cho từng phiên.
//
// Tách interface ra để WebRtcSession (thương lượng SDP, SSRC, DTLS, NACK...)
// dùng lại nguyên vẹn cho cả xem trực tiếp lẫn xem lại — phần khó và dễ vỡ
// nhất của WebRTC chỉ tồn tại một bản.

#include <gst/gst.h>

#include <cstdint>
#include <functional>
#include <string>

namespace stream {

class FrameSource {
public:
    // sink nhận (buffer, caps) MƯỢN: muốn giữ buffer phải tự gst_buffer_ref.
    // Gọi trên thread streaming của nguồn — làm cho nhanh, đừng chặn.
    using Sink = std::function<void(GstBuffer*, GstCaps*)>;

    virtual ~FrameSource() = default;

    // Trả về id để huỷ đăng ký. Người tiêu thụ mới chỉ bắt đầu nhận từ
    // KEYFRAME kế tiếp: bơm giữa GOP một chuỗi P-frame tham chiếu frame chưa
    // có sẽ ra hình vỡ tới tận IDR sau.
    virtual uint64_t addSink(Sink sink) = 0;
    virtual void removeSink(uint64_t id) = 0;

    // false = nguồn đã chết (camera rớt / hết dữ liệu): phiên nên dừng.
    virtual bool alive() const = 0;

    // "h264" / "h265" — quyết định passthrough hay transcode.
    virtual const std::string& codec() const = 0;
};

}  // namespace stream

#endif  // test_gstreamer_FrameSource_hpp
