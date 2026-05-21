#ifndef test_gstreamer_Uuid_hpp
#define test_gstreamer_Uuid_hpp

#include <string_view>

namespace http {

inline bool isHexDigit(char ch) {
    return (ch >= '0' && ch <= '9') ||
           (ch >= 'a' && ch <= 'f') ||
           (ch >= 'A' && ch <= 'F');
}

inline bool isUuid(std::string_view value) {
    if (value.size() != 36) return false;

    for (std::size_t i = 0; i < value.size(); ++i) {
        const bool dashPosition = i == 8 || i == 13 || i == 18 || i == 23;
        if (dashPosition) {
            if (value[i] != '-') return false;
        } else if (!isHexDigit(value[i])) {
            return false;
        }
    }

    return true;
}

}  // namespace http

#endif
