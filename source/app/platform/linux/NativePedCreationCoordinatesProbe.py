"""Development-only isolated retail reference; never used by the application.

No original executable/media is copied or redistributed. The native binary
under test reads NODES files independently. Camera/ground are explicit fixtures
on BOTH sides, not a claim of runtime source camera/population completeness.
"""
from pathlib import Path
import argparse
import hashlib
import struct
import subprocess

import pefile
from unicorn import Uc, UC_ARCH_X86, UC_MODE_32, UC_HOOK_CODE
from unicorn.x86_const import (
    UC_X86_REG_EAX, UC_X86_REG_ECX, UC_X86_REG_EDX, UC_X86_REG_EIP,
    UC_X86_REG_ESP, UC_X86_REG_FPCW,
)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--game-dir', default='/game')
parser.add_argument('--sanitized', action='store_true')
args = parser.parse_args()
root = Path(__file__).resolve().parents[5]
game = Path(args.game_dir).resolve()
out = root / 'artifacts/graphics'
assert out.is_dir(), 'create the repository artifact directory before running this development probe'
stem = 'NativePedCreationCoordinatesProbe' + ('-sanitized' if args.sanitized else '')
binary = root / 'build/godot-native/sa_core_ped_creation_probe'
if args.sanitized:
    binary = out / stem
    source = root / 'gta-reversed/source'
    command = ['g++', '-std=c++20', '-g', '-O1', '-Wall', '-Wextra', '-Wpedantic',
               '-fno-fast-math', '-ffp-contract=off', '-fno-omit-frame-pointer',
               '-fsanitize=address,undefined', '-I' + str(source), '-pthread']
    command += [str(source / 'app/platform/linux' / (name + '.cpp')) for name in
                ('NativePedCreationCoordinatesProbe', 'NativePedCreationCoordinates', 'NativePathGraph', 'NativeSourceRng')]
    command += [str(root / 'godot/native/os_file_posix.cpp'), '-o', str(binary)]
    with (out / (stem + '.build.log')).open('w') as log:
        subprocess.run(command, stdout=log, stderr=subprocess.STDOUT, check=True)
native = subprocess.run([str(binary), str(game)],
                        text=True, capture_output=True, check=True)
(out / (stem + '.log')).write_text(native.stdout + native.stderr)
assert (game / 'gta-sa.exe').stat().st_size == 5971456, 'retail development reference identity mismatch'
pe = pefile.PE(str(game / 'gta-sa.exe'))
assert pe.OPTIONAL_HEADER.ImageBase == 0x400000
function_hash = hashlib.sha256(pe.get_data(0x452280 - 0x400000, 0x538)).hexdigest()
assert function_hash == 'ed7f895f8bf98fab46e2d80d5903635310c6973a12ca31c4fc96ef2b87a9d94c', 'retail function changed'
width_hash = hashlib.sha256(pe.get_data(0x451390 - 0x400000, 0xdc)).hexdigest()
assert width_hash == '26702a56c50fc66e3779d6c87361e9c9b8dc56fd43dc4a1bfe62d409288c0e17', 'retail width helper changed'
image = pe.get_memory_mapped_image()
uc = Uc(UC_ARCH_X86, UC_MODE_32)
uc.mem_map(0x400000, (len(image) + 0xfff) & ~0xfff)
uc.mem_write(0x400000, image)
uc.mem_map(0x1000000, 0x1000000)
uc.mem_map(0x3000000, 0x100000)
paths = 0x1000000
cursor = paths + 0x10000
for area in range(64):
    raw = (game / f'data/Paths/NODES{area}.DAT').read_bytes()
    nodes, vehicles, peds, carlinks, addresses = struct.unpack_from('<5I', raw)
    assert nodes == vehicles + peds
    extended = addresses + 192 if addresses else 0
    node_bytes = raw[20:20 + nodes * 28]
    ptr = cursor
    uc.mem_write(ptr, node_bytes)
    cursor = (cursor + len(node_bytes) + 0xfff) & ~0xfff
    uc.mem_write(paths + 0x804 + area * 4, struct.pack('<I', ptr))
    uc.mem_write(paths + 0x10c4 + area * 4, struct.pack('<I', vehicles))
    uc.mem_write(paths + 0x11e4 + area * 4, struct.pack('<I', peds))
    link_offset = 20 + nodes * 28 + carlinks * 14
    links = raw[link_offset:link_offset + extended * 4]
    ptr = cursor
    uc.mem_write(ptr, links)
    cursor = (cursor + len(links) + 0xfff) & ~0xfff
    uc.mem_write(paths + 0xa44 + area * 4, struct.pack('<I', ptr))
    intersection_offset = link_offset + extended * 4 + addresses * 2 + extended
    intersections = raw[intersection_offset:intersection_offset + extended]
    ptr = cursor
    uc.mem_write(ptr, intersections)
    cursor = (cursor + len(intersections) + 0xfff) & ~0xfff
    uc.mem_write(paths + 0xc84 + area * 4, struct.pack('<I', ptr))
assert cursor < 0x2000000

# External observation/CRT hooks return via tiny isolated adapters. The actual
# target algorithms at452280 and451390, node-coordinates helper and read-only
# scalar data remain the owned reference bytes. No Windows imports execute.
uc.mem_write(0x853e0c, b'\xc3')
uc.mem_write(0x853d40, b'\xd9\xfa\xc3')  # sqrt ST0
uc.mem_write(0x853e30, bytes.fromhex(
    '83ec0c d93c24 668b0424 660d000c 6689442402 d96c2402 '
    'df7c2404 d92c24 8b442404 8b542408 83c40c c3'))
uc.mem_write(0x421f00, bytes.fromhex('c20800'))
uc.mem_write(0x421df0, bytes.fromhex('c20c00'))
uc.mem_write(0x582a50, bytes.fromhex('d90500000f03 c3'))
state = {}

def u32(address):
    return struct.unpack('<I', uc.mem_read(address, 4))[0]

def f32(address):
    return struct.unpack('<f', uc.mem_read(address, 4))[0]

def hook(machine, address, size, _):
    if address == 0x853e0c:
        state['rng'] = (state['rng'] * 214013 + 2531011) & 0xffffffff
        state['draws'] += 1
        machine.reg_write(UC_X86_REG_EAX, (state['rng'] >> 16) & 0x7fff)
    elif address in (0x421f00, 0x421df0):
        state['visible_calls'] += 1
        stack = machine.reg_read(UC_X86_REG_ESP)
        assert f32(stack + 8) == 2.0
        value = state['visibility'] == 0 or (state['visibility'] == 2 and address == 0x421df0)
        machine.reg_write(UC_X86_REG_EAX, int(value))
    elif address == 0x582a50:
        state['ground_calls'] += 1
        stack = machine.reg_read(UC_X86_REG_ESP)
        ground = f32(stack + 12) - 2.0
        if state['ground'] == 2:
            ground += 3.0
        if state['ground'] == 3:
            ground += 4.0
        machine.mem_write(0x30f0000, struct.pack('<f', ground))
        machine.mem_write(u32(stack + 16), bytes([state['ground'] != 1]))

uc.hook_add(UC_HOOK_CODE, hook)

def invoke(start, args):
    stack = 0x3080000
    uc.mem_write(stack, struct.pack('<I', 0x30e0000) + args)
    uc.reg_write(UC_X86_REG_ESP, stack)
    uc.reg_write(UC_X86_REG_ECX, paths)
    uc.reg_write(UC_X86_REG_FPCW, 0x37f)
    uc.emu_start(start, 0x30e0000, count=5000000)
    assert uc.reg_read(UC_X86_REG_EIP) == 0x30e0000, 'reference instruction cap reached'
    return uc.reg_read(UC_X86_REG_EAX) & 0xff

cases = {}
checks = 0
for line in native.stdout.splitlines():
    fields = line.split()
    if not fields:
        continue
    if fields[0] == 'CASE':
        assert len(fields) == 26, fields
        (index, seed, x, y, visible_min, visible_max, hidden_min, hidden_max, switched, alternate, visibility, ground, status,
         draws, rng, area1, node1, area2, node2, fraction, px, py, pz,
         visible_calls, ground_calls) = fields[1:]
        state = dict(rng=int(seed), draws=0, visibility=int(visibility), ground=int(ground),
                     visible_calls=0, ground_calls=0)
        pos, first, second, blend = 0x30c0000, 0x30c0010, 0x30c0014, 0x30c0018
        args = struct.pack('<6f6I', float(x), float(y), float(visible_min), float(visible_max), float(hidden_min), float(hidden_max),
                           pos, first, second, blend, int(switched), 0x30d0000 if int(alternate) else 0)
        result = invoke(0x452280, args)
        actual = (result, state['draws'], state['rng'], state['visible_calls'], state['ground_calls'])
        expected = (int(status) == 0, int(draws), int(rng), int(visible_calls), int(ground_calls))
        assert actual == expected, ('flow', index, actual, expected)
        checks += 1
        if result:
            actual = (u32(first) & 0xffff, u32(first) >> 16, u32(second) & 0xffff,
                      u32(second) >> 16, u32(blend), u32(pos), u32(pos + 4), u32(pos + 8))
            expected = tuple(map(int, (area1, node1, area2, node2, fraction, px, py, pz)))
            assert actual == expected, ('position', index, actual, expected)
            checks += 1
            cases[int(index)] = (u32(first), u32(second), tuple(map(int, (px, py, pz))))
    elif fields[0] == 'JITTER':
        index, seed, px, py, pz = map(int, fields[1:])
        first, second, original = cases[index]
        pos = 0x30c0000
        uc.mem_write(pos, struct.pack('<3I', *original))
        invoke(0x451390, struct.pack('<5I', first, second, seed, pos, pos + 4))
        actual = (u32(pos), u32(pos + 4), u32(pos + 8))
        assert actual == (px, py, pz), ('jitter', index, seed, actual, (px, py, pz))
        checks += 1
print('ped-creation-retail-oracle-ok', 'checks=' + str(checks), 'cases=250',
       'reference=isolated-development', 'camera-ground=explicit-fixtures',
      'x87-control=0x37f', 'function-sha256=' + function_hash, 'width-sha256=' + width_hash)
print(native.stdout.splitlines()[-1])
