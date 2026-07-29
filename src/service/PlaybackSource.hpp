#ifndef test_gstreamer_PlaybackSource_hpp
#define test_gstreamer_PlaybackSource_hpp

// Nguồn XEM LẠI: đọc thẳng các file .ts đã ghi trong máy và bơm access unit ra
// cho một phiên WebRTC, có seek / đổi tốc độ / tạm dừng.
//
// Vì sao tồn tại (thay cho HLS):
//   Với HLS, mỗi cú bấm vào timeline phải tải lại một playlist mô tả CẢ NGÀY —
//   đo được 1,17 MB / 9.122 dòng — rồi dựng lại player từ đầu; chi phí đó tăng
//   theo độ dài ngày chứ không theo thứ người dùng muốn xem. Ở đây phiên WebRTC
//   mở MỘT LẦN, mọi cú bấm sau đó chỉ là một lệnh "seek" gửi qua HTTP: engine
//   nhảy tới file chứa mốc đó, seek trong file tới keyframe gần nhất rồi bơm
//   tiếp. Không manifest, không dựng lại player, chi phí không đổi dù xem ngày
//   dài bao nhiêu.
//
// Nhịp phát nằm ở ĐÂY chứ không ở trình duyệt: feeder đọc trước rồi ngủ cho
// tới đúng lúc phải gửi, nên "tốc độ x4" nghĩa là mỗi giây thực gửi đi 4 giây
// nội dung. Nhờ vậy mới làm được thứ HLS không làm nổi: từ x4 trở lên chỉ gửi
// KEYFRAME, băng thông và công giải mã giảm hàng chục lần mà hình vẫn nhảy đều
// (bản ghi có 1 IDR mỗi giây nên x8 = 8 hình/giây, x16 = 16 hình/giây — càng
// tua nhanh càng mượt, ngược hẳn với cách tua của trình duyệt).
//
// Mỗi phiên xem lại có nguồn RIÊNG (khác CameraRtpSource vốn dùng chung theo
// camera): hai người xem lại hai mốc thời gian khác nhau thì không có gì để
// chia sẻ.

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include "service/FrameSource.hpp"
#include "service/StreamTypes.hpp"

namespace stream {

// Từ tốc độ này trở lên chỉ gửi keyframe. Xem giải thích ở đầu file.
inline constexpr double kKeyframeOnlyRate = 4.0;

// Chênh lệch tối đa giữa hai access unit liên tiếp mà còn coi là "liền mạch".
// Quá mức này (khoảng không ghi, hoặc vừa seek) thì dời mốc nhịp thay vì ngủ
// chờ thật — người xem không phải ngồi đợi khoảng trống của camera.
inline constexpr int64_t kPacingResyncMs = 1500;

// Không tìm thấy đoạn nào để phát liên tục ngần này thì mới coi là hết bản ghi.
// Phải lớn hơn độ dài một đoạn (4s) cộng thời gian ghi vào DB, không thì bám
// mép live sẽ nhấp nháy báo "hết bản ghi".
inline constexpr int64_t kEndedGraceMs = 10'000;

struct PlaybackSegment {
    std::string id;
    std::string path;
    int64_t startMs = 0;
    int64_t endMs = 0;
};

class PlaybackSource : public FrameSource {
public:
    // loader(fromMs, toMs) trả về các đoạn 'complete' đã sắp theo thời gian.
    // Gọi TỪ THREAD FEEDER nên phải an toàn đa luồng (bên gọi dùng pool DB của
    // oatpp — đã an toàn sẵn).
    using Loader = std::function<std::vector<PlaybackSegment>(int64_t, int64_t)>;

    PlaybackSource(std::string cameraId, std::string codec, Loader loader,
                   int64_t startWallMs)
        : m_cameraId(std::move(cameraId)),
          m_codec(std::move(codec)),
          m_loader(std::move(loader)),
          m_positionMs(startWallMs),
          m_targetMs(startWallMs),
          m_catchUpToMs(startWallMs) {}

    ~PlaybackSource() override { stop(); }

    PlaybackSource(const PlaybackSource&) = delete;
    PlaybackSource& operator=(const PlaybackSource&) = delete;

    bool start() {
        if (m_thread.joinable()) return true;
        m_alive.store(true);
        m_thread = std::thread([this] { feederLoop(); });
        return true;
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            m_stopping = true;
        }
        m_cmdWake.notify_all();
        if (m_thread.joinable()) m_thread.join();
        m_alive.store(false);
    }

    // ─── FrameSource ────────────────────────────────────────────────
    uint64_t addSink(Sink sink) override {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        const uint64_t id = m_nextSinkId++;
        m_consumers.emplace(id, Consumer{std::move(sink), true});
        return id;
    }

    void removeSink(uint64_t id) override {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        m_consumers.erase(id);
    }

    bool alive() const override { return m_alive.load(); }
    const std::string& codec() const override { return m_codec; }

    // ─── Điều khiển (gọi từ thread HTTP) ────────────────────────────
    // Trả về SỐ THỨ TỰ của lệnh seek này. Vị trí báo về chỉ đáng tin khi
    // appliedSeekSeq() đã bằng số đó — trước lúc feeder kịp áp lệnh, nguồn vẫn
    // đang ở chỗ CŨ và vẫn đẩy vị trí cũ xuống trình duyệt (nhịp 500ms). Không
    // có số này thì con trỏ timeline bị kéo ngược về chỗ cũ một nhịp rồi mới
    // nhảy tới chỗ vừa bấm.
    uint64_t seek(int64_t wallMs) {
        uint64_t seq;
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            m_targetMs = wallMs;
            m_seekPending = true;
            seq = ++m_seekSeq;
        }
        m_cmdWake.notify_all();
        return seq;
    }

    // Số thứ tự lệnh seek MỚI NHẤT ĐÃ ÁP (feeder đã nhảy chỗ xong).
    uint64_t appliedSeekSeq() const { return m_appliedSeekSeq.load(); }

    void setRate(double rate) {
        if (rate < 0.1) rate = 0.1;
        if (rate > 64.0) rate = 64.0;
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            m_rate = rate;
            // Nhịp tính theo mốc cũ sẽ sai ngay khi đổi tốc độ.
            m_resyncPacing = true;
        }
        m_cmdWake.notify_all();
    }

    void setPaused(bool paused) {
        {
            std::lock_guard<std::mutex> lock(m_cmdMutex);
            m_paused = paused;
            if (!paused) m_resyncPacing = true;
        }
        m_cmdWake.notify_all();
    }

    int64_t positionMs() const { return m_positionMs.load(); }
    double rate() const {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        return m_rate;
    }
    bool paused() const {
        std::lock_guard<std::mutex> lock(m_cmdMutex);
        return m_paused;
    }
    // Hết dữ liệu phía sau mốc hiện tại (đã phát tới cuối bản ghi).
    bool ended() const { return m_ended.load(); }
    // Đang ĐỢI đoạn kế: đã phát tới sát mép live, đoạn đang được ghi chưa đóng
    // nên chưa có gì để phát. Khác hẳn "hết bản ghi" — vài giây nữa là có.
    bool waiting() const { return m_waiting.load(); }

private:
    struct Consumer {
        Sink sink;
        bool needKeyframe = true;
    };

    // ─── Thread feeder ──────────────────────────────────────────────
    void feederLoop() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(m_cmdMutex);
                if (m_stopping) break;

                if (m_seekPending) {
                    m_seekPending = false;
                    const int64_t target = m_targetMs;
                    const uint64_t seq = m_seekSeq;
                    lock.unlock();
                    closeFile();
                    m_positionMs.store(target);
                    m_ended.store(false);
                    m_waiting.store(false);
                    m_noDataSinceMs = 0;
                    m_anchorValid = false;
                    m_catchUpToMs = target;
                    // Người xem vừa nhảy chỗ: mọi consumer phải chờ IDR đầu
                    // tiên của đoạn mới, không thì dính P-frame mồ côi.
                    requireKeyframe();
                    m_appliedSeekSeq.store(seq);
                    continue;
                }
                if (m_paused) {
                    m_cmdWake.wait_for(lock, std::chrono::milliseconds(50));
                    continue;
                }
                if (m_resyncPacing) {
                    m_resyncPacing = false;
                    m_anchorValid = false;
                }
                m_currentRate = m_rate;
            }

            if (!m_fileOpen && !openNextFile()) {
                // Chưa có đoạn nào để mở. KHÔNG kết luận "hết bản ghi" ngay:
                // bám sát mép live là chuyện bình thường — đoạn đang được ghi
                // chỉ vào danh sách sau khi splitmuxsink đóng nó (mỗi 4 giây),
                // nên vài giây trống ở đây chỉ nghĩa là phải đợi đoạn kế.
                // Kết luận sớm thì người xem thấy "Đã hết bản ghi" trong khi
                // camera vẫn đang ghi ngon lành.
                const int64_t now = nowMs();
                if (m_noDataSinceMs == 0) m_noDataSinceMs = now;

                // Sát mép live thì KHÔNG bao giờ là "hết bản ghi": đoạn đang
                // ghi chỉ vào danh sách khi splitmuxsink đóng nó, mà độ dài
                // đoạn tuỳ camera (4s hay 60s). Lấy chính độ dài đoạn dài nhất
                // vừa đọc được làm thước đo thay vì ghim cứng một con số.
                const int64_t edgeWindowMs = 2 * m_maxSegmentMs + 5'000;
                const bool atLiveEdge = now - m_positionMs.load() < edgeWindowMs;
                if (atLiveEdge) {
                    m_waiting.store(true);
                    m_ended.store(false);
                } else if (now - m_noDataSinceMs > kEndedGraceMs) {
                    m_waiting.store(false);
                    m_ended.store(true);
                }

                std::unique_lock<std::mutex> lock(m_cmdMutex);
                m_cmdWake.wait_for(lock, std::chrono::milliseconds(200));
                continue;
            }

            // Mở được file = còn dữ liệu. Cờ "hết bản ghi" phải TẮT ở đây, nếu
            // không nó chỉ bật một chiều: qua được một quãng trống là màn hình
            // báo hết bản ghi vĩnh viễn dù video vẫn đang chạy.
            m_noDataSinceMs = 0;
            m_ended.store(false);
            m_waiting.store(false);

            pumpOneBuffer();
        }
        closeFile();
        m_alive.store(false);
    }

    // Mở file chứa m_positionMs (hoặc đoạn kế tiếp nếu mốc rơi vào khoảng
    // trống) và seek trong file tới đúng vị trí. false = hết dữ liệu.
    bool openNextFile() {
        const int64_t want = m_positionMs.load();

        // Nạp danh sách quanh mốc: một cửa sổ đủ rộng để phát tiếp một lúc mà
        // không phải hỏi DB mỗi đoạn, nhưng không phải cả ngày.
        if (!m_haveWindow || want < m_windowFromMs || want >= m_windowToMs - 1000) {
            m_windowFromMs = want - 1000;
            m_windowToMs = want + kWindowMs;
            m_segments = m_loader ? m_loader(m_windowFromMs, m_windowToMs)
                                  : std::vector<PlaybackSegment>{};
            m_haveWindow = true;
            rememberSegmentLength();
        }

        const PlaybackSegment* chosen = nullptr;
        for (const auto& seg : m_segments) {
            if (seg.endMs <= want) continue;
            chosen = &seg;
            break;
        }
        if (!chosen) {
            // Có thể chỉ là hết cửa sổ chứ chưa hết bản ghi: thử một cửa sổ
            // nữa trước khi kết luận.
            if (m_windowToMs < nowMs() + kWindowMs) {
                m_windowFromMs = m_windowToMs;
                m_windowToMs = m_windowFromMs + kWindowMs;
                m_segments = m_loader ? m_loader(m_windowFromMs, m_windowToMs)
                                      : std::vector<PlaybackSegment>{};
                for (const auto& seg : m_segments) {
                    if (seg.endMs <= want) continue;
                    chosen = &seg;
                    break;
                }
            }
            if (!chosen) return false;  // người gọi quyết định có phải "hết" không
        }

        // Mốc rơi vào khoảng KHÔNG ghi -> nhảy thẳng tới đầu đoạn kế tiếp thay
        // vì ngồi đợi hết khoảng trống.
        const int64_t offsetMs = std::max<int64_t>(0, want - chosen->startMs);
        if (chosen->startMs > want) {
            m_positionMs.store(chosen->startMs);
            m_anchorValid = false;
        }

        m_segStartMs = chosen->startMs;
        m_segEndMs = chosen->endMs;
        return openFileAt(chosen->path, offsetMs);
    }

    bool openFileAt(const std::string& path, int64_t offsetMs) {
        closeFile();

        const bool h265 = (m_codec == "h265");
        std::ostringstream launch;
        launch
            << "filesrc location=" << stream::quoteLaunchValue(path)
            // tsdemux có pad động; gst_parse_launch tự hoãn việc nối lại nên
            // viết "d. ! ..." là đủ, không cần bắt tín hiệu pad-added.
            << " ! tsdemux name=d d. ! "
            << (h265 ? "h265parse" : "h264parse") << " config-interval=-1"
            << " ! " << (h265 ? "video/x-h265" : "video/x-h264")
            << ",stream-format=byte-stream,alignment=au"
            // drop=false + max-buffers nhỏ: khi feeder đang ngủ chờ đúng nhịp
            // thì pipeline tự khựng lại. Đó CHÍNH là cơ chế điều tiết — thiếu
            // nó thì cả file bị đọc tuốt vào RAM trong tích tắc.
            << " ! appsink name=out sync=false max-buffers=8 drop=false";

        GError* err = nullptr;
        m_filePipeline = gst_parse_launch(launch.str().c_str(), &err);
        if (!m_filePipeline || err) {
            g_printerr("[playback] %s: khong dung duoc pipeline doc file: %s\n",
                       m_cameraId.c_str(),
                       err && err->message ? err->message : "loi khong ro");
            if (err) g_error_free(err);
            closeFile();
            // Bỏ qua đoạn hỏng, đi tiếp đoạn sau — một file lỗi không được
            // làm chết cả phiên xem lại.
            m_positionMs.store(m_segEndMs);
            return false;
        }

        m_fileAppsink = gst_bin_get_by_name(GST_BIN(m_filePipeline), "out");
        if (!m_fileAppsink) {
            closeFile();
            m_positionMs.store(m_segEndMs);
            return false;
        }

        // PAUSED + đợi preroll rồi mới seek được: seek trên pipeline chưa có
        // trạng thái sẽ bị bỏ qua im lặng.
        gst_element_set_state(m_filePipeline, GST_STATE_PAUSED);
        gst_element_get_state(m_filePipeline, nullptr, nullptr, 3 * GST_SECOND);

        if (offsetMs > 0) {
            // SNAP_BEFORE + KEY_UNIT: lùi về keyframe gần nhất TRƯỚC mốc, vì
            // bắt đầu giữa GOP thì trình duyệt không có gì để giải mã.
            gst_element_seek_simple(
                m_filePipeline, GST_FORMAT_TIME,
                static_cast<GstSeekFlags>(GST_SEEK_FLAG_FLUSH |
                                          GST_SEEK_FLAG_KEY_UNIT |
                                          GST_SEEK_FLAG_SNAP_BEFORE),
                static_cast<gint64>(offsetMs) * GST_MSECOND);
        }
        gst_element_set_state(m_filePipeline, GST_STATE_PLAYING);

        m_fileOpen = true;
        return true;
    }

    void closeFile() {
        if (m_fileAppsink) {
            gst_object_unref(m_fileAppsink);
            m_fileAppsink = nullptr;
        }
        if (m_filePipeline) {
            gst_element_set_state(m_filePipeline, GST_STATE_NULL);
            gst_object_unref(m_filePipeline);
            m_filePipeline = nullptr;
        }
        m_fileOpen = false;
    }

    void pumpOneBuffer() {
        GstSample* sample = gst_app_sink_try_pull_sample(
            GST_APP_SINK(m_fileAppsink), 200 * GST_MSECOND);
        if (!sample) {
            if (gst_app_sink_is_eos(GST_APP_SINK(m_fileAppsink))) {
                // Hết đoạn: đi tiếp đoạn sau. Khoảng trống (nếu có) được
                // openNextFile() nhảy qua.
                closeFile();
                m_positionMs.store(m_segEndMs);
            }
            return;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        GstCaps* caps = gst_sample_get_caps(sample);
        if (!buffer) {
            gst_sample_unref(sample);
            return;
        }

        // Vị trí thật trong file = stream time của buffer. Dùng segment của
        // sample chứ không lấy PTS thô: sau khi seek, PTS vẫn nằm trong trục
        // thời gian gốc của file .ts (bắt đầu từ một giá trị bất kỳ), chỉ
        // stream time mới tính từ 0.
        int64_t inFileMs = -1;
        if (const GstSegment* seg = gst_sample_get_segment(sample)) {
            const guint64 pts = GST_BUFFER_PTS(buffer);
            if (GST_CLOCK_TIME_IS_VALID(pts)) {
                const guint64 stream =
                    gst_segment_to_stream_time(seg, GST_FORMAT_TIME, pts);
                if (GST_CLOCK_TIME_IS_VALID(stream)) {
                    inFileMs = static_cast<int64_t>(stream / GST_MSECOND);
                }
            }
        }
        const int64_t wallMs =
            inFileMs >= 0 ? m_segStartMs + inFileMs : m_positionMs.load();

        const bool keyframe =
            !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);

        // Nhịp: ngủ cho tới đúng lúc access unit này phải rời máy. Đây là chỗ
        // duy nhất quyết định tốc độ phát.
        pace(wallMs);
        m_positionMs.store(wallMs);

        // Tua nhanh thì chỉ gửi keyframe — xem đầu file.
        if (m_currentRate >= kKeyframeOnlyRate && !keyframe) {
            gst_sample_unref(sample);
            return;
        }

        deliver(buffer, caps, keyframe);
        gst_sample_unref(sample);
    }

    void pace(int64_t wallMs) {
        // BẮT KỊP sau khi seek. Seek trong file luôn lùi về keyframe TRƯỚC mốc
        // người dùng bấm (bắt đầu giữa GOP thì không giải mã được), nên giữa
        // keyframe đó và mốc bấm có tới cả một GOP nội dung. Phát đoạn bù đó
        // đúng nhịp thật nghĩa là bấm vào giữa GOP thì phải ngồi đợi hết GOP
        // mới thấy hình — đúng thứ ta đang cố bỏ đi. Nên đoạn bù đẩy đi hết tốc
        // lực; trình duyệt giải mã vèo một cái rồi vào nhịp bình thường.
        if (m_catchUpToMs > 0) {
            if (wallMs < m_catchUpToMs) return;
            m_catchUpToMs = 0;
            m_anchorValid = false;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!m_anchorValid) {
            m_anchorWallMs = wallMs;
            m_anchorReal = now;
            m_anchorValid = true;
            return;
        }

        const double rate = m_currentRate > 0 ? m_currentRate : 1.0;
        const int64_t contentAheadMs = wallMs - m_anchorWallMs;
        if (contentAheadMs < 0 || contentAheadMs > kPacingResyncMs * 20) {
            // Nhảy quá xa (đoạn mới sau khoảng trống dài): đặt lại mốc.
            m_anchorWallMs = wallMs;
            m_anchorReal = now;
            return;
        }

        const auto due = m_anchorReal + std::chrono::microseconds(
                                            static_cast<int64_t>(contentAheadMs * 1000.0 / rate));
        if (due <= now) {
            // Đang chậm hơn nhịp (đĩa chậm / vừa mở file): gửi ngay, và nếu
            // trễ nhiều thì đặt lại mốc để khỏi cố "đuổi" bằng cách xả một
            // tràng buffer vào trình duyệt.
            if (now - due > std::chrono::milliseconds(kPacingResyncMs)) {
                m_anchorWallMs = wallMs;
                m_anchorReal = now;
            }
            return;
        }

        // Ngủ có thể bị đánh thức bởi lệnh seek/pause/đổi tốc độ.
        std::unique_lock<std::mutex> lock(m_cmdMutex);
        m_cmdWake.wait_until(lock, due, [this] {
            return m_stopping || m_seekPending || m_paused || m_resyncPacing;
        });
    }

    void deliver(GstBuffer* buffer, GstCaps* caps, bool keyframe) {
        std::vector<Sink> targets;
        {
            std::lock_guard<std::mutex> lock(m_sinkMutex);
            targets.reserve(m_consumers.size());
            for (auto& item : m_consumers) {
                Consumer& c = item.second;
                if (c.needKeyframe) {
                    if (!keyframe) continue;
                    c.needKeyframe = false;
                }
                targets.push_back(c.sink);
            }
        }
        for (auto& sink : targets) sink(buffer, caps);
    }

    void requireKeyframe() {
        std::lock_guard<std::mutex> lock(m_sinkMutex);
        for (auto& item : m_consumers) item.second.needKeyframe = true;
    }

    // Đoạn dài nhất từng thấy — dùng để biết "sát mép live" là bao xa.
    void rememberSegmentLength() {
        for (const auto& seg : m_segments) {
            m_maxSegmentMs = std::max(m_maxSegmentMs, seg.endMs - seg.startMs);
        }
    }

    static int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    // Cửa sổ đoạn nạp sẵn mỗi lần hỏi DB.
    static constexpr int64_t kWindowMs = 10 * 60 * 1000;

    std::string m_cameraId;
    std::string m_codec;
    Loader m_loader;

    std::thread m_thread;
    std::atomic<bool> m_alive{false};
    std::atomic<bool> m_ended{false};
    std::atomic<bool> m_waiting{false};
    // Mặc định 4s = độ dài đoạn nhỏ nhất đang dùng; tự lớn lên theo dữ liệu thật.
    int64_t m_maxSegmentMs = 4'000;
    std::atomic<int64_t> m_positionMs{0};

    // Trạng thái điều khiển, dưới m_cmdMutex.
    mutable std::mutex m_cmdMutex;
    std::condition_variable m_cmdWake;
    bool m_stopping = false;
    bool m_seekPending = false;
    bool m_paused = false;
    bool m_resyncPacing = false;
    int64_t m_targetMs = 0;
    double m_rate = 1.0;
    uint64_t m_seekSeq = 0;
    std::atomic<uint64_t> m_appliedSeekSeq{0};

    // Chỉ thread feeder đụng vào.
    double m_currentRate = 1.0;
    // > 0 = đang bù cho tới mốc này (không ngủ theo nhịp). Xem pace().
    int64_t m_catchUpToMs = 0;
    // Mốc bắt đầu quãng không tìm được đoạn nào (0 = đang có dữ liệu).
    int64_t m_noDataSinceMs = 0;
    std::vector<PlaybackSegment> m_segments;
    bool m_haveWindow = false;
    int64_t m_windowFromMs = 0;
    int64_t m_windowToMs = 0;
    GstElement* m_filePipeline = nullptr;
    GstElement* m_fileAppsink = nullptr;
    bool m_fileOpen = false;
    int64_t m_segStartMs = 0;
    int64_t m_segEndMs = 0;
    bool m_anchorValid = false;
    int64_t m_anchorWallMs = 0;
    std::chrono::steady_clock::time_point m_anchorReal{};

    mutable std::mutex m_sinkMutex;
    std::unordered_map<uint64_t, Consumer> m_consumers;
    uint64_t m_nextSinkId = 1;
};

}  // namespace stream

#endif  // test_gstreamer_PlaybackSource_hpp
