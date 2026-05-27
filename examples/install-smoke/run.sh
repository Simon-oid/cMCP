#!/bin/sh
# install-smoke: regression gate for cMCP's packaging surface.
#
# Builds + installs cMCP into a throwaway temp PREFIX, then compiles
# two tiny external consumers against it (one via pkg-config, one via
# CMake find_package) and runs both. If either consumer fails to find
# headers, link the static libs, or run, the gate fails.
#
# Idempotent and hermetic — no shared state, temp dir is rm'd on exit.

set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"

PREFIX="$(mktemp -d 2>/dev/null || mktemp -d -t cmcp-install-smoke)"
trap 'rm -rf "$PREFIX"' EXIT

echo "=== install-smoke: PREFIX=$PREFIX ==="

# 1. Build + install cMCP into the temp prefix.
make -C "$ROOT" --no-print-directory all   >/dev/null
make -C "$ROOT" --no-print-directory install PREFIX="$PREFIX" >/dev/null

echo "=== install layout ==="
find "$PREFIX" -type f | sort | sed "s|$PREFIX/||"

# 2. pkg-config consumer.
echo
echo "=== pkg-config consumer ==="
PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig"
export PKG_CONFIG_PATH
pkg-config --cflags --libs cmcp-server cmcp-client cmcp-core

BUILD_PC="$PREFIX/build-pc"
mkdir -p "$BUILD_PC"
cp "$HERE/smoke.c" "$HERE/Makefile" "$BUILD_PC/"
make -C "$BUILD_PC" --no-print-directory pkgconfig-smoke
"$BUILD_PC/pkgconfig-smoke"

# 3. CMake consumer.
echo
echo "=== CMake consumer ==="
if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not installed; skipping cmake consumer"
else
    BUILD_CMAKE="$PREFIX/build-cmake"
    cmake -S "$HERE" -B "$BUILD_CMAKE" \
          -DCMAKE_PREFIX_PATH="$PREFIX" \
          -DCMAKE_BUILD_TYPE=Release \
          -Wno-dev >/dev/null
    cmake --build "$BUILD_CMAKE" >/dev/null
    "$BUILD_CMAKE/cmake-smoke"
fi

echo
echo "=== install-smoke: all green ==="
