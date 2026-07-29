#ifndef test_gstreamer_PlaybackController_hpp
#define test_gstreamer_PlaybackController_hpp

// XEM LẠI qua WebRTC — cùng bắt tay WHEP như xem trực tiếp, thêm một đường
// điều khiển:
//
//   POST   /cameras/{id}/playback/whep?at=<epochMs>&rate=<n>   body = SDP offer
//                                                             -> 201 + SDP answer
//   POST   /playback/{sessionId}/control   {atMs?, rate?, paused?} -> trạng thái
//   GET    /playback/{sessionId}                                   -> trạng thái
//   DELETE /playback/{sessionId}                                   -> 204
//
// Vì sao tách khỏi HLS: với HLS mỗi cú bấm timeline phải tải lại playlist cả
// ngày (đo được 1,17 MB / 9.122 dòng) rồi dựng lại player. Ở đây phiên mở một
// lần, mỗi cú bấm chỉ là một POST /control vài trăm byte.
//
// GET /playback/{id} vừa trả trạng thái vừa là NHỊP TIM: phiên xem lại có thể
// im lặng hợp lệ (đang tạm dừng) nên watchdog không đo được sức khoẻ bằng dòng
// dữ liệu như phiên live — xem WebRtcSession::useHeartbeatExpiry().

#include "dto/PlaybackDto.hpp"
#include "service/CameraService.hpp"
#include "service/HlsPlaylist.hpp"
#include "service/PlaybackService.hpp"
#include "service/ThumbnailExtractor.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

#include OATPP_CODEGEN_BEGIN(ApiController)

class PlaybackController : public oatpp::web::server::api::ApiController {
public:
    explicit PlaybackController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(playbackOffer) {
        info->summary = "Start a WebRTC playback (recording) session";
        info->description =
            "Body is a raw SDP offer. `at` is the epoch-ms wall clock position "
            "to start from; `rate` is the playback speed (>=4 sends keyframes "
            "only). Location header carries the control/session URL.";
        info->addResponse<String>(Status::CODE_201, "application/sdp");
        info->addResponse<String>(Status::CODE_400, "text/plain");
        info->addResponse<String>(Status::CODE_503, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/playback/whep", playbackOffer,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, at, "at"),
             QUERY(oatpp::String, rate, "rate", "1"),
             BODY_STRING(oatpp::String, offerSdp),
             REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        const std::string cameraId = id ? id->c_str() : "";
        const std::string offer = offerSdp ? offerSdp->c_str() : "";
        if (offer.empty()) {
            return withCors(createResponse(Status::CODE_400,
                                           "Missing SDP offer in request body"));
        }

        const int64_t startMs = at ? std::strtoll(at->c_str(), nullptr, 10) : 0;
        if (startMs <= 0) {
            return withCors(createResponse(Status::CODE_400,
                                           "Missing/invalid 'at' (epoch ms)"));
        }
        double speed = rate ? std::strtod(rate->c_str(), nullptr) : 1.0;
        if (!(speed > 0)) speed = 1.0;

        // Codec lấy từ CHÍNH bản ghi chứ không từ camera đang chạy: camera có
        // thể đã đổi codec sau khi ghi, mà file cũ thì không đổi theo.
        CameraService service = m_service;
        auto segments = loadSegments(service, cameraId, startMs - 1000, startMs + 60'000);
        std::string codec = segments.empty() ? std::string("h264") : segments.front().codec;
        if (codec != "h264" && codec != "h265") codec = "h264";

        auto loader = [service, cameraId](int64_t fromMs, int64_t toMs) mutable {
            std::vector<stream::PlaybackSegment> out;
            for (auto& seg : loadSegments(service, cameraId, fromMs, toMs)) {
                out.push_back({seg.id, seg.path, seg.startMs, seg.endMs});
            }
            return out;
        };

        const std::string clientHint =
            firstIpv4(request ? request->getHeader("X-Forwarded-For") : nullptr);

        auto result = m_playback->start(cameraId, codec, offer, clientHint,
                                        startMs, speed, std::move(loader));
        if (!result.ok()) {
            return withCors(createResponse(Status::CODE_503, result.error.c_str()));
        }

        auto response = createResponse(Status::CODE_201, result.answerSdp.c_str());
        response->putHeader("Content-Type", "application/sdp");
        response->putHeader("Location",
                            ("/playback/" + result.sessionId).c_str());
        return withCors(response);
    }

    ENDPOINT_INFO(playbackControl) {
        info->summary = "Seek / change speed / pause a playback session";
        info->addResponse<oatpp::Object<PlaybackStatusDto>>(
            Status::CODE_200, "application/json");
        info->addResponse<String>(Status::CODE_404, "text/plain");
    }
    ENDPOINT("POST", "/playback/{sessionId}/control", playbackControl,
             PATH(oatpp::String, sessionId),
             BODY_DTO(oatpp::Object<PlaybackControlDto>, body))
    {
        const std::string sid = sessionId ? sessionId->c_str() : "";
        playback::PlaybackStatus status;

        int64_t seekTo = 0;
        double speed = 0;
        bool paused = false;
        // So với nullptr chứ KHÔNG viết `if (body->paused)`: operator bool của
        // oatpp::Boolean trả về GIÁ TRỊ, không phải "có mặt hay không". Viết
        // tắt thì lệnh {"paused": false} bị bỏ qua — bấm tạm dừng được nhưng
        // bấm phát tiếp thì không có tác dụng. Int64/Float64 cũng vậy với giá
        // trị 0.
        const bool hasSeek = body && body->atMs != nullptr;
        const bool hasRate = body && body->rate != nullptr;
        const bool hasPaused = body && body->paused != nullptr;
        if (hasSeek) seekTo = *body->atMs;
        if (hasRate) speed = *body->rate;
        if (hasPaused) paused = *body->paused;

        const bool ok = m_playback->control(sid,
                                            hasSeek ? &seekTo : nullptr,
                                            hasRate ? &speed : nullptr,
                                            hasPaused ? &paused : nullptr,
                                            status);
        if (!ok) {
            return withCors(createResponse(Status::CODE_404, "No such playback session"));
        }
        return withCors(createDtoResponse(Status::CODE_200, toDto(sid, status)));
    }

    ENDPOINT_INFO(playbackStatus) {
        info->summary = "Playback session status (doubles as the keep-alive ping)";
        info->addResponse<oatpp::Object<PlaybackStatusDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/playback/{sessionId}", playbackStatus,
             PATH(oatpp::String, sessionId))
    {
        const std::string sid = sessionId ? sessionId->c_str() : "";
        playback::PlaybackStatus status;
        if (!m_playback->status(sid, status)) {
            return withCors(createResponse(Status::CODE_404, "No such playback session"));
        }
        return withCors(createDtoResponse(Status::CODE_200, toDto(sid, status)));
    }

    ENDPOINT_INFO(playbackStop) {
        info->summary = "Stop a playback session";
        info->addResponse<String>(Status::CODE_204, "text/plain");
    }
    ENDPOINT("DELETE", "/playback/{sessionId}", playbackStop,
             PATH(oatpp::String, sessionId))
    {
        m_playback->destroy(sessionId ? sessionId->c_str() : "");
        return withCors(createResponse(Status::CODE_204, ""));
    }

    ENDPOINT_INFO(playbackThumbnail) {
        info->summary = "Single JPEG frame at a wall-clock position (timeline hover preview)";
        info->description =
            "Decodes one keyframe (<=1 GOP before `at`) from the recorded .ts and "
            "returns it as JPEG. `at` is epoch-ms; `w` is the output width in px.";
        info->addResponse<String>(Status::CODE_200, "image/jpeg");
        info->addResponse<String>(Status::CODE_404, "text/plain");
    }
    ENDPOINT("GET", "/cameras/{id}/thumbnail", playbackThumbnail,
             PATH(oatpp::String, id),
             QUERY(oatpp::String, at, "at"),
             QUERY(oatpp::String, w, "w", "160"))
    {
        const std::string cameraId = id ? id->c_str() : "";
        const int64_t atMs = at ? std::strtoll(at->c_str(), nullptr, 10) : 0;
        if (atMs <= 0) {
            return withCors(createResponse(Status::CODE_400,
                                           "Missing/invalid 'at' (epoch ms)"));
        }
        int width = w ? static_cast<int>(std::strtol(w->c_str(), nullptr, 10)) : 160;
        if (width < 48) width = 48;
        if (width > 640) width = 640;

        // Cửa sổ RỘNG (3 phút): sát mép live, đoạn CHỨA mốc đang được ghi nên
        // chưa 'complete' (loadSegments chỉ lấy 'complete'). Phải với tới đoạn
        // hoàn tất gần nhất TRƯỚC đó — với camera ghi 60s/đoạn nó có thể bắt đầu
        // tới ~120s trước mốc. Không nới đủ rộng thì gần realtime không tìm ra
        // đoạn nào -> 404 -> frontend rơi về chip giờ nhỏ (nhìn như "ảnh bị nhỏ
        // lại").
        CameraService service = m_service;
        auto rows = loadSegments(service, cameraId, atMs - 180'000, atMs + 5'000);

        // Ưu tiên đoạn CHỨA mốc; nếu không có (mốc trong đoạn đang ghi hoặc rơi
        // vào khoảng trống) thì lấy đoạn hoàn tất gần nhất TRƯỚC đó.
        const SegmentRow* chosen = nullptr;
        for (const auto& seg : rows) {
            if (seg.startMs <= atMs && seg.endMs > atMs) { chosen = &seg; break; }
            if (seg.startMs <= atMs && (!chosen || seg.startMs > chosen->startMs)) {
                chosen = &seg;  // gần nhất trước mốc
            }
        }
        if (!chosen) {
            return withCors(createResponse(Status::CODE_404,
                                           "No recording at that time"));
        }

        // Mốc nằm TRONG đoạn -> lấy đúng chỗ. Mốc nằm SAU đoạn hoàn tất gần nhất
        // (đang trong đoạn đang ghi / khoảng trống) -> lấy KHUNG CUỐI của đoạn
        // đó = ảnh mới nhất sẵn có; offset quá độ dài đoạn sẽ seek lố -> EOS ->
        // ảnh rỗng.
        int64_t offsetMs;
        if (atMs < chosen->endMs) {
            offsetMs = std::max<int64_t>(0, atMs - chosen->startMs);
        } else {
            offsetMs = std::max<int64_t>(0, (chosen->endMs - chosen->startMs) - 500);
        }
        std::string jpeg =
            stream::ThumbnailExtractor::extract(chosen->path, chosen->codec,
                                                offsetMs, width);
        if (jpeg.empty()) {
            return withCors(createResponse(Status::CODE_404,
                                           "Could not decode frame"));
        }

        auto response = createResponse(Status::CODE_200,
                                       oatpp::String(jpeg.data(), jpeg.size()));
        response->putHeader("Content-Type", "image/jpeg");
        // Khung tại một mốc quá khứ không đổi -> cho trình duyệt cache lâu; mốc
        // đã làm tròn về bó 10s ở client nên URL trùng nhau tái dùng được.
        response->putHeader("Cache-Control", "public, max-age=86400");
        return withCors(response);
    }

    ENDPOINT("OPTIONS", "/cameras/{id}/playback/whep", playbackOfferOptions,
             PATH(oatpp::String, id))
    {
        (void)id;
        return withCors(createResponse(Status::CODE_204, ""));
    }

    ENDPOINT("OPTIONS", "/playback/{sessionId}", playbackSessionOptions,
             PATH(oatpp::String, sessionId))
    {
        (void)sessionId;
        return withCors(createResponse(Status::CODE_204, ""));
    }

    ENDPOINT("OPTIONS", "/playback/{sessionId}/control", playbackControlOptions,
             PATH(oatpp::String, sessionId))
    {
        (void)sessionId;
        return withCors(createResponse(Status::CODE_204, ""));
    }

private:
    struct SegmentRow {
        std::string id;
        std::string path;
        std::string codec;
        int64_t startMs = 0;
        int64_t endMs = 0;
    };

    // Đọc các đoạn 'complete' trong khoảng [fromMs, toMs).
    //
    // NUỐT mọi ngoại lệ: hàm này còn được gọi TỪ THREAD FEEDER của
    // PlaybackSource, mà CameraService ném lỗi HTTP của oatpp — ngoại lệ thoát
    // khỏi một thread không phải thread HTTP sẽ giết cả tiến trình.
    static std::vector<SegmentRow> loadSegments(CameraService& service,
                                                const std::string& cameraId,
                                                int64_t fromMs, int64_t toMs) {
        std::vector<SegmentRow> out;
        try {
            auto rows = service.getRecordingSegments(
                oatpp::String(cameraId.c_str()),
                oatpp::String(isoFromMs(fromMs).c_str()),
                oatpp::String(isoFromMs(toMs).c_str()));
            if (!rows) return out;
            for (const auto& row : *rows) {
                if (!row || !row->path || !row->startAt || !row->durationMs) continue;
                const std::string status = row->status ? row->status->c_str() : "complete";
                if (status != "complete") continue;
                const std::string container = row->container ? row->container->c_str() : "ts";
                if (container != "ts") continue;
                const int64_t startMs = parseEpochMs(row->startAt->c_str());
                if (startMs < 0) continue;
                SegmentRow item;
                item.id = row->id ? row->id->c_str() : "";
                item.path = row->path->c_str();
                item.codec = row->codec ? row->codec->c_str() : "h264";
                item.startMs = startMs;
                item.endMs = startMs + *row->durationMs;
                out.push_back(std::move(item));
            }
        } catch (const std::exception& exc) {
            OATPP_LOGE("PlaybackController", "loadSegments loi: %s", exc.what());
        } catch (...) {
            OATPP_LOGE("PlaybackController", "loadSegments loi khong ro");
        }
        return out;
    }

    // "2026-07-23 13:37:20.512+00" -> epoch ms. parseEpochSeconds bỏ phần thập
    // phân (nó chỉ cần so sánh giây), còn ở đây sai một phần giây là con trỏ
    // timeline lệch thấy được, nên phải cộng lại mili giây.
    static int64_t parseEpochMs(const std::string& value) {
        const long long seconds = playback::parseEpochSeconds(value);
        if (seconds < 0) return -1;
        int64_t millis = 0;
        const size_t dot = value.find('.');
        if (dot != std::string::npos) {
            std::string frac;
            for (size_t i = dot + 1; i < value.size() && std::isdigit(value[i]); ++i) {
                frac.push_back(value[i]);
            }
            frac.resize(3, '0');
            millis = std::strtoll(frac.c_str(), nullptr, 10);
        }
        return static_cast<int64_t>(seconds) * 1000 + millis;
    }

    static std::string isoFromMs(int64_t ms) {
        const std::time_t seconds = static_cast<std::time_t>(ms / 1000);
        std::tm tm{};
        gmtime_r(&seconds, &tm);
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer),
                      "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                      tm.tm_hour, tm.tm_min, tm.tm_sec,
                      static_cast<int>(((ms % 1000) + 1000) % 1000));
        return buffer;
    }

    static oatpp::Object<PlaybackStatusDto> toDto(const std::string& sessionId,
                                                  const playback::PlaybackStatus& in) {
        auto dto = PlaybackStatusDto::createShared();
        dto->sessionId = sessionId.c_str();
        dto->positionMs = in.positionMs;
        dto->rate = in.rate;
        dto->paused = in.paused;
        dto->ended = in.ended;
        dto->waiting = in.waiting;
        dto->seq = static_cast<v_int64>(in.seq);
        dto->seekSeq = static_cast<v_int64>(in.seekSeq);
        return dto;
    }

    // Giống WebRtcController: IP thật của trình duyệt để thay tên mDNS trong
    // ICE candidate của Chrome.
    static std::string firstIpv4(const oatpp::String& header) {
        if (!header) return "";
        std::string value = header->c_str();
        const size_t comma = value.find(',');
        if (comma != std::string::npos) value = value.substr(0, comma);
        const auto begin = value.find_first_not_of(" \t");
        if (begin == std::string::npos) return "";
        const auto end = value.find_last_not_of(" \t");
        value = value.substr(begin, end - begin + 1);
        const std::string mapped = "::ffff:";
        if (value.rfind(mapped, 0) == 0) value = value.substr(mapped.size());
        if (value.empty() ||
            value.find_first_not_of("0123456789.") != std::string::npos) {
            return "";
        }
        return value;
    }

    static std::shared_ptr<OutgoingResponse> withCors(
        const std::shared_ptr<OutgoingResponse>& response)
    {
        response->putHeader("Access-Control-Allow-Origin", "*");
        response->putHeader("Access-Control-Allow-Methods",
                            "GET, POST, DELETE, OPTIONS");
        response->putHeader("Access-Control-Allow-Headers", "Content-Type");
        response->putHeader("Access-Control-Expose-Headers", "Location");
        return response;
    }

    CameraService m_service;
    OATPP_COMPONENT(std::shared_ptr<playback::PlaybackService>, m_playback);
};

#include OATPP_CODEGEN_END(ApiController)

#endif  // test_gstreamer_PlaybackController_hpp
