#!/usr/bin/env python3
"""Owned restart command source proof and actual native host GL/SCM oracle."""
import argparse
import hashlib
import pathlib
import re
import shlex
import struct
import subprocess

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeRestartsProbe'


def build():
    directory = WORKSPACE / 'build'
    commands = subprocess.check_output(['ninja', '-C', str(directory), '-t', 'commands', 'mad-sa-linux'], text=True).splitlines()
    template = shlex.split(next(c for c in commands if '-c ' in c and '/Realtime.cpp' in c))
    flags, index = [], 0
    while index < len(template):
        if template[index] in ('-MT', '-MF', '-o', '-c'):
            index += 2
        elif template[index] == '-MD':
            index += 1
        else:
            flags.append(template[index]); index += 1
    units = ['StreamPager', 'NativeCollisionAssets', 'RealtimeGameplay', 'NativePlayerAssets', 'TexSample',
             'Handling', 'Collide', 'IfpAnim', 'CarPose', 'GxtText', 'MenuShot', 'NativePlayerActivity',
              'NativeGarages', 'NativeVehiclePool', 'NativeScriptSchema', 'NativeScriptCorpus',
              'NativeScriptSession', 'NativeScriptEntities', 'NativeEntryExits',
             'RealtimeScriptHost', 'NativeCarGenerators', 'NativeCarGeneratorResidency', 'NativeSourceRng',
             'RealtimeHud', 'RadarMap', 'NativeRestarts', NAME]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    objects = []
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in units + ['oswrapper_linux']:
            source = SOURCE / (unit + '.cpp') if unit != 'oswrapper_linux' else SOURCE.parents[2] / 'oswrapper/oswrapper_linux.cpp'
            obj = OUTPUT / ('restarts-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-c', str(source), '-o', str(obj)]
            log.write(shlex.join(command) + '\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        subprocess.run([flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lEGL', '-lGL', '-lpthread', '-lm',
                        '-o', str(OUTPUT / NAME)], cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)


def static(game):
    import capstone
    import pefile
    data = (game / 'gta-sa.exe').read_bytes()
    assert hashlib.sha256(data).hexdigest() == '15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb'
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    def ins(address, mnemonic, immediate=None):
        i = next(decoder.disasm(pe.get_data(address-base,15),address))
        assert i.mnemonic == mnemonic, (hex(address),i.mnemonic)
        if immediate is not None:
            assert any(op.type == capstone.x86.X86_OP_IMM and (op.imm & 0xffffffff) == (immediate & 0xffffffff) for op in i.operands), hex(address)
        return i
    # Source ProcessOneCommand compact469EB0 -> owned retail46EDD0:
    # opcode/100 indexes command group; group3 uses opcode-311 byte indirection.
    assert struct.unpack('<I',pe.get_data(0x914BB0-base+3*4,4))[0] == 0x481890
    ins(0x48189a,'add',-311)
    assert ins(0x4818ab,'movzx').operands[1].mem.disp == 0x48297c
    assert ins(0x4818b2,'jmp').operands[0].mem.disp == 0x4828bc
    assert struct.unpack('<d',pe.get_data(0x8a2528-base,8))[0] == -100
    for opcode, address, collect, compare, branch, target, ground, call, add in [
            (0x16c,0x481f75,0x481f77,0x481fa9,0x481fb4,0x481fd1,0x481fc6,0x481fef,0x465030),
            (0x16d,0x482001,0x482003,0x482035,0x482040,0x48205d,0x482052,0x48207b,0x465080)]:
        index = pe.get_data(0x48297c-base+opcode-311,1)[0]
        assert struct.unpack('<I',pe.get_data(0x4828bc-base+index*4,4))[0] == address
        ins(address,'push',5); ins(collect,'call',0x468fa0)
        assert ins(compare,'fcomp').operands[0].mem.disp == 0x8a2528
        ins(branch,'jp',target); ins(ground,'call',0x5829f0); ins(call,'call',add)
        body = list(decoder.disasm(pe.get_data(address-base,call+5-address),address))
        assert [i.operands[0].imm for i in body if i.mnemonic == 'call'] == [0x468fa0,0x5829f0,add]
        assert {op.mem.disp for i in body for op in i.operands if op.type == capstone.x86.X86_OP_MEM and 0xabb610 <= op.mem.disp <= 0xabb620} == {0xabb610,0xabb614,0xabb618,0xabb61c,0xabb620}
        print(f'STATIC source{opcode:04X} PASS handler={address:X} collect=5 add={add:X} rawHeading=1 groundSentinel=-100')


def scm(game):
    data = (game / 'data/script/main.scm').read_bytes()
    chunk = 0
    for _ in range(2):
        chunk = struct.unpack_from('<I',data,chunk+3)[0]
    start = struct.unpack_from('<I',data,chunk+24)[0]
    pos = start + 212309 - 200000
    records = []
    while struct.unpack_from('<H',data,pos)[0] in (0x16c,0x16d):
        opcode = struct.unpack_from('<H',data,pos)[0]; ip = 200000+pos-start; pos += 2
        args = []
        for _ in range(4):
            assert data[pos] == 6
            args.append(struct.unpack_from('<f',data,pos+1)[0]); pos += 5
        tag = data[pos]; fmt = {1:'i',4:'b',5:'h'}[tag]
        when = struct.unpack_from('<'+fmt,data,pos+1)[0]; pos += 1+struct.calcsize(fmt)
        records.append((opcode,ip,200000+pos-start,args,when))
    return records


def run(game):
    expected = scm(game)
    for suffix, options in [('cpu',['--cpu']),('gl',[])]:
        result = subprocess.run([str(OUTPUT / NAME),str(game.resolve()),*options],text=True,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,timeout=300)
        (OUTPUT / (NAME+'-'+suffix+'.log')).write_text(result.stdout)
        print(result.stdout,end=''); result.check_returncode()
        if suffix != 'gl':
            continue
        found = re.findall(r'actual(016[CD]) commands=(\d+) ip=(\d+) next=(\d+) xyz=([^ ]+) heading=([^ ]+) when=(-?\d+) count=(\d+)',result.stdout)
        assert len(found) == len(expected) and found
        for row, oracle in zip(found,expected):
            opcode,ip,next_ip,values,when = oracle
            assert (int(row[0],16),int(row[2]),int(row[3]),int(row[6])) == (opcode,ip,next_ip,when)
            actual = [float(x) for x in row[4].split(',')] + [float(row[5])]
            assert all(struct.pack('<f',x) == struct.pack('<f',y) for x,y in zip(actual,values))
        print(f'SCM independent typed-operand oracle PASS actualRegistrations={len(found)} nextIP={expected[-1][2]}')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--static',action='store_true'); mode.add_argument('--build',action='store_true'); mode.add_argument('--run',action='store_true')
    parser.add_argument('--game-dir',type=pathlib.Path,required=True)
    args = parser.parse_args()
    if args.static:
        static(args.game_dir)
    elif args.build:
        build()
    else:
        run(args.game_dir)
