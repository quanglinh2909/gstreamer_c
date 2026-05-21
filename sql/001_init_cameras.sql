CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE IF NOT EXISTS cameras (
  id                UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name              VARCHAR(128) NOT NULL,
  rtsp              VARCHAR(512) NOT NULL,
  status            VARCHAR(16)  NOT NULL DEFAULT 'offline',
  state             VARCHAR(32)  NOT NULL DEFAULT 'stopped',
  input_rtsp        VARCHAR(512) NOT NULL DEFAULT '',
  output_rtsp       VARCHAR(512) NOT NULL DEFAULT '',
  codec             VARCHAR(16)  NOT NULL DEFAULT 'unknown',
  hardware          VARCHAR(32)  NOT NULL DEFAULT 'auto',
  recording_enabled BOOLEAN      NOT NULL DEFAULT false,
  recording_mode    VARCHAR(16)  NOT NULL DEFAULT 'off',
  motion_enabled    BOOLEAN      NOT NULL DEFAULT false,
  motion_sensitivity DOUBLE PRECISION NOT NULL DEFAULT 0.5,
  motion_threshold  DOUBLE PRECISION NOT NULL DEFAULT 0.01,
  pre_motion_seconds INTEGER     NOT NULL DEFAULT 10,
  post_motion_seconds INTEGER    NOT NULL DEFAULT 20,
  segment_seconds   INTEGER      NOT NULL DEFAULT 10,
  motion_keyframe_only BOOLEAN   NOT NULL DEFAULT false,
  retry_count       INTEGER      NOT NULL DEFAULT 0,
  last_error        TEXT         NOT NULL DEFAULT '',
  last_changed_at   VARCHAR(32)  NOT NULL DEFAULT ''
);
CREATE INDEX IF NOT EXISTS idx_cameras_status ON cameras(status);

ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS state VARCHAR(32) NOT NULL DEFAULT 'stopped';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS input_rtsp VARCHAR(512) NOT NULL DEFAULT '';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS output_rtsp VARCHAR(512) NOT NULL DEFAULT '';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS codec VARCHAR(16) NOT NULL DEFAULT 'unknown';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS hardware VARCHAR(32) NOT NULL DEFAULT 'auto';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS recording_enabled BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS recording_mode VARCHAR(16) NOT NULL DEFAULT 'off';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_enabled BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_sensitivity DOUBLE PRECISION NOT NULL DEFAULT 0.5;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_threshold DOUBLE PRECISION NOT NULL DEFAULT 0.01;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS pre_motion_seconds INTEGER NOT NULL DEFAULT 10;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS post_motion_seconds INTEGER NOT NULL DEFAULT 20;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS segment_seconds INTEGER NOT NULL DEFAULT 10;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_keyframe_only BOOLEAN NOT NULL DEFAULT false;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS retry_count INTEGER NOT NULL DEFAULT 0;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS last_error TEXT NOT NULL DEFAULT '';
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS last_changed_at VARCHAR(32) NOT NULL DEFAULT '';

UPDATE cameras SET input_rtsp = rtsp WHERE input_rtsp = '';

CREATE TABLE IF NOT EXISTS motion_events (
  id          UUID             PRIMARY KEY DEFAULT gen_random_uuid(),
  camera_id   UUID             NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  start_at    TIMESTAMPTZ      NOT NULL,
  end_at      TIMESTAMPTZ,
  max_score   DOUBLE PRECISION NOT NULL DEFAULT 0,
  created_at  TIMESTAMPTZ      NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_motion_events_camera_time
  ON motion_events(camera_id, start_at, end_at);

CREATE TABLE IF NOT EXISTS recording_segments (
  id              UUID        PRIMARY KEY DEFAULT gen_random_uuid(),
  camera_id       UUID        NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  path            TEXT        NOT NULL,
  start_at        TIMESTAMPTZ NOT NULL,
  end_at          TIMESTAMPTZ NOT NULL,
  duration_ms     INTEGER     NOT NULL,
  codec           VARCHAR(16) NOT NULL,
  container       VARCHAR(16) NOT NULL,
  recording_mode  VARCHAR(16) NOT NULL,
  has_motion      BOOLEAN     NOT NULL DEFAULT false,
  motion_event_id UUID REFERENCES motion_events(id) ON DELETE SET NULL,
  status          VARCHAR(16) NOT NULL DEFAULT 'complete',
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_time
  ON recording_segments(camera_id, start_at, end_at);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_motion_time
  ON recording_segments(camera_id, has_motion, start_at);
CREATE INDEX IF NOT EXISTS idx_recording_segments_motion_event
  ON recording_segments(motion_event_id);
