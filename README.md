# Build Commands

| Lệnh | Tác dụng |
|---|---|
| `./build.sh` | Configure (nếu chưa) + incremental build |
| `./build.sh clean` | Xóa `build/` rồi reconfigure từ đầu |
| `./build.sh run` | Build xong chạy luôn `./build/test_gstreamer` |

---

# Recording Configuration

| Tham số | Kiểu | Mặc định | Ý nghĩa |
|---|---|---|---|
| `recordingEnabled` | `bool` | `false` | Bật ghi hình (tương đương chế độ `always`) |
| `recordingMode` | `string` | `off` | `off` = không ghi · `always` = ghi liên tục · `motion` = chỉ giữ đoạn có chuyển động |
| `motionEnabled` | `bool` | `false` | Bật dò chuyển động. Ở mode `always` vẫn dùng để đánh dấu cờ `has_motion` cho từng đoạn |
| `motionSensitivity` | `double (0–1)` | `0.5` | Độ nhạy của motioncells — cao = bắt thay đổi nhỏ hơn |
| `motionThreshold` | `double (0–1)` | `0.01` | Tỉ lệ ô lưới phải thay đổi mới coi là có motion |
| `preMotionSeconds` | `uint` | `10` | Giữ lại bao nhiêu giây video trước lúc motion bắt đầu |
| `postMotionSeconds` | `uint` | `20` | Giữ lại bao nhiêu giây video sau lúc motion kết thúc |
| `segmentSeconds` | `uint` | `10` | Độ dài mỗi file đoạn (`.ts`) |

---

## Lưu ý

- `effectiveRecordingMode`:
  - Nếu `recordingMode = off` nhưng `recordingEnabled = true`
  - Hệ thống sẽ tự coi như mode `always`.
- Phát lại liên tục trên trình duyệt qua HLS:
  - Playlist: `GET /cameras/{id}/playback.m3u8?from=<iso>&to=<iso>`
  - Segment: `GET /recording-segments/{segmentId}/file`
  - Chrome/Firefox dùng `hls.js`; Safari có thể phát HLS native.

---

# Cách hoạt động của `preMotion` / `postMotion`

Áp dụng trong mode `motion` (logic trong `CameraRecordingSession`):

- Các đoạn video chưa có motion sẽ được giữ tạm trong `m_pendingSegments`.
- Khi phát hiện motion:
  - Các đoạn nằm trong cửa sổ `preMotionSeconds` sẽ được giữ lại.
  - Các đoạn quá cũ sẽ bị xóa file.
- Sau khi motion kết thúc:
  - Hệ thống tiếp tục giữ thêm `postMotionSeconds`.

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
