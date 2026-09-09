#!/usr/bin/python3
"""Build this module's headless probe without touching shared CMake/build outputs.

Inside mad-sa-graphics-build:
 python3 /workspace/gta-reversed/source/app/platform/linux/RealtimeGameplayProbe.py
Uses existing ninja compile/link commands and writes only artifacts/graphics.
"""
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
compile_command = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
objects = []
probe = 'RealtimeGameplayTerrainProbe' if '--terrain' in sys.argv else 'RealtimeGameplayPoseProbe' if '--pose' in sys.argv else 'RealtimeGameplayProbe'
build_log = (output / (probe + '-build.log')).open('w')
for name in ('RealtimeGameplay', 'IfpAnim', probe):
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
    obj = output / ('RealtimeGameplay-' + name + '.o')
    command += ['-g', '-Wall', '-Wextra', '-DREALTIME_GAMEPLAY_POSE_AUDIT', '-c', str(source / (name + '.cpp')), '-o', str(obj)]
    compilation = subprocess.run(command, cwd=build, stdout=build_log, stderr=subprocess.STDOUT)
    if compilation.returncode:
        build_log.flush()
        print((output / (probe + '-build.log')).read_text())
        compilation.check_returncode()
    objects.append(str(obj))
link = next(c for c in commands if ' -o mad-sa-linux ' in c)
link = shlex.split(link.split('&&')[1])
link = [arg for arg in link if not any(arg.endswith('/' + name + '.cpp.o') for name in ('MainLinux', 'RealtimeGameplay', 'IfpAnim'))]
link[link.index('-o') + 1] = str(output / probe)
link[1:1] = objects
subprocess.run(link, cwd=build, check=True)
build_log.close()
with (output / (probe + '.log')).open('w') as log:
    result = subprocess.run([str(output / probe), '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print((output / (probe + '.log')).read_text(), end='')
result.check_returncode()
