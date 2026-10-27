/* LIBRARY ROUTINES linked into the game that the world's drawing reaches: the C library's memset
   (0x8a99b0) and D3DX's D3DXMatrixTranspose (0x7ca6f6, the dispatcher D3DX picks a CPU's version
   through, and 0x7ca6fc, the x87 version) [verified: disassembly; both are called from all over
   the game, so these replace them everywhere]. */
#include <string.h>
#include "world.h"

/* 0x8a99b0 (dst, byte, n) (CRT memset): n bytes of dst set to byte -> dst. Registers as the
   original leaves them: ecx 0 after its dword loop (n >= 4), else dst; edx 0. */
static int dc_8a99b0(CPU *c)
{
    uint32_t e = c->esp, dst = ARG(0), v = ARG(1) & 0xff, n = ARG(2);
    if (n)
        memset(gptr(dst), (int)v, n);
    c->ecx = n >= 4 ? 0 : dst;
    c->edx = 0;
    return decomp_ret(c, e, dst, 0);
}
DECOMP(8a99b0, dc_8a99b0);

#define D3DX_TRANSPOSE_IMPL    0x94a30cu  /* the version D3DX chose for this CPU (0x7ca6fc: x87) */

/* 0x7ca6f6 (out, in) (D3DXMatrixTranspose): jumps to the chosen version. */
static int dc_7ca6f6(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), in = ARG(1);
    game_call(c, R32(D3DX_TRANSPOSE_IMPL), 2, out, in);
    return decomp_ret(c, e, c->eax, 2);
}
DECOMP(7ca6f6, dc_7ca6f6);

/* 0x7ca6fc (out, in) (D3DXMatrixTranspose, x87): out = in transposed (out may be in) -> out. Each
   pair of elements is read before either is written; the diagonal is copied when out is not in.
   The elements only move, so they are copied as dwords (HOWTO.md). */
static int dc_7ca6fc(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), in = ARG(1);
    static const uint8_t pairs[6][2] = {{1, 4}, {2, 8}, {3, 12}, {6, 9}, {7, 13}, {11, 14}};
    for (int k = 0; k < 6; k++) {
        uint32_t a = 4u * pairs[k][0], b = 4u * pairs[k][1];
        uint32_t va = R32(in + a), vb = R32(in + b);
        W32(out + a, vb);
        W32(out + b, va);
    }
    if (out != in)
        for (uint32_t k = 0; k < 4; k++)
            W32(out + 20u * k, R32(in + 20u * k));
    return decomp_ret(c, e, out, 2);
}
DECOMP(7ca6fc, dc_7ca6fc);
