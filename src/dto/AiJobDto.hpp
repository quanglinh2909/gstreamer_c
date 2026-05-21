#ifndef test_gstreamer_AiJobDto_hpp
#define test_gstreamer_AiJobDto_hpp

#include "oatpp/core/Types.hpp"
#include "oatpp/core/macro/codegen.hpp"

#include OATPP_CODEGEN_BEGIN(DTO)

class AiJobDto : public oatpp::DTO {
    DTO_INIT(AiJobDto, DTO)

    DTO_FIELD_INFO(id) { info->description = "AI job id (UUID, server-generated)"; }
    DTO_FIELD(String, id);

    DTO_FIELD_INFO(name) { info->description = "Display name"; }
    DTO_FIELD(String, name);

    DTO_FIELD_INFO(cameraId) { info->description = "Camera this job runs on"; }
    DTO_FIELD(String, cameraId);

    DTO_FIELD_INFO(enabled) { info->description = "Whether the job runs"; }
    DTO_FIELD(Boolean, enabled);

    DTO_FIELD_INFO(modelPath) { info->description = "Model-1 .rknn path"; }
    DTO_FIELD(String, modelPath);

    DTO_FIELD_INFO(modelType) { info->description = "yolov8_detect | yolov8_seg | yolov8_pose"; }
    DTO_FIELD(String, modelType);

    DTO_FIELD_INFO(classFilter) { info->description = "Class ids to keep: 'all' or csv e.g. '0,2'"; }
    DTO_FIELD(String, classFilter);

    DTO_FIELD_INFO(modelPath2) { info->description = "Model-2 .rknn path (empty = single stage)"; }
    DTO_FIELD(String, modelPath2);

    DTO_FIELD_INFO(modelType2) { info->description = "face_recognition | yolov8_detect | ..."; }
    DTO_FIELD(String, modelType2);

    DTO_FIELD_INFO(transformData) { info->description = "'' | align_face | align_plate"; }
    DTO_FIELD(String, transformData);

    DTO_FIELD_INFO(primaryConf) { info->description = "Model-1 score threshold"; }
    DTO_FIELD(Float64, primaryConf);

    DTO_FIELD_INFO(secondaryConf) { info->description = "Model-2 score threshold"; }
    DTO_FIELD(Float64, secondaryConf);

    DTO_FIELD_INFO(maxFps) { info->description = "Inference fps cap (0 = unlimited)"; }
    DTO_FIELD(Int32, maxFps);
};

class CreateAiJobDto : public oatpp::DTO {
    DTO_INIT(CreateAiJobDto, DTO)

    DTO_FIELD(String, name);
    DTO_FIELD(String, cameraId);
    DTO_FIELD(Boolean, enabled);
    DTO_FIELD(String, modelPath);
    DTO_FIELD(String, modelType);
    DTO_FIELD(String, classFilter);
    DTO_FIELD(String, modelPath2);
    DTO_FIELD(String, modelType2);
    DTO_FIELD(String, transformData);
    DTO_FIELD(Float64, primaryConf);
    DTO_FIELD(Float64, secondaryConf);
    DTO_FIELD(Int32, maxFps);
};

#include OATPP_CODEGEN_END(DTO)

#endif
