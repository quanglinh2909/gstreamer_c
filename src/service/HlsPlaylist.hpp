#ifndef test_gstreamer_HlsPlaylist_hpp
#define test_gstreamer_HlsPlaylist_hpp

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace playback {

struct HlsSegment {
    std::string id;
    std::string startAt;
    int32_t durationMs = 0;
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

    for (const auto& segment : segments) {
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
