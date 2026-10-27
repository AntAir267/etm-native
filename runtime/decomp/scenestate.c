/* THE STATE AROUND THE OBJECTS (DESIGN.md "The drawing code's rewrite"): the service table's hooks
   the render pass calls before (0x7ac018, SVC +0xf0) and after (0x7ab9e8, SVC +0xf4) the objects
   (0x6f309c) and the view's scene (0x646c33) [verified: tools/calltree.sh 6465c8 6800; renderpass.c
   names them HOOK_BEFORE, HOOK_AFTER]. Direct3D state only, plus the light record the objects'
   lighting starts from; nothing here moves between updates. These two come from a different
   compiler than the rest (arguments stored at [esp + n], frames left with leave). */
#include "world.h"

#define RS_SPECULARENABLE      29u
#define RS_NORMALIZENORMALS    143u
#define WORLD_IDENTITY         0xe7c7b8u  /* a 4x4 [guess: the identity] */
#define LIGHT_BRIGHTNESS       0xe88058u  /* float: the objects' material (diffuse, ambient, specular) */
#define LIGHT_RANGE_SQ         0xe88054u  /* float: the light's range, squared */
#define LIGHT                  0xe7be00u  /* D3DLIGHT9: the objects' light, set up here */
#define LIGHT_0x68             0xe7be68u  /* (the dword after it) */
#define SET_0x278              0x278u     /* settings: set to 1 here */
#define FILTER_MODE            0x947f68u
#define MAX_ANISOTROPY         0x947f6cu
#define SQRT_F                 0x41e8d0u  /* (float) -> st(0): square root [verified: the characters batch's rewrite of it] */
#define MEMSET                 0x8a99b0u

/* 0x7ac018 (0, 0) (SVC +0xf0): before the objects: no depth bias, lights 0-2 and lighting off, no
   normalising, clockwise culling, no specular, stages 0 and 1 on texture coordinates 0 without
   transforms. */
static int dc_7ac018(CPU *c)
{
    uint32_t e = c->esp;
    game_call(c, DEPTH_BIAS_OFF, 0);
    light_enable(c, 0, 0);
    light_enable(c, 1, 0);
    light_enable(c, 2, 0);
    set_rs(c, RS_LIGHTING, 0);
    set_rs(c, RS_NORMALIZENORMALS, 0);
    set_rs(c, RS_CULLMODE, 2);
    set_rs(c, RS_SPECULARENABLE, 0);
    set_tss(c, 0, TSS_TEXTURETRANSFORMFLAGS, 0);
    set_tss(c, 0, TSS_TEXCOORDINDEX, 0);
    set_tss(c, 1, TSS_TEXTURETRANSFORMFLAGS, 0);
    set_tss(c, 1, TSS_TEXCOORDINDEX, 0);
    game_call(c, DEPTH_BIAS_OFF, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7ac018, dc_7ac018);

/* 0x7ab9e8 (0, 0) (SVC +0xf4): after the objects: no blending or alpha test, counter-clockwise
   culling, depth test (as W when set); the filter setting's filters on stage 0, then point filters
   over them; the camera's view, an identity world, lighting on with no textures, a grey material
   (LIGHT_BRIGHTNESS), material colours only, a white texture factor, and LIGHT cleared to a
   directional light of range sqrt(LIGHT_RANGE_SQ); normalised normals. */
static int dc_7ab9e8(CPU *c)
{
    uint32_t e = c->esp;
    W32(LIGHT_0x68, 0);
    W32(R32(SETTINGS) + SET_0x278, 1);
    set_rs(c, RS_ALPHABLENDENABLE, 0);
    set_rs(c, RS_ALPHATESTENABLE, 0);
    set_rs(c, RS_CULLMODE, 3);             /* D3DCULL_CCW */
    set_rs(c, RS_ZENABLE, R8(USE_W_BUFFER) ? 2 : 1);
    uint32_t f = R32(FILTER_MODE);
    if (f <= 2) {                          /* 0, 1, 2: linear */
        set_ss(c, 0, SAMP_MAGFILTER, 2);
        set_ss(c, 0, SAMP_MINFILTER, 2);
        set_ss(c, 0, SAMP_MIPFILTER, 2);
    } else if (f == 3) {                   /* anisotropic */
        set_ss(c, 0, SAMP_MAGFILTER, 3);
        set_ss(c, 0, SAMP_MINFILTER, 3);
        set_ss(c, 0, SAMP_MIPFILTER, 3);
        set_ss(c, 0, SAMP_MAXANISOTROPY, R32(MAX_ANISOTROPY));
    }
    set_ss(c, 0, SAMP_MAGFILTER, 1);       /* point, over whatever was set */
    set_ss(c, 0, SAMP_MINFILTER, 1);
    set_ss(c, 0, SAMP_MIPFILTER, 1);
    set_transform(c, D3DTS_VIEW, CAM_VIEW);
    set_transform(c, D3DTS_WORLD, WORLD_IDENTITY);
    set_rs(c, RS_LIGHTING, 1);
    D3DCALL(c, D3D_SET_TEXTURE, 2, 0, 0);
    D3DCALL(c, D3D_SET_TEXTURE, 2, 1, 0);
    uint32_t F = dc_frame(c, 0x44), mat = F;   /* D3DMATERIAL9 */
    game_call(c, MEMSET, 3, mat, 0, 0x44);  /* memset */
    for (uint32_t k = 0; k < 4; k++) {     /* (copied as dwords: the original only moves them) */
        W32(mat + 0x10 + 4 * k, R32(LIGHT_BRIGHTNESS));  /* ambient */
        W32(mat + 0x00 + 4 * k, R32(LIGHT_BRIGHTNESS));  /* diffuse */
        W32(mat + 0x20 + 4 * k, R32(LIGHT_BRIGHTNESS));  /* specular */
    }
    D3DCALL(c, D3D_SET_MATERIAL, 1, mat);
    dc_unframe(c, F, 0x44);
    set_rs(c, RS_AMBIENTMATERIALSOURCE, 0);
    set_rs(c, RS_DIFFUSEMATERIALSOURCE, 0);
    set_rs(c, RS_TEXTUREFACTOR, 0xffffffffu);
    game_call(c, MEMSET, 3, LIGHT, 0, 0x68);
    W32(LIGHT, 3);                         /* D3DLIGHT_DIRECTIONAL */
    f80 range = game_call_st0(c, SQRT_F, 1, R32(LIGHT_RANGE_SQ));
    WF32(LIGHT + 0x4c, (float)range);      /* Range */
    set_rs(c, RS_NORMALIZENORMALS, 1);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7ab9e8, dc_7ab9e8);
