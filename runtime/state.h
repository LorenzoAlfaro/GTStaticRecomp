// Save states: a serializer that each runtime module uses in one function for both saving
// and loading (so the two can't drift apart), and the state API (state.cpp).
#pragma once
#include <cstdint>
#include <cstring>
#include <deque>
#include <type_traits>
#include <vector>

namespace psx {

struct StateIO {
    bool loading;
    std::vector<uint8_t>& buf;
    size_t pos = 0;
    bool ok = true;

    StateIO(bool load, std::vector<uint8_t>& b) : loading(load), buf(b) {}

    void bytes(void* p, size_t n) {
        if (!loading) {
            buf.insert(buf.end(), (const uint8_t*)p, (const uint8_t*)p + n);
            return;
        }
        if (pos + n > buf.size()) {
            ok = false;
            memset(p, 0, n);
            return;
        }
        memcpy(p, buf.data() + pos, n);
        pos += n;
    }
    template <class T>
    void io(T& v) {
        static_assert(std::is_trivially_copyable_v<T>, "use vec/deq or a custom function");
        bytes(&v, sizeof v);
    }
    template <class T>
    void vec(std::vector<T>& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        uint64_t n = v.size();
        io(n);
        if (loading) v.resize(n);
        if (n) bytes(v.data(), n * sizeof(T));
    }
    template <class T>
    void deq(std::deque<T>& d) {
        static_assert(std::is_trivially_copyable_v<T>);
        uint64_t n = d.size();
        io(n);
        if (loading) d.resize(n);
        for (auto& x : d) io(x);
    }
    // a marker between modules, so a mismatch is caught where it happens
    void section(uint32_t tag) {
        uint32_t t = tag;
        io(t);
        if (loading && t != tag) ok = false;
    }
};

// per-module state (each in its own source file)
void hw_state(StateIO& s);
void cpu_state(StateIO& s);
void gpu_state(StateIO& s);
void gte_state(StateIO& s);
void spu_state(StateIO& s);
void cd_state(StateIO& s);
void sio_state(StateIO& s);
void mdec_state(StateIO& s);
void bios_state(StateIO& s);
void pgxp_state(StateIO& s);

// state.cpp
void state_init(const char* exe_path);              // build id, fixed emulation stack
void state_run(void (*fn)());                       // run fn on the fixed stack (or the current one)
void state_point();                                 // called at every vblank: handles requests
void state_request_save(int slot);
void state_request_load(int slot);
bool state_available();
const char* state_message();                        // last status message for the window title

}  // namespace psx
