// Internal runtime interfaces (C++ only).
#pragma once
#include <cstdint>
#include <cstdio>
#include <string>

#include "recomp.h"

extern CPU g_cpu;

namespace psx {

constexpr int64_t CPU_HZ = 33868800;
// NTSC: 263 lines of 3413 GPU clocks (53.693175 MHz) -> ~2152.8 CPU cycles per line
constexpr double CYCLES_PER_LINE_F = 3413.0 * 33868800.0 / 53693175.0;
constexpr int LINES_PER_FRAME = 263;
constexpr int VBLANK_START_LINE = 240;

// ---- logging -----------------------------------------------------------------
enum LogLevel { LOG_ERROR, LOG_WARN, LOG_INFO, LOG_DEBUG };
extern int g_log_level;
void log(int level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
#define LOGE(...) ::psx::log(::psx::LOG_ERROR, __VA_ARGS__)
#define LOGW(...) ::psx::log(::psx::LOG_WARN, __VA_ARGS__)
#define LOGI(...) ::psx::log(::psx::LOG_INFO, __VA_ARGS__)
#define LOGD(...) do { if (::psx::g_log_level >= ::psx::LOG_DEBUG) ::psx::log(::psx::LOG_DEBUG, __VA_ARGS__); } while (0)
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));

// ---- scheduler ---------------------------------------------------------------
enum EventId {
    EV_VBLANK,     // start of vblank (frame end)
    EV_VBLANK_END, // end of vblank
    EV_TIMER0, EV_TIMER1, EV_TIMER2,
    EV_CDROM,
    EV_CDROM_READ,
    EV_SIO,
    EV_SPU,
    EV_DMA,
    EV_COUNT
};
using EventFn = void (*)(int64_t now);
void sched_init();
void schedule(int id, int64_t when);  // absolute cycle count
void unschedule(int id);
bool scheduled(int id);
inline int64_t now() { return g_cpu.cycles; }
void run_events();  // run everything due
void register_event(int id, EventFn fn);

// ---- interrupts ----------------------------------------------------------------
enum Irq { IRQ_VBLANK = 0, IRQ_GPU = 1, IRQ_CDROM = 2, IRQ_DMA = 3, IRQ_TMR0 = 4, IRQ_TMR1 = 5,
           IRQ_TMR2 = 6, IRQ_SIO0 = 7, IRQ_SIO1 = 8, IRQ_SPU = 9, IRQ_PIO = 10 };
extern uint32_t i_stat, i_mask;
void irq_raise(int n);
bool irq_pending();
void irq_recheck();  // make sure a pending/enabled interrupt gets delivered promptly

// ---- hardware register access (hw.cpp) ------------------------------------------
uint32_t hw_read(uint32_t phys, int size);
void hw_write(uint32_t phys, uint32_t v, int size);
void hw_init();
extern uint64_t g_dma_generation;
extern uintptr_t g_slow_caller;
extern FILE* g_hash_log;
extern int64_t g_dump_frame;
extern int64_t g_trace_start, g_trace_end;
extern bool g_prim_log;
extern std::string g_dump_path;  // host return address of the last slow memory access  // bumped when DMA writes RAM (overlay cache)
int64_t hblank_count(int64_t cycles);
int64_t frame_count();

// ---- GPU ---------------------------------------------------------------------------
void gpu_init();
void gpu_write_gp0(uint32_t v);
void gpu_write_gp1(uint32_t v);
uint32_t gpu_read();
uint32_t gpu_stat();
void gpu_vblank(bool in_vblank);
struct DisplayInfo {
    int x, y, w, h;  // area in VRAM
    bool rgb24;
    bool enabled;
};
DisplayInfo gpu_display();
const uint16_t* gpu_vram();  // internal resolution: (1024*scale) x (512*scale)
uint16_t gpu_vram_native(int x, int y);
void gpu_sync();  // finish queued rendering before reading VRAM
void gpu_convert_display(uint32_t* dst, int pitch_px, int x, int y, int w, int h);
int gpu_scale();
void gpu_set_scale(int scale);  // 1, 2, 4 or 8 (rounded down to a power of two)
void gpu_set_dither(bool on);
bool gpu_dither();

// ---- enhancements (hooks.cpp) -------------------------------------------------------
void set_widescreen(bool on);
bool widescreen();

// ---- SPU ---------------------------------------------------------------------------
void spu_init();
uint16_t spu_read16(uint32_t off);
void spu_write16(uint32_t off, uint16_t v);
void spu_dma_write(const uint32_t* src, int words);
void spu_dma_read(uint32_t* dst, int words);
void spu_cd_audio(const int16_t* stereo, int frames, int rate);  // XA / CDDA input
void spu_run_until(int64_t cycles);

// ---- CD-ROM -------------------------------------------------------------------------
bool cd_open(const std::string& path);
void cd_init();
uint8_t cd_read8(int reg);
void cd_write8(int reg, uint8_t v);
void cd_dma_read(uint32_t* dst, int words);
bool cd_read_sector_raw(int lba, uint8_t* out2352);
bool cd_find_file(const char* name, int* lba, int* size);

// ---- SIO0 (pads / memory cards) ------------------------------------------------------
void sio_init();
uint32_t sio_read(uint32_t off, int size);
void sio_write(uint32_t off, uint32_t v, int size);
struct PadState {
    uint16_t buttons = 0xFFFF;  // active low, PS1 bit order
    uint8_t lx = 0x80, ly = 0x80, rx = 0x80, ry = 0x80;
    bool analog = false;
    bool connected = true;
};
extern PadState g_pads[2];

// ---- MDEC ---------------------------------------------------------------------------
void mdec_init();
uint32_t mdec_read(int reg);
void mdec_write(int reg, uint32_t v);
void mdec_dma_in(const uint32_t* src, int words);
void mdec_dma_out(uint32_t* dst, int words);

// ---- BIOS (HLE) ----------------------------------------------------------------------
void bios_init();
void bios_boot(CPU* c);
void bios_call(CPU* c, uint32_t vector);  // A0/B0/C0 with function number in t1
void bios_exception(CPU* c);             // interrupt entry
void bios_syscall(CPU* c, uint32_t code);
bool bios_in_exception();

// ---- CPU / dispatch (cpu.cpp) -----------------------------------------------------------
void cpu_init();
bool cpu_lookup(uint32_t addr, RecompFunc* out);
void interp_run(CPU* c, uint32_t pc, uint32_t stop);
extern bool g_pure_interp;
extern bool g_verify;       // --verify: differential native-vs-interpreter checking
extern bool g_verify_io;    // set when hardware/BIOS I/O happens during a verified call
extern bool g_in_verify;
void verify_abandon();
extern bool g_check_sregs;
void cpu_raise_interrupt_if_pending(CPU* c);

// ---- frontend (main.cpp) --------------------------------------------------------------
void frontend_vblank();
void frontend_audio(const int16_t* stereo, int frames);
void frontend_poll_input();
extern bool g_quit;
extern std::string g_data_dir;

}  // namespace psx
