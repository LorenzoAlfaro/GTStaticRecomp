// Function dispatch, interpreter fallback, COP0 and interrupts.
#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "cycles.h"
#include "psx.h"

void crash_dump_state();

CPU g_cpu;

namespace psx {

// ---------------------------------------------------------------------------------
// function lookup

struct Candidate {
    const FuncEntry* e;
    uint64_t verified_gen;
};
static std::unordered_map<uint32_t, std::vector<Candidate>> g_lookup;
static std::unordered_map<int, std::vector<uint8_t>> g_ovr_images;  // overlay index -> original bytes
static std::unordered_map<uint32_t, RecompFunc> g_overrides;
static std::unordered_map<uint32_t, uint64_t> g_miss_counts;

// Shadow stack of return addresses for dynamic calls (rt_call). CTR's hand-written
// renderer calls with "jalr t2, rX" and returns with "jr t2"; a dynamic jump to the
// address the innermost dynamic caller expects is a return.
// Each frame also has a setjmp point so hand-written code that returns more than one
// level up ("jr t2" from inside a routine called by the routine that was jalr'd with t2)
// can unwind the intermediate native frames.
struct RetFrame {
    uint32_t ret;
    int unwind_depth;
    rt_jmp_buf jb;
};
static RetFrame g_retframes[4096];
static int g_retsp = 0;
static int g_lj_target = -1;  // retframe index a longjmp is heading to
static void set_retsp(int v, int line) {
    if (v < 0 || v > 4096) fatal("bad return-stack index %d set at cpu.cpp:%d", v, line);
    g_retsp = v;
}
static int g_unwind_depth_shadow();

void cpu_register_overlay_image(int index, const uint8_t* data, size_t size);

void cpu_register_overlay_image(int index, const uint8_t* data, size_t size) {
    g_ovr_images[index].assign(data, data + size);
}

static bool overlay_resident(Candidate& cand) {
    const FuncEntry* e = cand.e;
    auto it = g_ovr_images.find(e->ovr);
    if (it == g_ovr_images.end()) return false;
    const OverlayInfo* info = nullptr;
    for (int i = 0; i < g_overlay_count; i++)
        if (g_overlays[i].index == e->ovr) info = &g_overlays[i];
    if (!info) return false;
    const uint8_t* img = it->second.data();
    uint32_t lo = e->lo, hi = e->hi;
    uint32_t off = lo - info->base;
    uint32_t len = hi - lo;
    const uint8_t* ram = g_ram + (lo & 0x1FFFFF);
    // cheap check every time (first 16 bytes), full check when DMA changed RAM
    if (memcmp(ram, img + off, std::min<uint32_t>(len, 16)) != 0) return false;
    if (cand.verified_gen == g_dma_generation) return true;
    if (memcmp(ram, img + off, len) != 0) return false;
    cand.verified_gen = g_dma_generation;
    return true;
}

bool g_pure_interp = false;  // --interp: run everything on the interpreter (reference mode)
bool g_verify = false, g_verify_io = false, g_in_verify = false;
bool g_check_sregs = false;  // --check-sregs: report dynamic calls that clobber s0-s7/fp/sp/gp
static int g_verify_retsp_base = 0, g_verify_unwind_base = 0;
static int64_t g_verify_saved_next = 0;
// I/O inside a verified call: it can't be replayed, so stop comparing and let
// events/interrupts run normally (the call may be polling hardware)
void verify_abandon() {
    if (!g_in_verify) return;
    g_verify_io = true;
    g_in_verify = false;
    g_cpu.next_event = g_verify_saved_next;
    if (g_cpu.cycles >= g_cpu.next_event) g_cpu.next_event = g_cpu.cycles;
}

// a longjmp is about to leave the verified call: abandon the comparison
static void verify_escape_check(int target_retsp, int target_unwind) {
    if (g_in_verify && (target_retsp < g_verify_retsp_base || target_unwind < g_verify_unwind_base)) {
        g_in_verify = false;
        g_cpu.next_event = g_verify_saved_next;
        if (g_cpu.cycles >= g_cpu.next_event) g_cpu.next_event = g_cpu.cycles;
    }
}

bool cpu_lookup(uint32_t addr, RecompFunc* out) {
    if (g_pure_interp) return false;
    auto ov = g_overrides.find(addr);
    if (ov != g_overrides.end()) { *out = ov->second; return true; }
    auto it = g_lookup.find(addr);
    if (it == g_lookup.end()) return false;
    for (auto& cand : it->second) {
        if (cand.e->ovr == 0 || overlay_resident(cand)) {
            *out = cand.e->fn;
            return true;
        }
    }
    return false;
}

void cpu_init() {
    memset(&g_cpu, 0, sizeof g_cpu);
    g_lookup.clear();
    for (int i = 0; i < g_func_table_count; i++) {
        const FuncEntry* e = &g_func_table[i];
        g_lookup[(e->addr & 0x1FFFFFFF) | 0x80000000].push_back({e, 0});
    }
    g_cpu.cop0[12] = 0x10900000;  // SR: COP0 usable, BEV
    g_cpu.cop0[15] = 0x00000002;  // PRId
}

// ---------------------------------------------------------------------------------
// interpreter

static inline uint32_t canon(uint32_t a) {
    // KUSEG/KSEG1 aliases of RAM map onto the KSEG0 addresses used by the tables
    uint32_t p = a & 0x1FFFFFFF;
    if (p < 0x00800000) return 0x80000000 | (p & 0x1FFFFF);
    return a;
}

static void count_miss(uint32_t addr) {
    uint64_t n = ++g_miss_counts[addr];
    if (n == 1 || n == 100 || n == 10000 || n == 1000000)
        LOGD("interpreting %08x (%llu times)", addr, (unsigned long long)n);
}

void rt_nonlocal_return_check(uint32_t target);

void interp_exec_simple(CPU* c, uint32_t w, uint32_t pc);

// a recompiled function of the resident program starts at t (all of GT's executables
// share addresses, so a function of another one doesn't count)
static bool is_func_entry(uint32_t t) {
    t = canon(t);
    if (g_overrides.count(t)) return true;
    auto it = g_lookup.find(t);
    if (it == g_lookup.end()) return false;
    for (auto& cand : it->second)
        if (cand.e->ovr == 0 || overlay_resident(cand)) return true;
    return false;
}

// Interpreter. Event checks and cycle accounting follow exactly the same rules as the
// recompiler (see gen/recomp.py): check before calls, on taken backward branches, on
// backward jumps and on tail calls; each instruction costs insn_cycles().
void interp_run(CPU* c, uint32_t pc, uint32_t stop) {
    int depth = 0;
    uint32_t* r = c->r;
    count_miss(canon(pc));
    if (g_trace_on && g_pure_interp && is_func_entry(canon(pc))) rt_trace(c, canon(pc));
    auto exec_delay = [&](uint32_t at) {
        uint32_t dw = MEM_LW(at + 4);
        c->pc = at + 4;
        c->cycles += insn_cycles(dw);
        interp_exec_simple(c, dw, at + 4);
    };
    // transfer control to t as a call (link) or jump; returns next pc to interpret
    auto transfer = [&](uint32_t t, uint32_t ret, bool link) -> uint32_t {
        uint32_t tc = canon(t);
        uint32_t p = tc & 0x1FFFFFFF;
        if (p == 0xA0 || p == 0xB0 || p == 0xC0) {
            bios_call(c, p);
            return link ? ret : r[31];
        }
        RecompFunc fn;
        if (cpu_lookup(tc, &fn)) {
            uint32_t cont = link ? ret : r[31];
            fn(c);
            return cont;
        }
        if (g_trace_on && is_func_entry(tc)) rt_trace(c, tc);  // mirror native TRACE_ENTRY
        if (link && ret == stop) depth++;
        return t;
    };
    for (;;) {
        if (pc == stop) {
            if (depth == 0) return;
            depth--;
        }
        if (g_hooks_on && rt_is_hook(canon(pc))) rt_hook(c, canon(pc));
        uint32_t w = MEM_LW(pc);
        uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
        uint32_t simm = (uint32_t)(int32_t)(int16_t)(w & 0xFFFF);
        c->cycles += insn_cycles(w);
        c->pc = pc;
        if (op == 0 && ((w & 63) == 0x08 || (w & 63) == 0x09)) {  // jr / jalr
            uint32_t t = r[rs];
            bool link = (w & 63) == 0x09;
            if (link && rd) r[rd] = pc + 8;
            exec_delay(pc);
            if (!link) {
                if (t == stop && depth == 0) return;
                rt_nonlocal_return_check(t);
                pc = transfer(t, pc + 8, false);
            } else {
                CHECK_EVENTS(c);
                pc = transfer(t, pc + 8, true);
            }
            continue;
        }
        if (op == 0x02 || op == 0x03) {  // j / jal
            uint32_t t = ((pc + 4) & 0xF0000000) | ((w & 0x3FFFFFF) << 2);
            if (op == 0x03) r[31] = pc + 8;
            exec_delay(pc);
            if (op == 0x03 || t <= pc || is_func_entry(t)) CHECK_EVENTS(c);
            pc = transfer(t, pc + 8, op == 0x03);
            continue;
        }
        if (op == 0x01 || (op >= 0x04 && op <= 0x07)) {  // branches
            bool taken, link = false;
            if (op == 0x01) {
                taken = (rt & 1) ? (int32_t)r[rs] >= 0 : (int32_t)r[rs] < 0;
                link = (rt & 0x1E) == 0x10;
                if (link) r[31] = pc + 8;
            } else if (op == 0x04) taken = r[rs] == r[rt];
            else if (op == 0x05) taken = r[rs] != r[rt];
            else if (op == 0x06) taken = (int32_t)r[rs] <= 0;
            else taken = (int32_t)r[rs] > 0;
            uint32_t target = pc + 4 + (simm << 2);
            exec_delay(pc);
            if (!taken) { pc += 8; continue; }
            if (link) {
                CHECK_EVENTS(c);
                pc = transfer(target, pc + 8, true);
            } else {
                if (target <= pc) CHECK_EVENTS(c);
                pc = target;
            }
            continue;
        }
        interp_exec_simple(c, w, pc);
        pc += 4;
    }
}

void interp_exec_simple(CPU* c, uint32_t w, uint32_t pc) {
    uint32_t* r = c->r;
    uint32_t op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    uint32_t sa = (w >> 6) & 31, imm = w & 0xFFFF;
    uint32_t simm = (uint32_t)(int32_t)(int16_t)imm;
    uint32_t addr = r[rs] + simm;
    uint32_t v = 0;
    bool wr = false;
    int dst = 0;
    switch (op) {
    case 0x00:
        dst = rd;
        wr = true;
        switch (w & 63) {
        case 0x00: v = r[rt] << sa; break;
        case 0x02: v = r[rt] >> sa; break;
        case 0x03: v = (uint32_t)((int32_t)r[rt] >> sa); break;
        case 0x04: v = r[rt] << (r[rs] & 31); break;
        case 0x06: v = r[rt] >> (r[rs] & 31); break;
        case 0x07: v = (uint32_t)((int32_t)r[rt] >> (r[rs] & 31)); break;
        case 0x0C: wr = false; c->pc = pc; rt_syscall(c, (w >> 6) & 0xFFFFF); break;
        case 0x0D: wr = false; rt_break(c, pc, (w >> 6) & 0xFFFFF); break;
        case 0x10: v = c->hi; break;
        case 0x11: wr = false; c->hi = r[rs]; break;
        case 0x12: v = c->lo; break;
        case 0x13: wr = false; c->lo = r[rs]; break;
        case 0x18: { wr = false; int64_t p = (int64_t)(int32_t)r[rs] * (int32_t)r[rt]; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); break; }
        case 0x19: { wr = false; uint64_t p = (uint64_t)r[rs] * r[rt]; c->lo = (uint32_t)p; c->hi = (uint32_t)(p >> 32); break; }
        case 0x1A: wr = false; DIV(c, r[rs], r[rt]); break;
        case 0x1B: wr = false; DIVU(c, r[rs], r[rt]); break;
        case 0x20: case 0x21: v = r[rs] + r[rt]; break;
        case 0x22: case 0x23: v = r[rs] - r[rt]; break;
        case 0x24: v = r[rs] & r[rt]; break;
        case 0x25: v = r[rs] | r[rt]; break;
        case 0x26: v = r[rs] ^ r[rt]; break;
        case 0x27: v = ~(r[rs] | r[rt]); break;
        case 0x2A: v = (int32_t)r[rs] < (int32_t)r[rt]; break;
        case 0x2B: v = r[rs] < r[rt]; break;
        default: wr = false; rt_invalid(c, pc); break;
        }
        break;
    case 0x08: case 0x09: dst = rt; wr = true; v = r[rs] + simm; break;
    case 0x0A: dst = rt; wr = true; v = (int32_t)r[rs] < (int32_t)simm; break;
    case 0x0B: dst = rt; wr = true; v = r[rs] < simm; break;
    case 0x0C: dst = rt; wr = true; v = r[rs] & imm; break;
    case 0x0D: dst = rt; wr = true; v = r[rs] | imm; break;
    case 0x0E: dst = rt; wr = true; v = r[rs] ^ imm; break;
    case 0x0F: dst = rt; wr = true; v = imm << 16; break;
    case 0x10:  // COP0
        if (rs == 0) { dst = rt; wr = true; v = rt_mfc0(c, rd); }
        else if (rs == 4) rt_mtc0(c, rd, r[rt]);
        else if (rs == 0x10 && (w & 63) == 0x10) rt_rfe(c);
        break;
    case 0x12:  // COP2
        if (rs & 0x10) gte_command(c, w & 0x1FFFFFF);
        else if (rs == 0) { dst = rt; wr = true; v = gte_read_data(c, rd); }
        else if (rs == 2) { dst = rt; wr = true; v = gte_read_ctrl(c, rd); }
        else if (rs == 4) gte_write_data(c, rd, r[rt]);
        else if (rs == 6) gte_write_ctrl(c, rd, r[rt]);
        break;
    case 0x20: dst = rt; wr = true; v = (uint32_t)(int32_t)(int8_t)MEM_LB(addr); break;
    case 0x21: dst = rt; wr = true; v = (uint32_t)(int32_t)(int16_t)MEM_LH(addr); break;
    case 0x22: dst = rt; wr = true; v = LWL(r[rt], addr); break;
    case 0x23: dst = rt; wr = true; v = MEM_LW(addr); break;
    case 0x24: dst = rt; wr = true; v = MEM_LB(addr); break;
    case 0x25: dst = rt; wr = true; v = MEM_LH(addr); break;
    case 0x26: dst = rt; wr = true; v = LWR(r[rt], addr); break;
    case 0x28: MEM_SB(addr, r[rt]); break;
    case 0x29: MEM_SH(addr, r[rt]); break;
    case 0x2A: SWL(addr, r[rt]); break;
    case 0x2B: MEM_SW(addr, r[rt]); break;
    case 0x2E: SWR(addr, r[rt]); break;
    case 0x32: gte_write_data(c, rt, MEM_LW(addr)); break;
    case 0x3A: MEM_SW(addr, gte_read_data(c, rt)); break;
    default: rt_invalid(c, pc); break;
    }
    if (wr && dst) r[dst] = v;
}

// ---------------------------------------------------------------------------------
// interrupts

static bool g_in_exception = false;
static rt_jmp_buf g_exc_jb;

bool bios_in_exception() { return g_in_exception; }

// Saved state for the (non-nesting) exception; static because locals don't survive
// __builtin_longjmp back into this frame.
static struct {
    uint32_t r[32];
    uint32_t hi, lo;
} g_exc_saved;
static int g_exc_retsp;
static int g_exc_unwind;
static void g_unwind_depth_restore(int d);

void cpu_raise_interrupt_if_pending(CPU* c) {
    if (g_in_exception) return;
    uint32_t sr = c->cop0[12];
    if (!irq_pending()) { c->cop0[13] &= ~0x400u; return; }
    c->cop0[13] |= 0x400;
    if (!(sr & 1) || !(sr & 0x400)) return;

    // take the exception: save everything the real exception handler would
    memcpy(g_exc_saved.r, c->r, sizeof g_exc_saved.r);
    g_exc_saved.hi = c->hi;
    g_exc_saved.lo = c->lo;
    c->cop0[13] = (c->cop0[13] & ~0x7Cu) | 0x400;  // ExcCode = INT
    c->cop0[12] = (sr & ~0x3Fu) | ((sr & 0xF) << 2);
    g_in_exception = true;
    g_exc_retsp = g_retsp;
    g_exc_unwind = g_unwind_depth_shadow();
    if (rt_setjmp(g_exc_jb) == 0) {
        bios_exception(&g_cpu);
    }
    set_retsp(g_exc_retsp, 386);
    g_unwind_depth_restore(g_exc_unwind);
    CPU* cc = &g_cpu;
    memcpy(cc->r, g_exc_saved.r, sizeof g_exc_saved.r);
    cc->hi = g_exc_saved.hi;
    cc->lo = g_exc_saved.lo;
    cc->cop0[12] = (cc->cop0[12] & ~0xFu) | ((cc->cop0[12] >> 2) & 0xF);
    g_in_exception = false;
    if (irq_pending()) irq_recheck();
}

// Called by the HLE ReturnFromException: unwinds to the exception entry above.
void cpu_return_from_exception() {
    if (!g_in_exception) {
        LOGW("ReturnFromException outside exception");
        return;
    }
    rt_longjmp(g_exc_jb, 1);
}

// ---------------------------------------------------------------------------------
// unwind points (rt_setjmp targets that an rt_longjmp across native frames lands in)

static int g_unwind_depth = 0;
static int g_unwind_depth_shadow() { return g_unwind_depth; }
static void g_unwind_depth_restore(int d) { g_unwind_depth = d; }

// Called by the interpreter on "jr reg": unwind if the target is a pending return
// address of an outer dynamic call.
void rt_nonlocal_return_check(uint32_t target) {
    if (g_pure_interp) return;  // the interpreter follows MIPS control flow exactly
    for (int k = g_retsp - 2; k >= 0; k--) {
        if (g_retframes[k].ret != target) continue;
        verify_escape_check(k, g_retframes[k].unwind_depth);
        LOGD("non-local return to %08x from interpreter", target);
        g_unwind_depth_restore(g_retframes[k].unwind_depth);
        g_lj_target = k;
        rt_longjmp(g_retframes[k].jb, 1);
    }
}

}  // namespace psx

using namespace psx;

extern "C" {

void rt_check_events(CPU* c) {
    if (g_in_verify) { c->next_event = INT64_MAX; return; }  // comparisons run without events
    if (g_trace_on) {
        uint32_t before[32];
        memcpy(before, c->r, sizeof before);
        run_events();
        if (memcmp(before, c->r, sizeof before)) {
            for (int i = 0; i < 32; i++)
                if (before[i] != c->r[i])
                    LOGE("run_events changed r%d: %08x -> %08x at cycles %lld", i, before[i], c->r[i], (long long)c->cycles);
        }
        memcpy(before, c->r, sizeof before);
        bool took = !g_in_exception && irq_pending() && (c->cop0[12] & 0x401) == 0x401;
        cpu_raise_interrupt_if_pending(c);
        if (memcmp(before, c->r, sizeof before))
            LOGE("interrupt (taken=%d) changed registers at cycles %lld", took, (long long)c->cycles);
        return;
    }
    run_events();
    cpu_raise_interrupt_if_pending(c);
}

static void rt_call_inner(CPU* c, uint32_t target, uint32_t ret);

static void dump_retstack() {
    for (int k = g_retsp - 1; k >= 0 && k >= g_retsp - 24; k--) LOGE("  frame %d: ret %08x", k, g_retframes[k].ret);
    std::unordered_map<uint32_t, int> hist;
    for (int k = 0; k < g_retsp; k++) hist[g_retframes[k].ret]++;
    for (auto& [a, n] : hist) if (n > 20) LOGE("  ret %08x x%d", a, n);
}

void rt_call(CPU* c, uint32_t target, uint32_t ret) {
    volatile int d = g_retsp;
    if (d >= 4096) { dump_retstack(); fatal("return stack overflow (target %08x)", target); }
    RetFrame& f = g_retframes[d];
    f.ret = ret;
    f.unwind_depth = g_unwind_depth_shadow();
    set_retsp(d + 1, 459);
    if (g_check_sregs) {
        uint32_t saved[11];
        for (int i = 0; i < 8; i++) saved[i] = g_cpu.r[16 + i];
        saved[8] = g_cpu.r[28]; saved[9] = g_cpu.r[29]; saved[10] = g_cpu.r[30];
        bool unwound = true;
        if (rt_setjmp(f.jb) == 0) { rt_call_inner(&g_cpu, target, ret); unwound = false; }
        static std::unordered_map<uint32_t, int> seen;
        const char* names[11] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "sp", "fp"};
        uint32_t now_v[11];
        for (int i = 0; i < 8; i++) now_v[i] = g_cpu.r[16 + i];
        now_v[8] = g_cpu.r[28]; now_v[9] = g_cpu.r[29]; now_v[10] = g_cpu.r[30];
        for (int i = 0; i < 11; i++)
            if (!unwound && saved[i] != now_v[i] && seen[target]++ < 3)
                LOGW("call %08x (ret %08x) clobbered %s: %08x -> %08x", target, ret, names[i], saved[i], now_v[i]);
        set_retsp(d, 474);
        return;
    }
    if (rt_setjmp(f.jb) == 0) {
        rt_call_inner(&g_cpu, target, ret);
    } else if (g_lj_target != d) {
        fatal("stale return frame: landed in %d, expected %d", (int)d, g_lj_target);
    }
    set_retsp(d, 482);
}

extern "C" size_t gte_state_size();
extern "C" void gte_save(void* dst);
extern "C" void gte_load(const void* src);

// Differential check: run a dynamic call natively and on the interpreter from the same
// state and compare RAM, scratchpad, GPRs and GTE. Calls doing I/O are not compared.
static void verify_call(CPU* c, RecompFunc fn, uint32_t target, uint32_t ret) {
    static std::vector<uint8_t> ram0(sizeof g_ram), ramN(sizeof g_ram), gte0, gteN;
    static uint8_t scr0[0x400], scrN[0x400];
    static std::unordered_map<uint32_t, int> reported;
    if (gte0.empty()) { gte0.resize(gte_state_size()); gteN.resize(gte_state_size()); }
    CPU c0 = *c;
    memcpy(ram0.data(), g_ram, sizeof g_ram);
    memcpy(scr0, g_scratch, sizeof scr0);
    gte_save(gte0.data());
    int64_t saved_next = c->next_event;
    c->next_event = INT64_MAX;  // no events/interrupts inside the comparison
    g_verify_saved_next = saved_next;
    g_verify_retsp_base = g_retsp;
    g_verify_unwind_base = g_unwind_depth_shadow();
    g_in_verify = true;
    g_verify_io = false;
    int retsp = g_retsp;
    fn(c);
    set_retsp(retsp, 509);
    static uint64_t n_checked = 0, n_io = 0;
    if (g_verify_io) n_io++;
    if ((n_checked + n_io) % 20000 == 0) LOGI("verify: %llu compared, %llu skipped (I/O)", (unsigned long long)n_checked, (unsigned long long)n_io);
    if (!g_verify_io && g_in_verify) {
        n_checked++;
        CPU cN = *c;
        memcpy(ramN.data(), g_ram, sizeof g_ram);
        memcpy(scrN, g_scratch, sizeof scrN);
        gte_save(gteN.data());
        // replay on the interpreter
        *c = c0;
        c->next_event = INT64_MAX;
        memcpy(g_ram, ram0.data(), sizeof g_ram);
        memcpy(g_scratch, scr0, sizeof scr0);
        gte_load(gte0.data());
        g_pure_interp = true;
        interp_run(c, target, ret);
        g_pure_interp = false;
        set_retsp(retsp, 528);
        bool bad = false;
        char msg[512];
        int n = 0;
        for (int i = 1; i < 32 && !bad; i++)
            if (c->r[i] != cN.r[i] && i != 26 && i != 27) {
                snprintf(msg, sizeof msg, "reg r%d native=%08x interp=%08x", i, cN.r[i], c->r[i]);
                bad = true;
            }
        if (!bad && (c->hi != cN.hi || c->lo != cN.lo)) { snprintf(msg, sizeof msg, "hi/lo"); bad = true; }
        if (!bad) {
            for (size_t i = 0; i < sizeof g_ram; i += 4) {
                if (*(uint32_t*)&g_ram[i] != *(uint32_t*)&ramN[i]) {
                    if (!bad) snprintf(msg, sizeof msg, "RAM %08zx native=%08x interp=%08x", 0x80000000 + i,
                                       *(uint32_t*)&ramN[i], *(uint32_t*)&g_ram[i]);
                    bad = true;
                    n++;
                }
            }
        }
        if (!bad && memcmp(g_scratch, scrN, sizeof scrN)) {
            for (int i = 0; i < 0x400; i += 4)
                if (*(uint32_t*)&g_scratch[i] != *(uint32_t*)&scrN[i]) {
                    snprintf(msg, sizeof msg, "scratch %03x native=%08x interp=%08x", i, *(uint32_t*)&scrN[i],
                             *(uint32_t*)&g_scratch[i]);
                    break;
                }
            bad = true;
        }
        if (!bad && memcmp(gteN.data(), gte0.data(), 0) != 0) bad = true;
        if (!bad) {
            std::vector<uint8_t> gteI(gte_state_size());
            gte_save(gteI.data());
            if (memcmp(gteI.data(), gteN.data(), gteI.size())) { snprintf(msg, sizeof msg, "GTE state"); bad = true; }
        }
        if (bad && reported[target]++ < 3)
            LOGE("VERIFY MISMATCH in call %08x (ret %08x): %s (%d RAM words differ)", target, ret, msg, n);
        // continue with the native result
        *c = cN;
        memcpy(g_ram, ramN.data(), sizeof g_ram);
        memcpy(g_scratch, scrN, sizeof scrN);
        gte_load(gteN.data());
    }
    if (g_in_verify) {
        g_in_verify = false;
        c->next_event = saved_next;
        if (c->cycles >= c->next_event) c->next_event = c->cycles;
    }
}

static void rt_call_inner(CPU* c, uint32_t target, uint32_t ret) {
    uint32_t t = canon(target);
    uint32_t p = t & 0x1FFFFFFF;
    if (p == 0xA0 || p == 0xB0 || p == 0xC0) {
        bios_call(c, p);
        return;
    }
    RecompFunc fn;
    if (cpu_lookup(t, &fn)) {
        static std::unordered_map<uint32_t, int> verify_count;
        if (g_verify && !g_in_verify && !g_in_exception && !g_pure_interp && verify_count[t]++ < 8)
            verify_call(c, fn, target, ret);
        else fn(c);
        return;
    }
    if (p >= 0x00200000 && !(p >= 0x1F800000 && p < 0x1F800400)) {
        fatal("call to invalid address %08x (return %08x, ra=%08x, sp=%08x)", target, ret, c->r[31], c->r[29]);
    }
    if (p == 0) fatal("call to NULL (ra=%08x)", ret);
    interp_run(c, target, ret);
}

int rt_frame_push(uint32_t ret) {
    int d = g_retsp;
    if (d >= 4096) { dump_retstack(); fatal("return stack overflow (frame)"); }
    g_retframes[d].ret = ret;
    g_retframes[d].unwind_depth = g_unwind_depth_shadow();
    set_retsp(d + 1, 605);
    return d;
}
uint64_t* rt_frame_jb(int d) { return g_retframes[d].jb; }
void rt_frame_pop(int d) {
    set_retsp(d, 610);
}

// "jr ra" whose ra differs from the value at function entry: control goes to an outer
// frame's return address (flattened hand-written loops). Unwind if we know the frame.
void rt_return_to(CPU* c, uint32_t target) {
    for (int k = g_retsp - 1; k >= 0; k--) {
        if (g_retframes[k].ret != target) continue;
        if (k == g_retsp - 1) return;
        LOGD("return to %08x unwinds %d levels", target, g_retsp - 1 - k);
        verify_escape_check(k, g_retframes[k].unwind_depth);
        g_unwind_depth_restore(g_retframes[k].unwind_depth);
        g_lj_target = k;
        rt_longjmp(g_retframes[k].jb, 1);
    }
}

void rt_jump(CPU* c, uint32_t target, uint32_t ret) {
    if (target == ret) return;  // jr to own return address
    for (int k = g_retsp - 1; k >= 0; k--) {
        if (g_retframes[k].ret != target) continue;
        if (k == g_retsp - 1) return;  // "jr t2"-style return from the innermost dynamic call
        LOGD("non-local return to %08x (%d levels)", target, g_retsp - 1 - k);
        verify_escape_check(k, g_retframes[k].unwind_depth);
        g_unwind_depth_restore(g_retframes[k].unwind_depth);
        g_lj_target = k;
        rt_longjmp(g_retframes[k].jb, 1);
    }
    rt_call_inner(c, target, ret);
}

// Exec (BIOS A0:43) replaces the running program: drop every native frame and dynamic
// return frame of the old one and start over from the top-level loop in bios_boot.
static rt_jmp_buf g_exec_jb;
static bool g_exec_jb_set = false;
uint64_t* cpu_exec_jb() {
    g_exec_jb_set = true;
    return g_exec_jb;
}
void cpu_exec_unwind() {
    if (!g_exec_jb_set) fatal("Exec before boot");
    if (g_in_exception) LOGW("Exec inside an exception handler");
    g_in_exception = false;
    set_retsp(0, 0);
    g_unwind_depth_restore(0);
    rt_longjmp(g_exec_jb, 1);
}

void rt_syscall(CPU* c, uint32_t code) { bios_syscall(c, code); }

void rt_break(CPU* c, uint32_t pc, uint32_t code) {
    LOGW("break %x at %08x (ra=%08x)", code, pc, c->r[31]);
}

void rt_invalid(CPU* c, uint32_t pc) {
    fatal("invalid instruction %08x at %08x (ra=%08x)", MEM_LW(pc), pc, c->r[31]);
}

void rt_fallthrough(CPU* c, uint32_t pc) {
    // recompiled code ran off the end of what was decoded: continue in the interpreter
    LOGD("fallthrough to %08x", pc);
    interp_run(c, pc, c->r[31]);
}

uintptr_t psx_module_base();
static uint32_t c_ra() { return g_cpu.r[31]; }
uint32_t g_watch_value = 0, g_watch_addr = 0;
int g_trace_on = 0;
FILE* g_trace_file = nullptr;
long long g_break_cycles = -1;
void rt_trace(CPU* c, uint32_t addr) {
    if (c->cycles == g_break_cycles) {
        fprintf(stderr, "[BREAK] at %08x cycles %lld\n", addr, (long long)c->cycles);
        crash_dump_state();
    }
    if (g_trace_file)
        fprintf(g_trace_file, "%08x %lld a0=%08x a1=%08x ra=%08x sp=%08x\n", addr, (long long)c->cycles, c->r[4], c->r[5],
                c->r[31], c->r[29]);
}
void rt_watch_hit(uint32_t a, uint32_t v, void* host_ra) {
    static int n = 0;
    if (n++ > 200) return;
    uintptr_t base = 0;
#ifdef _WIN32
    base = psx_module_base();
#endif
    LOGW("WATCH: store %08x -> [%08x] (host exe+%llx) ra=%08x cycles=%lld interp-pc=%08x", v, a,
         (unsigned long long)((uintptr_t)host_ra - base), c_ra(), (long long)g_cpu.cycles, g_cpu.pc);
    if (g_log_level >= LOG_DEBUG) crash_dump_state();
}

void rt_saved_regs_changed(CPU* c, const uint32_t* before, uint32_t target, uint32_t site) {
    static std::unordered_map<uint64_t, int> seen;
    if (seen[((uint64_t)target << 32) | site]++ >= 2) return;
    static const char* names[11] = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7", "gp", "sp", "fp"};
    const uint32_t now_v[11] = {c->r[16], c->r[17], c->r[18], c->r[19], c->r[20], c->r[21], c->r[22], c->r[23],
                                c->r[28], c->r[29], c->r[30]};
    for (int i = 0; i < 11; i++)
        if (before[i] != now_v[i])
            LOGW("jal %08x at %08x clobbered %s: %08x -> %08x", target, site, names[i], before[i], now_v[i]);
}

void rt_warn_delay(CPU* c, uint32_t pc) { LOGW("branch in delay slot at %08x", pc); }

void rt_bad_return(CPU* c, uint32_t expected) {
    LOGW("return mismatch: ra=%08x expected %08x", c->r[31], expected);
}

uint32_t rt_mfc0(CPU* c, int reg) {
    if (reg == 13) {
        uint32_t cause = c->cop0[13] & ~0x400u;
        if (irq_pending()) cause |= 0x400;
        return cause;
    }
    return c->cop0[reg];
}

void rt_mtc0(CPU* c, int reg, uint32_t v) {
    if (reg == 13) { c->cop0[13] = (c->cop0[13] & ~0x300u) | (v & 0x300); return; }
    c->cop0[reg] = v;
    if (reg == 12) irq_recheck();
}

void rt_rfe(CPU* c) {
    uint32_t sr = c->cop0[12];
    c->cop0[12] = (sr & ~0xFu) | ((sr >> 2) & 0xF);
    irq_recheck();
}

}  // extern "C"
