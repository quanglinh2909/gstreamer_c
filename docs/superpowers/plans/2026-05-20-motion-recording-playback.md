# Motion Recording Playback Implementation Plan

> **For agentic workers:** REQUIRED: Use superpowers:subagent-driven-development (if subagents available) or superpowers:executing-plans to implement this plan. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add the first working foundation for motion/continuous recording and timestamp playback.

**Architecture:** Keep live RTSP passthrough separate from recording policy. Add pure recording helpers and tests first, then extend DTO/DB/API surfaces, then add GStreamer recording pipeline support in small steps.

**Tech Stack:** C++20, Oat++, PostgreSQL, GStreamer, CTest.

---

## Chunk 1: Recording Policy Foundation

### Task 1: Add recording policy tests and helpers

**Files:**
- Modify: `tests/StreamLogicTests.cpp`
- Create: `src/service/RecordingTypes.hpp`

- [x] Write failing tests for recording mode parsing, motion-window segment retention, seek offset, and recording pipeline launch shape.
- [x] Run `cmake --build build -j 2 && ctest --test-dir build --output-on-failure` and verify tests fail because helpers are missing.
- [x] Implement minimal `RecordingTypes.hpp` helpers.
- [x] Run tests and verify pass.

## Chunk 2: API and Database Surface

### Task 2: Extend camera schema and DTOs

**Files:**
- Modify: `sql/001_init_cameras.sql`
- Modify: `src/dto/CameraDto.hpp`
- Create: `src/dto/RecordingDto.hpp`
- Modify: `src/db/CameraDb.hpp`
- Modify: `src/service/CameraService.hpp`
- Modify: `src/controller/CameraController.hpp`

- [x] Add camera recording fields and recording tables to SQL.
- [x] Add DTO fields for camera recording config, segment list, seek result, and motion events.
- [x] Extend create/update/select camera queries.
- [x] Add list/seek/file metadata methods.
- [x] Add HTTP endpoints for recordings and motion events.
- [x] Build and test.

## Chunk 3: Runtime Recording Pipeline

### Task 3: Add recording pipeline shell

**Files:**
- Create: `src/service/CameraRecordingSession.hpp`
- Modify: `src/service/CameraStreamSession.hpp`
- Modify: `src/service/GStreamerService.hpp`

- [x] Add a separate always-on recording pipeline for cameras with `recordingMode != off`.
- [x] Use `splitmuxsink` for segments and `motioncells` for motion mode.
- [x] Watch bus messages for splitmux fragment open/close and motion events.
- [x] Forward runtime events to service/DB callbacks.
- [x] Build and test.

## Chunk 4: Verification

### Task 4: Verify

**Files:**
- No new files.

- [x] Run `ctest --test-dir build --output-on-failure`.
- [x] Run `./build.sh`.
- [x] Run `git diff --check`.
- [ ] Manually verify a camera can still live-stream.
