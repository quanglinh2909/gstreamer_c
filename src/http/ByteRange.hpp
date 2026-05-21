#ifndef test_gstreamer_ByteRange_hpp
#define test_gstreamer_ByteRange_hpp

#include <cstdint>
#include <string>

namespace http {

// Result of parsing an HTTP Range request header. `start` / `end` are only
// meaningful when `satisfiable` is true.
struct ByteRange {
    bool    present     = false;  // a parseable single "bytes=" range was supplied
    bool    satisfiable = false;  // the range is valid for the given file size
    int64_t start       = 0;      // first byte, inclusive
    int64_t end         = 0;      // last byte, inclusive
};

// Parses an HTTP Range header value (e.g. "bytes=0-1023") against a known file
// size. Only a single byte range is supported; a multi-range or unparseable
// value yields {present = false}, so the caller serves the whole file as 200
// (per RFC 7233: an unparseable Range header is ignored).
inline ByteRange parseByteRange(const std::string& rangeHeader, int64_t fileSize) {
    ByteRange result;

    const std::string prefix = "bytes=";
    if (rangeHeader.size() <= prefix.size() ||
        rangeHeader.compare(0, prefix.size(), prefix) != 0) {
        return result;  // missing, or not a "bytes=" range — ignore
    }
    const std::string spec = rangeHeader.substr(prefix.size());
    if (spec.find(',') != std::string::npos) {
        return result;  // multi-range — unsupported, ignore
    }
    const auto dash = spec.find('-');
    if (dash == std::string::npos) {
        return result;  // malformed — ignore
    }

    const std::string startStr = spec.substr(0, dash);
    const std::string endStr   = spec.substr(dash + 1);

    // A non-empty decimal string that fits safely in int64 (<= 18 digits).
    const auto isNumber = [](const std::string& s) {
        if (s.empty() || s.size() > 18) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };

    if (startStr.empty()) {
        // Suffix range "bytes=-N" — the last N bytes.
        if (!isNumber(endStr)) return result;  // malformed — ignore
        result.present = true;
        const int64_t suffix = std::stoll(endStr);
        if (suffix <= 0 || fileSize <= 0) return result;  // unsatisfiable
        result.start = suffix >= fileSize ? 0 : fileSize - suffix;
        result.end = fileSize - 1;
        result.satisfiable = true;
        return result;
    }

    // "bytes=START-" or "bytes=START-END".
    if (!isNumber(startStr)) return result;                   // malformed — ignore
    if (!endStr.empty() && !isNumber(endStr)) return result;  // malformed — ignore
    result.present = true;
    result.start = std::stoll(startStr);
    result.end = endStr.empty() ? (fileSize - 1) : std::stoll(endStr);
    if (result.end > fileSize - 1) result.end = fileSize - 1;  // clamp to end-of-file
    result.satisfiable =
        fileSize > 0 && result.start < fileSize && result.start <= result.end;
    return result;
}

}  // namespace http

#endif
