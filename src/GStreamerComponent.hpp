#ifndef test_gstreamer_GStreamerComponent_hpp
#define test_gstreamer_GStreamerComponent_hpp

#include "config/ConfigDto.hpp"
#include "db/CameraDb.hpp"
#include "service/GStreamerService.hpp"
#include "service/StreamTypes.hpp"

#include "oatpp/core/macro/component.hpp"

#include <iostream>

class GStreamerComponent {
public:
    OATPP_CREATE_COMPONENT(std::shared_ptr<GStreamerService>, gStreamerService)([] {
        OATPP_COMPONENT(oatpp::Object<ConfigDto>, config);
        OATPP_COMPONENT(std::shared_ptr<CameraDb>, cameraDb);
        return std::make_shared<GStreamerService>(
            toStreamConfig(config),
            [cameraDb](const stream::StreamStatusSnapshot& snapshot) {
                if (!cameraDb || snapshot.id.empty()) return;
                try {
                    auto res = cameraDb->updateCameraStreamSnapshot(
                        snapshot.id.c_str(),
                        stream::cameraStatusFromState(snapshot.state),
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
            },
            [cameraDb](const recording::RecordingSegmentSnapshot& segment) {
                if (!cameraDb || segment.cameraId.empty() || segment.path.empty()) return;
                try {
                    auto res = cameraDb->insertRecordingSegment(
                        segment.cameraId.c_str(),
                        segment.path.c_str(),
                        segment.startAt.c_str(),
                        segment.endAt.c_str(),
                        segment.durationMs,
                        segment.codec.c_str(),
                        segment.container.c_str(),
                        segment.recordingMode.c_str(),
                        segment.hasMotion);
                    if (res && !res->isSuccess()) {
                        std::cerr << "[recording] insert segment failed: "
                                  << res->getErrorMessage()->c_str() << std::endl;
                    }
                } catch (const std::exception& error) {
                    std::cerr << "[recording] insert segment threw: "
                              << error.what() << std::endl;
                } catch (...) {
                    std::cerr << "[recording] insert segment threw unknown error"
                              << std::endl;
                }
            },
            [cameraDb](const recording::MotionEventSnapshot& event) {
                if (!cameraDb || event.cameraId.empty()) return;
                try {
                    auto res = cameraDb->insertMotionEvent(
                        event.cameraId.c_str(),
                        event.startAt.c_str(),
                        event.endAt.c_str(),
                        event.maxScore);
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
        out.defaultHardware = toStdString(in->defaultHardware, out.defaultHardware);
        if (in->recordingEnabled) out.recordingEnabled = *in->recordingEnabled;
        out.recordingDir = toStdString(in->recordingDir, out.recordingDir);
        return out;
    }
};

#endif
