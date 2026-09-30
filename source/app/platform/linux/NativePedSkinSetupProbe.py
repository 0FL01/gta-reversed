"""Original simple/complex SetClump skin-write oracle; dependencies are fixtures.

Actual x87 radius/weight writes and hierarchy branches execute. TXD/animation
refs, lighting, hierarchy binding and model publication are NOT certified here.
Original code stays in isolated developer-test memory, never application runtime.
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
stem = 'NativePedSkinSetupProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_skin_setup_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
        '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedSkinSetupProbe', 'NativePedSkinSetup')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)
assert 'native-ped-skin-setup-ok ' in native.stdout
game = Path(args.game_dir).resolve() / 'gta-sa.exe'
assert game.stat().st_size == 5971456
pe = pefile.PE(str(game))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
assert hashlib.sha256(pe.get_data(0x4cefb0 - 0x400000, 0x1ac)).hexdigest() == 'a3e4c787129ead17d06cb35c169ce48aca80a77f666619f60dd5ff3140a51882'
assert struct.unpack('<d', pe.get_data(0x8a1f50 - 0x400000, 8))[0] == struct.unpack('<f', struct.pack('<f', 1.2))[0]
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
atomic, geometry, morph, skin, hierarchy, model, vtable, clump, weights = (
    0x30a0000, 0x30a1000, 0x30a2000, 0x30a3000, 0x30a4000, 0x30a5000, 0x30a6000, 0x30a7000, 0x30b0000)
# External resources, callbacks and getters have stable explicit observations.
for stub in (0x7616f0, 0x767fb0, 0x4ceb70, 0x30f0000, 0x77e360,
             0x761690, 0x7fbd60, 0x761960, 0x7fc000):
    uc.mem_write(stub, b'\xc3')
counts = {}
def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))
def code(machine, address, size, _):
    values = {0x7616f0: 0, 0x30f0000: 0xffffffff, 0x761690: atomic,
              0x7fbd60: skin, 0x761960: hierarchy, 0x7fc000: weights}
    if address in values:
        machine.reg_write(UC_X86_REG_EAX, values[address])
    if address in (0x77e360, 0x7fc000):
        counts[address] = counts.get(address, 0) + 1
uc.hook_add(UC_HOOK_CODE, code)
cases = simple = complex_cases = 0
for line in native.stdout.splitlines():
    if not line.startswith('SKIN_SETUP '):
        if line.startswith('native-ped-skin-setup-ok '):
            print(line)
        continue
    fields = list(map(int, line.split()[1:]))
    index, complex_hierarchy, radius, flags, count, expected_radius, expected_flags, expected_count = fields[:8]
    original = fields[8:8 + count * 4]
    expected = fields[8 + count * 4:]
    assert len(original) == count * 4 and len(expected) == expected_count * 4
    counts.clear()
    for pointer in (atomic, geometry, morph, skin, hierarchy, model, vtable, clump):
        uc.mem_write(pointer, bytes(256))
    write(model, 'I', vtable)
    write(model + 0x12, 'H', 0x200 if complex_hierarchy else 0)
    write(vtable + 0x38, 'I', 0x30f0000)
    write(atomic + 0x18, 'I', geometry)
    write(geometry + 0x14, 'I', count)
    write(geometry + 0x5c, 'I', morph)
    write(morph + 0x10, 'I', radius)
    write(hierarchy, 'I', flags)
    write(weights, str(len(original)) + 'I', *original)
    stack = 0x30ff000
    write(stack, 'II', 0x30e0000, clump)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_ECX, model)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x4cefb0, 0x30e0000, count=10000)
    assert uc.reg_read(UC_X86_REG_ESP) == stack + 8
    observed_radius = struct.unpack('<I', uc.mem_read(morph + 0x10, 4))[0]
    observed_flags = struct.unpack('<I', uc.mem_read(hierarchy, 4))[0]
    observed_weights = list(struct.unpack('<' + str(count * 4) + 'I', uc.mem_read(weights, count * 16)))
    assert (observed_radius, observed_flags, observed_weights) == (
        expected_radius, expected_flags, original if complex_hierarchy else expected), ('mismatch', index, complex_hierarchy)
    assert counts.get(0x7fc000, 0) == (0 if complex_hierarchy else count)
    assert counts[0x77e360] == 2
    if complex_hierarchy:
        complex_cases += 1
    else:
        simple += 1
    cases += 1
assert cases == 8192 and simple == complex_cases == 4096
print('ped-skin-setup-source-oracle-ok cases=8192 simple=4096 complex=4096 weights=bit-exact '
      'radius=widened-float-constant dependencies=explicit loaded-state=unowned census=incomplete')
