/* THE LIGHTS AN OBJECT IS DRAWN WITH (object submission, DESIGN.md "The drawing code's rewrite"):
   0x6f3ec5 picks, for one object's position, the nearest lights of the level's light table and of
   its dynamic light list, and writes the object's light set (the records 0x7b1838 hands to
   SetLight / LightEnable), with the small vector helpers it uses. */
#include "objsubmit.h"

#define CRT_SQRT         0x8a9904u  /* the C runtime's sqrt (double) -> st0 */
#define FLOAT_ZERO       0x8fa020u  /* .rdata 0.0f */

/* 0x5e0b10 (a, b): the dot product of two 3-vectors, on the x87 stack. */
static int dc_5e0b10(CPU *c)
{
    uint32_t e = c->esp, a = ARG(0), b = ARG(1);
    f80 d = (f80)RF32(a) * RF32(b) + (f80)RF32(a + 4) * RF32(b + 4);
    d = d + (f80)RF32(a + 8) * RF32(b + 8);
    FPUSH(d);
    return decomp_ret(c, e, b, 0);
}
DECOMP(5e0b10, dc_5e0b10);

/* 0x627f10 (out, a, b): out = a - b (3-vectors; each component stored before the next is read,
   as the original). */
static int dc_627f10(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), a = ARG(1), b = ARG(2);
    WF32(out, (float)((f80)RF32(a) - RF32(b)));
    WF32(out + 4, (float)((f80)RF32(a + 4) - RF32(b + 4)));
    WF32(out + 8, (float)((f80)RF32(a + 8) - RF32(b + 8)));
    return decomp_ret(c, e, a, 0);
}
DECOMP(627f10, dc_627f10);

/* 0x628150 (v): v . v, the squared length of a 3-vector, on the x87 stack. */
static int dc_628150(CPU *c)
{
    uint32_t e = c->esp, v = ARG(0);
    f80 d = (f80)RF32(v) * RF32(v) + (f80)RF32(v + 4) * RF32(v + 4);
    d = d + (f80)RF32(v + 8) * RF32(v + 8);
    FPUSH(d);
    return decomp_ret(c, e, v, 0);
}
DECOMP(628150, dc_628150);

/* 0x6281c6 (v): the length of a 3-vector (sqrt of the squared length rounded to a float; 0 left
   as is), as a float on the x87 stack. */
static int dc_6281c6(CPU *c)
{
    uint32_t e = c->esp, v = ARG(0), eax = v;
    f80 d = (f80)RF32(v) * RF32(v) + (f80)RF32(v + 4) * RF32(v + 4);
    float l = (float)(d + (f80)RF32(v + 8) * RF32(v + 8));
    /* fcomp 0.0, test ah, 0x44, jnp: only "equal" (not unordered) skips the root */
    if (!(l == RF32(FLOAT_ZERO))) {
        double x = l;
        uint64_t b;
        memcpy(&b, &x, 8);
        l = (float)game_call_st0(c, CRT_SQRT, 2, (uint32_t)b, (uint32_t)(b >> 32));
        eax = c->eax;
    }
    FPUSH((f80)l);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6281c6, dc_6281c6);
