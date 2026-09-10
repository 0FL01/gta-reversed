#!/usr/bin/env python3
"""Hash-pinned STATIC retail/schema/SCM assertions; never executes game code.

Reads the owned install only. Emits bounded numeric evidence, no asset dumps.
Actual native readiness/execution is measured separately by NativePickupsProbe.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import urllib.request

import capstone
import pefile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game_dir', type=Path)
    args = parser.parse_args()
    data = (args.game_dir / 'gta-sa.exe').read_bytes()
    assert len(data) == 5971456
    assert hashlib.sha256(data).hexdigest() == '15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb'
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True

    def instruction(address, mnemonic, immediate=None):
        value = next(decoder.disasm(pe.get_data(address-base, 15), address))
        assert value.mnemonic == mnemonic, (hex(address), value.mnemonic)
        if immediate is not None:
            assert any(o.type == capstone.x86.X86_OP_IMM and o.imm == immediate for o in value.operands), hex(address)
        return value

    def scalar(address, fmt):
        return struct.unpack('<'+fmt, pe.get_data(address-base, struct.calcsize(fmt)))[0]

    # ProcessCommands500To599 -> exact 0213/0214 table cases, no name matching.
    i = instruction(0x483792, 'lea')
    assert i.operands[1].mem.disp == -500
    for opcode, case, handler in ((0x213,14,0x483BCC),(0x214,15,0x483C9C)):
        assert scalar(0x484720+opcode-500,'B') == case
        assert scalar(0x484668+case*4,'I') == handler
    instruction(0x483BCC,'push',5)
    instruction(0x483BD0,'call',0x468FA0)
    instruction(0x483BDD,'jns',0x483BF5)
    # UsedObjectArray contains name24 + modelIndex4; negative index stride28.
    instruction(0x483BEC,'mov',0xABC4F8)
    instruction(0x483C30,'call',0x5829F0)
    assert scalar(0x8A2528,'d') == -100 and scalar(0x8A1DD0,'d') == .5
    instruction(0x483C43,'call',0x469150)
    instruction(0x483C49,'call',0x4596A0)  # discarded old-reference lookup, no removal
    i = instruction(0x483C51,'movzx')
    assert i.operands[1].size == 1 and i.operands[1].mem.disp == 0xABB614
    for address in (0x483C6A,0x483C6C,0x483C71,0x483C73):
        instruction(address,'push',0)  # text, empty, moneyPerDay, ammo
    instruction(0x483C8A,'call',0x45B650)
    instruction(0x483C97,'jmp',0x483A27)  # common StoreParameters(1)
    instruction(0x483CAB,'call',0x458F50)  # source collected ring consumption
    instruction(0x483CC1,'call',0x48AF00)  # condition, NOT a create side effect
    # GenerateNewOne: first free slot ascending; 620 entries. Only money/timeout
    # types can be reclaimed when full (not type3 or either existing property).
    instruction(0x45B6D4,'xor')
    instruction(0x45B71E,'cmp',620)
    instruction(0x45B731,'cmp',8)
    instruction(0x45B783,'cmp',4)
    instruction(0x45B788,'cmp',5)
    instruction(0x45B7D0,'or',0xFFFFFFFF)
    instruction(0x45B875,'mov')  # creation timer, no type3 timeout adjustment
    instruction(0x45B97A,'call',0x459B80)  # initial IsVisible(cameraXY<100)
    instruction(0x45B9B5,'call',0x45AF10)  # conditional owned decoration object
    instruction(0x45B9C5,'call',0x57C410)
    # Shared DoPickUpEffects geometry, not a property-specific transform.
    instruction(0x45A15F,'movzx')
    instruction(0x45A1D1,'fdivr')
    assert scalar(0x8A1F50,'d') == struct.unpack('<f',struct.pack('<f',1.2))[0]
    assert scalar(0x8A23C8,'d') == struct.unpack('<f',struct.pack('<f',.6))[0]
    instruction(0x45A216,'and',2047)
    assert scalar(0x8A3378,'d') == .003056640736758709

    revision = '53ed1c2561bf6ca70dc16afca5d8f3a406066158'
    with urllib.request.urlopen(f'https://raw.githubusercontent.com/sannybuilder/library/{revision}/sa/sa.json', timeout=60) as response:
        schema = response.read()
    assert hashlib.sha256(schema).hexdigest() == '797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671'
    commands = {c['id']:c for e in json.loads(schema)['extensions'] if e['name']=='default' for c in e['commands'] if 'id' in c}
    assert [p['type'] for p in commands['0213']['input']] == ['model_object','PickupType','float','float','float']
    assert commands['0213']['output'] == [{'name':'handle','type':'Pickup','source':'var_any'}]
    assert commands['0321']['name'] == 'EXPLODE_CHAR_HEAD'  # not a pickup operation

    scm = (args.game_dir / 'data/script/main.scm').read_bytes()
    word = lambda offset: struct.unpack_from('<I',scm,offset)[0]
    headers, pos = [], 0
    for _ in range(6):
        headers.append(pos); pos = word(pos+3)
    count = word(headers[1]+8)
    assert count < 395
    assert scm[headers[1]+12+24*43:headers[1]+12+24*44].split(b'\0')[0] == b'PICKUPSAVE'
    bindings = []
    for line in (args.game_dir / 'data/maps/generic/dynamic.ide').read_text().splitlines():
        fields = [f.strip() for f in line.split(',')]
        if len(fields)==5 and fields[1]=='pickupsave':
            bindings.append((int(fields[0]),fields[2],int(fields[3]),int(fields[4])))
    assert bindings == [(1277,'icons4',100,128)]
    start = word(headers[2]+24)
    pos = start+5545
    signatures = {0x213:'iifffo',4:'oi',5:'of'}
    counts = {k:0 for k in signatures}
    for number in range(517,540):
        opcode, = struct.unpack_from('<H',scm,pos); pos += 2
        assert opcode in signatures
        values = []
        for kind in signatures[opcode]:
            tag = scm[pos]; pos += 1
            assert tag in ({2,3} if kind=='o' else {2,3,6} if kind=='f' else {1,2,3,4,5})
            fmt = {1:'i',2:'H',3:'H',4:'b',5:'h',6:'f'}[tag]
            values.append((tag,struct.unpack_from('<'+fmt,scm,pos)[0])); pos += struct.calcsize(fmt)
        if opcode==0x213:
            assert values[0:2] == [(4,-43),(4,3)]
        counts[opcode] += 1
    assert counts == {0x213:13,4:1,5:9} and pos-start+200000 == 205876
    assert struct.unpack_from('<H',scm,pos)[0] == 0x570
    print('STATIC PASS 0213 handler=483BCC collect5 model-table-stride28 type-byte create45B650 output6 ground=-100,+0.5')
    print('STATIC PASS model=1277 type=3 used-object=43 source-pool620 source-collected-ring20 token-has-no-inventory-credit')
    print('STATIC typed suffix ONLY 0213=13 0004=1 0005=9 next=0570@205876 native-execution/readiness=NOT-INFERRED')


if __name__ == '__main__':
    main()
