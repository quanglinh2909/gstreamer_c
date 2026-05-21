# Motion Recording and Playback Design

**Date:** 2026-05-20
**Project:** `test_gstreamer`
**Goal:** Add motion-triggered recording, continuous recording, and timestamp-based playback without slowing the live RTSP restream path.

## 1. Scope

Add per-camera recording with three modes:

- `off`: no recording files are retained.
- `always`: all recording segments are retained and indexed.
- `motion`: the system keeps only segments around detected motion events.

Add playback APIs that let clients list recordings by time range, seek to a specific timestamp, and download/play the selected segment over HTTP.

Out of scope for the first implementation:

- Browser video UI.
- RTSP VOD playback.
- Audio recording.
- Cloud storage.
- Multi-node storage.

## 2. Chosen Approach

Use a segment ring buffer plus a motion index.

Each camera session keeps the live RTSP branch passthrough-only, and adds isolated recording and motion branches behind a `tee`.

```text
rtspsrc
  -> depay
  -> parse
  -> tee name=video_t

video_t. -> queue
        -> rtph264pay/rtph265pay name=pay0

video_t. -> queue
        -> splitmuxsink
        -> short files, e.g. 5s or 10s

video_t. -> queue leaky=downstream
        -> decoder
        -> videoscale
        -> videoconvert
        -> video/x-raw,format=RGB,width=<small>
        -> motioncells
        -> fakesink
```

Only the motion branch decodes video. The live RTSP branch and recording branch stay on encoded H264/H265 data.

This avoids the common failure mode where motion starts recording too late and misses the first seconds of movement. The application always has recent short segments available, then decides whether to retain or delete them based on the camera recording mode and motion events.

## 3. Recording Behavior

### Continuous Recording

For `always`, every completed segment is moved from temporary ring-buffer state to retained state and inserted into the segment index.

### Motion Recording

For `motion`, segment files are still produced continuously, but retention is event-driven:

- Keep segments that overlap a motion event.
- Also keep pre-motion segments, default `10s`.
- Also keep post-motion segments, default `20s`.
- Delete unneeded temp segments after the post-motion window closes.

Motion starts when `motioncells` posts a motion bus message. Motion ends when `motioncells` posts its motion-finished message or no motion has been seen for the configured gap.

### Segment Length

Default segment length: `10s`.

Shorter segments make seeking and motion retention more precise, but create more files and database rows. Longer segments reduce file count, but increase seek granularity and retained bytes around motion.

## 4. Storage Layout

Use one directory tree under `recordingDir`.

```text
recordings/
  <camera-id>/
    tmp/
      2026/05/20/14/30/00-000001.mp4
    retained/
      2026/05/20/14/30/00-000001.mp4
```

Files should be named by UTC segment start time plus a monotonic fragment id to avoid collisions.

MP4 is preferred for browser/client compatibility. If MP4 finalization proves fragile for interrupted processes, Matroska can be used as a fallback because it is more forgiving for live segment files.

## 5. Database

Extend `cameras`:

```text
recording_mode VARCHAR(16) NOT NULL DEFAULT 'off'
motion_enabled BOOLEAN NOT NULL DEFAULT false
motion_sensitivity DOUBLE PRECISION NOT NULL DEFAULT 0.5
motion_threshold DOUBLE PRECISION NOT NULL DEFAULT 0.01
pre_motion_seconds INTEGER NOT NULL DEFAULT 10
post_motion_seconds INTEGER NOT NULL DEFAULT 20
segment_seconds INTEGER NOT NULL DEFAULT 10
```

Add `recording_segments`:

```text
id UUID PRIMARY KEY
camera_id UUID NOT NULL
path TEXT NOT NULL
start_at TIMESTAMPTZ NOT NULL
end_at TIMESTAMPTZ NOT NULL
duration_ms INTEGER NOT NULL
codec VARCHAR(16) NOT NULL
container VARCHAR(16) NOT NULL
recording_mode VARCHAR(16) NOT NULL
has_motion BOOLEAN NOT NULL DEFAULT false
motion_event_id UUID NULL
status VARCHAR(16) NOT NULL DEFAULT 'complete'
created_at TIMESTAMPTZ NOT NULL DEFAULT now()
```

Indexes:

```text
(camera_id, start_at, end_at)
(camera_id, has_motion, start_at)
(motion_event_id)
```

Add `motion_events`:

```text
id UUID PRIMARY KEY
camera_id UUID NOT NULL
start_at TIMESTAMPTZ NOT NULL
end_at TIMESTAMPTZ NULL
max_score DOUBLE PRECISION NOT NULL DEFAULT 0
created_at TIMESTAMPTZ NOT NULL DEFAULT now()
```

Indexes:

```text
(camera_id, start_at, end_at)
```

## 6. Runtime Components

### `RecordingPolicy`

Pure helper functions for:

- Deciding whether a completed segment should be retained.
- Mapping motion event windows to segment retention windows.
- Computing segment paths.
- Computing playback seek offsets.

This helper should be covered by unit tests first.

### `RecordingIndexService`

Owns database writes for segments and motion events:

- Insert segment when `splitmuxsink` finalizes a file.
- Mark segment retained/deleted.
- Insert and close motion events.
- Query segments by time range.
- Find the segment for a seek timestamp.

### `CameraStreamSession`

Owns the GStreamer pipeline and emits runtime events:

- `onSegmentClosed(cameraId, path, startAt, endAt, codec, container)`
- `onMotionStarted(cameraId, timestamp, score)`
- `onMotionUpdated(cameraId, timestamp, score)`
- `onMotionStopped(cameraId, timestamp)`

The session should not own database logic directly.

### `PlaybackService`

Uses `RecordingIndexService` to implement HTTP playback:

- List segments for a time range.
- Seek to timestamp.
- Return file responses or metadata needed by the client to request the segment.

## 7. API

Update camera create/update DTOs with recording fields:

```json
{
  "recordingMode": "off | always | motion",
  "motionEnabled": true,
  "motionSensitivity": 0.5,
  "motionThreshold": 0.01,
  "preMotionSeconds": 10,
  "postMotionSeconds": 20,
  "segmentSeconds": 10
}
```

Add endpoints:

```text
GET /cameras/{id}/recordings?from=<iso>&to=<iso>
GET /cameras/{id}/recordings/seek?at=<iso>
GET /recording-segments/{segmentId}/file
GET /cameras/{id}/motion-events?from=<iso>&to=<iso>
```

Seek response:

```json
{
  "segmentId": "...",
  "cameraId": "...",
  "fileUrl": "/recording-segments/.../file",
  "segmentStartAt": "2026-05-20T14:30:00Z",
  "segmentEndAt": "2026-05-20T14:30:10Z",
  "offsetMs": 4200
}
```

The first implementation can return `offsetMs` as metadata and leave actual client-side seek to the player. A later implementation can add byte-range or server-side remuxing if needed.

## 8. Motion Detection Details

Use `motioncells` from the GStreamer OpenCV plugin.

Default low-cost motion branch:

```text
queue leaky=downstream max-size-buffers=2
  -> decodebin
  -> videorate
  -> video/x-raw,framerate=5/1
  -> videoscale
  -> video/x-raw,width=320
  -> videoconvert
  -> video/x-raw,format=RGB
  -> motioncells display=false postallmotion=false
  -> fakesink sync=false
```

The motion branch may drop frames. It must never backpressure the live RTSP branch.

## 9. Error Handling

- If motion plugin is missing, camera can still run live stream and `always` recording, but `motion` recording should fail validation or report an explicit runtime error.
- If recording directory is not writable, stream can keep running but recording status becomes error.
- If segment DB insert fails, keep the file temporarily and retry or delete it according to retention policy.
- If a segment file is missing at playback time, return `404`.

## 10. Verification

Unit tests:

- Recording mode validation.
- Segment path generation.
- Motion window retention logic.
- Seek timestamp to segment lookup.

Integration/manual tests:

- `always` mode produces segments continuously.
- `motion` mode keeps pre/post motion windows and deletes unrelated segments.
- Live RTSP remains playable while motion and recording branches are enabled.
- Seek endpoint returns the correct segment and offset.
- Missing plugin and unwritable recording directory produce clear errors.

## 11. Dependencies

Required GStreamer elements:

- `motioncells` from GStreamer Bad/OpenCV plugin.
- `splitmuxsink` from GStreamer Good plugins.
- H264/H265 depay/parser/payloader elements already used by the live stream.

The local development machine currently has `motioncells` and `splitmuxsink` available.

