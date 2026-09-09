#!/usr/bin/python3
"""Run inside mad-sa-graphics-build after building mad-sa-linux.

docker exec mad-sa-graphics-build python3 /workspace/gta-reversed/source/app/platform/linux/RealtimeStreamingProbe.py
artifacts/graphics/realtime-streaming-probe Grand-Theft-Auto-San-Andreas [run|close-cpu|close-gpu|close-retire]

All outputs live in artifacts/graphics. Uses actual native build flags/objects;
does not modify the build, sources or game. Host execution uses surfaceless GL.
"""
import pathlib
import shlex
import subprocess

root = pathlib.Path('/workspace')
build = root / 'build'
out = root / 'artifacts/graphics'
commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
template = shlex.split(next(c for c in commands if '-c ' in c and '/Handling.cpp' in c))
flags = []
i = 0
while i < len(template):
    if template[i] in ('-MT', '-MF', '-o', '-c'):
        i += 2
    elif template[i] == '-MD':
        i += 1
    else:
        flags.append(template[i])
        i += 1
obj = out / 'realtime-streaming-probe.o'
with (out / 'realtime-streaming-build.log').open('w') as log:
    subprocess.run(flags + ['-UNDEBUG', '-ffunction-sections', '-fdata-sections', '-c',
        str(root / 'gta-reversed/source/app/platform/linux/RealtimeStreamingProbe.cpp'), '-o', str(obj)],
        cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    link = shlex.split(next(c for c in commands if ' -o mad-sa-linux ' in c).split('&&')[1])
    link = [a for a in link if not any(a.endswith('/' + n + '.cpp.o') for n in ('MainLinux', 'Realtime'))]
    link[link.index('-o') + 1] = str(out / 'realtime-streaming-probe')
    link[1:1] = ['-Wl,--gc-sections', str(obj)]
    subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
print(out / 'realtime-streaming-probe')
