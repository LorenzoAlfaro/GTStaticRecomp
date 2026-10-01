// Sub-pixel vertex precision ("PGXP"-style). The GTE rounds projected screen coordinates
// to whole pixels, which makes geometry wobble at any resolution. RTPS/RTPT record the
// unrounded position of every SXY they output, keyed by the rounded 32-bit SXY word; the
// GPU looks up each vertex word it receives. A stale or colliding entry has the same
// rounded value, so the worst case is an error below one native pixel (never worse than
// the original).
#pragma once
#include <cstdint>

namespace psx {

extern bool g_pgxp;

void pgxp_store(uint32_t sxy, float x, float y, float z);
bool pgxp_lookup(uint32_t sxy, float* x, float* y, float* z);

}  // namespace psx
