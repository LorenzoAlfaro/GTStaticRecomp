// Save states.
//
// Part of the game's state is the host call stack: at any moment the recompiled code is
// inside nested native functions. So the game runs on a stack of its own, and a state holds,
// besides every device's data, the bytes of that stack and a jump buffer taken at one fixed
// point: the start of the vblank event (state_point). Loading copies the stack bytes back
// from a scratch stack and jumps into the buffer, so execution continues exactly where the
// save was made.
//
// At that point the stack only holds scalars, addresses on the stack itself and addresses
// in the program (return addresses, pointers to globals); no heap pointers. A state made in
// another session is rebased: words pointing into the old program image or the old stack
// are moved by how far each moved. On Windows gt.exe has a fixed base and the stack a fixed
// address, so nothing moves; on Android libmain.so loads at a random address every launch.
// The build id (a hash of the program's code) makes sure the code is the same.
#include "state.h"

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#else
#include <link.h>
#include <sys/mman.h>
#include <sys/stat.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>

#include "psx.h"

namespace psx {

void frontend_after_load();
void cpu_jmpbufs(void (*fn)(void* p, size_t bytes));

static constexpr uintptr_t kStackHint = sizeof(void*) == 8 ? (uintptr_t)0x0000000700000000ull : 0;
static constexpr size_t kStackSize = 32u << 20;
static constexpr uint32_t kMagic = 0x53535447;  // "GTSS"
static constexpr uint32_t kVersion = 2;

static uint8_t* stack_base = nullptr;
static uint8_t* stack_top = nullptr;
static uintptr_t image_lo = 0, image_hi = 0;  // the program (gt.exe / libmain.so) in memory
static uint64_t build_id = 0;

static int req = 0, req_slot = 1;  // 1 save, 2 load
static rt_jmp_buf point_jb;
static std::string message;
static double message_time = -1e9;

// loading: what finish_load copies once it runs on the scratch stack
static std::vector<uint8_t> load_stack;
static uintptr_t load_sp = 0;
alignas(16) static uint8_t scratch_stack[64 * 1024];

static double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

static void set_message(const std::string& m) {
    message = m;
    message_time = now_s();
    LOGI("%s", m.c_str());
}

const char* state_message() { return now_s() - message_time < 3.0 ? message.c_str() : ""; }

bool state_available() { return stack_base != nullptr; }

static uint64_t fnv(uint64_t h, const uint8_t* p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// ---- platform: the program's extent and build id, the emulation stack ---------------------

#ifdef _WIN32
static void find_image(const char* exe_path) {
    auto* dos = (IMAGE_DOS_HEADER*)GetModuleHandleA(nullptr);
    auto* nt = (IMAGE_NT_HEADERS*)((uint8_t*)dos + dos->e_lfanew);
    image_lo = (uintptr_t)dos;
    image_hi = image_lo + nt->OptionalHeader.SizeOfImage;
    uint64_t h = 1469598103934665603ull;
    if (FILE* f = fopen(exe_path, "rb")) {
        uint8_t buf[65536];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) h = fnv(h, buf, n);
        fclose(f);
    }
    build_id = h;
}
static uint8_t* alloc_stack() {
    // GT_STACK_ANYWHERE=1: let the OS pick the address (tests rebasing of loaded states)
    void* hint = getenv("GT_STACK_ANYWHERE") ? nullptr : (void*)kStackHint;
    void* p = VirtualAlloc(hint, kStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!p) p = VirtualAlloc(nullptr, kStackSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return (uint8_t*)p;
}
static void make_dir(const char* d) { _mkdir(d); }
#else
// the shared object (or executable) containing this code: its PT_LOAD extent, and a hash of
// its executable segments as they are in memory (position-independent code: the same bytes
// at any load address)
void find_image_marker() {}
static int phdr_cb(dl_phdr_info* info, size_t, void*) {
    uintptr_t self = (uintptr_t)&find_image_marker;
    uintptr_t lo = UINTPTR_MAX, hi = 0;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_LOAD) continue;
        lo = std::min<uintptr_t>(lo, info->dlpi_addr + ph.p_vaddr);
        hi = std::max<uintptr_t>(hi, info->dlpi_addr + ph.p_vaddr + ph.p_memsz);
    }
    if (self < lo || self >= hi) return 0;
    image_lo = lo;
    image_hi = hi;
    uint64_t h = 1469598103934665603ull;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const auto& ph = info->dlpi_phdr[i];
        if (ph.p_type == PT_LOAD && (ph.p_flags & PF_X))
            h = fnv(h, (const uint8_t*)(info->dlpi_addr + ph.p_vaddr), ph.p_filesz);
    }
    build_id = h;
    return 1;
}
static void find_image(const char*) { dl_iterate_phdr(phdr_cb, nullptr); }
static uint8_t* alloc_stack() {
    void* p = mmap((void*)kStackHint, kStackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : (uint8_t*)p;
}
static void make_dir(const char* d) { mkdir(d, 0777); }
#endif

void state_init(const char* exe_path) {
    find_image(exe_path);
    stack_base = alloc_stack();
    if (stack_base) stack_top = stack_base + kStackSize;
    else LOGW("save states disabled: cannot allocate the emulation stack");
}

void state_run(void (*fn)()) {
    if (!stack_base) {
        fn();
        return;
    }
#ifdef _WIN32
    // tell the OS where the stack is (exception dispatch checks frames against these bounds)
    NT_TIB* tib = (NT_TIB*)NtCurrentTeb();
    void *old_base = tib->StackBase, *old_limit = tib->StackLimit;
    tib->StackBase = stack_top;
    tib->StackLimit = stack_base;
#endif
    rt_run_on_stack(stack_top, fn);
#ifdef _WIN32
    tib->StackBase = old_base;
    tib->StackLimit = old_limit;
#endif
}

void state_request_save(int slot) {
    if (!stack_base) { set_message("save states are not available (see the log)"); return; }
    req = 1;
    req_slot = slot;
}
void state_request_load(int slot) {
    if (!stack_base) { set_message("save states are not available (see the log)"); return; }
    req = 2;
    req_slot = slot;
}

static std::string slot_path(int slot) {
    make_dir("states");
    return "states/slot" + std::to_string(slot) + ".gtss";
}

struct Header {
    uint32_t magic, version;
    uint64_t build_id;
    uint64_t pointer_size;
    uint64_t image_lo, image_hi;  // where the program was
    uint64_t stack_lo, stack_hi;  // where the emulation stack was
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
    uintptr_t sp = rt_jmp_sp(point_jb);  // stack pointer after the rt_setjmp in state_point
    if (sp < (uintptr_t)stack_base || sp >= (uintptr_t)stack_top) {
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
    h.pointer_size = sizeof(void*);
    h.image_lo = image_lo;
    h.image_hi = image_hi;
    h.stack_lo = (uintptr_t)stack_base;
    h.stack_hi = (uintptr_t)stack_top;
    h.stack_len = (uintptr_t)stack_top - sp;
    h.data_len = data.size();
    memcpy(h.jb, point_jb, sizeof h.jb);
    std::string path = slot_path(slot);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) {
        set_message("save failed: cannot write " + path);
        return;
    }
    bool ok = fwrite(&h, sizeof h, 1, f) == 1 && fwrite((void*)sp, 1, h.stack_len, f) == h.stack_len &&
              fwrite(data.data(), 1, data.size(), f) == data.size();
    ok = fclose(f) == 0 && ok;
    set_message(ok ? "saved slot " + std::to_string(slot) : "save failed: write error");
}

// Rebasing a state from another session: words that pointed into the old program image or
// the old emulation stack are moved by the distance each one moved
static uintptr_t rb_img_lo, rb_img_hi, rb_stk_lo, rb_stk_hi;
static intptr_t rb_img_delta, rb_stk_delta;
static size_t rb_count;
static void rebase_words(void* p, size_t bytes) {
    auto* w = (uintptr_t*)p;
    for (size_t i = 0; i < bytes / sizeof(uintptr_t); i++) {
        uintptr_t v = w[i];
        if (v >= rb_img_lo && v < rb_img_hi) { w[i] = v + rb_img_delta; rb_count++; }
        else if (v >= rb_stk_lo && v < rb_stk_hi) { w[i] = v + rb_stk_delta; rb_count++; }
    }
}

static void finish_load() {
    // running on the scratch stack: the emulation stack can be overwritten now
    memcpy((void*)load_sp, load_stack.data(), load_stack.size());
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
    if (!ok || h.magic != kMagic) err = "not a save state";
    else if (h.version != kVersion) err = "made by an older version";
    else if (h.build_id != build_id || h.pointer_size != sizeof(void*)) err = "made by a different build";
    else if (h.stack_len > kStackSize || h.stack_len > (uintptr_t)(stack_top - stack_base) ||
             h.image_hi - h.image_lo != image_hi - image_lo || h.data_len > (256u << 20))
        err = "corrupt";
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
    memcpy(point_jb, h.jb, sizeof point_jb);
    rb_img_lo = (uintptr_t)h.image_lo;
    rb_img_hi = (uintptr_t)h.image_hi;
    rb_stk_lo = (uintptr_t)h.stack_lo;
    rb_stk_hi = (uintptr_t)h.stack_hi;
    rb_img_delta = (intptr_t)(image_lo - rb_img_lo);
    rb_stk_delta = (intptr_t)((uintptr_t)stack_top - rb_stk_hi);
    rb_count = 0;
    if (rb_img_delta || rb_stk_delta) {
        rebase_words(stack.data(), stack.size());
        rebase_words(point_jb, sizeof point_jb);
        cpu_jmpbufs(rebase_words);
        LOGI("save state rebased: program %+lld, stack %+lld bytes (%zu words)", (long long)rb_img_delta,
             (long long)rb_stk_delta, rb_count);
    }
    load_stack.swap(stack);
    load_sp = (uintptr_t)stack_top - h.stack_len;
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
