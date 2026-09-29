"""Read-only isolated original model-policy oracle; model/zone values are fixtures.

No executable bytes are copied to disk or used by the native application.
The three original policy bodies and attractor tables are hash-pinned; the CRT
case-insensitive string comparison and explicit cheat state are test adapters.
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
stem = 'NativePedModelPoliciesProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_model_policies_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedModelPoliciesProbe', 'NativePedModelPolicies')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary), '--cases'], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)

assert (game / 'gta-sa.exe').stat().st_size == 5971456
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
for start, size, expected in (
    (0x62e100, 0x48, '14bdfb88967f44dd971c10ea2548e97ca57a4e8f1ccf4ea8ec97490dae7fb1ee'),
    (0x62e930, 0x167, 'b82f8032e2849f236dd24e3d2514bdff9d50dfeda754de798cf13a319328a450'),
    (0x62eac0, 0x359, '08373f41184791445d0698eb8f473bca0454e011ee6f76ee2dbdf72736105d0e'),
):
    assert hashlib.sha256(pe.get_data(start - 0x400000, size)).hexdigest() == expected
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)


def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))


def read_int(address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]


def source_string(address):
    result = bytearray()
    for index in range(256):
        byte = uc.mem_read(address + index, 1)[0]
        if not byte:
            return bytes(result).lower()
        result.append(byte)
    raise AssertionError('unterminated source fixture string')


state = {}
# Only unrelated CRT/cheat helper bodies are adapted; policy code/table bytes stay intact.
uc.mem_write(0x4075e0, b'\xc3')
uc.mem_write(0x8559f4, b'\xc3')


def observe(machine, address, size, _):
    if address == 0x4075e0:
        machine.reg_write(UC_X86_REG_EAX, state['cheat'])
    elif address == 0x8559f4:
        stack = machine.reg_read(UC_X86_REG_ESP)
        equal = source_string(read_int(stack + 4)) == source_string(read_int(stack + 8))
        machine.reg_write(UC_X86_REG_EAX, 0 if equal else 1)


uc.hook_add(UC_HOOK_CODE, observe)
names = ['COPSIT', 'coplook', 'BROWSE', 'dancer', 'BARGUY', 'pedroul', 'PEDCARD',
         'PEDSLOT', 'STRIPW', 'stripm', 'ATM', '', 'UNKNOWN']
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] not in ('STATS', 'ZONE', 'ATTRACTOR'):
        continue
    values = list(map(int, fields[1:]))
    if fields[0] == 'STATS':
        first, second, expected = values
        start, arguments = 0x62e930, [first, second]
    elif fields[0] == 'ZONE':
        has_zone, cheat, race, mask, expected = values
        state['cheat'] = cheat
        write(0xc98fd8, 'I', 0x30a0000 if has_zone else 0)
        write(0x30a0010, 'B', mask)
        write(0xb12818 + 7 * 4, 'I', 0x30b0000)
        write(0x30b003a, 'B', race)
        start, arguments = 0x62e100, [7]
    else:
        model, ped_type, name, expected = values
        write(0xb12818 + model * 4, 'I', 0x30b0000)
        write(0x30b0028, 'i', ped_type)
        uc.mem_write(0x30c0000, names[name].encode('ascii') + b'\0')
        start, arguments = 0x62eac0, [model, 0x30c0000]
    stack = 0x3080000
    write(stack, 'I', 0x30e0000)
    for index, value in enumerate(arguments):
        write(stack + 4 + index * 4, 'I', value & 0xffffffff)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(start, 0x30e0000, count=10000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('instruction cap', fields)
    actual = uc.reg_read(UC_X86_REG_EAX) & 0xff
    assert actual == expected, ('model-policy', fields, actual)
    checks += 1
assert checks == 31392, checks
print('ped-model-policies-retail-oracle-ok checks=' + str(checks),
      'functions=zone,stats,attractor model-zone=explicit-fixtures census=incomplete')
print(native.stdout.splitlines()[-1])
