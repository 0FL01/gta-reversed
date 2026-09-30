"""Developer-only original request/deletable oracle; no runtime PE dependency.

Model type/TXD/animation/parent slots and RemoveModel are explicit observations
or external effects. Actual request control flow and list insertion run; this
does not certify recursive asset loading, removal, or population completeness.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.x86_const import UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_ESP

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
output = root / 'artifacts/graphics'
assert output.is_dir()
stem = 'NativeModelStreamingProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_model_streaming_probe'
if args.sanitized:
    source = root / 'gta-reversed/source'
    binary = output / stem
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic', '-Werror',
        '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
        '-fsanitize=address,undefined', '-I' + str(source)]
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativeModelStreamingProbe', 'NativeModelStreaming')]
    command += ['-o', str(binary)]
    with (output / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, check=True, stdout=log, stderr=subprocess.STDOUT)
native = subprocess.run([str(binary)], text=True, capture_output=True, check=True)
(output / (stem + '.log')).write_text(native.stdout + native.stderr)
assert 'native-model-streaming-ok ' in native.stdout
game = Path(args.game_dir).resolve() / 'gta-sa.exe'
assert game.stat().st_size == 5971456
pe = pefile.PE(str(game))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
for address, length, digest in (
    (0x408b00, 0x1cb, 'f0d2bac7f839e8309bc0240f5700dc4bd17cd77e7dbe5281d0637a16d3fd6b99'),
    (0x40a060, 0x54, 'abba374e0d2700ad6370dec48df8823ace563b7872d3f8d09ae60ff84634526c'),
    (0x407660, 0x60, '7f89410fbd8cb03e63893eeb07887f96e96dbf4b92187037a6c4fe67f7834755')):
    assert hashlib.sha256(pe.get_data(address - 0x400000, length)).hexdigest() == digest
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x3000000, 0x100000)
for stub in (0x4072a0, 0x4072c0, 0x4086d0, 0x408cd0, 0x30f0000, 0x30f0010):
    uc.mem_write(stub, b'\xc3')
info_base = 0x95c8a0
events = []
fixture = {}

def write(address, fmt, *values):
    uc.mem_write(address, struct.pack('<' + fmt, *values))

def code(machine, address, size, _):
    stack = machine.reg_read(UC_X86_REG_ESP)
    if address in (0x4072a0, 0x4072c0):
        slot, flags = struct.unpack('<iI', machine.mem_read(stack + 4, 8))
        events.append((4 if address == 0x4072a0 else 5,
            (20000 if address == 0x4072a0 else 25575) + slot, flags))
    elif address == 0x4086d0:
        assert struct.unpack('<i', machine.mem_read(stack + 4, 4))[0] == fixture['model'] - 20000
        machine.reg_write(UC_X86_REG_EAX, fixture['parent'] & 0xffffffff)
    elif address == 0x30f0000:
        machine.reg_write(UC_X86_REG_EAX, fixture['type'])
    elif address == 0x30f0010:
        machine.reg_write(UC_X86_REG_EAX, fixture['animation'] & 0xffffffff)
    elif address == 0x407660:
        head = struct.unpack('<I', machine.mem_read(stack + 4, 4))[0]
        assert machine.reg_read(UC_X86_REG_ECX) == fixture['pointer']
        assert head in (info_base + 26312 * 20, info_base + 26314 * 20)
        events.append((3 if head == info_base + 26312 * 20 else 6, 0, 0))
    elif address == 0x408ba2:
        events.append((2, 0, 0))
    elif address == 0x408cd0:
        model = struct.unpack('<i', machine.mem_read(stack + 4, 4))[0]
        events.append((9, model, 0))

def memory(machine, access, address, size, value, _):
    if address == fixture['pointer'] + 6:
        assert size == 1
        events.append((0, value & 255, 0))
    elif address == 0x95c78c:
        events.append((1, 0, 0))
    elif address == 0x95c89c:
        events.append((7, 0, 0))
    elif address == fixture['pointer'] + 16:
        assert value == 2
        events.append((8, 0, 0))

uc.hook_add(UC_HOOK_CODE, code)
uc.hook_add(UC_HOOK_MEM_WRITE, memory)
cases = {'R': 0, 'D': 0}
for line in native.stdout.splitlines():
    if not line.startswith('MODEL '):
        if line.startswith('native-model-streaming-ok '):
            print(line)
        continue
    fields = line.split()
    operation = fields[1]
    model, load, flags, linked, model_type, txd, animation, parent, requested, count = map(int, fields[2:12])
    values = list(map(int, fields[12:]))
    assert len(values) == count * 3
    expected = [tuple(values[i:i + 3]) for i in range(0, len(values), 3)]
    pointer = info_base + model * 20
    fixture.update(model=model, pointer=pointer, type=model_type, animation=animation, parent=parent)
    events.clear()
    for index, next_index, prev_index in ((26312, 26313, -1), (26313, -1, 26312),
                                        (26314, 26315, -1), (26315, -1, 26314)):
        uc.mem_write(info_base + index * 20, bytes(20))
        write(info_base + index * 20, 'hh', next_index, prev_index)
    uc.mem_write(pointer, bytes(20))
    write(pointer, 'hhhBBIIB', 26313 if linked else -1, 26312 if linked else -1, -1, flags, 0, 0, 0, load)
    if linked:
        write(info_base + 26312 * 20, 'h', model)
        write(info_base + 26313 * 20 + 2, 'h', model)
    write(0x9dd090, 'I', info_base)
    write(0x95c840, 'I', info_base + 26314 * 20)
    write(0x95c848, 'I', info_base + 26312 * 20)
    initial_counter = 0xffffffff if flags & 128 else 17
    write(0x95c78c, 'I', initial_counter)
    write(0x95c89c, 'I', initial_counter)
    if model < 20000:
        write(0xb12818 + model * 4, 'I', 0x30a0000)
        write(0x30a0000, 'I', 0x30a1000)
        write(0x30a000a, 'h', txd)
        write(0x30a1010, 'I', 0x30f0000)
        write(0x30a1038, 'I', 0x30f0010)
    stack = 0x30ff000
    write(stack, 'III', 0x30e0000, model, requested)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.emu_start(0x408b00 if operation == 'R' else 0x40a060, 0x30e0000, count=10000)
    assert events == expected, (fields[:12], events, expected)
    final_flags, final_load, final_linked = flags, load, bool(linked)
    for kind, argument, effect_flags in expected:
        if kind == 0:
            final_flags = argument
        elif kind == 2:
            final_linked = False
        elif kind in (3, 6):
            final_linked = True
        elif kind == 8:
            final_load = 2
    assert uc.mem_read(pointer + 6, 1)[0] == final_flags
    assert uc.mem_read(pointer + 16, 1)[0] == final_load
    assert (struct.unpack('<h', uc.mem_read(pointer, 2))[0] != -1) == final_linked
    if final_linked:
        head = 26314 if any(e[0] == 6 for e in expected) else 26312
        assert struct.unpack('<h', uc.mem_read(pointer + 2, 2))[0] == head
        assert struct.unpack('<h', uc.mem_read(info_base + head * 20, 2))[0] == model
        next_index = struct.unpack('<h', uc.mem_read(pointer, 2))[0]
        assert struct.unpack('<h', uc.mem_read(info_base + next_index * 20 + 2, 2))[0] == model
    elif linked:
        assert struct.unpack('<h', uc.mem_read(info_base + 26312 * 20, 2))[0] == 26313
        assert struct.unpack('<h', uc.mem_read(info_base + 26313 * 20 + 2, 2))[0] == 26312
    for address, effect_kind in ((0x95c78c, 1), (0x95c89c, 7)):
        assert struct.unpack('<I', uc.mem_read(address, 4))[0] == (
            initial_counter + sum(e[0] == effect_kind for e in expected)) & 0xffffffff
    cases[operation] += 1
assert cases == {'R': 13400, 'D': 2560}, cases
print('model-streaming-source-oracle-ok requests=13400 deletable=2560 list=original dependencies=explicit assets=unowned census=incomplete')
