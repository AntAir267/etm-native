/* THE DIRECTION ARROW (DESIGN.md "The drawing code's rewrite"): 0x5fef62, which the view's scene
   (0x646c33) calls for each view: in some player modes a model (the view's type 314) is placed in
   front of the camera, turned towards a target's position, faded from red to white as it points
   away from the camera, and drawn as an object (SVC +0xf8) [guess: an arrow to the objective;
   what it is: from the code only]. Its object record is a static one (0x97ca80). On the lobby
   route the player's mode keeps it from being drawn [verified: tools/calltree.sh 6465c8 6800:
   no calls under it].

   What moves between updates: the camera (CAM_VIEW, CAM_POSITION), the target's position, the
   view's character's position. */
#include "world.h"

#define ARROW                  0x97cf04u  /* pointer: the arrow's object (0x97ca80 once made), or 0 */
#define ARROW_RECORD           0x97ca80u  /* ... its 0x418 bytes */
#define ARROW_MATRIX_COPY      0x97c928u  /* where 0x6f240f copies its matrix (its +0x18c) */
#define TARGET                 0x97cef0u  /* the target (its +0xa8 position) [guess: the objective] */
#define TGT_SHOW               0xa4u      /* frames left to show the arrow */
#define TGT_POSITION           0xa8u
#define TGT_MODE               0xb4u
#define NO_TARGET_TIMER        0x97f804u  /* when 0, the target's timer is kept at 180 */
#define PLAYER                 0x97f560u
#define PL_MODE                0x2d4u     /* 2, 0xc, 7, 8: the arrow can show */
#define PL_MODE_NEXT           0x2d8u
#define PL_BLEND               0x2ecu
#define GAME_STATE_97ed80      0x97ed80u  /* 3: no arrow */
#define HIDE_ARROW             0x98d098u  /* byte */
#define SET_NO_ARROW           0x3cu      /* settings */
#define SET_0x1b4              0x1b4u     /* settings: 2, no arrow */
#define VIEW_OWNER             0x3cu      /* the view's character */
#define VIEW_TYPES             0x58u      /* the view's type records, 0x60 bytes each */
#define ARROW_TYPE             314u
#define CAM_POSITION           (CAM + 0x2d4)  /* vec3: the camera's position [guess: from its use] */

/* the arrow's object (as a level object's fields) */
#define AO_MATRIX              0x40u      /* 4x4 */
#define AO_COLOUR              0x1f4u     /* r, g, b floats 0-255 */
#define AO_FLAGS_D8            0xd8u
#define AO_FLAGS_DC            0xdcu
#define AO_POSITION            0xfcu
#define AO_TYPE                0x154u
#define AO_0x16c               0x16cu
#define AO_MATRIX_COPY         0x18cu
#define AO_CELL                0x1b4u

#define SVC_ARROW_DRAW         (SVC + 0xf8)   /* (object, 0): draws an object */
#define SVC_ARROW_FREE         (SVC + 0x100)  /* (object) */
#define VEC_TO_WORLD           0x6298b8u  /* (vec3, out, 4x4) [guess: by the inverse view] */
#define VEC_SUB                0x627f10u  /* (out, a, b) */
#define VEC_NORMALISE          0x6285d0u  /* (vec3) -> st(0) its length [guess: normalised in place] */
#define MAT_FACING             0x62a864u  /* (direction, 4x4 out) */
#define VEC_DOT                0x5e0b10u  /* (a, b) -> st(0) */
#define OBJ_MATRIX_SAVE        0x6f240fu  /* (object, 0): its matrix into its +0x18c */
#define MEMSET                 0x8a99b0u
#define K_2                    0x8fa794u  /* .rdata floats 2, 0.8036, 0, 1, 255; .data 0x92f3f0 */
#define K_HALF_FOV             0x8fae80u
#define K_0                    0x8fa020u
#define K_1                    0x8fa05cu
#define K_255                  0x8fa1e0u
#define FADE_RANGE             0x92f3f0u

/* 0x5fef62 (view, release): the direction arrow for a view. With release set, the arrow's object
   is freed (SVC +0x100) and forgotten. Else, when the player's mode shows it and there is a
   target: the arrow 20 units in front of the camera, off the picture's centre along the view's
   y by (the 3D view's height / 2 x 0.80 x 20 / focal), turned along the view's character's
   direction to the target (scaled 0.06), in that character's cell, coloured (255, a, a) with a
   falling from 255 as it points away from the camera, and drawn. */
static int dc_5fef62(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0), release = ARG(1);
    if (release) {
        if (R32(ARROW)) {
            if (R32(SVC_ARROW_FREE))
                game_call(c, R32(SVC_ARROW_FREE), 1, R32(ARROW));
            W32(ARROW, 0);
        }
        return decomp_ret(c, e, c->eax, 0);
    }
    uint32_t m = R32(R32(PLAYER) + PL_MODE);
    if (m != 2 && m != 0xc && m != 7 && m != 8)
        return decomp_ret(c, e, c->eax, 0);
    if (R8(HIDE_ARROW) || R32(R32(SETTINGS) + SET_NO_ARROW))
        return decomp_ret(c, e, c->eax, 0);
    if (!((f80)RF32(R32(PLAYER) + PL_BLEND) == (f80)RF32(K_1))
        && R32(R32(PLAYER) + PL_MODE_NEXT) != 2 && R32(R32(PLAYER) + PL_MODE_NEXT) != 0xc)
        return decomp_ret(c, e, c->eax, 0);
    if (R32(GAME_STATE_97ed80) == 3)
        return decomp_ret(c, e, c->eax, 0);
    if (!R32(NO_TARGET_TIMER) && R32(TARGET) && R32(R32(TARGET) + TGT_MODE) != 1)
        W32(R32(TARGET) + TGT_SHOW, 0xb4);
    if (!R32(TARGET) || !R32(R32(TARGET) + TGT_SHOW))
        return decomp_ret(c, e, c->eax, 0);
    if (!R32(view + VIEW_OWNER) || R32(R32(SETTINGS) + SET_0x1b4) == 2)
        return decomp_ret(c, e, c->eax, 0);
    if (!R32(ARROW)) {
        W32(ARROW, ARROW_RECORD);
        game_call(c, MEMSET, 3, R32(ARROW), 0, 0x418);
    }
    W32(R32(ARROW) + AO_TYPE, ARROW_TYPE * 0x60 + R32(view + VIEW_TYPES));

    /* the arrow's position: (0, y, 20) in the view, into the world */
    uint32_t F = dc_frame(c, 0x60);
    uint32_t v = F, at = F + 0x10, dir = F + 0x20, u = F + 0x30, w = F + 0x40;
    W32(v, 0);
    W32(v + 8, 0x41a00000u);               /* 20 (after 500) */
    f80 t = (f80)(int32_t)R32(CAM_HEIGHT) / (f80)RF32(K_2) * (f80)RF32(K_HALF_FOV);
    WF32(v + 4, (float)(t * (f80)RF32(v + 8) / (f80)RF32(CAM_FOCAL)));
    game_call(c, VEC_TO_WORLD, 3, v, at, CAM_VIEW);
    uint32_t a = R32(ARROW);
    W32(a + AO_POSITION, R32(at));
    W32(a + AO_POSITION + 4, R32(at + 4));
    W32(a + AO_POSITION + 8, R32(at + 8));
    /* turned along the view's character's direction to the target */
    game_call(c, VEC_SUB, 3, dir, R32(TARGET) + TGT_POSITION, R32(view + VIEW_OWNER) + AO_POSITION);
    game_call_st0(c, VEC_NORMALISE, 1, dir);
    game_call(c, MAT_FACING, 2, dir, R32(ARROW) + AO_MATRIX);
    /* (the original reads the view's character's +0x60..+0x68, negating two when its +0x1a4 is set,
       into locals it never uses) */
    static const uint8_t rot[9] = {0x40, 0x44, 0x48, 0x50, 0x54, 0x58, 0x60, 0x64, 0x68};
    for (int k = 0; k < 9; k++) {
        uint32_t p = R32(ARROW) + rot[k];
        WF32(p, (float)((f80)u2f(0x3d75c28fu) * (f80)RF32(p)));    /* x 0.06 */
    }
    for (uint32_t k = 0; k < 12; k += 4)
        W32(R32(ARROW) + 0x70 + k, R32(R32(ARROW) + AO_POSITION + k));
    W32(R32(ARROW) + AO_CELL, R32(R32(view + VIEW_OWNER) + AO_CELL));
    W32(R32(ARROW) + AO_MATRIX_COPY, ARROW_MATRIX_COPY);
    uint32_t type = R32(R32(ARROW) + AO_TYPE);
    if (!R32(R32(type + 0x14) + 0x40)) {   /* (its model has nothing to draw) */
        dc_unframe(c, F, 0x60);
        return decomp_ret(c, e, c->eax, 0);
    }
    /* faded by how far it points from the camera: (1 - cos) x FADE_RANGE + 255 - FADE_RANGE */
    game_call(c, VEC_SUB, 3, u, R32(TARGET) + TGT_POSITION, CAM_POSITION);
    game_call_st0(c, VEC_NORMALISE, 1, u);
    game_call(c, VEC_SUB, 3, w, at, CAM_POSITION);
    game_call_st0(c, VEC_NORMALISE, 1, w);
    float cosv = (float)-game_call_st0(c, VEC_DOT, 2, u, w);
    uint32_t fade = 0x437f0000u;           /* 255 */
    if ((f80)cosv > (f80)RF32(K_0)) {
        f80 f = ((f80)RF32(K_1) - (f80)cosv) * (f80)RF32(FADE_RANGE);
        f = f + ((f80)RF32(K_255) - (f80)RF32(FADE_RANGE));
        fade = f2u((float)f);
        if ((f80)u2f(fade) < (f80)RF32(K_0))
            fade = 0;
        else if ((f80)u2f(fade) > (f80)RF32(K_255))
            fade = 0x437f0000u;
    }
    dc_unframe(c, F, 0x60);
    W32(R32(ARROW) + AO_COLOUR, 0x437f0000u);
    W32(R32(ARROW) + AO_COLOUR + 4, fade);
    W32(R32(ARROW) + AO_COLOUR + 8, fade);
    W32(R32(ARROW) + AO_0x16c, 0x40000000u);   /* 2 */
    W32(R32(ARROW) + AO_FLAGS_DC, R32(R32(ARROW) + AO_FLAGS_DC) & ~4u);
    W32(R32(ARROW) + AO_FLAGS_DC, R32(R32(ARROW) + AO_FLAGS_DC) & ~2u);
    W32(R32(ARROW) + AO_FLAGS_D8, R32(R32(ARROW) + AO_FLAGS_D8) | 0x40000u);
    game_call(c, OBJ_MATRIX_SAVE, 2, R32(ARROW), 0);
    game_call(c, R32(SVC_ARROW_DRAW), 2, R32(ARROW), 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(5fef62, dc_5fef62);
