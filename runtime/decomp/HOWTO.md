# Rewriting a function as C

A rewrite replaces one translated function (gen/) with readable C over the game's own memory,
and must behave identically: the same memory and the same Direct3D calls, frame by frame
(tests/ab.sh). runtime/decomp/renderpass.c is the worked example; read it first. The method and
most of this file come from larush-native's rewrite (521 functions), adapted to this game.

## The pieces

- `builds/decomp_funcs.txt` lists the drawing code's functions. Each has a hook,
  `hook_dc_<address>` (gen/funcs.h), that runs at the function's entry. Functions with another
  hook already (builds/etm12.py HOOKS: blit_draw 0x7a73db, projection 0x62a236, settings_load,
  the stack fixes) have none: leave them, or ask.
- A file here registers a rewrite with `DECOMP(646c9e, fn)` at file scope (decomp.h). `fn` runs
  with the CPU record as at the original's entry: `c->esp` at the return address, stack
  arguments above it (`ARG(0)` is the first), `ecx` the object for a method (thiscall). It does
  the whole call and returns 1 through `decomp_ret(c, entry, eax, npop)`: `npop` is the
  original's `ret N` / 4 (0 for cdecl). A float result goes on the x87 stack: `decomp_ret_st0`.
- Calls into anything (translated code, a Direct3D or Windows thunk, another rewritten function
  through its hook, so ETM_DECOMP_OFF still switches each off): `game_call(c, FN, nargs, args...)`,
  `game_callm(c, FN, object, nargs, ...)` for a method, `game_call_st0` / `game_callm_st0` for a
  float on the x87 stack. FN a named `#define`, not a bare address. A Direct3D method is called
  through the object's vtable as the original does: `game_call(c, R32(R32(dev) + offset), n + 1,
  dev, args...)` (COM passes `this` on the stack; offset as in the original's `call [eax + X]`).
- **Incremental-linking stubs**: the original calls most functions through a one-jmp stub in
  0x401000-0x409000. Call the stub's target (tools/disfn.py prints it after `->`): the target is
  a function of its own in the translation (and has the hook when it is in the list).
- Locals a callee is handed (guest memory): `uint32_t F = dc_frame(c, size);` and
  `dc_unframe(c, F, size);` before returning; locals no callee sees are C locals. Read every
  ARG(n) before the first dc_frame or call (ARG reads above c->esp).

## Reading the original

1. `tools/disfn.py ADDR`: the instructions as the translator decoded them, stub targets resolved.
2. Ghidra's pseudo-C (REA MCP: `procedure_pseudo_code`, `batch_decompile`; open
   ~/.local/share/etm-native/game/Matrix.exe, or with the snapshot work/ghidra_matrix2.snapshot to
   skip the analysis) for the shape. It hides things the listing shows; the listing wins:
   - **hidden register arguments**: a callee's `in_EAX`, `unaff_EDI`... means it reads that
     register. Set `c->eax` etc. before the call as the original does. The callee's first
     instructions tell.
   - **this game is x87 throughout** (`fld`, `fmul`, `fstp`, `__ftol`): x87 maths is `f80`
     (long double, which runs with the game's control word: Direct3D left it at single precision,
     so results round as the original's), rounded to `float` where the original stores a dword.
     Same operations in the same order. Where the original multiplies by -1, `mul_neg1(x)`.
     Decomp files build with `-ffp-contract=off`, so nothing is fused.
   - **NaN operands**: the order of a commutative operation's operands can matter with NaNs; keep
     the original's. The translation moves floats through `fld`/`fstp` pairs as plain copies (a
     signalling NaN stays signalling): copy floats as integers where the original only moves them.
   - **compares**: x87 `fcomp`/`fnstsw ax`/`test ah, 0x41` sequences: work out the flags per case
     (C3 = equal, C0 = below, C2 = unordered; `test ah, 5` + `jp` etc.). Unordered (NaN) cases
     go where the original's flags send them. Integer `jl`/`jg` are signed, `jb`/`ja` unsigned.
   - **memory read twice**: the original (MSVC without optimisation) re-reads globals and fields
     after every call; read them again in C at the same point (a callee may change them).
   - **eax after a void function**: leave eax as the original does (its last call or load).
   - **a callee that sets part of eax** (`mov al, ...`) hands back the rest of the caller's eax:
     set eax before such a call as the original leaves it.
   - **constants in .data**: values at 0x91b000 and above are writable data: read them from
     memory, not as literals (.rdata 0x8fa000-0x91b000 is constant).
   - **ARG(n) is the argument's value**, not its address: a float argument is
     `RF32(entry + 4 + 4n)` (or ARG(n)'s bits), never `RF32(ARG(n))`.
3. Callee-saved registers (ebx, esi, edi, ebp) must be what the caller had when you return:
   restore any you set for a hidden argument.

## Writing it

- Name things. Globals and fields as `#define`s with a comment of what they are and where that's
  known from. Unknown ones keep their address in the name (`OBJ_0x88`) rather than a guess.
- A header comment per file and per function: what it does in game terms. Plain sentences,
  addresses in brackets; mark claims [verified] (with the evidence) or [guess] (shared rules).
- Don't copy the original's register shuffling or loop forms when C says the same thing; keep its
  order of memory accesses and calls.
- Integers as `uint32_t`/`int32_t` with the original's signedness; `R8/R16/R32/RF32`,
  `W8/W16/W32/WF32` for memory.

## Checking

- `make JOBS=4`, then `tests/ab.sh` (about 1-2 minutes: the test route to the Post Office lobby
  fight, hidden; the translation's own run is saved and reused while the translation is the same).
  It must print `same`. It compares memory (stacks excluded) and the draw-stream hash every 10
  frames; the frames in between are covered by the hashes being running ones.
- A difference: `ETM_DECOMP_OFF=addr,addr tests/ab.sh` (hex, no 0x) to bisect (those rewrites
  off); the draw hash (4th column) differing means a Direct3D call differs; only the memory hash
  (3rd) means memory. `--dump-memory FRAME FILE` on both modes (tests/ab.sh's keys, see its
  source) and `tools/memdiff.py A B` show the differing bytes; `ETM_WATCH=addr@frame` (watch.c)
  logs who changes them, with a backtrace of translated functions (F_<address>).
- **Stack leftovers**: the original sometimes copies stack contents it never wrote into memory or
  hands them to Direct3D (vertex padding, shader-constant lanes). Those bytes come from earlier
  calls' frames; a rewrite leaves different ones, so memory or the draw stream differs with
  nothing wrong in your C. Don't imitate the original's stack: fix the leak where it happens, in
  both modes: runtime/decomp/stackfix.c (the leaking function's frame cleared at entry; a new hook
  needs an entry in builds/etm12.py and a regeneration, which `make` does in ~3 minutes). The
  first one (0x64a2e9) is the example. A function with such a fix that you rewrite must write
  those bytes as zeros itself.
- `tools/calltree.sh FN FRAME OUT`: the real call tree of one call (with the test route's keys
  in ETM_KEYS, see tests/ab.sh), when the static picture isn't enough.
- Branches the test route never reaches: note them in your report with what would reach them;
  transcribe them as carefully as the rest.
