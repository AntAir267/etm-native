/* THE CHARACTERS' ANIMATION, evaluated when they are drawn: each playing animation (a channel)
   keeps a window of its last four decoded keys per track, decoded from a compressed key stream up
   to its current frame (0x6c1606, 0x6d7210, 0x6d71c0); a track's value at the channel's time comes
   from its window (positions 0x6be059 / 0x6be1e9, rotations 0x6be742 by slerps); 0x6cf798 blends
   every channel's tracks by weight into one rotation and position per bone and builds the bones'
   world matrices, which 0x6d0f2d (skin.c) turns into skinning matrices.
   [verified: call tree of the lobby frame 6800 (tools/calltree.sh 6465c8): 0x6f2732 -> 0x6d0f2d ->
   0x6cf798 -> these; disassembly of each]
   FOR INTERPOLATION, what the pose is made of (all read here at draw time): per channel (the
   character's +0x380, next at +0x440) its time in frames (+0x28, a float) and weight (+0x1c), its
   tweak angles (+0x44c, +0x450, +0x454); per character its world matrix (+0x40), lean angles
   (+0x374..+0x37c), position scale (+0x300) and the hook's answers. A frame between two updates
   would evaluate the same at blended channel times (the keys are decoded to the floor of the time,
   so a time going back rewinds the stream: 0x6c1279) and a blended world matrix. Side effects to
   keep to once per update: the turning block (pose_turning) moves channel times and +0x2e8 here.
   FLOAT ROUNDING as in quat.c: plain casts (LF) where the translation keeps the x87 stack in C
   locals (GCC narrows single float operations to SSE there), the asm barrier (LX, X) where it keeps
   it in the CPU record (tools/fpround.py). */
#include "decomp.h"

#define VEC3_SUB         0x62f4d0u  /* (this = vec3; out, vec3 *) -> out = this - v (mathlib.c) */
#define VEC3_SCALE       0x62f560u  /* (this = vec3; out, float *) -> out = this * s */
#define VEC3_DIV         0x62f790u  /* (this = vec3; out, float) -> out = this / s */
#define VEC3_ADD         0x62f850u  /* (this = vec3; out, vec3 *) -> out = this + v */
#define VEC3_COPY        0x41f100u  /* (this = vec3; vec3 *) */

#define F_ZERO           0x8fa020u  /* .rdata 0.0f */
#define F_HALF           0x8fa038u  /* .rdata 0.5f */
#define F_ONE            0x8fa05cu  /* .rdata 1.0f */
#define F_TWO            0x8fa794u  /* .rdata 2.0f */
#define F_THREE          0x8fa040u  /* .rdata 3.0f */
#define F_FOUR           0x8fa798u  /* .rdata 4.0f */

static inline f80 LF(uint32_t a) { return (f80)RF32(a); }
static inline void SF(uint32_t a, f80 v) { WF32(a, (float)v); }
static inline f80 X(f80 v) { __asm__("" : "+t"(v)); return v; }
static inline f80 LX(uint32_t a) { return X((f80)RF32(a)); }
static inline f80 LI(uint32_t a) { return (f80)(int32_t)R32(a); }
static inline uint32_t FU(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* A TRACK'S KEY WINDOW ("path"): its last four keys, four integer frame numbers at +0, +4, +8, +0xc
   and four values (vec3 or quaternion) 16 bytes apart from +0x10, evaluated at a time t (frames) in
   the first segment while t < key 1, in the last while key 2 < t, else in the middle one.
   [verified: 0x6c1606 shifts keys into them; 0x6cf798 evaluates them with these three] */
#define PATH_KEY(i)      (4u * (i))
#define PATH_POINT(i)    (0x10u + 0x10u * (i))

/* 0x6be059 (path *p, vec3 *out, float t) (cdecl): the point at t, linear within its segment:
   out = a + (b - a) * (t - ka) / (kb - ka), through the vec3 operators (temporaries in the frame
   as the original's). eax ends as out. */
static int dc_6be059(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), out = ARG(1);
    f80 t = LF(e + 12);
    uint32_t seg;
    if (LI(p + PATH_KEY(1)) > t)
        seg = 0;
    else if (LI(p + PATH_KEY(2)) < t)
        seg = 2;
    else
        seg = 1;
    /* the original's frame: frac at -0x10 (-0x44, -0x78 in the other two branches), then the four
       temporaries; one layout serves all three */
    uint32_t F = dc_frame(c, 0x34), d = F, s = F + 12, q = F + 24, sum = F + 36, frac = F + 48;
    SF(frac, t - LI(p + PATH_KEY(seg)));
    float len = (float)(f80)(int32_t)(R32(p + PATH_KEY(seg + 1)) - R32(p + PATH_KEY(seg)));
    uint32_t r = game_callm(c, VEC3_SUB, p + PATH_POINT(seg + 1), 2, d, p + PATH_POINT(seg));
    r = game_callm(c, VEC3_SCALE, r, 2, s, frac);
    r = game_callm(c, VEC3_DIV, r, 2, q, FU(len));
    r = game_callm(c, VEC3_ADD, p + PATH_POINT(seg), 2, sum, r);
    r = game_callm(c, VEC3_COPY, out, 1, r);
    dc_unframe(c, F, 0x34);
    return decomp_ret(c, e, r, 0);
}
DECOMP(6be059, dc_6be059);

/* the original's locals in their places, off its frame pointer fp: later code reads what a function
   leaves on the stack (tests/ab_levels.sh, 2026-10-30) */
#define LOC(off, v) W32(fp - (off), (uint32_t)(v))
#define LOCF(off, v) WF32(fp - (off), (v))

/* 0x6be1e9 (path *p, vec3 *out, float t) (cdecl): the point at t on a smooth curve through the
   path: four weights w0..w3 of the points from the position u within the segment, reshaped by a
   cubic when the neighbouring segments' lengths differ (r, the ratio, below 1), then
   out = w0 p0 + w1 p1 + w2 p2 + w3 p3 (x, y, z). eax ends as p. [guess: a quadratic B-spline
   blend; the weights sum to 1] */
static int dc_6be1e9(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), out = ARG(1), fp = e - 4;
    f80 t = LF(e + 12);
    float w0, w1, w2, w3;
    W32(fp, c->ebp);
    if (LI(p + 4) > t) {
        /* the first segment */
        int32_t da = (int32_t)(R32(p + 4) - R32(p)), db = (int32_t)(R32(p + 8) - R32(p + 4));
        float u = (float)((t - LI(p)) / (f80)da);
        float one = 1.0f;
        float r = (float)((f80)da / (f80)db);
        LOC(0x48, da); LOC(0x4c, da); LOC(0x50, db);
        LOCF(0x24, one); LOCF(0x1c, r);
        if ((f80)r < LF(F_ONE)) {
            f80 a = ((f80)r + (f80)one - LF(F_TWO)) * (f80)u * (f80)u * (f80)u;
            f80 b = (f80)r - (f80)one;
            f80 cc = ((f80)r + (f80)one - LF(F_TWO)) * LF(F_THREE);
            f80 dd = (b - cc) / LF(F_TWO) * (f80)u * (f80)u;
            u = (float)(a + dd + (f80)one * (f80)u);
        }
        LOCF(0x20, u);
        float h = (float)((f80)u / LF(F_TWO));
        f80 v = LF(F_ONE) - (f80)h;
        float g = (float)v;
        LOCF(0x18, h); LOCF(0x14, g);
        w0 = (float)(v * (f80)g - (f80)g * (f80)h);
        w1 = (float)(LF(F_FOUR) * (f80)g * (f80)h);
        w2 = (float)((f80)h * (f80)h - (f80)g * (f80)h);
        w3 = 0;
    } else if (LI(p + 8) < t) {
        /* the last segment */
        int32_t da = (int32_t)(R32(p + 12) - R32(p + 8)), db = (int32_t)(R32(p + 8) - R32(p + 4));
        float u = (float)((t - LI(p + 8)) / (f80)da);
        float r = (float)((f80)da / (f80)db);
        float one = 1.0f;
        LOC(0x54, da); LOC(0x58, da); LOC(0x5c, db);
        LOCF(0x30, r); LOCF(0x28, one);
        if ((f80)r < LF(F_ONE)) {
            f80 a = ((f80)one + (f80)r - LF(F_TWO)) * (f80)u * (f80)u * (f80)u;
            f80 b = (f80)one - (f80)r;
            f80 cc = ((f80)one + (f80)r - LF(F_TWO)) * LF(F_THREE);
            f80 dd = (b - cc) / LF(F_TWO) * (f80)u * (f80)u;
            u = (float)(a + dd + (f80)r * (f80)u);
        }
        if ((f80)u > LF(F_ONE))
            u = 1.0f;
        LOCF(0x2c, u);
        float h = (float)((f80)u / LF(F_TWO) + LF(F_HALF));
        float g = (float)(LF(F_ONE) - (f80)h);
        LOCF(0x18, h); LOCF(0x14, g);
        w0 = 0;
        w1 = (float)((f80)g * (f80)g - (f80)g * (f80)h);
        w2 = (float)(LF(F_FOUR) * (f80)g * (f80)h);
        w3 = (float)((f80)h * (f80)h - (f80)g * (f80)h);
    } else {
        /* the middle segment: the halves of two blends */
        int32_t d0 = (int32_t)(R32(p + 4) - R32(p)), d1 = (int32_t)(R32(p + 8) - R32(p + 4)),
                d2 = (int32_t)(R32(p + 12) - R32(p + 8));
        float u = (float)((t - LI(p + 4)) / (f80)d1);
        float ra = (float)((f80)d1 / (f80)d0);
        float rb = (float)((f80)d1 / (f80)d2);
        LOC(0x60, d1); LOC(0x64, d1); LOC(0x68, d0); LOC(0x6c, d1); LOC(0x70, d2);
        if ((f80)ra < LF(F_ONE) || (f80)rb < LF(F_ONE)) {
            if ((f80)ra > LF(F_ONE))
                ra = 1.0f;
            if ((f80)rb > LF(F_ONE))
                rb = 1.0f;
            f80 a = ((f80)rb + (f80)ra - LF(F_TWO)) * (f80)u * (f80)u * (f80)u;
            f80 b = (f80)rb - (f80)ra;
            f80 cc = ((f80)rb + (f80)ra - LF(F_TWO)) * LF(F_THREE);
            f80 dd = (b - cc) / LF(F_TWO) * (f80)u * (f80)u;
            u = (float)(a + dd + (f80)ra * (f80)u);
        }
        LOCF(0x44, ra); LOCF(0x34, rb); LOCF(0x38, u);
        float m = u;
        f80 v = LF(F_ONE) - (f80)m;
        float n = (float)v;
        float h = (float)(v / LF(F_TWO));
        float g = (float)(LF(F_ONE) - (f80)h);
        LOCF(0x3c, m); LOCF(0x40, n);
        w0 = (float)(((f80)h * (f80)h - (f80)h * (f80)g) * (f80)n);
        w1 = (float)(LF(F_FOUR) * (f80)h * (f80)g * (f80)n);
        w2 = (float)(((f80)g * (f80)g - (f80)h * (f80)g) * (f80)n);
        h = (float)((f80)h + LF(F_HALF));
        g = (float)(LF(F_ONE) - (f80)h);
        LOCF(0x14, h); LOCF(0x18, g);
        w1 = (float)(((f80)h * (f80)h - (f80)h * (f80)g) * (f80)m + (f80)w1);
        w2 = (float)(LF(F_FOUR) * (f80)h * (f80)g * (f80)m + (f80)w2);
        w3 = (float)(((f80)g * (f80)g - (f80)h * (f80)g) * (f80)m);
    }
    LOCF(4, w0); LOCF(8, w1); LOCF(0xc, w2); LOCF(0x10, w3);
    for (uint32_t i = 0; i < 3; i++)
        SF(out + 4 * i, (f80)w0 * LF(p + PATH_POINT(0) + 4 * i) + (f80)w1 * LF(p + PATH_POINT(1) + 4 * i)
                        + (f80)w2 * LF(p + PATH_POINT(2) + 4 * i) + (f80)w3 * LF(p + PATH_POINT(3) + 4 * i));
    return decomp_ret(c, e, p, 0);
}
DECOMP(6be1e9, dc_6be1e9);

/* a float as an x87 operand that GCC cannot narrow (for the CPU-record translations) */
#define V(f) X((f80)(f))
#define SLERP            0x6bb9b9u  /* (this = quat out; quat *a, quat *b, float t) (quat.c) */

/* 0x6be742 (path of quaternions *p, quat *out, float t) (cdecl): the rotation at t on the path's
   four keyed quaternions (+0x10, +0x20, +0x30, +0x40): the same four weights as 0x6be1e9 (here
   halving by * 0.5), then a key whose weight is exactly 1 is copied, else the quaternions are
   blended by successive slerps (SLERP) with the running weight fractions. eax as the original
   leaves it (the last dword copied, out, or the last slerp's). [guess: a bone's rotation track] */
static int dc_6be742(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), out = ARG(1);
    f80 t = LX(e + 12);
    float w0, w1, w2, w3;
    if (LI(p + 4) > t) {
        float u = (float)((t - LI(p)) / (f80)(int32_t)(R32(p + 4) - R32(p)));
        float one = 1.0f;
        float r = (float)((f80)(int32_t)(R32(p + 4) - R32(p)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        if (V(r) < LX(F_ONE)) {
            f80 a = (V(r) + V(one) - LX(F_TWO)) * V(u) * V(u) * V(u);
            f80 b = V(r) - V(one);
            f80 cc = (V(r) + V(one) - LX(F_TWO)) * LX(F_THREE);
            f80 dd = (b - cc) * LX(F_HALF) * V(u) * V(u);
            u = (float)(a + dd + V(one) * V(u));
        }
        float h = (float)(V(u) * LX(F_HALF));
        f80 v = LX(F_ONE) - V(h);
        float g = (float)v;
        w0 = (float)(v * V(g) - V(g) * V(h));
        w1 = (float)(LX(F_FOUR) * V(g) * V(h));
        w2 = (float)(V(h) * V(h) - V(g) * V(h));
        w3 = 0;
    } else if (LI(p + 8) < t) {
        float u = (float)((t - LI(p + 8)) / (f80)(int32_t)(R32(p + 12) - R32(p + 8)));
        float r = (float)((f80)(int32_t)(R32(p + 12) - R32(p + 8)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        float one = 1.0f;
        if (V(r) < LX(F_ONE)) {
            f80 a = (V(one) + V(r) - LX(F_TWO)) * V(u) * V(u) * V(u);
            f80 b = V(one) - V(r);
            f80 cc = (V(one) + V(r) - LX(F_TWO)) * LX(F_THREE);
            f80 dd = (b - cc) * LX(F_HALF) * V(u) * V(u);
            u = (float)(a + dd + V(r) * V(u));
        }
        if (V(u) > LX(F_ONE))
            u = 1.0f;
        float h = (float)(V(u) * LX(F_HALF) + LX(F_HALF));
        float g = (float)(LX(F_ONE) - V(h));
        w0 = 0;
        w1 = (float)(V(g) * V(g) - V(g) * V(h));
        w2 = (float)(LX(F_FOUR) * V(g) * V(h));
        w3 = (float)(V(h) * V(h) - V(g) * V(h));
    } else {
        float u = (float)((t - LI(p + 4)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        float ra = (float)((f80)(int32_t)(R32(p + 8) - R32(p + 4)) / (f80)(int32_t)(R32(p + 4) - R32(p)));
        float rb = (float)((f80)(int32_t)(R32(p + 8) - R32(p + 4)) / (f80)(int32_t)(R32(p + 12) - R32(p + 8)));
        if (V(ra) < LX(F_ONE) || V(rb) < LX(F_ONE)) {
            if (V(ra) > LX(F_ONE))
                ra = 1.0f;
            if (V(rb) > LX(F_ONE))
                rb = 1.0f;
            f80 a = (V(rb) + V(ra) - LX(F_TWO)) * V(u) * V(u) * V(u);
            f80 b = V(rb) - V(ra);
            f80 cc = (V(rb) + V(ra) - LX(F_TWO)) * LX(F_THREE);
            f80 dd = (b - cc) * LX(F_HALF) * V(u) * V(u);
            u = (float)(a + dd + V(ra) * V(u));
        }
        float m = u;
        f80 v = LX(F_ONE) - V(m);
        float n = (float)v;
        float h = (float)(v * LX(F_HALF));
        float g = (float)(LX(F_ONE) - V(h));
        w0 = (float)((V(h) * V(h) - V(h) * V(g)) * V(n));
        w1 = (float)(LX(F_FOUR) * V(h) * V(g) * V(n));
        w2 = (float)((V(g) * V(g) - V(h) * V(g)) * V(n));
        h = (float)(V(h) + LX(F_HALF));
        g = (float)(LX(F_ONE) - V(h));
        w1 = (float)((V(h) * V(h) - V(h) * V(g)) * V(m) + V(w1));
        w2 = (float)(LX(F_FOUR) * V(h) * V(g) * V(m) + V(w2));
        w3 = (float)((V(g) * V(g) - V(h) * V(g)) * V(m));
    }
    uint32_t eax;
    if (FU(w1) == 0x3f800000u) {
        for (uint32_t i = 0; i < 4; i++)
            W32(out + 4 * i, eax = R32(PATH_POINT(1) + p + 4 * i));
    } else if (FU(w2) == 0x3f800000u) {
        for (uint32_t i = 0; i < 4; i++)
            W32(out + 4 * i, R32(PATH_POINT(2) + p + 4 * i));
        eax = R32(PATH_POINT(2) + p + 8);
    } else if (FU(w3) == 0x3f800000u) {
        for (uint32_t i = 0; i < 4; i++)
            W32(out + 4 * i, R32(PATH_POINT(3) + p + 4 * i));
        eax = out;
    } else if (!(V(w0) == LX(F_ZERO))) {
        /* (unordered counts as not zero, as the original's test ah, 0x44 / jnp) */
        if (FU(w0) == 0x3f800000u) {
            for (uint32_t i = 0; i < 4; i++)
                W32(out + 4 * i, eax = R32(PATH_POINT(0) + p + 4 * i));
        } else {
            game_callm(c, SLERP, out, 3, p + PATH_POINT(0), p + PATH_POINT(1), FU((float)(V(w1) / (V(w0) + V(w1)))));
            eax = game_callm(c, SLERP, out, 3, out, p + PATH_POINT(2), FU((float)(V(w2) / (V(w0) + V(w1) + V(w2)))));
            if (!(V(w3) == LX(F_ZERO)))
                eax = game_callm(c, SLERP, out, 3, out, p + PATH_POINT(3),
                                 FU((float)(V(w3) / (V(w0) + V(w1) + V(w2) + V(w3)))));
        }
    } else {
        game_callm(c, SLERP, out, 3, p + PATH_POINT(1), p + PATH_POINT(2), FU((float)(V(w2) / (V(w1) + V(w2)))));
        eax = game_callm(c, SLERP, out, 3, out, p + PATH_POINT(3), FU((float)(V(w3) / (V(w1) + V(w2) + V(w3)))));
    }
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6be742, dc_6be742);

#define F_INV511         0x8fcd90u  /* .rdata 1/511 */

/* 0x6d7dd0 (int v) -> (v - 512) / 511 on the x87 stack (cdecl): a 10-bit quaternion component
   (0..1023) as a float in [-1, 1]. eax ends as v - 512. */
static int dc_6d7dd0(CPU *c)
{
    uint32_t e = c->esp, v = ARG(0) - 0x200u;
    f80 r = (f80)(int32_t)v * LF(F_INV511);
    return decomp_ret_st0(c, e, r, 0);
}
DECOMP(6d7dd0, dc_6d7dd0);

#define CRT_MEMCPY       0x8a9ae0u  /* memcpy(dst, src, n) (cdecl) */

/* 0x6d71c0 (void *dst, const void *src, ?, int format) -> bytes read (cdecl): a position key
   kept as three raw floats when the format's low two bits are 0 (12 bytes copied), else nothing
   (0). [guess: the track's position format; only the raw one is handled] */
static int dc_6d71c0(CPU *c)
{
    uint32_t e = c->esp, dst = ARG(0), src = ARG(1), fmt = ARG(3);
    if ((fmt & 3) != 0)
        return decomp_ret(c, e, 0, 0);
    game_call(c, CRT_MEMCPY, 3, dst, src, 12);
    return decomp_ret(c, e, 12, 0);
}
DECOMP(6d71c0, dc_6d71c0);

/* THE SKELETON (the character's +0x38c, skin.c): the bone count at +4, the bones 0x7c bytes apart
   from +0x24; each bone's level of detail at +0x68 (a signed byte: bones above 40 get a coarser
   key format) [guess: what +0x68 means, from 0x6d0f2d dropping bones by it against its 100] */
#define SKEL_BONES       4u
#define SKEL_BONE(i)     (0x24u + 0x7cu * (i))
#define BONE_LOD         0x68u

#define SQRTF            0x41e8d0u  /* (float) -> sqrt on the x87 stack (mathlib.c) */
#define DEC10            0x6d7dd0u  /* (int 10 bits) -> component on the x87 stack */
#define DEC16_A          0x6d7d70u  /* (int 16 bits) -> component on the x87 stack (format 8) */
#define DEC16_B          0x6d7d10u  /* (int 16 bits) -> component on the x87 stack (format 0) */
#define F_MINUS2         0x8fbdecu  /* .rdata -2.0f */

/* a unit quaternion's left-out component from the other three: sqrt(1 - a^2 - b^2 - d^2) */
static float quat_rest(CPU *c, float a, float b, float d)
{
    float s = (float)(LX(F_ONE) - V(a) * V(a) - V(b) * V(b) - V(d) * V(d));
    return (float)game_call_st0(c, SQRTF, 1, FU(s));
}

/* the quaternion written with the rebuilt component w at slot i (0 = w) and a, b, d in the
   other three in order */
static void quat_place(uint32_t q, uint32_t i, float a, float b, float d, float w)
{
    float rest[3] = {a, b, d}, v[4];
    for (uint32_t k = 0, n = 0; k < 4; k++)
        v[k] = k == i ? w : rest[n++];
    for (uint32_t k = 0; k < 4; k++)
        WF32(q + 4 * k, v[k]);
}

/* 0x6d7210 (quat *out, u8 *src, quat *prev or 0, int format, int bone, skeleton *s) -> bytes read
   (cdecl): one compressed rotation key. The format's bits 2-3 choose the encoding (bones past the
   skeleton's count use bits 6-7, bones whose level of detail is above 40 bits 4-5); the largest
   component is left out and rebuilt as sqrt(1 - the others' squares):
     12: a dword of three 10-bit components and the left-out slot (4 bytes);
      8 and 0: three 16-bit values decoded by DEC16_A / DEC16_B, the slot in the first two's top
         bits and the sign in the third's; added to prev's other three components when there is
         one, else halved (6 bytes);
      4: three raw floats, the slot told by the first two being offset by 2 or not (12 bytes). */
static int dc_6d7210(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), src = ARG(1), prev = ARG(2), bone = ARG(4), skel = ARG(5);
    int32_t fmt = (int32_t)ARG(3);
    /* the original's frame, its locals in their places: later code reads what these calls leave on
       the stack (the city drive: a record's float was -0.25 after the original, -209 after a
       rewrite with its locals elsewhere [verified: tests of 2026-10-30, memory at frame 1250]) */
    uint32_t fp = e - 4, ebp = c->ebp;
#define KEY_RET(n) do { c->ebp = ebp; return decomp_ret(c, e, (n), 0); } while (0)
    W32(fp, ebp);
    c->ebp = fp;
    c->esp = fp - 0x9c;
    if ((int32_t)bone >= (int32_t)R32(skel + SKEL_BONES))
        fmt >>= 4;
    else if ((int8_t)R8(skel + SKEL_BONE(bone) + BONE_LOD) > 0x28)
        fmt >>= 2;
    LOC(0x84, fmt & 0xc);
    if ((fmt & 0xc) == 0xc) {
        uint32_t v = R32(src), slot = v & 3;
        uint32_t i0 = (uint32_t)((int32_t)v >> 2) & 0x3ffu, i1 = (uint32_t)((int32_t)v >> 12) & 0x3ffu,
                 i2 = (uint32_t)((int32_t)v >> 22) & 0x3ffu;
        LOC(0x18, v); LOC(0x1c, slot); LOC(4, i0); LOC(8, i1); LOC(0x10, i2);
        float a = (float)game_call_st0(c, DEC10, 1, i0);
        LOCF(0x20, a);
        float b = (float)game_call_st0(c, DEC10, 1, i1);
        LOCF(0x24, b);
        float d = (float)game_call_st0(c, DEC10, 1, i2);
        LOCF(0xc, d);
        float w = quat_rest(c, a, b, d);
        LOCF(0x14, w);
        LOC(0x88, slot);
        quat_place(out, slot, a, b, d, w);
        KEY_RET(4);
    }
    if ((fmt & 0xc) == 8 || (fmt & 0xc) == 0) {
        /* (the two formats' locals: format 8's, format 0's) */
        int f8 = (fmt & 0xc) == 8;
        uint32_t dec = f8 ? DEC16_A : DEC16_B;
        uint32_t s0 = R16(src), s1 = R16(src + 2), s2 = R16(src + 4);
        uint32_t neg = s2 & 0x8000u, slot = ((s0 & 0x8000u) >> 14) + ((s1 & 0x8000u) >> 15);
        LOC(f8 ? 0x28 : 0x4c, s0); LOC(f8 ? 0x34 : 0x58, s1); LOC(f8 ? 0x3c : 0x60, s2);
        LOC(f8 ? 0x30 : 0x54, neg); LOC(f8 ? 0x40 : 0x64, slot);
        uint32_t la = f8 ? 0x44 : 0x68, lb = f8 ? 0x48 : 0x6c, ld = f8 ? 0x2c : 0x50, lw = f8 ? 0x38 : 0x5c;
        float a, b, d;
        if (prev) {
            a = (float)game_call_st0(c, dec, 1, s0);
            LOCF(la, a);
            b = (float)game_call_st0(c, dec, 1, s1);
            LOCF(lb, b);
            d = (float)game_call_st0(c, dec, 1, s2);
            LOCF(ld, d);
            LOC(f8 ? 0x8c : 0x94, slot);
            uint32_t o[3];    /* prev's three components other than the slot */
            for (uint32_t k = 0, n = 0; k < 4; k++)
                if (k != slot)
                    o[n++] = 4 * k;
            a = (float)(V(a) + LX(prev + o[0]));
            b = (float)(V(b) + LX(prev + o[1]));
            d = (float)(V(d) + LX(prev + o[2]));
        } else {
            a = (float)(game_call_st0(c, dec, 1, s0) * LX(F_HALF));
            LOCF(la, a);
            b = (float)(game_call_st0(c, dec, 1, s1) * LX(F_HALF));
            LOCF(lb, b);
            d = (float)(game_call_st0(c, dec, 1, s2) * LX(F_HALF));
        }
        LOCF(la, a); LOCF(lb, b); LOCF(ld, d);
        float w = quat_rest(c, a, b, d);
        if (neg)
            w = (float)-V(w);
        LOCF(lw, w);
        LOC(f8 ? 0x90 : 0x98, slot);
        quat_place(out, slot, a, b, d, w);
        KEY_RET(6);
    }
    /* 4: raw floats */
    float a = RF32(src), b = RF32(src + 4), d = RF32(src + 8);
    uint32_t bits = 0;
    if (V(a) >= LX(F_TWO)) {
        a = (float)(V(a) - LX(F_TWO));
        bits |= 1;
    } else if (V(a) <= LX(F_MINUS2)) {
        a = (float)(V(a) + LX(F_TWO));
        bits |= 1;
    }
    if (V(b) >= LX(F_TWO)) {
        b = (float)(V(b) - LX(F_TWO));
        bits |= 2;
    } else if (V(b) <= LX(F_MINUS2)) {
        b = (float)(V(b) + LX(F_TWO));
        bits |= 2;
    }
    LOCF(0x70, a); LOCF(0x74, b); LOCF(0x78, d); LOC(0x80, bits);
    float w = quat_rest(c, a, b, d);
    LOCF(0x7c, w);
    LOC(0x9c, bits);
    quat_place(out, 3 - bits, a, b, d, w);    /* bits 0: w last; 1: third; 2: second; 3: first */
    KEY_RET(12);
#undef KEY_RET
}
DECOMP(6d7210, dc_6d7210);

/* AN ANIMATION BEING PLAYED (the channels listed from the character's +0x380, skin.c): its data at
   +0xc, its time in frames at +0x28 (a float), how many frames of the key stream are decoded at
   +0x38, the stream's read pointer at +0x3c, and per track a window of its last four keys at
   +0x40 + 4 * track (a path of four keys: the frame numbers at +0, the values 16 bytes apart from
   +0x10, read by 0x6be1e9 / 0x6be742). [verified: 0x6c1606's reads and writes; what they are:
   guess from their use] */
#define CH_OWNER         4u         /* the character (its skeleton at +0x38c) */
#define CH_ANIM          0xcu
#define CH_TIME          0x28u
#define CH_DECODED       0x38u
#define CH_STREAM        0x3cu
#define CH_WINDOW(t)     (0x40u + 4u * (t))
#define ANIM_TRACKS      0x1cu      /* track table: 8 bytes per track from +0x10 */
#define ANIM_FRAMES      0x26u      /* short: frame count */
#define ANIM_FORMAT      0x28u      /* short: key formats (0x6d7210, 0x6d71c0) */
#define TRACK_BONE(t)    (0x10u + 8u * (t))   /* short */
#define TRACK_IS_POS(t)  (0x12u + 8u * (t))   /* byte: a position track (else rotation) */
#define CHAR_SKELETON    0x38cu

#define CRT_FTOL         0x8a9ef4u  /* _ftol: x87 top popped, truncated, in edx:eax */
#define CH_REWIND        0x6c1279u  /* (this = channel): the stream back to its start [guess] */
#define POS_KEY          0x6d71c0u
#define ROT_KEY          0x6d7210u

/* 0x6c1606 (this = channel): decodes the key stream up to the channel's current frame (its time
   truncated, at most the last frame; rewound first if the time went back): per frame a count of
   keys, each a track number, a frame delta (one byte, or two when the first's top bit is set) and
   the key, which shifts the track's window by one (frames and values) into its last slot. A count
   of 0xff ends the stream. eax as the original leaves it. */
static int dc_6c1606(CPU *c)
{
    uint32_t e = c->esp, ch = c->ecx, eax = ch, fp = e - 4, ebp = c->ebp;
    W32(fp, ebp);
    c->ebp = fp;
    c->esp = fp - 0x1c;
    LOC(0x1c, ch);
    if (!R32(ch + CH_ANIM)) {
        c->ebp = ebp;
        return decomp_ret(c, e, eax, 0);
    }
    int32_t fmt = (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FORMAT);
    LOC(8, fmt);
    FPUSH(LX(ch + CH_TIME));
    int32_t frame = (int32_t)game_call_at(c, 0x6c1634u, CRT_FTOL, 0);
    if (frame > (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1)
        frame = (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1;
    LOC(4, frame);
    eax = R32(ch + CH_DECODED) - 1;
    if (frame < (int32_t)eax)
        c->ecx = ch, eax = game_call_at(c, 0x6c166fu, CH_REWIND, 0);
    while (frame >= (int32_t)R32(ch + CH_DECODED)) {
        eax = ch;
        if (R8(R32(ch + CH_STREAM)) == 0xff)
            break;
        int32_t n = (int32_t)R8(R32(ch + CH_STREAM));
        LOC(0xc, n);
        W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
        for (; n > 0; LOC(0xc, --n)) {
            uint32_t track = R8(R32(ch + CH_STREAM));
            LOC(0x14, track);
            W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            uint32_t delta = R8(R32(ch + CH_STREAM));
            W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            if (delta & 0x80) {
                delta = (delta & 0x7f) << 8;
                delta += R8(R32(ch + CH_STREAM));
                W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            }
            LOC(0x10, delta);
            uint32_t w = R32(ch + CH_WINDOW(track));
            LOC(0x18, w);
            W32(w, R32(w + 4));
            W32(w + 4, R32(w + 8));
            W32(w + 8, R32(w + 12));
            W32(w + 12, R32(w + 12) + delta);
            game_call_at(c, 0x6c178cu, CRT_MEMCPY, 3, w + 0x10, w + 0x20, 0x10);
            game_call_at(c, 0x6c17a4u, CRT_MEMCPY, 3, w + 0x20, w + 0x30, 0x10);
            game_call_at(c, 0x6c17bcu, CRT_MEMCPY, 3, w + 0x30, w + 0x40, 0x10);
            uint32_t len;
            if (R8(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + TRACK_IS_POS(track)))
                len = game_call_at(c, 0x6c17f3u, POS_KEY, 4, w + 0x40, R32(ch + CH_STREAM), w + 0x40, (uint32_t)fmt);
            else
                len = game_call_at(c, 0x6c1841u, ROT_KEY, 6, w + 0x40, R32(ch + CH_STREAM), w + 0x40, (uint32_t)fmt,
                                (uint32_t)(int32_t)(int16_t)R16(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + TRACK_BONE(track)),
                                R32(R32(ch + CH_OWNER) + CHAR_SKELETON));
            W32(ch + CH_STREAM, len + R32(ch + CH_STREAM));
        }
        W32(ch + CH_DECODED, R32(ch + CH_DECODED) + 1);
        eax = ch;
    }
    c->ebp = ebp;
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6c1606, dc_6c1606);

/* ------------------------------------------------------------------ the pose (0x6cf798) */

/* THE CHARACTER (this of 0x6cf798 and 0x6d0f2d; skin.c has the rest) [verified: the reads below;
   names guess unless said] */
#define CHAR_WORLD       0x40u      /* 4x4: where the character stands */
#define CHAR_BONE_IDS    0x168u     /* table of special bones: +0x30 (pelvis?) +0x34 +0x38 +0x3c
                                       (spine/neck chain?) +0x6c +0x70 (head/neck?) [guess] */
#define CHAR_MODEL       0x2c0u     /* +0x14 float, +0x1c each bone's bind position (vec3) */
#define CHAR_FLAGS       0x2c4u
#define CHAR_BLEND       0x2e8u     /* float, eased toward 0.2 / 0.7 / 1 by the turning block */
#define CHAR_HOOK        0x2fcu     /* float (*)(int what, character *, void *) (0x6fb254) or 0 */
#define CHAR_SCALE       0x300u     /* float: the animations' positions scaled by it */
#define CHAR_LEAN        0x374u     /* three floats; the root turned by them when any is beyond
                                       +-0.01 [guess: lean / tilt] */
#define CHAR_CHANNELS    0x380u     /* first animation channel; each next at +0x440 */
#define CHAR_RANGE_OFF   0x394u     /* per bone range, the offset of its bones in the buffer */
#define MODEL_ROOT_Y     0x14u
#define MODEL_BIND_POS   0x1cu
#define SKEL_RANGES      0x10u      /* (short first bone, short count) per range */
#define SKEL_ROOT_Y      0x4cu
#define BONE_BIND_ROT    0x30u      /* 3x3 bind rotation (relative to the parent) */
#define BONE_BIND_QUAT   0x54u      /* the same as a quaternion */
#define BONE_PARENT      0x64u      /* short */
#define CH_NEXT          0x440u
#define CH_STATE         8u         /* +0xc float (turn per unit weight), +0x14 float */
#define CH_WEIGHT        0x1cu      /* float: how much of the pose this channel gives */
#define CH_TIME_COPY     0x2cu
#define CH_TURN_Z        0x44cu     /* float: the root rotated about z by -it */
#define CH_ROT_A         0x450u     /* floats added up weighted: the head/neck bones rotated by */
#define CH_ROT_B         0x454u     /*   halves of them [guess] */
#define ANIM_ROOT_Y      0x18u      /* float: the root's height when not 0 */
#define ANIM_ROOT_FLAGS  0x20u      /* short: not negative, the root keeps x and z at 0 */
#define ANIM_FLAGS       0x28u      /* short: 0x100 = applied after the others, on top */
#define TRACKS_COUNT     0xau       /* short, in the track table */
#define TRACK(k)         (0xcu + 8u * (k))   /* entry: float weight, short bone, byte position */
#define G_ANIM_SPEED     0x97f570u  /* float (.data) [guess: a speed scale] */

#define MAT3_CTOR        0x6bc6d0u  /* (this = mat3) */
#define MAT3_FROM_QUAT   0x6b9e68u  /* (this = mat3; quat *) (quat.c) */
#define MAT3_YAW         0x6b9a56u  /* (this = mat3) -> float on the x87 stack [guess] */
#define MAT3_IDENTITY    0x6b96fcu  /* (this = mat3) [guess] */
#define MAT3_ROTATE      0x6b9798u  /* (this = mat3; float, float, float) [guess: angles] */
#define MAT3_MUL         0x6b950bu  /* (this = mat3; mat3 *) */
#define MAT3_CTOR_QUAT   0x6bc800u  /* (this = mat3; quat *) (quat.c) */
#define QUAT_FROM_MAT3   0x6ba020u  /* (this = quat; mat3 *) */
#define QUAT_MUL         0x6ba5ddu  /* (this = quat; quat *out, quat *b) -> out */
#define QUAT_CTOR        0x6b42a0u  /* (this = quat) (mathlib.c) */
#define VEC3_CTOR        0x41e940u
#define VEC3_ADD_TO      0x5e0980u  /* (this = vec3; vec3 *) this += v */
#define VEC3_SCALE_BY    0x6b4470u  /* (this = vec3; float *) this *= s */
#define VEC_CTOR_ITER    0x5fe5c0u  /* (array, size, count, ctor) */
#define QUAT_CTOR_STUB   0x40755eu  /* the incremental-link stubs the original passes as ctors */
#define VEC3_CTOR_STUB   0x4017c1u
#define CHAR_SPEED       0x5f1870u  /* (this = character) -> float on the x87 stack [guess] */
#define CH_DECODE        0x6c1606u
#define PATH_LINEAR      0x6be059u
#define PATH_SMOOTH      0x6be1e9u
#define PATH_ROT         0x6be742u
#define MAT4_CTOR        0x47f630u
#define MAT4_IDENTITY    0x6ba8efu
#define MAT4_FROM_ROT    0x6bb121u  /* (this = mat4; mat3 *, vec3 *) */
#define MAT4_FROM_QUAT   0x6bb1d8u  /* (this = mat4; quat *, vec3 *) */
#define MAT4_MUL         0x6bb745u  /* (this = mat4; mat4 *) this *= b */
#define MAT4_EULER       0x6ba742u  /* (this = mat4; float ax, ay, az) degrees */
#define MAT4_ROTATE      0x6ba996u  /* (this = mat4; float, float, float) [guess: angles] */
#define MAT4_TURN        0x6bab43u  /* (this = mat4; float) [guess: about the vertical] */

#define F_100            0x8fa104u  /* .rdata 100.0f: weights are percentages */
#define F_HUNDREDTH      0x8fa048u  /* .rdata 0.01f */
#define F_INV360         0x8fcdfcu  /* .rdata 1/360 */
#define F_M180           0x8fbd28u
#define F_180            0x8fa04cu
#define F_360            0x8fae9cu
#define F_70             0x8fa134u
#define F_M70            0x8fa138u
#define F_TENTH          0x8fa790u
#define F_MINUS1         0x8fa0a0u
#define F_3_5            0x8fcde0u
#define F_0_65           0x8fcdf8u
#define F_0_35           0x8fa7e4u
#define F_20             0x8fa030u
#define F_30             0x8fa17cu
#define F_M30            0x8fcdf4u
#define F_0_8            0x8fa32cu
#define D_HUNDREDTH      0x8fbd90u  /* .rdata 0.01 (double) */
#define D_MHUNDREDTH     0x8fcde8u  /* .rdata -0.01 (double) */

/* the original's frame, kept as it was (fp is its ebp): the arrays and the locals callees get */
#define FR_ROTW          0x210u     /* float[128]: each bone's rotation weight so far */
#define FR_POSW          0xc20u     /* float[128]: position weight */
#define FR_ROT           0xa18u     /* quat[128]: the blended rotations (the hook gets them) */
#define FR_POS           0x1230u    /* vec3[128]: the blended positions */
#define FR_TMP           0x10u
#define FR_M3            0x12b0u
#define FR_HOOKARG       0x12b4u
#define FR_QBIND         0x12e4u
#define FR_Q             0x12f4u
#define FR_M3A           0x1318u
#define FR_M3B           0x133cu
#define FR_P             0x134cu
#define FR_QC            0x1374u
#define FR_R             0x13b8u
#define FR_QOUT          0x13c8u
#define FR_ONE           0x13ccu
#define FR_V3            0x13d8u
#define FR_S1            0x13dcu
#define FR_S2            0x13e0u
#define FR_SIZE          0x13e8u

typedef struct {
    uint32_t self, nb, mask, out, fp;
    float turn;    /* -0x1238: the root's turn, from rotation tracks of bone 0 */
    float ra;      /* -0xc: CH_ROT_A weighted */
    float rb;      /* -0x1248: CH_ROT_B weighted */
    float rc;      /* -8: from the hook's 0x27 answer */
    float lean;    /* -0x214: from rc */
} Pose;

#define ROT(fp, i)       ((fp) - FR_ROT + 16u * (i))
#define POS(fp, i)       ((fp) - FR_POS + 12u * (i))
#define ROTW(fp, i)      ((fp) - FR_ROTW + 4u * (i))
#define POSW(fp, i)      ((fp) - FR_POSW + 4u * (i))

static void copy16(uint32_t dst, uint32_t src)
{
    for (uint32_t k = 0; k < 16; k += 4)
        W32(dst + k, R32(src + k));
}

/* A channel with a turning state (its CH_STATE +0x14 and its +0x24 not negative), not reached on
   the lobby route [verified: the call tree has no hook calls 0x26/0x27]: asks the hook (0x26, 0x27)
   how far the character should turn, works out the angle still to go against the animation's
   time (a whole turn = its length), eases CHAR_BLEND and moves the channel's time by it. */
static void pose_turning(CPU *c, Pose *P, uint32_t ch)
{
    uint32_t self = P->self, fp = P->fp, hook;
    f80 n = (f80)(int32_t)(int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - LX(F_ONE);
    float len = (float)n;                        /* the last frame */
    float per = (float)(n * LX(F_INV360));       /* frames per degree */
    float a = 0;
    W32(fp - FR_HOOKARG, 0);
    if ((hook = R32(self + CHAR_HOOK))) {
        a = (float)(game_call_st0(c, hook, 3, 0x26, self, fp - FR_HOOKARG) + V(a));
        P->rc = (float)(game_call_st0(c, R32(self + CHAR_HOOK), 3, 0x27, self, fp - FR_HOOKARG) * LX(ch + CH_WEIGHT));
    }
    game_callm(c, MAT3_CTOR, fp - FR_M3, 0);
    game_callm(c, MAT3_FROM_QUAT, fp - FR_M3, 1, ROT(fp, 0));
    (void)(float)game_callm_st0(c, MAT3_YAW, fp - FR_M3, 0);    /* stored, never read */
    float ang = a;
    ang = (float)(V(ang) + V(P->turn));
    ang = (float)(V(ang) - (V(P->rb) * LX(F_ONE) + V(P->ra) * LX(F_ONE)));
    while (V(ang) < LX(F_M180))
        ang = (float)(V(ang) + LX(F_360));
    while (V(ang) >= LX(F_180))
        ang = (float)(V(ang) - LX(F_360));
    float d = (float)(V(ang) - LX(ch + CH_TIME) / V(per));
    while (V(d) <= LX(F_M180))
        d = (float)(V(d) + LX(F_360));
    uint32_t s = 0x3f333333u;                    /* 0.7 */
    if (V(ang) > LX(F_70) || V(ang) < LX(F_M70))
        s = 0x3e4ccccdu;                         /* 0.2 */
    if (LX(ch + CH_TIME) <= LX(F_ZERO)) {
        W32(self + CHAR_BLEND, 0x3f800000u);
        s = 0x3f800000u;
    }
    float sf;
    memcpy(&sf, &s, 4);
    if (V(sf) > LX(self + CHAR_BLEND))
        SF(self + CHAR_BLEND, (V(sf) - LX(self + CHAR_BLEND)) * LX(F_TENTH) + LX(self + CHAR_BLEND));
    else
        SF(self + CHAR_BLEND, (V(sf) - LX(self + CHAR_BLEND)) * LX(F_HALF) + LX(self + CHAR_BLEND));
    d = (float)(V(d) * LX(self + CHAR_BLEND));
    float v = (float)(game_callm_st0(c, CHAR_SPEED, self, 0) * LX(G_ANIM_SPEED));
    if (V(v) > LX(F_ONE))
        v = 1.0f;
    SF(ch + CH_TIME, V(d) * V(per) * V(v) + LX(ch + CH_TIME));
    while (LX(ch + CH_TIME) >= V(len))
        SF(ch + CH_TIME, LX(ch + CH_TIME) - V(len));
    while (LX(ch + CH_TIME) <= LX(F_ZERO))
        SF(ch + CH_TIME, V(len) + LX(ch + CH_TIME));
    W32(ch + CH_TIME_COPY, R32(ch + CH_TIME));
}

/* One rotation track's quaternion q (FR_Q) into bone slot's blend: the first contribution taken,
   later ones slerped in by weight; in the second pass (channels flagged 0x100) multiplied on top
   instead (a bone with nothing yet starts from its bind rotation, weight 100). */
static void pose_rot_into(CPU *c, Pose *P, int pass, uint32_t slot, uint32_t brec, float tw)
{
    uint32_t fp = P->fp;
    if (pass) {
        if (LX(ROTW(fp, slot)) == LX(F_ZERO)) {
            W32(ROTW(fp, slot), 0x42c80000u);
            copy16(ROT(fp, slot), brec + BONE_BIND_QUAT);
        }
        uint32_t r = game_callm(c, QUAT_MUL, fp - FR_Q, 2, fp - FR_QOUT, ROT(fp, slot));
        copy16(ROT(fp, slot), r);
    } else if (LX(ROTW(fp, slot)) == LX(F_ZERO)) {
        copy16(ROT(fp, slot), fp - FR_Q);
        WF32(ROTW(fp, slot), tw);
    } else {
        float nw = (float)(V(tw) + LX(ROTW(fp, slot)));
        game_callm(c, SLERP, ROT(fp, slot), 3, ROT(fp, slot), fp - FR_Q, FU((float)(V(tw) / V(nw))));
        WF32(ROTW(fp, slot), nw);
    }
}

/* One track of a channel (k: its index; trk its entry; win its key window slot). Returns 0 when
   the channel's remaining tracks are to be skipped (a bone past the skeleton with no ranges). */
static int pose_track(CPU *c, Pose *P, int pass, uint32_t ch, uint32_t trk, uint32_t win,
                      uint32_t wbits, float t, int32_t *j, int32_t *off, int32_t *limit)
{
    uint32_t self = P->self, fp = P->fp;
    while ((int16_t)R16(trk + 4) >= *limit) {
        if (!P->mask)
            return 0;
        ++*j;
        *off = (int32_t)R32(self + CHAR_RANGE_OFF + 4 * (uint32_t)*j);
        uint32_t rg = R32(R32(self + CHAR_SKELETON) + SKEL_RANGES);
        *limit = (int16_t)R16(rg + 4 * (uint32_t)*j) + (int16_t)R16(R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)*j + 2);
    }
    int32_t bone = (int16_t)R16(trk + 4);
    uint32_t slot = (uint32_t)(bone + *off);
    uint32_t brec = R32(self + CHAR_SKELETON) + 0x7c * (uint32_t)bone + 0x24;
    float tw;
    if (wbits == 0x3f800000u)
        memcpy(&tw, &(uint32_t){R32(trk)}, 4);
    else if (!(LX(trk) == LX(F_100))) {
        float w;
        memcpy(&w, &wbits, 4);
        tw = (float)(LX(F_100) / (LX(F_ONE) - V(w) * LX(trk) / (LX(trk) + LX(F_100))) - LX(F_100));
    } else {
        float w;
        memcpy(&w, &wbits, 4);
        tw = (float)(V(w) * LX(trk));
    }
    if (R32(win) == 0) {
        /* no keys: a rotation track blends in the bind rotation */
        if (R8(trk + 6) == 0) {
            if (LX(ROTW(fp, slot)) == LX(F_ZERO)) {
                copy16(ROT(fp, slot), brec + BONE_BIND_QUAT);
                WF32(ROTW(fp, slot), tw);
            } else {
                float nw = (float)(V(tw) + LX(ROTW(fp, slot)));
                copy16(fp - FR_QBIND, brec + BONE_BIND_QUAT);
                game_callm(c, SLERP, ROT(fp, slot), 3, ROT(fp, slot), fp - FR_QBIND, FU((float)(V(tw) / V(nw))));
                WF32(ROTW(fp, slot), nw);
            }
        }
        return 1;
    }
    if (R8(trk + 6) == 0) {
        /* a rotation */
        game_callm(c, QUAT_CTOR, fp - FR_Q, 0);
        if ((int32_t)R32(R32(win)) < 0)
            copy16(fp - FR_Q, R32(win) + PATH_POINT(0));
        else
            game_call(c, PATH_ROT, 3, R32(win), fp - FR_Q, FU(t));
        if (bone == 0) {
            if (!(LX(ch + CH_TURN_Z) == LX(F_ZERO))) {
                game_callm(c, MAT3_CTOR_QUAT, fp - FR_M3A, 1, fp - FR_Q);
                game_callm(c, MAT3_CTOR, fp - FR_M3B, 0);
                game_callm(c, MAT3_IDENTITY, fp - FR_M3B, 0);
                game_callm(c, MAT3_ROTATE, fp - FR_M3B, 3, 0, FU((float)-LX(ch + CH_TURN_Z)), 0);
                game_callm(c, MAT3_MUL, fp - FR_M3A, 1, fp - FR_M3B);
                game_callm(c, QUAT_FROM_MAT3, fp - FR_Q, 1, fp - FR_M3A);
            }
            P->turn = (float)(V(tw) * LX(R32(ch + CH_STATE) + 0xc) / LX(F_100) + V(P->turn));
        }
        pose_rot_into(c, P, pass, slot, brec, tw);
        return 1;
    }
    /* a position */
    game_callm(c, VEC3_CTOR, fp - FR_P, 0);
    if ((int32_t)R32(R32(win)) < 0)
        game_callm(c, VEC3_COPY, fp - FR_P, 1, R32(win) + PATH_POINT(0));
    else if (LX(POSW(fp, slot)) == LX(F_ZERO))
        game_call(c, PATH_SMOOTH, 3, R32(win), fp - FR_P, FU(t));
    else
        game_call(c, PATH_LINEAR, 3, R32(win), fp - FR_P, FU(t));
    if (bone == 0) {
        if (!((int16_t)R16(R32(ch + CH_ANIM) + ANIM_ROOT_FLAGS) < 0)) {
            W32(fp - FR_P, 0);
            W32(fp - FR_P + 8, 0);
        }
        if (!(LX(R32(ch + CH_ANIM) + ANIM_ROOT_Y) == LX(F_ZERO)))
            W32(fp - FR_P + 4, R32(R32(ch + CH_ANIM) + ANIM_ROOT_Y));
    }
    if (LX(POSW(fp, slot)) == LX(F_ZERO)) {
        W32(fp - FR_ONE, 0x3f800000u);
        uint32_t r = game_callm(c, VEC3_SCALE, fp - FR_P, 2, fp - FR_V3, fp - FR_ONE);
        game_callm(c, VEC3_COPY, POS(fp, slot), 1, r);
        WF32(POSW(fp, slot), tw);
    } else {
        float nw = (float)(V(tw) + LX(POSW(fp, slot)));
        SF(fp - FR_S1, LX(POSW(fp, slot)) / V(nw));
        game_callm(c, VEC3_SCALE_BY, POS(fp, slot), 1, fp - FR_S1);
        SF(fp - FR_S2, V(tw) / V(nw));
        game_callm(c, VEC3_SCALE_BY, fp - FR_P, 1, fp - FR_S2);
        game_callm(c, VEC3_ADD_TO, POS(fp, slot), 1, fp - FR_P);
        WF32(POSW(fp, slot), nw);
    }
    return 1;
}

/* One channel's tracks into the blend. */
static void pose_channel(CPU *c, Pose *P, int pass, uint32_t ch)
{
    uint32_t self = P->self, anim = R32(ch + CH_ANIM), tracks = R32(anim + ANIM_TRACKS);
    if (LX(R32(ch + CH_STATE) + 0x14) >= LX(F_ZERO) && LX(ch + 0x24) >= LX(F_ZERO))
        pose_turning(c, P, ch);
    if (!(LX(ch + CH_ROT_A) == LX(F_ZERO)))
        P->ra = (float)(LX(ch + CH_ROT_A) * LX(ch + CH_WEIGHT) + V(P->ra));
    if (!(LX(ch + CH_ROT_B) == LX(F_ZERO)))
        P->rb = (float)(LX(ch + CH_ROT_B) * LX(ch + CH_WEIGHT) + V(P->rb));
    float t;
    memcpy(&t, &(uint32_t){R32(ch + CH_TIME)}, 4);
    if (V(t) < LX(F_ZERO))
        t = 0;
    FPUSH(V(t));
    int32_t frame = (int32_t)game_call(c, CRT_FTOL, 0);
    if (frame > (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1) {
        frame = (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1;
        t = (float)(f80)frame;
    }
    uint32_t wbits = R32(ch + CH_WEIGHT);
    game_callm(c, CH_DECODE, ch, 0);
    int32_t j = -1, off = 0, limit = (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES);
    uint32_t trk = tracks + TRACK(0), win = ch + CH_WINDOW(0);
    for (int32_t k = 0; k < (int16_t)R16(tracks + TRACKS_COUNT); k++, trk += 8, win += 4)
        if (!pose_track(c, P, pass, ch, trk, win, wbits, t, &j, &off, &limit))
            break;
}

/* The adjustments of the head/neck/spine bones (CHAR_BONE_IDS) before a bone is placed on its
   parent: by halves of the channels' CH_ROT_A / CH_ROT_B and the hook's turn. Returns 1 when the
   bone is done (placed already). */
static int pose_bone_adjust(CPU *c, Pose *P, uint32_t i, uint32_t m, uint32_t parent)
{
    uint32_t ids = R32(P->self + CHAR_BONE_IDS);
    if ((int32_t)i > (int32_t)R32(ids + 0x3c) && (int32_t)i > (int32_t)R32(R32(P->self + CHAR_BONE_IDS) + 0x38))
        return 0;
    for (int pass = 0; pass < 2; pass++) {
        if ((int32_t)i != (int32_t)R32(R32(P->self + CHAR_BONE_IDS) + (pass ? 0x70 : 0x6c)))
            continue;
        if (!(V(P->rb) == LX(F_ZERO)))
            game_callm(c, MAT4_ROTATE, m, 3, 0, 0, FU((float)(V(P->rb) * LX(F_HALF))));
        if (!(V(P->rc) == LX(F_ZERO)))
            game_callm(c, MAT4_ROTATE, m, 3, 0, 0, FU((float)(V(P->rc) * LX(F_HALF))));
        game_callm(c, MAT4_MUL, m, 1, parent);
        if (!(V(P->ra) == LX(F_ZERO)))
            game_callm(c, MAT4_EULER, m, 3, 0, 0, FU((float)(V(P->ra) * LX(F_HALF))));
        return 1;
    }
    if ((int32_t)i == (int32_t)R32(R32(P->self + CHAR_BONE_IDS) + 0x3c)) {
        if (!(V(P->lean) == LX(F_ZERO)))
            game_callm(c, MAT4_ROTATE, m, 3, FU((float)(-V(P->lean) * LX(F_ONE))), 0, 0);
    } else if ((int32_t)i == (int32_t)R32(R32(P->self + CHAR_BONE_IDS) + 0x38)) {
        if ((R32(P->self + CHAR_FLAGS) & 0x280000u) && !(V(P->lean) == LX(F_ZERO)))
            game_callm(c, MAT4_ROTATE, m, 3, FU((float)(-V(P->lean) * LX(F_ONE))), 0, 0);
    } else if ((int32_t)i == (int32_t)R32(R32(P->self + CHAR_BONE_IDS) + 0x34)) {
        if (!(V(P->lean) == LX(F_ZERO)))
            game_callm(c, MAT4_ROTATE, m, 3, FU((float)(-V(P->lean) * LX(F_0_8))), 0, 0);
    }
    return 0;
}

/* one of the three lean values beyond +-0.01 (doubles in .rdata) */
static int lean_beyond(uint32_t a)
{
    return LX(a) > (f80)RF64(D_HUNDREDTH) || LX(a) < (f80)RF64(D_MHUNDREDTH);
}

/* The bone matrices from the blend: each bone's rotation (or its bind rotation when nothing
   animated it) and position (scaled, on the bind position; or the bind position) as a matrix,
   adjusted and placed on its parent's (bone 0: lifted to the model's height, leaned, turned,
   mirrored when nothing rotates it, and placed by the character's world matrix). Bones past the
   skeleton's count belong to the attached ranges (the mask's bits). */
static int32_t pose_matrices(CPU *c, Pose *P)
{
    uint32_t self = P->self, fp = P->fp, out = P->out;
    int32_t off2 = 0, j2 = -1, limit2 = (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES);
    uint32_t bit = 1;
    for (int32_t i = 0; i < (int32_t)P->nb; i++) {
        while (i >= limit2) {
            if (!P->mask)
                return i;
            j2++;
            uint32_t n = R16(R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)j2 + 2);
            if (P->mask & bit)
                limit2 += (int16_t)n;
            else
                off2 -= (int16_t)n;
            bit <<= 1;
        }
        int32_t src = i - off2;
        float pw = RF32(POSW(fp, (uint32_t)i)), rw = RF32(ROTW(fp, (uint32_t)i));
        uint32_t m = out + 0x40 * (uint32_t)i;
        uint32_t brec = R32(self + CHAR_SKELETON) + 0x7c * (uint32_t)src + 0x24;
        uint32_t p;
        if (!(V(pw) == LX(F_ZERO))) {
            p = POS(fp, (uint32_t)i);
            if (i != 0 && V(pw) < LX(F_100)) {
                SF(fp - FR_TMP, V(pw) * LX(F_HUNDREDTH) * LX(self + CHAR_SCALE));
                game_callm(c, VEC3_SCALE_BY, p, 1, fp - FR_TMP);
            } else
                game_callm(c, VEC3_SCALE_BY, p, 1, self + CHAR_SCALE);
            game_callm(c, VEC3_ADD_TO, p, 1, R32(R32(self + CHAR_MODEL) + MODEL_BIND_POS) + 12 * (uint32_t)src);
        } else
            p = R32(R32(self + CHAR_MODEL) + MODEL_BIND_POS) + 12 * (uint32_t)src;
        if (V(rw) > LX(F_ZERO)) {
            if (V(rw) < LX(F_100) && i > (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES)) {
                copy16(fp - FR_QC, brec + BONE_BIND_QUAT);
                game_callm(c, SLERP, ROT(fp, (uint32_t)i), 3, fp - FR_QC, ROT(fp, (uint32_t)i),
                           FU((float)(V(rw) * LX(F_HUNDREDTH))));
            }
            game_callm(c, MAT4_FROM_QUAT, m, 2, ROT(fp, (uint32_t)i), p);
        } else
            game_callm(c, MAT4_FROM_ROT, m, 2, brec + BONE_BIND_ROT, p);
        if (i == 0) {
            SF(out + 0x34, LX(R32(self + CHAR_SKELETON) + SKEL_ROOT_Y) * LX(R32(self + CHAR_MODEL) + MODEL_ROOT_Y) + LX(out + 0x34));
            SF(out + 0x34, LX(out + 0x34) - LX(R32(R32(self + CHAR_MODEL) + MODEL_BIND_POS) + 12 * (uint32_t)src + 4));
            if (lean_beyond(self + CHAR_LEAN) || lean_beyond(self + CHAR_LEAN + 4) || lean_beyond(self + CHAR_LEAN + 8)) {
                game_callm(c, MAT4_CTOR, fp - FR_R, 0);
                game_callm(c, MAT4_IDENTITY, fp - FR_R, 0);
                game_callm(c, MAT4_EULER, fp - FR_R, 3, 0, 0,
                           FU((float)(LX(self + CHAR_LEAN) * LX(F_3_5) + LX(self + CHAR_LEAN + 8))));
                game_callm(c, MAT4_MUL, out, 1, fp - FR_R);
                game_callm(c, MAT4_EULER, out, 3, FU((float)-LX(self + CHAR_LEAN + 4)), 0, 0);
            }
            if (!(V(P->turn) == LX(F_ZERO)))
                game_callm(c, MAT4_TURN, out, 1, FU(P->turn));
            if (V(rw) == LX(F_ZERO))
                for (uint32_t k = 0; k < 0x40; k += 8)
                    SF(out + k, LX(out + k) * LX(F_MINUS1));
            game_callm(c, MAT4_MUL, out, 1, self + CHAR_WORLD);
            continue;
        }
        int32_t par = (int16_t)R16(brec + BONE_PARENT);
        if (par >= (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES))
            par += off2;
        uint32_t parent = out + 0x40 * (uint32_t)par;
        if (pose_bone_adjust(c, P, (uint32_t)i, m, parent))
            continue;
        game_callm(c, MAT4_MUL, m, 1, parent);
    }
    return (int32_t)P->nb > 0 ? (int32_t)P->nb : 0;
}

/* 0x6cf798 (this = character; int bones, int mask, mat4 *out, 0, 0) (ret 0x14): the character's
   pose at the current time of each of its playing animations: every channel's tracks (rotations
   and positions per bone, from the key windows decoded to the channel's frame) blended by weight
   into per-bone quaternions and positions (channels flagged 0x100 in a second pass, on top), the
   hook told (2), then each bone's matrix built and placed on its parent's into out (bones' world
   matrices, out[i] 0x40 apart; 0x6d0f2d turns them into skinning matrices). The original's last
   two arguments are only slots it reuses for the weight arrays. It probes its 0x13e8-byte frame
   with _chkstk (0x8aa8a0) first, a stack probe left out here. */
static int dc_6cf798(CPU *c)
{
    uint32_t e = c->esp;
    Pose P = {c->ecx, ARG(0), ARG(1), ARG(2), 0, 0, 0, 0, 0, 0};
    uint32_t F = dc_frame(c, FR_SIZE + 4);
    uint32_t fp = F + FR_SIZE, self = P.self;
    P.fp = fp;
    game_call(c, VEC_CTOR_ITER, 4, ROT(fp, 0), 16, 0x80, QUAT_CTOR_STUB);
    game_call(c, VEC_CTOR_ITER, 4, POS(fp, 0), 12, 0x80, VEC3_CTOR_STUB);
    for (int32_t i = 0; i < (int32_t)P.nb; i++) {
        W32(ROTW(fp, (uint32_t)i), 0);
        W32(POSW(fp, (uint32_t)i), 0);
    }
    for (int pass = 0; pass < 2; pass++) {
        int defer = 0;
        for (uint32_t ch = R32(self + CHAR_CHANNELS); ch; ch = R32(ch + CH_NEXT)) {
            if (!R32(ch + CH_ANIM) || !R32(R32(ch + CH_ANIM) + ANIM_TRACKS))
                continue;
            if (pass == 0) {
                if ((int16_t)R16(R32(ch + CH_ANIM) + ANIM_FLAGS) & 0x100) {
                    defer = 1;
                    continue;
                }
            } else if (!((int16_t)R16(R32(ch + CH_ANIM) + ANIM_FLAGS) & 0x100))
                continue;
            pose_channel(c, &P, pass, ch);
        }
        if (!defer)
            break;
    }
    if (R32(self + CHAR_HOOK)) {
        game_call(c, R32(self + CHAR_HOOK), 3, 2, self, ROT(fp, 0));
        FPOP();
    }
    if (V(P.rc) > LX(F_ZERO)) {
        P.lean = (float)(V(P.rc) * LX(F_0_65));
        P.rc = (float)(V(P.rc) * LX(F_0_35));
        if (V(P.rc) > LX(F_20)) {
            P.lean = (float)(V(P.rc) - LX(F_20) + V(P.lean));
            P.rc = 20.0f;
        }
    } else {
        P.lean = (float)(V(P.rc) * LX(F_HALF));
        P.rc = (float)(V(P.rc) * LX(F_HALF));
        if (V(P.rc) < LX(F_M30)) {
            P.lean = (float)(V(P.lean) - (V(P.rc) + LX(F_30)));
            P.rc = -30.0f;
        }
    }
    uint32_t eax = (uint32_t)pose_matrices(c, &P);    /* the original leaves its bone counter */
    dc_unframe(c, F, FR_SIZE + 4);
    return decomp_ret(c, e, eax, 5);
}
DECOMP(6cf798, dc_6cf798);
