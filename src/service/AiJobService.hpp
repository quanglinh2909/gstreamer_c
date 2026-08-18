#ifndef test_gstreamer_AiJobService_hpp
#define test_gstreamer_AiJobService_hpp

#include "ai/AiCatalog.hpp"
#include "ai/AiManager.hpp"
#include "ai/pipeline/StageRunner.hpp"
#include "config/ConfigDto.hpp"
#include "db/AiJobDb.hpp"
#include "db/CameraDb.hpp"
#include "dto/AiCatalogDto.hpp"
#include "dto/AiJobDto.hpp"
#include "dto/CameraDto.hpp"
#include "dto/StatusDto.hpp"
#include "http/Uuid.hpp"
#include "service/AiStageMapper.hpp"

#include "oatpp/core/macro/component.hpp"
#include "oatpp/web/protocol/http/Http.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
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
                                     in->maxFps, serializeStages(in->stages));
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
        return toApiList(res->fetch<oatpp::List<oatpp::Object<AiJobRowDto>>>());
    }

    // All AI jobs (enabled and disabled) belonging to one camera.
    oatpp::List<oatpp::Object<AiJobDto>>
    getAiJobsByCamera(const oatpp::String& cameraId) {
        validateUuid(cameraId, "Invalid camera id");
        OATPP_ASSERT_HTTP(fetchCamera(cameraId), Status::CODE_404,
                          "Camera not found");
        auto res = m_db->getAiJobsByCamera(cameraId);
        assertSuccess(res);
        return toApiList(res->fetch<oatpp::List<oatpp::Object<AiJobRowDto>>>());
    }

    oatpp::Object<AiJobDto> updateAiJob(const oatpp::String& id,
                                        const oatpp::Object<CreateAiJobDto>& in) {
        validate(in, /* requireAll */ false);
        validateUuid(id, "Invalid AI job id");
        if (in->cameraId) requireCameraExists(in->cameraId);

        auto res = m_db->updateAiJob(id, in->name, in->cameraId, in->enabled,
                                     in->maxFps, serializeStages(in->stages));
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

    // Lists the crop helpers (transforms) a stage can apply to its parent's
    // detection before running. Driven by AiCatalog — a newly registered
    // transform shows up here automatically.
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

    // Supported values for stage.modelType. Driven by AiCatalog — a newly
    // registered model type shows up here automatically.
    oatpp::Object<AiModelTypesDto> getModelTypes() {
        auto dto = AiModelTypesDto::createShared();
        dto->types = oatpp::List<oatpp::String>::createShared();
        for (const auto& type : ai::modelTypes()) {
            dto->types->push_back(oatpp::String(type.c_str()));
        }
        return dto;
    }

    // Loads every enabled AI job from the database into the AiManager. Called
    // once at startup, after AiManager::start().
    void startAllFromDatabase() {
        auto res = m_db->getEnabledAiJobs();
        assertSuccess(res);
        auto jobs = toApiList(res->fetch<oatpp::List<oatpp::Object<AiJobRowDto>>>());
        if (!jobs) return;
        // GIÃN CÁCH như khởi động stream (xem CameraService): mỗi job dựng
        // pipeline giải mã + nạp model RKNN (khởi tạo NPU tốn CPU dồn cục).
        // Bật 14 job liền nhau tạo đỉnh CPU/điện; cách nhau kStaggerMs để trải
        // ra. Chạy trong thread khởi động nền nên sleep không chặn HTTP.
        bool first = true;
        for (const auto& job : *jobs) {
            if (!job) continue;
            if (!first) {
                std::this_thread::sleep_for(std::chrono::milliseconds(kStaggerMs));
            }
            first = false;
            syncToManager(job);
        }
    }

    // Giãn cách nạp job AI lúc boot (chống đỉnh CPU/điện). Nhỏ hơn stream vì AI
    // giờ dùng chung kết nối RTSP (không mở kết nối mới), chỉ tốn dựng decode+NPU.
    static constexpr int kStaggerMs = 400;

private:
    OATPP_COMPONENT(std::shared_ptr<AiJobDb>, m_db);
    OATPP_COMPONENT(std::shared_ptr<CameraDb>, m_cameraDb);
    OATPP_COMPONENT(std::shared_ptr<AiManager>, m_ai);
    OATPP_COMPONENT(oatpp::Object<ConfigDto>, m_config);
    // Cột stages là jsonb, đi qua lớp DB dưới dạng chuỗi; đây là chỗ dịch
    // chuỗi đó thành DTO và ngược lại.
    OATPP_COMPONENT(std::shared_ptr<oatpp::data::mapping::ObjectMapper>, m_mapper);

    static std::string stdstr(const oatpp::String& v) {
        return v ? std::string(v->c_str()) : std::string();
    }

    using StageList = oatpp::List<oatpp::Object<AiStageDto>>;

    // --- jsonb <-> DTO -----------------------------------------------------

    // Rỗng/không gửi => nullptr, để câu UPDATE giữ nguyên cây cũ (COALESCE).
    oatpp::String serializeStages(const StageList& stages) {
        if (!stages) return nullptr;
        return m_mapper->writeToString(stages);
    }

    // JSON hỏng trong DB là lỗi dữ liệu, không phải lỗi người gọi: trả mảng
    // rỗng rồi để StageRunner từ chối job đó, thay vì làm sập cả danh sách.
    StageList parseStages(const oatpp::String& json, const char* what) {
        if (!json || json->size() == 0) return nullptr;
        try {
            return m_mapper->readFromString<StageList>(json);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[ai] %s: stages JSON hong (%s)\n", what,
                         e.what());
            return nullptr;
        }
    }

    oatpp::Object<AiJobDto> toApi(const oatpp::Object<AiJobRowDto>& row) {
        if (!row) return nullptr;
        auto dto = AiJobDto::createShared();
        dto->id = row->id;
        dto->name = row->name;
        dto->cameraId = row->cameraId;
        dto->enabled = row->enabled;
        dto->maxFps = row->maxFps;
        dto->stages = parseStages(row->stages, stdstr(row->id).c_str());
        return dto;
    }

    oatpp::List<oatpp::Object<AiJobDto>>
    toApiList(const oatpp::List<oatpp::Object<AiJobRowDto>>& rows) {
        auto out = oatpp::List<oatpp::Object<AiJobDto>>::createShared();
        if (!rows) return out;
        for (const auto& row : *rows) {
            if (row) out->push_back(toApi(row));
        }
        return out;
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
        aiJob.maxFps = job->maxFps ? *job->maxFps : 0;
        aiJob.stages = ai_stage::toCfg(job->stages);

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
            OATPP_ASSERT_HTTP(in->stages && in->stages->size() > 0,
                              Status::CODE_400, "stages required (it nhat mot tang)");
        }
        if (in->maxFps) {
            OATPP_ASSERT_HTTP(*in->maxFps >= 0 && *in->maxFps <= 120,
                              Status::CODE_400, "maxFps must be 0..120");
        }
        if (in->stages) validateStages(in->stages);
    }

    // Bắt lỗi cấu hình NGAY LÚC LƯU chứ không đợi tới lúc chạy: job sai được
    // nhận vào DB thì tới lần khởi động sau nó lặng lẽ không lên, và người dùng
    // chỉ thấy "AI không chạy" mà không biết vì sao.
    //
    // Loại model và transform đối chiếu với AiCatalog nên đăng ký thêm loại mới
    // là tự hợp lệ, không phải sửa ở đây.
    static void validateStages(const StageList& stages) {
        OATPP_ASSERT_HTTP(stages->size() > 0, Status::CODE_400,
                          "stages rong");
        OATPP_ASSERT_HTTP(stages->size() <= StageRunner::kMaxStages,
                          Status::CODE_400, "qua nhieu tang");

        int index = 0;
        for (const auto& s : *stages) {
            const std::string at = "stage " + std::to_string(index) + ": ";
            OATPP_ASSERT_HTTP(s, Status::CODE_400, (at + "tang rong").c_str());
            OATPP_ASSERT_HTTP(s->modelPath && s->modelPath->size() > 0,
                              Status::CODE_400, (at + "thieu modelPath").c_str());
            OATPP_ASSERT_HTTP(s->modelType && ai::isModelType(s->modelType->c_str()),
                              Status::CODE_400,
                              (at + "modelType chua duoc dang ky").c_str());
            if (s->transform && s->transform->size() > 0) {
                OATPP_ASSERT_HTTP(ai::getTransform(s->transform->c_str()) != nullptr,
                                  Status::CODE_400,
                                  (at + "transform chua duoc dang ky").c_str());
            }
            if (s->conf != nullptr) {
                OATPP_ASSERT_HTTP(*s->conf >= 0.0 && *s->conf <= 1.0,
                                  Status::CODE_400, (at + "conf phai 0..1").c_str());
            }
            // Cha phải là tầng ĐỨNG TRƯỚC — vừa cấm vòng lặp, vừa bảo đảm khi
            // chạy tới tầng này thì tầng cha đã có kết quả (StageRunner kiểm
            // lại y hệt; ở đây chỉ để báo lỗi sớm và rõ ràng cho người gọi).
            if (index == 0) {
                OATPP_ASSERT_HTTP(s->parent == nullptr || *s->parent < 0,
                                  Status::CODE_400,
                                  "stage 0 phai chay tren khung (parent = -1)");
            } else if (s->parent != nullptr) {
                OATPP_ASSERT_HTTP(*s->parent >= 0 && *s->parent < index,
                                  Status::CODE_400,
                                  (at + "parent phai tro toi mot tang dung truoc")
                                      .c_str());
            }
            ++index;
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
    oatpp::Object<AiJobDto> fetchOne(
        const std::shared_ptr<Res>& res,
        const oatpp::web::protocol::http::Status& notFoundStatus,
        const char* notFoundMsg) {
        OATPP_ASSERT_HTTP(res->hasMoreToFetch(), notFoundStatus, notFoundMsg);
        auto list = res->template fetch<oatpp::List<oatpp::Object<AiJobRowDto>>>();
        OATPP_ASSERT_HTTP(list && list->size() > 0, notFoundStatus, notFoundMsg);
        return toApi(list[0]);
    }
};

#endif
