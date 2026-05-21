#ifndef test_gstreamer_AiJobController_hpp
#define test_gstreamer_AiJobController_hpp

#include "service/AiJobService.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/core/utils/ConversionUtils.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include OATPP_CODEGEN_BEGIN(ApiController)

// REST API for AI jobs. Each mutating endpoint persists to the ai_jobs table
// and pushes the change into the live in-process AiManager.
class AiJobController : public oatpp::web::server::api::ApiController {
public:
    explicit AiJobController(
        OATPP_COMPONENT(std::shared_ptr<ObjectMapper>, objectMapper))
        : oatpp::web::server::api::ApiController(objectMapper) {}

    ENDPOINT_INFO(getAll) {
        info->summary = "List AI jobs";
        info->addResponse<oatpp::List<oatpp::Object<AiJobDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/ai-jobs", getAll,
             QUERY(oatpp::String, limitStr, "limit", "50"),
             QUERY(oatpp::String, offsetStr, "offset", "0"))
    {
        v_int64 limit = oatpp::utils::conversion::strToInt64(limitStr->c_str());
        v_int64 offset = oatpp::utils::conversion::strToInt64(offsetStr->c_str());
        return createDtoResponse(Status::CODE_200,
                                 m_service.getAllAiJobs(limit, offset));
    }

    ENDPOINT_INFO(getModels) {
        info->summary = "List .rknn models in the weights directory";
        info->addResponse<oatpp::List<oatpp::Object<AiModelDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/ai-models", getModels)
    {
        return createDtoResponse(Status::CODE_200, m_service.getModels());
    }

    ENDPOINT_INFO(getTransforms) {
        info->summary = "List stage-2 helper functions (transforms)";
        info->addResponse<oatpp::List<oatpp::Object<AiTransformDto>>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/ai-transforms", getTransforms)
    {
        return createDtoResponse(Status::CODE_200, m_service.getTransforms());
    }

    ENDPOINT_INFO(getModelTypes) {
        info->summary = "List supported model types for modelType / modelType2";
        info->addResponse<oatpp::Object<AiModelTypesDto>>(
            Status::CODE_200, "application/json");
    }
    ENDPOINT("GET", "/ai-model-types", getModelTypes)
    {
        return createDtoResponse(Status::CODE_200, m_service.getModelTypes());
    }

    ENDPOINT_INFO(getOne) {
        info->summary = "Get an AI job by id";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/ai-jobs/{id}", getOne, PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200, m_service.getAiJobById(id));
    }

    ENDPOINT_INFO(create) {
        info->summary = "Create an AI job";
        info->addConsumes<oatpp::Object<CreateAiJobDto>>("application/json");
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_201, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_400, "application/json");
    }
    ENDPOINT("POST", "/ai-jobs", create,
             BODY_DTO(oatpp::Object<CreateAiJobDto>, dto))
    {
        return createDtoResponse(Status::CODE_201, m_service.createAiJob(dto));
    }

    ENDPOINT_INFO(update) {
        info->summary = "Update an AI job";
        info->addConsumes<oatpp::Object<CreateAiJobDto>>("application/json");
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_404, "application/json");
    }
    ENDPOINT("PUT", "/ai-jobs/{id}", update,
             PATH(oatpp::String, id),
             BODY_DTO(oatpp::Object<CreateAiJobDto>, dto))
    {
        return createDtoResponse(Status::CODE_200, m_service.updateAiJob(id, dto));
    }

    ENDPOINT_INFO(startJob) {
        info->summary = "Enable and start an AI job";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_404, "application/json");
    }
    ENDPOINT("POST", "/ai-jobs/{id}/start", startJob, PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200, m_service.startAiJob(id));
    }

    ENDPOINT_INFO(stopJob) {
        info->summary = "Disable and stop an AI job";
        info->addResponse<oatpp::Object<AiJobDto>>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_404, "application/json");
    }
    ENDPOINT("POST", "/ai-jobs/{id}/stop", stopJob, PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200, m_service.stopAiJob(id));
    }

    ENDPOINT_INFO(remove) {
        info->summary = "Delete an AI job";
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_200, "application/json");
        info->addResponse<oatpp::Object<StatusDto>>(Status::CODE_404, "application/json");
    }
    ENDPOINT("DELETE", "/ai-jobs/{id}", remove, PATH(oatpp::String, id))
    {
        return createDtoResponse(Status::CODE_200, m_service.deleteAiJob(id));
    }

private:
    AiJobService m_service;
};

#include OATPP_CODEGEN_END(ApiController)

#endif
