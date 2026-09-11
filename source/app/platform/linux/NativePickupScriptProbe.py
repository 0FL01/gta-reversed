#!/usr/bin/env python3
"""Build/run the actual HUD-ready pickup script/controller probe."""
import argparse
import pathlib
import re
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
BUILD = WORKSPACE / 'build'
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativePickupScriptProbe'


def build():
    commands = subprocess.check_output(
        ['ninja', '-C', str(BUILD), '-t', 'commands', 'mad-sa-linux'], text=True
    ).splitlines()
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
    product_objects = [arg for arg in link if arg.endswith('.cpp.o')]
    subprocess.run(['ninja', '-C', str(BUILD), *product_objects], check=True)
    replaced = ('MainLinux', 'Realtime', 'NativeScriptSession', 'RealtimeScriptHost', NAME)
    link = [arg for arg in link if not any(arg.endswith('/' + unit + '.cpp.o') for unit in replaced)]
    link[link.index('-o') + 1] = str(OUTPUT / NAME)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in ('NativeScriptSession', 'RealtimeScriptHost', NAME):
            obj = OUTPUT / ('pickup-script-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-Werror', '-ffunction-sections',
                '-fdata-sections', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
            link.insert(1, str(obj))
        link.insert(1, '-Wl,--gc-sections')
        log.write(shlex.join(link) + '\n')
        log.flush()
        subprocess.run(link, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
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
        result = subprocess.run([str(OUTPUT / NAME), str(args.game_dir.resolve())], cwd=WORKSPACE,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=240)
        (OUTPUT / (NAME + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        summary = re.search(r'^native-pickup-script .*$', result.stdout, re.MULTILINE)
        assert summary and 'failures=0' in summary.group() and 'hud33=actual-GL' in summary.group()
        assert 'actualCommands=679 terminal=014B@207007' in summary.group()
        assert 'collection=controller-owned ring=20 staleRef=safe' in summary.group()
        assert 'fullboot=0' in summary.group() and 'FAIL' not in result.stdout
