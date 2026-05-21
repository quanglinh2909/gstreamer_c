#ifndef test_gstreamer_CameraService_hpp
#define test_gstreamer_CameraService_hpp

#include "db/CameraDb.hpp"
#include "dto/CameraDto.hpp"
#include "dto/RecordingDto.hpp"
#include "dto/StreamStatusDto.hpp"
#include "dto/StatusDto.hpp"
#include "http/Uuid.hpp"
#include "service/GStreamerService.hpp"
#include "service/RecordingTypes.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <string>

class CameraService {
public:
    using Status = oatpp::web::protocol::http::Status;

    oatpp::Object<CameraDto> createCamera(const oatpp::Object<CreateCameraDto>& in) {
        validate(in, /* requireAll */ true);
        auto status = in->status ? in->status : oatpp::String("offline");

        auto res = m_db->createCamera(in->name, in->rtsp, status,
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
        auto res = m_db->updateCamera(id, in->name, in->rtsp, in->status,
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
        m_streams->cleanupCamera(id);

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
        if (in->status) {
            auto s = in->status;
            OATPP_ASSERT_HTTP(s == "online" || s == "offline" || s == "error",
                              Status::CODE_400, "status must be online|offline|error");
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
