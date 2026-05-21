#ifndef test_gstreamer_AiCatalogDto_hpp
#define test_gstreamer_AiCatalogDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

// One .rknn model file discovered in the weights directory.
class AiModelDto : public oatpp::DTO {
    DTO_INIT(AiModelDto, DTO)

    DTO_FIELD_INFO(fileName) { info->description = "Model file name"; }
    DTO_FIELD(String, fileName);

    DTO_FIELD_INFO(path) { info->description = "Absolute path, use as modelPath/modelPath2"; }
    DTO_FIELD(String, path);

    DTO_FIELD_INFO(sizeBytes) { info->description = "File size in bytes"; }
    DTO_FIELD(Int64, sizeBytes);
};

// One stage-2 helper (transform) that can be applied to a crop before model 2.
class AiTransformDto : public oatpp::DTO {
    DTO_INIT(AiTransformDto, DTO)

    DTO_FIELD_INFO(value) { info->description = "Value for AiJob.transformData"; }
    DTO_FIELD(String, value);

    DTO_FIELD_INFO(label) { info->description = "Human-readable name"; }
    DTO_FIELD(String, label);

    DTO_FIELD_INFO(description) { info->description = "What the helper does"; }
    DTO_FIELD(String, description);
};

// Supported model-type values for AiJob.modelType / modelType2.
class AiModelTypesDto : public oatpp::DTO {
    DTO_INIT(AiModelTypesDto, DTO)

    DTO_FIELD_INFO(stage1) { info->description = "Valid values for AiJob.modelType"; }
    DTO_FIELD(List<String>, stage1);

    DTO_FIELD_INFO(stage2) { info->description = "Valid values for AiJob.modelType2"; }
    DTO_FIELD(List<String>, stage2);
};

#include OATPP_CODEGEN_END(DTO)

#endif
