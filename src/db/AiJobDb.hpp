#ifndef test_gstreamer_AiJobDb_hpp
#define test_gstreamer_AiJobDb_hpp

#include "dto/AiJobDto.hpp"

#include "oatpp-postgresql/orm.hpp"

#include OATPP_CODEGEN_BEGIN(DbClient)

// UUID columns (id, camera_id) are cast to text on the way out and back to
// uuid on the way in, mirroring CameraDb. Cột `stages` (jsonb) đi theo ĐÚNG
// cách đó: ra thì `CAST(... AS text)`, vào thì `CAST(:stages AS jsonb)` —
// oatpp-postgresql không ánh xạ jsonb sang DTO lồng nhau, còn Postgres thì vẫn
// kiểm tra cú pháp JSON giúp. Mọi truy vấn trả về AiJobRowDto.
class AiJobDb : public oatpp::orm::DbClient {
public:
    explicit AiJobDb(const std::shared_ptr<oatpp::orm::Executor>& executor)
        : oatpp::orm::DbClient(executor) {}

    // Danh sách cột trả về, viết một lần cho mọi truy vấn.
#define AI_JOB_COLUMNS                                     \
    "CAST(id AS text) AS id, name, "                       \
    "CAST(camera_id AS text) AS \"cameraId\", enabled, "   \
    "max_fps AS \"maxFps\", CAST(stages AS text) AS stages"

    // ĐỪNG viết `'[]'::jsonb`: oatpp quét dấu hai chấm để tìm tham số, nên
    // `::jsonb` bị hiểu thành tham số tên `jsonb` và câu lệnh chết ngay lúc
    // chạy với "Parameter not found". Luôn dùng CAST(... AS ...).
    QUERY(createAiJob,
          "INSERT INTO ai_jobs(name, camera_id, enabled, max_fps, stages) "
          "VALUES (:name, CAST(:cameraId AS uuid), COALESCE(:enabled, true), "
          "        COALESCE(:maxFps, 0), "
          "        COALESCE(CAST(:stages AS jsonb), CAST('[]' AS jsonb))) "
          "RETURNING " AI_JOB_COLUMNS ";",
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::Boolean, enabled),
          PARAM(oatpp::Int32, maxFps),
          PARAM(oatpp::String, stages))

    QUERY(getAiJobById,
          "SELECT " AI_JOB_COLUMNS
          " FROM ai_jobs WHERE id = CAST(:id AS uuid) LIMIT 1;",
          PARAM(oatpp::String, id))

    QUERY(getAllAiJobs,
          "SELECT " AI_JOB_COLUMNS
          " FROM ai_jobs ORDER BY id LIMIT :limit OFFSET :offset;",
          PARAM(oatpp::Int64, limit),
          PARAM(oatpp::Int64, offset))

    QUERY(getEnabledAiJobs,
          "SELECT " AI_JOB_COLUMNS
          " FROM ai_jobs WHERE enabled = true ORDER BY id;")

    QUERY(getAiJobsByCamera,
          "SELECT " AI_JOB_COLUMNS
          " FROM ai_jobs WHERE camera_id = CAST(:cameraId AS uuid) ORDER BY id;",
          PARAM(oatpp::String, cameraId))

    // stages đi NGUYÊN KHỐI: gửi lên là thay cả cây, không gửi thì giữ nguyên
    // cây cũ. Vá lẻ từng tầng qua SQL là vô nghĩa vì chỉ số parent của các tầng
    // sau phụ thuộc vào cả mảng.
    QUERY(updateAiJob,
          "UPDATE ai_jobs SET "
          "  name      = COALESCE(:name, name), "
          "  camera_id = COALESCE(CAST(:cameraId AS uuid), camera_id), "
          "  enabled   = COALESCE(:enabled, enabled), "
          "  max_fps   = COALESCE(:maxFps, max_fps), "
          "  stages    = COALESCE(CAST(:stages AS jsonb), stages) "
          "WHERE id = CAST(:id AS uuid) "
          "RETURNING " AI_JOB_COLUMNS ";",
          PARAM(oatpp::String, id),
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::Boolean, enabled),
          PARAM(oatpp::Int32, maxFps),
          PARAM(oatpp::String, stages))

    QUERY(setAiJobEnabled,
          "UPDATE ai_jobs SET enabled = :enabled "
          "WHERE id = CAST(:id AS uuid) "
          "RETURNING " AI_JOB_COLUMNS ";",
          PARAM(oatpp::String, id),
          PARAM(oatpp::Boolean, enabled))

    QUERY(deleteAiJobReturning,
          "DELETE FROM ai_jobs WHERE id = CAST(:id AS uuid) "
          "RETURNING " AI_JOB_COLUMNS ";",
          PARAM(oatpp::String, id))

#undef AI_JOB_COLUMNS
};

#include OATPP_CODEGEN_END(DbClient)

#endif
