// Memory map, interrupt controller, DMA, timers and the event scheduler.
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "state.h"
#include "psx.h"

void crash_dump_state();
extern "C" FILE* g_trace_file;

uint8_t g_ram[0x200000];
uint8_t g_scratch[0x400];

namespace psx {

int g_log_level = LOG_INFO;

void log(int level, const char* fmt, ...) {
    if (level > g_log_level) return;
    static const char* tag[] = {"E", "W", "I", "D"};
    fprintf(stderr, "[%s] ", tag[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

void fatal(const char* fmt, ...) {
    fprintf(stderr, "[FATAL] ");
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
    ::crash_dump_state();
    _Exit(1);
}

// ---------------------------------------------------------------------------------
// scheduler

static int64_t ev_time[EV_COUNT];
static EventFn ev_fn[EV_COUNT];
static constexpr int64_t NEVER = INT64_MAX / 2;

static void update_next() {
    int64_t n = NEVER;
    for (int i = 0; i < EV_COUNT; i++) n = ev_time[i] < n ? ev_time[i] : n;
    g_cpu.next_event = n;
}

void sched_init() {
    for (auto& t : ev_time) t = NEVER;
    update_next();
}
void register_event(int id, EventFn fn) { ev_fn[id] = fn; }
void schedule(int id, int64_t when) {
    ev_time[id] = when;
    if (when < g_cpu.next_event) g_cpu.next_event = when;
}
void unschedule(int id) {
    ev_time[id] = NEVER;
    update_next();
}
bool scheduled(int id) { return ev_time[id] < NEVER; }

void run_events() {
    for (;;) {
        int64_t t = g_cpu.cycles;
        int best = -1;
        int64_t bt = NEVER;
        for (int i = 0; i < EV_COUNT; i++)
            if (ev_time[i] <= t && ev_time[i] < bt) { bt = ev_time[i]; best = i; }
        if (best < 0) break;
        ev_time[best] = NEVER;
        if (ev_fn[best]) ev_fn[best](bt);
    }
    update_next();
}

// ---------------------------------------------------------------------------------
// interrupts

uint32_t i_stat = 0, i_mask = 0;

void irq_raise(int n) {
    i_stat |= 1u << n;
    irq_recheck();
}
bool irq_pending() { return (i_stat & i_mask) != 0; }
void irq_recheck() {
    if (irq_pending()) g_cpu.next_event = g_cpu.cycles;  // deliver at the next check point
}

// ---------------------------------------------------------------------------------
// video timing

static int64_t frame_start_cycle = 0;
static int64_t frames = 0;
static bool in_vblank = false;

int64_t hblank_count(int64_t cycles) { return (int64_t)((double)cycles / CYCLES_PER_LINE_F); }
int64_t frame_count() { return frames; }

static int64_t line_cycle(int64_t frame_base, int line) {
    return frame_base + (int64_t)(line * CYCLES_PER_LINE_F);
}

FILE* g_hash_log = nullptr;   // --hash-log: per-frame RAM hash (lockstep debugging)
int64_t g_dump_frame = -1;     // --dump-at N: write RAM at frame N
std::string g_dump_path;

int64_t g_trace_start = -1, g_trace_end = -1;

bool g_prim_log = false;  // --prim-log: per-frame primitive buffer usage 

static void frame_debug_hooks() {
    if (g_prim_log) {
        uint32_t gGT = *(uint32_t*)(g_ram + 0x8d2ac);
        if ((gGT >> 24) == 0x80) {
            uint32_t base = gGT & 0x1FFFFF;
            for (int db = 0; db < 2; db++) {
                uint32_t pm = base + (db ? 0x130 : 0x8c);
                uint32_t size = *(uint32_t*)(g_ram + pm), start = *(uint32_t*)(g_ram + pm + 4);
                uint32_t curr = *(uint32_t*)(g_ram + pm + 12);
                if (size) LOGI("frame %lld db%d prim %u / %u%s", (long long)frames, db, curr - start, size,
                               curr - start > size ? "  OVERFLOW" : "");
            }
        }
    }
    if (g_trace_file) g_trace_on = frames >= g_trace_start && frames < g_trace_end;
    if (g_hash_log) {
        uint64_t h = 1469598103934665603ull;
        const uint64_t* p = (const uint64_t*)g_ram;
        for (size_t i = 0; i < sizeof g_ram / 8; i++) h = (h ^ p[i]) * 1099511628211ull;
        const uint64_t* sp = (const uint64_t*)g_scratch;
        for (size_t i = 0; i < sizeof g_scratch / 8; i++) h = (h ^ sp[i]) * 1099511628211ull;
        fprintf(g_hash_log, "%lld %lld %016llx\n", (long long)frames, (long long)g_cpu.cycles, (unsigned long long)h);
        fflush(g_hash_log);
    }
    if (frames == g_dump_frame) {
        FILE* f = fopen(g_dump_path.c_str(), "wb");
        if (f) {
            fwrite(g_ram, 1, sizeof g_ram, f);
            fwrite(g_scratch, 1, sizeof g_scratch, f);
            fwrite(g_cpu.r, 4, 32, f);
            fclose(f);
        }
    }
}

static void ev_vblank(int64_t t) {
    in_vblank = true;
    gpu_vblank(true);
    irq_raise(IRQ_VBLANK);
    frames++;
    frame_debug_hooks();
    spu_run_until(t);
    // save/load requests are handled here and only here: a fixed point where the native
    // stack holds no heap pointers (state.cpp); the frontend runs after it with fresh locals
    state_point();
    frontend_vblank();
    schedule(EV_VBLANK_END, line_cycle(frame_start_cycle, LINES_PER_FRAME));
}
static void ev_vblank_end(int64_t t) {
    in_vblank = false;
    gpu_vblank(false);
    frame_start_cycle = t;
    schedule(EV_VBLANK, line_cycle(frame_start_cycle, VBLANK_START_LINE));
}

// ---------------------------------------------------------------------------------
// timers (root counters)

struct Timer {
    uint32_t mode = 0, target = 0;
    uint32_t value = 0;      // value at base_time
    int64_t base_time = 0;   // time (in source ticks) at which value was set
    bool irq_done = false;
};
static Timer timers[3];

static int timer_source(int n) {
    // returns 0 sysclk, 1 hblank, 2 sysclk/8, 3 dotclock
    uint32_t src = (timers[n].mode >> 8) & 3;
    if (n == 0) return (src & 1) ? 3 : 0;
    if (n == 1) return (src & 1) ? 1 : 0;
    return (src & 2) ? 2 : 0;
}
static int64_t source_ticks(int src, int64_t cycles) {
    switch (src) {
    case 1: return hblank_count(cycles);
    case 2: return cycles / 8;
    case 3: return cycles * 11 / 7 / 8;  // approx dotclock at 320px
    default: return cycles;
    }
}
static int64_t ticks_to_cycles(int src, int64_t ticks) {
    switch (src) {
    case 1: return (int64_t)(ticks * CYCLES_PER_LINE_F) + 1;
    case 2: return ticks * 8;
    case 3: return ticks * 8 * 7 / 11 + 1;
    default: return ticks;
    }
}

static uint32_t timer_value(int n) {
    Timer& t = timers[n];
    int src = timer_source(n);
    int64_t elapsed = source_ticks(src, now()) - t.base_time;
    int64_t v = t.value + elapsed;
    bool reset_on_target = t.mode & 0x08;
    if (reset_on_target && t.target) {
        int64_t period = (int64_t)t.target + 1;
        if (v > t.target) v = (v - t.target - 1) % period;  // wrapped at least once
    }
    return (uint32_t)(v & 0xFFFF);
}

static void timer_schedule(int n) {
    Timer& t = timers[n];
    int id = EV_TIMER0 + n;
    unschedule(id);
    bool irq_target = t.mode & 0x10, irq_ovf = t.mode & 0x20;
    if (!irq_target && !irq_ovf) return;
    if (t.irq_done && !(t.mode & 0x40)) return;  // one-shot already fired
    int src = timer_source(n);
    uint32_t v = timer_value(n);
    int64_t dist = 0x10000 - v;  // to overflow
    if (irq_target && t.target > v) dist = std::min<int64_t>(dist, t.target - v);
    else if (irq_target && (t.mode & 0x08) && t.target) dist = std::min<int64_t>(dist, (int64_t)t.target + 1 - v + t.target);
    if (dist <= 0) dist = 1;
    int64_t now_ticks = source_ticks(src, now());
    schedule(id, ticks_to_cycles(src, now_ticks + dist));
}

static void timer_set_value(int n, uint32_t v) {
    Timer& t = timers[n];
    t.value = v & 0xFFFF;
    t.base_time = source_ticks(timer_source(n), now());
}

static void ev_timer(int n) {
    Timer& t = timers[n];
    uint32_t v = timer_value(n);
    bool hit_target = (t.mode & 0x10) && (v == t.target || ((t.mode & 0x08) && v == 0));
    if (hit_target) t.mode |= 0x800;
    if (v == 0 || v == 0xFFFF) t.mode |= 0x1000;
    t.mode &= ~0x400u;  // irq request (active low)
    irq_raise(IRQ_TMR0 + n);
    t.irq_done = true;
    if (!(t.mode & 0x08) && v >= t.target && (t.mode & 0x10)) {
        // free running: next event at overflow/target next time around
    }
    t.mode |= 0x400;
    timer_schedule(n);
}
static void ev_timer0(int64_t) { ev_timer(0); }
static void ev_timer1(int64_t) { ev_timer(1); }
static void ev_timer2(int64_t) { ev_timer(2); }

static uint32_t timer_read(uint32_t off) {
    int n = (off >> 4) & 3;
    if (n > 2) return 0;
    switch (off & 0xF) {
    case 0: return timer_value(n);
    case 4: {
        uint32_t m = timers[n].mode;
        timers[n].mode &= ~0x1800u;  // reached flags reset on read
        return m;
    }
    case 8: return timers[n].target;
    }
    return 0;
}
static void timer_write(uint32_t off, uint32_t v) {
    int n = (off >> 4) & 3;
    if (n > 2) return;
    switch (off & 0xF) {
    case 0: timer_set_value(n, v); break;
    case 4:
        timers[n].mode = (v & 0x3FF) | 0x400;
        timers[n].irq_done = false;
        timer_set_value(n, 0);
        break;
    case 8: timers[n].target = v & 0xFFFF; break;
    }
    timer_schedule(n);
}

// ---------------------------------------------------------------------------------
// DMA

struct DmaChan {
    uint32_t madr = 0, bcr = 0, chcr = 0;
};
static DmaChan dma[7];
static uint32_t dpcr = 0x07654321, dicr = 0;
uint64_t g_dma_generation = 1;

static void dicr_update() {
    bool force = dicr & (1u << 15);
    bool master = dicr & (1u << 23);
    uint32_t flags = (dicr >> 24) & 0x7F, en = (dicr >> 16) & 0x7F;
    bool old = dicr & 0x80000000u;
    bool irq = force || (master && (flags & en));
    dicr = (dicr & 0x7FFFFFFFu) | (irq ? 0x80000000u : 0);
    if (irq && !old) irq_raise(IRQ_DMA);
}

static uint32_t ram_word(uint32_t a) { return *(uint32_t*)(g_ram + (a & 0x1FFFFC)); }
static void ram_word_set(uint32_t a, uint32_t v) { *(uint32_t*)(g_ram + (a & 0x1FFFFC)) = v; }

static void dma_finish(int ch) {
    dma[ch].chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

static void dma_block(int ch, uint32_t addr, int words, bool to_device, int step) {
    // one buffer per nesting level: MDEC input (DMA0) can complete a waiting DMA1
    static std::vector<uint32_t> bufs[2];
    static int nest = 0;
    if (words <= 0) return;
    if (nest >= 2) fatal("nested DMA");
    std::vector<uint32_t>& bufv = bufs[nest++];
    struct Pop { ~Pop() { nest--; } } pop;
    if ((int)bufv.size() < words) bufv.resize(words);
    uint32_t* buf = bufv.data();
    if (to_device) {
        for (int i = 0; i < words; i++) buf[i] = ram_word(addr + (uint32_t)(i * step));
        switch (ch) {
        case 0: mdec_dma_in(buf, words); break;
        case 2: for (int i = 0; i < words; i++) gpu_write_gp0(buf[i]); break;
        case 4: spu_dma_write(buf, words); break;
        default: LOGW("DMA%d to device unsupported", ch); break;
        }
    } else {
        switch (ch) {
        case 1: mdec_dma_out(buf, words); break;
        case 2: for (int i = 0; i < words; i++) buf[i] = gpu_read(); break;
        case 3: cd_dma_read(buf, words); break;
        case 4: spu_dma_read(buf, words); break;
        default: memset(buf, 0, words * 4); LOGW("DMA%d from device unsupported", ch); break;
        }
        for (int i = 0; i < words; i++) ram_word_set(addr + (uint32_t)(i * step), buf[i]);
        g_dma_generation++;
    }
}

int mdec_out_available();
static bool mdec_out_pending = false;

static void dma_run(int ch) {
    DmaChan& d = dma[ch];
    uint32_t chcr = d.chcr;
    int sync = (chcr >> 9) & 3;
    bool to_device = chcr & 1;
    int step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d.madr & 0x1FFFFC;

    if (ch == 1) {
        // MDEC output: transfers complete instantly here, so a transfer started before the
        // MDEC has decoded enough waits until it has (dma_mdec_out_ready)
        int bs = d.bcr & 0xFFFF, ba = sync == 1 ? d.bcr >> 16 : 1;
        if (bs == 0) bs = 0x10000;
        mdec_out_pending = mdec_out_available() < bs * ba;
        if (mdec_out_pending) return;
    }

    if (ch == 6) {  // OTC: reverse-clear ordering table
        int n = d.bcr & 0xFFFF;
        if (n == 0) n = 0x10000;
        for (int i = 0; i < n; i++) {
            uint32_t a = addr - (uint32_t)(i * 4);
            ram_word_set(a, i == n - 1 ? 0x00FFFFFF : ((a - 4) & 0x1FFFFF));
        }
        g_dma_generation++;
        dma_finish(ch);
        return;
    }
    if (sync == 0) {
        int n = d.bcr & 0xFFFF;
        if (n == 0) n = 0x10000;
        dma_block(ch, addr, n, to_device, step);
    } else if (sync == 1) {
        int bs = d.bcr & 0xFFFF, ba = d.bcr >> 16;
        if (bs == 0) bs = 0x10000;
        int total = bs * ba;
        dma_block(ch, addr, total, to_device, step);
        d.madr = (addr + (uint32_t)(total * step)) & 0xFFFFFF;
        d.bcr &= 0xFFFF;
    } else if (sync == 2) {
        if (ch != 2) { LOGW("linked list DMA on ch%d", ch); dma_finish(ch); return; }
        uint32_t a = addr;
        for (int guard = 0; guard < 0x40000; guard++) {
            uint32_t hdr = ram_word(a);
            int n = hdr >> 24;
            for (int i = 0; i < n; i++) gpu_write_gp0(ram_word(a + 4 + i * 4));
            if (hdr & 0x800000) break;
            uint32_t next = hdr & 0x1FFFFC;
            if (next == a) break;
            a = next;
        }
        d.madr = 0x00FFFFFF;
    }
    dma_finish(ch);
}

void dma_mdec_out_ready() {
    if (mdec_out_pending && (dma[1].chcr & 0x01000000u)) dma_run(1);
}

static void dma_write(uint32_t off, uint32_t v) {
    if (off >= 0x70) {
        if (off == 0x70) dpcr = v;
        else if (off == 0x74) {
            uint32_t ack = v & 0x7F000000u;
            dicr = (dicr & 0xFF000000u & ~ack) | (v & 0x00FF803Fu);
            dicr_update();
        }
        return;
    }
    int ch = off >> 4;
    DmaChan& d = dma[ch];
    switch (off & 0xF) {
    case 0: d.madr = v & 0xFFFFFF; break;
    case 4: d.bcr = v; break;
    case 8: {
        d.chcr = v;
        bool start = v & 0x01000000u;
        int sync = (v >> 9) & 3;
        bool trigger = (v & 0x10000000u) || sync != 0;
        bool enabled = dpcr & (8u << (ch * 4));
        if (start && trigger && enabled) dma_run(ch);
        else if (start && trigger) dma_run(ch);  // many games forget DPCR on some channels
        break;
    }
    }
}
static uint32_t dma_read(uint32_t off) {
    if (off == 0x70) return dpcr;
    if (off == 0x74) return dicr;
    if (off >= 0x70) return 0;
    const DmaChan& d = dma[off >> 4];
    switch (off & 0xF) {
    case 0: return d.madr;
    case 4: return d.bcr;
    case 8: return d.chcr;
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// register map

static uint32_t expansion_post = 0;

uint32_t hw_read(uint32_t p, int size) {
    if (g_in_verify) verify_abandon();
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t off = p - 0x1F801000;
        if (off < 0x40) return 0;  // memory control
        if (off < 0x50) return sio_read(off - 0x40, size);
        if (off < 0x60) return off == 0x54 ? 0x5 : 0;  // SIO1 status: tx ready
        if (off < 0x70) return 0x00000B88;  // RAM size
        if (off == 0x70) return i_stat;
        if (off == 0x74) return i_mask;
        if (off >= 0x80 && off < 0x100) return dma_read(off - 0x80);
        if (off >= 0x100 && off < 0x130) return timer_read(off - 0x100);
        if (off >= 0x800 && off < 0x804) {
            uint32_t v = cd_read8(off - 0x800);
            if (size >= 2) v |= (uint32_t)cd_read8(off - 0x800) << 8;
            if (size == 4) { v |= (uint32_t)cd_read8(off - 0x800) << 16; v |= (uint32_t)cd_read8(off - 0x800) << 24; }
            return v;
        }
        if (off == 0x810) return gpu_read();
        if (off == 0x814) return gpu_stat();
        if (off == 0x820 || off == 0x824) return mdec_read((off - 0x820) >> 2);
        if (off >= 0xC00 && off < 0x1000) {
            uint32_t v = spu_read16((off - 0xC00) & ~1u);
            if (size == 4) v |= (uint32_t)spu_read16((off - 0xC00 + 2) & ~1u) << 16;
            return v;
        }
        return 0;
    }
    if (p >= 0x1F000000 && p < 0x1F800000) return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    if (p >= 0x1F802000 && p < 0x1F803000) return 0;
    if (p >= 0x1FC00000 && p < 0x1FC80000) return 0;  // no BIOS ROM
    if (p == 0x1FFE0130) return 0;
    if (g_log_level >= LOG_DEBUG) {
        static int n = 0;
        if (n++ < 40) {
            extern uintptr_t g_slow_caller;
            LOGD("unmapped read%d %08x (host exe+%llx)", size * 8, p, (unsigned long long)g_slow_caller);
        }
    }
    return 0;
}

void hw_write(uint32_t p, uint32_t v, int size) {
    if (g_in_verify) verify_abandon();
    if (p >= 0x1F801000 && p < 0x1F802000) {
        uint32_t off = p - 0x1F801000;
        if (off < 0x40) return;
        if (off < 0x50) { sio_write(off - 0x40, v, size); return; }
        if (off < 0x70) return;
        if (off == 0x70) { i_stat &= v; return; }
        if (off == 0x74) { i_mask = v & 0x7FF; irq_recheck(); return; }
        if (off >= 0x80 && off < 0x100) { dma_write(off - 0x80, v); return; }
        if (off >= 0x100 && off < 0x130) { timer_write(off - 0x100, v); return; }
        if (off >= 0x800 && off < 0x804) {
            cd_write8(off - 0x800, (uint8_t)v);
            if (size >= 2) LOGD("wide CD write %08x", p);
            return;
        }
        if (off == 0x810) { gpu_write_gp0(v); return; }
        if (off == 0x814) { gpu_write_gp1(v); return; }
        if (off == 0x820 || off == 0x824) { mdec_write((off - 0x820) >> 2, v); return; }
        if (off >= 0xC00 && off < 0x1000) {
            spu_write16((off - 0xC00) & ~1u, (uint16_t)v);
            if (size == 4) spu_write16((off - 0xC00 + 2) & ~1u, (uint16_t)(v >> 16));
            return;
        }
        return;
    }
    if (p >= 0x1F802000 && p < 0x1F803000) {
        if (p == 0x1F802041) expansion_post = v;
        return;
    }
    if (p == 0x1FFE0130) return;
    if (g_log_level >= LOG_DEBUG) {
        static int n = 0;
        if (n++ < 40) {
            extern uintptr_t g_slow_caller;
            LOGD("unmapped write%d %08x = %08x (host exe+%llx)", size * 8, p, v, (unsigned long long)g_slow_caller);
        }
    }
}

void hw_init() {
    memset(g_ram, 0, sizeof g_ram);
    memset(g_scratch, 0, sizeof g_scratch);
    sched_init();
    register_event(EV_VBLANK, ev_vblank);
    register_event(EV_VBLANK_END, ev_vblank_end);
    register_event(EV_TIMER0, ev_timer0);
    register_event(EV_TIMER1, ev_timer1);
    register_event(EV_TIMER2, ev_timer2);
    for (auto& t : timers) t.mode = 0x400;
    frame_start_cycle = 0;
    schedule(EV_VBLANK, line_cycle(0, VBLANK_START_LINE));
}

// ---------------------------------------------------------------------------------
// save states

void hw_state(StateIO& s) {
    s.section(0x48570001);
    s.bytes(g_ram, sizeof g_ram);
    s.bytes(g_scratch, sizeof g_scratch);
    s.bytes(ev_time, sizeof ev_time);
    s.io(frame_start_cycle);
    s.io(frames);
    s.io(in_vblank);
    s.io(i_stat);
    s.io(i_mask);
    for (auto& t : timers) s.io(t);
    for (auto& d : dma) s.io(d);
    s.io(dpcr);
    s.io(dicr);
    s.io(mdec_out_pending);
    s.io(expansion_post);
    if (s.loading) g_dma_generation++;  // code in RAM changed: recheck residency
}

}  // namespace psx

using namespace psx;

#ifdef _WIN32
#include <windows.h>
#endif
uintptr_t psx::g_slow_caller = 0;
static inline void note_caller(void* ra) {
#ifdef _WIN32
    static uintptr_t base = (uintptr_t)GetModuleHandleA(nullptr);
    psx::g_slow_caller = (uintptr_t)ra - base;
#endif
}

extern "C" {

uint32_t mem_read8_slow(uint32_t a) {
    note_caller(__builtin_return_address(0));
    uint32_t p = PHYS(a);
    uint32_t w = hw_read(p & ~0u, 1);
    return w & 0xFF;
}
uint32_t mem_read16_slow(uint32_t a) { note_caller(__builtin_return_address(0)); return hw_read(PHYS(a), 2) & 0xFFFF; }
uint32_t mem_read32_slow(uint32_t a) { note_caller(__builtin_return_address(0)); return hw_read(PHYS(a), 4); }
void mem_write8_slow(uint32_t a, uint32_t v) { note_caller(__builtin_return_address(0)); hw_write(PHYS(a), v & 0xFF, 1); }
void mem_write16_slow(uint32_t a, uint32_t v) { note_caller(__builtin_return_address(0)); hw_write(PHYS(a), v & 0xFFFF, 2); }
void mem_write32_slow(uint32_t a, uint32_t v) {
    note_caller(__builtin_return_address(0));
    if (a == 0xFFFE0130) return;
    hw_write(PHYS(a), v, 4);
}

}  // extern "C"
