#!/usr/bin/env python3
"""Ninja-derived isolated garage/host probe; never writes parent's build objects."""
import argparse
import pathlib
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'


def build(host):
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
    link = shlex.split(next(p for p in line.split('&&') if ' -o mad-sa-linux ' in p))
    # Rebuild the coupled placement/COL ABI into artifacts as well; parent may
    # still be integrating those headers while its normal objects are older.
    units = ['StreamPager', 'NativeCollisionAssets', 'RealtimeGameplay', 'NativeGarages',
             'NativeScriptSession', 'NativeScriptEntities', 'RealtimeScriptHost', 'NativeGaragesProbe']
    if host:
        units += ['RealtimeScriptHostProbe', 'RealtimeScriptHostGpuProbe', 'NativeEntryExitsProbe']
    excluded = units + ['MainLinux', 'Realtime']
    link = [a for a in link if not any(a.endswith('/'+n+'.cpp.o') for n in excluded)]
    name = 'garage-host-probe' if host else 'NativeGaragesProbe'
    link[link.index('-o')+1] = str(OUTPUT / name)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / (name+'-build.log')).open('w') as log:
        for unit in units:
            obj = OUTPUT / ('garage-'+unit+'.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections']
            if unit == 'NativeGaragesProbe' and not host:
                command += ['-DNATIVE_GARAGES_STANDALONE']
            command += ['-c', str(SOURCE / (unit+'.cpp')), '-o', str(obj)]
            log.write(shlex.join(command)+'\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            link.insert(1, str(obj))
        link.insert(1, '-Wl,--gc-sections')
        log.write(shlex.join(link)+'\n'); log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / name)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    modes = parser.add_mutually_exclusive_group(required=True)
    modes.add_argument('--build', action='store_true')
    modes.add_argument('--run', action='store_true')
    parser.add_argument('--host', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.build:
        build(args.host)
    else:
        name = 'garage-host-probe' if args.host else 'NativeGaragesProbe'
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())], text=True,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
        (OUTPUT / (name+'.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
