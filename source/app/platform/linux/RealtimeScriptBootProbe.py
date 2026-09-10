#!/usr/bin/env python3
"""Build in native container (--build), run in a graphical host session (--run).

Checks real production startup presentation and exact terminal Unsupported exit.
Optional MAD_SA_BOOT_CAPTURE is a prefix for three diagnostic GL_BACK PPMs.
"""
import os
import re
import subprocess
import sys

sys.dont_write_bytecode = True
from RealtimeScriptHostProbe import OUTPUT, WORKSPACE, build_probe, options

if __name__ == '__main__':
    args = options(__doc__)
    name = 'RealtimeScriptBootProbe'
    if args.build:
        build_probe(name)
    else:
        env = os.environ.copy()
        if env.get('WAYLAND_DISPLAY'):
            env['SDL_VIDEODRIVER'] = 'wayland'
        command = [str(OUTPUT / name), '--play', '--new-game', '--game-dir', str(args.game_dir.resolve()), '--seconds', '5']
        result = subprocess.run(command, cwd=WORKSPACE, env=env, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUTPUT / (name + '.log')).write_text(result.stdout + f'runner-exit={result.returncode}\n')
        print(result.stdout, end='')
        assert result.returncode == 1, f'Expected runtime terminal exit 1, got {result.returncode}'
        captures = re.findall(r'^boot-capture .*$', result.stdout, re.MULTILINE)
        assert len(captures) == 3, captures
        for frame, (capture, count, ip, revision) in enumerate(zip(captures, (0, 256, 512), (200000, 202662, 205508), (0, 12, 13)), 1):
            assert capture.startswith(f'boot-capture PASS frame={frame} black=1 clock=08:00 paired=1 actor=1 startup=1 scheduler=1 mission={count} ip={ip} '), capture
            assert f'worldRevision=3 sourceCOL=1 overrides=14 disabled=13 garageReady=1 updates=50 flagsCleared=13 garageRevision={revision} cameraUnchanged=1 sourceBody=1 physics=1 ticks={frame - 1} ' in capture, capture
        assert 'boot-runtime PASS exit=1 swaps=3 fullboot=0' in result.stdout
        terminals = re.findall(r'^play-.*terminal .*$', result.stdout, re.MULTILINE)
        assert len(terminals) == 1 and terminals[0].startswith('play-script-terminal status=Unsupported thread=1 generation=1 ip=205876 opcode=0570 executed=27 '), terminals
        assert 'play-ok' not in result.stdout and 'play-fail' not in result.stdout and 'FAIL' not in result.stdout
        print('boot-probe PASS actual-captures=3 mission-quanta=0/256/512+27 mission-prefix=539 terminal=0570@205876 '
            'worldRevision=3 sourceCOL-overrides=14/13-disabled garage-ready=50-each-frame camera-unchanged=1 '
            'actual-source-body=1 real-physics=1 clothes-not-reached=1 runtime-exit=1 fullboot=0')
