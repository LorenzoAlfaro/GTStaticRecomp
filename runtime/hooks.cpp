// Enhancement hooks: small register/memory patches applied at fixed instructions of the
// game. The addresses must match HOOKS in gen/recomp.py. Gran Turismo needs none so far:
// widescreen is done in the GTE (runtime/gte.cpp).
#include "recomp.h"
#include "psx.h"

int g_hooks_on = 0;

int rt_is_hook(uint32_t) { return 0; }

void rt_hook(CPU*, uint32_t) {}
