// MDEC (motion decoder): run-length decoding, IDCT and YUV->RGB of FMV macroblocks,
// following psx-spx. Commands and their parameters arrive through MDEC0 writes or DMA0;
// decoded pixels are read through DMA1 (hw.cpp holds a DMA1 transfer until enough output
// exists, since games usually start it before sending the input).
#include <algorithm>
#include <cstring>
#include <deque>
#include <vector>

#include "state.h"
#include "psx.h"

namespace psx {

void dma_mdec_out_ready();  // hw.cpp: retry a DMA1 transfer waiting for output

static const uint8_t zigzag[64] = {
    0,  1,  5,  6,  14, 15, 27, 28, 2,  4,  7,  13, 16, 26, 29, 42, 3,  8,  12, 17, 25, 30,
    41, 43, 9,  11, 18, 24, 31, 40, 44, 53, 10, 19, 23, 32, 39, 45, 52, 54, 20, 22, 33, 38,
    46, 51, 55, 60, 21, 34, 37, 47, 50, 56, 59, 61, 35, 36, 48, 49, 57, 58, 62, 63};
static uint8_t zagzig[64];

static uint8_t qt_y[64], qt_uv[64];
static int16_t scale[64];

static uint32_t cmd = 0;          // current command word
static uint32_t remaining = 0;    // parameter words still expected
static std::vector<uint32_t> params;
static std::deque<uint32_t> out;  // decoded output words
static bool en_in = false, en_out = false;

static int32_t signed10(uint32_t v) { return (int32_t)(v << 22) >> 22; }

// one 8x8 block: returns false when the stream ran out
static bool rl_decode_block(int16_t* blk, const uint16_t*& src, const uint16_t* end, const uint8_t* qt) {
    int32_t tmp[64] = {};
    uint16_t n;
    do {
        if (src >= end) return false;
        n = *src++;
    } while (n == 0xFE00);
    int q_scale = (n >> 10) & 0x3F;
    int k = 0;
    int32_t val = signed10(n & 0x3FF) * qt[0];
    for (;;) {
        if (q_scale == 0) val = signed10(n & 0x3FF) * 2;
        val = std::clamp(val, -0x400, 0x3FF);
        if (q_scale > 0) tmp[zagzig[k]] = val;
        else tmp[k] = val;
        if (src >= end) break;
        n = *src++;
        k += ((n >> 10) & 0x3F) + 1;
        if (k > 63) break;
        val = (signed10(n & 0x3FF) * qt[k] * q_scale + 4) / 8;
    }
    // IDCT: two passes of a separable 8-point transform with the game's scale table
    int64_t a[64], b[64];
    for (int i = 0; i < 64; i++) a[i] = tmp[i];
    int64_t* s = a;
    int64_t* d = b;
    for (int pass = 0; pass < 2; pass++) {
        for (int x = 0; x < 8; x++)
            for (int y = 0; y < 8; y++) {
                int64_t sum = 0;
                for (int z = 0; z < 8; z++) sum += s[y + z * 8] * (scale[x + z * 8] / 8);
                d[x + y * 8] = (sum + 0xFFF) >> 13;
            }
        std::swap(s, d);
    }
    for (int i = 0; i < 64; i++) blk[i] = (int16_t)std::clamp<int64_t>(s[i], -128, 127);
    return true;
}

static void put_color_macroblock(const int16_t* cr, const int16_t* cb, int16_t* const y[4]) {
    int depth = (cmd >> 27) & 3;
    bool sign = cmd & (1u << 26), bit15 = cmd & (1u << 25);
    uint32_t px[256];
    for (int yy = 0; yy < 16; yy++)
        for (int xx = 0; xx < 16; xx++) {
            int c = (xx >> 1) + (yy >> 1) * 8;
            double R = cr[c], B = cb[c];
            double G = -0.3437 * B - 0.7143 * R;
            R *= 1.402;
            B *= 1.772;
            int Y = y[(yy >> 3) * 2 + (xx >> 3)][(xx & 7) + (yy & 7) * 8];
            int r = std::clamp((int)(Y + R), -128, 127), g = std::clamp((int)(Y + G), -128, 127),
                b = std::clamp((int)(Y + B), -128, 127);
            if (!sign) { r += 128; g += 128; b += 128; }
            px[xx + yy * 16] = (uint8_t)r | ((uint8_t)g << 8) | ((uint8_t)b << 16);
        }
    if (depth == 2) {  // 24-bit: packed R,G,B bytes
        uint8_t bytes[768];
        for (int i = 0; i < 256; i++) {
            bytes[i * 3] = px[i] & 0xFF;
            bytes[i * 3 + 1] = (px[i] >> 8) & 0xFF;
            bytes[i * 3 + 2] = (px[i] >> 16) & 0xFF;
        }
        for (int i = 0; i < 192; i++) {
            uint32_t w;
            memcpy(&w, bytes + i * 4, 4);
            out.push_back(w);
        }
    } else {  // 15-bit
        for (int i = 0; i < 256; i += 2) {
            uint32_t h[2];
            for (int k = 0; k < 2; k++) {
                uint32_t p = px[i + k];
                h[k] = ((p >> 3) & 0x1F) | (((p >> 11) & 0x1F) << 5) | (((p >> 19) & 0x1F) << 10) | (bit15 ? 0x8000 : 0);
            }
            out.push_back(h[0] | (h[1] << 16));
        }
    }
}

static void decode_macroblocks() {
    const uint16_t* src = (const uint16_t*)params.data();
    const uint16_t* end = src + params.size() * 2;
    int depth = (cmd >> 27) & 3;
    bool sign = cmd & (1u << 26);
    int16_t blk[6][64];
    if (depth >= 2) {
        for (;;) {
            if (!rl_decode_block(blk[0], src, end, qt_uv) || !rl_decode_block(blk[1], src, end, qt_uv)) break;
            bool ok = true;
            for (int i = 2; i < 6 && ok; i++) ok = rl_decode_block(blk[i], src, end, qt_y);
            if (!ok) break;
            int16_t* y[4] = {blk[2], blk[3], blk[4], blk[5]};
            put_color_macroblock(blk[0], blk[1], y);
        }
    } else {
        while (rl_decode_block(blk[0], src, end, qt_y)) {
            uint8_t px[64];
            for (int i = 0; i < 64; i++) px[i] = (uint8_t)(blk[0][i] + (sign ? 0 : 128));
            if (depth == 1) {
                for (int i = 0; i < 64; i += 4) out.push_back(px[i] | (px[i + 1] << 8) | (px[i + 2] << 16) | ((uint32_t)px[i + 3] << 24));
            } else {
                for (int i = 0; i < 64; i += 8) {
                    uint32_t w = 0;
                    for (int k = 0; k < 8; k++) w |= (uint32_t)(px[i + k] >> 4) << (k * 4);
                    out.push_back(w);
                }
            }
        }
    }
}

static void command_done() {
    uint32_t op = cmd >> 29;
    const uint8_t* p = (const uint8_t*)params.data();
    if (op == 1) {
        decode_macroblocks();
        dma_mdec_out_ready();
    } else if (op == 2) {
        memcpy(qt_y, p, 64);
        if (cmd & 1) memcpy(qt_uv, p + 64, 64);
    } else if (op == 3) {
        memcpy(scale, p, 128);
    }
    params.clear();
}

static void write_word(uint32_t v) {
    if (remaining == 0) {
        cmd = v;
        params.clear();
        switch (v >> 29) {
        case 1: remaining = v & 0xFFFF; break;
        case 2: remaining = (v & 1) ? 32 : 16; break;
        case 3: remaining = 32; break;
        default: remaining = 0; break;
        }
        LOGD("MDEC command %08x (%u words)", v, remaining);
        if (remaining == 0) command_done();
        return;
    }
    params.push_back(v);
    if (--remaining == 0) command_done();
}

uint32_t mdec_read(int reg) {
    if (reg == 0) {
        if (out.empty()) return 0;
        uint32_t v = out.front();
        out.pop_front();
        return v;
    }
    uint32_t st = 0;
    if (out.empty()) st |= 1u << 31;
    if (remaining) st |= 1u << 29;
    if (en_in && remaining) st |= 1u << 28;
    if (en_out && !out.empty()) st |= 1u << 27;
    st |= ((cmd >> 25) & 0xF) << 23;  // output depth, signed, bit15
    st |= 4u << 16;                   // current block (Y)
    st |= (remaining - 1) & 0xFFFF;
    return st;
}

void mdec_write(int reg, uint32_t v) {
    if (reg == 0) { write_word(v); return; }
    if (v & 0x80000000u) {  // reset
        remaining = 0;
        params.clear();
        out.clear();
        cmd = 0;
    }
    en_in = v & (1u << 30);
    en_out = v & (1u << 29);
}

void mdec_dma_in(const uint32_t* src, int words) {
    for (int i = 0; i < words; i++) write_word(src[i]);
}

int mdec_out_available() { return (int)out.size(); }

void mdec_dma_out(uint32_t* dst, int words) {
    for (int i = 0; i < words; i++) {
        if (out.empty()) { dst[i] = 0; continue; }
        dst[i] = out.front();
        out.pop_front();
    }
}

void mdec_init() {
    for (int i = 0; i < 64; i++) zagzig[zigzag[i]] = (uint8_t)i;
    remaining = 0;
    params.clear();
    out.clear();
}

void mdec_state(StateIO& s) {
    s.section(0x4D444301);
    s.io(qt_y);
    s.io(qt_uv);
    s.io(scale);
    s.io(cmd);
    s.io(remaining);
    s.vec(params);
    s.deq(out);
    s.io(en_in);
    s.io(en_out);
}

}  // namespace psx
