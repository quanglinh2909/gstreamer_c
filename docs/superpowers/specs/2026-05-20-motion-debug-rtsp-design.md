# Motion-Debug RTSP Stream — Design

**Date:** 2026-05-20
**Status:** Approved

## Goal

Let an operator watch what the motion detector sees. Expose a second RTSP mount
per camera — `rtsp://host:8554/cameras/{id}/motion` — that serves the camera
video with the `motioncells` overlay drawn on it (`display=true`), so motion
cells are visible in a normal RTSP player (VLC).

## Background

`GStreamerService` runs one `GstRTSPServer`. `CameraStreamSession`, after
probing the camera codec, registers a live passthrough mount `/cameras/{id}`
backed by a `GstRTSPMediaFactory` whose launch string comes from
`stream::launchStringForCamera`. The recording pipeline's motion branch already
runs `motioncells` at 320px / 5fps; `motioncells` has a `display` property that,
when `true`, draws the detected motion grid onto the video frames.

A `GstRTSPMediaFactory` instantiates its pipeline only when a client connects
and tears it down after the last client disconnects — so an extra factory costs
no CPU while nobody is watching.

## Design

### Second RTSP mount per camera

`CameraStreamSession` registers a second `GstRTSPMediaFactory` at
`/cameras/{id}/motion`, alongside the live mount, in the same place in `start()`
(after the codec is confirmed H264/H265). It is removed in `cleanupMountLocked`
together with the live mount.

The motion-debug factory is plain: `gst_rtsp_media_factory_new`,
`set_launch(...)`, `set_shared(TRUE)`, `set_suspend_mode(GST_RTSP_SUSPEND_MODE_NONE)`,
`gst_rtsp_mount_points_add_factory`. Unlike the live factory it needs no
`media-configure` / bus-watch callback — it drives no reconnect logic.

(Alternative considered: a dedicated class for the debug mount, like
`CameraRecordingSession`. Rejected — the debug factory is a single static
factory with no runtime lifecycle of its own; a separate class is boilerplate
without benefit. The mount belongs with the camera's other RTSP mount, which is
`CameraStreamSession`'s responsibility.)

### Launch string

A new pure function `recording::motionDebugLaunchStringForCamera(config, camera,
codec, motionDecoder)` builds the factory launch string, wrapped in `( ... )`
(required by `gst_rtsp_media_factory_set_launch`):

```
( rtspsrc name=motion_dbg_src location=<rtsp> latency=<sourceLatencyMs>
    protocols=tcp drop-on-latency=true
  ! application/x-rtp,media=video,encoding-name=<H264|H265>
  ! <rtph264depay|rtph265depay>
  ! <h264parse|h265parse> config-interval=-1
  ! <motionDecoder>
  ! videorate drop-only=true ! video/x-raw,framerate=5/1
  ! videoscale ! video/x-raw,width=320
  ! videoconvert ! video/x-raw,format=RGB
  ! motioncells display=true postallmotion=false
      sensitivity=<motionSensitivity> threshold=<motionThreshold>
      gap=<postMotionSeconds>
  ! videoconvert ! videoscale ! video/x-raw,width=960
  ! x264enc tune=zerolatency speed-preset=ultrafast
  ! rtph264pay config-interval=1 name=pay0 pt=96 )
```

- `motioncells` runs at **320px / 5fps**, mirroring the recording motion branch,
  and takes `sensitivity` / `threshold` / `gap` from the camera config — so the
  debug view shows the *real* production detector behaviour.
- The annotated 320px frames are scaled up to ~960px purely for comfortable
  viewing (no extra detail — just a larger window).
- `x264enc` (ultrafast / zerolatency) re-encodes; output is **always H264** via
  `rtph264pay name=pay0`, even for H265 cameras — universally playable.

### Decoder reuse

The debug pipeline decodes the camera, so it picks a decoder the same way the
recording motion branch does. `CameraRecordingSession::resolveMotionDecoder`
(per-camera `hardware` field → first available decoder factory) changes from
`private` to `public static`; `CameraStreamSession` calls
`CameraRecordingSession::resolveMotionDecoder(m_camera, probeCodec)` and passes
the result to `motionDebugLaunchStringForCamera`.

## API

- `rtsp://<publicRtspHost>:<rtspPort>/cameras/{id}/motion` — the debug stream.
  By convention it is the live stream URL with `/motion` appended.
- Documented in `README.md`. No new DTO field — the URL is derivable from the
  existing `outputRtsp`.
- No authentication — consistent with the existing open RTSP live mounts.

## Files affected

- `src/service/RecordingTypes.hpp` — new pure
  `motionDebugLaunchStringForCamera(config, camera, codec, motionDecoder)`.
- `src/service/CameraRecordingSession.hpp` — `resolveMotionDecoder`:
  `private` → `public static`.
- `src/service/CameraStreamSession.hpp` — register the `/motion` factory in
  `start()`; remove it in `cleanupMountLocked()`; new members
  `m_motionFactory` / `m_motionMountPath`.
- `tests/StreamLogicTests.cpp` — unit test for `motionDebugLaunchStringForCamera`.
- `README.md` — document the `/cameras/{id}/motion` endpoint.

## Error handling

The motion-debug factory is best-effort. If its pipeline fails (camera
unreachable, decoder error), the RTSP client simply cannot play — the live
stream and the recording pipeline are unaffected. If the factory cannot be
built, registration is skipped and the live mount still works.

## CPU cost

Zero while no client is connected (the factory instantiates its pipeline
on-demand). While a client watches: one decode + `motioncells` at 320px/5fps +
one `x264enc` ultrafast — modest, and only during deliberate debugging.

## Testing

- Unit test (`stream_logic_tests`, no GStreamer link): `motionDebugLaunchStringForCamera`
  output is wrapped in `( ... )`, contains `motioncells display=true`, the
  supplied decoder, `video/x-raw,width=320`, `x264enc`, and `name=pay0`.
- GStreamer probe (manual): `gst_parse_launch` of the debug launch string
  returns a non-null element with no `GError` — confirms every element
  (`motioncells`, `x264enc`, the decoder, payloader) exists and links.

## Scope — non-goals (YAGNI)

- The debug stream does **not** replicate `motionKeyframeOnly`: it always
  analyses at 5fps, even for cameras with keyframe-only enabled. A keyframe-only
  (~1fps) debug view would be too choppy to watch, and the motion grid /
  sensitivity it shows are identical regardless of sampling rate. Documented as
  a deliberate simplification.
- No new DTO field for the URL — it follows the `{outputRtsp}/motion` convention.
- No authentication.
- No per-camera enable/disable — every running camera has the `/motion` mount;
  on-demand instantiation means no idle cost.
- Output codec is always H264 even for H265 cameras (re-encoding anyway).

## Known limitations

- A client watching the debug stream opens a third RTSP connection to the camera
  (live passthrough + recording + debug). Most cameras allow this; very limited
  cameras may refuse the extra session.
- The debug view is for observation/tuning, not archival — it is re-encoded with
  a fast low-quality preset.
