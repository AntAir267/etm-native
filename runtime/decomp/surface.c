/* THE WORLD'S SURFACE STATE (DESIGN.md "The drawing code's rewrite"): what a world batch's strip
   sets before it is drawn (0x7a3783): its material's textures, texture matrices, stage setup,
   texture factor, blend mode, alpha test and culling (0x7a68f0, 0x7a6fd0), the depth bias for
   decals (0x78ba44), and two small helpers of the renderer (0x7a6230, 0x7a6250). Each sets
   Direct3D state or copies; the texture matrices (MATERIAL_TEX0/1) are the only thing read here
   that may move between updates (scrolling textures) [guess: what writes them]. */
#include "world.h"

/* a material's state (the view's material table's +0x68) */
#define MATS_FLAGS             0x0u       /* 1 alpha test, 2 two-sided */
#define MATS_TEXTURE0          0x4u       /* IDirect3DTexture9 *, stages 0 and 1 */
#define MATS_TEXTURE1          0x8u
#define MATS_STAGES            0xcu       /* 0 colour x factor, 1 x texture, 2 x texture with alpha */
#define MATS_FACTOR            0x10u      /* the texture factor (ARGB) */
#define MATS_BLEND             0x14u      /* the blend mode (0x7a6fd0) */
#define MATS_TEX0              0x18u      /* 4x4: texture matrix of stage 0, then of stage 1 (+0x58) */
#define MATS_TEX1              0x58u

#define BLEND_MODE             0x7a6fd0u
#define BLEND_CURRENT          0x994a7cu  /* the blend mode set (0 none) */
#define DEPTH_BIAS_CAPS        0x993998u  /* byte: the device takes a depth bias */
#define K_BIAS_SCALE           0x8fcdd4u  /* .rdata float -0.0001 */

/* texture arguments and operations */
#define TA_DIFFUSE             0u
#define TA_CURRENT             1u
#define TA_TEXTURE             2u
#define TA_TFACTOR             3u
#define TOP_DISABLE            1u
#define TOP_SELECTARG1         2u
#define TOP_MODULATE           4u

/* 0x7a68f0 (material state): a world strip's material: textures 0 and 1, their texture matrices,
   the blend mode (0x7a6fd0), the texture stages (0: diffuse x factor; 1: diffuse x texture, then
   x factor, alpha diffuse x texture; 2: as 1, alpha x factor too), the texture factor, the alpha
   test (alpha >= 0x80) or none, two-sided or clockwise culling. */
static int dc_7a68f0(CPU *c)
{
    uint32_t e = c->esp, m = ARG(0);
    D3DCALL(c, D3D_SET_TEXTURE, 2, 0, R32(m + MATS_TEXTURE0));
    D3DCALL(c, D3D_SET_TEXTURE, 2, 1, R32(m + MATS_TEXTURE1));
    set_transform(c, D3DTS_TEXTURE0, m + MATS_TEX0);
    set_transform(c, D3DTS_TEXTURE1, m + MATS_TEX1);
    game_call(c, BLEND_MODE, 1, R32(m + MATS_BLEND));
    uint32_t stages = R32(m + MATS_STAGES);
    switch (stages) {
    case 0:
        set_tss(c, 0, TSS_COLORARG1, TA_DIFFUSE);
        set_tss(c, 0, TSS_COLORARG2, TA_TFACTOR);
        set_tss(c, 0, TSS_COLOROP, TOP_MODULATE);
        set_tss(c, 0, TSS_ALPHAARG1, TA_DIFFUSE);
        set_tss(c, 0, TSS_ALPHAARG2, TA_TFACTOR);
        set_tss(c, 0, TSS_ALPHAOP, TOP_MODULATE);
        set_tss(c, 1, TSS_COLOROP, TOP_DISABLE);
        set_tss(c, 1, TSS_ALPHAOP, TOP_DISABLE);
        set_rs(c, RS_TEXTUREFACTOR, R32(m + MATS_FACTOR));
        break;
    case 1:
    case 2:
        set_tss(c, 0, TSS_COLORARG1, TA_DIFFUSE);
        set_tss(c, 0, TSS_COLORARG2, TA_TEXTURE);
        set_tss(c, 0, TSS_COLOROP, TOP_MODULATE);
        set_tss(c, 0, TSS_ALPHAARG1, TA_DIFFUSE);
        set_tss(c, 0, TSS_ALPHAARG2, TA_TEXTURE);
        set_tss(c, 0, TSS_ALPHAOP, TOP_MODULATE);
        set_tss(c, 1, TSS_COLORARG1, TA_CURRENT);
        set_tss(c, 1, TSS_COLORARG2, TA_TFACTOR);
        set_tss(c, 1, TSS_COLOROP, TOP_MODULATE);
        set_tss(c, 1, TSS_ALPHAARG1, TA_CURRENT);
        if (stages == 1)
            set_tss(c, 1, TSS_ALPHAOP, TOP_SELECTARG1);
        else {
            set_tss(c, 1, TSS_ALPHAARG2, TA_TFACTOR);
            set_tss(c, 1, TSS_ALPHAOP, TOP_MODULATE);
        }
        set_tss(c, 2, TSS_COLOROP, TOP_DISABLE);
        set_tss(c, 2, TSS_ALPHAOP, TOP_DISABLE);
        set_rs(c, RS_TEXTUREFACTOR, R32(m + MATS_FACTOR));
        break;
    }
    if (R32(m + MATS_FLAGS) & 1) {
        set_rs(c, RS_ALPHATESTENABLE, 1);
        set_rs(c, RS_ALPHAREF, 0x80);
        set_rs(c, RS_ALPHAFUNC, 7);        /* D3DCMP_GREATEREQUAL */
    } else {
        set_rs(c, RS_ALPHATESTENABLE, 0);
        set_rs(c, RS_ALPHAFUNC, 8);        /* D3DCMP_ALWAYS */
    }
    set_rs(c, RS_CULLMODE, (R32(m + MATS_FLAGS) & 2) ? 1 : 2);   /* D3DCULL_NONE, CW */
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a68f0, dc_7a68f0);

/* 0x7a6fd0 (mode): the blend mode: 0 none (depth writes on); 1 additive (source alpha + one,
   depth writes off); 2 alpha (source alpha, inverse source alpha); others blending on only.
   Each with less-or-equal depth test, kept in BLEND_CURRENT. */
static int dc_7a6fd0(CPU *c)
{
    uint32_t e = c->esp, mode = ARG(0);
    if (mode) {
        W32(BLEND_CURRENT, mode);
        set_rs(c, RS_ALPHABLENDENABLE, 1);
        if (mode == 1) {
            set_rs(c, RS_SRCBLEND, 5);     /* D3DBLEND_SRCALPHA */
            set_rs(c, RS_DESTBLEND, 2);    /* D3DBLEND_ONE */
            set_rs(c, RS_ZWRITEENABLE, 0);
            set_rs(c, RS_ZFUNC, 4);
        } else if (mode == 2) {
            set_rs(c, RS_SRCBLEND, 5);
            set_rs(c, RS_DESTBLEND, 6);    /* D3DBLEND_INVSRCALPHA */
            set_rs(c, RS_ZFUNC, 4);
        }
    } else {
        set_rs(c, RS_ALPHABLENDENABLE, 0);
        W32(BLEND_CURRENT, 0);
        set_rs(c, RS_ZWRITEENABLE, 1);
        set_rs(c, RS_ZFUNC, 4);
    }
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a6fd0, dc_7a6fd0);

/* 0x78ba44 (float bias): a depth bias of bias x -0.0001, when the device takes one. */
static int dc_78ba44(CPU *c)
{
    uint32_t e = c->esp, bias = ARG(0);
    if (!R8(DEPTH_BIAS_CAPS))
        return decomp_ret(c, e, 0, 0);
    set_rs(c, RS_DEPTHBIAS, f2u((float)((f80)RF32(K_BIAS_SCALE) * (f80)u2f(bias))));
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(78ba44, dc_78ba44);

/* 0x7a6230: no vertex format cached (0x78ad3c sets the next one whatever it is). */
static int dc_7a6230(CPU *c)
{
    uint32_t e = c->esp;
    W32(FVF_CACHE, 0xffffffffu);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a6230, dc_7a6230);

/* 0x7a6250 (this = 4x4; src): this = src (64 bytes, CRT memcpy) -> this. */
static int dc_7a6250(CPU *c)
{
    uint32_t e = c->esp, self = c->ecx, src = ARG(0);
    game_call(c, MEMCPY, 3, self, src, 0x40);
    return decomp_ret(c, e, self, 1);
}
DECOMP(7a6250, dc_7a6250);
