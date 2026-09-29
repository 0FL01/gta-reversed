"""Development-only original gang-model and sequence oracle.

Groups and streaming bytes are explicit fixtures, not a live ambient census.
The original bodies exist only in isolated test memory; native runtime never
loads or calls the executable. No original code/media is copied to disk.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
game = Path(args.game_dir).resolve()
output = root / 'artifacts/graphics'
assert output.is_dir()
stem = 'NativeGangPedChoiceProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_gang_ped_choice_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativeGangPedChoiceProbe', 'NativeGangPedChoice', 'NativeSourceRng')]
    command += ['-pthread', '-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)

assert (game / 'gta-sa.exe').stat().st_size == 5971456
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
function_hash = hashlib.sha256(pe.get_data(0x5fa5a0 - 0x400000, 0xd9)).hexdigest()
sequence_hash = hashlib.sha256(pe.get_data(0x422900 - 0x400000, 0x68)).hexdigest()
assert function_hash == '9385173b0b6616128962eaae16ae69c728dc72b6805400a9d5d7e8ed582213c9'
assert sequence_hash == '6d420f087f643529e328f3ca1490aa8ade998a798d104e9307db23dc7db305f9'
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
uc.mem_write(0x853e0c, b'\xc3')

def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))

def read(address, fmt):
    return struct.unpack('<' + fmt, uc.mem_read(address, struct.calcsize('<' + fmt)))[0]

state = {}
def random_draw(machine, address, size, _):
    if address == 0x853e0c:
        state['value'] = (state['value'] * 214013 + 2531011) & 0xffffffff
        state['draws'] += 1
        machine.reg_write(UC_X86_REG_EAX, (state['value'] >> 16) & 32767)

uc.hook_add(UC_HOOK_CODE, random_draw)
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'CASE':
        continue
    assert len(fields) == 11, fields
    seed, count, profile, status, model, elements, offset, ascending, rng_state, draws = map(int, fields[1:])
    # Gang0: zone0 count row0, current zone1 data row1, no count-row guessing.
    write(0x945d98, '3I', 0, 1, 2)
    write(0xc9c018, '3h', count, 1, 0)
    write(0xc9d014, 'I', 1)
    write(0xc960e8, 'B', 17 if profile == 5 else 255)
    for slot in range(21):
        identity = 10 + slot
        write(0xc9c6b0 + (21 + slot) * 2, 'H', identity)
        loaded = (profile == 0 or (profile == 2 and slot % 3 == 0) or
                  (profile == 3 and slot == count - 1) or (profile == 4 and slot == 0))
        write(0x95c8b0 + identity * 20, 'B', loaded)
    write(0x9e0bb8, 'i', 77)
    write(0x9e0bb4, 'i', 33)
    write(0x9e0bb0, 'B', 1)
    state = dict(value=seed, draws=0)
    stack = 0x3080000
    write(stack, '2I', 0x30e0000, 0)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.emu_start(0x5fa5a0, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('instruction cap', fields)
    result = uc.reg_read(UC_X86_REG_EAX)
    result = result if result < 0x80000000 else result - 0x100000000
    actual = (result, read(0x9e0bb8, 'i'), read(0x9e0bb4, 'i'), read(0x9e0bb0, 'B'),
              state['value'], state['draws'])
    expected = (model if status == 0 else -1, elements, offset, ascending, rng_state, draws)
    assert actual == expected, ('gang model/sequence', fields, actual, expected)
    checks += 1
assert checks == 630
print('gang-ped-choice-retail-oracle-ok checks=' + str(checks),
      'slots=21 direction-bit=4 source-functions=gang,sequence-init,sequence-index',
      'streaming=explicit-fixtures census=incomplete',
      'function-sha256=' + function_hash, 'sequence-sha256=' + sequence_hash)
print(native.stdout.splitlines()[-1])
