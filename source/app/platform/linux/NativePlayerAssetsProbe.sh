#!/bin/bash
# Run from /workspace in mad-sa-graphics-build after the native build.
set -euo pipefail
O=build/source/CMakeFiles/mad-sa-linux.dir
mkdir -p artifacts/graphics
c++ -std=gnu++23 -O2 -Wall -Wextra -DRW_NULL -DNATIVE_PLAYER_ASSETS_PROBE -ffunction-sections -fdata-sections \
    -Igta-reversed/source -isystem gta-reversed/vendor/librw \
    gta-reversed/source/app/platform/linux/NativePlayerAssetsProbe.cpp \
    gta-reversed/source/app/platform/linux/NativePlayerAssets.cpp \
    "$O/app/platform/linux/TexSample.cpp.o" "$O/oswrapper/oswrapper_linux.cpp.o" \
    build/vendor/librw/src/librw.a -Wl,--gc-sections -lz -pthread \
    -o artifacts/graphics/native-player-assets-probe
artifacts/graphics/native-player-assets-probe "${1:-/game}" | tee artifacts/graphics/native-player-assets-probe.log
artifacts/graphics/native-player-assets-probe "${1:-/game}" --constructor-only | tee artifacts/graphics/native-player-assets-constructor-probe.log
