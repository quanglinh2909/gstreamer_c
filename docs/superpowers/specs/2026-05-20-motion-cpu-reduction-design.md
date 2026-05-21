# Motion Detection CPU Reduction — Design

**Date:** 2026-05-20
**Status:** Approved (per-camera configuration)

## Goal

Reduce CPU usage of the motion-detection branch in the recording pipeline,
which is too high when `recordingMode=motion` or `motionEnabled=true`.
Both controls are **per-camera**.

## Background — root cause

The motion branch of `recordingLaunchStringForCamera` decodes the camera's
**full main stream** with `decodebin`:

```
record_t. ! queue ! decodebin ! videorate ! 5/1 ! videoscale ! 320 ! videoconvert ! RGB ! motioncells ! fakesink
```

Two confirmed problems:

1. **`decodebin` always picks software decode.** On the test machine
   `vah264dec`/`vah265dec` (VA-API, GPU) have `rank=none (0)`, while
   `avdec_h264`/`avdec_h265` (software) have `rank=primary (256)`. `decodebin`
   autoplugs by rank, so it never uses the GPU even when one is present.
2. The branch decodes every frame at full resolution **before** dropping to
   5fps / 320px — paying the most expensive step then discarding most of it.

Constraints gathered:

- Cameras have **no low-resolution substream** — motion must use the main stream.
- Deployment spans **various machines**; GPU availability is unknown per machine.
- Both settings must be configurable **per camera** (not per machine).

## Design

Two independent, complementary changes, both **per-camera**. No global
`config.json` keys are added.

### Part A — Per-camera hardware decode

`decodebin` cannot be steered per camera by rank (rank is process-wide), so the
motion branch uses an **explicit decoder element** chosen from the camera's
existing `hardware` field (`auto` / `software` / `vaapi` / `nvdec` / `v4l2`).
The `hardware` column, DTO field and runtime-config field already exist and are
currently unused — this design wires them up. **No new DB column for Part A.**

- New pure function `recording::motionDecoderCandidates(hardware, codec)` in
  `RecordingTypes.hpp` — returns an ordered preference list of decoder element
  names. Every list **ends with the software decoder** so resolution always
  succeeds:
  - `software` → `[avdec_h26x]`
  - `vaapi`    → `[vah26xdec, avdec_h26x]`
  - `nvdec`    → `[nvh26xdec, avdec_h26x]`
  - `v4l2`     → `[v4l2h26xdec, avdec_h26x]`
  - `auto` / empty / unknown → `[vah26xdec, nvh26xdec, v4l2h26xdec, avdec_h26x]`
- New GStreamer-dependent helper `CameraRecordingSession::resolveMotionDecoder()`
  — picks the first candidate whose `gst_element_factory_find()` is non-null.
- `recordingLaunchStringForCamera` takes the resolved decoder name as a
  parameter and emits it in place of `decodebin`.
- Trade-off: no `decodebin` autoplug-fallback. If an explicitly forced hardware
  decoder fails at runtime, the motion branch posts a bus error and the existing
  `restartRecordOnlyAfterError` fallback takes over (recording continues without
  motion). If a forced hardware decoder's factory is simply absent, resolution
  falls through to the software decoder (logged to stderr).

### Part B — Per-camera keyframe-only analysis

When enabled for a camera, its motion branch decodes only IDR keyframes, cutting
decode work drastically even with software decode. New per-camera boolean
`motionKeyframeOnly`, default `false`.

- New DB column `motion_keyframe_only BOOLEAN NOT NULL DEFAULT false`
  (`CREATE TABLE` + idempotent `ALTER TABLE ... ADD COLUMN IF NOT EXISTS`).
- New DTO field `motionKeyframeOnly` on the camera DTOs; new
  `CameraRuntimeConfig.motionKeyframeOnly`.
- Wired through `createCamera` / `updateCamera` / all camera SELECT/RETURNING
  queries, `GStreamerService::toRuntimeConfig`, and `CameraService`.
  `recordingPatchRequiresRuntimeRestart` gains a `motionKeyframeOnly` parameter
  so changing it restarts the runtime pipeline.
- `recordingLaunchStringForCamera`: name the motion-branch queue
  `queue name=motion_q ...`, and add `drop-only=true` to `videorate` so it never
  duplicates frames when the input rate is below 5fps (correct for both modes).
- `CameraRecordingSession::startPipeline`: when `m_camera.motionKeyframeOnly` is
  set and the motion branch is present, add a buffer pad probe on the `motion_q`
  sink pad. The probe is a **stateless static function** — it drops buffers with
  `GST_BUFFER_FLAG_DELTA_UNIT` (P/B frames) and keeps keyframes. No userData, so
  no use-after-free risk; the probe is released with the pipeline.
- IDR frames are independently decodable and already carry SPS/PPS
  (`h264parse config-interval=-1`), so the decoder handles a keyframe-only stream.

## Configuration & API

- **No new `config.json` keys.** The previously proposed `motionHardwareDecode`
  / `motionKeyframeOnly` global keys are dropped — both settings are per-camera.
  Any such keys already added to `config.json` should be removed (harmless but
  inert).
- Per-camera, set via the existing camera API:
  - `hardware`: `auto` (default) / `software` / `vaapi` / `nvdec` / `v4l2`.
    Unknown values are treated as `auto`.
  - `motionKeyframeOnly`: boolean, default `false`.
- `gstreamer.defaultHardware` in `config.json` remains the fallback applied when
  a camera's `hardware` is empty (existing behaviour).

## Files affected

Part A:
- `src/service/RecordingTypes.hpp` — `motionDecoderCandidates()`;
  `recordingLaunchStringForCamera` takes the motion decoder name, names the
  motion queue `motion_q`, adds `videorate drop-only=true`.
- `src/service/CameraRecordingSession.hpp` — `resolveMotionDecoder()`; keyframe
  drop probe in `startPipeline`.

Part B (per-camera plumbing — follows the existing recording-field pattern):
- `sql/001_init_cameras.sql` — `motion_keyframe_only` column.
- `src/dto/CameraDto.hpp` — `motionKeyframeOnly` on the camera DTOs.
- `src/service/StreamTypes.hpp` — `CameraRuntimeConfig.motionKeyframeOnly`.
- `src/db/CameraDb.hpp` — column in create/update and all select/returning
  queries.
- `src/service/GStreamerService.hpp` — `toRuntimeConfig` mapping.
- `src/service/CameraService.hpp` — pass-through; `streamRelevantInputPresent`.
- `src/service/RecordingTypes.hpp` — `recordingPatchRequiresRuntimeRestart` param.
- `config/config.json` — remove the now-obsolete `motionHardwareDecode` /
  `motionKeyframeOnly` keys.
- `tests/StreamLogicTests.cpp` — tests.

## Testing

- Unit tests (string level, no GStreamer link):
  - `motionDecoderCandidates` returns the expected ordered lists per `hardware`
    value and codec, always ending with the software decoder.
  - `recordingLaunchStringForCamera` emits the supplied decoder, `motion_q`, and
    `videorate ... drop-only=true`.
- GStreamer-level verification probes (manual, as used for prior fixes):
  1. `resolveMotionDecoder` picks a hardware decoder when its factory exists and
     the software decoder otherwise.
  2. A pipeline with the keyframe probe on `motion_q` passes only keyframe
     buffers downstream.
  3. The motion branch parses to a valid pipeline for each decoder choice.

## Scope — non-goals (YAGNI)

- No global/per-machine config keys for these settings.
- No configurable analysis fps/resolution — keep 5fps / 320px.
- No GPU-side scaling/conversion (`vapostproc`) — hardware decode is the main win.
- No strict validation of the `hardware` value — unknown values fall back to
  `auto` rather than being rejected.

## Known limitations

- Keyframe-only mode samples motion at the camera's keyframe (GOP) rate. With a
  long GOP (e.g. a keyframe every 10s) the sampling rate is too low to be useful;
  operators enabling `motionKeyframeOnly` must know their cameras' GOP length.
- An explicitly forced hardware decoder that fails at runtime degrades that
  camera to record-only (no motion) via the existing fallback, rather than
  retrying in software.
