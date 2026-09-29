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
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
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
zone_hash = hashlib.sha256(pe.get_data(0x40b0e0 - 0x400000, 0x54b)).hexdigest()
assert zone_hash == 'd50fc51ffb16c70dd2a674c9fbf05536b64ab2f1c9af41122b8700e1b0eb0562'
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
    elif state.get('gang_mode'):
        if address == 0x4075e0:
            machine.reg_write(UC_X86_REG_EAX, int(state['cheat']))
        elif address == 0x408b00:
            stack = machine.reg_read(UC_X86_REG_ESP)
            model, flags = struct.unpack('<iI', machine.mem_read(stack + 4, 8))
            assert flags == 2, 'source gang GAME_REQUIRED-only flag'
            state['events'].append((1, model))
    elif state.get('zone_mode'):
        if address == 0x40b222:
            # Stop ONLY after the civilian phase, before the gang timer/tail.
            machine.emu_stop()
        elif address == 0x4075e0:
            machine.reg_write(UC_X86_REG_EAX, int(state['cheat']))
        elif address in (0x40a060, 0x408b00):
            stack = machine.reg_read(UC_X86_REG_ESP)
            model = struct.unpack('<i', machine.mem_read(stack + 4, 4))[0]
            state['events'].append((0 if address == 0x40a060 else 1, model))
            if address == 0x408b00:
                assert struct.unpack('<I', machine.mem_read(stack + 8, 4))[0] == 10, 'source KEEP|GAME flags'

def zone_flags(machine, access, address, size, value, _):
    instruction = machine.reg_read(UC_X86_REG_EIP)
    if state.get('gang_mode'):
        if instruction in (0x40b46a, 0x40b524):
            assert size == 1 and (address - 0x95c8a6) % 20 == 0
            state['events'].append((0 if instruction == 0x40b46a else 3,
                                    (address - 0x95c8a6) // 20))
        return
    if not state.get('zone_mode'):
        return
    if instruction not in (0x40b148, 0x40b1e6, 0x40b2cb, 0x40b2f1, 0x40b34a):
        return
    assert size == 1 and (address - 0x95c8a6) % 20 == 0
    model = (address - 0x95c8a6) // 20
    kind = 3 if instruction in (0x40b148, 0x40b34a) else 0 if instruction == 0x40b2f1 else 2
    state['events'].append((kind, model))

uc.hook_add(UC_HOOK_CODE, random_draw)
uc.hook_add(UC_HOOK_MEM_WRITE, zone_flags)
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
uc.mem_write(0x4075e0, b'\xc3')
zone_checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'ZONE':
        continue
    index, mode, seed, count, zone_type, timer, rng_state, draws, effect_count, *values = map(int, fields[1:])
    assert len(values) == 26 + effect_count * 2
    initial_slots = [(367 if index & 1 else 10) + j if index & (1 << j) else -1 for j in range(8)]
    group_count = (0, 1, 8, 21)[index % 4]
    write(0xc98fd8, 'I', 0 if mode == 4 else 0x30a0000)
    write(0x30a000f, 'B', 0xa0)
    write(0x30a0010, 'B', 15)
    write(0xc98fe0, 'I', 0)
    write(0xc98fe4, 'I', 0)
    write(0xc98fe8, '18B', *([0] * 17 + [100]))
    write(0xc9d014, 'I', 0)
    write(0x95c7e8, '8i', *initial_slots)
    write(0x95c79c, 'I', sum(model >= 0 for model in initial_slots))
    write(0x95c808, 'i', -1 if mode == 0 else 0)
    write(0x9dd0a8, 'i', 0 if mode == 1 else 299 if mode == 6 else -1)
    for group in range(18):
        write(0x945cc0 + group * 12, '3I', group, group, group)
        write(0xc9c018 + group * 2, 'h', group_count)
        write(0x95c7a0 + group * 4, 'i', 0)
        for j in range(group_count):
            model = 10 + group * 21 + j
            header = 0x30b0000 + model * 0x40
            write(0xc9c6b0 + (group * 21 + j) * 2, 'H', model)
            write(0xb12818 + model * 4, 'I', header)
            write(header + 0x3a, 'B', (j + group) % 5)
            write(header + 0xa, 'h', 42)
            write(0x95c8a6 + model * 20, 'B', 6)
    for j, model in enumerate(initial_slots):
        if model < 0:
            continue
        header = 0x30b0000 + model * 0x40
        write(0xb12818 + model * 4, 'I', header)
        write(header + 8, 'H', int(mode == 3 or (mode == 7 and j < 3)))
        write(header + 0xa, 'h', 42)
        write(0x95c8a6 + model * 20, 'B', 6)
    write(0x95c8a6 + 20042 * 20, 'B', 6)
    state = dict(zone_mode=True, cheat=mode == 5, value=seed, draws=0, events=[])
    stack = 0x3080000
    write(stack, '2I', 0x30e0000, 0)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x40b0e0, 0x30e0000, count=500000)
    assert uc.reg_read(UC_X86_REG_EIP) in (0x40b222, 0x30e0000), ('zone-phase instruction cap', fields)
    actual_slots = list(struct.unpack('<8i', uc.mem_read(0x95c7e8, 32)))
    actual_count = struct.unpack('<I', uc.mem_read(0x95c79c, 4))[0]
    actual_zone = struct.unpack('<i', uc.mem_read(0x95c808, 4))[0]
    actual_timer = struct.unpack('<i', uc.mem_read(0x9dd0a8, 4))[0]
    actual_cursors = list(struct.unpack('<18i', uc.mem_read(0x95c7a0, 72)))
    expected_events = []
    for offset in range(26, len(values), 2):
        event = tuple(values[offset:offset + 2])
        expected_events.append(event)
        if event[0] == 0:
            expected_events.append((3, 20042))
    actual = (actual_slots, actual_count, actual_zone, actual_timer, actual_cursors,
              state['value'], state['draws'], state['events'])
    expected = (values[:8], count, zone_type, timer, values[8:26], rng_state, draws, expected_events)
    assert actual == expected, ('civilian zone phase slots/count/zone/timer/cursors/RNG/ordered effects', fields, actual, expected)
    zone_checks += 1
assert zone_checks == 2048
gang_checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'GANG':
        continue
    mode, mask, count, current, timer, expected_current, expected_timer, effect_count, *effects = map(int, fields[1:])
    assert len(effects) == effect_count * 2
    # Run the COMPLETE original call with a waiting civilian timer, rather than
    # entering the tail with guessed register/stack state. No RNG is expected.
    write(0xc98fd8, 'I', 0 if mode == 1 else 0x30a0000)
    write(0x30a000f, 'B', 0)
    write(0x95c808, 'i', 0)
    write(0x95c79c, 'I', 0)
    write(0x95c7e8, '8i', *([-1] * 8))
    write(0x9dd0a8, 'i', 0)
    write(0x9dd0ac, 'i', timer)
    write(0x9dd0b0, 'i', current)
    write(0x95c798, 'H', mask)
    for gang in range(10):
        group = ordinals[(gang + 18) * 3]
        write(0xc9c018 + group * 2, 'h', count)
        for slot in range(count):
            model = 10 + gang * 21 + slot
            header = 0x30b0000 + model * 0x40
            write(0xc9c6b0 + (group * 21 + slot) * 2, 'H', model)
            write(0xb12818 + model * 4, 'I', header)
            write(header + 0xa, 'h', 42)
            # Explicit flags suppress external queues, not parser-completed
            # assets. Original model/TXD GAME_REQUIRED clears are still run.
            write(0x95c8a6 + model * 20, 'B', 6)
    write(0x95c8a6 + 20042 * 20, 'B', 6)
    state = dict(gang_mode=True, cheat=mode == 2, value=1792, draws=0, events=[])
    stack = 0x3080000
    write(stack, '2I', 0x30e0000, 0)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x40b0e0, 0x30e0000, count=500000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('gang-phase instruction cap', fields)
    actual_timer, actual_current = struct.unpack('<2i', uc.mem_read(0x9dd0ac, 8))
    expected_events = []
    for offset in range(0, len(effects), 2):
        event = tuple(effects[offset:offset + 2])
        expected_events.append(event)
        if event[0] == 0:
            expected_events.append((3, 20042))
    assert (actual_current, actual_timer, state['events'], state['value'], state['draws']) == (
        expected_current, expected_timer, expected_events, 1792, 0), ('gang state/intents/no-RNG', fields, state)
    assert list(struct.unpack('<8i', uc.mem_read(0x95c7e8, 32))) == [-1] * 8
    assert struct.unpack('<i', uc.mem_read(0x9dd0a8, 4))[0] == (-1 if mode == 0 else 0)
    gang_checks += 1
assert gang_checks == 4328
print('ped-streaming-retail-oracle-ok checks=' + str(checks),
      'group-boundary=strict-less fraction=rand/32768 cursors=preincrement translation=99',
      'slot-plans=' + str(slot_checks), 'zone-phases=' + str(zone_checks),
      'zone-change-timer=299 replacement-timer=300 gang-phases=' + str(gang_checks),
      'slots=requested-fixtures census=incomplete', 'function-sha256=' + function_hash,
      'slot-function-sha256=' + slot_hash, 'zone-function-sha256=' + zone_hash)
print(native.stdout.splitlines()[-1])
