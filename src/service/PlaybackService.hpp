#ifndef test_gstreamer_PlaybackService_hpp
#define test_gstreamer_PlaybackService_hpp

// Sổ các phiên XEM LẠI qua WebRTC.
//
// Phiên WebRTC vẫn nằm trong WebRtcService (dùng chung watchdog, cùng đường
// thương lượng SDP). Lớp này chỉ giữ thêm đường dây tới PlaybackSource của
// từng phiên để phục vụ lệnh seek / đổi tốc độ / tạm dừng.
//
// Giữ WEAK_PTR chứ không phải shared: chủ sở hữu thật của nguồn là
// WebRtcSession. Phiên bị watchdog dọn (tab đóng đột ngột) -> nguồn tự huỷ ->
// thread feeder dừng, file đóng. Giữ shared ở đây thì một tab bị đóng sẽ để
// lại thread đọc file chạy mãi.

#include "service/PlaybackSource.hpp"
#include "service/WebRtcService.hpp"

#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace playback {

struct PlaybackStatus {
    int64_t positionMs = 0;
    double rate = 1.0;
    bool paused = false;
    bool ended = false;
    // Đang đợi đoạn ghi kế tiếp (sát mép live), KHÔNG phải hết bản ghi.
    bool waiting = false;
    // Số thứ tự lệnh seek đã áp xong (đi kèm mọi bản tin trạng thái)...
    uint64_t seq = 0;
    // ...và số thứ tự lệnh seek VỪA nhận (chỉ có trong trả lời của /control).
    // Client bỏ qua mọi bản tin có seq < seekSeq: đó là vị trí CŨ.
    uint64_t seekSeq = 0;
};

class PlaybackService {
public:
    explicit PlaybackService(std::shared_ptr<webrtc::WebRtcService> webrtc)
        : m_webrtc(std::move(webrtc)) {}

    struct StartResult {
        std::string sessionId;
        std::string answerSdp;
        std::string error;
        bool ok() const { return error.empty() && !answerSdp.empty(); }
    };

    StartResult start(const std::string& cameraId,
                      const std::string& codec,
                      const std::string& offerSdp,
                      const std::string& clientAddressHint,
                      int64_t startWallMs,
                      double rate,
                      stream::PlaybackSource::Loader loader) {
        StartResult out;

        auto source = std::make_shared<stream::PlaybackSource>(
            cameraId, codec, std::move(loader), startWallMs);
        if (rate > 0 && rate != 1.0) source->setRate(rate);
        // Bắt đầu đọc file NGAY, trước cả khi thương lượng xong: bắt tay ICE
        // mất ~0,6s, đủ để feeder mở file và sẵn keyframe đầu tiên — người xem
        // thấy hình gần như cùng lúc kết nối xong.
        source->start();

        // Nguồn JSON trạng thái đẩy xuống trình duyệt qua kênh dữ liệu — thay
        // hẳn việc client hỏi HTTP mỗi giây. weak_ptr để closure không giữ
        // nguồn sống lâu hơn phiên.
        std::weak_ptr<stream::PlaybackSource> weak = source;
        auto provider = [weak]() -> std::string {
            auto locked = weak.lock();
            if (!locked) return "";
            std::ostringstream json;
            json << "{\"seq\":" << locked->appliedSeekSeq()
                 << ",\"positionMs\":" << locked->positionMs()
                 << ",\"rate\":" << locked->rate()
                 << ",\"paused\":" << (locked->paused() ? "true" : "false")
                 << ",\"ended\":" << (locked->ended() ? "true" : "false")
                 << ",\"waiting\":" << (locked->waiting() ? "true" : "false") << "}";
            return json.str();
        };

        auto created = m_webrtc->createSessionWithSource(cameraId, offerSdp,
                                                         clientAddressHint, codec, source,
                                                         std::move(provider));
        if (!created.ok()) {
            source->stop();
            out.error = created.error;
            return out;
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sources[created.sessionId] = source;
            pruneLocked();
        }

        out.sessionId = created.sessionId;
        out.answerSdp = std::move(created.answerSdp);
        return out;
    }

    // Bất kỳ tham số nào cũng có thể bỏ trống (nullptr) = giữ nguyên.
    bool control(const std::string& sessionId,
                 const int64_t* seekToMs,
                 const double* rate,
                 const bool* paused,
                 PlaybackStatus& out) {
        auto source = find(sessionId);
        if (!source) return false;
        m_webrtc->heartbeat(sessionId);

        if (rate) source->setRate(*rate);
        if (paused) source->setPaused(*paused);
        // Seek SAU khi đổi tốc độ: hai lệnh hay đi cùng nhau (bấm x8 rồi nhảy
        // chỗ), và seek là thứ đặt lại mốc nhịp nên phải là lệnh cuối.
        const uint64_t seekSeq = seekToMs ? source->seek(*seekToMs) : 0;

        fill(*source, out);
        out.seekSeq = seekSeq;
        return true;
    }

    bool status(const std::string& sessionId, PlaybackStatus& out) {
        auto source = find(sessionId);
        if (!source) return false;
        m_webrtc->heartbeat(sessionId);
        fill(*source, out);
        return true;
    }

    void destroy(const std::string& sessionId) {
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_sources.erase(sessionId);
        }
        m_webrtc->destroySession(sessionId);
    }

private:
    static void fill(const stream::PlaybackSource& source, PlaybackStatus& out) {
        out.positionMs = source.positionMs();
        out.rate = source.rate();
        out.paused = source.paused();
        out.ended = source.ended();
        out.waiting = source.waiting();
        out.seq = source.appliedSeekSeq();
    }

    std::shared_ptr<stream::PlaybackSource> find(const std::string& sessionId) {
        std::lock_guard<std::mutex> lock(m_mutex);
        auto found = m_sources.find(sessionId);
        if (found == m_sources.end()) return nullptr;
        auto source = found->second.lock();
        if (!source) {
            m_sources.erase(found);
            return nullptr;
        }
        return source;
    }

    void pruneLocked() {
        for (auto it = m_sources.begin(); it != m_sources.end();) {
            it = it->second.expired() ? m_sources.erase(it) : std::next(it);
        }
    }

    std::shared_ptr<webrtc::WebRtcService> m_webrtc;
    std::mutex m_mutex;
    std::unordered_map<std::string, std::weak_ptr<stream::PlaybackSource>> m_sources;
};

}  // namespace playback

#endif  // test_gstreamer_PlaybackService_hpp
