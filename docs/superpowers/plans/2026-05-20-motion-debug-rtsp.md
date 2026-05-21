# Motion-Debug RTSP Stream Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a second on-demand RTSP mount per camera (`/cameras/{id}/motion`) that serves the camera video with the `motioncells` motion overlay drawn on it.

**Architecture:** A new pure function builds the debug factory's launch string. `CameraStreamSession` registers a second `GstRTSPMediaFactory` for it alongside the live mount; the factory instantiates its pipeline only while a client is connected, so it costs no CPU when idle.

**Tech Stack:** C++20, GStreamer 1.20 (gst-rtsp-server), Oat++, CTest.

**Spec:** `docs/superpowers/specs/2026-05-20-motion-debug-rtsp-design.md`

---

## Notes for the implementer

- Build: `cmake --build build -j"$(nproc)"`  ·  Unit tests: `ctest --test-dir build --output-on-failure`
- The unit-test target `stream_logic_tests` does NOT link GStreamer. `motionDebugLaunchStringForCamera` must stay a pure function (it takes the decoder name as a string parameter).
- Keep the build green after every task — both `test_gstreamer` and `stream_logic_tests` must compile.
- The debug factory's launch string is wrapped in `( ... )` because it is fed to `gst_rtsp_media_factory_set_launch` (the RTSP factory API), unlike `recordingLaunchStringForCamera` which is fed to `gst_parse_launch`.

---

## Task 1: Add `motionDebugLaunchStringForCamera`

**Files:**
- Modify: `src/service/RecordingTypes.hpp`
- Test: `tests/StreamLogicTests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/StreamLogicTests.cpp`, add this function just before the closing `}`
of the anonymous namespace (after `testMotionDecoderCandidates`):

```cpp
void testMotionDebugLaunchString() {
    stream::GStreamerConfig config;
    config.sourceLatencyMs = 100;

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";
    camera.rtsp = "rtsp://example.local/stream1";
    camera.motionSensitivity = 0.5;
    camera.motionThreshold = 0.01;
    camera.postMotionSeconds = 20;

    const auto h264 = recording::motionDebugLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(!h264.empty());
    assert(h264.front() == '(');   // RTSP media-factory launch string
    assert(h264.back() == ')');
    assert(h264.find("motioncells display=true") != std::string::npos);
    assert(h264.find("rtph264depay") != std::string::npos);
    assert(h264.find("! avdec_h264 !") != std::string::npos);
    assert(h264.find("video/x-raw,width=320") != std::string::npos);  // analyse at 320px
    assert(h264.find("x264enc") != std::string::npos);
    assert(h264.find("name=pay0") != std::string::npos);

    const auto h265 = recording::motionDebugLaunchStringForCamera(
        config, camera, stream::StreamCodec::H265, "vah265dec");
    assert(h265.find("rtph265depay") != std::string::npos);
    assert(h265.find("! vah265dec !") != std::string::npos);

    // Unsupported codec or empty decoder -> empty string.
    assert(recording::motionDebugLaunchStringForCamera(
               config, camera, stream::StreamCodec::Unknown, "avdec_h264").empty());
    assert(recording::motionDebugLaunchStringForCamera(
               config, camera, stream::StreamCodec::H264, "").empty());
}
```

Register it in `main()` — add this line after the existing
`testMotionDecoderCandidates();` call:

```cpp
    testMotionDebugLaunchString();
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -20`
Expected: compile error — `motionDebugLaunchStringForCamera` is not a member of `recording`.

- [ ] **Step 3: Implement `motionDebugLaunchStringForCamera`**

In `src/service/RecordingTypes.hpp`, add this function inside `namespace
recording`, immediately AFTER the `recordingLaunchStringForCamera` function
(after its closing `}`):

```cpp
// Builds the launch string for the on-demand motion-debug RTSP mount. The
// string is wrapped in "( ... )" because it is fed to
// gst_rtsp_media_factory_set_launch. motioncells runs at 320px/5fps exactly
// like the recording motion branch (so the operator sees real production
// behaviour); the annotated frames are scaled up to 960px purely for viewing
// and re-encoded to H264. Returns {} for a non-H264/H265 codec or an empty
// decoder name.
inline std::string motionDebugLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                    const stream::CameraRuntimeConfig& camera,
                                                    stream::StreamCodec codec,
                                                    const std::string& motionDecoder) {
    const bool h264 = codec == stream::StreamCodec::H264;
    const bool h265 = codec == stream::StreamCodec::H265;
    if ((!h264 && !h265) || motionDecoder.empty()) return {};

    const char* encoding = h264 ? "H264" : "H265";
    const char* depay = h264 ? "rtph264depay" : "rtph265depay";
    const char* parser = h264 ? "h264parse" : "h265parse";

    std::ostringstream launch;
    launch
        << "( rtspsrc name=motion_dbg_src location=" << stream::quoteLaunchValue(camera.rtsp)
        << " latency=" << config.sourceLatencyMs
        << " protocols=tcp"
        << " drop-on-latency=true"
        << " ! application/x-rtp,media=video,encoding-name=" << encoding
        << " ! " << depay
        << " ! " << parser << " config-interval=-1"
        << " ! " << motionDecoder
        << " ! videorate drop-only=true"
        << " ! video/x-raw,framerate=5/1"
        << " ! videoscale"
        << " ! video/x-raw,width=320"
        << " ! videoconvert"
        << " ! video/x-raw,format=RGB"
        << " ! motioncells display=true postallmotion=false"
        << " sensitivity=" << camera.motionSensitivity
        << " threshold=" << camera.motionThreshold
        << " gap=" << camera.postMotionSeconds
        << " ! videoscale"
        << " ! video/x-raw,width=960"
        << " ! videoconvert"
        << " ! x264enc tune=zerolatency speed-preset=ultrafast"
        << " ! rtph264pay config-interval=1 name=pay0 pt=96 )";
    return launch.str();
}
```

- [ ] **Step 4: Build and run unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 5: GStreamer verification probe**

Run this probe — it confirms the launch string parses with every element
present (rtspsrc, depay, parse, decoder, videorate, videoscale, videoconvert,
motioncells, x264enc, rtph264pay):

```bash
cat > /tmp/dbg_probe.cpp <<'EOF'
#include "service/RecordingTypes.hpp"
#include <gst/gst.h>
#include <iostream>
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    stream::GStreamerConfig config;
    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a"; camera.rtsp = "rtsp://example.local/s";
    auto launch = recording::motionDebugLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    GError* e = nullptr;
    GstElement* p = gst_parse_launch(launch.c_str(), &e);
    std::cout << "elem=" << (p ? "non-null" : "null")
              << " err=" << (e ? e->message : "none") << "\n";
    bool ok = p && !e;
    if (e) g_error_free(e);
    if (p) gst_object_unref(p);
    return ok ? 0 : 1;
}
EOF
g++ -std=c++20 -I src /tmp/dbg_probe.cpp -o /tmp/dbg_probe $(pkg-config --cflags --libs gstreamer-1.0) \
  && /tmp/dbg_probe; echo "exit=$?"
rm -f /tmp/dbg_probe /tmp/dbg_probe.cpp
```

Expected: `elem=non-null err=none`, `exit=0`.

- [ ] **Step 6: Commit**

```bash
git add src/service/RecordingTypes.hpp tests/StreamLogicTests.cpp
git commit -m "feat(recording): add motionDebugLaunchStringForCamera"
```

End the commit message with this trailer on its own line after a blank line:
`Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`

---

## Task 2: Register the `/cameras/{id}/motion` RTSP mount

**Files:**
- Modify: `src/service/CameraRecordingSession.hpp`
- Modify: `src/service/CameraStreamSession.hpp`

- [ ] **Step 1: Make `resolveMotionDecoder` public**

In `src/service/CameraRecordingSession.hpp`, the `resolveMotionDecoder` method
is currently in the `private:` section. Move it to the public section.

(a) DELETE this block from the private section (it sits between `consumeGError`
and the `dropDeltaFramesProbe` comment):

```cpp
    // Picks the first decoder element that exists for this camera's hardware
    // preference. recordingLaunchStringForCamera uses an explicit decoder, so
    // (unlike decodebin) there is no autoplug fallback — the candidate list
    // always ends with the software decoder for a supported codec. Returns ""
    // only for an unsupported codec (empty candidate list), for which the
    // caller's recordingLaunchStringForCamera also produces no launch string.
    static std::string resolveMotionDecoder(const stream::CameraRuntimeConfig& camera,
                                            stream::StreamCodec codec) {
        for (const auto& name : recording::motionDecoderCandidates(camera.hardware, codec)) {
            GstElementFactory* factory = gst_element_factory_find(name.c_str());
            if (factory) {
                gst_object_unref(factory);
                return name;
            }
        }
        return {};
    }

```

(After deletion, `consumeGError`'s closing `}` is followed directly by a blank
line and then the `// Stateless pad probe:` comment.)

(b) INSERT that same block into the public section — immediately after the
closing `}` of the `stop()` method and before the blank line + `private:`.
The result must read:

```cpp
        deleteRecordingFiles(pendingFilesToDelete);
    }

    // Picks the first decoder element that exists for this camera's hardware
    // preference. recordingLaunchStringForCamera uses an explicit decoder, so
    // (unlike decodebin) there is no autoplug fallback — the candidate list
    // always ends with the software decoder for a supported codec. Returns ""
    // only for an unsupported codec (empty candidate list), for which the
    // caller's recordingLaunchStringForCamera also produces no launch string.
    static std::string resolveMotionDecoder(const stream::CameraRuntimeConfig& camera,
                                            stream::StreamCodec codec) {
        for (const auto& name : recording::motionDecoderCandidates(camera.hardware, codec)) {
            GstElementFactory* factory = gst_element_factory_find(name.c_str());
            if (factory) {
                gst_object_unref(factory);
                return name;
            }
        }
        return {};
    }

private:
```

- [ ] **Step 2: Add the motion-factory members to `CameraStreamSession`**

In `src/service/CameraStreamSession.hpp`, find the member declarations:

```cpp
    GstRTSPMediaFactory* m_factory = nullptr;
    std::string m_mountPath;
```

and replace them with:

```cpp
    GstRTSPMediaFactory* m_factory = nullptr;
    GstRTSPMediaFactory* m_motionFactory = nullptr;
    std::string m_mountPath;
    std::string m_motionMountPath;
```

- [ ] **Step 3: Initialise `m_motionMountPath` in the constructor**

In the constructor initialiser list, find:

```cpp
          m_mountPath(stream::mountPathForCamera(m_camera.id)),
          m_statusSink(std::move(statusSink)),
```

and replace with:

```cpp
          m_mountPath(stream::mountPathForCamera(m_camera.id)),
          m_motionMountPath(m_mountPath + "/motion"),
          m_statusSink(std::move(statusSink)),
```

- [ ] **Step 4: Keep `m_motionMountPath` in sync in `restart()`**

In `restart()`, find:

```cpp
            m_mountPath = stream::mountPathForCamera(m_camera.id);
        }
        start();
```

and replace with:

```cpp
            m_mountPath = stream::mountPathForCamera(m_camera.id);
            m_motionMountPath = m_mountPath + "/motion";
        }
        start();
```

- [ ] **Step 5: Register the motion factory in `start()`**

In `start()`, find:

```cpp
            gst_rtsp_mount_points_add_factory(m_mounts, m_mountPath.c_str(), factory);

            m_codec = probe.codec;
```

and replace with:

```cpp
            gst_rtsp_mount_points_add_factory(m_mounts, m_mountPath.c_str(), factory);

            // Second, on-demand mount: the camera video with the motioncells
            // overlay. Its pipeline is instantiated by gst-rtsp-server only
            // while a client is connected, so it is free when nobody watches.
            const auto motionDebugLaunch = recording::motionDebugLaunchStringForCamera(
                m_config, m_camera, probe.codec,
                CameraRecordingSession::resolveMotionDecoder(m_camera, probe.codec));
            if (!motionDebugLaunch.empty()) {
                auto* motionFactory = gst_rtsp_media_factory_new();
                gst_rtsp_media_factory_set_launch(motionFactory, motionDebugLaunch.c_str());
                gst_rtsp_media_factory_set_shared(motionFactory, TRUE);
                gst_rtsp_media_factory_set_suspend_mode(motionFactory, GST_RTSP_SUSPEND_MODE_NONE);
                m_motionFactory = GST_RTSP_MEDIA_FACTORY(g_object_ref(motionFactory));
                gst_rtsp_mount_points_add_factory(m_mounts, m_motionMountPath.c_str(), motionFactory);
            }

            m_codec = probe.codec;
```

- [ ] **Step 6: Remove the motion mount in `cleanupMountLocked()`**

In `cleanupMountLocked()`, find:

```cpp
        if (m_mounts && !m_mountPath.empty()) {
            gst_rtsp_mount_points_remove_factory(m_mounts, m_mountPath.c_str());
        }
        if (m_factory) {
            g_object_unref(m_factory);
            m_factory = nullptr;
        }
    }
```

and replace with:

```cpp
        if (m_mounts && !m_mountPath.empty()) {
            gst_rtsp_mount_points_remove_factory(m_mounts, m_mountPath.c_str());
        }
        if (m_mounts && !m_motionMountPath.empty()) {
            gst_rtsp_mount_points_remove_factory(m_mounts, m_motionMountPath.c_str());
        }
        if (m_factory) {
            g_object_unref(m_factory);
            m_factory = nullptr;
        }
        if (m_motionFactory) {
            g_object_unref(m_motionFactory);
            m_motionFactory = nullptr;
        }
    }
```

- [ ] **Step 7: Build and run unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 8: Commit**

```bash
git add src/service/CameraRecordingSession.hpp src/service/CameraStreamSession.hpp
git commit -m "feat(stream): serve a per-camera motion-debug RTSP mount"
```

End the commit message with this trailer on its own line after a blank line:
`Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`

---

## Task 3: Document the endpoint in the README

**Files:**
- Modify: `README.md`

- [ ] **Step 1: Append a documentation section**

Read `README.md` first. Append this section at the end of the file (note:
`README.md` may already have unrelated uncommitted edits — that is fine, leave
them and just add this section):

```markdown

## Luồng debug motion (RTSP)

Mỗi camera có một mount RTSP thứ 2 để quan sát motion detection:

```
rtsp://<publicRtspHost>:<rtspPort>/cameras/<id>/motion
```

Mở bằng VLC sẽ thấy video kèm overlay `motioncells` — ô lưới nào phát hiện
chuyển động sẽ được tô màu. Đây chính là quy ước: URL luồng live cộng thêm
`/motion`.

Luồng này chạy theo nhu cầu — pipeline chỉ được dựng khi có client kết nối và
tháo khi client cuối rời đi, nên không tốn CPU khi không ai xem. motioncells
phân tích ở 320px/5fps đúng như nhánh recording, dùng `sensitivity`/`threshold`
của camera, nên phản ánh đúng hành vi production.
```

- [ ] **Step 2: Verify the markdown**

Run: `grep -n "cameras/<id>/motion" README.md`
Expected: one matching line.

- [ ] **Step 3: Commit**

```bash
git add README.md
git commit -m "docs: document the motion-debug RTSP endpoint"
```

End the commit message with this trailer on its own line after a blank line:
`Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`

---

## Operator verification (manual, after the plan)

The motion-debug factory only runs its pipeline when a client connects, so it
cannot be exercised without a real camera. After the build:

1. Start the server (`./build.sh run`) with at least one camera configured.
2. Open `rtsp://127.0.0.1:8554/cameras/<id>/motion` in VLC.
3. Confirm the video plays with the `motioncells` grid overlaid, and that cells
   light up where there is movement.
4. Confirm the live mount `rtsp://127.0.0.1:8554/cameras/<id>` and recording are
   unaffected.

---

## Self-review notes

- **Spec coverage:** the `/motion` RTSP mount → Task 2; the launch string with
  motioncells/320p/5fps/x264enc → Task 1; decoder reuse via public
  `resolveMotionDecoder` → Task 2 Step 1; README documentation → Task 3. All
  spec sections covered.
- **Build stays green:** Task 1 adds an unused inline function (compiles). Task 2
  moves `resolveMotionDecoder` (still callable everywhere) and adds the factory
  registration plus its members/initialisers together in one task.
- **Names:** `motionDebugLaunchStringForCamera`, `resolveMotionDecoder`,
  `m_motionFactory`, `m_motionMountPath`, mount suffix `/motion`, element name
  `motion_dbg_src`, payloader `name=pay0` — used consistently.
