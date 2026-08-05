#ifndef test_gstreamer_GStreamerService_hpp
#define test_gstreamer_GStreamerService_hpp

#include "dto/CameraDto.hpp"
#include "dto/StreamStatusDto.hpp"
#include "service/CameraSourceRegistry.hpp"
#include "service/CameraStreamSession.hpp"
#include "service/StreamTypes.hpp"

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>

#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

class GStreamerService {
public:
    explicit GStreamerService(stream::GStreamerConfig config,
                              stream::StreamStatusSink statusSink = {},
                              recording::RecordingSegmentSink segmentSink = {},
                              recording::MotionEventSink motionSink = {},
                              recording::MotionFrameSink motionFrameSink = {},
                              std::shared_ptr<stream::CameraSourceRegistry> sources = {})
        : m_config(std::move(config)),
          m_statusSink(std::move(statusSink)),
          m_segmentSink(std::move(segmentSink)),
          m_motionSink(std::move(motionSink)),
          m_motionFrameSink(std::move(motionFrameSink)),
          m_sourceRegistry(sources ? std::move(sources)
                                    : std::make_shared<stream::CameraSourceRegistry>()) {}

    // Registry nguồn dùng chung — WebRtcService cần cùng instance để xem live và
    // ghi hình chia sẻ một kết nối camera. Tạo sẵn nếu component không tiêm vào.
    std::shared_ptr<stream::CameraSourceRegistry> sourceRegistry() const {
        return m_sourceRegistry;
    }

    ~GStreamerService() {
        cleanup();
    }

    void start() {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_running) return;

        m_server = gst_rtsp_server_new();
        const auto service = std::to_string(m_config.rtspPort);
        g_object_set(m_server,
                     "address", m_config.rtspHost.c_str(),
                     "service", service.c_str(),
                     nullptr);

        m_mounts = gst_rtsp_server_get_mount_points(m_server);
        m_loop = g_main_loop_new(nullptr, FALSE);
        m_serverSourceId = gst_rtsp_server_attach(m_server, nullptr);
        if (m_serverSourceId == 0) {
            g_object_unref(m_mounts);
            g_object_unref(m_server);
            g_main_loop_unref(m_loop);
            m_mounts = nullptr;
            m_server = nullptr;
            m_loop = nullptr;
            throw std::runtime_error("Failed to attach GStreamer RTSP server");
        }
        m_running = true;
        m_loopThread = std::thread([this] {
            g_main_loop_run(m_loop);
        });
    }

    void stop() {
        std::vector<std::shared_ptr<CameraStreamSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (auto& item : m_sessions) {
                sessions.push_back(item.second);
            }
        }

        for (auto& session : sessions) {
            session->stop();
        }

        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_running) return;
            if (m_loop) g_main_loop_quit(m_loop);
            if (m_serverSourceId != 0) {
                g_source_remove(m_serverSourceId);
                m_serverSourceId = 0;
            }
            m_running = false;
        }

        if (m_loopThread.joinable()) {
            m_loopThread.join();
        }
    }

    void cleanup() {
        stop();

        std::lock_guard<std::mutex> lock(m_mutex);
        m_sessions.clear();

        if (m_mounts) {
            g_object_unref(m_mounts);
            m_mounts = nullptr;
        }
        if (m_server) {
            g_object_unref(m_server);
            m_server = nullptr;
        }
        if (m_loop) {
            g_main_loop_unref(m_loop);
            m_loop = nullptr;
        }
    }

    // Báo cho bên ngoài biết luồng của một camera vừa bị dựng lại / dừng hẳn,
    // để họ dọn thứ đang bám vào luồng cũ (hiện tại: các phiên xem WebRTC).
    //
    // Dùng callback thay vì gọi thẳng WebRtcService: vòng đời hai bên độc lập
    // (stream sống theo camera, phiên xem sống theo tab trình duyệt) và
    // GStreamerService không nên biết gì về WebRTC.
    using StreamResetSink = std::function<void(const std::string& cameraId)>;

    void setStreamResetSink(StreamResetSink sink) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_streamResetSink = std::move(sink);
    }

    void startCamera(const stream::CameraRuntimeConfig& camera) {
        ensureStarted();
        // Gọi TRƯỚC khi dựng lại: mount cũ sắp biến mất, phiên đang bám vào nó
        // phải chết ngay thay vì đợi watchdog phát hiện hết dữ liệu.
        notifyStreamReset(camera.id);

        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            if (!m_mounts) return;

            auto found = m_sessions.find(camera.id);
            if (found == m_sessions.end()) {
                session = std::make_shared<CameraStreamSession>(
                    m_config, camera, m_mounts, m_statusSink, m_segmentSink, m_motionSink,
                    m_motionFrameSink,
                    m_sourceRegistry);
                if (m_motionJpegSource) session->setMotionJpegSource(m_motionJpegSource);
                m_sessions[camera.id] = session;
            } else {
                session = found->second;
            }
        }

        session->restart(camera);

        // Báo cho AI biết camera này có cần dò chuyển động không. Gọi NGOÀI
        // khoá: AiManager giữ khoá riêng và có thể gọi ngược lại vào đây
        // (noteMotionCells) — giữ hai khoá chéo nhau là công thức deadlock.
        notifyMotionConfig(camera, camera.motionEnabled);
    }

    void startCamera(const oatpp::Object<CameraDto>& camera) {
        startCamera(toRuntimeConfig(camera));
    }

    void stopCamera(const oatpp::String& cameraId) {
        const auto id = toStdString(cameraId);
        notifyStreamReset(id);
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(id);
            if (found != m_sessions.end()) session = found->second;
        }
        if (session) session->stop();
        stream::CameraRuntimeConfig gone;
        gone.id = id;
        notifyMotionConfig(gone, false);
    }

    void cleanupCamera(const oatpp::String& cameraId) {
        const auto id = toStdString(cameraId);
        notifyStreamReset(id);
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(id);
            if (found == m_sessions.end()) return;
            session = found->second;
            m_sessions.erase(found);
        }
        session->cleanup();
        stream::CameraRuntimeConfig gone;
        gone.id = id;
        notifyMotionConfig(gone, false);
    }

    /**
     * "Camera này vừa có sự kiện AI" — giữ đoạn ghi quanh thời điểm đó.
     * false = camera không tồn tại hoặc đang không ghi hình.
     */
    /**
     * Ô đã động của một khung, do AiManager gửi sang (MotionDetector chạy trên
     * khung của pipeline AI). Bỏ qua trong im lặng khi camera không còn phiên
     * — pipeline AI có thể còn nhả nốt vài khung sau khi camera đã dừng.
     */
    void noteMotionCells(const std::string& cameraId, const std::string& indices) {
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(cameraId);
            if (found != m_sessions.end()) session = found->second;
        }
        if (session) session->noteMotionCells(indices);
    }

    /**
     * Nơi báo cho AiManager biết camera nào cần dò chuyển động. Đặt bằng
     * std::function để GStreamerService KHÔNG include AiManager (AiManager đã
     * include service/ rồi, include ngược lại là vòng).
     */
    /** Nguồn ảnh sự kiện chuyển động (AiManager::grabMotionJpeg). */
    void setMotionJpegSource(
        std::function<std::vector<uint8_t>(const std::string&)> source) {
        std::vector<std::shared_ptr<CameraStreamSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_motionJpegSource = source;
            for (auto& entry : m_sessions) sessions.push_back(entry.second);
        }
        for (auto& s : sessions) s->setMotionJpegSource(source);
    }

    void setMotionConfigSink(
        std::function<void(const stream::CameraRuntimeConfig&, bool)> sink) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_motionConfigSink = std::move(sink);
    }

private:
    void notifyMotionConfig(const stream::CameraRuntimeConfig& camera, bool enabled) {
        std::function<void(const stream::CameraRuntimeConfig&, bool)> sink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sink = m_motionConfigSink;
        }
        if (sink) sink(camera, enabled);
    }

public:

    bool noteAiEvent(const oatpp::String& cameraId) {
        const auto id = toStdString(cameraId);
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(id);
            if (found != m_sessions.end()) session = found->second;
        }
        if (!session) return false;
        return session->noteAiEvent();
    }

    void restartCamera(const stream::CameraRuntimeConfig& camera) {
        startCamera(camera);
    }

    void restartCamera(const oatpp::Object<CameraDto>& camera) {
        restartCamera(toRuntimeConfig(camera));
    }

    oatpp::Object<StreamStatusDto> getStatus(const oatpp::String& cameraId) const {
        const auto id = toStdString(cameraId);
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(id);
            if (found != m_sessions.end()) session = found->second;
        }

        if (session) {
            return toDto(session->snapshot());
        }

        stream::StreamStatusSnapshot snapshot;
        snapshot.id = id;
        snapshot.state = stream::StreamState::Stopped;
        snapshot.outputRtsp = stream::outputUrlForCamera(m_config, id);
        snapshot.hardware = m_config.defaultHardware;
        snapshot.recordingEnabled = m_config.recordingEnabled;
        return toDto(snapshot);
    }

    oatpp::Object<StreamStatusDto> getStatus(const oatpp::Object<CameraDto>& camera) const {
        if (!camera) return getStatus(oatpp::String(""));

        const auto id = toStdString(camera->id);
        std::shared_ptr<CameraStreamSession> session;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            auto found = m_sessions.find(id);
            if (found != m_sessions.end()) session = found->second;
        }

        if (session) {
            return toDto(session->snapshot());
        }
        return toDto(camera, m_config);
    }

    oatpp::List<oatpp::Object<StreamStatusDto>> getAllStatuses() const {
        auto list = oatpp::List<oatpp::Object<StreamStatusDto>>::createShared();

        std::vector<std::shared_ptr<CameraStreamSession>> sessions;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& item : m_sessions) {
                sessions.push_back(item.second);
            }
        }

        for (const auto& session : sessions) {
            list->push_back(toDto(session->snapshot()));
        }

        return list;
    }

    stream::GStreamerConfig getConfig() const {
        return m_config;
    }

private:
    void ensureStarted() {
        bool shouldStart = false;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            shouldStart = !m_running;
        }
        if (shouldStart) start();
    }

    static std::string toStdString(const oatpp::String& value) {
        return value ? std::string(value->c_str()) : std::string();
    }

    static stream::CameraRuntimeConfig toRuntimeConfig(const oatpp::Object<CameraDto>& dto) {
        stream::CameraRuntimeConfig out;
        if (!dto) return out;
        out.id = toStdString(dto->id);
        out.name = toStdString(dto->name);
        out.rtsp = toStdString(dto->rtsp);
        out.hardware = toStdString(dto->hardware);
        if (dto->recordingEnabled) out.recordingEnabled = *dto->recordingEnabled;
        out.recordingMode = toStdString(dto->recordingMode);
        if (dto->motionEnabled) out.motionEnabled = *dto->motionEnabled;
        if (dto->motionSensitivity) out.motionSensitivity = *dto->motionSensitivity;
        if (dto->motionThreshold) out.motionThreshold = *dto->motionThreshold;
        if (dto->preMotionSeconds) out.preMotionSeconds = *dto->preMotionSeconds;
        if (dto->postMotionSeconds) out.postMotionSeconds = *dto->postMotionSeconds;
        if (dto->segmentSeconds) out.segmentSeconds = *dto->segmentSeconds;
        if (dto->motionGridX) out.motionGridX = *dto->motionGridX;
        if (dto->motionGridY) out.motionGridY = *dto->motionGridY;
        out.motionCellLevels = toStdString(dto->motionCellLevels);
        out.motionZones = toStdString(dto->motionZones);
        if (dto->motionSaveEvents) out.motionSaveEvents = *dto->motionSaveEvents;
        return out;
    }

    static oatpp::Object<StreamStatusDto> toDto(const stream::StreamStatusSnapshot& snapshot) {
        auto dto = StreamStatusDto::createShared();
        dto->id = snapshot.id.c_str();
        dto->state = stream::toString(snapshot.state);
        dto->inputRtsp = snapshot.inputRtsp.c_str();
        dto->outputRtsp = snapshot.outputRtsp.c_str();
        dto->codec = stream::toString(snapshot.codec);
        dto->hardware = snapshot.hardware.c_str();
        dto->recordingEnabled = snapshot.recordingEnabled;
        dto->retryCount = snapshot.retryCount;
        dto->lastError = snapshot.lastError.c_str();
        dto->lastChangedAt = snapshot.lastChangedAt.c_str();
        return dto;
    }

    static oatpp::Object<StreamStatusDto> toDto(
        const oatpp::Object<CameraDto>& camera,
        const stream::GStreamerConfig& config)
    {
        auto dto = StreamStatusDto::createShared();
        if (!camera) return dto;

        const auto id = toStdString(camera->id);
        const auto output = camera->outputRtsp && camera->outputRtsp->size() > 0
            ? toStdString(camera->outputRtsp)
            : stream::outputUrlForCamera(config, id);

        dto->id = camera->id;
        dto->state = camera->state ? camera->state : oatpp::String("offline");
        dto->inputRtsp = camera->inputRtsp && camera->inputRtsp->size() > 0
            ? camera->inputRtsp
            : camera->rtsp;
        dto->outputRtsp = output.c_str();
        dto->codec = camera->codec ? camera->codec : oatpp::String("unknown");
        dto->hardware = camera->hardware && camera->hardware->size() > 0
            ? camera->hardware
            : oatpp::String(config.defaultHardware.c_str());
        const bool recordingModeEnabled =
            camera->recordingMode &&
            recording::recordingModeFromString(camera->recordingMode->c_str()) !=
                recording::RecordingMode::Off;
        // getValue(): oatpp::Boolean::operator bool() returns the value, not a
        // null check, so a plain ternary mis-reads an explicit false.
        const bool recordingEnabled = recordingModeEnabled ||
            camera->recordingEnabled.getValue(config.recordingEnabled);
        dto->recordingEnabled = recordingEnabled;
        dto->retryCount = camera->retryCount
            ? camera->retryCount
            : oatpp::UInt32(static_cast<v_uint32>(0));
        dto->lastError = camera->lastError ? camera->lastError : oatpp::String("");
        dto->lastChangedAt = camera->lastChangedAt ? camera->lastChangedAt : oatpp::String("");
        return dto;
    }

    // Gọi sink NGOÀI khoá: nó dọn phiên WebRTC (đưa pipeline về NULL, mất một
    // lúc), giữ khoá suốt thời gian đó sẽ chặn mọi thao tác camera khác.
    void notifyStreamReset(const std::string& cameraId) {
        StreamResetSink sink;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            sink = m_streamResetSink;
        }
        if (sink) sink(cameraId);
    }

    stream::GStreamerConfig m_config;
    StreamResetSink m_streamResetSink;
    stream::StreamStatusSink m_statusSink;
    recording::RecordingSegmentSink m_segmentSink;
    recording::MotionEventSink m_motionSink;
    recording::MotionFrameSink m_motionFrameSink;
    std::shared_ptr<stream::CameraSourceRegistry> m_sourceRegistry;
    // Báo cho AiManager camera nào cần dò chuyển động. Xem setMotionConfigSink.
    std::function<void(const stream::CameraRuntimeConfig&, bool)> m_motionConfigSink;
    std::function<std::vector<uint8_t>(const std::string&)> m_motionJpegSource;
    GstRTSPServer* m_server = nullptr;
    GstRTSPMountPoints* m_mounts = nullptr;
    GMainLoop* m_loop = nullptr;
    guint m_serverSourceId = 0;
    std::thread m_loopThread;
    bool m_running = false;

    mutable std::mutex m_mutex;
    std::unordered_map<std::string, std::shared_ptr<CameraStreamSession>> m_sessions;
};

#endif
