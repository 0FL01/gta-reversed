#!/usr/bin/env python3
"""Isolated source-arithmetic/owned-asset/legacy-parser low-cloud provider probe."""
import argparse
import hashlib
import pathlib
import re
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
REPO = SOURCE.parents[3]
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / "artifacts/graphics"
BASELINE = "f7cb8383"


def proof():
    game = SOURCE.parents[2] / "game_sa"
    enum = (game / "Enums/eWeatherType.h").read_text()
    weather = re.findall(r"WEATHER_([A-Z_0-9]+) = (\d+),", enum)
    assert [int(value) for _, value in weather] == list(range(23))
    time = (game / "TimeCycle.cpp").read_text()
    colour = (game / "Collision/ColourSet.cpp").read_text()
    hours = re.search(r"TimeSamples\s*= std::to_array\(\{([^}]+)\}", time).group(1)
    assert [int(v) for v in hours.split(",")] == [0, 5, 6, 7, 12, 19, 20, 22, 24]
    assert "&lowCloudR, &lowCloudG, &lowCloudB," in time
    assert "&farClip, &fogStart, &lightOnGround," in time
    for component in ("Red", "Green", "Blue"):
        assert re.search(rf"m_nLowClouds{component}\s*= \(uint16\)\(A->m_nLowClouds{component}\s*\* multA \+ B->m_nLowClouds{component}\s*\* multB\)", colour)
    assert "std::clamp((camPos.z - 20.0f) / 200.0f, 0.0f, 1.0f)" in time
    for token in ("WEATHER_EXTRASUNNY_SMOG_LA", "WEATHER_SUNNY_SMOG_LA", "currentOld.Interpolate", "nextOld.Interpolate"):
        assert token in time
    assert time.index("nextOld.Interpolate") < time.index("a.Interpolate(&currentOld, &nextOld")
    original = subprocess.check_output(["git", "show", f"{BASELINE}:source/app/platform/linux/RealtimeEnvironment.cpp"], cwd=REPO, text=True)
    wind = re.search(r"constexpr float wind\[\]\{([^}]+)\}", original).group(1)
    extra_block = enum[enum.index("inline bool IsExtraSunny"):]
    extras = set(re.findall(r"case WEATHER_(\w+):", extra_block))
    # Fixed Old==New factors supplied by the round8 CWeather::Update static
    # evidence; Wind independently extracted from the pre-change native table.
    cloudy = {4, 7, 8, 9, 12, 15, 16, 19, 20, 21, 22}
    names = ", ".join('"' + name + '"' for name, _ in weather)
    coverage = ", ".join(str(int(i in cloudy)) for i in range(23))
    sunny = ", ".join(str(int(name in extras)) for name, _ in weather)
    digest = hashlib.sha256((enum + time + colour + original).encode()).hexdigest()
    header = f'''// Probe oracle provenance: {digest}
#pragma once
#include <array>
namespace source_low_clouds {{
inline constexpr char Baseline[] = "{BASELINE}";
inline constexpr std::array<const char*, 23> Names{{{names}}};
inline constexpr std::array<int, 9> Hours{{{hours}}};
inline constexpr std::array<float, 23> Wind{{{wind}}};
inline constexpr std::array<int, 23> Cloudy{{{coverage}}};
inline constexpr std::array<int, 23> ExtraSunny{{{sunny}}};
}}
'''
    (OUTPUT / "RealtimeLowCloudParamsProbe.source.h").write_text(header)
    legacy = subprocess.check_output(["git", "show", f"{BASELINE}:source/app/platform/linux/TimeCycle.cpp"], cwd=REPO, text=True)
    (OUTPUT / "RealtimeLowCloudParamsProbe.legacy.cpp").write_text(legacy)
    print("low-cloud-source-proof-sha256=" + digest, flush=True)


def build():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    proof()
    directory = WORKSPACE / "build"
    commands = subprocess.check_output(["ninja", "-C", str(directory), "-t", "commands", "mad-sa-linux"], text=True).splitlines()
    template = shlex.split(next(line for line in commands if "-c " in line and "/RealtimeEnvironment.cpp" in line))
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
    link_line = next(line for line in commands if " -o mad-sa-linux " in line)
    original_link = shlex.split(next(part for part in link_line.split("&&") if " -o mad-sa-linux " in part))
    objects = []
    for suffix in ("/TexSample.cpp.o", "/WaterLevel.cpp.o", "vendor/librw/src/librw.a"):
        value = next(value for value in original_link if value.endswith(suffix))
        assert (directory / value).is_file(), value
        objects.append(value)
    jobs = []
    compiled = []
    for source, suffix, defines in (
        (SOURCE / "RealtimeLowCloudParamsProbe.cpp", "", []),
        (SOURCE / "TimeCycle.cpp", "-TimeCycle", []),
        (OUTPUT / "RealtimeLowCloudParamsProbe.legacy.cpp", "-legacy", [
            "-DTimeCycle_LoadWeatherHour=LegacyTimeCycle_LoadWeatherHour", "-DTimeCycle_LoadHour=LegacyTimeCycle_LoadHour"]),
    ):
        obj = OUTPUT / ("RealtimeLowCloudParamsProbe" + suffix + ".o")
        compiled.append(str(obj))
        jobs.append(flags + ["-UNDEBUG", "-Wall", "-Wextra", "-ffp-contract=off", "-ffunction-sections", "-fdata-sections",
                             "-I" + str(OUTPUT)] + defines + ["-c", str(source), "-o", str(obj)])
    jobs.append([original_link[0], "-Wl,--gc-sections"] + compiled + objects +
                ["-lGL", "-ldl", "-lpthread", "-lm", "-o", str(OUTPUT / "RealtimeLowCloudParamsProbe")])
    log_path = OUTPUT / "RealtimeLowCloudParamsProbe-build.log"
    with log_path.open("w") as log:
        for command in jobs:
            log.write(shlex.join(command) + "\n")
            log.flush()
            result = subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                print(log_path.read_text())
                result.check_returncode()


def run(game_dir):
    result = subprocess.run([str(OUTPUT / "RealtimeLowCloudParamsProbe"), str(game_dir.resolve())],
                            cwd=WORKSPACE, capture_output=True, text=True, timeout=180)
    text = result.stdout + result.stderr
    (OUTPUT / "RealtimeLowCloudParamsProbe.log").write_text(text)
    print(text, end="")
    result.check_returncode()
    assert "realtime-low-cloud-params-probe-ok fixed-weather-only" in text


def offline_hashes(game_dir):
    """Relink a private binary, rebuilding every product consumer of our headers.

    Reuses other existing Ninja objects; never rebuilds/modifies the parent's
    build target. Expected image hashes are the existing etalon contract.
    """
    OUTPUT.mkdir(parents=True, exist_ok=True)
    directory = WORKSPACE / "build"
    commands = subprocess.check_output(["ninja", "-C", str(directory), "-t", "commands", "mad-sa-linux"], text=True).splitlines()
    link_line = next(line for line in commands if " -o mad-sa-linux " in line)
    link = shlex.split(next(part for part in link_line.split("&&") if " -o mad-sa-linux " in part))
    binary = OUTPUT / "RealtimeLowCloudParamsProbe-offline"
    link[link.index("-o") + 1] = str(binary)
    log_path = OUTPUT / "RealtimeLowCloudParamsProbe-offline-build.log"
    with log_path.open("w") as log:
        for name in ("MainLinux", "ShoreShot", "TimeCycle", "RealtimeEnvironment", "Realtime"):
            template = shlex.split(next(line for line in commands if "-c " in line and f"/{name}.cpp" in line))
            original_object = template[template.index("-o") + 1]
            obj = OUTPUT / f"RealtimeLowCloudParamsProbe-offline-{name}.o"
            command = []
            index = 0
            while index < len(template):
                if template[index] in ("-MT", "-MF"):
                    index += 2
                elif template[index] == "-MD":
                    index += 1
                elif template[index] == "-o":
                    command += ["-o", str(obj)]
                    index += 2
                else:
                    command.append(template[index])
                    index += 1
            link[link.index(original_object)] = str(obj)
            log.write(shlex.join(command) + "\n")
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
        log.write(shlex.join(link) + "\n")
        log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    fixtures = (
        ("scene", "8041393264061736146", []),
        ("hour0", "12424149891741056524", ["--hour", "0"]),
        ("hour7", "10905788431574263010", ["--hour", "7"]),
        ("hour12", "8440076533160693072", ["--hour", "12"]),
        ("cloudy", "17456348052958327601", ["--hour", "12", "--weather", "CLOUDY_LA"]),
        ("rainy", "11361061957936333104", ["--hour", "12", "--weather", "RAINY_SF"]),
        ("fogcloudy", "13110586735901234051", ["--hour", "12", "--weather", "CLOUDY_LA", "--fog"]),
        ("fogextra", "17103271862050636373", ["--hour", "12", "--fog"]),
    )
    with (OUTPUT / "RealtimeLowCloudParamsProbe-offline.log").open("w") as log:
        for label, expected, options in fixtures:
            command = [str(binary), "--game-dir", str(game_dir.resolve()), "--shot-scene",
                       str(OUTPUT / f"RealtimeLowCloudParamsProbe-{label}.tga"), "--frames", "30"] + options
            result = subprocess.run(command, cwd=WORKSPACE, capture_output=True, text=True, timeout=180)
            log.write(shlex.join(command) + "\n" + result.stdout + result.stderr)
            log.flush()
            result.check_returncode()
            assert expected in result.stdout, (label, expected, result.stdout, result.stderr)
            print(f"offline-hash-ok {label} {expected}", flush=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build", action="store_true")
    mode.add_argument("--run", action="store_true")
    mode.add_argument("--offline-hashes", action="store_true")
    parser.add_argument("--game-dir", type=pathlib.Path, default=pathlib.Path("/game"))
    args = parser.parse_args()
    if args.build:
        build()
    elif args.offline_hashes:
        offline_hashes(args.game_dir)
    else:
        run(args.game_dir)
