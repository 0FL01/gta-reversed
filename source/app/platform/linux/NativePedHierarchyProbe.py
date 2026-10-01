"""Original HAnim traversal oracle; callback matrices/attachment are explicit.

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
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ESP, UC_X86_REG_FPCW

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
for address in (0x824b50, 0x8251a0, 0x825120, callback): uc.mem_write(address, b'\xc3')
updates, applies = [], []
def code(machine, address, size, _):
    stack = machine.reg_read(UC_X86_REG_ESP)
    if address == 0x824b50:
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
cases = 0
for line in native.stdout.splitlines():
    if not line.startswith('HIERARCHY '):
        if line.startswith('native-ped-hierarchy-ok '): print(line)
        continue
    fields = list(map(int, line.split()[1:]))
    index, mode, flags, has_parent, parent_index, private = fields[:6]
    parent, sub_parent, count = fields[6:19], fields[19:32], fields[32]
    authored, expected = fields[33:33 + count * 17], fields[33 + count * 17:]
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
    write(interpolator + 0x24, 'I', 64)
    write(interpolator + 0x3c, 'I', callback)
    for j in range(count):
        tag, node_flags, has_frame, frame_private, *applied = authored[j * 17:(j + 1) * 17]
        frame = frames + j * 0x100
        write(nodes + j * 16, '4I', tag, j, node_flags, frame if has_frame else 0)
        write(frame + 3, 'B', frame_private)
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
    assert applies == list(range(count)), (index, mode, 'callback order', applies)
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
assert cases == 2048
print('ped-hierarchy-source-oracle-ok cases=2048 matrices=original traversal=original '
      'callback=explicit frame-effects=planned loaded-state=unowned census=incomplete')
