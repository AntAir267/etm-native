/* THE CHARACTERS' ANIMATION, evaluated when they are drawn: the pose of each bone at the current
   time of each playing animation, blended (0x6cf798 and its helpers). See skin.c for where it is
   called from. [verified: call tree of the lobby frame 6800 (tools/calltree.sh 6465c8): 0x6f2732
   -> 0x6d0f2d -> 0x6cf798 -> these; disassembly of each]
   FLOAT ROUNDING as in quat.c: plain casts (LF) where the translation keeps the x87 stack in C
   locals (GCC narrows single float operations to SSE there), the asm barrier (LX, X) where it keeps
   it in the CPU record (work/x87mode.py). */
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

/* A PATH OF FOUR KEYS (0x6be059, 0x6be1e9): four integer frame numbers at +0, +4, +8, +0xc and four
   vec3 points 16 bytes apart from +0x10, evaluated at a time t (frames) in the first segment while
   t < key 1, in the last while key 2 < t, else in the middle one. [guess: a character's movement
   path within an animation (root motion); from the layout] */
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

/* 0x6be1e9 (path *p, vec3 *out, float t) (cdecl): the point at t on a smooth curve through the
   path: four weights w0..w3 of the points from the position u within the segment, reshaped by a
   cubic when the neighbouring segments' lengths differ (r, the ratio, below 1), then
   out = w0 p0 + w1 p1 + w2 p2 + w3 p3 (x, y, z). eax ends as p. [guess: a quadratic B-spline
   blend; the weights sum to 1] */
static int dc_6be1e9(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), out = ARG(1);
    f80 t = LF(e + 12);
    float w0, w1, w2, w3;
    if (LI(p + 4) > t) {
        /* the first segment */
        float u = (float)((t - LI(p)) / (f80)(int32_t)(R32(p + 4) - R32(p)));
        float one = 1.0f;
        float r = (float)((f80)(int32_t)(R32(p + 4) - R32(p)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        if ((f80)r < LF(F_ONE)) {
            f80 a = ((f80)r + (f80)one - LF(F_TWO)) * (f80)u * (f80)u * (f80)u;
            f80 b = (f80)r - (f80)one;
            f80 cc = ((f80)r + (f80)one - LF(F_TWO)) * LF(F_THREE);
            f80 dd = (b - cc) / LF(F_TWO) * (f80)u * (f80)u;
            u = (float)(a + dd + (f80)one * (f80)u);
        }
        float h = (float)((f80)u / LF(F_TWO));
        f80 v = LF(F_ONE) - (f80)h;
        float g = (float)v;
        w0 = (float)(v * (f80)g - (f80)g * (f80)h);
        w1 = (float)(LF(F_FOUR) * (f80)g * (f80)h);
        w2 = (float)((f80)h * (f80)h - (f80)g * (f80)h);
        w3 = 0;
    } else if (LI(p + 8) < t) {
        /* the last segment */
        float u = (float)((t - LI(p + 8)) / (f80)(int32_t)(R32(p + 12) - R32(p + 8)));
        float r = (float)((f80)(int32_t)(R32(p + 12) - R32(p + 8)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        float one = 1.0f;
        if ((f80)r < LF(F_ONE)) {
            f80 a = ((f80)one + (f80)r - LF(F_TWO)) * (f80)u * (f80)u * (f80)u;
            f80 b = (f80)one - (f80)r;
            f80 cc = ((f80)one + (f80)r - LF(F_TWO)) * LF(F_THREE);
            f80 dd = (b - cc) / LF(F_TWO) * (f80)u * (f80)u;
            u = (float)(a + dd + (f80)r * (f80)u);
        }
        if ((f80)u > LF(F_ONE))
            u = 1.0f;
        float h = (float)((f80)u / LF(F_TWO) + LF(F_HALF));
        float g = (float)(LF(F_ONE) - (f80)h);
        w0 = 0;
        w1 = (float)((f80)g * (f80)g - (f80)g * (f80)h);
        w2 = (float)(LF(F_FOUR) * (f80)g * (f80)h);
        w3 = (float)((f80)h * (f80)h - (f80)g * (f80)h);
    } else {
        /* the middle segment: the halves of two blends */
        float u = (float)((t - LI(p + 4)) / (f80)(int32_t)(R32(p + 8) - R32(p + 4)));
        float ra = (float)((f80)(int32_t)(R32(p + 8) - R32(p + 4)) / (f80)(int32_t)(R32(p + 4) - R32(p)));
        float rb = (float)((f80)(int32_t)(R32(p + 8) - R32(p + 4)) / (f80)(int32_t)(R32(p + 12) - R32(p + 8)));
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
        float m = u;
        f80 v = LF(F_ONE) - (f80)m;
        float n = (float)v;
        float h = (float)(v / LF(F_TWO));
        float g = (float)(LF(F_ONE) - (f80)h);
        w0 = (float)(((f80)h * (f80)h - (f80)h * (f80)g) * (f80)n);
        w1 = (float)(LF(F_FOUR) * (f80)h * (f80)g * (f80)n);
        w2 = (float)(((f80)g * (f80)g - (f80)h * (f80)g) * (f80)n);
        h = (float)((f80)h + LF(F_HALF));
        g = (float)(LF(F_ONE) - (f80)h);
        w1 = (float)(((f80)h * (f80)h - (f80)h * (f80)g) * (f80)m + (f80)w1);
        w2 = (float)(LF(F_FOUR) * (f80)h * (f80)g * (f80)m + (f80)w2);
        w3 = (float)(((f80)g * (f80)g - (f80)h * (f80)g) * (f80)m);
    }
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
    if ((int32_t)bone >= (int32_t)R32(skel + SKEL_BONES))
        fmt >>= 4;
    else if ((int8_t)R8(skel + SKEL_BONE(bone) + BONE_LOD) > 0x28)
        fmt >>= 2;
    if ((fmt & 0xc) == 0xc) {
        uint32_t v = R32(src), slot = v & 3;
        float a = (float)game_call_st0(c, DEC10, 1, (uint32_t)((int32_t)v >> 2) & 0x3ffu);
        float b = (float)game_call_st0(c, DEC10, 1, (uint32_t)((int32_t)v >> 12) & 0x3ffu);
        float d = (float)game_call_st0(c, DEC10, 1, (uint32_t)((int32_t)v >> 22) & 0x3ffu);
        float w = quat_rest(c, a, b, d);
        quat_place(out, slot, a, b, d, w);
        return decomp_ret(c, e, 4, 0);
    }
    if ((fmt & 0xc) == 8 || (fmt & 0xc) == 0) {
        uint32_t dec = (fmt & 0xc) == 8 ? DEC16_A : DEC16_B;
        uint32_t s0 = R16(src), s1 = R16(src + 2), s2 = R16(src + 4);
        uint32_t neg = s2 & 0x8000u, slot = ((s0 & 0x8000u) >> 14) + ((s1 & 0x8000u) >> 15);
        float a, b, d;
        if (prev) {
            a = (float)game_call_st0(c, dec, 1, s0);
            b = (float)game_call_st0(c, dec, 1, s1);
            d = (float)game_call_st0(c, dec, 1, s2);
            uint32_t o[3];    /* prev's three components other than the slot */
            for (uint32_t k = 0, n = 0; k < 4; k++)
                if (k != slot)
                    o[n++] = 4 * k;
            a = (float)(V(a) + LX(prev + o[0]));
            b = (float)(V(b) + LX(prev + o[1]));
            d = (float)(V(d) + LX(prev + o[2]));
        } else {
            a = (float)(game_call_st0(c, dec, 1, s0) * LX(F_HALF));
            b = (float)(game_call_st0(c, dec, 1, s1) * LX(F_HALF));
            d = (float)(game_call_st0(c, dec, 1, s2) * LX(F_HALF));
        }
        float w = quat_rest(c, a, b, d);
        if (neg)
            w = (float)-V(w);
        quat_place(out, slot, a, b, d, w);
        return decomp_ret(c, e, 6, 0);
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
    float w = quat_rest(c, a, b, d);
    quat_place(out, 3 - bits, a, b, d, w);    /* bits 0: w last; 1: third; 2: second; 3: first */
    return decomp_ret(c, e, 12, 0);
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
    uint32_t e = c->esp, ch = c->ecx, eax = ch;
    if (!R32(ch + CH_ANIM))
        return decomp_ret(c, e, eax, 0);
    int32_t fmt = (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FORMAT);
    FPUSH(LX(ch + CH_TIME));
    int32_t frame = (int32_t)game_call(c, CRT_FTOL, 0);
    if (frame > (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1)
        frame = (int16_t)R16(R32(ch + CH_ANIM) + ANIM_FRAMES) - 1;
    eax = R32(ch + CH_DECODED) - 1;
    if (frame < (int32_t)eax)
        eax = game_callm(c, CH_REWIND, ch, 0);
    while (frame >= (int32_t)R32(ch + CH_DECODED)) {
        eax = ch;
        if (R8(R32(ch + CH_STREAM)) == 0xff)
            break;
        int32_t n = (int32_t)R8(R32(ch + CH_STREAM));
        W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
        for (; n > 0; n--) {
            uint32_t track = R8(R32(ch + CH_STREAM));
            W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            uint32_t delta = R8(R32(ch + CH_STREAM));
            W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            if (delta & 0x80) {
                delta = (delta & 0x7f) << 8;
                delta += R8(R32(ch + CH_STREAM));
                W32(ch + CH_STREAM, R32(ch + CH_STREAM) + 1);
            }
            uint32_t w = R32(ch + CH_WINDOW(track));
            W32(w, R32(w + 4));
            W32(w + 4, R32(w + 8));
            W32(w + 8, R32(w + 12));
            W32(w + 12, R32(w + 12) + delta);
            game_call(c, CRT_MEMCPY, 3, w + 0x10, w + 0x20, 0x10);
            game_call(c, CRT_MEMCPY, 3, w + 0x20, w + 0x30, 0x10);
            game_call(c, CRT_MEMCPY, 3, w + 0x30, w + 0x40, 0x10);
            uint32_t len;
            if (R8(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + TRACK_IS_POS(track)))
                len = game_call(c, POS_KEY, 4, w + 0x40, R32(ch + CH_STREAM), w + 0x40, (uint32_t)fmt);
            else
                len = game_call(c, ROT_KEY, 6, w + 0x40, R32(ch + CH_STREAM), w + 0x40, (uint32_t)fmt,
                                (uint32_t)(int32_t)(int16_t)R16(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + TRACK_BONE(track)),
                                R32(R32(ch + CH_OWNER) + CHAR_SKELETON));
            W32(ch + CH_STREAM, len + R32(ch + CH_STREAM));
        }
        W32(ch + CH_DECODED, R32(ch + CH_DECODED) + 1);
        eax = ch;
    }
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6c1606, dc_6c1606);
