/* THE CHARACTERS' SKINNING MATRICES: before an animated character is drawn, 0x6f2732 (object
   submission) points 0x9920b0 at the character's bone buffer (its entity's +0x18c) and calls
   0x6d0f2d on its animated model (the entity's +0x188), which poses the skeleton (pose.c, 0x6cf798:
   bone world matrices), keeps some bone positions on the model for the game (feet, hands, the
   head...), then turns each bone's matrix into its skinning matrix (times the bone's inverse bind
   pose) in place, the buffer the drawing then hands to the vertex shader.
   [verified: tools/calltree.sh 6465c8 6800 (lobby frame): 0x6f2732 -> 0x6d0f2d 4 times, no Direct3D
   calls below it; disassembly of 0x6f2732 at 0x6f2c5e..0x6f2cd7 and 0x6f2d04..0x6f2d8a]
   FOR INTERPOLATION: the pose is evaluated here, at draw time, from each channel's time (+0x28)
   and weight (+0x1c) and the character's world matrix (+0x40): the state between two updates is
   those (see pose.c's header). But this draw-time code also WRITES game state, so it cannot simply
   run again on a frame between updates: the root bone's movement is taken off the character's
   position (root_motion: +0x70.. of its world matrix and +0x80.., against +0x3dc), bone positions
   are kept for the game (+0x3f4.., +0x4c4, +0x49c), the world matrix is copied to +0 and the hook
   places held props (charhook.c). Those belong to the update; an interpolated frame needs only the
   matrices (pose.c's 0x6cf798 and the skinning loop here) from blended inputs.
   Float maths as in quat.c: this function's translation keeps the x87 stack in the CPU record, so
   everything stays on the x87 (LX, V: loads through an asm barrier). */
#include "decomp.h"

static inline f80 X(f80 v) { __asm__("" : "+t"(v)); return v; }
static inline f80 LX(uint32_t a) { return X((f80)RF32(a)); }
#define V(f) X((f80)(f))

#define BONE_BUFFER      0x9920b0u  /* the bone matrices of the character being drawn (0x40 each) */
#define LOOK_BONES       0x991d5cu  /* int[8]: bones whose positions are kept when the model is a
                                       0x10 or 0xe kind [guess: from 0x6d0f2d only] */

/* the character (pose.c has the fields 0x6cf798 reads) */
#define CHAR_PREV_WORLD  0x00u      /* 4x4: the world matrix as it was last drawn [guess: copied
                                       from +0x40 after posing] */
#define CHAR_WORLD       0x40u      /* 4x4: where the character stands; its position row +0x70 */
#define CHAR_POS2        0x80u      /* vec3 moved with the world position [guess] */
#define CHAR_ENTITY      0x164u     /* the game entity (+0x154 -> +0x1c its kind) */
#define CHAR_BONE_IDS    0x168u     /* int[]: special bones from +0x30 (14 kept positions) */
#define CHAR_MODEL       0x2c0u     /* +0x20 each bone's bind position (vec3) */
#define CHAR_BONES_USED  0x2f0u     /* how many bones were posed this time */
#define CHAR_HOOK        0x2fcu     /* float (*)(int what, character *, void *) or 0 (0x6fb254) */
#define CHAR_FROZEN      0x308u     /* not 0: no posing (only the copies) [guess] */
#define CHAR_LOOK_KIND   0x340u     /* byte: keep LOOK_BONES' positions */
#define CHAR_CHANNELS    0x380u     /* first animation channel (+0x440 the next) */
#define CHAR_SKELETON    0x38cu
#define CHAR_RANGE_MASK  0x390u     /* which of the skeleton's extra bone ranges are in use */
#define CHAR_RANGE_OFF   0x394u     /* int per range: where its bones sit in the buffer */
#define CHAR_ROOT_BONE   0x3d8u     /* int: a bone whose movement moves the character, or < 0 */
#define CHAR_ROOT_POS    0x3dcu     /* vec3: that bone's position last time */
#define CHAR_POS2_COPY   0x3e8u     /* vec3: CHAR_POS2 as it was when posed */
#define CHAR_KEPT_POS    0x3f4u     /* vec3[14]: kept bone positions */
#define CHAR_PELVIS_ROT  0x49cu     /* 3x3: the rotation of bone ids[+0x30] */
#define CHAR_HEIGHT_POS  0x4c4u     /* vec3 (+4 is -20000 when there is none) */
#define CHAR_ROOT_Y      0x4d0u     /* byte: the root bone's height moves the character too */
#define CHAR_ROOT_NO_X   0x4d1u     /* byte: ... but not across (x in its own frame) */
#define CHAR_ROOT_NO_Z   0x4d2u     /* byte: ... but not forward (z) */
#define CHAR_ROOT_RESET  0x4d3u     /* byte: take the root bone's position afresh next time */
#define CH_ANIM          0xcu
#define CH_NEXT          0x440u
#define ANIM_TRACKS      0x1cu      /* +4 byte: ranges used, +5 byte: extra bones' level */
#define CH_HEIGHT_INFO   0x10u      /* (the first channel's) an object whose +0x18 short is a bone, or < 0 */
#define SKEL_BONES       4u
#define SKEL_BASE_BONES  8u         /* bones always posed */
#define SKEL_NRANGES     0xcu
#define SKEL_RANGES      0x10u      /* (short first bone, short count) per range */
#define SKEL_BONE(i)     (0x24u + 0x7cu * (i))  /* +0 the 3x3 inverse bind rotation */
#define BONE_PARENT      0x64u      /* short */
#define BONE_LOD         0x68u      /* signed byte: the level of detail it is kept down to */

#define POSE             0x6cf798u  /* (this = character; bones, mask, out, 0, 0) (pose.c) */
#define MAT_POSITION     0x41f450u  /* (this = mat4) -> &its position row (mathlib.c) */
#define VEC3_COPY        0x41f100u  /* (this = vec3; vec3 *) */
#define VEC3_SUB         0x62f4d0u  /* (this = vec3; out, vec3 *) out = this - v */
#define MAT4_COPY        0x41f400u  /* (this = mat4; mat4 *) */
#define QUAT_CTOR_MAT4   0x6b4540u  /* (this = quat; mat4 *) (quat.c) */
#define MAT3_FROM_QUAT   0x6b9e68u  /* (this = mat3; quat *) */
#define VEC3_TO_LOCAL    0x6b7b40u  /* (this = vec3; out, mat4 *) [guess: into the matrix's frame] */
#define VEC3_FROM_LOCAL  0x6d7e60u  /* (this = vec3; out, mat4 *) [guess: back out of it] */
#define CRT_MEMCPY       0x8a9ae0u

static uint32_t bone_pos(CPU *c, uint32_t buf, int32_t bone)
{
    return game_callm(c, MAT_POSITION, buf + 0x40 * (uint32_t)bone, 0);
}

/* How far the root bone moved since last time, taken off the character's position (so the
   animation's own movement moves the character), across and/or forward only if told to.
   [guess: root motion] */
static void root_motion(CPU *c, uint32_t self, uint32_t buf)
{
    uint32_t F = dc_frame(c, 0x48);
    uint32_t d = F, v = F + 12, t1 = F + 24, t2 = F + 36, r;    /* d at -0x78, v at -0x84 */
    r = bone_pos(c, buf, (int32_t)R32(self + CHAR_ROOT_BONE));
    W32(v, R32(r));
    W32(v + 4, R32(r + 4));
    W32(v + 8, R32(r + 8));
    game_callm(c, VEC3_SUB, v, 2, d, self + CHAR_ROOT_POS);
    if (R8(self + CHAR_ROOT_NO_X)) {
        r = game_callm(c, VEC3_TO_LOCAL, d, 2, t1, self + CHAR_WORLD);
        game_callm(c, VEC3_COPY, d, 1, r);
        W32(d, 0);
        r = game_callm(c, VEC3_FROM_LOCAL, d, 2, t2, self + CHAR_WORLD);
        game_callm(c, VEC3_COPY, d, 1, r);
    } else if (R8(self + CHAR_ROOT_NO_Z)) {
        r = game_callm(c, VEC3_TO_LOCAL, d, 2, t1, self + CHAR_WORLD);
        game_callm(c, VEC3_COPY, d, 1, r);
        W32(d + 8, 0);
        r = game_callm(c, VEC3_FROM_LOCAL, d, 2, t2, self + CHAR_WORLD);
        game_callm(c, VEC3_COPY, d, 1, r);
    }
    WF32(self + 0x70, (float)(LX(self + 0x70) - LX(d)));
    WF32(self + 0x78, (float)(LX(self + 0x78) - LX(d + 8)));
    WF32(self + 0x80, (float)(LX(self + 0x80) - LX(d)));
    WF32(self + 0x88, (float)(LX(self + 0x88) - LX(d + 8)));
    if (R8(self + CHAR_ROOT_Y)) {
        WF32(self + 0x74, (float)(LX(self + 0x74) - LX(d + 4)));
        WF32(self + 0x84, (float)(LX(self + 0x84) - LX(d + 4)));
    }
    dc_unframe(c, F, 0x48);
}

/* After posing: the positions the game reads back (the root's movement, a height bone, the kept
   bones, the pelvis rotation), then the hook (1). */
static void after_pose(CPU *c, uint32_t self, uint32_t buf)
{
    if ((int32_t)R32(self + CHAR_ROOT_BONE) < 0)
        W8(self + CHAR_ROOT_RESET, 1);
    else if (R8(self + CHAR_ROOT_RESET) == 0)
        root_motion(c, self, buf);
    else {
        W8(self + CHAR_ROOT_RESET, 0);
        uint32_t r = bone_pos(c, buf, (int32_t)R32(self + CHAR_ROOT_BONE));
        game_callm(c, VEC3_COPY, self + CHAR_ROOT_POS, 1, r);
    }
    uint32_t ch = R32(self + CHAR_CHANNELS);
    if (ch && R32(ch + CH_HEIGHT_INFO) && (int16_t)R16(R32(R32(self + CHAR_CHANNELS) + CH_HEIGHT_INFO) + 0x18) >= 0) {
        uint32_t r = bone_pos(c, buf, (int16_t)R16(R32(R32(self + CHAR_CHANNELS) + CH_HEIGHT_INFO) + 0x18));
        game_callm(c, VEC3_COPY, self + CHAR_HEIGHT_POS, 1, r);
    } else
        W32(self + CHAR_HEIGHT_POS + 4, 0xc69c4000u);     /* -20000 */
    if (R8(self + CHAR_LOOK_KIND) && (R32(R32(R32(self + CHAR_ENTITY) + 0x154) + 0x1c) == 0x10
                                      || R32(R32(R32(self + CHAR_ENTITY) + 0x154) + 0x1c) == 0xe)) {
        for (uint32_t i = 0; i < 8; i++) {
            int32_t b = (int32_t)R32(LOOK_BONES + 4 * i);
            if (b > (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES)) {
                /* an extra bone: where its range sits in the buffer */
                for (int32_t k = 0; k < (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_NRANGES); k++) {
                    uint32_t rg = R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)k;
                    if ((int16_t)R16(rg) > b)
                        continue;
                    if ((int16_t)R16(rg) + (int16_t)R16(rg + 2) <= b)
                        continue;
                    b += (int32_t)R32(self + CHAR_RANGE_OFF + 4 * (uint32_t)k);
                    break;
                }
            }
            uint32_t r = bone_pos(c, buf, b);
            game_callm(c, VEC3_COPY, self + CHAR_KEPT_POS + 12 * i, 1, r);
        }
    } else {
        for (uint32_t i = 0; i < 14; i++) {
            uint32_t r = bone_pos(c, buf, (int32_t)R32(R32(self + CHAR_BONE_IDS) + 0x30 + 4 * i));
            game_callm(c, VEC3_COPY, self + CHAR_KEPT_POS + 12 * i, 1, r);
        }
    }
    uint32_t F = dc_frame(c, 16);    /* the quaternion (-0xd4) */
    game_callm(c, QUAT_CTOR_MAT4, F, 1, buf + 0x40 * R32(R32(self + CHAR_BONE_IDS) + 0x30));
    game_callm(c, MAT3_FROM_QUAT, self + CHAR_PELVIS_ROT, 1, F);
    dc_unframe(c, F, 16);
    game_callm(c, VEC3_COPY, self + CHAR_POS2_COPY, 1, self + CHAR_POS2);
    game_callm(c, MAT4_COPY, self + CHAR_PREV_WORLD, 1, self + CHAR_WORLD);
    if (R32(self + CHAR_HOOK)) {
        game_call(c, R32(self + CHAR_HOOK), 3, 1, self, buf);
        FPOP();
    }
}

/* 0x6d0f2d (this = character; bool full, int lod) (ret 8): the character's skinning matrices in
   BONE_BUFFER. Unless frozen: which bones to pose (full: the base bones plus the extra ranges the
   playing animations use, CHAR_RANGE_OFF placing each range; else the base bones down to the lod
   (0x6f2732 passes 100) plus the animations' extra level), the pose (0x6cf798), after_pose. Then
   each posed bone's matrix times its inverse bind rotation, less the bind position so rotated
   (the skinning matrix); bones not posed copy their parent's; the hook (3). eax: the character,
   or the hook's. */
static int dc_6d0f2d(CPU *c)
{
    uint32_t e = c->esp, self = c->ecx, full = ARG(0) & 0xff;
    int32_t lod = (int32_t)ARG(1);
    if (!R32(self + CHAR_SKELETON))
        return decomp_ret(c, e, self, 2);
    uint32_t buf = R32(BONE_BUFFER);
    if (R32(self + CHAR_FROZEN) == 0) {
        int32_t n;
        W32(self + CHAR_RANGE_MASK, 0);
        if (full) {
            n = (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES);
            for (uint32_t ch = R32(self + CHAR_CHANNELS); ch; ch = R32(ch + CH_NEXT))
                if (R32(ch + CH_ANIM) && R32(R32(ch + CH_ANIM) + ANIM_TRACKS))
                    W32(self + CHAR_RANGE_MASK, R32(self + CHAR_RANGE_MASK) | R8(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + 4));
            if (R32(self + CHAR_RANGE_MASK) == 0) {
                full = 0;
                for (int32_t i = 0; i < (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_NRANGES); i++)
                    W32(self + CHAR_RANGE_OFF + 4 * (uint32_t)i, 0);
            } else {
                uint32_t bit = 1;
                int32_t i;
                for (i = 0; i < (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_NRANGES); i++) {
                    uint32_t rg = R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)i;
                    W32(self + CHAR_RANGE_OFF + 4 * (uint32_t)i, (uint32_t)(n - (int16_t)R16(rg)));
                    if (R32(self + CHAR_RANGE_MASK) & bit)
                        n += (int16_t)R16(R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)i + 2);
                    bit <<= 1;
                }
                /* (one past the last range: the previous entry, as the original writes it) */
                W32(self + CHAR_RANGE_OFF + 4 * (uint32_t)i, R32(self + CHAR_RANGE_MASK + 4 * (uint32_t)i));
            }
        } else {
            for (int32_t i = 0; i < (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_NRANGES); i++)
                W32(self + CHAR_RANGE_OFF + 4 * (uint32_t)i, 0);
            int32_t extra = 0;
            for (uint32_t ch = R32(self + CHAR_CHANNELS); ch; ch = R32(ch + CH_NEXT))
                if (R32(ch + CH_ANIM) && (int32_t)R8(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + 5) > extra)
                    extra = (int32_t)R8(R32(R32(ch + CH_ANIM) + ANIM_TRACKS) + 5);
            n = (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BASE_BONES) + extra;
            if (extra == 0)
                while (lod < (int8_t)R8(R32(self + CHAR_SKELETON) + SKEL_BONE((uint32_t)(n - 1)) + BONE_LOD))
                    n--;
        }
        W32(self + CHAR_BONES_USED, (uint32_t)n);
        game_callm(c, POSE, self, 5, (uint32_t)n, R32(self + CHAR_RANGE_MASK), buf, 0, 0);
        after_pose(c, self, buf);
    } else
        W32(self + CHAR_BONES_USED, 0);

    /* the skinning matrices of the posed bones */
    int32_t off = 0, j = -1, limit = (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES);
    uint32_t bit = 1;
    for (int32_t i = 0; i < (int32_t)R32(self + CHAR_BONES_USED); i++) {
        int stop = 0;
        while (i >= limit) {
            if (!full) {
                stop = 1;
                break;
            }
            j++;
            uint32_t cnt = R16(R32(R32(self + CHAR_SKELETON) + SKEL_RANGES) + 4 * (uint32_t)j + 2);
            if (R32(self + CHAR_RANGE_MASK) & bit)
                limit += (int16_t)cnt;
            else
                off -= (int16_t)cnt;
            bit <<= 1;
        }
        if (stop)
            break;
        int32_t src = i - off;
        uint32_t m = buf + 0x40 * (uint32_t)i;
        uint32_t b = R32(self + CHAR_SKELETON) + SKEL_BONE((uint32_t)src);
        float r[9];
        for (uint32_t a = 0; a < 3; a++)
            for (uint32_t k = 0; k < 3; k++)
                r[3 * a + k] = (float)(LX(m + 4 * k) * LX(b + 4 * a) + LX(m + 0x10 + 4 * k) * LX(b + 0xc + 4 * a)
                                       + LX(m + 0x20 + 4 * k) * LX(b + 0x18 + 4 * a));
        for (uint32_t a = 0; a < 3; a++)
            for (uint32_t k = 0; k < 3; k++)
                WF32(m + 4 * (k + 4 * a), r[3 * a + k]);
        uint32_t p = R32(R32(self + CHAR_MODEL) + 0x20) + 12 * (uint32_t)src;
        for (uint32_t k = 0; k < 3; k++)
            WF32(m + 0x30 + 4 * k, (float)(LX(m + 0x30 + 4 * k)
                                           - (V(r[k]) * LX(p) + V(r[3 + k]) * LX(p + 4) + V(r[6 + k]) * LX(p + 8))));
    }

    /* bones not posed: their parent's matrix */
    for (int32_t i = (int32_t)R32(self + CHAR_BONES_USED); i < (int32_t)R32(R32(self + CHAR_SKELETON) + SKEL_BONES); i++)
        game_call(c, CRT_MEMCPY, 3, buf + 0x40 * (uint32_t)i,
                  buf + 0x40 * (uint32_t)(int32_t)(int16_t)R16(R32(self + CHAR_SKELETON) + SKEL_BONE((uint32_t)i) + BONE_PARENT), 0x40);
    uint32_t eax = self;
    if (R32(self + CHAR_HOOK)) {
        eax = game_call(c, R32(self + CHAR_HOOK), 3, 3, self, buf);
        FPOP();
    }
    return decomp_ret(c, e, eax, 2);
}
DECOMP(6d0f2d, dc_6d0f2d);
