"""Development-only isolated retail occupation oracle.

Eight streaming slots/model metadata are explicit controlled observations.
Zone, attractor and stat policies are fixtures, not ambient census authority.
No executable bytes are copied to disk or used by the native application.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP, UC_X86_REG_FPCW

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
game = Path(args.game_dir).resolve()
output = root / 'artifacts/graphics'
assert output.is_dir()
stem = 'NativeCivilianOccupationProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_civilian_occupation_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativeCivilianOccupationProbe', 'NativeCivilianOccupation')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)

assert (game / 'gta-sa.exe').stat().st_size == 5971456
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
function_hash = hashlib.sha256(pe.get_data(0x630b50 - 0x400000, 0x1d6)).hexdigest()
assert function_hash == '52c471e199e84807386bc0c04ad6de1e6a13ecb6796723cfef56a88cf676b5d7'
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)

def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))

def read_int(address):
    return struct.unpack('<i', uc.mem_read(address, 4))[0]

for address in (0x62e100, 0x62eac0, 0x62e930):
    uc.mem_write(address, b'\xc3')
state = {}

def observe(machine, address, size, _):
    stack = machine.reg_read(UC_X86_REG_ESP)
    profile = state['profile']
    if address == 0x62e100:
        state['zone'] += 1
        model = read_int(stack + 4)
        accepted = profile % 3 == 0 or (model + profile) % 3 != 0
    elif address == 0x62eac0:
        state['attractor'] += 1
        model = read_int(stack + 4)
        assert uc.mem_read(read_int(stack + 8), 4) == b'ATM\0'
        accepted = (model + profile) % 4 != 0
    elif address == 0x62e930:
        state['stats'] += 1
        actual, requested = read_int(stack + 4), read_int(stack + 8)
        accepted = (actual + requested + profile) % 3 == 0
    else:
        return
    machine.reg_write(UC_X86_REG_EAX, int(accepted))

uc.hook_add(UC_HOOK_CODE, observe)
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'CASE':
        continue
    assert len(fields) == 9, fields
    index, profile, salt, status, model, zone, attractor, stats = map(int, fields[1:])
    exterior = profile % 4 != 0
    write(0xbffbe4, 'I', not exterior)
    write(0x945cbc, 'i', 21 if profile % 5 == 0 else 20)
    rain_bits = 0 if profile % 3 == 0 else 0x3dcccccc if profile % 3 == 1 else 0x3dcccccd
    write(0xd0d074, 'I', rain_bits)
    for slot in range(8):
        identity = -1 if (slot + salt) % 11 == 0 else 10 + slot
        write(0x95c7e8 + slot * 4, 'i', identity)
        if identity < 0:
            continue
        write(0x95c8b0 + identity * 20, 'B', (slot + salt) % 5 != 0)
        info = 0x30a0000 + slot * 0x100
        write(0xb12818 + identity * 4, 'I', info)
        write(info + 8, 'h', (slot * 3 + salt) % 9)
        write(info + 0x24, 'i', (slot + salt) % 3)
        write(info + 0x28, 'i', 17 if (slot + salt) % 4 == 0 else 4 if slot % 2 == 0 else 5)
        write(info + 0x2c, 'i', 38 if slot % 3 == 0 else 39 if slot % 3 == 1 else 2)
        write(info + 0x30, 'H', 0x1000 if (slot + salt) % 2 == 0 else 0)
    uc.mem_write(0x30b0000, b'ATM\0')
    stack = 0x3080000
    write(stack, '10i', 0x30e0000,
          profile % 6 == 1 or profile == 19,
          profile % 6 == 2 or profile == 19,
          1 if profile % 7 == 2 else -1,
          12 if profile % 7 == 4 else -1,
          3 if profile % 6 == 4 else -1,
          profile % 4 == 1, profile % 3 != 0, profile % 5 == 2, 0x30b0000)
    state = dict(profile=profile, zone=0, attractor=0, stats=0)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x630b50, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('instruction cap', index)
    result = uc.reg_read(UC_X86_REG_EAX)
    result = result if result < 0x80000000 else result - 0x100000000
    actual = result, state['zone'], state['attractor'], state['stats']
    expected = model if status == 0 else -1, zone, attractor, stats
    assert actual == expected, ('occupation', index, actual, expected)
    checks += 1
print('civilian-occupation-retail-oracle-ok checks=' + str(checks),
      'slots=8 reference-passes=3,5,7 policies=explicit-fixtures census=incomplete',
      'function-sha256=' + function_hash)
print(native.stdout.splitlines()[-1])
