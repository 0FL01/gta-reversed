#!/usr/bin/env python3
"""Isolated native car-generator main-thread adapter probe; labelled SCM fixture."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeCarGeneratorRuntimeProbe'


def build():
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(
        ['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
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
    units = ['StreamPager', 'NativeCollisionAssets', 'RealtimeGameplay', 'NativePlayerAssets', 'TexSample',
             'Handling', 'Collide', 'IfpAnim', 'CarPose', 'GxtText', 'MenuShot', 'NativePlayerActivity',
              'NativeGarages', 'NativeVehiclePool', 'NativeScriptSchema', 'NativeScriptCorpus',
              'NativeScriptSession', 'NativeScriptEntities', 'NativeEntryExits',
             'RealtimeScriptHost', 'NativeRestarts', 'NativeSourceRng', 'NativeCarGeneratorResidency',
             'NativeCarGenerators', 'NativeCarGeneratorRuntime', NAME]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    objects = []
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in units + ['oswrapper_linux']:
            source = SOURCE / (unit + '.cpp') if unit != 'oswrapper_linux' else SOURCE.parents[2] / 'oswrapper/oswrapper_linux.cpp'
            obj = OUTPUT / ('cargen-runtime-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(source), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        link = [flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lpthread', '-lm',
                '-o', str(OUTPUT / NAME)]
        log.write(shlex.join(link) + '\n')
        log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.build:
        build()
    else:
        result = subprocess.run([str(OUTPUT / NAME), str(args.game_dir.resolve())], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
        (OUTPUT / (NAME + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
