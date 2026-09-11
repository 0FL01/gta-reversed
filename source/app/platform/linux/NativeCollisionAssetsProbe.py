#!/usr/bin/env python3
"""Read-only COL diagnosis and owned model lookup; not a rendered/runtime gate."""
import subprocess
import sys

sys.dont_write_bytecode = True
from RealtimeScriptHostProbe import OUTPUT, WORKSPACE, build_probe, options

if __name__ == '__main__':
    args = options(__doc__)
    name = 'NativeCollisionAssetsProbe'
    if args.build:
        build_probe(name)
    else:
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())],
            cwd=WORKSPACE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, timeout=180)
        (OUTPUT / (name + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
        assert 'LOOKUP narrow catalog lookup PASS' in result.stdout
        assert 'native-collision-assets diagnosis PASS' in result.stdout
