/* QUATERNIONS AND ROTATION MATRICES for the characters' posing (skin.c): a quaternion is four
   floats (w, x, y, z); a 3x3 rotation is nine floats, rows of three; a 4x4 matrix is rows of four
   with the position in the last row (+0x30), points multiplied as rows (p * M). Methods of the
   game's C++ classes (thiscall). [verified: disassembly of each; the layouts from how they are
   read and written here and in skin.c]
   FLOAT ROUNDING, as in the translation (the A/B reference): the game's x87 control word is 0x047f
   (single precision, rounding down). Where the translator kept a function's x87 stack in C locals,
   GCC compiles a single operation on two floats stored as a float, (float)((f80)a + (f80)b), as an
   SSE addss, rounding to nearest; where it kept it in the CPU record (FPUSH/ST()), everything stays
   on the x87. So a rewrite uses plain casts (LF) for the first kind and loads through an asm barrier
   (LX, X) for the second; tools/fpround.py tells which a function is and compares the two
   builds' counts of SSE and x87 operations. */
#include "decomp.h"

#define SQRTF            0x41e8d0u  /* (float) -> sqrt on the x87 stack (mathlib.c) */
#define ACOSF            0x41f780u  /* (float) -> acos on the x87 stack */
#define SINF             0x62f3e0u  /* (float) -> sin on the x87 stack */
#define COSF             0x62f410u  /* (float) -> cos on the x87 stack */
#define QUAT_FROM_MAT4   0x6ba255u  /* (this = quat; mat4 *) */
#define MAT3_FROM_QUAT   0x6b9e68u  /* (this = mat3; quat *) */
#define MAT3_CTOR_QUAT   0x6bc800u  /* (this = mat3; quat *) -> this */
#define MAT4_FROM_ROT    0x6bb121u  /* (this = mat4; mat3 *, vec3 *) */

#define F_ZERO           0x8fa020u  /* .rdata 0.0f */
#define F_HALF           0x8fa038u  /* .rdata 0.5f */
#define F_ONE            0x8fa05cu  /* .rdata 1.0f */
#define F_TWO            0x8fa794u  /* .rdata 2.0f */
#define F_PI             0x8fafb4u  /* .rdata 3.1415927f */
#define F_180            0x8fa04cu  /* .rdata 180.0f */
#define F_SLERP_EPS      0x8fcc04u  /* .rdata 1e-5f: below this 1 - cos(angle), slerp blends linearly */

static inline f80 LF(uint32_t a) { return (f80)RF32(a); }
/* a value GCC cannot narrow to SSE: kept on the x87 stack through an empty asm */
static inline f80 X(f80 v) { __asm__("" : "+t"(v)); return v; }
static inline f80 LX(uint32_t a) { return X((f80)RF32(a)); }
static inline void SF(uint32_t a, f80 v) { WF32(a, (float)v); }
static inline uint32_t FU(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* 0x6b4540 (this = quat; mat4 *m): constructor, the rotation of m (QUAT_FROM_MAT4); returns this. */
static int dc_6b4540(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx;
    game_callm(c, QUAT_FROM_MAT4, t, 1, ARG(0));
    return decomp_ret(c, e, t, 1);
}
DECOMP(6b4540, dc_6b4540);

/* 0x6bc800 (this = mat3; quat *q): constructor, the rotation q (MAT3_FROM_QUAT); returns this. */
static int dc_6bc800(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx;
    game_callm(c, MAT3_FROM_QUAT, t, 1, ARG(0));
    return decomp_ret(c, e, t, 1);
}
DECOMP(6bc800, dc_6bc800);

/* 0x6ba255 (this = quat; mat4 *m): the quaternion of m's rotation (the usual trace method: from the
   trace when it is positive, else from the largest diagonal element); returns this. */
static int dc_6ba255(CPU *c)
{
    static const int next[3] = {1, 2, 0};
    uint32_t e = c->esp, q = c->ecx, m = ARG(0);
#define M(i, j) LX(m + 4 * (4 * (i) + (j)))
    float tr = (float)(M(0, 0) + M(1, 1) + M(2, 2));
    if (tr > 0) {
        f80 r = game_call_st0(c, SQRTF, 1, FU((float)(X((f80)tr) + LX(F_ONE))));
        float s = (float)r;
        SF(q, r / LX(F_TWO));
        s = (float)(LX(F_HALF) / X((f80)s));
        SF(q + 4, (M(2, 1) - M(1, 2)) * X((f80)s));
        SF(q + 8, (M(0, 2) - M(2, 0)) * X((f80)s));
        SF(q + 12, (M(1, 0) - M(0, 1)) * X((f80)s));
    } else {
        int i = 0;
        if (M(1, 1) > M(0, 0))
            i = 1;
        if (M(2, 2) > M(i, i))
            i = 2;
        int j = next[i], k = next[j];
        float v[3], w;
        f80 r = game_call_st0(c, SQRTF, 1, FU((float)(M(i, i) - (M(j, j) + M(k, k)) + LX(F_ONE))));
        float s = (float)r;
        v[i] = (float)(r * LX(F_HALF));
        if (s != 0)
            s = (float)(LX(F_HALF) / X((f80)s));
        w = (float)((M(k, j) - M(j, k)) * X((f80)s));
        v[j] = (float)((M(j, i) + M(i, j)) * X((f80)s));
        v[k] = (float)((M(k, i) + M(i, k)) * X((f80)s));
        WF32(q + 4, v[0]);
        WF32(q + 8, v[1]);
        WF32(q + 12, v[2]);
        WF32(q, w);
    }
#undef M
    return decomp_ret(c, e, q, 1);
}
DECOMP(6ba255, dc_6ba255);

/* 0x6b9e68 (this = mat3; quat *q): the rotation matrix of the unit quaternion q. */
static int dc_6b9e68(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, q = ARG(0);
    float x2 = (float)(LF(q + 4) + LF(q + 4));
    float y2 = (float)(LF(q + 8) + LF(q + 8));
    float z2 = (float)(LF(q + 12) + LF(q + 12));
    float xx = (float)((f80)x2 * LF(q + 4));
    float xy = (float)((f80)y2 * LF(q + 4));
    float xz = (float)((f80)z2 * LF(q + 4));
    float yy = (float)((f80)y2 * LF(q + 8));
    float yz = (float)((f80)z2 * LF(q + 8));
    float zz = (float)((f80)z2 * LF(q + 12));
    float wx = (float)((f80)x2 * LF(q));
    float wy = (float)((f80)y2 * LF(q));
    float wz = (float)((f80)z2 * LF(q));
    SF(m, LF(F_ONE) - ((f80)yy + (f80)zz));
    SF(m + 4, (f80)xy - (f80)wz);
    SF(m + 8, (f80)xz + (f80)wy);
    SF(m + 12, (f80)xy + (f80)wz);
    SF(m + 16, LF(F_ONE) - ((f80)xx + (f80)zz));
    SF(m + 20, (f80)yz - (f80)wx);
    SF(m + 24, (f80)xz - (f80)wy);
    SF(m + 28, (f80)yz + (f80)wx);
    SF(m + 32, LF(F_ONE) - ((f80)xx + (f80)yy));
    return decomp_ret(c, e, m, 1);
}
DECOMP(6b9e68, dc_6b9e68);

/* 0x6ba8ef (this = mat4): the identity (written from a local copy, first to last). eax ends as the
   loop's count, 16. */
static int dc_6ba8ef(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx;
    for (uint32_t i = 0; i < 16; i++)
        W32(m + 4 * i, i % 5 == 0 ? 0x3f800000u : 0);
    return decomp_ret(c, e, 16, 0);
}
DECOMP(6ba8ef, dc_6ba8ef);

/* 0x6bb121 (this = mat4; mat3 *r, vec3 *p): the matrix rotating by r and placing at p: r's rows in
   the first three rows, 0 in the fourth column, p in the last row and 1 in its corner (all copied
   as dwords); returns this. */
static int dc_6bb121(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, r = ARG(0), p = ARG(1);
    for (int32_t i = 0; i < 9; i++)
        W32(m + 4 * (4 * (uint32_t)(i / 3) + (uint32_t)(i % 3)), R32(r + 4 * (uint32_t)i));
    for (uint32_t i = 0; i < 3; i++)
        W32(m + 16 * i + 12, 0);
    for (uint32_t i = 0; i < 3; i++)
        W32(m + 0x30 + 4 * i, R32(p + 4 * i));
    W32(m + 0x3c, 0x3f800000u);
    return decomp_ret(c, e, m, 2);
}
DECOMP(6bb121, dc_6bb121);

/* 0x6bb1d8 (this = mat4; quat *q or 0, vec3 *p): the matrix rotating by q (none if 0) and placing
   at p; returns this. */
static int dc_6bb1d8(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, q = ARG(0), p = ARG(1);
    if (q) {
        uint32_t F = dc_frame(c, 0x24);    /* the 3x3 rotation */
        game_callm(c, MAT3_CTOR_QUAT, F, 1, q);
        game_callm(c, MAT4_FROM_ROT, m, 2, F, p);
        dc_unframe(c, F, 0x24);
    } else {
        for (uint32_t i = 0; i < 12; i++)
            W32(m + 4 * i, 0);
        W32(m, 0x3f800000u);
        W32(m + 0x14, 0x3f800000u);
        W32(m + 0x28, 0x3f800000u);
        W32(m + 0x3c, 0x3f800000u);
        for (uint32_t i = 0; i < 3; i++)
            W32(m + 0x30 + 4 * i, R32(p + 4 * i));
    }
    return decomp_ret(c, e, m, 2);
}
DECOMP(6bb1d8, dc_6bb1d8);

/* 0x6bb745 (this = mat4; mat4 *b): this = this * b for affine matrices (b's fourth column taken as
   0, 0, 0, 1; this's left as it is), row by row in place: each row's first two results kept in
   locals while the third is stored, then the two copied in; the last row adds b's position.
   Returns this. */
static int dc_6bb745(CPU *c)
{
    uint32_t e = c->esp, a = c->ecx, b = ARG(0);
    for (uint32_t r = 0; r < 4; r++) {
        uint32_t row = a + 16 * r;
        f80 x = LF(row) * LF(b) + LF(row + 4) * LF(b + 0x10) + LF(row + 8) * LF(b + 0x20);
        if (r == 3)
            x = x + LF(b + 0x30);
        float c0 = (float)x;
        f80 y = LF(row) * LF(b + 4) + LF(row + 4) * LF(b + 0x14) + LF(row + 8) * LF(b + 0x24);
        if (r == 3)
            y = y + LF(b + 0x34);
        float c1 = (float)y;
        f80 z = LF(row) * LF(b + 8) + LF(row + 4) * LF(b + 0x18) + LF(row + 8) * LF(b + 0x28);
        if (r == 3)
            z = z + LF(b + 0x38);
        SF(row + 8, z);
        WF32(row, c0);
        WF32(row + 4, c1);
    }
    return decomp_ret(c, e, a, 1);
}
DECOMP(6bb745, dc_6bb745);

/* 0x6ba742 (this = mat4; float ax, float ay, float az): rotates the matrix's first three rows by
   the angles in degrees, column by column (all four): about x (rows 1, 2), then y (rows 0, 2), then
   z (rows 0, 1). eax ends as the last column's pointer (this + 12). */
static int dc_6ba742(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx;
    f80 ax = LX(e + 4), ay = LX(e + 8), az = LX(e + 12);
    float sx = (float)game_call_st0(c, SINF, 1, FU((float)(ax * LX(F_PI) / LX(F_180))));
    float cx = (float)game_call_st0(c, COSF, 1, FU((float)(ax * LX(F_PI) / LX(F_180))));
    float sy = (float)game_call_st0(c, SINF, 1, FU((float)(ay * LX(F_PI) / LX(F_180))));
    float cy = (float)game_call_st0(c, COSF, 1, FU((float)(ay * LX(F_PI) / LX(F_180))));
    float sz = (float)game_call_st0(c, SINF, 1, FU((float)(az * LX(F_PI) / LX(F_180))));
    float cz = (float)game_call_st0(c, COSF, 1, FU((float)(az * LX(F_PI) / LX(F_180))));
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t p = m + 4 * i;
        float a = (float)(X((f80)cx) * LX(p + 0x10) + X((f80)sx) * LX(p + 0x20));
        float b = (float)(X((f80)cx) * LX(p + 0x20) - X((f80)sx) * LX(p + 0x10));
        WF32(p + 0x10, a);
        WF32(p + 0x20, b);
        a = (float)(X((f80)cy) * LX(p) - X((f80)sy) * LX(p + 0x20));
        b = (float)(X((f80)cy) * LX(p + 0x20) + X((f80)sy) * LX(p));
        WF32(p, a);
        WF32(p + 0x20, b);
        a = (float)(X((f80)cz) * LX(p) - X((f80)sz) * LX(p + 0x10));
        b = (float)(X((f80)cz) * LX(p + 0x10) + X((f80)sz) * LX(p));
        WF32(p, a);
        WF32(p + 0x10, b);
    }
    return decomp_ret(c, e, m + 12, 3);
}
DECOMP(6ba742, dc_6ba742);

/* 0x6bb9b9 (this = quat; quat *a, quat *b, float t): spherical interpolation from a to b by t,
   along the shorter way (b's weight negated when the dot product is negative); linear when the
   angle is tiny. The results written x, y, z, then w; eax ends as a. */
static int dc_6bb9b9(CPU *c)
{
    uint32_t e = c->esp, o = c->ecx, a = ARG(0), b = ARG(1);
    uint32_t tbits = ARG(2);
    f80 t = LX(e + 12);
    float d = (float)(LX(a + 4) * LX(b + 4) + LX(a + 8) * LX(b + 8) + LX(a + 12) * LX(b + 12) + LX(a) * LX(b));
    int neg = 0;
    float s0, s1;
    if (d < 0) {
        d = (float)-X((f80)d);     /* fld; fchs; fstp */
        neg = 1;
    }
    if (LX(F_ONE) - X((f80)d) > LX(F_SLERP_EPS)) {
        float om = (float)game_call_st0(c, ACOSF, 1, FU(d));
        float so = (float)game_call_st0(c, SINF, 1, FU(om));
        s0 = (float)(game_call_st0(c, SINF, 1, FU((float)((LX(F_ONE) - t) * X((f80)om)))) / X((f80)so));
        s1 = (float)(game_call_st0(c, SINF, 1, FU((float)(t * X((f80)om)))) / X((f80)so));
    } else {
        s0 = (float)(LX(F_ONE) - t);
        memcpy(&s1, &tbits, 4);
    }
    if (neg)
        s1 = (float)-X((f80)s1);
    SF(o + 4, X((f80)s0) * LX(a + 4) + X((f80)s1) * LX(b + 4));
    SF(o + 8, X((f80)s0) * LX(a + 8) + X((f80)s1) * LX(b + 8));
    SF(o + 12, X((f80)s0) * LX(a + 12) + X((f80)s1) * LX(b + 12));
    SF(o, X((f80)s0) * LX(a) + X((f80)s1) * LX(b));
    return decomp_ret(c, e, a, 3);
}
DECOMP(6bb9b9, dc_6bb9b9);
