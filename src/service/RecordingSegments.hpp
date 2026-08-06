#ifndef test_gstreamer_RecordingSegments_hpp
#define test_gstreamer_RecordingSegments_hpp

// Đọc danh sách đoạn ghi từ DB cho các phiên XEM LẠI.
//
// Tách khỏi PlaybackController vì bây giờ có HAI đường xem lại dùng chung nó:
// WHEP/WebRTC (PlaybackController) và MoQ (MoqController). Chép đôi phần lọc
// 'complete'/'ts' và phần đổi mốc thời gian là cách chắc chắn nhất để hai
// đường trôi lệch nhau sau vài lần sửa.

#include "dto/CameraDto.hpp"
#include "service/CameraService.hpp"
#include "service/HlsPlaylist.hpp"

#include "oatpp/core/macro/component.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>
#include <vector>

namespace playback {

struct SegmentRow {
    std::string id;
    std::string path;
    std::string codec;
    int64_t startMs = 0;
    int64_t endMs = 0;
};

// "2026-07-23 13:37:20.512+00" -> epoch ms. parseEpochSeconds bỏ phần thập
// phân (nó chỉ cần so sánh giây), còn ở đây sai một phần giây là con trỏ
// timeline lệch thấy được, nên phải cộng lại mili giây.
inline int64_t parseEpochMs(const std::string& value) {
    const long long seconds = parseEpochSeconds(value);
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

inline std::string isoFromMs(int64_t ms) {
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

// Đọc các đoạn 'complete' trong khoảng [fromMs, toMs).
//
// NUỐT mọi ngoại lệ: hàm này còn được gọi TỪ THREAD FEEDER của
// PlaybackSource, mà CameraService ném lỗi HTTP của oatpp — ngoại lệ thoát
// khỏi một thread không phải thread HTTP sẽ giết cả tiến trình.
inline std::vector<SegmentRow> loadSegments(CameraService& service,
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
        OATPP_LOGE("RecordingSegments", "loadSegments loi: %s", exc.what());
    } catch (...) {
        OATPP_LOGE("RecordingSegments", "loadSegments loi khong ro");
    }
    return out;
}

}  // namespace playback

#endif  // test_gstreamer_RecordingSegments_hpp
