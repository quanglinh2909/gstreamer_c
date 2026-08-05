#ifndef test_gstreamer_StreamTypes_hpp
#define test_gstreamer_StreamTypes_hpp

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>

namespace stream {

enum class StreamCodec {
    Unknown,
    H264,
    H265,
    Unsupported
};

enum class StreamState {
    Stopped,
    Starting,
    Running,
    Reconnecting,
    AuthError,
    UnsupportedCodec,
    Error
};

struct GStreamerConfig {
    std::string rtspHost = "0.0.0.0";
    std::string publicRtspHost = "127.0.0.1";
    uint16_t rtspPort = 8554;
    uint32_t retryInitialMs = 1000;
    uint32_t retryMaxMs = 30000;
    uint32_t sourceLatencyMs = 100;
    // How often a Running stream re-probes its camera to confirm it is still
    // reachable. The republish pipeline only runs while a client is watching,
    // so without this poll a camera that drops while unwatched would stay
    // reported as online forever. 0 disables the poll.
    uint32_t healthCheckIntervalMs = 20000;
    std::string defaultHardware = "auto";
    bool recordingEnabled = false;
    std::string recordingDir = "recordings";
    // Ảnh JPEG chụp lúc bắt đầu MỖI sự kiện chuyển động, để xem lại còn thấy
    // được khung hình như các sự kiện AI khác.
    //
    // Thư mục RIÊNG, không nằm trong recordingDir: bộ dọn dung lượng bên
    // Python đo dung lượng từng loại bằng `du` trên đúng một thư mục, nhét
    // chung vào recordings là ảnh chuyển động bị tính vào hạn mức của video.
    std::string motionSnapshotDir = "motion-snapshots";
    // STUN server cho WebRTC. Để TRỐNG khi xem trong cùng mạng LAN: lúc đó
    // candidate host là đủ và không phải chờ round-trip ra Internet. Chỉ cần
    // đặt (vd "stun://stun.l.google.com:19302") khi người xem ở mạng khác.
    std::string webrtcStunServer;
    // TURN server cho WebRTC, dạng "turn://user:pass@host:port". Dùng khi NAT
    // hai đầu không đục lỗ được (NAT symmetric) hoặc mạng chặn UDP: media đi
    // vòng qua TURN thay vì nối thẳng. Tốn băng thông của TURN nên chỉ được
    // ICE chọn khi mọi cặp candidate trực tiếp đều hỏng.
    // Mật khẩu có ký tự lạ (@ : / ?) thì phải mã hoá %XX.
    std::string webrtcTurnServer;
};

struct CameraRuntimeConfig {
    std::string id;
    std::string name;
    std::string rtsp;
    std::string hardware = "auto";
    bool recordingEnabled = false;
    std::string recordingMode = "off";
    bool motionEnabled = false;
    double motionSensitivity = 0.5;
    double motionThreshold = 0.01;
    uint32_t preMotionSeconds = 10;
    uint32_t postMotionSeconds = 20;
    uint32_t segmentSeconds = 10;
    // Lưới phát hiện chuyển động theo ô. motioncells chỉ nhận gridx/gridy trong
    // khoảng 8..32.
    uint32_t motionGridX = 32;
    uint32_t motionGridY = 32;
    // BỎ KHÔNG DÙNG. Bản trước lưu một chữ số mức cho MỖI ô rồi gom ô cùng mức
    // thành một motioncells kèm mặt nạ che ô của mức khác. Cách đó CHẾT vì
    // motioncells chỉ đọc 255 ô đầu của motionmaskcellspos (đo trực tiếp: che
    // 255, 256 hay 300 ô đều ra kết quả y hệt) — lưới 20x20 trở lên là mặt nạ
    // vượt ngưỡng và phần dò SAI trong im lặng. Giữ cột lại để không mất dữ
    // liệu cũ; cái đang dùng là motionZones.
    std::string motionCellLevels;
    // Danh sách VÙNG chữ nhật, JSON:
    //   [{"r1":4,"c1":4,"r2":6,"c2":7,"level":8}, ...]
    // Toạ độ theo Ô của lưới, bao gồm cả hai đầu. level 1..10 = cần level*10%
    // số ô CỦA CHÍNH VÙNG ĐÓ cùng động. Hai vùng cùng mức vẫn là hai vùng riêng.
    std::string motionZones;
    // Ghi sự kiện xuống DB hay chỉ bắn WebSocket. Tắt thì lớp phủ live vẫn vẽ
    // bình thường, chỉ là không còn lịch sử để xem lại/tìm kiếm.
    bool motionSaveEvents = true;
};

struct StreamStatusSnapshot {
    std::string id;
    StreamState state = StreamState::Stopped;
    std::string inputRtsp;
    std::string outputRtsp;
    StreamCodec codec = StreamCodec::Unknown;
    std::string hardware;
    bool recordingEnabled = false;
    uint32_t retryCount = 0;
    std::string lastError;
    std::string lastChangedAt;
};

using StreamStatusSink = std::function<void(const StreamStatusSnapshot&)>;

inline std::string toLower(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

inline std::string toUpper(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

inline StreamCodec codecFromEncodingName(const std::string& encodingName) {
    const auto normalized = toUpper(encodingName);
    if (normalized == "H264") return StreamCodec::H264;
    if (normalized == "H265" || normalized == "HEVC") return StreamCodec::H265;
    if (normalized.empty()) return StreamCodec::Unknown;
    return StreamCodec::Unsupported;
}

inline const char* toString(StreamCodec codec) {
    switch (codec) {
        case StreamCodec::H264: return "h264";
        case StreamCodec::H265: return "h265";
        case StreamCodec::Unsupported: return "unsupported";
        case StreamCodec::Unknown:
        default: return "unknown";
    }
}

// Public, user-facing camera state. The fine-grained StreamState enum drives
// runtime logic (reconnect timers, error classification), but externally a
// camera is only ever reported as one of three values: online, offline, or
// error. Diagnostic detail remains available via lastError / retryCount.
inline const char* toString(StreamState state) {
    switch (state) {
        case StreamState::Running:
            return "online";
        case StreamState::AuthError:
        case StreamState::UnsupportedCodec:
        case StreamState::Error:
            return "error";
        case StreamState::Stopped:
        case StreamState::Starting:
        case StreamState::Reconnecting:
        default:
            return "offline";
    }
}

inline bool isAuthErrorMessage(const std::string& message) {
    const auto lower = toLower(message);
    return lower.find("401") != std::string::npos ||
           lower.find("403") != std::string::npos ||
           lower.find("unauthorized") != std::string::npos ||
           lower.find("forbidden") != std::string::npos ||
           lower.find("authentication") != std::string::npos ||
           lower.find("authorization") != std::string::npos ||
           lower.find("not authorized") != std::string::npos ||
           lower.find("permission denied") != std::string::npos;
}

inline uint32_t nextRetryDelayMs(const GStreamerConfig& config, uint32_t retryCount) {
    uint64_t delay = config.retryInitialMs;
    for (uint32_t i = 0; i < retryCount && delay < config.retryMaxMs; ++i) {
        delay *= 2;
    }
    return static_cast<uint32_t>(std::min<uint64_t>(delay, config.retryMaxMs));
}

inline std::string mountPathForCamera(const std::string& cameraId) {
    return "/cameras/" + cameraId;
}

inline std::string outputUrlForCamera(const GStreamerConfig& config,
                                      const std::string& cameraId) {
    const auto& host = config.publicRtspHost.empty()
        ? config.rtspHost
        : config.publicRtspHost;

    std::ostringstream out;
    out << "rtsp://" << host << ":" << config.rtspPort << mountPathForCamera(cameraId);
    return out.str();
}

inline std::string quoteLaunchValue(const std::string& value) {
    std::string escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back('"');
    for (char ch : value) {
        if (ch == '"' || ch == '\\') escaped.push_back('\\');
        escaped.push_back(ch);
    }
    escaped.push_back('"');
    return escaped;
}

inline std::string launchStringForCamera(const GStreamerConfig& config,
                                         const CameraRuntimeConfig& camera,
                                         StreamCodec codec) {
    const bool h264 = codec == StreamCodec::H264;
    const bool h265 = codec == StreamCodec::H265;
    if (!h264 && !h265) return {};

    const char* encoding = h264 ? "H264" : "H265";
    const char* depay = h264 ? "rtph264depay" : "rtph265depay";
    const char* parser = h264 ? "h264parse" : "h265parse";
    const char* pay = h264 ? "rtph264pay" : "rtph265pay";

    std::ostringstream launch;
    launch
        // KHÔNG drop-on-latency ở nhánh phân phối này: camera gửi IDR frame
        // lớn (4K ~ hàng trăm gói TCP dồn cục); lúc board bận, cả cục vượt
        // hạn latency và jitterbuffer vứt phần đuôi -> IDR hỏng -> MỌI người
        // xem (RTSP lẫn WebRTC) nhận luồng không giải mã nổi, hình đen/đứng
        // từng đợt theo tải máy. Nhánh này chỉ phục vụ xem live, trễ thêm vài
        // trăm ms vô hại; deadline thời gian thực đã có nhánh AI lo riêng.
        << "( rtspsrc name=src location=" << quoteLaunchValue(camera.rtsp)
        << " latency=" << (config.sourceLatencyMs < 300 ? 300 : config.sourceLatencyMs)
        << " protocols=tcp"
        << " ! application/x-rtp,media=video,encoding-name=" << encoding
        << " ! " << depay
        << " ! " << parser << " config-interval=-1"
        << " ! " << pay << " config-interval=1 name=pay0 pt=96 )";
    return launch.str();
}

} // namespace stream

#endif
