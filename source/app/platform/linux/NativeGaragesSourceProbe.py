#!/usr/bin/env python3
"""Bounded, READ-ONLY garage diagnosis; no native-service or execution claim.

Reads owned DAT/IDE/IPL/IMG/COL and a hash-pinned retail PE. Never executes the
PE, writes asset bytes, or emits SCM/PE dumps. Existing dev-image pefile and
Capstone are diagnostic dependencies only. Output is derived numeric evidence.

02B9 schema: sannybuilder/library 53ed1c2561bf6ca70dc16afca5d8f3a406066158,
sa/sa.json SHA256 797f32be6d3ebae87fd65b57ccc0c0b1cbc2e129c089668761e366740b5bd671:
one string input, no output or compare. Local PE assertions verify the handler.
"""
import argparse
import collections
import hashlib
import math
from pathlib import Path
import struct

import capstone
import pefile


def f32(x):
    return struct.unpack('<f', struct.pack('<f', x))[0]


def asset(root, relative):
    path = root
    for part in relative.replace('\\', '/').split('/'):
        assert part not in ('..', '.')
        if part:
            matches = [p for p in path.iterdir() if p.name.lower() == part.lower()]
            assert len(matches) == 1
            path = matches[0]
    return path


def sections(path):
    section, row = '', 0
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        line = line.split('#')[0].strip()
        if not line:
            continue
        if line.lower() == 'end':
            section = ''
        elif not section:
            section, row = line.lower(), 0
        else:
            row += 1
            yield section, line_number, row, line.replace(',', ' ').split()


def archive(path):
    with path.open('rb') as file:
        assert file.read(4) == b'VER2'
        count, = struct.unpack('<I', file.read(4))
        assert count < 100000
        directory = file.read(count * 32)
        for i in range(count):
            offset, size, _, name = struct.unpack_from('<IHH24s', directory, i * 32)
            name = name.split(b'\0')[0].decode().lower()
            if name.endswith(('.ipl', '.col')):
                file.seek(offset * 2048)
                yield name, file.read(size * 2048)


def garage_geometry(garage):
    x,y,z,ax,ay,bx,by,top = garage['Coords']
    a, b = (f32(ax-x), f32(ay-y)), (f32(bx-x), f32(by-y))
    width, height = f32(math.hypot(*a)), f32(math.hypot(*b))
    assert width > 0 and height > 0
    return (x,y,z), (a[0]/width,a[1]/width), (b[0]/height,b[1]/height), width, height, top


def door_binding(garages, placement, center):
    # Independent scalar oracle for original FindGarageForObject, not a native
    # service. Origin+COL bound center, IPL inverse quaternion, radius7 oriented
    # containment, then strict3D distance to garage XY center at BASE Z.
    px,py,pz,qx,qy,qz,qw = placement[4]
    norm = math.sqrt(qx*qx+qy*qy+qz*qz+qw*qw)
    x,y,z,w = -qx/norm,-qy/norm,-qz/norm,qw/norm
    axes = ((1-2*(y*y+z*z),2*(x*y+z*w),2*(x*z-y*w)),
            (2*(x*y-z*w),1-2*(x*x+z*z),2*(y*z+x*w)),
            (2*(x*z+y*w),2*(y*z-x*w),1-2*(x*x+y*y)))
    point = tuple(f32(p+sum(center[j]*axes[j][i] for j in range(3))) for i,p in enumerate((px,py,pz)))
    candidates = []
    for i,g in enumerate(garages):
        origin,a,b,width,height,top = garage_geometry(g)
        dx,dy = point[0]-origin[0],point[1]-origin[1]
        if not (origin[2]-7 <= point[2] <= top+7 and -7 <= dx*a[0]+dy*a[1] <= width+7 and -7 <= dx*b[0]+dy*b[1] <= height+7):
            continue
        midpoint = (origin[0]+a[0]*width*.5+b[0]*height*.5,origin[1]+a[1]*width*.5+b[1]*height*.5,origin[2])
        candidates.append((i, f32(math.dist(point,midpoint))))
    best, distance = -1, f32(99999.9)
    for i,d in candidates:
        if d < distance:
            best, distance = i,d
    return best, point, candidates


def static_suffix(scm, start, garages):
    # Exact typed subset already registered in NativeScriptSession plus proposed
    # 02B9. No services run, variables evaluate, branches skip, or readiness is
    # inferred. Thus this is a SYNTACTIC candidate boundary only.
    signatures = {4:'oi',5:'of',0x517:'fffso',0x518:'fffiso',0x570:'fffio',0x18B:'ii',0x9B4:'fffii',0x2B9:'s'}
    pos, number, counts, writes = start+1129, 136, collections.Counter(), []
    while number < 1000:
        ip = 200000+pos-start
        opcode, = struct.unpack_from('<H',scm,pos)
        pos += 2
        if opcode not in signatures:
            break
        values = []
        for kind in signatures[opcode]:
            tag = scm[pos]
            pos += 1
            assert tag in ({9} if kind=='s' else {2,3} if kind=='o' else {2,3,6} if kind=='f' else {1,2,3,4,5})
            fmt = {1:'i',2:'H',3:'H',4:'b',5:'h',6:'f',9:'8s'}[tag]
            values.append(struct.unpack_from('<'+fmt,scm,pos)[0])
            pos += struct.calcsize(fmt)
        if opcode == 0x2B9:
            name = values[0].split(b'\0')[0].decode().lower()
            index = next(i for i,g in enumerate(garages) if g['Name']==name)
            writes.append((number,ip,index,garages[index]['Type']))
        counts[opcode] += 1
        number += 1
    assert (number,ip,opcode) == (517,205545,0x0213)
    assert len(writes)==13 and sum(counts.values())==381
    print('STATIC SYNTACTIC ONLY potentialAdditional=',sum(counts.values()),'potentialMissionTotal=',number-1,'next=',f'{opcode:04X}@{ip}','counts=',{f'{k:04X}':v for k,v in counts.items()})
    print('STATIC future garage (command,IP,index,type)=',writes,'earlier-service-readiness=NOT-EVALUATED')


def static_proof(path):
    data = path.read_bytes()
    assert len(data) == 5971456
    assert hashlib.sha256(data).hexdigest() == '15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb'
    pe = pefile.PE(data=data)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True

    def read(address, fmt):
        return struct.unpack('<' + fmt, pe.get_data(address - 0x400000, struct.calcsize(fmt)))[0]

    def instruction(address, mnemonic, immediate=None):
        ins = next(decoder.disasm(pe.get_data(address - 0x400000, 15), address))
        assert ins.mnemonic == mnemonic, (hex(address), ins.mnemonic)
        if immediate is not None:
            assert any(o.type == capstone.x86.X86_OP_IMM and o.imm == immediate for o in ins.operands), hex(address)
        return ins

    # Actual 02B9 dispatch, fixed text8, case-insensitive first matching name.
    instruction(0x48479B, 'add', 0xFFFFFD6D)  # opcode - 0x293
    assert instruction(0x4847AC, 'movzx').operands[1].mem.disp == 0x484E3C
    assert instruction(0x4847B3, 'jmp').operands[0].mem.disp == 0x484DF4
    slot = read(0x484E3C + 0x2B9 - 0x293, 'B')
    handler = read(0x484DF4 + slot * 4, 'I')
    instruction(handler, 'push', 8)
    instruction(0x484DB9, 'call', 0x468C50)
    instruction(0x484DC2, 'call', 0x44A550)
    instruction(0x484DCD, 'js', 0x484854)  # missing name skips setter in original
    instruction(0x484DD4, 'call', 0x44ABA0)
    instruction(0x44ABA7, 'imul', 0xD8)
    instruction(0x44ABAD, 'add', 0x9E3AC0)
    setter = instruction(0x44ABB2, 'or', 2)
    assert setter.operands[0].mem.disp == 0x4E and setter.operands[0].size == 1
    # Startup is Init -> DAT registration -> Init_AfterRestart/InitDoorsAtStart.
    instruction(0x44A43C, 'push', 7)  # AddOne name-copy bound
    instruction(0x44A498, 'or', 8)
    instruction(0x44A4A5, 'or', 16)
    instruction(0x44A4B2, 'or', 32)
    instruction(0x44A4D3, 'and', 0x39)
    instruction(0x44A4D5, 'or', 0x40)
    init = {t: read(0x44A510 + 4 * read(0x44A51C + t - 1, 'B'), 'I') for t in range(1, 46)}
    assert init[17] == 0x44A4FA  # closed, position0
    assert [t for t, target in init.items() if target == 0x44A503] == [2, 3, 4, 5]
    # Crucially this guard follows camera/entity-volume work, not Update entry.
    instruction(0x44E057, 'call', 0x44BC40)
    instruction(0x44E11A, 'test', 2)
    assert instruction(0x44E11F, 'cmp', 0).operands[0].mem.disp == 0x4D
    instruction(0x44E123, 'je', 0x44FF17)
    assert read(0x44FF24 + 4 * read(0x44FF5C + 16, 'B'), 'I') == 0x44F3A7
    assert read(0x44FFDC, 'I') == 0x44F523  # type17/closed path
    assert read(0x8A2310, 'd') == 950
    assert read(0x8A31CC, 'f') == 12.25
    assert read(0x8A2228, 'd') == 100
    instruction(0x44F574, 'call', 0x44AC70)  # XY AABB rectangle squared distance
    instruction(0x44F8CE, 'call', 0x44B420)  # restore up to4 real stored vehicles
    instruction(0x44F8DC, 'mov', 3)  # OPENING only after restore succeeds
    # OPEN is not suppressed by inactive: proximity decides which real vehicle
    # obstruction/capacity query is required. Near on-foot OPEN stays OPEN.
    assert read(0x8A31D8, 'd') == 225
    assert read(0x8A1F5C, 'f') == 16
    assert read(0x8A31D0, 'f') == 4900
    instruction(0x44F43C, 'call', 0x44C590)
    instruction(0x44F475, 'call', 0x44C690)
    assert instruction(0x44F42D, 'cmp', 10).operands[0].mem.disp == 0x594
    # EntirelyInside/Outside use source transformed COL spheres, not render
    # AABBs or an arbitrary capsule. Radius contracts/expands the garage test.
    instruction(0x44BD0C, 'call', 0x545010)
    instruction(0x44BD6E, 'call', 0x44B6A0)
    assert instruction(0x44BD4A, 'fsub').operands[0].mem.disp == 12
    instruction(0x44BEAC, 'call', 0x44B6A0)
    assert instruction(0x44BE85, 'fld').operands[0].mem.disp == 12
    # Startup idle-body oracle. These are exact type/state dispatch branches,
    # not a generic native "far from garage" or "no vehicles anywhere" claim.
    dispatch = lambda t: read(0x44FF24 + 4 * read(0x44FF5C+t-1, 'B'), 'I')
    assert dispatch(1)==0x44EEC6 and read(0x44FFB4,'I')==0x44F0A6
    assert instruction(0x44F0A6,'mov').operands[1].mem.disp==0x40
    instruction(0x44F0AD,'call',0x560DA0)
    instruction(0x44F0B7,'jne',0x44FF19)
    instruction(0x44F0BF,'je',0x44FF19)  # no player vehicle returns regardless of target
    assert all(dispatch(t)==0x44EAB2 for t in (2,3,4)) and read(0x44FFA4,'I')==0x44EAC6
    instruction(0x44EACA,'call',0x44DBE0)
    instruction(0x44DBEB,'call',0x560DA0)
    instruction(0x44DBF5,'je',0x44DCEC)
    instruction(0x44EAD1,'je',0x44FF19)
    assert dispatch(5)==0x44E176 and read(0x44FF90,'I')==0x44E1B1
    instruction(0x44E1B8,'jne',0x44FF19)  # NoResprays
    instruction(0x44E2C1,'call',0x44BEE0)
    instruction(0x44BF23,'call',0x44BD90)  # actual on-foot source sphere test
    assert instruction(0x44E2E6,'cmp').operands[1].mem.disp==0x9E3A6C
    instruction(0x44E2EC,'jne',0x44E2FC)
    instruction(0x44E30A,'je',0x44FF19)
    assert all(dispatch(t)==0x44FAF4 for t in (33,34,35))
    assert read(0x8A31B8,'d')==3600 and read(0x8A1F70,'d')==2
    instruction(0x44FC5F,'jp',0x44FF19)
    instruction(0x44FC67,'je',0x44FF19)
    assert dispatch(44)==dispatch(45)==0x44F3A7
    # Maintenance always enumerates the real vehicle pool, even far away.
    instruction(0x45016E,'jmp',0x44CEC0)
    instruction(0x45017D,'jmp',0x44CDF0)
    assert instruction(0x44CDFB,'mov').operands[1].mem.disp==0xC018AC
    assert instruction(0x44CEC6,'mov').operands[1].mem.disp==0xC018AC
    instruction(0x44CE88,'call',0x57C480)  # real world removal, not a harmless timer
    instruction(0x44CFCF,'call',0x57C480)
    # Garage contribution to the on-foot camera: fixed15 versus follow-ped4.
    instruction(0x537B67,'call',0x44B520)
    instruction(0x537B73,'je',0x537BA1)
    assert instruction(0x537B81,'cmp',0).operands[0].mem.disp==0x81C
    instruction(0x537B90,'mov',15)
    instruction(0x537BAF,'mov',4)
    assert read(0x8A30E4, 'f') == 7
    instruction(0x44D5F8, 'call', 0x44B6A0)  # bound center in oriented garage +/-7
    instruction(0x44D715, 'jne', 0x44D71F)  # strict nearest-center comparison
    print(f'STATIC-RE PASS 02B9 handler={handler:08X} translationSlot={slot} flagOffset=78 mask=2 stride=216 missingName=skip')
    print('STATIC-RE consumer inactive+closed=skip-type-update camera-before-guard=1 onfoot-distanceSquared<12.25 vehicle-distanceSquared<100 playerZ<950 restoreSlots=4 nextState=3')
    print('STATIC-RE idle branches PASS mission=playerVehicleNull bombshop=playerVehicleNull respray=outside+lastGarageDiff impound=distanceSquared>=3600-or-outsideZ maintenance=realVehiclePool camera=garageFixed15/followPed4')
    return init


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('game', type=Path)
    parser.add_argument('exe', type=Path)
    args = parser.parse_args()
    init = static_proof(args.exe)
    root = args.game
    paths = {'ipl': [], 'ide': []}
    for dat in ('data/default.dat', 'data/gta.dat'):
        for line in asset(root, dat).read_text().splitlines():
            words = line.split('#')[0].split()
            if len(words) >= 2 and words[0].lower() in paths:
                paths[words[0].lower()].append(asset(root, words[1]))
    garages, rejected, placements, models = [], [], [], {}
    for path in paths['ipl']:
        for section, line, row, words in sections(path):
            provenance = (str(path.relative_to(root)), line, row)
            if section == 'grge':
                if len(words) != 11:  # source sscanf==11; two shipped nameless rows
                    rejected.append((*provenance, len(words)))
                    continue
                coords = tuple(f32(float(x)) for x in words[:8])
                assert all(math.isfinite(x) for x in coords)
                flag, kind = map(int, words[8:10])
                name = words[10][:7].lower()
                corners = [(coords[0], coords[1]), (coords[3], coords[4]), (coords[5], coords[6]),
                           (f32(coords[3]+coords[5]-coords[0]), f32(coords[4]+coords[6]-coords[1]))]
                rect = (min(x for x,y in corners), max(x for x,y in corners), min(y for x,y in corners), max(y for x,y in corners))
                garages.append(dict(Name=name, Source=provenance, Coords=coords, Type=kind, Bounds=rect,
                    Flags=((flag & 7) << 3) | 0x40, State=1 if init[kind] == 0x44A503 else 0))
            elif section == 'inst':
                placements.append((int(words[0]), *provenance[:2], False, tuple(map(float, words[3:10]))))
    for path in paths['ide']:
        for section, line, _, words in sections(path):
            if section in ('objs', 'tobj'):
                models[int(words[0])] = (words[1].lower(), words[2].lower(), int(words[-3] if section == 'tobj' else words[-1]))
    collisions, binary_files = {}, 0
    for relative in ('models/gta3.img', 'models/gta_int.img', 'models/player.img'):
        for name, data in archive(asset(root, relative)):
            if name.endswith('.ipl'):
                assert data[:4] == b'bnry'
                count, offset = struct.unpack_from('<I', data, 4)[0], struct.unpack_from('<I', data, 28)[0]
                assert offset >= 76 and offset + count * 40 <= len(data)
                binary_files += 1
                for i in range(count):
                    row = struct.unpack_from('<7fiii', data, offset + i * 40)
                    placements.append((row[7], relative + ':' + name, i, True, row[:7]))
            else:
                pos = 0
                while pos + 72 <= len(data) and data[pos:pos+4] in (b'COLL', b'COL2', b'COL3', b'COL4'):
                    size = struct.unpack_from('<I', data, pos+4)[0] + 8
                    assert size >= 72 and pos + size <= len(data)
                    key = data[pos+8:pos+30].split(b'\0')[0].decode().lower()
                    v1 = data[pos:pos+4] == b'COLL'
                    collisions[key] = (relative + ':' + name, struct.unpack_from('<H', data, pos+30)[0],
                        struct.unpack_from('<3f', data, pos+(48 if v1 else 32)), struct.unpack_from('<3f', data, pos+(60 if v1 else 44)),
                        struct.unpack_from('<3f', data, pos+(36 if v1 else 56)))
                    pos += size
    assert len(garages) == 50 and len(rejected) == 2
    names = collections.Counter(g['Name'] for g in garages)
    scm = asset(root, 'data/script/main.scm').read_bytes()
    pos = 0
    for _ in range(2):
        pos = struct.unpack_from('<I', scm, pos+3)[0]
    start = struct.unpack_from('<I', scm, pos+24)[0]
    assert struct.unpack_from('<HB', scm, start+1129) == (0x02B9, 9)
    target_name = scm[start+1132:start+1140].split(b'\0')[0].decode().lower()
    index, target = next((i,g) for i,g in enumerate(garages) if g['Name'] == target_name)
    assert index == 13 and target['Type'] == 17 and target['State'] == 0 and target['Flags'] == 0x48
    door_candidates = [p for p in placements if p[0] in models and models[p[0]][2] & 0x800]
    # Locate the actual local object, then independently check original binding.
    left, right, front, back = target['Bounds']
    near = [p for p in door_candidates if left-7 <= p[4][0] <= right+7 and front-7 <= p[4][1] <= back+7]
    assert len(near) == 1
    door = near[0]
    model = models[door[0]]
    col = collisions[model[0]]
    assert door[0] == 6517 and door[3]
    height = f32(f32(col[3][2]-col[2][2]) - f32(.1))
    bound_index, center, candidates = door_binding(garages,door,col[4])
    assert bound_index == index
    point = (door[4][0],door[4][1]-2,door[4][2])
    dx,dy = max(left-point[0],0,point[0]-right),max(front-point[1],0,point[1]-back)
    distance_squared = f32(dx*dx+dy*dy)
    assert point[2]<950 and distance_squared<12.25
    assert not (target['Flags'] & 2) and ((target['Flags'] | 2) & 2) and target['State']==0
    print('DAT IPLpaths=', len(paths['ipl']), 'validGarages=', len(garages), 'rejected=', rejected)
    print('registration types=', dict(collections.Counter(g['Type'] for g in garages)), 'uniqueNames=', len(names), 'duplicateNames=', {k:v for k,v in names.items() if v>1}, 'nameBound=7 firstMatch=pool-order')
    print('target SCM=02B9@201129 next=201140 command=136 NOT-EXECUTED index=', index, 'record=', target)
    print('doors IDEflag=2048 models=', sum(bool(m[2]&0x800) for m in models.values()), 'placements=', len(door_candidates), 'binaryIPL=', binary_files)
    print('target-door candidate=', door, 'IDE=', model, 'COL=', col, 'sourceDoorHeight=', height, 'COLheaderMatchesIDE=', col[1] == door[0])
    print('DERIVED binding index=',bound_index,'worldCOLcenter=',center,'candidates(index,distance)=',candidates)
    print('DERIVED post-camera closed-hideout branch onfoot=1 point=',point,'rectangleDistanceSquared=',distance_squared,'before=restore4-slots-then-opening after=skip-type-update actualRuntimeExecution=0')
    print('derived-deactivation flags=72->74 state=0->0 fraction=0->0 collisionEnabled=1->1 poseChange=0')
    static_suffix(scm,start,garages)
    print('STATIC ONLY: no SCM execution or native readiness measured; use NativeGaragesProbe for actual progression')


if __name__ == '__main__':
    main()
