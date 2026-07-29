#ifndef test_gstreamer_WebRtcController_hpp
#define test_gstreamer_WebRtcController_hpp

// WHEP (WebRTC-HTTP Egress Protocol) — cùng giao thức MediaMTX dùng, nên
// client WHEP có sẵn ngoài kia cũng cắm vào được.
//
//   POST   /cameras/{id}/whep              body = SDP offer  -> 201 + SDP answer
//   DELETE /cameras/{id}/whep/{sessionId}                    -> 204
//   OPTIONS  (CORS preflight cho hai cái trên)
//
// Content-Type là application/sdp chứ không phải JSON, nên các endpoint này
// nhận/trả chuỗi thô thay vì DTO.

#include "service/GStreamerService.hpp"
#include "service/WebRtcService.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

#include OATPP_CODEGEN_BEGIN(ApiController)

class WebRtcController : public oatpp::web::server::api::ApiController {
public:
    explicit WebRtcController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(whepOffer) {
        info->summary = "Start a WebRTC (WHEP) live view session";
        info->description =
            "Body is a raw SDP offer (application/sdp). Returns the SDP answer "
            "with ICE candidates already embedded — no trickle needed. The "
            "Location header carries the session URL to DELETE when done.";
        info->addResponse<String>(Status::CODE_201, "application/sdp");
        info->addResponse<String>(Status::CODE_400, "text/plain");
        info->addResponse<String>(Status::CODE_503, "text/plain");
    }
    ENDPOINT("POST", "/cameras/{id}/whep", whepOffer,
             PATH(oatpp::String, id),
             BODY_STRING(oatpp::String, offerSdp),
             REQUEST(std::shared_ptr<IncomingRequest>, request))
    {
        const std::string cameraId = id ? id->c_str() : "";
        const std::string offer = offerSdp ? offerSdp->c_str() : "";
        // IP thật của trình duyệt, do proxy Next.js gắn vào. Engine dùng nó để
        // thay cho tên mDNS ".local" trong ICE candidate của Chrome — máy này
        // không có mDNS resolver nên tên đó không tự phân giải được.
        const std::string clientHint =
            firstIpv4(request ? request->getHeader("X-Forwarded-For") : nullptr);
        if (offer.empty()) {
            return withCors(createResponse(Status::CODE_400,
                                           "Missing SDP offer in request body"));
        }

        // Kiểm tra camera TRƯỚC khi dựng phiên. Không kiểm thì id sai vẫn trả
        // 201 kèm answer hợp lệ — webrtcbin thương lượng được nhờ capsfilter
        // tĩnh, chẳng cần luồng thật — và trình duyệt treo ở "đang kết nối"
        // vĩnh viễn, không có lấy một thông báo lỗi.
        // getStatus() trả "offline" cho cả id không tồn tại lẫn camera đang
        // mất kết nối — không phân biệt được hai ca, mà cũng không cần: với
        // người dùng thì cả hai đều là "chưa có gì để xem".
        auto status = m_gstreamer->getStatus(id);
        const std::string state = status && status->state ? status->state->c_str() : "offline";
        if (state != "online") {
            const std::string detail =
                "Camera dang " + state + ", chua co luong de phat";
            return withCors(createResponse(Status::CODE_503, detail.c_str()));
        }

        // Codec quyết định đường ống: h264 passthrough, h265 passthrough nếu
        // trình duyệt nhận được H265, không thì transcode phần cứng sang H264.
        const std::string codec = status->codec ? status->codec->c_str() : "h264";
        // URL RTSP thật của camera: nguồn dùng chung kéo trực tiếp thay vì đi
        // vòng qua RTSP server nội bộ (:8554) như trước — bỏ được cả một chặng
        // depay/parse/jitterbuffer lặp lại trên mỗi phiên. Xem CameraRtpSource.
        const std::string cameraRtsp =
            status->inputRtsp ? status->inputRtsp->c_str() : "";
        if (cameraRtsp.empty()) {
            return withCors(createResponse(Status::CODE_503,
                                           "Camera chua co luong RTSP dau vao"));
        }

        auto result =
            m_webrtc->createSession(cameraId, offer, clientHint, codec, cameraRtsp);
        if (!result.ok()) {
            // 503 chứ không phải 500: hầu hết thất bại ở đây là camera chưa
            // online / luồng RTSP chưa sẵn sàng — tức tạm thời, client thử lại
            // được. Kèm nguyên văn lỗi để còn lần ra nguyên nhân.
            return withCors(createResponse(Status::CODE_503, result.error.c_str()));
        }

        auto response = createResponse(Status::CODE_201, result.answerSdp.c_str());
        response->putHeader("Content-Type", "application/sdp");
        // Location là cách WHEP báo cho client biết phải DELETE vào đâu.
        const std::string location =
            "/cameras/" + cameraId + "/whep/" + result.sessionId;
        response->putHeader("Location", location.c_str());
        return withCors(response);
    }

    ENDPOINT_INFO(whepViewers) {
        info->summary = "List active WebRTC viewers";
        info->description =
            "Số phiên đang xem và thông tin từng phiên (camera, IP trình duyệt, "
            "trực tiếp/xem lại, đã kết nối chưa, sống bao lâu). Lọc theo camera "
            "bằng query ?cameraId=.";
        info->addResponse<String>(Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/webrtc/viewers", whepViewers,
             QUERY(oatpp::String, cameraId, "cameraId", ""))
    {
        const std::string filter = cameraId ? cameraId->c_str() : "";
        auto sessions = m_webrtc->listSessions();

        size_t live = 0, playback = 0;
        std::ostringstream items;
        bool first = true;
        for (const auto& s : sessions) {
            if (!filter.empty() && s.cameraId != filter) continue;
            if (s.playback) ++playback; else ++live;
            if (!first) items << ',';
            first = false;
            items << "{"
                  << "\"sessionId\":\"" << jsonEscape(s.sessionId) << "\","
                  << "\"cameraId\":\""  << jsonEscape(s.cameraId)  << "\","
                  << "\"clientAddr\":\"" << jsonEscape(s.clientAddr) << "\","
                  << "\"codec\":\""     << jsonEscape(s.codec)     << "\","
                  << "\"mode\":\"" << (s.playback ? "playback" : "live") << "\","
                  << "\"connected\":" << (s.connected ? "true" : "false") << ","
                  << "\"ageMs\":" << s.ageMs << ","
                  << "\"ageSeconds\":" << (s.ageMs / 1000) << ","
                  << "\"rtpPackets\":" << s.rtpPackets
                  << "}";
        }

        std::ostringstream body;
        body << "{"
             << "\"total\":" << (live + playback) << ","
             << "\"live\":" << live << ","
             << "\"playback\":" << playback << ","
             << "\"sessions\":[" << items.str() << "]"
             << "}";

        auto response = createResponse(Status::CODE_200, body.str().c_str());
        response->putHeader("Content-Type", "application/json");
        return withCors(response);
    }

    ENDPOINT_INFO(whepStop) {
        info->summary = "Stop a WebRTC live view session";
        info->addResponse<String>(Status::CODE_204, "text/plain");
    }
    ENDPOINT("DELETE", "/cameras/{id}/whep/{sessionId}", whepStop,
             PATH(oatpp::String, id),
             PATH(oatpp::String, sessionId))
    {
        (void)id;  // sessionId đã đủ định danh; id giữ cho URL đọc được nghĩa
        m_webrtc->destroySession(sessionId ? sessionId->c_str() : "");
        // Luôn 204 kể cả khi không tìm thấy phiên: DELETE phải idempotent, và
        // watchdog có thể đã dọn trước khi trình duyệt kịp gọi.
        return withCors(createResponse(Status::CODE_204, ""));
    }

    // Trình duyệt gửi preflight vì POST kèm Content-Type: application/sdp
    // không thuộc nhóm "simple request".
    ENDPOINT("OPTIONS", "/cameras/{id}/whep", whepOptions, PATH(oatpp::String, id)) {
        (void)id;
        return withCors(createResponse(Status::CODE_204, ""));
    }

    ENDPOINT("OPTIONS", "/cameras/{id}/whep/{sessionId}", whepSessionOptions,
             PATH(oatpp::String, id), PATH(oatpp::String, sessionId))
    {
        (void)id;
        (void)sessionId;
        return withCors(createResponse(Status::CODE_204, ""));
    }

private:
    // Thoát ký tự đặc biệt cho chuỗi JSON. Các trường ở đây (id hex, uuid, IP)
    // hầu như luôn an toàn, nhưng escape cho chắc để một giá trị lạ không làm
    // hỏng cả body.
    static std::string jsonEscape(const std::string& in) {
        std::string out;
        out.reserve(in.size() + 2);
        for (char c : in) {
            switch (c) {
                case '"':  out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n";  break;
                case '\r': out += "\\r";  break;
                case '\t': out += "\\t";  break;
                default:
                    if (static_cast<unsigned char>(c) < 0x20) {
                        char buf[7];
                        std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                        out += buf;
                    } else {
                        out += c;
                    }
            }
        }
        return out;
    }

    // Lấy IPv4 đầu tiên trong chuỗi X-Forwarded-For ("ip1, ip2, ...").
    // Chỉ nhận IPv4 dạng chấm: candidate mDNS của Chrome là UDP/IPv4, nhét
    // IPv6 (hoặc rác) vào đó chỉ tạo thêm một candidate chết nữa. Dạng
    // "::ffff:192.168.1.5" (IPv4 bọc trong IPv6, hay gặp từ socket Node) được
    // bóc ra dùng. Không hợp lệ -> trả chuỗi rỗng, engine bỏ qua gợi ý.
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
        response->putHeader("Access-Control-Allow-Methods", "POST, DELETE, OPTIONS");
        response->putHeader("Access-Control-Allow-Headers", "Content-Type");
        // Không có Location trong danh sách này thì JS đọc header đó ra null và
        // không biết đường gọi DELETE — phiên sẽ chỉ chết theo watchdog.
        response->putHeader("Access-Control-Expose-Headers", "Location");
        return response;
    }

    OATPP_COMPONENT(std::shared_ptr<webrtc::WebRtcService>, m_webrtc);
    OATPP_COMPONENT(std::shared_ptr<GStreamerService>, m_gstreamer);
};

#include OATPP_CODEGEN_END(ApiController)

#endif
