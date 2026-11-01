# Step 4: x86 to C Lifting

## Overview

This is the core of the static recompiler. The lifter takes each x86 instruction from the disassembly output and translates it to equivalent C code. The result is a massive C codebase that, when compiled with a native compiler (MSVC or GCC), produces a Windows executable that behaves identically to the original Xbox code.

For Burnout 3, the lifter translates 2.73 MB of x86 machine code into approximately 4.43 million lines of C across multiple output files.

The key insight is that we don't need to *understand* the code. We translate each instruction mechanically, preserving exact semantics. The C compiler then optimizes the result. Understanding comes later, during debugging, when we selectively replace translated functions with hand-written equivalents.

## Register Model

x86 has 8 general-purpose 32-bit registers. The lifter maps them to C variables:

### Global Registers (shared across all functions)

```c
extern uint32_t g_eax;  // return value, accumulator
extern uint32_t g_ecx;  // 'this' pointer in thiscall, loop counter
extern uint32_t g_edx;  // high dword of mul/div, general purpose
extern uint32_t g_esp;  // stack pointer (points into Xbox memory)
extern uint32_t g_ebx;  // callee-saved, but global for cross-function param passing
extern uint32_t g_esi;  // callee-saved, often 'this' in game code
extern uint32_t g_edi;  // callee-saved, general purpose
```

In generated code files, the `RECOMP_GENERATED_CODE` preprocessor define enables shorthand aliases:

```c
#define eax g_eax
#define ecx g_ecx
#define edx g_edx
#define esp g_esp
#define ebx g_ebx
#define esi g_esi
#define edi g_edi
```

### Why Are Callee-Saved Registers Global?

In standard x86 calling conventions, `ebx`, `esi`, and `edi` are callee-saved -- the called function must preserve their values. You might expect these to be local variables in each translated function.

However, MSVC-compiled Xbox code frequently passes implicit parameters through these registers, especially in optimized builds. For example, many Burnout 3 functions expect `esi` to contain a pointer to the car object (`0x557880`). Making these registers global ensures that values set by the caller are visible to the callee, exactly as on real hardware.

The callee-save contract is enforced by the generated code itself -- the lifter emits `PUSH32(esp, ebx)` at the prologue and `POP32(esp, ebx)` at the epilogue, mirroring what the original x86 code does.

### The EBP Exception

`ebp` is the only register that is **local** to each translated function. Why? Because over 20,000 functions in Burnout 3 use Frame Pointer Omission (FPO), meaning they use `ebp` as a scratch register without saving/restoring it. If `ebp` were global, these functions would corrupt each other's frame pointers.

For the ~200 functions that use SEH (Structured Exception Handling), a special bridge variable `g_seh_ebp` transfers the `ebp` value between the SEH prolog helper and the calling function:

```c
// In __SEH_prolog (sub_00244784):
g_seh_ebp = ebp;   // write bridge

// In the calling function, after call to __SEH_prolog:
ebp = g_seh_ebp;   // read bridge
```

## Memory Access Model

The Xbox has a flat 32-bit address space. All memory accesses in the original code use absolute addresses. The lifter translates these through macros that add a runtime offset:

```c
#define XBOX_PTR(addr) ((uintptr_t)(uint32_t)(addr) + g_xbox_mem_offset)
#define MEM8(addr)     (*(volatile uint8_t  *)XBOX_PTR(addr))
#define MEM16(addr)    (*(volatile uint16_t *)XBOX_PTR(addr))
#define MEM32(addr)    (*(volatile uint32_t *)XBOX_PTR(addr))
#define MEMF(addr)     (*(volatile float    *)XBOX_PTR(addr))
```

The `(uint32_t)` cast is critical. Xbox addresses are 32-bit, and arithmetic in the recompiled code can overflow. Without the mask, a 64-bit `uintptr_t` cast would preserve overflow bits, landing the access 4 GB past the mapped region and causing an access violation.

When Xbox memory is mapped at its original addresses (the ideal case), `g_xbox_mem_offset` is 0 and these macros are essentially no-ops.

## Stack Simulation

The Xbox stack lives in Xbox memory. `g_esp` is an Xbox virtual address pointing into mapped memory. Push and pop operations work through the memory macros:

```c
// Push: decrement ESP, then write value
#define PUSH32(sp, val) do {           \
    uint32_t _pv = (uint32_t)(val);    \
    (sp) -= 4;                         \
    MEM32(sp) = _pv;                   \
} while(0)

// Pop: read value, then increment ESP
#define POP32(sp, dst) do {            \
    (dst) = MEM32(sp);                 \
    (sp) += 4;                         \
} while(0)
```

Note that `PUSH32` evaluates the value *before* decrementing `sp`. This matches x86 semantics where `push [esp+N]` reads the operand at the old ESP before adjusting.

The stack is initialized to `XBOX_STACK_TOP` (0x00F7FFF0) -- the top of an 8 MB region allocated in Xbox memory space.

## Instruction Translation

### Data Movement

```
x86:  mov eax, [ecx+0x10]
C:    eax = MEM32(ecx + 0x10);

x86:  mov [esi+0x1B4], eax
C:    MEM32(esi + 0x1B4) = eax;

x86:  mov al, [edx]
C:    SET_LO8(eax, MEM8(edx));

x86:  movzx eax, byte ptr [ecx+3]
C:    eax = ZX8(MEM8(ecx + 3));

x86:  movsx eax, word ptr [esi]
C:    eax = SX16(MEM16(esi));

x86:  lea eax, [ecx+edx*4+0x10]
C:    eax = ecx + edx * 4 + 0x10;

x86:  xchg eax, ecx
C:    { uint32_t _t = eax; eax = ecx; ecx = _t; }
```

### Arithmetic

```
x86:  add eax, ebx
C:    eax = eax + ebx;

x86:  sub ecx, 0x10
C:    ecx = ecx - 0x10;

x86:  imul eax, [esi+8], 0x18
C:    eax = (int32_t)MEM32(esi + 8) * 0x18;

x86:  mul ecx                    ; edx:eax = eax * ecx
C:    { uint64_t _r = (uint64_t)eax * ecx; eax = (uint32_t)_r; edx = (uint32_t)(_r >> 32); }

x86:  div ecx                    ; eax = edx:eax / ecx, edx = remainder
C:    { uint64_t _dv = ((uint64_t)edx << 32) | eax; eax = (uint32_t)(_dv / ecx); edx = (uint32_t)(_dv % ecx); }

x86:  inc edi
C:    edi = edi + 1;

x86:  neg eax
C:    eax = (uint32_t)(-(int32_t)eax);

x86:  not eax
C:    eax = ~eax;
```

### Bitwise Operations

```
x86:  and eax, 0xFF
C:    eax = eax & 0xFF;

x86:  or ecx, 0x80000000
C:    ecx = ecx | 0x80000000;

x86:  xor eax, eax              ; common idiom for "eax = 0"
C:    eax = 0;

x86:  shl eax, 4
C:    eax = eax << 4;

x86:  shr ecx, cl               ; shift by register
C:    ecx = ecx >> (ecx & 31);  ; (note: x86 masks shift count to 0-31)

x86:  sar eax, 1                ; arithmetic shift right
C:    eax = (uint32_t)((int32_t)eax >> 1);

x86:  rol eax, 8
C:    eax = ROL32(eax, 8);

x86:  bswap eax
C:    eax = BSWAP32(eax);
```

### Comparison and Branching

The lifter pattern-matches `cmp`/`test` + `jcc` pairs into C conditionals:

```
x86:  cmp eax, 0
      jz loc_001234
C:    if (CMP_EQ(eax, 0)) goto loc_001234;

x86:  cmp ecx, ebx
      jb loc_001234
C:    if (CMP_B(ecx, ebx)) goto loc_001234;

x86:  cmp eax, edx
      jge loc_001234
C:    if (CMP_GE(eax, edx)) goto loc_001234;

x86:  test eax, eax
      jnz loc_001234
C:    if (TEST_NZ(eax, eax)) goto loc_001234;

x86:  test al, 0x04
      jz loc_001234
C:    if (TEST_Z(LO8(eax), 0x04)) goto loc_001234;
```

The `CMP_*` macros perform the comparison with proper signedness:

```c
#define CMP_EQ(a, b)  ((uint32_t)(a) == (uint32_t)(b))
#define CMP_B(a, b)   ((uint32_t)(a) <  (uint32_t)(b))   // unsigned below
#define CMP_L(a, b)   ((int32_t)(a)  <  (int32_t)(b))    // signed less
#define CMP_GE(a, b)  ((int32_t)(a)  >= (int32_t)(b))    // signed greater-equal
#define TEST_Z(a, b)  (((uint32_t)(a) & (uint32_t)(b)) == 0)
```

### Function Calls

```
x86:  push eax
      push ecx
      call sub_00012345
      add esp, 8           ; cdecl cleanup
C:    PUSH32(esp, eax);
      PUSH32(esp, ecx);
      PUSH32(esp, 0xDEAD); // dummy return address
      sub_00012345();
      esp += 4;            // pop dummy return address
      esp += 8;            // cdecl stack cleanup

x86:  ret
C:    esp += 4;            // pop return address
      return;
```

All translated functions have signature `void sub_XXXXXXXX(void)`. Arguments are passed through the simulated stack, and return values come back in `g_eax`.

### Indirect Calls (ICALL)

When the target of a `call` is a register or memory operand (not a constant), the lifter emits a dispatch macro:

```
x86:  call eax
C:    PUSH32(esp, 0xDEAD);   // dummy return address
      RECOMP_ICALL(eax);

x86:  call [ecx+0x10]         // vtable dispatch
C:    PUSH32(esp, 0xDEAD);
      RECOMP_ICALL(MEM32(ecx + 0x10));
```

The `RECOMP_ICALL` macro implements a 3-tier lookup:

```c
#define RECOMP_ICALL(xbox_va) do {                                  \
    uint32_t _va = (uint32_t)(xbox_va);                             \
    /* Record in trace ring buffer */                               \
    g_icall_trace[g_icall_trace_idx & (ICALL_TRACE_SIZE-1)] = _va;  \
    g_icall_trace_idx++;                                            \
    g_icall_count++;                                                \
    /* Early-out: garbage VA range */                               \
    if (_va >= 0x00400000 && _va < 0xFE000000) {                    \
        g_esp += 4; eax = 0; break;                                \
    }                                                               \
    /* Tier 1: manual overrides (hand-written replacements) */      \
    recomp_func_t _fn = recomp_lookup_manual(_va);                  \
    /* Tier 2: auto-generated dispatch table (binary search) */     \
    if (!_fn) _fn = recomp_lookup(_va);                             \
    /* Tier 3: kernel bridge (synthetic VAs at 0xFE000000+) */      \
    if (!_fn) _fn = recomp_lookup_kernel(_va);                      \
    if (_fn) _fn();                                                 \
    else { g_esp += 4; eax = 0; }  /* pop dummy ret addr */        \
} while(0)
```

If lookup fails, the dummy return address is popped from the stack to prevent stack leaks. Without this, each failed indirect call would leak 4 bytes of stack space per frame.

## FPU (x87 Floating Point)

The x87 FPU uses an 8-deep register stack (ST(0) through ST(7)). The lifter translates x87 operations to local float/double variables:

```
x86:  fld dword ptr [esi+0x18]     ; push float onto FPU stack
C:    double _st0 = (double)MEMF(esi + 0x18);

x86:  fmul dword ptr [ecx+0x1C]    ; ST(0) *= mem
C:    _st0 = _st0 * (double)MEMF(ecx + 0x1C);

x86:  fadd st(0), st(1)            ; ST(0) += ST(1)
C:    _st0 = _st0 + _st1;

x86:  fstp dword ptr [edi+0x20]    ; store and pop
C:    MEMF(edi + 0x20) = (float)_st0;
      _st0 = _st1; _st1 = _st2;   // shift stack down

x86:  fxch st(1)                    ; swap ST(0) and ST(1)
C:    { double _t = _st0; _st0 = _st1; _st1 = _t; }

x86:  fild dword ptr [esp+4]       ; load integer as float
C:    _st0 = (double)(int32_t)MEM32(esp + 4);

x86:  fistp dword ptr [eax]        ; store as integer
C:    MEM32(eax) = (int32_t)_st0;
```

The x87 translation is one of the trickiest parts of the lifter. The stack-based model requires tracking which `_stN` variable holds which logical value across every instruction.

## SSE/MMX

Some XDK library code uses SSE instructions (the Xbox Pentium III supports SSE1). These are translated to explicit `__m128` operations or scalar equivalents:

```
x86:  movaps xmm0, [eax]
C:    __m128 xmm0 = _mm_load_ps((float*)XBOX_PTR(eax));

x86:  mulps xmm0, xmm1
C:    xmm0 = _mm_mul_ps(xmm0, xmm1);
```

In practice, SSE usage in Xbox games is limited. Most math uses x87 FPU.

## Jump Tables (Switch Statements)

The disassembler identifies switch statement jump tables. The lifter converts them to C switch statements:

```
x86:  cmp eax, 12
      ja loc_default
      jmp [eax*4 + 0x0036C100]   ; jump table
      ; table at 0x0036C100: 0x001234, 0x001250, 0x001270, ...

C:    if (eax > 12) goto loc_default;
      switch (eax) {
          case 0: goto loc_001234;
          case 1: goto loc_001250;
          case 2: goto loc_001270;
          ...
          default: goto loc_default;
      }
```

This is more readable and compiles more efficiently than the original indirect jump.

## Output Files

### Generated Code Structure

The lifter produces several output files:

**recomp_XXXX.c** (split files):
```c
#define RECOMP_GENERATED_CODE
#include "recomp_types.h"
#include "gen/recomp_funcs.h"

void sub_00011000(void) {
    uint32_t ebp;
    PUSH32(esp, ebp);
    ebp = esp;
    esp -= 0x10;
    eax = MEM32(ecx + 0x10);
    if (CMP_EQ(eax, 0)) goto loc_00011020;
    eax = MEM32(eax + 4);
    PUSH32(esp, eax);
    PUSH32(esp, 0xDEAD);
    sub_00012340();
    esp += 4;
    esp += 4;
loc_00011020:
    esp = ebp;
    POP32(esp, ebp);
    esp += 4; // pop return address
    return;
}

void sub_00011050(void) {
    // ... next function ...
}
```

**recomp_funcs.h**: Forward declarations for all translated functions:
```c
void sub_00011000(void);
void sub_00011050(void);
void sub_000110E0(void);
// ... 22,095 declarations ...
```

**recomp_dispatch.c**: Binary-searchable lookup table mapping Xbox VAs to function pointers:
```c
static const struct { uint32_t va; recomp_func_t fn; } dispatch_table[] = {
    { 0x00011000, sub_00011000 },
    { 0x00011050, sub_00011050 },
    { 0x000110E0, sub_000110E0 },
    // ... sorted by VA for binary search ...
};
static const size_t dispatch_table_size = 22097;
```

### Splitting

With 22,095 functions, a single C file would be enormous and slow to compile. The `--split N` option distributes functions across multiple files, each containing at most N functions:

```bash
py -3 -m tools.recomp "default.xbe" --all --split 1000
```

This produces `recomp_0000.c` through `recomp_0022.c` (23 files for Burnout 3).

## Code Size

The translation is intentionally verbose -- it prioritizes correctness over readability. Each x86 instruction becomes 1-3 lines of C. For Burnout 3:

- Input: 2.73 MB of x86 (approximately 650,000 instructions)
- Output: 4.43 million lines of C
- Compile time: ~65 seconds with MSVC (Release, full optimization)

The C compiler's optimizer eliminates most of the overhead. The final binary is roughly 2-3x the size of the original x86 code, which is acceptable.

## Invocation

```bash
py -3 -m tools.recomp "path/to/default.xbe" --all --split 1000
```

Options:
- `--all`: translate all detected functions (default: only translate functions reachable from entry point)
- `--split N`: maximum functions per output file
- `--output-dir <dir>`: where to write generated files (default: `src/game/recomp/gen/`)
- `--verbose`: print per-function translation progress
- `--spin-waits FILE`: lower the busy-wait loops the file selects (see
  [Spin waits](#spin-waits))
- `--patches FILE`: follow listed 32-bit loads with a runtime-steered
  replacement (see [Guest patches](#guest-patches))

The output directory is typically gitignored because the generated files are large (200+ MB total) and can be regenerated from the XBE at any time.

## Spin waits

A title that waits for its next frame often does it in a loop that only
reads memory and branches back to itself (`L: cmp [counter], eax; jl L`).
Lifted as is, the loop holds a host core for as long as it waits. With
`--spin-waits FILE` the lifter finds such loops and calls `RECOMP_SPIN_WAIT`
on the taken back edge only:

```c
loc_00060475: ;
    ...
    if (CMP_L(...)) { RECOMP_SPIN_WAIT(0x00060475u); goto loc_00060475; }
```

The first test of the condition never waits. `RECOMP_SPIN_WAIT`
(`templates/runtime/recomp_types.h`) does nothing unless the runtime's
pacing mode is `sleep` (the `present.pacing` key,
[enhance-config](../runtime/enhance-config.md)); then the thread blocks until
the kernel signals a state change or 1 ms passes (`src/kernel/kernel_pacing.h`).

The matcher (`tools/recomp/spin_waits.py`) is deliberately narrow. A
candidate is one basic block ending in a conditional jump to its own start,
whose other instructions are only:

- `cmp` or `test` reading memory, or a register the block loads from memory;
- `mov`, `movzx` or `movsx` of memory or an immediate into a register that no
  address in the block uses;
- `pause`.

Anything that writes memory, calls, pushes or pops, uses a string, locked,
port, `cpuid`, `rdtsc` or FPU instruction, changes a register an address
uses, or reads no memory at all is refused.

The file is JSON:

```json
{"auto": false,
 "sites": [{"va": "0x00060475", "note": "main loop's vblank wait"}],
 "exclude": []}
```

- `"auto": false` lowers only the listed `sites`; `true` lowers every
  candidate except those in `exclude`.
- A listed site that fails the matcher in any body that contains it, or that
  no body has as a self-loop, is an error naming the VA, the body and the
  rule. The run finishes and then exits 1; a refused shape is never forced
  through.
- The run writes `spin_waits.json` to the `-o` directory: every self-loop
  block seen, per VA, with its state (`lowered`, `rejected` with the
  reasons, `excluded`, or `candidate (not listed)`), the number of bodies
  that contain it, and the errors. The unlisted candidates are where to look
  for further sites.

Without the flag the matcher does not run, and the output is byte for byte
what it was. Tests: `tools/recomp/test_spin_waits.py`.

## Guest patches

A title can carry a behaviour it never turns on for the player's region
(Burnout 3's US build keeps the km/h speedometer of its other languages,
chosen by the language index it loads at nine places). `--patches FILE`
follows a listed `mov reg, [mem]` with `reg = RECOMP_PATCH_U32(key, when,
value, reg)`: the loaded value stands unless the setting `key` reads `when`,
then it is `value`. Memory is not touched, so every other reader of the
variable (the title's text, here) sees the real value.

```json
{"patches": [{"va": "0x00059BF7", "kind": "load_u32",
              "setting": "game.speed_units", "when": "kmh", "value": 2,
              "note": "..."}]}
```

The runtime side is `src/platform/recomp_patch.h`: the host opts the setting
in (`recomp_patch_enable(key, env_id)`), and the call reads that environment
variable, which the overlay and `settings.toml` already drive, so a change
shows at the next load with no restart. A setting the host did not enable
leaves the load alone. `game.speed_units` (`RECOMP_SPEED_UNITS=mph|kmh`) is
an overlay row on the Display page, shown only when enabled.

An optional `"latch": "set"` or `"use"` pairs sites that read one choice at
different times (Burnout 3 builds the speedometer's unit label once, when the
HUD is made, and its number every frame): a `use` site follows what the last
`set` site sampled, so a change mid-race shows whole at the next set-up
instead of as 171 "mph".

`"kind": "load_u8"` is the same for a byte flag, `mov al|bl|cl|dl, [mem]`:
`SET_LO8(eax, RECOMP_PATCH_U32(key, when, value, LO8(eax)))`, the value
0..255, only the register's low byte changed. A title's own on/off byte read
back as 0 makes the title take its own "off" path (Burnout 3's graphics
manager has one for its blur, on since the constructor and with no menu
entry: `graphics.motion_blur`, `RECOMP_GFX_MOTION_BLUR=0`; `graphics.bloom`
is a `load_u32` of the value its bloom call is gated by). These two settings
default to on, so the patch is their `"when": "0"`; their rows are on the
overlay's Graphics page. They are strengths from 0 to 1 (BO-212): the sites
that read a weight or an opacity (`load_f32`, also on an x87 `fld dword ptr
[mem]`) are scaled by it while the setting is anything but `1`.

`"kind": "cmp_u8"` is for a byte flag the title does not load but compares
where it lies, `cmp byte ptr [mem], r8|imm8`. The lifter snapshots a
compare's operands (`_fa`, `_fb`, and their sign-extended twins) and every
later `jcc`, `setcc` and `cmovcc` reads the snapshot, so the call rewrites
the left one after the compare:
`_fa = RECOMP_PATCH_U32(key, when, value, _fa) & 0xFFu; _fas = (int32_t)(int8_t)(_fa);`
(and `_cf = (int)(_fa < _fb);` in a function that tracks the carry). The
compare then reads the value where the memory byte was, whatever sits
between it and the branch, a compare fused with its `jcc` included. No
latch. Burnout 3 chooses its frame rate this way: a stage's update compares
its "a crash camera is on" byte and asks for the 30 Hz mode (presentation
interval 2, two simulation steps a frame); read as 0 the title takes its own
full-rate path (`graphics.crash_60fps`, `RECOMP_GFX_CRASH_60FPS=1`, off by
default; the game repository's `docs/crash60.md`).

`"kind": "cmp_u32"` is the same for a 32-bit value, `cmp dword ptr [mem],
r32|imm` (Burnout 3's music manager compares the player count with 1 and
stays silent in split screen; read as 1 it plays: `audio.split_music`).
`"kind": "store_u8"` is for a byte the title stores once and reads later,
`mov byte ptr [mem], imm8|r8`: the store is followed by
`MEM8(addr) = (uint8_t)RECOMP_PATCH_U32(key, when, value, MEM8(addr));`, so
the byte is stored again as the value while the setting says so. Unlike the
load and compare kinds it writes the guest's memory, as `store_f32` does
(Burnout 3's two-player stage asks for its half frame rate with one store at
set-up: `graphics.split_60fps`; the game repository's
`docs/split-screen.md`).
`"kind": "store_u32"` is the same for a 32-bit value, `mov dword ptr
[mem], imm32|r32`, followed by
`MEM32(addr) = RECOMP_PATCH_U32(key, when, value, MEM32(addr));` (Burnout
3's one-player stage start stores its simulation stepper's mode, 0, one step
a frame whatever the time; stored again as 1, the mode the title itself
switches to after the first crash camera: `game.race_clock`; the game repository's `docs/race-clock.md`).

A float goes by `"kind": "load_f32"` (after `movss xmm, [mem]`, or a 32-bit
`mov reg, [mem]` that carries a float as bits) or `"store_f32"` (after
`movss [mem], xmm` / `mov [mem], reg`: the stored float is rewritten, for a
value whose register has other uses) or `"cmp_f32"` (after `comiss` /
`ucomiss xmm, dword ptr [mem]`: the compare's snapshot of the memory operand,
`_fcb`, is rewritten, so the branch compares with the value while the
constant stays in memory for its other readers; Burnout 3's rival marker,
`display.rival_marker`). Its value is an `"expr"`, arithmetic
over numbers, `loaded` (the float the instruction moved) and names the host
sets with `recomp_patch_set_var` before the guest runs, because a layout
value depends on the picture's shape:

```json
{"va": "0x00053858", "kind": "load_f32", "setting": "display.hud_fix",
 "when": "1", "expr": "loaded + (640 - 480 * aspect) / 2"}
```

```c
xmm0 = XMM_SCALAR(MEMF(0x3A7964)); /* movss */
xmm0.f[0] = RECOMP_PATCH_F32("display.hud_fix", "1", 0,
    ((double)(xmm0.f[0]) + (640.0 - 480.0 * recomp_patch_var("aspect")) / 2.0),
    xmm0.f[0]);
```

`f32[reg + 0xNN]` in an expression reads another guest float at the site, for
a value that depends on a neighbour (a width squeezed about an anchor x).
The expression is parsed by the recompiler (nothing unparsed reaches the
generated code), evaluated in double, and only while the setting is on.
`"if": {"reg": "edi", "in": ["0x60EA14", "0x60EA84"]}` limits a site in
shared code to the objects whose address the register holds there (Burnout
3's 2D scene code serves the HUD, the menus and the loading screen; the
patch is for the two HUD scenes). `display.hud_fix` (`RECOMP_GFX_HUD_FIX=1`)
is the second overlay row of this kind.

`"when": "!off"` is on for a setting that is set and reads anything but
`off` (a choice with several "on" values). `"kind": "mark"` patches nothing:
after the instruction at `va` the code calls `RECOMP_PATCH_MARK("name")`, and
a host that registered a function for the name (`recomp_patch_on_mark`)
learns the title got there ("a stage is being set up", "the 3D world was
drawn in this frame", "a frame ends"). Burnout 3's ultrawide mode keeps its
state from four such marks and shows each frame at the shape it was drawn
for (`nv2a_present_frame_aspect_fn`, `docs/runtime/overlay.md`:
`display.ultrawide`).

A listed site that is not the shape its kind names, or that no lifted body
contains, fails the run; the report is `patches.json` in `-o`. Without the
flag the output is unchanged. Tests: `tools/recomp/test_patches.py`.

## Post-Generation Patches

After regenerating the code, you will likely need to re-apply manual patches to the generated files. These patches disable broken functions, add diagnostic traces, or fix code generation bugs:

- `#if 0` / `#endif` around functions that crash or hang (replaced by manual overrides)
- Vtable guard insertions that check pointer validity before indirect calls
- Jump table replacements where the lifter's pattern matching failed
- `extern` declarations for global variables shared with manual code

Maintain a checklist of required patches and re-apply them after every regeneration. For Burnout 3, there are 10+ patch locations across 8 generated files.

## Limitations

1. **Self-modifying code**: the lifter assumes code is static. Xbox games rarely use self-modifying code, but some copy protection schemes do.

2. **FS segment**: x86 `fs:` segment overrides (used for Thread Information Block access) are dropped. The lifter translates `mov eax, fs:[0x28]` as `MEM32(0x28)`, which reads from the wrong memory. This affects ~35 instructions in Burnout 3.

3. **Precise flag behavior**: the `CMP_*` macros handle common flag patterns, but exotic sequences (e.g., using carry flag from arithmetic to feed into a conditional branch 10 instructions later) may require manual intervention.

4. **Exception handling**: the original code uses SEH for error recovery (e.g., catching access violations from bad pointers). The translated code cannot use SEH in the same way because it's not real x86 code. A Vectored Exception Handler (VEH) partially bridges this gap.
