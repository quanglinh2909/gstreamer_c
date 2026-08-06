#ifndef test_gstreamer_MoqController_hpp
#define test_gstreamer_MoqController_hpp

// Đường điều khiển cho MoQ (Media over QUIC) — đường xem THỨ HAI, song song
// với WHEP/WebRTC.
//
//   POST   /moq/feeds          {feed, cameraId, mode, atMs?, rate?} -> {sessionId}
//   DELETE /moq/feeds/{id}                                          -> 204
//   GET    /moq/feeds                                               -> {total, ...}
//
// AI GỌI: chỉ máy chủ MoQ (tiến trình Python giữ phần QUIC/WebTransport), khi
// trình duyệt gửi SUBSCRIBE. Trình duyệt KHÔNG gọi thẳng vào đây — nó chỉ nói
// MoQ. Bởi vậy body không có SDP, không có ICE: phần vận chuyển đã nằm chỗ
// khác, ở đây chỉ còn "mở một đường bơm access unit".
//
// XEM LẠI dùng chung y nguyên /playback/{sessionId}/control của WHEP — xem
// PlaybackService::adopt(). Không có đường điều khiển riêng cho MoQ.

#include "dto/MoqDto.hpp"
#include "service/CameraService.hpp"
#include "service/GStreamerService.hpp"
#include "service/MoqFeedService.hpp"
#include "service/PlaybackService.hpp"
#include "service/PlaybackSource.hpp"
#include "service/RecordingSegments.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include OATPP_CODEGEN_BEGIN(ApiController)

class MoqController : public oatpp::web::server::api::ApiController {
public:
    explicit MoqController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(moqCreateFeed) {
        info->summary = "Open a MoQ frame feed (called by the MoQ server)";
        info->description =
            "mode=live bám vào nguồn RTSP dùng chung của camera; mode=playback "
            "dựng một PlaybackSource đọc bản ghi từ atMs. Trả sessionId — dùng "
            "nó cho /playback/{sessionId}/control khi xem lại.";
        info->addResponse<oatpp::Object<MoqFeedDto>>(Status::CODE_201, "application/json");
        info->addResponse<String>(Status::CODE_400, "text/plain");
        info->addResponse<String>(Status::CODE_503, "text/plain");
    }
    ENDPOINT("POST", "/moq/feeds", moqCreateFeed,
             BODY_DTO(oatpp::Object<MoqFeedRequestDto>, body))
    {
        const std::string feedId = body && body->feed ? body->feed->c_str() : "";
        const std::string cameraId = body && body->cameraId ? body->cameraId->c_str() : "";
        const std::string mode = body && body->mode ? body->mode->c_str() : "live";
        if (feedId.empty() || cameraId.empty()) {
            return createResponse(Status::CODE_400, "Missing feed/cameraId");
        }
        if (mode == "playback") return startPlayback(body, feedId, cameraId);
        return startLive(feedId, cameraId);
    }

    ENDPOINT_INFO(moqDeleteFeed) {
        info->summary = "Close a MoQ frame feed";
        info->addResponse<String>(Status::CODE_204, "text/plain");
    }
    ENDPOINT("DELETE", "/moq/feeds/{sessionId}", moqDeleteFeed,
             PATH(oatpp::String, sessionId))
    {
        const std::string sid = sessionId ? sessionId->c_str() : "";
        if (!m_moq->destroySession(sid)) {
            return createResponse(Status::CODE_404, "No such MoQ feed");
        }
        return createResponse(Status::CODE_204, "");
    }

    ENDPOINT_INFO(moqViewers) {
        info->summary = "How many viewers are watching over MoQ";
        info->addResponse<String>(Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/moq/feeds", moqViewers,
             QUERY(oatpp::String, cameraId, "cameraId", ""))
    {
        const std::string filter = cameraId ? cameraId->c_str() : "";
        const size_t total =
            filter.empty() ? m_moq->viewerCount() : m_moq->viewerCount(filter);
        std::ostringstream json;
        json << "{\"total\":" << total << "}";
        auto response = createResponse(Status::CODE_200, json.str().c_str());
        response->putHeader("Content-Type", "application/json");
        return response;
    }

private:
    std::shared_ptr<OutgoingResponse> startLive(const std::string& feedId,
                                                const std::string& cameraId) {
        // Cùng kiểm tra như WHEP: camera chưa online thì báo ngay, đừng để
        // người xem treo ở màn đen mà không có lấy một dòng lỗi.
        auto status = m_gstreamer->getStatus(oatpp::String(cameraId.c_str()));
        const std::string state = status && status->state ? status->state->c_str() : "offline";
        if (state != "online") {
            const std::string detail = "Camera dang " + state + ", chua co luong de phat";
            return createResponse(Status::CODE_503, detail.c_str());
        }
        const std::string codec = status->codec ? status->codec->c_str() : "h264";
        const std::string rtsp = status->inputRtsp ? status->inputRtsp->c_str() : "";
        if (rtsp.empty()) {
            return createResponse(Status::CODE_503, "Camera chua co luong RTSP dau vao");
        }

        auto result = m_moq->createLive(cameraId, feedId, rtsp, codec);
        if (!result.ok()) {
            return createResponse(Status::CODE_503, result.error.c_str());
        }
        return created(result.sessionId);
    }

    std::shared_ptr<OutgoingResponse> startPlayback(
        const oatpp::Object<MoqFeedRequestDto>& body,
        const std::string& feedId, const std::string& cameraId)
    {
        const int64_t startMs = body->atMs ? *body->atMs : 0;
        if (startMs <= 0) {
            return createResponse(Status::CODE_400, "Missing/invalid atMs");
        }
        double speed = body->rate ? *body->rate : 1.0;
        if (!(speed > 0)) speed = 1.0;

        // Codec lấy từ CHÍNH bản ghi, không từ camera đang chạy — camera có
        // thể đã đổi codec sau khi ghi, file cũ thì không đổi theo.
        CameraService service = m_service;
        auto segments =
            playback::loadSegments(service, cameraId, startMs - 1000, startMs + 60'000);
        std::string codec = segments.empty() ? std::string("h264") : segments.front().codec;
        if (codec != "h264" && codec != "h265") codec = "h264";

        auto loader = [service, cameraId](int64_t fromMs, int64_t toMs) mutable {
            std::vector<stream::PlaybackSegment> out;
            for (auto& seg : playback::loadSegments(service, cameraId, fromMs, toMs)) {
                out.push_back({seg.id, seg.path, seg.startMs, seg.endMs});
            }
            return out;
        };

        auto source = std::make_shared<stream::PlaybackSource>(
            cameraId, codec, std::move(loader), startMs);
        if (speed != 1.0) source->setRate(speed);
        source->start();

        auto result = m_moq->createWithSource(cameraId, feedId, source);
        if (!result.ok()) {
            source->stop();
            return createResponse(Status::CODE_503, result.error.c_str());
        }
        // Đăng ký để /playback/{sessionId}/control điều khiển được nguồn này.
        m_playback->adopt(result.sessionId, source);
        return created(result.sessionId);
    }

    std::shared_ptr<OutgoingResponse> created(const std::string& sessionId) {
        auto dto = MoqFeedDto::createShared();
        dto->sessionId = sessionId.c_str();
        return createDtoResponse(Status::CODE_201, dto);
    }

    CameraService m_service;
    OATPP_COMPONENT(std::shared_ptr<moq::MoqFeedService>, m_moq);
    OATPP_COMPONENT(std::shared_ptr<playback::PlaybackService>, m_playback);
    OATPP_COMPONENT(std::shared_ptr<GStreamerService>, m_gstreamer);
};

#include OATPP_CODEGEN_END(ApiController)

#endif  // test_gstreamer_MoqController_hpp
