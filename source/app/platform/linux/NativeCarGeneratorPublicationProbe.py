#!/usr/bin/env python3
"""Build isolated Ninja-derived test TU; run actual LiveWorld on host Wayland."""
import os
import subprocess
import sys

sys.dont_write_bytecode = True
from RealtimeScriptBootProbe import OUTPUT, WORKSPACE, build_probe, options

if __name__ == '__main__':
    args = options(__doc__)
    name = 'NativeCarGeneratorPublicationProbe'
    if args.build:
        build_probe(name, 'cargens-boot-publication-build.log')
    else:
        env = os.environ.copy()
        env['SDL_VIDEODRIVER'] = 'wayland'
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())], cwd=WORKSPACE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        (OUTPUT / 'cargens-boot-publication.log').write_text(result.stdout + f'runner-exit={result.returncode}\n')
        print(result.stdout, end='')
        result.check_returncode()
        assert 'publication-ready PASS generation=4 ' in result.stdout
        assert 'publication-probe PASS worker=4 ready-after-gpu-before-swap=1 pending=5 ' in result.stdout
        assert 'retained=4 error-retained=1 false-cleanup=0 fullboot=0' in result.stdout
        assert result.stdout.count('play-cargen-residency-terminal status=Unsupported generation=5 ') == 2
        assert result.stdout.count('play-cargen-residency-terminal status=Error generation=4 ') == 1
        assert 'FAIL' not in result.stdout
