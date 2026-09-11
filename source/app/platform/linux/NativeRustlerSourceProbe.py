#!/usr/bin/env python3
"""Independent S0 oracle: owned assets in memory; optional static retail proof.

No asset payload or frame/vertex table is embedded or exported. The normal
oracle never opens an executable. Retail VAs below identify the independently
recovered constructor/first PreRender formulas, not an observed execution.
"""
import math
import struct


def f32(value):
    return struct.unpack('<f', struct.pack('<f', value))[0]


def chunks(buf):
    offset = 0
    while offset < len(buf):
        kind, size, _ = struct.unpack_from('<III', buf, offset)
        assert offset + 12 + size <= len(buf)
        yield kind, buf[offset + 12:offset + 12 + size]
        offset += 12 + size


def metadata(game):
    with (game / 'models/gta3.img').open('rb') as archive:
        magic, count = struct.unpack('<4sI', archive.read(8))
        assert magic == b'VER2'
        directory = archive.read(count * 32)
        matches = []
        for offset in range(0, len(directory), 32):
            sector, size, name = struct.unpack_from('<II24s', directory, offset)
            if name.split(b'\0')[0] == b'rustler.dff':
                matches.append((sector, size & 0x7fff))
        assert len(matches) == 1
        sector, size = matches[0]
        archive.seek(sector * 2048)
        buf = archive.read(size * 2048)
    kind, size, _ = struct.unpack_from('<III', buf)
    assert kind == 16 and size + 12 <= len(buf)
    frames, atomics, geometries = [], [], []
    for kind, body in chunks(buf[12:12 + size]):
        if kind == 14:
            sub = list(chunks(body))
            assert sub[0][0] == 1
            count, = struct.unpack_from('<I', sub[0][1])
            assert len(sub) == count + 1
            for i in range(count):
                values = struct.unpack_from('<12fiI', sub[0][1], 4 + 56 * i)
                name = dict(chunks(sub[i + 1][1]))[0x253f2fe].split(b'\0')[0].decode()
                frames.append(dict(name=name, parent=values[12], matrix=list(values[:12])))
        elif kind == 20:
            atomics.append(struct.unpack('<4I', dict(chunks(body))[1]))
        elif kind == 26:
            for kind, geometry in chunks(body):
                if kind != 15:
                    continue
                parts = dict(chunks(geometry))
                fmt, triangles, _, _ = struct.unpack_from('<4I', parts[1])
                materials = [dict(chunks(value))[1] for kind, value in chunks(parts[8]) if kind == 7]
                geometries.append(dict(format=fmt, triangles=triangles, alpha=[m[7] for m in materials]))
    assert len(frames) == 27 and len(atomics) == len(geometries) == 12
    assert all(a[2] == 5 for a in atomics)
    assert frames[atomics[1][0]]['name'] == 'static_prop'
    assert frames[atomics[9][0]]['name'] == 'moving_prop'
    assert geometries[1]['alpha'] == [230] and geometries[9]['alpha'] == [255]
    assert geometries[1]['triangles'] == 84 and geometries[9]['triangles'] == 8
    assert geometries[1]['format'] & 0x40 and not geometries[9]['format'] & 0x40
    return frames, atomics, geometries


def source_pose(game, width_divisor=f32(0.7), left_yaw=f32(math.pi)):
    frames, atomics, geometries = metadata(game)
    by_name = {f['name']: i for i, f in enumerate(frames)}
    assert len(by_name) == len(frames)
    identity = [1, 0, 0, 0, 1, 0, 0, 0, 1]
    assert all(f['matrix'][:9] == identity for f in frames[:24])
    assert frames[by_name['wheel_rf_dummy']]['parent'] == by_name['gear_r']
    assert frames[by_name['wheel_lf_dummy']]['parent'] == by_name['gear_l']
    ide = [line.replace(',', ' ').split() for line in (game / 'data/vehicles.ide').read_text().splitlines()
           if line.replace(',', ' ').split()[:1] == ['476']]
    handling = [line.split() for line in (game / 'data/handling.cfg').read_text().splitlines()
                if line.split()[:1] == ['RUSTLER']]
    assert len(ide) == len(handling) == 1
    ide, handling = ide[0], handling[0]
    assert ide[1:5] == ['rustler', 'rustler', 'plane', 'RUSTLER']
    assert ide[-4] == '-1' and tuple(map(float, ide[-3:-1])) == (.6, .3)
    assert handling[31:33] == ['4008108', '400020']
    force, upper, lower = map(float, (handling[21], handling[24], handling[25]))
    assert (force, upper, lower) == (2, .5, -.2)
    front, rear = map(f32, map(float, ide[-3:-1]))
    force, upper, lower = map(f32, (force, upper, lower))

    def world_pos(i):
        pos = frames[i]['matrix'][9:12].copy()
        parent = frames[i]['parent']
        while parent >= 0:
            assert parent < i and frames[parent]['matrix'][:9] == identity
            pos = [f32(x + y) for x, y in zip(pos, frames[parent]['matrix'][9:12])]
            i, parent = parent, frames[parent]['parent']
        return pos

    # SetupSuspensionLines 0x6D44D0; constructor 0x6DEF80 uses FRONT
    # height for all four wheels before any ProcessControl/collision update.
    spring = f32(upper - lower)
    start = f32(world_pos(by_name['wheel_lf_dummy'])[2] + upper)
    height = f32(f32((1 - 1 / (force * 4)) * spring) - start + front * .5)
    poses = {}
    for name in ('wheel_lf_dummy', 'wheel_lb_dummy', 'wheel_rf_dummy', 'wheel_rb_dummy'):
        i = by_name[name]
        left, is_front = name[6] == 'l', name[7] == 'f'
        size = front if is_front else rear
        z = f32(f32(size * .5 - height) - world_pos(i)[2] + frames[i]['matrix'][11])
        # UpdateWheelMatrix 0x6D84F0: zero roll/steer, left yaw pi,
        # width normalization double at 0x8A2270, left yaw at 0x8A3610.
        yz = f32(1 if is_front else rear / front)
        x = f32(f32(front / width_divisor) * yz)
        yaw = left_yaw if left else 0
        si, co = f32(math.sin(yaw)), f32(math.cos(yaw))
        poses[i] = [f32(x * co), f32(x * si), 0, f32(-yz * si), f32(yz * co), 0,
                    0, 0, yz, *frames[i]['matrix'][9:11], z]
        assert abs(z - frames[i]['matrix'][11]) > .1
    # SetComponentRotation 0x715B80 resets basis and preserves translation.
    # S0 prop phase / landingGearStatus are zero: four zero rotations.
    for name in ('static_prop', 'moving_prop', 'gear_l', 'gear_r'):
        i = by_name[name]
        poses[i] = identity + frames[i]['matrix'][9:12]
    visible = [i for i in range(len(atomics)) if i not in (7, 9, 11)]
    triangles = sum(geometries[atomics[i][1]]['triangles'] for i in visible) + 4 * geometries[7]['triangles']
    assert len(poses) == 8 and len(visible) == 9 and triangles == 2470
    # Retain the original static proof's bounded numeric sanity assertions.
    assert abs(height - 2.0826525688) < 1e-6
    assert abs(poses[by_name['wheel_lf_dummy']][11] + .9958093) < 1e-6
    assert abs(poses[by_name['wheel_lb_dummy']][11] + 1.9326526) < 1e-6
    return poses, height


def verify_retail(game):
    # Only this explicit developer action imports PE/disassembly dependencies
    # or opens the owned retail executable. Nothing is written or executed.
    import hashlib
    import pefile
    from capstone import Cs, CS_ARCH_X86, CS_MODE_32
    raw = (game / 'gta-sa.exe').read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    assert len(raw) == 5971456
    assert digest == '15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb'
    pe = pefile.PE(data=raw)
    base = pe.OPTIONAL_HEADER.ImageBase
    assert base == 0x400000 and pe.FILE_HEADER.Machine == 0x14c
    md = Cs(CS_ARCH_X86, CS_MODE_32)

    def data(va, size):
        offset = pe.get_offset_from_rva(va - base)
        value = raw[offset:offset + size]
        assert len(value) == size
        return value

    def ins(lo, hi):
        return md.disasm(data(lo, hi - lo), lo)

    def u32(va):
        return struct.unpack('<I', data(va, 4))[0]

    checks = []

    def instruction(va, mnemonic, operands):
        i = next(ins(va, va + 16))
        assert (i.mnemonic, i.op_str) == (mnemonic, operands), (hex(va), i.mnemonic, i.op_str)
        checks.append(va)

    instruction(0x6fdb2d, 'call', '0x6def80')
    instruction(0x6fdb32, 'fldz', '')
    instruction(0x6fdb34, 'mov', 'dword ptr [esi], 0x8bd0bc')
    for va, field in [(0x6fdb84, 0x9c4), (0x6fdb8a, 0x9c8), (0x6fdb90, 0x9cc)]:
        instruction(va, 'fst', f'dword ptr [esi + {hex(field)}]')
    assert u32(0x8bd0bc + 17 * 4) == 0x6fe1c0
    instruction(0x6fe209, 'call', '0x7104f0')
    instruction(0x6fe225, 'test', 'byte ptr [esi + 0x42b], 1')
    instruction(0x6fe22c, 'je', '0x6fe3a4')
    instruction(0x6fe3e1, 'fld', 'dword ptr [esi + 0x9c4]')
    instruction(0x6fe3e7, 'fmul', 'dword ptr [0xc0fd50]')
    instruction(0x6fe3ed, 'fadd', 'dword ptr [esi + 0x9c8]')
    instruction(0x6fe430, 'and', 'al, 0xf8')
    instruction(0x6fe432, 'cmp', 'al, 0x18')
    instruction(0x6fe434, 'ja', '0x6fe9a7')
    instruction(0x6fe9a9, 'mov', 'edi, 0xc')
    instruction(0x6fe9ae, 'lea', 'ebx, [edi + 1]')
    instruction(0x6fe9c4, 'fadd', 'st(0), st(0)')
    instruction(0x6fe9c9, 'fmul', 'dword ptr [esi + 0x9c8]')
    instruction(0x6fe9d8, 'push', '1')
    instruction(0x6fe9db, 'call', '0x715b80')
    instruction(0x6fea07, 'push', '0xff')
    instruction(0x6fea0d, 'call', '0x70caf0')
    instruction(0x6fea25, 'fchs', '')
    instruction(0x6fea28, 'fmul', 'dword ptr [esi + 0x9c8]')
    instruction(0x6fea68, 'push', '0')
    instruction(0x6fea6b, 'call', '0x70caf0')
    instruction(0x6fea7e, 'mov', 'edi, 0xe')
    instruction(0x6ce126, 'test', 'byte ptr [eax + 2], 4')
    instruction(0x6ce12a, 'je', '0x6ce131')
    instruction(0x6ce12f, 'mov', 'dword ptr [ecx], eax')
    instruction(0x70cafc, 'or', 'dword ptr [eax + 8], 0x40')
    instruction(0x70cb01, 'push', '0x70cae0')
    instruction(0x70cae9, 'mov', 'byte ptr [eax + 7], cl')
    instruction(0x715c0c, 'call', '0x5b1fd0')
    instruction(0x6d8704, 'fmul', 'qword ptr [0x8a2270]')
    instruction(0x6d88b3, 'call', '0x5b1350')
    instruction(0x6d8955, 'call', '0x5b16b0')
    instruction(0x6d4712, 'fsub', 'dword ptr [edi + 0x898]')
    instruction(0x6df6fc, 'call', '0x6d44d0')
    assert len(checks) == 40
    prop = list(ins(0x6fe9a7, 0x6fea8b))
    assert not any('[esi + 0x9c4]' in i.op_str or '[esi + 0x428]' in i.op_str for i in prop)
    assert sum(i.mnemonic == 'call' and i.op_str == '0x70caf0' for i in prop) == 2
    assert data(0x6ff890, 1)[0] == 0 and u32(0x6ff878) == 0x6feab6
    gear_left = struct.unpack('<f', data(0x8bd1f0, 4))[0]
    gear_right = struct.unpack('<f', data(0x8bd098, 4))[0]
    assert abs(gear_left + math.radians(85)) < 1e-6 and gear_right == -gear_left
    descriptors = []
    for va in range(0x916740, 0x916848, 12):
        ptr, node, flags = struct.unpack('<3I', data(va, 12))
        name = data(ptr, 40).split(b'\0')[0].decode()
        if name in ('static_prop', 'moving_prop'):
            descriptors.append((node, flags))
    assert descriptors == [(12, 0x40482), (13, 0x40482)]
    width = struct.unpack('<d', data(0x8a2270, 8))[0]
    yaw = struct.unpack('<f', data(0x8a3610, 4))[0]
    # The source double stores the promoted float normalization constant.
    assert width == f32(.7) and yaw == f32(math.pi)
    actual, height = source_pose(game, width, yaw)
    expected, _ = source_pose(game)
    assert actual == expected
    print(f'retail-source-proof-ok size={len(raw)} SHA256={digest} instructions={len(checks)} '
          f'matrices={len(actual)} frontHeight={height:.10f} in-memory=1 static-only=1')
