// Crash reporter: host backtrace (symbolize with tools/symbolize.sh) + MIPS register dump.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdio>

#include "psx.h"

extern "C" uintptr_t psx_module_base() { return (uintptr_t)GetModuleHandleA(nullptr); }

void crash_dump_state() {
    HMODULE base = GetModuleHandleA(nullptr);
    void* frames[64];
    USHORT n = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < n; i++)
        fprintf(stderr, "[CRASH]   exe+%llx\n", (unsigned long long)((char*)frames[i] - (char*)base));
    const CPU& c = g_cpu;
    fprintf(stderr, "[CRASH] MIPS regs:\n");
    for (int i = 0; i < 32; i += 4)
        fprintf(stderr, "  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x\n", i, c.r[i], i + 1, c.r[i + 1], i + 2,
                c.r[i + 2], i + 3, c.r[i + 3]);
    fprintf(stderr, "  hi %08x lo %08x sr %08x cycles %lld\n", c.hi, c.lo, c.cop0[12], (long long)c.cycles);
    FILE* f = fopen("build/crash_ram.bin", "wb");
    if (f) {
        fwrite(g_ram, 1, sizeof g_ram, f);
        fwrite(g_scratch, 1, sizeof g_scratch, f);
        fclose(f);
        fprintf(stderr, "[CRASH] RAM dumped to build/crash_ram.bin\n");
    }
    fflush(stderr);
}

static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    HMODULE base = GetModuleHandleA(nullptr);
    fprintf(stderr, "\n[CRASH] code=%08lx addr=%p (exe+%llx)\n", ep->ExceptionRecord->ExceptionCode,
            ep->ExceptionRecord->ExceptionAddress,
            (unsigned long long)((char*)ep->ExceptionRecord->ExceptionAddress - (char*)base));
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION)
        fprintf(stderr, "[CRASH] %s address %p\n", ep->ExceptionRecord->ExceptionInformation[0] ? "writing" : "reading",
                (void*)ep->ExceptionRecord->ExceptionInformation[1]);
    crash_dump_state();
    return EXCEPTION_EXECUTE_HANDLER;
}

struct CrashInit {
    CrashInit() { SetUnhandledExceptionFilter(crash_filter); }
} crash_init;
#else
void crash_dump_state() {}
#endif
