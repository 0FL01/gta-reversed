#!/usr/bin/env python3
"""Isolated native build; actual owned main53/COL probe, outputs artifacts only."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeLiveEntityBoundsProbe'


def build(sanitized=False):
    name = NAME + ('-sanitized' if sanitized else '')
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
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
    # NativeVehicleState's optional model-info handle changes its layout: rebuild
    # every production consumer locally, without touching shared build objects.
    units = [NAME, 'NativeLiveEntityBounds', 'NativeGeneratedVehicleAssets', 'NativeVehiclePool',
             'NativeGaragesRuntime', 'NativeCarGenerators', 'NativeCarGeneratorRuntime',
              'NativeCarGeneratorResidency', 'NativeScriptSchema', 'NativeScriptCorpus',
                 'NativeScriptSession', 'NativeScriptServiceTransaction', 'NativeStuntJumps', 'RealtimeScriptHost',
                 'NativeScriptPeds']
    # A concurrently integrated host service may precede the parent CMake edit.
    if (SOURCE / 'NativeRestarts.cpp').exists():
        units.append('NativeRestarts')
    link = [a for a in link if not any(a.endswith('/' + n + '.cpp.o') for n in ['MainLinux', 'Realtime', *units])]
    link[link.index('-o') + 1] = str(OUTPUT / name)
    instrumentation = ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-O1'] if sanitized else []
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / (name + '-build.log')).open('w') as log:
        for unit in units:
            obj = OUTPUT / (name + '-' + unit + '.o')
            command = flags + instrumentation + ['-UNDEBUG', '-Wall', '-Wextra', '-ffp-contract=off', '-fno-fast-math',
                               '-ffunction-sections', '-fdata-sections', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            link.insert(1, str(obj))
        link.insert(1, '-Wl,--gc-sections')
        link[1:1] = instrumentation
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)


def run(game, sanitized=False):
    name = NAME + ('-sanitized' if sanitized else '')
    trace = OUTPUT / (name + '-open.trace')
    # LeakSanitizer cannot run under ptrace. The ordinary route retains the
    # independent no-IO/no-EXE strace gate; the sanitizer route runs untraced.
    command = [str(OUTPUT / name), str(game.resolve())]
    if not sanitized:
        command = ['strace', '-f', '-s', '512', '-e', 'trace=open,openat,write', '-o', str(trace), *command]
    result = subprocess.run(command, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    (OUTPUT / (name + '.log')).write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()
    if sanitized:
        return
    lines = trace.read_text().splitlines()
    assert not any('.exe' in line.lower() for line in lines if 'open(' in line or 'openat(' in line)
    start = next(i for i, line in enumerate(lines) if 'query-only-begin' in line)
    end = next(i for i, line in enumerate(lines) if 'query-only-end' in line)
    assert not any('open(' in line or 'openat(' in line for line in lines[start:end])
    print('no-exe-access PASS; query-only file opens=0')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, default=WORKSPACE / 'Grand-Theft-Auto-San-Andreas')
    parser.add_argument('--sanitized', action='store_true', help='ASan/UBSan rebuilt owner path; run without ptrace')
    args = parser.parse_args()
    build(args.sanitized) if args.build else run(args.game_dir, args.sanitized)
