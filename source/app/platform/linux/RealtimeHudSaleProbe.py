#!/usr/bin/python3
"""Run inside mad-sa-graphics-build: python3 /workspace/gta-reversed/source/app/platform/linux/RealtimeHudSaleProbe.py.

Uses native build flags/libraries, writes only artifacts/graphics, reads /game:ro.
Actual offscreen EGL/GL readback; no desktop captures or asset copies.
"""
import pathlib
import shlex
import subprocess

workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source/app/platform/linux'
output = workspace / 'artifacts/graphics'
output.mkdir(parents=True, exist_ok=True)
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
template = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
names = ('MenuShot', 'RadarMap', 'NativeScriptEntities', 'RealtimeHudSaleProbe')
objects = []
logfile = output / 'RealtimeHudSaleProbe-build.log'
with logfile.open('w') as log:
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
        obj = output / ('RealtimeHudSale-' + name + '.o')
        command += ['-g', '-Wall', '-Wextra', '-c', str(source / (name + '.cpp')), '-o', str(obj)]
        result = subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT)
        if result.returncode:
            log.flush()
            print(logfile.read_text())
            result.check_returncode()
        objects.append(str(obj))
    link = shlex.split(next(c for c in commands if ' -o mad-sa-linux ' in c).split('&&')[1])
    replaced = ('MainLinux', 'RealtimeHud') + names
    link = [arg for arg in link if not any(arg.endswith('/' + name + '.cpp.o') for name in replaced)]
    link[link.index('-o') + 1] = str(output / 'RealtimeHudSaleProbe')
    link[1:1] = objects
    result = subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        log.flush()
        print(logfile.read_text())
        result.check_returncode()
with (output / 'RealtimeHudSaleProbe.log').open('w') as log:
    result = subprocess.run([str(output / 'RealtimeHudSaleProbe'), '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print((output / 'RealtimeHudSaleProbe.log').read_text(), end='')
result.check_returncode()
