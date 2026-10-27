/* AN OBJECT'S MODEL DRAWN (object submission, DESIGN.md "The drawing code's rewrite"): 0x7b1808
   draws the parts of one actor's model for one pass (the 4x4s, the bone palette, each part's
   material, buffers and DrawIndexedPrimitive), with the matrix and vector helpers it uses and the
   bone palette loader (0x7ae7b8, 0x7ae788). Its caller is 0x7b1838 (objects.c). */
#include "objsubmit.h"

/* ---------------------------------------------------------------- matrix and vector helpers */

/* 0x7a68d0: the 4x4 matrix's constructor, which does nothing (returns this). The rewrites here
   don't call it. */
static int dc_7a68d0(CPU *c)
{
    return decomp_ret(c, c->esp, c->ecx, 0);
}
DECOMP(7a68d0, dc_7a68d0);

/* 0x7a6280 (this = 4x4; src): this = src (16 dwords); returns this. */
static int dc_7a6280(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, src = ARG(0);
    for (uint32_t i = 0; i < 64; i += 4)
        W32(m + i, R32(src + i));
    return decomp_ret(c, e, m, 1);
}
DECOMP(7a6280, dc_7a6280);

/* 0x7a6430 (this = A; out, B): out = A x B (row vectors: A applied first), through a temporary,
   so out may be A or B; returns out. */
static int dc_7a6430(CPU *c)
{
    uint32_t e = c->esp, a = c->ecx, out = ARG(0), b = ARG(1);
    uint32_t t[16];
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t j = 0; j < 4; j++) {
            uint32_t r = a + 16 * i, k = b + 4 * j;
            f80 x = (f80)RF32(r) * RF32(k) + (f80)RF32(r + 4) * RF32(k + 0x10);
            x = x + (f80)RF32(r + 8) * RF32(k + 0x20);
            x = x + (f80)RF32(r + 12) * RF32(k + 0x30);
            t[4 * i + j] = f2u((float)x);
        }
    for (uint32_t i = 0; i < 16; i++)
        W32(out + 4 * i, t[i]);
    return decomp_ret(c, e, out, 2);
}
DECOMP(7a6430, dc_7a6430);

/* 0x7b40c4 (this = vec3; x, y, z): sets the vector; returns this. */
static int dc_7b40c4(CPU *c)
{
    uint32_t e = c->esp, v = c->ecx;
    W32(v, ARG(0));
    W32(v + 4, ARG(1));
    W32(v + 8, ARG(2));
    return decomp_ret(c, e, v, 3);
}
DECOMP(7b40c4, dc_7b40c4);

/* 0x7b3ef8 (this = 4x4; v): v = v rotated by the matrix's 3x3 (a direction: no translation), in
   place; returns v. */
static int dc_7b3ef8(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, v = ARG(0);
    float x = RF32(v), y = RF32(v + 4), z = RF32(v + 8);
    for (uint32_t i = 0; i < 3; i++) {
        f80 r = (f80)RF32(m + 4 * i) * x + (f80)RF32(m + 0x10 + 4 * i) * y;
        WF32(v + 4 * i, (float)(r + (f80)RF32(m + 0x20 + 4 * i) * z));
    }
    return decomp_ret(c, e, v, 1);
}
DECOMP(7b3ef8, dc_7b3ef8);

#define VEC3_SET         0x7b40c4u  /* (this = vec3; x, y, z) */

/* 0x7b3fd4 (out, v, m): out = v transformed by the 4x4 m as a point; returns out. */
static int dc_7b3fd4(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), v = ARG(1), m = ARG(2);
    uint32_t r[3];
    for (uint32_t i = 0; i < 3; i++) {
        f80 x = (f80)RF32(m + 4 * i) * RF32(v) + (f80)RF32(m + 0x10 + 4 * i) * RF32(v + 4);
        x = x + (f80)RF32(m + 0x20 + 4 * i) * RF32(v + 8);
        r[i] = f2u((float)(x + RF32(m + 0x30 + 4 * i)));
    }
    game_callm(c, VEC3_SET, out, 3, r[0], r[1], r[2]);
    return decomp_ret(c, e, out, 0);
}
DECOMP(7b3fd4, dc_7b3fd4);

/* ---------------------------------------------------------------- the bone palette */

#define BONE_PALETTE     0xe80f60u  /* the bone matrices the parts are skinned with, 64 bytes each (x87 version) */

/* 0x7ae788 (first, src, count): copies count 4x4s to the bone palette from entry first. The SSE
   version (another palette, plus a constant splat) stays with the translation (objsubmit.h). */
static int dc_7ae788(CPU *c)
{
    if (cpu_sse_version(c))
        return 0;
    uint32_t e = c->esp, first = ARG(0), src = ARG(1), count = ARG(2);
    uint32_t eax = game_call(c, MEMCPY, 3, BONE_PALETTE + first * 64, src, count * 64);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(7ae788, dc_7ae788);

#define PALETTE_LOAD     0x7ae788u
#define ACTOR_BONE_MASK  0x390u     /* bit per palette group: the group has bones of its own */
#define ACTOR_SKELETON   0x38cu     /* the skeleton record: +4 its bone count, +0x10 per group (short first, short count) */
#define ACTOR_GROUP_BASE 0x394u     /* per group: the first matrix's index offset */
#define SKEL_BONES       4u
#define SKEL_GROUPS      0x10u
#define PART_GROUP       0x24u      /* short: the part's palette group */
#define PART_EXTRA_N     0x26u      /* short: extra matrices the part adds after its group's */
#define PART_EXTRA       0x28u      /* the extra matrices' indices (dwords) */

/* 0x7ae7b8 (part, actor, matrices, &skinned, &loaded group): loads the bone palette a part is
   drawn with, from the actor's matrices (64 bytes each): its group's run (when the group has
   bones of its own) plus the part's extra matrices after it, and notes the group; a part of a
   group without bones has the whole skeleton loaded, once (loaded group -1). */
static int dc_7ae7b8(CPU *c)
{
    uint32_t e = c->esp, part = ARG(0), actor = ARG(1), mats = ARG(2), skinned = ARG(3), loaded = ARG(4);
    int32_t g = (int16_t)R16(part + PART_GROUP);
    if ((1u << (g & 31)) & R32(actor + ACTOR_BONE_MASK)) {
        uint32_t grp = R32(R32(actor + ACTOR_SKELETON) + SKEL_GROUPS) + 4 * g;
        game_call(c, PALETTE_LOAD, 3, 0,
                  (int16_t)R16(grp) * 64 + mats + R32(actor + ACTOR_GROUP_BASE + 4 * g) * 64,
                  (uint32_t)(int16_t)R16(grp + 2));
        for (int32_t j = 0; j < (int16_t)R16(part + PART_EXTRA_N); j++) {
            uint32_t grp2 = R32(R32(actor + ACTOR_SKELETON) + SKEL_GROUPS) + 4 * g;
            game_call(c, PALETTE_LOAD, 3, (uint32_t)((int16_t)R16(grp2 + 2) + j),
                      R32(R32(part + PART_EXTRA) + 4 * j) * 64 + mats, 1);
        }
        W32(loaded, (uint32_t)g);
        W8(skinned, 1);
    } else {
        if (R32(loaded) != 0xffffffffu) {
            game_call(c, PALETTE_LOAD, 3, 0, mats, R32(R32(actor + ACTOR_SKELETON) + SKEL_BONES));
            W32(loaded, 0xffffffffu);
        }
        W8(skinned, 0);
    }
    return decomp_ret(c, e, skinned, 0);
}
DECOMP(7ae7b8, dc_7ae7b8);
