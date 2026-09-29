#!/usr/bin/env python3
"""Build/run the real script CPU/COL/GPU publication regression on Wayland."""
import os
import subprocess
import sys

sys.dont_write_bytecode = True
from RealtimeScriptBootProbe import OUTPUT, WORKSPACE, build_probe, options

if __name__ == '__main__':
    args = options(__doc__)
    name = 'NativeScriptWorldPublicationProbe'
    if args.build:
        build_probe(name, 'script-world-publication-build.log')
    else:
        env = dict(os.environ, SDL_VIDEODRIVER='wayland')
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())],
            cwd=WORKSPACE, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=240)
        (OUTPUT / 'script-world-publication.log').write_text(
            result.stdout + f'runner-exit={result.returncode}\n')
        print(result.stdout, end='')
        result.check_returncode()
        assert 'script-world-publication-ok' in result.stdout
        assert 'gpu-before-ready=1 adjacent-services=paired cancel=retired building-ground=owned rng-draws=0' in result.stdout
