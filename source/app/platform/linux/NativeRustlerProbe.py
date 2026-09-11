#!/usr/bin/env python3
"""Build/run independent actual-asset Rustler S0 + production GpuScene gates."""
import argparse
import math
import pathlib
import subprocess
import sys
sys.dont_write_bytecode = True
import NativeGeneratedVehicleAssetsProbe as base
from NativeRustlerSourceProbe import source_pose, verify_retail

NAME = 'NativeRustlerProbe'


def game_state(game):
    # Metadata only: normal runtime verification must not read even EXE bytes.
    result = {}
    for path in game.rglob('*'):
        st = path.stat()
        result[str(path.relative_to(game))] = (st.st_mode, st.st_size, st.st_mtime_ns, st.st_ctime_ns)
    return result


def compare_poses(poses, expected):
    assert poses.keys() == expected.keys(), 'all eight unique source targets required'
    for frame, matrix in expected.items():
        assert len(poses[frame]) == len(matrix) == 12
        assert all(math.isfinite(a) and abs(a - b) < 1e-6 for a, b in zip(poses[frame], matrix)), frame


def run(game):
    expected, height = source_pose(game)
    trace = base.OUTPUT / (NAME + '-open.trace')
    result = subprocess.run(['strace', '-f', '-e', 'trace=open,openat', '-o', str(trace),
                             str(base.OUTPUT / NAME), str(game.resolve()), str(base.OUTPUT)],
                            cwd=base.WORKSPACE, text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, timeout=240)
    (base.OUTPUT / (NAME + '.log')).write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()
    assert '.exe' not in trace.read_text().lower()
    poses = {}
    for line in result.stdout.splitlines():
        if line.startswith('pose '):
            _, frame, *matrix = line.split()
            assert int(frame) not in poses, 'duplicate emitted pose'
            poses[int(frame)] = list(map(float, matrix))
    compare_poses(poses, expected)
    assert 'rustler-assets-ok' in result.stdout
    print(f'rustler-numericproof-ok matrices=8 scalars=96 tolerance=1e-6 frontHeight={height:.10f} '
          'raw-DFF+source-formulas=1 prior-proof-required=0 no-exe-access')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    mode.add_argument('--verify-retail', action='store_true')
    parser.add_argument('--game-dir', required=True, type=pathlib.Path)
    parser.add_argument('--output-dir', type=pathlib.Path, default=base.OUTPUT)
    args = parser.parse_args()
    if not __debug__:
        raise RuntimeError('probe requires Python assertions enabled')
    base.OUTPUT = args.output_dir.resolve()
    if args.build:
        base.build(NAME, gpu=True)
    else:
        base.OUTPUT.mkdir(parents=True, exist_ok=True)
        before = game_state(args.game_dir)
        try:
            if args.verify_retail:
                verify_retail(args.game_dir)
            else:
                run(args.game_dir)
        finally:
            assert game_state(args.game_dir) == before, 'game files changed'
        print(f'game-readonly-ok entries={len(before)} size/mode/mtime/ctime unchanged')
