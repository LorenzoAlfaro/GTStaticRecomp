# Recompilation lessons log

A running record of the blockers hit while building static recompilations, starting with
Crash Team Racing (PS1, SCUS-94426). The goal is to make the next recomp faster: each entry
says what broke, why, how it was found, and what to do up front next time.

**How to maintain this file:** add an entry when something costs more than a few minutes,
update the ranking table, and move items out of *Open issues* when they are solved. Keep
entries short: symptom, root cause, fix, lesson.

**Time rating** (how long it took to get working, relative within a project):

| Rating | Meaning |
|---|---|
| 5 | Biggest time sink: many iterations, needed new tooling to even see the cause |
| 4 | Major: hours of investigation, non-obvious root cause |
| 3 | Significant: several attempts, but the cause was findable with existing tools |
| 2 | Moderate: one or two attempts once noticed |
| 1 | Quick: minutes |

Ratings are estimates of relative effort made after the fact, not measured hours.

---

## Project: Crash Team Racing (PS1, USA) — `CTRStaticRecomp`

Approach: Python recompiler (MIPS R3000A → C, one C function per game function) plus a C++
runtime emulating the PS1 hardware (HLE BIOS, GTE, software GPU, SPU, CD-ROM/XA, SIO), SDL2
frontend. An interpreter runs code that cannot be recompiled safely.

### Ranking

| # | Blocker | Area | Time | Status |
|---|---|---|---|---|
| 1 | Hand-written render assembly with non-standard control flow | Recompiler design | 5 | Solved (interpreter regions) |
| 2 | Silent divergence: `__builtin_longjmp` did not restore XMM registers | Runtime / host ABI | 5 | Solved (custom setjmp) |
| 3 | Interrupt handlers ran on the game's stack, which the render asm uses as data | HLE BIOS | 4 | Solved (kernel stack) |
| 4 | Game thread scheduler abandons a tick mid-call (`ThTick_SetAndExec` / `FastRET`) | Control flow | 4 | Solved (unwind point) |
| 5 | First attempt on the decomp's PC port: build problems, then unplayable | Strategy | 4 | Abandoned for full recomp |
| 6 | Function discovery: data decoded as code, delay-slot targets, jump tables | Recompiler | 3 | Solved |
| 7 | Boot hangs from timing: VSync timeout, missing CD interrupts | Hardware timing | 3 | Solved |
| 8 | 4x HD rendering too slow (34 fps) | GPU / performance | 3 | Solved (threads, ~120 fps) |
| 9 | Shadow return stack overflow / garbage stack pointer | Runtime | 2 | Solved |
| 10 | `__builtin_setjmp` clobbering locals of the function that called it | Runtime / compiler | 2 | Solved |
| 11 | Fixed-size DMA buffer overflow corrupting host memory | Runtime | 2 | Solved |
| 12 | Overlays share load addresses | Recompiler design | 2 | Solved (residency check) |
| 13 | PsyQ `_patch_card` needs real BIOS kernel tables | HLE BIOS | 2 | Solved (fake tables) |
| 14 | Controls not recognized (focus, vJoy, wrong receiver) | Frontend | 2 | Solved |
| 15 | Tooling friction on Windows (shell escaping, encodings, locks) | Tooling | 2 | Ongoing, workarounds known |
| 16 | Menu text drawn as orange blocks | GPU | 1 | Solved |
| 17 | Widescreen hook points | Enhancement | 1 | Solved (HUD still stretched) |
| 18 | Misdiagnosed "aspect ratio bug" | Enhancement | 1 | Not a bug |
| 19 | Hairline seams in 2D art at high resolution | GPU | 1 | Open |

### Details

#### 1. Hand-written render assembly (rating 5)
- **Symptom:** crashes and corrupted rendering in the level/model renderers; recompiled
  functions returned to the wrong place.
- **Root cause:** CTR's renderer (≈0x80069BB0–0x800715E8 plus overlays 226–229) is hand-written
  assembly that does not follow the C calling convention:
  - it uses `jalr t2` with a non-`ra` link register as a continuation;
  - flattened loops return several call levels at once;
  - it uses `ra` and `sp` as data registers.
- **What was tried:** shadow return frames (`rt_frame_push/pop`, `rt_return_to`, non-local
  return checks). These handled some cases but stayed fragile.
- **Fix:** mark those address ranges *interpreter-only*. The interpreter follows real MIPS
  control flow, so everything works, and it is still fast enough for full speed.
- **Lesson:** identify hand-written asm early. Signs:
  - `jalr` with `rd != 31`;
  - writes to `ra`/`sp` as data;
  - jumps into the middle of other functions.

  Route that code to an interpreter instead of forcing it through the recompiler. A fast
  interpreter is a required part of a recomp, not just a debugging aid.

#### 2. XMM registers not restored by `__builtin_longjmp` (rating 5)
- **Symptom:** recompiled and interpreted runs diverged at frame 154 with no crash, just
  different RAM.
- **How found:** built a lockstep tool (`tools/lockstep.ps1`) that compares RAM hashes per
  frame. Then:
  - a `PRECISE` recompiler mode (per-instruction cycle accounting) so recompiled code and the
    interpreter match cycle-for-cycle;
  - `--trace A B` logging every function entry with cycle counts, to find the first
    differing call.
- **Root cause:** `__builtin_setjmp/longjmp` do not save or restore callee-saved XMM6–XMM15 on
  Win64. Clang keeps values in those registers across calls, so a non-local jump returned
  with stale values.
- **Fix:** custom `rt_setjmp/rt_longjmp` in assembly (`runtime/jmp.c`). They save every Win64
  callee-saved register including XMM6–15 and MXCSR/FPCW.
- **Lesson:** never use compiler builtins for non-local control flow in a recomp runtime. Write
  your own, matched to the host ABI. Build the lockstep/differential tooling at the start;
  without it this bug was nearly invisible.

#### 3. Interrupt handlers on the game's stack (rating 4)
- **Symptom:** primitive buffer overflow in the Naughty Dog box intro, thread pool corruption,
  crash with "KART" in the logs.
- **Root cause:** the HLE BIOS ran interrupt-chain handlers on whatever `sp` the game had. The
  render asm uses `sp` as a data pointer into the primitive buffer, so interrupt frames were
  written into primitive data.
- **Fix:** run the interrupt handler chain on a separate kernel stack (`0x8000F000`), as the
  real BIOS does.
- **Lesson:** emulate the BIOS's stack discipline exactly. Never assume `sp` is a stack while
  game code runs.

#### 4. Thread scheduler unwinding (rating 4)
- **Symptom:** game threads resumed in the wrong place; a single tick ran twice.
- **Root cause:** `ThTick_SetAndExec` and `ThTick_FastRET` abandon the current tick and resume
  `ThTick_RunBucket`, a non-local exit across recompiled C frames.
- **Fix:** an unwind point at the call site (`rt_unwind_call` at 0x80071670) and overrides for
  the two functions, using `rt_setjmp/longjmp` (with stale-frame detection).
- **Lesson:** look for game-level schedulers and coroutines that switch stacks or jump between
  call chains. Each needs an explicit unwind model in the runtime.

#### 5. Decomp PC port route (rating 4)
- **What happened:** the first plan was to build on the CTR-ModSDK decompilation's PC port.
- **Problems along the way:**
  - macro syntax errors with clang;
  - pointer-type errors;
  - OpenAL needed a static build, then clashed with PsyCross's EFX symbols;
  - `lld` rejected `-Ttext`;
  - the BIGFILE had to be extracted from the disc;
  - the profiler hung the intro.
- **Why it was abandoned:** once it built, races used a placeholder free camera. The port was
  not playable, so the project switched to a full static recomp.
- **Lesson:** check how complete a decomp's PC port is *in-game* before investing in its
  build. A static recomp of the original binary is complete by construction. Decomp
  projects are still very valuable as references (see *Provenance*).

#### 6. Function discovery (rating 3)
- **Problems:**
  - data was decoded as code;
  - branches targeted delay slots;
  - functions fell through into code that was never discovered;
  - jump-table bases were set up several instructions before the jump.
- **Fixes:**
  - **Seeds:** strong seeds (symbols, `jal` targets, entry point) are separate from weak seeds
    (pointer scan, `lui/addiu` constants), and weak seeds must pass a `plausible()` check.
  - **Delay slots:** reaching an address by walking is tracked separately from reaching it as
    a delay slot.
  - **Fall-through:** a missing fall-through calls `rt_fallthrough` into the interpreter.
  - **Jump tables:** the table base is found by recursive constant tracking
    (`reg_const_before`).
- **Lesson:** a symbol map makes discovery much easier. Even so, always back the recompiled
  code with the interpreter, so a discovery miss costs speed rather than correctness.

#### 7. Timing hangs (rating 3)
- **Symptoms:** VSync timeouts at boot, the CD stalled forever.
- **Root causes:**
  - **CPU timing:** the cycle model was too crude. It now uses realistic costs (loads 5, mult
    7, div 36, GTE table) with event checks at calls and backward branches.
  - **CD interrupts:** the CD-ROM interrupt-enable register defaulted to 0. It now defaults
    to 0x1F.
- **Lesson:** start with a plausible cycle model and NTSC timing (263 lines × ~2152.8 cycles).
  Many "hangs" are timeouts.

#### 8. HD rendering performance (rating 3)
- **Symptom:** 4x internal resolution ran at 34 fps.
- **Fixes, in order:**
  1. **Threaded rasterizer.** Worker threads with interleaved row bands render the queue in
     submission order.
  2. **Display conversion on the workers.** Converting each frame for display had taken
     4.4 ms on the emulation thread.
  3. **Finer hazard tiles.** 16x16 tiles flagged textures stored just below the 216-line
     framebuffer as hazards; 16x8 tiles fixed that.
  4. **Queued VRAM copies.** A 2x1 self-copy the game issues every frame forced a full queue
     drain (~6 ms).
- **Result:** ~8 ms/frame at 4x, with output pixel-identical to single-threaded rendering.
- **Lesson:**
  - Profile with a per-frame breakdown first (`CTR_PROFILE=1`).
  - Make every renderer change verifiable by hashing screenshots against the previous
    renderer.
  - Hazard tracking must cover both read-after-write and write-after-read.

#### 9–11. Runtime memory bugs (rating 2 each)
- **Shadow return stack:** overflowed and was left with a garbage stack pointer; fixed with
  range checks in `set_retsp`.
- **`__builtin_setjmp` locals:** clobbered the `c` (CPU*) local of the function that called
  it; fixed with static/volatile state, and later replaced entirely (#2).
- **DMA buffer:** a fixed 64 KiB buffer overflowed and silently corrupted host memory; it is
  now a `std::vector`.
- **Lesson:** fixed-size runtime buffers will overflow eventually. Add a crash handler early;
  `runtime/crash.cpp` prints a host backtrace and dumps emulated RAM.

#### 12. Overlays sharing load addresses (rating 2)
- CTR loads code overlays 221–233 into three shared regions. All overlays are recompiled.
- Calls into a region check which overlay is resident by comparing code bytes with RAM. The
  result is cached per DMA generation.
- **Lesson:** find the overlay table and load regions before writing the recompiler; it
  shapes the function-lookup design.

#### 13. PsyQ memory-card patching (rating 2)
- PsyQ's `_patch_card` reads the BIOS's B0/C0 jump tables (B0:56/57) and patches them.
- Fix: the HLE BIOS provides fake tables at 0xC000 and 0xC400.
- **Lesson:** HLE BIOS implementations must also provide the kernel *data structures* that
  SDK libraries poke directly, not just the functions.

#### 14. Controls (rating 2)
- **Console focus:** the console window took keyboard focus. Fix: build as a GUI app
  (`-mwindows`), raise and focus the game window, log to `ctr.log`.
- **vJoy slot:** a virtual vJoy device took the only "unmapped joystick" slot. Fix: open every
  controller and combine their inputs.
- **Logitech F710:** each controller is paired with its own receiver; with two pads, try each
  receiver. In X mode it is recognized as an Xbox controller and works out of the box,
  including the analog sticks.
- **Lesson:** log every input device (name, GUID, layout) at startup. Most "controls don't
  work" reports are detection or focus problems.

#### 15. Tooling friction on Windows (rating 2, cumulative)
- **Bash heredocs:** they turn `\n` inside C string literals into real newlines. Use file
  editing tools for C code with escapes, or write patch scripts to a file first.
- **Python file writes:** without `encoding="utf-8"`, a write crashed on Windows' cp1252
  encoding and emptied `README.md`. Always pass the encoding.
- **PowerShell:** Windows PowerShell blocks `.ps1` scripts; use PowerShell 7 (`pwsh`).
- **Locked executable:** a leftover `ctr.exe` process locks the file and breaks the build.
  Run `Get-Process ctr | Stop-Process -Force` first.

#### 16. Orange blocks instead of menu text (rating 1)
- **Root cause:** the texture-disable bit (GP0 E1 bit 11) was honoured even though GP1(09h)
  had not enabled it.
- **Fix:** honour the bit only when GP1(09h) allows it.

#### 17. Widescreen (rating 1)
- **Fix:** runtime hooks on three instructions, scaling the view-projection X row by 3/4,
  widening the frustum by 4/3 and doubling the far clip. The decomp's 16BY9 mod showed where.
- **Lesson:** a small hook mechanism in the recompiler, emitting a call before chosen
  instructions and checked by the interpreter too, makes enhancements cheap.
- **Still open:** 2D HUD and menus stretch in 16:9.

#### 18. Aspect ratio (rating 1)
- I thought stretching 216 lines to 4:3 was wrong. The game's projection (Y scaled by
  0x360/0x600) assumes exactly that, so the existing display was already correct.
- **Lesson:** check the game's projection math before "fixing" aspect ratio.

#### 19. Hairline seams at high resolution (rating 1, open)
- Thin gaps appear between adjacent 2D pieces, such as the minimap, at 2x and above, with or
  without PGXP. Native resolution hides sub-pixel gaps that upscaling exposes; most upscaling
  renderers show the same thing.

### Open issues
- MDEC (FMV decoding) is a stub; `LoadExec` (demo discs) is not implemented.
- Only the digital pad is emulated (analog sticks are mapped to the D-pad).
- 2D HUD in widescreen; hairline seams in HD (#19).
- No perspective-correct texturing yet. Depth is already recorded per PGXP vertex.

### Provenance: what came from the CTR decompilation
The static recomp is **not** built on the decompiled C code. Every game function is translated
from the original MIPS machine code on the disc, and the runtime emulates the hardware. The
CTR-ModSDK decomp project was used only as a reference:
- **Symbol map:** `symbols/syms926.txt` provides function names and extra discovery seeds.
  The recompiler would still work without it, with less readable output and slightly worse
  discovery.
- **Function behaviour:** names and decomp sources helped explain behaviour such as the
  thread scheduler and the render asm.
- **Widescreen:** the 16BY9 mod showed which camera functions to patch and by how much.

The separate first attempt (CTRRecomp, the decomp's PC port) is unrelated to this code.

---

## Project: Gran Turismo (PS1, USA, v1.1) — `GTStaticRecomp`

Approach: the CTR recompiler and runtime, with the CTR-specific parts removed (thread-scheduler
unwinding, render-asm interpreter ranges, symbol map, camera hooks). Went from an empty folder
to racing at full speed with FMV, music and 4x/16:9 rendering in one session, because almost
everything that cost time on CTR (interpreter, custom setjmp, HLE BIOS, threaded GPU, lockstep
tooling) carried over unchanged. No decomp or symbol map was used.

### Ranking

| # | Blocker | Area | Time | Status |
|---|---|---|---|---|
| 1 | Five executables replacing each other through BIOS `Exec` | Recompiler / HLE BIOS | 3 | Solved (modules + unwinding Exec) |
| 2 | FMV needs a real MDEC | Hardware | 3 | Solved |
| 3 | Interpreter event checks used functions of non-resident programs | Runtime / lockstep | 2 | Solved |
| 4 | Packed executables | Disc / recompiler input | 2 | Solved (unpacker in extractor + runtime) |
| 5 | Weak seeds turned internal `j` into tail calls | Recompiler | 1 | Solved |
| 6 | `setjmp` placed in a helper that returns | Runtime | 1 | Caught before running |
| 7 | Heredoc escaping (again) | Tooling | 1 | Workaround known |

### Details

#### 1. Five executables and `Exec` (rating 3)
- **What:** `SCUS_941.94`, `GTOS.EXE`, `GTMENU.EXE`, `GTMAIN.EXE` and `GTEND.EXE` all load at
  0x80010000. A program reads the next file into a buffer, copies a small routine onto the
  stack, and that routine copies the text to 0x80010000 and calls BIOS A0:43 `Exec`.
- **Fix:**
  - each executable is a recompiler module with its own function table entries; calls check
    which one is resident (the CTR overlay mechanism, with one region covering everything);
  - HLE `Exec` sets gp/sp/a0/a1 and `rt_longjmp`s to a top-level loop in `bios_boot`, dropping
    the old program's native frames and the dynamic return stack. Nesting the new program
    inside the old one's frames would grow the host stack with every menu ↔ race switch;
  - `FlushCache` and `Exec` bump the residency-cache generation, since the CPU (not DMA)
    rewrote the code.
- **Lesson:** look at every executable on the disc and grep the code for `Exec`/`LoadExec`
  (A0:43 / A0:51) early. Multi-executable games need an exec model that unwinds, not nests.

#### 2. MDEC (rating 3)
- **Symptom:** the SCEA logo showed, then black screens: the intro is an FMV.
- **Fix:** a full MDEC from psx-spx (run-length decode, IDCT with the game's scale table,
  YUV → RGB in 15/24-bit, 4/8-bit mono). It worked on the first run.
- **Two traps with instant DMA:**
  - games start the output DMA (ch1) before sending the input (ch0), so ch1 has to wait until
    enough macroblocks are decoded and complete when they are;
  - that completion happens *inside* the ch0 transfer, and `dma_block` used one static buffer,
    so the nested ch1 transfer would have overwritten ch0's remaining input. Each nesting level
    now has its own buffer.
- **Lesson:** when DMA completes instantly, check every place one transfer can trigger another.

#### 3. Lockstep divergence from cross-program lookups (rating 2)
- **Symptom:** lockstep showed identical RAM but a 3-cycle difference at frame 393; tracing
  found the CD interrupt taken at a different instruction.
- **Root cause:** the interpreter checks events on a `j` whose target "is a function entry".
  That lookup covered all five programs, so a forward `j` inside a GTOS decompressor counted as
  a tail call because GTMENU or GTMAIN has a function at that address. The recompiled code
  (correctly) had no check there.
- **Fix:** the interpreter only counts functions of the resident program.
- **Lesson:** once modules share addresses, every address-keyed lookup must be residency-aware,
  including debug-only and timing paths. Lockstep finds these within minutes.

#### 4. Packed executables (rating 2)
- **Symptom:** `strings` on the executables gave broken words ("gtos. exek").
- **Root cause:** each EXE is a stub plus a backwards-LZ stream (`PSLZ` header: unpacked end,
  size, packed end, stub copy target, entry point).
- **Fix:** the extractor runs the same decompression and the recompiler works on the unpacked
  images; the runtime unpacks too, to know what each program looks like once resident. The
  stub itself runs on the interpreter at boot, as on the console.
- **Lesson:** check for packers before writing discovery: broken strings and an entry point
  near the end of the text section are the signs.

#### 5. Weak seeds and `j` (rating 1)
- **Symptom:** a menu routine resumed in the interpreter about 100 times per second.
- **Root cause:** a data word pointed at a label inside a function. That made it a weak seed,
  and `j` to any seed was emitted as a tail call; the label was not a function, so the call fell
  back to the interpreter.
- **Fix:** only strong seeds (entry points, `jal` targets) turn `j` into a tail call.

#### 6. `setjmp` in a returning helper (rating 1)
- The first version of the `Exec` landing point called `rt_setjmp` inside a small function that
  then returned, so a later `rt_longjmp` would have landed in a dead frame. Caught in review
  before the first run; the setjmp is now in `bios_boot` itself.
- **Lesson:** a setjmp must sit in the frame that stays alive.

#### 7. Tooling (rating 1)
- Python edits through a bash heredoc turned `\n` inside C strings into real newlines again
  (see CTR #15). Use the file-editing tool for C code with escapes.

### Enhancements
- **Widescreen without game hooks:** scaling projected X by 3/4 in RTPS/RTPT works for any
  game that projects with the GTE. It needed no reverse engineering. The cost: the game still
  culls to its 4:3 view, so objects near the edges can appear late, and the 2D HUD stretches.
- **Frontend:** `--display N` / `display=` picks the monitor; scripted runs (`--frames`) no
  longer take keyboard focus.

### Open issues
- Simulation Mode (`GTMENU.EXE`), `GTEND.EXE`, replays, two-player and memory cards are
  untested.
- Only the digital pad is emulated (analog sticks are mapped to the D-pad).
- 2D HUD in widescreen; edge culling in widescreen.

---

## Checklist for the next recomp
1. **Inventory the disc:** every executable (and whether it is packed), overlays and their
   load regions, how programs hand over (`Exec`/`LoadExec`), BIOS calls used, and whether a
   symbol map or decomp exists.
2. **Interpreter fallback:** build the interpreter alongside the recompiler, with identical
   cycle and event-check rules.
3. **Non-local jumps:** write host-ABI-correct `setjmp/longjmp` from day one.
4. **Debug tooling first:**
   - lockstep RAM-hash comparison against the interpreter;
   - function-entry tracing with cycle counts;
   - a crash handler that dumps a backtrace and emulated RAM;
   - scripted input with screenshots for headless tests.
5. **Hand-written asm:** scan for it (`jalr` with `rd != 31`, `ra`/`sp` used as data) and send
   it to the interpreter.
6. **HLE BIOS fidelity:** match the kernel stack, interrupt chain, kernel tables, and event
   semantics.
7. **Determinism:** after any renderer or runtime change, compare screenshot hashes against
   the previous build.
8. **Frontend:** log input devices, build as a GUI app, and save settings to a config file.
9. **Shared addresses:** if programs or overlays share addresses, make every address lookup
   (dispatch, interpreter, tracing) residency-aware.
10. **FMV:** expect to need MDEC; with instant DMA, let the output DMA wait for decoded data.
