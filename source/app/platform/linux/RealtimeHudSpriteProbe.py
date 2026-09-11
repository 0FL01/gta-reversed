#!/usr/bin/python3
"""All-source-sprite EGL probe; run inside mad-sa-graphics-build.

Reads /game:ro and writes only artifacts/graphics; no asset copies or exe use.
"""
import pathlib
import re
import shlex
import subprocess

workspace = pathlib.Path('/workspace')
build = workspace / 'build'
source = workspace / 'gta-reversed/source/app/platform/linux'
output = workspace / 'artifacts/graphics'
assert output.parent.is_dir()
output.mkdir(exist_ok=True)

# Independent source table order, including intentional nullptr entries and the
# enum's extra value 64 which must never index the 64-element texture table.
radar = (workspace / 'gta-reversed/source/game_sa/Radar.cpp').read_text()
table = radar.split('SpriteFileName CRadar::RadarBlipFileNames[] = {', 1)[1].split('};', 1)[0]
expected = re.findall(r'\{\s*(nullptr|"[^"]+")\s*,\s*nullptr\s*\}', table)
hud = (source / 'RealtimeHud.cpp').read_text()
actual = re.findall(r'nullptr|"[^"]+"', hud.split('kRadarNames{', 1)[1].split('};', 1)[0])
assert actual == expected and len(actual) == 64
print('sprite-table PASS exact original 64 entries, 62 named, NONE/WHITE null, 64 outside bounds', flush=True)

commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
template = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
names = ('MenuShot', 'RadarMap', 'NativeScriptEntities', 'RealtimeHudSpriteProbe')
objects = []
logfile = output / 'RealtimeHudSpriteProbe-build.log'
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
        obj = output / ('RealtimeHudSprite-' + name + '.o')
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
    link[link.index('-o') + 1] = str(output / 'RealtimeHudSpriteProbe')
    link[1:1] = objects
    result = subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode:
        log.flush()
        print(logfile.read_text())
        result.check_returncode()
with (output / 'RealtimeHudSpriteProbe.log').open('w') as log:
    result = subprocess.run([str(output / 'RealtimeHudSpriteProbe'), '/game'], cwd=workspace, stdout=log, stderr=subprocess.STDOUT)
print((output / 'RealtimeHudSpriteProbe.log').read_text(), end='')
result.check_returncode()
