/* OBJECT SUBMISSION, shared definitions (objects.c, lights.c, modeldraw.c, material.c).
   Direct3D 9: the game's device pointer and the vtable offsets of the methods these files call,
   through the vtable as the original does (HOWTO.md). The offsets are IDirect3DDevice9's method
   order times 4 [verified: each matches the method tools/calltree.sh names for the call, e.g.
   0x7b1838's +0xd4 calls reach LightEnable, +0xcc SetLight]. */
#pragma once
#include <string.h>
#include "decomp.h"

#define D3D_DEVICE               0x994a10u  /* IDirect3DDevice9 * [verified: every call here] */

#define D3D_SET_TRANSFORM        0xb0u
#define D3D_SET_LIGHT            0xccu
#define D3D_LIGHT_ENABLE         0xd4u
#define D3D_SET_RENDER_STATE     0xe4u
#define D3D_SET_TEXTURE          0x104u
#define D3D_SET_TSS              0x10cu     /* SetTextureStageState */
#define D3D_SET_SAMPLER_STATE    0x114u
#define D3D_DRAW_INDEXED         0x148u     /* DrawIndexedPrimitive */
#define D3D_SET_VERTEX_DECL      0x15cu
#define D3D_SET_VERTEX_SHADER    0x170u
#define D3D_SET_STREAM_SOURCE    0x190u
#define D3D_SET_INDICES          0x1a0u
#define D3D_SET_PIXEL_SHADER     0x1acu

/* transform states (D3DTRANSFORMSTATETYPE) */
#define D3DTS_VIEW               2u
#define D3DTS_PROJECTION         3u
#define D3DTS_TEXTURE0           16u
#define D3DTS_TEXTURE1           17u
#define D3DTS_WORLD              256u

/* render states (D3DRENDERSTATETYPE) */
#define D3DRS_ZWRITEENABLE       14u
#define D3DRS_SRCBLEND           19u
#define D3DRS_DESTBLEND          20u
#define D3DRS_CULLMODE           22u
#define D3DRS_ALPHABLENDENABLE   27u
#define D3DRS_AMBIENT            139u
#define D3DRS_DEPTHBIAS          195u

/* the device's method at vtable offset off, with n arguments after the device */
#define D3DCALL(c, off, n, ...) ({ uint32_t _dev = R32(D3D_DEVICE); \
    game_call(c, R32(R32(_dev) + (off)), (n) + 1, _dev, ##__VA_ARGS__); })

/* a float's bits and back (arguments and copies the original moves as dwords) */
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }

/* x87 maths rounded to a float as the original's fstp does (the program's x87 rounding). TRAP:
   GCC narrows (float)((long double)a op b) of two floats to an SSE mulss / addss / subss /
   divss, which rounds by the SSE control word; the x87's and the SSE's rounding differ at times
   in this game, so the result's last bit can differ. It does so in the translation too, per
   site: gen's 0x627f10 compiled to subss, its 0x7b1838 kept fmul [verified: objdump of
   F_00627f10_orig and F_007b1838_orig]. The A/B check compares with the translation, so a
   rewrite rounds as the translation's compiled code does: the switched-in helpers keep the
   (float) casts that narrow alike (0x627f10, 0x62981c, ...: tests/ab.sh same); f32 where the
   translation kept the x87 (0x7b1838's light colours: the plain cast gave 0x3f3aa79c against
   the translation's 0x3f3aa79b on the lobby route's first SetLight). */
static inline float f32(f80 x)
{
    float r;
    __asm__("fstps %0" : "=m"(r) : "t"(x) : "st");
    return r;
}

/* A rewrite written but not switched in yet (left to the translation: tests/ab.sh still differs
   with it, see its comment): kept compiled, not registered. Replace with DECOMP once it matches. */
#define DECOMP_PENDING(a, f) \
    static int (*const decomp_pending_##a)(CPU *) __attribute__((used)) = f

#define MEMCPY                   0x8a9ae0u  /* the C runtime's memcpy (dst, src, n), cdecl */

/* The CPU-specific copies: some functions start with a dispatch on the CPU class the game detected
   (0x8bbd80, run on first use): a Pentium 4 with SSE2 the system supports (flags 0x200) takes an
   SSE version, anything else (flags 1..0x100) the x87 one. runtime/fpu.c reports a Pentium III
   (family 6, SSE), so the flags are 0x40 or 0x80 and the x87 version runs [verified: 0x8bbd80's
   disassembly: family 6 -> 4, MMX 0x10, FXSR 0x20, SSE 0x40/0x80; family 15 with SSE2 -> 0x100/
   0x200]. The rewrites are of the x87 versions; on the other they return 0 from the hook, which
   leaves the whole call to the translation. */
#define CPU_CLASS                0xe807c0u  /* the detected CPU class flags, 0 until detected */
#define CPU_DETECT               0x8bbd80u  /* sets CPU_CLASS (keeps every register) */
static inline int cpu_sse_version(CPU *c)
{
    while (R32(CPU_CLASS) == 0)
        game_call(c, CPU_DETECT, 0);
    return (R32(CPU_CLASS) & 0xfffffe00u) != 0;
}
