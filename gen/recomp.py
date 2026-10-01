#!/usr/bin/env python3
"""Static recompiler: Gran Turismo (SCUS-94194) MIPS R3000A code -> C.

Usage: python gen/recomp.py [--data data] [--out build/gen]

GT is five executables that all load at 0x80010000 and replace each other (see
tools/extract.py). Each one is a module; the runtime checks which one is resident before
calling into it, like overlays sharing a load region.

Produces:
  funcs_NN.c        recompiled functions (split into chunks)
  funcs.h           prototypes
  func_table.c      address -> function tables used by the runtime dispatcher
  recomp_report.txt statistics / unresolved jumps
"""
import argparse
import json
import os
import struct
import sys

sys.path.insert(0, os.path.dirname(__file__))
from mips import Insn, LOADS, STORES  # noqa: E402

# module index (FuncEntry.ovr; 0 would mean "always resident") -> executable
EXES = {1: "SCUS_941.94", 2: "GTOS.EXE", 3: "GTMENU.EXE", 4: "GTMAIN.EXE", 5: "GTEND.EXE"}

# Enhancement hooks: rt_hook(c, addr) runs before these instructions when g_hooks_on is set
# (runtime/hooks.cpp). Keyed by (module index, address).
HOOKS = set()
# Code with non-standard control flow that must run on the interpreter: (module, lo, hi)
INTERP_ONLY = []
# Functions replaced by runtime implementations
OVERRIDES = {}


def interp_only(mod, a):
    return any(m == mod.ovr and lo <= a < hi for m, lo, hi in INTERP_ONLY)


def interp_only_target(mod, a):
    return interp_only(mod, a)


class Module:
    def __init__(self, name, base, data, lo, hi, ovr=None):
        self.name = name
        self.base = base
        self.data = data
        self.lo = lo  # code range
        self.hi = hi
        self.ovr = ovr  # module index
        self.funcs = {}  # addr -> Function
        self.seeds = {}  # addr -> strength (2 strong, 1 weak)
        self.names = {}

    def contains(self, a):
        return self.lo <= a < self.hi

    def word(self, a):
        if not (self.base <= a < self.base + len(self.data) - 3):
            return None
        return struct.unpack_from("<I", self.data, a - self.base)[0]

    def insn(self, a):
        w = self.word(a)
        return None if w is None else Insn(a, w)

    def prefix(self):
        return f"F{self.ovr}"

    def fname(self, a):
        return f"{self.prefix()}_{a:08X}"


class Function:
    def __init__(self, mod, entry):
        self.mod = mod
        self.entry = entry
        self.insns = {}  # addr -> Insn (reachable, incl. delay slots)
        self.labels = set()
        self.jumptables = {}  # jr addr -> [targets]
        self.dyn_jr = set()
        self.ok = True
        self.calls = set()  # (target) from jal
        self.consts = set()  # lui/addiu constants (potential function pointers)

    @property
    def lo(self):
        return min(self.insns)

    @property
    def hi(self):
        return max(self.insns) + 4


# approximate R3000A timing: loads stall on uncached RAM, mult/div latency, GTE command time
GTE_CYCLES = {0x01: 15, 0x06: 8, 0x0C: 6, 0x10: 8, 0x11: 8, 0x12: 8, 0x13: 19, 0x14: 13, 0x16: 44,
              0x1B: 17, 0x1C: 11, 0x1E: 14, 0x20: 30, 0x28: 5, 0x29: 8, 0x2A: 17, 0x2D: 5, 0x2E: 6,
              0x30: 23, 0x3D: 5, 0x3E: 5, 0x3F: 39}


def cycles(i):
    op = i.op
    if op in LOADS or op == "lwc2":
        return 5
    if op in ("mult", "multu"):
        return 7
    if op in ("div", "divu"):
        return 36
    if op == "cop2":
        return GTE_CYCLES.get(i.word & 0x3F, 8)
    return 1


def is_uncond_branch(i):
    return (i.op == "beq" and i.rs == 0 and i.rt == 0) or (i.op == "bgez" and i.rs == 0)


def reg_const_before(mod, fn, addr, reg, limit=256, depth=0):
    """Constant value of reg just before addr, via a backward scan for
    lui / addiu / ori / addu-with-zero definitions (linear, best effort)."""
    if reg == 0:
        return 0
    if depth > 4:
        return None
    a = addr - 4
    for _ in range(limit):
        i = mod.insn(a)
        if i is None or not i.valid:
            return None
        if i.writes() == reg:
            if i.op == "lui":
                return (i.imm << 16) & 0xFFFFFFFF
            if i.op == "addiu":
                v = reg_const_before(mod, fn, a, i.rs, limit, depth + 1)
                return None if v is None else (v + i.simm) & 0xFFFFFFFF
            if i.op == "ori":
                v = reg_const_before(mod, fn, a, i.rs, limit, depth + 1)
                return None if v is None else v | i.imm
            if i.op in ("addu", "or") and 0 in (i.rs, i.rt):
                return reg_const_before(mod, fn, a, i.rs or i.rt, limit, depth + 1)
            return None
        a -= 4
    return None


def find_jumptable(mod, fn, jr):
    """Resolve 'jr reg' fed by a jump table. Returns list of targets or None."""
    reg = jr.rs
    a = jr.addr - 4
    lw = None
    for _ in range(16):
        i = mod.insn(a)
        if i is None:
            return None
        if i.writes() == reg:
            if i.op == "lw":
                lw = i
            break
        a -= 4
    if lw is None:
        return None
    base_reg, off = lw.rs, lw.simm
    # base = addu(x, y) where one operand is a lui-constant (the table)
    a = lw.addr - 4
    addu = None
    for _ in range(16):
        i = mod.insn(a)
        if i is None:
            return None
        if i.writes() == base_reg:
            if i.op == "addu":
                addu = i
            break
        a -= 4
    if addu is None:
        return None
    table = None
    for r in (addu.rs, addu.rt):
        v = reg_const_before(mod, fn, addu.addr, r)
        if v is not None:
            table = (v + off) & 0xFFFFFFFF
            break
    if table is None:
        return None
    # bound: nearest sltiu before the load
    bound = None
    a = jr.addr - 4
    for _ in range(24):
        i = mod.insn(a)
        if i is None:
            break
        if i.op == "sltiu":
            bound = i.imm
            break
        a -= 4
    targets = []
    for k in range(bound if bound else 512):
        w = read_word_any(mod, table + 4 * k)
        if w is None or w & 3 or not mod.contains(w):
            if bound:
                return None
            break
        targets.append(w)
    return targets or None


MODULES = []


def read_word_any(mod, a):
    return mod.word(a)


def decode_function(mod, entry, strong):
    fn = Function(mod, entry)
    work = [entry]
    seen = set()  # addresses walked as part of the normal instruction stream
    fn.labels.add(entry)
    lui = {}
    while work:
        a = work.pop()
        while True:
            if a in seen:
                break
            if not mod.contains(a):
                if not strong:
                    fn.ok = False
                    return fn
                break
            i = mod.insn(a)
            seen.add(a)
            fn.insns[a] = i
            if not i.valid:
                if not strong:
                    fn.ok = False
                    return fn
                break
            # track lui/addiu constants for function pointer discovery
            if i.op == "lui":
                lui[i.rt] = i.imm << 16
            elif i.op in ("addiu", "ori") and i.rs in lui:
                v = (lui[i.rs] + i.simm) if i.op == "addiu" else (lui[i.rs] | i.imm)
                fn.consts.add(v & 0xFFFFFFFF)
            if i.has_delay:
                d = mod.insn(a + 4)
                if d is None:
                    fn.ok = False
                    return fn
                fn.insns[a + 4] = d
                if not d.valid and not strong:
                    fn.ok = False
                    return fn
                if d.op == "lui":
                    lui[d.rt] = d.imm << 16
                elif d.op in ("addiu", "ori") and d.rs in lui:
                    v = (lui[d.rs] + d.simm) if d.op == "addiu" else (lui[d.rs] | d.imm)
                    fn.consts.add(v & 0xFFFFFFFF)
                op = i.op
                if op in ("beq", "bne", "blez", "bgtz", "bltz", "bgez"):
                    fn.labels.add(i.target)
                    work.append(i.target)
                    if is_uncond_branch(i):
                        break
                    a += 8
                    fn.labels.add(a)
                    continue
                if op in ("bltzal", "bgezal", "jal"):
                    fn.calls.add(i.target)
                    a += 8
                    fn.labels.add(a)
                    continue
                if op == "jalr":
                    a += 8
                    fn.labels.add(a)
                    continue
                if op == "j":
                    t = i.target
                    if t == entry or (mod.contains(t) and mod.seeds.get(t, 0) < 2 and (mod.ovr, t) not in OVERRIDES):
                        fn.labels.add(t)
                        work.append(t)
                    break
                if op == "jr":
                    if i.rs != 31:
                        tabs = find_jumptable(mod, fn, i)
                        if tabs:
                            fn.jumptables[a] = tabs
                            for t in tabs:
                                fn.labels.add(t)
                                work.append(t)
                        else:
                            fn.dyn_jr.add(a)
                    break
            a += 4
    return fn


def in_any_code(a):
    return any(m.contains(a) for m in MODULES) or a in (0xA0, 0xB0, 0xC0)


def plausible(mod, fn):
    """Extra sanity checks for weakly-seeded functions (might be data)."""
    has_return = False
    for a, i in fn.insns.items():
        # compilers never write $zero (except the canonical nop) or jump through it
        if i.word != 0 and i.op not in ("j", "jal", "jr", "jalr", "syscall", "break", "mtc0", "mtc2", "ctc2",
                                         "cop2", "rfe", "mthi", "mtlo", "mult", "multu", "div", "divu")                 and not i.is_branch and i.op not in STORES and i.op != "swc2" and i.writes() is None                 and i.op not in ("lwc2",):
            return False
        if i.op == "jr" and i.rs == 0:
            return False
        if i.op in ("syscall", "break") and a != fn.entry:
            return False
        if i.op in ("j", "jal") and not in_any_code(i.target):
            return False
        if i.is_branch and not mod.contains(i.target):
            return False
        if (i.writes() in (26, 27)) or (i.op not in ("j", "jal") and (i.rs in (26, 27) and i.op not in ("lui",))):
            if i.op not in ("j", "jal", "lui"):
                return False
        if i.has_delay:
            d = fn.insns.get(a + 4)
            if d is not None and d.has_delay:
                return False
        if i.op in ("jr", "j"):
            has_return = True
    return has_return


def scan_pointers(src, mods):
    """Words in src data that point into any module's code -> weak seeds."""
    d = src.data
    for off in range(0, len(d) - 3, 4):
        w = struct.unpack_from("<I", d, off)[0]
        if w & 3:
            continue
        for mod in mods:
            if mod.contains(w) and w not in mod.seeds:
                mod.seeds[w] = 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--data", default="data")
    ap.add_argument("--out", default="build/gen")
    ap.add_argument("--chunk", type=int, default=400)
    args = ap.parse_args()

    meta = json.load(open(os.path.join(args.data, "exe", "exes.json")))
    for idx, name in EXES.items():
        m = meta[name]
        data = open(os.path.join(args.data, "exe", name + ".img"), "rb").read()
        mod = Module(name, m["base"], data, m["base"], m["base"] + len(data), ovr=idx)
        mod.packed = open(os.path.join(args.data, "exe", name + ".bin"), "rb").read()
        mod.seeds[m["entry"]] = 2
        MODULES.append(mod)

    # data -> code pointers (each program only points into itself)
    for mod in MODULES:
        scan_pointers(mod, [mod])

    # --- discovery fixpoint ---
    # Strong seeds (entry, jal targets) are always decoded. Weak seeds (pointer scans /
    # lui+addiu constants) are only tried once strong seeds are exhausted, and only if
    # they aren't already inside a decoded function body (those are almost always
    # jump-table case labels).
    rounds = 0
    bad = {m: set() for m in MODULES}
    while True:
        rounds += 1
        progress = False
        for want_strong in (True, False):
            for mod in MODULES:
                covered = set()
                if not want_strong:
                    for f in mod.funcs.values():
                        covered.update(f.insns)
                for a, strength in sorted(mod.seeds.items()):
                    if (strength >= 2) != want_strong:
                        continue
                    if a in mod.funcs or a in bad[mod] or (mod.ovr, a) in OVERRIDES:
                        continue
                    if interp_only(mod, a):
                        continue
                    if not want_strong and a in covered:
                        continue
                    fn = decode_function(mod, a, want_strong)
                    if not fn.ok or (not want_strong and not plausible(mod, fn)):
                        bad[mod].add(a)
                        continue
                    mod.funcs[a] = fn
                    progress = True
                    if not want_strong:
                        covered.update(fn.insns)
                    for t in fn.calls:
                        if mod.contains(t) and mod.seeds.get(t, 0) < 2:
                            mod.seeds[t] = 2
                    for v in fn.consts:
                        if mod.contains(v) and not (v & 3) and v not in mod.seeds:
                            mod.seeds[v] = 1
            if progress:
                break  # re-run strong seeds before more weak ones
        if not progress:
            break

    emit(args)
    print(f"discovery rounds: {rounds}")


# ------------------------------------------------------------------------------
# C emission


def R(n):
    return "0" if n == 0 else f"c->r[{n}]"


def W(n, expr):
    if n == 0:
        return f"(void)({expr});"
    return f"c->r[{n}] = {expr};"


def emit_simple(i):
    """C for a non-control-flow instruction."""
    op = i.op
    rs, rt, rd = R(i.rs), R(i.rt), R(i.rd)
    if i.word == 0:
        return ""
    if op == "sll":
        return W(i.rd, f"{rt} << {i.sa}") if i.rd else ""
    if op == "srl":
        return W(i.rd, f"{rt} >> {i.sa}") if i.rd else ""
    if op == "sra":
        return W(i.rd, f"(uint32_t)((int32_t){rt} >> {i.sa})") if i.rd else ""
    if op == "sllv":
        return W(i.rd, f"{rt} << ({rs} & 31)") if i.rd else ""
    if op == "srlv":
        return W(i.rd, f"{rt} >> ({rs} & 31)") if i.rd else ""
    if op == "srav":
        return W(i.rd, f"(uint32_t)((int32_t){rt} >> ({rs} & 31))") if i.rd else ""
    if op in ("add", "addu"):
        return W(i.rd, f"{rs} + {rt}") if i.rd else ""
    if op in ("sub", "subu"):
        return W(i.rd, f"{rs} - {rt}") if i.rd else ""
    if op == "and":
        return W(i.rd, f"{rs} & {rt}") if i.rd else ""
    if op == "or":
        return W(i.rd, f"{rs} | {rt}") if i.rd else ""
    if op == "xor":
        return W(i.rd, f"{rs} ^ {rt}") if i.rd else ""
    if op == "nor":
        return W(i.rd, f"~({rs} | {rt})") if i.rd else ""
    if op == "slt":
        return W(i.rd, f"(int32_t){rs} < (int32_t){rt}") if i.rd else ""
    if op == "sltu":
        return W(i.rd, f"{rs} < {rt}") if i.rd else ""
    if op == "mfhi":
        return W(i.rd, "c->hi") if i.rd else ""
    if op == "mflo":
        return W(i.rd, "c->lo") if i.rd else ""
    if op == "mthi":
        return f"c->hi = {rs};"
    if op == "mtlo":
        return f"c->lo = {rs};"
    if op == "mult":
        return f"{{ int64_t p = (int64_t)(int32_t){rs} * (int32_t){rt}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
    if op == "multu":
        return f"{{ uint64_t p = (uint64_t){rs} * {rt}; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); }}"
    if op == "div":
        return f"DIV(c, {rs}, {rt});"
    if op == "divu":
        return f"DIVU(c, {rs}, {rt});"
    if op in ("addi", "addiu"):
        return W(i.rt, f"{rs} + {i.simm & 0xFFFFFFFF:#x}u") if i.rt else ""
    if op == "slti":
        return W(i.rt, f"(int32_t){rs} < {i.simm}") if i.rt else ""
    if op == "sltiu":
        return W(i.rt, f"{rs} < {i.simm & 0xFFFFFFFF:#x}u") if i.rt else ""
    if op == "andi":
        return W(i.rt, f"{rs} & {i.imm:#x}") if i.rt else ""
    if op == "ori":
        return W(i.rt, f"{rs} | {i.imm:#x}") if i.rt else ""
    if op == "xori":
        return W(i.rt, f"{rs} ^ {i.imm:#x}") if i.rt else ""
    if op == "lui":
        return W(i.rt, f"{(i.imm << 16):#x}u") if i.rt else ""
    addr = f"{rs} + {i.simm & 0xFFFFFFFF:#x}u" if i.simm else rs
    if op == "lb":
        return W(i.rt, f"(uint32_t)(int8_t)MEM_LB({addr})")
    if op == "lbu":
        return W(i.rt, f"MEM_LB({addr})")
    if op == "lh":
        return W(i.rt, f"(uint32_t)(int16_t)MEM_LH({addr})")
    if op == "lhu":
        return W(i.rt, f"MEM_LH({addr})")
    if op == "lw":
        return W(i.rt, f"MEM_LW({addr})")
    if op == "lwl":
        return W(i.rt, f"LWL({rt}, {addr})")
    if op == "lwr":
        return W(i.rt, f"LWR({rt}, {addr})")
    if op == "sb":
        return f"MEM_SB({addr}, {rt});"
    if op == "sh":
        return f"MEM_SH({addr}, {rt});"
    if op == "sw":
        return f"MEM_SW({addr}, {rt});"
    if op == "swl":
        return f"SWL({addr}, {rt});"
    if op == "swr":
        return f"SWR({addr}, {rt});"
    if op == "lwc2":
        return f"gte_write_data(c, {i.rt}, MEM_LW({addr}));"
    if op == "swc2":
        return f"MEM_SW({addr}, gte_read_data(c, {i.rt}));"
    if op == "mfc2":
        return W(i.rt, f"gte_read_data(c, {i.rd})")
    if op == "cfc2":
        return W(i.rt, f"gte_read_ctrl(c, {i.rd})")
    if op == "mtc2":
        return f"gte_write_data(c, {i.rd}, {rt});"
    if op == "ctc2":
        return f"gte_write_ctrl(c, {i.rd}, {rt});"
    if op == "cop2":
        return f"gte_command(c, {i.word & 0x1FFFFFF:#x});"
    if op == "mfc0":
        return W(i.rt, f"rt_mfc0(c, {i.rd})")
    if op == "mtc0":
        return f"rt_mtc0(c, {i.rd}, {rt});"
    if op == "rfe":
        return "rt_rfe(c);"
    if op == "syscall":
        return f"c->pc = {i.addr:#x}u; rt_syscall(c, {(i.word >> 6) & 0xFFFFF:#x});"
    if op == "break":
        return f"rt_break(c, {i.addr:#x}u, {(i.word >> 6) & 0xFFFFF:#x});"
    raise ValueError(f"unhandled {op} at {i.addr:08x}")


PRECISE = os.environ.get("RECOMP_PRECISE") == "1"  # per-instruction cycle updates (lockstep debugging)


class Emitter:
    def __init__(self, mod, fn):
        self.mod = mod
        self.fn = fn
        self.out = []
        self.pending = 0
        self.terminated = False

    def line(self, s, ind=1):
        if s:
            self.out.append("    " * ind + s)

    def flush_cycles(self, ind=1):
        if self.pending:
            self.line(f"CYC({self.pending});", ind)
            self.pending = 0

    def call_expr(self, target, ret):
        """C statement calling target (a direct jal/bal)."""
        mod = self.mod
        if interp_only_target(mod, target):
            return f"rt_call(c, {target:#x}u, {ret:#x}u);"
        if (mod.ovr, target) in OVERRIDES:
            return f"{OVERRIDES[(mod.ovr, target)]}(c);"
        if mod.contains(target) and target in mod.funcs:
            return f"{mod.fname(target)}(c);"
        return f"rt_call(c, {target:#x}u, {ret:#x}u);"

    def tail_expr(self, target):
        mod = self.mod
        if interp_only_target(mod, target):
            return f"rt_jump(c, {target:#x}u, ra_entry); return;"
        if (mod.ovr, target) in OVERRIDES:
            return f"{OVERRIDES[(mod.ovr, target)]}(c); return;"
        if mod.contains(target) and target in mod.funcs:
            return f"{mod.fname(target)}(c); return;"
        return f"rt_jump(c, {target:#x}u, ra_entry); return;"

    def delay(self, a, ind):
        d = self.fn.insns.get(a + 4)
        if PRECISE:
            self.line(f"CYC({cycles(d) if d is not None and d.valid else 1});", ind)
        if d is not None and d.word != 0:
            if not d.valid:
                self.line(f"rt_invalid(c, {a + 4:#x}u);", ind)
            elif d.has_delay:
                self.line(f"/* branch in delay slot at {a + 4:08x} */ rt_warn_delay(c, {a + 4:#x}u);", ind)
            else:
                self.line(emit_simple(d), ind)

    def run(self):
        fn, mod = self.fn, self.mod
        name = mod.fname(fn.entry)
        sym = mod.names.get(fn.entry)
        self.out.append(f"// {sym}" if sym else f"// {mod.name} {fn.entry:08x}")
        needs_frame = any(i.op == "jalr" and i.rd != 31 for i in fn.insns.values())
        if needs_frame:
            # hand-written code with custom link registers may return several levels at once
            self.out.append(f"static void {name}_body(CPU* c);")
            self.out.append(f"void {name}(CPU* c) {{")
            self.out.append("    volatile int d = rt_frame_push(c->r[31]);")
            self.out.append(f"    if (rt_setjmp(rt_frame_jb(d)) == 0) {name}_body(c);")
            self.out.append("    rt_frame_pop(d);")
            self.out.append("}")
            self.out.append(f"static void {name}_body(CPU* c) {{")
        else:
            self.out.append(f"void {name}(CPU* c) {{")
        self.line("uint32_t ra_entry = c->r[31]; (void)ra_entry;")
        if PRECISE:
            self.line(f"TRACE_ENTRY(c, {fn.entry:#x}u);")
        self.line(f"goto L_{fn.entry:08X};")
        addrs = sorted(fn.insns)
        # delay slots that are only reached as delay slots are emitted inline
        delay_only = set()
        for a in addrs:
            i = fn.insns[a]
            if i.has_delay:
                delay_only.add(a + 4)
        prev = None
        for a in addrs:
            i = fn.insns[a]
            if a in delay_only and a not in fn.labels:
                prev = a
                continue
            if prev is not None and a != prev + 4 and not self.terminated:
                # gap: previous instruction falls through to an address we don't have
                self.flush_cycles()
                self.line(f"rt_fallthrough(c, {prev + 4:#x}u); return;")
            if a in fn.labels:
                self.flush_cycles()
                self.out.append(f"L_{a:08X}:;")
            self.terminated = False
            if a in delay_only and a in fn.labels:
                # branch target inside a delay slot: execute it then continue at a+4
                self.pending += cycles(i) if i.valid else 1
                self.flush_cycles()
                self.line(emit_simple(i) if i.valid else f"rt_invalid(c, {a:#x}u); return;")
                self.line(f"goto L_{a + 4:08X};")
                fn.labels.add(a + 4)
                prev = a
                continue
            if (self.mod.ovr, a) in HOOKS:
                self.line(f"if (g_hooks_on) rt_hook(c, {a:#x}u);")
            self.pending += cycles(i) if i.valid else 1
            if PRECISE:
                self.flush_cycles()
            if not i.valid:
                self.flush_cycles()
                self.line(f"rt_invalid(c, {a:#x}u); return;")
                self.terminated = True
            elif i.has_delay:
                d = fn.insns.get(a + 4)
                if not PRECISE:
                    self.pending += cycles(d) if d is not None and d.valid else 1
                self.control(i)
                self.terminated = i.op in ("j", "jr") or is_uncond_branch(i)
            else:
                self.line(emit_simple(i))
            prev = a if not i.has_delay else a + 4
        # the last instruction might fall off the end
        if not self.terminated:
            self.flush_cycles()
            self.line(f"rt_fallthrough(c, {prev + 4:#x}u); return;")
        self.out.append("}")
        return "\n".join(self.out)

    @staticmethod
    def ends_block(i, a):
        if i is None:
            return True
        if not i.valid:
            return True
        return False

    def control(self, i):
        a = i.addr
        op = i.op
        fn = self.fn
        if op in ("beq", "bne", "blez", "bgtz", "bltz", "bgez"):
            rs, rt = R(i.rs), R(i.rt)
            cond = {
                "beq": f"{rs} == {rt}", "bne": f"{rs} != {rt}",
                "blez": f"(int32_t){rs} <= 0", "bgtz": f"(int32_t){rs} > 0",
                "bltz": f"(int32_t){rs} < 0", "bgez": f"(int32_t){rs} >= 0",
            }[op]
            back = i.target <= a
            if is_uncond_branch(i):
                self.delay(a, 1)
                self.flush_cycles()
                if back:
                    self.line("CHECK_EVENTS(c);")
                self.line(f"goto L_{i.target:08X};")
                return
            self.line("{")
            self.line(f"int cond = {cond};", 2)
            self.delay(a, 2)
            self.flush_cycles(2)
            if back:
                self.line(f"if (cond) {{ CHECK_EVENTS(c); goto L_{i.target:08X}; }}", 2)
            else:
                self.line(f"if (cond) goto L_{i.target:08X};", 2)
            self.line("}")
            if a + 8 in fn.insns and a + 8 not in fn.labels:
                pass
            return
        if op in ("bltzal", "bgezal"):
            rs = R(i.rs)
            cond = f"(int32_t){rs} < 0" if op == "bltzal" else f"(int32_t){rs} >= 0"
            self.line("{")
            self.line(f"int cond = {cond};", 2)
            self.line(f"c->r[31] = {a + 8:#x}u;", 2)
            self.delay(a, 2)
            self.flush_cycles(2)
            self.line(f"if (cond) {{ CHECK_EVENTS(c); {self.call_expr(i.target, a + 8)} }}", 2)
            self.line("}")
            return
        if op == "jal":
            self.line(f"c->r[31] = {a + 8:#x}u;")
            self.delay(a, 1)
            self.flush_cycles()
            self.line("CHECK_EVENTS(c);")
            self.line(f"{{ CALL_CHECK_BEGIN(); {self.call_expr(i.target, a + 8)} CALL_CHECK_END({i.target:#x}u, {a:#x}u); }}")
            return
        if op == "jalr":
            self.line("{")
            self.line(f"uint32_t t = {R(i.rs)};", 2)
            if i.rd:
                self.line(f"c->r[{i.rd}] = {a + 8:#x}u;", 2)
            self.delay(a, 2)
            self.flush_cycles(2)
            self.line("CHECK_EVENTS(c);", 2)
            self.line(f"rt_call(c, t, {a + 8:#x}u);", 2)
            self.line("}")
            return
        if op == "j":
            t = i.target
            self.delay(a, 1)
            self.flush_cycles()
            if t in fn.labels and (t == fn.entry or not (self.mod.seeds.get(t, 0) >= 2 and t != fn.entry)) and t in fn.insns:
                if t <= a:
                    self.line("CHECK_EVENTS(c);")
                self.line(f"goto L_{t:08X};")
            else:
                self.line("CHECK_EVENTS(c);")
                self.line(self.tail_expr(t))
            return
        if op == "jr":
            if i.rs == 31:
                self.delay(a, 1)
                self.flush_cycles()
                self.line("if (c->r[31] != ra_entry) rt_return_to(c, c->r[31]);")
                self.line("return;")
                return
            self.line("{")
            self.line(f"uint32_t t = {R(i.rs)};", 2)
            self.delay(a, 2)
            self.flush_cycles(2)
            if a in fn.jumptables:
                self.line("switch (t) {", 2)
                for tgt in sorted(set(fn.jumptables[a])):
                    self.line(f"case {tgt:#x}u: goto L_{tgt:08X};", 3)
                self.line("}", 2)
            self.line("rt_jump(c, t, ra_entry); return;", 2)
            self.line("}")
            return
        raise ValueError(op)


def emit(args):
    out = args.out
    os.makedirs(out, exist_ok=True)
    allfns = []
    for mod in MODULES:
        for a in sorted(mod.funcs):
            allfns.append((mod, mod.funcs[a]))

    with write_if_changed(os.path.join(out, "funcs.h")) as h:
        h.write("// generated by gen/recomp.py - do not edit\n#pragma once\n#include \"recomp.h\"\n")
        for mod, fn in allfns:
            h.write(f"void {mod.fname(fn.entry)}(CPU* c);\n")

    nfiles = 0
    for k in range(0, len(allfns), args.chunk):
        with write_if_changed(os.path.join(out, f"funcs_{nfiles:02d}.c")) as f:
            f.write("// generated by gen/recomp.py - do not edit\n#include \"funcs.h\"\n\n")
            for mod, fn in allfns[k:k + args.chunk]:
                f.write(Emitter(mod, fn).run())
                f.write("\n\n")
        nfiles += 1

    # remove chunks left over from a previous run with more functions
    stale = nfiles
    while os.path.exists(os.path.join(out, f"funcs_{stale:02d}.c")):
        os.remove(os.path.join(out, f"funcs_{stale:02d}.c"))
        stale += 1

    with write_if_changed(os.path.join(out, "func_table.c")) as f:
        f.write("// generated by gen/recomp.py - do not edit\n#include \"funcs.h\"\n\n")
        f.write("const FuncEntry g_func_table[] = {\n")
        for mod, fn in sorted(allfns, key=lambda x: (x[1].entry, x[0].ovr)):
            ovr = mod.ovr
            lo, hi = fn.lo, fn.hi
            f.write(f"    {{ {fn.entry:#x}u, {ovr}, {lo:#x}u, {hi:#x}u, {mod.fname(fn.entry)} }},\n")
        f.write("};\n")
        f.write(f"const int g_func_table_count = {len(allfns)};\n")
        # executables: unpacked image extent + hash of the packed file on the disc, so the
        # runtime can verify the disc matches and knows which program is resident
        f.write("const OverlayInfo g_overlays[] = {\n")
        for mod in MODULES:
            f.write(f"    {{ {mod.ovr}, {mod.base:#x}u, {len(mod.data):#x}u, {fnv1a(mod.packed):#x}u, \"{mod.name}\" }},\n")
        f.write("};\n")
        f.write(f"const int g_overlay_count = {len(MODULES)};\n")

    with open(os.path.join(out, "sources.txt"), "w") as f:
        for n in range(nfiles):
            f.write(f"funcs_{n:02d}.c\n")
        f.write("func_table.c\n")

    with open(os.path.join(out, "recomp_report.txt"), "w") as rep:
        for mod in MODULES:
            nj = sum(len(fn.jumptables) for fn in mod.funcs.values())
            nd = sum(len(fn.dyn_jr) for fn in mod.funcs.values())
            rep.write(f"{mod.name}: {len(mod.funcs)} functions, {nj} jump tables, {nd} dynamic jr\n")
            for fn in mod.funcs.values():
                for a in sorted(fn.dyn_jr):
                    rep.write(f"   dyn jr at {a:08x} in {mod.fname(fn.entry)}\n")
    for mod in MODULES:
        print(f"{mod.name}: {len(mod.funcs)} functions")


class write_if_changed:
    """open(path, 'w')-like context manager that leaves the file untouched if unchanged."""

    def __init__(self, path):
        self.path = path
        self.parts = []

    def __enter__(self):
        return self

    def write(self, text):
        self.parts.append(text)

    def __exit__(self, *exc):
        data = "".join(self.parts)
        try:
            with open(self.path) as f:
                if f.read() == data:
                    return False
        except FileNotFoundError:
            pass
        with open(self.path, "w") as f:
            f.write(data)
        return False


def fnv1a(b):
    h = 0x811C9DC5
    for x in b:
        h = ((h ^ x) * 0x01000193) & 0xFFFFFFFF
    return h


if __name__ == "__main__":
    main()
