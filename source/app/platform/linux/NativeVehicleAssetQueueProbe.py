#!/usr/bin/env python3
"""Isolated same-worker vehicle/world probe. Every output has its own prefix."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeVehicleAssetQueueProbe'


def build():
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, index = [], 0
    while index < len(template):
        if template[index] in ('-MT', '-MF', '-o', '-c'):
            index += 2
        elif template[index] == '-MD':
            index += 1
        else:
            flags.append(template[index])
            index += 1
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(p for p in line.split('&&') if ' -o mad-sa-linux ' in p))
    units = ['NativeVehicleAssetQueue', 'NativeGeneratedVehicleAssets', 'NativeCarGenerators',
             'NativeCollisionAssets', 'RealtimeGameplay', 'StreamPager', 'CarPose', 'TexSample', NAME]
    link = [arg for arg in link if not any(arg.endswith('/' + unit + '.cpp.o') for unit in units + ['MainLinux', 'Realtime'])]
    link[link.index('-o') + 1] = str(OUTPUT / NAME)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        objects = {}
        for unit in units:
            obj = OUTPUT / (NAME + '-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects[unit] = str(obj)
            link.insert(1, str(obj))
        specs = [
            ('StreamPager_Update', objects['StreamPager'], 'bool',
             'float x, float y, float z, WorldShotScene& s, E2EPagerFrame& f, char* e, std::size_t n, const std::shared_ptr<const NativePlacementOverrides>& o, std::vector<NativePlacementIdentity>* p',
             'x,y,z,s,f,e,n,o,p', 0, ''),
            ('NativeGeneratedVehicleAssets_Load', objects['NativeGeneratedVehicleAssets'], 'NativeGeneratedVehicleAssetResult',
             'const char* g, const NativeCarGeneratorModelDefinition& d, const NativeCollisionAssets& c, NativeGeneratedVehicleAsset& o',
             'g,d,c,o', 1, ''),
            ('TexSample_LinkedParse', objects['TexSample'], 'LinkedClump',
             'const uint8_t* b, std::size_t n, rw::TexDictionary* p, rw::TexDictionary* const* f, std::size_t c, rw::TexDictionary* v',
             'b,n,p,f,c,v', 2, ''),
            ('OS_FileOpen', next(a for a in link if a.endswith('/oswrapper_linux.cpp.o')), 'int32',
             'OSFileDataArea a, void** o, const char* p, OSFileAccessType t', 'a,o,p,t', 3,
             'if (NativeVehicleAssetQueueProbe_FailOpen(p)) { *o = nullptr; return 1; }'),
            ('OS_SetFilePathOffset', next(a for a in link if a.endswith('/oswrapper_linux.cpp.o')), 'void',
             'const char* p', 'p', 4, 'NativeVehicleAssetQueueProbe_Offset(p);'),
        ]
        wrappers = ['#include "app/platform/linux/NativeVehicleAssetQueue.h"',
                    '#include "app/platform/linux/StreamPager.h"', '#include "app/platform/linux/TexSample.h"',
                    'using int32 = int32_t; using uint32 = uint32_t; using int64 = int64_t; using uint64 = uint64_t;',
                    '#ifndef __stdcall', '#define __stdcall', '#endif', '#include "oswrapper/oswrapper.h"',
                    'void NativeVehicleAssetQueueProbe_Trace(int);', 'bool NativeVehicleAssetQueueProbe_FailOpen(const char*);',
                    'void NativeVehicleAssetQueueProbe_Offset(const char*);']
        for name, obj, result, parameters, arguments, kind, extra in specs:
            symbols = subprocess.check_output(['nm', '--defined-only', obj], cwd=directory, text=True).splitlines()
            symbol, = [line.split()[-1] for line in symbols if len(line.split()) == 3 and line.split()[1] == 'T'
                       and name in line.split()[-1] and not line.split()[-1].endswith('.cold')]
            wrappers += [f'{result} Real{name}({parameters}) asm("__real_{symbol}");',
                         f'{result} Wrap{name}({parameters}) asm("__wrap_{symbol}");',
                         f'{result} Wrap{name}({parameters}) {{ NativeVehicleAssetQueueProbe_Trace({kind}); {extra} return Real{name}({arguments}); }}']
            link.insert(1, '-Wl,--wrap=' + symbol)
        wrapper_source = OUTPUT / (NAME + '-wrappers.cpp')
        wrapper_source.write_text('\n'.join(wrappers) + '\n')
        wrapper_obj = OUTPUT / (NAME + '-wrappers.o')
        subprocess.run(flags + ['-c', str(wrapper_source), '-o', str(wrapper_obj)], cwd=directory,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        link.insert(1, str(wrapper_obj)); link.insert(1, '-Wl,--gc-sections')
        log.write(shlex.join(link) + '\n'); log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
        # Compile/link the existing default-API probe WITHOUT the new queue .cpp
        # or any wrappers. This detects accidental mandatory link dependencies.
        legacy_obj = OUTPUT / (NAME + '-legacy-default.o')
        subprocess.run(flags + ['-UNDEBUG', '-ffunction-sections', '-fdata-sections', '-c',
                               str(SOURCE / 'RealtimeStreamingProbe.cpp'), '-o', str(legacy_obj)],
                       cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
        legacy = [arg for arg in link if arg not in (str(wrapper_obj), objects[NAME], objects['NativeVehicleAssetQueue'])
                  and not arg.startswith('-Wl,--wrap=')]
        legacy.insert(1, str(legacy_obj))
        legacy[legacy.index('-o') + 1] = str(OUTPUT / (NAME + '-legacy-default'))
        subprocess.run(legacy, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


def run(game):
    trace = OUTPUT / (NAME + '-open.trace')
    command = ['strace', '-f', '-e', 'trace=open,openat', '-o', str(trace), str(OUTPUT / NAME), str(game.resolve())]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=360)
    (OUTPUT / (NAME + '.log')).write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()
    assert '.exe' not in trace.read_text().lower(), 'EXE access in file-open trace'
    print('vehicle-queue no-exe-access-ok')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', required=True, type=pathlib.Path)
    args = parser.parse_args()
    if args.build:
        build()
    else:
        run(args.game_dir)
