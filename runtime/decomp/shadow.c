/* SHADOW RECEIVERS (DESIGN.md "The drawing code's rewrite"): world batches the level's shadow
   projectors fall on, drawn again after the translucent pass (0x6ea603, kind-2 records) with each
   projector's shadow texture projected onto them by vertex and pixel shaders (0x7a4a5e). Needs
   shader support (0x78bab9) and at least one projector: on the lobby route neither holds, so only
   the test at its start runs [verified: tools/calltree.sh 6465c8 6800: 25 calls, each only
   0x78bab9]; the rest is transcribed from the disassembly.

   What moves between updates: the projectors' matrices (SHP_MATRIX, from the characters they
   follow) and intensities, the camera (CAM_VIEW, CAM_PROJECTION), the batch's world matrix. */
#include <string.h>
#include "world.h"

#define LEVEL_SHADOWS          0x1c8u     /* the level's shadow projectors record: */
#define SH_COUNT               0x8u       /* projectors (at most 2 used) */
#define SH_PROJECTOR           0x1c4u     /* the first projector, 0x1e8 bytes each: */
#define SHP_SIZE               0x1e8u
#define SHP_INTENSITY          0x0u       /* float, 0-255 */
#define SHP_MATRIX             0xe4u      /* 4x4: world to the projector's texture */
#define SHADOW_TEXTURES        0x994a9cu  /* IDirect3DTexture9 * per projector */

#define SHADERS_SUPPORTED      0x78bab9u  /* () -> al: vertex and pixel shaders 1.1 */
#define SHADER_SELECT          0x78bd53u  /* (index): the renderer's shader pair */
#define MAT_INIT               0x7a68d0u  /* (this = 4x4): constructor (does nothing) */
#define MAT_TRANSPOSED         0x7a71d0u  /* (this = 4x4; out) -> out = this transposed */
#define ATEXIT                 0x8ab25fu  /* CRT atexit */
#define TEX_BIAS_DTOR          0x7a51c4u  /* the static matrix's destructor, for atexit */
#define BLEND_MODE             0x7a6fd0u
#define DEPTH_BIAS             0x78ba44u
#define D3D_SET_VERTEX_SHADER  0x170u

/* the vertex shader constants c83-c95 (13 vectors from 0xe75dd8) and the static bias matrix */
#define VS_ONES                0xe75dd8u  /* c83 (1, 1, 1, 1) */
#define VS_SHADOW1             0xe75de8u  /* c84-c87: projector 1, transposed */
#define VS_SHADOW0             0xe75e28u  /* c88-c91: projector 0, transposed */
#define VS_WVP                 0xe75e68u  /* c92-c95: world x view x projection, transposed */
#define TEX_BIAS               0xe75eb0u  /* texture space: x / 2 + 1/2, -y / 2 + 1/2 */
#define TEX_BIAS_INIT          0xe75ea9u  /* bit 0: TEX_BIAS constructed */
#define IDENTITY               0xe7c7b8u  /* a 4x4 TEX_BIAS starts from [guess: the identity] */
#define K_255                  0x8fa1e0u  /* .rdata floats 255, 0, 1 */
#define K_0                    0x8fa020u
#define K_1                    0x8fa05cu

/* projector i's matrix into dst: (batch world x projector) x bias, transposed */
static void shadow_matrix(CPU *c, uint32_t b, uint32_t pm, uint32_t dst)
{
    uint32_t F = dc_frame(c, 0x140);
    uint32_t tp = F, tw = F + 0x40, m1 = F + 0x80, m2 = F + 0xc0, tr = F + 0x100;
    uint32_t r1 = game_callm(c, MAT_COPY, tp, 1, pm);
    uint32_t r2 = game_callm(c, MAT_COPY, tw, 1, b);
    game_callm(c, MAT_MUL, r2, 2, m1, r1);
    uint32_t r = game_callm(c, MAT_MUL, m1, 2, m2, TEX_BIAS);
    memcpy(gptr(dst), gptr(r), 64);
    r = game_callm(c, MAT_TRANSPOSED, dst, 1, tr);
    memcpy(gptr(dst), gptr(r), 64);
    dc_unframe(c, F, 0x140);
}

/* 0x7a4a5e (view, batch) (SVC_SHADOW_RECEIVER): one shadow receiver, with shaders and projectors
   only: fog off, shader pair 0, the vertex shader constants (each projector's texture matrix
   when its intensity is above 0, world x view x projection, ones), a pixel shader constant per
   projector (its intensity / 255 kept within 0..1, 0 for the unused one), then the batch's strips
   blended over the world (depth bias 2, no depth writes, no culling) with each projector's
   shadow texture projected, clamped and filtered on its stage. The usual state back after. */
static int dc_7a4a5e(CPU *c)
{
    uint32_t e = c->esp, b = ARG(1);
    uint32_t level = R32(LEVEL_PTR);
    if (!R32(R32(level + LEVEL_SHADOWS) + SH_COUNT) || !(game_call(c, SHADERS_SUPPORTED, 0) & 0xff))
        return decomp_ret(c, e, c->eax, 0);
    set_rs(c, RS_FOGENABLE, 0);
    int32_t n = (int32_t)R32(R32(level + LEVEL_SHADOWS) + SH_COUNT);
    uint32_t geom = R32(b + 0x78), buf = R32(geom + 0x128);   /* BATCH_GEOMETRY, GEOM_BUFFERS */
    game_call(c, SHADER_SELECT, 1, 0);
    if (!(R8(TEX_BIAS_INIT) & 1)) {
        W8(TEX_BIAS_INIT, R8(TEX_BIAS_INIT) | 1);
        game_callm(c, MAT_INIT, TEX_BIAS, 0);
        game_call(c, ATEXIT, 1, TEX_BIAS_DTOR);
    }
    memcpy(gptr(TEX_BIAS), gptr(IDENTITY), 64);
    W32(TEX_BIAS + 0x00, 0x3f000000u);     /* 0.5 */
    W32(TEX_BIAS + 0x14, 0xbf000000u);     /* -0.5 */
    W32(TEX_BIAS + 0x30, 0x3f000000u);
    W32(TEX_BIAS + 0x34, 0x3f000000u);
    uint32_t p0 = R32(level + LEVEL_SHADOWS) + SH_PROJECTOR;
    if ((f80)RF32(p0 + SHP_INTENSITY) > (f80)0.0f)
        shadow_matrix(c, b, R32(level + LEVEL_SHADOWS) + SH_PROJECTOR + SHP_MATRIX, VS_SHADOW0);
    if (n > 1 && (f80)RF32(R32(level + LEVEL_SHADOWS) + SH_PROJECTOR + SHP_SIZE + SHP_INTENSITY) > (f80)0.0f)
        shadow_matrix(c, b, R32(level + LEVEL_SHADOWS) + SH_PROJECTOR + SHP_SIZE + SHP_MATRIX, VS_SHADOW1);

    /* world x view x projection, transposed */
    uint32_t F = dc_frame(c, 0x180);
    uint32_t tv = F, tw = F + 0x40, wv = F + 0x80, tp = F + 0xc0, wvp = F + 0x100, tr = F + 0x140;
    uint32_t r1 = game_callm(c, MAT_COPY, tv, 1, CAM_VIEW);
    uint32_t r2 = game_callm(c, MAT_COPY, tw, 1, b);
    game_callm(c, MAT_MUL, r2, 2, wv, r1);
    uint32_t r3 = game_callm(c, MAT_COPY, tp, 1, CAM_PROJECTION);
    uint32_t r = game_callm(c, MAT_MUL, wv, 2, wvp, r3);
    memcpy(gptr(VS_WVP), gptr(r), 64);
    r = game_callm(c, MAT_TRANSPOSED, VS_WVP, 1, tr);
    memcpy(gptr(VS_WVP), gptr(r), 64);
    dc_unframe(c, F, 0x180);
    for (int k = 3; k >= 0; k--)
        W32(VS_ONES + 4u * (uint32_t)k, 0x3f800000u);
    set_vs_const(c, 83, VS_ONES, 13);

    /* each projector's strength: its intensity / 255, kept within 0..1; 0 for one not in use */
    for (int32_t i = 0; i < 2; i++) {
        f80 v = (f80)RF32(R32(level + LEVEL_SHADOWS) + (uint32_t)i * SHP_SIZE + SH_PROJECTOR + SHP_INTENSITY)
                / (f80)RF32(K_255) - (f80)0.0f;
        uint32_t s = v < (f80)RF32(K_0) ? 0 : f2u((float)v);
        v = (f80)u2f(s) / ((f80)RF32(K_1) - (f80)0.0f);
        s = v > (f80)RF32(K_1) ? 0x3f800000u : f2u((float)v);
        if (i >= n)
            s = 0;
        uint32_t G = dc_frame(c, 16);
        W32(G, s);
        W32(G + 4, s);
        W32(G + 8, s);
        W32(G + 12, s);
        D3DCALL(c, D3D_SET_PS_CONST, 3, (uint32_t)i, G, 1);
        dc_unframe(c, G, 16);
    }

    game_call(c, DEPTH_BIAS, 1, 0x40000000u);      /* 2 */
    set_rs(c, RS_ZFUNC, 4);
    set_rs(c, RS_ZWRITEENABLE, 0);
    set_rs(c, RS_CULLMODE, 1);             /* D3DCULL_NONE */
    game_call(c, BLEND_MODE, 1, 2);
    for (int32_t i = 0; i < n; i++) {
        D3DCALL(c, D3D_SET_TEXTURE, 2, (uint32_t)i, R32(SHADOW_TEXTURES + 4u * (uint32_t)i));
        set_ss(c, (uint32_t)i, SAMP_ADDRESSU, 3);   /* D3DTADDRESS_CLAMP */
        set_ss(c, (uint32_t)i, SAMP_ADDRESSV, 3);
        set_tss(c, (uint32_t)i, TSS_TEXTURETRANSFORMFLAGS, 0x100);  /* D3DTTFF_PROJECTED */
        set_ss(c, (uint32_t)i, SAMP_MAGFILTER, 2);  /* D3DTEXF_LINEAR */
        set_ss(c, (uint32_t)i, SAMP_MINFILTER, 2);
        set_ss(c, (uint32_t)i, SAMP_MIPFILTER, 2);
    }
    D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 0, R32(buf + 0xc), 0, R32(buf + 0x4));   /* BUF_VB, BUF_STRIDE */
    D3DCALL(c, D3D_SET_INDICES, 1, R32(buf + 0x14));                            /* BUF_IB */
    uint32_t start = 0;
    if (R32(geom + 0x94)) {                /* GEOM_HAS_STRIPS: the strips, as 0x7a3783 draws them */
        uint32_t sp = R32(geom + 0xf0);
        for (int32_t i = 0; i < (int32_t)R32(geom + 0x90); i++) {
            uint32_t len = R16(sp) + 2;
            sp += 2;
            D3DCALL(c, D3D_DRAW_INDEXED, 6, 5, 0, 0, R32(geom + 0x84), start, len - 2);
            sp += len * 2;
            start += len;
        }
    }
    set_rs(c, RS_ZFUNC, 4);
    set_rs(c, RS_ZWRITEENABLE, 1);
    set_ss(c, 0, SAMP_ADDRESSU, 1);
    set_ss(c, 0, SAMP_ADDRESSV, 1);
    set_ss(c, 1, SAMP_ADDRESSU, 1);
    set_ss(c, 1, SAMP_ADDRESSV, 1);
    D3DCALL(c, D3D_SET_VERTEX_SHADER, 1, 0);
    D3DCALL(c, D3D_SET_PIXEL_SHADER, 1, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a4a5e, dc_7a4a5e);
