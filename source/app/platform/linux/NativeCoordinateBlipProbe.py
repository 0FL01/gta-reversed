#!/usr/bin/env python3
"""Owned original-SCM coordinate blips: static RE and real host/GL probe."""
import argparse
import pathlib
import struct
import subprocess
import shlex

SOURCE = pathlib.Path(__file__).resolve().parent
WORKSPACE = SOURCE.parents[4]
OUTPUT = WORKSPACE / 'artifacts/graphics'
NAME = 'NativeCoordinateBlipProbe'


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
               'NativeScriptSession', 'NativeScriptServiceTransaction', 'NativeScriptEntities', 'NativeEntryExits',
             'RealtimeScriptHost', 'NativeCarGenerators', 'NativeCarGeneratorResidency', 'NativeSourceRng', 'NativeRestarts',
             'RealtimeHud', 'RadarMap', NAME]
    OUTPUT.mkdir(parents=True, exist_ok=True)
    objects = []
    with (OUTPUT / (NAME + '-build.log')).open('w') as log:
        for unit in units + ['oswrapper_linux']:
            source = SOURCE / (unit + '.cpp') if unit != 'oswrapper_linux' else SOURCE.parents[2] / 'oswrapper/oswrapper_linux.cpp'
            obj = OUTPUT / ('coordinate-' + unit + '.o')
            command = flags + ['-UNDEBUG', '-Wall', '-Wextra', '-ffunction-sections', '-fdata-sections', '-c', str(source), '-o', str(obj)]
            log.write(shlex.join(command) + '\n'); log.flush()
            subprocess.run(command, cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)
            objects.append(str(obj))
        subprocess.run([flags[0], '-Wl,--gc-sections', *objects, 'vendor/librw/src/librw.a', '-lEGL', '-lGL', '-lpthread', '-lm',
                        '-o', str(OUTPUT / NAME)], cwd=directory, stdout=log, stderr=subprocess.STDOUT, check=True)


def static(game):
    import capstone
    import pefile
    import hashlib
    data = (game / 'gta-sa.exe').read_bytes()
    assert hashlib.sha256(data).hexdigest() == '15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb'
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True
    def instruction(address, mnemonic, immediate=None):
        ins = next(decoder.disasm(pe.get_data(address-base,15),address))
        assert ins.mnemonic == mnemonic, (hex(address),ins.mnemonic)
        if immediate is not None:
            assert any(op.type == capstone.x86.X86_OP_IMM and op.imm == immediate for op in ins.operands), hex(address)
        return ins
    # Static disassembly only; never execute or copy the owned binary.
    assert instruction(0x490f21,'lea').operands[1].mem.disp == -0x4b0
    assert instruction(0x490f34,'movzx').operands[1].mem.disp == 0x492568
    assert instruction(0x490f3b,'jmp').operands[0].mem.disp == 0x492460
    index = pe.get_data(0x492568-base + 0x04ce-0x04b0, 1)[0]
    address = struct.unpack('<I', pe.get_data(0x492460-base + index*4, 4))[0]
    assert address == 0x4914ee
    instruction(address,'push',4)  # collect XYZ,sprite
    instruction(0x4914f2,'call',0x468fa0)
    instruction(0x491515,'fcomp')
    assert struct.unpack('<d',pe.get_data(0x8a2528-base,8))[0] == -100
    instruction(0x491520,'jp',0x49153d)
    instruction(0x491532,'call',0x5829f0)  # source FindGroundZForCoord
    instruction(0x491569,'push',3)  # BOTH
    instruction(0x49156b,'push',5)  # passed color is ignored by SetCoordBlip
    instruction(0x49157a,'push',4)  # BLIP_COORD, NOT CONTACT_POINT=5
    instruction(0x49157f,'call',0x5a0af0)
    instruction(0x5a0b1a,'call',0x5a09f0)  # SetShortRangeCoordBlip -> SetCoordBlip
    instruction(0x5a0b5a,'or',4)  # short-range bit
    instruction(0x5a0abe,'mov',8)  # BLIP_COLOUR_DESTINATION
    instruction(0x49158e,'call',0x5a0f50)  # SetBlipSprite
    instruction(0x491596,'push',1)
    instruction(0x4915a0,'call',0x4692e0)  # output reference, one parameter
    print('STATIC original04CE PASS handler=4914EE type=4 display=3 shortRange=1 colour=8 groundSentinel=-100')
    data = (game / 'data/script/main.scm').read_bytes()
    chunk = 0
    for _ in range(2):
        chunk = struct.unpack_from('<I',data,chunk+3)[0]
    start = struct.unpack_from('<I',data,chunk+24)[0]
    pos = start + 212086 - 200000
    for count in range(9):
        ip = 200000 + pos - start
        assert struct.unpack_from('<H',data,pos)[0] == 0x04ce
        pos += 2
        xyz = []
        for _ in range(3):
            assert data[pos] == 6
            xyz.append(struct.unpack_from('<f',data,pos+1)[0]); pos += 5
        assert data[pos] == 4
        sprite = struct.unpack_from('<b',data,pos+1)[0]; pos += 2
        assert data[pos] == 2
        output = struct.unpack_from('<H',data,pos+1)[0]; pos += 3
        assert sprite == 63
        if not count:
            assert output == 1796 and ip == 212086
            print('STATIC first SCM04CE',ip,'xyz',xyz,'sprite',sprite,'output',output,'next',200000+pos-start)
    assert 200000 + pos - start == 212284
    print('STATIC nine04CE extent=212086..212284; runtime frontier is measured separately')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument('--static', action='store_true')
    mode.add_argument('--build', action='store_true')
    mode.add_argument('--run', action='store_true')
    parser.add_argument('--game-dir', type=pathlib.Path, required=True)
    args = parser.parse_args()
    if args.static:
        static(args.game_dir)
    elif args.build:
        build()
    else:
        for suffix, options in [('cpu', ['--cpu']), ('gl', [])]:
            result = subprocess.run([str(OUTPUT / NAME), str(args.game_dir.resolve()), *options], text=True,
                                    stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=300)
            (OUTPUT / (NAME + '-' + suffix + '.log')).write_text(result.stdout)
            print(result.stdout, end='')
            result.check_returncode()
