#!/usr/bin/env python3
"""Isolated Ninja-derived production garage driver probe; no parent object writes."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeGaragesRuntimeProbe'


def build():
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
            flags.append(template[i]); i += 1
    units = ['StreamPager', 'NativeCollisionAssets', 'RealtimeGameplay', 'NativePlayerAssets', 'TexSample', 'Handling',
             'Collide', 'IfpAnim', 'CarPose', 'GxtText', 'MenuShot',
              'NativePlayerActivity', 'NativeGarages', 'NativeVehiclePool', 'NativeGaragesRuntime',
                'NativeScriptSchema', 'NativeScriptCorpus', 'NativeScriptSession', 'NativeScriptServiceTransaction', 'NativeStuntJumps',
             'NativeScriptEntities', 'NativeEntryExits', 'RealtimeScriptHost', 'NativeRestarts',
             'NativeSourceRng', 'NativeCarGenerators', 'NativeCarGeneratorResidency', NAME]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    objects = []
    with (OUTPUT / (NAME+'-build.log')).open('w') as log:
        for unit in units:
            obj = OUTPUT / ('garage-runtime-'+unit+'.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit+'.cpp')), '-o', str(obj)]
            log.write(shlex.join(command)+'\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        # Use the current native target's complete dependency closure: the
        # script host acquired new owner TUs since this isolated probe began.
        # Replace only the explicitly recompiled probe/owner objects, and omit
        # both executable entrypoints. Do not silently omit new host services.
        line = next(c for c in commands if ' -o mad-sa-linux ' in c)
        link = shlex.split(next(p for p in line.split('&&') if ' -o mad-sa-linux ' in p))
        excluded = [*units, 'MainLinux', 'Realtime']
        link = [arg for arg in link if not any(arg.endswith('/'+unit+'.cpp.o') for unit in excluded)]
        link.insert(1, '-Wl,--gc-sections')
        link[1:1] = objects
        link[link.index('-o') + 1] = str(OUTPUT / NAME)
        log.write(shlex.join(link)+'\n'); log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)


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
        (OUTPUT / (NAME+'.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
