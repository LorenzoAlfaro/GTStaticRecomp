// CD-ROM controller (command/response protocol, sector streaming, XA-ADPCM) and disc image.
#include <cstring>
#include <deque>
#include <string>
#include <vector>

#include "psx.h"

namespace psx {

// ---- disc image -------------------------------------------------------------------------
static FILE* g_bin = nullptr;
static int g_disc_sectors = 0;

bool cd_open(const std::string& path) {
    std::string bin = path;
    if (path.size() > 4 && (path.substr(path.size() - 4) == ".cue" || path.substr(path.size() - 4) == ".CUE")) {
        FILE* f = fopen(path.c_str(), "r");
        if (!f) return false;
        char line[1024];
        bin.clear();
        while (fgets(line, sizeof line, f)) {
            const char* q = strchr(line, '"');
            if (strncmp(line, "FILE", 4) == 0 && q) {
                const char* e = strchr(q + 1, '"');
                std::string name(q + 1, e ? e - q - 1 : 0);
                size_t slash = path.find_last_of("/\\");
                bin = (slash == std::string::npos ? "" : path.substr(0, slash + 1)) + name;
                break;
            }
        }
        fclose(f);
        if (bin.empty()) return false;
    }
    g_bin = fopen(bin.c_str(), "rb");
    if (!g_bin) return false;
    fseek(g_bin, 0, SEEK_END);
    g_disc_sectors = (int)(ftell(g_bin) / 2352);
    LOGI("disc: %s (%d sectors)", bin.c_str(), g_disc_sectors);
    return true;
}

bool cd_read_sector_raw(int lba, uint8_t* out) {
    if (!g_bin || lba < 0 || lba >= g_disc_sectors) {
        memset(out, 0, 2352);
        return false;
    }
    fseek(g_bin, (long)lba * 2352, SEEK_SET);
    return fread(out, 1, 2352, g_bin) == 2352;
}

static bool read_data(int lba, uint8_t* out2048) {
    uint8_t raw[2352];
    if (!cd_read_sector_raw(lba, raw)) return false;
    memcpy(out2048, raw + 24, 2048);
    return true;
}

bool cd_find_file(const char* name, int* lba, int* size) {
    uint8_t pvd[2048];
    if (!read_data(16, pvd)) return false;
    uint32_t dir_lba, dir_size;
    memcpy(&dir_lba, pvd + 156 + 2, 4);
    memcpy(&dir_size, pvd + 156 + 10, 4);
    std::string path = name;
    for (;;) {
        size_t slash = path.find_first_of("/\\");
        std::string part = path.substr(0, slash);
        bool found = false;
        std::vector<uint8_t> dir(((dir_size + 2047) / 2048) * 2048);
        for (uint32_t i = 0; i * 2048 < dir_size; i++) read_data((int)dir_lba + (int)i, dir.data() + i * 2048);
        for (size_t off = 0; off < dir_size;) {
            uint8_t len = dir[off];
            if (len == 0) { off = (off / 2048 + 1) * 2048; continue; }
            std::string n((char*)&dir[off + 33], dir[off + 32]);
            size_t semi = n.find(';');
            if (semi != std::string::npos) n = n.substr(0, semi);
            if (strcasecmp(n.c_str(), part.c_str()) == 0) {
                memcpy(&dir_lba, &dir[off + 2], 4);
                memcpy(&dir_size, &dir[off + 10], 4);
                found = true;
                break;
            }
            off += len;
        }
        if (!found) return false;
        if (slash == std::string::npos) {
            *lba = (int)dir_lba;
            *size = (int)dir_size;
            return true;
        }
        path = path.substr(slash + 1);
    }
}

// ---- controller state ---------------------------------------------------------------------
static inline uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline int unbcd(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static uint8_t index_reg = 0;
static std::deque<uint8_t> params;
static uint8_t int_enable = 0, int_flag = 0;
static std::vector<uint8_t> resp;
static size_t resp_pos = 0;
static uint8_t stat = 0x02;  // motor on
static uint8_t mode = 0;
static int setloc_lba = 0, cur_lba = 0;
static bool setloc_pending = false;
static bool reading = false, read_audio_only = false;
static bool muted = false;
static uint8_t filter_file = 0, filter_chan = 0;
static uint8_t vol_ll = 0x80, vol_lr = 0, vol_rl = 0, vol_rr = 0x80;
static uint8_t pend_vol[4] = {0x80, 0, 0, 0x80};
static bool busy = false;

// sector data FIFO
static uint8_t sector_raw[2352];        // last sector delivered with INT1
static uint8_t last_header[8];
static std::vector<uint8_t> data_fifo;
static size_t data_pos = 0;

struct Pending {
    uint8_t type;
    std::vector<uint8_t> bytes;
    int64_t delay;              // delay before delivery once the controller is free
    bool has_sector = false;
    uint8_t sector[2352];
};
static std::deque<Pending> queue;

static uint8_t cur_stat() {
    uint8_t s = stat;
    s &= ~0xE0;
    if (reading) s |= 0x20;
    return s;
}

static void try_deliver();

static void push(uint8_t type, std::vector<uint8_t> bytes, int64_t delay) {
    Pending p;
    p.type = type;
    p.bytes = std::move(bytes);
    p.delay = delay;
    queue.push_back(std::move(p));
    if (!scheduled(EV_CDROM) && int_flag == 0) schedule(EV_CDROM, now() + queue.front().delay);
}

static void deliver(Pending& p) {
    LOGD("CD INT%d delivered (enable %02x)", p.type, int_enable);
    int_flag = p.type;
    resp = p.bytes;
    resp_pos = 0;
    if (p.has_sector) {
        memcpy(sector_raw, p.sector, 2352);
        memcpy(last_header, p.sector + 12, 8);
    }
    if (int_flag & int_enable) irq_raise(IRQ_CDROM);
}

static void ev_cdrom(int64_t) {
    busy = false;
    if (int_flag != 0 || queue.empty()) return;
    Pending p = std::move(queue.front());
    queue.pop_front();
    deliver(p);
}

static void try_deliver() {
    if (int_flag == 0 && !queue.empty() && !scheduled(EV_CDROM))
        schedule(EV_CDROM, now() + std::max<int64_t>(queue.front().delay / 4, 1500));
}

// ---- XA ADPCM -------------------------------------------------------------------------------
static int16_t xa_old[2], xa_older[2];

static void decode_xa(const uint8_t* raw) {
    uint8_t coding = raw[19];
    bool stereo = coding & 1;
    int rate = (coding & 4) ? 18900 : 37800;
    bool eight = coding & 0x10;
    static const int K0[4] = {0, 60, 115, 98}, K1[4] = {0, 0, -52, -55};
    static int16_t out[4032 * 2];
    int n = 0;
    std::vector<int16_t> left, right;
    left.reserve(4032);
    right.reserve(2016);
    for (int g = 0; g < 18; g++) {
        const uint8_t* grp = raw + 24 + g * 128;
        int units = eight ? 4 : 8;
        for (int u = 0; u < units; u++) {
            uint8_t param = grp[4 + u];
            int shift = param & 0xF;
            if (shift > 12) shift = 9;
            int f = (param >> 4) & 3;
            int ch = stereo ? (u & 1) : 0;
            std::vector<int16_t>& dst = (stereo && (u & 1)) ? right : left;
            for (int i = 0; i < 28; i++) {
                int s;
                if (eight) s = (int16_t)(grp[16 + i * 4 + u] << 8) >> shift;
                else {
                    uint8_t b = grp[16 + i * 4 + (u >> 1)];
                    int nib = (u & 1) ? (b >> 4) : (b & 0xF);
                    s = (int16_t)(nib << 12) >> shift;
                }
                s += (xa_old[ch] * K0[f] + xa_older[ch] * K1[f] + 32) >> 6;
                s = std::clamp(s, -32768, 32767);
                xa_older[ch] = xa_old[ch];
                xa_old[ch] = (int16_t)s;
                dst.push_back((int16_t)s);
            }
        }
    }
    if (stereo) {
        n = (int)std::min(left.size(), right.size());
        for (int i = 0; i < n; i++) { out[2 * i] = left[i]; out[2 * i + 1] = right[i]; }
    } else {
        n = (int)left.size();
        for (int i = 0; i < n; i++) out[2 * i] = out[2 * i + 1] = left[i];
    }
    if (muted) return;
    // apply CD volume matrix
    for (int i = 0; i < n; i++) {
        int l = out[2 * i], r = out[2 * i + 1];
        int nl = (l * vol_ll + r * vol_rl) >> 7, nr = (r * vol_rr + l * vol_lr) >> 7;
        out[2 * i] = (int16_t)std::clamp(nl, -32768, 32767);
        out[2 * i + 1] = (int16_t)std::clamp(nr, -32768, 32767);
    }
    spu_cd_audio(out, n, rate);
}

// ---- sector reading -------------------------------------------------------------------------
static int64_t sector_cycles() { return (mode & 0x80) ? CPU_HZ / 150 : CPU_HZ / 75; }

static void ev_read(int64_t t) {
    if (!reading) return;
    uint8_t raw[2352];
    cd_read_sector_raw(cur_lba, raw);
    cur_lba++;
    schedule(EV_CDROM_READ, t + sector_cycles());

    bool form2 = raw[15] == 2 && (raw[18] & 0x20);
    bool audio = raw[15] == 2 && (raw[18] & 0x04) && form2;
    if ((mode & 0x40) && audio) {
        bool match = !(mode & 0x08) || (raw[16] == filter_file && raw[17] == filter_chan);
        if (match) decode_xa(raw);
        memcpy(last_header, raw + 12, 8);
        return;  // real-time audio sectors are not delivered to the CPU
    }
    if (read_audio_only) return;
    // drop if too many unacknowledged sectors are queued
    int queued = 0;
    for (auto& p : queue) queued += p.has_sector;
    if (queued >= 2) {
        LOGW("CD: dropping unread sector (lba %d), CPU too slow to acknowledge INT1", cur_lba - 3);
        for (auto it = queue.begin(); it != queue.end(); ++it)
            if (it->has_sector) { queue.erase(it); break; }
    }
    LOGD("CD sector %d read (%02x:%02x:%02x)", cur_lba - 1, raw[12], raw[13], raw[14]);
    Pending p;
    p.type = 1;
    p.bytes = {cur_stat()};
    p.delay = 0;
    p.has_sector = true;
    memcpy(p.sector, raw, 2352);
    queue.push_back(std::move(p));
    try_deliver();
    if (int_flag == 0 && !scheduled(EV_CDROM)) schedule(EV_CDROM, t + 100);
}

static void start_reading(bool xa_stream) {
    if (setloc_pending) {
        cur_lba = setloc_lba;
        setloc_pending = false;
    }
    reading = true;
    read_audio_only = false;
    (void)xa_stream;
    stat = (stat & ~0xC0) | 0x02;
    unschedule(EV_CDROM_READ);
    schedule(EV_CDROM_READ, now() + sector_cycles() + 20000);
}

static void stop_reading() {
    reading = false;
    unschedule(EV_CDROM_READ);
    // pending sector interrupts are discarded
    for (auto it = queue.begin(); it != queue.end();) {
        if (it->has_sector) it = queue.erase(it);
        else ++it;
    }
}

static void command(uint8_t c) {
    std::vector<uint8_t> p(params.begin(), params.end());
    params.clear();
    const int64_t ACK = 25000;
    auto P = [&](size_t i) -> uint8_t { return i < p.size() ? p[i] : 0; };
    busy = true;
    LOGD("CD cmd %02x params %zu", c, p.size());
    switch (c) {
    case 0x01: push(3, {cur_stat()}, ACK); break;  // Getstat
    case 0x02:  // Setloc
        setloc_lba = (unbcd(P(0)) * 60 + unbcd(P(1))) * 75 + unbcd(P(2)) - 150;
        setloc_pending = true;
        push(3, {cur_stat()}, ACK);
        break;
    case 0x03:  // Play (CDDA) - no audio tracks on this disc
        push(3, {cur_stat()}, ACK);
        break;
    case 0x06: case 0x1B:  // ReadN / ReadS
        push(3, {cur_stat()}, ACK);
        start_reading(c == 0x1B);
        break;
    case 0x07:  // MotorOn
        stat |= 0x02;
        push(3, {cur_stat()}, ACK);
        push(2, {cur_stat()}, 400000);
        break;
    case 0x08:  // Stop
        stop_reading();
        push(3, {cur_stat()}, ACK);
        stat &= ~0x02;
        push(2, {cur_stat()}, 400000);
        break;
    case 0x09: {  // Pause
        bool was = reading;
        uint8_t s = cur_stat();
        stop_reading();
        push(3, {s}, ACK);
        push(2, {cur_stat()}, was ? 2000000 / 6 : 7000);
        break;
    }
    case 0x0A:  // Init
        stop_reading();
        mode = 0;
        stat = 0x02;
        push(3, {cur_stat()}, 80000);
        push(2, {cur_stat()}, 120000);
        break;
    case 0x0B: muted = true; push(3, {cur_stat()}, ACK); break;
    case 0x0C: muted = false; push(3, {cur_stat()}, ACK); break;
    case 0x0D: filter_file = P(0); filter_chan = P(1); push(3, {cur_stat()}, ACK); break;
    case 0x0E: mode = P(0); push(3, {cur_stat()}, ACK); break;
    case 0x0F: push(3, {cur_stat(), mode, 0, filter_file, filter_chan}, ACK); break;  // Getparam
    case 0x10: {  // GetlocL
        std::vector<uint8_t> r(last_header, last_header + 8);
        push(3, r, ACK);
        break;
    }
    case 0x11: {  // GetlocP
        int lba = std::max(0, cur_lba - 1);
        int abs = lba + 150, rel = lba;
        push(3, {1, 1, bcd(rel / 75 / 60), bcd(rel / 75 % 60), bcd(rel % 75),
                 bcd(abs / 75 / 60), bcd(abs / 75 % 60), bcd(abs % 75)}, ACK);
        break;
    }
    case 0x12: push(3, {cur_stat()}, ACK); break;  // SetSession
    case 0x13: push(3, {cur_stat(), 1, 1}, ACK); break;  // GetTN
    case 0x14: {  // GetTD
        int t = unbcd(P(0));
        int lba = t == 0 ? g_disc_sectors : 0;
        int a = lba + 150;
        push(3, {cur_stat(), bcd(a / 75 / 60), bcd(a / 75 % 60)}, ACK);
        break;
    }
    case 0x15: case 0x16: {  // SeekL / SeekP
        stop_reading();
        if (setloc_pending) { cur_lba = setloc_lba; setloc_pending = false; }
        push(3, {(uint8_t)(cur_stat() | 0x40)}, ACK);
        push(2, {cur_stat()}, 150000);
        break;
    }
    case 0x19:  // Test
        if (P(0) == 0x20) push(3, {0x94, 0x09, 0x19, 0xC0}, ACK);
        else push(3, {cur_stat()}, ACK);
        break;
    case 0x1A:  // GetID
        push(3, {cur_stat()}, ACK);
        push(2, {0x02, 0x00, 0x20, 0x00, 'S', 'C', 'E', 'A'}, 40000);
        break;
    case 0x1E:  // ReadTOC
        push(3, {cur_stat()}, ACK);
        push(2, {cur_stat()}, 400000);
        break;
    default:
        LOGW("unhandled CD command %02x", c);
        push(5, {(uint8_t)(cur_stat() | 1), 0x40}, ACK);
        break;
    }
}

static uint8_t cd_read8_impl(int reg);
uint8_t cd_read8(int reg) {
    uint8_t v = cd_read8_impl(reg);
    if (g_log_level >= LOG_DEBUG && reg != 2) LOGD("CD rd %d.%d -> %02x", reg, index_reg, v);
    return v;
}
static uint8_t cd_read8_impl(int reg) {
    switch (reg) {
    case 0: {
        uint8_t s = index_reg & 3;
        if (params.empty()) s |= 0x08;
        if (params.size() < 16) s |= 0x10;
        if (resp_pos < resp.size()) s |= 0x20;
        if (data_pos < data_fifo.size()) s |= 0x40;
        if (busy) s |= 0x80;
        return s;
    }
    case 1: return resp_pos < resp.size() ? resp[resp_pos++] : 0;
    case 2: return data_pos < data_fifo.size() ? data_fifo[data_pos++] : 0;
    case 3:
        if (index_reg & 1) return (uint8_t)(int_flag | 0xE0);
        return (uint8_t)(int_enable | 0xE0);
    }
    return 0;
}

void cd_write8(int reg, uint8_t v) {
    LOGD("CD wr %d.%d <- %02x", reg, index_reg, v);
    if (reg == 0) { index_reg = v & 3; return; }
    switch ((reg << 2) | index_reg) {
    case (1 << 2) | 0: command(v); break;
    case (2 << 2) | 0: if (params.size() < 16) params.push_back(v); break;
    case (3 << 2) | 0:  // request register
        if (v & 0x80) {
            if (data_pos >= data_fifo.size()) {
                if (mode & 0x20) data_fifo.assign(sector_raw + 12, sector_raw + 12 + 2340);
                else data_fifo.assign(sector_raw + 24, sector_raw + 24 + 2048);
                data_pos = 0;
            }
        } else {
            data_fifo.clear();
            data_pos = 0;
        }
        break;
    case (2 << 2) | 1: int_enable = v & 0x1F; break;
    case (3 << 2) | 1:
        int_flag &= ~(v & 0x1F);
        if (v & 0x40) params.clear();
        if (int_flag == 0) {
            resp.clear();
            resp_pos = 0;
            try_deliver();
        }
        break;
    case (2 << 2) | 2: pend_vol[0] = v; break;  // L->L
    case (3 << 2) | 2: pend_vol[1] = v; break;  // L->R
    case (1 << 2) | 3: pend_vol[3] = v; break;  // R->R
    case (2 << 2) | 3: pend_vol[2] = v; break;  // R->L
    case (3 << 2) | 3:
        if (v & 0x20) { vol_ll = pend_vol[0]; vol_lr = pend_vol[1]; vol_rl = pend_vol[2]; vol_rr = pend_vol[3]; }
        break;
    default: break;
    }
}

void cd_dma_read(uint32_t* dst, int words) {
    if ((size_t)words * 4 > data_fifo.size() - std::min(data_pos, data_fifo.size()))
        LOGW("CD DMA of %d words but only %zu bytes buffered", words, data_fifo.size() - data_pos);
    for (int i = 0; i < words; i++) {
        uint32_t w = 0;
        for (int b = 0; b < 4; b++) {
            uint8_t x = data_pos < data_fifo.size() ? data_fifo[data_pos++] : 0;
            w |= (uint32_t)x << (8 * b);
        }
        dst[i] = w;
    }
}

void cd_init() {
    register_event(EV_CDROM, ev_cdrom);
    register_event(EV_CDROM_READ, ev_read);
    queue.clear();
    int_flag = 0;
    int_enable = 0x1F;  // set by the BIOS during boot
    stat = 0x02;
}

}  // namespace psx
