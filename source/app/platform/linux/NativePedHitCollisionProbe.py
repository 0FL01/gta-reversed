"""Original skinned hit-COL oracle. Actual RW inverse/multiply/point math runs.

Allocation and the supplied current HAnim matrices are explicit observations,
not a Loaded model or an animation/controller proof. Owned retail code remains
in isolated developer-test memory; the application never reads the EXE.
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
stem = 'NativePedHitCollisionProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_hit_col_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
        '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedHitCollisionProbe', 'NativePedHitCollision')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)
assert 'native-ped-hit-col-ok ' in native.stdout
game = Path(args.game_dir).resolve() / 'gta-sa.exe'
assert game.stat().st_size == 5971456
pe = pefile.PE(str(game))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
ranges = (
    (0x4d1040, 0x1ad, '098eaff273af1aef22e1091afad9fe0e26cc3d4bf7d6f4f060f8fefdb63e004f'),
    (0x826890, 0x250, 'ddbfe0ae8695520fe4e951e2e54d747b65c5775f09b032f00bcd69179d290530'),
    (0x826dc0, 0x170, '58eea36da49f69f1cf48270b81a62bffdcc034b681dc03741cbd521b8726a87a'),
    (0x825b00, 0x1b0, '6988fcb800e040af65e7ca1e5751813d286119575e9edad7fe25f66184438c74'),
    (0x821e80, 0xb7, '3539682ed3428299afc7b64d7af87c9aaaeae512be4b313bd4f365dab73fdf69'),
    (0x9162f0, 12 * 28, 'bd805f0407d5ef787d63af93b303d58a574e25bb17916e12df2c2f336339fb4a'),
)
for address, size, digest in ranges:
    assert hashlib.sha256(pe.get_data(address - 0x400000, size)).hexdigest() == digest
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0, 0x1000)  # Original exception registration, not a host exception handler.
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
model, clump, frame, hierarchy, nodes, matrices, cm, data, spheres, inverse, combined, engine = (
    0x30a0000, 0x30a1000, 0x30a2000, 0x30a3000, 0x30a4000, 0x30b0000,
    0x30c0000, 0x30c1000, 0x30c2000, 0x30c3000, 0x30c4000, 0x30d0000)
def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))
def read(address, fmt):
    return struct.unpack('<' + fmt, uc.mem_read(address, struct.calcsize('<' + fmt)))
def matrix(address, values):
    flags, *bits = values
    for i in range(4):
        write(address + i * 16, '4I', *bits[i * 3:i * 3 + 3], flags if i == 0 else 0)
# RW initialized-default dispatch from the original engine initializer:
# identity mask 0x20000, actual original multiply/transform implementations.
write(0xd23664, 'I', engine)
write(0xd234fc, 'I', 0x100)
write(engine + 0x104, 'I', 0x20000)
write(engine + 0x108, 'I', 0x825b00)
write(0xd23474, 'I', 0x200)
write(engine + 0x20c, 'I', 0x821e80)
write(0xbffbf0, 'II', inverse, combined)
uc.mem_write(0x761890, b'\xc3')
uc.mem_write(0x41a9b0, b'\xc3')
uc.mem_write(0x41a8c0, b'\xc3')
uc.mem_write(0x41a600, b'\xc2\x18\x00')
def code(machine, address, size, _):
    if address == 0x761890:
        machine.reg_write(UC_X86_REG_EAX, hierarchy)
    elif address == 0x41a9b0:
        assert read(machine.reg_read(UC_X86_REG_ESP) + 4, 'I')[0] == 0x30
        machine.reg_write(UC_X86_REG_EAX, cm)
    elif address == 0x41a8c0:
        machine.reg_write(UC_X86_REG_EAX, cm)
    elif address == 0x41a600:
        assert read(machine.reg_read(UC_X86_REG_ESP) + 4, '6I') == (12, 0, 0, 0, 0, 0)
        write(cm + 0x2c, 'I', data)
        write(data + 8, 'I', spheres)
uc.hook_add(UC_HOOK_CODE, code)
cases = 0
for line in native.stdout.splitlines():
    if not line.startswith('HIT_COL '):
        if line.startswith('native-ped-hit-col-ok '): print(line)
        continue
    fields = list(map(int, line.split()[1:]))
    index, root_matrix, count = fields[0], fields[1:14], fields[14]
    bone_fields, expected = fields[15:15 + count * 14], fields[15 + count * 14:]
    assert len(expected) == 72
    uc.mem_write(cm, bytes(0x30)); uc.mem_write(spheres, bytes(12 * 20))
    matrix(frame + 0x10, root_matrix)
    write(clump + 4, 'I', frame)
    write(hierarchy + 4, 'II', count, matrices)
    write(hierarchy + 0x10, 'I', nodes)
    for j in range(count):
        tag, *values = bone_fields[j * 14:(j + 1) * 14]
        write(nodes + j * 16, 'I', tag)
        matrix(matrices + j * 64, values)
    stack = 0x30ff000
    write(stack, 'II', 0x30e0000, clump)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_ECX, model)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x4d1040, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_ESP) == stack + 8
    observed = []
    for j in range(12):
        observed += list(read(spheres + j * 20, '4I2B'))
    assert observed == expected, ('hit-col mismatch', index, [(j, a, b) for j, (a, b) in enumerate(zip(observed, expected)) if a != b])
    assert read(model + 0x34, 'I')[0] == cm
    assert read(cm, '6f') == (-0.5, -0.5, struct.unpack('<f', struct.pack('<f', -1.2))[0],
                             0.5, 0.5, struct.unpack('<f', struct.pack('<f', 1.2))[0])
    assert read(cm + 0x18, '4f') == (0.0, 0.0, 0.0, 1.5)
    assert read(cm + 0x28, 'B')[0] == 0
    cases += 1
assert cases == 4112
print('ped-hit-col-source-oracle-ok cases=4112 nodes=12 matrix-math=original '
      'hierarchy=explicit loaded-state=unowned census=incomplete')
