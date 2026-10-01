"""Original HAnim application/traversal oracle; sampled frames/attachment are explicit.

Original matrix multiply, parent stack, frame writes and root dirty linking run
in isolated development memory. No EXE is an application dependency.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP, UC_X86_REG_FPCW

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
output = root / 'artifacts/graphics'
assert output.is_dir()
stem = 'NativePedHierarchyProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_hierarchy_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
        '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedHierarchyProbe', 'NativePedHierarchy', 'NativePedHitCollision')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)
assert 'native-ped-hierarchy-ok ' in native.stdout
game = Path(args.game_dir).resolve() / 'gta-sa.exe'
assert game.stat().st_size == 5971456
pe = pefile.PE(str(game))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
for address, size, digest in (
    (0x7f99d0, 0x9b0, '27467bd2efb933929e18ae487e5f31ae29eb32a25952fa4adf36726f79371139'),
    (0x8260d0, 0x120, 'e746e17a6b7b22c0cc99b2032a62f44500a4121ff987f980cbf7856ad83b64db'),
    (0x825b00, 0x1b0, '6988fcb800e040af65e7ca1e5751813d286119575e9edad7fe25f66184438c74'),
    (0x7fa380, 0x119, 'ff75eb56594ae64417f8e62a9e26cf679aa5677ba7c853f6434b66b7c31e787f'),
    (0x7621d0, 0x155, 'd836046f33038a48b6356b3d39b50834ff2618eae42be34c1209319aaf740f1a'),
    (0x826890, 0x250, 'ddbfe0ae8695520fe4e951e2e54d747b65c5775f09b032f00bcd69179d290530'),
    (0x821e80, 0xb7, '3539682ed3428299afc7b64d7af87c9aaaeae512be4b313bd4f365dab73fdf69'),
    (0x8225a0, 0x30, 'e58d2bcb919c084fecd028a2145039b0179e164a2c9877bdb6672b181cdc70d2'),
    (0x4e0cb0, 0xf0, '898833854a15ddc34736e7be39fe49f0f3cee1e2db6be9f6f7ff1a33a1f4c155'),
    (0x4e0560, 0xb, '2b67ebf27c8bd5fe73d20608ae252ec497fa45d525be19f8e843b94410b169c7'),
    (0x4e0150, 0x120, '4b78b3122364957eedd55b78d0b2ecf48ed0305b7a8384e222ed2439de34a097'),
    (0x4e0270, 0x20, '1112b331d32b2a162ff3cda0b819fcd36bbbf7d4dfc3cd48fa1f0ad4fa3546d8'),
    (0x4e0310, 0x9c, 'bb67c59479d09648673076b84cf2e062f3d08062b2533a7cb1a70a0ecf617032'),
    (0x4d9430, 0x100, '747f15ce4710d2aa7f6886dc6b42e280f3a26c42834c5b766090ed519ac0c705'),
):
    assert hashlib.sha256(pe.get_data(address - 0x400000, size)).hexdigest() == digest
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0, 0x1000)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
hierarchy, parent_hierarchy, root_frame, parent_frame, nodes, matrices, sub_matrices, interpolator, frames, engine = (
    0x30a0000, 0x30a1000, 0x30a2000, 0x30a3000, 0x30a4000, 0x30a5000,
    0x30a6000, 0x30b0000, 0x30c0000, 0x30d0000)
parent_ltm, callback, sentinel = 0x30a7000, 0x30f0000, 0x30d1000
bind_skin, bind_geometry, bind_atomic, bind_clump = 0x30a8000, 0x30a9000, 0x30a9100, 0x30a9200
bind_count = 0
blend_data, blend_frames = 0x30a9400, 0x30c9000
initializing = False
def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))
def read(address, fmt):
    return struct.unpack('<' + fmt, uc.mem_read(address, struct.calcsize('<' + fmt)))
def matrix(address, values):
    flags, *bits = values
    for i in range(4):
        write(address + i * 16, '4I', *bits[i * 3:i * 3 + 3], flags if i == 0 else 0)
def matrix_values(address):
    result = [read(address + 12, 'I')[0]]
    for i in range(4): result.extend(read(address + i * 16, '3I'))
    return result
write(0xd23664, 'I', engine)
write(0xd234fc, 'I', 0x100)
write(engine + 0x104, 'I', 0x20000)
write(engine + 0x108, 'I', 0x825b00)
write(0xd23474, 'I', 0x200)
write(engine + 0x20c, 'I', 0x821e80)
write(bind_atomic + 0x18, 'I', bind_geometry)
for address in (0x824b50, 0x8251a0, 0x825120, callback,
                0x77e360, 0x7fbd60, 0x7fbff0, 0x7fc020, 0x8535ae, 0x778710,
                0x7805c0, 0x801550, 0x761690, 0x761890): uc.mem_write(address, b'\xc3')
updates, applies = [], []
def code(machine, address, size, _):
    stack = machine.reg_read(UC_X86_REG_ESP)
    if address == 0x7805c0:
        assert read(stack + 4, '5I') == (4, 0x253f2fb, 0x4e00e0, 0x4e02d0, 0x4e0140)
        machine.reg_write(UC_X86_REG_EAX, 0x40)
    elif address == 0x801550:
        info = read(stack + 4, 'I')[0]
        assert read(info, '12I') == (0x253f2fb, 28, 36, 0x4e0150, 0x7fa8c0, 0x4e0270,
                                   0x7faf20, 0x7fadc0, 0x7facc0, 0x7fad40, 0x7fadb0, 0)
    elif address == 0x8535ae:
        assert initializing and read(stack + 4, 'I')[0] == 20
        machine.reg_write(UC_X86_REG_EAX, blend_data)
    elif address == 0x778710:
        assert initializing and read(stack + 4, '3I') == (((bind_count * 24 + 63) // 64) * 64, 64, 0)
        machine.reg_write(UC_X86_REG_EAX, blend_frames)
    elif address in (0x761690, 0x761890):
        assert initializing and read(stack + 4, 'I')[0] == bind_clump
        machine.reg_write(UC_X86_REG_EAX, bind_atomic if address == 0x761690 else hierarchy)
    elif address == 0x4e0150:
        source = read(stack + 8, 'I')[0]
        if not initializing and source >= interpolator + 0x4c:
            applies.append((source - interpolator - 0x4c) // 28)
    elif address == 0x77e360:
        clump, getter, destination = read(stack + 4, '3I')
        assert clump == bind_clump and getter in (0x761680, 0x761870)
        write(destination, 'I', bind_atomic if getter == 0x761680 else hierarchy)
        machine.reg_write(UC_X86_REG_EAX, clump)
    elif address == 0x7fbd60:
        assert read(stack + 4, 'I')[0] == bind_geometry
        machine.reg_write(UC_X86_REG_EAX, bind_skin)
    elif address in (0x7fbff0, 0x7fc020):
        assert read(stack + 4, 'I')[0] == bind_skin
        machine.reg_write(UC_X86_REG_EAX, bind_count if address == 0x7fbff0 else matrices)
    elif address == 0x824b50:
        assert read(stack + 4, 'I')[0] == parent_frame
        machine.reg_write(UC_X86_REG_EAX, 0)  # Already synchronized parent observation.
    elif address == 0x8251a0:
        assert read(stack + 4, 'I')[0] == parent_frame
        machine.reg_write(UC_X86_REG_EAX, parent_ltm)
    elif address == callback:
        destination, source = read(stack + 4, 'II')
        assert source >= interpolator + 0x4c and (source - interpolator - 0x4c) % 64 == 0
        applies.append((source - interpolator - 0x4c) // 64)
        machine.mem_write(destination, bytes(machine.mem_read(source, 64)))
    elif address == 0x825120:
        updates.append(read(stack + 4, 'I')[0])  # Explicit external object-update intent.
uc.hook_add(UC_HOOK_CODE, code)
cases = interpolation_cases = default_cases = bind_cases = blend_cases = init_cases = gta_cases = 0
stack = 0x30ff000
write(stack, 'I', 0x30e0000)
uc.reg_write(UC_X86_REG_ESP, stack)
uc.emu_start(0x4e0310, 0x30e0000, count=1000)
assert uc.reg_read(UC_X86_REG_EAX) == 1 and read(0xbd6f4c, 'I')[0] == 0x40
write(interpolator + 0x4c, '7I', *([0x55555555] * 7))
stack = 0x30ff000
write(stack, '6I', 0x30e0000, interpolator + 0x4c, 0, 0, 0, 0)
uc.reg_write(UC_X86_REG_ESP, stack)
uc.reg_write(UC_X86_REG_FPCW, 0x37f)
uc.emu_start(0x4e0270, 0x30e0000, count=1000)
assert read(interpolator + 0x4c, '7I') == (0, 0, 0, 0x3f800000, 0, 0, 0)
for line in native.stdout.splitlines():
    if line.startswith('BLEND_INIT '):
        fields = list(map(int, line.split()[1:]))
        index, bind_count, frame_stride = fields[:3]
        authored, expected = fields[3:3 + bind_count * 15], fields[3 + bind_count * 15:]
        write(0xbd6f4c, 'I', 0x40)
        write(hierarchy + 0x10, 'I', nodes)
        write(hierarchy + 0x20, 'I', interpolator)
        write(interpolator + 0x24, 'I', frame_stride)
        for j in range(bind_count):
            tag, node_flags, *values = authored[j * 15:(j + 1) * 15]
            write(nodes + j * 16, '4I', tag, j, node_flags, 0)
            matrix(matrices + j * 64, values)
        uc.mem_write(blend_frames, b'\x55' * (bind_count * 24 + 24))
        uc.mem_write(interpolator + 0x4c, b'\x66' * (bind_count * frame_stride))
        stack = 0x30ff000
        write(stack, 'I', 0x30e0000)
        uc.reg_write(UC_X86_REG_EAX, bind_clump)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_FPCW, 0x37f)
        initializing = True
        uc.emu_start(0x4e0cb0, 0x30e0000, count=100000)
        initializing = False
        assert uc.reg_read(UC_X86_REG_ESP) == stack + 4
        observed = []
        for j in range(bind_count):
            frame = blend_frames + j * 24
            observed += [read(frame, 'B')[0], read(frame + 20, 'i')[0],
                         read(frame + 16, 'I')[0] - interpolator - 0x4c,
                         *read(frame + 4, '3I')]
            assert bytes(uc.mem_read(frame + 1, 3)) == b'\x55' * 3
        assert observed == expected, (index, bind_count, observed, expected)
        assert bytes(uc.mem_read(blend_frames + bind_count * 24, 24)) == b'\x55' * 24
        assert bytes(uc.mem_read(interpolator + 0x4c, bind_count * frame_stride)) == b'\x66' * (bind_count * frame_stride)
        init_cases += 1
        continue
    if line.startswith('BIND_POSITION '):
        fields = list(map(int, line.split()[1:]))
        index, bind_count = fields[:2]
        authored, expected = fields[2:2 + bind_count * 14], fields[2 + bind_count * 14:]
        write(hierarchy + 0x10, 'I', nodes)
        for j in range(bind_count):
            node_flags, *values = authored[j * 14:(j + 1) * 14]
            write(nodes + j * 16, '4I', j, j, node_flags, 0)
            matrix(matrices + j * 64, values)
        uc.mem_write(frames, b'\x55' * (bind_count * 12 + 12))
        stack = 0x30ff000
        write(stack, '3I', 0x30e0000, bind_clump, frames)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_FPCW, 0x37f)
        uc.emu_start(0x7621d0, 0x30e0000, count=100000)
        assert uc.reg_read(UC_X86_REG_ESP) == stack + 4
        observed = list(read(frames, 'I' * (bind_count * 3)))
        assert observed == expected, (index, bind_count, observed, expected)
        assert bytes(uc.mem_read(frames + bind_count * 12, 12)) == b'\x55' * 12
        bind_cases += 1
        continue
    blend = line.startswith('BLEND_APPLICATION ')
    if blend or line.startswith('INTERPOLATION '):
        fields = list(map(int, line.split()[1:]))
        index, pose, expected = fields[0], fields[1:8], fields[8:]
        if blend: write(interpolator + 0x4c, '7I', *pose)
        else: write(interpolator + 0x4c, '9I', 0, 0, *pose)
        uc.mem_write(matrices, b'\x55' * 64)
        stack = 0x30ff000
        write(stack, '3I', 0x30e0000, matrices, interpolator + 0x4c)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_FPCW, 0x37f)
        uc.emu_start(0x4e0150 if blend else 0x7fa380, 0x30e0000, count=1000)
        assert uc.reg_read(UC_X86_REG_ESP) == stack + 4
        assert matrix_values(matrices) == expected, (index, matrix_values(matrices), expected)
        if blend: blend_cases += 1
        else: interpolation_cases += 1
        continue
    default = line.startswith('HIERARCHY_DEFAULT ')
    gta = line.startswith('HIERARCHY_BLEND ')
    if not default and not gta and not line.startswith('HIERARCHY '):
        if line.startswith('native-ped-hierarchy-ok '): print(line)
        continue
    fields = list(map(int, line.split()[1:]))
    index, mode, flags, has_parent, parent_index, private = fields[:6]
    parent, sub_parent, count = fields[6:19], fields[19:32], fields[32]
    stride = 24 if default or gta else 17
    authored, expected = fields[33:33 + count * stride], fields[33 + count * stride:]
    uc.mem_write(frames, bytes(count * 0x100))
    uc.mem_write(matrices, bytes(count * 64))
    write(hierarchy, 'I', flags)
    write(hierarchy + 4, 'II', count, matrices)
    write(hierarchy + 0x10, '5I', nodes, root_frame, parent_hierarchy, parent_index & 0xffffffff, interpolator)
    write(parent_hierarchy + 8, 'I', sub_matrices)
    write(parent_hierarchy + 0x14, 'I', root_frame)
    matrix(sub_matrices, sub_parent)
    matrix(parent_ltm, parent)
    write(root_frame + 3, 'B', 0)
    write(root_frame + 4, 'I', parent_frame if has_parent else 0)
    write(root_frame + 0xa0, 'I', root_frame)
    write(root_frame + 3, 'B', private)
    write(root_frame + 8, 'II', sentinel, sentinel)
    write(engine + 0xbc, 'I', sentinel)
    write(sentinel, 'II', engine + 0xbc, engine + 0xbc)
    write(interpolator + 0x24, 'I', 28 if gta else 36 if default else 64)
    write(interpolator + 0x3c, 'I', 0x4e0150 if gta else 0x7fa380 if default else callback)
    for j in range(count):
        tag, node_flags, has_frame, frame_private, *applied = authored[j * stride:(j + 1) * stride]
        frame = frames + j * 0x100
        write(nodes + j * 16, '4I', tag, j, node_flags, frame if has_frame else 0)
        write(frame + 3, 'B', frame_private)
        if gta:
            write(interpolator + 0x4c + j * 28, '7I', *applied[13:])
        elif default:
            write(interpolator + 0x4c + j * 36, '9I', 0, 0, *applied[13:])
        else:
            matrix(interpolator + 0x4c + j * 64, applied)
        uc.mem_write(frame + 0x10, b'\x55' * 64)
        uc.mem_write(frame + 0x50, b'\x66' * 64)
    updates.clear(); applies.clear()
    stack = 0x30ff000
    write(stack, 'II', 0x30e0000, hierarchy)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x7f99d0, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_ESP) == stack + 4 and uc.reg_read(UC_X86_REG_EAX) == 1
    enqueue, root_private = expected[:2]
    assert applies == ([] if default else list(range(count))), (index, mode, 'callback order', applies)
    assert read(root_frame + 3, 'B')[0] == (root_private if flags & 0x2000 else private)
    assert read(engine + 0xbc, 'I')[0] == (root_frame + 8 if enqueue else sentinel)
    if enqueue:
        assert read(root_frame + 8, 'II') == (sentinel, engine + 0xbc)
        assert read(sentinel + 4, 'I')[0] == root_frame + 8
    offset = 2
    expected_updates = []
    for j in range(count):
        frame_private, update = expected[offset:offset + 2]; offset += 2
        frame = frames + j * 0x100
        if update: expected_updates.append(frame)
        assert read(frame + 3, 'B')[0] == frame_private, (index, mode, j, 'frame flags')
        for destination, untouched in ((matrices + j * 64, bytes(64)), (frame + 0x10, b'\x55' * 64),
                                       (frame + 0x50, b'\x66' * 64)):
            present = expected[offset]; offset += 1
            if present:
                values = expected[offset:offset + 13]; offset += 13
                observed = matrix_values(destination)
                assert observed == values, (index, mode, j, hex(destination), observed, values)
            else:
                assert bytes(uc.mem_read(destination, 64)) == untouched, (index, mode, j, 'unexpected write')
    assert offset == len(expected) and updates == expected_updates
    cases += 1
    default_cases += int(default)
    gta_cases += int(gta)
assert cases == 6144 and default_cases == 2048 and gta_cases == 2048 and interpolation_cases == 8192 and bind_cases == 1280
assert blend_cases == 8192 and init_cases == 1280
print('ped-hierarchy-source-oracle-ok cases=6144 interpolation=8192 inline=2048 bind-positions=1280 blend-init=1280 '
      'blend-application=8192 GTA-callback=2048 matrices=original traversal=original '
      'callbacks=explicit,original-default,GTA frame-effects=planned loaded-state=unowned census=incomplete')
