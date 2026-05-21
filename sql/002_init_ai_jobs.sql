-- AI jobs: one detector (optionally cascaded into a model-2 stage) per row.
-- A camera can have many AI jobs. Deleting a camera removes its AI jobs.

CREATE TABLE IF NOT EXISTS ai_jobs (
  id              UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name            VARCHAR(128) NOT NULL,
  camera_id       UUID         NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  enabled         BOOLEAN      NOT NULL DEFAULT true,
  model_path      VARCHAR(512) NOT NULL,
  model_type      VARCHAR(32)  NOT NULL DEFAULT 'yolov8_detect',
  class_filter    VARCHAR(256) NOT NULL DEFAULT 'all',
  model_path_2    VARCHAR(512) NOT NULL DEFAULT '',
  model_type_2    VARCHAR(32)  NOT NULL DEFAULT '',
  transform_data  VARCHAR(32)  NOT NULL DEFAULT '',
  primary_conf    DOUBLE PRECISION NOT NULL DEFAULT 0.25,
  secondary_conf  DOUBLE PRECISION NOT NULL DEFAULT 0.25,
  max_fps         INTEGER      NOT NULL DEFAULT 0,
  created_at      TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_ai_jobs_camera ON ai_jobs(camera_id);
CREATE INDEX IF NOT EXISTS idx_ai_jobs_enabled ON ai_jobs(enabled);
