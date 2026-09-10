#!/usr/bin/python3
"""Build in mad-sa-graphics-build with --build; run on host Wayland with --run.

Includes the actual Realtime.cpp with a test-only pre-swap readback wrapper;
all remaining objects/libraries (including MainLinux and HUD) are production.
No desktop capture, game asset dump, runtime code edit, or CMake edit.
"""
import argparse
import hashlib
import os
import pathlib
import re
import shlex
import shutil
import struct
import subprocess
import zlib

parser = argparse.ArgumentParser(description=__doc__)
mode = parser.add_mutually_exclusive_group(required=True)
mode.add_argument('--build', action='store_true')
mode.add_argument('--run', action='store_true')
parser.add_argument('--seconds', type=int, choices=(16, 28), default=16)
scene = parser.add_mutually_exclusive_group()
scene.add_argument('--player-cj', action='store_true')
scene.add_argument('--water', action='store_true', help='static shoreline free-camera, actual unpaused water clock')
args = parser.parse_args()
source = pathlib.Path(__file__).resolve().parent
workspace = source.parents[4]
build = workspace / 'build'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
binary = output / 'RealtimeHudCapture'

if args.build:
    subprocess.run(['ninja', '-C', str(build), 'mad-sa-linux'], check=True)
    commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    command = []
    i = 0
    while i < len(template):
        arg = template[i]
        if arg in ('-MT', '-MF', '-o', '-c'):
            i += 2
        elif arg == '-MD':
            i += 1
        else:
            command.append(arg)
            i += 1
    obj = output / 'RealtimeHudCapture.o'
    command += ['-Wall', '-Wextra', '-c', str(source / 'RealtimeHudCapture.cpp'), '-o', str(obj)]
    link = shlex.split(next(c for c in commands if ' -o mad-sa-linux ' in c).split('&&')[1])
    replaced = [i for i, arg in enumerate(link) if arg.endswith('/Realtime.cpp.o')]
    assert len(replaced) == 1
    link[replaced[0]] = str(obj)
    link[link.index('-o') + 1] = str(binary)
    with (output / 'RealtimeHudCapture-build.log').open('w') as log:
        for invocation in (command, link):
            log.write(shlex.join(invocation) + '\n')
            log.flush()
            subprocess.run(invocation, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    print('integrated capture built:', binary)
else:
    assert os.environ.get('WAYLAND_DISPLAY') and os.environ.get('XDG_RUNTIME_DIR'), 'Host Wayland session required'
    assert shutil.which('mangohud'), 'Host MangoHud required'
    game = workspace / 'Grand-Theft-Auto-San-Andreas'
    assert game.is_dir() and binary.is_file()
    env = os.environ.copy()
    env['SDL_VIDEODRIVER'] = 'wayland'
    if pathlib.Path('/usr/share/X11/locale').is_dir():
        env['XLOCALEDIR'] = '/usr/share/X11/locale'
    # Exactly the native launcher configuration, including continuous logging.
    env['MANGOHUD_CONFIG'] = ('fps,frametime,gpu_name,gpu_stats,cpu_stats,autostart_log=1,'
        f'log_duration=0,log_interval=100,output_folder={output}')
    command = ['mangohud', str(binary), '--play', '--game-dir', str(game), '--seconds', str(args.seconds)]
    command += ['--freecam', '--cam', '820,-1880,6', '--freeze-time'] if args.water else ['--demo']
    if args.player_cj:
        command.append('--player-cj')
    path = output / 'RealtimeHudCapture.log'
    with path.open('w') as log:
        log.write(shlex.join(command) + '\nMANGOHUD_CONFIG=' + env['MANGOHUD_CONFIG'] + '\n')
        log.write('binary-sha256=' + hashlib.sha256(binary.read_bytes()).hexdigest() + '\n')
        log.flush()
        result = subprocess.run(command, cwd=workspace, env=env, stdout=log, stderr=subprocess.STDOUT, timeout=args.seconds + 180)
        log.write(f'capture-runtime-exit={result.returncode}\n')
    text = path.read_text()
    print(text, end='')

    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

    captures = re.findall(r'hud-capture PASS .*?path=(\S+)', text)
    for relative in captures:
        image = workspace / relative
        magic, dimensions, maximum, rgb = image.read_bytes().split(b'\n', 3)
        assert magic == b'P6' and maximum == b'255'
        width, height = map(int, dimensions.split())
        assert len(rgb) == width * height * 3
        rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
        image.with_suffix('.png').write_bytes(b'\x89PNG\r\n\x1a\n'
            + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))
    result.check_returncode()
    assert len(captures) >= 3, 'Expected initial plus 5-second and 12-second captures'
    assert 'play-ok ' in text and 'play-fail ' not in text and 'hud-capture FAIL' not in text
    assert 'driver=wayland EGL=yes' in text and 'play-hud radar=144-tiles' in text
    if args.player_cj:
        assert 'play-player model=player outfit=startup-fat200-muscle50-preview' in text
    states = re.findall(r'play-state .*', text)
    replay = bool(states and all(re.search(rf'\b{name}=[1-9]\d*\b', states[-1]) for name in ('jumps', 'landings', 'entries', 'exits')))
    if args.water:
        assert 'player=hidden-freecam' in text
        clocks = [int(value) for value in re.findall(r'water-capture clockMs=(\d+)', text)]
        assert len(clocks) >= 3 and clocks[0] == 0 and clocks[-1] >= 12000
        assert all(b > a for a, b in zip(clocks, clocks[1:]))
        flow = re.findall(r'water-flow-capture gameNs=(\d+) ticks=(\d+) polygon=(-?\d+) current=([^\n]+)', text)
        assert len(flow) == len(clocks)
        for ns, ticks, polygon, current in flow:
            assert int(ticks) == int(ns) * 30 // 1_000_000_000
            assert int(ticks) < 29 or int(polygon) >= 0
            assert current == '0.000000000,0.000000000', 'Owned water.dat has zero authored flows; do not invent a current'
        first = (workspace / captures[0]).read_bytes().split(b'\n', 3)[3]
        last = (workspace / captures[-1]).read_bytes().split(b'\n', 3)[3]
        changed = sum(first[i:i+3] != last[i:i+3] for i in range(0, len(first), 3))
        assert changed > 1000, 'Static-camera/frozen-hour water must visibly animate'
        print(f'integrated-water PASS clock={clocks[0]}..{clocks[-1]} changedPixels={changed}')
    else:
        assert replay, 'Integrated demo must finish jump/land/entry/drive/brake/exit'
    print(f'integrated-capture PASS captures={len(captures)} runtimeExit=0 replayComplete={int(replay)} overlayInReadback=0')
