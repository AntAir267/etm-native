/* THE CHARACTERS' HOOK (0x6fb254): the function a character's +0x2fc points at, a switch over 95
   questions and notices (what: 1..95, the character, an argument) answering a float. The posing
   (pose.c, skin.c) calls it with 2 (the blended rotations are ready), 1 (the bone matrices are
   ready) and 3 (the skinning matrices are ready); the turning block of the pose asks 0x26 / 0x27.
   Those are rewritten here; the other questions are the game's (AI, state queries) and stay with
   the translation: the rewrite hands them back (the hook returns 0, so the translated function
   runs the whole call). [verified: the jump table at 0x700b68; the lobby frame's call tree has the
   hook called only from 0x6d0f2d and 0x6cf798]
   FOR INTERPOLATION: notice 1 places the things a character holds or carries (its actor's
   attachment object, else the props listed from the actor's +0x380) at its bones: they move with
   the pose, so they must be placed from the interpolated bone matrices too.
   Float maths on the x87 (the translation keeps its x87 stack in the CPU record): LX, V. */
#include "decomp.h"

static inline f80 X(f80 v) { __asm__("" : "+t"(v)); return v; }
static inline f80 LX(uint32_t a) { return X((f80)RF32(a)); }
static inline void SF(uint32_t a, f80 v) { WF32(a, (float)v); }

#define HOOK_TABLE       0x700b68u  /* the switch's jump table (what - 1, 0..0x5e) */
#define HOOK_DEFAULT     0x700b48u  /* its default: answers 0 */
#define HOOK_CHECK       0x6f9c04u  /* (character, line) (cdecl): empty [guess: a debug check] */

#define CHAR_ACTOR       0x164u     /* the game actor (+0x178 its controller, +0x1a4 an attachment
                                       object, +0x380 its first prop) */
#define CHAR_BONES_USED  0x2f0u
#define CHAR_SKELETON    0x38cu
#define ACTOR_CTRL       0x178u     /* +0x148, +0x14c: angles in 4096ths of a turn [guess] */
#define ACTOR_ATTACHED   0x1a4u     /* as renderpass.c's ACTOR_EXTRA */
#define ACTOR_PROPS      0x380u
#define PROP_WORLD       0x40u      /* 4x4: where the prop is (its position row +0x70) */
#define PROP_POS         0x70u
#define PROP_REL         0x80u      /* 4x4: the prop relative to its anchor (position row +0xb0) */
#define PROP_REL_POS     0xb0u
#define PROP_REL_QUAT    0xc0u      /* quat: the relative rotation it eases from */
#define PROP_NEXT        0xd0u
#define PROP_FLAGS       0xd8u      /* 4: anchored without a bone */
#define PROP_BONE        0x3a8u     /* the bone it is held by */
#define PROP_BONE2       0x3acu     /* a second bone it moves toward, or < 0 */
#define PROP_BLEND       0x3b0u     /* float: 0..1 toward the second bone / the anchor */
#define PROP_BLEND_PREV  0x3b4u     /* float: the blend as of last time (1: settled) */
#define PROP_ANCHOR      0x3bcu     /* mat4 *: the anchor's matrix, or 0 */
#define SKEL_BONE_PARENT(b) (0x88u + 0x7cu * (b))  /* short */

#define ATTACHED_PLACE   0x77b750u  /* (actor, bones) (cdecl): the attachment object at the bones */
#define ATTACHED_ROTS    0x77b8eau  /* (actor, rotations) (cdecl) */
#define MAT_POSITION     0x41f450u  /* (this = mat4) -> &its position row (mathlib.c) */
#define VEC3_COPY        0x41f100u
#define VEC3_SCALE_BY    0x6b4470u  /* (this = vec3; float *) */
#define VEC3_ADD_TO      0x5e0980u  /* (this = vec3; vec3 *) */
#define VEC3_SCALE       0x62f560u  /* (this = vec3; out, float *) -> out */
#define MAT4_COPY        0x41f400u
#define MAT4_CTOR        0x47f630u
#define MAT4_TIMES       0x6bb4fcu  /* (this = mat4; out, mat4 *b) -> out = this * b [guess] */
#define MAT4_REL         0x6bb288u  /* (this = mat4; out, mat4 *b) -> out [guess: this * b^-1] */
#define MAT4_FROM_QUAT   0x6bb1d8u  /* (this = mat4; quat *, vec3 *) (quat.c) */
#define QUAT_CTOR_MAT4   0x6b4540u  /* (this = quat; mat4 *) */
#define QUAT_FROM_MAT4   0x6ba255u
#define QUAT_IDENTITY    0x702c30u  /* (this = quat) [guess] */
#define SLERP            0x6bb9b9u  /* (this = quat; a, b, float t) */

#define F_ZERO           0x8fa020u
#define F_ONE            0x8fa05cu
#define F_TURN_4096      0x8fa0acu  /* .rdata 360/4096 */
#define D_ONE            0x8fa0d8u  /* .rdata 1.0 (double) */
#define D_ZERO           0x8faf18u  /* .rdata 0.0 (double) */

/* the original's frame (fp its ebp), for the locals the props' placing hands to callees */
#define FR_SIZE          0x8e8u
#define FR_A             0x410u
#define FR_B             0x450u
#define FR_Q1            0x470u
#define FR_Q2            0x460u
#define FR_M             0x4b0u
#define FR_W             0x4bcu
#define FR_C             0x500u
#define FR_K             0x504u
#define FR_Q4            0x514u
#define FR_Q3            0x524u
#define FR_T1            0x760u
#define FR_T2            0x7a0u
#define FR_S1            0x7a4u
#define FR_S2            0x7a8u
#define FR_V             0x7b4u
#define FR_T3            0x7f4u
#define FR_S             0x7f8u
#define FR_V2            0x804u
#define FR_T4            0x844u
#define FR_T5            0x884u

static void copy_mat(uint32_t dst, uint32_t src)    /* rep movsd, 16 dwords */
{
    for (uint32_t k = 0; k < 0x40; k += 4)
        W32(dst + k, R32(src + k));
}

/* a bone the character posed: up the parents until one is */
static int32_t posed_bone(uint32_t self, int32_t b)
{
    while (!((int32_t)R32(self + CHAR_BONES_USED) > b))
        b = (int16_t)R16(R32(self + CHAR_SKELETON) + SKEL_BONE_PARENT((uint32_t)b));
    return b;
}

/* One prop held by a bone (not flag 4): settled (blend as of last time 1): its matrix is the
   bone's times its relative one. Else, first (blend below 0) its relative matrix re-taken from
   where it is; then either moving toward its anchor (no second bone: position and rotation
   eased by the blend's progress since last time) or blended between its two bones by the blend. */
static void prop_place(CPU *c, uint32_t self, uint32_t buf, uint32_t fp, uint32_t prop, uint32_t anchor)
{
    int32_t b1 = posed_bone(self, (int32_t)R32(prop + PROP_BONE));
    if (LX(prop + PROP_BLEND_PREV) == (f80)RF64(D_ONE)) {
        game_callm(c, MAT4_COPY, prop + PROP_WORLD, 1, buf + 0x40 * (uint32_t)b1);
        uint32_t r = game_callm(c, MAT4_TIMES, prop + PROP_REL, 2, fp - FR_T5, prop + PROP_WORLD);
        game_callm(c, MAT4_COPY, prop + PROP_WORLD, 1, r);
        return;
    }
    if (LX(prop + PROP_BLEND) < (f80)RF64(D_ZERO)) {
        (void)posed_bone(self, (int32_t)R32(prop + PROP_BONE2));    /* (worked out, not used) */
        copy_mat(fp - FR_A, buf + 0x40 * (uint32_t)b1);
        copy_mat(fp - FR_B, buf + 0x40 * (uint32_t)b1);
        uint32_t r = game_callm(c, MAT4_TIMES, prop + PROP_REL, 2, fp - FR_T1, fp - FR_B);
        game_callm(c, MAT4_COPY, fp - FR_B, 1, r);
        r = game_callm(c, MAT4_REL, fp - FR_B, 2, fp - FR_T2, fp - FR_A);
        game_callm(c, MAT4_COPY, prop + PROP_REL, 1, r);
        game_callm(c, MAT4_COPY, prop + PROP_WORLD, 1, fp - FR_A);
        W32(prop + PROP_BLEND, 0x3f800000u);
    }
    if ((int32_t)R32(prop + PROP_BONE2) < 0) {
        /* toward the anchor: k, the blend's progress since last time, of what was left */
        uint32_t k = fp - FR_K;
        SF(k, (LX(prop + PROP_BLEND) - LX(prop + PROP_BLEND_PREV)) / ((f80)RF64(D_ONE) - LX(prop + PROP_BLEND_PREV)));
        W32(prop + PROP_BLEND_PREV, R32(prop + PROP_BLEND));
        copy_mat(fp - FR_C, buf + 0x40 * (uint32_t)b1);
        SF(fp - FR_S1, LX(F_ONE) - LX(k));
        game_callm(c, VEC3_SCALE_BY, prop + PROP_POS, 1, fp - FR_S1);
        uint32_t r = game_callm(c, MAT_POSITION, fp - FR_C, 0);
        game_callm(c, VEC3_SCALE_BY, r, 1, k);
        r = game_callm(c, MAT_POSITION, fp - FR_C, 0);
        game_callm(c, VEC3_ADD_TO, prop + PROP_POS, 1, r);
        game_callm(c, QUAT_CTOR_MAT4, fp - FR_Q1, 1, prop + PROP_WORLD);
        game_callm(c, QUAT_CTOR_MAT4, fp - FR_Q2, 1, fp - FR_C);
        game_callm(c, SLERP, fp - FR_Q2, 3, fp - FR_Q1, fp - FR_Q2, R32(k));
        game_callm(c, MAT4_FROM_QUAT, prop + PROP_WORLD, 2, fp - FR_Q2, prop + PROP_POS);
        SF(fp - FR_S2, LX(F_ONE) - LX(k));
        game_callm(c, VEC3_SCALE_BY, prop + PROP_REL_POS, 1, fp - FR_S2);
        r = game_callm(c, MAT_POSITION, anchor, 0);
        r = game_callm(c, VEC3_SCALE, r, 2, fp - FR_V, k);
        game_callm(c, VEC3_ADD_TO, prop + PROP_REL_POS, 1, r);
        game_callm(c, QUAT_FROM_MAT4, fp - FR_Q1, 1, prop + PROP_REL);
        game_callm(c, QUAT_FROM_MAT4, fp - FR_Q2, 1, anchor);
        game_callm(c, SLERP, fp - FR_Q2, 3, fp - FR_Q1, fp - FR_Q2, R32(k));
        game_callm(c, MAT4_FROM_QUAT, prop + PROP_REL, 2, fp - FR_Q2, prop + PROP_REL_POS);
        game_callm(c, MAT4_CTOR, fp - FR_M, 0);
        W32(fp - FR_W, R32(prop + PROP_REL_POS));
        W32(fp - FR_W + 4, R32(prop + PROP_REL_POS + 4));
        W32(fp - FR_W + 8, R32(prop + PROP_REL_POS + 8));
        game_callm(c, VEC3_SCALE_BY, fp - FR_W, 1, k);
        game_callm(c, QUAT_IDENTITY, fp - FR_Q1, 0);
        game_callm(c, SLERP, fp - FR_Q2, 3, fp - FR_Q1, fp - FR_Q2, R32(k));
        game_callm(c, MAT4_FROM_QUAT, fp - FR_M, 2, fp - FR_Q2, fp - FR_W);
        r = game_callm(c, MAT4_TIMES, fp - FR_M, 2, fp - FR_T3, prop + PROP_WORLD);
        game_callm(c, MAT4_COPY, prop + PROP_WORLD, 1, r);
        return;
    }
    /* between the two bones by the blend */
    int32_t b2 = posed_bone(self, (int32_t)R32(prop + PROP_BONE2));
    uint32_t r = game_callm(c, MAT_POSITION, buf + 0x40 * (uint32_t)b2, 0);
    game_callm(c, VEC3_COPY, prop + PROP_POS, 1, r);
    SF(fp - FR_S, LX(F_ONE) - LX(prop + PROP_BLEND));
    game_callm(c, VEC3_SCALE_BY, prop + PROP_POS, 1, fp - FR_S);
    r = game_callm(c, MAT_POSITION, buf + 0x40 * (uint32_t)b1, 0);
    r = game_callm(c, VEC3_SCALE, r, 2, fp - FR_V2, prop + PROP_BLEND);
    game_callm(c, VEC3_ADD_TO, prop + PROP_POS, 1, r);
    game_callm(c, QUAT_CTOR_MAT4, fp - FR_Q3, 1, buf + 0x40 * (uint32_t)b2);
    game_callm(c, QUAT_CTOR_MAT4, fp - FR_Q4, 1, buf + 0x40 * (uint32_t)b1);
    game_callm(c, SLERP, fp - FR_Q4, 3, fp - FR_Q3, fp - FR_Q4, R32(prop + PROP_BLEND));
    game_callm(c, MAT4_FROM_QUAT, prop + PROP_WORLD, 2, fp - FR_Q4, prop + PROP_POS);
    r = game_callm(c, MAT_POSITION, anchor, 0);
    game_callm(c, VEC3_COPY, prop + PROP_REL_POS, 1, r);
    game_callm(c, VEC3_SCALE_BY, prop + PROP_REL_POS, 1, prop + PROP_BLEND);
    game_callm(c, QUAT_FROM_MAT4, fp - FR_Q4, 1, anchor);
    game_callm(c, SLERP, fp - FR_Q4, 3, prop + PROP_REL_QUAT, fp - FR_Q4, R32(prop + PROP_BLEND));
    game_callm(c, MAT4_FROM_QUAT, prop + PROP_REL, 2, fp - FR_Q4, prop + PROP_REL_POS);
    r = game_callm(c, MAT4_TIMES, prop + PROP_REL, 2, fp - FR_T4, prop + PROP_WORLD);
    game_callm(c, MAT4_COPY, prop + PROP_WORLD, 1, r);
}

/* Notice 1 (the bone matrices are ready): the actor's attachment object placed at the bones
   (answers 1); else each of its props with an anchor (answers 0): flag 4 ones settle on their
   anchor's matrix once their blend is done; the others are placed at their bones (prop_place). */
static f80 hook_bones_ready(CPU *c, uint32_t self, uint32_t buf)
{
    uint32_t actor = R32(self + CHAR_ACTOR);
    if (R32(actor + ACTOR_ATTACHED)) {
        game_call(c, ATTACHED_PLACE, 2, R32(self + CHAR_ACTOR), buf);
        return LX(F_ONE);
    }
    uint32_t F = dc_frame(c, FR_SIZE), fp = F + FR_SIZE;
    for (uint32_t prop = R32(R32(self + CHAR_ACTOR) + ACTOR_PROPS); prop; prop = R32(prop + PROP_NEXT)) {
        uint32_t anchor = R32(prop + PROP_ANCHOR);
        if (!anchor)
            continue;
        if (R32(prop + PROP_FLAGS) & 4) {
            if (!(LX(prop + PROP_BLEND_PREV) == (f80)RF64(D_ONE)) && LX(prop + PROP_BLEND) <= LX(F_ZERO)) {
                W32(prop + PROP_BLEND_PREV, 0x3f800000u);
                copy_mat(prop + PROP_REL, R32(prop + PROP_ANCHOR));
            }
            continue;
        }
        prop_place(c, self, buf, fp, prop, anchor);
    }
    dc_unframe(c, F, FR_SIZE);
    return LX(F_ZERO);
}

/* 0x6fb254 (int what, character *ch, void *arg) -> float on the x87 stack (cdecl): see the
   header. Rewritten: 1 (hook_bones_ready), 2 (the rotations are ready: the actor's attachment
   object told, ATTACHED_ROTS), 0x26 / 0x27 (the actor's controller's two angles in degrees) and
   every default case (3 among them: answers 0). eax is left as the original's only for the default
   and the angle cases; after 1 and 2 the posing's callers overwrite it unread
   [verified: 0x6d0f2d at 0x6d1723 / 0x6d1b57, 0x6cf798 at 0x6d05f9]. */
static int dc_6fb254(CPU *c)
{
    uint32_t e = c->esp, what = ARG(0), self = ARG(1), arg = ARG(2);
    uint32_t sw = what - 1, target = sw > 0x5e ? HOOK_DEFAULT : R32(HOOK_TABLE + 4 * sw);
    if (!(what == 1 || what == 2 || what == 0x26 || what == 0x27 || target == HOOK_DEFAULT))
        return 0;    /* the game's questions: the translated function runs the call */
    uint32_t eax = c->eax;
    if (self) {
        game_call(c, HOOK_CHECK, 2, self, 0x3b7);
        eax = self;
    }
    if (target == HOOK_DEFAULT) {
        if (sw <= 0x5e)
            eax = sw;
        FPUSH(LX(F_ZERO));
        return decomp_ret(c, e, eax, 0);
    }
    f80 r;
    switch (what) {
    case 0x26:
    case 0x27:
        eax = R32(R32(self + CHAR_ACTOR) + ACTOR_CTRL);
        r = LX(eax + (what == 0x26 ? 0x148 : 0x14c)) * LX(F_TURN_4096);
        break;
    case 2:
        eax = R32(self + CHAR_ACTOR);
        if (R32(eax + ACTOR_ATTACHED))
            eax = game_call(c, ATTACHED_ROTS, 2, R32(self + CHAR_ACTOR), arg);
        r = LX(F_ZERO);
        break;
    default:    /* 1 */
        r = hook_bones_ready(c, self, arg);
        eax = c->eax;
        break;
    }
    FPUSH(r);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6fb254, dc_6fb254);

/* 0x6f9c04 (cdecl): returns at once (eax untouched). [verified: disassembly] */
static int dc_6f9c04(CPU *c)
{
    return decomp_ret(c, c->esp, c->eax, 0);
}
DECOMP(6f9c04, dc_6f9c04);

/* ------------------------------------------------------------------ a target search */

#define LEVEL_PTR        0x97f720u  /* the level: +0x50 its first actor, +0x3c the player [guess] */
#define LEVEL_ACTORS     0x50u
#define LEVEL_PLAYER     0x3cu
#define ACTOR_NEXT       0xd0u
#define ACTOR_FLAGS      0xd8u      /* 0x80000000: only the player may pick it [guess] */
#define ACTOR_AXES       0x48u      /* floats +0x48, +0x58, +0x68 scale the offset [guess: axes] */
#define ACTOR_EYE        0xfcu      /* vec3 */
#define ACTOR_CENTRE     0x144u     /* vec3 */
#define ACTOR_HEIGHT     0x150u     /* float */
#define ACTOR_TYPE       0x154u     /* +0 flags, +0x1c kind, +0x20 subkind, +0x54 flags */
#define ACTOR_TEAM       0x1b4u
#define ACTOR_STATE      0x3e8u     /* +0x60, +0x64 */
#define F_125            0x8fce84u
#define F_TWO            0x8fa794u
#define DIST             0x62817bu  /* (vec3 *, vec3 *) -> float on the x87 stack (cdecl) */
#define CAN_TARGET       0x6fa387u  /* (actor, actor) -> bool in al (cdecl) */
#define RAY              0x634defu  /* (from, to, ..., flags 0x80020, ...) (cdecl, 12) -> hit */

/* 0x6f9697 (actor *self, bool any, vec3 *offset or 0, float radius, float *dist or 0, bool
   no_people) -> actor or 0 (cdecl): the nearest actor of the self's team of the kinds 0x11, 0xd
   or 0x13 (filtered by subkind against the self's type flags and by their state) within radius of
   the self's eye point (moved by the offset along its axes; a target higher up is measured 125
   lower, or at the self's height), the scan ending at the first kind 0xf; the nearest distance
   goes to *dist; the nearest is dropped when a ray between the two centres (2 above their
   heights) hits. Game logic (the hook's questions), not drawing; not reached on the lobby route. */
static int dc_6f9697(CPU *c)
{
    uint32_t e = c->esp, self = ARG(0), any = ARG(1) & 0xff, ofs = ARG(2), outd = ARG(4);
    uint32_t nopeople = ARG(5) & 0xff;
    uint32_t level = R32(LEVEL_PTR), a = R32(level + LEVEL_ACTORS), found = 0;
    uint32_t F = dc_frame(c, 0x30), p = F, q = F + 12, s = F + 24, t = F + 36;
    uint32_t best = 0x4cbebc20u;    /* 1e8 */
    W32(p, R32(self + ACTOR_EYE));
    W32(p + 4, R32(self + ACTOR_EYE + 4));
    W32(p + 8, R32(self + ACTOR_EYE + 8));
    if (ofs) {
        SF(p, LX(self + ACTOR_AXES) * LX(ofs) + LX(p));
        SF(p + 4, LX(self + ACTOR_AXES + 0x10) * LX(ofs + 4) + LX(p + 4));
        SF(p + 8, LX(self + ACTOR_AXES + 0x20) * LX(ofs + 8) + LX(p + 8));
    }
    float r2 = (float)(LX(e + 16) * LX(e + 16));
    for (; a; a = R32(a + ACTOR_NEXT)) {
        uint32_t kind = R32(R32(a + ACTOR_TYPE) + 0x1c);
        if (kind == 0xf)
            break;
        if (R32(a + ACTOR_TEAM) != R32(self + ACTOR_TEAM))
            continue;
        if ((R32(a + ACTOR_FLAGS) & 0x80000000u) && self != R32(level + LEVEL_PLAYER))
            continue;
        if (nopeople && (kind == 0x11 || kind == 0xd))
            continue;
        if (!any && kind == 0x11)
            continue;
        if (!(kind == 0x11 || kind == 0xd || kind == 0x13))
            continue;
        uint32_t sub = R32(R32(a + ACTOR_TYPE) + 0x20);
        if (sub == 0x137 && !(R32(R32(self + ACTOR_TYPE)) & 0x3000))
            continue;
        if ((R32(R32(a + ACTOR_TYPE) + 0x20) == 0xa2 || R32(R32(a + ACTOR_TYPE) + 0x20) == 0x11f)
            && !(R32(R32(self + ACTOR_TYPE) + 0x54) & 1))
            continue;
        if (R32(R32(a + ACTOR_TYPE) + 0x20) == 0x20 && !(R32(R32(self + ACTOR_TYPE) + 0x54) & 2))
            continue;
        if ((R32(R32(a + ACTOR_TYPE) + 0x20) == 0x26 || R32(R32(a + ACTOR_TYPE) + 0x20) == 0xad)
            && !(R32(R32(self + ACTOR_TYPE) + 0x54) & 4))
            continue;
        if (kind == 0x11 || kind == 0xd) {
            if (R32(R32(a + ACTOR_STATE) + 0x60) == 0 && R32(R32(a + ACTOR_STATE) + 0x64) == 0)
                continue;
        } else if (R32(R32(a + ACTOR_STATE) + 0x60) == 0 && R32(R32(a + ACTOR_TYPE) + 0x20) != 0x11f)
            continue;
        W32(q, R32(a + ACTOR_EYE));
        W32(q + 4, R32(a + ACTOR_EYE + 4));
        W32(q + 8, R32(a + ACTOR_EYE + 8));
        if (LX(q + 4) > LX(p + 4)) {
            if (LX(p + 4) + LX(F_125) < LX(q + 4))
                SF(q + 4, LX(q + 4) - LX(F_125));
            else
                W32(q + 4, R32(p + 4));
        }
        float d = (float)game_call_st0(c, DIST, 2, p, q);
        float bf;
        memcpy(&bf, &best, 4);
        if ((f80)d < X((f80)r2) && X((f80)d) < X((f80)bf)
            && (game_call(c, CAN_TARGET, 2, self, a) & 0xff)) {
            memcpy(&best, &d, 4);
            found = a;
        }
    }
    if (outd)
        W32(outd, best);
    uint32_t eax = found;
    if (found) {
        W32(s, R32(self + ACTOR_CENTRE));
        W32(s + 4, R32(self + ACTOR_CENTRE + 4));
        W32(s + 8, R32(self + ACTOR_CENTRE + 8));
        W32(t, R32(found + ACTOR_CENTRE));
        W32(t + 4, R32(found + ACTOR_CENTRE + 4));
        W32(t + 8, R32(found + ACTOR_CENTRE + 8));
        SF(s + 4, LX(self + ACTOR_HEIGHT) + LX(F_TWO) + LX(s + 4));
        SF(t + 4, LX(found + ACTOR_HEIGHT) + LX(F_TWO) + LX(t + 4));
        uint32_t hit = game_call(c, RAY, 12, s, t, 0, 0, 0, 0, 0, 0x80020, 0, 0, 0, 0);
        if (R32(hit + 0xbc) & 1)
            eax = 0;
    }
    dc_unframe(c, F, 0x30);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(6f9697, dc_6f9697);
