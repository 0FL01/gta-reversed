#!/usr/bin/env python3
"""Build the production-object joint pickup probe and run exactly four lanes."""

import argparse
import os
import pathlib
import re
import shlex
import subprocess
import time


SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
BUILD = WORKSPACE / 'build'
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativePickupFeedbackProbe'
LANES = 4


def verify_source():
    game = WORKSPACE / 'gta-reversed/source/game_sa/Pickup.cpp'
    realtime = SOURCE / 'Realtime.cpp'
    probe = SOURCE / (NAME + '.cpp')
    checks = (
        (game.read_text(), r'StartShake\(120,\s*100u,\s*0\)', 'source pickup shake tuple'),
        (realtime.read_text(), r'padFeedback\.HandleEvent\(event\)', 'parent typed-event forwarding'),
        (realtime.read_text(), r'padFeedback\.Update\(true\).*?padFeedback\.Update\(false\)',
         'parent pause/resume updates'),
        (realtime.read_text(),
         r'while\s*\(const auto shake = entities\.ConsumePadShake\(\)\)\s*\{.*?'
         r'padFeedback\.Submit\(\*shake,\s*true\)', 'parent consume-to-submit handoff'),
    )
    for text, pattern, description in checks:
        if not re.search(pattern, text, re.DOTALL):
            raise RuntimeError(f'missing {description}')
    if re.search(r'\bSDL_Quit\s*\(', probe.read_text()):
        raise RuntimeError('joint probe must not use global SDL_Quit')


def native_commands():
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

    for index, argument in enumerate(flags):
        if index and flags[index - 1] == '-isystem' and argument.startswith('/opt/conan/') and not pathlib.Path(argument).exists():
            matches = list(pathlib.Path('/opt/conan/p').glob('**/p/include/SDL3/SDL.h'))
            if len(matches) == 1:
                flags[index] = str(matches[0].parents[1])
    for index, argument in enumerate(link):
        path = pathlib.Path(argument)
        if argument.startswith('/opt/conan/') and path.suffix in ('.a', '.so') and not path.exists():
            matches = list(pathlib.Path('/opt/conan/p').glob('**/p/lib/' + path.name))
            if len(matches) == 1:
                link[index] = str(matches[0])
    return flags, link


def build():
    verify_source()
    OUTPUT.mkdir(parents=True, exist_ok=True)
    flags, link = native_commands()
    product_objects = [argument for argument in link if argument.endswith('.cpp.o')]
    subprocess.run(['ninja', '-C', str(BUILD), *product_objects], cwd=WORKSPACE, check=True)
    link = [argument for argument in link if not argument.endswith('/MainLinux.cpp.o')]
    link[link.index('-o') + 1] = str(OUTPUT / NAME)
    obj = OUTPUT / ('round7-' + NAME + '.o')
    compile_command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror', '-ffunction-sections',
                               '-fdata-sections', '-c', str(SOURCE / (NAME + '.cpp')), '-o', str(obj)]
    link[1:1] = ['-Wl,--gc-sections', str(obj)]
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for command in (compile_command, link):
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=BUILD, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


def run(game_dir):
    verify_source()
    environment = os.environ.copy()
    environment.setdefault('SDL_VIDEODRIVER', 'dummy')
    started = time.monotonic()
    processes = [
        subprocess.Popen([str(OUTPUT / NAME), str(game_dir.resolve())], cwd=WORKSPACE, env=environment,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for _ in range(LANES)
    ]
    outputs = []
    try:
        for lane, process in enumerate(processes):
            lane_started = time.monotonic()
            output, _ = process.communicate(timeout=300)
            elapsed = time.monotonic() - lane_started
            (OUTPUT / f'{NAME}-lane{lane}.log').write_text(output)
            outputs.append(output)
            print(f'parallel-lane={lane} waitSeconds={elapsed:.3f}')
            print(output, end='')
            if process.returncode:
                raise subprocess.CalledProcessError(process.returncode, process.args, output=output)
            summary = re.search(r'^native-pickup-feedback .*$', output, re.MULTILINE)
            assert summary and 'failures=0' in summary.group()
            assert 'actualCommands=679 terminal=014B@207007 hud33=actual-GL' in summary.group()
            assert 'callback=1 motors=25700/25700 durationMs=120' in summary.group()
            assert 'ring=0214-true-false-negated-true' in summary.group()
            assert 'noDevice=normal unsupported=explicit virtual-only=1' in summary.group()
            assert 'originalPSAVE=0 persistence=memory-only saveFrontend=unimplemented fullboot=0' in summary.group()
            assert 'FAIL' not in output
    except BaseException:
        for process in processes:
            if process.poll() is None:
                process.terminate()
        raise
    (OUTPUT / (NAME + '.log')).write_text(''.join(
        f'parallel-lane={lane}\n{output}' for lane, output in enumerate(outputs)))
    print(f'{NAME} parallel={LANES} wallSeconds={time.monotonic() - started:.3f}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    build() if args.build else run(args.game_dir)
