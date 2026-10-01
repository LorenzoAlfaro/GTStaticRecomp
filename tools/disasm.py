#!/usr/bin/env python3
"""Disassemble a range of one of GT's unpacked executables, or find references to an address.

Usage: python tools/disasm.py <EXE> <start_hex> <end_hex>
       python tools/disasm.py <EXE> --xref <addr_hex>     (lui/addiu|ori pairs and words)
EXE is SCUS_941.94, GTOS.EXE, GTMENU.EXE, GTMAIN.EXE or GTEND.EXE.
"""
import json
import os
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "gen"))
from mips import Insn  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..")


def load(name):
    meta = json.load(open(os.path.join(ROOT, "data", "exe", "exes.json")))[name]
    return meta["base"], open(os.path.join(ROOT, "data", "exe", name + ".img"), "rb").read()


def main():
    base, data = load(sys.argv[1])
    if sys.argv[2] == "--xref":
        want = int(sys.argv[3], 16)
        lui = {}
        for off in range(0, len(data) - 3, 4):
            a = base + off
            w = struct.unpack_from("<I", data, off)[0]
            if w == want:
                print(f"{a:08x}: word")
            i = Insn(a, w)
            if i.op == "lui":
                lui[i.rt] = (i.imm << 16, a)
            elif i.op in ("addiu", "ori", "lw", "sw", "lbu", "lhu", "lh", "lb", "sh", "sb") and i.rs in lui:
                v = (lui[i.rs][0] + (i.simm if i.op != "ori" else i.imm)) & 0xFFFFFFFF
                if v == want:
                    print(f"{a:08x}: {i}   (lui at {lui[i.rs][1]:08x})")
        return
    start, end = int(sys.argv[2], 16), int(sys.argv[3], 16)
    for a in range(start, end, 4):
        w = struct.unpack_from("<I", data, a - base)[0]
        print(f"{a:08x}: {w:08x}  {Insn(a, w)}")


if __name__ == "__main__":
    main()
