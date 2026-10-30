/* THE SMALL MATHS CLASSES the drawing code shares: a 3-float vector (x, y, z), a 4x4 matrix of
   floats (rows of four, the position in the last row at +0x30), a quaternion (w, x, y, z) and
   the float wrappers round the C library's sqrt, sin, cos and acos. Methods of the game's C++
   classes (thiscall: ecx the object), compiled without optimisation, so each one is a call; they
   run very often (renderpass.c and the characters' posing, skin.c, call them).
   [verified: disassembly of each; what the classes are: from their use here and in skin.c]
   The C library routines themselves (sqrt 0x8a9904, acos 0x8a9a24/0x8a9a2d, memcpy 0x8a9ae0,
   _ftol 0x8a9ef4, _chkstk 0x8aa8a0, sin 0x8aa8e4, cos 0x8aa994 and their helpers 0x8af415 and
   0x8af445) stay with the translation. */
#include "decomp.h"

#define CRT_SQRT         0x8a9904u  /* double sqrt(double) (MSVC CRT; result on the x87 stack) */
#define CRT_ACOS         0x8a9a24u  /* double acos(double) */
#define CRT_SIN          0x8aa8e4u  /* double sin(double) */
#define CRT_COS          0x8aa994u  /* double cos(double) */
#define VEC3_SET         0x41f0c0u  /* (this = vec3; float *x, float *y, float *z) */

#define F_ONE            0x8fa05cu  /* .rdata 1.0f */

/* A float in memory loaded as the x87 does (fld dword), and an x87 value stored as a float (fstp
   dword). Plain casts, as the translation's own C has them, ON PURPOSE: GCC compiles a single x87
   operation on two floats whose result is only stored as a float, (float)((f80)a + (f80)b), as an
   SSE addss, rounding to nearest, where the x87 rounds as the game's control word says (0x047f:
   single precision, rounding down). The translation (the A/B reference) is compiled that way, so a
   rewrite written the same way rounds the same; an asm barrier keeping it on the x87 differed by
   an ulp (A/B memory difference at frame 2290; 1 / 1071.3 rounded down vs to nearest). */
static inline f80 LF(uint32_t a) { return (f80)RF32(a); }
static inline void SF(uint32_t a, f80 v) { WF32(a, (float)v); }

/* a C library function of one double, in the original's frame (later code reads what is left on the
   stack; the return address differs in memory otherwise, tests/ab_levels.sh on Lower Wacker and the
   freeway): the float argument widened (fld dword; fstp qword), the result left on the x87 stack and
   kept in a local as the original keeps it */
static int crt_double(CPU *c, uint32_t e, uint32_t fn, uint32_t ret)
{
    uint32_t fp = e - 4, ebp = c->ebp;
    double d = (double)LF(e + 4);
    uint64_t b;
    memcpy(&b, &d, 8);
    W32(fp, ebp);
    W32(fp - 4, c->ecx);
    c->ebp = fp;
    c->esp = fp - 4;
    game_call_at(c, ret, fn, 2, (uint32_t)b, (uint32_t)(b >> 32));
    WF32(fp - 4, (float)ST(0));
    c->ebp = ebp;
    return decomp_ret(c, e, c->eax, 0);
}

/* 0x41e8d0 (float x) -> sqrt(x) on the x87 stack (cdecl). */
static int dc_41e8d0(CPU *c) { return crt_double(c, c->esp, CRT_SQRT, 0x41e8e2u); }
DECOMP(41e8d0, dc_41e8d0);

/* 0x41f780 (float x) -> acos(x) on the x87 stack (cdecl). */
static int dc_41f780(CPU *c) { return crt_double(c, c->esp, CRT_ACOS, 0x41f792u); }
DECOMP(41f780, dc_41f780);

/* 0x62f3e0 (float x) -> sin(x) on the x87 stack (cdecl). */
static int dc_62f3e0(CPU *c) { return crt_double(c, c->esp, CRT_SIN, 0x62f3f2u); }
DECOMP(62f3e0, dc_62f3e0);

/* 0x62f410 (float x) -> cos(x) on the x87 stack (cdecl). */
static int dc_62f410(CPU *c) { return crt_double(c, c->esp, CRT_COS, 0x62f422u); }
DECOMP(62f410, dc_62f410);

/* Constructors that leave the object as it is and return it: 0x41e940 (vec3), 0x47f630 (4x4
   matrix), 0x6b42a0 (quaternion), 0x790330 and 0x7b99e0 (classes not known [guess: the parts of
   0x7b99b0's object]). */
static int dc_this(CPU *c)
{
    return decomp_ret(c, c->esp, c->ecx, 0);
}
DECOMP(41e940, dc_this);
DECOMP(47f630, dc_this);
DECOMP(6b42a0, dc_this);
DECOMP(790330, dc_this);
DECOMP(7b99e0, dc_this);

#define CTOR_7B99E0      0x7b99e0u
#define CTOR_790330      0x790330u

/* 0x7b99b0: constructor of an object made of a 0x7b99e0 part at +0 and a 0x790330 part at +0x10
   (both empty constructors); returns this. */
static int dc_7b99b0(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx;
    game_callm(c, CTOR_7B99E0, t, 0);
    game_callm(c, CTOR_790330, t + 0x10, 0);
    return decomp_ret(c, e, t, 0);
}
DECOMP(7b99b0, dc_7b99b0);

/* 0x41f0c0 (this = vec3; float *x, float *y, float *z): this = (*x, *y, *z) (the floats copied as
   dwords); returns this. */
static int dc_41f0c0(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx;
    W32(t, R32(ARG(0)));
    W32(t + 4, R32(ARG(1)));
    W32(t + 8, R32(ARG(2)));
    return decomp_ret(c, e, t, 3);
}
DECOMP(41f0c0, dc_41f0c0);

/* 0x41f100 (this = vec3; vec3 *v): this = *v (copied as dwords); returns this. */
static int dc_41f100(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, v = ARG(0);
    W32(t, R32(v));
    W32(t + 4, R32(v + 4));
    W32(t + 8, R32(v + 8));
    return decomp_ret(c, e, t, 1);
}
DECOMP(41f100, dc_41f100);

/* 0x41f400 (this = 4x4 matrix; matrix *m): this = *m (16 dwords, first to last); returns this. */
static int dc_41f400(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, m = ARG(0);
    for (uint32_t i = 0; i < 16; i++)
        W32(t + 4 * i, R32(m + 4 * i));
    return decomp_ret(c, e, t, 1);
}
DECOMP(41f400, dc_41f400);

/* 0x41f450 (this = 4x4 matrix) -> the position (the last row's vec3, this + 0x30). */
static int dc_41f450(CPU *c)
{
    return decomp_ret(c, c->esp, c->ecx + 0x30, 0);
}
DECOMP(41f450, dc_41f450);

/* 0x5e0980 (this = vec3; vec3 *v): this += *v; returns this. */
static int dc_5e0980(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, v = ARG(0);
    SF(t, LF(t) + LF(v));
    SF(t + 4, LF(t + 4) + LF(v + 4));
    SF(t + 8, LF(t + 8) + LF(v + 8));
    return decomp_ret(c, e, t, 1);
}
DECOMP(5e0980, dc_5e0980);

/* 0x6b4470 (this = vec3; float *s): this *= *s; returns this. */
static int dc_6b4470(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, s = ARG(0);
    SF(t, LF(t) * LF(s));
    SF(t + 4, LF(t + 4) * LF(s));
    SF(t + 8, LF(t + 8) * LF(s));
    return decomp_ret(c, e, t, 1);
}
DECOMP(6b4470, dc_6b4470);

/* The vec3 operators that return a new vector (this = left operand; vec3 *out, right operand):
   the three components computed z, y, x into locals and handed to VEC3_SET for out; return out.
   The locals are the original's frame order (x at -0xc, y at -8, z at -4). */
static uint32_t vec3_out(CPU *c, uint32_t out, float x, float y, float z)
{
    uint32_t F = dc_frame(c, 12);
    WF32(F + 8, z);
    WF32(F + 4, y);
    WF32(F, x);
    game_callm(c, VEC3_SET, out, 3, F, F + 4, F + 8);
    dc_unframe(c, F, 12);
    return out;
}

/* 0x62f4d0 (this = vec3; vec3 *out, vec3 *v): *out = this - *v; returns out. */
static int dc_62f4d0(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, out = ARG(0), v = ARG(1);
    float z = (float)(LF(t + 8) - LF(v + 8)), y = (float)(LF(t + 4) - LF(v + 4)), x = (float)(LF(t) - LF(v));
    return decomp_ret(c, e, vec3_out(c, out, x, y, z), 2);
}
DECOMP(62f4d0, dc_62f4d0);

/* 0x62f850 (this = vec3; vec3 *out, vec3 *v): *out = this + *v; returns out. */
static int dc_62f850(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, out = ARG(0), v = ARG(1);
    float z = (float)(LF(t + 8) + LF(v + 8)), y = (float)(LF(t + 4) + LF(v + 4)), x = (float)(LF(t) + LF(v));
    return decomp_ret(c, e, vec3_out(c, out, x, y, z), 2);
}
DECOMP(62f850, dc_62f850);

/* 0x62f560 (this = vec3; vec3 *out, float *s): *out = this * *s; returns out. */
static int dc_62f560(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, out = ARG(0), s = ARG(1);
    float z = (float)(LF(t + 8) * LF(s)), y = (float)(LF(t + 4) * LF(s)), x = (float)(LF(t) * LF(s));
    return decomp_ret(c, e, vec3_out(c, out, x, y, z), 2);
}
DECOMP(62f560, dc_62f560);

/* 0x62f790 (this = vec3; vec3 *out, float s): *out = this * (1 / s), the reciprocal rounded to a
   float first (it is stored over the argument); returns out. */
static int dc_62f790(CPU *c)
{
    uint32_t e = c->esp, t = c->ecx, out = ARG(0);
    f80 k = (float)(LF(F_ONE) / LF(e + 8));
    float z = (float)(k * LF(t + 8)), y = (float)(k * LF(t + 4)), x = (float)(k * LF(t));
    return decomp_ret(c, e, vec3_out(c, out, x, y, z), 2);
}
DECOMP(62f790, dc_62f790);

/* 0x5fe5c0 (void *array, size, count, void (*ctor)(this)): the compiler's vector constructor
   iterator: ctor on each of count elements size bytes apart (stdcall). eax ends as the count run
   down to -1 (signed), ecx as the last element's pointer plus size. */
static int dc_5fe5c0(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), size = ARG(1), fn = ARG(3);
    int32_t n = (int32_t)ARG(2);
    int ran = 0;
    while (--n >= 0) {
        game_callm(c, fn, p, 0);
        p += size;
        ran = 1;
    }
    if (ran)
        c->ecx = p;
    return decomp_ret(c, e, (uint32_t)n, 4);
}
DECOMP(5fe5c0, dc_5fe5c0);
