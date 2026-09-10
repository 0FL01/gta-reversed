#!/usr/bin/env python3
"""Build in native container (--build), run in a graphical host session (--run).

Checks real production startup presentation and exact terminal Unsupported exit.
Optional MAD_SA_BOOT_CAPTURE writes a diagnostic PPM of the same GL_BACK frame.
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
        assert 'boot-capture PASS frame=1 black=1 clock=08:00 paired=1 actor=1 startup=1' in result.stdout
        assert 'boot-runtime PASS exit=1 swaps=1 fullboot=0' in result.stdout
        assert len(re.findall(r'play-script-terminal status=Unsupported thread=1 generation=1 ip=201129 opcode=02B9 executed=135 ', result.stdout)) == 1
        assert 'play-ok' not in result.stdout and 'play-fail' not in result.stdout and 'FAIL' not in result.stdout
        print('boot-probe PASS actual-startup-frame=1 mission-prefix=135 terminal=02B9@201129 runtime-exit=1 fullboot=0')
