#!/usr/bin/env bash
# Build script for test_gstreamer.
# Usage:
#   ./build.sh            # configure (if needed) + incremental build
#   ./build.sh clean      # wipe build/ then full reconfigure + build
#   ./build.sh run        # build then run ./build/test_gstreamer
#
# Env overrides:
#   VCPKG_ROOT     path to vcpkg checkout (auto-detected if unset)
#   BUILD_TYPE     Debug | Release (default: Debug)
#   JOBS           parallel build jobs (default: nproc)

set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$PROJECT_ROOT"

BUILD_DIR="${PROJECT_ROOT}/build"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
JOBS="${JOBS:-$(nproc)}"

# --- 0. Check system build tools ----------------------------------------------
# vcpkg builds many ports from source and needs these on first install.
MISSING=()
for tool in cc c++ make cmake bison flex pkg-config; do
    command -v "$tool" >/dev/null 2>&1 || MISSING+=("$tool")
done
if (( ${#MISSING[@]} > 0 )); then
    echo "ERROR: missing system build tools: ${MISSING[*]}" >&2
    echo "  Install (Debian/Ubuntu):" >&2
    echo "    sudo apt-get install -y build-essential bison flex pkg-config \\" >&2
    echo "        autoconf automake libtool curl zip unzip tar cmake ninja-build" >&2
    exit 1
fi

# --- 1. Locate vcpkg ----------------------------------------------------------
if [[ -z "${VCPKG_ROOT:-}" ]]; then
    for candidate in "$HOME/vcpkg" "$HOME/.vcpkg" "/opt/vcpkg" "/usr/local/vcpkg"; do
        if [[ -f "$candidate/scripts/buildsystems/vcpkg.cmake" ]]; then
            export VCPKG_ROOT="$candidate"
            break
        fi
    done
fi

if [[ -z "${VCPKG_ROOT:-}" || ! -f "$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" ]]; then
    echo "ERROR: vcpkg not found." >&2
    echo "  Install:  git clone https://github.com/microsoft/vcpkg ~/vcpkg && ~/vcpkg/bootstrap-vcpkg.sh" >&2
    echo "  Then export VCPKG_ROOT=~/vcpkg" >&2
    exit 1
fi

# --- 2. Handle subcommands ----------------------------------------------------
MODE="${1:-build}"

case "$MODE" in
    clean)
        echo ">> Cleaning $BUILD_DIR"
        rm -rf "$BUILD_DIR"
        ;;
    build|run)
        ;;
    *)
        echo "Unknown mode: $MODE (expected: build | clean | run)" >&2
        exit 2
        ;;
esac

# --- 3. Configure (only if cache missing) -------------------------------------
if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    echo ">> Configuring (vcpkg: $VCPKG_ROOT, type: $BUILD_TYPE)"
    cmake -B "$BUILD_DIR" -S "$PROJECT_ROOT" \
        -DCMAKE_TOOLCHAIN_FILE="$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake" \
        -DCMAKE_BUILD_TYPE="$BUILD_TYPE"
fi

# --- 4. Build -----------------------------------------------------------------
echo ">> Building (-j$JOBS)"
cmake --build "$BUILD_DIR" -j "$JOBS"

echo ">> Done: $BUILD_DIR/test_gstreamer"

# --- 5. Optionally run --------------------------------------------------------
if [[ "$MODE" == "run" ]]; then
    echo ">> Running"
    exec "$BUILD_DIR/test_gstreamer"
fi
