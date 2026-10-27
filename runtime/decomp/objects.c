/* THE OBJECTS' DRAWING (object submission, DESIGN.md "The drawing code's rewrite"): the part of
   each frame's render pass that draws the level's actors (characters, props) with their lights.
   0x6f309c walks the level's actor list, 0x6f2732 prepares one actor (level of detail, its light
   set, its skeleton) and calls the actor drawer (0x7b0aa8 through the pointer at 0x97ee80), which
   draws the actor's model in one to three passes (0x7b1838: lights, ambient, transforms, then the
   model's parts, modeldraw.c). */
#include "objsubmit.h"

/* ---------------------------------------------------------------- small helpers */

/* 0x62981c (v, out, m): out = v transformed by the 4x4 m as a point (row vector times m, plus
   m's translation row). */
static int dc_62981c(CPU *c)
{
    uint32_t e = c->esp, v = ARG(0), out = ARG(1), m = ARG(2);
    for (uint32_t i = 0; i < 3; i++) {
        f80 x = (f80)RF32(v) * RF32(m + 4 * i) + (f80)RF32(v + 4) * RF32(m + 0x10 + 4 * i);
        x = x + (f80)RF32(v + 8) * RF32(m + 0x20 + 4 * i);
        WF32(out + 4 * i, (float)(x + RF32(m + 0x30 + 4 * i)));
    }
    return decomp_ret(c, e, out, 0);
}
DECOMP(62981c, dc_62981c);

#define ACTOR_MODEL      0x154u     /* the actor's model record (+0x14 -> +0x40: its mesh) */
#define MODEL_KIND       0x1cu      /* 0xf for the kind 0x7b2018 and 0x6f2732 single out [guess: a class code] */
#define MODEL_ID         0x20u      /* 0xb6 and 0xba are exempt from 0x7b2018's test */
#define ACTOR_FLAGS_D8   0xd8u
#define ACTOR_LOD        0x16cu     /* the level of detail as a float (0x7b1838 rounds it) */
#define SETTINGS         0x97f800u  /* the game's options record */
#define SET_0x278        0x278u
#define TIME_0x97f570    0x97f570u  /* two floats compared for equality [what they are: unknown] */
#define TIME_0x97ec54    0x97ec54u
#define FRAME_FLAGS      0x97ecccu  /* bit 0 [unknown] */
#define LOD_KIND_F       0xe88138u  /* the level of detail kind 0xf models get (writable data) */

/* 0x7b2018 (actor): whether to draw the actor's model now: no for a kind-0xf model (other than
   ids 0xb6 / 0xba) while 0x97ec54 equals 0x97f570 and bit 0 of 0x97eccc is clear; such a model
   also gets flag 2 at +0xd8 and the level of detail at 0xe88138 when option +0x278 is off. */
static int dc_7b2018(CPU *c)
{
    uint32_t e = c->esp, a = ARG(0);
    if (R32(R32(a + ACTOR_MODEL) + MODEL_ID) != 0xb6 && R32(R32(a + ACTOR_MODEL) + MODEL_ID) != 0xba
            && R32(R32(a + ACTOR_MODEL) + MODEL_KIND) == 0xf) {
        /* fld 0x97f570, fld 0x97ec54, fcompp: equal and ordered */
        if (RF32(TIME_0x97ec54) == RF32(TIME_0x97f570) && !(R32(FRAME_FLAGS) & 1))
            return decomp_ret(c, e, 0, 0);
        if (R32(R32(SETTINGS) + SET_0x278) == 0) {
            W32(a + ACTOR_FLAGS_D8, R32(a + ACTOR_FLAGS_D8) | 2);
            W32(a + ACTOR_LOD, R32(LOD_KIND_F));
        }
    }
    return decomp_ret(c, e, 1, 0);
}
DECOMP(7b2018, dc_7b2018);

#define PROJ_OWN_0xe7c4e5 0xe7c4e5u /* byte: build the projection here instead of using 0x97f288 */
#define PROJECTION       0x97f288u  /* the view's projection matrix (0x62a236's) [verified: 0x7b1838 hands it to SetTransform(PROJECTION) through here] */
#define NEAR_0x9943fc    0x9943fcu  /* the near distance for the projection built here, at least 1 [guess] */
#define FOV_0x97f274     0x97f274u  /* half the field of view in degrees [guess: doubled, times K_DEG2RAD] */
#define K_ONE            0x8fa05cu  /* .rdata 1.0f */
#define K_DEG2RAD        0x8fa344u  /* .rdata pi / 180 [guess: value] */
#define PERSPECTIVE      0x7a2cd5u  /* (out 4x4, fov, aspect, near, far) [guess: a perspective matrix] */

/* 0x7a2df4 (out, w, h, a3, a4, far): sets Direct3D's projection: normally the view's own
   (0x97f288); with 0xe7c4e5 set, a perspective built here (0x7a2cd5 with the field of view at
   0x97f274, h / w, the near distance at 0x9943fc but at least 1, and far) and also copied to out. */
static int dc_7a2df4(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), w = ARG(1), h = ARG(2), far = ARG(5), eax;
    if (R8(PROJ_OWN_0xe7c4e5)) {
        float aspect = (float)((f80)u2f(h) / u2f(w));
        uint32_t near = R32(NEAR_0x9943fc);
        if (u2f(near) < RF32(K_ONE))
            near = 0x3f800000u;
        f80 fov = (f80)RF32(FOV_0x97f274);
        fov = (fov + fov) * RF32(K_DEG2RAD);
        uint32_t F = dc_frame(c, 0x40);
        game_call(c, PERSPECTIVE, 5, F, f2u((float)fov), f2u(aspect), near, far);
        D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_PROJECTION, F);
        for (uint32_t col = 0; col < 16; col += 4)       /* the original copies column by column */
            for (uint32_t row = 0; row < 64; row += 16)
                W32(out + row + col, R32(F + row + col));
        dc_unframe(c, F, 0x40);
        eax = out;
    } else
        eax = D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_PROJECTION, PROJECTION);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(7a2df4, dc_7a2df4);

/* ---------------------------------------------------------------- one pass of an actor's model */

#define MODEL_FLAGS      0u         /* the model record's flags: bit 4 drawn by 0x7b1838 */
#define MODEL_NAME       4u
#define MODEL_DATA       0x14u      /* -> +0x40: the mesh */
#define DATA_MESH        0x40u
#define MESH_NAME        4u
#define MESH_CONFIGS_N   0x34u      /* the mesh's configurations (part sets), 20 bytes each */
#define MESH_CONFIGS     0x38u
#define MESH_LODS        0x50u      /* short: its levels of detail */
#define ACTOR_FLAGS      0xdcu      /* bit 0 reflection pass due (0x6f309c), 1 a second pass (and 0x6f2732: far), 2 near (animated in detail), 3 shader pass due, 5 prepared this frame, 6 keep the ambient colour, 7 stretched pass due */
#define ACTOR_CONFIG     0x3f4u     /* the configuration drawn normally */
#define ACTOR_CONFIG_ALT 0x3f8u     /* the configuration of the second passes (negative: none) */
#define ACTOR_LIGHTS     0x1ccu     /* the actor's light set (0x6f3ec5, lights.c): */
#define ACTOR_AMBIENT    0x1f4u     /*   ambient r, g, b as floats 0..255 */
#define ACTOR_DIRLIGHT   0x228u     /*   three directional lights 0x40 apart: direction (3 floats), then colour r, g, b (0..255) at +0xc */
#define LIGHT_STRIDE     0x40u
#define LOD_BIAS         0xe88110u  /* subtracted from ACTOR_LOD before rounding */
#define LOD_MAX          0x947f80u  /* the level-of-detail option's limit (unsigned) */
#define LOD_ZERO_0xe7c4ed 0xe7c4edu /* byte: level of detail 0 and no second passes [what sets it: unknown] */
#define ONE_LIGHT_0xe7c4dc 0xe7c4dcu /* byte: only the first directional light */
#define LIGHT_MIN        0xe8810cu  /* a light whose r + g + b is below this is off */
#define LIGHT_SCALE      0xe88108u  /* colour 0..255 to Direct3D's 0..1 [guess: 1/255] */
#define D3DLIGHT         0xe7be00u  /* the D3DLIGHT9 record handed to SetLight (type etc. set elsewhere) */
#define D3DLIGHT_DIFFUSE 0xe7be04u
#define D3DLIGHT_DIR     0xe7be40u
#define BLEND_CURRENT    0xe7bec4u  /* material.c's blend-mode cache */
#define WORLD_IDENTITY   0xe7c7b8u  /* the world matrix for skinned drawing [guess: identity; the vertices are skinned on the CPU] */
#define VIEW             0x97f474u  /* the view (camera) matrix */
#define PROJ_ARGS        0x97d6b0u  /* 0x7a2df4's arguments: +0xc w, +0x10 h, +0, +8, +4 */
#define LOG_ERROR        0x6303eeu  /* (format, ...) the game's error report */
#define ERR_BAD_CONFIG   0xe88114u  /* "invalid config in model %s %s ..." */
#define DRAW_NOW         0x7b2018u
#define SET_PROJECTION   0x7a2df4u
#define MODEL_DRAW       0x7b1808u  /* (config, lod, actor, 1, world or 0, second pass) modeldraw.c */

/* one directional light of the actor's set into Direct3D's light i, or light i off when its colour
   is (nearly) black */
static void dir_light(CPU *c, uint32_t a, uint32_t i)
{
    uint32_t l = a + ACTOR_DIRLIGHT + LIGHT_STRIDE * i;
    f80 s = (f80)RF32(l + 0xc) + RF32(l + 0x10);
    float sum = f32(s + RF32(l + 0x14));
    if (sum < RF32(LIGHT_MIN)) {                    /* fcompp, jb (ordered) */
        D3DCALL(c, D3D_LIGHT_ENABLE, 2, i, 0);
        return;
    }
    for (uint32_t k = 0; k < 12; k += 4)
        W32(D3DLIGHT_DIR + k, R32(l + k));
    for (uint32_t k = 0; k < 12; k += 4)
        WF32(D3DLIGHT_DIFFUSE + k, f32((f80)RF32(l + 0xc + k) * RF32(LIGHT_SCALE)));
    D3DCALL(c, D3D_SET_LIGHT, 2, i, D3DLIGHT);
    D3DCALL(c, D3D_LIGHT_ENABLE, 2, i, 1);
}

/* 0x7b1838 (actor, world, second): one pass of the actor's model: the configuration (the second
   passes' when asked or flagged), the level of detail (ACTOR_LOD rounded, clamped to the mesh's
   and the option's), the actor's three directional lights and ambient colour, blending off, the
   camera's view and projection unless a world matrix is given (the passes 0x7b0aa8 draws with
   one: the reflection and the stretched pass), then the parts (0x7b1808), and depth writes back on. */
static int dc_7b1838(CPU *c)
{
    uint32_t e = c->esp, a = ARG(0), world = ARG(1), second = ARG(2);
    uint32_t mesh = R32(R32(R32(a + ACTOR_MODEL) + MODEL_DATA) + DATA_MESH);
    if (!mesh)
        return decomp_ret(c, e, 0, 0);
    uint32_t mf = R32(R32(a + ACTOR_MODEL) + MODEL_FLAGS);
    if (!(mf & 0x10))
        return decomp_ret(c, e, mf, 0);
    uint32_t now = game_call(c, DRAW_NOW, 1, a);
    if (!(now & 0xff))
        return decomp_ret(c, e, now, 0);
    uint32_t ci;
    if ((R32(a + ACTOR_FLAGS) & 2) && (int32_t)R32(a + ACTOR_CONFIG_ALT) >= 0)
        ci = R32(a + ACTOR_CONFIG_ALT);
    else
        ci = R32(a + ACTOR_CONFIG);
    if (second)
        ci = R32(a + ACTOR_CONFIG_ALT);
    if (ci >= R32(mesh + MESH_CONFIGS_N))
        game_call(c, LOG_ERROR, 4, ERR_BAD_CONFIG, R32(a + ACTOR_MODEL) + MODEL_NAME, mesh + MESH_NAME, ci);
    uint32_t cfg = ci * 20 + R32(mesh + MESH_CONFIGS);
    int32_t lod = fpu_ist32((f80)RF32(a + ACTOR_LOD) - RF32(LOD_BIAS));
    if (lod < 0)
        lod = 0;
    uint32_t m2 = R32(R32(R32(a + ACTOR_MODEL) + MODEL_DATA) + DATA_MESH);
    if (lod > (int16_t)R16(m2 + MESH_LODS) - 1)
        lod = (int16_t)R16(m2 + MESH_LODS) - 1;
    if ((uint32_t)lod > R32(LOD_MAX))
        lod = (int32_t)R32(LOD_MAX);
    if (lod > (int16_t)R16(m2 + MESH_LODS) - 1)
        lod = (int16_t)R16(m2 + MESH_LODS) - 1;
    if (R8(LOD_ZERO_0xe7c4ed))
        lod = 0;
    dir_light(c, a, 0);
    for (uint32_t i = 1; i < 3; i++)
        if (R8(ONE_LIGHT_0xe7c4dc))
            D3DCALL(c, D3D_LIGHT_ENABLE, 2, i, 0);
        else
            dir_light(c, a, i);
    uint32_t r = (uint32_t)fpu_ist64(RF32(a + ACTOR_AMBIENT));
    uint32_t g = (uint32_t)fpu_ist64(RF32(a + ACTOR_AMBIENT + 4));
    uint32_t b = (uint32_t)fpu_ist64(RF32(a + ACTOR_AMBIENT + 8));
    D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_AMBIENT, (r << 16) | 0xff000000u | (g << 8) | b);
    D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_ALPHABLENDENABLE, 0);
    W32(BLEND_CURRENT, 0);
    /* (the original copies the projection and the view into locals it never reads) */
    if (!world) {
        D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_WORLD, WORLD_IDENTITY);
        D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_VIEW, VIEW);
        game_call(c, SET_PROJECTION, 6, PROJECTION, R32(PROJ_ARGS + 0xc), R32(PROJ_ARGS + 0x10),
                  R32(PROJ_ARGS), R32(PROJ_ARGS + 8), R32(PROJ_ARGS + 4));
    }
    game_call(c, MODEL_DRAW, 6, cfg, (uint32_t)lod, a, 1, world, second);
    uint32_t eax = D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_ZWRITEENABLE, 1);
    return decomp_ret(c, e, eax, 0);
}
/* Not switched in yet: with it in, the lobby route's first SetLight (frame 2283) had the diffuse
   colours one bit off (fixed since by f32: GCC had made the multiply an SSE mulss), and the
   check still differed after that. */
DECOMP_PENDING(7b1838, dc_7b1838);

/* ---------------------------------------------------------------- the actor drawer */

#define ACTOR_MIRROR     0u         /* the actor's reflection 4x4 (0x6f309c writes it: y mirrored about the floor) */
#define ACTOR_BONES      0x18cu     /* the actor's bone matrices, 64 bytes each */
#define ACTOR_TARGET     0x390u     /* an object whose +0x70 the stretched bone reaches toward, or 0 */
#define ACTOR_POS        0xfcu      /* the actor's position (3 floats) */
#define ACTOR_SHADER_N   0x350u     /* (short) cleared after the shader pass */
#define SET_REFLECTIONS  0x1ecu     /* option: actors reflected in shiny floors */
#define STRETCH_DIST     0xe88080u  /* the stretch's distance without ACTOR_TARGET */
#define STRETCH_DIV      0xe8807cu
#define STRETCH_MAX      0xe88078u
#define MAT_ONE          0xe88074u  /* 1.0 and 0.0 as the stretch matrix's entries [guess: values] */
#define MAT_ZERO         0xe88070u
#define CULL_CW          2u
#define CULL_CCW         3u
#define VERTEX_DECL      0x78ad3cu
#define MODEL_PASS       0x7b1838u
#define SHADER_SUPPORT   0x78bab9u
#define SHADER_PASS      0x7b0e78u  /* (actor, 0) the model with shaders [not rewritten: not on the route] */
#define DISTANCE         0x628292u  /* (a, b) -> st0 */
#define MAT_COPY         0x7a6280u  /* (this = 4x4; src) */

/* 0x7b0aa8 (actor), the actor drawer (0x6f2732 calls it through 0x97ee80): the actor's model in
   up to four passes: the normal one; its reflection (option +0x1ec and actor flag 0, set by
   0x6f309c over a shiny floor: drawn again through the mirror 4x4 at the actor's +0, front faces
   culled since the mirror turns the winding); the shader pass (flag 3, where vertex and pixel
   shaders 1.1 exist, 0x7b0e78); the stretched pass (flag 7, set by 0x6f2732 for kind-0xf models
   with a second configuration, with option +0x278: bone 1 becomes a z scale by the distance to
   ACTOR_TARGET over STRETCH_DIV, at most STRETCH_MAX, and the second configuration is drawn
   through it [guess: a beam or tracer reaching its target]). */
static int dc_7b0aa8(CPU *c)
{
    uint32_t e = c->esp, a = ARG(0);
    D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_CULLMODE, CULL_CCW);
    game_call(c, VERTEX_DECL, 1, 0);
    game_call(c, MODEL_PASS, 3, a, 0, 0);
    if (!R8(LOD_ZERO_0xe7c4ed) && R32(R32(SETTINGS) + SET_REFLECTIONS) && (R32(a + ACTOR_FLAGS) & 1)) {
        D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_CULLMODE, CULL_CW);
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 2);
        uint32_t F = dc_frame(c, 0x40);
        game_callm(c, MAT_COPY, F, 1, a + ACTOR_MIRROR);
        game_call(c, MODEL_PASS, 3, a, F, 0);
        dc_unframe(c, F, 0x40);
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~1u);
        D3DCALL(c, D3D_SET_RENDER_STATE, 2, D3DRS_CULLMODE, CULL_CCW);
    }
    if (!R8(LOD_ZERO_0xe7c4ed) && (R32(a + ACTOR_FLAGS) & 8) && (game_call(c, SHADER_SUPPORT, 0) & 0xff)) {
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 2);
        game_call(c, SHADER_PASS, 2, a, 0);
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~8u);
        W16(a + ACTOR_SHADER_N, 0);
    }
    if (R32(a + ACTOR_FLAGS) & 0x80) {
        if (R32(R32(SETTINGS) + SET_0x278) && !(R32(a + ACTOR_FLAGS) & 2)
                && (int32_t)R32(a + ACTOR_CONFIG_ALT) >= 0) {
            float d = RF32(STRETCH_DIST);
            if (R32(a + ACTOR_TARGET))
                d = f32(game_call_st0(c, DISTANCE, 2, a + ACTOR_POS, R32(a + ACTOR_TARGET) + 0x70));
            uint32_t s = f2u(f32((f80)d / RF32(STRETCH_DIV)));
            if (RF32(STRETCH_MAX) < u2f(s))             /* fcompp, jb (ordered) */
                s = R32(STRETCH_MAX);
            uint32_t m = R32(a + ACTOR_BONES) + 0x40;   /* bone 1 */
            for (uint32_t i = 0; i < 16; i++)
                W32(m + 4 * i, i == 10 ? s : R32(i % 5 ? MAT_ZERO : MAT_ONE));
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~4u);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 2);
            uint32_t F = dc_frame(c, 0x40);
            game_callm(c, MAT_COPY, F, 1, m);
            game_call(c, MODEL_PASS, 3, a, F, 1);
            dc_unframe(c, F, 0x40);
        }
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~0x80u);
    }
    return decomp_ret(c, e, R32(a + ACTOR_FLAGS), 0);
}
/* Not switched in yet: never checked alone (switched in with 0x7b1838, 0x6f2732, 0x6f3ec5, which
   differ). */
DECOMP_PENDING(7b0aa8, dc_7b0aa8);

/* ---------------------------------------------------------------- one actor prepared and drawn */

#define LEVEL_PTR        0x97f720u  /* the level */
#define LEVEL_CLOSE_OBJ  0x170u     /* an object whose +0x40 set means the close-up level of detail [guess: a cinematic / Focus view] */
#define LEVEL_PLAYER     0x3cu      /* the player's actor [guess: always drawn in detail] */
#define ACTOR_HEAD_POS   0x144u     /* where the actor's lights are gathered (3 floats; +20 up when +0x1a4 is 0) */
#define ACTOR_SIZE_A     0x160u     /* two sizes the level-of-detail distances scale with */
#define ACTOR_SIZE_B     0x164u
#define ACTOR_LOD_TOP    0x168u     /* the level of detail when nearest (ACTOR_LOD falls to 2 with the distance) */
#define ACTOR_SKEL       0x188u     /* the actor's skeleton instance, or 0 */
#define ACTOR_ANIM_PHASE 0x15cu     /* bit 0 against 0x97ec48: the frames the cheap animation update may run */
#define ACTOR_ROOM       0x1b4u     /* the actor's room: its light table (lights.c), flags at +0 (0x4000: ambient from +0xb4) */
#define ACTOR_AMBIENT_B  0x1c8u     /* the ambient colour bytes r, g, b, a (lights.c's input) */
#define ACTOR_ATTACHED   0x388u     /* the actor this one is attached to (shares its lights), or 0 */
#define ACTOR_0x34C      0x34cu     /* copied from the attached-to actor */
#define ACTOR_0x150      0x150u     /* set to 10, 5 or 1000 for kind-0xf models [unknown] */
#define ACTOR_0x1A4      0x1a4u
#define SET_COUNT_ACTORS 0x18cu     /* option: count the actors drawn (0x97fa78 own, 0x97fa7c attached) */
#define COUNT_OWN        0x97fa78u
#define COUNT_ATTACHED   0x97fa7cu
#define LOD_SCALE        0x97ecbcu  /* the level-of-detail distance scale (5 when LEVEL_CLOSE_OBJ is set) */
#define CAMERA_0x97f560  0x97f560u  /* +0x2d4 == 4: the distance is the actor's size, not its depth [unknown mode] */
#define BONES_TARGET     0x9920b0u  /* where the characters' code poses the bones (the actor's ACTOR_BONES) */
#define CHEAP_ANIM_ON    0x9924e4u
#define ANIM_PHASE       0x97ec48u
#define K_UP20           0x8fa030u  /* .rdata 20.0f */
#define K_1_5            0x8fa044u  /* .rdata 1.5f */
#define K_HALF           0x8fa038u  /* .rdata 0.5f */
#define K_LOD_FAR        0x8fa794u  /* .rdata 2.0f: the far level of detail */
#define ACTOR_DRAWER     0x97ee80u  /* the service table's actor drawer (0x7b0aa8) */
#define LIGHT_SET        0x6f3ec5u  /* lights.c */
#define POINT_BY         0x62981cu
#define ANIMATE          0x6d0f2du  /* (this = skeleton instance; detailed, 100) poses the bones */
#define ANIMATE_CHEAP    0x6d1b6au  /* (this = skeleton instance) */
#define POSE_RIGID       0x6f240fu  /* (actor, view): the actor's 4x4 (+0x40) copied to its first bone matrix [verified: disassembly] */

/* an actor of its own (not attached): its light set, its level of detail by its depth in view
   (between half its scaled sizes; near or far, and always near for the player and what he holds),
   the kind-0xf extras, then its bones posed */
static void prepare_own(CPU *c, uint32_t a, uint32_t close)
{
    uint32_t F = dc_frame(c, 0x20), pos = F, v = F + 0x10;
    for (uint32_t k = 0; k < 12; k += 4)
        W32(pos + k, R32(a + ACTOR_HEAD_POS + k));
    if (!R32(a + ACTOR_0x1A4))
        WF32(pos + 4, f32((f80)RF32(pos + 4) + RF32(K_UP20)));
    if (R32(R32(a + ACTOR_ROOM)) & 0x4000) {
        if (R32(R32(a + ACTOR_MODEL) + MODEL_KIND) == 0xf) {
            W8(a + ACTOR_AMBIENT_B, 0x80);
            W8(a + ACTOR_AMBIENT_B + 1, 0x80);
            W8(a + ACTOR_AMBIENT_B + 2, 0x80);
            W8(a + ACTOR_AMBIENT_B + 3, 0xff);
        } else if (!(R32(a + ACTOR_FLAGS) & 0x40))
            W32(a + ACTOR_AMBIENT_B, R32(R32(a + ACTOR_ROOM) + 0xb4));
    }
    game_call(c, LIGHT_SET, 9, R32(a + ACTOR_ROOM), pos, 0x42c80000u /* 100.0f */, a + ACTOR_LIGHTS, 4,
              0x30, a + ACTOR_AMBIENT_B, 0, 0);
    if (close)
        W32(LOD_SCALE, 0x40a00000u);                /* 5.0f */
    game_call(c, POINT_BY, 3, a + ACTOR_POS, v, VIEW);
    float sb = f32((f80)RF32(a + ACTOR_SIZE_B) * RF32(K_1_5));
    float sa = f32((f80)RF32(a + ACTOR_SIZE_A) * RF32(K_1_5));
    uint32_t z = R32(v + 8);                        /* the depth in view */
    if (R32(R32(CAMERA_0x97f560) + 0x2d4) == 4)
        z = f2u(sa);
    sa = f32((f80)sa * RF32(K_HALF));
    sb = f32((f80)sb * RF32(K_HALF));
    sa = f32((f80)sa * RF32(LOD_SCALE) + sa);
    sb = f32((f80)sb * RF32(LOD_SCALE) + sb);
    if (u2f(z) < sa)                                /* test ah, 5, jp: ordered less */
        z = f2u(sa);
    if (u2f(z) > sb) {                              /* and 0x4100 clear: ordered greater */
        z = f2u(sb);
        if (close) {
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 4);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~2u);
        } else {
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~4u);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 2);
        }
    } else {
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 4);
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~2u);
    }
    float t = f32(((f80)u2f(z) - sa) / ((f80)sb - sa));
    f80 lod = ((f80)RF32(K_ONE) - t) * ((f80)RF32(a + ACTOR_LOD_TOP) - RF32(K_LOD_FAR));
    WF32(a + ACTOR_LOD, f32(lod + RF32(K_LOD_FAR)));
    dc_unframe(c, F, 0x20);
    uint32_t ski = a + ACTOR_SKEL;
    if (!close && R32(ski)) {
        uint32_t pl = R32(R32(LEVEL_PTR) + LEVEL_PLAYER);
        if (a == pl
                || R32(R32(ski) + 0x338) == R32(R32(R32(LEVEL_PTR) + LEVEL_PLAYER) + ACTOR_SKEL)
                || R32(R32(ski) + 0x334) == R32(R32(R32(LEVEL_PTR) + LEVEL_PLAYER) + ACTOR_SKEL)) {
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 4);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~2u);
        }
    }
    if (!R32(ski))
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~4u);
    if (R32(R32(a + ACTOR_MODEL) + MODEL_KIND) == 0xf) {
        if (R32(R32(SETTINGS) + SET_0x278) == 0) {
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~2u);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~4u);
            W32(a + ACTOR_0x150, 0x41200000u);      /* 10.0f */
        } else if (RF32(TIME_0x97f570) == RF32(TIME_0x97ec54) && !(R32(FRAME_FLAGS) & 1))
            W32(a + ACTOR_0x150, 0x40a00000u);      /* 5.0f */
        else if ((int32_t)R32(a + ACTOR_CONFIG_ALT) >= 0) {
            W32(a + ACTOR_FLAGS_D8, R32(a + ACTOR_FLAGS_D8) | 2);
            W32(a + ACTOR_LOD, 0x40800000u);        /* 4.0f */
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~2u);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 4);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 0x80);
            W32(a + ACTOR_0x150, 0x447a0000u);      /* 1000.0f */
        }
    }
    W32(BONES_TARGET, R32(a + ACTOR_BONES));
    if (R32(ski)) {
        if (R32(CHEAP_ANIM_ON) && (R32(a + ACTOR_FLAGS) & 2)
                && (R32(ANIM_PHASE) & 1) == (R32(a + ACTOR_ANIM_PHASE) & 1))
            game_callm(c, ANIMATE_CHEAP, R32(ski), 0);
        else
            game_callm(c, ANIMATE, R32(ski), 2, (R32(a + ACTOR_FLAGS) & 4) != 0, 100);
    } else
        game_call(c, POSE_RIGID, 2, a, VIEW);
}

/* 0x6f2732 (level, actor, attached, pass): one actor of the object pass: prepared once a frame
   (unless 0x97eccc bit 0 asks again): an actor of its own gets its light set, level of detail and
   pose (prepare_own); an attached one takes its holder's light set and is posed in detail. Then
   the actor drawer (0x7b0aa8), with the actor's flags kept as they were before it. */
static int dc_6f2732(CPU *c)
{
    uint32_t e = c->esp, lvl = ARG(0), a = ARG(1), attached = ARG(2) & 0xff, pass = ARG(3) & 0xff;
    uint32_t model = R32(a + ACTOR_MODEL), co = R32(lvl + LEVEL_CLOSE_OBJ);
    uint32_t close = co && R32(co + 0x40);
    if (!(R32(a + ACTOR_FLAGS) & 0x20) || (R32(FRAME_FLAGS) & 1)) {
        W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 0x20);
        if (R32(R32(SETTINGS) + SET_COUNT_ACTORS)) {
            if (!attached)
                W32(COUNT_OWN, R32(COUNT_OWN) + 1);
            else
                W32(COUNT_ATTACHED, R32(COUNT_ATTACHED) + 1);
        }
        if (!attached) {
            if (R32(R32(model + MODEL_DATA) + DATA_MESH))
                prepare_own(c, a, close);
        } else if (R32(R32(model + MODEL_DATA) + DATA_MESH)) {
            W32(BONES_TARGET, R32(a + ACTOR_BONES));
            W32(a + ACTOR_LOD, 0x40000000u);        /* 2.0f */
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~4u);
            game_call(c, MEMCPY, 3, a + ACTOR_LIGHTS, R32(a + ACTOR_ATTACHED) + ACTOR_LIGHTS, 0x180);
            W32(a + ACTOR_0x34C, R32(R32(a + ACTOR_ATTACHED) + ACTOR_0x34C));
            if (R32(a + ACTOR_SKEL))
                game_callm(c, ANIMATE, R32(a + ACTOR_SKEL), 2, 1, 100);
            else
                game_call(c, POSE_RIGID, 2, a, VIEW);
        }
    }
    uint32_t keep = R32(a + ACTOR_FLAGS);
    /* (the original pushes the pass byte with the rest of edx as it happened to be) */
    uint32_t eax = game_call(c, R32(ACTOR_DRAWER), 2, a, pass);
    W32(a + ACTOR_FLAGS, keep);
    return decomp_ret(c, e, eax, 0);
}
/* Not switched in yet: with it in (and 0x6f3ec5 off) the lights' directions on the lobby route's
   first SetLight were a bit off (f32 since; not re-checked alone). */
DECOMP_PENDING(6f2732, dc_6f2732);

/* ---------------------------------------------------------------- the object pass */

#define SET_OBJECTS      0x100u     /* option: draw the objects at all */
#define HOOK_OBJ_BEFORE  0x97ee78u  /* the service table's hooks round the object pass, (1, pass) */
#define HOOK_OBJ_AFTER   0x97ee7cu
#define BUCKETS          0x9924e8u  /* the pass's sorted buckets (0x6f2dcf(200) makes them once) */
#define BUCKETS_FIRST    0x10u      /* first bucket; each: +0xc next bucket, entries chained at +8, actor at +4 */
#define LEVEL_ACTORS     0x50u      /* the level's actors (next at +0xd0) */
#define LEVEL_OTHERS     0x54u      /* other actors, flagged 8 at +0xd8 (not drawn here) */
#define ACTOR_NEXT       0xd0u
#define ACTOR_0xE4       0xe4u      /* -1: 0x6ee140 runs for it */
#define ACTOR_FLOOR      0x398u     /* the floor (room) under the actor: +0x60 bit 0x100000 shiny, +0x34 its height */
#define ACTOR_FEET       0x100u     /* a height the mirror plane may not exceed [guess: the actor's base] */
#define DRAW_STATE       0x7bf160u  /* (0, 0xff, 0, 0) before, (0, 0, 0, 0) after [world batch] */
#define BUCKETS_NEW      0x6f2dcfu
#define BUCKETS_CLEAR    0x6f2e67u
#define ACTOR_0x6ee140   0x6ee140u
#define ACTOR_VISIBLE    0x6f242fu  /* (level, actor) -> al [world batch; guess: in view] */
#define BUCKETS_ADD      0x6f2fabu  /* (buckets, actor) -> entries added */
#define DRAW_LATER       0x6f36d9u  /* (actor) -> al: drawn after the rest [guess: translucent] */
#define ACTOR_PREPARE    0x6f2732u

/* one actor of the buckets: its reflection 4x4 (where reflections are on and the floor under it
   is shiny: y mirrored about the floor's height, at most the actor's ACTOR_FEET), then prepared
   and drawn */
static void pass_actor(CPU *c, uint32_t lvl, uint32_t a, uint32_t refl, uint32_t pass)
{
    if (refl) {
        if (R32(a + ACTOR_FLOOR) && (R32(R32(a + ACTOR_FLOOR) + 0x60) & 0x100000)) {
            uint32_t h = R32(R32(a + ACTOR_FLOOR) + 0x34);
            if (u2f(h) > RF32(a + ACTOR_FEET))      /* and 0x4100 clear: ordered greater */
                h = R32(a + ACTOR_FEET);
            static const uint32_t mirror[16] = {
                0x3f800000u, 0, 0, 0,  0, 0xbf800000u, 0, 0,  0, 0, 0x3f800000u, 0,  0, 0, 0, 0x3f800000u };
            for (uint32_t i = 0; i < 16; i++)
                W32(a + ACTOR_MIRROR + 4 * i, i == 13 ? f2u(f32((f80)u2f(h) + u2f(h))) : mirror[i]);
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) | 1);
        } else
            W32(a + ACTOR_FLAGS, R32(a + ACTOR_FLAGS) & ~1u);
    }
    game_call(c, ACTOR_PREPARE, 4, lvl, a, R32(a + ACTOR_ATTACHED) ? 1u : 0u, pass);
}

/* 0x6f309c (pass): the object pass of a view: the level's actors that are in view (0x6f242f) go
   into the sorted buckets, then each is prepared and drawn (pass_actor) in bucket order, up to 64
   of those 0x6f36d9 picks deferred to the end; between the service table's hooks and 0x7bf160's
   state. A nonzero pass draws only actors flagged 8 at +0xdc. */
static int dc_6f309c(CPU *c)
{
    uint32_t e = c->esp, pass = ARG(0) & 0xff, lvl = R32(LEVEL_PTR);
    uint32_t refl = R32(R32(SETTINGS) + SET_REFLECTIONS) != 0, only = pass ? 8 : 0;
    if (!R32(R32(SETTINGS) + SET_OBJECTS))
        return decomp_ret(c, e, R32(SETTINGS), 0);
    /* (the original pushes the pass byte in ecx, whose rest is the level pointer's) */
    if (R32(HOOK_OBJ_BEFORE))
        game_call(c, R32(HOOK_OBJ_BEFORE), 2, 1, (lvl & ~0xffu) | pass);
    game_call(c, DRAW_STATE, 4, 0, 0xff, 0, 0);
    if (!R32(BUCKETS))
        W32(BUCKETS, game_call(c, BUCKETS_NEW, 1, 200));
    game_call(c, BUCKETS_CLEAR, 1, R32(BUCKETS));
    for (uint32_t a = R32(lvl + LEVEL_OTHERS), next; a; a = next) {
        W32(a + ACTOR_FLAGS_D8, R32(a + ACTOR_FLAGS_D8) | 8);
        next = R32(a + ACTOR_NEXT);
        if (R32(a + ACTOR_0xE4) == 0xffffffffu)
            game_call(c, ACTOR_0x6ee140, 1, a);
    }
    for (uint32_t a = R32(lvl + LEVEL_ACTORS), next; a; a = next) {
        W32(a + ACTOR_FLAGS_D8, R32(a + ACTOR_FLAGS_D8) & ~8u);
        next = R32(a + ACTOR_NEXT);
        if ((!only || (R32(a + ACTOR_FLAGS) & only)) && (game_call(c, ACTOR_VISIBLE, 2, lvl, a) & 0xff))
            game_call(c, BUCKETS_ADD, 2, R32(BUCKETS), a);
    }
    uint32_t later[64];
    int32_t n = 0;
    for (uint32_t b = R32(R32(BUCKETS) + BUCKETS_FIRST); b; b = R32(b + 0xc))
        for (uint32_t en = b; en; en = R32(en + 8)) {
            uint32_t a = R32(en + 4);
            if ((uint32_t)n * 4 < 0x100 && (game_call(c, DRAW_LATER, 1, a) & 0xff))
                later[n++] = a;
            else
                pass_actor(c, lvl, a, refl, pass);
        }
    for (int32_t i = 0; i < n; i++)
        pass_actor(c, lvl, later[i], refl, pass);
    uint32_t eax = game_call(c, DRAW_STATE, 4, 0, 0, 0, 0);
    if (R32(HOOK_OBJ_AFTER))
        eax = game_call(c, R32(HOOK_OBJ_AFTER), 2, 1, (c->edx & ~0xffu) | pass);
    return decomp_ret(c, e, eax, 0);
}
/* Not switched in yet: never checked alone. */
DECOMP_PENDING(6f309c, dc_6f309c);
