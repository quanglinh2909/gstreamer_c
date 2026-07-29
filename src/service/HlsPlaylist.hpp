#ifndef test_gstreamer_HlsPlaylist_hpp
#define test_gstreamer_HlsPlaylist_hpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace playback {

// Parse mốc thời gian "YYYY-MM-DD[ T]HH:MM:SS..." thành epoch giây. Bỏ qua phần
// thập phân và offset múi giờ: chỉ dùng để so SÁI KHÁC giữa hai đoạn liên tiếp
// (cùng camera nên cùng offset, hiệu số không đổi). Trả -1 nếu không parse được.
inline long long parseEpochSeconds(const std::string& value) {
    if (value.size() < 19) return -1;
    std::string s = value.substr(0, 19);
    if (s[10] == ' ') s[10] = 'T';
    std::tm tm{};
    std::istringstream in(s);
    in >> std::get_time(&tm, "%Y-%m-%dT%H:%M:%S");
    if (in.fail()) return -1;
    return static_cast<long long>(timegm(&tm));
}

struct HlsSegment {
    std::string id;
    std::string startAt;
    int32_t durationMs = 0;
    // Mốc phiên ghi (epoch ms). Phiên đổi = PTS reset (mpegtsmux luôn bắt đầu
    // ~3600s mỗi pipeline mới) -> BẮT BUỘC chèn DISCONTINUITY dù wall-clock
    // liền mạch. 0 = không rõ (dữ liệu cũ) -> chỉ dựa luật khoảng trống.
    int64_t sessionStart = 0;
};

inline std::string normalizeProgramDateTime(std::string value) {
    if (value.size() > 10 && value[10] == ' ') {
        value[10] = 'T';
    }
    if (value.size() >= 3) {
        const auto sign = value[value.size() - 3];
        const auto d1 = value[value.size() - 2];
        const auto d2 = value[value.size() - 1];
        if ((sign == '+' || sign == '-') &&
            d1 >= '0' && d1 <= '9' &&
            d2 >= '0' && d2 <= '9') {
            value += ":00";
        }
    }
    return value;
}

inline int targetDurationSeconds(const std::vector<HlsSegment>& segments) {
    int target = 1;
    for (const auto& segment : segments) {
        target = std::max(target, static_cast<int>(std::ceil(
            static_cast<double>(std::max<int32_t>(0, segment.durationMs)) / 1000.0)));
    }
    return target;
}

inline std::string segmentFileUrl(const std::string& segmentId) {
    return "/recording-segments/" + segmentId + "/file";
}

inline std::string buildVodPlaylist(const std::vector<HlsSegment>& segments) {
    std::ostringstream out;
    out << "#EXTM3U\n"
        << "#EXT-X-VERSION:3\n"
        << "#EXT-X-PLAYLIST-TYPE:VOD\n"
        << "#EXT-X-TARGETDURATION:" << targetDurationSeconds(segments) << "\n"
        << "#EXT-X-MEDIA-SEQUENCE:0\n";

    for (size_t i = 0; i < segments.size(); ++i) {
        const auto& segment = segments[i];

        // Chèn #EXT-X-DISCONTINUITY khi:
        // (a) đoạn này thuộc PHIÊN ghi khác đoạn trước — pipeline mới = PTS
        //     reset (mpegtsmux bắt đầu lại ~3600s) dù wall-clock liền mạch;
        //     thiếu tag này hls.js seek/phát ngang qua sẽ loạn PTS và kẹt; hoặc
        // (b) có quãng không ghi ở giữa (khoảng trống wall-clock). Ngưỡng > 2s
        //     để bỏ qua sai số làm tròn giữa hai đoạn kề nhau.
        if (i > 0) {
            const auto& prev = segments[i - 1];
            const bool sessionChanged = prev.sessionStart != segment.sessionStart &&
                (prev.sessionStart != 0 || segment.sessionStart != 0);
            const long long prevStart = parseEpochSeconds(prev.startAt);
            const long long curStart = parseEpochSeconds(segment.startAt);
            const long long prevDur =
                std::max<int32_t>(0, prev.durationMs) / 1000;
            const bool wallGap = prevStart >= 0 && curStart >= 0 &&
                curStart - prevStart > prevDur + 2;
            if (sessionChanged || wallGap) {
                out << "#EXT-X-DISCONTINUITY\n";
            }
        }

        out << "#EXT-X-PROGRAM-DATE-TIME:" << normalizeProgramDateTime(segment.startAt) << "\n"
            << "#EXTINF:" << std::fixed << std::setprecision(3)
            << (static_cast<double>(std::max<int32_t>(0, segment.durationMs)) / 1000.0)
            << ",\n"
            << segmentFileUrl(segment.id) << "\n";
    }

    out << "#EXT-X-ENDLIST\n";
    return out.str();
}

}  // namespace playback

#endif
