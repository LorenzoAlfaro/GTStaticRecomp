// Interface between recompiled code (generated C) and the runtime (C++).
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct CPU {
    uint32_t r[32];
    uint32_t hi, lo;
    uint32_t pc;          // only meaningful in the interpreter / diagnostics
    int64_t cycles;       // emulated CPU cycles since boot
    int64_t next_event;   // cycles value at which rt_check_events must run
    uint32_t cop0[32];
    uint32_t gte_data[32];
    uint32_t gte_ctrl[32];
} CPU;

typedef void (*RecompFunc)(CPU* c);

// setjmp/longjmp that preserve all Win64 callee-saved registers incl. XMM6-15 (jmp.c)
typedef uint64_t rt_jmp_buf[32];
int rt_setjmp(uint64_t* buf) __attribute__((returns_twice));
__attribute__((noreturn)) void rt_longjmp(uint64_t* buf, int val);

typedef struct FuncEntry {
    uint32_t addr;
    int ovr;              // executable index (1..5, see EXES in gen/recomp.py)
    uint32_t lo, hi;      // code extent, used to verify the executable is resident
    RecompFunc fn;
} FuncEntry;

typedef struct OverlayInfo {
    int index;
    uint32_t base, size, hash;  // unpacked image extent, hash of the packed file on the disc
    const char* name;
} OverlayInfo;

extern const FuncEntry g_func_table[];
extern const int g_func_table_count;
extern const OverlayInfo g_overlays[];
extern const int g_overlay_count;

extern uint8_t g_ram[0x200000];
extern uint8_t g_scratch[0x400];

// ---- memory ------------------------------------------------------------------
uint32_t mem_read8_slow(uint32_t a);
uint32_t mem_read16_slow(uint32_t a);
uint32_t mem_read32_slow(uint32_t a);
void mem_write8_slow(uint32_t a, uint32_t v);
void mem_write16_slow(uint32_t a, uint32_t v);
void mem_write32_slow(uint32_t a, uint32_t v);

#define PHYS(a) ((a) & 0x1FFFFFFFu)
#define IS_RAM(p) ((p) < 0x00800000u)
#define IS_SCRATCH(p) (((p) & 0xFFFFFC00u) == 0x1F800000u)

extern uint32_t g_watch_value, g_watch_addr;  // debug store watchpoints (RECOMP_CHECK_CALLS builds)
#ifdef RECOMP_CHECK_CALLS
void rt_watch_hit(uint32_t a, uint32_t v, void* host_ra);
#define WATCH_SW(a, v) do { if (((v) == g_watch_value && g_watch_value) || (((a) & 0x1FFFFC) == g_watch_addr && g_watch_addr))     rt_watch_hit((a), (v), __builtin_return_address(0)); } while (0)
#define WATCH_SSUB(a, v) do { if ((((a) & 0x1FFFFC) == g_watch_addr && g_watch_addr))     rt_watch_hit((a), (v), __builtin_return_address(0)); } while (0)
#else
#define WATCH_SW(a, v) ((void)0)
#define WATCH_SSUB(a, v) ((void)0)
#endif

static inline uint32_t MEM_LB(uint32_t a) {
    uint32_t p = PHYS(a);
    if (IS_RAM(p)) return g_ram[p & 0x1FFFFF];
    if (IS_SCRATCH(p)) return g_scratch[p & 0x3FF];
    return mem_read8_slow(a);
}
static inline uint32_t MEM_LH(uint32_t a) {
    uint32_t p = PHYS(a);
    if (IS_RAM(p)) return *(uint16_t*)(g_ram + (p & 0x1FFFFE));
    if (IS_SCRATCH(p)) return *(uint16_t*)(g_scratch + (p & 0x3FE));
    return mem_read16_slow(a);
}
static inline uint32_t MEM_LW(uint32_t a) {
    uint32_t p = PHYS(a);
    if (IS_RAM(p)) return *(uint32_t*)(g_ram + (p & 0x1FFFFC));
    if (IS_SCRATCH(p)) return *(uint32_t*)(g_scratch + (p & 0x3FC));
    return mem_read32_slow(a);
}
static inline void MEM_SB(uint32_t a, uint32_t v) {
    uint32_t p = PHYS(a);
    WATCH_SSUB(a, v);
    if (IS_RAM(p)) { g_ram[p & 0x1FFFFF] = (uint8_t)v; return; }
    if (IS_SCRATCH(p)) { g_scratch[p & 0x3FF] = (uint8_t)v; return; }
    mem_write8_slow(a, v);
}
static inline void MEM_SH(uint32_t a, uint32_t v) {
    uint32_t p = PHYS(a);
    WATCH_SSUB(a, v);
    if (IS_RAM(p)) { *(uint16_t*)(g_ram + (p & 0x1FFFFE)) = (uint16_t)v; return; }
    if (IS_SCRATCH(p)) { *(uint16_t*)(g_scratch + (p & 0x3FE)) = (uint16_t)v; return; }
    mem_write16_slow(a, v);
}
static inline void MEM_SW(uint32_t a, uint32_t v) {
    uint32_t p = PHYS(a);
    WATCH_SW(a, v);
    if (IS_RAM(p)) { *(uint32_t*)(g_ram + (p & 0x1FFFFC)) = v; return; }
    if (IS_SCRATCH(p)) { *(uint32_t*)(g_scratch + (p & 0x3FC)) = v; return; }
    mem_write32_slow(a, v);
}

static inline uint32_t LWL(uint32_t rt, uint32_t a) {
    uint32_t w = MEM_LW(a & ~3u);
    switch (a & 3) {
    case 0: return (rt & 0x00FFFFFFu) | (w << 24);
    case 1: return (rt & 0x0000FFFFu) | (w << 16);
    case 2: return (rt & 0x000000FFu) | (w << 8);
    default: return w;
    }
}
static inline uint32_t LWR(uint32_t rt, uint32_t a) {
    uint32_t w = MEM_LW(a & ~3u);
    switch (a & 3) {
    case 0: return w;
    case 1: return (rt & 0xFF000000u) | (w >> 8);
    case 2: return (rt & 0xFFFF0000u) | (w >> 16);
    default: return (rt & 0xFFFFFF00u) | (w >> 24);
    }
}
static inline void SWL(uint32_t a, uint32_t rt) {
    uint32_t w = MEM_LW(a & ~3u);
    switch (a & 3) {
    case 0: w = (w & 0xFFFFFF00u) | (rt >> 24); break;
    case 1: w = (w & 0xFFFF0000u) | (rt >> 16); break;
    case 2: w = (w & 0xFF000000u) | (rt >> 8); break;
    default: w = rt; break;
    }
    MEM_SW(a & ~3u, w);
}
static inline void SWR(uint32_t a, uint32_t rt) {
    uint32_t w = MEM_LW(a & ~3u);
    switch (a & 3) {
    case 0: w = rt; break;
    case 1: w = (w & 0x000000FFu) | (rt << 8); break;
    case 2: w = (w & 0x0000FFFFu) | (rt << 16); break;
    default: w = (w & 0x00FFFFFFu) | (rt << 24); break;
    }
    MEM_SW(a & ~3u, w);
}

static inline void DIV(CPU* c, uint32_t n, uint32_t d) {
    int32_t sn = (int32_t)n, sd = (int32_t)d;
    if (sd == 0) { c->hi = n; c->lo = sn >= 0 ? 0xFFFFFFFFu : 1u; }
    else if (n == 0x80000000u && sd == -1) { c->hi = 0; c->lo = 0x80000000u; }
    else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
static inline void DIVU(CPU* c, uint32_t n, uint32_t d) {
    if (d == 0) { c->hi = n; c->lo = 0xFFFFFFFFu; }
    else { c->lo = n / d; c->hi = n % d; }
}

// ---- control flow / events ---------------------------------------------------
void rt_check_events(CPU* c);
#define CYC(n) (c->cycles += (n))
#define CHECK_EVENTS(c) do { if ((c)->cycles >= (c)->next_event) rt_check_events(c); } while (0)

#ifdef RECOMP_RET_CHECK
void rt_bad_return(CPU* c, uint32_t expected);
#define RET_CHECK(c, ra) do { if ((c)->r[31] != (ra)) rt_bad_return((c), (ra)); } while (0)
#else
#define RET_CHECK(c, ra) ((void)0)
#endif

#ifdef RECOMP_CHECK_CALLS
void rt_saved_regs_changed(CPU* c, const uint32_t* before, uint32_t target, uint32_t site);
#define CALL_CHECK_BEGIN() uint32_t _sv[11]; for (int _i = 0; _i < 8; _i++) _sv[_i] = c->r[16 + _i];     _sv[8] = c->r[28]; _sv[9] = c->r[29]; _sv[10] = c->r[30];
#define CALL_CHECK_END(t, site) do { int _bad = 0; for (int _i = 0; _i < 8; _i++) _bad |= _sv[_i] != c->r[16 + _i];     _bad |= _sv[8] != c->r[28] || _sv[9] != c->r[29] || _sv[10] != c->r[30];     if (_bad) rt_saved_regs_changed(c, _sv, (t), (site)); } while (0)
#else
#define CALL_CHECK_BEGIN()
#define CALL_CHECK_END(t, site) ((void)0)
#endif

extern int g_trace_on;
void rt_trace(CPU* c, uint32_t addr);
#define TRACE_ENTRY(c, a) do { if (g_trace_on) rt_trace((c), (a)); } while (0)

void rt_call(CPU* c, uint32_t target, uint32_t ret);
void rt_return_to(CPU* c, uint32_t target);
int rt_frame_push(uint32_t ret);
uint64_t* rt_frame_jb(int d);
void rt_frame_pop(int d);
void rt_jump(CPU* c, uint32_t target, uint32_t ret);

void rt_syscall(CPU* c, uint32_t code);
void rt_break(CPU* c, uint32_t pc, uint32_t code);
void rt_invalid(CPU* c, uint32_t pc);
void rt_fallthrough(CPU* c, uint32_t pc);
void rt_warn_delay(CPU* c, uint32_t pc);

// Enhancement hooks (runtime/hooks.cpp): the recompiler emits a call before each
// instruction listed in gen/recomp.py HOOKS; the interpreter checks rt_is_hook().
extern int g_hooks_on;
void rt_hook(CPU* c, uint32_t pc);
int rt_is_hook(uint32_t pc);

uint32_t rt_mfc0(CPU* c, int reg);
void rt_mtc0(CPU* c, int reg, uint32_t v);
void rt_rfe(CPU* c);

// ---- GTE ---------------------------------------------------------------------
uint32_t gte_read_data(CPU* c, int reg);
void gte_write_data(CPU* c, int reg, uint32_t v);
uint32_t gte_read_ctrl(CPU* c, int reg);
void gte_write_ctrl(CPU* c, int reg, uint32_t v);
void gte_command(CPU* c, uint32_t cmd);

#ifdef __cplusplus
}
#endif
