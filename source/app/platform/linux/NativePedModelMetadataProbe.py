"""Independent owned-data ped namespace oracle and canonical DLL-table differential.

Run inside mad-sa-graphics-build, /workspace and /game:ro. No game bytes are
copied. Numeric metadata readiness never certifies streaming, audio or census.
"""
import argparse
import collections
import pathlib
import re
import struct
import subprocess
import sys
import zlib

sys.dont_write_bytecode = True
from NativeIplFlagsProbe import build_flags
from NativeWorldEntityInfoProbe import rows

ROOT = pathlib.Path('/workspace')
SOURCE = ROOT / 'gta-reversed/source'
OUT = ROOT / 'artifacts/graphics'
BASE = '5064fef3'


def baseline(relative):
    return subprocess.check_output(['git', '-c', 'safe.directory=/workspace/gta-reversed',
        '-C', str(ROOT / 'gta-reversed'), 'show', BASE + ':source/' + relative], text=True)


def canonical(log):
    original = baseline('game_sa/Animation/AnimAssocDefinitions.cpp')
    before = re.findall(r'\{\s*"([^"]+)",\s*"([^"]+)",\s*(\w+),\s*awc\((\w+)\),\s*(\w+)\s*\}', original)
    after = re.findall(r'^GTA_ANIM_ASSOC_GROUP\("([^"]+)", "([^"]+)", (\w+), (\w+), (\w+)\)$',
        (SOURCE / 'game_sa/Animation/AnimAssocGroups.inc').read_text(), re.M)
    assert len(before) == 118 and before == after, 'canonical animation extraction changed a source field/order'
    old_types = baseline('game_sa/PedType.cpp').split('ePedType CPedType::FindPedType', 1)[1]
    before_types = re.findall(r'\{\s*"([^"]+)",\s*(PED_TYPE_\w+)\s*\}', old_types)
    after_types = re.findall(r'^GTA_PED_TYPE_NAME\("([^"]+)", (PED_TYPE_\w+)\)$',
        (SOURCE / 'game_sa/PedTypeNames.inc').read_text(), re.M)
    assert len(before_types) == 32 and before_types == after_types, 'canonical ped type extraction changed'
    # Compile the complete original/new association initializer against the
    # same synthetic arrays. This is a common-C++ extraction check, not a claim
    # that the address-backed Win32 DLL has been built on Linux.
    code = '#include <array>\n#include <cstdint>\n#include <cstdlib>\n#include <string_view>\n'
    code += 'using uint32 = std::uint32_t;\n#define NOTSA_UNREACHABLE() std::abort()\n'
    code += '#include "game_sa/Enums/ePedType.h"\n'
    code += ''.join(f'static_assert({symbol} == {index});\n' for index, (_, symbol) in enumerate(after_types))
    code += 'enum {MODEL_MALE01=7, MODEL_INVALID=-1, NUM_ANIM_ASSOC_GROUPS=118};\n'
    code += 'struct AnimAssocDefinition {std::string_view Group, Block; int Model; std::size_t Count; const void* Names; const void* Descriptions;};\n'
    for index, name in enumerate(sorted({row[3] for row in before})):
        code += f'int {name}[{index + 1}]{{}};\n'
    for name in sorted({row[4] for row in before}):
        code += f'int {name}[1]{{}};\n'
    modified = (SOURCE / 'game_sa/Animation/AnimAssocDefinitions.cpp').read_text()
    for name, body in (('before', original), ('after', modified)):
        body = '\n'.join(line for line in body.splitlines() if not line.startswith('#include') or 'AnimAssocGroups.inc' in line)
        code += f'namespace {name} {{struct CAnimManager {{static std::array<AnimAssocDefinition,118> ms_aAnimAssocDefinitionsX;}};\n{body}\n}}\n'
    code += '''int main() {
    for (std::size_t i=0; i<118; ++i) {
        const auto& a=before::CAnimManager::ms_aAnimAssocDefinitionsX[i];
        const auto& b=after::CAnimManager::ms_aAnimAssocDefinitionsX[i];
        if (a.Group!=b.Group || a.Block!=b.Block || a.Model!=b.Model || a.Count!=b.Count ||
            a.Names!=b.Names || a.Descriptions!=b.Descriptions) return 1;
    }
}
'''
    binary = OUT / 'NativePedModelMetadata-canonical'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-Wno-switch',
        '-I' + str(SOURCE), '-I' + str(SOURCE / 'game_sa/Animation'), '-x', 'c++', '-', '-o', str(binary)],
        input=code, text=True, check=True, stdout=log, stderr=subprocess.STDOUT)
    subprocess.run([str(binary)], check=True, stdout=log, stderr=subprocess.STDOUT)
    return before, before_types


def reference(animations, types):
    models = {}
    for dat in ('data/default.dat', 'data/gta.dat'):
        for _, directive in rows(dat):
            if directive[0] != 'IDE': continue
            section = None
            for line, tokens in rows(directive[1]):
                if len(tokens) == 1:
                    section = None if tokens[0] == 'end' else tokens[0]
                elif section in ('objs', 'tobj', 'anim', 'cars', 'peds', 'weap', 'hier'):
                    models[int(tokens[0])] = (section, tokens, directive[1], line)
    stats = list(rows('data/pedstats.dat'))
    assert len(stats) == 43 and all(len(row) == 11 for _, row in stats)
    names = [row[0] for _, row in stats]
    groups = [entry[0] for entry in animations]
    in_group = False
    for _, tokens in rows('data/animgrp.dat'):
        if not in_group:
            assert len(tokens) >= 4
            groups.append(tokens[0])
            in_group = True
        elif tokens[0] == 'end': in_group = False
    assert not in_group
    type_names = [entry[0] for entry in types]
    peds = {}
    for ident, (section, tokens, ide, line) in models.items():
        if section != 'peds': continue
        assert len(tokens) == 14
        race = next(({'B': 1, 'W': 2, 'O': 3, 'I': 3, 'H': 4}[c]
            for c in tokens[1][:2].upper() if c in 'BWOIH'), 0)
        stat = names.index(tokens[4]) if tokens[4] in names else 16
        anim = next(i for i, name in enumerate(groups) if name.lower() == tokens[5].lower())
        peds[ident] = [str(ident), tokens[1], tokens[2], str(type_names.index(tokens[3])), str(stat), str(anim),
            str(race), str(int(tokens[4] not in names)), str(int(tokens[6], 16) & 65535), str(int(tokens[7], 16) & 65535),
            tokens[8], tokens[9], tokens[10], *tokens[11:14], ide, str(line)]
    keys = collections.defaultdict(list)
    for ident, (_, tokens, _, _) in models.items():
        keys[zlib.crc32(tokens[1].upper().encode('ascii')) ^ 0xffffffff].append(ident)
    ped_groups = []
    for line, tokens in rows('data/pedgrp.dat'):
        accepted = []
        for name in tokens:
            if name.startswith('#') or len(accepted) == 21: break
            matches = keys.get(zlib.crc32(name.upper().encode('ascii')) ^ 0xffffffff, [])
            assert len(matches) <= 1, (line, name, matches)
            if matches:
                assert matches[0] in peds and matches[0] != 0
                accepted.append(matches[0])
        if accepted:
            ped_groups.append([str(len(ped_groups)), str(len(accepted)), str(line),
                *map(str, accepted + [2000] * (21 - len(accepted)))])
    assert len(peds) == 276 and len(ped_groups) == 57
    return peds, stats, groups, ped_groups


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sanitized', action='store_true')
    args = parser.parse_args()
    assert OUT.is_dir()
    stem = 'NativePedModelMetadataProbe' + ('-sanitized' if args.sanitized else '')
    binary = ROOT / 'build/godot-native/sa_core_ped_metadata_probe'
    with (OUT / (stem + '.build.log')).open('w') as log:
        animations, types = canonical(log)
        if args.sanitized:
            flags = build_flags() + ['-UNDEBUG', '-O1', '-g', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
                '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                '-ffunction-sections', '-fdata-sections']
            objects = []
            for name in ('NativePedModelMetadata', 'NativePedModelMetadataProbe', 'NativeWorldEntityInfo',
                         'NativeCivilianOccupation', 'NativePedModelPolicies', 'NativePedStreaming', 'NativeSourceRng'):
                obj = OUT / (stem + '-' + name + '.o')
                subprocess.run(flags + ['-c', str(SOURCE / 'app/platform/linux' / (name + '.cpp')), '-o', str(obj)],
                    cwd=ROOT / 'build', check=True, stdout=log, stderr=subprocess.STDOUT)
                objects.append(str(obj))
            obj = OUT / (stem + '-file.o')
            subprocess.run(flags + ['-c', str(ROOT / 'godot/native/os_file_posix.cpp'), '-o', str(obj)],
                cwd=ROOT / 'build', check=True, stdout=log, stderr=subprocess.STDOUT)
            binary = OUT / stem
            subprocess.run([flags[0], '-fsanitize=address,undefined', '-Wl,--gc-sections', *objects, str(obj),
                '-lpthread', '-o', str(binary)], cwd=ROOT / 'build', check=True, stdout=log, stderr=subprocess.STDOUT)
            namespace_obj = OUT / (stem + '-namespace.o')
            subprocess.run(flags + ['-c', str(SOURCE / 'app/platform/linux/NativePedNamespaceProbe.cpp'), '-o', str(namespace_obj)],
                cwd=ROOT / 'build', check=True, stdout=log, stderr=subprocess.STDOUT)
            namespace_binary = OUT / (stem + '-namespace')
            dependencies = [name for name in objects if not name.endswith('-NativePedModelMetadataProbe.o')]
            subprocess.run([flags[0], '-fsanitize=address,undefined', '-Wl,--gc-sections', *dependencies, str(namespace_obj), str(obj),
                '-lpthread', '-o', str(namespace_binary)], cwd=ROOT / 'build', check=True, stdout=log, stderr=subprocess.STDOUT)
            result = subprocess.run([str(namespace_binary), '/game'], capture_output=True, text=True)
            (OUT / (stem + '.namespace.log')).write_text(result.stdout + result.stderr)
            result.check_returncode()
            print(result.stdout.strip())
    result = subprocess.run([str(binary), '/game', '--rows'], capture_output=True, text=True)
    (OUT / (stem + '.log')).write_text(result.stdout + result.stderr)
    result.check_returncode()
    peds, stats, groups, ped_groups = reference(animations, types)
    observed = collections.defaultdict(dict)
    for line in result.stdout.splitlines():
        row = line.split('\t')
        if row[0] in ('PED', 'STAT', 'ANIM', 'GROUP'):
            ident = int(row[1])
            assert ident not in observed[row[0]], ('duplicate row', row)
            observed[row[0]][ident] = row[1:]
        else: print(line)
    assert observed['PED'] == peds, 'resolved model identities differ from the independent ordered IDE/data oracle'
    bits = lambda value: str(struct.unpack('<I', struct.pack('<f', float(value)))[0])
    for i, (line, stat) in enumerate(stats):
        expected = [str(i), stat[0], bits(stat[1]), bits(stat[2]),
            *[str(int(n) & 255) for n in stat[3:7]], bits(stat[7]), bits(stat[8]), str(int(stat[9]) & 65535),
            str((int(stat[10]) + 128) % 256 - 128), str(line)]
        assert observed['STAT'][i] == expected, ('stat', i, observed['STAT'][i], expected)
    assert len(observed['STAT']) == 43
    assert observed['ANIM'] == {i: [str(i), name] for i, name in enumerate(groups)}
    assert observed['GROUP'] == dict(enumerate(ped_groups))
    print('ped-metadata-source-oracle-ok models=276 stats=43 animations=' + str(len(groups)),
          'groups=57 canonical=118,32 streaming=explicit-observation census=incomplete')


if __name__ == '__main__':
    main()
