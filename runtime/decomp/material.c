/* MATERIAL STATE for the objects' model parts (object submission, DESIGN.md "The drawing code's
   rewrite"): a part's blending, culling and texture stages (0x7ad508, with 0x7ae2d8 for the blend
   mode), the vertex declaration (0x78ad3c), the depth bias reset (0x78ba81) and the shader-support
   test (0x78bab9). Each sets Direct3D state only; nothing here moves between updates. */
#include "objsubmit.h"

/* texture stage states (D3DTEXTURESTAGESTATETYPE) and their values */
#define TSS_COLOROP      1u
#define TSS_COLORARG1    2u
#define TSS_COLORARG2    3u
#define TSS_ALPHAOP      4u
#define TSS_ALPHAARG1    5u
#define TSS_ALPHAARG2    6u
#define TSS_TEXCOORDINDEX 11u
#define TSS_TTFLAGS      24u        /* TEXTURETRANSFORMFLAGS */
#define TSS_COLORARG0    26u
#define TOP_DISABLE      1u
#define TOP_SELECTARG1   2u
#define TOP_MODULATE     4u
#define TOP_MULTIPLYADD  25u
#define TA_DIFFUSE       0u
#define TA_CURRENT       1u
#define TA_TEXTURE       2u
#define TCI_CAMERASPACENORMAL 0x10000u
#define TTF_COUNT2       2u
/* sampler states and filters */
#define SAMP_MAGFILTER   5u
#define SAMP_MINFILTER   6u
#define SAMP_MIPFILTER   7u
#define SAMP_MAXANISOTROPY 10u
#define TEXF_POINT       1u
#define TEXF_LINEAR      2u
#define TEXF_ANISOTROPIC 3u
#define CULL_NONE        1u
#define CULL_CCW         3u
#define BLEND_ONE        2u
#define BLEND_SRCALPHA   5u
#define BLEND_INVSRCALPHA 6u

static uint32_t tss(CPU *c, uint32_t stage, uint32_t type, uint32_t value)
{
    return D3DCALL(c, D3D_SET_TSS, 3, stage, type, value);
}
static uint32_t samp(CPU *c, uint32_t type, uint32_t value)
{
    return D3DCALL(c, D3D_SET_SAMPLER_STATE, 3, 0, type, value);
}
static uint32_t rs(CPU *c, uint32_t state, uint32_t value)
{
    return D3DCALL(c, D3D_SET_RENDER_STATE, 2, state, value);
}

/* ---------------------------------------------------------------- 0x78ad3c */

#define DECL_CURRENT     0x94234cu  /* the vertex declaration index last set (0x78ad3c's cache) */
#define DECL_TABLE       0x994a30u  /* 12-byte entries, the declaration (IDirect3DVertexDeclaration9 *) first */

/* 0x78ad3c (index): selects the vertex declaration of table entry index (bit 31 ignored) for
   fixed-function drawing (no vertex or pixel shader), unless it is the one set last. */
static int dc_78ad3c(CPU *c)
{
    uint32_t e = c->esp, i = ARG(0), eax = R32(DECL_CURRENT);
    if (eax != i) {
        D3DCALL(c, D3D_SET_VERTEX_SHADER, 1, 0);
        D3DCALL(c, D3D_SET_PIXEL_SHADER, 1, 0);
        eax = D3DCALL(c, D3D_SET_VERTEX_DECL, 1, R32(DECL_TABLE + (i & 0x7fffffffu) * 12));
        W32(DECL_CURRENT, i);
    }
    return decomp_ret(c, e, eax, 0);
}
DECOMP(78ad3c, dc_78ad3c);

/* ---------------------------------------------------------------- 0x78ba81, 0x78bab9 */

#define DEPTH_BIAS_ON    0x993998u  /* byte: the device takes a depth bias (0x7ad508 sets one for some parts) */

/* 0x78ba81: puts the depth bias back to 0 where the device takes one. */
static int dc_78ba81(CPU *c)
{
    uint32_t e = c->esp, eax = R8(DEPTH_BIAS_ON);
    if (eax)
        eax = rs(c, D3DRS_DEPTHBIAS, 0);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(78ba81, dc_78ba81);

#define CAPS_VS_VERSION  0x99341cu  /* D3DCAPS9.VertexShaderVersion, caps at 0x993358 [guess: the */
#define CAPS_PS_VERSION  0x993424u  /* offsets 0xc4 / 0xcc and the vs_1_1 / ps_1_1 constants] */
#define SHADERS_0x994404 0x994404u  /* nonzero: shaders allowed [guess] */

/* 0x78bab9: whether the device has vertex and pixel shaders 1.1 and 0x994404 is set (al; the rest
   of eax is the caller's). */
static int dc_78bab9(CPU *c)
{
    uint32_t e = c->esp;
    uint32_t r = R32(CAPS_VS_VERSION) >= 0xfffe0101u && R32(CAPS_PS_VERSION) >= 0xffff0101u
                 && R32(SHADERS_0x994404) != 0;
    return decomp_ret(c, e, (c->eax & ~0xffu) | r, 0);
}
DECOMP(78bab9, dc_78bab9);

/* ---------------------------------------------------------------- 0x7ae2d8 */

#define BLEND_CURRENT    0xe7bec4u  /* the blend mode last set (0x7ae2d8's cache; 0x7b1838 resets it to 0) */
#define TEX_FILTER       0x947f68u  /* texture filtering option: 0..2 linear, 3 anisotropic */
#define TEX_ANISOTROPY   0x947f6cu

/* 0x7ae2d8 (mode): the blend mode for the parts that follow, unless it is the current one:
   0 opaque (no blending, depth writes), 1 additive (source alpha, one), 2 alpha blended (source
   alpha, inverse source alpha); others blend with the factors left as they were. Each change also
   sets stage 0's filters: for mode 0 first linear or anisotropic as the option says, then point
   (the original's order: the option's filters never stay set). */
static int dc_7ae2d8(CPU *c)
{
    uint32_t e = c->esp, mode = ARG(0), eax;
    if (R32(BLEND_CURRENT) == mode)
        return decomp_ret(c, e, mode, 0);
    W32(BLEND_CURRENT, mode);
    if (mode == 0) {
        rs(c, D3DRS_ALPHABLENDENABLE, 0);
        rs(c, D3DRS_ZWRITEENABLE, 1);
        uint32_t f = R32(TEX_FILTER);
        if (f == 0 || f == 1 || f == 2) {
            samp(c, SAMP_MINFILTER, TEXF_LINEAR);
            samp(c, SAMP_MAGFILTER, TEXF_LINEAR);
            samp(c, SAMP_MIPFILTER, TEXF_LINEAR);
        } else if (f == 3) {
            samp(c, SAMP_MINFILTER, TEXF_ANISOTROPIC);
            samp(c, SAMP_MAGFILTER, TEXF_ANISOTROPIC);
            samp(c, SAMP_MIPFILTER, TEXF_ANISOTROPIC);
            samp(c, SAMP_MAXANISOTROPY, R32(TEX_ANISOTROPY));
        }
        samp(c, SAMP_MINFILTER, TEXF_POINT);
        samp(c, SAMP_MAGFILTER, TEXF_POINT);
        eax = samp(c, SAMP_MIPFILTER, TEXF_POINT);
    } else {
        rs(c, D3DRS_ALPHABLENDENABLE, 1);
        rs(c, D3DRS_ZWRITEENABLE, 0);
        samp(c, SAMP_MINFILTER, TEXF_POINT);
        samp(c, SAMP_MAGFILTER, TEXF_POINT);
        samp(c, SAMP_MIPFILTER, TEXF_POINT);
        eax = mode;
        if (mode == 1) {
            rs(c, D3DRS_SRCBLEND, BLEND_SRCALPHA);
            eax = rs(c, D3DRS_DESTBLEND, BLEND_ONE);
        } else if (mode == 2) {
            rs(c, D3DRS_SRCBLEND, BLEND_SRCALPHA);
            eax = rs(c, D3DRS_DESTBLEND, BLEND_INVSRCALPHA);
        }
    }
    return decomp_ret(c, e, eax, 0);
}
DECOMP(7ae2d8, dc_7ae2d8);

/* ---------------------------------------------------------------- 0x7ad508 */

#define MATERIAL_SKIP    0xe7bec0u  /* byte: leave the material state alone [what sets it: unknown] */
#define PART_TEXTURE     0x20u      /* the part's base texture record, or 0 */
#define PART_ENVMAP      0x28u      /* the part's environment (reflection) texture record, or 0 */
#define PART_OPACITY     0x2cu      /* float: chooses the blend mode */
#define TEXREC_D3D       0x30u      /* a texture record's IDirect3DTexture9 * */
#define TEXREC_TWO_SIDED 0x29u      /* byte: draw without culling [guess: from its use] */
#define OPAQUE_A         0xe8806cu  /* float: an opacity drawn opaque (and the depth-bias limit) [guess: 1.0] */
#define OPAQUE_B         0xe88068u  /* float: the other opacity drawn opaque */
#define ALPHA_LIMIT      0xe88060u  /* double: above it additive, at or below it alpha blended */
#define DEPTH_BIAS_VALUE 0x947ef0u  /* the depth bias for translucent single-texture parts */
#define ENVMAP_TRANSFORM 0xe7be80u  /* 4x4: the environment map's texture transform */
#define DEPTH_BIAS_RESET 0x78ba81u
#define BLEND_MODE       0x7ae2d8u

/* 0x7ad508 (part): a model part's material: the blend mode by its opacity (0x7ae2d8), no culling
   for a two-sided base texture, then texture stages for the base texture (stage 0 modulated by the
   vertex colour), the environment map alone (camera-space normals through ENVMAP_TRANSFORM), both
   (the map added in stage 1, multiply-add) or neither (vertex colour only). A translucent part
   with the base texture alone gets the depth bias where the device takes one; every other part
   has it put back to 0. */
static int dc_7ad508(CPU *c)
{
    uint32_t e = c->esp, part = ARG(0), eax;
    if (R8(MATERIAL_SKIP))
        return decomp_ret(c, e, (c->eax & ~0xffu) | R8(MATERIAL_SKIP), 0);
    float op = RF32(part + PART_OPACITY);
    if (RF32(OPAQUE_A) == op || RF32(OPAQUE_B) == op)  /* fcompp, equal and ordered */
        game_call(c, BLEND_MODE, 1, 0);
    else if (RF64(ALPHA_LIMIT) < op)
        game_call(c, BLEND_MODE, 1, 1);
    else                                                /* at or below the limit, or NaN */
        game_call(c, BLEND_MODE, 1, 2);
    uint32_t kind = (R32(part + PART_TEXTURE) != 0) | (R32(part + PART_ENVMAP) ? 2u : 0u);
    if (R32(part + PART_TEXTURE) && R8(R32(part + PART_TEXTURE) + TEXREC_TWO_SIDED))
        rs(c, D3DRS_CULLMODE, CULL_NONE);
    else
        rs(c, D3DRS_CULLMODE, CULL_CCW);
    int bias = 0;
    if (kind == 1) {                        /* base texture */
        tss(c, 0, TSS_COLOROP, TOP_MODULATE);
        tss(c, 0, TSS_COLORARG1, TA_TEXTURE);
        tss(c, 0, TSS_COLORARG2, TA_DIFFUSE);
        tss(c, 0, TSS_ALPHAOP, TOP_SELECTARG1);
        tss(c, 0, TSS_ALPHAARG1, TA_TEXTURE);
        tss(c, 1, TSS_COLOROP, TOP_DISABLE);
        tss(c, 1, TSS_ALPHAOP, TOP_DISABLE);
        tss(c, 0, TSS_TTFLAGS, 0);
        tss(c, 0, TSS_TEXCOORDINDEX, 0);
        tss(c, 1, TSS_TTFLAGS, 0);
        tss(c, 1, TSS_TEXCOORDINDEX, 0);
        D3DCALL(c, D3D_SET_TEXTURE, 2, 0, R32(R32(part + PART_TEXTURE) + TEXREC_D3D));
        eax = D3DCALL(c, D3D_SET_TEXTURE, 2, 1, 0);
        /* fld opacity, fld OPAQUE_A, fcompp, ja: OPAQUE_A above the opacity, ordered */
        if (R8(DEPTH_BIAS_ON) && RF32(OPAQUE_A) > RF32(part + PART_OPACITY)) {
            bias = 1;
            eax = rs(c, D3DRS_DEPTHBIAS, R32(DEPTH_BIAS_VALUE));
        }
    } else if (kind == 2) {                 /* environment map */
        tss(c, 0, TSS_COLOROP, TOP_MODULATE);
        tss(c, 0, TSS_COLORARG1, TA_TEXTURE);
        tss(c, 0, TSS_COLORARG2, TA_DIFFUSE);
        tss(c, 0, TSS_ALPHAOP, TOP_SELECTARG1);
        tss(c, 0, TSS_ALPHAARG1, TA_TEXTURE);
        tss(c, 1, TSS_COLOROP, TOP_DISABLE);
        tss(c, 1, TSS_ALPHAOP, TOP_DISABLE);
        tss(c, 0, TSS_TEXCOORDINDEX, TCI_CAMERASPACENORMAL);
        tss(c, 0, TSS_TTFLAGS, TTF_COUNT2);
        D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_TEXTURE0, ENVMAP_TRANSFORM);
        D3DCALL(c, D3D_SET_TEXTURE, 2, 0, R32(R32(part + PART_ENVMAP) + TEXREC_D3D));
        eax = D3DCALL(c, D3D_SET_TEXTURE, 2, 1, 0);
    } else if (kind == 3) {                 /* base texture plus environment map */
        tss(c, 0, TSS_COLOROP, TOP_MODULATE);
        tss(c, 0, TSS_COLORARG1, TA_TEXTURE);
        tss(c, 0, TSS_COLORARG2, TA_DIFFUSE);
        tss(c, 0, TSS_ALPHAOP, TOP_SELECTARG1);
        tss(c, 0, TSS_ALPHAARG1, TA_TEXTURE);
        tss(c, 1, TSS_COLOROP, TOP_MULTIPLYADD);
        tss(c, 1, TSS_COLORARG0, TA_CURRENT);
        tss(c, 1, TSS_COLORARG1, TA_TEXTURE);
        tss(c, 1, TSS_COLORARG2, TA_DIFFUSE);
        tss(c, 1, TSS_ALPHAOP, TOP_MODULATE);
        tss(c, 1, TSS_ALPHAARG1, TA_TEXTURE);
        tss(c, 1, TSS_ALPHAARG2, TA_DIFFUSE);
        tss(c, 2, TSS_COLOROP, TOP_DISABLE);
        tss(c, 2, TSS_ALPHAOP, TOP_DISABLE);
        tss(c, 0, TSS_TTFLAGS, 0);
        tss(c, 0, TSS_TEXCOORDINDEX, 0);
        tss(c, 1, TSS_TEXCOORDINDEX, TCI_CAMERASPACENORMAL);
        tss(c, 1, TSS_TTFLAGS, TTF_COUNT2);
        D3DCALL(c, D3D_SET_TRANSFORM, 2, D3DTS_TEXTURE1, ENVMAP_TRANSFORM);
        D3DCALL(c, D3D_SET_TEXTURE, 2, 0, R32(R32(part + PART_TEXTURE) + TEXREC_D3D));
        eax = D3DCALL(c, D3D_SET_TEXTURE, 2, 1, R32(R32(part + PART_ENVMAP) + TEXREC_D3D));
    } else {                                /* untextured */
        D3DCALL(c, D3D_SET_TEXTURE, 2, 0, 0);
        D3DCALL(c, D3D_SET_TEXTURE, 2, 1, 0);
        tss(c, 0, TSS_COLOROP, TOP_SELECTARG1);
        tss(c, 0, TSS_COLORARG1, TA_TEXTURE);
        tss(c, 0, TSS_ALPHAOP, TOP_SELECTARG1);
        tss(c, 0, TSS_ALPHAARG1, TA_TEXTURE);
        tss(c, 1, TSS_COLOROP, TOP_DISABLE);
        eax = tss(c, 1, TSS_ALPHAOP, TOP_DISABLE);
    }
    if (bias)
        eax = (eax & ~0xffu) | 1;
    else
        eax = game_call(c, DEPTH_BIAS_RESET, 0);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(7ad508, dc_7ad508);
