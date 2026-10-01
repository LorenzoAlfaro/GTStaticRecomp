// Geometry Transformation Engine (COP2), following psx-spx semantics.
#include <algorithm>
#include <cstdint>
#include <cstring>

#include "pgxp.h"
#include "recomp.h"
#include "state.h"

namespace {

struct GTE {
    // data registers
    int16_t V[3][3];      // VX, VY, VZ for V0..V2
    uint8_t RGBC[4];
    uint16_t OTZ;
    int16_t IR[4];        // IR0..IR3
    int32_t SXY[3][2];    // FIFO SX/SY 0..2 (stored as int16 values)
    uint16_t SZ[4];       // SZ0..SZ3
    uint8_t RGB[3][4];    // RGB0..RGB2
    uint32_t RES1;
    int32_t MAC[4];
    uint32_t LZCS, LZCR;
    // control registers
    int16_t RT[3][3];
    int32_t TR[3];
    int16_t LLM[3][3];
    int32_t BK[3];
    int16_t LCM[3][3];
    int32_t FC[3];
    int32_t OFX, OFY;
    uint16_t H;
    int16_t DQA;
    int32_t DQB;
    int16_t ZSF3, ZSF4;
    uint32_t FLAG;
};

GTE g;
uint8_t unr_table[0x101];

struct UnrInit {
    UnrInit() {
        for (int i = 0; i < 0x101; i++)
            unr_table[i] = (uint8_t)std::max(0, (0x40000 / (i + 0x100) + 1) / 2 - 0x101);
    }
} unr_init;

inline void flag(int bit) { g.FLAG |= 1u << bit; }

// 44-bit MAC1..3 overflow check (index 1..3) and sign extension of the intermediate result
inline int64_t ext44(int i, int64_t v) {
    if (v > 0x7FFFFFFFFFFLL) flag(31 - i);         // 30,29,28
    else if (v < -0x80000000000LL) flag(28 - i);   // 27,26,25
    return (v << 20) >> 20;
}
inline void check_mac0(int64_t v) {
    if (v > 0x7FFFFFFFLL) flag(16);
    else if (v < -0x80000000LL) flag(15);
}
inline int32_t set_mac(int i, int64_t v, int shift) {
    ext44(i, v);
    g.MAC[i] = (int32_t)(v >> shift);
    return g.MAC[i];
}
inline int16_t sat_ir(int i, int32_t v, bool lm) {
    int32_t lo = lm ? 0 : -0x8000;
    if (v < lo) { flag(25 - i); v = lo; }        // 24,23,22
    else if (v > 0x7FFF) { flag(25 - i); v = 0x7FFF; }
    g.IR[i] = (int16_t)v;
    return g.IR[i];
}
inline void set_mac_ir(int i, int64_t v, int shift, bool lm) { sat_ir(i, set_mac(i, v, shift), lm); }

inline void push_sz(int64_t v) {
    if (v < 0) { flag(18); v = 0; }
    else if (v > 0xFFFF) { flag(18); v = 0xFFFF; }
    g.SZ[0] = g.SZ[1]; g.SZ[1] = g.SZ[2]; g.SZ[2] = g.SZ[3]; g.SZ[3] = (uint16_t)v;
}
inline void push_sxy(int32_t x, int32_t y) {
    if (x < -0x400) { flag(14); x = -0x400; } else if (x > 0x3FF) { flag(14); x = 0x3FF; }
    if (y < -0x400) { flag(13); y = -0x400; } else if (y > 0x3FF) { flag(13); y = 0x3FF; }
    g.SXY[0][0] = g.SXY[1][0]; g.SXY[0][1] = g.SXY[1][1];
    g.SXY[1][0] = g.SXY[2][0]; g.SXY[1][1] = g.SXY[2][1];
    g.SXY[2][0] = x; g.SXY[2][1] = y;
}
inline uint8_t sat_color(int bit, int32_t v) {
    if (v < 0) { flag(bit); return 0; }
    if (v > 255) { flag(bit); return 255; }
    return (uint8_t)v;
}
inline void push_rgb_from_mac() {
    for (int k = 0; k < 4; k++) { g.RGB[0][k] = g.RGB[1][k]; g.RGB[1][k] = g.RGB[2][k]; }
    g.RGB[2][0] = sat_color(21, g.MAC[1] >> 4);
    g.RGB[2][1] = sat_color(20, g.MAC[2] >> 4);
    g.RGB[2][2] = sat_color(19, g.MAC[3] >> 4);
    g.RGB[2][3] = g.RGBC[3];
}

uint32_t divide(uint16_t h, uint16_t sz3) {
    if ((uint32_t)h >= (uint32_t)sz3 * 2) { flag(17); return 0x1FFFF; }
    int z = __builtin_clz((uint32_t)sz3) - 16;
    uint32_t n = (uint32_t)h << z;
    uint32_t d = (uint32_t)sz3 << z;
    uint32_t u = unr_table[(d - 0x7FC0) >> 7] + 0x101;
    d = (0x2000080 - d * u) >> 8;
    d = (0x0000080 + d * u) >> 8;
    uint64_t q = ((uint64_t)n * d + 0x8000) >> 16;
    return (uint32_t)std::min<uint64_t>(q, 0x1FFFF);
}

// MAC_i = (T_i << 12 + M_i . V) >> shift, IR_i = sat
void mul_mat_vec(const int16_t M[3][3], const int16_t V[3], const int32_t* T, int shift, bool lm) {
    for (int i = 0; i < 3; i++) {
        int64_t acc = T ? (int64_t)T[i] << 12 : 0;
        acc = ext44(i + 1, acc + (int64_t)M[i][0] * V[0]);
        acc = ext44(i + 1, acc + (int64_t)M[i][1] * V[1]);
        acc = acc + (int64_t)M[i][2] * V[2];
        set_mac_ir(i + 1, acc, shift, lm);
    }
}

void interpolate_color(int64_t m1, int64_t m2, int64_t m3, int shift, bool lm) {
    int64_t in[3] = {m1, m2, m3};
    int32_t ir[3];
    for (int i = 0; i < 3; i++) {
        ir[i] = sat_ir(i + 1, set_mac(i + 1, ((int64_t)g.FC[i] << 12) - in[i], shift), false);
    }
    for (int i = 0; i < 3; i++) set_mac_ir(i + 1, (int64_t)ir[i] * g.IR[0] + in[i], shift, lm);
}

bool g_widescreen = false;

void rtp(int n, int shift, bool lm, bool last) {
    const int16_t* v = g.V[n];
    int64_t mac[3];
    for (int i = 0; i < 3; i++) {
        int64_t acc = (int64_t)g.TR[i] << 12;
        acc = ext44(i + 1, acc + (int64_t)g.RT[i][0] * v[0]);
        acc = ext44(i + 1, acc + (int64_t)g.RT[i][1] * v[1]);
        acc = ext44(i + 1, acc + (int64_t)g.RT[i][2] * v[2]);
        mac[i] = acc;
        g.MAC[i + 1] = (int32_t)(acc >> shift);
    }
    sat_ir(1, g.MAC[1], lm);
    sat_ir(2, g.MAC[2], lm);
    // IR3 quirk: flag from MAC3>>12, value from shifted MAC3
    {
        int32_t z12 = (int32_t)(mac[2] >> 12);
        if (z12 < -0x8000 || z12 > 0x7FFF) flag(22);
        int32_t lo = lm ? 0 : -0x8000;
        g.IR[3] = (int16_t)std::clamp<int32_t>(g.MAC[3], lo, 0x7FFF);
    }
    push_sz(mac[2] >> 12);
    uint32_t hs = divide(g.H, g.SZ[3]);
    // widescreen: projected X shrinks by 3/4 around the screen center, so a 16:9 field
    // of view fits in the game's 4:3 buffer, which the frontend shows stretched to 16:9
    int64_t sx = (int64_t)hs * g.IR[1];
    if (g_widescreen) sx = sx * 3 / 4;
    sx += g.OFX;
    int64_t sy = (int64_t)hs * g.IR[2] + g.OFY;
    check_mac0(sx);
    check_mac0(sy);
    uint32_t flag_before = g.FLAG;
    push_sxy((int32_t)(sx >> 16), (int32_t)(sy >> 16));
    // sub-pixel position of this vertex (no rounding of IR1/IR2 or SZ3, exact division)
    if (psx::g_pgxp && !((g.FLAG & ~flag_before) & ((1u << 17) | (1u << 14) | (1u << 13))) && mac[2] > 0 &&
        g.IR[1] == (int32_t)(mac[0] >> shift) && g.IR[2] == (int32_t)(mac[1] >> shift)) {
        double z = (double)mac[2];  // view Z << 12
        double scale = (double)g.H * 4096.0 / z;
        double px = (double)mac[0] / (double)(1 << shift) * scale * (g_widescreen ? 0.75 : 1.0) + g.OFX / 65536.0;
        double py = (double)mac[1] / (double)(1 << shift) * scale + g.OFY / 65536.0;
        int32_t ix = g.SXY[2][0], iy = g.SXY[2][1];
        if (px > ix - 2 && px < ix + 2 && py > iy - 2 && py < iy + 2)
            psx::pgxp_store((uint16_t)ix | ((uint32_t)(uint16_t)iy << 16), (float)px, (float)py, (float)(z / 4096.0));
    }
    if (last) {
        int64_t dq = (int64_t)g.DQA * hs + g.DQB;
        check_mac0(dq);
        g.MAC[0] = (int32_t)dq;
        int64_t ir0 = dq >> 12;
        if (ir0 < 0) { flag(12); ir0 = 0; } else if (ir0 > 0x1000) { flag(12); ir0 = 0x1000; }
        g.IR[0] = (int16_t)ir0;
    }
}

void ncs(int n, int shift, bool lm, int kind) {
    // kind: 0 = NC, 1 = NCC, 2 = NCD
    mul_mat_vec(g.LLM, g.V[n], nullptr, shift, lm);
    int16_t ir[3] = {g.IR[1], g.IR[2], g.IR[3]};
    mul_mat_vec(g.LCM, ir, g.BK, shift, lm);
    if (kind == 1) {
        for (int i = 0; i < 3; i++) set_mac_ir(i + 1, ((int64_t)g.RGBC[i] << 4) * g.IR[i + 1], shift, lm);
    } else if (kind == 2) {
        interpolate_color(((int64_t)g.RGBC[0] << 4) * g.IR[1], ((int64_t)g.RGBC[1] << 4) * g.IR[2],
                          ((int64_t)g.RGBC[2] << 4) * g.IR[3], shift, lm);
    }
    push_rgb_from_mac();
}

void mvmva(uint32_t cmd, int shift, bool lm) {
    int mx = (cmd >> 17) & 3, vi = (cmd >> 15) & 3, cv = (cmd >> 13) & 3;
    int16_t M[3][3];
    if (mx == 0) std::copy(&g.RT[0][0], &g.RT[0][0] + 9, &M[0][0]);
    else if (mx == 1) std::copy(&g.LLM[0][0], &g.LLM[0][0] + 9, &M[0][0]);
    else if (mx == 2) std::copy(&g.LCM[0][0], &g.LCM[0][0] + 9, &M[0][0]);
    else {
        int16_t r = (int16_t)(g.RGBC[0] << 4);
        int16_t m[3][3] = {{(int16_t)-r, r, g.IR[0]}, {g.RT[0][2], g.RT[0][2], g.RT[0][2]}, {g.RT[1][1], g.RT[1][1], g.RT[1][1]}};
        std::copy(&m[0][0], &m[0][0] + 9, &M[0][0]);
    }
    int16_t V[3];
    if (vi < 3) { V[0] = g.V[vi][0]; V[1] = g.V[vi][1]; V[2] = g.V[vi][2]; }
    else { V[0] = g.IR[1]; V[1] = g.IR[2]; V[2] = g.IR[3]; }
    int32_t zero[3] = {0, 0, 0};
    const int32_t* T = cv == 0 ? g.TR : cv == 1 ? g.BK : cv == 2 ? g.FC : zero;
    if (cv == 2) {
        // hardware bug: FC + first column only affect flags
        for (int i = 0; i < 3; i++) {
            int64_t t = ((int64_t)T[i] << 12) + (int64_t)M[i][0] * V[0];
            ext44(i + 1, t);
            sat_ir(i + 1, (int32_t)(t >> shift), false);
            int64_t acc = ext44(i + 1, (int64_t)M[i][1] * V[1]);
            acc = acc + (int64_t)M[i][2] * V[2];
            set_mac_ir(i + 1, acc, shift, lm);
        }
        return;
    }
    mul_mat_vec(M, V, T, shift, lm);
}

inline int32_t se16(uint32_t v) { return (int16_t)(v & 0xFFFF); }

} // namespace

extern "C" {

size_t gte_state_size() { return sizeof(GTE); }
void gte_save(void* dst) { memcpy(dst, &g, sizeof g); }
void gte_load(const void* src) { memcpy(&g, src, sizeof g); }

uint32_t gte_read_data(CPU*, int r) {
    switch (r) {
    case 0: return (uint16_t)g.V[0][0] | ((uint32_t)(uint16_t)g.V[0][1] << 16);
    case 1: return (uint32_t)(int32_t)g.V[0][2];
    case 2: return (uint16_t)g.V[1][0] | ((uint32_t)(uint16_t)g.V[1][1] << 16);
    case 3: return (uint32_t)(int32_t)g.V[1][2];
    case 4: return (uint16_t)g.V[2][0] | ((uint32_t)(uint16_t)g.V[2][1] << 16);
    case 5: return (uint32_t)(int32_t)g.V[2][2];
    case 6: return g.RGBC[0] | (g.RGBC[1] << 8) | (g.RGBC[2] << 16) | ((uint32_t)g.RGBC[3] << 24);
    case 7: return g.OTZ;
    case 8: case 9: case 10: case 11: return (uint32_t)(int32_t)g.IR[r - 8];
    case 12: case 13: case 14:
        return (uint16_t)g.SXY[r - 12][0] | ((uint32_t)(uint16_t)g.SXY[r - 12][1] << 16);
    case 15: return (uint16_t)g.SXY[2][0] | ((uint32_t)(uint16_t)g.SXY[2][1] << 16);
    case 16: case 17: case 18: case 19: return g.SZ[r - 16];
    case 20: case 21: case 22: {
        const uint8_t* c = g.RGB[r - 20];
        return c[0] | (c[1] << 8) | (c[2] << 16) | ((uint32_t)c[3] << 24);
    }
    case 23: return g.RES1;
    case 24: case 25: case 26: case 27: return (uint32_t)g.MAC[r - 24];
    case 28: case 29: {
        auto c5 = [](int16_t v) { return (uint32_t)std::clamp(v >> 7, 0, 0x1F); };
        return c5(g.IR[1]) | (c5(g.IR[2]) << 5) | (c5(g.IR[3]) << 10);
    }
    case 30: return g.LZCS;
    case 31: return g.LZCR;
    }
    return 0;
}

void gte_write_data(CPU*, int r, uint32_t v) {
    switch (r) {
    case 0: case 2: case 4: g.V[r / 2][0] = (int16_t)v; g.V[r / 2][1] = (int16_t)(v >> 16); break;
    case 1: case 3: case 5: g.V[r / 2][2] = (int16_t)v; break;
    case 6: g.RGBC[0] = v; g.RGBC[1] = v >> 8; g.RGBC[2] = v >> 16; g.RGBC[3] = v >> 24; break;
    case 7: g.OTZ = (uint16_t)v; break;
    case 8: case 9: case 10: case 11: g.IR[r - 8] = (int16_t)v; break;
    case 12: case 13: case 14: g.SXY[r - 12][0] = (int16_t)v; g.SXY[r - 12][1] = (int16_t)(v >> 16); break;
    case 15:
        g.SXY[0][0] = g.SXY[1][0]; g.SXY[0][1] = g.SXY[1][1];
        g.SXY[1][0] = g.SXY[2][0]; g.SXY[1][1] = g.SXY[2][1];
        g.SXY[2][0] = (int16_t)v; g.SXY[2][1] = (int16_t)(v >> 16);
        break;
    case 16: case 17: case 18: case 19: g.SZ[r - 16] = (uint16_t)v; break;
    case 20: case 21: case 22:
        g.RGB[r - 20][0] = v; g.RGB[r - 20][1] = v >> 8; g.RGB[r - 20][2] = v >> 16; g.RGB[r - 20][3] = v >> 24;
        break;
    case 23: g.RES1 = v; break;
    case 24: case 25: case 26: case 27: g.MAC[r - 24] = (int32_t)v; break;
    case 28:
        g.IR[1] = (int16_t)((v & 0x1F) << 7);
        g.IR[2] = (int16_t)(((v >> 5) & 0x1F) << 7);
        g.IR[3] = (int16_t)(((v >> 10) & 0x1F) << 7);
        break;
    case 29: break;
    case 30:
        g.LZCS = v;
        g.LZCR = (int32_t)v >= 0 ? (v ? __builtin_clz(v) : 32) : (~v ? __builtin_clz(~v) : 32);
        break;
    case 31: break;
    }
}

uint32_t gte_read_ctrl(CPU*, int r) {
    auto pack = [](int16_t a, int16_t b) { return (uint16_t)a | ((uint32_t)(uint16_t)b << 16); };
    auto m = [&](int16_t M[3][3], int k) -> uint32_t {
        switch (k) {
        case 0: return pack(M[0][0], M[0][1]);
        case 1: return pack(M[0][2], M[1][0]);
        case 2: return pack(M[1][1], M[1][2]);
        case 3: return pack(M[2][0], M[2][1]);
        default: return (uint32_t)(int32_t)M[2][2];
        }
    };
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: return m(g.RT, r);
    case 5: case 6: case 7: return (uint32_t)g.TR[r - 5];
    case 8: case 9: case 10: case 11: case 12: return m(g.LLM, r - 8);
    case 13: case 14: case 15: return (uint32_t)g.BK[r - 13];
    case 16: case 17: case 18: case 19: case 20: return m(g.LCM, r - 16);
    case 21: case 22: case 23: return (uint32_t)g.FC[r - 21];
    case 24: return (uint32_t)g.OFX;
    case 25: return (uint32_t)g.OFY;
    case 26: return (uint32_t)(int32_t)(int16_t)g.H;  // hardware quirk: reads sign-extended
    case 27: return (uint32_t)(int32_t)g.DQA;
    case 28: return (uint32_t)g.DQB;
    case 29: return (uint32_t)(int32_t)g.ZSF3;
    case 30: return (uint32_t)(int32_t)g.ZSF4;
    case 31: {
        uint32_t f = g.FLAG & 0x7FFFF000u;
        if (f & 0x7F87E000u) f |= 0x80000000u;
        return f;
    }
    }
    return 0;
}

void gte_write_ctrl(CPU*, int r, uint32_t v) {
    auto set = [&](int16_t M[3][3], int k) {
        switch (k) {
        case 0: M[0][0] = (int16_t)v; M[0][1] = (int16_t)(v >> 16); break;
        case 1: M[0][2] = (int16_t)v; M[1][0] = (int16_t)(v >> 16); break;
        case 2: M[1][1] = (int16_t)v; M[1][2] = (int16_t)(v >> 16); break;
        case 3: M[2][0] = (int16_t)v; M[2][1] = (int16_t)(v >> 16); break;
        default: M[2][2] = (int16_t)v; break;
        }
    };
    switch (r) {
    case 0: case 1: case 2: case 3: case 4: set(g.RT, r); break;
    case 5: case 6: case 7: g.TR[r - 5] = (int32_t)v; break;
    case 8: case 9: case 10: case 11: case 12: set(g.LLM, r - 8); break;
    case 13: case 14: case 15: g.BK[r - 13] = (int32_t)v; break;
    case 16: case 17: case 18: case 19: case 20: set(g.LCM, r - 16); break;
    case 21: case 22: case 23: g.FC[r - 21] = (int32_t)v; break;
    case 24: g.OFX = (int32_t)v; break;
    case 25: g.OFY = (int32_t)v; break;
    case 26: g.H = (uint16_t)v; break;
    case 27: g.DQA = (int16_t)v; break;
    case 28: g.DQB = (int32_t)v; break;
    case 29: g.ZSF3 = (int16_t)v; break;
    case 30: g.ZSF4 = (int16_t)v; break;
    case 31: g.FLAG = v & 0x7FFFF000u; break;
    }
}

void gte_command(CPU*, uint32_t cmd) {
    int shift = (cmd & (1u << 19)) ? 12 : 0;
    bool lm = (cmd & (1u << 10)) != 0;
    g.FLAG = 0;
    switch (cmd & 0x3F) {
    case 0x01: rtp(0, shift, lm, true); break;  // RTPS
    case 0x06: {  // NCLIP
        int64_t x0 = g.SXY[0][0], y0 = g.SXY[0][1], x1 = g.SXY[1][0], y1 = g.SXY[1][1];
        int64_t x2 = g.SXY[2][0], y2 = g.SXY[2][1];
        int64_t v = x0 * y1 + x1 * y2 + x2 * y0 - x0 * y2 - x1 * y0 - x2 * y1;
        check_mac0(v);
        g.MAC[0] = (int32_t)v;
        break;
    }
    case 0x0C: {  // OP
        int64_t d1 = g.RT[0][0], d2 = g.RT[1][1], d3 = g.RT[2][2];
        int64_t i1 = g.IR[1], i2 = g.IR[2], i3 = g.IR[3];
        set_mac_ir(1, i3 * d2 - i2 * d3, shift, lm);
        set_mac_ir(2, i1 * d3 - i3 * d1, shift, lm);
        set_mac_ir(3, i2 * d1 - i1 * d2, shift, lm);
        break;
    }
    case 0x10:  // DPCS
        interpolate_color((int64_t)g.RGBC[0] << 16, (int64_t)g.RGBC[1] << 16, (int64_t)g.RGBC[2] << 16, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x11:  // INTPL
        interpolate_color((int64_t)g.IR[1] << 12, (int64_t)g.IR[2] << 12, (int64_t)g.IR[3] << 12, shift, lm);
        push_rgb_from_mac();
        break;
    case 0x12: mvmva(cmd, shift, lm); break;
    case 0x13: ncs(0, shift, lm, 2); break;  // NCDS
    case 0x14: {  // CDP
        int16_t ir[3] = {g.IR[1], g.IR[2], g.IR[3]};
        mul_mat_vec(g.LCM, ir, g.BK, shift, lm);
        interpolate_color(((int64_t)g.RGBC[0] << 4) * g.IR[1], ((int64_t)g.RGBC[1] << 4) * g.IR[2],
                          ((int64_t)g.RGBC[2] << 4) * g.IR[3], shift, lm);
        push_rgb_from_mac();
        break;
    }
    case 0x16: for (int n = 0; n < 3; n++) ncs(n, shift, lm, 2); break;  // NCDT
    case 0x1B: ncs(0, shift, lm, 1); break;  // NCCS
    case 0x1C: {  // CC
        int16_t ir[3] = {g.IR[1], g.IR[2], g.IR[3]};
        mul_mat_vec(g.LCM, ir, g.BK, shift, lm);
        for (int i = 0; i < 3; i++) set_mac_ir(i + 1, ((int64_t)g.RGBC[i] << 4) * g.IR[i + 1], shift, lm);
        push_rgb_from_mac();
        break;
    }
    case 0x1E: ncs(0, shift, lm, 0); break;  // NCS
    case 0x20: for (int n = 0; n < 3; n++) ncs(n, shift, lm, 0); break;  // NCT
    case 0x28:  // SQR
        for (int i = 1; i <= 3; i++) set_mac_ir(i, (int64_t)g.IR[i] * g.IR[i], shift, lm);
        break;
    case 0x29:  // DCPL
        interpolate_color(((int64_t)g.RGBC[0] << 4) * g.IR[1], ((int64_t)g.RGBC[1] << 4) * g.IR[2],
                          ((int64_t)g.RGBC[2] << 4) * g.IR[3], shift, lm);
        push_rgb_from_mac();
        break;
    case 0x2A:  // DPCT
        for (int n = 0; n < 3; n++) {
            interpolate_color((int64_t)g.RGB[0][0] << 16, (int64_t)g.RGB[0][1] << 16, (int64_t)g.RGB[0][2] << 16, shift, lm);
            push_rgb_from_mac();
        }
        break;
    case 0x2D: {  // AVSZ3
        int64_t v = (int64_t)g.ZSF3 * ((int64_t)g.SZ[1] + g.SZ[2] + g.SZ[3]);
        check_mac0(v);
        g.MAC[0] = (int32_t)v;
        int64_t o = v >> 12;
        if (o < 0) { flag(18); o = 0; } else if (o > 0xFFFF) { flag(18); o = 0xFFFF; }
        g.OTZ = (uint16_t)o;
        break;
    }
    case 0x2E: {  // AVSZ4
        int64_t v = (int64_t)g.ZSF4 * ((int64_t)g.SZ[0] + g.SZ[1] + g.SZ[2] + g.SZ[3]);
        check_mac0(v);
        g.MAC[0] = (int32_t)v;
        int64_t o = v >> 12;
        if (o < 0) { flag(18); o = 0; } else if (o > 0xFFFF) { flag(18); o = 0xFFFF; }
        g.OTZ = (uint16_t)o;
        break;
    }
    case 0x30:  // RTPT
        rtp(0, shift, lm, false);
        rtp(1, shift, lm, false);
        rtp(2, shift, lm, true);
        break;
    case 0x3D:  // GPF
        for (int i = 1; i <= 3; i++) set_mac_ir(i, (int64_t)g.IR[0] * g.IR[i], shift, lm);
        push_rgb_from_mac();
        break;
    case 0x3E:  // GPL
        for (int i = 1; i <= 3; i++)
            set_mac_ir(i, ((int64_t)g.MAC[i] << shift) + (int64_t)g.IR[0] * g.IR[i], shift, lm);
        push_rgb_from_mac();
        break;
    case 0x3F: for (int n = 0; n < 3; n++) ncs(n, shift, lm, 1); break;  // NCCT
    default: break;
    }
}

}  // extern "C"

namespace psx {
void set_widescreen(bool on) { g_widescreen = on; }
bool widescreen() { return g_widescreen; }
void gte_state(StateIO& s) {
    s.section(0x47544501);
    s.io(g);
}

}  // namespace psx
