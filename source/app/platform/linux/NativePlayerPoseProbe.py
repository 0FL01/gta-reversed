#!/usr/bin/python3
"""Isolated startup CJ pose/runtime and legacy regression probes; existing ninja flags."""
import pathlib
import shlex
import subprocess
import sys

workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source/app/platform/linux'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
template = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
probe = sys.argv[1] if len(sys.argv) > 1 else 'NativePlayerPoseProbe'
assert probe in ('NativePlayerPoseProbe', 'RealtimeGameplayProbe', 'RealtimeGameplayTerrainProbe', 'RealtimeGameplayDriveProbe', 'RealtimeGameplayPoseProbe')
names = ('RealtimeGameplay', 'NativePlayerAssets', probe) if probe == 'NativePlayerPoseProbe' else ('RealtimeGameplay', 'IfpAnim', 'NativePlayerAssets', probe)
objects = []
with (output / (probe + '-build.log')).open('w') as log:
    for name in names:
        command = []
        i = 0
        while i < len(template):
            arg = template[i]
            if arg in ('-MT', '-MF', '-o', '-c'):
                i += 2
            elif arg in ('-MD', '-DNDEBUG'):
                i += 1
            else:
                command.append(arg)
                i += 1
        obj = output / ('CJ-' + name + '.o')
        command += ['-g', '-Wall', '-Wextra', '-DREALTIME_GAMEPLAY_POSE_AUDIT', '-c', str(source / (name + '.cpp')), '-o', str(obj)]
        subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
        objects.append(str(obj))
    link = shlex.split(next(c for c in commands if ' -o mad-sa-linux ' in c).split('&&')[1])
    link = [a for a in link if not any(a.endswith('/' + n + '.cpp.o') for n in ('MainLinux', 'IfpAnim', *names))]
    link[link.index('-o') + 1] = str(output / probe)
    link[1:1] = objects
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
with (output / (probe + '.log')).open('w') as log:
    result = subprocess.run([str(output / probe), '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode == 0 and probe == 'NativePlayerPoseProbe':
        result = subprocess.run([str(output / probe), '/game', '--startup'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print((output / (probe + '.log')).read_text(), end='')
result.check_returncode()
