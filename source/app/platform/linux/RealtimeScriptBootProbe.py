#!/usr/bin/env python3
"""Build in native container (--build), run in a graphical host session (--run).

Checks real production startup presentation and exact terminal Unsupported exit.
Optional MAD_SA_BOOT_CAPTURE is a prefix for five diagnostic GL_BACK PPMs.
"""
import os
import re
import shlex
import subprocess
import sys

sys.dont_write_bytecode = True
from RealtimeScriptHostProbe import OUTPUT, SOURCE, WORKSPACE, options


def build_probe(name, log_name):
    """Link the parent's built product modules with one isolated test TU."""
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
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(part for part in line.split('&&') if ' -o mad-sa-linux ' in part))
    link = [arg for arg in link if not any(arg.endswith('/' + unit + '.cpp.o') for unit in ('MainLinux', 'Realtime'))]
    for unit in ('NativeSourceRng', 'NativeCarGenerators', 'NativeCarGeneratorResidency', 'NativeCarGeneratorRuntime'):
        assert any(arg.endswith('/' + unit + '.cpp.o') for arg in link), f'Parent must integrate {unit} first'
    assert all((directory / arg).exists() for arg in link if arg.endswith('.cpp.o')), 'Build the production target first'
    OUTPUT.mkdir(parents=True, exist_ok=True)
    obj = OUTPUT / ('cargens-boot-' + name + '.o')
    compile_command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
        '-c', str(SOURCE / (name + '.cpp')), '-o', str(obj)]
    link[link.index('-o') + 1] = str(OUTPUT / name)
    link[1:1] = ['-Wl,--gc-sections', str(obj)]
    with (OUTPUT / log_name).open('w') as log:
        for command in (compile_command, link):
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / name)

if __name__ == '__main__':
    args = options(__doc__)
    name = 'RealtimeScriptBootProbe'
    if args.build:
        build_probe(name, 'cargens-boot-build.log')
    else:
        env = os.environ.copy()
        if env.get('WAYLAND_DISPLAY'):
            env['SDL_VIDEODRIVER'] = 'wayland'
        command = [str(OUTPUT / name), '--play', '--new-game', '--game-dir', str(args.game_dir.resolve()), '--seconds', '5']
        result = subprocess.run(command, cwd=WORKSPACE, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUTPUT / 'cargens-boot-runtime.log').write_text(result.stdout + f'runner-exit={result.returncode}\n')
        print(result.stdout, end='')
        assert result.returncode == 1, f'Expected runtime terminal exit 1, got {result.returncode}'
        captures = re.findall(r'^boot-capture .*$', result.stdout, re.MULTILINE)
        assert len(captures) == 5, captures
        for frame, (capture, count, ip, revision) in enumerate(zip(captures, (0, 256, 512, 768, 1024),
                (200000, 202662, 205508, 207969, 210403), (0, 12, 13, 13, 13)), 1):
            assert capture.startswith(f'boot-capture PASS frame={frame} black=1 clock=08:00 paired=1 actor=1 startup=1 scheduler=1 mission={count} ip={ip} '), capture
            assert f'worldRevision=3 sourceCOL=1 overrides=14 disabled=13 garageReady=1 updates=50 flagsCleared=13 garageRevision={revision} cameraUnchanged=1 sourceBody=1 physics=1 ticks={frame - 1} ' in capture, capture
        generators = re.findall(r'^boot-cargens .*$', result.stdout, re.MULTILINE)
        assert len(generators) == 5, generators
        seeds = []
        for frame, (line, quarter, created) in enumerate(zip(generators, (1, 2, 3, 0, 1), (0, 0, 0, 9, 10)), 1):
            assert line.startswith(f'boot-cargens PASS frame={frame} quarter={quarter} sources=22 registered={88 + created} creates={created} switches={created} demands=0 poolCreated=0 '), line
            seed = re.search(r'seed=(\d+) draws=0 borrowed=1$', line)
            assert seed and 0 <= int(seed[1]) <= 0xffffffff, line
            seeds.append(int(seed[1]))
        assert len(set(seeds)) == 1, seeds  # one platform capture; no fixed seed oracle
        assert 'boot-runtime PASS exit=1 swaps=5 fullboot=0' in result.stdout
        terminals = re.findall(r'^play-.*terminal .*$', result.stdout, re.MULTILINE)
        assert len(terminals) == 1 and terminals[0].startswith('play-script-terminal status=Unsupported thread=1 generation=1 ip=212086 opcode=04CE executed=183 '), terminals
        assert 'play-ok' not in result.stdout and 'play-fail' not in result.stdout and 'FAIL' not in result.stdout
        print('boot-probe PASS actual-captures=5 mission-quanta=0/256/512/768/1024+183 mission-prefix=1207 terminal=04CE@212086 '
            'worldRevision=3 sourceCOL-overrides=14/13-disabled garage-ready=50-each-frame camera-unchanged=1 '
            'actual-source-body=1 real-physics=1 quarters=1/2/3/0/1 sources=22 initial=88 creates=10 switches=10 '
            'no-demand=1 pool-created=0 rng-draws=0 borrowed-generation=3 clothes-not-reached=1 runtime-exit=1 fullboot=0')
