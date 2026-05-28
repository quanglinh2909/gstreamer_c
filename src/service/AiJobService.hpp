#ifndef test_gstreamer_AiJobService_hpp
#define test_gstreamer_AiJobService_hpp

#include "ai/AiCatalog.hpp"
#include "ai/AiManager.hpp"
#include "config/ConfigDto.hpp"
#include "db/AiJobDb.hpp"
#include "db/CameraDb.hpp"
#include "dto/AiCatalogDto.hpp"
#include "dto/AiJobDto.hpp"
#include "dto/CameraDto.hpp"
#include "dto/StatusDto.hpp"
#include "http/Uuid.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

// CRUD + lifecycle for AI jobs. Persists rows in the ai_jobs table and keeps
// the in-process AiManager in sync so create/update/start/stop/delete take
// effect live.
class AiJobService {
public:
    using Status = oatpp::web::protocol::http::Status;

    oatpp::Object<AiJobDto> createAiJob(const oatpp::Object<CreateAiJobDto>& in) {
        validate(in, /* requireAll */ true);
        requireCameraExists(in->cameraId);

        auto res = m_db->createAiJob(in->name, in->cameraId, in->enabled,
                                     in->modelPath, in->modelType, in->classFilter,
                                     in->modelPath2, in->modelType2, in->transformData,
                                     in->primaryConf, in->secondaryConf, in->maxFps);
        assertSuccess(res);
        auto job = fetchOne(res, Status::CODE_500, "Failed to create AI job");
        syncToManager(job);
        return job;
    }

    oatpp::Object<AiJobDto> getAiJobById(const oatpp::String& id) {
        validateUuid(id, "Invalid AI job id");
        auto res = m_db->getAiJobById(id);
        assertSuccess(res);
        return fetchOne(res, Status::CODE_404, "AI job not found");
    }

    oatpp::List<oatpp::Object<AiJobDto>>
    getAllAiJobs(const oatpp::Int64& limit, const oatpp::Int64& offset) {
        auto res = m_db->getAllAiJobs(limit, offset);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<AiJobDto>>>();
    }

    // All AI jobs (enabled and disabled) belonging to one camera.
    oatpp::List<oatpp::Object<AiJobDto>>
    getAiJobsByCamera(const oatpp::String& cameraId) {
        validateUuid(cameraId, "Invalid camera id");
        OATPP_ASSERT_HTTP(fetchCamera(cameraId), Status::CODE_404,
                          "Camera not found");
        auto res = m_db->getAiJobsByCamera(cameraId);
        assertSuccess(res);
        return res->fetch<oatpp::List<oatpp::Object<AiJobDto>>>();
    }

    oatpp::Object<AiJobDto> updateAiJob(const oatpp::String& id,
                                        const oatpp::Object<CreateAiJobDto>& in) {
        validate(in, /* requireAll */ false);
        validateUuid(id, "Invalid AI job id");
        if (in->cameraId) requireCameraExists(in->cameraId);

        auto res = m_db->updateAiJob(id, in->name, in->cameraId, in->enabled,
                                     in->modelPath, in->modelType, in->classFilter,
                                     in->modelPath2, in->modelType2, in->transformData,
                                     in->primaryConf, in->secondaryConf, in->maxFps);
        assertSuccess(res);
        auto job = fetchOne(res, Status::CODE_404, "AI job not found");
        syncToManager(job);
        return job;
    }

    oatpp::Object<AiJobDto> startAiJob(const oatpp::String& id) {
        validateUuid(id, "Invalid AI job id");
        auto res = m_db->setAiJobEnabled(id, oatpp::Boolean(true));
        assertSuccess(res);
        auto job = fetchOne(res, Status::CODE_404, "AI job not found");
        syncToManager(job);
        return job;
    }

    oatpp::Object<AiJobDto> stopAiJob(const oatpp::String& id) {
        validateUuid(id, "Invalid AI job id");
        auto res = m_db->setAiJobEnabled(id, oatpp::Boolean(false));
        assertSuccess(res);
        auto job = fetchOne(res, Status::CODE_404, "AI job not found");
        m_ai->removeJob(stdstr(job->cameraId), stdstr(job->id));
        return job;
    }

    oatpp::Object<StatusDto> deleteAiJob(const oatpp::String& id) {
        validateUuid(id, "Invalid AI job id");
        auto res = m_db->deleteAiJobReturning(id);
        assertSuccess(res);
        auto job = fetchOne(res, Status::CODE_404, "AI job not found");
        m_ai->removeJob(stdstr(job->cameraId), stdstr(job->id));

        auto dto = StatusDto::createShared();
        dto->statusCode = 200;
        dto->message = "Deleted";
        return dto;
    }

    // Lists the .rknn model files found in the configured weights directory.
    oatpp::List<oatpp::Object<AiModelDto>> getModels() {
        auto list = oatpp::List<oatpp::Object<AiModelDto>>::createShared();

        std::string dir = "weights";
        if (m_config && m_config->ai && m_config->ai->weightsDir) {
            dir = m_config->ai->weightsDir->c_str();
        }

        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) return list;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && entry.path().extension() == ".rknn") {
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());

        for (const auto& file : files) {
            auto dto = AiModelDto::createShared();
            const std::string fileName = file.filename().string();
            const std::string fullPath = file.string();
            dto->fileName = oatpp::String(fileName.c_str());
            dto->path = oatpp::String(fullPath.c_str());
            std::error_code sizeEc;
            dto->sizeBytes = static_cast<v_int64>(
                std::filesystem::file_size(file, sizeEc));
            list->push_back(dto);
        }
        return list;
    }

    // Lists the stage-2 helper functions (transforms) a job can apply to a
    // crop before model 2. Driven by AiCatalog — a newly registered transform
    // shows up here automatically.
    oatpp::List<oatpp::Object<AiTransformDto>> getTransforms() {
        auto list = oatpp::List<oatpp::Object<AiTransformDto>>::createShared();
        for (const auto& transform : ai::transformList()) {
            auto dto = AiTransformDto::createShared();
            dto->value = oatpp::String(transform->id().c_str());
            dto->label = oatpp::String(transform->label().c_str());
            dto->description = oatpp::String(transform->description().c_str());
            list->push_back(dto);
        }
        return list;
    }

    // Supported model-type values for modelType (model 1) and modelType2
    // (model 2). Models are stage-agnostic, so both lists are identical.
    // Driven by AiCatalog — a newly registered model type shows up here
    // automatically.
    oatpp::Object<AiModelTypesDto> getModelTypes() {
        auto dto = AiModelTypesDto::createShared();
        dto->stage1 = oatpp::List<oatpp::String>::createShared();
        dto->stage2 = oatpp::List<oatpp::String>::createShared();
        for (const auto& type : ai::modelTypes()) {
            dto->stage1->push_back(oatpp::String(type.c_str()));
            dto->stage2->push_back(oatpp::String(type.c_str()));
        }
        return dto;
    }

    // Loads every enabled AI job from the database into the AiManager. Called
    // once at startup, after AiManager::start().
    void startAllFromDatabase() {
        auto res = m_db->getEnabledAiJobs();
        assertSuccess(res);
        auto jobs = res->fetch<oatpp::List<oatpp::Object<AiJobDto>>>();
        if (!jobs) return;
        for (const auto& job : *jobs) {
            if (job) syncToManager(job);
        }
    }

private:
    OATPP_COMPONENT(std::shared_ptr<AiJobDb>, m_db);
    OATPP_COMPONENT(std::shared_ptr<CameraDb>, m_cameraDb);
    OATPP_COMPONENT(std::shared_ptr<AiManager>, m_ai);
    OATPP_COMPONENT(oatpp::Object<ConfigDto>, m_config);

    static std::string stdstr(const oatpp::String& v) {
        return v ? std::string(v->c_str()) : std::string();
    }

    // Pushes a job row (and its camera) into the live AiManager. A disabled
    // job is sent through too — AiManager keeps it in the desired set but does
    // not run it.
    void syncToManager(const oatpp::Object<AiJobDto>& job) {
        auto cameraDto = fetchCamera(job->cameraId);
        if (!cameraDto) {
            std::fprintf(stderr, "[ai] job %s references missing camera %s\n",
                         stdstr(job->id).c_str(), stdstr(job->cameraId).c_str());
            return;
        }

        cfg::Camera camera;
        camera.id = stdstr(cameraDto->id);
        camera.name = stdstr(cameraDto->name);
        camera.uri = stdstr(cameraDto->rtsp);
        camera.enabled = true;

        cfg::AiJob aiJob;
        aiJob.jobId = stdstr(job->id);
        aiJob.name = stdstr(job->name);
        aiJob.cameraId = stdstr(job->cameraId);
        // NOTE: oatpp::Boolean::operator bool() returns the *value*, not a
        // null check — so `job->enabled ? *job->enabled : true` wrongly
        // yields true when enabled is false. getValue() reads it correctly.
        aiJob.enabled = job->enabled.getValue(true);
        aiJob.model1Path = stdstr(job->modelPath);
        aiJob.model1Type = stdstr(job->modelType);
        aiJob.classFilter = cfg::parseClassFilter(stdstr(job->classFilter));
        aiJob.model2Path = stdstr(job->modelPath2);
        aiJob.model2Type = stdstr(job->modelType2);
        aiJob.transform = stdstr(job->transformData);
        aiJob.primaryConf = job->primaryConf ? static_cast<float>(*job->primaryConf) : 0.25f;
        aiJob.secondaryConf = job->secondaryConf ? static_cast<float>(*job->secondaryConf) : 0.25f;
        aiJob.maxFps = job->maxFps ? *job->maxFps : 0;

        m_ai->applyJob(camera, aiJob);
    }

    oatpp::Object<CameraDto> fetchCamera(const oatpp::String& cameraId) {
        if (!cameraId || !http::isUuid(cameraId->c_str())) return nullptr;
        auto res = m_cameraDb->getCameraById(cameraId);
        if (!res || !res->isSuccess() || !res->hasMoreToFetch()) return nullptr;
        auto list = res->fetch<oatpp::List<oatpp::Object<CameraDto>>>();
        if (!list || list->size() == 0) return nullptr;
        return list[0];
    }

    void requireCameraExists(const oatpp::String& cameraId) {
        OATPP_ASSERT_HTTP(cameraId && http::isUuid(cameraId->c_str()),
                          Status::CODE_400, "Invalid cameraId");
        OATPP_ASSERT_HTTP(fetchCamera(cameraId), Status::CODE_400,
                          "cameraId does not exist");
    }

    static void validate(const oatpp::Object<CreateAiJobDto>& in, bool requireAll) {
        OATPP_ASSERT_HTTP(in, Status::CODE_400, "Body required");

        if (requireAll) {
            OATPP_ASSERT_HTTP(in->name && in->name->size() > 0,
                              Status::CODE_400, "name required");
            OATPP_ASSERT_HTTP(in->cameraId && in->cameraId->size() > 0,
                              Status::CODE_400, "cameraId required");
            OATPP_ASSERT_HTTP(in->modelPath && in->modelPath->size() > 0,
                              Status::CODE_400, "modelPath required");
        }

        // Model types and transforms are validated against the AiCatalog
        // registry, so adding a new type needs no change here. Models are
        // stage-agnostic: any registered type is valid for either stage.
        if (in->modelType) {
            OATPP_ASSERT_HTTP(ai::isModelType(in->modelType->c_str()),
                              Status::CODE_400,
                              "modelType is not a registered model");
        }
        if (in->modelType2 && in->modelType2->size() > 0) {
            OATPP_ASSERT_HTTP(ai::isModelType(in->modelType2->c_str()),
                              Status::CODE_400,
                              "modelType2 is not a registered model");
        }
        if (in->transformData && in->transformData->size() > 0) {
            OATPP_ASSERT_HTTP(ai::getTransform(in->transformData->c_str()) != nullptr,
                              Status::CODE_400,
                              "transformData is not a registered transform");
        }
        if (in->primaryConf) {
            OATPP_ASSERT_HTTP(*in->primaryConf >= 0.0 && *in->primaryConf <= 1.0,
                              Status::CODE_400, "primaryConf must be 0..1");
        }
        if (in->secondaryConf) {
            OATPP_ASSERT_HTTP(*in->secondaryConf >= 0.0 && *in->secondaryConf <= 1.0,
                              Status::CODE_400, "secondaryConf must be 0..1");
        }
        if (in->maxFps) {
            OATPP_ASSERT_HTTP(*in->maxFps >= 0 && *in->maxFps <= 120,
                              Status::CODE_400, "maxFps must be 0..120");
        }
    }

    static void validateUuid(const oatpp::String& id, const char* message) {
        OATPP_ASSERT_HTTP(id && http::isUuid(id->c_str()), Status::CODE_400,
                          message);
    }

    template <class Res>
    static void assertSuccess(const std::shared_ptr<Res>& res) {
        OATPP_ASSERT_HTTP(res->isSuccess(), Status::CODE_500,
                          res->getErrorMessage()->c_str());
    }

    template <class Res>
    static oatpp::Object<AiJobDto> fetchOne(
        const std::shared_ptr<Res>& res,
        const oatpp::web::protocol::http::Status& notFoundStatus,
        const char* notFoundMsg) {
        OATPP_ASSERT_HTTP(res->hasMoreToFetch(), notFoundStatus, notFoundMsg);
        auto list = res->template fetch<oatpp::List<oatpp::Object<AiJobDto>>>();
        OATPP_ASSERT_HTTP(list && list->size() > 0, notFoundStatus, notFoundMsg);
        return list[0];
    }
};

#endif
