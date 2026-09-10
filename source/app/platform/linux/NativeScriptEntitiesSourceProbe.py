#!/usr/bin/env python3
"""Developer-only STATIC RE assertions on the owned retail executable.

No execution, asset output, copying, or runtime dependency. Deliberately pins
the local retail hash: compact 1.0 addresses are NOT usable for this build.
pefile/Capstone already ship in mad-sa:dev. Output is derived scalar evidence.
"""
import argparse
import hashlib
from pathlib import Path
import struct

import capstone
import pefile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("exe", type=Path)
    args = parser.parse_args()
    data = args.exe.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    assert digest == "15e3cfedba9a841df67d8194e7249afb493b0e10d6138fb8ebab2c136e543efb", "unverified executable layout"
    pe = pefile.PE(data=data)
    base = pe.OPTIONAL_HEADER.ImageBase
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.detail = True

    def instruction(address, mnemonic, immediate=None):
        ins = next(decoder.disasm(pe.get_data(address - base, 15), address))
        assert ins.mnemonic == mnemonic, (hex(address), ins.mnemonic)
        if immediate is not None:
            assert any(op.type == capstone.x86.X86_OP_IMM and op.imm == immediate for op in ins.operands), hex(address)
        return ins

    def scalar(address, fmt, expected):
        actual = struct.unpack("<" + fmt, pe.get_data(address - base, struct.calcsize(fmt)))[0]
        assert actual == expected, (hex(address), actual)

    # Identify CHud's three 400-byte help buffers and nonpermanent reset,
    # matched against source/game_sa/Hud.cpp:208 (SetHelpMessage).
    for address in (0x594CE8, 0x594CF9, 0x594D0A):
        instruction(address, "push", 400)
    for address, buffer in ((0x594CEF, 0xC355A8), (0x594D00, 0xC35738), (0x594D11, 0xC358C8)):
        instruction(address, "push", buffer)
    instruction(0x594DC8, "mov", 0)  # nonpermanent state=0, including replacement
    # DrawHelpText=597B40 (compact reference 58B6E0). Wrapped lines + 3 seconds.
    instruction(0x597C54, "call", 0x738050)
    instruction(0x738069, "call", 0x737B90)  # ProcessCurrentString(false,...)
    instruction(0x597C5A, "add", 3)
    scalar(0x8A1EE0, "d", 1000.0)
    scalar(0x8A36C8, "f", 200.0)
    scalar(0x8A1C20, "d", 200.0)
    scalar(0x8A1D68, "f", 0.0)  # surprising original fade-in threshold
    instruction(0x597DE4, "mov", 600)
    instruction(0x597D2E, "fild")
    instruction(0x597D34, "fld")
    instruction(0x597D3A, "fdiv")
    instruction(0x597D3E, "fmul")
    for address in (0x597D02, 0x597D5D):
        ins = instruction(address, "lea")
        assert ins.operands[1].mem.scale == 2  # +/- twice elapsed milliseconds
    instruction(0x597E15, "jnp", 0x597E2E)  # strict lifetime comparison
    scalar(0x8B1410, "d", float(struct.unpack("<f", struct.pack("<f", 0.52))[0]))
    scalar(0x8A2298, "d", float(struct.unpack("<f", struct.pack("<f", 1.1))[0]))
    scalar(0x8ADD30, "d", 34.0)
    scalar(0x8A3318, "d", 28.0)
    # Property model index binding -> DoPickUpEffects property transform path.
    instruction(0x5D023B, "push", 0x93FE70)
    instruction(0x5D0240, "push", 0x8B4218)
    assert pe.get_data(0x8B4218 - base, 16) == b"property_locked\0"
    instruction(0x459E38, "je", 0x459E87)  # property colour category, shared transform below
    # Largest of x/y/z COL extents. The x87 compare of 1 versus 1.2/extent
    # selects a MINIMUM ratio of 1, then lerps from 1 by 0.6 (not a shrink cap).
    instruction(0x45A173, "fsub")
    instruction(0x45A17B, "fsub")
    instruction(0x45A184, "fsub")
    instruction(0x45A1D1, "fdivr")
    scalar(0x8A1F50, "d", float(struct.unpack("<f", struct.pack("<f", 1.2))[0]))
    instruction(0x45A1E0, "jne", 0x45A1E8)
    instruction(0x45A1EA, "fstp")
    instruction(0x45A1F5, "fsub")
    instruction(0x45A1F7, "fmul")
    scalar(0x8A23C8, "d", float(struct.unpack("<f", struct.pack("<f", 0.6))[0]))
    instruction(0x45A1FD, "faddp")
    instruction(0x45A216, "and", 2047)
    scalar(0x8A3378, "d", 0.003056640736758709)
    instruction(0x45A236, "call", 0x854350)  # cos
    instruction(0x45A24A, "call", 0x854480)  # sin
    instruction(0x8543BD, "fcos")
    instruction(0x8544ED, "fsin")
    instruction(0x45A27A, "fstp")  # right.x
    instruction(0x45A29B, "fstp")  # right.y
    instruction(0x45A2E0, "fstp")  # forward.x
    instruction(0x45A302, "fstp")  # forward.y
    # Exact opcode dispatch: subtract0515, use byte translation, then table.
    instruction(0x4925DB, "add", 0xFFFFFAEB)
    instruction(0x4925E3, "cmp", 0x60)
    assert instruction(0x4925EC, "movzx").operands[1].mem.disp == 0x4935AC
    assert instruction(0x4925F3, "jmp").operands[0].mem.disp == 0x49350C
    index = pe.get_data(0x4935AC - base + 0x518 - 0x515, 1)[0]
    assert index == 2
    scalar(0x49350C + index * 4, "I", 0x4926E0)
    instruction(0x4926E0, "push", 4)  # x,y,z,price, THEN fixed text8, THEN output
    instruction(0x49273B, "call", 0x468C50)
    instruction(0x492749, "call", 0x6CDA20)
    instruction(0x492750, "call", 0x469150)
    instruction(0x492788, "push", 18)
    instruction(0x49279E, "call", 0x45B650)
    instruction(0x4927AF, "call", 0x4692E0)
    scalar(0x8A2528, "d", -100.0)
    scalar(0x8A1DD0, "d", 0.5)
    instruction(0x5D024A, "push", 0x93FE74)
    instruction(0x5D024F, "push", 0x8B4208)
    assert pe.get_data(0x8B4208 - base, 15) == b"property_fsale\0"
    instruction(0x459E43, "je", 0x459E80)
    instruction(0x459E80, "mov", 47)
    assert pe.get_data(0x9148D8 - base + 47 * 8, 3) == bytes((255,100,100))
    scalar(0x8A2580, "d", 14.0)
    scalar(0x8A2270, "d", float(struct.unpack("<f", struct.pack("<f", 0.7))[0]))
    scalar(0x8A3248, "d", 255.0)
    instruction(0x459FB5, "call", 0x7539F0)  # projected before admitted to16 queue
    instruction(0x45A0A2, "movzx")  # uint16 object cost, then five times this
    instruction(0x597E1F, "fcomp")
    scalar(0x8A1C18, "f", 3000.0)  # quick-help strict elapsed>3000 branch
    instruction(0x597E2A, "jne", 0x597E48)
    print(f"STATIC-RE PASS retail-size={len(data)} sha256={digest}")
    print("help retail=597B40 reference=58B6E0 duration=(lines+3)*1000 alpha=200 fade=600-2*elapsed scale=200/1000 replacement=reset")
    print("property retail=459C10 reference=455720 time-mask=2047 angle-scale=0.003056640736758709 scale=1+0.6*(max(1,1.2/maxCOLextent)-1)")
    print("0518 STATIC handler=4926E0 table=49350C translation=4935AC index=2 collect=4 text=8 output=1 type=18 modelBinding=property_fsale groundSentinel=-100 groundAdd=0.5")
    print("sale-label STATIC category=47 rgb=255,100,100 radiusXY=14 zAdd=0.7 alpha=255*(1-distance/14) projectedQueue=16 quickHelpMs=3000")


if __name__ == "__main__":
    main()
