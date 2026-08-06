#ifndef test_gstreamer_MoqFeedSession_hpp
#define test_gstreamer_MoqFeedSession_hpp

// Một người xem qua MoQ: bơm access unit sang máy chủ QUIC (tiến trình Python)
// qua unix socket.
//
// VÌ SAO CHỈ CÓ NHIÊU ĐÂY: mọi thứ đắt tiền — kéo RTSP, giải mã, transcode
// H265->H264 — đã do stream::FrameSource lo và ĐANG DÙNG CHUNG với WebRTC và
// với recording. Thêm một người xem MoQ chỉ là thêm một sink vào nguồn có sẵn,
// đúng như thêm một người xem WebRTC. Lớp này vì thế không dựng pipeline nào.
//
// Khung trên dây (mọi số big-endian), khớp app/moq/hub.py:
//     tiêu đề : "MOQF1 " + JSON một dòng + '\n'
//     mỗi khung: u8 cờ | u64 pts_us | u32 độ_dài | Annex-B
//     cờ bit0 = keyframe
//
// CHỐNG NGHẼN: ghi bằng socket không chặn. Sink chạy trên thread streaming của
// nguồn — chặn ở đây là chặn luôn recording và mọi người xem khác của CÙNG
// camera. Nên khi socket đầy: giữ lại phần đuôi của ĐÚNG một khung đang ghi
// dở (không thể bỏ giữa chừng, bên kia sẽ lệch khung), và bỏ hẳn các khung
// tới sau cho đến khi đuôi đó đẩy xong. Người xem chậm thì mất hình một lúc,
// không kéo theo ai khác.

#include <fcntl.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>

#include <gst/gst.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "service/FrameSource.hpp"

namespace moq {

inline constexpr size_t kFrameHeaderBytes = 13;
inline constexpr uint8_t kFlagKeyframe = 0x01;

class MoqFeedSession {
public:
    MoqFeedSession(std::string sessionId, std::string cameraId, std::string feedId,
                   std::string socketPath, std::string codec,
                   std::shared_ptr<stream::FrameSource> source)
        : m_sessionId(std::move(sessionId)),
          m_cameraId(std::move(cameraId)),
          m_feedId(std::move(feedId)),
          m_socketPath(std::move(socketPath)),
          m_codec(std::move(codec)),
          m_source(std::move(source)),
          m_wire(std::make_shared<Wire>()) {}

    ~MoqFeedSession() { stop(); }

    MoqFeedSession(const MoqFeedSession&) = delete;
    MoqFeedSession& operator=(const MoqFeedSession&) = delete;

    const std::string& sessionId() const { return m_sessionId; }
    const std::string& cameraId() const { return m_cameraId; }

    bool start() {
        if (!m_source) return false;
        if (!connect()) return false;

        auto wire = m_wire;
        m_sinkId = m_source->addSink([wire](GstBuffer* buffer, GstCaps*) {
            push(*wire, buffer);
        });
        return true;
    }

    void stop() {
        if (m_source && m_sinkId != 0) {
            m_source->removeSink(m_sinkId);
            m_sinkId = 0;
        }
        // Đóng SAU removeSink, và dưới khoá của chính Wire: CameraRtpSource
        // chép danh sách sink ra rồi mới gọi NGOÀI khoá của nó, nên một lần
        // gọi vẫn có thể đang chạy ngay lúc này. Khoá ở đây là thứ duy nhất
        // ngăn nó ghi vào một fd vừa bị đóng.
        std::lock_guard<std::mutex> lock(m_wire->mutex);
        if (m_wire->fd >= 0) {
            ::close(m_wire->fd);
            m_wire->fd = -1;
        }
        m_wire->alive = false;
    }

    // false = phía Python đã đóng, hoặc nguồn đã chết -> nên dọn phiên.
    bool alive() const {
        if (!m_wire->alive.load()) return false;
        return m_source && m_source->alive();
    }

    uint64_t framesSent() const { return m_wire->frames.load(); }
    uint64_t framesDropped() const { return m_wire->dropped.load(); }

private:
    struct Wire {
        std::mutex mutex;
        int fd = -1;
        std::atomic<bool> alive{false};
        std::atomic<uint64_t> frames{0};
        std::atomic<uint64_t> dropped{0};
        // Đuôi còn lại của MỘT khung đang ghi dở. Không bao giờ chứa quá một
        // khung: khung mới chỉ được bắt đầu khi chỗ này rỗng.
        std::vector<uint8_t> pending;
    };

    bool connect() {
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            g_printerr("[moq] socket() loi: %s\n", std::strerror(errno));
            return false;
        }
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        if (m_socketPath.size() >= sizeof(addr.sun_path)) {
            g_printerr("[moq] duong dan socket qua dai: %s\n", m_socketPath.c_str());
            ::close(fd);
            return false;
        }
        std::snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", m_socketPath.c_str());
        if (::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
            g_printerr("[moq] khong noi duoc toi %s: %s\n",
                       m_socketPath.c_str(), std::strerror(errno));
            ::close(fd);
            return false;
        }

        // Tiêu đề gửi ở chế độ CHẶN: vài chục byte, và nếu nó không qua được
        // thì phiên vô nghĩa — thà hỏng ngay ở đây còn hơn hỏng lặng lẽ sau.
        std::ostringstream header;
        header << "MOQF1 {\"feed\":\"" << m_feedId << "\",\"camera\":\"" << m_cameraId
               << "\",\"session\":\"" << m_sessionId << "\",\"codec\":\"" << m_codec
               << "\"}\n";
        const std::string blob = header.str();
        size_t off = 0;
        while (off < blob.size()) {
            const ssize_t n = ::send(fd, blob.data() + off, blob.size() - off, MSG_NOSIGNAL);
            if (n <= 0) {
                g_printerr("[moq] gui tieu de loi: %s\n", std::strerror(errno));
                ::close(fd);
                return false;
            }
            off += static_cast<size_t>(n);
        }

        // Bộ đệm gửi lớn: một keyframe 1080p có thể vài trăm KB, mặc định của
        // hệ (~200 KB) làm khung đầu mỗi GOP luôn phải đi đường "đuôi còn lại".
        int sndbuf = 2 << 20;
        ::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

        std::lock_guard<std::mutex> lock(m_wire->mutex);
        m_wire->fd = fd;
        m_wire->alive.store(true);
        return true;
    }

    static void push(Wire& wire, GstBuffer* buffer) {
        if (!buffer) return;
        std::lock_guard<std::mutex> lock(wire.mutex);
        if (wire.fd < 0 || !wire.alive.load()) return;

        if (!flushPending(wire)) return;
        if (!wire.pending.empty()) {          // đuôi cũ vẫn chưa đi hết
            wire.dropped.fetch_add(1);
            return;
        }

        GstMapInfo map;
        if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) return;

        const bool keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        const uint64_t ptsUs =
            GST_BUFFER_PTS_IS_VALID(buffer) ? GST_BUFFER_PTS(buffer) / 1000 : 0;

        uint8_t head[kFrameHeaderBytes];
        head[0] = keyframe ? kFlagKeyframe : 0;
        for (int i = 0; i < 8; ++i) head[1 + i] = (ptsUs >> (56 - 8 * i)) & 0xFF;
        const uint32_t length = static_cast<uint32_t>(map.size);
        for (int i = 0; i < 4; ++i) head[9 + i] = (length >> (24 - 8 * i)) & 0xFF;

        iovec parts[2];
        parts[0].iov_base = head;
        parts[0].iov_len = sizeof(head);
        parts[1].iov_base = map.data;
        parts[1].iov_len = map.size;

        msghdr msg{};
        msg.msg_iov = parts;
        msg.msg_iovlen = 2;
        const ssize_t sent = ::sendmsg(wire.fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
        const size_t total = sizeof(head) + map.size;

        if (sent < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                wire.dropped.fetch_add(1);
            } else {
                wire.alive.store(false);   // EPIPE: người xem đã đi
            }
        } else if (static_cast<size_t>(sent) < total) {
            // Ghi dở: giữ đúng phần đuôi lại, KHÔNG được bỏ.
            wire.pending.assign(total - static_cast<size_t>(sent), 0);
            size_t copied = 0;
            size_t skip = static_cast<size_t>(sent);
            for (const auto& part : parts) {
                const uint8_t* base = static_cast<const uint8_t*>(part.iov_base);
                if (skip >= part.iov_len) { skip -= part.iov_len; continue; }
                const size_t take = part.iov_len - skip;
                std::memcpy(wire.pending.data() + copied, base + skip, take);
                copied += take;
                skip = 0;
            }
            wire.frames.fetch_add(1);
        } else {
            wire.frames.fetch_add(1);
        }

        gst_buffer_unmap(buffer, &map);
    }

    // true = socket còn sống (kể cả khi đuôi chưa đẩy hết).
    static bool flushPending(Wire& wire) {
        while (!wire.pending.empty()) {
            const ssize_t n = ::send(wire.fd, wire.pending.data(), wire.pending.size(),
                                     MSG_NOSIGNAL | MSG_DONTWAIT);
            if (n < 0) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
                wire.alive.store(false);
                return false;
            }
            wire.pending.erase(wire.pending.begin(), wire.pending.begin() + n);
        }
        return true;
    }

    std::string m_sessionId;
    std::string m_cameraId;
    std::string m_feedId;
    std::string m_socketPath;
    std::string m_codec;
    std::shared_ptr<stream::FrameSource> m_source;
    std::shared_ptr<Wire> m_wire;
    uint64_t m_sinkId = 0;
};

}  // namespace moq

#endif  // test_gstreamer_MoqFeedSession_hpp
