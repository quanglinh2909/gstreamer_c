#ifndef test_gstreamer_StreamStatusDto_hpp
#define test_gstreamer_StreamStatusDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class StreamStatusDto : public oatpp::DTO {
    DTO_INIT(StreamStatusDto, DTO)

    DTO_FIELD(String, id);
    DTO_FIELD(String, state);
    DTO_FIELD(String, inputRtsp);
    DTO_FIELD(String, outputRtsp);
    DTO_FIELD(String, codec);
    DTO_FIELD(String, hardware);
    DTO_FIELD(Boolean, recordingEnabled);
    DTO_FIELD(UInt32, retryCount);
    DTO_FIELD(String, lastError);
    DTO_FIELD(String, lastChangedAt);
};

#include OATPP_CODEGEN_END(DTO)

#endif
