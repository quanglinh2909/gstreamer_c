#!/bin/bash
#
# Cai thu vien can thiet cho engine tren MAY MOI (RK3588 / Ubuntu 22.04 aarch64).
#
# Moi ten goi trong file nay deu TRUY NGUOC tu may that bang:
#     gst-inspect-1.0 <element> | awk '/Filename/{print $2}'   -> duong dan .so
#     dpkg -S <duong-dan-.so>                                  -> ten goi
# chu khong liet ke theo tri nho. Neu them element moi vao pipeline thi lam
# dung hai buoc do roi bo sung vao day.
#
# Chay:  bash scripts/install-deps.sh
#
set -euo pipefail

say() { printf '\n\033[1;36m==> %s\033[0m\n' "$*"; }
warn() { printf '\033[1;33m!!  %s\033[0m\n' "$*"; }

if [ "$(uname -m)" != "aarch64" ]; then
    warn "Kien truc $(uname -m), khong phai aarch64. Phan Rockchip (NPU/VPU) se khong dung duoc."
fi

# ---------------------------------------------------------------------------
say "1/7  Cong cu build"
sudo apt-get update
sudo apt-get install -y \
    build-essential cmake ninja-build pkg-config git curl ca-certificates \
    meson

# ---------------------------------------------------------------------------
say "2/7  GStreamer - thu vien phat trien (theo pkg_check_modules trong CMakeLists.txt)"
# gstreamer-1.0            -> libgstreamer1.0-dev
# gstreamer-rtsp/app/video/allocators/sdp/pbutils/rtp-1.0
#                          -> libgstreamer-plugins-base1.0-dev
# gstreamer-webrtc-1.0     -> libgstreamer-plugins-bad1.0-dev   (DE QUEN: nam o BAD)
# gstreamer-rtsp-server-1.0-> libgstrtspserver-1.0-dev
sudo apt-get install -y \
    libgstreamer1.0-dev \
    libgstreamer-plugins-base1.0-dev \
    libgstreamer-plugins-bad1.0-dev \
    libgstrtspserver-1.0-dev

# ---------------------------------------------------------------------------
say "3/7  GStreamer - plugin chay (theo tung element engine thuc su dung)"
# good : rtspsrc, rtph26Xpay/depay, splitmuxsink, capssetter, jpegenc, rtprtxsend
# bad  : h26Xparse, webrtcbin, dtlssrtpenc, srtpenc, mpegtsmux, tsdemux
# base : decodebin, videoconvert
# nice : nicesink, nicesrc  <-- THIEU GOI NAY LA LOI "could not link queueN to webrtc"
sudo apt-get install -y \
    gstreamer1.0-plugins-base \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad \
    gstreamer1.0-nice \
    gstreamer1.0-tools

# ---------------------------------------------------------------------------
say "4/7  OpenCV + PostgreSQL client + OpenSSL"
sudo apt-get install -y libopencv-dev libpq-dev libssl-dev

# ---------------------------------------------------------------------------
say "5/7  Python (backend AI) + Node/pm2"
sudo apt-get install -y python3 python3-venv python3-pip
if ! command -v node >/dev/null; then
    warn "Chua co Node.js. Cai roi chay lai, hoac: curl -fsSL https://deb.nodesource.com/setup_20.x | sudo -E bash - && sudo apt-get install -y nodejs"
else
    command -v pm2 >/dev/null || sudo npm install -g pm2
fi

# ---------------------------------------------------------------------------
say "6/7  Kiem tra phan Rockchip (KHONG co trong kho Ubuntu chuan)"
# mppvideodec / mpph264enc -> goi gstreamer1.0-rockchip1 (kho Armbian/Rockchip BSP).
# librknnrt.so + librga.so -> tu BSP Rockchip, thuong nam san o /usr/lib.
missing=0
for e in mppvideodec mpph264enc; do
    if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
        printf '    %-14s OK\n' "$e"
    else
        printf '    %-14s THIEU\n' "$e"; missing=1
    fi
done
for l in librknnrt.so librga.so; do
    if ldconfig -p | grep -q "$l"; then
        printf '    %-14s OK\n' "$l"
    else
        printf '    %-14s THIEU\n' "$l"; missing=1
    fi
done
if [ "$missing" = 1 ]; then
    warn "Thieu phan Rockchip. Cai: sudo apt-get install -y gstreamer1.0-rockchip1"
    warn "(can kho Armbian/Rockchip). librknnrt.so/librga.so lay tu BSP, chep vao /usr/lib roi sudo ldconfig."
fi

# ---------------------------------------------------------------------------
say "7/7  Kiem tra lai toan bo element engine dung"
fail=0
for e in rtspsrc rtph264pay rtph265pay rtph264depay rtph265depay \
         h264parse h265parse decodebin videoconvert \
         webrtcbin nicesink nicesrc dtlssrtpenc srtpenc rtprtxsend \
         mpegtsmux tsdemux splitmuxsink capssetter jpegenc appsrc appsink; do
    if gst-inspect-1.0 "$e" >/dev/null 2>&1; then
        printf '    %-14s OK\n' "$e"
    else
        printf '    %-14s THIEU\n' "$e"; fail=1
    fi
done

echo
if [ "$fail" = 0 ]; then
    say "Xong. Tat ca element GStreamer engine can deu co."
else
    warn "Con element THIEU o tren - engine se bao loi kho hieu (vi du webrtcbin"
    warn "van tao duoc nhung xin pad that bai -> 'could not link queueN to webrtc')."
fi

cat <<'GHICHU'

--------------------------------------------------------------------------
CON LAI PHAI LAM TAY (khong nam trong apt)
--------------------------------------------------------------------------
1) oatpp (oatpp, oatpp-postgresql, oatpp-swagger, oatpp-websocket)
   Cai qua vcpkg roi build voi:
     cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake

2) ONNX Runtime: da kem san trong third_party/onnxruntime, khong can cai.

3) PostgreSQL: dung container, vi du
     docker run -d --name postgres-parking -p 5433:5432 \
       -e POSTGRES_PASSWORD=... -e POSTGRES_DB=parking postgres

4) Backend Python:
     cd ../gstreamer_ai_python && python3 -m venv .venv \
       && .venv/bin/pip install -r requirements.txt
   Luu y: pymilvus/milvus-lite BAT BUOC ghim dung phien ban trong requirements.txt,
   lech phien ban lam hong du lieu khuon mat da dang ky.

5) TOI UU CPU (khuyen nghi manh, khong bat buoc):
   libsrtp2 cua Ubuntu KHONG link OpenSSL nen dung AES thuan C. Do duoc tren
   RK3588: srtpenc ton +1,68% CPU cho moi luong 4,6 Mbps; build lai voi OpenSSL
   con +0,52% (giam 69%). Cach lam:
     curl -sL https://github.com/cisco/libsrtp/archive/refs/tags/v2.7.0.tar.gz | tar xz
     cd libsrtp-2.7.0
     meson setup build -Dcrypto-library=openssl -Dbuildtype=release \
       --libdir=lib/aarch64-linux-gnu --prefix=/usr/local
     ninja -C build && meson test -C build      # phai dat 11/11
     sudo systemctl stop ... / pm2 stop test_gstreamer   # DUNG engine truoc!
     sudo cp build/libsrtp2.so.1 /usr/local/lib/aarch64-linux-gnu/ && sudo ldconfig
   CANH BAO: dung `cp` de len file .so khi tien trinh DANG chay se lam no
   SEGFAULT (da dinh mot lan). Dung engine truoc, hoac dung `mv`.

6) Bien moi truong BAT BUOC trong ecosystem.config.js:
     GSETTINGS_BACKEND=memory
   Thieu no thi worker dconf ban SIGTERM khi phien D-Bus hong -> engine
   crash-loop rat kho doan.
--------------------------------------------------------------------------
GHICHU
