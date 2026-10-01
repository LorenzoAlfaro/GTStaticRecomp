// Save states.
//
// Part of the game's state is the host call stack: at any moment the recompiled code is
// inside nested native functions. So the game runs on a stack at a fixed virtual address
// (and gt.exe is linked without ASLR), and a state holds, besides every device's data, the
// bytes of that stack and a jump buffer taken at one fixed point: the start of the vblank
// event (state_point). Loading copies the stack bytes back from a scratch stack and jumps
// into the buffer, so execution continues exactly where the save was made. The stack only
// holds scalars and pointers into gt.exe and its globals at that point, so a state stays
// valid in a later session of the same build (the build id is checked).
#include "state.h"

#include <windows.h>
#include <direct.h>

#include <cstdio>
#include <string>

#include "pgxp.h"
#include "psx.h"

namespace psx {

void frontend_after_load();

static constexpr uintptr_t kStackAddr = 0x0000000700000000ull;  // fixed emulation stack
static constexpr size_t kStackSize = 32u << 20;
static constexpr uint32_t kMagic = 0x53535447;  // "GTSS"
static constexpr uint32_t kVersion = 1;

static uint8_t* stack_base = nullptr;
static uint8_t* stack_top = nullptr;
static uint64_t build_id = 0;
static bool fixed_base = false;

static int req = 0, req_slot = 1;  // 1 save, 2 load
static rt_jmp_buf point_jb;
static std::string message;
static uint64_t message_tick = 0;

// loading: what finish_load copies once it runs on the scratch stack
static std::vector<uint8_t> load_stack;
static uintptr_t load_rsp = 0;
alignas(16) static uint8_t scratch_stack[64 * 1024];

static void set_message(const std::string& m) {
    message = m;
    message_tick = GetTickCount64();
    LOGI("%s", m.c_str());
}

const char* state_message() {
    return GetTickCount64() - message_tick < 3000 ? message.c_str() : "";
}

bool state_available() { return stack_base != nullptr; }

void state_init(const char* exe_path) {
    // build id: hash of gt.exe, so a state is only loaded into the build that made it
    uint64_t h = 1469598103934665603ull;
    if (FILE* f = fopen(exe_path, "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0)
            for (size_t i = 0; i < n; i++) h = (h ^ buf[i]) * 1099511628211ull;
        fclose(f);
    }
    build_id = h;
    // gt.exe must sit at its preferred base for stack contents to stay valid across sessions
    auto* dos = (IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS64*)((uint8_t*)dos + dos->e_lfanew);
    fixed_base = !(nt->OptionalHeader.DllCharacteristics & IMAGE_DLLCHARACTERISTICS_DYNAMIC_BASE) &&
                 (uintptr_t)dos == nt->OptionalHeader.ImageBase;
    stack_base = (uint8_t*)VirtualAlloc((void*)kStackAddr, kStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (stack_base) stack_top = stack_base + kStackSize;
    if (!stack_base) LOGW("save states disabled: cannot allocate the emulation stack at %llx", (unsigned long long)kStackAddr);
    else if (!fixed_base) LOGW("gt.exe is relocated: save states only load in the session that made them");
}

void state_run(void (*fn)()) {
    if (!stack_base) {
        fn();
        return;
    }
    // tell the OS where the stack is (exception dispatch checks frames against these bounds)
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    void *old_base = tib->StackBase, *old_limit = tib->StackLimit;
    tib->StackBase = stack_top;
    tib->StackLimit = stack_base;
    rt_run_on_stack(stack_top, fn);
    tib->StackBase = old_base;
    tib->StackLimit = old_limit;
}

void state_request_save(int slot) {
    if (!stack_base) { set_message("save states are not available (see gt.log)"); return; }
    req = 1;
    req_slot = slot;
}
void state_request_load(int slot) {
    if (!stack_base) { set_message("save states are not available (see gt.log)"); return; }
    req = 2;
    req_slot = slot;
}

static std::string slot_path(int slot) {
    _mkdir("states");
    return "states/slot" + std::to_string(slot) + ".gtss";
}

struct Header {
    uint32_t magic, version;
    uint64_t build_id;
    uint64_t image_base;
    uint64_t stack_top;
    uint64_t stack_len;
    uint64_t data_len;
    uint64_t jb[32];
};

static void all_modules(StateIO& s) {
    hw_state(s);
    cpu_state(s);
    gpu_state(s);
    gte_state(s);
    spu_state(s);
    cd_state(s);
    sio_state(s);
    mdec_state(s);
    bios_state(s);
    pgxp_state(s);
}

__attribute__((noinline)) static void save_file(int slot) {
    uintptr_t rsp = (uintptr_t)point_jb[8];  // rsp after the rt_setjmp in state_point
    if (rsp < (uintptr_t)stack_base || rsp >= (uintptr_t)stack_top) {
        set_message("save failed: not on the emulation stack");
        return;
    }
    std::vector<uint8_t> data;
    StateIO s(false, data);
    all_modules(s);
    Header h{};
    h.magic = kMagic;
    h.version = kVersion;
    h.build_id = build_id;
    h.image_base = (uintptr_t)GetModuleHandleA(nullptr);
    h.stack_top = (uintptr_t)stack_top;
    h.stack_len = (uintptr_t)stack_top - rsp;
    h.data_len = data.size();
    memcpy(h.jb, point_jb, sizeof h.jb);
    std::string path = slot_path(slot);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        set_message("save failed: cannot write " + path);
        return;
    }
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite((void*)rsp, 1, h.stack_len, f) == h.stack_len &&
              fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    set_message(ok ? "saved slot " + std::to_string(slot) : "save failed: write error");
}

static void finish_load() {
    // running on the scratch stack: the emulation stack can be overwritten now
    memcpy((void*)load_rsp, load_stack.data(), load_stack.size());
    rt_longjmp(point_jb, 1);
}

// returns only if the state can't be loaded (nothing has been changed then)
__attribute__((noinline)) static void load_file(int slot) {
    std::string path = slot_path(slot);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) {
        set_message("slot " + std::to_string(slot) + " is empty");
        return;
    }
    Header h{};
    bool ok = fread(&h, sizeof h, 1, f) == 1;
    std::string err;
    if (!ok || h.magic != kMagic || h.version != kVersion) err = "not a save state";
    else if (h.build_id != build_id) err = "made by a different build of gt.exe";
    else if (h.image_base != (uintptr_t)GetModuleHandleA(nullptr) || h.stack_top != (uintptr_t)stack_top)
        err = "made in a session with a different memory layout";
    else if (h.stack_len > kStackSize || h.data_len > (256u << 20)) err = "corrupt";
    std::vector<uint8_t> stack, data;
    if (err.empty()) {
        stack.resize(h.stack_len);
        data.resize(h.data_len);
        if (fread(stack.data(), 1, stack.size(), f) != stack.size() || fread(data.data(), 1, data.size(), f) != data.size())
            err = "truncated";
    }
    fclose(f);
    if (!err.empty()) {
        set_message("cannot load slot " + std::to_string(slot) + ": " + err);
        return;
    }
    StateIO s(true, data);
    all_modules(s);
    if (!s.ok || s.pos != data.size()) fatal("save state %s is inconsistent with this build", path.c_str());
    load_stack.swap(stack);
    load_rsp = (uintptr_t)stack_top - h.stack_len;
    memcpy(point_jb, h.jb, sizeof point_jb);
    set_message("loaded slot " + std::to_string(slot));
    rt_run_on_stack(scratch_stack + sizeof scratch_stack, finish_load);
}

__attribute__((noinline)) void state_point() {
    if (!req || !stack_base) return;
    int r = req;
    req = 0;
    if (r == 1) {
        if (rt_setjmp(point_jb) != 0) {
            // a load jumped here: this is the moment that state was saved
            load_stack.clear();
            load_stack.shrink_to_fit();
            frontend_after_load();
            return;
        }
        save_file(req_slot);
        return;
    }
    load_file(req_slot);
}

}  // namespace psx
