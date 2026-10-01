// High-level emulation of the PS1 BIOS functions Gran Turismo uses, plus boot, Exec and memory cards.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <string>
#include <vector>

#include "psx.h"

namespace psx {

void cpu_return_from_exception();
void cpu_register_overlay_image(int index, const uint8_t* data, size_t size);
extern "C" uint64_t* cpu_exec_jb();
extern "C" [[noreturn]] void cpu_exec_unwind();
static void bios_exec(CPU* c, uint32_t hdr, uint32_t a0, uint32_t a1);

static inline uint32_t rd32(uint32_t a) { return MEM_LW(a); }
static inline void wr32(uint32_t a, uint32_t v) { MEM_SW(a, v); }

static std::string read_cstr(uint32_t a, size_t max = 256) {
    std::string s;
    for (size_t i = 0; i < max; i++) {
        char ch = (char)MEM_LB(a + (uint32_t)i);
        if (!ch) break;
        s += ch;
    }
    return s;
}

// ---------------------------------------------------------------------------------
// events

struct EvCB {
    uint32_t cls = 0, spec = 0, mode = 0, func = 0;
    uint32_t status = 0;  // 0 free, 0x1000 disabled, 0x2000 enabled/busy, 0x4000 ready
};
static EvCB events[32];

static uint32_t open_event(uint32_t cls, uint32_t spec, uint32_t mode, uint32_t func) {
    for (int i = 0; i < 32; i++) {
        if (events[i].status == 0) {
            events[i] = {cls, spec, mode, func, 0x1000};
            return 0xF1000000u | i;
        }
    }
    return 0xFFFFFFFF;
}
static EvCB* ev(uint32_t h) {
    uint32_t i = h & 0xFFFF;
    return i < 32 ? &events[i] : nullptr;
}
void deliver_event(uint32_t cls, uint32_t spec) {
    for (auto& e : events) {
        if (e.status != 0x2000 || e.cls != cls || e.spec != spec) continue;
        if (e.mode == 0x2000) e.status = 0x4000;  // EvMdNOINTR: mark ready
        else if (e.mode == 0x1000 && e.func) rt_call(&g_cpu, e.func, 0xFFFFFFF0);  // EvMdINTR
    }
}

// ---------------------------------------------------------------------------------
// memory card (128 KiB image, standard .mcd layout)

struct MemCard {
    std::string path;
    std::vector<uint8_t> data;
    bool dirty = false;
    bool present = true;
};
static MemCard cards[2];

static uint8_t dir_checksum(const uint8_t* f) {
    uint8_t x = 0;
    for (int i = 0; i < 127; i++) x ^= f[i];
    return x;
}
static void card_format(MemCard& mc) {
    mc.data.assign(128 * 1024, 0);
    uint8_t* d = mc.data.data();
    d[0] = 'M'; d[1] = 'C';
    d[127] = dir_checksum(d);
    for (int i = 1; i < 16; i++) {
        uint8_t* f = d + i * 128;
        f[0] = 0xA0;
        f[8] = 0xFF; f[9] = 0xFF;
        f[127] = dir_checksum(f);
    }
    for (int i = 16; i < 36; i++) {  // broken sector list
        uint8_t* f = d + i * 128;
        f[0] = 0xFF; f[1] = 0xFF; f[2] = 0xFF; f[3] = 0xFF;
        f[8] = 0xFF; f[9] = 0xFF;
        f[127] = dir_checksum(f);
    }
    uint8_t* last = d + 63 * 128;
    memcpy(last, d, 128);
    mc.dirty = true;
}
static void card_load(int port, const std::string& path) {
    MemCard& mc = cards[port];
    mc.path = path;
    FILE* f = fopen(path.c_str(), "rb");
    if (f) {
        mc.data.assign(128 * 1024, 0);
        size_t n = fread(mc.data.data(), 1, mc.data.size(), f);
        fclose(f);
        if (n != mc.data.size() || mc.data[0] != 'M' || mc.data[1] != 'C') {
            LOGW("memory card %s is invalid, formatting", path.c_str());
            card_format(mc);
        }
    } else {
        card_format(mc);
    }
}
void bios_flush_cards() {
    for (auto& mc : cards) {
        if (!mc.dirty || mc.path.empty()) continue;
        FILE* f = fopen(mc.path.c_str(), "wb");
        if (f) {
            fwrite(mc.data.data(), 1, mc.data.size(), f);
            fclose(f);
            mc.dirty = false;
        }
    }
}

struct CardFile {
    bool open = false;
    int port = 0, first_block = 0;
    uint32_t pos = 0;
    bool async = false;
};
static CardFile files[16];

static uint8_t* dir_entry(MemCard& mc, int block) { return mc.data.data() + block * 128; }

static int card_find(MemCard& mc, const std::string& name) {
    for (int b = 1; b < 16; b++) {
        uint8_t* e = dir_entry(mc, b);
        if (e[0] == 0x51 && name == std::string((char*)e + 10, strnlen((char*)e + 10, 20))) return b;
    }
    return -1;
}
static int card_block_chain(MemCard& mc, int first, int index) {
    int b = first;
    for (int i = 0; i < index && b > 0; i++) {
        uint8_t* e = dir_entry(mc, b);
        uint16_t next = e[8] | (e[9] << 8);
        b = next == 0xFFFF ? -1 : next + 1;
    }
    return b;
}
static int card_free_blocks(MemCard& mc) {
    int n = 0;
    for (int b = 1; b < 16; b++) if ((dir_entry(mc, b)[0] & 0xF0) == 0xA0) n++;
    return n;
}
static int card_create(MemCard& mc, const std::string& name, int nblocks) {
    if (card_find(mc, name) >= 0 || nblocks < 1 || card_free_blocks(mc) < nblocks) return -1;
    std::vector<int> blocks;
    for (int b = 1; b < 16 && (int)blocks.size() < nblocks; b++)
        if ((dir_entry(mc, b)[0] & 0xF0) == 0xA0) blocks.push_back(b);
    for (size_t i = 0; i < blocks.size(); i++) {
        uint8_t* e = dir_entry(mc, blocks[i]);
        memset(e, 0, 128);
        e[0] = i == 0 ? 0x51 : (i + 1 == blocks.size() ? 0x53 : 0x52);
        if (i == 0) {
            uint32_t size = (uint32_t)nblocks * 8192;
            memcpy(e + 4, &size, 4);
            strncpy((char*)e + 10, name.c_str(), 20);
        }
        uint16_t next = i + 1 < blocks.size() ? (uint16_t)(blocks[i + 1] - 1) : 0xFFFF;
        e[8] = next & 0xFF; e[9] = next >> 8;
        e[127] = dir_checksum(e);
        memset(mc.data.data() + blocks[i] * 8192, 0, 8192);
    }
    mc.dirty = true;
    return blocks[0];
}
static bool card_delete(MemCard& mc, const std::string& name) {
    int b = card_find(mc, name);
    if (b < 0) return false;
    while (b > 0) {
        uint8_t* e = dir_entry(mc, b);
        uint16_t next = e[8] | (e[9] << 8);
        e[0] = (e[0] & 0x0F) | 0xA0;
        e[127] = dir_checksum(e);
        b = next == 0xFFFF ? -1 : next + 1;
    }
    mc.dirty = true;
    return true;
}

// "bu00:NAME" / "bu10:NAME"
static bool parse_card_path(const std::string& p, int* port, std::string* name) {
    if (p.size() < 5 || p.compare(0, 2, "bu") != 0 || p[4] != ':') return false;
    *port = p[2] == '1' ? 1 : 0;
    *name = p.substr(5);
    return true;
}

static void card_event_ok(bool sw = true) {
    deliver_event(sw ? 0xF4000001 : 0xF0000011, 0x0004);  // EvSpIOE
}

// dirent iteration state (firstfile/nextfile)
static struct {
    int port = 0;
    std::string pattern;
    int next_block = 1;
} dir_iter;

static bool wildcard_match(const char* pat, const char* s) {
    for (; *pat; pat++, s++) {
        if (*pat == '*') return true;
        if (!*s) return false;
        if (*pat != '?' && toupper((unsigned char)*pat) != toupper((unsigned char)*s)) return false;
    }
    return *s == 0;
}

static uint32_t dir_next(uint32_t dirent) {
    MemCard& mc = cards[dir_iter.port];
    if (!mc.present) return 0;
    for (int b = dir_iter.next_block; b < 16; b++) {
        uint8_t* e = dir_entry(mc, b);
        if (e[0] != 0x51) continue;
        char name[21] = {};
        memcpy(name, e + 10, 20);
        if (!wildcard_match(dir_iter.pattern.c_str(), name)) continue;
        dir_iter.next_block = b + 1;
        // struct DIRENTRY { char name[20]; int attr; int size; DIRENTRY* next; int head; char system[4]; }
        for (int i = 0; i < 20; i++) MEM_SB(dirent + i, (uint8_t)name[i]);
        uint32_t size;
        memcpy(&size, e + 4, 4);
        wr32(dirent + 20, 0x50);
        wr32(dirent + 24, size);
        wr32(dirent + 28, 0);
        wr32(dirent + 32, b);
        return dirent;
    }
    return 0;
}

// ---------------------------------------------------------------------------------
// interrupt chain / exception exits

static uint32_t int_chain[4];   // SysEnqIntRP heads by priority
static uint32_t custom_exit = 0;
static bool clear_rcnt[4] = {true, true, true, true};

static uint32_t rand_next = 0x24040001;

// ---------------------------------------------------------------------------------
// printf

static std::string ps_printf(CPU* c, uint32_t fmt_addr, int first_arg) {
    std::string fmt = read_cstr(fmt_addr, 1024), out;
    int argi = first_arg;
    auto arg = [&]() -> uint32_t {
        uint32_t v = argi < 4 ? c->r[4 + argi] : rd32(c->r[29] + 4 * argi);
        argi++;
        return v;
    };
    for (size_t i = 0; i < fmt.size(); i++) {
        if (fmt[i] != '%') { out += fmt[i]; continue; }
        std::string spec = "%";
        i++;
        while (i < fmt.size() && strchr("-+ #0123456789.l", fmt[i])) {
            if (fmt[i] != 'l') spec += fmt[i];
            i++;
        }
        if (i >= fmt.size()) break;
        char conv = fmt[i];
        char buf[512];
        switch (conv) {
        case 'd': case 'i': snprintf(buf, sizeof buf, (spec + "d").c_str(), (int)arg()); out += buf; break;
        case 'u': case 'x': case 'X': case 'o': case 'c':
            snprintf(buf, sizeof buf, (spec + conv).c_str(), arg()); out += buf; break;
        case 'p': snprintf(buf, sizeof buf, "%08x", arg()); out += buf; break;
        case 's': {
            std::string s = read_cstr(arg());
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case '%': out += '%'; break;
        default: out += spec + conv; break;
        }
    }
    return out;
}

static void tty_out(const std::string& s) {
    static std::string line;
    for (char ch : s) {
        if (ch == '\n') { LOGI("[tty] %s", line.c_str()); line.clear(); }
        else line += ch;
    }
}

// ---------------------------------------------------------------------------------

void bios_init() {
    memset(events, 0, sizeof events);
    memset(int_chain, 0, sizeof int_chain);
    custom_exit = 0;
    card_load(0, g_data_dir + "/memcard1.mcd");
    card_load(1, g_data_dir + "/memcard2.mcd");
}

static void call_a0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
    uint32_t& v0 = c->r[2];
    switch (fn) {
    case 0x13: {  // SaveState / setjmp
        wr32(a0 + 0, c->r[31]);
        wr32(a0 + 4, c->r[29]);
        wr32(a0 + 8, c->r[30]);
        for (int i = 0; i < 8; i++) wr32(a0 + 12 + 4 * i, c->r[16 + i]);
        wr32(a0 + 44, c->r[28]);
        v0 = 0;
        break;
    }
    case 0x15: {  // strcat
        uint32_t d = a0;
        while (MEM_LB(d)) d++;
        for (uint32_t s = a1;; s++, d++) { uint8_t ch = MEM_LB(s); MEM_SB(d, ch); if (!ch) break; }
        v0 = a0;
        break;
    }
    case 0x17: case 0x18: {  // strcmp / strncmp
        uint32_t n = fn == 0x18 ? a2 : 0xFFFFFFFF;
        int r = 0;
        for (uint32_t i = 0; i < n; i++) {
            int x = MEM_LB(a0 + i), y = MEM_LB(a1 + i);
            if (x != y) { r = x - y; break; }
            if (!x) break;
        }
        v0 = (uint32_t)r;
        break;
    }
    case 0x19: {  // strcpy
        for (uint32_t i = 0;; i++) { uint8_t ch = MEM_LB(a1 + i); MEM_SB(a0 + i, ch); if (!ch) break; }
        v0 = a0;
        break;
    }
    case 0x1A: {  // strncpy
        uint32_t i = 0;
        for (; i < a2; i++) { uint8_t ch = MEM_LB(a1 + i); MEM_SB(a0 + i, ch); if (!ch) break; }
        for (; i < a2; i++) MEM_SB(a0 + i, 0);
        v0 = a0;
        break;
    }
    case 0x1B: { uint32_t n = 0; while (MEM_LB(a0 + n)) n++; v0 = n; break; }  // strlen
    case 0x0E: case 0x0F: v0 = (int32_t)a0 < 0 ? (uint32_t)-(int32_t)a0 : a0; break;  // abs / labs
    case 0x10: case 0x11: v0 = (uint32_t)atoi(read_cstr(a0).c_str()); break;  // atoi / atol
    case 0x16: {  // strncat
        uint32_t d = a0;
        while (MEM_LB(d)) d++;
        uint32_t i = 0;
        for (; i < a2; i++) { uint8_t ch = MEM_LB(a1 + i); if (!ch) break; MEM_SB(d + i, ch); }
        MEM_SB(d + i, 0);
        v0 = a0;
        break;
    }
    case 0x1C: case 0x1E: case 0x1D: case 0x1F: {  // index/strchr, rindex/strrchr
        bool last = fn == 0x1D || fn == 0x1F;
        v0 = 0;
        for (uint32_t i = 0;; i++) {
            uint8_t ch = MEM_LB(a0 + i);
            if (ch == (a1 & 0xFF)) { v0 = a0 + i; if (!last) break; }
            if (!ch) break;
        }
        break;
    }
    case 0x20: case 0x21: case 0x22: {  // strpbrk / strspn / strcspn
        std::string s = read_cstr(a0, 65536), set = read_cstr(a1);
        size_t n = fn == 0x21 ? s.find_first_not_of(set) : s.find_first_of(set);
        if (n == std::string::npos) n = s.size();
        v0 = fn == 0x20 ? (n < s.size() ? a0 + (uint32_t)n : 0) : (uint32_t)n;
        break;
    }
    case 0x24: {  // strstr
        std::string s = read_cstr(a0, 65536), t = read_cstr(a1, 65536);
        size_t n = s.find(t);
        v0 = n == std::string::npos ? 0 : a0 + (uint32_t)n;
        break;
    }
    case 0x25: v0 = (uint32_t)toupper((int)(a0 & 0xFF)); break;
    case 0x26: v0 = (uint32_t)tolower((int)(a0 & 0xFF)); break;
    case 0x27: {  // bcopy(src, dst, n)
        std::vector<uint8_t> tmp(a2);
        for (uint32_t i = 0; i < a2; i++) tmp[i] = (uint8_t)MEM_LB(a0 + i);
        for (uint32_t i = 0; i < a2; i++) MEM_SB(a1 + i, tmp[i]);
        break;
    }
    case 0x29: case 0x2D: {  // bcmp / memcmp
        int r = 0;
        for (uint32_t i = 0; i < a2 && !r; i++) r = (int)MEM_LB(a0 + i) - (int)MEM_LB(a1 + i);
        v0 = (uint32_t)r;
        break;
    }
    case 0x2C: {  // memmove
        std::vector<uint8_t> tmp(a2);
        for (uint32_t i = 0; i < a2; i++) tmp[i] = (uint8_t)MEM_LB(a1 + i);
        for (uint32_t i = 0; i < a2; i++) MEM_SB(a0 + i, tmp[i]);
        v0 = a0;
        break;
    }
    case 0x28: for (uint32_t i = 0; i < a1; i++) MEM_SB(a0 + i, 0); v0 = a0; break;  // bzero
    case 0x2A: for (uint32_t i = 0; i < a2; i++) MEM_SB(a0 + i, MEM_LB(a1 + i)); v0 = a0; break;  // memcpy
    case 0x2B: for (uint32_t i = 0; i < a2; i++) MEM_SB(a0 + i, a1); v0 = a0; break;  // memset
    case 0x2E: {  // memchr
        v0 = 0;
        for (uint32_t i = 0; i < a2; i++) if (MEM_LB(a0 + i) == (a1 & 0xFF)) { v0 = a0 + i; break; }
        break;
    }
    case 0x2F: rand_next = rand_next * 1103515245u + 12345u; v0 = (rand_next >> 16) & 0x7FFF; break;
    case 0x30: rand_next = a0; break;
    case 0x39: v0 = 0; break;  // InitHeap
    case 0x3F: tty_out(ps_printf(c, a0, 1)); v0 = 0; break;
    case 0x43: bios_exec(c, a0, a1, a2); break;  // Exec(header, argc, argv): does not return
    case 0x44: g_dma_generation++; break;  // FlushCache: code may have changed, recheck residency
    case 0x49: gpu_write_gp0(a0); break;  // GPU_cw
    case 0x51: LOGW("LoadExec(%s) not supported", read_cstr(a0).c_str()); break;
    case 0x70: case 0x71: case 0x72: break;  // _bu_init / _96_init / _96_remove
    case 0xAB:  // _card_info
        if (cards[a0 >> 4 & 1].present) card_event_ok(true);
        else deliver_event(0xF4000001, 0x0100);
        v0 = 1;
        break;
    case 0xAC:  // _card_load
        card_event_ok(true);
        v0 = 1;
        break;
    default:
        LOGW("unimplemented A0:%02x (ra=%08x)", fn, c->r[31]);
        v0 = 0;
        break;
    }
}

static void call_b0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
    uint32_t& v0 = c->r[2];
    switch (fn) {
    case 0x07: deliver_event(a0, a1); v0 = 1; break;
    case 0x08: v0 = open_event(a0, a1, a2, a3); break;
    case 0x09: if (EvCB* e = ev(a0)) e->status = 0; v0 = 1; break;
    case 0x0A: {  // WaitEvent
        EvCB* e = ev(a0);
        v0 = 0;
        if (e && e->status == 0x4000) { e->status = 0x2000; v0 = 1; }
        else if (e && e->status == 0x2000) { LOGW("WaitEvent would block"); v0 = 0; }
        break;
    }
    case 0x0B: {  // TestEvent
        EvCB* e = ev(a0);
        v0 = 0;
        if (e && e->status == 0x4000) { e->status = 0x2000; v0 = 1; }
        break;
    }
    case 0x0C: if (EvCB* e = ev(a0)) e->status = 0x2000; v0 = 1; break;
    case 0x0D: if (EvCB* e = ev(a0)) e->status = 0x1000; v0 = 1; break;
    case 0x17: cpu_return_from_exception(); break;
    case 0x18: custom_exit = 0; break;
    case 0x19: custom_exit = a0; break;
    case 0x32: {  // open
        std::string path = read_cstr(a0);
        int port;
        std::string name;
        v0 = 0xFFFFFFFF;
        if (!parse_card_path(path, &port, &name) || !cards[port].present) break;
        MemCard& mc = cards[port];
        int first = card_find(mc, name);
        if (a1 & 0x200) {  // FCREATE: number of blocks in the upper 16 bits
            int nb = (int)(a1 >> 16);
            if (first >= 0) break;
            first = card_create(mc, name, nb ? nb : 1);
            if (first < 0) break;
        }
        if (first < 0) break;
        for (int fd = 2; fd < 16; fd++) {
            if (!files[fd].open) {
                files[fd] = {true, port, first, 0, (a1 & 0x8000) != 0};
                v0 = (uint32_t)fd;
                break;
            }
        }
        break;
    }
    case 0x33: {  // lseek
        v0 = 0xFFFFFFFF;
        if (a0 < 16 && files[a0].open) {
            if (a2 == 0) files[a0].pos = a1;
            else if (a2 == 1) files[a0].pos += a1;
            v0 = files[a0].pos;
        }
        break;
    }
    case 0x34: case 0x35: {  // read / write
        v0 = 0xFFFFFFFF;
        if (a0 >= 16 || !files[a0].open) break;
        CardFile& f = files[a0];
        MemCard& mc = cards[f.port];
        uint32_t n = a2, done = 0;
        while (done < n) {
            int blk = card_block_chain(mc, f.first_block, (int)(f.pos / 8192));
            if (blk <= 0) break;
            uint32_t off = f.pos % 8192;
            uint32_t chunk = std::min<uint32_t>(n - done, 8192 - off);
            uint8_t* p = mc.data.data() + blk * 8192 + off;
            if (fn == 0x34) for (uint32_t i = 0; i < chunk; i++) MEM_SB(a1 + done + i, p[i]);
            else { for (uint32_t i = 0; i < chunk; i++) p[i] = (uint8_t)MEM_LB(a1 + done + i); mc.dirty = true; }
            done += chunk;
            f.pos += chunk;
        }
        v0 = done;
        if (f.async) card_event_ok(true);
        if (fn == 0x35) bios_flush_cards();
        break;
    }
    case 0x36: if (a0 < 16) files[a0].open = false; v0 = a0; bios_flush_cards(); break;
    case 0x3F: tty_out(read_cstr(a0) + "\n"); break;
    case 0x41: {  // format
        int port;
        std::string name;
        v0 = 0;
        if (parse_card_path(read_cstr(a0), &port, &name)) { card_format(cards[port]); bios_flush_cards(); v0 = 1; }
        break;
    }
    case 0x42: {  // firstfile
        int port;
        std::string name;
        v0 = 0;
        if (!parse_card_path(read_cstr(a0), &port, &name)) break;
        dir_iter.port = port;
        dir_iter.pattern = name.empty() ? "*" : name;
        dir_iter.next_block = 1;
        v0 = dir_next(a1);
        break;
    }
    case 0x43: v0 = dir_next(a0); break;  // nextfile
    case 0x45: {  // delete
        int port;
        std::string name;
        v0 = 0;
        if (parse_card_path(read_cstr(a0), &port, &name) && card_delete(cards[port], name)) {
            bios_flush_cards();
            v0 = 1;
        }
        break;
    }
    case 0x4A: case 0x4B: case 0x4C: case 0x50: break;  // InitCard/StartCard/StopCard/allow_new_card
    case 0x4E: case 0x4F: {  // write_card_sector / read_card_sector (port, sector, buf)
        MemCard& mc = cards[(a0 >> 4) & 1];
        uint32_t sec = a1 & 1023;
        uint8_t* p = mc.data.data() + sec * 128;
        if (fn == 0x4E) { for (int i = 0; i < 128; i++) p[i] = (uint8_t)MEM_LB(a2 + i); mc.dirty = true; bios_flush_cards(); }
        else for (int i = 0; i < 128; i++) MEM_SB(a2 + i, p[i]);
        deliver_event(0xF0000011, 0x0004);
        v0 = 1;
        break;
    }
    case 0x56: v0 = 0x0000C000; break;  // GetC0Table (fake tables, see bios_boot)
    case 0x57: v0 = 0x0000C400; break;  // GetB0Table
    case 0x5B: break;  // ChangeClearPad
    default:
        LOGW("unimplemented B0:%02x (ra=%08x)", fn, c->r[31]);
        v0 = 0;
        break;
    }
}

static void call_c0(CPU* c, uint32_t fn) {
    uint32_t a0 = c->r[4], a1 = c->r[5];
    uint32_t& v0 = c->r[2];
    switch (fn) {
    case 0x02: {  // SysEnqIntRP(priority, struct)
        int prio = a0 & 3;
        LOGI("SysEnqIntRP(%d, %08x) func1=%08x func2=%08x ra=%08x", prio, a1, rd32(a1 + 8), rd32(a1 + 4), c->r[31]);
        wr32(a1, int_chain[prio]);
        int_chain[prio] = a1;
        v0 = 0;
        break;
    }
    case 0x03: {  // SysDeqIntRP
        int prio = a0 & 3;
        LOGI("SysDeqIntRP(%d, %08x)", prio, a1);
        uint32_t* link = &int_chain[prio];
        uint32_t cur = *link, prev = 0;
        while (cur) {
            if (cur == a1) {
                uint32_t next = rd32(cur);
                if (prev) wr32(prev, next); else *link = next;
                break;
            }
            prev = cur;
            cur = rd32(cur);
        }
        v0 = 0;
        break;
    }
    case 0x0A: {  // ChangeClearRCnt(t, flag) -> old
        v0 = clear_rcnt[a0 & 3];
        clear_rcnt[a0 & 3] = a1 != 0;
        break;
    }
    default:
        LOGW("unimplemented C0:%02x (ra=%08x)", fn, c->r[31]);
        v0 = 0;
        break;
    }
}

void bios_call(CPU* c, uint32_t vector) {
    uint32_t fn = c->r[9] & 0xFF;
    if (g_log_level >= LOG_DEBUG && !(vector == 0xA0 && (fn == 0x2A || fn == 0x2B || fn == 0x1B || fn == 0x17 || fn == 0x19)))
        LOGD("BIOS %02X:%02X a0=%08x a1=%08x a2=%08x a3=%08x ra=%08x", vector, fn, c->r[4], c->r[5], c->r[6], c->r[7], c->r[31]);
    // pure string/memory helpers can be replayed; everything else counts as I/O
    if (g_in_verify && !(vector == 0xA0 && ((fn >= 0x13 && fn <= 0x2E) || fn == 0x44))) verify_abandon();
    if (vector == 0xA0) call_a0(c, fn);
    else if (vector == 0xB0) call_b0(c, fn);
    else call_c0(c, fn);
}

void bios_syscall(CPU* c, uint32_t) {
    // The real kernel edits the SR saved at exception entry (IEp/IM2) so that rfe
    // yields the new state; our syscalls aren't real exceptions, so edit IEc/IM2.
    uint32_t sr = c->cop0[12];
    switch (c->r[4]) {
    case 1:  // EnterCriticalSection
        c->r[2] = (sr & 0x401) == 0x401 ? 1 : 0;
        c->cop0[12] = sr & ~0x401u;
        break;
    case 2:  // ExitCriticalSection
        c->cop0[12] = sr | 0x401;
        irq_recheck();
        break;
    default:
        break;
    }
}

void bios_exception(CPU* c) {
    // The kernel runs interrupt handlers on its own stack: the interrupted code may be
    // using sp as a data pointer.
    c->r[29] = 0x8000F000;
    c->r[30] = c->r[29];
    // BIOS default IRQ handling: priority chain first
    for (int prio = 0; prio < 4; prio++) {
        for (uint32_t e = int_chain[prio]; e; e = rd32(e)) {
            uint32_t f1 = rd32(e + 8), f2 = rd32(e + 4);
            if ((f1 && (f1 >> 24) != 0x80) || (f2 && (f2 >> 24) != 0x80))
                fatal("bad interrupt chain entry %08x (prio %d): func1=%08x func2=%08x", e, prio, f1, f2);
            uint32_t result = 1;
            if (f1) {
                c->r[31] = 0xFFFFFFF0;
                rt_call(c, f1, 0xFFFFFFF0);
                result = c->r[2];
            }
            if (result && f2) {
                c->r[4] = result;
                c->r[31] = 0xFFFFFFF0;
                rt_call(c, f2, 0xFFFFFFF0);
            }
        }
    }
    // root counter / vblank events handled by the kernel
    static const uint32_t rc_irq[4] = {IRQ_TMR0, IRQ_TMR1, IRQ_TMR2, IRQ_VBLANK};
    for (int n = 0; n < 4; n++) {
        uint32_t bit = 1u << rc_irq[n];
        if ((i_stat & i_mask & bit)) {
            deliver_event(0xF2000000u + n, 0x0002);
            if (clear_rcnt[n]) i_stat &= ~bit;
        }
    }
    if (custom_exit) {
        uint32_t b = custom_exit;
        c->r[31] = rd32(b + 0);
        c->r[29] = rd32(b + 4);
        c->r[30] = rd32(b + 8);
        for (int i = 0; i < 8; i++) c->r[16 + i] = rd32(b + 12 + 4 * i);
        c->r[28] = rd32(b + 44);
        c->r[2] = 1;
        rt_call(c, c->r[31], 0xFFFFFFF0);
    }
    // default exit: ReturnFromException
}

static std::vector<uint8_t> read_disc_file(const char* name) {
    int lba, size;
    if (!cd_find_file(name, &lba, &size)) fatal("%s not found on disc", name);
    std::vector<uint8_t> buf((size + 2047) & ~2047);
    uint8_t raw[2352];
    for (int i = 0; i * 2048 < size; i++) {
        if (!cd_read_sector_raw(lba + i, raw)) fatal("disc read error in %s", name);
        memcpy(buf.data() + i * 2048, raw + 24, 2048);
    }
    buf.resize(size);
    return buf;
}

// GT's executables are packed; the stub at the entry point decompresses them in place
// (backwards LZ, "PSLZ" header) and jumps to the result. Same algorithm as
// tools/extract.py; used here only to know what each program looks like once resident.
static std::vector<uint8_t> unpack_exe(const std::vector<uint8_t>& exe, const char* name) {
    auto u32 = [&](size_t off) { uint32_t v; memcpy(&v, &exe[off], 4); return v; };
    uint32_t pc = u32(0x10), taddr = u32(0x18), tsize = u32(0x1C);
    auto text = [&](uint32_t a) { return u32(0x800 + (a - taddr)); };
    uint32_t w0 = text(pc), w1 = text(pc + 4);
    if (w0 >> 16 != 0x3C04 || w1 >> 16 != 0x3484) fatal("%s: unknown packer stub", name);
    uint32_t hdr = ((w0 & 0xFFFF) << 16) | (w1 & 0xFFFF);
    if (text(hdr) != 0x5A4C5350) fatal("%s: no PSLZ header", name);
    uint32_t dst_end = text(hdr + 4), size = text(hdr + 8), src_end = text(hdr + 12);
    uint32_t base = dst_end + 1 - size, lo = std::min(base, taddr);
    std::vector<uint8_t> mem(std::max(dst_end + 1, taddr + tsize) - lo);
    memcpy(&mem[taddr - lo], &exe[0x800], tsize);
    int64_t s = src_end - lo, d = dst_end - lo;
    for (;;) {
        uint8_t flags = mem[s--];
        bool done = false;
        for (int k = 0; k < 8; k++, flags >>= 1) {
            if (!(s < d)) { done = true; break; }
            uint8_t v = mem[s--];
            if (!(flags & 1)) { mem[d--] = v; continue; }
            int n = v + 3;
            uint32_t off = mem[s--];
            if (off & 0x80) off = ((off & 0x7F) << 8) | mem[s--];
            int64_t from = d + off + 1;
            while (n--) mem[d--] = mem[from--];
        }
        if (done || !(s < d)) break;
    }
    return std::vector<uint8_t>(mem.begin() + (base - lo), mem.begin() + (base - lo) + size);
}

static uint32_t g_boot_pc;

// A0:43 Exec. GT loads the next program's file itself, copies its text to 0x80010000 with
// code running from the stack, then calls Exec, which never returns here. The old
// program's native frames are discarded and the new one starts from bios_boot's loop.
static void bios_exec(CPU* c, uint32_t hdr, uint32_t argc, uint32_t argv) {
    uint32_t pc = rd32(hdr), gp = rd32(hdr + 4);
    uint32_t baddr = rd32(hdr + 0x18), bsize = rd32(hdr + 0x1C);
    uint32_t saddr = rd32(hdr + 0x20), ssize = rd32(hdr + 0x24);
    LOGI("Exec: entry %08x gp %08x stack %08x+%x (ra=%08x)", pc, gp, saddr, ssize, c->r[31]);
    for (uint32_t i = 0; i < bsize; i += 4) wr32(baddr + i, 0);
    if (saddr) c->r[29] = c->r[30] = saddr + ssize;
    c->r[28] = gp;
    c->r[4] = argc;
    c->r[5] = argv;
    c->r[31] = 0xFFFFFFF0;
    g_dma_generation++;
    g_boot_pc = pc;
    cpu_exec_unwind();
}

void bios_boot(CPU* c) {
    // every executable must match the one that was recompiled
    for (int k = 0; k < g_overlay_count; k++) {
        const OverlayInfo& o = g_overlays[k];
        std::vector<uint8_t> exe = read_disc_file(o.name);
        uint32_t h = 0x811C9DC5;
        for (uint8_t x : exe) h = (h ^ x) * 0x01000193;
        if (h != o.hash) fatal("%s does not match the recompiled executable (hash %08x)", o.name, h);
        std::vector<uint8_t> img = unpack_exe(exe, o.name);
        if (img.size() != o.size) fatal("%s: unpacked size %zx, expected %x", o.name, img.size(), o.size);
        cpu_register_overlay_image(o.index, img.data(), img.size());
    }

    // like the real BIOS: load the boot executable named in SYSTEM.CNF (SCUS_941.94, still packed)
    std::vector<uint8_t> exe = read_disc_file("SCUS_941.94");
    uint32_t pc, gp, taddr, tsize;
    memcpy(&pc, &exe[0x10], 4);
    memcpy(&gp, &exe[0x14], 4);
    memcpy(&taddr, &exe[0x18], 4);
    memcpy(&tsize, &exe[0x1C], 4);
    memcpy(g_ram + (taddr & 0x1FFFFF), exe.data() + 0x800, tsize);
    LOGI("booting SCUS_941.94: entry %08x, %u bytes", pc, tsize);

    // Fake kernel tables for PsyQ's memory card "patches" (_patch_card/_patch_card2): they
    // follow BIOS table pointers and copy fix-up code into kernel RAM. Point them at unused
    // kernel RAM so the copies are harmless; the HLE BIOS never executes them.
    wr32(0xC000 + 6 * 4, 0x0000C100);        // C0[06] ExceptionHandler
    wr32(0xC100 + 0x70, 0x3C1A0000);         // lui k0, 0x0000
    wr32(0xC100 + 0x74, 0x375AC800);         // ori k0, k0, 0xC800
    wr32(0xC400 + 0x5B * 4, 0x0000D000);     // B0[5B] ChangeClearPad

    c->r[28] = gp;
    c->r[29] = 0x801FFF00;  // SYSTEM.CNF: STACK = 801fff00
    c->r[30] = c->r[29];
    c->r[31] = 0xFFFFFFF0;
    c->cop0[12] = 0x40000000;  // COP2 usable, interrupts off (kernel enables them)
    g_boot_pc = pc;
    // top-level loop: Exec lands here with the registers set up for the new program
    rt_setjmp(cpu_exec_jb());
    rt_call(&g_cpu, g_boot_pc, 0xFFFFFFF0);
    LOGI("program returned to the BIOS");
}

}  // namespace psx
