# GStreamer Camera RTSP Restream Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a GStreamer RTSP restream service that starts one managed stream session per camera and exposes each camera at `/cameras/<id>` on a shared RTSP server.

**Architecture:** Keep camera CRUD in the existing Oat++ controller/service/db layers and add a dedicated GStreamer runtime layer. Pure stream policy lives in small testable helpers; GStreamer object ownership lives in `GStreamerService` and `CameraStreamSession`; `CameraService` coordinates DB changes with stream lifecycle calls.

**Tech Stack:** C++20, Oat++, PostgreSQL, GStreamer 1.0, gst-rtsp-server, CMake/CTest.

---

## Chunk 1: Testable Stream Policy

### Task 1: Add stream helper tests

**Files:**
- Create: `tests/StreamLogicTests.cpp`
- Modify: `CMakeLists.txt`

- [ ] **Step 1: Write failing tests**

Create assert-based tests for codec detection, mount/output URL generation, retry delay, and auth error classification. The tests include `service/StreamTypes.hpp`, which does not exist yet.

- [ ] **Step 2: Run tests to verify RED**

Run:

```bash
cmake --build build -j "$(nproc)"
```

Expected: compile fails because `service/StreamTypes.hpp` is missing.

### Task 2: Implement stream helper logic

**Files:**
- Create: `src/service/StreamTypes.hpp`

- [ ] **Step 1: Add minimal helper implementation**

Implement:

- `StreamCodec`
- `StreamState`
- `GStreamerConfig`
- `CameraRuntimeConfig`
- `codecFromEncodingName`
- `isAuthErrorMessage`
- `nextRetryDelayMs`
- `mountPathForCamera`
- `outputUrlForCamera`
- `launchStringForCamera`

- [ ] **Step 2: Run helper tests**

Run:

```bash
cmake --build build -j "$(nproc)"
ctest --test-dir build --output-on-failure
```

Expected: `stream_logic_tests` passes.

## Chunk 2: Runtime DTOs, Config, and GStreamer Service

### Task 3: Add config and runtime DTOs

**Files:**
- Modify: `src/config/ConfigDto.hpp`
- Modify: `config/config.json`
- Create: `src/dto/StreamStatusDto.hpp`

- [ ] **Step 1: Add `GStreamerConfigDto`**

Fields: `rtspHost`, `publicRtspHost`, `rtspPort`, `retryInitialMs`, `retryMaxMs`, `sourceLatencyMs`, `defaultHardware`, `recordingEnabled`, `recordingDir`.

- [ ] **Step 2: Add `StreamStatusDto`**

Fields: `id`, `state`, `inputRtsp`, `outputRtsp`, `codec`, `hardware`, `recordingEnabled`, `retryCount`, `lastError`, `lastChangedAt`.

### Task 4: Add GStreamer runtime service

**Files:**
- Create: `src/service/CameraStreamSession.hpp`
- Create: `src/service/GStreamerService.hpp`
- Create: `src/GStreamerComponent.hpp`

- [ ] **Step 1: Implement `CameraStreamSession`**

Own one RTSP media factory mount, track state, classify transient/auth/unsupported errors, expose `start`, `stop`, `restart`, `cleanup`, and `snapshot`.

- [ ] **Step 2: Implement `GStreamerService`**

Own one `GstRTSPServer`, a GLib main loop thread, and a map of sessions. Expose `start`, `stop`, `cleanup`, `startCamera`, `stopCamera`, `restartCamera`, `getStatus`, and `getAllStatuses`.

- [ ] **Step 3: Add DI component**

Expose a shared `GStreamerService` from config.

## Chunk 3: Oat++ Integration

### Task 5: Wire camera CRUD and stream endpoints

**Files:**
- Modify: `src/db/CameraDb.hpp`
- Modify: `src/service/CameraService.hpp`
- Modify: `src/controller/CameraController.hpp`

- [ ] **Step 1: Add DB helper queries**

Add unpaginated `getAllCamerasForStartup` and a delete query returning deleted row.

- [ ] **Step 2: Inject `GStreamerService` into `CameraService`**

Start streams after create, restart after RTSP update, stop after delete, and add status/manual lifecycle methods.

- [ ] **Step 3: Add controller endpoints**

Add `GET /camera-streams`, `GET /cameras/{id}/stream`, and manual start/stop/restart endpoints.

### Task 6: Wire application startup and shutdown

**Files:**
- Modify: `src/App.cpp`
- Modify: `CMakeLists.txt`
- Modify: `vcpkg.json`

- [ ] **Step 1: Initialize GStreamer runtime**

Call `gst_init`, construct `GStreamerComponent`, start the service, auto-start all cameras from DB, and cleanup on shutdown.

- [ ] **Step 2: Link GStreamer**

Use `PkgConfig` to require `gstreamer-1.0`, `gstreamer-rtsp-1.0`, and `gstreamer-rtsp-server-1.0`. Do not add GStreamer to the vcpkg manifest; use system GStreamer packages so the application can load the matching runtime plugin set.

## Chunk 4: Verification

### Task 7: Verify

**Files:**
- No new files.

- [ ] **Step 1: Run tests**

Run:

```bash
ctest --test-dir build --output-on-failure
```

- [ ] **Step 2: Run build**

Run:

```bash
./build.sh
```

Expected: If `gstreamer-rtsp-server-1.0` is still missing locally, configure/build reports that dependency blocker. If installed, build succeeds.

- [ ] **Step 3: Commit**

Stage only GStreamer implementation files and commit with:

```bash
git commit -m "feat: add gstreamer camera restream service"
```
