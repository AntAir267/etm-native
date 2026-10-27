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

/* ---------------------------------------------------------------- 0x6f3ec5 */

#define LEVEL_PTR        0x97f720u  /* the level */
#define LEVEL_0x170      0x170u     /* lights flagged 0x400 need it set, 0x800 need it clear [what it is: unknown] */
#define LEVEL_DYN_ON     0x1bcu     /* the dynamic lights are on */
#define LEVEL_DYN        0x1c0u     /* the dynamic lights' list (next at +0x1a4) */
#define TABLE_N          0x3cu      /* a light table (the object's +0x1b4): its count */
#define TABLE_LIGHTS     0x60u      /*   and its lights, 0x1b8 bytes each */
#define LIGHT_SIZE       0x1b8u
/* a light (table entry or dynamic light) */
#define LT_FLAGS         0u         /* 1 on; 0xe its kind: 2 directional, 4 spot, 8 point; 0x10 the
                                       second list; 0x60 lights objects; 0x100 limited range */
#define LT_POS           4u
#define LT_RANGE_IN      0x1cu      /* full colour inside, fading to 0 at LT_RANGE (0x100 lights) */
#define LT_RANGE         0x20u
#define LT_COLOUR        0x24u      /* r, g, b bytes */
#define LT_CONE_IN       0x2cu      /* spot: cosines of the full-colour cone and the outer cone */
#define LT_CONE_OUT      0x30u
#define LT_DIR           0xf4u      /* directional and spot: the direction */
#define DYN_OWNER        0x1a0u     /* a dynamic light's table (or 0: any) */
#define DYN_NEXT         0x1a4u
/* the light set (records 0x40 apart, the object's +0x1cc): slot 0 the ambient colour,
   slots 1-3 the nearest lights, slots 4-5 the nearest second-list lights (record pointer only) */
#define LS_KIND          0u         /* slot 0: 3; lights: 1 | 0x10 directional, 8 spot, 4 point; 0 empty */
#define LS_POS           0x10u
#define LS_DIR           0x1cu      /* unit vector from the light to the object (or its direction) */
#define LS_COLOUR        0x28u      /* r, g, b (and slot 0's a) as floats 0..255 */
#define LS_LIGHT         0x38u      /* the light record */
#define NO_DIST          0x56b5e621u /* 1e14f: an empty nearest-list entry */

#define VEC_SUB          0x627f10u
#define LENGTH_SQ        0x628150u
#define LENGTH           0x6281c6u
#define DOT3             0x5e0b10u
#define FLOAT_SQRT       0x41e8d0u  /* (float) -> st0 [guess: its square root, from its use] */
#define K_ONE            0x8fa05cu  /* .rdata 1.0f */

typedef struct { uint32_t light, dist; } Near;

/* inserts a light into a nearest-first list of n when it is nearer than one there */
static int near_insert(Near *l, int32_t n, uint32_t light, float dsq)
{
    for (int32_t i = 0; i < n; i++)
        if (dsq < u2f(l[i].dist)) {                     /* fcomp, test ah, 5, jp: ordered less */
            for (int32_t j = n - 1; j > i; j--)
                l[j] = l[j - 1];
            l[i].light = light;
            l[i].dist = f2u(dsq);
            return 1;
        }
    return 0;
}

static float bytef(uint32_t a) { return (float)(int32_t)R8(a); }

/* 0x6f3ec5 (table, position, radius, set, most, kinds, ambient bytes or 0, no colours, no
   lights): an object's light set: the ambient colour, then the nearest (at most three, and at
   most "most" - 1) of the table's lights of the given kinds that are on and reach the position
   (within the radius plus the light's range, inside a spot's cone) and of the level's dynamic
   lights, each as a direction from the light and a colour faded by range and cone, and the two
   nearest second-list lights. Returns 1 plus the lights set. The search uses the first light
   slot's direction as its scratch vector, as the original. */
static int dc_6f3ec5(CPU *c)
{
    uint32_t e = c->esp, table = ARG(0), pos = ARG(1), radius = ARG(2), out = ARG(3);
    int32_t most = (int32_t)ARG(4);
    uint32_t kinds = ARG(5), amb = ARG(6), no_colour = ARG(7) & 0xff, no_lights = ARG(8) & 0xff;
    uint32_t level = R32(LEVEL_PTR), second = out + 0x100;
    if (amb) {
        for (uint32_t k = 0; k < 4; k++)
            WF32(out + LS_COLOUR + 4 * k, bytef(amb + k));
    } else {
        W32(out + LS_COLOUR + 8, 0);
        W32(out + LS_COLOUR + 4, 0);
        W32(out + LS_COLOUR, 0);
    }
    W32(out + LS_KIND, 3);
    W32(out + LS_LIGHT, 0);
    for (uint32_t s = 1; s < 6; s++)
        for (uint32_t k = LS_COLOUR; k <= LS_LIGHT; k += 4)
            W32(out + 0x40 * s + k, 0);
    out += 0x40;
    int32_t count = 1;
    if (!table)
        return decomp_ret(c, e, (uint32_t)count, 0);
    Near near3[3], near2[2];
    for (int i = 0; i < 3; i++)
        near3[i] = (Near){0, NO_DIST};
    for (int i = 0; i < 2; i++)
        near2[i] = (Near){0, NO_DIST};
    int found = 0;
    uint32_t L = R32(table + TABLE_LIGHTS);
    for (int32_t k = (int32_t)R32(table + TABLE_N); k > 0; k--, L += LIGHT_SIZE) {
        if (kinds && !(R32(L + LT_FLAGS) & kinds))
            continue;
        if (!(R32(L + LT_FLAGS) & 1))
            continue;
        if ((R32(L + LT_FLAGS) & 0x400) && R32(level + LEVEL_0x170) == 0)
            continue;
        if ((R32(L + LT_FLAGS) & 0x800) && R32(level + LEVEL_0x170) != 0)
            continue;
        game_call(c, VEC_SUB, 3, out + LS_DIR, pos, L + LT_POS);
        float dsq = f32(game_call_st0(c, LENGTH_SQ, 1, out + LS_DIR));
        if (R32(L + LT_FLAGS) & 0x100) {
            f80 r = (f80)u2f(radius) + RF32(L + LT_RANGE), r2 = (f80)u2f(radius) + RF32(L + LT_RANGE);
            if (!(r * r2 > dsq))                        /* and 0x4100: at most, or unordered */
                continue;
        }
        if (!no_lights && (R32(L + LT_FLAGS) & 0xe) == 4) {
            f80 s = game_call_st0(c, FLOAT_SQRT, 1, f2u(dsq));
            float inv = f32(RF32(K_ONE) / s);
            for (uint32_t i = 0; i < 12; i += 4)
                WF32(out + LS_DIR + i, f32((f80)inv * RF32(out + LS_DIR + i)));
            float dot = f32(game_call_st0(c, DOT3, 2, out + LS_DIR, L + LT_DIR));
            if (dot < RF32(L + LT_CONE_OUT))            /* test ah, 5, jp: ordered less */
                continue;
        }
        if ((R32(L + LT_FLAGS) & 0x60) && (R8(L + LT_COLOUR) || R8(L + LT_COLOUR + 1) || R8(L + LT_COLOUR + 2)))
            found |= near_insert(near3, 3, L, dsq);
        if (R32(L + LT_FLAGS) & 0x10)
            found |= near_insert(near2, 2, L, dsq);
    }
    if (R32(level + LEVEL_DYN_ON)) {
        for (uint32_t d = R32(level + LEVEL_DYN); d; d = R32(d + DYN_NEXT)) {
            if (R32(d + DYN_OWNER) && R32(d + DYN_OWNER) != table)
                continue;
            game_call(c, VEC_SUB, 3, out + LS_DIR, pos, d + LT_POS);
            float dsq = f32(game_call_st0(c, LENGTH_SQ, 1, out + LS_DIR));
            if (dsq == RF32(FLOAT_ZERO)) {
                W32(out + LS_DIR, 0);
                W32(out + LS_DIR + 4, 0x3f800000u);
                W32(out + LS_DIR + 8, 0);
                dsq = 1.0f;
            }
            f80 r = (f80)u2f(radius) + RF32(d + LT_RANGE), r2 = (f80)u2f(radius) + RF32(d + LT_RANGE);
            if (!(r * r2 > dsq))
                continue;
            found |= near_insert(near3, 3, d, dsq);
            if (R32(d + LT_FLAGS) & 0x10)
                found |= near_insert(near2, 2, d, dsq);
        }
    }
    if (!found)
        return decomp_ret(c, e, 1, 0);
    if (!no_lights) {
        for (int i = 0; i < 3; i++) {
            L = near3[i].light;
            if (L) {
                game_call(c, VEC_SUB, 3, out + LS_DIR, pos, L + LT_POS);
                W32(out + LS_LIGHT, L);
                float len = f32(game_call_st0(c, LENGTH, 1, out + LS_DIR));
                if (len == RF32(FLOAT_ZERO)) {
                    W32(out + LS_DIR, 0);
                    W32(out + LS_DIR + 4, 0x3f800000u);
                    W32(out + LS_DIR + 8, 0);
                    len = 1.0f;
                }
                W32(out + LS_KIND, 1);
                if (!no_colour) {
                    for (uint32_t k = 0; k < 12; k += 4)
                        W32(out + LS_POS + k, R32(L + LT_POS + k));
                    uint32_t kind = R32(L + LT_FLAGS) & 0xe;
                    if (kind == 2) {
                        W32(out + LS_KIND, R32(out + LS_KIND) | 0x10);
                        for (uint32_t k = 0; k < 12; k += 4)
                            W32(out + LS_DIR + k, R32(L + LT_DIR + k));
                        for (uint32_t k = 0; k < 3; k++)
                            WF32(out + LS_COLOUR + 4 * k, bytef(L + LT_COLOUR + k));
                    } else if (kind == 4) {
                        W32(out + LS_KIND, R32(out + LS_KIND) | 8);
                        float inv = f32(RF32(K_ONE) / len);
                        for (uint32_t k = 0; k < 12; k += 4)
                            WF32(out + LS_DIR + k, f32((f80)inv * RF32(out + LS_DIR + k)));
                        float dot = f32(game_call_st0(c, DOT3, 2, out + LS_DIR, L + LT_DIR));
                        if (dot > RF32(L + LT_CONE_IN)) {       /* and 0x4100 clear: ordered greater */
                            for (uint32_t k = 0; k < 3; k++)
                                WF32(out + LS_COLOUR + 4 * k, bytef(L + LT_COLOUR + k));
                        } else {
                            float t = f32(((f80)dot - RF32(L + LT_CONE_OUT))
                                              / ((f80)RF32(L + LT_CONE_IN) - RF32(L + LT_CONE_OUT)));
                            if (t < RF32(FLOAT_ZERO))
                                t = u2f(0);
                            for (uint32_t k = 0; k < 3; k++)
                                WF32(out + LS_COLOUR + 4 * k, f32((f80)bytef(L + LT_COLOUR + k) * t));
                        }
                    } else if (kind == 8) {
                        W32(out + LS_KIND, R32(out + LS_KIND) | 4);
                        float inv = f32(RF32(K_ONE) / len);
                        if (inv < RF32(FLOAT_ZERO))
                            inv = u2f(0);
                        for (uint32_t k = 0; k < 12; k += 4)
                            WF32(out + LS_DIR + k, f32((f80)inv * RF32(out + LS_DIR + k)));
                        for (uint32_t k = 0; k < 3; k++)
                            WF32(out + LS_COLOUR + 4 * k, bytef(L + LT_COLOUR + k));
                    }
                    if ((R32(L + LT_FLAGS) & 0x100) && len > RF32(L + LT_RANGE_IN)) {
                        f80 q = ((f80)len - RF32(L + LT_RANGE_IN)) / ((f80)RF32(L + LT_RANGE) - RF32(L + LT_RANGE_IN));
                        float t = f32(RF32(K_ONE) - q);
                        if (t < RF32(FLOAT_ZERO)) {
                            W32(out + LS_COLOUR, 0);
                            W32(out + LS_COLOUR + 4, 0);
                            W32(out + LS_COLOUR + 8, 0);
                        } else
                            for (uint32_t k = 0; k < 12; k += 4)
                                WF32(out + LS_COLOUR + k, f32((f80)t * RF32(out + LS_COLOUR + k)));
                    }
                }
                count++;
            } else {
                W32(out + LS_KIND, 0);
                W32(out + LS_DIR + 8, 0);
                W32(out + LS_DIR + 4, 0);
                W32(out + LS_DIR, 0);
                W32(out + LS_COLOUR + 8, 0);
                W32(out + LS_COLOUR + 4, 0);
                W32(out + LS_COLOUR, 0);
            }
            out += 0x40;
            if (!(count < most))
                break;
        }
    }
    for (int i = 0; i < 2; i++, second += 0x40)
        W32(second + LS_LIGHT, near2[i].light);
    return decomp_ret(c, e, (uint32_t)count, 0);
}
/* Not switched in yet: with it in (and 0x6f2732 off) the lights' directions on the lobby route's
   first SetLight were a bit off (f32 since; not re-checked alone). */
DECOMP_PENDING(6f3ec5, dc_6f3ec5);
