# Recording File HTTP Range Support — Design

**Date:** 2026-05-20
**Status:** Approved

## Goal

Make the recording-segment download endpoint (`GET /recording-segments/{id}/file`)
support HTTP Range requests, so a recording plays and seeks in a normal browser
video player. As a side effect, in-browser variable-speed playback (x2/x3/x4 via
the player's own controls) starts working.

## Background

`CameraController::getRecordingFile` currently reads the **entire** segment file
into a `std::string`, wraps it in an `oatpp::String`, and returns it as a single
`200 OK` response (`Content-Type: video/mp4`). It ignores the `Range` request
header.

`curl` downloads such a response fine. But a browser's built-in `video/mp4`
player relies on HTTP Range requests (`206 Partial Content`) to stream and seek;
against a Range-less endpoint it fails (`ERR_CONNECTION_RESET`, observed). The
endpoint also loads the whole file into memory on every request.

## Non-goals (explicitly out of scope)

- Joining consecutive segments into a continuous timeline — a separate project
  (the standard solution is an HLS `.m3u8` playlist).
- x16 fast-scrub (keyframe-only playback) — a separate project.
- Multi-range requests (`bytes=0-99,200-299`) — browsers do not need them.
- Authentication — consistent with the other (open) endpoints.
- Response-size capping.

## Design

### 1. Pure Range parser — `src/http/ByteRange.hpp` (new)

A GStreamer-free, oatpp-free header so it is unit-testable in `stream_logic_tests`.

```cpp
namespace http {

struct ByteRange {
    bool    present     = false;  // a parseable Range header was supplied
    bool    satisfiable = false;  // the range is valid for this file size
    int64_t start       = 0;      // first byte, inclusive
    int64_t end         = 0;      // last byte, inclusive
};

// Parses an HTTP Range header value against a known file size.
ByteRange parseByteRange(const std::string& rangeHeader, int64_t fileSize);
}
```

Behaviour:
- Empty header, or anything not starting with `bytes=`, or otherwise
  unparseable → `{present = false}` (caller serves the whole file as `200`).
- `bytes=START-END` → `start = START`, `end = min(END, fileSize-1)`.
- `bytes=START-` → `start = START`, `end = fileSize-1`.
- `bytes=-N` (suffix) → `start = max(0, fileSize-N)`, `end = fileSize-1`.
- `present = true`, `satisfiable = false` when `fileSize == 0`, `START >= fileSize`,
  or the resolved `start > end`.
- Only the first range is considered; a comma (multi-range) → treat the value as
  unparseable (`present = false`).

### 2. `getRecordingFile` endpoint — `src/controller/CameraController.hpp` (modify)

- Add `HEADER(oatpp::String, rangeHeader, "Range")` to the endpoint.
- Resolve the segment file path (as today) and 404 if it is missing.
- Get the file size with `std::filesystem::file_size`.
- `auto range = http::parseByteRange(rangeHeader value or "", fileSize);`
  - **No range** (`!present`) → read the whole file → `200 OK`, headers
    `Content-Type: video/mp4`, `Accept-Ranges: bytes`. (Keeps curl/VLC behaviour.)
  - **Unsatisfiable** (`present && !satisfiable`) → `416 Range Not Satisfiable`,
    header `Content-Range: bytes */<fileSize>`.
  - **Valid range** → open the file, `seekg(start)`, read exactly
    `end - start + 1` bytes → `206 Partial Content`, headers
    `Content-Type: video/mp4`, `Accept-Ranges: bytes`,
    `Content-Range: bytes <start>-<end>/<fileSize>`.
- Only the requested byte span is read from disk — the whole file is no longer
  loaded into memory for ranged requests.

`Content-Length` is set automatically by oatpp from the response body size.
Statuses use `oatpp::web::protocol::http::Status::CODE_206` / `CODE_416`.

## Files affected

- Create: `src/http/ByteRange.hpp` — `http::parseByteRange` + `http::ByteRange`.
- Modify: `src/controller/CameraController.hpp` — `getRecordingFile` endpoint.
- Modify: `tests/StreamLogicTests.cpp` — unit tests for `parseByteRange`.

## Testing

- Unit tests (`stream_logic_tests`, pure — no oatpp/GStreamer link) for
  `parseByteRange`: no header; `bytes=0-`; `bytes=100-199`; `bytes=-500`
  (suffix); `END` past end-of-file (clamped); `START` past end-of-file
  (unsatisfiable); `fileSize == 0`; a multi-range value and a garbage value
  (both → `present = false`).
- Manual verification: `curl -r 0-99 -v <url>` returns `206` with a correct
  `Content-Range` and 100 bytes; `curl -v <url>` (no range) still returns `200`
  with `Accept-Ranges: bytes`; the recording plays and seeks in a browser.

## Error handling

- Missing file → `404` (unchanged).
- Unsatisfiable range → `416` with `Content-Range: bytes */<fileSize>`.
- A malformed/unparseable `Range` header is ignored and the full file is served
  as `200` (per RFC 7233 — ignore an unparseable Range header).
