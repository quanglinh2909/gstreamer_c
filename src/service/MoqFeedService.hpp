#ifndef test_gstreamer_MoqFeedService_hpp
#define test_gstreamer_MoqFeedService_hpp

// Sổ các phiên bơm khung cho MoQ, kèm watchdog dọn phiên chết.
//
// Song song với webrtc::WebRtcService và cố ý giống nó: cùng lấy nguồn từ
// CameraSourceRegistry, cùng chia sẻ bộ transcode H265->H264, cùng kiểu
// watchdog. Khác đúng hai chỗ:
//   * không thương lượng SDP/ICE/DTLS — QUIC lo phần vận chuyển ở tiến trình
//     Python, nên ở đây chỉ còn "mở socket rồi bơm";
//   * LUÔN đưa ra H264. WebRTC còn hỏi trình duyệt có nhận H265 không rồi mới
//     quyết; MoQ thì phía nhận là WebCodecs, mà hỗ trợ HEVC ở đó vá víu tuỳ
//     nền tảng — chọn một đường chắc chắn chạy thay vì một đường lúc được lúc
//     không. Camera H265 vì thế đi qua ĐÚNG bộ transcode dùng chung mà người
//     xem WebRTC đang dùng, không dựng thêm cái thứ hai.

#include "service/CameraSourceRegistry.hpp"
#include "service/MoqFeedSession.hpp"
#include "service/StreamTypes.hpp"
#include "service/TranscodedRtpSource.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace moq {

inline constexpr int kMoqReaperIntervalMs = 5000;

class MoqFeedService {
public:
    MoqFeedService(stream::GStreamerConfig config,
                   std::shared_ptr<stream::CameraSourceRegistry> sources)
        : m_config(std::move(config)), m_sourceRegistry(std::move(sources)) {}

    ~MoqFeedService() { stop(); }

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

        std::unordered_map<std::string, std::shared_ptr<MoqFeedSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sessions.swap(m_sessions);
        }
        for (auto& item : sessions) item.second->stop();
    }

    struct CreateResult {
        std::string sessionId;
        std::string error;
        bool ok() const { return error.empty() && !sessionId.empty(); }
    };

    // XEM TRỰC TIẾP: bám vào nguồn RTSP dùng chung của camera.
    CreateResult createLive(const std::string& cameraId, const std::string& feedId,
                            const std::string& cameraRtsp,
                            const std::string& cameraCodec) {
        CreateResult out;
        if (cameraRtsp.empty()) {
            out.error = "Missing camera RTSP url";
            return out;
        }
        if (!m_sourceRegistry) {
            out.error = "Khong co so nguon camera";
            return out;
        }
        auto base = m_sourceRegistry->acquire(cameraId, cameraRtsp, cameraCodec, m_config);
        if (!base) {
            out.error = "Khong khoi dong duoc nguon RTP cua camera";
            return out;
        }

        std::shared_ptr<stream::FrameSource> source = base;
        if (base->codec() == "h265") {
            source = m_sourceRegistry->acquireTranscoded(cameraId, base, m_config);
            if (!source) {
                out.error = "Khong dung duoc bo transcode H265->H264";
                return out;
            }
        }
        return spawn(cameraId, feedId, std::move(source));
    }

    // XEM LẠI: nguồn do bên gọi dựng (PlaybackSource đọc file trong máy).
    // Bản ghi H265 cũng phải qua transcode, nhưng RIÊNG cho phiên này —
    // mỗi người xem lại ở một mốc thời gian khác nhau nên không dùng chung được.
    CreateResult createWithSource(const std::string& cameraId,
                                  const std::string& feedId,
                                  std::shared_ptr<stream::FrameSource> source) {
        CreateResult out;
        if (!source) {
            out.error = "Missing source";
            return out;
        }
        if (source->codec() == "h265") {
            auto transcoder = std::make_shared<stream::TranscodedRtpSource>(
                cameraId, source, m_config);
            if (!transcoder->start()) {
                out.error = "Khong dung duoc bo transcode H265->H264 cho xem lai";
                return out;
            }
            source = transcoder;
        }
        return spawn(cameraId, feedId, std::move(source));
    }

    bool destroySession(const std::string& sessionId) {
        std::shared_ptr<MoqFeedSession> session;
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

    void destroySessionsForCamera(const std::string& cameraId) {
        std::vector<std::shared_ptr<MoqFeedSession>> doomed;
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

private:
    CreateResult spawn(const std::string& cameraId, const std::string& feedId,
                       std::shared_ptr<stream::FrameSource> source) {
        start();  // watchdog chỉ chạy khi thực sự có người xem

        CreateResult out;
        const std::string sessionId = newSessionId();
        auto session = std::make_shared<MoqFeedSession>(
            sessionId, cameraId, feedId, m_config.moqFeedSocket, "h264",
            std::move(source));
        if (!session->start()) {
            out.error = "Khong noi duoc toi may chu MoQ (" + m_config.moqFeedSocket + ")";
            return out;
        }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sessions[sessionId] = std::move(session);
        }
        out.sessionId = sessionId;
        return out;
    }

    static std::string newSessionId() {
        static thread_local std::mt19937_64 rng{std::random_device{}()};
        std::ostringstream out;
        out << std::hex << rng() << rng();
        return out.str();
    }

    void reaperLoop() {
        std::unique_lock<std::mutex> lock(m_mutex);
        while (m_running) {
            m_wake.wait_for(lock, std::chrono::milliseconds(kMoqReaperIntervalMs));
            if (!m_running) break;

            std::vector<std::shared_ptr<MoqFeedSession>> doomed;
            for (auto it = m_sessions.begin(); it != m_sessions.end();) {
                if (!it->second->alive()) {
                    doomed.push_back(std::move(it->second));
                    it = m_sessions.erase(it);
                } else {
                    ++it;
                }
            }
            if (doomed.empty()) continue;
            lock.unlock();
            for (auto& session : doomed) {
                g_print("[moq] don phien %s (camera %s, %lu khung, %lu khung bo)\n",
                        session->sessionId().c_str(), session->cameraId().c_str(),
                        static_cast<unsigned long>(session->framesSent()),
                        static_cast<unsigned long>(session->framesDropped()));
                session->stop();
            }
            doomed.clear();
            lock.lock();
        }
    }

    stream::GStreamerConfig m_config;
    std::shared_ptr<stream::CameraSourceRegistry> m_sourceRegistry;
    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    std::unordered_map<std::string, std::shared_ptr<MoqFeedSession>> m_sessions;
    bool m_running = false;
    std::thread m_reaper;
};

}  // namespace moq

#endif  // test_gstreamer_MoqFeedService_hpp
