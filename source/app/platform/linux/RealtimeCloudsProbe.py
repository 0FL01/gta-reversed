#!/usr/bin/env python3
"""Ninja-derived isolated source/real-TXD/EGL probe for RealtimeClouds."""

import argparse
import hashlib
import pathlib
import re
import shlex
import subprocess


SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / "artifacts/graphics"


def source_proof():
    clouds = (SOURCE.parents[2] / "game_sa/Clouds.cpp").read_text()
    start = clouds.index("void CClouds::Render_RenderLowClouds(")
    end = clouds.index("\n// From `CClouds::Render`", start)
    function = clouds[start:end]
    render_start = clouds.index("void CClouds::Render()")
    render_end = clouds.index("\n// 0x714650", render_start)
    consumer = clouds[render_start:render_end]
    init_start = clouds.index("void CClouds::Init()")
    init_end = clouds.index("\n// 0x712FF0", init_start)
    init = clouds[init_start:init_end]
    assert 'RwTextureRead("cloud1", nullptr)' in init
    assert "std::max(CWeather::Foggyness, CWeather::CloudCoverage)" in consumer
    assert "std::max(colorBalance, CWeather::ExtraSunnyness)" in consumer
    assert "false, true" in function
    balance_start = clouds.index("uint8 CalculateColorWithBalance(")
    balance_end = clouds.index("\n}", balance_start) + 2
    balance = clouds[balance_start:balance_end]
    assert "return lerp<uint8>(blue, 0u, colorBalance);" in balance
    common = (SOURCE.parents[2] / "game_sa/common.h").read_text()
    lerp_start = common.index("T lerp(const T& from, const T& to, float t)")
    lerp_end = common.index("\n}", lerp_start) + 2
    lerp = common[lerp_start:lerp_end]
    assert "return static_cast<T>(to * t + from * (1.f - t));" in lerp
    sprites = (SOURCE.parents[2] / "game_sa/Sprite.cpp").read_text()
    calc = sprites[sprites.index("bool CSprite::CalcScreenCoors("):sprites.index("// 0x70E3E0")]
    assert "out->z <= CDraw::GetNearClipZ() + 1.0f && checkMinVisible" in calc
    assert "out->z >= CDraw::GetFarClipZ() && checkMaxVisible" in calc
    assert "*w = SCREEN_WIDTH  * rd / CDraw::GetFOV() * 70.0f" in calc
    assert "*h = SCREEN_HEIGHT * rd / CDraw::GetFOV() * 70.0f" in calc
    assert "plugin::Call<0x70EAB0>" in sprites
    assert "rwRENDERSTATEZWRITEENABLE,      RWRSTATE(FALSE)" in consumer
    assert "rwRENDERSTATEZTESTENABLE,       RWRSTATE(FALSE)" in consumer
    assert "rwRENDERSTATESRCBLEND,          RWRSTATE(rwBLENDONE)" in consumer
    assert "rwRENDERSTATEDESTBLEND,         RWRSTATE(rwBLENDONE)" in consumer

    # Compile the actual production camera method, without linking/editing its
    # translation unit or replicating its GL projection in the probe.
    realtime = (SOURCE / "Realtime.cpp").read_text()
    camera_start = realtime.index("    void Apply(int width, int height, float farPlane) const {")
    camera_end = realtime.index("\n    }", camera_start) + len("\n    }")
    camera_apply = realtime[camera_start:camera_end]
    assert "std::tan(3.14159265 / 6.0)" in camera_apply
    assert "glFrustum(-right, right, -top, top, 0.1, farPlane)" in camera_apply

    coordinates_block = function[function.index("LOW_CLOUDS_COORDS[]{"):function.index("};", function.index("LOW_CLOUDS_COORDS[]{"))]
    number = r"([-+]?(?:\d+(?:\.\d*)?|\.\d+)f)"
    coordinates = re.findall(r"\{\s*" + number + r",\s*" + number + r",\s*" + number + r"\s*\}", coordinates_block)
    assert len(coordinates) == 12
    scale = re.search(r"offset \* CVector\{" + number + r",\s*" + number + r",\s*" + number +
                      r"\}\s*\+ CVector\{" + number + r",\s*" + number + r",\s*" + number + r"\}", function)
    dimensions = re.search(r"cloudSizeScr \* CVector2D\{\s*" + number + r",\s*" + number + r"\s*\}", function)
    assert scale and dimensions and scale.group(4, 5) == ("0.f", "0.f")
    assert "colorR, colorG, colorB, 255" in function and "ms_cameraRoll" in function

    def cpp_float(value):
        return repr(float(value[:-1])) + "f"

    digest = hashlib.sha256((init + function + consumer + calc + balance + lerp).encode()).hexdigest()
    rows = ",\n    ".join("{" + ", ".join(cpp_float(value) for value in row) + "}" for row in coordinates)
    header = f'''// Generated from game_sa/Clouds.cpp; never shipped in the product target.
#pragma once
#include <array>
namespace source_clouds {{
inline constexpr char SourceSha256[] = "{digest}";
inline constexpr std::array<std::array<float, 3>, 12> Offsets{{{{
    {rows}
}}}};
inline constexpr std::array<float, 3> PositionScale{{{", ".join(cpp_float(value) for value in scale.group(1, 2, 3))}}};
inline constexpr float HeightOffset = {cpp_float(scale.group(6))};
inline constexpr std::array<float, 2> SpriteDimensions{{{", ".join(cpp_float(value) for value in dimensions.group(1, 2))}}};
inline constexpr float DefaultFov = 70.0f;
inline constexpr unsigned Intensity = 255u;
}} // namespace source_clouds
struct ProductionCamera {{
    float x, y, z, yaw, pitch;
{camera_apply}
}};
'''
    (OUTPUT / "RealtimeCloudsProbe.source.h").write_text(header)
    print("cloud-source-proof-sha256=" + digest, flush=True)
    print("cloud-production-camera-apply-sha256=" + hashlib.sha256(camera_apply.encode()).hexdigest(), flush=True)


def build():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    source_proof()
    build_dir = WORKSPACE / "build"
    commands = subprocess.check_output(
        ["ninja", "-C", str(build_dir), "-t", "commands", "mad-sa-linux"], text=True
    ).splitlines()
    template = shlex.split(next(command for command in commands if "-c " in command and "/RealtimeEnvironment.cpp" in command))
    flags = []
    index = 0
    while index < len(template):
        if template[index] in ("-MT", "-MF", "-o", "-c"):
            index += 2
        elif template[index] == "-MD":
            index += 1
        else:
            flags.append(template[index])
            index += 1
    link_line = next(command for command in commands if " -o mad-sa-linux " in command)
    link = shlex.split(next(part for part in link_line.split("&&") if " -o mad-sa-linux " in part))
    link = [argument for argument in link if not argument.endswith("/MainLinux.cpp.o")]
    # Do not ask Ninja to rebuild concurrently edited product TUs. The probe
    # provides test-only OS_File shims and needs only the existing TXD decoder,
    # Ninja's librw archive, and system EGL/GL.
    texsample = next((value for value in link if value.endswith("/TexSample.cpp.o")), None)
    librw = next((value for value in link if value.endswith("vendor/librw/src/librw.a")), None)
    if not texsample or not (build_dir / texsample).is_file():
        raise RuntimeError("required Ninja TexSample object is absent")
    if not librw or not (build_dir / librw).is_file():
        raise RuntimeError("required Ninja librw archive is absent")
    probe_object = OUTPUT / "RealtimeCloudsProbe.o"
    compile_command = flags + ["-UNDEBUG", "-Wall", "-Wextra", "-ffunction-sections", "-fdata-sections",
                               "-I" + str(OUTPUT), "-c", str(SOURCE / "RealtimeCloudsProbe.cpp"), "-o", str(probe_object)]
    link = [link[0], "-Wl,--gc-sections", str(probe_object), texsample, librw,
            "-lEGL", "-lGL", "-ldl", "-lpthread", "-lm", "-o", str(OUTPUT / "RealtimeCloudsProbe")]
    log_path = OUTPUT / "RealtimeCloudsProbe-build.log"
    with log_path.open("w") as log:
        for command in (compile_command, link):
            log.write(shlex.join(command) + "\n")
            log.flush()
            result = subprocess.run(command, cwd=build_dir, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                log.flush()
                print(log_path.read_text())
                result.check_returncode()


def run(game_dir, run_dir):
    run_dir.mkdir(parents=True, exist_ok=True)
    result = subprocess.run([str(OUTPUT / "RealtimeCloudsProbe"), str(game_dir.resolve()), str(run_dir.resolve())],
                            cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
    (run_dir / "RealtimeCloudsProbe.log").write_text(result.stdout)
    print(result.stdout, end="")
    result.check_returncode()
    assert "realtime-clouds-probe-ok layer=low only" in result.stdout


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build", action="store_true")
    mode.add_argument("--run", action="store_true")
    parser.add_argument("--game-dir", type=pathlib.Path, default=pathlib.Path("/game"))
    parser.add_argument("--run-dir", type=pathlib.Path, default=OUTPUT)
    args = parser.parse_args()
    if args.build:
        build()
    else:
        run(args.game_dir, args.run_dir)
