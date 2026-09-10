#!/usr/bin/env python3
"""Ninja-derived native water oracle/GL probe. Run --build then --run in mad-sa:dev.

Only source and owned assets are read; generated oracle, objects, logs and own
EGL application captures go to artifacts/graphics. No executable is inspected.
"""
import argparse
import hashlib
from fractions import Fraction
import pathlib
import shlex
import struct
import statistics
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'


def seabed_fixtures():
    # Independent exact-rational fixtures from static retail RenderWater
    # 728B89..728D2A and segment stores 720EF0/7210A0. No native builder calls,
    # executable reads, source geometry copies, or runtime-generated textures.
    # Specify the EXPECTED cut coordinates, rather than reimplement dispatch.
    def f32(value):
        return struct.unpack('<f', struct.pack('<f', float(value)))[0]

    edge0, edge1 = Fraction(f32(.04)), Fraction(f32(.96))
    whole, half = [0, 1], [0, Fraction(1, 2), 1]
    cases = [
        (-1, 6, -3250, 250, 0, [(half, half)]),
        (-1, 6, -2650, 250, 0, [(whole, whole)]),  # EXACT 600: flat
        (-1, 6, -2650.000244140625, 250, 0, [(half, half)]),
        (12, 6, 3250, 250, 5, [(half, half)]),
        (0, 6, -2750, 250, 0, [([0, edge0], half)]),
        (11, 6, 2750, 250, 0, [([edge1, 1], half)]),
        (6, 0, 250, -2750, 0, [(half, [0, edge0])]),
        (6, 11, 250, 2750, 0, [(half, [edge1, 1])]),
        (0, 0, -2750, -2750, 0, [([0, edge0], half), (half, [0, edge0])]),
        (11, 11, 2750, 2750, 0, [([edge1, 1], half), (half, [edge1, 1])]),
        (0, 0, 0, 0, 0, [([0, edge0], whole), (whole, [0, edge0])]),
        (11, 11, 0, 0, 0, [([edge1, 1], whole), (whole, [edge1, 1])]),
        (6, 6, 250, 250, 0, []),  # NEVER a floor under arbitrary water.dat
        (-1, 6, -3250, 250, 1, []),
        (-1, 6, -3250, 250, 13, []),
    ]
    result = 'struct SeaBedFixture { int x,y; float cx,cy; int area; std::vector<RealtimeSeaBedVertex> vertices; };\n'
    result += 'inline const std::vector<SeaBedFixture> seaBedFixtures{\n'
    for bx, by, cx, cy, area, patches in cases:
        vertices = []
        for xs, ys in patches:
            for x0, x1 in zip(xs, xs[1:]):
                for y0, y1 in zip(ys, ys[1:]):
                    for x, y in [(x0, y0), (x0, y1), (x1, y0), (x1, y1)]:
                        values = [(bx+x)*500-3000, (by+y)*500-3000, -70, x*8, y*8]
                        vertices.append('{' + ','.join(f32(v).hex()+'f' for v in values) + '}')
        result += '{%d,%d,%s,%s,%d,{%s}},\n' % (bx, by, f32(cx).hex()+'f', f32(cy).hex()+'f', area, ','.join(vertices))
    return result + '};\n'


def extract_oracle():
    source = (SOURCE.parents[2] / 'game_sa/WaterLevel.cpp').read_text()
    start = source.index('void CWaterLevel::CalculateWavesOnlyForCoordinate(')
    end = source.index('\n// 0x6E5810', start)
    function = source[start:end]
    start = source.index('void CWaterLevel::UpdateFlow()')
    flow = source[start:source.index('\n// 0x6EB690', start)]
    start = source.index('uint32 CWaterLevel::AddWaterLevelVertex(')
    vertex = source[start:source.index('\nstruct SortableVtx', start)]
    # Keep the original function text unmodified. These are isolated stand-ins
    # for its static refs and vector/math dependencies, not the native module.
    prefix = r'''
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <ranges>
#include <tuple>
namespace water_oracle {
using int16 = int16_t;
using int32 = int32_t;
using uint32 = uint32_t;
namespace rng = std::ranges;
namespace rngv {
    template<class Range> auto enumerate(Range r) {
        return std::views::iota(size_t{0}, std::ranges::size(r)) | std::views::transform([r](size_t i) {
            return std::tuple<size_t, decltype(r[i])>{i, r[i]};
        });
    }
}
constexpr float PI = 3.14159265358979323846f, TWO_PI = 2.0f * PI, E_CONST = .577f;
struct CVector2D { float x, y; };
struct Bounds {
    bool DoConstrainPoint(CVector2D& p) const {
        const auto old = p;
        p.x = std::clamp(p.x, -3000.f, 3000.f); p.y = std::clamp(p.y, -3000.f, 3000.f);
        return p.x != old.x || p.y != old.y;
    }
};
constexpr Bounds WORLD_BOUNDS{};
struct CRenPar { float z{}, big{}, small{}; int8_t flowX{}, flowY{}; };
struct Vertex { int16_t x{}, y{}; CRenPar rp{}; };
struct CVector {
    float x, y, z;
    CVector(float a, float b, float c) : x(a), y(b), z(c) {}
    void operator+=(CVector v) { x += v.x; y += v.y; z += v.z; }
    void Normalise() { const float r = 1.0f / std::sqrt(x*x+y*y+z*z); x *= r; y *= r; z *= r; }
};
struct CWeather { static inline float Wavyness, SunGlare; };
struct CTimer {
    static inline uint32_t time, m_FrameCounter;
    static inline float step;
    static uint32_t GetTimeInMS() { return time; }
    static float GetTimeStep() { return step; }
};
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
    static inline std::array<Vertex, 2048> m_aVertices;
    static inline uint32 NumWaterVertices;
    static uint32 AddWaterLevelVertex(int32, int32, CRenPar);
    static inline CVector2D m_CurrentFlow{}, m_CurrentDesiredFlow{};
    static inline std::function<void()> nearestCallback;
    static void FindNearestWaterAndItsFlow() { nearestCallback(); }
    static void UpdateFlow();
};
'''
    digest = hashlib.sha256(function.encode()).hexdigest()
    (OUTPUT / 'RealtimeWaterProbe.oracle.h').write_text(
        '// Extracted source SHA256 ' + digest + '\n' + prefix + function + flow + vertex + seabed_fixtures() + '\n}\n')
    print('water-oracle-source-sha256=' + digest, flush=True)
    for name, body in [('UpdateFlow', flow), ('AddWaterLevelVertex', vertex)]:
        print('water-oracle-' + name + '-sha256=' + hashlib.sha256(body.encode()).hexdigest(), flush=True)


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
    mode.add_argument('--perf', action='store_true', help='same-context warmed production-scene A/B; run on host for real GPU')
    mode.add_argument('--regress', action='store_true', help='rebuild/run existing world, vehicle material and HUD pixel probes')
    parser.add_argument('--game-dir', type=pathlib.Path, default=pathlib.Path('/game'))
    parser.add_argument('--perf-run', choices=('1', '2'), default='1', help='retain independent hardware A/B logs')
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
        result = subprocess.run([str(OUTPUT / 'RealtimeWaterProbe'), str(args.game_dir.resolve()), str(OUTPUT)] + (['--perf'] if args.perf else []),
            cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=300)
        (OUTPUT / ('RealtimeWaterProbe-perf-' + args.perf_run + '.log' if args.perf else 'RealtimeWaterProbe.log')).write_text(result.stdout)
        if args.perf and result.returncode == 0:
            isolated = {}
            for line in result.stdout.splitlines():
                if line.startswith('water-perf-isolated '):
                    parts = line.split()
                    fields = dict(part.split('=', 1) for part in parts[2:])
                    isolated.setdefault((parts[1], fields['mode']), []).append(fields)
                elif not line.startswith('water-perf-round '):
                    print(line)
            for (view, mode), rows in isolated.items():
                print('water-perf-isolated-summary', view, 'mode=' + mode,
                    ' '.join('median-' + field + '=%.6f' % statistics.median(float(row[field]) for row in rows)
                        for field in ('wall-ms', 'submit-ms', 'gpu-ms')))
            for view in ('cj', 'water', 'boundary'):
                before = isolated[(view, 'before-authored')]
                after = isolated[(view, 'after-convenience')]
                deltas = [float(b['wall-ms']) - float(a['wall-ms']) for a, b in zip(before, after)]
                print('water-perf-isolated-paired', view, 'rounds=8 median-delta-ms=%.6f min=%.6f max=%.6f' %
                    (statistics.median(deltas), min(deltas), max(deltas)))
        else:
            print(result.stdout, end='')
        result.check_returncode()
        assert 'water-probe-ok' in result.stdout
