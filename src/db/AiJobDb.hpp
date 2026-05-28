#ifndef test_gstreamer_AiJobDb_hpp
#define test_gstreamer_AiJobDb_hpp

#include "dto/AiJobDto.hpp"

#include "oatpp-postgresql/orm.hpp"

#include OATPP_CODEGEN_BEGIN(DbClient)

// UUID columns (id, camera_id) are cast to text on the way out and back to
// uuid on the way in, mirroring CameraDb.
class AiJobDb : public oatpp::orm::DbClient {
public:
    explicit AiJobDb(const std::shared_ptr<oatpp::orm::Executor>& executor)
        : oatpp::orm::DbClient(executor) {}

    QUERY(createAiJob,
          "INSERT INTO ai_jobs("
          "  name, camera_id, enabled, model_path, model_type, class_filter, "
          "  model_path_2, model_type_2, transform_data, primary_conf, "
          "  secondary_conf, max_fps"
          ") VALUES ("
          "  :name, CAST(:cameraId AS uuid), COALESCE(:enabled, true), "
          "  :modelPath, COALESCE(:modelType, 'yolov8_detect'), "
          "  COALESCE(:classFilter, 'all'), COALESCE(:modelPath2, ''), "
          "  COALESCE(:modelType2, ''), COALESCE(:transformData, ''), "
          "  COALESCE(:primaryConf, 0.25), COALESCE(:secondaryConf, 0.25), "
          "  COALESCE(:maxFps, 0)"
          ") "
          "RETURNING CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\";",
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::Boolean, enabled),
          PARAM(oatpp::String, modelPath),
          PARAM(oatpp::String, modelType),
          PARAM(oatpp::String, classFilter),
          PARAM(oatpp::String, modelPath2),
          PARAM(oatpp::String, modelType2),
          PARAM(oatpp::String, transformData),
          PARAM(oatpp::Float64, primaryConf),
          PARAM(oatpp::Float64, secondaryConf),
          PARAM(oatpp::Int32, maxFps))

    QUERY(getAiJobById,
          "SELECT CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\" "
          "FROM ai_jobs WHERE id = CAST(:id AS uuid) LIMIT 1;",
          PARAM(oatpp::String, id))

    QUERY(getAllAiJobs,
          "SELECT CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\" "
          "FROM ai_jobs ORDER BY id LIMIT :limit OFFSET :offset;",
          PARAM(oatpp::Int64, limit),
          PARAM(oatpp::Int64, offset))

    QUERY(getEnabledAiJobs,
          "SELECT CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\" "
          "FROM ai_jobs WHERE enabled = true ORDER BY id;")

    QUERY(getAiJobsByCamera,
          "SELECT CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\" "
          "FROM ai_jobs WHERE camera_id = CAST(:cameraId AS uuid) ORDER BY id;",
          PARAM(oatpp::String, cameraId))

    QUERY(updateAiJob,
          "UPDATE ai_jobs SET "
          "  name           = COALESCE(:name, name), "
          "  camera_id      = COALESCE(CAST(:cameraId AS uuid), camera_id), "
          "  enabled        = COALESCE(:enabled, enabled), "
          "  model_path     = COALESCE(:modelPath, model_path), "
          "  model_type     = COALESCE(:modelType, model_type), "
          "  class_filter   = COALESCE(:classFilter, class_filter), "
          "  model_path_2   = COALESCE(:modelPath2, model_path_2), "
          "  model_type_2   = COALESCE(:modelType2, model_type_2), "
          "  transform_data = COALESCE(:transformData, transform_data), "
          "  primary_conf   = COALESCE(:primaryConf, primary_conf), "
          "  secondary_conf = COALESCE(:secondaryConf, secondary_conf), "
          "  max_fps        = COALESCE(:maxFps, max_fps) "
          "WHERE id = CAST(:id AS uuid) "
          "RETURNING CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\";",
          PARAM(oatpp::String, id),
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::Boolean, enabled),
          PARAM(oatpp::String, modelPath),
          PARAM(oatpp::String, modelType),
          PARAM(oatpp::String, classFilter),
          PARAM(oatpp::String, modelPath2),
          PARAM(oatpp::String, modelType2),
          PARAM(oatpp::String, transformData),
          PARAM(oatpp::Float64, primaryConf),
          PARAM(oatpp::Float64, secondaryConf),
          PARAM(oatpp::Int32, maxFps))

    QUERY(setAiJobEnabled,
          "UPDATE ai_jobs SET enabled = :enabled "
          "WHERE id = CAST(:id AS uuid) "
          "RETURNING CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\";",
          PARAM(oatpp::String, id),
          PARAM(oatpp::Boolean, enabled))

    QUERY(deleteAiJobReturning,
          "DELETE FROM ai_jobs WHERE id = CAST(:id AS uuid) "
          "RETURNING CAST(id AS text) AS id, name, "
          "CAST(camera_id AS text) AS \"cameraId\", enabled, "
          "model_path AS \"modelPath\", model_type AS \"modelType\", "
          "class_filter AS \"classFilter\", model_path_2 AS \"modelPath2\", "
          "model_type_2 AS \"modelType2\", transform_data AS \"transformData\", "
          "primary_conf AS \"primaryConf\", secondary_conf AS \"secondaryConf\", "
          "max_fps AS \"maxFps\";",
          PARAM(oatpp::String, id))
};

#include OATPP_CODEGEN_END(DbClient)

#endif
