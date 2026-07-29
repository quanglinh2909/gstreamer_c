#ifndef test_gstreamer_ConfigComponent_hpp
#define test_gstreamer_ConfigComponent_hpp

// Đọc config/config.json rồi cho phép GHI ĐÈ từng tham số bằng biến môi
// trường. Mục đích: một file config.json DUY NHẤT dùng chung cho mọi máy
// (commit vào repo), còn thứ khác nhau giữa các máy — đường dẫn weights,
// chuỗi kết nối DB, IP RTSP công khai... — thì đặt bằng env trên từng máy
// (pm2 / systemd / export trong ~/.bashrc). Không còn cảnh mỗi máy sửa tay
// một bản config rồi commit đè lẫn nhau.
//
// Mọi biến đều có tiền tố GS_ để không đụng với biến của backend Python
// chạy cùng máy — đặc biệt là DATABASE_URL: Python cần dạng
// "postgresql+asyncpg://..." còn bản C++ này cần "postgresql://...", dùng
// chung một tên biến sẽ làm hỏng một trong hai.
//
//   GS_SERVER_HOST        server.host
//   GS_SERVER_PORT        server.port
//   GS_DATABASE_URL       database.url
//   GS_RTSP_HOST          gstreamer.rtspHost
//   GS_PUBLIC_RTSP_HOST   gstreamer.publicRtspHost
//   GS_RTSP_PORT          gstreamer.rtspPort
//   GS_DEFAULT_HARDWARE   gstreamer.defaultHardware
//   GS_RECORDING_ENABLED  gstreamer.recordingEnabled   (1/true/yes/on)
//   GS_RECORDING_DIR      gstreamer.recordingDir
//   GS_WEBRTC_STUN_SERVER gstreamer.webrtcStunServer
//   GS_WEBRTC_TURN_SERVER gstreamer.webrtcTurnServer
//   GS_WEIGHTS_DIR        ai.weightsDir

#include "config/ConfigDto.hpp"

#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/component.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

class ConfigComponent {
private:
    std::string m_path;

    // Giá trị env, hoặc nullptr khi không đặt / đặt rỗng. Chuỗi rỗng bị coi
    // như "không đặt" để `GS_FOO=` không vô tình xoá giá trị trong JSON.
    static const char* env(const char* name) {
        const char* v = std::getenv(name);
        return (v && *v) ? v : nullptr;
    }

    static void overrideStr(const char* name, oatpp::String& field) {
        if (const char* v = env(name)) {
            field = oatpp::String(v);
            std::cerr << "[config] " << name << " ghi de -> " << v << std::endl;
        }
    }

    // Số không hợp lệ -> giữ nguyên giá trị JSON và cảnh báo, thay vì im lặng
    // chạy với 0 (một cổng bằng 0 sẽ hỏng theo kiểu rất khó lần ra).
    template <typename T>
    static void overrideNum(const char* name, T& field, unsigned long maxVal) {
        const char* v = env(name);
        if (!v) return;
        try {
            const std::string s(v);
            size_t used = 0;
            unsigned long n = std::stoul(s, &used);
            if (used != s.size() || n > maxVal) throw std::out_of_range(s);
            field = static_cast<typename T::UnderlyingType>(n);
            std::cerr << "[config] " << name << " ghi de -> " << n << std::endl;
        } catch (const std::exception&) {
            std::cerr << "[config] BO QUA " << name << "='" << v
                      << "' (khong phai so hop le, toi da " << maxVal
                      << ") -- giu gia tri trong config.json" << std::endl;
        }
    }

    static void overrideBool(const char* name, oatpp::Boolean& field) {
        const char* v = env(name);
        if (!v) return;
        std::string s(v);
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        const bool on = (s == "1" || s == "true" || s == "yes" || s == "on");
        const bool off = (s == "0" || s == "false" || s == "no" || s == "off");
        if (!on && !off) {
            std::cerr << "[config] BO QUA " << name << "='" << v
                      << "' (can 1/0, true/false, yes/no, on/off)" << std::endl;
            return;
        }
        field = on;
        std::cerr << "[config] " << name << " ghi de -> "
                  << (on ? "true" : "false") << std::endl;
    }

    // Tạo các section còn thiếu để env vẫn dùng được với một config.json tối
    // giản (ví dụ file không có mục "ai" nhưng máy này muốn set GS_WEIGHTS_DIR).
    static void applyEnvOverrides(const oatpp::Object<ConfigDto>& cfg) {
        if (!cfg->server)    cfg->server    = ServerConfigDto::createShared();
        if (!cfg->database)  cfg->database  = DatabaseConfigDto::createShared();
        if (!cfg->gstreamer) cfg->gstreamer = GStreamerConfigDto::createShared();
        if (!cfg->ai)        cfg->ai        = AiConfigDto::createShared();

        overrideStr("GS_SERVER_HOST", cfg->server->host);
        overrideNum("GS_SERVER_PORT", cfg->server->port, 65535);

        overrideStr("GS_DATABASE_URL", cfg->database->url);

        overrideStr("GS_RTSP_HOST", cfg->gstreamer->rtspHost);
        overrideStr("GS_PUBLIC_RTSP_HOST", cfg->gstreamer->publicRtspHost);
        overrideNum("GS_RTSP_PORT", cfg->gstreamer->rtspPort, 65535);
        overrideStr("GS_DEFAULT_HARDWARE", cfg->gstreamer->defaultHardware);
        overrideBool("GS_RECORDING_ENABLED", cfg->gstreamer->recordingEnabled);
        overrideStr("GS_RECORDING_DIR", cfg->gstreamer->recordingDir);
        overrideStr("GS_WEBRTC_STUN_SERVER", cfg->gstreamer->webrtcStunServer);
        overrideStr("GS_WEBRTC_TURN_SERVER", cfg->gstreamer->webrtcTurnServer);

        overrideStr("GS_WEIGHTS_DIR", cfg->ai->weightsDir);
    }

public:
    explicit ConfigComponent(std::string configPath)
        : m_path(std::move(configPath)) {}

    OATPP_CREATE_COMPONENT(oatpp::Object<ConfigDto>, appConfig)([this] {
        std::ifstream file(m_path);
        if (!file.is_open()) {
            throw std::runtime_error("Cannot open config file: " + m_path);
        }
        std::stringstream ss;
        ss << file.rdbuf();
        auto mapper = oatpp::parser::json::mapping::ObjectMapper::createShared();
        auto cfg = mapper->readFromString<oatpp::Object<ConfigDto>>(
            oatpp::String(ss.str()));
        if (!cfg) {
            throw std::runtime_error("Cannot parse config file: " + m_path);
        }
        applyEnvOverrides(cfg);
        return cfg;
    }());
};

#endif
