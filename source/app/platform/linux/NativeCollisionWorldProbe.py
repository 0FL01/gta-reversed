#!/usr/bin/env python3
"""Build with native Ninja flags; verify source COL and five-sphere gameplay."""
import argparse
import os
import re
import shlex
import subprocess
import sys
sys.dont_write_bytecode = True
from RealtimeScriptHostProbe import OUTPUT, WORKSPACE, build_probe, options

def build_runtime():
    """Exact product objects/link flags, including pending parent TU integration."""
    build = WORKSPACE / 'build'
    commands = subprocess.check_output(
        ['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    line = next(c for c in commands if ' -o mad-sa-linux ' in c)
    link = shlex.split(next(part for part in line.split('&&') if ' -o mad-sa-linux ' in part))
    subprocess.run(['ninja', '-C', str(build)] + [a for a in link if a.endswith('.cpp.o')], check=True)
    # build_probe compiled this TU from the same native flags if CMake has not
    # yet registered it. Once registered, use the normal product object.
    if not any(a.endswith('/NativeScriptEntities.cpp.o') for a in link):
        link.insert(1, str(OUTPUT / 'NativeScriptEntities.o'))
    link[link.index('-o') + 1] = str(OUTPUT / 'source-col-mad-sa-linux')
    with (OUTPUT / 'source-col-runtime-build.log').open('w') as log:
        subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)

if __name__ == '__main__':
    # Runtime mode is explicit: the default probe needs no window/display.
    runtime_parser = argparse.ArgumentParser(add_help=False, allow_abbrev=False)
    runtime_parser.add_argument('--runtime', action='store_true')
    runtime, rest = runtime_parser.parse_known_args()
    sys.argv[1:] = rest
    args = options(__doc__)
    name = 'NativeCollisionWorldProbe'
    if args.build:
        build_probe(name)
        if runtime.runtime:
            build_runtime()
    else:
        command = [str(OUTPUT / name), str(args.game_dir.resolve())]
        if runtime.runtime:
            name = 'source-col-runtime-curb'
            command = [str(OUTPUT / 'source-col-mad-sa-linux'), '--play', '--demo-curb',
                '--seconds', '7', '--game-dir', str(args.game_dir.resolve())]
        result = subprocess.run(command, cwd=WORKSPACE, env=os.environ.copy(),
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, timeout=180)
        log_name = f'{name}-{os.getuid()}.log' if runtime.runtime else name + '.log'
        (OUTPUT / log_name).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        if runtime.runtime:
            assert 'play-ok' in result.stdout and 'play-fail' not in result.stdout
            assert 'play-col-curb low=12.382812500 high=12.546875000 rise=0.164062500' in result.stdout
        else:
            assert 'native-collision-world PASS' in result.stdout
            timings = dict(re.findall(r'COL Tick benchmark (\S+) .*?meanMs=([0-9.]+)', result.stdout))
            for reference, indexed in [('exhaustive', 'indexed'), ('CJ-source-900m-exhaustive', 'CJ-source-900m-indexed')]:
                ratio = float(timings[reference]) / float(timings[indexed])
                assert ratio > 2, f'Volume broadphase Tick regression: {indexed} speedup={ratio:.2f}'
                print(f'PASS fixed-step production Tick {indexed} speedup={ratio:.2f}x versus exhaustive source volumes')
