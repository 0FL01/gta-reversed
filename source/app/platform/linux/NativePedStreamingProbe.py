"""Development-only original ped-stream request oracle; no runtime PE dependency.

Source groups/races/requested slots are explicit fixtures. Loaded assets, model
references, actors and census completeness are not certified by this selector.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (UC_X86_REG_EAX, UC_X86_REG_EIP, UC_X86_REG_ESP,
    UC_X86_REG_FPCW, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EDX)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
game = Path(args.game_dir).resolve()
output = root / 'artifacts/graphics'
assert output.is_dir()
stem = 'NativePedStreamingProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_streaming_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-ffunction-sections', '-fdata-sections', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedStreamingProbe', 'NativePedStreaming', 'NativeSourceRng')]
    command += ['-Wl,--gc-sections', '-pthread', '-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)

assert (game / 'gta-sa.exe').stat().st_size == 5971456
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
function_hash = hashlib.sha256(pe.get_data(0x62d9c0 - 0x400000, 0x190)).hexdigest()
assert function_hash == 'd11830a567b5e1331e7bca80c2a86be5a6a9f82a04037d2cd3834a21d46876c8'
slot_hash = hashlib.sha256(pe.get_data(0x40ce10 - 0x400000, 0x250)).hexdigest()
assert slot_hash == '555539fe76c00e7543195218708300a680889d2cfd8e47d9ebe398cbbc629d27'
translation = pe.get_data(0x945cc0 - 0x400000, 33 * 3 * 4)
assert hashlib.sha256(translation).hexdigest() == 'd4649a51706637c3136cf70d24b1a00d68a766f5958b3f6d0d6c0ac755e55900'
# Compare authored native ordinals, not a duplicate handwritten reference table.
translation_program = '''#include "app/platform/linux/NativePedStreaming.h"
#include <cstdio>
int main() {
    for (const auto& row : NativePedGroupTranslation)
        for (const auto value : row) std::printf("%u ", unsigned(value));
}
'''
translation_binary = output / 'NativePedStreaming-translation'
subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
    '-I' + str(root / 'gta-reversed/source'), '-x', 'c++', '-', '-o', str(translation_binary)],
    input=translation_program, text=True, capture_output=True, check=True)
ordinals = tuple(map(int, subprocess.check_output([str(translation_binary)], text=True).split()))
assert ordinals == struct.unpack('<99I', translation), 'ped-group translation ordinal/order changed'

image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
uc.mem_write(0x853e0c, b'\xc3')
# Test stub for CRT x87 truncation; original selection arithmetic remains intact.
uc.mem_write(0x853e30, bytes.fromhex(
    '83ec0c d93c24 668b0424 660d000c 6689442402 d96c2402 '
    'df7c2404 d92c24 8b442404 8b542408 83c40c c3'))

def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))

state = {}
def random_draw(machine, address, size, _):
    if address == 0x853e0c:
        state['value'] = (state['value'] * 214013 + 2531011) & 0xffffffff
        state['draws'] += 1
        machine.reg_write(UC_X86_REG_EAX, (state['value'] >> 16) & 32767)
    elif state.get('slots_mode'):
        slot = (machine.reg_read(UC_X86_REG_ESI) - 0x95c7e8) // 4
        if address == 0x40ce40:
            state['events'].append((0, slot, machine.reg_read(UC_X86_REG_EDI)))
        elif address in (0x40cea2, 0x40cf3e):
            state['events'].append((2, slot, machine.reg_read(UC_X86_REG_EDX)))
        elif address in (0x40a060, 0x408b00):
            stack = machine.reg_read(UC_X86_REG_ESP)
            model = struct.unpack('<i', machine.mem_read(stack + 4, 4))[0]
            state['events'].append((0 if address == 0x40a060 else 1, slot, model))
            if address == 0x408b00:
                assert struct.unpack('<I', machine.mem_read(stack + 8, 4))[0] == 8, 'source KEEP_IN_MEMORY flag'

uc.hook_add(UC_HOOK_CODE, random_draw)
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'CASE':
        continue
    assert len(fields) == 27, fields
    seed, profile, count, distribution, status, model, rng_state, draws, *expected_cursors = map(int, fields[1:])
    percentages = [6] * 18
    if distribution == 1: percentages = [0] * 17 + [100]
    if distribution == 2: percentages = [1, 99] + [0] * 16
    race_mask = 0 if profile == 1 else 4 if profile == 2 else 15
    write(0xc98fd8, 'I', 0x30a0000)
    write(0x30a000f, 'B', 0)
    write(0x30a0010, 'B', race_mask)
    write(0xc98fe0, 'I', 0)
    write(0xc98fe4, 'I', 0)
    write(0xc98fe8, '18B', *percentages)
    write(0xc9d014, 'I', 0)
    write(0x95c7e8, '8i', *([10 + 17 * 21 + j for j in range(8)] if profile == 3 else [-1] * 8))
    for group in range(18):
        write(0x945cc0 + group * 12, '3I', group, group, group)
        write(0xc9c018 + group * 2, 'h', count)
        write(0x95c7a0 + group * 4, 'i', ((seed + group) & 0xffffffff) % 23)
        for j in range(count):
            ident = 10 + group * 21 + j
            address = 0x30b0000 + ident * 0x40
            write(0xc9c6b0 + (group * 21 + j) * 2, 'H', ident)
            write(0xb12818 + ident * 4, 'I', address)
            write(address + 0x3a, 'B', 1 if profile == 1 else (j + group) % 5)
    state = dict(value=seed, draws=0)
    stack = 0x3080000
    write(stack, 'I', 0x30e0000)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x62d9c0, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('instruction cap', fields)
    actual_model = uc.reg_read(UC_X86_REG_EAX)
    if actual_model >= 0x80000000: actual_model -= 0x100000000
    cursors = list(struct.unpack('<18i', uc.mem_read(0x95c7a0, 18 * 4)))
    actual = (actual_model, state['value'], state['draws'], cursors)
    expected = (model if status == 0 else -1, rng_state, draws, expected_cursors)
    assert actual == expected, ('ped-stream-request/cursors/RNG', fields, actual, expected)
    checks += 1
assert checks == 3072
uc.mem_write(0x40a060, b'\xc3')
uc.mem_write(0x408b00, b'\xc3')
slot_checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'SLOTS':
        continue
    index, count, effect_count, *values = map(int, fields[1:])
    assert len(values) == 8 + effect_count * 3
    slots = [10 + i if index & (1 << i) else -1 for i in range(8)]
    requests = []
    for i in range(8):
        mode = (index + i) % 5
        requests.append(-1 if mode == 0 else -2 if mode == 1 else
            (slots[i] if slots[i] >= 0 else 10) if mode == 2 else 10 if mode == 3 else -3)
    write(0x95c7e8, '8i', *slots)
    write(0x95c79c, 'I', sum(model >= 0 for model in slots))
    write(0x30c0000, '8i', *requests)
    for model in range(10, 18):
        header = 0x30b0000 + model * 0x40
        write(0xb12818 + model * 4, 'I', header)
        write(header + 0xa, 'h', 42)
        # Explicit GAME_REQUIRED fixture skips internal asset queue mutation;
        # all original slot, TXD-index and request-effect ordering remains live.
        write(0x95c8a6 + model * 20, 'B', 6)
    write(0x95c8a6 + 20042 * 20, 'B', 6)
    state = dict(slots_mode=True, events=[])
    stack = 0x3080000
    write(stack, '2I', 0x30e0000, 0x30c0000)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.emu_start(0x40ce10, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('slot instruction cap', fields)
    actual_slots = list(struct.unpack('<8i', uc.mem_read(0x95c7e8, 32)))
    actual_count = struct.unpack('<I', uc.mem_read(0x95c79c, 4))[0]
    expected_events = []
    for offset in range(8, len(values), 3):
        event = tuple(values[offset:offset + 3])
        expected_events.append(event)
        if event[0] == 0: expected_events.append((2, event[1], 20042))
    assert (actual_slots, actual_count, state['events']) == (values[:8], count, expected_events), (
        'requested-slot identities/count/ordered model+TXD+KEEP effects', fields,
        actual_slots, actual_count, state['events'], expected_events)
    slot_checks += 1
assert slot_checks == 256
print('ped-streaming-retail-oracle-ok checks=' + str(checks),
      'group-boundary=strict-less fraction=rand/32768 cursors=preincrement translation=99',
      'slot-plans=' + str(slot_checks), 'slots=requested-fixtures census=incomplete',
      'function-sha256=' + function_hash, 'slot-function-sha256=' + slot_hash)
print(native.stdout.splitlines()[-1])
