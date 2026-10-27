/* DECOMPILED FUNCTIONS: the game's drawing code rewritten as readable C, replacing the translated
   functions (DESIGN.md, "The drawing code's rewrite"; tooling from larush-native). Each replaces
   one hooked function (builds/decomp_funcs.txt): its hook runs the C and returns 1.
   decomp_install() switches them all in; option decomp = off, or ETM_DECOMP_OFF=addr,... (or
   all), leaves them (or some) to the translation, so two deterministic runs (--test --hash,
   tests/ab.sh) compare the two frame by frame. */
#pragma once
#include <stdarg.h>
#include "runtime.h"
#include "funcs.h"

void decomp_install(void);
int decomp_enabled(uint32_t fn);    /* not switched off by option or ETM_DECOMP_OFF */

/* The drawing code's functions (builds/decomp_funcs.txt) each have a hook, hook_dc_<address>.
   A file in runtime/decomp/ registers a rewrite for one with DECOMP(address, fn), at file
   scope; decomp_install switches in every registered one (the table is a linker section, so
   files register without a central list). fn is the hook: it runs with the CPU record as at the
   original's entry (esp at the return address, arguments above it, ecx the object for a
   method) and returns 1 after doing the whole call, decomp_ret included. */
typedef struct { uint32_t addr; int (**hook)(CPU *); int (*fn)(CPU *); } DecompEntry;
#define DECOMP(a, f) \
    static const DecompEntry decomp_entry_##a __attribute__((used, section("decomp_table"), aligned(8))) = \
        {0x##a, &hook_dc_##a, f}

static inline int decomp_hook(CPU *c, int (*dc)(CPU *)) { return dc && dc(c); }
static inline void decomp_or_orig(CPU *c, int (*dc)(CPU *), void (*orig)(CPU *))
{
    if (!decomp_hook(c, dc))
        orig(c);
}

/* CALLS AT THE CURRENT STACK POINTER (the tidied form, 2026-10-26). A rewritten function keeps no
   stack layout of the original's: it calls other code (translated or rewritten, through its hook,
   so ETM_DECOMP_OFF still switches each off) with game_call, the arguments pushed below
   c->esp and DC_RET as the return address, and keeps the locals it hands to callees in a frame
   it lowers c->esp for (dc_frame / dc_unframe). Registers as before: hidden register arguments set
   before the call, callee-saved ones put back before returning, eax/ecx/edx the callee's. Where
   the callee looks at its return address, game_call_at passes the original's. DC_RET is the
   program's first byte (the "MZ" header): mapped, and no code. */
#define DC_RET 0x00400000u
static inline uint32_t game_callv(CPU *c, uint32_t ret, uint32_t fn, int nargs, const uint32_t *args)
{
    uint32_t sp = c->esp, esp = sp - 4u * (uint32_t)nargs;
    for (int i = 0; i < nargs; i++)
        W32(esp + 4u * (uint32_t)i, args[i]);
    c->esp = esp - 4;
    W32(c->esp, ret);
    icall(c, fn);
    if (c->ret != ret)
        fatal("decomp: %08x returned to %08x, not %08x", fn, c->ret, ret);
    c->esp = sp;
    return c->eax;
}
#define DC_ARGS_(nargs, ...) ((const uint32_t[]){0, ##__VA_ARGS__} + 1)
#define game_call(c, fn, nargs, ...) game_callv(c, DC_RET, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))
#define game_call_at(c, ret, fn, nargs, ...) game_callv(c, ret, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))
#define game_callm(c, fn, obj, nargs, ...) ((c)->ecx = (obj), game_call(c, fn, nargs, ##__VA_ARGS__))
#define game_call_st0(...) ({ game_call(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
#define game_callm_st0(...) ({ game_callm(__VA_ARGS__); f80 _v = ST(0); FPOP(); _v; })
/* the same for a rewritten function called as C (int fn(CPU *): it reads its arguments above
   c->esp, ARG(n), and returns with decomp_ret), for one with no hook of its own; one with a hook
   is called with game_call, so it can be switched off alone */
static inline uint32_t local_callv(CPU *c, int (*fn)(CPU *), int nargs, const uint32_t *args)
{
    uint32_t sp = c->esp, esp = sp - 4u * (uint32_t)nargs;
    for (int i = 0; i < nargs; i++)
        W32(esp + 4u * (uint32_t)i, args[i]);
    c->esp = esp - 4;
    W32(c->esp, DC_RET);
    fn(c);
    c->esp = sp;
    return c->eax;
}
#define local_call(c, fn, nargs, ...) local_callv(c, fn, nargs, DC_ARGS_(nargs, ##__VA_ARGS__))
/* a frame for locals a callee is handed (guest memory): c->esp lowered by size (rounded to 16),
   its base returned; dc_unframe puts c->esp back before the function returns */
static inline uint32_t dc_frame(CPU *c, uint32_t size)
{
    c->esp -= (size + 15u) & ~15u;
    return c->esp;
}
static inline void dc_unframe(CPU *c, uint32_t base, uint32_t size)
{
    c->esp = base + ((size + 15u) & ~15u);
}

/* x times -1 as the original's mulss computes it: GCC turns x * -1.0f into a sign flip, which
   flips a NaN's sign where the multiplication returns the NaN unchanged */
static inline float mul_neg1(float x)
{
    float m1 = -1.0f;
    __asm__("mulss %1, %0" : "+x"(x) : "x"(m1));
    return x;
}

/* the replaced function's return (esp back at entry): eax, pop the return address and nargs
   dwords (stdcall; 0 for cdecl) */
static inline int decomp_ret(CPU *c, uint32_t entry, uint32_t eax, int nargs)
{
    c->esp = entry;
    c->eax = eax;
    c->ret = R32(c->esp);
    c->esp += 4 + 4u * (uint32_t)nargs;
    return 1;
}

/* the same for a function whose result is a float: pushed on the x87 stack as the original's */
static inline int decomp_ret_st0(CPU *c, uint32_t entry, f80 v, int nargs)
{
    FPUSH(v);
    return decomp_ret(c, entry, c->eax, nargs);
}
