#ifndef test_gstreamer_ConfigComponent_hpp
#define test_gstreamer_ConfigComponent_hpp

#include "config/ConfigDto.hpp"

#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/macro/component.hpp"

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

class ConfigComponent {
private:
    std::string m_path;

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
        return mapper->readFromString<oatpp::Object<ConfigDto>>(
            oatpp::String(ss.str()));
    }());
};

#endif
