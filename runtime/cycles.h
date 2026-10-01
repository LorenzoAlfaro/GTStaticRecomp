// Instruction timing model shared by the interpreter. MUST match cycles() in gen/recomp.py
// so recompiled and interpreted execution are cycle-identical (lockstep debugging).
#pragma once
#include <stdint.h>

static inline int gte_cmd_cycles(uint32_t w) {
    switch (w & 0x3F) {
    case 0x01: return 15; case 0x06: return 8;  case 0x0C: return 6;  case 0x10: return 8;
    case 0x11: return 8;  case 0x12: return 8;  case 0x13: return 19; case 0x14: return 13;
    case 0x16: return 44; case 0x1B: return 17; case 0x1C: return 11; case 0x1E: return 14;
    case 0x20: return 30; case 0x28: return 5;  case 0x29: return 8;  case 0x2A: return 17;
    case 0x2D: return 5;  case 0x2E: return 6;  case 0x30: return 23; case 0x3D: return 5;
    case 0x3E: return 5;  case 0x3F: return 39;
    default: return 8;
    }
}

static inline int insn_cycles(uint32_t w) {
    uint32_t op = w >> 26;
    if ((op >= 0x20 && op <= 0x26) || op == 0x32) return 5;  // loads (incl. lwc2)
    if (op == 0) {
        uint32_t f = w & 63;
        if (f == 0x18 || f == 0x19) return 7;   // mult/multu
        if (f == 0x1A || f == 0x1B) return 36;  // div/divu
    }
    if (op == 0x12 && (w & (1u << 25))) return gte_cmd_cycles(w);
    return 1;
}
