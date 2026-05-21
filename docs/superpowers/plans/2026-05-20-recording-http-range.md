# Recording File HTTP Range Support Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `GET /recording-segments/{id}/file` honour HTTP `Range` requests so recordings play and seek in a browser video player.

**Architecture:** A new pure header parses the `Range` header into a byte span; the controller endpoint uses it to return `206 Partial Content` (reading only the requested bytes) or `200` (whole file) or `416`.

**Tech Stack:** C++20, Oat++ 1.3.0, CTest.

**Spec:** `docs/superpowers/specs/2026-05-20-recording-http-range-design.md`

---

## Notes for the implementer

- Build: `cmake --build build -j"$(nproc)"`  ·  Unit tests: `ctest --test-dir build --output-on-failure`
- `src/http/ByteRange.hpp` is a new **header-only** file — no CMake change is needed (`target_include_directories` already exposes `src/` to both targets).
- The unit-test target `stream_logic_tests` does not link Oat++ or GStreamer — `parseByteRange` must stay pure (it does: only `<string>` / `<cstdint>`).
- The endpoint itself cannot be unit-tested in this repo's harness; it is verified by a green build plus the manual `curl` checks at the end.

---

## Task 1: Pure `Range` header parser

**Files:**
- Create: `src/http/ByteRange.hpp`
- Test: `tests/StreamLogicTests.cpp`

- [ ] **Step 1: Write the failing test**

In `tests/StreamLogicTests.cpp`, add this include after the existing
`#include "service/RecordingTypes.hpp"` line:

```cpp
#include "http/ByteRange.hpp"
```

Add this test function just before the closing `}` of the anonymous namespace
(after `testMotionMessageClassification`):

```cpp
void testParseByteRange() {
    const int64_t size = 1000;  // a pretend 1000-byte file: valid bytes 0..999

    // Missing / non-"bytes=" / unparseable / multi-range -> not present.
    assert(!http::parseByteRange("", size).present);
    assert(!http::parseByteRange("seconds=0-1", size).present);
    assert(!http::parseByteRange("bytes=abc", size).present);
    assert(!http::parseByteRange("bytes=100-abc", size).present);
    assert(!http::parseByteRange("bytes=0-99,200-299", size).present);

    // bytes=0- -> the whole file.
    {
        auto r = http::parseByteRange("bytes=0-", size);
        assert(r.present && r.satisfiable && r.start == 0 && r.end == 999);
    }
    // bytes=100-199 -> an exact span.
    {
        auto r = http::parseByteRange("bytes=100-199", size);
        assert(r.present && r.satisfiable && r.start == 100 && r.end == 199);
    }
    // END past end-of-file -> clamped to the last byte.
    {
        auto r = http::parseByteRange("bytes=900-99999", size);
        assert(r.present && r.satisfiable && r.start == 900 && r.end == 999);
    }
    // Suffix range bytes=-500 -> the last 500 bytes.
    {
        auto r = http::parseByteRange("bytes=-500", size);
        assert(r.present && r.satisfiable && r.start == 500 && r.end == 999);
    }
    // Suffix larger than the file -> the whole file.
    {
        auto r = http::parseByteRange("bytes=-99999", size);
        assert(r.present && r.satisfiable && r.start == 0 && r.end == 999);
    }
    // START past end-of-file -> present but not satisfiable (-> 416).
    {
        auto r = http::parseByteRange("bytes=1000-1100", size);
        assert(r.present && !r.satisfiable);
    }
    // start > end -> present but not satisfiable.
    {
        auto r = http::parseByteRange("bytes=100-50", size);
        assert(r.present && !r.satisfiable);
    }
    // Empty file -> any range unsatisfiable.
    {
        auto r = http::parseByteRange("bytes=0-", 0);
        assert(r.present && !r.satisfiable);
    }
}
```

Register it in `main()` — add this line after the existing
`testMotionMessageClassification();` call:

```cpp
    testParseByteRange();
```

- [ ] **Step 2: Run the build to verify it fails**

Run: `cmake --build build -j"$(nproc)" 2>&1 | tail -20`
Expected: compile error — `http/ByteRange.hpp` not found (`fatal error: http/ByteRange.hpp: No such file or directory`).

- [ ] **Step 3: Create `src/http/ByteRange.hpp`**

Create the file `src/http/ByteRange.hpp` with exactly this content:

```cpp
#ifndef test_gstreamer_ByteRange_hpp
#define test_gstreamer_ByteRange_hpp

#include <cstdint>
#include <string>

namespace http {

// Result of parsing an HTTP Range request header.
struct ByteRange {
    bool    present     = false;  // a parseable single "bytes=" range was supplied
    bool    satisfiable = false;  // the range is valid for the given file size
    int64_t start       = 0;      // first byte, inclusive
    int64_t end         = 0;      // last byte, inclusive
};

// Parses an HTTP Range header value (e.g. "bytes=0-1023") against a known file
// size. Only a single byte range is supported; a multi-range or unparseable
// value yields {present = false}, so the caller serves the whole file as 200
// (per RFC 7233: an unparseable Range header is ignored).
inline ByteRange parseByteRange(const std::string& rangeHeader, int64_t fileSize) {
    ByteRange result;

    const std::string prefix = "bytes=";
    if (rangeHeader.size() <= prefix.size() ||
        rangeHeader.compare(0, prefix.size(), prefix) != 0) {
        return result;  // missing, or not a "bytes=" range — ignore
    }
    const std::string spec = rangeHeader.substr(prefix.size());
    if (spec.find(',') != std::string::npos) {
        return result;  // multi-range — unsupported, ignore
    }
    const auto dash = spec.find('-');
    if (dash == std::string::npos) {
        return result;  // malformed — ignore
    }

    const std::string startStr = spec.substr(0, dash);
    const std::string endStr   = spec.substr(dash + 1);

    // A non-empty decimal string that fits safely in int64 (<= 18 digits).
    const auto isNumber = [](const std::string& s) {
        if (s.empty() || s.size() > 18) return false;
        for (char c : s) {
            if (c < '0' || c > '9') return false;
        }
        return true;
    };

    if (startStr.empty()) {
        // Suffix range "bytes=-N" — the last N bytes.
        if (!isNumber(endStr)) return result;  // malformed — ignore
        result.present = true;
        const int64_t suffix = std::stoll(endStr);
        if (suffix <= 0 || fileSize <= 0) return result;  // unsatisfiable
        result.start = suffix >= fileSize ? 0 : fileSize - suffix;
        result.end = fileSize - 1;
        result.satisfiable = true;
        return result;
    }

    // "bytes=START-" or "bytes=START-END".
    if (!isNumber(startStr)) return result;                   // malformed — ignore
    if (!endStr.empty() && !isNumber(endStr)) return result;  // malformed — ignore
    result.present = true;
    result.start = std::stoll(startStr);
    result.end = endStr.empty() ? (fileSize - 1) : std::stoll(endStr);
    if (result.end > fileSize - 1) result.end = fileSize - 1;  // clamp to end-of-file
    result.satisfiable =
        fileSize > 0 && result.start < fileSize && result.start <= result.end;
    return result;
}

}  // namespace http

#endif
```

- [ ] **Step 4: Build and run the unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 5: Commit**

```bash
git add src/http/ByteRange.hpp tests/StreamLogicTests.cpp
git commit -m "feat(http): add HTTP Range header parser"
```

End the commit message with this trailer on its own line after a blank line:
`Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`

---

## Task 2: Serve recording files with Range support

**Files:**
- Modify: `src/controller/CameraController.hpp`

- [ ] **Step 1: Add the required includes**

In `src/controller/CameraController.hpp`, find the include block:

```cpp
#include "service/CameraService.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/utils/ConversionUtils.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <fstream>
#include <iterator>
```

and replace it with:

```cpp
#include "service/CameraService.hpp"
#include "http/ByteRange.hpp"

#include "oatpp/web/server/api/ApiController.hpp"
#include "oatpp/parser/json/mapping/ObjectMapper.hpp"
#include "oatpp/core/utils/ConversionUtils.hpp"
#include "oatpp/core/macro/codegen.hpp"
#include "oatpp/core/macro/component.hpp"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
```

- [ ] **Step 2: Replace the `getRecordingFile` endpoint**

In the same file, replace this entire block:

```cpp
    ENDPOINT_INFO(getRecordingFile) {
        info->summary = "Download a recording segment file";
        info->addResponse<String>(Status::CODE_200, "video/mp4");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/recording-segments/{id}/file", getRecordingFile,
             PATH(oatpp::String, id))
    {
        auto segment = m_service.getRecordingSegmentById(id);
        OATPP_ASSERT_HTTP(segment->path && segment->path->size() > 0,
                          Status::CODE_404,
                          "Recording file not found");

        std::ifstream file(segment->path->c_str(), std::ios::binary);
        OATPP_ASSERT_HTTP(file.good(), Status::CODE_404, "Recording file not found");

        std::string contents((std::istreambuf_iterator<char>(file)),
                             std::istreambuf_iterator<char>());
        auto response = createResponse(Status::CODE_200,
                                       oatpp::String(contents.data(), contents.size()));
        response->putHeader("Content-Type", "video/mp4");
        return response;
    }
```

with:

```cpp
    ENDPOINT_INFO(getRecordingFile) {
        info->summary = "Download a recording segment file (supports HTTP Range)";
        info->addResponse<String>(Status::CODE_200, "video/mp4");
        info->addResponse<String>(Status::CODE_206, "video/mp4");
        info->addResponse<oatpp::Object<StatusDto>>(
            Status::CODE_404, "application/json");
    }
    ENDPOINT("GET", "/recording-segments/{id}/file", getRecordingFile,
             PATH(oatpp::String, id),
             HEADER(oatpp::String, rangeHeader, "Range"))
    {
        auto segment = m_service.getRecordingSegmentById(id);
        OATPP_ASSERT_HTTP(segment->path && segment->path->size() > 0,
                          Status::CODE_404,
                          "Recording file not found");

        const std::string path = segment->path->c_str();
        std::error_code ec;
        const auto fileSize =
            static_cast<v_int64>(std::filesystem::file_size(path, ec));
        OATPP_ASSERT_HTTP(!ec, Status::CODE_404, "Recording file not found");

        std::ifstream file(path, std::ios::binary);
        OATPP_ASSERT_HTTP(file.good(), Status::CODE_404, "Recording file not found");

        const http::ByteRange range = http::parseByteRange(
            rangeHeader ? std::string(rangeHeader->c_str()) : std::string(),
            fileSize);

        // A Range was requested but cannot be served for this file.
        if (range.present && !range.satisfiable) {
            auto response = createResponse(Status::CODE_416, "");
            response->putHeader("Content-Range", "bytes */" + std::to_string(fileSize));
            return response;
        }

        // No Range header — serve the whole file.
        if (!range.present) {
            std::string contents((std::istreambuf_iterator<char>(file)),
                                 std::istreambuf_iterator<char>());
            auto response = createResponse(
                Status::CODE_200, oatpp::String(contents.data(), contents.size()));
            response->putHeader("Content-Type", "video/mp4");
            response->putHeader("Accept-Ranges", "bytes");
            return response;
        }

        // Satisfiable Range — read and return only [start, end].
        const v_int64 length = range.end - range.start + 1;
        std::string chunk(static_cast<std::size_t>(length), '\0');
        file.seekg(range.start);
        file.read(chunk.data(), length);

        auto response = createResponse(
            Status::CODE_206, oatpp::String(chunk.data(), chunk.size()));
        response->putHeader("Content-Type", "video/mp4");
        response->putHeader("Accept-Ranges", "bytes");
        response->putHeader("Content-Range",
                            "bytes " + std::to_string(range.start) + "-" +
                            std::to_string(range.end) + "/" + std::to_string(fileSize));
        return response;
    }
```

- [ ] **Step 3: Build and run the unit tests**

Run: `cmake --build build -j"$(nproc)" && ctest --test-dir build --output-on-failure 2>&1 | tail -6`
Expected: build succeeds, `100% tests passed`.

- [ ] **Step 4: Commit**

```bash
git add src/controller/CameraController.hpp
git commit -m "feat(recording): serve recording files with HTTP Range support"
```

End the commit message with this trailer on its own line after a blank line:
`Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`

---

## Manual verification (operator, after the plan)

The endpoint needs a running server with at least one recording segment, so it
cannot be exercised automatically here. After building, with the server running
(`./build.sh run`) and a known segment id:

```bash
ID=<a recording segment id from GET /cameras/{cameraId}/recordings>

# No Range -> 200, whole file, Accept-Ranges present
curl -s -D - -o /dev/null "http://localhost:8009/recording-segments/$ID/file"
#   expect: HTTP/1.1 200 OK ; Accept-Ranges: bytes ; Content-Type: video/mp4

# Range -> 206 with Content-Range and exactly 100 bytes
curl -s -r 0-99 -D - -o /dev/null "http://localhost:8009/recording-segments/$ID/file"
#   expect: HTTP/1.1 206 Partial Content ; Content-Range: bytes 0-99/<size> ; Content-Length: 100
```

Then open `http://localhost:8009/recording-segments/<id>/file` in a browser —
the recording should play and seek.

---

## Self-review notes

- **Spec coverage:** `parseByteRange` pure helper in `src/http/ByteRange.hpp` →
  Task 1. Endpoint `206`/`200`/`416` handling, `Accept-Ranges`, `Content-Range`,
  reading only the requested bytes → Task 2. Unit tests for all parser cases in
  the spec → Task 1. Non-goals (multi-range, concat, x16, auth) are not
  implemented. All spec sections covered.
- **Build stays green:** Task 1 is self-contained (new header + test). Task 2's
  include additions and endpoint rewrite are one task, built together.
- **Names:** `http::ByteRange`, `http::parseByteRange`, fields `present` /
  `satisfiable` / `start` / `end` — used consistently between the header, the
  tests, and the endpoint.
