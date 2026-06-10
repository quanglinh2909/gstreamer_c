#!/usr/bin/env bash
# Chạy test_gstreamer dưới gdb (batch mode). Khi binary segfault / abort,
# gdb bắt signal, in full backtrace (kèm file:line vì binary có debug_info)
# ra cả pm2 log lẫn một file crash riêng có timestamp, rồi thoát để pm2
# tự restart. Lúc chạy bình thường gdb không can thiệp (các signal vô hại
# được pass thẳng cho chương trình).
#
# Dùng: pm2 trỏ vào script này thay vì binary trực tiếp (xem README ở cuối).
set -u

BIN=/home/orangepi/Documents/test_gstremer/build/test_gstreamer
WORKDIR=/home/orangepi/Documents/test_gstremer/build
LOGDIR="$WORKDIR/crashlogs"
mkdir -p "$LOGDIR"
STAMP=$(date +%Y%m%d_%H%M%S)
CRASHLOG="$LOGDIR/crash_${STAMP}.log"

cd "$WORKDIR" || exit 1

# Bật core dump phòng khi cần phân tích lại sau (gdic sẽ không tạo core khi
# chạy under gdb, nhưng để sẵn nếu chuyển sang chạy trực tiếp).
ulimit -c unlimited 2>/dev/null || true

exec gdb -q -batch \
  -ex 'set pagination off' \
  -ex 'set confirm off' \
  -ex 'set print pretty on' \
  -ex 'set print frame-arguments all' \
  -ex 'handle SIGPIPE nostop noprint pass' \
  -ex 'handle SIGUSR1 nostop noprint pass' \
  -ex 'handle SIGUSR2 nostop noprint pass' \
  -ex 'handle SIGCHLD nostop noprint pass' \
  -ex "set logging file $CRASHLOG" \
  -ex 'set logging overwrite on' \
  -ex 'set logging enabled on' \
  -ex 'run' \
  -ex 'printf "\n=================== CRASH CAUGHT ===================\n"' \
  -ex 'printf "signal/exit at frame:\n"' \
  -ex 'frame' \
  -ex 'printf "\n----- backtrace (full, with locals) -----\n"' \
  -ex 'bt full' \
  -ex 'printf "\n----- all threads -----\n"' \
  -ex 'thread apply all bt' \
  -ex 'printf "\n----- registers -----\n"' \
  -ex 'info registers' \
  -ex 'printf "\n----- disassemble around fault -----\n"' \
  -ex 'x/4i $pc' \
  -ex 'printf "\n===================================================\n"' \
  -ex 'quit' \
  --args "$BIN"
