#!/usr/bin/env python3
"""Execute extracted Physical/World dynamic-sector bodies against the owned replay.

Generated source, objects and evidence stay under artifacts/graphics. No game
data or original executable is needed. --sanitized instruments both owner TUs.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess


def function(text, signature):
    start = text.index(signature)
    cursor = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[cursor] == '{') - (text[cursor] == '}')
        cursor += 1
    return text[start:cursor]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitized', action='store_true')
    args = parser.parse_args()
    root = Path('/workspace')
    source = root / 'gta-reversed/source'
    here = source / 'app/platform/linux'
    out = root / 'artifacts/graphics'
    assert out.is_dir()
    evidence = []

    def extract(path, signature):
        text = (source / path).read_text()
        body = function(text, signature)
        evidence.append(f'{path}:{signature} sha256={hashlib.sha256(body.encode()).hexdigest()}')
        return body

    bodies = []
    for signature in ('void CPhysical::Add()', 'void CPhysical::RemoveAndAdd()'):
        body = extract('game_sa/Entity/Physical.cpp', signature)
        assert body.count('#if 0') == 1 and body.count('#endif') == 1
        body = body.replace('#if 0', '').replace('#endif', '')
        if signature == 'void CPhysical::Add()':
            for member in ('Vehicles', 'Peds', 'Objects'):
                assert body.count('repeatSector.' + member) == 1
                body = body.replace('repeatSector.' + member, 'repeatSector->' + member)
            evidence.append('oracle: fix disabled reference pointer-member spelling; no control-flow change')
        bodies.append(body)
        evidence.append('oracle: execute documented disabled-body reference; retail 0x555980/0x553530 confirms structure')
    bodies += [extract('game_sa/Entity/Physical.cpp', 'void CPhysical::Remove()'),
               extract('game_sa/Entity/Physical.cpp', 'CRect CPhysical::GetBoundRect() const')]
    scan = extract('game_sa/World.cpp', 'void CWorld::FindObjectsKindaCollidingSectorList(')
    bodies.append('template<typename PtrListType>\n' + scan)
    bodies.append(extract('game_sa/World.cpp', 'void CWorld::FindObjectsKindaColliding(const CVector&'))
    grid = []
    for signature in ('static int32 GetSectorX(float x)', 'static int32 GetSectorY(float y)',
                      'static bool IterateSectors(int32 minX',
                      'static bool IterateSectorsOverlappedByRect(CRect rect'):
        body = extract('game_sa/World.h', signature)
        if 'Iterate' in signature:
            body = 'template<std::predicate<int32, int32> Fn>\n' + body
        grid.append(body)
    assert 'Original code uses `&` instead of `%`' in (source / 'game_sa/World.h').read_text()
    assert 'std::exchange(head, node)' in (source / 'game_sa/Core/PtrListDoubleLink.h').read_text()
    fixture = (here / 'NativeDynamicSectorOracle.h').read_text()
    assert fixture.count('// SOURCE_DYNAMIC_GRID_INSERT') == 1
    assert fixture.count('// SOURCE_DYNAMIC_BODIES_INSERT') == 1
    fixture = fixture.replace('// SOURCE_DYNAMIC_GRID_INSERT', '\n'.join(grid))
    fixture = fixture.replace('// SOURCE_DYNAMIC_BODIES_INSERT', '\n'.join(bodies))
    (out / 'NativeDynamicSectorSourceOracle.inc').write_text(fixture)
    suffix = '-sanitized' if args.sanitized else ''
    stem = out / ('NativeDynamicSectorListsProbe' + suffix)
    flags = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
             '-fno-fast-math', '-ffp-contract=off', '-ffunction-sections', '-fdata-sections',
             '-I' + str(source), '-I' + str(here), '-I' + str(out)]
    if args.sanitized:
        flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    with stem.with_suffix('.build.log').open('w') as log:
        objects = []
        for unit in ('NativeDynamicSectorLists', 'NativeWorldGround', 'NativeDynamicSectorListsProbe'):
            obj = out / (unit + '-oracle' + suffix + '.o')
            command = flags + (['-DNATIVE_DYNAMIC_SECTOR_SOURCE_ORACLE'] if unit.endswith('Probe') else [])
            subprocess.run(command + ['-c', str(here / (unit + '.cpp')), '-o', str(obj)],
                           stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        linker = ['g++', '-Wl,--gc-sections']
        if args.sanitized:
            linker += ['-fsanitize=address,undefined']
        subprocess.run(linker + objects + ['-o', str(stem)],
                       stdout=log, stderr=subprocess.STDOUT, check=True)
    run = subprocess.run([str(stem)], capture_output=True, text=True)
    stem.with_suffix('.log').write_text('\n'.join(evidence) + '\n' + run.stdout + run.stderr)
    print(run.stdout, end='')
    print(run.stderr, end='')
    run.check_returncode()


if __name__ == '__main__':
    main()
