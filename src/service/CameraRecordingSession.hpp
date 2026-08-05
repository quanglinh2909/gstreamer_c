#ifndef test_gstreamer_CameraRecordingSession_hpp
#define test_gstreamer_CameraRecordingSession_hpp

#include "service/AppSrcBridge.hpp"
#include "service/CameraRtpSource.hpp"
#include "service/RecordingTypes.hpp"
#include "service/StreamTypes.hpp"

#include <gst/gst.h>
#include <gst/app/gstappsink.h>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <set>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

class CameraRecordingSession : public std::enable_shared_from_this<CameraRecordingSession> {
public:
    CameraRecordingSession(stream::GStreamerConfig config,
                           stream::CameraRuntimeConfig camera,
                           stream::StreamCodec codec,
                           recording::RecordingSegmentSink segmentSink,
                           recording::MotionEventSink motionSink,
                           recording::MotionFrameSink frameSink,
                           recording::RecordingErrorSink errorSink,
                           std::shared_ptr<stream::CameraRtpSource> source)
        : m_config(std::move(config)),
          m_camera(std::move(camera)),
          m_codec(codec),
          m_segmentSink(std::move(segmentSink)),
          m_motionSink(std::move(motionSink)),
          m_motionFrameSink(std::move(frameSink)),
          m_errorSink(std::move(errorSink)),
          m_source(std::move(source)) {}

    ~CameraRecordingSession() {
        stop();
    }

    bool start() {
        stop();

        if (recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Off) {
            return true;
        }

        std::string error;
        if (startPipeline(/* includeMotionBranch */ true, error)) {
            return true;
        }

        if (canFallbackToRecordOnly()) {
            emitRecordingError("Recording motion branch failed, retrying record-only: " + error);
            std::string fallbackError;
            if (startPipeline(/* includeMotionBranch */ false, fallbackError)) {
                m_fallbackAttempted = true;
                return true;
            }
            emitRecordingError("Recording record-only fallback failed: " + fallbackError);
            return false;
        }

        emitRecordingError("Recording failed: " + error);
        return false;
    }

    /**
     * "Camera này vừa có SỰ KIỆN AI" — giữ lại đoạn ghi quanh thời điểm đó.
     *
     * Dùng ĐÚNG cơ chế mà chuyển động vẫn dùng: giữ đoạn chờ trong khoảng
     * "ghi trước", đánh dấu mọi đoạn đang mở là "có chuyện", và kéo dài cửa sổ
     * "ghi sau". Cửa sổ lấy từ CHÍNH CAMERA (preMotionSeconds/postMotionSeconds)
     * chứ không phải từ bên gọi: "chỉ ghi khi có sự kiện" là một cài đặt của
     * camera, không phải của từng AI. Nhờ vậy cửa sổ ghi-trước luôn bằng đúng
     * độ sâu bộ đệm đoạn-chờ, không còn cảnh AI xin 30 giây trong khi bộ đệm
     * chỉ giữ 10.
     *
     * KHÔNG dựng một sự kiện chuyển động: nó không có ô nào, không có ảnh, và
     * nhét vào bảng motion_events là bịa ra chuyển động chưa từng xảy ra. Sự
     * kiện thật đã nằm ở bảng riêng của loại AI đó rồi.
     *
     * Chạy được cả khi camera KHÔNG bật phát hiện chuyển động: lúc ấy pipeline
     * không có nhánh dò nào (đỡ CPU) mà chế độ ghi 'motion' vẫn hoạt động —
     * chỉ là do AI đánh thức thay vì do ô lưới.
     */
    /**
     * Danh sách ô đã động của MỘT khung ("r:c,r:c"), rỗng = khung này không có
     * gì. Điểm vào DUY NHẤT cho việc xét vùng, dùng chung cho hai nguồn:
     *
     *   * motioncells trong pipeline ghi hình (đường cũ), và
     *   * MotionDetector chạy trên khung của pipeline AI (đường mới, rẻ hơn
     *       một bậc vì khung đã được giải mã và RGA co giãn sẵn).
     *
     * Gọi từ thread nào cũng được: mọi trạng thái đụng tới đều nằm trong khoá.
     */
    /** Nguồn ảnh sự kiện: (cameraId) -> JPEG. Xem writeMotionSnapshot. */
    void setMotionJpegSource(
        std::function<std::vector<uint8_t>(const std::string&)> source) {
        m_motionJpegSource = std::move(source);
    }

    void noteMotionCells(const std::string& indices) {
        if (!isMotionMode() && !m_camera.motionEnabled) return;

        const auto hitCells = evaluateZones(indices);
        if (!hitCells.empty()) {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                for (const auto& cell : hitCells) m_motionCells.insert(cell);
                m_lastZoneHit = Clock::now();
            }
            startOrUpdateMotionEvent();
            return;
        }

        // Có động nhưng KHÔNG vùng nào đủ ngưỡng (hoặc không động gì). Nếu đã
        // im đủ lâu thì đóng sự kiện ngay tại đây: cảnh có thứ động liên tục
        // ngoài vùng (cây, quạt) thì không bao giờ có tin "hết chuyển động",
        // và sự kiện sẽ treo mãi.
        bool expired = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            expired = m_motionActive &&
                      (Clock::now() - m_lastZoneHit) >= postMotionDuration();
        }
        if (expired) finishMotionEvent();
    }

    bool noteAiEvent() {
        // Chế độ 'always' đã ghi mọi thứ, 'off' thì không ghi gì — cả hai đều
        // không có đoạn nào để giữ. Trả false chứ không im lặng bỏ qua: bên
        // gọi cần biết cấu hình của mình chưa có tác dụng.
        if (!isMotionMode()) return false;

        std::vector<recording::RecordingSegmentSnapshot> toEmit;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            retainPreMotionSegmentsLocked(toEmit, now);
            for (auto& item : m_openSegments) {
                item.second.hadMotion = true;
            }
            // max chứ không gán đè: sự kiện AI không được phép rút ngắn cửa sổ
            // mà một đợt chuyển động vừa mở.
            const auto until = now + postMotionDuration();
            if (until > m_postMotionUntil) m_postMotionUntil = until;
            drainExpiredPendingLocked(toDelete, now);
        }
        emitSegments(toEmit, m_segmentSink);
        deleteRecordingFiles(toDelete);
        return true;
    }

    bool startPipeline(bool includeMotionBranch, std::string& errorOut) {
        try {
            std::filesystem::create_directories(
                std::filesystem::path(m_config.recordingDir) / m_camera.id);
        } catch (const std::exception& error) {
            errorOut = error.what();
            return false;
        }

        // Đọc vùng MỘT lần cho cả phiên: sửa vùng là engine dựng lại pipeline
        // (streamRelevantInputPresent), nên không có chuyện vùng đổi giữa chừng.
        m_motionZones = recording::motionZonesOf(m_camera);

        const auto launch =
            recording::recordingLaunchStringForCamera(
                m_config, m_camera, m_codec,
                includeMotionBranch ? resolveMotionDecoder(m_camera, m_codec)
                                    : std::string{},
                includeMotionBranch);
        if (launch.empty()) {
            errorOut = "Could not build recording launch string";
            return false;
        }

        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(launch.c_str(), &error);
        if (!pipeline) {
            errorOut = consumeGError(error);
            return false;
        }
        if (error) {
            errorOut = consumeGError(error);
            gst_object_unref(pipeline);
            return false;
        }

        installFormatLocationHandler(pipeline);

        GstBus* bus = gst_element_get_bus(pipeline);
        guint watchId = 0;
        if (bus) {
            // Hand the watch a heap weak_ptr (freed by the GDestroyNotify) so a
            // bus message dispatched on the GLib loop thread can never touch a
            // CameraRecordingSession that stop()/the destructor freed on another
            // thread — onBusMessage locks it to a shared_ptr for the call.
            auto* weak = new std::weak_ptr<CameraRecordingSession>(weak_from_this());
            watchId = gst_bus_add_watch_full(
                bus,
                G_PRIORITY_DEFAULT,
                &CameraRecordingSession::onBusMessage,
                weak,
                [](gpointer data) {
                    delete static_cast<std::weak_ptr<CameraRecordingSession>*>(data);
                });
            gst_object_unref(bus);
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_pipeline = pipeline;
            m_busWatchId = watchId;
            m_motionBranchEnabled = includeMotionBranch && shouldUseMotionBranch();
        }

        const auto stateResult = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (stateResult == GST_STATE_CHANGE_FAILURE) {
            errorOut = "Failed to set recording pipeline to PLAYING";
            stop();
            return false;
        }

        // Đấu nguồn dùng chung vào appsrc SAU khi pipeline PLAYING. Nguồn phải
        // sẵn sàng — CameraStreamSession chỉ dựng recording khi acquire được.
        if (!m_source || !m_source->alive()) {
            errorOut = "Nguon RTP dung chung chua san sang cho ghi hinh";
            stop();
            return false;
        }
        if (GstElement* appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "record_src")) {
            m_bridge.attach(m_source, appsrc);
            gst_object_unref(appsrc);  // pipeline vẫn giữ; bridge chỉ mượn con trỏ
        } else {
            errorOut = "Recording pipeline thieu appsrc record_src";
            stop();
            return false;
        }
        // Mốc phiên ghi: mọi segment của pipeline này mang cùng giá trị để
        // playlist biết chỗ nào PTS reset (phiên mới) mà chèn DISCONTINUITY.
        m_sessionStartMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        return true;
    }

    void stop() {
        // Cắt cầu nối nguồn TRƯỚC khi finalize/tháo: ngừng bơm buffer để EOS
        // đóng segment cuối sạch sẽ, và không còn push nào chạy khi pipeline về
        // NULL. Gọi được nhiều lần (bridge tự idempotent).
        m_bridge.detach();

        GstElement* pipeline = nullptr;
        guint watchId = 0;
        std::vector<std::string> pendingFilesToDelete;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            pipeline = m_pipeline;
            m_pipeline = nullptr;
            watchId = m_busWatchId;
            m_busWatchId = 0;
            m_openSegments.clear();
            for (const auto& pending : m_pendingSegments) {
                pendingFilesToDelete.push_back(pending.snapshot.path);
            }
            m_pendingSegments.clear();
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_motionCells.clear();
            m_motionImagePath.clear();
            m_lastZoneHit = {};
            m_postMotionUntil = {};
            m_motionBranchEnabled = false;
        }

        if (watchId != 0) g_source_remove(watchId);
        if (pipeline) {
            gst_element_send_event(pipeline, gst_event_new_eos());
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
        deleteRecordingFiles(pendingFilesToDelete);
        // KHÔNG reset m_source ở đây: start() gọi stop() để dọn trước khi dựng
        // lại, reset sẽ làm mất nguồn giữa chừng. Nguồn được buông khi cả
        // CameraRecordingSession bị huỷ (m_source member tự hết) -> lúc đó, nếu
        // không còn ai xem, nguồn tự tắt.
    }

    // Picks the first decoder element that exists for this camera's hardware
    // preference. recordingLaunchStringForCamera uses an explicit decoder, so
    // (unlike decodebin) there is no autoplug fallback — the candidate list
    // always ends with the software decoder for a supported codec. Returns ""
    // only for an unsupported codec (empty candidate list), for which the
    // caller's recordingLaunchStringForCamera also produces no launch string.
    static std::string resolveMotionDecoder(const stream::CameraRuntimeConfig& camera,
                                            stream::StreamCodec codec) {
        for (const auto& name : recording::motionDecoderCandidates(camera.hardware, codec)) {
            GstElementFactory* factory = gst_element_factory_find(name.c_str());
            if (factory) {
                gst_object_unref(factory);
                return name;
            }
        }
        return {};
    }

    // H264 encoder for the motion-debug mount: the Rockchip MPP hardware
    // encoder when installed, otherwise software x264enc. x264enc at 960px
    // is one of the most CPU-expensive elements in the whole service.
    static std::string resolveDebugH264Encoder() {
        GstElementFactory* factory = gst_element_factory_find("mpph264enc");
        if (factory) {
            gst_object_unref(factory);
            return "mpph264enc";
        }
        return "x264enc";
    }

private:
    using Clock = std::chrono::system_clock;

    struct OpenSegment {
        std::string startedAt;
        Clock::time_point startedClock;
        bool hadMotion = false;
    };

    struct PendingSegment {
        recording::RecordingSegmentSnapshot snapshot;
        Clock::time_point startedClock;
        Clock::time_point endedClock;
    };

    static std::string nowIso8601() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        gmtime_s(&tm, &time);
#else
        gmtime_r(&time, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
        return out.str();
    }

    static std::string nowLocalFileTimestamp() {
        const auto now = std::chrono::system_clock::now();
        const auto time = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
#if defined(_WIN32)
        localtime_s(&tm, &time);
#else
        localtime_r(&time, &tm);
#endif
        std::ostringstream out;
        out << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
        return out.str();
    }

    static gchar* formatSegmentLocation(GstElement*, guint, gpointer userData) {
        auto* self = static_cast<CameraRecordingSession*>(userData);
        if (!self) return nullptr;

        const auto path = recording::recordingFilePathForTimestamp(
            self->m_config, self->m_camera, nowLocalFileTimestamp());
        return g_strdup(path.c_str());
    }

    void installFormatLocationHandler(GstElement* pipeline) {
        GstElement* sink = gst_bin_get_by_name(GST_BIN(pipeline), "record_sink");
        if (!sink) return;
        g_signal_connect(sink,
                         "format-location",
                         G_CALLBACK(&CameraRecordingSession::formatSegmentLocation),
                         this);
        gst_object_unref(sink);
    }

    static std::string consumeGError(GError* error) {
        if (!error) return "Unknown GStreamer error";
        std::string message = error->message ? error->message : "Unknown GStreamer error";
        g_error_free(error);
        return message;
    }

    // Stateless pad probe: drops encoded P/B frames so the motion decoder only
    // processes IDR keyframes. No userData, so nothing to outlive — the probe
    // is released together with the pipeline.
    static std::string gstMessageErrorText(GstMessage* message) {
        GError* error = nullptr;
        gchar* debug = nullptr;
        gst_message_parse_error(message, &error, &debug);

        std::string out = consumeGError(error);
        if (debug) {
            out += " (";
            out += debug;
            out += ")";
            g_free(debug);
        }
        return out;
    }

    bool isMotionMode() const {
        return recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Motion;
    }

    // Nhánh dò chuyển động chỉ chạy khi NGƯỜI DÙNG bật phát hiện chuyển động.
    // KHÔNG kéo theo `mode == Motion`: xem ghi chú dài ở RecordingTypes.hpp,
    // chỗ dựng chuỗi launch. Đây phải khớp đúng với điều kiện ở đó, lệch nhau
    // là m_motionBranchEnabled nói dối về một nhánh không tồn tại.
    bool shouldUseMotionBranch() const {
        return m_camera.motionEnabled;
    }

    bool canFallbackToRecordOnly() const {
        return recording::effectiveRecordingMode(m_camera) == recording::RecordingMode::Always &&
               m_camera.motionEnabled &&
               !m_fallbackAttempted;
    }

    void emitRecordingError(const std::string& message) const {
        if (!m_errorSink) return;
        recording::RecordingErrorSnapshot snapshot;
        snapshot.cameraId = m_camera.id;
        snapshot.message = message;
        m_errorSink(snapshot);
    }

    std::chrono::seconds preMotionDuration() const {
        return std::chrono::seconds(std::max<uint32_t>(0, m_camera.preMotionSeconds));
    }

    std::chrono::seconds postMotionDuration() const {
        return std::chrono::seconds(std::max<uint32_t>(0, m_camera.postMotionSeconds));
    }

    static bool timeRangesOverlap(Clock::time_point leftStart,
                                  Clock::time_point leftEnd,
                                  Clock::time_point rightStart,
                                  Clock::time_point rightEnd) {
        return leftStart < rightEnd && rightStart < leftEnd;
    }

    bool isPostMotionActiveLocked(Clock::time_point now) const {
        return m_postMotionUntil != Clock::time_point{} && now <= m_postMotionUntil;
    }

    static void emitSegments(const std::vector<recording::RecordingSegmentSnapshot>& segments,
                             const recording::RecordingSegmentSink& sink) {
        if (!sink) return;
        for (const auto& segment : segments) {
            sink(segment);
        }
    }

    static void deleteRecordingFiles(const std::vector<std::string>& paths) {
        for (const auto& path : paths) {
            if (path.empty()) continue;
            std::error_code ec;
            std::filesystem::remove(path, ec);
        }
    }

    static gboolean onBusMessage(GstBus*, GstMessage* message, gpointer userData) {
        auto* weak = static_cast<std::weak_ptr<CameraRecordingSession>*>(userData);
        auto self = weak->lock();
        if (!self) return G_SOURCE_REMOVE;

        switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ELEMENT:
                self->handleElementMessage(message);
                return G_SOURCE_CONTINUE;
            case GST_MESSAGE_ERROR:
                return self->handleErrorMessage(message);
            case GST_MESSAGE_EOS:
                return G_SOURCE_REMOVE;
            default:
                return G_SOURCE_CONTINUE;
        }
    }

    gboolean handleErrorMessage(GstMessage* message) {
        const auto errorText = gstMessageErrorText(message);
        if (canFallbackToRecordOnly()) {
            restartRecordOnlyAfterError(errorText);
            return G_SOURCE_REMOVE;
        }

        emitRecordingError("Recording pipeline error: " + errorText);
        return G_SOURCE_CONTINUE;
    }

    void restartRecordOnlyAfterError(const std::string& errorText) {
        GstElement* oldPipeline = nullptr;
        std::vector<std::string> pendingFilesToDelete;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            oldPipeline = m_pipeline;
            m_pipeline = nullptr;
            m_busWatchId = 0;
            m_motionBranchEnabled = false;
            m_fallbackAttempted = true;
            m_openSegments.clear();
            for (const auto& pending : m_pendingSegments) {
                pendingFilesToDelete.push_back(pending.snapshot.path);
            }
            m_pendingSegments.clear();
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_postMotionUntil = {};
        }

        if (oldPipeline) {
            gst_element_set_state(oldPipeline, GST_STATE_NULL);
            gst_object_unref(oldPipeline);
        }
        deleteRecordingFiles(pendingFilesToDelete);

        emitRecordingError("Recording motion branch failed, falling back to record-only: " + errorText);
        std::string fallbackError;
        if (!startPipeline(/* includeMotionBranch */ false, fallbackError)) {
            emitRecordingError("Recording record-only fallback failed: " + fallbackError);
        }
    }

    void handleElementMessage(GstMessage* message) {
        const GstStructure* structure = gst_message_get_structure(message);
        if (!structure) return;

        const auto* name = gst_structure_get_name(structure);
        if (!name) return;

        const std::string messageName(name);
        if (messageName == "splitmuxsink-fragment-opened") {
            const gchar* location = gst_structure_get_string(structure, "location");
            if (!location) return;

            std::string openedAt;
            bool announceOpen = false;
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                openedAt = nowIso8601();
                m_openSegments[location] = {
                    openedAt,
                    Clock::now(),
                    m_motionActive || isPostMotionActiveLocked(Clock::now())};
                // Chỉ báo "đang ghi" cho chế độ always: chế độ motion còn phải
                // đợi xác nhận chuyển động mới biết có giữ đoạn hay không, báo
                // sớm sẽ để lại hàng mồ côi khi đoạn bị vứt.
                announceOpen = !isMotionMode();
            }
            // Emit NGOÀI khoá: sink đi tới DB.
            if (announceOpen && m_segmentSink) {
                recording::RecordingSegmentSnapshot open;
                open.cameraId = m_camera.id;
                open.path = location;
                open.startAt = openedAt;
                open.codec = stream::toString(m_codec);
                open.container = "ts";
                open.recordingMode = m_camera.recordingMode;
                open.status = "recording";
                open.sessionStartMs = m_sessionStartMs;
                // durationMs mang segmentSeconds để DB tính end_at ước lượng.
                open.durationMs = static_cast<int32_t>(
                    std::max<uint32_t>(1, m_camera.segmentSeconds));
                m_segmentSink(open);
            }
            return;
        }

        if (messageName == "splitmuxsink-fragment-closed") {
            const gchar* location = gst_structure_get_string(structure, "location");
            if (!location) return;
            closeSegment(location);
            return;
        }

        // motioncells posts element messages named "motion" for both the start
        // and the end of a motion event; the field present distinguishes them
        // ("motion_begin" vs "motion_finished") — the name is "motion" in both.
        const auto kind = recording::classifyMotionMessage(
            messageName,
            gst_structure_has_field(structure, "motion_begin"),
            gst_structure_has_field(structure, "motion_finished"),
            gst_structure_has_field(structure, "motion_cells_indices"));
        if (kind == recording::MotionMessageKind::None) return;

        // motioncells giờ chỉ làm MỘT việc: báo "khung này những ô nào đổi".
        // Ngưỡng của nó đặt ở mức một ô cũng báo, và KHÔNG có mặt nạ — vì mặt nạ
        // chỉ đọc được 255 ô đầu (đã đo). Việc "ô đó có thuộc vùng nào không" và
        // "vùng đó đủ số ô chưa" do chính chỗ này quyết định.
        // Phần tử tự báo hết chuyển động (im lặng suốt `gap` giây) — chốt sự
        // kiện. Đây là đường lùi cho trường hợp cả khung hình đứng yên hẳn, lúc
        // đó không còn thông điệp nào để xét theo vùng nữa.
        if (kind == recording::MotionMessageKind::Finished) {
            finishMotionEvent();
            return;
        }

        const gchar* cells = gst_structure_get_string(structure, "motion_cells_indices");
        noteMotionCells(cells ? cells : "");
    }

    /**
     * Chuỗi ô của một khung ("1:2,3:4") -> những ô thuộc các vùng ĐỦ NGƯỠNG.
     *
     * Trả về rỗng nghĩa là khung này không đánh thức vùng nào. Chỉ ô của vùng
     * đã đủ ngưỡng mới được giữ: có thế thì lớp phủ vẽ lại mới đúng chỗ đã kích
     * hoạt sự kiện, chứ không phải mọi nhiễu lác đác trong khung.
     */
    static std::string joinCells(const std::vector<std::string>& cells) {
        std::string out;
        for (const auto& cell : cells) {
            if (!out.empty()) out.push_back(',');
            out += cell;
        }
        return out;
    }

    std::vector<std::string> evaluateZones(const std::string& indices) {
        std::vector<std::pair<int, int>> moved;
        moved.reserve(64);
        size_t start = 0;
        while (start <= indices.size()) {
            const auto comma = indices.find(',', start);
            const auto piece = indices.substr(
                start, comma == std::string::npos ? std::string::npos : comma - start);
            const auto colon = piece.find(':');
            if (colon != std::string::npos) {
                try {
                    moved.emplace_back(std::stoi(piece.substr(0, colon)),
                                       std::stoi(piece.substr(colon + 1)));
                } catch (...) {
                    // Ô hỏng: bỏ qua, thà thiếu một ô còn hơn đếm nhầm.
                }
            }
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (moved.empty()) return {};

        // Ô nằm trong BẤT KỲ vùng nào (dù vùng đó chưa đủ ngưỡng) và ô nằm ngoài
        // hết. Hai danh sách này chỉ để VẼ, không quyết định sự kiện.
        std::vector<std::string> insideAny;
        std::vector<std::string> outside;
        for (const auto& [row, col] : moved) {
            const auto key = std::to_string(row) + ":" + std::to_string(col);
            bool in = false;
            for (const auto& zone : m_motionZones) {
                if (zone.contains(row, col)) { in = true; break; }
            }
            (in ? insideAny : outside).push_back(key);
        }
        if (m_motionFrameSink) {
            recording::MotionFrameSnapshot frame;
            frame.cameraId = m_camera.id;
            frame.insideCells = joinCells(insideAny);
            frame.outsideCells = joinCells(outside);
            frame.gridX = recording::clampMotionGrid(m_camera.motionGridX);
            frame.gridY = recording::clampMotionGrid(m_camera.motionGridY);
            m_motionFrameSink(frame);
        }

        // Sự kiện thì vẫn theo NGƯỠNG của từng vùng — chỉ ô của vùng đã đủ
        // ngưỡng mới được ghi lại.
        std::vector<std::string> out;
        for (const auto& zone : m_motionZones) {
            std::vector<std::string> inZone;
            for (const auto& [row, col] : moved) {
                if (zone.contains(row, col)) {
                    inZone.push_back(std::to_string(row) + ":" + std::to_string(col));
                }
            }
            if (inZone.size() >= zone.needCells()) {
                out.insert(out.end(), inZone.begin(), inZone.end());
            }
        }
        return out;
    }

    void closeSegment(const std::string& location) {
        recording::RecordingSegmentSnapshot snapshot;
        Clock::time_point startedClock;
        Clock::time_point endedClock;
        bool motionMode = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_openSegments.find(location);
            if (found == m_openSegments.end()) return;

            endedClock = Clock::now();
            startedClock = found->second.startedClock;
            motionMode = isMotionMode();
            snapshot.cameraId = m_camera.id;
            snapshot.path = location;
            snapshot.startAt = found->second.startedAt;
            snapshot.endAt = nowIso8601();
            snapshot.durationMs = static_cast<int32_t>(
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    endedClock - startedClock).count());
            snapshot.codec = stream::toString(m_codec);
            snapshot.container = "ts";
            snapshot.recordingMode = m_camera.recordingMode;
            snapshot.sessionStartMs = m_sessionStartMs;
            snapshot.hasMotion =
                found->second.hadMotion || m_motionActive || isPostMotionActiveLocked(endedClock);
            m_openSegments.erase(found);
        }

        if (motionMode) {
            handleClosedMotionSegment({snapshot, startedClock, endedClock});
        } else if (m_segmentSink) {
            m_segmentSink(snapshot);
        }
    }

    void handleClosedMotionSegment(PendingSegment segment) {
        std::vector<recording::RecordingSegmentSnapshot> toEmit;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (segment.snapshot.hasMotion || isPostMotionActiveLocked(segment.endedClock)) {
                segment.snapshot.hasMotion = true;
                toEmit.push_back(segment.snapshot);
            } else {
                m_pendingSegments.push_back(std::move(segment));
            }
            drainExpiredPendingLocked(toDelete, now);
        }

        emitSegments(toEmit, m_segmentSink);
        deleteRecordingFiles(toDelete);
    }

    /** Cứu các đoạn đang chờ có phủ khoảng ["ghi trước" giây trước, now]. */
    void retainPreMotionSegmentsLocked(std::vector<recording::RecordingSegmentSnapshot>& toEmit,
                                       Clock::time_point now) {
        const auto keepStart = now - preMotionDuration();
        auto item = m_pendingSegments.begin();
        while (item != m_pendingSegments.end()) {
            if (timeRangesOverlap(item->startedClock, item->endedClock, keepStart, now)) {
                item->snapshot.hasMotion = true;
                toEmit.push_back(item->snapshot);
                item = m_pendingSegments.erase(item);
            } else {
                ++item;
            }
        }
    }

    void drainExpiredPendingLocked(std::vector<std::string>& toDelete,
                                   Clock::time_point now) {
        // ĐỘ SÂU của bộ đệm đoạn-chờ: đoạn cũ hơn thế thì xoá khỏi đĩa, không
        // ai cứu được nữa. Bằng đúng "ghi trước" của camera — cũng là cửa sổ mà
        // cả chuyển động lẫn sự kiện AI dùng để cứu đoạn, nên hai con số không
        // thể lệch nhau.
        const auto cutoff = now - preMotionDuration();
        while (!m_pendingSegments.empty()) {
            const auto& pending = m_pendingSegments.front();
            if (pending.endedClock >= cutoff) break;
            toDelete.push_back(pending.snapshot.path);
            m_pendingSegments.pop_front();
        }
    }

    /**
     * Ghi khung hình JPEG mới nhất của nhánh dò ra đĩa; trả về đường dẫn tương
     * đối để lưu vào DB, hoặc "" nếu chưa có khung nào.
     *
     * Gọi lúc sự kiện BẮT ĐẦU, đúng một lần cho mỗi sự kiện. Không "chụp" gì
     * cả — nhánh ảnh đã encode sẵn ở 1 khung/giây, đây chỉ là lấy cái mới nhất
     * ra và đổ xuống file, nên không đụng tới đường giải mã.
     */
    std::string writeMotionSnapshot() {
        // Không lưu sự kiện thì cũng không ghi ảnh: sẽ không có hàng nào trỏ
        // tới nó, file chỉ nằm chiếm chỗ.
        if (!m_camera.motionSaveEvents) return {};

        // Ảnh giờ lấy từ khung của pipeline AI (bộ dò chuyển động giữ khung
        // mới nhất) thay vì từ một appsink jpegenc riêng trong pipeline ghi
        // hình. Nhánh cũ encode đều 1 khung/giây cho MỌI camera bật chuyển
        // động — đo được 5,6% CPU mỗi camera, mà gần như toàn bộ số ảnh ấy bị
        // vứt vì sự kiện thì thi thoảng mới có.
        if (!m_motionJpegSource) return {};
        const auto jpeg = m_motionJpegSource(m_camera.id);
        if (jpeg.empty()) return {};

        const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::string relative = recording::motionSnapshotRelativePath(
            m_config, m_camera.id, todayYmd(), nowMs);
        const std::filesystem::path path(relative);
        std::error_code ec;
        std::filesystem::create_directories(path.parent_path(), ec);
        std::ofstream out(path, std::ios::binary);
        if (out) {
            out.write(reinterpret_cast<const char*>(jpeg.data()),
                      static_cast<std::streamsize>(jpeg.size()));
        }
        // Ghi hỏng (đĩa đầy, hết quyền) thì trả rỗng: thà sự kiện không có ảnh
        // còn hơn DB trỏ vào một file không tồn tại.
        if (!out || !out.good()) {
            std::filesystem::remove(path, ec);
            relative.clear();
        }
        return relative;
    }



    static std::string todayYmd() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        std::tm tm{};
        localtime_r(&seconds, &tm);
        char buffer[16];
        std::strftime(buffer, sizeof(buffer), "%Y-%m-%d", &tm);
        return buffer;
    }

    void startOrUpdateMotionEvent() {
        std::vector<recording::RecordingSegmentSnapshot> toEmit;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();

        bool justStarted = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_motionActive) {
                m_motionActive = true;
                m_motionStartedAt = nowIso8601();
                justStarted = true;
            }
            m_postMotionUntil = {};
            retainPreMotionSegmentsLocked(toEmit, now);
            for (auto& item : m_openSegments) {
                item.second.hadMotion = true;
            }
            drainExpiredPendingLocked(toDelete, now);
        }

        // Chụp NGOÀI khoá và chỉ ở khung đầu tiên của đợt: một sự kiện kéo dài
        // hàng chục giây sẽ vào đây mỗi khung hình, chụp mỗi lần là ghi hàng
        // trăm file cho một sự kiện rồi vứt hết trừ cái cuối.
        if (justStarted) {
            auto path = writeMotionSnapshot();
            std::lock_guard<std::mutex> lock(m_mutex);
            // Sự kiện có thể đã kết thúc trong lúc ghi file. Chỉ gán khi đợt
            // này vẫn đang chạy, để ảnh không dính sang sự kiện kế tiếp.
            if (m_motionActive) m_motionImagePath = std::move(path);
        }

        emitSegments(toEmit, m_segmentSink);
        deleteRecordingFiles(toDelete);
    }

    void finishMotionEvent() {
        recording::MotionEventSnapshot snapshot;
        std::vector<std::string> toDelete;
        const auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_motionActive) return;
            snapshot.cameraId = m_camera.id;
            snapshot.startAt = m_motionStartedAt;
            snapshot.endAt = nowIso8601();
            snapshot.maxScore = 1.0;
            snapshot.gridX = recording::clampMotionGrid(m_camera.motionGridX);
            snapshot.gridY = recording::clampMotionGrid(m_camera.motionGridY);
            snapshot.saveToDb = m_camera.motionSaveEvents;
            snapshot.imagePath = m_motionImagePath;
            m_motionImagePath.clear();
            {
                std::ostringstream cells;
                bool first = true;
                for (const auto& cell : m_motionCells) {
                    if (!first) cells << ",";
                    cells << cell;
                    first = false;
                }
                snapshot.cells = cells.str();
            }
            m_motionCells.clear();
            m_motionActive = false;
            m_motionStartedAt.clear();
            m_postMotionUntil = now + postMotionDuration();
            drainExpiredPendingLocked(toDelete, now);
        }

        if (m_motionSink) m_motionSink(snapshot);
        deleteRecordingFiles(toDelete);
    }

    stream::GStreamerConfig m_config;
    stream::CameraRuntimeConfig m_camera;
    stream::StreamCodec m_codec = stream::StreamCodec::Unknown;
    recording::RecordingSegmentSink m_segmentSink;
    recording::MotionEventSink m_motionSink;
    recording::MotionFrameSink m_motionFrameSink;
    recording::RecordingErrorSink m_errorSink;

    // Nguồn RTP dùng chung với xem live + cầu nối vào appsrc ghi hình.
    std::shared_ptr<stream::CameraRtpSource> m_source;
    stream::AppSrcBridge m_bridge;

    mutable std::mutex m_mutex;
    GstElement* m_pipeline = nullptr;
    // Epoch ms lúc pipeline ghi hiện tại lên PLAYING — xem RecordingSegmentSnapshot.
    int64_t m_sessionStartMs = 0;
    guint m_busWatchId = 0;
    bool m_motionActive = false;
    bool m_motionBranchEnabled = false;
    bool m_fallbackAttempted = false;
    std::string m_motionStartedAt;
    // Các vùng đã vẽ, đọc MỘT lần lúc dựng phiên (đổi vùng là engine dựng lại
    // pipeline nên không cần đọc lại giữa chừng).
    std::vector<recording::MotionZone> m_motionZones;
    std::function<std::vector<uint8_t>(const std::string&)> m_motionJpegSource;
    // Lần gần nhất có MỘT vùng đủ ngưỡng. Dùng để đóng sự kiện khi cảnh vẫn
    // nhiễu bên ngoài vùng — lúc đó motioncells không bao giờ báo hết chuyển động.
    Clock::time_point m_lastZoneHit;
    // Ô đã động trong sự kiện hiện tại, gộp lại. std::set để tự sắp và tự khử
    // trùng — cùng một ô sẽ đến rất nhiều lần vì postallmotion bắn mỗi khung.
    std::set<std::string> m_motionCells;
    // Ảnh của sự kiện ĐANG diễn ra (đường dẫn tương đối), chụp ở khung đầu
    // tiên. Xoá đi khi sự kiện được chốt.
    std::string m_motionImagePath;
    Clock::time_point m_postMotionUntil;
    std::unordered_map<std::string, OpenSegment> m_openSegments;
    std::deque<PendingSegment> m_pendingSegments;
};

#endif
