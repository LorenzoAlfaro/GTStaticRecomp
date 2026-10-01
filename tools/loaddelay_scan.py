#!/usr/bin/env python3
"""Find instructions that read a register loaded by the immediately preceding load
(R3000 load delay slot: they see the OLD value on real hardware)."""
import json, os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "gen"))
from mips import Insn, LOADS

def reads(i):
    op = i.op
    if op is None: return set()
    r = set()
    if op in ("sll", "srl", "sra"): r = {i.rt}
    elif op in ("sllv", "srlv", "srav"): r = {i.rt, i.rs}
    elif op in ("jr",): r = {i.rs}
    elif op == "jalr": r = {i.rs}
    elif op in ("mthi", "mtlo"): r = {i.rs}
    elif op in ("mult", "multu", "div", "divu", "add", "addu", "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu"): r = {i.rs, i.rt}
    elif op in ("beq", "bne"): r = {i.rs, i.rt}
    elif op in ("blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"): r = {i.rs}
    elif op in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori"): r = {i.rs}
    elif op in LOADS: r = {i.rs} | ({i.rt} if op in ("lwl", "lwr") else set())
    elif op in ("sb", "sh", "sw", "swl", "swr"): r = {i.rs, i.rt}
    elif op in ("lwc2", "swc2"): r = {i.rs}
    elif op in ("mtc0", "mtc2", "ctc2"): r = {i.rt}
    r.discard(0)
    return r

def scan(name, base, data, lo, hi):
    hits = []
    for a in range(lo, hi - 4, 4):
        i = Insn(a, struct.unpack_from("<I", data, a - base)[0])
        if not (i.op in LOADS or i.op in ("mfc2", "cfc2", "mfc0")): continue
        d = i.rt
        if d == 0: continue
        j = Insn(a + 4, struct.unpack_from("<I", data, a + 4 - base)[0])
        if j.op is None: continue
        # lwl/lwr pairs merge with the pending load legitimately
        if i.op in ("lwl", "lwr") and j.op in ("lwl", "lwr") and j.rt == d: continue
        if d in reads(j):
            hits.append((a, str(i), str(j)))
    print(f"{name}: {len(hits)} load-use pairs")
    for a, x, y in hits[:400]:
        print(f"  {a:08x}: {x:28s} | {y}")

meta = json.load(open("data/exe/exes.json"))
for name, m in meta.items():
    d = open(f"data/exe/{name}.img", "rb").read()
    scan(name, m["base"], d, m["base"], m["base"] + len(d))
