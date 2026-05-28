#ifndef test_gstreamer_CameraService_hpp
#define test_gstreamer_CameraService_hpp

#include "ai/AiManager.hpp"
#include "db/CameraDb.hpp"
#include "dto/CameraDto.hpp"
#include "dto/RecordingDto.hpp"
#include "dto/StreamStatusDto.hpp"
#include "dto/StatusDto.hpp"
#include "http/Uuid.hpp"
#include "service/GStreamerService.hpp"
#include "service/RecordingTypes.hpp"
#include "service/SnapshotGrabber.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <string>

class CameraService {
public:
    using Status = oatpp::web::protocol::http::Status;

    oatpp::Object<CameraDto> createCamera(const oatpp::Object<CreateCameraDto>& in) {
        validate(in, /* requireAll */ true);

        auto res = m_db->createCamera(in->name, in->rtsp,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds, in->motionKeyframeOnly);
        assertSuccess(res);
        auto camera = fetchOne(res, Status::CODE_500, "Failed to create camera");
        m_streams->startCamera(camera);
        return getCameraById(camera->id);
    }

    oatpp::Object<CameraDto> getCameraById(const oatpp::String& id) {
        validateUuid(id, "Invalid camera id");
        auto res = m_db->getCameraById(id);
        assertSuccess(res);
        return fetchOne(res, Status::CODE_404, "Camera not found");
    }

    // Captures one JPEG frame from the camera's live RTSP stream.
    //
    // Fast path: if the in-process AI pipeline is running for this camera
    // it has already RGA-cropped and hardware-encoded the latest frame to
    // JPEG; serve that without touching the network. This is both faster
    // (microseconds, no RTSP handshake / first-I-frame wait) and friendlier
    // to the camera (no extra concurrent RTSP session, which is what
    // causes the intermittent 502s under load).
    //
    // Fallback: AI not running for the camera, or the cached frame is
    // older than the freshness window — open the short-lived snapshot
    // pipeline and grab from RTSP.
    std::string getSnapshot(const oatpp::String& id) {
        auto camera = getCameraById(id);  // validates id, 404 if missing
        const std::string cameraId =
            camera->id ? std::string(camera->id->c_str()) : std::string();

        if (!cameraId.empty()) {
            auto cached = m_ai->getLatestJpeg(cameraId, /*maxAgeMs=*/2000);
            if (!cached.empty()) {
                return std::string(cached.begin(), cached.end());
            }
        }

        const std::string rtsp =
            camera->rtsp ? std::string(camera->rtsp->c_str()) : std::string();
        auto grab = snapshot::grabJpeg(rtsp,
                                       m_streams->getConfig().sourceLatencyMs);
        if (!grab.ok()) {
            const std::string message =
                grab.error.empty() ? "Snapshot failed" : grab.error;
            OATPP_ASSERT_HTTP(false, Status::CODE_502, message.c_str());
        }
        return std::string(grab.jpeg.begin(), grab.jpeg.end());
    }

    oatpp::List<oatpp::Object<CameraDto>>
    getAllCameras(const oatpp::Int64& limit, const oatpp::Int64& offset) {
        auto res = m_db->getAllCameras(limit, offset);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<CameraDto>>>();
    }

    oatpp::List<oatpp::Object<CameraDto>> getAllCamerasForStartup() {
        auto res = m_db->getAllCamerasForStartup();
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<CameraDto>>>();
    }

    oatpp::Object<CameraDto> putCamera(
        const oatpp::String& id,
        const oatpp::Object<CreateCameraDto>& in)
    {
        validate(in, /* requireAll */ false);
        validateUuid(id, "Invalid camera id");
        const bool shouldRestart = streamRelevantInputPresent(in);

        // Pass nullable strings straight through — the SQL COALESCEs against
        // the existing row, so missing fields keep their current values.
        auto res = m_db->updateCamera(id, in->name, in->rtsp,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds, in->motionKeyframeOnly);
        assertSuccess(res);
        auto camera = fetchOne(res, Status::CODE_404, "Camera not found");
        if (shouldRestart) {
            m_streams->restartCamera(camera);
            camera = getCameraById(id);
        }
        return camera;
    }

    oatpp::Object<StatusDto> deleteCamera(const oatpp::String& id) {
        validateUuid(id, "Invalid camera id");
        auto res = m_db->deleteCameraReturning(id);
        assertSuccess(res);
        fetchOne(res, Status::CODE_404, "Camera not found");
        // ai_jobs rows are removed by ON DELETE CASCADE; stop the live AI
        // runtime (pipeline + workers) so it does not outlive the camera.
        m_streams->cleanupCamera(id);
        m_ai->removeCamera(id->c_str());

        auto dto = StatusDto::createShared();
        dto->statusCode = 200;
        dto->message = "Deleted";
        return dto;
    }

    void startAllStreamsFromDatabase() {
        auto cameras = getAllCamerasForStartup();
        for (const auto& camera : *cameras) {
            m_streams->startCamera(camera);
        }
    }

    oatpp::Object<StreamStatusDto> getStreamStatus(const oatpp::String& id) {
        auto camera = getCameraById(id);
        return m_streams->getStatus(camera);
    }

    oatpp::List<oatpp::Object<StreamStatusDto>> getAllStreamStatuses() {
        return m_streams->getAllStatuses();
    }

    oatpp::Object<StreamStatusDto> startStream(const oatpp::String& id) {
        auto camera = getCameraById(id);
        m_streams->startCamera(camera);
        return m_streams->getStatus(id);
    }

    oatpp::Object<StreamStatusDto> stopStream(const oatpp::String& id) {
        getCameraById(id);
        m_streams->stopCamera(id);
        return m_streams->getStatus(id);
    }

    oatpp::Object<StreamStatusDto> restartStream(const oatpp::String& id) {
        auto camera = getCameraById(id);
        m_streams->restartCamera(camera);
        return m_streams->getStatus(id);
    }

    oatpp::List<oatpp::Object<RecordingSegmentDto>> getRecordingSegments(
        const oatpp::String& id,
        const oatpp::String& from,
        const oatpp::String& to)
    {
        getCameraById(id);
        auto res = m_db->listRecordingSegments(id, from, to);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<RecordingSegmentDto>>>();
    }

    oatpp::Object<RecordingSeekDto> seekRecording(
        const oatpp::String& id,
        const oatpp::String& at)
    {
        getCameraById(id);
        auto res = m_db->seekRecordingSegment(id, at);
        assertSuccess(res);
        return fetchOneTyped<RecordingSeekDto>(res, Status::CODE_404, "Recording segment not found");
    }

    oatpp::Object<RecordingSegmentDto> getRecordingSegmentById(const oatpp::String& id) {
        validateUuid(id, "Invalid recording segment id");
        auto res = m_db->getRecordingSegmentById(id);
        assertSuccess(res);
        return fetchOneTyped<RecordingSegmentDto>(res, Status::CODE_404, "Recording segment not found");
    }

    oatpp::List<oatpp::Object<MotionEventDto>> getMotionEvents(
        const oatpp::String& id,
        const oatpp::String& from,
        const oatpp::String& to)
    {
        getCameraById(id);
        auto res = m_db->listMotionEvents(id, from, to);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<MotionEventDto>>>();
    }

private:
    OATPP_COMPONENT(std::shared_ptr<CameraDb>, m_db);
    OATPP_COMPONENT(std::shared_ptr<GStreamerService>, m_streams);
    OATPP_COMPONENT(std::shared_ptr<AiManager>, m_ai);

    static void validate(const oatpp::Object<CreateCameraDto>& in, bool requireAll) {
        OATPP_ASSERT_HTTP(in, Status::CODE_400, "Body required");

        if (requireAll) {
            OATPP_ASSERT_HTTP(in->name && in->name->size() > 0,
                              Status::CODE_400, "name required");
            OATPP_ASSERT_HTTP(in->rtsp && in->rtsp->size() > 0,
                              Status::CODE_400, "rtsp required");
        } else {
            if (in->name) {
                OATPP_ASSERT_HTTP(in->name->size() > 0,
                                  Status::CODE_400, "name must not be empty");
            }
            if (in->rtsp) {
                OATPP_ASSERT_HTTP(in->rtsp->size() > 0,
                                  Status::CODE_400, "rtsp must not be empty");
            }
        }

        if (in->rtsp) {
            OATPP_ASSERT_HTTP(in->rtsp->find("rtsp://") == 0,
                              Status::CODE_400, "rtsp must start with rtsp://");
        }
        if (in->hardware) {
            OATPP_ASSERT_HTTP(in->hardware->size() > 0,
                              Status::CODE_400, "hardware must not be empty");
        }
        if (in->recordingMode) {
            OATPP_ASSERT_HTTP(recording::isValidRecordingMode(in->recordingMode->c_str()),
                              Status::CODE_400, "recordingMode must be off|always|motion");
        }
        if (in->motionSensitivity) {
            OATPP_ASSERT_HTTP(*in->motionSensitivity >= 0.0 && *in->motionSensitivity <= 1.0,
                              Status::CODE_400, "motionSensitivity must be between 0 and 1");
        }
        if (in->motionThreshold) {
            OATPP_ASSERT_HTTP(*in->motionThreshold >= 0.0 && *in->motionThreshold <= 1.0,
                              Status::CODE_400, "motionThreshold must be between 0 and 1");
        }
        if (in->segmentSeconds) {
            OATPP_ASSERT_HTTP(*in->segmentSeconds >= 1 && *in->segmentSeconds <= 3600,
                              Status::CODE_400, "segmentSeconds must be between 1 and 3600");
        }
    }

    static void validateUuid(const oatpp::String& id, const char* message) {
        OATPP_ASSERT_HTTP(id && http::isUuid(id->c_str()),
                          Status::CODE_400,
                          message);
    }

    template <class Res>
    static void assertSuccess(const std::shared_ptr<Res>& res) {
        OATPP_ASSERT_HTTP(res->isSuccess(),
                          Status::CODE_500,
                          res->getErrorMessage()->c_str());
    }

    template <class Res>
    static oatpp::Object<CameraDto> fetchOne(
        const std::shared_ptr<Res>& res,
        const oatpp::web::protocol::http::Status& notFoundStatus,
        const char* notFoundMsg)
    {
        return fetchOneTyped<CameraDto>(res, notFoundStatus, notFoundMsg);
    }

    template <class Dto, class Res>
    static oatpp::Object<Dto> fetchOneTyped(
        const std::shared_ptr<Res>& res,
        const oatpp::web::protocol::http::Status& notFoundStatus,
        const char* notFoundMsg)
    {
        OATPP_ASSERT_HTTP(res->hasMoreToFetch(), notFoundStatus, notFoundMsg);
        auto list = res->template fetch<oatpp::List<oatpp::Object<Dto>>>();
        OATPP_ASSERT_HTTP(list && list->size() > 0, notFoundStatus, notFoundMsg);
        return list[0];
    }

    static bool streamRelevantInputPresent(const oatpp::Object<CreateCameraDto>& in) {
        return in->rtsp || in->hardware ||
               recording::recordingPatchRequiresRuntimeRestart(
                   static_cast<bool>(in->recordingEnabled),
                   static_cast<bool>(in->recordingMode),
                   static_cast<bool>(in->motionEnabled),
                   static_cast<bool>(in->motionSensitivity),
                   static_cast<bool>(in->motionThreshold),
                   static_cast<bool>(in->preMotionSeconds),
                   static_cast<bool>(in->postMotionSeconds),
                   static_cast<bool>(in->segmentSeconds),
                   static_cast<bool>(in->motionKeyframeOnly));
    }
};

#endif
