#!/bin/bash
# Run from /workspace in mad-sa-graphics-build after the production build.
set -euo pipefail
O=build/source/CMakeFiles/mad-sa-linux.dir
mkdir -p artifacts/graphics
sdlIncludes=(/opt/conan/p/b/sdl*/p/include)
c++ -std=gnu++23 -O2 -DRW_NULL -ffunction-sections -fdata-sections \
    -Igta-reversed/source -Igta-reversed/vendor/librw "${sdlIncludes[@]/#/-isystem}" \
    gta-reversed/source/app/platform/linux/VehicleMaterialProbe.cpp \
    gta-reversed/source/app/platform/linux/VehicleMaterialGpuProbe.cpp \
    "$O/app/platform/linux/TexSample.cpp.o" "$O/oswrapper/oswrapper_linux.cpp.o" \
    "$O/app/platform/linux/RealtimeEnvironment.cpp.o" "$O/app/platform/linux/TimeCycle.cpp.o" \
    "$O/app/platform/linux/WaterLevel.cpp.o" \
    build/vendor/librw/src/librw.a -Wl,--gc-sections -lEGL -lGL -lz -pthread \
    -o artifacts/graphics/vehicle-material-probe
artifacts/graphics/vehicle-material-probe "${1:-/game}" | tee artifacts/graphics/vehicle-material-probe.log
