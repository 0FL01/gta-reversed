#!/usr/bin/env python3
"""Ninja-derived source predicate and production controller activity probe."""

import argparse
import pathlib
import shlex
import subprocess


SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
BUILD = WORKSPACE / 'build'
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativePlayerActivityProbe'
BUILD_LOG = OUTPUT / 'round6-activity-build.log'
RUN_LOG = OUTPUT / 'round6-activity.log'


def build():
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with BUILD_LOG.open('w') as log:
        subprocess.run(['ninja', '-C', str(BUILD), 'mad-sa-linux'], cwd=WORKSPACE,
                       stdout=log, stderr=subprocess.STDOUT, check=True)
        commands = subprocess.check_output(
            ['ninja', '-C', str(BUILD), '-t', 'commands', 'mad-sa-linux'], text=True
        ).splitlines()
        template = shlex.split(next(command for command in commands if '-c ' in command and '/Realtime.cpp' in command))
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

        line = next(command for command in commands if ' -o mad-sa-linux ' in command)
        link = shlex.split(next(part for part in line.split('&&') if ' -o mad-sa-linux ' in part))
        units = ('NativePlayerActivity', NAME)
        link = [arg for arg in link if not any(arg.endswith('/' + unit + '.cpp.o') for unit in units + ('MainLinux',))]
        link[link.index('-o') + 1] = str(OUTPUT / NAME)

        objects = []
        for unit in units:
            obj = OUTPUT / ('round6-activity-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        link[1:1] = objects
        log.write(shlex.join(link) + '\n')
        log.flush()
        subprocess.run(link, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)


def run(game_dir):
    result = subprocess.run([str(OUTPUT / NAME), str(game_dir.resolve())], cwd=WORKSPACE,
                            text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    RUN_LOG.write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    build() if args.build else run(args.game_dir)
