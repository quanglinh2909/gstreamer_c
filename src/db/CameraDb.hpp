#ifndef test_gstreamer_CameraDb_hpp
#define test_gstreamer_CameraDb_hpp

#include "dto/CameraDto.hpp"
#include "dto/RecordingDto.hpp"

#include "oatpp-postgresql/orm.hpp"

#include OATPP_CODEGEN_BEGIN(DbClient)

// NOTE: oatpp-postgresql 1.3.0 doesn't natively deserialize the UUID OID.
// We keep `id` as UUID in the table for storage efficiency / indexing,
// but cast to text on the way out (RETURNING / SELECT) and back to UUID
// on the way in (WHERE).
class CameraDb : public oatpp::orm::DbClient {
public:
    explicit CameraDb(const std::shared_ptr<oatpp::orm::Executor>& executor)
        : oatpp::orm::DbClient(executor) {}

    QUERY(createCamera,
          "INSERT INTO cameras("
          "  name, rtsp, state, input_rtsp, output_rtsp, codec, "
          "  hardware, recording_enabled, recording_mode, motion_enabled, "
          "  motion_sensitivity, motion_threshold, pre_motion_seconds, "
          "  post_motion_seconds, segment_seconds, motion_keyframe_only, "
          "  retry_count, last_error, last_changed_at"
          ") "
          "VALUES ("
          "  :name, :rtsp, 'offline', :rtsp, '', 'unknown', "
          "  COALESCE(:hardware, 'auto'), COALESCE(:recordingEnabled, false), "
          "  COALESCE(:recordingMode, 'off'), COALESCE(:motionEnabled, false), "
          "  COALESCE(:motionSensitivity, 0.5), COALESCE(:motionThreshold, 0.01), "
          "  COALESCE(:preMotionSeconds, 10), COALESCE(:postMotionSeconds, 20), "
          "  COALESCE(:segmentSeconds, 10), COALESCE(:motionKeyframeOnly, false), 0, '', ''"
          ") "
          "RETURNING CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, rtsp),          PARAM(oatpp::String, hardware),
          PARAM(oatpp::Boolean, recordingEnabled),
          PARAM(oatpp::String, recordingMode),
          PARAM(oatpp::Boolean, motionEnabled),
          PARAM(oatpp::Float64, motionSensitivity),
          PARAM(oatpp::Float64, motionThreshold),
          PARAM(oatpp::UInt32, preMotionSeconds),
          PARAM(oatpp::UInt32, postMotionSeconds),
          PARAM(oatpp::UInt32, segmentSeconds),
          PARAM(oatpp::Boolean, motionKeyframeOnly))

    QUERY(getCameraById,
          "SELECT CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\" "
          "FROM cameras WHERE id = CAST(:id AS uuid) LIMIT 1;",
          PARAM(oatpp::String, id))

    QUERY(getAllCameras,
          "SELECT CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\" "
          "FROM cameras ORDER BY id LIMIT :limit OFFSET :offset;",
          PARAM(oatpp::Int64, limit),
          PARAM(oatpp::Int64, offset))

    QUERY(getAllCamerasForStartup,
          "SELECT CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\" "
          "FROM cameras ORDER BY id;")

    QUERY(updateCamera,
          "UPDATE cameras SET "
          "  name              = COALESCE(:name, name), "
          "  rtsp              = COALESCE(:rtsp, rtsp), "
          "  input_rtsp        = COALESCE(:rtsp, input_rtsp), "
          "  hardware          = COALESCE(:hardware, hardware), "
          "  recording_enabled = COALESCE(:recordingEnabled, recording_enabled), "
          "  recording_mode    = COALESCE(:recordingMode, recording_mode), "
          "  motion_enabled    = COALESCE(:motionEnabled, motion_enabled), "
          "  motion_sensitivity = COALESCE(:motionSensitivity, motion_sensitivity), "
          "  motion_threshold  = COALESCE(:motionThreshold, motion_threshold), "
          "  pre_motion_seconds = COALESCE(:preMotionSeconds, pre_motion_seconds), "
          "  post_motion_seconds = COALESCE(:postMotionSeconds, post_motion_seconds), "
          "  segment_seconds   = COALESCE(:segmentSeconds, segment_seconds), "
          "  motion_keyframe_only = COALESCE(:motionKeyframeOnly, motion_keyframe_only) "
          "WHERE id = CAST(:id AS uuid) "
          "RETURNING CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
          PARAM(oatpp::String, id),
          PARAM(oatpp::String, name),
          PARAM(oatpp::String, rtsp),          PARAM(oatpp::String, hardware),
          PARAM(oatpp::Boolean, recordingEnabled),
          PARAM(oatpp::String, recordingMode),
          PARAM(oatpp::Boolean, motionEnabled),
          PARAM(oatpp::Float64, motionSensitivity),
          PARAM(oatpp::Float64, motionThreshold),
          PARAM(oatpp::UInt32, preMotionSeconds),
          PARAM(oatpp::UInt32, postMotionSeconds),
          PARAM(oatpp::UInt32, segmentSeconds),
          PARAM(oatpp::Boolean, motionKeyframeOnly))

    QUERY(updateCameraStreamSnapshot,
          "UPDATE cameras SET "
          "  state             = :state, "
          "  input_rtsp        = :inputRtsp, "
          "  output_rtsp       = :outputRtsp, "
          "  codec             = :codec, "
          "  hardware          = :hardware, "
          "  recording_enabled = :recordingEnabled, "
          "  retry_count       = :retryCount, "
          "  last_error        = :lastError, "
          "  last_changed_at   = :lastChangedAt "
          "WHERE id = CAST(:id AS uuid);",
          PARAM(oatpp::String, id),          PARAM(oatpp::String, state),
          PARAM(oatpp::String, inputRtsp),
          PARAM(oatpp::String, outputRtsp),
          PARAM(oatpp::String, codec),
          PARAM(oatpp::String, hardware),
          PARAM(oatpp::Boolean, recordingEnabled),
          PARAM(oatpp::UInt32, retryCount),
          PARAM(oatpp::String, lastError),
          PARAM(oatpp::String, lastChangedAt))

    QUERY(deleteCamera,
          "DELETE FROM cameras WHERE id = CAST(:id AS uuid);",
          PARAM(oatpp::String, id))

    QUERY(deleteCameraReturning,
          "DELETE FROM cameras WHERE id = CAST(:id AS uuid) "
          "RETURNING CAST(id AS text) AS id, name, rtsp, state, "
          "input_rtsp AS \"inputRtsp\", output_rtsp AS \"outputRtsp\", codec, hardware, "
          "recording_enabled AS \"recordingEnabled\", retry_count AS \"retryCount\", "
          "recording_mode AS \"recordingMode\", motion_enabled AS \"motionEnabled\", "
          "motion_sensitivity AS \"motionSensitivity\", motion_threshold AS \"motionThreshold\", "
          "pre_motion_seconds AS \"preMotionSeconds\", post_motion_seconds AS \"postMotionSeconds\", "
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
          PARAM(oatpp::String, id))

    QUERY(listRecordingSegments,
          "SELECT CAST(id AS text) AS id, CAST(camera_id AS text) AS \"cameraId\", path, "
          "CAST(start_at AS text) AS \"startAt\", CAST(end_at AS text) AS \"endAt\", "
          "duration_ms AS \"durationMs\", codec, container, recording_mode AS \"recordingMode\", "
          "has_motion AS \"hasMotion\", CAST(motion_event_id AS text) AS \"motionEventId\", status "
          "FROM recording_segments "
          "WHERE camera_id = CAST(:cameraId AS uuid) "
          "  AND start_at < CAST(:to AS timestamptz) "
          "  AND end_at > CAST(:from AS timestamptz) "
          "ORDER BY start_at;",
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::String, from),
          PARAM(oatpp::String, to))

    QUERY(insertRecordingSegment,
          "INSERT INTO recording_segments("
          "  camera_id, path, start_at, end_at, duration_ms, codec, container, "
          "  recording_mode, has_motion, status"
          ") VALUES ("
          "  CAST(:cameraId AS uuid), :path, CAST(:startAt AS timestamptz), "
          "  CAST(:endAt AS timestamptz), :durationMs, :codec, :container, "
          "  :recordingMode, :hasMotion, 'complete'"
          ");",
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::String, path),
          PARAM(oatpp::String, startAt),
          PARAM(oatpp::String, endAt),
          PARAM(oatpp::Int32, durationMs),
          PARAM(oatpp::String, codec),
          PARAM(oatpp::String, container),
          PARAM(oatpp::String, recordingMode),
          PARAM(oatpp::Boolean, hasMotion))

    QUERY(seekRecordingSegment,
          "SELECT CAST(id AS text) AS \"segmentId\", CAST(camera_id AS text) AS \"cameraId\", "
          "CAST('/recording-segments/' || CAST(id AS text) || '/file' AS text) AS \"fileUrl\", "
          "CAST(start_at AS text) AS \"segmentStartAt\", CAST(end_at AS text) AS \"segmentEndAt\", "
          "CAST(ROUND(EXTRACT(EPOCH FROM (CAST(:at AS timestamptz) - start_at)) * 1000) AS bigint) AS \"offsetMs\" "
          "FROM recording_segments "
          "WHERE camera_id = CAST(:cameraId AS uuid) "
          "  AND start_at <= CAST(:at AS timestamptz) "
          "  AND end_at > CAST(:at AS timestamptz) "
          "  AND status = 'complete' "
          "ORDER BY start_at DESC LIMIT 1;",
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::String, at))

    QUERY(getRecordingSegmentById,
          "SELECT CAST(id AS text) AS id, CAST(camera_id AS text) AS \"cameraId\", path, "
          "CAST(start_at AS text) AS \"startAt\", CAST(end_at AS text) AS \"endAt\", "
          "duration_ms AS \"durationMs\", codec, container, recording_mode AS \"recordingMode\", "
          "has_motion AS \"hasMotion\", CAST(motion_event_id AS text) AS \"motionEventId\", status "
          "FROM recording_segments WHERE id = CAST(:id AS uuid) LIMIT 1;",
          PARAM(oatpp::String, id))

    QUERY(listMotionEvents,
          "SELECT CAST(id AS text) AS id, CAST(camera_id AS text) AS \"cameraId\", "
          "CAST(start_at AS text) AS \"startAt\", CAST(end_at AS text) AS \"endAt\", "
          "max_score AS \"maxScore\" "
          "FROM motion_events "
          "WHERE camera_id = CAST(:cameraId AS uuid) "
          "  AND start_at < CAST(:to AS timestamptz) "
          "  AND COALESCE(end_at, start_at) > CAST(:from AS timestamptz) "
          "ORDER BY start_at;",
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::String, from),
          PARAM(oatpp::String, to))

    QUERY(insertMotionEvent,
          "INSERT INTO motion_events(camera_id, start_at, end_at, max_score) "
          "VALUES (CAST(:cameraId AS uuid), CAST(:startAt AS timestamptz), "
          "CAST(:endAt AS timestamptz), :maxScore);",
          PARAM(oatpp::String, cameraId),
          PARAM(oatpp::String, startAt),
          PARAM(oatpp::String, endAt),
          PARAM(oatpp::Float64, maxScore))
};

#include OATPP_CODEGEN_END(DbClient)

#endif
