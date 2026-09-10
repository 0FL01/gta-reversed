#!/usr/bin/env python3
"""Ninja-derived native water oracle/GL probe. Run --build then --run in mad-sa:dev.

Only source and owned assets are read; generated oracle, objects, logs and own
EGL application captures go to artifacts/graphics. No executable is inspected.
"""
import argparse
import hashlib
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'


def extract_oracle():
    source = (SOURCE.parents[2] / 'game_sa/WaterLevel.cpp').read_text()
    start = source.index('void CWaterLevel::CalculateWavesOnlyForCoordinate(')
    end = source.index('\n// 0x6E5810', start)
    function = source[start:end]
    # Keep the original function text unmodified. These are isolated stand-ins
    # for its static refs and vector/math dependencies, not the native module.
    prefix = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
namespace water_oracle {
using int32 = int32_t;
constexpr float PI = 3.14159265358979323846f, TWO_PI = 2.0f * PI, E_CONST = .577f;
struct CVector2D { float x, y; };
struct CVector {
    float x, y, z;
    CVector(float a, float b, float c) : x(a), y(b), z(c) {}
    void operator+=(CVector v) { x += v.x; y += v.y; z += v.z; }
    void Normalise() { const float r = 1.0f / std::sqrt(x*x+y*y+z*z); x *= r; y *= r; z *= r; }
};
struct CWeather { static inline float Wavyness, SunGlare; };
struct CTimer { static inline uint32_t time; static uint32_t GetTimeInMS() { return time; } };
struct CMaths {
    static float GetSinFast(float rad) {
        static const auto table = [] {
            std::array<float, 256> t{};
            for (unsigned i = 0; i < t.size(); ++i) t[i] = std::sin(float(i) * (TWO_PI / 256));
            return t;
        }();
        return table[uint32_t(rad / (TWO_PI / 256)) % 256];
    }
    static float GetCosFast(float rad) { return GetSinFast(rad + PI / 2); }
};
struct CWaterLevel {
    // Original constants: compact 8D38C8/8D38E8, retail 94BE28/94BE48.
    static inline float faWaveMultipliersX[8]{1,.85f,.73f,.77f,.75f,.8f,.73f,.8f};
    static inline float faWaveMultipliersY[8]{.75f,.9f,.95f,.82f,.7f,.75f,.9f,1};
    static inline uint32_t m_nWaterTimeOffset;
    static void CalculateWavesOnlyForCoordinate(int32,int32,float,float,float&,float&,float&,CVector&);
};
'''
    digest = hashlib.sha256(function.encode()).hexdigest()
    (OUTPUT / 'RealtimeWaterProbe.oracle.h').write_text(
        '// Extracted source SHA256 ' + digest + '\n' + prefix + function + '\n}\n')
    print('water-oracle-source-sha256=' + digest, flush=True)


REGRESSIONS = ('RealtimeStreamingProbe', 'VehicleMaterialProbe', 'RealtimeHudProbe')


def build(regress=False):
    OUTPUT.mkdir(parents=True, exist_ok=True)
    extract_oracle()
    # Independent pre-change parser/builder, symbol-renamed only. The additive
    # header fields default to zero here; original offline rendering stays real.
    legacy = subprocess.check_output(['git', '-c', 'safe.directory=' + str(SOURCE.parents[3]), '-C', str(SOURCE.parents[3]), 'show',
        '4209d974:source/app/platform/linux/WaterLevel.cpp'], text=True)
    legacy_path = OUTPUT / 'RealtimeWaterProbe.legacy.cpp'
    legacy_path.write_text('float RealtimeWaterProbeLegacy_LegacyShadeUp();\n' +
        legacy.replace('WaterLevel_', 'RealtimeWaterProbeLegacy_'))
    build_dir = WORKSPACE / 'build'
    commands = subprocess.check_output(
        ['ninja', '-C', str(build_dir), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, i = [], 0
    while i < len(template):
        if template[i] in ('-MT', '-MF', '-o', '-c'):
            i += 2
        elif template[i] == '-MD':
            i += 1
        else:
            flags.append(template[i])
            i += 1
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(part for part in line.split('&&') if ' -o mad-sa-linux ' in part))
    link = [a for a in link if not any(a.endswith('/' + n + '.cpp.o') for n in ('MainLinux', 'Realtime'))]
    subprocess.run(['ninja', '-C', str(build_dir)] + [a for a in link if a.endswith('.cpp.o')], check=True)
    base_link = link.copy()
    obj = OUTPUT / 'RealtimeWaterProbe.o'
    compile_command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
        '-I' + str(OUTPUT), '-c', str(SOURCE / 'RealtimeWaterProbe.cpp'), '-o', str(obj)]
    link[link.index('-o') + 1] = str(OUTPUT / 'RealtimeWaterProbe')
    legacy_obj = OUTPUT / 'RealtimeWaterProbe.legacy.o'
    legacy_command = flags + ['-c', str(legacy_path), '-o', str(legacy_obj)]
    link[1:1] = ['-Wl,--gc-sections', str(obj), str(legacy_obj)]
    with (OUTPUT / 'RealtimeWaterProbe-build.log').open('w') as log:
        for command in (compile_command, legacy_command, link):
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=build_dir, stdout=log, stderr=subprocess.STDOUT, check=True)
    if regress:
        for name in REGRESSIONS:
            units = [name, 'VehicleMaterialGpuProbe'] if name == 'VehicleMaterialProbe' else [name]
            included = {'VehicleMaterialProbe': '/CarPose.cpp.o', 'RealtimeHudProbe': '/RealtimeHud.cpp.o'}.get(name)
            regression_link = [a for a in base_link if not (included and a.endswith(included))]
            regression_link[regression_link.index('-o') + 1] = str(OUTPUT / ('RealtimeWaterProbe-' + name))
            regression_link.insert(1, '-Wl,--gc-sections')
            with (OUTPUT / ('RealtimeWaterProbe-' + name + '-build.log')).open('w') as log:
                for unit in units:
                    obj = OUTPUT / ('RealtimeWaterProbe-' + unit + '.o')
                    command = flags + ['-UNDEBUG', '-ffunction-sections', '-fdata-sections',
                        '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
                    log.write(shlex.join(command) + '\n'); log.flush()
                    subprocess.run(command, cwd=build_dir, stdout=log, stderr=subprocess.STDOUT, check=True)
                    regression_link.insert(1, str(obj))
                log.write(shlex.join(regression_link) + '\n'); log.flush()
                subprocess.run(regression_link, cwd=build_dir, stdout=log, stderr=subprocess.STDOUT, check=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--regress', action='store_true', help='rebuild/run existing world, vehicle material and HUD pixel probes')
    parser.add_argument('--game-dir', type=pathlib.Path, default=pathlib.Path('/game'))
    args = parser.parse_args()
    if args.build or args.regress:
        build(args.regress)
        if args.regress:
            for name in REGRESSIONS:
                result = subprocess.run([str(OUTPUT / ('RealtimeWaterProbe-' + name)), str(args.game_dir.resolve())],
                    cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
                (OUTPUT / ('RealtimeWaterProbe-' + name + '.log')).write_text(result.stdout)
                print(result.stdout, end='')
                result.check_returncode()
    else:
        result = subprocess.run([str(OUTPUT / 'RealtimeWaterProbe'), str(args.game_dir.resolve()), str(OUTPUT)],
            cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUTPUT / 'RealtimeWaterProbe.log').write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        assert 'water-probe-ok' in result.stdout
