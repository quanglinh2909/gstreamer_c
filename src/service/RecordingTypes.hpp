#ifndef test_gstreamer_RecordingTypes_hpp
#define test_gstreamer_RecordingTypes_hpp

#include "service/StreamTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace recording {

enum class RecordingMode {
    Off,
    Always,
    Motion
};

struct SegmentTime {
    int64_t startMs = 0;
    int64_t endMs = 0;
};

struct MotionWindow {
    int64_t startMs = 0;
    int64_t endMs = 0;
    int64_t preMotionMs = 10'000;
    int64_t postMotionMs = 20'000;
};

struct RecordingSegmentSnapshot {
    std::string cameraId;
    std::string path;
    std::string startAt;
    std::string endAt;
    int32_t durationMs = 0;
    std::string codec;
    std::string container = "ts";
    std::string recordingMode;
    bool hasMotion = false;
    // "recording" = đoạn ĐANG ghi (mới mở, dùng cho live-edge của timeline);
    // "complete"  = đoạn đã đóng, số liệu end_at/duration là thật.
    std::string status = "complete";
    // Epoch ms lúc PHIÊN ghi (pipeline) bắt đầu — mọi đoạn của cùng một phiên
    // mang cùng giá trị. Phiên mới = PTS reset (mpegtsmux bắt đầu lại ~3600s);
    // playlist chèn DISCONTINUITY khi giá trị này đổi giữa hai đoạn kề nhau.
    int64_t sessionStartMs = 0;
};

struct MotionEventSnapshot {
    std::string cameraId;
    std::string startAt;
    std::string endAt;
    double maxScore = 0.0;
    // Các ô đã động trong suốt sự kiện, gộp lại, dạng "hàng:cột" ngăn bằng dấu
    // phẩy — đúng định dạng motioncells trả về. Dùng để vẽ lại và tìm theo vùng.
    std::string cells;
    uint32_t gridX = 0;
    uint32_t gridY = 0;
    // Khung hình lúc sự kiện BẮT ĐẦU, đường dẫn tương đối thư mục gstreamer_c
    // (vd "motion-snapshots/<camera>/2026-07-31/1753948800123.jpg"). Rỗng khi
    // nhánh ảnh chưa có khung nào — sự kiện vẫn hợp lệ, chỉ là không có ảnh.
    std::string imagePath;
    // Cờ đi CÙNG sự kiện chứ không tra lại camera ở nơi nhận: sink chạy trên
    // luồng GStreamer và chỉ có mỗi snapshot này trong tay.
    bool saveToDb = true;
};

// Ảnh chụp MỘT KHUNG có chuyển động — bắn liên tục (5 khung/giây) chứ không đợi
// sự kiện kết thúc như MotionEventSnapshot. Chỉ dùng để VẼ lên video trực tiếp:
// người xem cần thấy động là thấy ngay, và thấy cả chỗ động NGOÀI vùng đã vẽ
// (vẽ khác màu) chứ không phải chỉ những gì đã đủ ngưỡng sinh sự kiện.
//
// Không lưu DB, không dựng sự kiện. Engine chỉ gửi cho socket nào ĐÃ ĐĂNG KÝ
// đúng camera này — 16 camera × 5 khung/giây đổ cho mọi người xem là vô ích.
struct MotionFrameSnapshot {
    std::string cameraId;
    /** Ô đã động NẰM TRONG một vùng đã vẽ. */
    std::string insideCells;
    /** Ô đã động nằm ngoài mọi vùng. */
    std::string outsideCells;
    uint32_t gridX = 0;
    uint32_t gridY = 0;
};

struct RecordingErrorSnapshot {
    std::string cameraId;
    std::string message;
};

using RecordingSegmentSink = std::function<void(const RecordingSegmentSnapshot&)>;
using MotionEventSink = std::function<void(const MotionEventSnapshot&)>;
using MotionFrameSink = std::function<void(const MotionFrameSnapshot&)>;
using RecordingErrorSink = std::function<void(const RecordingErrorSnapshot&)>;

inline const char* toString(RecordingMode mode) {
    switch (mode) {
        case RecordingMode::Always: return "always";
        case RecordingMode::Motion: return "motion";
        case RecordingMode::Off:
        default: return "off";
    }
}

inline RecordingMode recordingModeFromString(const std::string& value) {
    const auto normalized = stream::toLower(value);
    if (normalized == "always") return RecordingMode::Always;
    if (normalized == "motion") return RecordingMode::Motion;
    return RecordingMode::Off;
}

inline bool isValidRecordingMode(const std::string& value) {
    const auto normalized = stream::toLower(value);
    return normalized == "off" || normalized == "always" || normalized == "motion";
}

inline RecordingMode effectiveRecordingMode(const stream::CameraRuntimeConfig& camera) {
    const auto mode = recordingModeFromString(camera.recordingMode);
    if (mode == RecordingMode::Off && camera.recordingEnabled) {
        return RecordingMode::Always;
    }
    return mode;
}

inline bool recordingPatchRequiresRuntimeRestart(bool recordingEnabledPresent,
                                                 bool recordingModePresent,
                                                 bool motionEnabledPresent,
                                                 bool motionSensitivityPresent,
                                                 bool motionThresholdPresent,
                                                 bool preMotionSecondsPresent,
                                                 bool postMotionSecondsPresent,
                                                 bool segmentSecondsPresent,
                                                 bool motionKeyframeOnlyPresent) {
    return recordingEnabledPresent ||
           recordingModePresent ||
           motionEnabledPresent ||
           motionSensitivityPresent ||
           motionThresholdPresent ||
           preMotionSecondsPresent ||
           postMotionSecondsPresent ||
           segmentSecondsPresent ||
           motionKeyframeOnlyPresent;
}

enum class MotionMessageKind {
    None,
    Started,
    Finished
};

// motioncells gửi thông điệp tên "motion" cho cả ba việc, phân biệt bằng TRƯỜNG
// nào có mặt (tên cấu trúc lúc nào cũng là "motion"):
//   motion_begin    -> khung ĐẦU TIÊN của một đợt chuyển động
//   motion          -> MỖI khung tiếp theo còn chuyển động (postallmotion=true)
//   motion_finished -> đã im lặng đủ `gap` giây
//
// Trường "motion" (khung giữa) TỪNG BỊ BỎ QUA ở đây, và đó là lỗi: bản dò theo
// vùng đếm ô trên TỪNG khung, mà chỉ khung đầu mới có motion_begin — nên gần như
// mọi khung đều bị vứt và vùng không bao giờ đủ ngưỡng. Đo được trên
// gst-launch: gói đầu có `motion_begin=(guint64)…`, các gói sau chỉ có
// `motion=(guint64)…`, cả hai đều kèm motion_cells_indices.
inline MotionMessageKind classifyMotionMessage(const std::string& structureName,
                                               bool hasBeginField,
                                               bool hasFinishedField,
                                               bool hasCellsField) {
    if (structureName != "motion") return MotionMessageKind::None;
    if (hasFinishedField) return MotionMessageKind::Finished;
    if (hasBeginField || hasCellsField) return MotionMessageKind::Started;
    return MotionMessageKind::None;
}

inline bool containsTimestamp(const SegmentTime& segment, int64_t timestampMs) {
    return segment.startMs <= timestampMs && timestampMs < segment.endMs;
}

inline int64_t seekOffsetMs(const SegmentTime& segment, int64_t timestampMs) {
    if (timestampMs <= segment.startMs) return 0;
    if (timestampMs >= segment.endMs) {
        return std::max<int64_t>(0, segment.endMs - segment.startMs);
    }
    return timestampMs - segment.startMs;
}

inline bool rangesOverlap(int64_t leftStartMs,
                          int64_t leftEndMs,
                          int64_t rightStartMs,
                          int64_t rightEndMs) {
    return leftStartMs < rightEndMs && rightStartMs < leftEndMs;
}

inline bool segmentOverlapsMotionWindow(const SegmentTime& segment,
                                        const MotionWindow& motion) {
    const auto keepStart = motion.startMs - std::max<int64_t>(0, motion.preMotionMs);
    const auto keepEnd = motion.endMs + std::max<int64_t>(0, motion.postMotionMs);
    return rangesOverlap(segment.startMs, segment.endMs, keepStart, keepEnd);
}

// Ordered preference list of decoder element names for the motion branch.
// Every list ends with the software decoder so resolution always succeeds.
inline std::vector<std::string> motionDecoderCandidates(const std::string& hardware,
                                                        stream::StreamCodec codec) {
    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if (!h264 && !h265) return {};  // Unknown / Unsupported — no decoder to offer

    const std::string va  = h264 ? "vah264dec"   : "vah265dec";
    const std::string nv  = h264 ? "nvh264dec"   : "nvh265dec";
    const std::string v4l = h264 ? "v4l2h264dec" : "v4l2h265dec";
    const std::string sw  = h264 ? "avdec_h264"  : "avdec_h265";
    // Rockchip MPP decoder is multi-codec; it takes the parsed H264/H265 the
    // motion branch always provides (it taps the tee after h264parse /
    // h265parse). On RK35xx boards it is typically the ONLY decoder
    // installed — without it in the list the motion branch silently resolved
    // to no decoder and motion-triggered recording never armed.
    const std::string mpp = "mppvideodec";

    const auto mode = stream::toLower(hardware);
    if (mode == "software" || mode == "sw" || mode == "none" || mode == "off") {
        return {sw};
    }
    if (mode == "vaapi" || mode == "va") {
        return {va, sw};
    }
    if (mode == "nvdec" || mode == "nvidia" || mode == "cuda") {
        return {nv, sw};
    }
    if (mode == "v4l2") {
        return {v4l, sw};
    }
    if (mode == "mpp" || mode == "rockchip") {
        return {mpp, sw};
    }
    return {mpp, va, nv, v4l, sw};  // auto / empty / unknown
}

// ---------------------------------------------------------------------------
// Lưới chuyển động theo ô
// ---------------------------------------------------------------------------
//
// motioncells CHỈ có MỘT `sensitivity` cho cả khung — không có mức riêng từng
// ô (đã kiểm bằng gst-inspect trên chính board này). Nên muốn mỗi ô một mức thì
// phải dựng NHIỀU motioncells song song trên cùng một nhánh giải mã: mỗi mức
// một phần tử, và mặt nạ `motionmaskcellspos` che hết những ô KHÔNG thuộc mức
// đó. Nhánh này chỉ chạy ở 320px/5fps nên vài phần tử vẫn rẻ, nhưng số mức
// khác nhau bị chặn ở 9 nên chi phí có trần.

inline uint32_t clampMotionGrid(uint32_t value) {
    // motioncells chặn gridx/gridy trong 8..32; ra ngoài là GStreamer tự kẹp mà
    // không báo, dẫn tới toạ độ ô lệch hẳn so với lưới người dùng đã vẽ.
    return std::min<uint32_t>(32, std::max<uint32_t>(8, value));
}

/**
 * Một VÙNG chuyển động: hình chữ nhật theo toạ độ Ô của lưới (bao gồm cả hai
 * đầu) kèm mức 1..10.
 *
 * Mức N = "cần N×10% số ô CỦA CHÍNH VÙNG NÀY cùng động trong một khung".
 * Hai vùng cùng mức vẫn là HAI vùng độc lập — vẽ hai ô mức 8 nghĩa là hai chỗ
 * cần canh riêng, không phải một vùng 2 ô.
 */
struct MotionZone {
    int r1 = 0, c1 = 0, r2 = 0, c2 = 0;
    int level = 1;

    size_t cellCount() const {
        return static_cast<size_t>(r2 - r1 + 1) * static_cast<size_t>(c2 - c1 + 1);
    }
    /** Số ô phải cùng động thì vùng này tính là "có động". */
    size_t needCells() const {
        const int lv = std::min(10, std::max(1, level));
        const size_t cells = cellCount();
        const size_t need = (static_cast<size_t>(lv) * cells + 9) / 10;
        return std::max<size_t>(1, std::min(cells, need));
    }
    bool contains(int row, int col) const {
        return row >= r1 && row <= r2 && col >= c1 && col <= c2;
    }
};

/**
 * Đọc JSON `[{"r1":..,"c1":..,"r2":..,"c2":..,"level":..}, ...]`.
 *
 * Tự bóc tay chứ không kéo thư viện JSON vào: header này còn được biên dịch cho
 * bộ test không có oatpp, và hình dạng dữ liệu chỉ có đúng 5 số nguyên. Bất cứ
 * thứ gì không đọc được thì BỎ vùng đó — thà thiếu một vùng còn hơn dò nhầm chỗ.
 */
inline std::vector<MotionZone> parseMotionZones(const std::string& json,
                                                uint32_t gridX,
                                                uint32_t gridY) {
    std::vector<MotionZone> out;
    const int maxRow = static_cast<int>(gridY) - 1;
    const int maxCol = static_cast<int>(gridX) - 1;

    size_t i = 0;
    while (true) {
        const auto open = json.find('{', i);
        if (open == std::string::npos) break;
        const auto close = json.find('}', open);
        if (close == std::string::npos) break;
        const std::string body = json.substr(open + 1, close - open - 1);
        i = close + 1;

        auto field = [&body](const char* key, int fallback) {
            const std::string needle = std::string("\"") + key + "\"";
            const auto at = body.find(needle);
            if (at == std::string::npos) return fallback;
            const auto colon = body.find(':', at + needle.size());
            if (colon == std::string::npos) return fallback;
            try {
                return static_cast<int>(std::lround(std::stod(body.substr(colon + 1))));
            } catch (...) {
                return fallback;
            }
        };

        MotionZone zone;
        zone.r1 = field("r1", -1);
        zone.c1 = field("c1", -1);
        zone.r2 = field("r2", -1);
        zone.c2 = field("c2", -1);
        zone.level = field("level", 1);
        if (zone.r1 < 0 || zone.c1 < 0 || zone.r2 < 0 || zone.c2 < 0) continue;

        // Kéo chuột từ phải sang trái / dưới lên trên là r2<r1 — chuẩn hoá chứ
        // không vứt, người dùng vẫn vẽ ra một hình chữ nhật hợp lệ.
        if (zone.r1 > zone.r2) std::swap(zone.r1, zone.r2);
        if (zone.c1 > zone.c2) std::swap(zone.c1, zone.c2);
        zone.r1 = std::max(0, std::min(maxRow, zone.r1));
        zone.r2 = std::max(0, std::min(maxRow, zone.r2));
        zone.c1 = std::max(0, std::min(maxCol, zone.c1));
        zone.c2 = std::max(0, std::min(maxCol, zone.c2));
        zone.level = std::min(10, std::max(1, zone.level));
        out.push_back(zone);
    }
    return out;
}

inline std::vector<MotionZone> motionZonesOf(const stream::CameraRuntimeConfig& camera) {
    return parseMotionZones(camera.motionZones,
                            clampMotionGrid(camera.motionGridX),
                            clampMotionGrid(camera.motionGridY));
}

inline std::string recordingFilePattern(const stream::GStreamerConfig& config,
                                        const stream::CameraRuntimeConfig& camera) {
    std::ostringstream out;
    out << config.recordingDir << "/" << camera.id << "/segment-%010d.ts";
    return out.str();
}

/**
 * Đường dẫn TƯƠNG ĐỐI (so với thư mục làm việc của engine = gstreamer_c) của
 * ảnh một sự kiện chuyển động: "<dir>/<cameraId>/<YYYY-MM-DD>/<epochMs>.jpg".
 *
 * Tương đối chứ không tuyệt đối vì đây là giá trị ĐI VÀO DB, và bộ dọn dung
 * lượng bên Python ghép nó với thư mục gstreamer_c của riêng nó — hệt cách
 * recording_segments.path đang làm.
 *
 * Chia theo ngày để một thư mục không phình tới hàng chục nghìn file: camera
 * ngoài trời sinh vài nghìn sự kiện mỗi ngày, và `du`/`rm` trên thư mục phẳng
 * cỡ đó chậm thấy rõ.
 */
inline std::string motionSnapshotRelativePath(const stream::GStreamerConfig& config,
                                              const std::string& cameraId,
                                              const std::string& dateYmd,
                                              int64_t epochMs) {
    std::ostringstream out;
    out << config.motionSnapshotDir << "/" << cameraId << "/" << dateYmd
        << "/" << epochMs << ".jpg";
    return out.str();
}

inline std::string recordingFilePathForTimestamp(const stream::GStreamerConfig& config,
                                                 const stream::CameraRuntimeConfig& camera,
                                                 const std::string& timestamp) {
    std::ostringstream out;
    out << config.recordingDir << "/" << camera.id << "/" << timestamp << ".ts";
    return out.str();
}

inline std::string recordingLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                  const stream::CameraRuntimeConfig& camera,
                                                  stream::StreamCodec codec,
                                                  const std::string& motionDecoder,
                                                  bool includeMotionBranch = true) {
    const auto mode = effectiveRecordingMode(camera);
    if (mode == RecordingMode::Off) return {};

    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if (!h264 && !h265) return {};

    const char* parsedCaps = h264
        ? "video/x-h264,stream-format=byte-stream,alignment=au"
        : "video/x-h265,stream-format=byte-stream,alignment=au";

    const uint64_t segmentNs =
        static_cast<uint64_t>(std::max<uint32_t>(1, camera.segmentSeconds)) * 1000ULL * 1000ULL * 1000ULL;

    // Nguồn là appsrc do CameraRtpSource (dùng chung với xem live) bơm access
    // unit đã parse vào — KHÔNG còn rtspsrc riêng, camera chỉ bị kéo một lần dù
    // vừa ghi vừa xem. do-timestamp=true: buffer từ pipeline nguồn mang PTS
    // đồng hồ khác, appsrc phải tự dán lại theo đồng hồ pipeline ghi hình thì
    // splitmuxsink mới cắt segment đúng. Xem AppSrcBridge / CameraRecordingSession.
    //
    // No surrounding "( ... )": this string is parsed with gst_parse_launch,
    // which returns a real GstPipeline only for an unwrapped description.
    std::ostringstream launch;
    launch
        << "appsrc name=record_src is-live=true format=time do-timestamp=true"
        << " max-bytes=0 block=false"
        << " ! " << parsedCaps
        << " ! tee name=record_t"
        << " record_t. ! queue"
        << " ! splitmuxsink name=record_sink async-finalize=true"
        << " message-forward=true"
        << " muxer-factory=mpegtsmux"
        << " max-size-time=" << segmentNs
        << " send-keyframe-requests=true"
        << " location=" << stream::quoteLaunchValue(recordingFilePattern(config, camera));

    if (includeMotionBranch && (mode == RecordingMode::Motion || camera.motionEnabled)) {
        const auto gx = clampMotionGrid(camera.motionGridX);
        const auto gy = clampMotionGrid(camera.motionGridY);
        const double totalCells = static_cast<double>(gx) * gy;

        // ĐÚNG MỘT motioncells cho cả camera, KHÔNG mặt nạ, ngưỡng đặt ở mức
        // "một ô cũng báo". Toàn bộ chuyện vùng — ô nào thuộc vùng nào, vùng đó
        // cần bao nhiêu ô — do CameraRecordingSession tự tính từ danh sách ô mà
        // phần tử gửi lên.
        //
        // Vì sao KHÔNG dùng motionmaskcellspos + một phần tử cho mỗi vùng như
        // bản trước: ĐO ĐƯỢC motioncells chỉ đọc 255 ô ĐẦU TIÊN của mặt nạ.
        // Trên lưới 32x32, che 255 / 256 / 300 ô cho ra kết quả giống hệt nhau
        // (ô đầu tiên bị bỏ lọt luôn là chỉ số 255). Vùng nhỏ trên lưới lớn cần
        // che cả nghìn ô, tức mặt nạ vô tác dụng mà không có lỗi nào báo.
        //
        // Đổi lại còn rẻ hơn: một phần tử cho mọi vùng thay vì một phần tử cho
        // mỗi mức.
        launch
            << " record_t. ! queue name=motion_q leaky=downstream max-size-buffers=2"
            << " ! " << motionDecoder
            << " ! videorate drop-only=true"
            << " ! video/x-raw,framerate=5/1"
            // Rẽ đôi SAU giải mã: một nhánh dò chuyển động, một nhánh giữ sẵn
            // ảnh để chụp lúc sự kiện bắt đầu. Rẽ ở đây chứ không rẽ sớm hơn để
            // không phải giải mã lần thứ hai — giải mã là phần đắt nhất.
            << " ! tee name=motion_tee"
            << " motion_tee. ! queue leaky=downstream max-size-buffers=2"
            << " ! videoscale"
            << " ! video/x-raw,width=320"
            << " ! videoconvert"
            << " ! video/x-raw,format=RGB"
            // postallmotion=true: cần danh sách ô của MỖI khung; chỉ lấy
            // begin/finished thì chỉ biết ô của đúng khung đầu tiên.
            << " ! motioncells name=motion_detector display=false postallmotion=true"
            << " gridx=" << gx
            << " gridy=" << gy
            // sensitivity = mức đổi PIXEL trong một ô mới tính ô đó động; giữ
            // mặc định của motioncells. Cái người dùng chỉnh là MỨC CỦA VÙNG.
            << " sensitivity=0.5"
            // Ngưỡng của phần tử chỉ để "có ít nhất một ô động thì gửi tin".
            // Đã đo: so sánh là >=, nên lấy nửa ô cho chắc.
            << " threshold=" << (0.5 / totalCells)
            // gap CHỈ nhận 1..60 (gst-inspect motioncells). Đặt ngoài dải là
            // GStreamer bỏ qua giá trị, in một cảnh báo rồi dùng mặc định 5 —
            // tức là camera để "Ghi sau = 0" thì thực tế chạy 5 giây mà không
            // ai biết. Kẹp ở đây, KHÔNG kẹp postMotionSeconds nói chung: cửa sổ
            // giữ đoạn ghi (postMotionDuration) thì 0 là một giá trị hợp lệ.
            << " gap=" << std::min<uint32_t>(60, std::max<uint32_t>(1, camera.postMotionSeconds))
            << " ! fakesink sync=false";

        // Nhánh ẢNH: giữ sẵn một khung JPEG mới nhất để lúc sự kiện bắt đầu là
        // ghi ra đĩa ngay. Trước đây thẻ sự kiện chuyển động phải mượn khung từ
        // endpoint thumbnail của BẢN GHI — hỏng ở hai chỗ: camera không bật ghi
        // thì không có gì để trích, và sự kiện vừa xảy ra thì đoạn chứa nó còn
        // đang ghi dở nên trả 404.
        //
        // 1 khung/giây chứ không 5: ảnh chỉ dùng làm ảnh đại diện cho cả sự
        // kiện, chậm tối đa một giây là không nhìn ra được, mà encode JPEG bằng
        // phần mềm thì đắt gấp bội việc dò ô.
        launch
            << " motion_tee. ! queue leaky=downstream max-size-buffers=1"
            << " ! videorate drop-only=true"
            << " ! video/x-raw,framerate=1/1"
            << " ! videoscale"
            // pixel-aspect-ratio=1/1 là BẮT BUỘC, không phải trang trí: chỉ ép
            // width thì videoscale giữ nguyên height và bù tỉ lệ bằng PAR, ra
            // 640x1080 với PAR không vuông. JPEG không mang được PAR nên ảnh
            // hiện lên bị kéo dọc. Đo được: bỏ dòng này -> "JPEG 640x1080".
            << " ! video/x-raw,width=640,pixel-aspect-ratio=1/1"
            << " ! videoconvert"
            << " ! jpegenc quality=75"
            // drop=true + max-buffers=1: appsink này không ai đọc cho tới lúc
            // có sự kiện, không cho rơi khung là nó chặn ngược cả nhánh dò.
            << " ! appsink name=motion_snap emit-signals=false sync=false"
            << " max-buffers=1 drop=true";
    }

    return launch.str();
}

// Builds the launch string for the on-demand motion-debug RTSP mount. The
// string is wrapped in "( ... )" because it is fed to
// gst_rtsp_media_factory_set_launch. motioncells runs at 320px/5fps exactly
// like the recording motion branch (so the operator sees real production
// behaviour); the annotated frames are scaled up to 960px purely for viewing
// and re-encoded to H264. Returns {} for a non-H264/H265 codec or an empty
// decoder name.
inline std::string motionDebugLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                    const stream::CameraRuntimeConfig& camera,
                                                    stream::StreamCodec codec,
                                                    const std::string& motionDecoder,
                                                    const std::string& h264Encoder = "x264enc") {
    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if ((!h264 && !h265) || motionDecoder.empty()) return {};

    const char* encoding = h264 ? "H264" : "H265";
    const char* depay = h264 ? "rtph264depay" : "rtph265depay";
    const char* parser = h264 ? "h264parse" : "h265parse";

    std::ostringstream launch;
    launch
        << "( rtspsrc name=motion_dbg_src location=" << stream::quoteLaunchValue(camera.rtsp)
        << " latency=" << config.sourceLatencyMs
        << " protocols=tcp"
        << " drop-on-latency=true"
        << " ! application/x-rtp,media=video,encoding-name=" << encoding
        << " ! " << depay
        << " ! " << parser << " config-interval=-1"
        << " ! " << motionDecoder
        << " ! videorate drop-only=true"
        << " ! video/x-raw,framerate=5/1"
        << " ! videoscale"
        << " ! video/x-raw,width=320"
        << " ! videoconvert"
        << " ! video/x-raw,format=RGB"
        // Luồng XEM THỬ: một motioncells duy nhất phủ cả khung, chỉ để nhìn ô
        // nào đang nhấp nháy. KHÔNG dùng camera.motionSensitivity/motionThreshold
        // nữa — hai cột đó không còn ai đặt (giao diện đã bỏ, mức của vùng mới
        // là thứ quyết định) nên phần lớn camera đang để 0, mà threshold=0 nghĩa
        // là "0% số ô cũng tính": xem thử thì lúc nào cũng thấy động.
        << " ! motioncells display=true postallmotion=false"
        << " sensitivity=0.5"
        << " threshold=0.01"
        // Kẹp 1..60 như nhánh ghi hình ở trên — cùng một property, cùng một bẫy.
        << " gap=" << std::min<uint32_t>(60, std::max<uint32_t>(1, camera.postMotionSeconds))
        << " ! videoscale"
        << " ! video/x-raw,width=960,height=540"
        << " ! videoconvert";
    if (h264Encoder == "mpph264enc") {
        // Hardware encode: feed the MPP encoder its native NV12.
        launch << " ! video/x-raw,format=NV12"
               << " ! mpph264enc";
    } else {
        launch << " ! video/x-raw,format=I420"
               << " ! x264enc tune=zerolatency speed-preset=ultrafast";
    }
    launch << " ! rtph264pay config-interval=1 name=pay0 pt=96 )";
    return launch.str();
}

} // namespace recording

#endif
