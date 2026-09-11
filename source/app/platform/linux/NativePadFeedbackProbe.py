#!/usr/bin/env python3
"""Build/run the SDL3 virtual-gamepad native feedback probe."""
import argparse
import os
import pathlib
import re
import shlex
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativePadFeedbackProbe'


def verify_source():
    game = WORKSPACE / 'gta-reversed/source/game_sa'
    pickup = (game / 'Pickup.cpp').read_text()
    pad = (game / 'Pad.cpp').read_text()
    menu = (game / 'MenuManager.cpp').read_text()
    checks = (
        (pickup, r'StartShake\(120,\s*100u,\s*0\)', 'pickup StartShake tuple'),
        (pad, r'!FrontEndMenuManager\.m_PrefsUseVibration\s*\|\|\s*CCutsceneMgr::ms_running',
         'source vibration/cutscene gate'),
        (pad, r'ShakeDur\s*=\s*time', 'source shake duration store'),
        (pad, r'ShakeFreq\s*=\s*freq', 'source shake frequency store'),
        (menu, r'm_PrefsUseVibration\s*=\s*true', 'source vibration default'),
    )
    for text, pattern, description in checks:
        if not re.search(pattern, text):
            raise RuntimeError(f'missing {description}')


def build():
    verify_source()
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(
        ['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True
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
    for i, arg in enumerate(flags):
        if i and flags[i - 1] == '-isystem' and arg.startswith('/opt/conan/') and not pathlib.Path(arg).exists():
            matches = list(pathlib.Path('/opt/conan/p').glob('**/p/include/SDL3/SDL.h'))
            if len(matches) == 1:
                flags[i] = str(matches[0].parents[1])
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(part for part in line.split('&&') if ' -o mad-sa-linux ' in part))
    units = ('NativePadFeedback', NAME)
    removed = units + ('MainLinux', 'Realtime')
    link = [arg for arg in link if not any(arg.endswith('/' + unit + '.cpp.o') for unit in removed)]
    for i, arg in enumerate(link):
        path = pathlib.Path(arg)
        if arg.startswith('/opt/conan/') and path.suffix in ('.a', '.so') and not path.exists():
            matches = list(pathlib.Path('/opt/conan/p').glob('**/p/lib/' + path.name))
            if len(matches) == 1:
                link[i] = str(matches[0])
    link[link.index('-o') + 1] = str(OUTPUT / NAME)
    OUTPUT.mkdir(parents=True, exist_ok=True)
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in units:
            obj = OUTPUT / ('pad-feedback-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            link.insert(1, str(obj))
        link.insert(1, '-Wl,--gc-sections')
        log.write(shlex.join(link) + '\n')
        log.flush()
        subprocess.run(link, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    args = parser.parse_args()
    if args.build:
        build()
    else:
        verify_source()
        environment = os.environ.copy()
        environment.setdefault('SDL_VIDEODRIVER', 'dummy')
        result = subprocess.run([str(OUTPUT / NAME)], text=True, env=environment,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30)
        (OUTPUT / (NAME + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
