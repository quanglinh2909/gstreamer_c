#ifndef test_gstreamer_WebRtcSession_hpp
#define test_gstreamer_WebRtcSession_hpp

// Một phiên xem live WebRTC của MỘT trình duyệt.
//
// Giao thức là WHEP (WebRTC-HTTP Egress Protocol) — đúng cái MediaMTX dùng:
// trình duyệt POST một SDP offer, server trả về SDP answer, xong. Không cần
// WebSocket signalling riêng.
//
// Đường đi của luồng:
//
//   camera ──RTSP──> RTSP server của chính engine ──RTSP──> phiên này ──> browser
//                     (rtsp://127.0.0.1:8554/cameras/<id>)
//
// Nối vào RTSP server nội bộ chứ không mở kết nối mới tới camera: factory của
// CameraStreamSession đặt shared=TRUE nên mọi client RTSP dùng CHUNG một kết
// nối tới camera vật lý. Mười người cùng xem vẫn chỉ một luồng ra khỏi camera
// — nhiều camera Dahua/Hikvision giới hạn số luồng đồng thời nên điều này là
// bắt buộc chứ không phải tối ưu.
//
// Toàn bộ đường ống là passthrough: depay → parse → pay lại. KHÔNG giải mã,
// KHÔNG mã hoá lại, nên thêm người xem gần như không tốn CPU/NPU.

#include "service/StreamTypes.hpp"

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/app/gstappsrc.h>
#include <gst/rtp/gstrtpbuffer.h>

#define GST_USE_UNSTABLE_API
#include <gst/webrtc/webrtc.h>
#undef GST_USE_UNSTABLE_API

#include <atomic>
#include <cstring>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "service/AppSrcBridge.hpp"
#include "service/FrameSource.hpp"

namespace webrtc {

// Đợi ICE gathering xong rồi mới trả answer (WHEP "non-trickle"). Đổi lại là
// chậm hơn trickle vài trăm ms, nhưng client chỉ cần một lần POST duy nhất —
// không phải hiện thực PATCH/trickle ở cả hai đầu. Trong LAN, candidate host
// có ngay nên thực tế gần như không đợi.
inline constexpr int kIceGatherTimeoutMs = 5000;

// Ngân sách hỏi STUN/TURN của libnice khi gom candidate — xem tuneIceAgent().
// Mặc định của libnice (500ms x 3 lần) làm mỗi lần POST /whep chậm 2,3 giây.
inline constexpr guint kStunInitialTimeoutMs = 200;
inline constexpr guint kStunMaxRetransmissions = 2;

// Thời gian tối đa cho phép một phiên hoàn tất bắt tay (ICE + DTLS). Quá hạn
// mà chưa connected thì coi như hỏng và dọn.
//
// CHỈ áp cho giai đoạn bắt tay. Phiên đã connected thì KHÔNG bao giờ hết hạn
// theo đồng hồ: WebRTC không phát tín hiệu định kỳ nào cả, một phiên đang xem
// bình thường sẽ nằm im ở trạng thái connected mãi mãi. Đo "im lặng" rồi coi
// là chết sẽ giết đúng những phiên đang chạy tốt.
//
// Trình duyệt biến mất không báo trước được phát hiện bằng chính cơ chế của
// WebRTC: consent freshness làm connection-state rơi xuống disconnected rồi
// failed trong khoảng 30s, lúc đó mới bắt đầu đếm giờ dọn.
inline constexpr int kHandshakeTimeoutMs = 30000;

// Phiên đã connected mà KHÔNG có gói RTP mới nào trong ngần này thì coi như
// nguồn đã chết -> dọn để trình duyệt nối lại phiên mới.
//
// Đây là lớp tự chữa quan trọng nhất: khi camera bị sửa setting (đổi codec,
// đổi độ phân giải) hoặc luồng RTSP nội bộ dựng lại, đường ống của phiên cũ
// vẫn "connected" hoàn hảo ở mức WebRTC — ICE thông, DTLS thông — chỉ là
// không còn dữ liệu nào chảy qua. Không có mốc này thì phiên đó bất tử
// (m_connected miễn nhiễm watchdog) và người xem ngồi nhìn hình đứng mãi.
//
// 10s đủ rộng để không giết oan camera GOP dài đang giữa hai keyframe.
inline constexpr int kMediaStallTimeoutMs = 10000;

// Phiên đã connected mà TRÌNH DUYỆT không gửi RTCP nào trong ngần này thì coi
// như người xem đã biến mất -> dọn.
//
// Vì sao cần thêm mốc này khi đã có connection-state: phát hiện "trình duyệt
// biến mất" của WebRTC dựa vào ICE consent freshness, mà consent freshness chỉ
// có từ libnice 0.1.19. Máy chạy Ubuntu 22.04 có libnice 0.1.18 (không có nó),
// nên ICE nằm "connected" VĨNH VIỄN sau khi tab đóng — connection-state không
// bao giờ rời connected, m_connected miễn nhiễm watchdog, còn m_lastMedia thì
// do CHÍNH đường ống của ta bơm vào nên luôn tươi. Kết quả: phiên bất tử, số
// người xem không bao giờ giảm. Trên máy có libnice 0.1.23 thì có giảm, nhưng
// phải đợi consent (~30s) rồi mới tới watchdog — tức là rất lâu.
//
// RTCP thì không phụ thuộc phiên bản gì: mọi trình duyệt đều gửi Receiver
// Report đều đặn (RFC 3550 ép tối thiểu 5 giây một lần), và ngừng ngay khi tab
// chết. 15s = 3 lần chu kỳ tối thiểu, đủ rộng để không giết oan lúc mạng nghẽn.
inline constexpr int kPeerSilenceTimeoutMs = 15000;

// Phiên XEM LẠI hết hạn sau ngần này nếu client không hỏi trạng thái lần nào.
// Client hỏi mỗi ~1s, nên 30s là rộng rãi; đây là cái duy nhất dọn được phiên
// khi tab bị đóng đột ngột (không kịp gửi DELETE).
inline constexpr int kHeartbeatTimeoutMs = 30000;

// Nhịp đẩy vị trí đang phát xuống trình duyệt qua kênh dữ liệu. 500ms cho con
// trỏ timeline chạy mượt mà vẫn không tốn gì: gói đi trên chính kết nối WebRTC
// đang mở, không phải một request HTTP mới.
inline constexpr int kStatusPushIntervalMs = 500;

struct SessionResult {
    std::string answerSdp;
    std::string error;  // rỗng = thành công
    bool ok() const { return error.empty() && !answerSdp.empty(); }
};

class WebRtcSession {
public:
    WebRtcSession(std::string sessionId,
                  std::string cameraId,
                  stream::GStreamerConfig config,
                  std::string cameraCodec,
                  std::shared_ptr<stream::FrameSource> source)
        : m_sessionId(std::move(sessionId)),
          m_cameraId(std::move(cameraId)),
          m_config(std::move(config)),
          m_cameraCodec(std::move(cameraCodec)),
          m_source(std::move(source)) {
        touch();
    }

    ~WebRtcSession() { stop(); }

    WebRtcSession(const WebRtcSession&) = delete;
    WebRtcSession& operator=(const WebRtcSession&) = delete;

    const std::string& sessionId() const { return m_sessionId; }
    const std::string& cameraId() const { return m_cameraId; }

    // Số ms kể từ lúc tạo phiên. Log kèm mốc này thì mới biết khâu nào chậm;
    // thứ tự dòng log giữa nhiều thread không đủ để kết luận.
    long elapsedMs() const {
        const auto age = std::chrono::steady_clock::now() - m_createdAt;
        return static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(age).count());
    }

    // Chạy trên thread xử lý HTTP: có block. Trả về SDP answer.
    //
    // clientAddressHint: IP của trình duyệt nhìn từ phía server HTTP (lấy qua
    // X-Forwarded-For). Dùng để thay cho tên mDNS ".local" trong ICE candidate
    // — xem giải thích ở rewriteMdnsCandidate(). Rỗng = không có gợi ý.
    SessionResult start(const std::string& offerSdp,
                        const std::string& clientAddressHint = "") {
        SessionResult result;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_clientAddr = clientAddressHint;
        }

        // Chốt vai trò DTLS của trình duyệt thành "active" TRƯỚC khi đưa offer
        // cho webrtcbin, để webrtcbin tự chọn "passive" cho mình.
        //
        // Vì sao phải passive: Chrome che IP nội bộ sau tên mDNS
        // "<uuid>.local" trong ICE candidate, mà libnice không phân giải được
        // tên đó. Là DTLS client thì engine phải CHỦ ĐỘNG gửi ClientHello tới
        // một địa chỉ nó không biết -> gửi vào hư không, bắt tay treo vĩnh
        // viễn dù ICE vẫn báo completed (ICE qua được nhờ candidate
        // peer-reflexive học từ gói tin trình duyệt gửi đến).
        //
        // Passive thì trình duyệt gửi ClientHello trước, engine chỉ việc trả
        // lời vào đúng địa chỉ gói tin vừa đến. MediaMTX cũng làm vậy.
        //
        // Sửa OFFER chứ không sửa answer: webrtcbin suy ra vai trò của mình từ
        // vai trò của phía bên kia. Sửa thẳng "a=setup:" trong answer thì chỉ
        // đổi được chữ trong SDP, còn bên trong webrtcbin vẫn chạy role=client
        // — đã thử và log dtlsconnection cho thấy đúng như vậy.
        //
        // "actpass" nghĩa là trình duyệt nhận cả hai vai, nên chốt nó thành
        // "active" là cách hiểu hợp lệ, không phải lách luật.
        const std::string patchedOffer = forceRemoteDtlsActive(offerSdp);

        GstSDPMessage* offerMsg = nullptr;
        if (gst_sdp_message_new_from_text(patchedOffer.c_str(), &offerMsg) != GST_SDP_OK ||
            !offerMsg) {
            result.error = "Malformed SDP offer";
            if (offerMsg) gst_sdp_message_free(offerMsg);
            return result;
        }

        // Là bên TRẢ LỜI, ta phải phát bằng đúng số payload bên chào đã đặt.
        // Chrome thường để VP8 ở 96 còn H264 ở 103/109/127..., nên ghim cứng
        // một số bất kỳ làm caps của pipeline không giao được với offer:
        // webrtcbin lẳng lặng bỏ rơi luồng video của ta, tạo transceiver
        // recvonly trống cho m-line, và transportreceivebin giữ nguyên probe
        // chặn dữ liệu vào — gói DTLS ClientHello của trình duyệt bị nhốt
        // trong queue, phiên treo ở "connecting" vĩnh viễn. Triệu chứng cách
        // xa nguyên nhân cả một tầng giao thức.
        //
        // Chọn đường ống theo codec camera + khả năng của trình duyệt:
        //   camera h264                       -> passthrough H264
        //   camera h265 + offer có H265      -> passthrough H265 (Safari,
        //                                       Chrome mới có HW decode)
        //   camera h265 + offer chỉ có H264  -> transcode phần cứng
        //                                       mppvideodec ! mpph264enc
        bool sourceH265 = (m_cameraCodec == "h265");
        bool transcode = false;
        int payloadType = -1;
        if (sourceH265) {
            payloadType = pickPayloadType(offerMsg, "H265/", false);
            // Trình duyệt chào H265 ở số payload NGOÀI dải payloader tự phát
            // được (Chrome/Windows: 49) thì phải đi đường viết-lại-PT. Đường đó
            // tiết kiệm rất nhiều CPU (bỏ hẳn transcode: đo được 6 camera H265
            // từ +168% xuống ~0%) NHƯNG chưa xác nhận được là Chrome dựng được
            // hình, nên MẶC ĐỊNH TẮT — bật bằng AI_WEBRTC_H265_PASSTHROUGH=1.
            // Tắt thì coi như trình duyệt không chào H265 -> rơi xuống transcode
            // dùng chung (đường đã chạy ổn định lâu nay).
            if (payloadType >= 0 && !payloadTypeUsable(payloadType) &&
                !h265PtRewriteEnabled()) {
                g_print("[webrtc] session %s: H265 o payload %d can viet lai PT "
                        "nhung dang TAT (AI_WEBRTC_H265_PASSTHROUGH=1 de bat) "
                        "-> dung transcode\n",
                        m_sessionId.c_str(), payloadType);
                payloadType = -1;
            }
            if (payloadType < 0) {
                payloadType = pickPayloadType(offerMsg, "H264/", true);
                // Trình duyệt không nhận H265. Ưu tiên nguồn transcode DÙNG CHUNG
                // (transcode một lần cho mọi người xem) thay vì tự dựng cụm
                // mppvideodec ! mpph264enc riêng cho phiên này. Chỉ tự transcode
                // nếu nguồn chung không dựng được (fallback an toàn).
                std::shared_ptr<stream::FrameSource> shared =
                    m_transcodedProvider ? m_transcodedProvider() : nullptr;
                if (shared && shared->alive()) {
                    m_source = std::move(shared);  // đổi sang nguồn H264 chung
                    sourceH265 = false;            // -> buildLaunch passthrough H264
                    g_print("[webrtc] session %s: camera H265 -> dung nguon "
                            "transcode H264 DUNG CHUNG\n", m_sessionId.c_str());
                } else {
                    transcode = true;              // fallback: tự transcode
                }
            }
        } else {
            payloadType = pickPayloadType(offerMsg, "H264/", true);
        }
        if (payloadType < 0) {
            // Kèm nhắc về dải payload: nếu trình duyệt CÓ chào codec nhưng ở số
            // ngoài [96,127] thì pickPayloadType đã bỏ qua và in lý do ở trên —
            // không có dòng này thì thông báo "không chào codec" gây hiểu nhầm.
            result.error = sourceH265
                ? "Offer has no usable H265/H264 video payload (dynamic PT 96-127 required)"
                : "Offer has no usable H264 video payload (dynamic PT 96-127 required)";
            gst_sdp_message_free(offerMsg);
            return result;
        }

        // Chốt SSRC ngay tại đây và ghim vào CẢ rtph264pay LẪN capsfilter.
        // Không ghim thì rtph264pay tự bốc một SSRC ngẫu nhiên còn webrtcbin
        // quảng bá một SSRC ngẫu nhiên KHÁC trong "a=ssrc:" của answer —
        // Chrome vẫn giải mã luồng "vô danh" đó (stats vẫn nhảy!) nhưng track
        // gắn vào <video> thì chờ đúng SSRC đã khai và đói vĩnh viễn: màn đen
        // trong khi mọi trạng thái đều "connected".
        const guint32 ssrc = sessionSsrc();

        const std::string launch =
            buildLaunch(payloadType, ssrc, sourceH265, transcode);
        if (transcode) {
            g_print("[webrtc] session %s: camera H265, trinh duyet khong nhan "
                    "H265 -> transcode phan cung sang H264\n",
                    m_sessionId.c_str());
        }
        GError* err = nullptr;
        m_pipeline = gst_parse_launch(launch.c_str(), &err);
        // gst_parse_launch báo lỗi phục hồi được (thiếu element, không link
        // được) bằng cách trả về pipeline THIẾU phần tử kèm err — pipeline đó
        // không bao giờ chạy và cũng không bao giờ báo lỗi lên bus, nên phải
        // coi mọi err là chí tử ngay tại đây.
        if (!m_pipeline || err) {
            result.error = err && err->message ? err->message
                                               : "Failed to build WebRTC pipeline";
            if (err) g_error_free(err);
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }

        // Bus watch: thiếu nó thì mọi lỗi bên trong đường ống (thương lượng
        // caps hỏng, dtls lỗi, rtspsrc rớt) chỉ nằm im trong pipeline và triệu
        // chứng duy nhất nhìn thấy được là "không có hình" — không manh mối.
        if (GstBus* bus = gst_element_get_bus(m_pipeline)) {
            // Giữ lại id để gỡ trong stop(): watch gắn vào default main context
            // và giữ tham chiếu tới `this`, không gỡ là dùng con trỏ đã chết.
            m_busWatchId = gst_bus_add_watch(bus, &WebRtcSession::onBusMessage, this);
            gst_object_unref(bus);
        }

        m_webrtc = gst_bin_get_by_name(GST_BIN(m_pipeline), "webrtc");
        if (!m_webrtc) {
            result.error = "WebRTC pipeline missing webrtcbin";
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }

        // Trình duyệt chào codec ở số payload ngoài dải payloader phát được:
        // caps đã do capssetter sửa, còn byte PT trong header từng gói RTP thì
        // phải tự viết ở đây. Thiếu bước này là answer khai một số, gói mang
        // một số khác -> trình duyệt bỏ hết gói (màn đen mà mọi state
        // "connected"), đúng bẫy đã ghi ở mục SSRC.
        if (!payloadTypeUsable(payloadType) &&
            !attachPayloadTypeRewrite(payloadType)) {
            result.error = "Khong gan duoc bo viet lai payload-type";
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }

        // Phải gọi TRƯỚC khi gom ICE (gom bắt đầu ở set-local-description).
        tuneIceAgent();

        // Cầu nối nguồn dùng chung -> appsrc của phiên. Nguồn phải sẵn sàng và
        // còn sống; không thì trả lỗi để controller báo 503 và client thử lại.
        m_appsrc = gst_bin_get_by_name(GST_BIN(m_pipeline), "src");
        if (!m_appsrc) {
            result.error = "WebRTC pipeline missing appsrc";
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }
        if (!m_source || !m_source->alive()) {
            result.error = "Nguon RTP cua camera chua san sang";
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }

        // Bật NACK trên transceiver TRƯỚC khi thương lượng, để answer quảng bá
        // "a=rtcp-fb nack": trình duyệt sẽ xin phát lại đúng gói bị rớt thay vì
        // vứt cả GOP. Quan trọng vì đây là passthrough — ta KHÔNG thể tạo
        // keyframe theo yêu cầu (PLI vô tác dụng), mà nhiều camera bật smart
        // encoding để IDR cách nhau hàng chục giây: mất một gói không NACK
        // nghĩa là hình đứng cho tới IDR kế tiếp.
        {
            GstWebRTCRTPTransceiver* transceiver = nullptr;
            g_signal_emit_by_name(m_webrtc, "get-transceiver", 0, &transceiver);
            if (transceiver) {
                g_object_set(transceiver, "do-nack", TRUE, nullptr);
                gst_object_unref(transceiver);
            }
        }

        // Đếm số buffer RTP từ camera thực sự đi vào webrtcbin. "Kết nối xong
        // mà không có hình" có hai họ nguyên nhân trái ngược nhau — nguồn không
        // nhả dữ liệu, hoặc dữ liệu vào mà không ra được tới client — và không
        // đếm ở biên này thì không tách được hai họ đó.
        if (GstPad* sinkPad = gst_element_get_static_pad(m_webrtc, "sink_0")) {
            gst_pad_add_probe(
                sinkPad,
                static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                             GST_PAD_PROBE_TYPE_BUFFER_LIST),
                [](GstPad*, GstPadProbeInfo* info, gpointer userData) -> GstPadProbeReturn {
                    auto* self = static_cast<WebRtcSession*>(userData);
                    self->m_rtpInCount.fetch_add(1);
                    // Mốc "còn sống" của phiên. Chạy trên thread streaming nên
                    // giữ phần trong khoá thật ngắn.
                    {
                        std::lock_guard<std::mutex> lock(self->m_mutex);
                        self->m_lastMedia = std::chrono::steady_clock::now();
                    }
                    return GST_PAD_PROBE_OK;
                },
                this, nullptr);
            gst_object_unref(sinkPad);
        }
        // Log tiến độ mỗi 5s trên main loop mặc định (GStreamerService quay nó).
        m_statsTimerId = g_timeout_add_seconds(5, &WebRtcSession::onStatsTick, this);

        // Kênh dữ liệu do TRÌNH DUYỆT tạo (nó gọi createDataChannel trước
        // createOffer nên m-line "application" đã nằm sẵn trong offer). Engine
        // dùng kênh này đẩy vị trí đang phát xuống, thay cho việc client hỏi
        // HTTP mỗi giây.
        g_signal_connect(m_webrtc, "on-data-channel",
                         G_CALLBACK(&WebRtcSession::onDataChannel), this);

        g_signal_connect(m_webrtc, "notify::ice-gathering-state",
                         G_CALLBACK(&WebRtcSession::onIceGatheringState), this);
        g_signal_connect(m_webrtc, "notify::connection-state",
                         G_CALLBACK(&WebRtcSession::onConnectionState), this);
        g_signal_connect(m_webrtc, "notify::ice-connection-state",
                         G_CALLBACK(&WebRtcSession::onIceConnectionState), this);

        // Nhịp sống của TRÌNH DUYỆT (xem kPeerSilenceTimeoutMs). webrtcbin đặt
        // tên phần tử rtpbin bên trong đúng là "rtpbin"; "on-ssrc-active" nổ mỗi
        // khi nhận được RTCP từ một nguồn — với phiên chỉ-gửi như ở đây thì đó
        // chính là Receiver Report của trình duyệt.
        if (GstElement* rtpbin = gst_bin_get_by_name(GST_BIN(m_webrtc), "rtpbin")) {
            g_signal_connect(rtpbin, "on-ssrc-active",
                             G_CALLBACK(&WebRtcSession::onSsrcActive), this);
            gst_object_unref(rtpbin);
        } else {
            g_print("[webrtc] session %s: KHONG tim thay rtpbin trong webrtcbin —"
                    " khong do duoc nhip RTCP cua trinh duyet\n",
                    m_sessionId.c_str());
        }

        // PLAYING trước khi thương lượng: webrtcbin cần đường ống chạy thì mới
        // gom được ICE candidate. Caps của m-line không phụ thuộc dữ liệu thật
        // nhờ capsfilter cố định ngay trước webrtcbin (xem buildLaunch), nên
        // answer vẫn đúng kể cả khi camera chưa kịp gửi frame nào.
        if (gst_element_set_state(m_pipeline, GST_STATE_PLAYING) ==
            GST_STATE_CHANGE_FAILURE) {
            result.error = "Could not start WebRTC pipeline";
            gst_sdp_message_free(offerMsg);
            stop();
            return result;
        }

        // Đăng ký nhận buffer SAU khi appsrc đã PLAYING: nguồn bắt đầu bơm
        // access unit từ keyframe kế tiếp vào phiên này.
        registerSink();

        auto* offer = gst_webrtc_session_description_new(
            GST_WEBRTC_SDP_TYPE_OFFER, offerMsg);  // nhận sở hữu offerMsg
        GstPromise* promise = gst_promise_new();
        g_signal_emit_by_name(m_webrtc, "set-remote-description", offer, promise);
        gst_promise_wait(promise);
        gst_promise_unref(promise);

        // BẮT BUỘC: webrtcbin KHÔNG tự lấy các dòng "a=candidate:" trong SDP
        // offer ra dùng — nó chỉ nhận candidate qua tín hiệu add-ice-candidate.
        // Client của ta không trickle (gửi candidate kèm luôn trong offer), nên
        // thiếu bước này thì webrtcbin không biết một địa chỉ nào của trình
        // duyệt: SDP trao đổi xong xuôi, ICE treo mãi ở checking rồi phiên bị
        // watchdog dọn — đúng triệu chứng "xoay một hồi rồi báo mất kết nối".
        const int added =
            addRemoteCandidatesFromSdp(m_webrtc, offer->sdp, clientAddressHint);
        g_print("[webrtc] +%5ldms session %s: nap %d ICE candidate tu offer\n",
                elapsedMs(), m_sessionId.c_str(), added);

        gst_webrtc_session_description_free(offer);

        // create-answer chạy bất đồng bộ; đợi promise rồi lấy answer ra.
        promise = gst_promise_new();
        g_signal_emit_by_name(m_webrtc, "create-answer", nullptr, promise);
        gst_promise_wait(promise);
        const GstStructure* reply = gst_promise_get_reply(promise);
        GstWebRTCSessionDescription* answer = nullptr;
        if (reply) {
            gst_structure_get(reply, "answer",
                              GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &answer, nullptr);
        }
        gst_promise_unref(promise);

        if (!answer) {
            result.error = "webrtcbin produced no SDP answer";
            stop();
            return result;
        }

        // Answer đã mang a=setup:passive nhờ việc chốt offer thành "active" ở
        // đầu hàm — xem forceRemoteDtlsActive().
        promise = gst_promise_new();
        g_signal_emit_by_name(m_webrtc, "set-local-description", answer, promise);
        gst_promise_wait(promise);
        gst_promise_unref(promise);
        gst_webrtc_session_description_free(answer);

        if (!waitForIceGathering()) {
            result.error = "Timed out gathering ICE candidates";
            stop();
            return result;
        }

        // Đọc lại local-description SAU khi gom xong ICE: lúc này SDP đã kèm
        // đầy đủ candidate, client không cần trickle.
        GstWebRTCSessionDescription* local = nullptr;
        g_object_get(m_webrtc, "local-description", &local, nullptr);
        if (!local || !local->sdp) {
            result.error = "WebRTC local description unavailable";
            if (local) gst_webrtc_session_description_free(local);
            stop();
            return result;
        }

        gchar* text = gst_sdp_message_as_text(local->sdp);
        if (text) {
            result.answerSdp = text;
            // In candidate của CHÍNH engine trong answer (đối xứng với log
            // candidate của trình duyệt). Đây là thứ cần để chẩn đoán lỗi "xem
            // từ mạng ngoài không được": nếu KHÔNG có dòng "typ relay" ở đây
            // thì TURN phía board hỏng; nếu có relay mà vẫn fail thì lỗi nằm ở
            // phía trình duyệt không lấy được relay (mạng chặn UDP tới TURN).
            for (const char* p = text; (p = std::strstr(p, "a=candidate")); ++p) {
                const char* nl = std::strchr(p, '\r');
                if (!nl) nl = std::strchr(p, '\n');
                g_print("[webrtc] LOCAL %.*s\n",
                        (int)(nl ? (nl - p) : (int)std::strlen(p)), p);
            }
            g_free(text);
        }
        gst_webrtc_session_description_free(local);

        if (result.answerSdp.empty()) {
            result.error = "Could not serialise SDP answer";
            stop();
            return result;
        }

        // Answer PHẢI khai "a=ssrc:" khớp với SSRC trong gói RTP. Việc đó do
        // capsfilter tĩnh trong buildLaunch lo (xem chú thích ở đó). Chỉ CẢNH
        // BÁO nếu vẫn thiếu — KHÔNG tự chèn số vào: chèn nhầm số còn tệ hơn
        // thiếu (đo tận nơi: khai một SSRC mà gói mang SSRC khác thì bên nhận
        // lọc sạch, 0 gói tới bộ giải mã).
        if (result.answerSdp.find("a=ssrc:") == std::string::npos) {
            g_print("[webrtc] CANH BAO: answer khong co a=ssrc — trinh duyet se "
                    "khong noi duoc track vao <video> (man den)\n");
        }

        touch();
        return result;
    }

    void stop() {
        // Cắt cầu nối nguồn TRƯỚC khi tháo appsrc (bridge lo đúng thứ tự an
        // toàn: hạ cờ, đợi push đang chạy xong, removeSink).
        m_bridge.detach();

        if (m_statsTimerId != 0) {
            g_source_remove(m_statsTimerId);
            m_statsTimerId = 0;
        }
        if (m_statusTimerId != 0) {
            g_source_remove(m_statusTimerId);
            m_statusTimerId = 0;
        }
        if (m_dataChannel) {
            g_object_unref(m_dataChannel);
            m_dataChannel = nullptr;
        }
        if (m_busWatchId != 0) {
            g_source_remove(m_busWatchId);
            m_busWatchId = 0;
        }
        if (m_pipeline) {
            gst_element_set_state(m_pipeline, GST_STATE_NULL);
        }
        // RÒ RỈ ĐÃ BIẾT, ĐỪNG SỬA LẠI THEO HƯỚNG DISPOSE (đã thử 29/07/2026):
        // mỗi phiên đóng lại để rớt một luồng "webrtc:ice" + ~0,5MB, không bao
        // giờ được dọn. Gốc là webrtcbin GStreamer 1.28: GstWebRTCICE giữ một
        // GThread + GMainContext + GMainLoop, chỉ dừng khi đối tượng được
        // finalize, mà một ref treo (idle source giữ ref tới webrtcbin — lỗi
        // upstream) khiến nó không bao giờ finalize. KHÔNG phải lỗi vòng đời
        // của stop() này: đã tái hiện bằng script tối giản chỉ dùng webrtcbin
        // (set-remote-description + create-answer) — 4 phiên rớt đúng 4 luồng.
        //
        // ĐÃ THỬ `g_object_run_dispose` lên ice-agent (và cả NiceAgent bên
        // dưới) ngay tại đây: script tối giản thì sạch (11 luồng -> 3), nhưng
        // TRONG ENGINE KHÔNG ăn thua (8 phiên vẫn rớt 8 luồng dù log xác nhận
        // dispose đã chạy đủ 8 lần), VÀ khi kết hợp với tuneIceAgent() thì
        // script bắn "gst_webrtc_ice_add_stream: assertion GST_IS_WEBRTC_ICE
        // failed" ở phiên kế tiếp = dùng nhầm đối tượng đã dispose. Rủi ro mà
        // không được gì, nên bỏ.
        //
        // Ảnh hưởng thực tế: 0% CPU (luồng ngủ), chỉ phình bộ nhớ chậm. Cách
        // sống chung: theo dõi RSS, khởi động lại engine lúc rảnh nếu cần, và
        // nâng GStreamer khi upstream vá.
        if (m_appsrc) {
            gst_object_unref(m_appsrc);
            m_appsrc = nullptr;
        }
        if (m_webrtc) {
            g_signal_handlers_disconnect_by_data(m_webrtc, this);
            gst_object_unref(m_webrtc);
            m_webrtc = nullptr;
        }
        if (m_pipeline) {
            gst_object_unref(m_pipeline);
            m_pipeline = nullptr;
        }
        // Buông nguồn dùng chung: phiên cuối buông -> nguồn tự huỷ -> đóng camera.
        m_source.reset();
    }

    // Đấu nguồn dùng chung vào appsrc của phiên qua bridge (xem AppSrcBridge).
    void registerSink() { m_bridge.attach(m_source, m_appsrc); }

    // Phiên XEM LẠI: tạm dừng là im lặng HỢP LỆ, nên không đo được sức khoẻ
    // bằng dòng dữ liệu như phiên live. Thay bằng chính trạng thái WebRTC:
    // còn `connected` là còn người xem. Trình duyệt biến mất không báo trước
    // vẫn bị bắt, chỉ chậm hơn — consent freshness của WebRTC kéo trạng thái
    // xuống disconnected rồi failed trong khoảng 30s, lúc đó phiên hết miễn
    // nhiễm và bị dọn như thường.
    void useHeartbeatExpiry() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_heartbeatMode = true;
    }

    // JSON trạng thái để đẩy xuống trình duyệt qua KÊNH DỮ LIỆU (thay cho việc
    // client hỏi HTTP mỗi giây). Trả chuỗi rỗng = bỏ qua nhịp này.
    void setStatusProvider(std::function<std::string()> provider) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_statusProvider = std::move(provider);
    }

    // Client vừa gọi điều khiển/hỏi trạng thái — phiên còn người dùng.
    void heartbeat() { touch(); }

    // Nhà cung cấp nguồn H264 DÙNG CHUNG cho camera H265. Khi start() phát hiện
    // trình duyệt không nhận H265, nó gọi cái này để lấy nguồn transcode dùng
    // chung (một lần cho mọi người xem) thay vì tự dựng cụm transcode riêng.
    // Không đặt (hoặc trả nullptr) thì phiên tự transcode như cũ. Chỉ đặt cho
    // phiên XEM TRỰC TIẾP; xem lại không dùng. Đặt TRƯỚC start().
    void setTranscodedProvider(std::function<std::shared_ptr<stream::FrameSource>()> provider) {
        m_transcodedProvider = std::move(provider);
    }

    bool isExpired() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_failed) return true;
        const auto now = std::chrono::steady_clock::now();
        // Trình duyệt đã im lặng ở mức RTCP = người xem không còn, dù ICE/DTLS
        // vẫn báo "connected". Áp cho CẢ xem lại lẫn xem trực tiếp, và chỉ áp
        // sau khi đã từng nhận được RTCP — client lạ không gửi RTCP thì giữ
        // nguyên hành vi cũ thay vì bị dọn oan.
        if (m_connected && m_rtcpSeen) {
            const auto quiet = now - m_lastRtcp;
            if (std::chrono::duration_cast<std::chrono::milliseconds>(quiet).count() >
                kPeerSilenceTimeoutMs) {
                return true;
            }
        }
        if (m_heartbeatMode) {
            // Đang kết nối được với trình duyệt = còn người xem, kể cả khi
            // đang tạm dừng và không có một byte media nào chảy.
            if (m_connected) return false;
            const auto age = now - m_lastSeen;
            return std::chrono::duration_cast<std::chrono::milliseconds>(age).count() >
                   kHandshakeTimeoutMs;
        }
        if (m_connected) {
            // Đang xem: chỉ chết khi DỮ LIỆU ngừng chảy, không phải khi trạng
            // thái WebRTC im lặng (trạng thái luôn im khi mọi thứ tốt đẹp).
            const auto since = now - m_lastMedia;
            return std::chrono::duration_cast<std::chrono::milliseconds>(since).count() >
                   kMediaStallTimeoutMs;
        }
        const auto age = now - m_lastSeen;
        return std::chrono::duration_cast<std::chrono::milliseconds>(age).count() >
               kHandshakeTimeoutMs;
    }

    // Ảnh chụp thông tin phiên để hiển thị "ai đang xem". Read-only, khoá ngắn.
    struct Info {
        std::string sessionId;
        std::string cameraId;
        std::string clientAddr;   // IP trình duyệt (có thể rỗng)
        std::string codec;        // codec camera nguồn
        bool        playback;     // true = xem lại, false = xem trực tiếp
        bool        connected;    // đã bắt tay WebRTC xong (media đang chảy)
        long        ageMs;        // sống bao lâu rồi
        long        rtpPackets;   // số gói RTP đã đẩy tới trình duyệt
    };

    Info snapshot() const {
        std::lock_guard<std::mutex> lock(m_mutex);
        Info info;
        info.sessionId  = m_sessionId;
        info.cameraId   = m_cameraId;
        info.clientAddr = m_clientAddr;
        info.codec      = m_cameraCodec;
        info.playback   = m_heartbeatMode;
        info.connected  = m_connected;
        info.ageMs      = static_cast<long>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - m_createdAt).count());
        info.rtpPackets = m_rtpInCount.load();
        return info;
    }

private:
    // rtph264pay/rtph265pay CHỈ TỰ phát được payload trong DẢI ĐỘNG [96,127] —
    // đó là caps của SRC pad template, không phải quy ước cho vui:
    //
    //   SRC template: application/x-rtp, payload: [ 96, 127 ], ...
    //
    // Chrome đời mới đã CẠN số payload trong dải đó nên nó phát thêm codec ở
    // khối 35..63 — trên Windows (nơi Chrome/Edge có giải mã HEVC phần cứng)
    // offer chào "a=rtpmap:49 H265/90000". Ghim thẳng số đó vào pipeline thì
    // hỏng ngay lúc dựng:
    //
    //   could not link rtph265pay142 to queue200, rtph265pay142 can't handle
    //   caps application/x-rtp, ..., encoding-name=(string)H265, payload=(int)49
    //
    // Và KHÔNG có đường vòng bằng cách chỉ đặt thuộc tính pt: đo tận nơi, đặt
    // `rtph265pay pt=49` bị lặng lẽ bỏ qua, phần tử vẫn phát payload=96 — tức
    // gói đi ra mang số khác hẳn số trong answer, trình duyệt vứt sạch (kết nối
    // "connected" mà màn hình đen).
    //
    // Cách xử lý: payloader vẫn chạy ở một số HỢP LỆ, rồi buildLaunch cắm
    // `capssetter` sửa caps và attachPayloadTypeRewrite() sửa byte PT trong
    // TỪNG gói RTP về đúng số trình duyệt chào (xem hai hàm đó). Nhờ vậy camera
    // H265 đi THẲNG tới trình duyệt, khỏi transcode — đo trên board: 6 camera
    // H265 transcode ngốn +168% CPU, passthrough gần như bằng 0.
    static bool payloadTypeUsable(int pt) { return pt >= 96 && pt <= 127; }

    // Số payload payloader chạy thật khi số trình duyệt chào nằm ngoài dải.
    static constexpr int kPayloaderInternalPt = 96;

    // Công tắc cho đường viết-lại-PT (H265 thẳng tới Chrome/Windows). MẶC ĐỊNH
    // BẬT — đã xác nhận hiển thị được trên Chrome/Windows (29/07/2026) và bỏ
    // hẳn transcode: 6 camera H265 từ +168% CPU xuống ~0%. Đặt
    // AI_WEBRTC_H265_PASSTHROUGH=0 để tắt khẩn cấp (quay về transcode) nếu gặp
    // trình duyệt lạ. Đọc một lần.
    static bool h265PtRewriteEnabled() {
        static const bool on = [] {
            const char* v = std::getenv("AI_WEBRTC_H265_PASSTHROUGH");
            return !(v && v[0] == '0');
        }();
        return on;
    }

    // Chọn payload type của codec `codecPrefix` ("H264/" / "H265/") từ m-line
    // video đầu tiên của offer. Với H264, ưu tiên bản packetization-mode=1
    // (gói FU-A, mọi trình duyệt hỗ trợ và là chế độ rtph264pay dùng); H265
    // không có tham số tương đương nên lấy mục đầu tiên. Số ngoài dải [96,127]
    // VẪN trả về (đường viết-lại-PT lo được), chỉ ưu tiên số trong dải trước vì
    // đường đó thẳng hơn. Trả -1 nếu offer không chào codec đó.
    static int pickPayloadType(const GstSDPMessage* sdp,
                               const char* codecPrefix,
                               bool preferPacketizationMode1) {
        if (!sdp || gst_sdp_message_medias_len(sdp) == 0) return -1;
        const GstSDPMedia* media = gst_sdp_message_get_media(sdp, 0);
        if (!media) return -1;

        const size_t prefixLen = strlen(codecPrefix);
        int fallback = -1;
        int outOfRange = -1;  // dùng khi không có số nào trong dải
        const guint attrs = gst_sdp_media_attributes_len(media);
        for (guint i = 0; i < attrs; ++i) {
            const GstSDPAttribute* attr = gst_sdp_media_get_attribute(media, i);
            if (!attr || g_strcmp0(attr->key, "rtpmap") != 0 || !attr->value) continue;
            int pt = -1;
            char codec[64] = {0};
            // "109 H264/90000"
            if (sscanf(attr->value, "%d %63s", &pt, codec) != 2) continue;
            if (g_ascii_strncasecmp(codec, codecPrefix, prefixLen) != 0) continue;
            if (!payloadTypeUsable(pt)) {
                if (outOfRange < 0) outOfRange = pt;
                continue;
            }
            if (fallback < 0) fallback = pt;
            if (!preferPacketizationMode1) return pt;

            // Tìm fmtp tương ứng xem có packetization-mode=1 không.
            const std::string prefix = std::to_string(pt) + " ";
            for (guint j = 0; j < attrs; ++j) {
                const GstSDPAttribute* fmtp = gst_sdp_media_get_attribute(media, j);
                if (!fmtp || g_strcmp0(fmtp->key, "fmtp") != 0 || !fmtp->value) continue;
                const std::string value = fmtp->value;
                if (value.rfind(prefix, 0) == 0 &&
                    value.find("packetization-mode=1") != std::string::npos) {
                    return pt;
                }
            }
        }
        if (fallback < 0 && outOfRange >= 0) {
            // Không có số nào trong dải: vẫn nhận số ngoài dải, buildLaunch sẽ
            // chạy payloader ở 96 rồi viết lại PT trong caps + từng gói RTP.
            g_print("[webrtc] offer chao %s o payload %d (ngoai dai [96,127]) "
                    "-> chay payloader o %d roi viet lai PT ve %d\n",
                    codecPrefix, outOfRange, kPayloaderInternalPt, outOfRange);
            return outOfRange;
        }
        return fallback;
    }

    // Rút ngắn thời gian gom ICE của libnice.
    //
    // Đo trên board: gom xong mất 2,30s và HTTP POST /whep bị chặn đúng ngần
    // ấy (client không trickle, phải đợi answer đủ candidate). Bỏ STUN+TURN đi
    // thì chỉ còn 0,02s -> toàn bộ thời gian nằm ở việc hỏi STUN/TURN.
    //
    // Không phải vì mạng chậm (RTT tới stun.l.google.com là 33ms) mà vì
    // libnice hỏi trên MỌI địa chỉ cục bộ — gồm cả bridge docker 172.18.0.1 và
    // hai địa chỉ link-local fe80:: vốn KHÔNG BAO GIỜ ra được Internet. Trên
    // các địa chỉ đó nó chờ hết ngân sách phát lại mặc định (500ms, 3 lần, tăng
    // gấp đôi mỗi lần) rồi mới chịu kết thúc. Ngân sách đó mới là 2,3 giây.
    //
    // 200ms x 2 lần vẫn dư gấp 6 lần RTT thật, mà trần chờ chỉ còn 600ms:
    // đo lại còn 0,57s và answer vẫn đủ srflx + relay. Đặt qua tên thuộc tính
    // của GObject nên không cần include header libnice; phiên bản nào không có
    // thuộc tính này thì bỏ qua, chạy như cũ.
    void tuneIceAgent() const {
        GObject* ice = nullptr;
        g_object_get(m_webrtc, "ice-agent", &ice, nullptr);
        if (!ice) return;

        GObject* agent = nullptr;
        if (g_object_class_find_property(G_OBJECT_GET_CLASS(ice), "agent")) {
            g_object_get(ice, "agent", &agent, nullptr);
        }
        if (agent) {
            GObjectClass* klass = G_OBJECT_GET_CLASS(agent);
            if (g_object_class_find_property(klass, "stun-initial-timeout")) {
                g_object_set(agent, "stun-initial-timeout", kStunInitialTimeoutMs, nullptr);
            }
            if (g_object_class_find_property(klass, "stun-max-retransmissions")) {
                g_object_set(agent, "stun-max-retransmissions", kStunMaxRetransmissions,
                             nullptr);
            }
            g_object_unref(agent);
        }
        g_object_unref(ice);
    }

    // Viết lại byte payload-type trong MỘT gói RTP. Trả về TRUE để
    // gst_buffer_list_foreach giữ gói lại trong danh sách.
    static gboolean rewriteOnePacketPt(GstBuffer** buf, guint, gpointer user) {
        if (!buf || !*buf) return TRUE;
        const guint8 want = static_cast<guint8>(GPOINTER_TO_UINT(user) & 0x7F);
        *buf = gst_buffer_make_writable(*buf);
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (gst_rtp_buffer_map(*buf, GST_MAP_READWRITE, &rtp)) {
            gst_rtp_buffer_set_payload_type(&rtp, want);
            gst_rtp_buffer_unmap(&rtp);
        }
        return TRUE;
    }

    // Probe trên src pad của payloader. aggregate-mode=zero-latency đẩy NAL đi
    // theo BUFFER LIST nên phải xử lý CẢ hai dạng — chỉ bắt buffer đơn thì gói
    // trong list lọt lưới mang PT cũ và trình duyệt vứt chúng.
    static GstPadProbeReturn onPayloadTypeProbe(GstPad*, GstPadProbeInfo* info,
                                                gpointer user) {
        if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
            GstBufferList* list = gst_pad_probe_info_get_buffer_list(info);
            if (list) {
                list = gst_buffer_list_make_writable(list);
                GST_PAD_PROBE_INFO_DATA(info) = list;
                gst_buffer_list_foreach(list, &WebRtcSession::rewriteOnePacketPt,
                                        user);
            }
        } else if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
            GstBuffer* buf = gst_pad_probe_info_get_buffer(info);
            if (buf) {
                rewriteOnePacketPt(&buf, 0, user);
                GST_PAD_PROBE_INFO_DATA(info) = buf;
            }
        }
        return GST_PAD_PROBE_OK;
    }

    // Gắn bộ viết lại PT lên payloader tên "pay" (buildLaunch đặt tên).
    bool attachPayloadTypeRewrite(int wirePt) {
        GstElement* pay = gst_bin_get_by_name(GST_BIN(m_pipeline), "pay");
        if (!pay) return false;
        GstPad* src = gst_element_get_static_pad(pay, "src");
        if (!src) {
            gst_object_unref(pay);
            return false;
        }
        gst_pad_add_probe(
            src,
            static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                                         GST_PAD_PROBE_TYPE_BUFFER_LIST),
            &WebRtcSession::onPayloadTypeProbe,
            GUINT_TO_POINTER(static_cast<guint>(wirePt)), nullptr);
        gst_object_unref(src);
        gst_object_unref(pay);
        g_print("[webrtc] session %s: viet lai payload-type RTP %d -> %d "
                "(H265 di thang, khong transcode)\n",
                m_sessionId.c_str(), kPayloaderInternalPt, wirePt);
        return true;
    }

    // SSRC dẫn xuất từ sessionId: mỗi phiên một giá trị ổn định, khác 0.
    guint32 sessionSsrc() const {
        guint32 value = 0;
        for (const char c : m_sessionId) {
            value = value * 31u + static_cast<guint32>(c);
        }
        return value ? value : 1u;
    }

    // sourceH265: luồng camera là H265. transcode: giải mã H265 rồi mã hoá lại
    // H264 bằng VPU (mppvideodec/mpph264enc) vì trình duyệt không nhận H265.
    std::string buildLaunch(int payloadType, guint32 ssrc,
                            bool sourceH265, bool transcode) const {
        // Nguồn KHÔNG còn là rtspsrc nối vào :8554 nữa: mỗi phiên làm vậy sẽ
        // lặp lại jitterbuffer + depay + parse trên cùng một luồng. Thay bằng
        // appsrc do CameraRtpSource (dùng chung theo camera) bơm access unit đã
        // parse vào — xem CameraRtpSource.hpp. Camera vẫn chỉ bị kéo một lần.
        //
        // Caps cố định để webrtcbin thương lượng được ngay cả khi chưa có buffer
        // nào (giống vai trò capsfilter cũ). is-live=true + do-timestamp=false:
        // giữ nguyên PTS của buffer từ nguồn, không tự dán dấu thời gian mới.
        const char* sourceMedia = sourceH265 ? "video/x-h265" : "video/x-h264";
        // Codec gửi cho trình duyệt: chỉ còn là H265 khi passthrough H265.
        const bool sendH265 = sourceH265 && !transcode;

        std::ostringstream launch;
        launch
            // do-timestamp=true: appsrc tự dán dấu thời gian theo đồng hồ của
            // pipeline PHIÊN NÀY lúc buffer tới. Bắt buộc, vì buffer đến từ
            // pipeline NGUỒN mang PTS theo đồng hồ khác — giữ nguyên PTS đó thì
            // rtph26Xpay tính ra RTP timestamp vô nghĩa và trình duyệt không
            // dựng lại được luồng (kết nối "connected" mà màn hình đen).
            << "appsrc name=src is-live=true format=time do-timestamp=true"
            << " max-bytes=0 block=false"
            << " ! " << sourceMedia << ",stream-format=byte-stream,alignment=au";

        if (transcode) {
            // Toàn bộ trên VPU, không đụng CPU/NPU. gop=-1 nghĩa là GOP = FPS
            // (một keyframe mỗi giây): người xem vào là có hình ngay ~1s, kể
            // cả khi camera gốc để GOP hàng chục giây — điểm cộng phụ của
            // đường transcode. bps=0 để encoder tự tính theo độ phân giải.
            // h265parse trước decoder cho chắc caps: giá của nó không đáng gì so
            // với phần giải mã, mà tránh mọi trục trặc thương lượng với mppvideodec.
            launch << " ! h265parse config-interval=-1"
                   << " ! mppvideodec ! mpph264enc gop=-1 rc-mode=vbr bps=0"
                   << " ! h264parse config-interval=-1";
        }

        // aggregate-mode=zero-latency: đẩy từng NAL đi ngay thay vì gom
        // theo access unit — đây là thứ quyết định độ trễ ở mức dưới giây.
        // ssrc phải trùng giữa payloader và capsfilter vì webrtcbin lấy ssrc
        // TỪ CAPS để viết "a=ssrc:" trong answer — lệch là Chrome giải mã
        // luồng "vô danh" còn track của <video> đói vĩnh viễn (màn đen).
        // Trình duyệt chào codec ở số payload ngoài dải payloader tự phát được
        // (Chrome/Windows: H265 ở 49) thì payloader chạy ở số hợp lệ rồi
        // `capssetter` sửa caps xuống đúng số đã chào — webrtcbin đọc caps để
        // viết "a=rtpmap:" trong answer nên answer khớp offer. Byte PT trong
        // TỪNG gói RTP do attachPayloadTypeRewrite() sửa (caps thôi là chưa đủ:
        // gói vẫn mang số cũ, trình duyệt vứt hết -> màn đen).
        const bool needPtRewrite = !payloadTypeUsable(payloadType);
        const int payPt = needPtRewrite ? kPayloaderInternalPt : payloadType;
        const char* encodingName = sendH265 ? "H265" : "H264";

        launch << " ! " << (sendH265 ? "rtph265pay" : "rtph264pay")
               << " name=pay pt=" << payPt << " ssrc=" << ssrc
               << " config-interval=-1";
        // aggregate-mode: H264 giữ zero-latency (đã chạy ổn lâu nay). H265 thì
        // dùng "none" — zero-latency gộp VPS/SPS/PPS vào gói AP (Aggregation
        // Packet), là điểm hay vênh nhất giữa các bộ giải mã HEVC; "none" chỉ
        // phát NAL đơn / FU mà mọi bộ giải mã đều nhận, và không thêm độ trễ.
        launch << (sendH265 ? " aggregate-mode=none"
                            : " aggregate-mode=zero-latency");
        if (needPtRewrite) {
            // capssetter join=true + replace=false: chỉ ĐÈ trường payload, giữ
            // nguyên media/encoding-name/clock-rate/ssrc payloader đã đặt.
            // KHÔNG nối capsfilter THẲNG vào payloader được — nó vướng đúng pad
            // template [96,127]; capssetter (template ANY) đứng giữa gỡ nút đó.
            launch << " ! capssetter join=true replace=false"
                   << " caps=application/x-rtp,payload=(int)" << payloadType;
        }
        // capsfilter ĐẦY ĐỦ, kể cả khi có capssetter phía trước. Không phải cho
        // đẹp: webrtcbin đọc caps trên sink pad LÚC TẠO ANSWER để viết
        // "a=ssrc:" và để chốt SSRC nó dùng khi gửi. Caps của capssetter chỉ
        // thành hình khi dữ liệu chảy — tức SAU answer — nên nếu chỉ có
        // capssetter thì answer KHÔNG có dòng a=ssrc nào và webrtcbin tự bốc
        // một SSRC khác: Chrome nhận gói, giải mã được, nhưng track gắn vào
        // <video> không bao giờ được nối -> MÀN ĐEN dù mọi state "connected"
        // (đo tận nơi: answer H264 có a=ssrc, answer H265 qua capssetter thì
        // không; MediaMTX — vốn chạy được trên Windows — gửi đủ bộ ssrc/msid).
        launch << " ! application/x-rtp,media=video,encoding-name="
               << encodingName << ",payload=" << payloadType
               << ",clock-rate=90000,ssrc=(uint)" << ssrc;

        launch
            // queue tách thread: nhánh rtspsrc không bao giờ bị khựng theo
            // đường gửi WebRTC (và ngược lại). Giới hạn 1s để người xem quá
            // chậm không kéo cả phiên phình bộ nhớ vô hạn.
            << " ! queue max-size-buffers=0 max-size-bytes=0 max-size-time=1000000000"
            << " ! webrtcbin name=webrtc bundle-policy=max-bundle latency=0";
        if (!m_config.webrtcStunServer.empty()) {
            launch << " stun-server=" << stream::quoteLaunchValue(m_config.webrtcStunServer);
        }
        if (!m_config.webrtcTurnServer.empty()) {
            launch << " turn-server=" << stream::quoteLaunchValue(m_config.webrtcTurnServer);
        }
        return launch.str();
    }

    // Đổi mọi "a=setup:actpass" trong offer thành "a=setup:active".
    // Trả lại nguyên bản nếu không có gì để đổi (trình duyệt đã chốt vai trò).
    static std::string forceRemoteDtlsActive(const std::string& sdp) {
        const std::string from = "a=setup:actpass";
        const std::string to = "a=setup:active";
        std::string out = sdp;
        for (size_t pos = out.find(from); pos != std::string::npos;
             pos = out.find(from, pos + to.size())) {
            out.replace(pos, from.size(), to);
        }
        return out;
    }

    // Chrome che IP nội bộ sau tên mDNS "<uuid>.local" trong ICE candidate và
    // KỲ VỌNG phía bên kia tự phân giải tên đó bằng mDNS. Máy này không có
    // avahi/nss-mdns nên getaddrinfo không phân giải được ".local" — candidate
    // đó với libnice là địa chỉ chết.
    //
    // May là ta biết IP thật của trình duyệt bằng đường khác: nó vừa POST offer
    // qua HTTP, proxy Next.js đọc được địa chỉ TCP nguồn và chuyển xuống qua
    // X-Forwarded-For. Thay hostname ".local" bằng IP đó cho ra đúng candidate
    // mà mDNS lẽ ra phải phân giải ra (cổng trong candidate vẫn là cổng thật,
    // Chrome chỉ che mỗi phần địa chỉ).
    //
    // Giới hạn chấp nhận được: nếu trình duyệt nằm sau NAT so với server thì IP
    // này là IP sau NAT và candidate vẫn chết — nhưng hệ này chạy trong LAN.
    //
    // Cấu trúc candidate (RFC 5245): "<foundation> <component> <transport>
    // <priority> <address> <port> typ <type> ..." — địa chỉ là token thứ 5.
    static std::string rewriteMdnsCandidate(const std::string& value,
                                            const std::string& hint) {
        if (hint.empty()) return value;
        std::istringstream in(value);
        std::vector<std::string> tokens;
        for (std::string token; in >> token;) tokens.push_back(token);
        if (tokens.size() < 6) return value;
        const std::string& address = tokens[4];
        const std::string suffix = ".local";
        if (address.size() <= suffix.size() ||
            address.compare(address.size() - suffix.size(), suffix.size(), suffix) != 0) {
            return value;  // địa chỉ thường, không đụng vào
        }
        tokens[4] = hint;
        std::ostringstream out;
        for (size_t i = 0; i < tokens.size(); ++i) {
            if (i) out << ' ';
            out << tokens[i];
        }
        return out.str();
    }

    // Trả về số candidate đã đẩy vào webrtcbin.
    static int addRemoteCandidatesFromSdp(GstElement* webrtcbin,
                                          const GstSDPMessage* sdp,
                                          const std::string& clientAddressHint) {
        if (!sdp) return 0;
        int added = 0;
        const guint medias = gst_sdp_message_medias_len(sdp);
        for (guint mline = 0; mline < medias; ++mline) {
            const GstSDPMedia* media = gst_sdp_message_get_media(sdp, mline);
            if (!media) continue;
            const guint attrs = gst_sdp_media_attributes_len(media);
            for (guint i = 0; i < attrs; ++i) {
                const GstSDPAttribute* attr = gst_sdp_media_get_attribute(media, i);
                if (!attr || !attr->key || g_strcmp0(attr->key, "candidate") != 0) continue;
                const std::string original = attr->value ? attr->value : "";
                const std::string rewritten =
                    rewriteMdnsCandidate(original, clientAddressHint);
                // In nguyên văn cả hai dạng: khi candidate vẫn còn ".local" mà
                // không có hint thay thế, triệu chứng y hệt lỗi mạng thường —
                // dòng log này là manh mối duy nhất.
                if (rewritten != original) {
                    g_print("[webrtc]   remote candidate[m%u]: %s\n"
                            "[webrtc]     -> thay mDNS bang IP tu HTTP: %s\n",
                            mline, original.c_str(), rewritten.c_str());
                } else {
                    g_print("[webrtc]   remote candidate[m%u]: %s\n", mline,
                            original.c_str());
                }
                // add-ice-candidate cần nguyên chuỗi kèm tiền tố "candidate:",
                // còn SDP tách key/value nên phải ghép lại.
                const std::string candidate = std::string("candidate:") + rewritten;
                g_signal_emit_by_name(webrtcbin, "add-ice-candidate", mline,
                                      candidate.c_str());
                ++added;
            }
        }
        return added;
    }

    bool waitForIceGathering() {
        std::unique_lock<std::mutex> lock(m_mutex);
        if (m_iceGatheringDone) return true;
        return m_cv.wait_for(lock,
                             std::chrono::milliseconds(kIceGatherTimeoutMs),
                             [this] { return m_iceGatheringDone; });
    }

    void touch() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_lastSeen = std::chrono::steady_clock::now();
    }

    static void onDataChannel(GstElement*, GObject* channel, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        if (!channel) return;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            if (self->m_dataChannel) g_object_unref(self->m_dataChannel);
            self->m_dataChannel = G_OBJECT(g_object_ref(channel));
        }
        g_print("[webrtc] +%5ldms session %s: trinh duyet mo kenh du lieu\n",
                self->elapsedMs(), self->m_sessionId.c_str());
        if (self->m_statusTimerId == 0) {
            self->m_statusTimerId =
                g_timeout_add(kStatusPushIntervalMs, &WebRtcSession::onStatusPush, self);
        }
    }

    // Đẩy JSON trạng thái xuống trình duyệt. Chỉ gửi khi kênh đã OPEN: gọi
    // send-string lúc kênh còn "connecting" là cảnh báo GLib chứ không gửi.
    static gboolean onStatusPush(gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        GObject* channel = nullptr;
        std::function<std::string()> provider;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            if (!self->m_dataChannel || !self->m_statusProvider) return G_SOURCE_CONTINUE;
            channel = G_OBJECT(g_object_ref(self->m_dataChannel));
            provider = self->m_statusProvider;
        }
        GstWebRTCDataChannelState state = GST_WEBRTC_DATA_CHANNEL_STATE_CONNECTING;
        g_object_get(channel, "ready-state", &state, nullptr);
        if (state == GST_WEBRTC_DATA_CHANNEL_STATE_OPEN) {
            const std::string payload = provider();
            if (!payload.empty()) {
                g_signal_emit_by_name(channel, "send-string", payload.c_str());
            }
        }
        g_object_unref(channel);
        return G_SOURCE_CONTINUE;
    }

    static gboolean onStatsTick(gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        {
            // Phiên đã connected thì thôi: mỗi người xem một dòng log mỗi 5s
            // sẽ ngập pm2 logs. Bộ đếm vẫn chạy, chỉ ngừng in.
            std::lock_guard<std::mutex> lock(self->m_mutex);
            if (self->m_connected) return G_SOURCE_CONTINUE;
        }
        g_print("[webrtc] +%5ldms session %s: %ld goi RTP tu camera da vao webrtcbin\n",
                self->elapsedMs(), self->m_sessionId.c_str(),
                static_cast<long>(self->m_rtpInCount.load()));
        return G_SOURCE_CONTINUE;
    }

    static void onIceGatheringState(GstElement* webrtcbin, GParamSpec*, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        GstWebRTCICEGatheringState state;
        g_object_get(webrtcbin, "ice-gathering-state", &state, nullptr);
        if (state != GST_WEBRTC_ICE_GATHERING_STATE_COMPLETE) return;
        {
            std::lock_guard<std::mutex> lock(self->m_mutex);
            self->m_iceGatheringDone = true;
        }
        self->m_cv.notify_all();
    }

    // Chỉ để chẩn đoán: ICE hỏng là nguyên nhân phổ biến nhất khiến "SDP xong
    // mà không có hình", và không có dòng log này thì chịu, không biết hỏng ở
    // đâu giữa gathering / checking / failed.
    static void onIceConnectionState(GstElement* webrtcbin, GParamSpec*, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        GstWebRTCICEConnectionState state;
        g_object_get(webrtcbin, "ice-connection-state", &state, nullptr);
        static const char* names[] = {"new", "checking", "connected", "completed",
                                      "failed", "disconnected", "closed"};
        const int index = static_cast<int>(state);
        g_print("[webrtc] +%5ldms session %s: ice-connection-state -> %s\n",
                self->elapsedMs(), self->m_sessionId.c_str(),
                (index >= 0 && index < 7) ? names[index] : "?");
    }

    static gboolean onBusMessage(GstBus*, GstMessage* message, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_error(message, &error, &debug);
            const gchar* srcName = GST_OBJECT_NAME(GST_MESSAGE_SRC(message));

            // Lỗi từ KÊNH DỮ LIỆU (SCTP) KHÔNG được giết phiên. Kênh này chỉ
            // để đẩy JSON trạng thái xuống trình duyệt — video chạy hoàn toàn
            // độc lập với nó. Quan sát thực tế: một phiên đã ICE "completed",
            // trình duyệt đã mở kênh dữ liệu, rồi sctpenc0 báo "Could not write
            // to resource" lúc association đổi trạng thái; vì mọi GST_MESSAGE_
            // ERROR đều đặt m_failed nên bộ dọn khai tử luôn phiên đang xem
            // tốt -> ô video tắt và player phải nối lại. Hiếm (1/254 phiên đo
            // được) nhưng là mất hình vô cớ. Ghi log rồi bỏ qua.
            const bool fromDataChannel =
                srcName && g_str_has_prefix(srcName, "sctp");
            g_print("[webrtc] session %s: %s tu %s: %s (%s)\n",
                    self->m_sessionId.c_str(),
                    fromDataChannel ? "LOI KENH DU LIEU (bo qua)" : "LOI",
                    srcName ? srcName : "?",
                    error && error->message ? error->message : "?",
                    debug ? debug : "");
            if (error) g_error_free(error);
            if (debug) g_free(debug);
            if (!fromDataChannel) {
                std::lock_guard<std::mutex> lock(self->m_mutex);
                self->m_failed = true;
            }
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) {
            // Mount RTSP nội bộ biến mất (camera bị sửa setting / restart) thì
            // rtspsrc báo EOS chứ KHÔNG báo lỗi. Không bắt ở đây là phiên nằm
            // im ở trạng thái connected với đường ống rỗng.
            g_print("[webrtc] session %s: nguon het du lieu (EOS), dong phien\n",
                    self->m_sessionId.c_str());
            std::lock_guard<std::mutex> lock(self->m_mutex);
            self->m_failed = true;
        } else if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) {
            GError* error = nullptr;
            gchar* debug = nullptr;
            gst_message_parse_warning(message, &error, &debug);
            g_print("[webrtc] session %s: canh bao tu %s: %s\n",
                    self->m_sessionId.c_str(),
                    GST_OBJECT_NAME(GST_MESSAGE_SRC(message)),
                    error && error->message ? error->message : "?");
            if (error) g_error_free(error);
            if (debug) g_free(debug);
        }
        return G_SOURCE_CONTINUE;
    }

    // RTCP về từ trình duyệt. Chạy trên thread RTCP của rtpbin — giữ khoá ngắn.
    static void onSsrcActive(GstElement*, guint, guint, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        std::lock_guard<std::mutex> lock(self->m_mutex);
        self->m_lastRtcp = std::chrono::steady_clock::now();
        self->m_rtcpSeen = true;
    }

    static void onConnectionState(GstElement* webrtcbin, GParamSpec*, gpointer userData) {
        auto* self = static_cast<WebRtcSession*>(userData);
        GstWebRTCPeerConnectionState state;
        g_object_get(webrtcbin, "connection-state", &state, nullptr);
        static const char* names[] = {"new", "connecting", "connected",
                                      "disconnected", "failed", "closed"};
        const int index = static_cast<int>(state);
        g_print("[webrtc] +%5ldms session %s: connection-state -> %s\n",
                self->elapsedMs(), self->m_sessionId.c_str(),
                (index >= 0 && index < 6) ? names[index] : "?");

        std::lock_guard<std::mutex> lock(self->m_mutex);
        if (state == GST_WEBRTC_PEER_CONNECTION_STATE_CONNECTED) {
            // Từ đây watchdog đo theo DỮ LIỆU chứ không theo trạng thái. Đặt
            // mốc ngay lúc này để camera GOP dài (chưa có gói nào) không bị
            // tính là đứng từ trước khi kịp bắt đầu.
            self->m_connected = true;
            self->m_lastMedia = std::chrono::steady_clock::now();
        } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_FAILED ||
                   state == GST_WEBRTC_PEER_CONNECTION_STATE_CLOSED) {
            // Trình duyệt biến mất mà không gọi DELETE (đóng tab, mất mạng):
            // dọn ngay, không giữ kết nối RTSP vô ích.
            self->m_failed = true;
        } else if (state == GST_WEBRTC_PEER_CONNECTION_STATE_DISCONNECTED) {
            // disconnected có thể chỉ là mạng chập chờn và tự hồi. Không giết
            // ngay, nhưng bỏ quyền miễn nhiễm và đếm lại từ đầu — hồi được thì
            // quay lại connected, không thì watchdog dọn sau kHandshakeTimeoutMs.
            self->m_connected = false;
            self->m_lastSeen = std::chrono::steady_clock::now();
        }
    }

    std::string m_sessionId;
    std::string m_cameraId;
    stream::GStreamerConfig m_config;
    std::string m_cameraCodec = "h264";
    // IP trình duyệt (X-Forwarded-For) — chỉ để hiển thị "ai đang xem", không
    // dùng cho logic. Rỗng nếu proxy không gắn header (xem WebRtcController).
    std::string m_clientAddr;

    GstElement* m_pipeline = nullptr;
    GstElement* m_webrtc = nullptr;
    guint m_busWatchId = 0;
    guint m_statsTimerId = 0;
    std::atomic<long> m_rtpInCount{0};

    // Nguồn RTP dùng chung của camera này, và cầu nối vào appsrc của phiên.
    std::shared_ptr<stream::FrameSource> m_source;
    // Lấy nguồn transcode H264 dùng chung cho camera H265 (xem
    // setTranscodedProvider). nullptr ở phiên xem lại và camera H264.
    std::function<std::shared_ptr<stream::FrameSource>()> m_transcodedProvider;
    GstElement* m_appsrc = nullptr;  // sở hữu (unref trong stop)
    stream::AppSrcBridge m_bridge;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_iceGatheringDone = false;
    bool m_failed = false;
    bool m_connected = false;
    // true = phiên xem lại: sức khoẻ đo bằng trạng thái WebRTC, không bằng
    // dòng dữ liệu (tạm dừng là im lặng hợp lệ).
    bool m_heartbeatMode = false;
    // Kênh dữ liệu + nguồn JSON trạng thái đẩy xuống trình duyệt.
    GObject* m_dataChannel = nullptr;
    std::function<std::string()> m_statusProvider;
    guint m_statusTimerId = 0;
    std::chrono::steady_clock::time_point m_lastSeen;
    std::chrono::steady_clock::time_point m_lastMedia = std::chrono::steady_clock::now();
    // Lần cuối trình duyệt gửi RTCP về. m_rtcpSeen phân biệt "chưa từng nhận"
    // với "đã im lặng" — xem kPeerSilenceTimeoutMs.
    std::chrono::steady_clock::time_point m_lastRtcp = std::chrono::steady_clock::now();
    bool m_rtcpSeen = false;
    std::chrono::steady_clock::time_point m_createdAt = std::chrono::steady_clock::now();
};

}  // namespace webrtc

#endif
