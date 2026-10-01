"""MIPS R3000A (PS1) instruction decoding shared by the recompiler and tools."""

REG = ["zero", "at", "v0", "v1", "a0", "a1", "a2", "a3",
       "t0", "t1", "t2", "t3", "t4", "t5", "t6", "t7",
       "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7",
       "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"]

SPECIAL = {
    0x00: "sll", 0x02: "srl", 0x03: "sra", 0x04: "sllv", 0x06: "srlv", 0x07: "srav",
    0x08: "jr", 0x09: "jalr", 0x0C: "syscall", 0x0D: "break",
    0x10: "mfhi", 0x11: "mthi", 0x12: "mflo", 0x13: "mtlo",
    0x18: "mult", 0x19: "multu", 0x1A: "div", 0x1B: "divu",
    0x20: "add", 0x21: "addu", 0x22: "sub", 0x23: "subu",
    0x24: "and", 0x25: "or", 0x26: "xor", 0x27: "nor", 0x2A: "slt", 0x2B: "sltu",
}

PRIMARY = {
    0x02: "j", 0x03: "jal", 0x04: "beq", 0x05: "bne", 0x06: "blez", 0x07: "bgtz",
    0x08: "addi", 0x09: "addiu", 0x0A: "slti", 0x0B: "sltiu", 0x0C: "andi", 0x0D: "ori",
    0x0E: "xori", 0x0F: "lui",
    0x20: "lb", 0x21: "lh", 0x22: "lwl", 0x23: "lw", 0x24: "lbu", 0x25: "lhu", 0x26: "lwr",
    0x28: "sb", 0x29: "sh", 0x2A: "swl", 0x2B: "sw", 0x2E: "swr",
    0x32: "lwc2", 0x3A: "swc2",
}

BRANCHES = {"beq", "bne", "blez", "bgtz", "bltz", "bgez", "bltzal", "bgezal"}
LOADS = {"lb", "lh", "lwl", "lw", "lbu", "lhu", "lwr"}
STORES = {"sb", "sh", "swl", "sw", "swr"}


class Insn:
    __slots__ = ("addr", "word", "op", "rs", "rt", "rd", "sa", "imm", "simm", "target", "funct")

    def __init__(self, addr, word):
        self.addr = addr
        self.word = word
        opc = word >> 26
        self.rs = (word >> 21) & 31
        self.rt = (word >> 16) & 31
        self.rd = (word >> 11) & 31
        self.sa = (word >> 6) & 31
        self.funct = word & 63
        self.imm = word & 0xFFFF
        self.simm = self.imm - 0x10000 if self.imm & 0x8000 else self.imm
        self.target = None
        if opc == 0:
            self.op = SPECIAL.get(self.funct)
        elif opc == 1:
            self.op = {0x00: "bltz", 0x01: "bgez", 0x10: "bltzal", 0x11: "bgezal"}.get(self.rt)
            if self.op is None:
                # R3000 decodes any REGIMM rt: bit0 = gez, bits 4..1 == 1000 => link
                self.op = ("bgez" if self.rt & 1 else "bltz") + ("al" if (self.rt & 0x1E) == 0x10 else "")
        elif opc == 0x10:
            self.op = self._cop0()
        elif opc == 0x12:
            self.op = self._cop2()
        else:
            self.op = PRIMARY.get(opc)
        if self.op in ("j", "jal"):
            self.target = ((addr + 4) & 0xF0000000) | ((word & 0x3FFFFFF) << 2)
        elif self.op in BRANCHES:
            self.target = (addr + 4 + (self.simm << 2)) & 0xFFFFFFFF

    def _cop0(self):
        if self.rs == 0x00:
            return "mfc0"
        if self.rs == 0x04:
            return "mtc0"
        if self.rs == 0x10 and self.funct == 0x10:
            return "rfe"
        return None

    def _cop2(self):
        if self.rs & 0x10:
            return "cop2"
        return {0x00: "mfc2", 0x02: "cfc2", 0x04: "mtc2", 0x06: "ctc2"}.get(self.rs)

    @property
    def valid(self):
        return self.op is not None

    @property
    def is_branch(self):
        return self.op in BRANCHES

    @property
    def has_delay(self):
        return self.op in BRANCHES or self.op in ("j", "jal", "jr", "jalr")

    def writes(self):
        """GPR written by this instruction (or None)."""
        op = self.op
        if op in ("sll", "srl", "sra", "sllv", "srlv", "srav", "mfhi", "mflo", "add", "addu",
                  "sub", "subu", "and", "or", "xor", "nor", "slt", "sltu", "jalr"):
            return self.rd or None
        if op in ("addi", "addiu", "slti", "sltiu", "andi", "ori", "xori", "lui",
                  "mfc0", "mfc2", "cfc2") or op in LOADS:
            return self.rt or None
        if op in ("jal", "bltzal", "bgezal"):
            return 31
        return None

    def __str__(self):
        return disasm(self)


def disasm(i):
    op = i.op
    r = REG
    if op is None:
        return f".word 0x{i.word:08x}"
    if i.word == 0:
        return "nop"
    if op in ("sll", "srl", "sra"):
        return f"{op} {r[i.rd]}, {r[i.rt]}, {i.sa}"
    if op in ("sllv", "srlv", "srav"):
        return f"{op} {r[i.rd]}, {r[i.rt]}, {r[i.rs]}"
    if op == "jr":
        return f"jr {r[i.rs]}"
    if op == "jalr":
        return f"jalr {r[i.rd]}, {r[i.rs]}"
    if op in ("syscall", "break"):
        return f"{op} 0x{(i.word >> 6) & 0xFFFFF:x}"
    if op in ("mfhi", "mflo"):
        return f"{op} {r[i.rd]}"
    if op in ("mthi", "mtlo"):
        return f"{op} {r[i.rs]}"
    if op in ("mult", "multu", "div", "divu"):
        return f"{op} {r[i.rs]}, {r[i.rt]}"
    if op in SPECIAL.values():
        return f"{op} {r[i.rd]}, {r[i.rs]}, {r[i.rt]}"
    if op in ("j", "jal"):
        return f"{op} 0x{i.target:08x}"
    if op in ("beq", "bne"):
        return f"{op} {r[i.rs]}, {r[i.rt]}, 0x{i.target:08x}"
    if op in BRANCHES:
        return f"{op} {r[i.rs]}, 0x{i.target:08x}"
    if op == "lui":
        return f"lui {r[i.rt]}, 0x{i.imm:x}"
    if op in ("andi", "ori", "xori"):
        return f"{op} {r[i.rt]}, {r[i.rs]}, 0x{i.imm:x}"
    if op in ("addi", "addiu", "slti", "sltiu"):
        return f"{op} {r[i.rt]}, {r[i.rs]}, {i.simm}"
    if op in LOADS or op in STORES:
        return f"{op} {r[i.rt]}, {i.simm}({r[i.rs]})"
    if op in ("lwc2", "swc2"):
        return f"{op} ${i.rt}, {i.simm}({r[i.rs]})"
    if op in ("mfc0", "mtc0", "mfc2", "mtc2", "cfc2", "ctc2"):
        return f"{op} {r[i.rt]}, ${i.rd}"
    if op == "rfe":
        return "rfe"
    if op == "cop2":
        return f"cop2 0x{i.word & 0x1FFFFFF:07x}"
    return op
