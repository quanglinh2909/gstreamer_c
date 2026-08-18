-- Toan bo schema cua engine C++ (cameras + recording_segments + motion_events
-- + ai_jobs) trong MOT file. Truoc day tach 001/002 theo kieu migration danh so,
-- nhung khong co gi chay chung theo thu tu ca: engine KHONG tu chay file nay,
-- nguoi cai dat phai psql -f bang tay. Hai file danh so ma chi chay tay thi chi
-- tao co hoi chay thieu mot file.
--
-- CHAY LAI BAO NHIEU LAN CUNG DUOC: moi cau deu la CREATE ... IF NOT EXISTS
-- hoac ALTER ... ADD COLUMN IF NOT EXISTS, nen dung duoc ca cho may moi lan may
-- dang chay du lieu that.
--
--   psql "postgresql://oryza:Oryza%40123@localhost:5433/parking" -f sql/init.sql
--
-- THU TU QUAN TRONG: ai_jobs co khoa ngoai toi cameras(id) nen phai nam SAU
-- phan cameras -- dung dao nguoc hai khoi.

CREATE EXTENSION IF NOT EXISTS pgcrypto;

CREATE TABLE IF NOT EXISTS cameras (
  id                UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name              VARCHAR(128) NOT NULL,
  rtsp              VARCHAR(512) NOT NULL,
  state             VARCHAR(32)  NOT NULL DEFAULT 'offline',
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
-- 'status' was redundant (always derivable from 'state'); drop it.
DROP INDEX IF EXISTS idx_cameras_status;
ALTER TABLE IF EXISTS cameras DROP COLUMN IF EXISTS status;

ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS state VARCHAR(32) NOT NULL DEFAULT 'offline';
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

-- 'state' is now a coarse, user-facing value: online | offline | error.
-- Ensure existing columns carry the new default and normalize any rows that
-- still hold the old detailed runtime states.
ALTER TABLE IF EXISTS cameras ALTER COLUMN state SET DEFAULT 'offline';
UPDATE cameras SET state = CASE
    WHEN state = 'running'                                  THEN 'online'
    WHEN state IN ('auth_error', 'unsupported_codec', 'error') THEN 'error'
    ELSE 'offline'
  END
  WHERE state NOT IN ('online', 'offline', 'error');

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
  -- Mốc (epoch ms) lúc PHIÊN ghi bắt đầu. Mỗi lần engine/pipeline khởi động lại
  -- là một phiên mới với PTS reset (mpegtsmux luôn bắt đầu ~3600s); playlist
  -- builder chèn EXT-X-DISCONTINUITY khi session_start đổi giữa hai đoạn kề —
  -- khoảng-trống-wall-clock nhỏ không phát hiện được PTS reset, cột này thì có.
  session_start   BIGINT      NOT NULL DEFAULT 0,
  created_at      TIMESTAMPTZ NOT NULL DEFAULT now()
);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_time
  ON recording_segments(camera_id, start_at, end_at);
CREATE INDEX IF NOT EXISTS idx_recording_segments_camera_motion_time
  ON recording_segments(camera_id, has_motion, start_at);
CREATE INDEX IF NOT EXISTS idx_recording_segments_motion_event
  ON recording_segments(motion_event_id);
-- path là duy nhất cho mỗi file segment. Unique index này cho phép UPSERT theo
-- path: chèn hàng 'recording' lúc mở đoạn rồi finalize thành 'complete' lúc
-- đóng (live-edge của timeline), hoặc chèn thẳng 'complete' cho chế độ motion.
CREATE UNIQUE INDEX IF NOT EXISTS idx_recording_segments_path
  ON recording_segments(path);

-- Lưới phát hiện chuyển động theo ô (kiểu đầu ghi): mỗi ô một mức 0-9.
--   0      = bỏ qua ô (đưa vào motionmaskcellspos)
--   1..9   = mức, 5 trung tính; càng lớn càng phải động nhiều mới tính
-- motion_cell_levels là chuỗi CHỮ SỐ dài đúng grid_x*grid_y, đọc theo hàng.
-- Rỗng = mọi ô ở mức 5.
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_grid_x SMALLINT NOT NULL DEFAULT 32;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_grid_y SMALLINT NOT NULL DEFAULT 32;
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_cell_levels TEXT NOT NULL DEFAULT '';
-- ADD COLUMN IF NOT EXISTS ở trên KHÔNG đụng tới cột đã tồn tại, nên máy nào
-- chạy bản trước (mặc định 10) vẫn giữ 10. Đặt lại tường minh cho khớp.
ALTER TABLE IF EXISTS cameras ALTER COLUMN motion_grid_x SET DEFAULT 32;
ALTER TABLE IF EXISTS cameras ALTER COLUMN motion_grid_y SET DEFAULT 32;

-- VÙNG chuyển động: JSON [{"r1","c1","r2","c2","level"}]. Thay cho
-- motion_cell_levels (một chữ số mỗi ô): cách cũ gom ô cùng mức thành một
-- motioncells kèm mặt nạ, mà motioncells chỉ đọc 255 ô đầu của mặt nạ (đã đo)
-- nên lưới lớn là dò sai trong im lặng. Giờ engine tự xét vùng, không cần mặt nạ.
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_zones TEXT NOT NULL DEFAULT '';
-- Có ghi sự kiện chuyển động xuống DB không. Tắt = chỉ bắn WebSocket để vẽ
-- live, giống nhận diện khẩu trang (loại đó vốn không có bảng nào). Camera
-- ngoài trời nhiều cây cối sinh hàng nghìn sự kiện/ngày mà chẳng ai xem lại.
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_save_events BOOLEAN NOT NULL DEFAULT true;

-- Ô nào đã động trong một sự kiện — để vẽ lại và tìm kiếm theo vùng.
-- cells: danh sách "hàng:cột" ngăn bằng dấu phẩy, gộp cả sự kiện.
ALTER TABLE IF EXISTS motion_events ADD COLUMN IF NOT EXISTS cells TEXT NOT NULL DEFAULT '';
ALTER TABLE IF EXISTS motion_events ADD COLUMN IF NOT EXISTS grid_x SMALLINT NOT NULL DEFAULT 0;
ALTER TABLE IF EXISTS motion_events ADD COLUMN IF NOT EXISTS grid_y SMALLINT NOT NULL DEFAULT 0;

-- Khung hình lúc sự kiện BẮT ĐẦU, đường dẫn tương đối thư mục engine
-- ("motion-snapshots/<camera>/<ngày>/<epoch_ms>.jpg"). Rỗng = không có ảnh.
--
-- Trước đây thẻ sự kiện chuyển động mượn khung từ endpoint thumbnail của bản
-- ghi: camera không bật ghi thì chẳng có gì để trích, mà sự kiện vừa xảy ra
-- thì đoạn chứa nó còn đang ghi dở nên trả 404. Giờ sự kiện có ảnh của chính
-- nó, như ba loại sự kiện AI kia.
ALTER TABLE IF EXISTS motion_events ADD COLUMN IF NOT EXISTS image_path TEXT NOT NULL DEFAULT '';

-- Hạn lưu theo NGÀY của riêng camera này; 0 = không giới hạn. ENGINE KHÔNG ĐỌC
-- CỘT NÀY — bộ dọn dung lượng bên Python đọc (storage_cleanup_service.py).
--
-- Cột nằm ở đây thay vì một bảng riêng để camera bị xoá thì hạn của nó đi theo,
-- và để hạn vẫn có hiệu lực khi camera đã TẮT GHI HÌNH hoặc mất kết nối: lượt
-- dọn chỉ đọc DB, không cần luồng nào đang chạy.
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS retention_days INTEGER NOT NULL DEFAULT 0;

-- ===========================================================================
-- AI jobs (truoc day o file 002_init_ai_jobs.sql)
-- ===========================================================================

-- AI jobs: MOT CAY MODEL cho moi dong. Mot camera co nhieu job. Xoa camera thi
-- xoa luon job cua no.
--
-- `stages` la mang JSON theo thu tu chay, phan tu 0 chay tren ca khung hinh,
-- moi phan tu sau tro ve mot tang DUNG TRUOC qua `parent`:
--
--   [{"modelPath":"weights/yolov8.rknn","modelType":"yolov8_detect",
--     "classFilter":"2,3,5,7","conf":0.25},
--    {"parent":0,"modelPath":"weights/plate_det.rknn",
--     "modelType":"paddle_ocr_det","inputClasses":"7","conf":0.3},
--    {"parent":1,"modelPath":"weights/plate_rec.rknn",
--     "modelType":"paddle_ocr_rec","conf":0.3}]
--
-- VI SAO JSONB CHU KHONG PHAI BANG RIENG: engine luon nap CA CAY mot luot va
-- chi so `parent` chi co nghia trong pham vi ca mang, nen vá le tung tang la vo
-- nghia. Mot dong = mot cay, doc-ghi nguyen khoi.
--
-- Cỡ dau vao cua tung tang KHONG nam o day: engine doc thang tu file .rknn.

CREATE TABLE IF NOT EXISTS ai_jobs (
  id              UUID         PRIMARY KEY DEFAULT gen_random_uuid(),
  name            VARCHAR(128) NOT NULL,
  camera_id       UUID         NOT NULL REFERENCES cameras(id) ON DELETE CASCADE,
  enabled         BOOLEAN      NOT NULL DEFAULT true,
  max_fps         INTEGER      NOT NULL DEFAULT 0,
  stages          JSONB        NOT NULL DEFAULT '[]'::jsonb,
  created_at      TIMESTAMPTZ  NOT NULL DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_ai_jobs_camera ON ai_jobs(camera_id);
CREATE INDEX IF NOT EXISTS idx_ai_jobs_enabled ON ai_jobs(enabled);

-- Chuyen DB cu (mot model_path + mot model_path_2 tuy chon) sang `stages`.
-- Idempotent va tu bo qua khi bang da o dang moi: dieu kien la con cot cu.
-- Chay TRUOC khi DROP COLUMN ben duoi, va chi dung cho dong stages con rong.
ALTER TABLE IF EXISTS ai_jobs ADD COLUMN IF NOT EXISTS stages JSONB NOT NULL DEFAULT '[]'::jsonb;

DO $$
BEGIN
  IF EXISTS (SELECT 1 FROM information_schema.columns
             WHERE table_name = 'ai_jobs' AND column_name = 'model_path') THEN
    EXECUTE $mig$
      UPDATE ai_jobs SET stages =
        CASE WHEN COALESCE(model_path_2, '') = '' THEN
          jsonb_build_array(jsonb_build_object(
            'modelPath', model_path, 'modelType', model_type,
            'classFilter', class_filter, 'conf', primary_conf, 'parent', -1))
        ELSE
          jsonb_build_array(
            jsonb_build_object(
              'modelPath', model_path, 'modelType', model_type,
              'classFilter', class_filter, 'conf', primary_conf, 'parent', -1),
            jsonb_build_object(
              'modelPath', model_path_2, 'modelType', model_type_2,
              'transform', transform_data, 'conf', secondary_conf, 'parent', 0))
        END
      WHERE stages = '[]'::jsonb
    $mig$;
  END IF;
END $$;

ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS model_path;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS model_type;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS class_filter;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS model_path_2;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS model_type_2;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS transform_data;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS primary_conf;
ALTER TABLE IF EXISTS ai_jobs DROP COLUMN IF EXISTS secondary_conf;
