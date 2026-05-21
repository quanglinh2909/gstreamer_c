#ifndef test_gstreamer_ConfigDto_hpp
#define test_gstreamer_ConfigDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class ServerConfigDto : public oatpp::DTO {
    DTO_INIT(ServerConfigDto, DTO)
    DTO_FIELD(String, host);
    DTO_FIELD(UInt16, port);
};

class DatabaseConfigDto : public oatpp::DTO {
    DTO_INIT(DatabaseConfigDto, DTO)
    DTO_FIELD(String, url);
    DTO_FIELD(UInt32, poolMaxConnections);
    DTO_FIELD(UInt32, poolIdleSeconds);
};

class SwaggerConfigDto : public oatpp::DTO {
    DTO_INIT(SwaggerConfigDto, DTO)
    DTO_FIELD(String, title);
    DTO_FIELD(String, description);
    DTO_FIELD(String, version);
};

class GStreamerConfigDto : public oatpp::DTO {
    DTO_INIT(GStreamerConfigDto, DTO)
    DTO_FIELD(String, rtspHost);
    DTO_FIELD(String, publicRtspHost);
    DTO_FIELD(UInt16, rtspPort);
    DTO_FIELD(UInt32, retryInitialMs);
    DTO_FIELD(UInt32, retryMaxMs);
    DTO_FIELD(UInt32, sourceLatencyMs);
    DTO_FIELD(String, defaultHardware);
    DTO_FIELD(Boolean, recordingEnabled);
    DTO_FIELD(String, recordingDir);
};

class AiConfigDto : public oatpp::DTO {
    DTO_INIT(AiConfigDto, DTO)
    DTO_FIELD(String, weightsDir);  // directory scanned for .rknn models
};

class ConfigDto : public oatpp::DTO {
    DTO_INIT(ConfigDto, DTO)
    DTO_FIELD(Object<ServerConfigDto>,   server);
    DTO_FIELD(Object<DatabaseConfigDto>, database);
    DTO_FIELD(Object<SwaggerConfigDto>,  swagger);
    DTO_FIELD(Object<GStreamerConfigDto>, gstreamer);
    DTO_FIELD(Object<AiConfigDto>,       ai);
};

#include OATPP_CODEGEN_END(DTO)

#endif
