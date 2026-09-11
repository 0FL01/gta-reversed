#!/usr/bin/env python3
"""Owned-asset source-COL reconciliation probe against the production public API."""
import argparse
import math
import pathlib
import shlex
import struct
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeCarGeneratorResidencyProbe'


def source_proof(game_dir):
    """Independent read-only IMG/header/0x30 CFileCarGenerator census."""
    def resolve(relative):
        path = game_dir
        for component in pathlib.PurePosixPath(relative).parts:
            path = next(p for p in path.iterdir() if p.name.lower() == component.lower())
        return path

    sources = records = accepted = 0
    rejected = []
    for archive in ('models/gta3.img', 'models/gta_int.img'):
        with resolve(archive).open('rb') as image:
            magic, count = struct.unpack('<4sI', image.read(8))
            assert magic == b'VER2'
            directory = [struct.unpack('<IHH24s', image.read(32)) for _ in range(count)]
            for sector, streaming, archived, raw_name in directory:
                name = raw_name.split(b'\0', 1)[0].decode('ascii').lower()
                if not name.endswith('.ipl'):
                    continue
                sources += 1
                base = sector * 2048
                image.seek(base)
                header = image.read(0x4c)
                assert header[:4] == b'bnry'
                cars = struct.unpack_from('<I', header, 20)[0]
                offset = struct.unpack_from('<I', header, 60)[0]
                assert not cars or 0x4c <= offset <= (archived or streaming) * 2048 - cars * 0x30
                image.seek(base + offset)
                for row in range(cars):
                    values = struct.unpack('<4f8i', image.read(0x30))
                    assert all(math.isfinite(v) for v in values[:4])
                    records += 1
                    model = values[4]
                    if model == -1 or 400 <= model <= 630:
                        accepted += 1
                    else:
                        rejected.append((name, row + 1, model))
                        print(f'raw-source-reject {archive}:{name} row={row + 1} container={base} '
                              f'offset={offset + row * 0x30} bytes=48 model={model} '
                              f'xyz={values[:3]} angle={values[3]} colors/flags/alarm/lock/delays={values[5:]}')
    assert sources == 190 and records == 1045 and accepted == 1037 and len(rejected) == 8
    assert [(row, model) for name, row, model in rejected if name == 'vegass_stream0.ipl'] == [
        (1, 2), (3, 2), (4, 2), (5, 4), (7, 4)]
    print(f'raw-source-proof PASS binaryIpls={sources} authored={records} accepted={accepted} rejected={len(rejected)}', flush=True)


def build():
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(
        ['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
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
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in ['NativeVehiclePool', 'NativeCarGenerators', 'NativeCarGeneratorResidency',
                     'NativeCollisionAssets', 'StreamPager', 'TexSample', NAME]:
            obj = OUTPUT / ('residency-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections',
                               '-c', str(SOURCE / (unit + '.cpp')), '-o', str(obj)]
            log.write(shlex.join(command) + '\n')
            log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        command = [flags[0], '-Wl,--gc-sections', *objects,
                   str(directory / 'vendor/librw/src/librw.a'), '-o', str(OUTPUT / NAME)]
        log.write(shlex.join(command) + '\n')
        log.flush()
        subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
    print(OUTPUT / NAME)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.build:
        build()
    else:
        source_proof(args.game_dir.resolve())
        name = NAME
        result = subprocess.run([str(OUTPUT / name), str(args.game_dir.resolve())], text=True,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=180)
        (OUTPUT / (name + '.log')).write_text(result.stdout)
        print(result.stdout, end='')
        result.check_returncode()
