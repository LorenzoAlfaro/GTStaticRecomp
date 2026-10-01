// Sub-pixel vertex table, see pgxp.h.
#include "pgxp.h"

namespace psx {

bool g_pgxp = false;

namespace {

struct Entry {
    uint32_t key;
    bool used;
    float x, y, z;
};

constexpr int kBits = 16;
Entry table[1 << kBits];

inline uint32_t slot(uint32_t key) { return (key * 2654435761u) >> (32 - kBits); }

}  // namespace

void pgxp_store(uint32_t sxy, float x, float y, float z) {
    Entry& e = table[slot(sxy)];
    e.key = sxy;
    e.used = true;
    e.x = x;
    e.y = y;
    e.z = z;
}

bool pgxp_lookup(uint32_t sxy, float* x, float* y, float* z) {
    const Entry& e = table[slot(sxy)];
    if (!e.used || e.key != sxy) return false;
    *x = e.x;
    *y = e.y;
    if (z) *z = e.z;
    return true;
}

}  // namespace psx
