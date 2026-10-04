#!/bin/sh
# Builds srt-live-transmit with SRT Telemetry.
#
#   build.sh SRT_SOURCE_DIR TELEMETRY_PREFIX [BUILD_DIR]
#
# SRT_SOURCE_DIR: a Haivision SRT v1.5.7 checkout (the patch is applied to it).
# TELEMETRY_PREFIX: where SRT Telemetry is installed (cmake --install), built
#   with the public SRT adapter and the HTTP server.
# BUILD_DIR: default ./srt-live-transmit-build. The program is BUILD_DIR/srt-live-transmit.
set -eu
src=$1
prefix=$2
build=${3:-srt-live-transmit-build}
here=$(cd "$(dirname "$0")" && pwd)
patch="$here/srt-live-transmit-v1.5.7.patch"

if git -C "$src" apply --reverse --check "$patch" 2>/dev/null; then
    echo "patch already applied"
else
    git -C "$src" apply "$patch"
fi
cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release -DENABLE_APPS=ON -DENABLE_STATIC=OFF \
    -DCMAKE_PREFIX_PATH="$prefix"
cmake --build "$build" --target srt-live-transmit --parallel
echo "built $build/srt-live-transmit"
