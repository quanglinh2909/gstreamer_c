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

    DTO_FIELD_INFO(path) { info->description = "Absolute path, dung lam stage.modelPath"; }
    DTO_FIELD(String, path);

    DTO_FIELD_INFO(sizeBytes) { info->description = "File size in bytes"; }
    DTO_FIELD(Int64, sizeBytes);
};

// Một cách dựng ảnh đầu vào cho tầng con từ hộp của tầng cha.
class AiTransformDto : public oatpp::DTO {
    DTO_INIT(AiTransformDto, DTO)

    DTO_FIELD_INFO(value) { info->description = "Value for stage.transform"; }
    DTO_FIELD(String, value);

    DTO_FIELD_INFO(label) { info->description = "Human-readable name"; }
    DTO_FIELD(String, label);

    DTO_FIELD_INFO(description) { info->description = "What the helper does"; }
    DTO_FIELD(String, description);
};

// Các giá trị hợp lệ cho stage.modelType. MỘT danh sách chứ không tách theo
// tầng: model là stage-agnostic, tầng nào cũng nhận được mọi loại — loại nào
// không cài vai trò của tầng đó thì đơn giản là không ra kết quả.
class AiModelTypesDto : public oatpp::DTO {
    DTO_INIT(AiModelTypesDto, DTO)

    DTO_FIELD_INFO(types) { info->description = "Valid values for stage.modelType"; }
    DTO_FIELD(List<String>, types);
};

#include OATPP_CODEGEN_END(DTO)

#endif
