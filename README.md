# Cài đặt thư viện (chạy `./build.sh` không lỗi)

Môi trường đã kiểm chứng: **Ubuntu 22.04 (jammy), arm64, Rockchip RK3588**.

### 1. Build tools + thư viện hệ thống

```bash
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config \
    bison flex autoconf automake libtool \
    git curl zip unzip tar \
    libgstreamer1.0-dev libgstreamer-plugins-base1.0-dev \
    libgstrtspserver-1.0-dev \
    gstreamer1.0-plugins-base gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad gstreamer1.0-libav \
    libopencv-dev librga-dev
```

Trong đó:

| Nhóm | Dùng cho |
|---|---|
| `build-essential`, `cmake`, `ninja-build`, `pkg-config` | biên dịch + `find_package` / `pkg_check_modules` |
| `bison`, `flex`, `autoconf`, `automake`, `libtool`, `curl`, `zip`, `unzip`, `tar` | vcpkg build các port từ source (bắt buộc ở lần cài đầu) |
| `libgstreamer1.0-dev`, `libgstreamer-plugins-base1.0-dev`, `libgstrtspserver-1.0-dev` | `gstreamer-1.0`, `-app`, `-video`, `-allocators`, `-rtsp`, `-rtsp-server` |
| `gstreamer1.0-plugins-*`, `gstreamer1.0-libav` | plugin lúc chạy (decode/encode, RTSP) |
| `libopencv-dev` | `pkg_check_modules(OPENCV ... opencv4)` |
| `librga-dev` | `librga.so` (RGA 2D — crop/convert) |

Tăng tốc phần cứng Rockchip (khuyến nghị, cho `mppvideodec` / `mppjpegenc`):

```bash
sudo apt-get install -y gstreamer1.0-rockchip
```

> **Máy Rockchip đã có sẵn GStreamer của BSP:** nhiều image (Orange Pi, Radxa…)
> ghim (`hold`) các gói GStreamer để giữ bản vá MPP, nên lệnh trên có thể báo
> `pkgProblemResolver::Resolve generated breaks ... held packages`. Kiểm tra:
>
> ```bash
> apt-mark showhold | grep -i gst      # xem gói nào bị ghim
> pkg-config --modversion gstreamer-1.0 gstreamer-rtsp-server-1.0
> ```
>
> Nếu `pkg-config` đã trả ra phiên bản thì **header có đủ rồi, bỏ qua bước cài
> GStreamer** — đừng gỡ hold, vì làm vậy sẽ thay mất bản GStreamer có MPP và
> hỏng tăng tốc phần cứng. Chỉ cài các gói GStreamer ở trên khi máy sạch /
> chưa từng có GStreamer.

### 2. Runtime NPU của Rockchip — `librknnrt.so`

**Không có trên apt.** Thư viện này do vendor/BSP cung cấp, phải đặt tay:

```bash
# Lấy từ rknn-toolkit2 (chọn đúng bản aarch64) rồi:
sudo cp librknnrt.so /usr/lib/
sudo ldconfig
# Kiểm tra:
ls -l /usr/lib/librknnrt.so
```

> Header `rknn_api.h` và ONNX Runtime đã được vendor sẵn trong `third_party/`
> nên **không cần cài thêm** — CMake tự lấy từ đó.

### 3. vcpkg (cung cấp oatpp)

`oatpp`, `oatpp-postgresql`, `oatpp-swagger`, `oatpp-websocket` được khai báo
trong `vcpkg.json` và vcpkg tự build khi configure:

```bash
git clone https://github.com/microsoft/vcpkg ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh
export VCPKG_ROOT=~/vcpkg          # thêm vào ~/.bashrc cho lần sau
```

`build.sh` tự dò `~/vcpkg`, `~/.vcpkg`, `/opt/vcpkg`, `/usr/local/vcpkg`; đặt
`VCPKG_ROOT` nếu bạn để chỗ khác.

> Lần build đầu vcpkg biên dịch oatpp từ source nên **khá lâu** (có thể vài
> chục phút trên RK3588). Các lần sau dùng cache, rất nhanh.

### 4. Build

```bash
./build.sh
```

---

# Build Commands

| Lệnh | Tác dụng |
|---|---|
| `./build.sh` | Configure (nếu chưa) + incremental build |
| `./build.sh clean` | Xóa `build/` rồi reconfigure từ đầu |
| `./build.sh run` | Build xong chạy luôn `./build/test_gstreamer` |

Biến môi trường: `VCPKG_ROOT`, `BUILD_TYPE` (`Debug`/`Release`, mặc định
`Debug`), `JOBS` (mặc định `nproc`).

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
