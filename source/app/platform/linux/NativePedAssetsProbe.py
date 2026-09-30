#!/usr/bin/env python3
"""Independent owned DFF skin/frame oracle; no executable or runtime fallback."""
import argparse
import pathlib
import shlex
import struct
import subprocess
import sys

sys.dont_write_bytecode = True
ROOT = pathlib.Path('/workspace')
SOURCE = ROOT / 'gta-reversed/source/app/platform/linux'
OUTPUT = ROOT / 'artifacts/graphics'


def chunks(data):
    offset = 0
    while offset < len(data):
        if not any(data[offset:]):
            return
        assert len(data) - offset >= 12, 'RW header bounds'
        kind, size, version = struct.unpack_from('<III', data, offset)
        assert offset + 12 + size <= len(data), 'RW payload bounds'
        yield kind, data[offset + 12:offset + 12 + size], version
        offset += size + 12


def child(data, kind):
    matches = [payload for tag, payload, _ in chunks(data) if tag == kind]
    assert len(matches) == 1, (kind, len(matches))
    return matches[0]


def u32(data, offset=0):
    return struct.unpack_from('<I', data, offset)[0]


def words(data):
    assert len(data) % 4 == 0
    return list(struct.unpack('<' + 'I' * (len(data) // 4), data))


def archive(path):
    with path.open('rb') as stream:
        magic, count = struct.unpack('<4sI', stream.read(8))
        assert magic == b'VER2' and count <= 300000
        entries = {}
        for _ in range(count):
            offset, sectors, name = struct.unpack('<II24s', stream.read(32))
            key = name.split(b'\0')[0].decode('ascii').lower()
            assert key not in entries
            entries[key] = (offset * 2048, (sectors & 0x7fff) * 2048)
        return entries


def read_entry(path, entries, name):
    offset, size = entries[name.lower()]
    with path.open('rb') as stream:
        stream.seek(offset)
        data = stream.read(size)
    assert len(data) == size
    return data


def geometry(data):
    raw = child(data, 1)
    flags, triangles, vertices, morphs = struct.unpack_from('<4I', raw)
    assert morphs == 1 and not flags & 0x1000000
    offset = 16
    colors = [list(raw[offset + v * 4:offset + v * 4 + 4]) for v in range(vertices)] if flags & 8 else [[255] * 4] * vertices
    if flags & 8:
        offset += vertices * 4
    texsets = (flags >> 16) & 255
    if not texsets:
        texsets = 1 if flags & 4 else 2 if flags & 128 else 0
    uv = [words(raw[offset + v * 8:offset + v * 8 + 8]) for v in range(vertices)] if texsets else [[0, 0]] * vertices
    offset += texsets * vertices * 8
    tris = []
    for t in range(triangles):
        b, a, material, c = struct.unpack_from('<4H', raw, offset + t * 8)
        tris.append([a, b, c, material])
    offset += triangles * 8
    radius = u32(raw, offset + 12)
    has_positions, has_normals = struct.unpack_from('<2I', raw, offset + 16)
    assert has_positions and has_normals
    offset += 24
    positions = [words(raw[offset + v * 12:offset + v * 12 + 12]) for v in range(vertices)]
    offset += vertices * 12
    normals = [words(raw[offset + v * 12:offset + v * 12 + 12]) for v in range(vertices)]
    offset += vertices * 12
    assert offset == len(raw)
    skin = child(child(data, 3), 0x116)
    bones, used, influences, _ = skin[:4]
    assert used and 1 <= influences <= 4
    offset = 4 + used
    indices = [list(skin[offset + v * 4:offset + v * 4 + 4]) for v in range(vertices)]
    offset += vertices * 4
    weights = [words(skin[offset + v * 16:offset + v * 16 + 16]) for v in range(vertices)]
    offset += vertices * 16
    inverse = []
    for b in range(bones):
        values = words(skin[offset + b * 64:offset + b * 64 + 64])
        inverse.append(values[0:3] + values[4:7] + values[8:11] + values[12:15])
    materials = child(data, 8)
    material_header = child(materials, 1)
    material_count = u32(material_header)
    assert all(value == 0xffffffff for value in words(material_header[4:])), 'fixture has reused material slots'
    authored_materials = []
    for kind, material, _ in chunks(materials):
        if kind != 7:
            continue
        raw_material = child(material, 1)
        rgba = [u32(struct.pack('<f', value / 255.0)) for value in raw_material[4:8]]
        surface = words(raw_material[16:28])
        texture_name = None
        if u32(raw_material, 12):
            names = [payload for tag, payload, _ in chunks(child(material, 6)) if tag == 2]
            texture_name = names[0].split(b'\0')[0].decode('ascii').lower()
        authored_materials.append((rgba + surface, texture_name))
    assert len(authored_materials) == material_count
    return flags & 0xff00ffff, bones, positions, normals, uv, colors, indices, weights, inverse, tris, authored_materials, radius


def verify(log, game):
    rows = {}
    for line in log.splitlines():
        fields = line.split()
        if fields and fields[0] in ('PED_ASSET', 'GEOM', 'GEOM_SETUP', 'BONE', 'VERT', 'TRI', 'MAT', 'IMG'):
            rows.setdefault(fields[0], []).append(fields[1:])
    path = game / 'models/gta3.img'
    entries = archive(path)
    total_vertices = total_bones = total_triangles = 0
    assert len(rows['PED_ASSET']) == 3
    for id_text, model, txd, mesh_count, image_count in rows['PED_ASSET']:
        model_id = int(id_text)
        assert model_id in (7, 105, 280) and int(image_count) > 0
        clump = child(read_entry(path, entries, model + '.dff'), 0x10)
        frames = child(clump, 0xe)
        frame_data = child(frames, 1)
        frame_count = u32(frame_data)
        assert len(frame_data) == 4 + frame_count * 56
        extensions = [payload for kind, payload, _ in chunks(frames) if kind == 3]
        assert len(extensions) == frame_count
        frames_by_tag = {}
        nodes = None
        hierarchy_flags = None
        for f, extension in enumerate(extensions):
            for kind, payload, _ in chunks(extension):
                if kind != 0x11e:
                    continue
                version, tag, count = struct.unpack_from('<3i', payload)
                assert version == 0x100 and tag not in frames_by_tag
                frames_by_tag[tag] = words(frame_data[4 + f * 56:4 + f * 56 + 48])
                if count:
                    assert nodes is None
                    hierarchy_flags = u32(payload, 12)
                    nodes = [struct.unpack_from('<3i', payload, 20 + n * 12) for n in range(count)]
        assert nodes
        geometries = [payload for kind, payload, _ in chunks(child(clump, 0x1a)) if kind == 0xf]
        atomics = [payload for kind, payload, _ in chunks(clump) if kind == 0x14]
        assert len(atomics) == int(mesh_count)
        for g, atomic in enumerate(atomics):
            _, index, atomic_flags, _ = struct.unpack('<4i', child(atomic, 1))
            flags, bones, positions, normals, uv, colors, indices, weights, inverse, tris, materials, radius = geometry(geometries[index])
            header = next(row for row in rows['GEOM'] if list(map(int, row[:2])) == [model_id, g])
            assert list(map(int, header[2:])) == [flags, bones, len(positions), len(tris), len(materials), atomic_flags & 255]
            setup = next(row for row in rows['GEOM_SETUP'] if list(map(int, row[:2])) == [model_id, g])
            assert list(map(int, setup[2:])) == [radius, hierarchy_flags], ('setup', model_id, g, setup, radius, hierarchy_flags)
            for m, (values, texture_name) in enumerate(materials):
                row = next(row for row in rows['MAT'] if list(map(int, row[:3])) == [model_id, g, m])
                assert list(map(int, row[4:])) == values, ('material', model_id, g, m)
                image = int(row[3])
                if texture_name is None:
                    assert image == -1
                else:
                    image_row = next(row for row in rows['IMG'] if list(map(int, row[:2])) == [model_id, image])
                    assert image_row[2].lower() == texture_name
                    width, height, length = map(int, image_row[3:])
                    assert width > 0 and height > 0 and length == width * height * 4
            assert len(nodes) == bones
            parent, stack = -1, []
            for b, (tag, node_index, node_flags) in enumerate(nodes):
                assert node_index == b
                expected = [model_id, g, b, tag, parent, node_flags] + frames_by_tag[tag] + inverse[b]
                actual = next(row for row in rows['BONE'] if list(map(int, row[:3])) == [model_id, g, b])
                assert list(map(int, actual)) == expected, ('bone', model_id, g, b)
                if node_flags & 2:
                    stack.append(parent)
                parent = b
                if node_flags & 1:
                    parent = stack.pop() if stack else -1
            assert not stack
            vertex_rows = [list(map(int, row)) for row in rows['VERT'] if list(map(int, row[:2])) == [model_id, g]]
            assert len(vertex_rows) == len(positions)
            for v, actual in enumerate(vertex_rows):
                expected = [model_id, g, v] + positions[v] + normals[v] + uv[v] + colors[v] + indices[v] + weights[v]
                assert actual == expected, ('vertex', model_id, g, v)
            triangle_rows = [list(map(int, row)) for row in rows['TRI'] if list(map(int, row[:2])) == [model_id, g]]
            assert triangle_rows == [[model_id, g, t] + tri for t, tri in enumerate(tris)]
            total_vertices += len(positions)
            total_bones += bones
            total_triangles += len(tris)
    print(f'ped-assets-source-oracle-ok models=3 bones={total_bones} vertices={total_vertices} triangles={total_triangles} '
          'source=DFF skin=authored worker=sole loaded-state=unowned census=incomplete')


def verify_catalog(log, game):
    rows = [line.split() for line in log.splitlines() if line.startswith('PED_CATALOG ')]
    assert len(rows) == 275
    entries = archive(game / 'models/gta3.img')
    parsed = 0
    unavailable = []
    for row in rows:
        _, model_id, model, txd, ready, *diagnostic = row
        expected = model.lower() + '.dff' in entries and txd.lower() + '.txd' in entries
        assert bool(int(ready)) == expected, (model_id, model, txd, diagnostic)
        if expected:
            parsed += 1
        else:
            unavailable.append(int(model_id))
    assert parsed == 265 and unavailable == list(range(290, 300))
    print('ped-assets-catalog-source-oracle-ok declarations=275 parsed=265 special-unbound=10 '
          'modular-player=separate loaded-state=unowned census=incomplete')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--game-dir', type=pathlib.Path, default=pathlib.Path('/game'))
    parser.add_argument('--sanitized', action='store_true')
    args = parser.parse_args()
    executable = ROOT / 'build/godot-native/sa_ped_assets_probe'
    suffix = '-sanitized' if args.sanitized else ''
    if args.sanitized:
        build = ROOT / 'build/godot-native'
        commands = subprocess.check_output(['ninja', '-C', str(build), '-t', 'commands', 'sa_ped_assets_probe'], text=True).splitlines()
        objects = []
        with (OUTPUT / ('NativePedAssetsProbe' + suffix + '.build.log')).open('w') as log:
            for name in ('NativePedAssetsProbe', 'NativeScriptEntities', 'MenuShot', 'NativePedSkinSetup'):
                original = shlex.split(next(line for line in commands if '-c ' in line and '/' + name + '.cpp' in line))
                command, i = [], 0
                while i < len(original):
                    arg = original[i]
                    if arg in ('-MT', '-MF', '-o', '-c'):
                        i += 2
                    elif arg in ('-MD', '-DNDEBUG'):
                        i += 1
                    elif arg.startswith('-I') and '/vendor/librw' in arg:
                        command.extend(['-isystem', arg[2:]])
                        i += 1
                    else:
                        command.append(arg)
                        i += 1
                obj = OUTPUT / ('PedAssetSan-' + name + '.o')
                command += ['-UNDEBUG', '-O1', '-Werror', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-c', str(SOURCE / (name + '.cpp')), '-o', str(obj)]
                subprocess.run(command, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
                objects.append(str(obj))
            link_line = next(line for line in commands if ' -o sa_ped_assets_probe ' in line)
            link = shlex.split(next(part for part in link_line.split('&&') if ' -o sa_ped_assets_probe ' in part))
            link = [arg for arg in link if not any(arg.endswith('/' + name + '.cpp.o') for name in
                    ('NativePedAssetsProbe', 'NativeScriptEntities', 'MenuShot')) and arg != ':']
            executable = OUTPUT / 'NativePedAssetsProbe-sanitized'
            link[link.index('-o') + 1] = str(executable)
            link[1:1] = ['-fsanitize=address,undefined', *objects]
            subprocess.run(link, cwd=build, stdout=log, stderr=subprocess.STDOUT, check=True)
    path = OUTPUT / ('NativePedAssetsProbe' + suffix + '.log')
    with path.open('w') as log:
        result = subprocess.run([str(executable), str(args.game_dir), '--rows'], stdout=log, stderr=subprocess.STDOUT,
                                cwd=ROOT, timeout=120)
    result.check_returncode()
    text = path.read_text()
    assert 'native-ped-assets-ok ' in text
    print(next(line for line in text.splitlines() if line.startswith('native-ped-assets-ok ')))
    verify(text, args.game_dir)
    catalog_path = OUTPUT / ('NativePedAssetsProbe' + suffix + '.catalog.log')
    with catalog_path.open('w') as log:
        result = subprocess.run([str(executable), str(args.game_dir), '--catalog'], stdout=log, stderr=subprocess.STDOUT,
                                cwd=ROOT, timeout=180)
    result.check_returncode()
    catalog = catalog_path.read_text()
    assert 'native-ped-assets-catalog-ok ' in catalog
    print(next(line for line in catalog.splitlines() if line.startswith('native-ped-assets-catalog-ok ')))
    verify_catalog(catalog, args.game_dir)


if __name__ == '__main__':
    main()
