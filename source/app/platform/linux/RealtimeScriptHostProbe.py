#!/usr/bin/env python3
"""Build in the native dev container (--build), run against owned assets (--run).

Reusable Ninja-derived compiler/linker flags; outputs only artifacts/graphics.
The host probe checks real SCM services, not generated/mocked startup data.
"""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'


def build_probe(name):
    build = WORKSPACE / 'build'
    OUTPUT.mkdir(parents=True, exist_ok=True)
    commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags = []
    i = 0
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
    # Build production objects, not the product link: children can verify their
    # new TUs before the parent's CMake integration. No build-file edits here.
    subprocess.run(['ninja', '-C', str(build)] + [a for a in link if a.endswith('.cpp.o')], check=True)
    link[link.index('-o') + 1] = str(OUTPUT / name)
    names = [name]
    for extra in ('NativeScriptEntities', 'NativeCollisionAssets', 'NativeEntryExits'):
        if (SOURCE / (extra + '.cpp')).exists() and not any(a.endswith('/' + extra + '.cpp.o') for a in link):
            names.append(extra)
    if name == 'RealtimeScriptHostProbe':
        names.append('RealtimeScriptHostGpuProbe')
        names.append('NativeEntryExitsProbe')
    compile_commands = []
    for unit in names:
        obj = OUTPUT / (unit + '.o')
        compile_commands.append(flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
            '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)])
        link.insert(1, str(obj))
    link.insert(1, '-Wl,--gc-sections')
    with (OUTPUT / (name + '-build.log')).open('w') as log:
        for command in (*compile_commands, link):
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / name)


def options(description):
    parser = argparse.ArgumentParser(description=description)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path,
        default=WORKSPACE / 'Grand-Theft-Auto-San-Andreas')
    return parser.parse_args()


if __name__ == '__main__':
    args = options(__doc__)
    name = 'RealtimeScriptHostProbe'
    if args.build:
        build_probe(name)
    else:
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())],
            cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUTPUT / (name + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        assert 'host-probe failures=0 firstpass=53 mission-prefix=132 terminal=0518@201080' in result.stdout
