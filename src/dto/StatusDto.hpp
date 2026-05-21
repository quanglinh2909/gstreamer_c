#ifndef test_gstreamer_StatusDto_hpp
#define test_gstreamer_StatusDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class StatusDto : public oatpp::DTO {
    DTO_INIT(StatusDto, DTO)

    DTO_FIELD(Int32,  statusCode);
    DTO_FIELD(String, message);
};

#include OATPP_CODEGEN_END(DTO)

#endif
