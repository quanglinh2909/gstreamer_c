#ifndef test_gstreamer_WebRtcService_hpp
#define test_gstreamer_WebRtcService_hpp

// Sổ đăng ký các phiên xem WebRTC, kèm watchdog dọn phiên chết.
//
// Mỗi tab trình duyệt đang xem = một WebRtcSession = một kết nối RTSP tới
// server nội bộ. Phiên không được dọn nghĩa là giữ mãi một kết nối vô ích,
// nên watchdog ở đây không phải phần phụ: trình duyệt đóng đột ngột (kill
// process, mất điện, mất wifi) sẽ KHÔNG gửi được DELETE.

#include "service/CameraSourceRegistry.hpp"
#include "service/StreamTypes.hpp"
#include "service/WebRtcSession.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace webrtc {

inline constexpr int kReaperIntervalMs = 5000;

class WebRtcService {
public:
    WebRtcService(stream::GStreamerConfig config,
                  std::shared_ptr<stream::CameraSourceRegistry> sources)
        : m_config(std::move(config)), m_sourceRegistry(std::move(sources)) {}

    ~WebRtcService() { stop(); }

    void start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return;
        m_running = true;
        m_reaper = std::thread([this] { reaperLoop(); });
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) return;
            m_running = false;
        }
        m_wake.notify_all();
        if (m_reaper.joinable()) m_reaper.join();

        std::unordered_map<std::string, std::shared_ptr<WebRtcSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sessions.swap(m_sessions);
        }
        for (auto& item : sessions) item.second->stop();
    }

    struct CreateResult {
        std::string sessionId;
        std::string answerSdp;
        std::string error;
        bool ok() const { return error.empty() && !answerSdp.empty(); }
    };

    // Chạy trên thread HTTP: có block tới vài trăm ms để gom ICE.
    // clientAddressHint: IP trình duyệt nhìn từ proxy — xem WebRtcSession.
    // cameraCodec: "h264"/"h265" — quyết định passthrough hay transcode.
    // cameraRtsp: URL RTSP THẬT của camera, để nguồn dùng chung kéo trực tiếp.
    CreateResult createSession(const std::string& cameraId,
                               const std::string& offerSdp,
                               const std::string& clientAddressHint = "",
                               const std::string& cameraCodec = "h264",
                               const std::string& cameraRtsp = "") {
        start();  // watchdog chỉ chạy khi thực sự có người xem

        CreateResult out;
        if (cameraId.empty()) {
            out.error = "Missing camera id";
            return out;
        }
        if (offerSdp.empty()) {
            out.error = "Empty SDP offer";
            return out;
        }
        if (cameraRtsp.empty()) {
            out.error = "Missing camera RTSP url";
            return out;
        }

        auto source = m_sourceRegistry
            ? m_sourceRegistry->acquire(cameraId, cameraRtsp, cameraCodec, m_config)
            : nullptr;
        if (!source) {
            out.error = "Khong khoi dong duoc nguon RTP cua camera";
            return out;
        }

        // Camera H265: chuẩn bị "lối lấy" nguồn transcode H264 DÙNG CHUNG. Phiên
        // chỉ gọi tới khi thực sự cần (trình duyệt không nhận H265) — dựng lười,
        // và nhiều phiên cùng camera dùng CHUNG một nguồn (transcode một lần).
        std::function<std::shared_ptr<stream::FrameSource>()> transcodedProvider;
        if (source->codec() == "h265" && m_sourceRegistry) {
            auto registry = m_sourceRegistry;
            auto cfg = m_config;
            const std::string cid = cameraId;
            auto base = source;  // shared_ptr<CameraRtpSource>, giữ nền sống
            transcodedProvider =
                [registry, cfg, cid, base]() -> std::shared_ptr<stream::FrameSource> {
                    return registry->acquireTranscoded(cid, base, cfg);
                };
        }

        return negotiate(cameraId, offerSdp, clientAddressHint, cameraCodec,
                         std::move(source), /*heartbeatExpiry=*/false, nullptr,
                         std::move(transcodedProvider));
    }

    // Phiên XEM LẠI: nguồn do bên gọi dựng sẵn (PlaybackSource đọc file trong
    // máy) thay vì lấy từ registry camera. Mọi phần WebRTC còn lại giống hệt
    // xem trực tiếp — đó là lý do WebRtcSession nhận stream::FrameSource chứ
    // không nhận thẳng CameraRtpSource.
    CreateResult createSessionWithSource(
        const std::string& cameraId,
        const std::string& offerSdp,
        const std::string& clientAddressHint,
        const std::string& codec,
        std::shared_ptr<stream::FrameSource> source,
        std::function<std::string()> statusProvider = nullptr) {
        start();

        CreateResult out;
        if (cameraId.empty()) {
            out.error = "Missing camera id";
            return out;
        }
        if (offerSdp.empty()) {
            out.error = "Empty SDP offer";
            return out;
        }
        if (!source) {
            out.error = "Missing playback source";
            return out;
        }
        return negotiate(cameraId, offerSdp, clientAddressHint, codec,
                         std::move(source), /*heartbeatExpiry=*/true,
                         std::move(statusProvider));
    }

    // Gửi nhịp tim cho phiên xem lại (client hỏi trạng thái / điều khiển).
    // false = không còn phiên đó.
    bool heartbeat(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_sessions.find(sessionId);
        if (found == m_sessions.end()) return false;
        found->second->heartbeat();
        return true;
    }

    bool destroySession(const std::string& sessionId) {
        std::shared_ptr<WebRtcSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(sessionId);
            if (found == m_sessions.end()) return false;
            session = std::move(found->second);
            m_sessions.erase(found);
        }
        session->stop();
        return true;
    }

    // Dọn mọi phiên của một camera — gọi khi camera bị sửa/xoá/dừng, vì luồng
    // RTSP phía dưới sắp biến mất.
    void destroySessionsForCamera(const std::string& cameraId) {
        std::vector<std::shared_ptr<WebRtcSession>> doomed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                if (it->second->cameraId() == cameraId) {
                    doomed.push_back(std::move(it->second));
                    it = m_sessions.erase(it);
                } else {
                    ++it;
                }
            }
        }
        for (auto& session : doomed) session->stop();
    }

    size_t viewerCount() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_sessions.size();
    }

    size_t viewerCount(const std::string& cameraId) const {
        std::lock_guard<std::mutex> lock(m_mutex);
        size_t count = 0;
        for (const auto& item : m_sessions) {
            if (item.second->cameraId() == cameraId) ++count;
        }
        return count;
    }

    // Ảnh chụp mọi phiên đang mở, để endpoint "ai đang xem" trả về. Chép ra
    // vector rồi mới snapshot() NGOÀI khoá m_mutex là không cần — snapshot()
    // tự khoá m_mutex RIÊNG của từng phiên, không đụng khoá này; giữ khoá ở đây
    // chỉ để lặp m_sessions an toàn.
    std::vector<WebRtcSession::Info> listSessions() const {
        std::vector<WebRtcSession::Info> out;
        std::lock_guard<std::mutex> lock(m_mutex);
        out.reserve(m_sessions.size());
        for (const auto& item : m_sessions) out.push_back(item.second->snapshot());
        return out;
    }

private:
    // Phần chung của hai đường tạo phiên: dựng phiên, thương lượng SDP NGOÀI
    // khoá (start() block vài trăm ms — giữ khoá suốt thời gian đó sẽ chặn mọi
    // người xem khác và cả watchdog), rồi mới ghi vào sổ.
    CreateResult negotiate(const std::string& cameraId,
                           const std::string& offerSdp,
                           const std::string& clientAddressHint,
                           const std::string& codec,
                           std::shared_ptr<stream::FrameSource> source,
                           bool heartbeatExpiry,
                           std::function<std::string()> statusProvider,
                           std::function<std::shared_ptr<stream::FrameSource>()>
                               transcodedProvider = nullptr) {
        CreateResult out;
        const auto sessionId = newSessionId();
        auto session = std::make_shared<WebRtcSession>(sessionId, cameraId, m_config,
                                                       codec, std::move(source));
        if (heartbeatExpiry) session->useHeartbeatExpiry();
        // Đặt TRƯỚC start(): kênh dữ liệu có thể mở ngay trong lúc thương lượng.
        if (statusProvider) session->setStatusProvider(std::move(statusProvider));
        // Đặt TRƯỚC start(): start() quyết định có cần transcode hay không.
        if (transcodedProvider)
            session->setTranscodedProvider(std::move(transcodedProvider));

        auto result = session->start(offerSdp, clientAddressHint);
        if (!result.ok()) {
            session->stop();
            out.error = result.error.empty() ? "WebRTC negotiation failed" : result.error;
            return out;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sessions[sessionId] = std::move(session);
        }

        out.sessionId = sessionId;
        out.answerSdp = std::move(result.answerSdp);
        return out;
    }

    static std::string newSessionId() {
        // Không cần bí mật mã hoá — id chỉ để trình duyệt tự gọi DELETE đúng
        // phiên của mình. thread_local để nhiều thread HTTP không đụng nhau.
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::ostringstream out;
        out << std::hex << rng() << rng();
        return out.str();
    }

    void reaperLoop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running) {
            m_wake.wait_for(lock, std::chrono::milliseconds(kReaperIntervalMs));
            if (!m_running) break;

            std::vector<std::shared_ptr<WebRtcSession>> doomed;
            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                if (it->second->isExpired()) {
                    doomed.push_back(std::move(it->second));
                    it = m_sessions.erase(it);
                } else {
                    ++it;
                }
            }

            if (doomed.empty()) continue;
            // stop() đưa pipeline về NULL, có thể mất một lúc — nhả khoá ra để
            // người xem mới không phải đợi.
            lock.unlock();
            for (auto& session : doomed) {
                g_print("[webrtc] reaped session %s (camera %s)\n",
                        session->sessionId().c_str(), session->cameraId().c_str());
                session->stop();
            }
            doomed.clear();
            lock.lock();
        }
    }

    stream::GStreamerConfig m_config;
    // Nguồn RTP dùng chung theo camera — chia sẻ với recording. Xem
    // CameraSourceRegistry. WebRtcSession giữ shared_ptr; registry chỉ weak.
    std::shared_ptr<stream::CameraSourceRegistry> m_sourceRegistry;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::unordered_map<std::string, std::shared_ptr<WebRtcSession>> m_sessions;
    bool m_running = false;
    std::thread m_reaper;
};

}  // namespace webrtc

#endif
