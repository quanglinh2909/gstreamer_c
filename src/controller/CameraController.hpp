#ifndef test_gstreamer_CameraController_hpp
#define test_gstreamer_CameraController_hpp

#include "service/CameraService.hpp"
#include "service/HlsPlaylist.hpp"
#include "http/ByteRange.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/utils/ConversionUtils.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include OATPP_CODEGEN_BEGIN(ApiController)

class CameraController : public oatpp::web::server::api::ApiController {
public:
    explicit CameraController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(getAll) {
        info->summary = "List cameras";
        info->addResponse<oatpp::List<oatpp::Object<CameraDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/cameras", getAll,
             QUERY(oatpp::String, limitStr,  "limit",  "50"),
             QUERY(oatpp::String, offsetStr, "offset", "0"))
    {
        v_int64 limit  = oatpp::utils::conversion::strToInt64(limitStr->c_str());
        v_int64 offset = oatpp::utils::conversion::strToInt64(offsetStr->c_str());
        return createDtoResponse(Status::CODE_200,
                                 m_service.getAllCameras(limit, offset));
    }

    ENDPOINT_INFO(getAllStreams) {
        info->summary = "List runtime stream statuses";
        info->addResponse<oatpp::List<oatpp::Object<StreamStatusDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/camera-streams", getAllStreams)
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getAllStreamStatuses());
    }

    ENDPOINT_INFO(getOne) {
        info->summary = "Get a camera by id";
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}", getOne,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getCameraById(id));
    }

    ENDPOINT_INFO(getStream) {
        info->summary = "Get runtime stream status for a camera";
        info->addResponse<oatpp::Object<StreamStatusDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/stream", getStream,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getStreamStatus(id));
    }

    ENDPOINT_INFO(create) {
        info->summary = "Create a camera";
        info->addConsumes<oatpp::Object<CreateCameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_201, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_400, "application/json");
    }
    ENDPOINT("POST", "/cameras", create,
             BODY_DTO(oatpp::Object<CreateCameraDto>, dto))
    {
        return createDtoResponse(Status::CODE_201,
                                 m_service.createCamera(dto));
    }

    ENDPOINT_INFO(update) {
        info->summary = "Update a camera";
        info->addConsumes<oatpp::Object<CreateCameraDto>>("application/json");
        info->addResponse<oatpp::Object<CameraDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("PUT", "/cameras/{id}", update,
             PATH(oatpp::String, id),
             BODY_DTO(oatpp::Object<CreateCameraDto>, dto))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.putCamera(id, dto));
    }

    ENDPOINT_INFO(startStream) {
        info->summary = "Start runtime stream for a camera";
        info->addResponse<oatpp::Object<StreamStatusDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/start", startStream,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.startStream(id));
    }

    ENDPOINT_INFO(stopStream) {
        info->summary = "Stop runtime stream for a camera";
        info->addResponse<oatpp::Object<StreamStatusDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/stop", stopStream,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.stopStream(id));
    }

    ENDPOINT_INFO(restartStream) {
        info->summary = "Restart runtime stream for a camera";
        info->addResponse<oatpp::Object<StreamStatusDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("POST", "/cameras/{id}/stream/restart", restartStream,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.restartStream(id));
    }

    ENDPOINT_INFO(getRecordings) {
        info->summary = "List recording segments for a camera";
        info->addResponse<oatpp::List<oatpp::Object<RecordingSegmentDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/recordings", getRecordings,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, from, "from", "1970-01-01T00:00:00Z"),
             QUERY(oatpp::String, to, "to", "9999-12-31T23:59:59Z"))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getRecordingSegments(id, from, to));
    }

    ENDPOINT_INFO(getPlaybackPlaylist) {
        info->summary = "Build an HLS VOD playlist for camera recordings";
        info->addResponse<String>(
            Status::CODE_200, "application/vnd.apple.mpegurl");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_400, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/playback.m3u8", getPlaybackPlaylist,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, from, "from"),
             QUERY(oatpp::String, to, "to"))
    {
        auto segmentsDto = m_service.getRecordingSegments(id, from, to);
        std::vector<playback::HlsSegment> segments;
        if (segmentsDto) {
            segments.reserve(segmentsDto->size());
            for (const auto& segment : *segmentsDto) {
                if (!segment || !segment->id || !segment->startAt ||
                    !segment->durationMs || !segment->container ||
                    segment->container != "ts") {
                    continue;
                }
                segments.push_back({
                    segment->id->c_str(),
                    segment->startAt->c_str(),
                    *segment->durationMs,
                });
            }
        }

        const std::string playlist = playback::buildVodPlaylist(segments);
        auto response = createResponse(
            Status::CODE_200,
            oatpp::String(playlist.c_str(), playlist.size()));
        response->putHeader("Content-Type", "application/vnd.apple.mpegurl");
        response->putHeader("Cache-Control", "no-store");
        response->putHeader("Access-Control-Allow-Origin", "*");
        return response;
    }

    ENDPOINT_INFO(seekRecording) {
        info->summary = "Find the recording segment containing a timestamp";
        info->addResponse<oatpp::Object<RecordingSeekDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/recordings/seek", seekRecording,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, at, "at"))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.seekRecording(id, at));
    }

    ENDPOINT_INFO(getMotionEvents) {
        info->summary = "List motion events for a camera";
        info->addResponse<oatpp::List<oatpp::Object<MotionEventDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/cameras/{id}/motion-events", getMotionEvents,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, from, "from", "1970-01-01T00:00:00Z"),
             QUERY(oatpp::String, to, "to", "9999-12-31T23:59:59Z"))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.getMotionEvents(id, from, to));
    }

    ENDPOINT_INFO(getRecordingFile) {
        info->summary = "Download a recording segment file (supports HTTP Range)";
        info->addResponse<String>(Status::CODE_200, "video/mp2t");
        info->addResponse<String>(Status::CODE_206, "video/mp2t");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/recording-segments/{id}/file", getRecordingFile,
             PATH(oatpp::String, id),
             REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        auto segment = m_service.getRecordingSegmentById(id);
        OATPP_ASSERT_HTTP(segment->path && segment->path->size() > 0,
                          Status::CODE_404,
                          "Recording file not found");

        const std::string path = segment->path->c_str();
        std::error_code ec;
        const auto fileSize =
            static_cast<v_int64>(std::filesystem::file_size(path, ec));
        OATPP_ASSERT_HTTP(!ec, Status::CODE_404, "Recording file not found");

        std::ifstream file(path, std::ios::binary);
        OATPP_ASSERT_HTTP(file.good(), Status::CODE_404, "Recording file not found");

        const auto rangeHeader = request->getHeader("Range");
        const http::ByteRange range = http::parseByteRange(
            rangeHeader ? std::string(rangeHeader->c_str()) : std::string(),
            fileSize);

        // A Range was requested but cannot be served for this file.
        if (range.present && !range.satisfiable) {
            auto response = createResponse(Status::CODE_416, "");
            response->putHeader("Content-Range", "bytes */" + std::to_string(fileSize));
            return response;
        }

        // No Range header — serve the whole file.
        if (!range.present) {
            std::string contents((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            auto response = createResponse(
                Status::CODE_200, oatpp::String(contents.data(), contents.size()));
            response->putHeader("Content-Type", contentTypeForRecording(segment->container));
            response->putHeader("Accept-Ranges", "bytes");
            response->putHeader("Access-Control-Allow-Origin", "*");
            return response;
        }

        // Satisfiable Range — read and return only [start, end].
        const v_int64 length = range.end - range.start + 1;
        std::string chunk(static_cast<std::size_t>(length), '\0');
        file.seekg(range.start);
        file.read(chunk.data(), length);
        OATPP_ASSERT_HTTP(file.gcount() == static_cast<std::streamsize>(length),
                          Status::CODE_500, "Failed to read recording file");

        auto response = createResponse(
            Status::CODE_206, oatpp::String(chunk.data(), chunk.size()));
        response->putHeader("Content-Type", contentTypeForRecording(segment->container));
        response->putHeader("Accept-Ranges", "bytes");
        response->putHeader("Access-Control-Allow-Origin", "*");
        response->putHeader("Content-Range",
                            "bytes " + std::to_string(range.start) + "-" +
                            std::to_string(range.end) + "/" + std::to_string(fileSize));
        return response;
    }

    ENDPOINT_INFO(remove) {
        info->summary = "Delete a camera";
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("DELETE", "/cameras/{id}", remove,
             PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200,
                                 m_service.deleteCamera(id));
    }

private:
    CameraService m_service;

    static const char* contentTypeForRecording(const oatpp::String& container) {
        if (container == "ts") return "video/mp2t";
        if (container == "mp4") return "video/mp4";
        return "application/octet-stream";
    }
};

#include OATPP_CODEGEN_END(ApiController)

#endif
