// PS1 GPU: command processing and a software rasterizer.
//
// VRAM is stored at an internal resolution scale S (1, 2, 4 or 8): every native VRAM pixel
// is an SxS block. CPU uploads write whole blocks; CPU downloads, texture and CLUT fetches
// read the top-left sample of a block, so the emulated machine sees exactly the native
// 1024x512 VRAM. Polygons, rectangles and lines are rasterized at the full internal
// resolution, with sub-pixel vertex positions from the GTE when PGXP is enabled.
// At S=1 without PGXP the output is identical to a native-resolution renderer.
#include <immintrin.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#include "pgxp.h"
#include "psx.h"

namespace psx {

static int sh = 0;                    // log2(scale)
static int VW = 1024, VH = 512;       // internal VRAM size
static std::vector<uint16_t> vram(1024 * 512);
static int dither_mode = 1;           // 0 off, 1 native pattern (scaled up)

// ---- state -----------------------------------------------------------------------
static uint32_t stat_texpage = 0;   // GP0 E1 bits 0..10 (+ texture disable bit 11)
static bool tex_disable = false;
static bool allow_tex_disable = false;  // GP1(09h): texture disable only works when allowed
static bool dither = false;
static bool draw_to_display = false;
static bool mask_set = false, mask_check = false;
static uint32_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
static int da_x1, da_y1, da_x2, da_y2;  // drawing area (inclusive)
static int off_x, off_y;
static bool display_disabled = true;
static int dma_dir = 0;
static int disp_x = 0, disp_y = 0;
static int h_start = 0x200, h_end = 0xC00, v_start = 0x10, v_end = 0x100;
static uint32_t disp_mode = 0;
static bool irq_flag = false;
static bool vblank_now = false;

// command FIFO
static uint32_t cmd[64];
static int cmd_len = 0, cmd_need = 0;
static bool polyline = false;
static struct {
    uint32_t op, prev_v, prev_c, cur_c;
    bool gour, have_prev, want_color;
} pl;

// VRAM transfers
static struct {
    bool active = false;
    int x, y, w, h, cx, cy;
} upload, download;

// ---- VRAM access -------------------------------------------------------------------
// internal-resolution pixel
static inline uint16_t& hpx(int x, int y) {
    return vram[((size_t)((unsigned)y & (unsigned)(VH - 1)) << (10 + sh)) | ((unsigned)x & (unsigned)(VW - 1))];
}
// native pixel (top-left sample of its block)
static inline uint16_t& px(int x, int y) { return hpx((int)((unsigned)x << sh), (int)((unsigned)y << sh)); }
// write a whole native pixel block
static inline void px_fill(int x, int y, uint16_t v) {
    int s = 1 << sh;
    for (int j = 0; j < s; j++) {
        uint16_t* row = &hpx((int)((unsigned)x << sh), (int)((unsigned)y << sh) + j);
        for (int i = 0; i < s; i++) row[i] = v;
    }
}

constexpr int SUB = 8;  // sub-pixel bits of vertex positions (internal-resolution pixels)

struct Vertex {
    int32_t x, y;  // internal-resolution position, SUB fractional bits
    int nx, ny;    // native integer position (for the GPU's size limits)
    int r, g, b;
    int u, v;
};

// Everything a primitive needs, captured when it is queued (workers render it later).
struct DrawCtx {
    bool textured, raw, semi, gouraud;
    int semi_mode;
    int tp_x, tp_y, tp_depth;  // texpage base (pixels) and depth 0=4bit 1=8bit 2=15bit
    int clut_x, clut_y;
    bool do_dither;
    bool mask_set, mask_check;
    uint8_t tw_mask_x, tw_mask_y, tw_off_x, tw_off_y;
    int16_t da_x1, da_y1, da_x2, da_y2;
};

// Rows of the internal-resolution VRAM are split into bands of 8 lines, dealt round-robin
// to the render workers; a worker only touches its own rows, so every pixel still sees
// its primitives in submission order.
struct Band {
    int id, n;
    bool mine(int y) const { return n == 1 || ((unsigned)(y >> 3) % (unsigned)n) == (unsigned)id; }
};

static const int dither_tab[4][4] = {{-4, 0, -3, 1}, {2, -2, 3, -1}, {-3, 1, -4, 0}, {3, -1, 2, -2}};

static inline uint16_t sample_tex(const DrawCtx& d, int u, int v) {
    u &= 0xFF;
    v &= 0xFF;
    u = (u & ~(d.tw_mask_x * 8)) | ((d.tw_off_x & d.tw_mask_x) * 8);
    v = (v & ~(d.tw_mask_y * 8)) | ((d.tw_off_y & d.tw_mask_y) * 8);
    switch (d.tp_depth) {
    case 0: {
        uint16_t w = px(d.tp_x + (u >> 2), d.tp_y + v);
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return px(d.clut_x + idx, d.clut_y);
    }
    case 1: {
        uint16_t w = px(d.tp_x + (u >> 1), d.tp_y + v);
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return px(d.clut_x + idx, d.clut_y);
    }
    default:
        return px(d.tp_x + u, d.tp_y + v);
    }
}

// x, y: internal-resolution destination
static inline void plot(const DrawCtx& d, int x, int y, int r, int g, int b, int u, int v) {
    uint16_t& dst = hpx(x, y);
    if (d.mask_check && (dst & 0x8000)) return;
    uint16_t out;
    bool semi_px = d.semi;
    if (d.textured) {
        uint16_t t = sample_tex(d, u, v);
        if (t == 0) return;
        semi_px = d.semi && (t & 0x8000);
        int tr = t & 31, tg = (t >> 5) & 31, tb = (t >> 10) & 31;
        if (!d.raw) {
            if (d.do_dither) {
                int dt = dither_tab[(y >> sh) & 3][(x >> sh) & 3];
                int rr = std::clamp(((tr << 3) * r >> 7) + dt, 0, 255);
                int gg = std::clamp(((tg << 3) * g >> 7) + dt, 0, 255);
                int bb = std::clamp(((tb << 3) * b >> 7) + dt, 0, 255);
                tr = rr >> 3; tg = gg >> 3; tb = bb >> 3;
            } else {
                tr = std::min(31, tr * r >> 7);
                tg = std::min(31, tg * g >> 7);
                tb = std::min(31, tb * b >> 7);
            }
        }
        out = (uint16_t)(tr | (tg << 5) | (tb << 10) | (t & 0x8000));
    } else {
        if (d.do_dither) {
            int dt = dither_tab[(y >> sh) & 3][(x >> sh) & 3];
            r = std::clamp(r + dt, 0, 255);
            g = std::clamp(g + dt, 0, 255);
            b = std::clamp(b + dt, 0, 255);
        }
        out = (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10));
    }
    if (semi_px) {
        int br = dst & 31, bg = (dst >> 5) & 31, bb = (dst >> 10) & 31;
        int fr = out & 31, fg = (out >> 5) & 31, fb = (out >> 10) & 31;
        switch (d.semi_mode) {
        case 0: fr = (br + fr) >> 1; fg = (bg + fg) >> 1; fb = (bb + fb) >> 1; break;
        case 1: fr = std::min(31, br + fr); fg = std::min(31, bg + fg); fb = std::min(31, bb + fb); break;
        case 2: fr = std::max(0, br - fr); fg = std::max(0, bg - fg); fb = std::max(0, bb - fb); break;
        default: fr = std::min(31, br + (fr >> 2)); fg = std::min(31, bg + (fg >> 2)); fb = std::min(31, bb + (fb >> 2)); break;
        }
        out = (uint16_t)(fr | (fg << 5) | (fb << 10) | (out & 0x8000));
    }
    if (d.mask_set) out |= 0x8000;
    dst = out;
}

static DrawCtx make_ctx(uint32_t op, uint32_t texpage_word, uint32_t clut_word, bool textured) {
    DrawCtx d{};
    d.textured = textured && !tex_disable;
    d.raw = d.textured && (op & 1);
    d.semi = op & 2;
    uint32_t tp = textured ? texpage_word : stat_texpage;
    d.semi_mode = (tp >> 5) & 3;
    d.tp_x = (tp & 0xF) * 64;
    d.tp_y = ((tp >> 4) & 1) * 256;
    d.tp_depth = std::min<int>((tp >> 7) & 3, 2);
    d.clut_x = (clut_word & 0x3F) * 16;
    d.clut_y = (clut_word >> 6) & 0x1FF;
    d.mask_set = mask_set;
    d.mask_check = mask_check;
    d.tw_mask_x = (uint8_t)tw_mask_x; d.tw_mask_y = (uint8_t)tw_mask_y;
    d.tw_off_x = (uint8_t)tw_off_x; d.tw_off_y = (uint8_t)tw_off_y;
    d.da_x1 = (int16_t)da_x1; d.da_y1 = (int16_t)da_y1; d.da_x2 = (int16_t)da_x2; d.da_y2 = (int16_t)da_y2;
    return d;
}

// ---- triangles ------------------------------------------------------------------------
static inline int64_t edge(const Vertex& a, const Vertex& b, int64_t x, int64_t y) {
    return (int64_t)(b.x - a.x) * (y - a.y) - (int64_t)(b.y - a.y) * (x - a.x);
}

static inline int ceil_sub(int32_t v) { return (int)((v + (1 << SUB) - 1) >> SUB); }

static void draw_triangle(const DrawCtx& d, Vertex v0, Vertex v1, Vertex v2, Band band) {
    int64_t area = edge(v0, v1, v2.x, v2.y);
    if (area == 0) return;
    if (area < 0) { std::swap(v1, v2); area = -area; }
    // sample points are integer internal-resolution coordinates
    int minx = ceil_sub(std::min({v0.x, v1.x, v2.x})), maxx = ceil_sub(std::max({v0.x, v1.x, v2.x}));
    int miny = ceil_sub(std::min({v0.y, v1.y, v2.y})), maxy = ceil_sub(std::max({v0.y, v1.y, v2.y}));
    minx = std::max(minx, d.da_x1 << sh); maxx = std::min(maxx, (d.da_x2 + 1) << sh);
    miny = std::max(miny, d.da_y1 << sh); maxy = std::min(maxy, (d.da_y2 + 1) << sh);
    if (minx >= maxx || miny >= maxy) return;

    // attribute gradients per internal pixel (fixed 16.16)
    const int64_t lim = (int64_t)1 << 40;
    auto grad = [&](int a0, int a1, int a2, int64_t& ddx, int64_t& ddy) {
        int64_t dx1 = v1.x - v0.x, dy1 = v1.y - v0.y, dx2 = v2.x - v0.x, dy2 = v2.y - v0.y;
        int64_t da1 = a1 - a0, da2 = a2 - a0;
        int64_t den = dx1 * dy2 - dx2 * dy1;
        ddx = std::clamp(((da1 * dy2 - da2 * dy1) << (16 + SUB)) / den, -lim, lim);
        ddy = std::clamp(((da2 * dx1 - da1 * dx2) << (16 + SUB)) / den, -lim, lim);
    };
    int64_t rdx = 0, rdy = 0, gdx = 0, gdy = 0, bdx = 0, bdy = 0, udx = 0, udy = 0, vdx = 0, vdy = 0;
    if (d.gouraud) {
        grad(v0.r, v1.r, v2.r, rdx, rdy);
        grad(v0.g, v1.g, v2.g, gdx, gdy);
        grad(v0.b, v1.b, v2.b, bdx, bdy);
    }
    if (d.textured) {
        grad(v0.u, v1.u, v2.u, udx, udy);
        grad(v0.v, v1.v, v2.v, vdx, vdy);
    }
    // top-left rule: an edge is "top" or "left" if ... (with CCW positive area in y-down space)
    auto is_tl = [](const Vertex& a, const Vertex& b) {
        int dy = b.y - a.y, dx = b.x - a.x;
        return dy < 0 || (dy == 0 && dx > 0);
    };
    int bias0 = is_tl(v1, v2) ? 0 : -1;
    int bias1 = is_tl(v2, v0) ? 0 : -1;
    int bias2 = is_tl(v0, v1) ? 0 : -1;
    const int64_t one = 1 << SUB;
    int64_t s0 = -(int64_t)(v2.y - v1.y) * one, s1 = -(int64_t)(v0.y - v2.y) * one, s2 = -(int64_t)(v1.y - v0.y) * one;

    // attribute value at (x, y) = a0 + (ddx * (x - x0) + ddy * (y - y0)), positions in SUB units
    auto start = [&](int a0, int64_t ddx, int64_t ddy, int64_t fx, int64_t fy) {
        return ((int64_t)a0 << 16) + ((ddx * fx + ddy * fy) >> SUB) + 0x8000;
    };
    for (int y = miny; y < maxy; y++) {
        if (!band.mine(y)) continue;
        int64_t sx = (int64_t)minx << SUB, sy = (int64_t)y << SUB;
        int64_t w0 = edge(v1, v2, sx, sy), w1 = edge(v2, v0, sx, sy), w2 = edge(v0, v1, sx, sy);
        // skip the empty span left of the triangle on this row
        int x = minx;
        auto first_inside = [&](int64_t w, int64_t s, int bias) -> int64_t {
            if (w + bias >= 0) return 0;
            if (s <= 0) return INT64_MAX;
            return (-(w + bias) + s - 1) / s;
        };
        int64_t skip = std::max({first_inside(w0, s0, bias0), first_inside(w1, s1, bias1), first_inside(w2, s2, bias2)});
        if (skip >= maxx - minx) continue;
        x += (int)skip;
        w0 += s0 * skip; w1 += s1 * skip; w2 += s2 * skip;
        int64_t fx = ((int64_t)x << SUB) - v0.x, fy = sy - v0.y;
        int64_t ra = 0, ga = 0, ba = 0, ua = 0, va = 0;
        if (d.gouraud) {
            ra = start(v0.r, rdx, rdy, fx, fy);
            ga = start(v0.g, gdx, gdy, fx, fy);
            ba = start(v0.b, bdx, bdy, fx, fy);
        }
        if (d.textured) {
            ua = start(v0.u, udx, udy, fx, fy);
            va = start(v0.v, vdx, vdy, fx, fy);
        }
        for (; x < maxx; x++, w0 += s0, w1 += s1, w2 += s2, ra += rdx, ga += gdx, ba += bdx, ua += udx, va += vdx) {
            if (w0 + bias0 < 0 || w1 + bias1 < 0 || w2 + bias2 < 0) {
                // convex: once inside, leaving means the span is done
                if (x > minx + skip) break;
                continue;
            }
            int r = v0.r, g = v0.g, b = v0.b, u = 0, v = 0;
            if (d.gouraud) {
                r = std::clamp((int)(ra >> 16), 0, 255);
                g = std::clamp((int)(ga >> 16), 0, 255);
                b = std::clamp((int)(ba >> 16), 0, 255);
            }
            if (d.textured) {
                u = std::clamp((int)(ua >> 16), 0, 255);
                v = std::clamp((int)(va >> 16), 0, 255);
            }
            plot(d, x, y, r, g, b, u, v);
        }
    }
}

// ---- rectangles / lines / fills ---------------------------------------------------------
static void draw_rect(const DrawCtx& d, int x, int y, int w, int h, int r, int g, int b, int u0, int v0, Band band) {
    int s = 1 << sh;
    int x0 = std::max(x, (int)d.da_x1) * s, y0 = std::max(y, (int)d.da_y1) * s;
    int x1 = std::min(x + w, d.da_x2 + 1) * s, y1 = std::min(y + h, d.da_y2 + 1) * s;
    DrawCtx dd = d;
    dd.do_dither = false;  // rectangles are never dithered
    for (int yy = y0; yy < y1; yy++) {
        if (!band.mine(yy)) continue;
        for (int xx = x0; xx < x1; xx++)
            plot(dd, xx, yy, r, g, b, u0 + ((xx - x * s) >> sh), v0 + ((yy - y * s) >> sh));
    }
}

static void draw_line(const DrawCtx& d, Vertex a, Vertex b, Band band) {
    int dx = b.nx - a.nx, dy = b.ny - a.ny;
    if (std::abs(dx) >= 1024 || std::abs(dy) >= 512) return;
    int n = std::max(std::abs(dx), std::abs(dy));
    int s = 1 << sh;
    for (int i = 0; i <= n; i++) {
        int x = n ? a.nx + dx * i / n : a.nx;
        int y = n ? a.ny + dy * i / n : a.ny;
        if (x < d.da_x1 || x > d.da_x2 || y < d.da_y1 || y > d.da_y2) continue;
        int r = a.r, g = a.g, bb = a.b;
        if (d.gouraud && n) {
            r = a.r + (b.r - a.r) * i / n;
            g = a.g + (b.g - a.g) * i / n;
            bb = a.b + (b.b - a.b) * i / n;
        }
        for (int j = 0; j < s; j++) {
            if (!band.mine(y * s + j)) continue;
            for (int k = 0; k < s; k++) plot(d, x * s + k, y * s + j, r, g, bb, 0, 0);
        }
    }
}

static void do_fill(int x, int y, int w, int h, uint16_t col, Band band) {
    int s = 1 << sh;
    for (int yy = 0; yy < h * s; yy++) {
        int hy = (y * s + yy) & (VH - 1);
        if (!band.mine(hy)) continue;
        for (int xx = 0; xx < w * s; xx++) hpx(x * s + xx, hy) = col;
    }
}

// ---- render queue -------------------------------------------------------------------------
// At internal scales above 1x primitives are queued and rasterized by worker threads while
// the emulated CPU keeps running. The queue is drained (gpu_sync) before anything reads
// VRAM on the emulation thread, and before queuing a primitive whose texture/CLUT area may
// have been written by primitives still in the queue (render-to-texture).
enum JobKind : uint8_t { JOB_TRI, JOB_RECT, JOB_LINE, JOB_FILL, JOB_CONVERT, JOB_COPY };
struct Job {
    JobKind kind;
    DrawCtx d;
    Vertex v[3];  // triangle / line vertices; rect: v[0].nx/ny pos, v[1].nx/ny size, v[0].u/v
    uint32_t* dst;  // JOB_CONVERT: ARGB output (v[0] = internal-res origin, v[1] = size)
    int pitch;      // JOB_COPY: v[0] = src, v[1] = dst, v[2] = size (native, nx/ny)
};

static void do_copy(const Job& j, Band band) {
    int sx = j.v[0].nx, sy = j.v[0].ny, dx = j.v[1].nx, dy = j.v[1].ny, w = j.v[2].nx, h = j.v[2].ny;
    int s = 1 << sh;
    for (int yy = 0; yy < h * s; yy++) {
        if (!band.mine((dy * s + yy) & (VH - 1))) continue;
        for (int xx = 0; xx < w * s; xx++) {
            uint16_t src = hpx(sx * s + xx, sy * s + yy);
            uint16_t& d = hpx(dx * s + xx, dy * s + yy);
            if (j.d.mask_check && (d & 0x8000)) continue;
            d = src | (j.d.mask_set ? 0x8000 : 0);
        }
    }
}

// 15-bit VRAM pixel -> ARGB8888
static uint32_t rgb_lut[32768];
static struct LutInit {
    LutInit() {
        for (uint32_t p = 0; p < 32768; p++) {
            uint32_t r = (p & 31) << 3, g = ((p >> 5) & 31) << 3, b = ((p >> 10) & 31) << 3;
            r |= r >> 5; g |= g >> 5; b |= b >> 5;
            rgb_lut[p] = 0xFF000000u | (r << 16) | (g << 8) | b;
        }
    }
} lut_init;

static void do_convert(const Job& j, Band band) {
    int x0 = j.v[0].nx, y0 = j.v[0].ny, w = j.v[1].nx, h = j.v[1].ny;
    for (int y = 0; y < h; y++) {
        int vy = (y0 + y) & (VH - 1);
        if (!band.mine(vy)) continue;
        const uint16_t* row = &vram[(size_t)vy << (10 + sh)];
        uint32_t* out = j.dst + (size_t)y * j.pitch;
        if (x0 + w <= VW) {
            row += x0;
            for (int x = 0; x < w; x++) out[x] = rgb_lut[row[x] & 0x7FFF];
        } else {
            for (int x = 0; x < w; x++) out[x] = rgb_lut[row[(x0 + x) & (VW - 1)] & 0x7FFF];
        }
    }
}

static constexpr int kQueueSize = 1 << 14;
static Job queue[kQueueSize];
static std::atomic<uint64_t> q_head{0};  // jobs published
static int n_workers = 0;
struct alignas(64) WorkerState {
    std::atomic<uint64_t> done{0};
    std::thread th;
};
static WorkerState* workers = nullptr;
static std::atomic<bool> workers_quit{false};
static std::mutex q_mutex;
static std::condition_variable q_cv;
static std::atomic<int> q_sleepers{0};
static bool threaded = false;  // queue in use (scale > 1 and workers available)

// Hazard tracking between the emulation thread and the workers: VRAM areas written and read
// by queued jobs since the last sync, in 16x8 native-pixel tiles (8 rows, so a framebuffer
// ending at line 215 does not overlap textures stored from line 216). A job is queued only if
// it reads nothing pending to be written and writes nothing pending to be read; otherwise
// the queue is drained first. Coordinates wrap around VRAM like the hardware does.
struct Rgn { int x0, y0, x1, y1; };  // native, exclusive end
struct TileMap {
    uint64_t bits[64];
    bool any;
    static void norm(Rgn& r) {
        if (r.x1 - r.x0 >= 1024) { r.x0 = 0; r.x1 = 1024; }
        if (r.y1 - r.y0 >= 512) { r.y0 = 0; r.y1 = 512; }
    }
    void mark(Rgn r) {
        norm(r);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) return;
        for (int ty = r.y0 >> 3; ty <= (r.y1 - 1) >> 3; ty++)
            for (int tx = r.x0 >> 4; tx <= (r.x1 - 1) >> 4; tx++) bits[ty & 63] |= 1ull << (tx & 63);
        any = true;
    }
    bool test(Rgn r) const {
        if (!any) return false;
        norm(r);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) return false;
        for (int ty = r.y0 >> 3; ty <= (r.y1 - 1) >> 3; ty++)
            for (int tx = r.x0 >> 4; tx <= (r.x1 - 1) >> 4; tx++)
                if (bits[ty & 63] & (1ull << (tx & 63))) return true;
        return false;
    }
    void clear() { memset(bits, 0, sizeof bits); any = false; }
};
static TileMap pend_write, pend_read;

static void run_job(const Job& j, Band band) {
    switch (j.kind) {
    case JOB_TRI: draw_triangle(j.d, j.v[0], j.v[1], j.v[2], band); break;
    case JOB_RECT:
        draw_rect(j.d, j.v[0].nx, j.v[0].ny, j.v[1].nx, j.v[1].ny, j.v[0].r, j.v[0].g, j.v[0].b, j.v[0].u, j.v[0].v, band);
        break;
    case JOB_LINE: draw_line(j.d, j.v[0], j.v[1], band); break;
    case JOB_FILL: do_fill(j.v[0].nx, j.v[0].ny, j.v[1].nx, j.v[1].ny, (uint16_t)j.v[0].r, band); break;
    case JOB_CONVERT: do_convert(j, band); break;
    case JOB_COPY: do_copy(j, band); break;
    }
}

static void worker_main(int id) {
    WorkerState& me = workers[id];
    Band band{id, n_workers};
    for (;;) {
        uint64_t done = me.done.load(std::memory_order_relaxed);
        uint64_t head = q_head.load(std::memory_order_acquire);
        if (done < head) {
            run_job(queue[done % kQueueSize], band);
            me.done.store(done + 1, std::memory_order_release);
            continue;
        }
        if (workers_quit.load()) return;
        // brief spin, then sleep until new work is published
        bool got = false;
        for (int i = 0; i < 4000 && !got; i++) {
            _mm_pause();
            got = q_head.load(std::memory_order_acquire) > done;
        }
        if (got) continue;
        std::unique_lock<std::mutex> lk(q_mutex);
        q_sleepers++;
        q_cv.wait_for(lk, std::chrono::milliseconds(2),
                      [&] { return q_head.load(std::memory_order_acquire) > done || workers_quit.load(); });
        q_sleepers--;
    }
}

static uint64_t min_done() {
    uint64_t m = UINT64_MAX;
    for (int i = 0; i < n_workers; i++) m = std::min(m, workers[i].done.load(std::memory_order_acquire));
    return m;
}

static void wake_workers() {
    if (q_sleepers.load(std::memory_order_relaxed) > 0) {
        std::lock_guard<std::mutex> lk(q_mutex);
        q_cv.notify_all();
    }
}

// wait until every queued primitive has been rendered
uint64_t g_gpu_stalls = 0;  // syncs that had to wait (profiling)
double g_gpu_wait_s = 0, g_gpu_full_s = 0;
static double wall() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
uint64_t g_gpu_stall_why[4];  // 0 other, 1 texture hazard, 2 upload, 3 download

static void gpu_sync_why(int why) {
    if (threaded && min_done() < q_head.load(std::memory_order_relaxed)) g_gpu_stall_why[why]++;
    gpu_sync();
}

void gpu_sync() {
    if (!threaded) return;
    uint64_t head = q_head.load(std::memory_order_relaxed);
    if (min_done() < head) {
        g_gpu_stalls++;
        double t = wall();
        wake_workers();
        while (min_done() < head) _mm_pause();
        g_gpu_wait_s += wall() - t;
    }
    pend_write.clear();
    pend_read.clear();
}

static void submit(const Job& j) {
    if (!threaded) {
        run_job(j, Band{0, 1});
        return;
    }
    uint64_t head = q_head.load(std::memory_order_relaxed);
    if (head - min_done() >= kQueueSize) {
        double t = wall();
        wake_workers();
        while (head - min_done() >= kQueueSize) _mm_pause();
        g_gpu_full_s += wall() - t;
    }
    queue[head % kQueueSize] = j;
    q_head.store(head + 1, std::memory_order_release);
    if ((head & 31) == 0) wake_workers();
}

// native area a job writes (for hazard tracking)
static int job_writes(const Job& j, Rgn* out) {
    auto clip = [&](int x0, int y0, int x1, int y1) {
        return Rgn{std::max(x0, (int)j.d.da_x1), std::max(y0, (int)j.d.da_y1), std::min(x1, j.d.da_x2 + 1),
                   std::min(y1, j.d.da_y2 + 1)};
    };
    switch (j.kind) {
    case JOB_TRI:
        // sub-pixel (PGXP) vertices can lie up to 2 native pixels from the integer ones
        out[0] = clip(std::min({j.v[0].nx, j.v[1].nx, j.v[2].nx}) - 2, std::min({j.v[0].ny, j.v[1].ny, j.v[2].ny}) - 2,
                      std::max({j.v[0].nx, j.v[1].nx, j.v[2].nx}) + 2, std::max({j.v[0].ny, j.v[1].ny, j.v[2].ny}) + 2);
        return 1;
    case JOB_LINE:
        out[0] = clip(std::min(j.v[0].nx, j.v[1].nx), std::min(j.v[0].ny, j.v[1].ny), std::max(j.v[0].nx, j.v[1].nx) + 1,
                      std::max(j.v[0].ny, j.v[1].ny) + 1);
        return 1;
    case JOB_RECT:
        out[0] = clip(j.v[0].nx, j.v[0].ny, j.v[0].nx + j.v[1].nx, j.v[0].ny + j.v[1].ny);
        return 1;
    case JOB_FILL:
        out[0] = Rgn{j.v[0].nx, j.v[0].ny, j.v[0].nx + j.v[1].nx, j.v[0].ny + j.v[1].ny};
        return 1;
    case JOB_COPY:
        out[0] = Rgn{j.v[1].nx, j.v[1].ny, j.v[1].nx + j.v[2].nx, j.v[1].ny + j.v[2].ny};
        return 1;
    case JOB_CONVERT: return 0;
    }
    return 0;
}

// texture page and CLUT area a textured primitive reads
static int texture_reads(const DrawCtx& d, int umin, int umax, int vmin, int vmax, Rgn* out) {
    if (!d.textured) return 0;
    int shift = d.tp_depth == 0 ? 2 : d.tp_depth == 1 ? 1 : 0;
    if (d.tw_mask_x || umin < 0 || umax > 255) { umin = 0; umax = 255; }
    if (d.tw_mask_y || vmin < 0 || vmax > 255) { vmin = 0; vmax = 255; }
    out[0] = Rgn{d.tp_x + (umin >> shift), d.tp_y + vmin, d.tp_x + (umax >> shift) + 1, d.tp_y + vmax + 1};
    if (d.tp_depth == 2) return 1;
    out[1] = Rgn{d.clut_x, d.clut_y, d.clut_x + (d.tp_depth == 0 ? 16 : 256), d.clut_y + 1};
    return 2;
}

static void queue_job(const Job& j, const Rgn* reads = nullptr, int nreads = 0) {
    if (threaded) {
        Rgn w[1];
        int nw = job_writes(j, w);
        bool hz = false;
        for (int i = 0; i < nreads && !hz; i++) hz = pend_write.test(reads[i]);  // read after write
        bool raw = hz;
        for (int i = 0; i < nw && !hz; i++) hz = pend_read.test(w[i]);  // write after read
        if (hz) {
            static int logged = 0;
            if (logged < 40 && getenv("GT_HAZARD_LOG")) {
                logged++;
                LOGI("hazard (%s) job %d: write %d,%d-%d,%d read %d,%d-%d,%d", raw ? "RAW" : "WAR", j.kind, nw ? w[0].x0 : 0,
                     nw ? w[0].y0 : 0, nw ? w[0].x1 : 0, nw ? w[0].y1 : 0, nreads ? reads[0].x0 : 0, nreads ? reads[0].y0 : 0,
                     nreads ? reads[0].x1 : 0, nreads ? reads[0].y1 : 0);
            }
            gpu_sync_why(1);
        }
        for (int i = 0; i < nw; i++) pend_write.mark(w[i]);
        for (int i = 0; i < nreads; i++) pend_read.mark(reads[i]);
    }
    submit(j);
}

static void draw_triangle_q(const DrawCtx& d, const Vertex& a, const Vertex& b, const Vertex& c) {
    // the GPU rejects polygons larger than 1023x511 (native)
    if (std::max({a.nx, b.nx, c.nx}) - std::min({a.nx, b.nx, c.nx}) >= 1024 ||
        std::max({a.ny, b.ny, c.ny}) - std::min({a.ny, b.ny, c.ny}) >= 512)
        return;
    Rgn rd[2];
    int nr = texture_reads(d, std::min({a.u, b.u, c.u}), std::max({a.u, b.u, c.u}), std::min({a.v, b.v, c.v}),
                           std::max({a.v, b.v, c.v}), rd);
    Job j;
    j.kind = JOB_TRI;
    j.d = d;
    j.v[0] = a; j.v[1] = b; j.v[2] = c;
    queue_job(j, rd, nr);
}

// ---- command decoding ------------------------------------------------------------------
static inline int sx11(uint32_t v) { return ((int32_t)(v << 21)) >> 21; }

static Vertex read_vertex(const uint32_t* w, uint32_t color, bool precise = false) {
    Vertex v;
    v.nx = sx11(w[0] & 0xFFFF) + off_x;
    v.ny = sx11(w[0] >> 16) + off_y;
    float fx, fy;
    if (precise && g_pgxp && pgxp_lookup(w[0], &fx, &fy, nullptr)) {
        float k = (float)(1 << (sh + SUB));
        v.x = (int32_t)std::lround((fx - (int16_t)(w[0] & 0xFFFF) + v.nx) * k);
        v.y = (int32_t)std::lround((fy - (int16_t)(w[0] >> 16) + v.ny) * k);
    } else {
        v.x = v.nx * (1 << (sh + SUB));
        v.y = v.ny * (1 << (sh + SUB));
    }
    v.r = color & 0xFF;
    v.g = (color >> 8) & 0xFF;
    v.b = (color >> 16) & 0xFF;
    v.u = v.v = 0;
    return v;
}

static int poly_words(uint32_t op) {
    bool quad = op & 8, tex = op & 4, gour = op & 0x10;
    int n = quad ? 4 : 3;
    return 1 + n * (1 + (tex ? 1 : 0) + (gour ? 1 : 0)) - (gour ? 1 : 0);
}

static void exec_polygon() {
    uint32_t op = cmd[0] >> 24;
    bool quad = op & 8, tex = op & 4, gour = op & 0x10;
    int n = quad ? 4 : 3;
    Vertex v[4];
    uint32_t clut = 0, tpage = 0;
    int i = 0;
    uint32_t color = cmd[0] & 0xFFFFFF;
    for (int k = 0; k < n; k++) {
        if (gour && k > 0) color = cmd[i] & 0xFFFFFF;
        if (k == 0 || gour) i++;
        v[k] = read_vertex(&cmd[i++], color, true);
        if (tex) {
            uint32_t t = cmd[i++];
            v[k].u = t & 0xFF;
            v[k].v = (t >> 8) & 0xFF;
            if (k == 0) clut = t >> 16;
            if (k == 1) tpage = t >> 16;
        }
    }
    DrawCtx d = make_ctx(op, tpage, clut, tex);
    d.gouraud = gour;
    d.do_dither = dither_mode && dither && (gour || (d.textured && !d.raw));
    if (tex) {
        // polygon texpage updates the global texpage (bits 0-8, 11)
        stat_texpage = (stat_texpage & ~0x9FFu) | (tpage & 0x9FF);
    }
    if (!gour) for (int k = 1; k < n; k++) { v[k].r = v[0].r; v[k].g = v[0].g; v[k].b = v[0].b; }
    if (d.raw) for (int k = 0; k < n; k++) v[k].r = v[k].g = v[k].b = 128;
    draw_triangle_q(d, v[0], v[1], v[2]);
    if (quad) draw_triangle_q(d, v[1], v[2], v[3]);
}

static void exec_rect() {
    uint32_t op = cmd[0] >> 24;
    bool tex = op & 4;
    int size = (op >> 3) & 3;
    uint32_t color = cmd[0] & 0xFFFFFF;
    int i = 1;
    Vertex p = read_vertex(&cmd[i++], color);
    int u = 0, v = 0;
    uint32_t clut = 0;
    if (tex) {
        uint32_t t = cmd[i++];
        u = t & 0xFF;
        v = (t >> 8) & 0xFF;
        clut = t >> 16;
    }
    int w, h;
    if (size == 0) { w = cmd[i] & 0x3FF; h = (cmd[i] >> 16) & 0x1FF; }
    else if (size == 1) w = h = 1;
    else if (size == 2) w = h = 8;
    else w = h = 16;
    DrawCtx d = make_ctx(op, stat_texpage, clut, tex);
    d.gouraud = false;
    Rgn rd[2];
    int nr = texture_reads(d, u, u + w - 1, v, v + h - 1, rd);
    Job j;
    j.kind = JOB_RECT;
    j.d = d;
    j.v[0] = p;
    j.v[0].u = u;
    j.v[0].v = v;
    if (d.raw) j.v[0].r = j.v[0].g = j.v[0].b = 128;
    j.v[1].nx = w;
    j.v[1].ny = h;
    queue_job(j, rd, nr);
}

static void exec_line_segment(const uint32_t* w0, uint32_t c0, const uint32_t* w1, uint32_t c1, uint32_t op) {
    DrawCtx d = make_ctx(op, stat_texpage, 0, false);
    d.gouraud = op & 0x10;
    d.do_dither = dither_mode && dither && d.gouraud;
    Job j;
    j.kind = JOB_LINE;
    j.d = d;
    j.v[0] = read_vertex(w0, c0);
    j.v[1] = read_vertex(w1, c1);
    queue_job(j);
}

static void fill_rect() {
    uint32_t c = cmd[0];
    Job j;
    j.kind = JOB_FILL;
    j.d = DrawCtx{};
    j.v[0].nx = cmd[1] & 0x3F0;
    j.v[0].ny = (cmd[1] >> 16) & 0x1FF;
    j.v[1].nx = ((cmd[2] & 0x3FF) + 0xF) & ~0xF;
    j.v[1].ny = (cmd[2] >> 16) & 0x1FF;
    j.v[0].r = (int)(((c >> 3) & 31) | (((c >> 11) & 31) << 5) | (((c >> 19) & 31) << 10));
    queue_job(j);
}

uint64_t g_gpu_copies = 0;
static void vram_copy() {
    g_gpu_copies++;
    Job j;
    j.kind = JOB_COPY;
    j.d = DrawCtx{};
    j.d.mask_set = mask_set;
    j.d.mask_check = mask_check;
    j.v[0].nx = cmd[1] & 0x3FF; j.v[0].ny = (cmd[1] >> 16) & 0x1FF;
    j.v[1].nx = cmd[2] & 0x3FF; j.v[1].ny = (cmd[2] >> 16) & 0x1FF;
    j.v[2].nx = cmd[3] & 0x3FF; j.v[2].ny = (cmd[3] >> 16) & 0x1FF;
    if (!j.v[2].nx) j.v[2].nx = 0x400;
    if (!j.v[2].ny) j.v[2].ny = 0x200;
    if (j.v[0].ny == j.v[1].ny) {
        // source and destination rows are the same: one worker does each row in order
        queue_job(j);
        return;
    }
    Rgn src{j.v[0].nx, j.v[0].ny, j.v[0].nx + j.v[2].nx, j.v[0].ny + j.v[2].ny};
    Rgn dst{j.v[1].nx, j.v[1].ny, j.v[1].nx + j.v[2].nx, j.v[1].ny + j.v[2].ny};
    TileMap t{};
    t.mark(src);
    if (t.test(dst)) {
        // a copy between different but overlapping rows depends on row order: do it here
        gpu_sync();
        run_job(j, Band{0, 1});
        return;
    }
    queue_job(j, &src, 1);
}

static void start_workers() {
    unsigned hc = std::thread::hardware_concurrency();
    n_workers = (int)std::clamp<unsigned>(hc > 2 ? hc - 2 : 1, 1, 12);
    if (const char* e = getenv("GT_GPU_THREADS")) n_workers = std::clamp(atoi(e), 1, 32);
    workers = new WorkerState[n_workers];
    for (int i = 0; i < n_workers; i++) workers[i].th = std::thread(worker_main, i);
    LOGI("GPU: %d render threads", n_workers);
}

static int command_length(uint32_t w) {
    uint32_t op = w >> 24;
    switch (op >> 5) {
    case 1: return poly_words(op);
    case 2: return (op & 8) ? -1 : ((op & 0x10) ? 4 : 3);  // polyline: variable
    case 3: {
        int n = 2;
        if (op & 4) n++;
        if (((op >> 3) & 3) == 0) n++;
        return n;
    }
    case 4: return 4;
    case 5: case 6: return 3;
    default: return op == 0x02 ? 3 : 1;
    }
}

static void exec_command() {
    uint32_t op = cmd[0] >> 24;
    switch (op >> 5) {
    case 1: exec_polygon(); return;
    case 2: {
        bool gour = op & 0x10;
        uint32_t c0 = cmd[0] & 0xFFFFFF, c1 = gour ? cmd[2] & 0xFFFFFF : c0;
        exec_line_segment(&cmd[1], c0, gour ? &cmd[3] : &cmd[2], c1, op);
        return;
    }
    case 3: exec_rect(); return;
    case 4: vram_copy(); return;
    case 5: {
        gpu_sync_why(2);  // uploads are written directly by the emulation thread
        upload.active = true;
        upload.x = cmd[1] & 0x3FF; upload.y = (cmd[1] >> 16) & 0x1FF;
        upload.w = ((cmd[2] & 0xFFFF) - 1) % 1024 + 1; upload.h = (((cmd[2] >> 16) & 0xFFFF) - 1) % 512 + 1;
        upload.cx = upload.cy = 0;
        return;
    }
    case 6: {
        gpu_sync_why(3);
        download.active = true;
        download.x = cmd[1] & 0x3FF; download.y = (cmd[1] >> 16) & 0x1FF;
        download.w = ((cmd[2] & 0xFFFF) - 1) % 1024 + 1; download.h = (((cmd[2] >> 16) & 0xFFFF) - 1) % 512 + 1;
        download.cx = download.cy = 0;
        return;
    }
    default: break;
    }
    uint32_t v = cmd[0];
    switch (op) {
    case 0x02: fill_rect(); break;
    case 0x1F: irq_flag = true; irq_raise(IRQ_GPU); break;
    case 0xE1:
        stat_texpage = v & 0x9FF;
        tex_disable = allow_tex_disable && (v & 0x800);
        dither = v & 0x200;
        draw_to_display = v & 0x400;
        break;
    case 0xE2:
        tw_mask_x = v & 31; tw_mask_y = (v >> 5) & 31;
        tw_off_x = (v >> 10) & 31; tw_off_y = (v >> 15) & 31;
        break;
    case 0xE3: da_x1 = v & 0x3FF; da_y1 = (v >> 10) & 0x3FF; break;
    case 0xE4: da_x2 = v & 0x3FF; da_y2 = (v >> 10) & 0x3FF; break;
    case 0xE5: off_x = sx11(v & 0x7FF); off_y = sx11((v >> 11) & 0x7FF); break;
    case 0xE6: mask_set = v & 1; mask_check = v & 2; break;
    default: break;
    }
}

static void upload_word(uint32_t w) {
    for (int k = 0; k < 2 && upload.active; k++) {
        uint16_t p = (uint16_t)(k ? w >> 16 : w);
        int x = upload.x + upload.cx, y = upload.y + upload.cy;
        if (!(mask_check && (px(x, y) & 0x8000))) px_fill(x, y, p | (mask_set ? 0x8000 : 0));
        if (++upload.cx >= upload.w) {
            upload.cx = 0;
            if (++upload.cy >= upload.h) upload.active = false;
        }
    }
}

void gpu_write_gp0(uint32_t w) {
    if (upload.active) { upload_word(w); return; }
    if (polyline) {
        if ((w & 0xF000F000u) == 0x50005000u && pl.have_prev) { polyline = false; return; }
        if (pl.gour && pl.want_color) { pl.cur_c = w & 0xFFFFFF; pl.want_color = false; return; }
        if (pl.have_prev) exec_line_segment(&pl.prev_v, pl.prev_c, &w, pl.cur_c, pl.op);
        pl.prev_v = w;
        pl.prev_c = pl.cur_c;
        pl.have_prev = true;
        pl.want_color = pl.gour;
        return;
    }
    if (cmd_len == 0) {
        int n = command_length(w);
        uint32_t op = w >> 24;
        if (n < 0) {  // polyline
            polyline = true;
            pl.op = op;
            pl.gour = op & 0x10;
            pl.cur_c = w & 0xFFFFFF;
            pl.have_prev = false;
            pl.want_color = false;
            return;
        }
        (void)op;
        cmd_need = n;
    }
    cmd[cmd_len++] = w;
    if (cmd_len >= cmd_need) {
        exec_command();
        cmd_len = 0;
    }
}

void gpu_write_gp1(uint32_t v) {
    switch (v >> 24) {
    case 0x00:
        cmd_len = 0; polyline = false; upload.active = download.active = false;
        display_disabled = true; dma_dir = 0; irq_flag = false;
        stat_texpage = 0; tex_disable = dither = draw_to_display = false;
        mask_set = mask_check = false;
        tw_mask_x = tw_mask_y = tw_off_x = tw_off_y = 0;
        da_x1 = da_y1 = 0; da_x2 = da_y2 = 0; off_x = off_y = 0;
        disp_x = disp_y = 0; disp_mode = 0;
        break;
    case 0x01: cmd_len = 0; polyline = false; upload.active = false; break;
    case 0x02: irq_flag = false; break;
    case 0x03: display_disabled = v & 1; break;
    case 0x04: dma_dir = v & 3; break;
    case 0x05: disp_x = v & 0x3FE; disp_y = (v >> 10) & 0x1FF; break;
    case 0x06: h_start = v & 0xFFF; h_end = (v >> 12) & 0xFFF; break;
    case 0x07: v_start = v & 0x3FF; v_end = (v >> 10) & 0x3FF; break;
    case 0x08: disp_mode = v & 0xFF; break;
    case 0x09: allow_tex_disable = v & 1; break;
    case 0x10: {
        uint32_t r = 0;
        switch (v & 7) {
        case 2: r = tw_mask_x | (tw_mask_y << 5) | (tw_off_x << 10) | (tw_off_y << 15); break;
        case 3: r = da_x1 | (da_y1 << 10); break;
        case 4: r = da_x2 | (da_y2 << 10); break;
        case 5: r = (off_x & 0x7FF) | ((off_y & 0x7FF) << 11); break;
        case 7: r = 2; break;
        }
        download.active = false;
        extern uint32_t gpu_info_latch;
        gpu_info_latch = r;
        break;
    }
    default: break;
    }
}

uint32_t gpu_info_latch = 0;

uint32_t gpu_read() {
    if (!download.active) return gpu_info_latch;
    uint32_t out = 0;
    for (int k = 0; k < 2 && download.active; k++) {
        out |= (uint32_t)px(download.x + download.cx, download.y + download.cy) << (16 * k);
        if (++download.cx >= download.w) {
            download.cx = 0;
            if (++download.cy >= download.h) download.active = false;
        }
    }
    return out;
}

uint32_t gpu_stat() {
    uint32_t s = stat_texpage & 0x7FF;
    if (mask_set) s |= 1u << 11;
    if (mask_check) s |= 1u << 12;
    if (tex_disable) s |= 1u << 15;
    s |= ((disp_mode >> 6) & 1) << 16;   // hres2
    s |= (disp_mode & 3) << 17;          // hres1
    s |= ((disp_mode >> 2) & 1) << 19;   // vres
    s |= ((disp_mode >> 3) & 1) << 20;   // PAL
    s |= ((disp_mode >> 4) & 1) << 21;   // 24-bit
    s |= ((disp_mode >> 5) & 1) << 22;   // interlace
    if (display_disabled) s |= 1u << 23;
    if (irq_flag) s |= 1u << 24;
    s |= 1u << 26;                       // ready for command
    if (download.active) s |= 1u << 27;  // ready to send VRAM to CPU
    s |= 1u << 28;                       // ready for DMA block
    s |= (uint32_t)dma_dir << 29;
    switch (dma_dir) {
    case 1: case 2: s |= 1u << 25; break;
    case 3: if (download.active) s |= 1u << 25; break;
    default: break;
    }
    // odd/even line
    if (!vblank_now) {
        bool interlace = disp_mode & 0x20;
        if (interlace) s |= (uint32_t)(frame_count() & 1) << 31;
        else s |= (uint32_t)(hblank_count(now()) & 1) << 31;
    }
    if (disp_mode & 0x20) s |= (uint32_t)(frame_count() & 1) << 13;
    return s;
}

void gpu_vblank(bool in_vblank) { vblank_now = in_vblank; }

DisplayInfo gpu_display() {
    static const int hres[8] = {256, 320, 512, 640, 368, 368, 368, 368};
    DisplayInfo d;
    int idx = (disp_mode & 3) | ((disp_mode >> 4) & 4);
    d.w = (disp_mode & 0x40) ? 368 : hres[disp_mode & 3];
    (void)idx;
    int lines = v_end - v_start;
    if (lines <= 0 || lines > 256) lines = 240;
    d.h = (disp_mode & 0x24) == 0x24 ? lines * 2 : lines;
    d.x = disp_x;
    d.y = disp_y;
    d.rgb24 = disp_mode & 0x10;
    d.enabled = !display_disabled;
    return d;
}

const uint16_t* gpu_vram() { return vram.data(); }

// Convert the displayed 15-bit area (native x, y, w, h) at internal resolution to ARGB,
// on the render threads after all queued drawing. Returns when done.
void gpu_convert_display(uint32_t* dst, int pitch_px, int x, int y, int w, int h) {
    Job j;
    j.kind = JOB_CONVERT;
    j.v[0].nx = x << sh;
    j.v[0].ny = y << sh;
    j.v[1].nx = w << sh;
    j.v[1].ny = h << sh;
    j.dst = dst;
    j.pitch = pitch_px;
    submit(j);
    gpu_sync();
}
uint16_t gpu_vram_native(int x, int y) { return px(x, y); }
int gpu_scale() { return 1 << sh; }

void gpu_set_scale(int scale) {
    int nsh = 0;
    while (nsh < 3 && (2 << nsh) <= scale) nsh++;
    gpu_sync();
    threaded = nsh > 0;
    if (threaded && !workers) start_workers();
    if (nsh == sh) return;
    // resample the current contents so a change while running keeps the picture
    std::vector<uint16_t> old;
    old.swap(vram);
    int osh = sh;
    sh = nsh;
    VW = 1024 << sh;
    VH = 512 << sh;
    vram.assign((size_t)VW * VH, 0);
    for (int y = 0; y < VH; y++)
        for (int x = 0; x < VW; x++) {
            int ox = osh >= sh ? x << (osh - sh) : x >> (sh - osh);
            int oy = osh >= sh ? y << (osh - sh) : y >> (sh - osh);
            vram[((size_t)y << (10 + sh)) | x] = old[((size_t)oy << (10 + osh)) | ox];
        }
}

void gpu_set_dither(bool on) { dither_mode = on ? 1 : 0; }
bool gpu_dither() { return dither_mode != 0; }

void gpu_init() {
    gpu_sync();
    std::fill(vram.begin(), vram.end(), 0);
    gpu_write_gp1(0);
}

}  // namespace psx
