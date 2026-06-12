#!/usr/bin/env bash
# Runs test_gstreamer with crash-survivable logging.
#
# Captures, flushed to disk every few seconds so a hard freeze / power pull
# loses at most the last seconds:
#   logs/app-<timestamp>.log     stdout+stderr of the app, line-timestamped
#   logs/kernel-<timestamp>.log  live kernel log (dmesg --follow) — this is
#                                where RGA/MPP/IOMMU/CMA driver errors land
#                                right before a system freeze
#
# Usage:
#   ./run_with_logs.sh                 # run build/test_gstreamer
#   ./run_with_logs.sh <cmd> [args]    # wrap another command instead
#
# After an freeze + power cycle, read the tail of both files:
#   tail -50 logs/kernel-*.log logs/app-*.log

set -u

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_DIR="$PROJECT_ROOT/logs"
mkdir -p "$LOG_DIR"

STAMP="$(date +%Y%m%d-%H%M%S)"
APP_LOG="$LOG_DIR/app-$STAMP.log"
KMSG_LOG="$LOG_DIR/kernel-$STAMP.log"

# Default command: the AI engine, started from build/ so the relative
# "config/config.json" path keeps working.
if [[ $# -gt 0 ]]; then
    CMD=("$@")
    WORKDIR="$PWD"
else
    CMD=("$PROJECT_ROOT/build/test_gstreamer")
    WORKDIR="$PROJECT_ROOT/build"
fi

# Kernel log follower. dmesg works unprivileged here (dmesg_restrict=0).
dmesg --follow --time-format iso >> "$KMSG_LOG" 2>&1 &
KMSG_PID=$!

# Flush page cache to disk every 5s — the part that makes the logs survive
# a power pull. `sync -d <file>` is cheap (fdatasync on just these files).
(
    while sleep 5; do
        sync -d "$APP_LOG" "$KMSG_LOG" 2>/dev/null || sync
    done
) &
SYNC_PID=$!

cleanup() {
    kill "$KMSG_PID" "$SYNC_PID" 2>/dev/null
    sync -d "$APP_LOG" "$KMSG_LOG" 2>/dev/null || sync
}
trap cleanup EXIT

echo ">> app log:    $APP_LOG"
echo ">> kernel log: $KMSG_LOG"

# stdbuf -oL -eL: line-buffered so lines reach the file the moment they are
# printed, not in 4KB chunks that would vanish in a freeze. awk stamps each
# line and fflush()es it through the pipe.
cd "$WORKDIR"
stdbuf -oL -eL "${CMD[@]}" 2>&1 \
    | awk '{ printf "%s %s\n", strftime("%Y-%m-%d %H:%M:%S"), $0; fflush() }' \
    >> "$APP_LOG" &
APP_PID=$!

# Mirror the log to the terminal so interactive use feels unchanged.
tail -f --pid="$APP_PID" "$APP_LOG" &
wait "$APP_PID"
EXIT_CODE=$?
echo ">> app exited with code $EXIT_CODE (logs kept in $LOG_DIR)"
exit "$EXIT_CODE"
