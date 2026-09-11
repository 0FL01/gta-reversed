#!/usr/bin/env python3
"""Check local source provenance, build/run the asset-free source RNG probe."""
import argparse
import pathlib
import re
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
REPO = SOURCE.parents[3]
OUTPUT = SOURCE.parents[4] / 'artifacts/graphics'
NAME = 'NativeSourceRngProbe'


def check_source():
    def read(relative):
        return (REPO / 'source' / relative).read_text()

    game = read('app/app_game.cpp')
    assert re.search(r'void GameInit\(\)\s*\{\s*VERIFY\(RwInitialize\(nullptr\)\);\s*'
                     r'srand\(RsTimer\(\)\);', game)
    assert re.search(r'uint32 RsTimer\(\)\s*\{\s*return psTimer\(\);', read('app/platform/platform.cpp'))
    assert re.search(r'uint32 psTimer\(\)\s*\{\s*return OS_TimeMS\(\);', read('app/platform/win/WinPs.cpp'))
    assert re.search(r'uint32 OS_TimeMS\(\)\s*\{\s*return static_cast<uint32>\('
                     r'GetOSWPerformanceTime\(\) / 1000000ULL\);', read('oswrapper/oswrapper_linux.cpp'))
    general = read('game_sa/General.cpp').split('uint16 CGeneral::GetRandomNumber()')[1].split('\n}')[0]
    assert 'RAND_MAX == 0x7FFF' in general and 'return rand();' in general
    header = read('game_sa/General.h')
    assert not re.search(r'^\s*#define BETTER_RNG\b', header, re.MULTILINE)
    assert '1.0f / static_cast<float>(RAND_MAX)' in header
    assert 'static_cast<float>(GetRandomNumber()) * RAND_MAX_FLOAT_RECIPROCAL' in header
    assert 'static_cast<float>(max) - 1.f' in header
    assert 'return static_cast<T>(to * t + from * (1.f - t));' in read('game_sa/common.h')
    car = read('game_sa/CarGenerator.cpp')
    insertion = car.index('CWorld::Add(vehicle);')
    alarm = car.index('CGeneral::GetRandomNumberInRange(0, 100) < m_nAlarmChance')
    lock = car.index('CGeneral::GetRandomNumberInRange(0, 100) < m_nDoorLockChance')
    assert insertion < alarm < lock
    prepared = read('app/platform/linux/NativeCarGenerators.h')
    assert 'SharedSourceRand15Required' in prepared and 'PostSpawnRand15Calls = 2;' in prepared
    print('source-rng-source-ok: GameInit/RsTimer/psTimer/OS_TimeMS CRT-rand15 local-range alarm-before-lock')


def build(cxx, sanitize):
    if not OUTPUT.is_dir():
        raise RuntimeError(f'Expected existing artifact directory: {OUTPUT}')
    command = [cxx, '-std=c++20', '-O2', '-pipe', '-pthread', '-UNDEBUG', '-Wall', '-Wextra',
               '-Wconversion', '-Werror', '-pedantic', '-ffp-contract=off']
    if sanitize:
        command += ['-fsanitize=undefined', '-fno-sanitize-recover=undefined']
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        objects = []
        for unit in ('NativeSourceRng', NAME):
            obj = OUTPUT / (unit + '.o')
            compile_command = command + ['-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(compile_command) + '\n')
            log.flush()
            subprocess.run(compile_command, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        link = command + objects + ['-o', str(OUTPUT / NAME)]
        log.write(shlex.join(link) + '\n')
        log.flush()
        subprocess.run(link, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


def run():
    result = subprocess.run([str(OUTPUT / NAME)], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=30)
    (OUTPUT / (NAME + '.log')).write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--source-check', action='store_true')
    parser.add_argument('--cxx', default='c++')
    parser.add_argument('--sanitize', action='store_true')
    args = parser.parse_args()
    check_source()
    if not args.run and not args.source_check:
        build(args.cxx, args.sanitize)
    if not args.build and not args.source_check:
        run()
