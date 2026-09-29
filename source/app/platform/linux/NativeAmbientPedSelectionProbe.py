"""Isolated development-only retail selector oracle, not an application dependency.

The exact original selector reads controlled count/zone/model fixtures here.
Police/gang/civilian model helpers are explicit observations on both sides;
this gate does not certify real loaded-ped authorities or an ambient census.
No executable/media bytes are copied to disk or redistributed.
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
stem = 'NativeAmbientPedSelectionProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ambient_ped_selection_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-I' + str(source), '-pthread']
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativeAmbientPedSelectionProbe', 'NativeAmbientPedSelection', 'NativeSourceRng')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)

assert (game / 'gta-sa.exe').stat().st_size == 5971456
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
function_hash = hashlib.sha256(pe.get_data(0x62d4d0 - 0x400000, 0x4e6)).hexdigest()
assert function_hash == 'a2912c529b1fd5d70ea13b8db8f92fdf60e89db40ea27bae2810b5c814f7e632', 'retail selector changed'
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)

def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))

def read_int(address):
    return struct.unpack('<i', uc.mem_read(address, 4))[0]

for address in (0x853e0c, 0x446950, 0x62e920, 0x62d160, 0x62ef20, 0x630b50):
    uc.mem_write(address, b'\xc3')
uc.mem_write(0x853e30, bytes.fromhex(
    '83ec0c d93c24 668b0424 660d000c 6689442402 d96c2402 '
    'df7c2404 d92c24 8b442404 8b542408 83c40c c3'))
state = {}

def hook(machine, address, size, _):
    if address == 0x853e0c:
        state['rng'] = (state['rng'] * 214013 + 2531011) & 0xffffffff
        state['draws'] += 1
        machine.reg_write(UC_X86_REG_EAX, (state['rng'] >> 16) & 0x7fff)
    elif address == 0x446950:
        machine.reg_write(UC_X86_REG_EAX, int(state['profile'] == 12))
    elif address == 0x62e920:
        state['police'] += 1
        machine.reg_write(UC_X86_REG_EAX, 280)
    elif address == 0x62d160:
        state['gang'] += 1
        machine.reg_write(UC_X86_REG_EAX, 0 if state['mode'] == 2 else 7)
    elif address == 0x62ef20:
        machine.reg_write(UC_X86_REG_EAX, 0xffffffff if state['mode'] == 1 else 102)
    elif address == 0x630b50:
        state['civilian'] += 1
        machine.reg_write(UC_X86_REG_EAX,
                          0xffffffff if state['mode'] == 3 else 7 if state['mode'] == 4 else 9)

uc.hook_add(UC_HOOK_CODE, hook)
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'SELECTION':
        continue
    assert len(fields) == 13, fields
    index, seed, profile, mode, status, ped, model, draws, rng, police, gang, civilian = map(int, fields[1:])
    state = dict(rng=seed, draws=0, profile=profile, mode=mode, police=0, gang=0, civilian=0)
    write(0xc98fd8, 'I', 0 if profile == 0 else 0x30a0000)
    write(0x30a000f, 'B', 0x80 if profile == 11 else 0)
    write(0xc9d00e, 'B', profile == 9)
    write(0xc9d00b, 'B', profile in (6, 7))
    write(0xc9d00a, 'B', 0)
    write(0xc9d00c, 'B', 0)
    write(0xbffbe4, 'I', profile == 8)
    write(0xc98fb0, 'f', 0 if profile == 1 else 20 if profile == 2 else 4)
    write(0xc98fb4, 'f', 0 if profile == 1 else 20 if profile == 3 else 4)
    write(0xc9bce8, 'f', 0 if profile == 1 else 20 if profile in (4, 13) else 4)
    write(0xc98fb8, 'f', 0 if profile == 1 else 20 if profile == 5 else 4)
    if profile >= 14:
        targets = [2.0, 1.0, 1.0, 1.0]
        if profile == 14:
            targets = [3.5, 2.999, 2.125, 2.75]
        elif profile == 15:
            targets[0] += 2.0
        elif profile == 16:
            targets = [4.0, 3.0, 3.0, 3.0]
        else:
            targets[2] += 1.5
        for address, target in zip((0xc98fb0, 0xc98fb4, 0xc9bce8, 0xc98fb8), targets):
            write(address, 'f', target)
    write(0xc9bf90, 'i', 1)
    write(0xc9bf94, '10i', 1, *([0] * 9))
    write(0xc9bfc0, 'i', 1)
    write(0xc9bfc4, '2i', 1, 1)
    write(0x945e10, 'I', 5)
    write(0xc9c018 + 5 * 2, 'H', 0 if profile == 13 else 3)
    write(0xc9c6b0 + 5 * 21 * 2, '3H', 28, 29, 30)
    for identity, loaded in ((28, 1), (29, 0), (30, 1)):
        write(0x95c8b0 + identity * 20, 'B', loaded)
    write(0xb12818 + 9 * 4, 'I', 0x30a0100)
    write(0x30a0100 + 0x28, 'i', 4)
    ped_address, model_address = 0x30c0000, 0x30c0010
    write(ped_address, 'i', 31)
    write(model_address, 'i', 123)
    stack = 0x3080000
    write(stack, '5I', 0x30e0000, ped_address, model_address, profile == 7, profile == 10)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x62d4d0, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('instruction cap', index)
    selected = bool(uc.reg_read(UC_X86_REG_EAX) & 0xff)
    actual = (selected, state['draws'], state['rng'], state['police'], state['gang'], state['civilian'])
    expected = (status == 0, draws, rng, police, gang, civilian)
    assert actual == expected, ('flow', index, actual, expected)
    if selected:
        assert (read_int(ped_address), read_int(model_address)) == (ped, model), ('selection', index)
    checks += 1
print('ambient-ped-selection-retail-oracle-ok checks=' + str(checks),
      'reference=isolated-development model-helpers=explicit-fixtures census=incomplete x87-control=0x37f',
      'function-sha256=' + function_hash)
print(native.stdout.splitlines()[-1])
