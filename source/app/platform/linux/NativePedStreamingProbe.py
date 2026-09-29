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
    UC_X86_REG_FPCW, UC_X86_REG_ESI, UC_X86_REG_EDI, UC_X86_REG_EDX, UC_X86_REG_EBP)

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
mask_hash = hashlib.sha256(pe.get_data(0x40b630 - 0x400000, 0x5df)).hexdigest()
assert mask_hash == '0ebf5bdece38676c2e0f30498bed9c50572ed4998804fa8b54737ad4bdd46ca3'
count_hash = hashlib.sha256(pe.get_data(0x62f660 - 0x400000, 0x13)).hexdigest()
assert count_hash == 'ceeb1af1a1f9de70fe643654ad33bf7af354e533ded599fa518c7f587da0e814'
war_hash = hashlib.sha256(pe.get_data(0x446c00 - 0x400000, 0x76)).hexdigest()
assert war_hash == 'fee5d80169ea5e16192146be6982222db3d6ae9e2c05a744be5a396b072c53cb'
startup_ranges = ((0x446780, 0x24, '82bac8089a0819c0c3a3029fe2027924f61f55eec360fff78581873ec149ea00'),
    (0x5d3989, 0x99, '55aca3278c7cf6d070770472e13d609652cbb05b0d34db0b11fb8cf91d599b92'),
    (0x5d3a49, 0x22, 'e4dc76ff3bcd153c4edde78047fee8d0ba6f8bba3ab4261ca43c8b641811ebff'))
for address, length, expected_hash in startup_ranges:
    assert hashlib.sha256(pe.get_data(address - 0x400000, length)).hexdigest() == expected_hash
update_prefix_hash = hashlib.sha256(pe.get_data(0x449510 - 0x400000, 0x6e)).hexdigest()
assert update_prefix_hash == '85873e89aa4ac267d73b9cc099b424ee27a1a973d4efaf0832c8fbecb70f277b'
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
    elif state.get('war_update_mode'):
        if address == 0x469de0:
            machine.reg_write(UC_X86_REG_EAX, int(state['mission']))
            state['mission_calls'] += 1
        elif address == 0x4493d0:
            stack = machine.reg_read(UC_X86_REG_ESP)
            assert struct.unpack('<I', machine.mem_read(stack + 4, 4))[0] == 1
            state['events'].append(0)
        elif address == 0x446ce0:
            state['events'].append(1)
        elif address == 0x444000:
            machine.reg_write(UC_X86_REG_EAX, int(state['coop']))
            state['coop_calls'] += 1
        elif address == 0x449546:
            state['events'].append(3)
        elif address in (0x44957e, 0x449e29):
            if address == 0x44957e:
                state['events'].append(2)
            machine.emu_stop()
    elif state.get('demand_mode'):
        if address == 0x560ce0:
            # Player position is an explicit owner observation, not an actor
            # constructor/default position. Original helper arithmetic runs.
            stack = machine.reg_read(UC_X86_REG_ESP)
            pointer, player = struct.unpack('<Ii', machine.mem_read(stack + 4, 8))
            assert player == -1
            machine.mem_write(pointer, struct.pack('<3I', *state['player'], 0))
            machine.reg_write(UC_X86_REG_EAX, pointer)
            state['player_calls'] += 1
        elif address == 0x40b6d1:
            pointer = machine.reg_read(UC_X86_REG_EBP) - 8
            state['wanted'] = struct.unpack('<I', machine.mem_read(pointer, 4))[0]
            machine.emu_stop()
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
    elif state.get('mask_mode'):
        if address == 0x446c00:
            # Explicit external GangWars observation. The demand controller is
            # not owned by this request kernel; no hidden no-attack fallback.
            stack = machine.reg_read(UC_X86_REG_ESP)
            pointer = struct.unpack('<I', machine.mem_read(stack + 4, 4))[0]
            wanted = struct.unpack('<I', machine.mem_read(pointer, 4))[0]
            write(pointer, 'I', wanted | state['war_extra'])
        elif address == 0x408b00:
            stack = machine.reg_read(UC_X86_REG_ESP)
            model, flags = struct.unpack('<iI', machine.mem_read(stack + 4, 8))
            assert flags == 8, 'source mask transition KEEP_IN_MEMORY flag'
            state['events'].append((1, model))
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
    if state.get('mask_mode'):
        if instruction in (0x40b7cc, 0x40b884):
            assert size == 1 and (address - 0x95c8a6) % 20 == 0
            state['events'].append((0 if instruction == 0x40b7cc else 3,
                                    (address - 0x95c8a6) // 20))
        return
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
mask_checks = 0
uc.mem_write(0x446c00, b'\xc3')  # External GangWars demand observation, hooked above.
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'MASK':
        continue
    (base, profile, ped_count, car_count, current, seed, peds_before, cars_before,
     peds_after, cars_after, draws, rng_state, effect_count, *effects) = map(int, fields[1:])
    assert len(effects) == effect_count * 2
    write(0xc98fd8, 'I', 0 if profile == 4 else 0x30a0000)
    write(0x30a0000, '10B', *[255 if base & (1 << gang) else 0 for gang in range(10)])
    write(0x9e0c83, 'B', int(profile == 5))
    write(0x95c798, 'H', peds_before)
    write(0x95c794, 'H', cars_before)
    write(0x9dd0b0, 'i', current)
    for gang in range(10):
        group = ordinals[(gang + 18) * 3]
        write(0xc9c018 + group * 2, 'h', ped_count)
        for slot in range(ped_count):
            model = 10 + gang * 21 + slot
            header = 0x30b0000 + model * 0x40
            write(0xc9c6b0 + (group * 21 + slot) * 2, 'H', model)
            write(0xb12818 + model * 4, 'I', header)
            write(header + 0xa, 'h', 42)
            write(0x95c8a6 + model * 20, 'B', 6)
        loaded_count = 23 if profile == 2 else int(profile == 1 and gang % 3 == 0)
        # Original CountMembers stops at a NEGATIVE model, not upstream 2000.
        # Run that pinned routine itself with a stable explicit loaded-group
        # snapshot. Requests are mocked intents and never mutate these counts.
        members = [500 + gang * 23 + slot for slot in range(loaded_count)] + [-1] * (23 - loaded_count)
        write(0xc9bd80 + gang * 46, '23h', *members)
        write(0xc9bff4 + gang * 2, 'h', car_count)
        for slot in range(car_count):
            model = 500 + gang * 23 + slot
            loaded = profile == 3 or (profile == 1 and (gang + slot) % 2 == 0)
            write(0xc9c3cc + (gang * 23 + slot) * 2, 'H', model)
            write(0x95c8b0 + model * 20, 'B', int(loaded))
    write(0x95c8a6 + 20042 * 20, 'B', 6)
    state = dict(mask_mode=True, war_extra=512 if profile == 5 else 0, value=seed, draws=0, events=[])
    stack = 0x3080000
    write(stack, '2I', 0x30e0000, 0)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x40b630, 0x30e0000, count=500000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, ('gang-mask instruction cap', fields)
    actual_peds = struct.unpack('<H', uc.mem_read(0x95c798, 2))[0]
    actual_cars = struct.unpack('<H', uc.mem_read(0x95c794, 2))[0]
    expected_events = []
    for offset in range(0, len(effects), 2):
        event = tuple(effects[offset:offset + 2])
        expected_events.append(event)
        if event[0] == 0:
            expected_events.append((3, 20042))
    actual = actual_peds, actual_cars, state['value'], state['draws'], state['events']
    expected = peds_after, cars_after, rng_state, draws, expected_events
    assert actual == expected, ('gang masks/ped-car interleave/CRT draws/ordered intents', fields, actual, expected)
    assert struct.unpack('<i', uc.mem_read(0x9dd0b0, 4))[0] == current
    mask_checks += 1
assert mask_checks == 6481
demand_checks = 0
# Restore actual GangWars helper after the earlier external-demand fixtures.
uc.mem_write(0x446c00, pe.get_data(0x446c00 - 0x400000, 0x76))
uc.ctl_remove_cache(0x446c00, 0x446c76)
uc.mem_write(0x560ce0, b'\xc3')
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'DEMAND':
        continue
    (mask, cheat, attack_state, gang, has_zone, px, py, ax, ay, expected) = map(int, fields[1:])
    write(0xc98fd8, 'I', 0x30a0000 if has_zone else 0)
    write(0x30a0000, '10B', *[1 + (mask + i) % 255 if mask & (1 << i) else 0 for i in range(10)])
    write(0x9e0c83, 'B', cheat)
    write(0x9e25ec, 'i', attack_state)
    write(0x9e2624, 'i', gang)
    write(0x9e2654, '3I', ax, ay, 0)
    state = dict(demand_mode=True, player=(px, py), player_calls=0, value=1792, draws=0)
    stack = 0x3080000
    write(stack, 'I', 0x30e0000)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x40b630, 0x30e0000, count=100000)
    assert uc.reg_read(UC_X86_REG_EIP) == (0x40b6d1 if has_zone else 0x30e0000), ('demand cap', fields)
    assert state.get('wanted', 0) == expected, ('source demand/float-spill distance', fields, state)
    assert state['player_calls'] == int(has_zone and attack_state != 0), ('source player guard', fields)
    assert state['value'] == 1792 and state['draws'] == 0, 'demand has no RNG'
    demand_checks += 1
assert demand_checks == 6625
startup_checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'STARTUP':
        continue
    (seed, timer, count, zone, timer_after, peds, offensive, attack_state, zones, provocation,
        active, mission, ax, ay, gang) = map(int, fields[1:])
    write(0x95c79c, 'I', seed % 9)
    write(0x95c7e8, '8i', *[10 + seed] * 8)
    write(0x95c7a0, '18i', *[21 + seed] * 18)
    write(0x95c808, 'i', seed % 32)
    write(0x95c798, 'H', seed * 1023 & 0xffff)
    cars_before, gang_timer, member = seed * 7757 & 0xffff, 17 + seed, seed % 21
    write(0x95c794, 'H', cars_before)
    write(0x9dd0a8, '3i', timer, gang_timer, member)
    write(0x9e2604, 'i', 1 + seed % 7)
    write(0x9e25ec, 'i', 1 + seed % 2)
    write(0x9e2634, 'i', 6)
    write(0x9e25fc, 'f', seed + 1)
    write(0x9e2630, 'B', 1)
    write(0x9e2640, 'B', 1)
    write(0x9e2654, '2I', ax, ay)
    write(0x9e2624, 'i', gang)
    state = dict(value=1792, draws=0)
    for start, end in ((0x5d3989, 0x5d3a22), (0x5d3a49, 0x5d3a6b), (0x446780, 0x30e0000)):
        stack = 0x3080000
        write(stack, 'I', 0x30e0000)
        uc.reg_write(UC_X86_REG_ESP, stack)
        uc.reg_write(UC_X86_REG_EBP, stack + 0x100)
        # Second reset range follows unrelated allocation/CDirectory effects;
        # initialized ESI=-1, EDI=0 are the first range's actual live registers.
        uc.reg_write(UC_X86_REG_ESI, 0xffffffff)
        uc.reg_write(UC_X86_REG_EDI, 0)
        uc.reg_write(UC_X86_REG_FPCW, 0x37f)
        uc.emu_start(start, end, count=10000)
        assert uc.reg_read(UC_X86_REG_EIP) == end, ('startup range cap', fields)
    actual = (struct.unpack('<I', uc.mem_read(0x95c79c, 4))[0],
        struct.unpack('<i', uc.mem_read(0x95c808, 4))[0],
        struct.unpack('<i', uc.mem_read(0x9dd0a8, 4))[0],
        struct.unpack('<H', uc.mem_read(0x95c798, 2))[0],
        struct.unpack('<i', uc.mem_read(0x9e2604, 4))[0],
        struct.unpack('<i', uc.mem_read(0x9e25ec, 4))[0],
        struct.unpack('<i', uc.mem_read(0x9e2634, 4))[0],
        struct.unpack('<I', uc.mem_read(0x9e25fc, 4))[0],
        uc.mem_read(0x9e2630, 1)[0], uc.mem_read(0x9e2640, 1)[0],
        *struct.unpack('<2I', uc.mem_read(0x9e2654, 8)),
        struct.unpack('<i', uc.mem_read(0x9e2624, 4))[0])
    assert actual == (count, zone, timer_after, peds, offensive, attack_state, zones,
        provocation, active, mission, ax, ay, gang), ('startup owned writes/preserved observations', fields, actual)
    assert struct.unpack('<8i', uc.mem_read(0x95c7e8, 32)) == (-1,) * 8
    assert struct.unpack('<18i', uc.mem_read(0x95c7a0, 72)) == (0,) * 18
    assert struct.unpack('<H', uc.mem_read(0x95c794, 2))[0] == cars_before
    assert struct.unpack('<2i', uc.mem_read(0x9dd0ac, 8)) == (gang_timer, member)
    assert state['value'] == 1792 and state['draws'] == 0
    startup_checks += 1
assert startup_checks == 64
update_checks = 0
# These dependencies are explicit observed/intended effects, not implementations
# of EndGangWar, territory ownership or the active war/controller body. Execute
# the actual admission prefix and stop before an unowned active update.
for address in (0x469de0, 0x4493d0, 0x446ce0, 0x444000):
    uc.mem_write(address, b'\xc3')
    uc.ctl_remove_cache(address, address + 1)
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields or fields[0] != 'WAR_UPDATE':
        continue
    bits, frame, mission_after, effect_count, *effects = map(int, fields[1:])
    assert len(effects) == effect_count
    write(0x9e2640, 'B', bool(bits & 1))
    write(0x9e2634, 'i', 6 if bits & 2 else 0)
    write(0x9e2630, 'B', bool(bits & 4))
    write(0xbd6f3e, 'B', bool(bits & 16))
    write(0xc0fd40, 'I', frame)
    state = dict(war_update_mode=True, mission=bool(bits & 8), coop=bool(bits & 32),
        mission_calls=0, coop_calls=0, events=[], value=1792, draws=0)
    stack = 0x3080000
    write(stack, 'I', 0x30e0000)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(0x449510, 0x30e0000, count=10000)
    assert uc.reg_read(UC_X86_REG_EIP) in (0x44957e, 0x449e29), ('update admission cap', fields)
    assert state['events'] == effects, ('source update admission/order/low-byte frame counter', fields, state)
    assert uc.mem_read(0x9e2640, 1)[0] == mission_after
    assert state['mission_calls'] == 2 and state['coop_calls'] == int(bool(bits & 4) and not bits & 16)
    assert state['value'] == 1792 and state['draws'] == 0
    update_checks += 1
assert update_checks == 512
print('ped-streaming-retail-oracle-ok checks=' + str(checks),
      'group-boundary=strict-less fraction=rand/32768 cursors=preincrement translation=99',
      'slot-plans=' + str(slot_checks), 'zone-phases=' + str(zone_checks),
      'zone-change-timer=299 replacement-timer=300 gang-phases=' + str(gang_checks),
      'gang-masks=' + str(mask_checks), 'gang-demands=' + str(demand_checks),
      'startup-resets=' + str(startup_checks), 'streaming-reset=ped-fields-only',
      'war-update-plans=' + str(update_checks), 'territory-frame=low-byte-56',
      'loaded-gang-car-guard=skip-nonempty demand=source-war-helper observations=explicit',
      'slots=requested-fixtures census=incomplete', 'function-sha256=' + function_hash,
      'slot-function-sha256=' + slot_hash, 'zone-function-sha256=' + zone_hash,
      'mask-function-sha256=' + mask_hash, 'car-count-function-sha256=' + count_hash,
      'war-function-sha256=' + war_hash, 'war-update-prefix-sha256=' + update_prefix_hash)
print(native.stdout.splitlines()[-1])
