# Motion Detection CPU Reduction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cut CPU usage of the motion-detection branch by giving each camera its own hardware-decoder choice and an opt-in keyframe-only analysis mode.

**Architecture:** Part A replaces `decodebin` in the recording pipeline's motion branch with an explicit decoder element chosen from the camera's existing `hardware` field. Part B adds a per-camera `motionKeyframeOnly` flag (new DB column) that installs a pad probe dropping non-keyframe buffers before the decoder.

**Tech Stack:** C++20, GStreamer 1.20, Oat++, PostgreSQL, CTest.

**Spec:** `docs/superpowers/specs/2026-05-20-motion-cpu-reduction-design.md`

---

## Notes for the implementer

- The unit-test target `stream_logic_tests` does **not** link GStreamer. Only pure
  functions in `RecordingTypes.hpp` / `StreamTypes.hpp` can be unit-tested there.
  GStreamer-dependent code is verified with one-off probe programs (Tasks 2, 7).
- Build everything with: `cmake --build build -j"$(nproc)"`
- Run unit tests with: `ctest --test-dir build --output-on-failure`
- Keep the build green after every task — both `test_gstreamer` and
  `stream_logic_tests` must compile.
- `motionKeyframeOnly` is the C++/DTO/JSON name; `motion_keyframe_only` is the DB
  column name. Use exactly these spellings.

---

## Chunk 1: Part A — Per-camera hardware decode

### Task 1: Add `motionDecoderCandidates()` helper

**Files:**
- Modify: `src/service/RecordingTypes.hpp`
- Test: `tests/StreamLogicTests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/StreamLogicTests.cpp`, add `#include <vector>` after the existing
`#include <string>` line (top of file), so the include block reads:

```cpp
#include "service/StreamTypes.hpp"
#include "service/RecordingTypes.hpp"

#include <cassert>
#include <string>
#include <vector>
```

Add this test function just before the closing `}` of the anonymous namespace
(after `testRecordingLaunchStringBuildsExpectedBranches`):

```cpp
void testMotionDecoderCandidates() {
    using SC = stream::StreamCodec;

    assert((recording::motionDecoderCandidates("software", SC::H264)
            == std::vector<std::string>{"avdec_h264"}));
    assert((recording::motionDecoderCandidates("vaapi", SC::H264)
            == std::vector<std::string>{"vah264dec", "avdec_h264"}));
    assert((recording::motionDecoderCandidates("nvdec", SC::H265)
            == std::vector<std::string>{"nvh265dec", "avdec_h265"}));
    assert((recording::motionDecoderCandidates("v4l2", SC::H264)
            == std::vector<std::string>{"v4l2h264dec", "avdec_h264"}));

    const auto autoList = recording::motionDecoderCandidates("auto", SC::H264);
    assert(autoList.size() == 4);
    assert(autoList.front() == "vah264dec");
    assert(autoList.back() == "avdec_h264");

    // Unknown / empty values behave like "auto".
    assert(recording::motionDecoderCandidates("banana", SC::H264)
           == recording::motionDecoderCandidates("auto", SC::H264));
    assert(recording::motionDecoderCandidates("", SC::H265)
           == recording::motionDecoderCandidates("auto", SC::H265));

    // Every list ends with a software decoder so resolution always succeeds.
    assert(recording::motionDecoderCandidates("vaapi", SC::H265).back() == "avdec_h265");
}
```

Register it in `main()` — add this line after the existing
`testRecordingLaunchStringBuildsExpectedBranches();` call:

```cpp
    testMotionDecoderCandidates();
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -20`
Expected: compile error — `motionDecoderCandidates` is not a member of `recording`.

- [ ] **Step 3: Implement `motionDecoderCandidates()`**

In `src/service/RecordingTypes.hpp`, add `#include <vector>` to the include
block (after `#include <string>`):

```cpp
#include <algorithm>
#include <cstdint>
#include <functional>
#include <sstream>
#include <string>
#include <vector>
```

Add this function inside `namespace recording`, immediately before
`recordingFilePattern`:

```cpp
// Ordered preference list of decoder element names for the motion branch.
// Every list ends with the software decoder so resolution always succeeds.
inline std::vector<std::string> motionDecoderCandidates(const std::string& hardware,
                                                        stream::StreamCodec codec) {
    const bool h264 = codec == stream::StreamCodec::H264;
    const std::string va  = h264 ? "vah264dec"   : "vah265dec";
    const std::string nv  = h264 ? "nvh264dec"   : "nvh265dec";
    const std::string v4l = h264 ? "v4l2h264dec" : "v4l2h265dec";
    const std::string sw  = h264 ? "avdec_h264"  : "avdec_h265";

    const auto mode = stream::toLower(hardware);
    if (mode == "software" || mode == "sw" || mode == "none" || mode == "off") {
        return {sw};
    }
    if (mode == "vaapi" || mode == "va") {
        return {va, sw};
    }
    if (mode == "nvdec" || mode == "nvenc" || mode == "nvidia" || mode == "cuda") {
        return {nv, sw};
    }
    if (mode == "v4l2") {
        return {v4l, sw};
    }
    return {va, nv, v4l, sw};  // auto / empty / unknown
}
```

- [ ] **Step 4: Run the test to verify it passes**

Run: `cmake --build build -j"$(nproc)" && ./build/stream_logic_tests; echo "exit=$?"`
Expected: build succeeds, `exit=0`.

- [ ] **Step 5: Commit**

```bash
git add src/service/RecordingTypes.hpp tests/StreamLogicTests.cpp
git commit -m "feat(recording): add motionDecoderCandidates helper"
```

---

### Task 2: Use an explicit motion-branch decoder

**Files:**
- Modify: `src/service/RecordingTypes.hpp` (`recordingLaunchStringForCamera`)
- Modify: `src/service/CameraRecordingSession.hpp` (`resolveMotionDecoder`, `startPipeline`)
- Test: `tests/StreamLogicTests.cpp`

- [ ] **Step 1: Update the failing test first**

In `tests/StreamLogicTests.cpp`, replace the whole body of
`testRecordingLaunchStringBuildsExpectedBranches()` with this version (it passes
the new `motionDecoder` argument and asserts the new launch-string shape):

```cpp
void testRecordingLaunchStringBuildsExpectedBranches() {
    stream::GStreamerConfig config;
    config.sourceLatencyMs = 100;
    config.recordingDir = "recordings";

    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a";
    camera.rtsp = "rtsp://example.local/stream1";
    camera.recordingMode = recording::toString(recording::RecordingMode::Always);
    camera.segmentSeconds = 10;

    const auto alwaysLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(alwaysLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(alwaysLaunch.find("message-forward=true") != std::string::npos);
    assert(alwaysLaunch.find("motioncells") == std::string::npos);
    assert(alwaysLaunch.find("max-size-time=10000000000") != std::string::npos);
    // The recording launch string is fed to gst_parse_launch (not an RTSP
    // media factory). A leading "(" makes gst_parse_launch return a clock-less
    // GstBin instead of a GstPipeline, and the recording never runs.
    assert(!alwaysLaunch.empty() && alwaysLaunch.front() != '(');
    assert(alwaysLaunch.back() != ')');

    camera.motionEnabled = true;
    const auto alwaysWithMotionIndexLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264");
    assert(alwaysWithMotionIndexLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("motioncells name=motion_detector") != std::string::npos);
    // Motion branch uses an explicit decoder, a named queue, and drop-only rate.
    assert(alwaysWithMotionIndexLaunch.find("decodebin") == std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("queue name=motion_q") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("videorate drop-only=true") != std::string::npos);
    assert(alwaysWithMotionIndexLaunch.find("! avdec_h264 !") != std::string::npos);

    const auto fallbackLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H264, "avdec_h264", false);
    assert(fallbackLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(fallbackLaunch.find("motioncells") == std::string::npos);
    assert(fallbackLaunch.find("queue name=motion_q") == std::string::npos);

    camera.motionEnabled = false;
    camera.recordingMode = recording::toString(recording::RecordingMode::Motion);
    const auto motionLaunch = recording::recordingLaunchStringForCamera(
        config, camera, stream::StreamCodec::H265, "vah265dec");
    assert(motionLaunch.find("splitmuxsink name=record_sink") != std::string::npos);
    assert(motionLaunch.find("motioncells name=motion_detector") != std::string::npos);
    assert(motionLaunch.find("video/x-raw,format=RGB") != std::string::npos);
    assert(motionLaunch.find("! vah265dec !") != std::string::npos);
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -20`
Expected: compile error — `recordingLaunchStringForCamera` called with 4/5
arguments but the current signature takes 3/4.

- [ ] **Step 3: Update `recordingLaunchStringForCamera`**

In `src/service/RecordingTypes.hpp`, change the function signature from:

```cpp
inline std::string recordingLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                  const stream::CameraRuntimeConfig& camera,
                                                  stream::StreamCodec codec,
                                                  bool includeMotionBranch = true) {
```

to:

```cpp
inline std::string recordingLaunchStringForCamera(const stream::GStreamerConfig& config,
                                                  const stream::CameraRuntimeConfig& camera,
                                                  stream::StreamCodec codec,
                                                  const std::string& motionDecoder,
                                                  bool includeMotionBranch = true) {
```

In the same function, replace the motion-branch block:

```cpp
    if (includeMotionBranch && (mode == RecordingMode::Motion || camera.motionEnabled)) {
        launch
            << " record_t. ! queue leaky=downstream max-size-buffers=2"
            << " ! decodebin"
            << " ! videorate"
            << " ! video/x-raw,framerate=5/1"
```

with:

```cpp
    if (includeMotionBranch && (mode == RecordingMode::Motion || camera.motionEnabled)) {
        launch
            << " record_t. ! queue name=motion_q leaky=downstream max-size-buffers=2"
            << " ! " << motionDecoder
            << " ! videorate drop-only=true"
            << " ! video/x-raw,framerate=5/1"
```

(the remaining lines of that block — `videoscale`, `videoconvert`,
`motioncells`, `fakesink` — are unchanged.)

- [ ] **Step 4: Add `resolveMotionDecoder()` to `CameraRecordingSession`**

In `src/service/CameraRecordingSession.hpp`, add this private static method
immediately after the `consumeGError` method (in the `private:` section):

```cpp
    // Picks the first decoder element that exists for this camera's hardware
    // preference. recordingLaunchStringForCamera uses an explicit decoder, so
    // (unlike decodebin) there is no autoplug fallback — the candidate list
    // always ends with the software decoder, which is guaranteed to exist.
    static std::string resolveMotionDecoder(const stream::CameraRuntimeConfig& camera,
                                            stream::StreamCodec codec) {
        for (const auto& name : recording::motionDecoderCandidates(camera.hardware, codec)) {
            GstElementFactory* factory = gst_element_factory_find(name.c_str());
            if (factory) {
                gst_object_unref(factory);
                return name;
            }
        }
        return codec == stream::StreamCodec::H264 ? "avdec_h264" : "avdec_h265";
    }
```

- [ ] **Step 5: Pass the resolved decoder in `startPipeline`**

In `src/service/CameraRecordingSession.hpp`, inside `startPipeline`, replace:

```cpp
        const auto launch =
            recording::recordingLaunchStringForCamera(
                m_config, m_camera, m_codec, includeMotionBranch);
```

with:

```cpp
        const auto launch =
            recording::recordingLaunchStringForCamera(
                m_config, m_camera, m_codec,
                resolveMotionDecoder(m_camera, m_codec),
                includeMotionBranch);
```

- [ ] **Step 6: Build and run unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 7: GStreamer verification probe**

Run this probe — it confirms the launch string parses to a real `GstPipeline`
for an explicit software decoder, and for the VA-API decoder when present:

```bash
cat > /tmp/dec_probe.cpp <<'EOF'
#include "service/RecordingTypes.hpp"
#include <gst/gst.h>
#include <iostream>
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    stream::GStreamerConfig config; config.recordingDir = "/tmp/rec";
    stream::CameraRuntimeConfig camera;
    camera.id = "cam-a"; camera.rtsp = "rtsp://example.local/s";
    camera.recordingMode = "motion";
    int hardFails = 0;
    struct { const char* dec; stream::StreamCodec codec; bool required; } cases[] = {
        {"avdec_h264", stream::StreamCodec::H264, true},
        {"vah264dec",  stream::StreamCodec::H264, false},
    };
    for (auto& c : cases) {
        if (!c.required && !gst_element_factory_find(c.dec)) {
            std::cout << "decoder=" << c.dec << " (absent, skipped)\n";
            continue;
        }
        auto launch = recording::recordingLaunchStringForCamera(
            config, camera, c.codec, c.dec, true);
        GError* e = nullptr;
        GstElement* p = gst_parse_launch(launch.c_str(), &e);
        bool ok = p && GST_IS_PIPELINE(p) && !e;
        std::cout << "decoder=" << c.dec << " parse=" << (ok ? "OK" : "FAIL")
                  << (e ? std::string(" err=") + e->message : "") << "\n";
        if (!ok && c.required) ++hardFails;
        if (e) g_error_free(e);
        if (p) gst_object_unref(p);
    }
    return hardFails;
}
EOF
g++ -std=c++20 -I src /tmp/dec_probe.cpp -o /tmp/dec_probe $(pkg-config --cflags --libs gstreamer-1.0) \
  && /tmp/dec_probe; echo "exit=$?"
rm -f /tmp/dec_probe /tmp/dec_probe.cpp
```

Expected: `decoder=avdec_h264 parse=OK`, `decoder=vah264dec parse=OK` (or
`(absent, skipped)` on a machine without VA-API), `exit=0`.

- [ ] **Step 8: Commit**

```bash
git add src/service/RecordingTypes.hpp src/service/CameraRecordingSession.hpp tests/StreamLogicTests.cpp
git commit -m "feat(recording): use an explicit per-camera motion decoder"
```

---

## Chunk 2: Part B — Per-camera keyframe-only mode

### Task 3: Add the `motion_keyframe_only` database column

**Files:**
- Modify: `sql/001_init_cameras.sql`

- [ ] **Step 1: Add the column to `CREATE TABLE`**

In `sql/001_init_cameras.sql`, in the `CREATE TABLE IF NOT EXISTS cameras`
block, change:

```sql
  segment_seconds   INTEGER      NOT NULL DEFAULT 10,
  retry_count       INTEGER      NOT NULL DEFAULT 0,
```

to:

```sql
  segment_seconds   INTEGER      NOT NULL DEFAULT 10,
  motion_keyframe_only BOOLEAN   NOT NULL DEFAULT false,
  retry_count       INTEGER      NOT NULL DEFAULT 0,
```

- [ ] **Step 2: Add the idempotent migration**

In the same file, after the line:

```sql
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS segment_seconds INTEGER NOT NULL DEFAULT 10;
```

add:

```sql
ALTER TABLE IF EXISTS cameras ADD COLUMN IF NOT EXISTS motion_keyframe_only BOOLEAN NOT NULL DEFAULT false;
```

- [ ] **Step 3: Verify the file**

Run: `grep -n motion_keyframe_only sql/001_init_cameras.sql`
Expected: two lines — one in `CREATE TABLE`, one `ALTER TABLE`.

- [ ] **Step 4: Commit**

```bash
git add sql/001_init_cameras.sql
git commit -m "feat(db): add motion_keyframe_only column to cameras"
```

---

### Task 4: Add `motionKeyframeOnly` to DTOs and runtime config

**Files:**
- Modify: `src/dto/CameraDto.hpp`
- Modify: `src/service/StreamTypes.hpp`

- [ ] **Step 1: Add the field to `CameraDto`**

In `src/dto/CameraDto.hpp`, inside `class CameraDto`, after the `segmentSeconds`
field block:

```cpp
    DTO_FIELD_INFO(segmentSeconds) { info->description = "Recording segment duration in seconds"; }
    DTO_FIELD(UInt32, segmentSeconds);
```

add:

```cpp
    DTO_FIELD_INFO(motionKeyframeOnly) { info->description = "Analyze motion on keyframes only (lower CPU)"; }
    DTO_FIELD(Boolean, motionKeyframeOnly);
```

- [ ] **Step 2: Add the field to `CreateCameraDto`**

In the same file, inside `class CreateCameraDto`, after:

```cpp
    DTO_FIELD(UInt32, segmentSeconds);
```

add:

```cpp
    DTO_FIELD(Boolean, motionKeyframeOnly);
```

- [ ] **Step 3: Add the field to `CameraRuntimeConfig`**

In `src/service/StreamTypes.hpp`, inside `struct CameraRuntimeConfig`, change:

```cpp
    uint32_t segmentSeconds = 10;
};
```

to:

```cpp
    uint32_t segmentSeconds = 10;
    bool motionKeyframeOnly = false;
};
```

- [ ] **Step 4: Build to verify it compiles**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -6`
Expected: build succeeds (the new fields are unused so far).

- [ ] **Step 5: Commit**

```bash
git add src/dto/CameraDto.hpp src/service/StreamTypes.hpp
git commit -m "feat(camera): add motionKeyframeOnly DTO and runtime fields"
```

---

### Task 5: Wire `motionKeyframeOnly` through the database layer

**Files:**
- Modify: `src/db/CameraDb.hpp`
- Modify: `src/service/CameraService.hpp`

- [ ] **Step 1: Update `createCamera`**

In `src/db/CameraDb.hpp`, in the `createCamera` QUERY:

Change the column list — replace:

```
          "  post_motion_seconds, segment_seconds, retry_count, last_error, last_changed_at"
```

with:

```
          "  post_motion_seconds, segment_seconds, motion_keyframe_only, "
          "  retry_count, last_error, last_changed_at"
```

Change the VALUES — replace:

```
          "  COALESCE(:segmentSeconds, 10), 0, '', ''"
```

with:

```
          "  COALESCE(:segmentSeconds, 10), COALESCE(:motionKeyframeOnly, false), 0, '', ''"
```

Change the RETURNING list — replace:

```
          "segment_seconds AS \"segmentSeconds\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
```

with:

```
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
```

Add the parameter — replace:

```cpp
          PARAM(oatpp::UInt32, segmentSeconds))
```

with:

```cpp
          PARAM(oatpp::UInt32, segmentSeconds),
          PARAM(oatpp::Boolean, motionKeyframeOnly))
```

- [ ] **Step 2: Update `getCameraById`, `getAllCameras`, `getAllCamerasForStartup`, `deleteCameraReturning`**

Each of these four QUERYs contains the line:

```
          "segment_seconds AS \"segmentSeconds\", "
```

In **each** of the four, replace that line with:

```
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
```

- [ ] **Step 3: Update `updateCamera`**

In the `updateCamera` QUERY:

Add the SET assignment — replace:

```
          "  segment_seconds   = COALESCE(:segmentSeconds, segment_seconds) "
```

with:

```
          "  segment_seconds   = COALESCE(:segmentSeconds, segment_seconds), "
          "  motion_keyframe_only = COALESCE(:motionKeyframeOnly, motion_keyframe_only) "
```

Change the RETURNING list — replace:

```
          "segment_seconds AS \"segmentSeconds\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
```

with:

```
          "segment_seconds AS \"segmentSeconds\", "
          "motion_keyframe_only AS \"motionKeyframeOnly\", "
          "last_error AS \"lastError\", last_changed_at AS \"lastChangedAt\";",
```

Add the parameter — replace:

```cpp
          PARAM(oatpp::UInt32, segmentSeconds))
```

with:

```cpp
          PARAM(oatpp::UInt32, segmentSeconds),
          PARAM(oatpp::Boolean, motionKeyframeOnly))
```

- [ ] **Step 4: Update the `CameraService` call sites**

In `src/service/CameraService.hpp`, in `createCamera`, replace:

```cpp
        auto res = m_db->createCamera(in->name, in->rtsp, status,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds);
```

with:

```cpp
        auto res = m_db->createCamera(in->name, in->rtsp, status,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds, in->motionKeyframeOnly);
```

In `putCamera`, replace:

```cpp
        auto res = m_db->updateCamera(id, in->name, in->rtsp, in->status,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds);
```

with:

```cpp
        auto res = m_db->updateCamera(id, in->name, in->rtsp, in->status,
                                      in->hardware, in->recordingEnabled,
                                      in->recordingMode, in->motionEnabled,
                                      in->motionSensitivity, in->motionThreshold,
                                      in->preMotionSeconds, in->postMotionSeconds,
                                      in->segmentSeconds, in->motionKeyframeOnly);
```

- [ ] **Step 5: Build to verify it compiles**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -6`
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/db/CameraDb.hpp src/service/CameraService.hpp
git commit -m "feat(db): persist motionKeyframeOnly through camera queries"
```

---

### Task 6: Map `motionKeyframeOnly` into runtime config and restart logic

**Files:**
- Modify: `src/service/GStreamerService.hpp` (`toRuntimeConfig`)
- Modify: `src/service/RecordingTypes.hpp` (`recordingPatchRequiresRuntimeRestart`)
- Modify: `src/service/CameraService.hpp` (`streamRelevantInputPresent`)
- Test: `tests/StreamLogicTests.cpp`

- [ ] **Step 1: Update the failing test first**

In `tests/StreamLogicTests.cpp`, replace the body of
`testRecordingPatchPresenceRequiresRuntimeRestart()` with:

```cpp
void testRecordingPatchPresenceRequiresRuntimeRestart() {
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, true, false, false, false, false, false, false, false));
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, false, true, false, false, false, false, false, false));
    // The new motionKeyframeOnly flag also forces a runtime restart.
    assert(recording::recordingPatchRequiresRuntimeRestart(
        false, false, false, false, false, false, false, false, true));
    assert(!recording::recordingPatchRequiresRuntimeRestart(
        false, false, false, false, false, false, false, false, false));
}
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -20`
Expected: compile error — `recordingPatchRequiresRuntimeRestart` called with 9
arguments but the current signature takes 8.

- [ ] **Step 3: Update `recordingPatchRequiresRuntimeRestart`**

In `src/service/RecordingTypes.hpp`, replace the whole function:

```cpp
inline bool recordingPatchRequiresRuntimeRestart(bool recordingEnabledPresent,
                                                 bool recordingModePresent,
                                                 bool motionEnabledPresent,
                                                 bool motionSensitivityPresent,
                                                 bool motionThresholdPresent,
                                                 bool preMotionSecondsPresent,
                                                 bool postMotionSecondsPresent,
                                                 bool segmentSecondsPresent) {
    return recordingEnabledPresent ||
           recordingModePresent ||
           motionEnabledPresent ||
           motionSensitivityPresent ||
           motionThresholdPresent ||
           preMotionSecondsPresent ||
           postMotionSecondsPresent ||
           segmentSecondsPresent;
}
```

with:

```cpp
inline bool recordingPatchRequiresRuntimeRestart(bool recordingEnabledPresent,
                                                 bool recordingModePresent,
                                                 bool motionEnabledPresent,
                                                 bool motionSensitivityPresent,
                                                 bool motionThresholdPresent,
                                                 bool preMotionSecondsPresent,
                                                 bool postMotionSecondsPresent,
                                                 bool segmentSecondsPresent,
                                                 bool motionKeyframeOnlyPresent) {
    return recordingEnabledPresent ||
           recordingModePresent ||
           motionEnabledPresent ||
           motionSensitivityPresent ||
           motionThresholdPresent ||
           preMotionSecondsPresent ||
           postMotionSecondsPresent ||
           segmentSecondsPresent ||
           motionKeyframeOnlyPresent;
}
```

- [ ] **Step 4: Update `streamRelevantInputPresent`**

In `src/service/CameraService.hpp`, replace:

```cpp
    static bool streamRelevantInputPresent(const oatpp::Object<CreateCameraDto>& in) {
        return in->rtsp || in->hardware ||
               recording::recordingPatchRequiresRuntimeRestart(
                   static_cast<bool>(in->recordingEnabled),
                   static_cast<bool>(in->recordingMode),
                   static_cast<bool>(in->motionEnabled),
                   static_cast<bool>(in->motionSensitivity),
                   static_cast<bool>(in->motionThreshold),
                   static_cast<bool>(in->preMotionSeconds),
                   static_cast<bool>(in->postMotionSeconds),
                   static_cast<bool>(in->segmentSeconds));
    }
```

with:

```cpp
    static bool streamRelevantInputPresent(const oatpp::Object<CreateCameraDto>& in) {
        return in->rtsp || in->hardware ||
               recording::recordingPatchRequiresRuntimeRestart(
                   static_cast<bool>(in->recordingEnabled),
                   static_cast<bool>(in->recordingMode),
                   static_cast<bool>(in->motionEnabled),
                   static_cast<bool>(in->motionSensitivity),
                   static_cast<bool>(in->motionThreshold),
                   static_cast<bool>(in->preMotionSeconds),
                   static_cast<bool>(in->postMotionSeconds),
                   static_cast<bool>(in->segmentSeconds),
                   static_cast<bool>(in->motionKeyframeOnly));
    }
```

- [ ] **Step 5: Map the field in `toRuntimeConfig`**

In `src/service/GStreamerService.hpp`, in `toRuntimeConfig`, replace:

```cpp
        if (dto->segmentSeconds) out.segmentSeconds = *dto->segmentSeconds;
        return out;
```

with:

```cpp
        if (dto->segmentSeconds) out.segmentSeconds = *dto->segmentSeconds;
        if (dto->motionKeyframeOnly) out.motionKeyframeOnly = *dto->motionKeyframeOnly;
        return out;
```

- [ ] **Step 6: Build and run unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 7: Commit**

```bash
git add src/service/RecordingTypes.hpp src/service/CameraService.hpp src/service/GStreamerService.hpp tests/StreamLogicTests.cpp
git commit -m "feat(camera): restart runtime when motionKeyframeOnly changes"
```

---

### Task 7: Drop non-keyframe buffers when `motionKeyframeOnly` is set

**Files:**
- Modify: `src/service/CameraRecordingSession.hpp`

- [ ] **Step 1: Add the drop probe and installer**

In `src/service/CameraRecordingSession.hpp`, add these two private static
methods immediately after `resolveMotionDecoder` (added in Task 2):

```cpp
    // Stateless pad probe: drops encoded P/B frames so the motion decoder only
    // processes IDR keyframes. No userData, so nothing to outlive — the probe
    // is released together with the pipeline.
    static GstPadProbeReturn dropDeltaFramesProbe(GstPad*, GstPadProbeInfo* info, gpointer) {
        GstBuffer* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (buffer && GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) {
            return GST_PAD_PROBE_DROP;
        }
        return GST_PAD_PROBE_OK;
    }

    static void installKeyframeOnlyProbe(GstElement* pipeline) {
        GstElement* queue = gst_bin_get_by_name(GST_BIN(pipeline), "motion_q");
        if (!queue) return;  // no motion branch in this pipeline
        GstPad* pad = gst_element_get_static_pad(queue, "sink");
        if (pad) {
            gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER,
                              &CameraRecordingSession::dropDeltaFramesProbe,
                              nullptr, nullptr);
            gst_object_unref(pad);
        }
        gst_object_unref(queue);
    }
```

- [ ] **Step 2: Install the probe in `startPipeline`**

In `src/service/CameraRecordingSession.hpp`, in `startPipeline`, find the block
that creates the bus watch. Immediately **after** this closing brace of the
`if (bus) { ... }` block:

```cpp
            gst_object_unref(bus);
        }
```

add:

```cpp

        if (includeMotionBranch && m_camera.motionKeyframeOnly) {
            installKeyframeOnlyProbe(pipeline);
        }
```

- [ ] **Step 3: Build**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -6`
Expected: build succeeds.

- [ ] **Step 4: GStreamer verification probe**

Run this probe — it builds a pipeline with a named `motion_q`, installs the
same drop probe, and confirms only keyframes pass through:

```bash
cat > /tmp/kf_probe.cpp <<'EOF'
#include <gst/gst.h>
#include <iostream>
static int g_passed = 0;
static GstPadProbeReturn dropDelta(GstPad*, GstPadProbeInfo* info, gpointer) {
    GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
    if (b && GST_BUFFER_FLAG_IS_SET(b, GST_BUFFER_FLAG_DELTA_UNIT))
        return GST_PAD_PROBE_DROP;
    return GST_PAD_PROBE_OK;
}
static GstPadProbeReturn count(GstPad*, GstPadProbeInfo*, gpointer) { ++g_passed; return GST_PAD_PROBE_OK; }
int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    GError* e = nullptr;
    GstElement* p = gst_parse_launch(
        "videotestsrc num-buffers=60 ! video/x-raw,width=320,height=240 "
        "! x264enc key-int-max=10 ! h264parse "
        "! queue name=motion_q ! avdec_h264 ! fakesink name=sink", &e);
    if (!p || e) { std::cout << "parse FAIL " << (e?e->message:"") << "\n"; return 1; }
    GstElement* q = gst_bin_get_by_name(GST_BIN(p), "motion_q");
    GstPad* qpad = gst_element_get_static_pad(q, "sink");
    gst_pad_add_probe(qpad, GST_PAD_PROBE_TYPE_BUFFER, &dropDelta, nullptr, nullptr);
    gst_object_unref(qpad); gst_object_unref(q);
    GstElement* s = gst_bin_get_by_name(GST_BIN(p), "sink");
    GstPad* spad = gst_element_get_static_pad(s, "sink");
    gst_pad_add_probe(spad, GST_PAD_PROBE_TYPE_BUFFER, &count, nullptr, nullptr);
    gst_object_unref(spad); gst_object_unref(s);
    gst_element_set_state(p, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(p);
    gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE,
        (GstMessageType)(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    gst_object_unref(bus);
    gst_element_set_state(p, GST_STATE_NULL);
    gst_object_unref(p);
    std::cout << "frames reaching decoder: " << g_passed
              << " (expected ~6 of 60 = keyframes only)\n";
    return (g_passed > 0 && g_passed <= 12) ? 0 : 1;
}
EOF
g++ -std=c++20 /tmp/kf_probe.cpp -o /tmp/kf_probe $(pkg-config --cflags --libs gstreamer-1.0) \
  && /tmp/kf_probe; echo "exit=$?"
rm -f /tmp/kf_probe /tmp/kf_probe.cpp
```

Expected: `frames reaching decoder: 6` (or a similar small number ≤ 12),
`exit=0`.

- [ ] **Step 5: Commit**

```bash
git add src/service/CameraRecordingSession.hpp
git commit -m "feat(recording): keyframe-only motion analysis when enabled"
```

---

## Chunk 3: Cleanup and verification

### Task 8: Remove obsolete config keys and verify the whole feature

**Files:**
- Modify: `config/config.json`

- [ ] **Step 1: Remove the obsolete config keys**

Open `config/config.json`. In the `gstreamer` object, delete the two lines:

```json
    "motionHardwareDecode": true,
    "motionKeyframeOnly": true
```

These were placeholders; the settings are now per-camera (via the camera API),
not global config. Ensure the JSON remains valid — the property before them
must not end with a dangling comma. The `gstreamer` block should end:

```json
    "defaultHardware": "auto",
    "recordingEnabled": false,
    "recordingDir": "recordings"
  }
```

- [ ] **Step 2: Verify the JSON is valid**

Run: `python3 -c "import json; json.load(open('config/config.json')); print('config.json OK')"`
Expected: `config.json OK`.

- [ ] **Step 3: Full build and unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 4: Whitespace check**

Run: `git diff --check`
Expected: no output.

- [ ] **Step 5: Commit**

```bash
git add config/config.json
git commit -m "chore(config): drop obsolete global motion-decode keys"
```

- [ ] **Step 6: Apply the database migration (manual / operator step)**

The app does not run SQL automatically. Before running the server against an
existing database, apply the migration so the `motion_keyframe_only` column
exists:

```bash
psql "postgresql://oryza:Oryza%40123@localhost:5433/test_gstreamer" -f sql/001_init_cameras.sql
```

(The `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` statements make this safe to
re-run.)

- [ ] **Step 7: Manual end-to-end verification (operator step)**

With a real camera and database:

1. Set a camera to motion recording with software decode and keyframe-only off:
   `PUT /cameras/{id}` body `{"recordingMode":"motion","hardware":"software","motionKeyframeOnly":false}` —
   note CPU usage of the process.
2. Switch to hardware decode: `PUT /cameras/{id}` body `{"hardware":"auto"}` —
   on a GPU machine, CPU should drop sharply.
3. Switch to keyframe-only: `PUT /cameras/{id}` body `{"motionKeyframeOnly":true}` —
   CPU should drop on machines without a GPU.
4. Confirm motion events are still recorded in `motion_events` and segments in
   `recording_segments`.

---

## Self-review notes

- **Spec coverage:** Part A (explicit decoder) → Tasks 1–2. Part B (keyframe-only)
  → Tasks 3–7. Config cleanup → Task 8. `recordingPatchRequiresRuntimeRestart`
  parameter → Task 6. All spec sections are covered.
- **Build stays green:** signature changes to `recordingLaunchStringForCamera`
  (Task 2) and `recordingPatchRequiresRuntimeRestart` (Task 6) are made together
  with their call sites within the same task; DB query signature changes and
  their `CameraService` call sites are both in Task 5.
- **Names:** `motionKeyframeOnly` (C++/DTO/JSON), `motion_keyframe_only` (DB
  column), `motion_q` (element), `motionDecoderCandidates`, `resolveMotionDecoder`,
  `dropDeltaFramesProbe`, `installKeyframeOnlyProbe` — used consistently.
