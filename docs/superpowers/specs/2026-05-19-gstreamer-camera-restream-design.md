# GStreamer Camera RTSP Restream — Design

**Date:** 2026-05-19
**Project:** `test_gstreamer`
**Stack:** C++20 + Oat++ + PostgreSQL + GStreamer + gst-rtsp-server
**Goal:** Start and manage one zero-decode GStreamer restream session per camera.

## 1. Goal & Scope

Add runtime GStreamer management to the existing camera CRUD API:

- When a camera is created, start a GStreamer session for it immediately.
- When a camera is deleted, stop and clean up its session.
- When a camera RTSP URL is changed, close the old session and create a new one for the updated source.
- On application startup, load all cameras from PostgreSQL and auto-start their sessions.
- Expose each camera as one mount path on a shared RTSP server:

```text
rtsp://<configured-host>:<configured-port>/cameras/<camera-id>
```

The first implementation supports H264/H265 passthrough only. It must not decode or transcode in the restream path. Unsupported codecs move the session to an explicit runtime error state.

Out of scope for the first implementation:

- Actual AI processing branches.
- Actual recording branches.
- User-facing authentication for the output RTSP server.
- Multi-host/distributed stream workers.

The service must still be structured so AI queues, recording, and hardware-specific branches can be added later without rewriting camera lifecycle management.

## 2. Chosen Approach

Use a shared `GStreamerService` with one RTSP server and one session object per camera:

```text
App.cpp
  -> gst_init()
  -> GStreamerService.start()
  -> CameraService.startAllStreamsFromDatabase()

CameraController
  -> CameraService
       -> CameraDb
       -> GStreamerService

GStreamerService
  -> GstRTSPServer on configured port
  -> map<cameraId, CameraStreamSession>

CameraStreamSession
  -> input RTSP source state
  -> output mount path
  -> codec detection
  -> pipeline lifecycle
  -> reconnect policy
  -> runtime status tracking
```

This keeps HTTP/DB concerns separate from stream runtime concerns. `CameraService` coordinates database changes and stream changes, while `GStreamerService` owns all GStreamer objects and cleanup.

## 3. Runtime Components

### `GStreamerComponent.hpp`

Oat++ DI component exposing one shared `GStreamerService`.

Responsibilities:

- Read GStreamer config from `ConfigDto`.
- Construct the service once.
- Let `CameraService` and `App.cpp` access the same stream manager.

### `service/GStreamerService.hpp`

Thread-safe manager for all camera sessions.

Public API:

```cpp
void start();
void stop();
void cleanup();

void startCamera(const CameraRuntimeConfig& camera);
void stopCamera(const oatpp::String& cameraId);
void restartCamera(const CameraRuntimeConfig& camera);

StreamStatusDto getStatus(const oatpp::String& cameraId) const;
oatpp::List<oatpp::Object<StreamStatusDto>> getAllStatuses() const;
```

Internal responsibilities:

- Initialize and attach `GstRTSPServer`.
- Register and remove mount points.
- Own `CameraStreamSession` instances in a `std::unordered_map`.
- Guard session map with a mutex.
- Provide lifecycle methods for global server shutdown.
- Avoid database ownership; callers pass camera rows into the stream manager.

### `service/CameraStreamSession.hpp`

One object per camera stream.

Responsibilities:

- Build a source pipeline for one input camera.
- Detect H264/H265 from `rtspsrc` dynamic pad caps.
- Link codec-specific passthrough elements.
- Publish stream to `gst-rtsp-server` mount `/cameras/<id>`.
- Track current state and last error.
- Retry network/timeout failures with exponential backoff.
- Stop retrying on authentication/authorization failures.
- Clean up all GStreamer refs, bus watches, timers, and mount registrations.

### `dto/StreamStatusDto.hpp`

Runtime status returned by API.

Fields:

```cpp
id: String
state: String
inputRtsp: String
outputRtsp: String
codec: String
hardware: String
recordingEnabled: Boolean
retryCount: UInt32
lastError: String
lastChangedAt: String
```

States:

- `stopped`
- `starting`
- `running`
- `reconnecting`
- `auth_error`
- `unsupported_codec`
- `error`

Database `cameras.status` can stay coarse (`online|offline|error`) for now. Detailed stream state lives in memory and is exposed by stream status endpoints.

## 4. Configuration

Extend `ConfigDto` and `config/config.json` with a `gstreamer` section:

```json
{
  "gstreamer": {
    "rtspHost": "0.0.0.0",
    "publicRtspHost": "127.0.0.1",
    "rtspPort": 8554,
    "retryInitialMs": 1000,
    "retryMaxMs": 30000,
    "sourceLatencyMs": 200,
    "defaultHardware": "auto",
    "recordingEnabled": false,
    "recordingDir": "recordings"
  }
}
```

Field meaning:

- `rtspHost`: bind address for the shared output RTSP server.
- `publicRtspHost`: host used when returning playable URLs to clients.
- `rtspPort`: output RTSP port.
- `retryInitialMs`: first reconnect delay for transient failures.
- `retryMaxMs`: maximum reconnect delay.
- `sourceLatencyMs`: `rtspsrc latency` value.
- `defaultHardware`: reserved for future hardware-aware branches (`auto`, `cpu`, `vaapi`, `nvdec`, etc.).
- `recordingEnabled`: reserved default for future recording branch.
- `recordingDir`: reserved output directory for recordings.

The first implementation does not decode, so hardware selection is stored and reported but does not change the main restream path yet.

## 5. Pipeline Model

The restream path must avoid `decodebin`, software decoders, hardware decoders, and encoders.

Input handling:

```text
rtspsrc name=src location=<camera-rtsp> latency=<sourceLatencyMs> protocols=tcp
```

On `pad-added`, inspect caps:

- `application/x-rtp, media=video, encoding-name=H264`
- `application/x-rtp, media=video, encoding-name=H265`
- `application/x-rtp, media=video, encoding-name=HEVC`

For H264:

```text
src dynamic pad
  -> rtph264depay
  -> h264parse config-interval=-1
  -> rtph264pay name=pay0 pt=96
```

For H265/HEVC:

```text
src dynamic pad
  -> rtph265depay
  -> h265parse config-interval=-1
  -> rtph265pay name=pay0 pt=96
```

Unsupported codecs:

- Do not insert decode/transcode fallback.
- Set state `unsupported_codec`.
- Stop the session until the camera config changes or the service is manually restarted.

### Future tee layout

The restream path should be built around an internal branch model even if only the RTSP branch is active first:

```text
source RTP
  -> depay
  -> parse
  -> tee name=video_t
       -> queue leaky=downstream
       -> rtppay pay0

       -> queue leaky=downstream
       -> future AI branch

       -> queue
       -> future recording branch
```

For the first implementation, only the RTSP payloader branch is enabled. Future AI branches may require decode, but that decode must happen only in the AI branch, not in the main restream path.

## 6. RTSP Server

Use `gst-rtsp-server`.

Server behavior:

- One server instance per application.
- Port comes from `config.json`.
- One mount per camera:

```text
/cameras/<camera-id>
```

Each mount should be shared so multiple clients can consume the same output stream without creating separate upstream camera connections whenever possible.

Expected client URL:

```text
rtsp://<publicRtspHost>:<rtspPort>/cameras/<camera-id>
```

## 7. Camera CRUD Integration

### Create camera

1. Validate request.
2. Insert camera into DB.
3. Fetch inserted row.
4. Call `GStreamerService.startCamera(row)`.
5. Return camera DTO including existing DB fields. Runtime stream status is available from status endpoints.

If DB insert succeeds but stream start fails immediately, the API still returns the created camera. The runtime status records `error`, `auth_error`, or `unsupported_codec`.

### Update camera

For `PUT /cameras/{id}`:

- If `rtsp` is absent, update DB fields and keep the existing stream.
- If `rtsp` is present and unchanged, update DB fields and keep the existing stream.
- If `rtsp` changes:
  1. Fetch existing camera.
  2. Update DB.
  3. If DB update succeeds, call `GStreamerService.restartCamera(updatedRow)`.
  4. If DB update fails, keep the old stream untouched.

The user request says "sửa rtsp thì tạo tại close"; this design interprets that as close/cleanup the old session and create a new session at the same mount path after the DB update succeeds.

### Delete camera

1. Fetch existing camera to confirm it exists.
2. Delete DB row.
3. If delete succeeds, call `GStreamerService.stopCamera(id)`.
4. Return deleted status.

If the DB delete fails, the stream remains active.

### App startup

1. Load config.
2. Initialize Oat++ components and DB.
3. Call `gst_init`.
4. Start `GStreamerService`.
5. `CameraService` queries all cameras with a service-level method that does not apply pagination.
6. `CameraService` passes every camera row to `GStreamerService.startCamera`.
7. Start HTTP server.

### App shutdown

On `SIGINT`/`SIGTERM`:

1. Stop Oat++ HTTP server.
2. Call `GStreamerService.stop()`.
3. Call `GStreamerService.cleanup()`.
4. Destroy Oat++ environment.

## 8. Reconnect & Error Classification

Retry only transient errors:

- Network timeout.
- Connection refused.
- EOS after a running session.
- Temporary RTSP server/source disconnect.

Do not retry authentication/authorization errors:

- 401 Unauthorized.
- 403 Forbidden.
- Error messages containing authentication/authorization failure from `rtspsrc` or RTSP response.

Behavior:

- Transient errors set state `reconnecting`.
- Retry delay starts at `retryInitialMs` and doubles up to `retryMaxMs`.
- A successful run resets `retryCount` and delay.
- Auth errors set state `auth_error` and leave the session stopped.
- Unsupported codec sets state `unsupported_codec` and leaves the session stopped.

The first implementation may classify auth errors conservatively from GStreamer error domain/message if direct RTSP status code is not available. The important rule is that once classified as auth-related, it must not reconnect automatically.

## 9. API Additions

Add runtime status endpoints:

| Method | Path | Response |
|---|---|---|
| `GET` | `/cameras/{id}/stream` | `StreamStatusDto` |
| `GET` | `/camera-streams` | `List<StreamStatusDto>` |
| `POST` | `/cameras/{id}/stream/start` | `StreamStatusDto` |
| `POST` | `/cameras/{id}/stream/stop` | `StreamStatusDto` |
| `POST` | `/cameras/{id}/stream/restart` | `StreamStatusDto` |

The manual start/stop/restart endpoints are useful for operations and testing. CRUD still performs the automatic behavior described above.

`GET /camera-streams` is intentionally outside `/cameras/{id}` to avoid route ambiguity with the existing dynamic camera-id endpoint.

## 10. Database Changes

No database migration is required for the first implementation.

The existing table remains:

```sql
cameras(id, name, rtsp, status)
```

Runtime stream state is in memory. If persistence is later needed, add a separate `camera_stream_settings` table instead of overloading the current CRUD table.

## 11. Build Dependencies

Add GStreamer dependencies to the build:

- `gstreamer-1.0`
- `gstreamer-rtsp-server-1.0`
- RTP plugins providing `rtph264depay`, `rtph264pay`, `rtph265depay`, `rtph265pay`
- codec parser plugins providing `h264parse`, `h265parse`

Local environment note from design exploration:

- `gstreamer-1.0` is available as `1.20.3`.
- `gstreamer-rtsp-server-1.0` was not found by `pkg-config` and must be installed before full RTSP-server build verification can pass.

## 12. Testing Strategy

### Unit-style tests where practical

Focus on logic that can be tested without live cameras:

- Codec caps classification: H264, H265, HEVC, unsupported.
- Reconnect policy: retry transient errors, stop on auth errors.
- Output URL/mount path generation.
- Session state transitions.

### Build verification

Run:

```bash
./build.sh
```

Expected blocker until dependency is installed:

```text
gstreamer-rtsp-server-1.0 not found
```

### Runtime smoke test

With a valid camera RTSP URL:

1. Start app.
2. Create camera through `POST /cameras`.
3. Check `GET /cameras/{id}/stream`.
4. Open `rtsp://<publicRtspHost>:<rtspPort>/cameras/<id>` from another machine.
5. Confirm multiple clients can read the same output URL.
6. Update camera RTSP URL and confirm the session restarts at the same mount.
7. Delete camera and confirm the output URL stops.

### Auth error smoke test

Use a known wrong credential:

1. Create/update a camera with invalid RTSP credentials.
2. Confirm status becomes `auth_error`.
3. Confirm retry count does not keep increasing.
4. Confirm no rapid reconnect loop hits the camera.

## 13. Risks & Follow-ups

- `gst-rtsp-server` must be installed locally and available to CMake/pkg-config.
- GStreamer auth error details can vary by plugin/version; classification should start conservative and improve with real camera logs.
- True zero-copy across every element depends on negotiated memory types and hardware. The first guarantee is "no decode/no transcode" in the restream path.
- Future AI and recording branches must be isolated behind queues. AI decode must not block the RTSP branch.
- If stream count grows large, move GStreamer runtime to a separate worker process or service.
