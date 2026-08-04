#ifndef test_gstreamer_GStreamerComponent_hpp
#define test_gstreamer_GStreamerComponent_hpp

#include "config/ConfigDto.hpp"
#include "db/CameraDb.hpp"
#include "service/GStreamerService.hpp"
#include "service/StreamTypes.hpp"
#include "service/PlaybackService.hpp"
#include "service/WebRtcService.hpp"
#include "ws/CameraStateSocket.hpp"
#include "ws/MotionEventSocket.hpp"

#include "oatpp/core/macro/component.hpp"

#include <iostream>

class GStreamerComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<GStreamerService>, gStreamerService)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);
        OATPP_COMPONENT(std::shared_ptr<CameraDb>, cameraDb);
        OATPP_COMPONENT(std::shared_ptr<ws::CameraStateRegistry>, stateRegistry);
        OATPP_COMPONENT(std::shared_ptr<ws::MotionEventRegistry>, motionRegistry);
        return std::make_shared<GStreamerService>(
            toStreamConfig(config),
            [cameraDb, stateRegistry](const stream::StreamStatusSnapshot& snapshot) {
                if (snapshot.id.empty()) return;
                if (cameraDb) {
                    try {
                        auto res = cameraDb->updateCameraStreamSnapshot(
                            snapshot.id.c_str(),
                            stream::toString(snapshot.state),
                            snapshot.inputRtsp.c_str(),
                            snapshot.outputRtsp.c_str(),
                            stream::toString(snapshot.codec),
                            snapshot.hardware.c_str(),
                            snapshot.recordingEnabled,
                            snapshot.retryCount,
                            snapshot.lastError.c_str(),
                            snapshot.lastChangedAt.c_str());
                        if (res && !res->isSuccess()) {
                            std::cerr << "[gstreamer] stream status update failed: "
                                      << res->getErrorMessage()->c_str() << std::endl;
                        }
                    } catch (const std::exception& error) {
                        std::cerr << "[gstreamer] stream status update threw: "
                                  << error.what() << std::endl;
                    } catch (...) {
                        std::cerr << "[gstreamer] stream status update threw unknown error"
                                  << std::endl;
                    }
                }
                // Push the change to any connected camera-state WebSocket
                // clients. broadcastState swallows its own send errors.
                if (stateRegistry) {
                    stateRegistry->broadcastState(snapshot);
                }
            },
            [cameraDb](const recording::RecordingSegmentSnapshot& segment) {
                if (!cameraDb || segment.cameraId.empty() || segment.path.empty()) return;
                try {
                    // status="recording": đoạn vừa MỞ (live-edge) -> chèn hàng
                    // tạm; durationMs mang segmentSeconds để DB tính end ước
                    // lượng. status="complete": đoạn đã ĐÓNG -> UPSERT finalize.
                    auto res = (segment.status == "recording")
                        ? cameraDb->insertRecordingSegmentOpen(
                              segment.cameraId.c_str(),
                              segment.path.c_str(),
                              segment.startAt.c_str(),
                              segment.durationMs,
                              segment.codec.c_str(),
                              segment.container.c_str(),
                              segment.recordingMode.c_str(),
                              segment.sessionStartMs)
                        : cameraDb->insertRecordingSegment(
                              segment.cameraId.c_str(),
                              segment.path.c_str(),
                              segment.startAt.c_str(),
                              segment.endAt.c_str(),
                              segment.durationMs,
                              segment.codec.c_str(),
                              segment.container.c_str(),
                              segment.recordingMode.c_str(),
                              segment.hasMotion,
                              segment.sessionStartMs);
                    if (res && !res->isSuccess()) {
                        std::cerr << "[recording] upsert segment failed: "
                                  << res->getErrorMessage()->c_str() << std::endl;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[recording] upsert segment threw: "
                              << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "[recording] upsert segment threw unknown error"
                              << std::endl;
                }
            },
            [cameraDb, motionRegistry](const recording::MotionEventSnapshot& event) {
                if (event.cameraId.empty()) return;
                // Bắn cho client TRƯỚC khi ghi DB: Live View chỉ cần biết
                // "vừa có chuyển động ở mấy ô này", không nên chờ một vòng
                // ghi đĩa mới thấy.
                if (motionRegistry) motionRegistry->broadcastEvent(event);
                // Tắt "lưu sự kiện" thì DỪNG ở đây: live vẫn vẽ được vì đã bắn
                // WebSocket ở trên, chỉ không còn lịch sử. Giống hệt nhận diện
                // khẩu trang — loại đó vốn không có bảng nào.
                if (!event.saveToDb) return;
                if (!cameraDb) return;
                try {
                    auto res = cameraDb->insertMotionEvent(
                        event.cameraId.c_str(),
                        event.startAt.c_str(),
                        event.endAt.c_str(),
                        event.maxScore,
                        event.cells.c_str(),
                        static_cast<v_int32>(event.gridX),
                        static_cast<v_int32>(event.gridY),
                        event.imagePath.c_str());
                    if (res && !res->isSuccess()) {
                        std::cerr << "[recording] insert motion event failed: "
                                  << res->getErrorMessage()->c_str() << std::endl;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[recording] insert motion event threw: "
                              << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "[recording] insert motion event threw unknown error"
                              << std::endl;
                }
            },
            // KHUNG chuyển động: chỉ đẩy ra WebSocket, KHÔNG ghi DB. Nó bắn 5
            // lần/giây cho mỗi camera đang động — ghi đĩa ngần đó là vô nghĩa,
            // dữ liệu này chỉ để vẽ lên video đang xem. Registry tự bỏ qua nếu
            // không ai đăng ký camera đó.
            [motionRegistry](const recording::MotionFrameSnapshot& frame) {
                if (motionRegistry) motionRegistry->broadcastFrame(frame);
            });
    }());

private:
    static std::string toStdString(const oatpp::String& value, const std::string& fallback) {
        return value ? std::string(value->c_str()) : fallback;
    }

    static stream::GStreamerConfig toStreamConfig(const oatpp::Object<ConfigDto>& config) {
        stream::GStreamerConfig out;
        if (!config || !config->gstreamer) return out;

        const auto& in = config->gstreamer;
        out.rtspHost = toStdString(in->rtspHost, out.rtspHost);
        out.publicRtspHost = toStdString(in->publicRtspHost, out.publicRtspHost);
        if (in->rtspPort) out.rtspPort = static_cast<uint16_t>(*in->rtspPort);
        if (in->retryInitialMs) out.retryInitialMs = *in->retryInitialMs;
        if (in->retryMaxMs) out.retryMaxMs = *in->retryMaxMs;
        if (in->sourceLatencyMs) out.sourceLatencyMs = *in->sourceLatencyMs;
        if (in->healthCheckIntervalMs) out.healthCheckIntervalMs = *in->healthCheckIntervalMs;
        out.defaultHardware = toStdString(in->defaultHardware, out.defaultHardware);
        if (in->recordingEnabled) out.recordingEnabled = *in->recordingEnabled;
        out.recordingDir = toStdString(in->recordingDir, out.recordingDir);
        out.motionSnapshotDir = toStdString(in->motionSnapshotDir, out.motionSnapshotDir);
        out.webrtcStunServer = toStdString(in->webrtcStunServer, out.webrtcStunServer);
        out.webrtcTurnServer = toStdString(in->webrtcTurnServer, out.webrtcTurnServer);
        return out;
    }

public:
    // Sổ đăng ký phiên xem WebRTC. Tách khỏi GStreamerService vì vòng đời khác
    // hẳn: session sống theo tab trình duyệt, còn stream sống theo camera.
    OATPP_CREATE_COMPONENT(std::shared_ptr<webrtc::WebRtcService>, webRtcService)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);
        OATPP_COMPONENT(std::shared_ptr<GStreamerService>, streams);
        // Dùng CHUNG registry nguồn của GStreamerService: xem live và ghi hình
        // qua đó chia sẻ đúng một kết nối RTSP tới mỗi camera.
        auto service = std::make_shared<webrtc::WebRtcService>(
            toStreamConfig(config), streams->sourceRegistry());

        // Camera bị dựng lại (đổi setting, đổi codec, restart) thì mount RTSP
        // cũ biến mất — mọi phiên xem đang bám vào nó phải chết ngay. Trình
        // duyệt tự nối lại và phiên mới được dựng theo codec/độ phân giải mới.
        // Không có mắc nối này thì người xem đứng hình cho tới khi tự bấm lại.
        //
        // weak_ptr: sink sống trong GStreamerService, giữ shared_ptr sẽ tạo
        // vòng tham chiếu khiến cả hai service không bao giờ được giải phóng.
        std::weak_ptr<webrtc::WebRtcService> weak = service;
        streams->setStreamResetSink([weak](const std::string& cameraId) {
            if (auto locked = weak.lock()) locked->destroySessionsForCamera(cameraId);
        });
        return service;
    }());

    // Sổ phiên XEM LẠI. Dựa trên WebRtcService (dùng chung watchdog và đường
    // thương lượng SDP), chỉ thêm đường dây điều khiển seek/tốc độ/tạm dừng.
    OATPP_CREATE_COMPONENT(std::shared_ptr<playback::PlaybackService>, playbackService)([] {
        OATPP_COMPONENT(std::shared_ptr<webrtc::WebRtcService>, webrtc);
        return std::make_shared<playback::PlaybackService>(webrtc);
    }());
};

#endif
