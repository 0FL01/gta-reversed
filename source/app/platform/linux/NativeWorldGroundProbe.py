#!/usr/bin/env python3
"""Independent extracted-source + full immutable IPL/COL world-authority gate.

Run: docker exec mad-sa-graphics-build python3
 /workspace/gta-reversed/source/app/platform/linux/NativeWorldGroundProbe.py
Generated objects/logs only under artifacts/graphics. Assets stay read-only.
"""
import hashlib
import pathlib
import shlex
import subprocess


def function(text, signature):
    start = text.index(signature)
    brace = text.index('{', start)
    end, depth = brace + 1, 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]


def main():
    root = pathlib.Path('/workspace')
    source = root / 'gta-reversed/source'
    here = source / 'app/platform/linux'
    out = root / 'artifacts/graphics'
    assert out.is_dir()
    bodies, evidence = [], []
    for path, signature in (
        ('game_sa/Entity/Building.cpp', 'CBuilding::CBuilding()'),
        ('game_sa/Entity/Entity.cpp', 'CRect CEntity::GetBoundRect() const'),
        ('game_sa/Entity/Entity.cpp', 'void CEntity::SetupBigBuilding()'),
        ('game_sa/Entity/Entity.cpp', 'void CEntity::Add(const CRect& rect)'),
        ('game_sa/Core/Matrix.cpp', 'void CMatrix::SetRotate(const CQuaternion& quat)'),
        ('game_sa/Core/Matrix.cpp', 'void CMatrix::SetRotateZOnly(float angle)'),
    ):
        text = (source / path).read_text()
        body = function(text, signature)
        bodies.append(body)
        evidence.append(f'{path}:{text[:text.index(signature)].count(chr(10))+1} sha256={hashlib.sha256(body.encode()).hexdigest()}')
    # Independent source-policy premises. The compiled fixtures additionally
    # execute extracted source constructor, BigBuilding, matrix and bound code.
    policies = {
        'game_sa/Entity/Entity.cpp': ['m_nFlags = 0;', 'case ENTITY_TYPE_BUILDING: ProcessAddItem(s.Buildings);', 'if (m_bIsBIGBuilding)', 'usedRect.right = 2999.0F;'],
        'game_sa/FileLoader.cpp': ['if (mi->m_nObjectInfoIndex == -1)', 'newEntity = new CDummyObject();', 'if (cm)', 'newEntity->SetUsesCollision(false);', 'building->GetNumLodChildren() || TheCamera.m_fLODDistMultiplier', 'lodMI->SetColModel(cm);', 'CWorld::Add(ipl);'],
        'game_sa/Models/BaseModelInfo.cpp': ['m_pColModel = nullptr;'],
        'game_sa/Models/AtomicModelInfo.cpp': ['CBaseModelInfo::Init();'],
        'game_sa/Models/ClumpModelInfo.cpp': ['CBaseModelInfo::Init();'],
        'game_sa/World.cpp': ['entity->IsScanCodeCurrent() || !entity->GetUsesCollision() || entity == pIgnoreEntity', '*entity->GetColModel()'],
        'game_sa/Core/PtrList.h': ['Add item to the head (front) of the list'],
        'game_sa/World.h': ['std::floor(GetSectorfX(x))', 'return x / static_cast<float>(MAX_WORLD_UNITS / MAX_SECTORS_X);'],
    }
    for path, snippets in policies.items():
        text = (source / path).read_text()
        for snippet in snippets:
            assert snippet in text, (path, snippet)
        evidence.append(f'{path} sha256={hashlib.sha256(text.encode()).hexdigest()}')
    loader = (source / 'game_sa/FileLoader.cpp').read_text()
    assert 'ms_colModelBBox' in function(loader, 'int32 CFileLoader::LoadClumpObject(')
    for signature in ('int32 CFileLoader::LoadObject(', 'int32 CFileLoader::LoadAnimatedClumpObject('):
        assert 'SetColModel' not in function(loader, signature)
    ipl = function((source / 'game_sa/IplStore.cpp').read_text(), 'bool CIplStore::LoadIpl(')
    assert 'obj->Add();' in ipl and 'SetupBigBuilding' not in ipl
    probe = (here / 'NativeWorldGroundProbe.cpp').read_text()
    assert probe.count('// SOURCE_WORLD_ORACLE_INSERT') == 1
    probe = probe.replace('// SOURCE_WORLD_ORACLE_INSERT', '\n'.join(bodies))
    world = (source / 'game_sa/World.h').read_text()
    sector_bodies = []
    for signature in ('static int32 GetSectorX(float x)', 'static int32 GetSectorY(float y)',
                      'static bool IterateSectors(int32 minX', 'static bool IterateSectorsOverlappedByRect(CRect rect'):
        body = function(world, signature)
        if 'Iterate' in signature:
            body = 'template<std::predicate<int32, int32> Fn>\n' + body
        sector_bodies.append(body)
    assert probe.count('// SOURCE_SECTOR_ORACLE_INSERT') == 1
    probe = probe.replace('// SOURCE_SECTOR_ORACLE_INSERT', '\n'.join(sector_bodies))
    commands = subprocess.check_output(['ninja', '-C', str(root / 'build'), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    args = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, i = [], 0
    while i < len(args):
        if args[i] in ('-MT', '-MF', '-o', '-c'):
            i += 2
        elif args[i] == '-MD':
            i += 1
        else:
            flags.append(args[i]); i += 1
    flags += ['-I' + str(here), '-UNDEBUG', '-Wall', '-Wextra', '-ffp-contract=off', '-fno-fast-math', '-ffunction-sections', '-fdata-sections']
    stem = out / 'NativeWorldGroundProbe'
    objects = []
    with stem.with_suffix('.build.log').open('w') as log:
        for unit in ('NativeWorldGround', 'NativeSourceGround', 'NativeWorldEntityInfo', 'NativeCollisionAssets', 'StreamPager', 'TexSample'):
            obj = out / ('worldground-' + unit + '.o')
            subprocess.run(flags + (['-Werror'] if unit == 'NativeWorldGround' else []) +
                           ['-c', str(here / (unit + '.cpp')), '-o', str(obj)], cwd=root / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        obj = stem.with_suffix('.o')
        subprocess.run(flags + ['-Werror', '-x', 'c++', '-', '-c', '-o', str(obj)], input=probe, text=True, cwd=root / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
        subprocess.run([flags[0], '-Wl,--gc-sections', str(obj), *objects, 'vendor/librw/src/librw.a', '-lpthread', '-lm', '-o', str(stem)], cwd=root / 'build', stdout=log, stderr=subprocess.STDOUT, check=True)
    run = subprocess.run([str(stem), '/game'], capture_output=True, text=True)
    stem.with_suffix('.log').write_text('\n'.join(evidence) + '\n' + run.stdout + run.stderr)
    print(run.stdout, end='')
    print(run.stderr, end='')
    run.check_returncode()


if __name__ == '__main__':
    main()
