#!/usr/bin/python3
"""Isolated HUD probe in mad-sa-graphics-build (/workspace, /game:ro).

Uses the native build's exact flags/libraries; only writes artifacts/graphics.
The probe includes RealtimeHud.cpp to audit its source-coordinate helpers.
"""
import pathlib
import shlex
import subprocess
import struct
import zlib
import sys
import hashlib

workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source/app/platform/linux'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
compile_command = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
objects = []
with (output / 'RealtimeHudProbe-build.log').open('w') as log:
    for name in ('MenuShot', 'RadarMap', 'RealtimeHudProbe'):
        command = []
        i = 0
        while i < len(compile_command):
            arg = compile_command[i]
            if arg in ('-MT', '-MF', '-o', '-c'):
                i += 2
            elif arg in ('-MD', '-DNDEBUG'):
                i += 1
            else:
                command.append(arg)
                i += 1
        obj = output / ('RealtimeHud-' + name + '.o')
        command += ['-g', '-Wall', '-Wextra', '-c', str(source / (name + '.cpp')), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            log.flush()
            print((output / 'RealtimeHudProbe-build.log').read_text())
            result.check_returncode()
        objects.append(str(obj))
original_link = shlex.split(next(c for c in commands if ' -o mad-sa-linux ' in c).split('&&')[1])
link = original_link.copy()
link = [arg for arg in link if not any(arg.endswith('/' + name + '.cpp.o') for name in ('MainLinux', 'MenuShot', 'RadarMap', 'RealtimeHud'))]
link[link.index('-o') + 1] = str(output / 'RealtimeHudProbe')
link[1:1] = objects
subprocess.run(link, cwd=build, check=True)
with (output / 'RealtimeHudProbe.log').open('w') as log:
    result = subprocess.run([str(output / 'RealtimeHudProbe'), '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print((output / 'RealtimeHudProbe.log').read_text(), end='')
result.check_returncode()

# Lossless viewable versions of actual GL captures; no desktop/asset dumps.
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))

for image in output.glob('realtime-hud-*.ppm'):
    magic, dimensions, maximum, rgb = image.read_bytes().split(b'\n', 3)
    assert magic == b'P6' and maximum == b'255'
    width, height = map(int, dimensions.split())
    rows = b''.join(b'\0' + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    image.with_suffix('.png').write_bytes(
        b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0))
        + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))

if '--legacy' in sys.argv:
    # Relink original MainLinux with only the two additive reader objects;
    # compare rendered fixture bytes against the existing native executable.
    legacy = [arg for arg in original_link if not any(arg.endswith('/' + name + '.cpp.o') for name in ('MenuShot', 'RadarMap'))]
    legacy[legacy.index('-o') + 1] = str(output / 'RealtimeHudLegacyProbe')
    legacy[1:1] = objects[:2]
    subprocess.run(legacy, cwd=build, check=True)
    with (output / 'RealtimeHudLegacyProbe.log').open('w') as log:
        subprocess.run([str(output / 'RealtimeHudLegacyProbe'), '--smoke'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT, check=True)
        for mode in ('menu', 'radar'):
            images = []
            for label, binary in (('before', build / 'mad-sa-linux'), ('after', output / 'RealtimeHudLegacyProbe')):
                image = output / ('hud-legacy-' + mode + '-' + label + '.tga')
                subprocess.run([str(binary), '--shot-' + mode, str(image), '--game-dir', '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT, check=True)
                images.append(image.read_bytes())
            assert images[0] == images[1], mode + ' legacy fixture changed'
            message = 'hud-legacy PASS ' + mode + ' identical sha256=' + hashlib.sha256(images[0]).hexdigest()
            print(message)
            log.write(message + '\n')
