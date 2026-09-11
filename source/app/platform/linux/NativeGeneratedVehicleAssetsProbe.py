#!/usr/bin/env python3
"""Read-only source census and isolated CPU generated-vehicle asset probe."""
import argparse
import pathlib
import shlex
import struct
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeGeneratedVehicleAssetsProbe'


def build(name=NAME, gpu=False):
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, index = [], 0
    while index < len(template):
        if template[index] in ('-MT', '-MF', '-o', '-c'):
            index += 2
        elif template[index] == '-MD':
            index += 1
        else:
            flags.append(template[index])
            index += 1
    OUTPUT.mkdir(parents=True, exist_ok=True)
    objects = []
    with (OUTPUT / (name + '-build.log')).open('w') as log:
        for unit in ['NativeCarGenerators', 'NativeCollisionAssets', 'CarPose', 'TexSample',
                     'NativeGeneratedVehicleAssets', name] + (['NativeRustlerGpuProbe', 'RealtimeEnvironment', 'TimeCycle', 'WaterLevel'] if gpu else []):
            obj = OUTPUT / (name + '-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        subprocess.run([flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lpthread', '-lm',
                        *(['-lEGL', '-lGL', '-lz'] if gpu else []),
                        '-o', str(OUTPUT / name)], cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)


def run(game):
    trace = OUTPUT / (NAME + '-open.trace')
    command = ['strace', '-f', '-e', 'trace=open,openat', '-o', str(trace), str(OUTPUT / NAME), str(game.resolve())]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=240)
    (OUTPUT / (NAME + '.log')).write_text(result.stdout)
    print(result.stdout, end='')
    result.check_returncode()
    assert '.exe' not in trace.read_text().lower(), 'EXE access in process file-open trace'
    print('no-exe-access-ok strace open/openat')


def chunks(data):
    offset = 0
    while offset < len(data):
        if not any(data[offset:]):
            return
        kind, size, version = struct.unpack_from('<III', data, offset)
        assert offset + 12 + size <= len(data)
        yield kind, data[offset + 12:offset + 12 + size]
        offset += 12 + size


def inspect(game):
    with (game / 'models/gta3.img').open('rb') as archive:
        magic, count = struct.unpack('<4sI', archive.read(8))
        assert magic == b'VER2'
        entries = {}
        for _ in range(count):
            offset, size, name = struct.unpack('<II24s', archive.read(32))
            entries[name.split(b'\0')[0].decode().lower()] = (offset * 2048, (size & 0x7fff) * 2048)
        for model in ('rustler', 'landstal', 'pcj600', 'predator'):
            offset, size = entries[model + '.dff']
            archive.seek(offset)
            clump = next(data for kind, data in chunks(archive.read(size)) if kind == 16)
            names = []
            atomics = []
            for kind, data in chunks(clump):
                if kind == 3:
                    for plugin, value in chunks(data):
                        print(f'{model} clump-plugin={plugin:#x} bytes={len(value)} prefix={value[:32]!r}')
                if kind == 14:
                    frames = list(chunks(data))
                    count, = struct.unpack_from('<I', frames[0][1])
                    for i in range(count):
                        name = next((value.rstrip(b'\0').decode() for plugin, value in chunks(frames[i + 1][1])
                                     if plugin == 0x253f2fe), '')
                        parent, = struct.unpack_from('<i', frames[0][1], 4 + i * 56 + 48)
                        names.append(name)
                        print(f'{model} frame={i} parent={parent} name={name}')
                elif kind == 20:
                    atomic = next(value for plugin, value in chunks(data) if plugin == 1)
                    atomics.append(struct.unpack_from('<IIII', atomic))
            for frame, geometry, flags, _ in atomics:
                print(f'{model} atomic frame={frame}:{names[frame]} geometry={geometry} flags={flags}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--inspect', action='store_true')
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', required=True, type=pathlib.Path)
    args = parser.parse_args()
    if args.inspect:
        inspect(args.game_dir)
    elif args.build:
        build()
    else:
        run(args.game_dir)
