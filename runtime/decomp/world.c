/* THE WORLD: the level's own geometry as each view draws it (DESIGN.md "The drawing code's
   rewrite"). The render pass calls 0x747b66 per view: the renderer's default state (0x7a2f26),
   every visible world mesh (0x7a3531: each of its batches drawn now by 0x7a3783, or queued by
   depth when it has to be drawn later), then the state after the world (0x7a3472). Later in the
   pass 0x6ea603 draws what was queued: some batches first, the translucent ones far to near, then
   the shadow receivers (0x7a4a5e in shadow.c).

   What moves between updates [verified: the reads below]: the camera (CAM_VIEW, CAM_PROJECTION,
   read for every batch), each batch's own 4x4 (its +0, the world transform: most world batches
   never move, the movers are the level's animated parts), the level's clock (LEVEL_PTR +8, a
   vertex shader constant), and the queue's depths (computed from the view each frame). */
#include "world.h"

/* ------------------------------------------------------------------ the world, per view */

#define WORLD_REBUILT          0x7489bcu  /* (view, flag): called when the settings' +0x1c4 changes */
#define MESH_DRAW_DEFAULT      0x747cd3u  /* (view, mesh): without the renderer's own mesh drawer */
#define WORLD_AFTER            0x7473cbu  /* (): when the settings' +0x18c is set */
#define SET_WORLD_CHANGED      0x1c4u     /* settings: a world state, and the one last seen (+0x1c8) */
#define SET_WORLD_SEEN         0x1c8u
#define SET_WORLD_ON           0x108u     /* settings: the world's meshes are drawn */
#define SET_WORLD_AFTER        0x18cu
#define MESH_FLAGS             0x0u       /* mesh: flags (4: skipped here) */

/* 0x747b66 (view): the world, for one view. When the settings' world state changed since the last
   view, 0x7489bc is told first. Then the renderer's default state, each of the camera's meshes
   (last to first) not flagged 4 through the renderer's mesh drawer (0x7a3531), the state after
   the world, and 0x7473cb when the settings ask. A renderer world drawer (SVC_DRAW_WORLD), when
   set, draws everything instead (and nothing after it runs). */
static int dc_747b66(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0);
    if (R32(SETTINGS) && R32(R32(SETTINGS) + SET_WORLD_SEEN) != R32(R32(SETTINGS) + SET_WORLD_CHANGED)) {
        /* (the flag's upper bytes are the settings pointer's in the original; 0x7489bc reads the
           low byte only) */
        game_call(c, WORLD_REBUILT, 2, view, R32(R32(SETTINGS) + SET_WORLD_CHANGED) != 0);
        W32(R32(SETTINGS) + SET_WORLD_SEEN, R32(R32(SETTINGS) + SET_WORLD_CHANGED));
    }
    /* (the original reads CAM_VIEWREC into a local it never uses) */
    if (R32(SVC_BEGIN))
        game_call(c, R32(SVC_BEGIN), 2, view, 1);
    if (R32(R32(SETTINGS) + SET_WORLD_ON)) {
        if (R32(SVC_DRAW_WORLD)) {
            game_call(c, R32(SVC_DRAW_WORLD), 1, view);
            return decomp_ret(c, e, c->eax, 0);
        }
        for (int32_t i = (int32_t)R32(CAM_MESH_COUNT) - 1; i >= 0; i--) {
            uint32_t mesh = R32(CAM_MESHES + 4u * (uint32_t)i);
            if (R32(mesh + MESH_FLAGS) & 4)
                continue;
            if (R32(SVC_DRAW_MESH))
                game_call(c, R32(SVC_DRAW_MESH), 2, view, mesh);
            else
                game_call(c, MESH_DRAW_DEFAULT, 2, view, mesh);
        }
    }
    if (R32(SVC_END))
        game_call(c, R32(SVC_END), 2, view, 1);
    if (R32(R32(SETTINGS) + SET_WORLD_AFTER))
        game_call(c, WORLD_AFTER, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(747b66, dc_747b66);

/* ------------------------------------------------------------------ scene state */

#define SET_VIEW_MODE          0xf8u      /* settings: 4 and 5 keep the projection as it is */
#define SET_FOG_RANGE          0x200u     /* settings: set to (0x947f90 != 0) by 0x7a2f26 */
#define PROJECTION_FOR         0x7a3327u  /* (wide): the projection for the next meshes */
#define PROJECTION             0x62a236u  /* (4x4 out, width, height, focal, far, aspect): game.c hooks it */
#define PROJECTION_SET         0x7a2df4u  /* (4x4, 5 values): the projection for Direct3D */
#define FVF_CACHE_RESET        0x7a6230u
#define K_1000                 0x8fa158u  /* .rdata float 1000 */
#define FILTER_MODE            0x947f68u  /* texture filtering: 0, 1 point, 2 linear, 3 anisotropic */
#define MAX_ANISOTROPY         0x947f6cu
#define FOG_RANGE_SETTING      0x947f90u
#define GLOBAL_9475fc          0x9475fcu  /* set to 7 with the default state [guess: a state cache] */
#define PROJ_ARGS              0x97d6b0u  /* five values 0x7a2df4 gets (+0xc, +0x10, +0, +8, +4) */

/* 0x7a3327 (wide): the projection for the next meshes (0x62a236 into CAM_PROJECTION): the camera's
   size, its focal value, and its far distance (CAM_FAR_WIDE when wide, a byte, else CAM_FAR; both
   at least 1000, and at least the focal value + 1000). Wide meshes draw with depth as Z, the
   others with the W-buffer setting. Returns 1 (and does nothing) in view modes 4 and 5, else 0
   when not wide, 1 when wide. */
static int dc_7a3327(CPU *c)
{
    uint32_t e = c->esp, wide = ARG(0) & 0xff;
    if (R32(R32(SETTINGS) + SET_VIEW_MODE) == 4 || R32(R32(SETTINGS) + SET_VIEW_MODE) == 5)
        return decomp_ret(c, e, 1, 0);
    if ((f80)RF32(CAM_FAR_WIDE) < (f80)RF32(K_1000))
        W32(CAM_FAR_WIDE, 0x447a0000u);    /* 1000 */
    if ((f80)RF32(CAM_FAR) < (f80)RF32(K_1000))
        W32(CAM_FAR, 0x447a0000u);
    uint32_t far = wide ? R32(CAM_FAR_WIDE) : R32(CAM_FAR);
    uint32_t focal = f2u((float)((f80)RF32(CAM_FOCAL) / (f80)RF32(SVC_FOCAL_DIV)));
    if ((f80)u2f(far) < (f80)u2f(focal))
        far = f2u((float)((f80)u2f(focal) + (f80)RF32(K_1000)));
    W8(USE_W_BUFFER, wide ? 0 : R8(USE_W_BUFFER_DEFAULT));
    f80 w = (f80)(int32_t)R32(CAM_WIDTH), h = (f80)(int32_t)R32(CAM_HEIGHT);
    game_call(c, PROJECTION, 6, CAM_PROJECTION, f2u((float)w), f2u((float)h), focal, far, R32(SVC_ASPECT));
    return decomp_ret(c, e, wide ? 1 : 0, 0);
}
DECOMP(7a3327, dc_7a3327);

/* 0x7a3472 (SVC_END): the state after the world: alpha test off (always passes), lights 0-2 and
   lighting off, depth writes on, no depth bias, the normal projection (0x7a3327). Returns
   0x7a3327's result. */
static int dc_7a3472(CPU *c)
{
    uint32_t e = c->esp;
    set_rs(c, RS_ALPHATESTENABLE, 0);
    set_rs(c, RS_ALPHAFUNC, 8);            /* D3DCMP_ALWAYS */
    light_enable(c, 0, 0);
    light_enable(c, 1, 0);
    light_enable(c, 2, 0);
    set_rs(c, RS_LIGHTING, 0);
    set_rs(c, RS_ZWRITEENABLE, 1);
    game_call(c, DEPTH_BIAS_OFF, 0);
    game_call(c, PROJECTION_FOR, 1, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a3472, dc_7a3472);

/* 0x7a2f26 (SVC_BEGIN): the scene's default state: no cached vertex format, the camera's view
   matrix and projection (0x7a2df4), depth writes unless in the translucent pass, depth test (as W
   when set), less-or-equal, clockwise culling, no blending; stages 0 and 1 on their first texture
   coordinates with 2D texture transforms, wrapping; the filter setting's filters on stage 0;
   lighting off with vertex colours as the ambient source; the settings' +0x200 from 0x947f90. */
static int dc_7a2f26(CPU *c)
{
    uint32_t e = c->esp;
    game_call(c, FVF_CACHE_RESET, 0);
    set_transform(c, D3DTS_VIEW, CAM_VIEW);
    game_call(c, PROJECTION_SET, 6, CAM_PROJECTION, R32(PROJ_ARGS + 0xc), R32(PROJ_ARGS + 0x10),
              R32(PROJ_ARGS), R32(PROJ_ARGS + 8), R32(PROJ_ARGS + 4));
    set_rs(c, RS_ZWRITEENABLE, R8(TRANSLUCENT_PASS) ? 0 : 1);
    set_rs(c, RS_ZENABLE, R8(USE_W_BUFFER) ? 2 : 1);
    set_rs(c, RS_ZFUNC, 4);                /* D3DCMP_LESSEQUAL */
    set_rs(c, RS_CULLMODE, 2);             /* D3DCULL_CW */
    set_rs(c, RS_ALPHABLENDENABLE, 0);
    set_tss(c, 0, TSS_TEXCOORDINDEX, 0);
    set_tss(c, 1, TSS_TEXCOORDINDEX, 0);
    set_tss(c, 0, TSS_TEXTURETRANSFORMFLAGS, 2);   /* D3DTTFF_COUNT2 */
    set_tss(c, 1, TSS_TEXTURETRANSFORMFLAGS, 2);
    set_ss(c, 0, SAMP_ADDRESSU, 1);        /* D3DTADDRESS_WRAP */
    set_ss(c, 1, SAMP_ADDRESSU, 1);
    set_ss(c, 0, SAMP_ADDRESSV, 1);
    set_ss(c, 1, SAMP_ADDRESSV, 1);
    uint32_t f = R32(FILTER_MODE);         /* 0, 1 point; 2 linear; 3 anisotropic; others none */
    if (f <= 3) {
        uint32_t t = f <= 1 ? 1 : f;       /* D3DTEXF_POINT, LINEAR, ANISOTROPIC */
        set_ss(c, 0, SAMP_MAGFILTER, t);
        set_ss(c, 0, SAMP_MINFILTER, t);
        set_ss(c, 0, SAMP_MIPFILTER, t);
        if (f == 3)
            set_ss(c, 0, SAMP_MAXANISOTROPY, R32(MAX_ANISOTROPY));
    }
    W32(GLOBAL_9475fc, 7);
    set_rs(c, RS_LIGHTING, 0);
    set_rs(c, RS_COLORVERTEX, 1);
    set_rs(c, RS_AMBIENT, 0xffffffffu);
    set_rs(c, RS_AMBIENTMATERIALSOURCE, 1);    /* D3DMCS_COLOR1 */
    set_rs(c, RS_DIFFUSEMATERIALSOURCE, 0);    /* D3DMCS_MATERIAL */
    set_rs(c, RS_EMISSIVEMATERIALSOURCE, 0);
    set_rs(c, RS_SPECULARMATERIALSOURCE, 0);
    W32(R32(SETTINGS) + SET_FOG_RANGE, R32(FOG_RANGE_SETTING) ? 1 : 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a2f26, dc_7a2f26);

/* 0x7c1317 (test, write) (SVC_DEPTH; bytes): the depth buffer's use: writes on or off, the test on
   (as W when set) or off. */
static int dc_7c1317(CPU *c)
{
    uint32_t e = c->esp, test = ARG(0) & 0xff, write = ARG(1) & 0xff;
    set_rs(c, RS_ZWRITEENABLE, write ? 1 : 0);
    set_rs(c, RS_ZENABLE, test ? (R8(USE_W_BUFFER) ? 2 : 1) : 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7c1317, dc_7c1317);

/* ------------------------------------------------------------------ meshes and batches */

/* a world mesh (what CAM_MESHES lists) */
#define MESH_FAR_WIDE          0x10u      /* flags: drawn with the wide far distance */
#define MESH_NO_FOG            0x200u     /* flags: drawn without fog */
#define MESH_BATCH_COUNT       0x2cu
#define MESH_BATCHES           0x58u      /* its batches, 0xa0 bytes each */
#define MESH_LATE_FROM         0x94u      /* batches from this one on are queued by depth; -1 none */
#define BATCH_SIZE             0xa0u
#define SET_DEFER_FLAGGED      0x1ecu     /* settings: batches flagged BF_DEFER are queued first */

/* a batch: its world 4x4 at +0, then */
#define BATCH_FLAGS            0x60u
#define BF_TWO_SOURCE          0x2u       /* lit from a second vertex stream (its light record) */
#define BF_HIDDEN              0x10080u   /* not drawn */
#define BF_VISIBLE             0x20000u   /* this view sees it */
#define BF_DEFER               0x100000u  /* queued to be drawn first in the late pass (with
                                             SET_DEFER_FLAGGED); drawn without depth bias */
#define BF_DECAL               0x2000000u /* drawn with a depth bias */
#define BF_RECEIVER            0x80000000u /* a shadow receiver (with BF_VISIBLE: queued as one) */
#define BATCH_CENTRE           0x50u      /* vec3: its centre, for the queue's depth */
#define BATCH_GEOMETRY         0x78u      /* the geometry record */
#define BATCH_LOD              0x7eu      /* short: the level of detail, -1 none */
#define BATCH_COLOURS          0x84u      /* its own vertex colours (RGBA bytes), or 0 */
#define BATCH_LIGHTING         0x8cu      /* the light record (BF_TWO_SOURCE), or 0 */
#define QUEUE_FIRST            0xc0000000u /* SVC_QUEUE's depths: -2 drawn first, */
#define QUEUE_RECEIVER         0xbf800000u /* -1 a shadow receiver */

/* the geometry record */
#define GEOM_VERTEX_COUNT      0x84u
#define GEOM_STRIP_COUNT       0x90u
#define GEOM_HAS_STRIPS        0x94u
#define GEOM_STRIPS            0xf0u      /* shorts: each strip's triangle count n, then n+2 indices */
#define GEOM_STRIP_MATERIALS   0xf8u      /* shorts: each strip's material (the view's table) */
#define GEOM_LODS              0x120u     /* 16 bytes each: +8 the stream-1 vertex offset, +0xc the geometry */
#define GEOM_BUFFERS           0x128u     /* the Direct3D buffers record: */
#define BUF_FORMAT             0x0u       /* the vertex format (SET_VERTEX_FORMAT; +1 with stream 1) */
#define BUF_STRIDE             0x4u
#define BUF_VERTICES           0x8u
#define BUF_VB                 0xcu       /* IDirect3DVertexBuffer9 * */
#define BUF_IB                 0x14u      /* IDirect3DIndexBuffer9 * */
#define BUF_FLAGS              0x18u      /* 0x40000000: the batch's colours not in the buffer yet */

/* the light record (BATCH_LIGHTING) */
#define LIT_VB                 0x0u       /* stream 1: a colour per vertex */
#define LIT_COUNT              0x4u       /* low byte: lights */
#define LIT_MATERIAL           0x8u       /* D3DMATERIAL9 */
#define LIT_LIGHTS             0x4cu      /* D3DLIGHT9, 0x68 bytes each */

#define VIEW_MATERIALS         0xb0u      /* the view's material table, 0x6c bytes each: */
#define MATERIAL_STATE         0x68u      /* ... the state 0x7a68f0 sets */
#define BATCH_DRAW             0x7a3783u
#define MATERIAL_SET           0x7a68f0u
#define DEPTH_BIAS             0x78ba44u  /* (float bias) */
#define D3DX_TRANSPOSE         0x7ca6f6u
#define MAT_ASSIGN             0x7a6250u
#define OWN_COLOURS_MODE       0xe7c4edu  /* byte: batches' own colours go into their vertex buffers */
#define NO_LIGHTS              0xe7c4dcu  /* byte: the light records' lights are not used */
#define STREAM1_BY_LOD         0x99342cu  /* bit 0: stream 1 starts at the level of detail's offset */
#define K_8                    0x8fa054u  /* .rdata float 8 */

/* 0x7a3531 (view, mesh) (SVC_DRAW_MESH): one world mesh: the projection for it (wide or not), fog
   off for MESH_NO_FOG meshes, then each visible batch: those before MESH_LATE_FROM drawn now
   (0x7a3783) unless queued to be drawn first in the late pass, those from it on queued by their
   centre's depth in the view. Fog back on after, and the W-buffer setting back. */
static int dc_7a3531(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0), mesh = ARG(1);
    uint32_t b = R32(mesh + MESH_BATCHES);
    uint32_t defer = R32(R32(SETTINGS) + SET_DEFER_FLAGGED) != 0;
    game_call(c, PROJECTION_FOR, 1, (R32(mesh + MESH_FLAGS) & MESH_FAR_WIDE) != 0);
    uint32_t no_fog = (R32(mesh + MESH_FLAGS) & MESH_NO_FOG) != 0;
    if (no_fog && R32(FOG_ON))
        game_call(c, FOG_OFF, 0);
    int late = R32(mesh + MESH_LATE_FROM) != 0xffffffffu;
    int32_t now = (int32_t)(late ? R32(mesh + MESH_LATE_FROM) : R32(mesh + MESH_BATCH_COUNT));
    for (int32_t i = now; i > 0; i--, b += BATCH_SIZE) {
        if (!(R32(b + BATCH_FLAGS) & BF_VISIBLE))
            continue;
        if (defer && (R32(b + BATCH_FLAGS) & BF_DEFER))
            game_call(c, R32(SVC_QUEUE), 2, b, QUEUE_FIRST);
        else
            game_call(c, BATCH_DRAW, 2, view, b);
    }
    if (late) {
        b = R32(mesh + MESH_LATE_FROM) * BATCH_SIZE + R32(mesh + MESH_BATCHES);
        for (int32_t i = (int32_t)(R32(mesh + MESH_BATCH_COUNT) - R32(mesh + MESH_LATE_FROM)); i > 0; i--, b += BATCH_SIZE) {
            if (!(R32(b + BATCH_FLAGS) & BF_VISIBLE))
                continue;
            uint32_t F = dc_frame(c, 12), v = F;     /* the centre in the view */
            game_call(c, VEC_BY_MATRIX_TO, 3, b + BATCH_CENTRE, v, CAM_VIEW);
            game_call(c, R32(SVC_QUEUE), 2, b, R32(v + 8));
            dc_unframe(c, F, 12);
        }
    }
    if (no_fog && R32(FOG_ON)) {
        set_rs(c, RS_FOGENABLE, 1);
        set_rs(c, RS_FOGVERTEXMODE, 3);    /* D3DFOG_LINEAR */
    }
    W8(USE_W_BUFFER, R8(USE_W_BUFFER_DEFAULT));
    return decomp_ret(c, e, (c->eax & ~0xffu) | R8(USE_W_BUFFER_DEFAULT), 0);
}
DECOMP(7a3531, dc_7a3531);

/* 0x7a3783 (view, batch) (SVC_DRAW_BATCH): one world batch, unless hidden or empty: its geometry
   (the level of detail's when it has one), the vertex format and streams (stream 1 the light
   record's colours for two-source batches, with its material and up to 3 lights, lighting on;
   else lighting off), the index buffer, its world matrix, a depth bias for decals (except those
   flagged BF_DEFER), the vertex shader constants (c0-c3 world x view x
   projection transposed, c13.x the level's clock / 8, the rest fixed), then each triangle strip
   with its material set when it changes. A visible shadow receiver is queued as one after. */
static int dc_7a3783(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0), b = ARG(1);
    if (R32(b + BATCH_FLAGS) & BF_HIDDEN)
        return decomp_ret(c, e, c->eax, 0);
    uint32_t geom = R32(b + BATCH_GEOMETRY), mats = R32(view + VIEW_MATERIALS), lod_first = 0;
    if (R32(geom + GEOM_LODS)) {
        int32_t lod = (int16_t)R16(b + BATCH_LOD);
        if (lod < 0)
            W16(b + BATCH_LOD, 0xffff);
        else {
            uint32_t l = (uint32_t)lod * 16 + R32(geom + GEOM_LODS);
            if (R32(l + 0xc)) {
                geom = R32(l + 0xc);
                lod_first = R32(l + 8);
            }
        }
    }
    uint32_t buf = R32(geom + GEOM_BUFFERS), lit = R32(b + BATCH_LIGHTING);
    if (!R32(geom + GEOM_VERTEX_COUNT))
        return decomp_ret(c, e, c->eax, 0);
    if (!R8(OWN_COLOURS_MODE)) {
        if ((R32(b + BATCH_FLAGS) & BF_TWO_SOURCE) && lit) {
            game_call(c, SET_VERTEX_FORMAT, 1, R32(buf + BUF_FORMAT) + 1);
            D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 0, R32(buf + BUF_VB), 0, R32(buf + BUF_STRIDE));
            if (!(R32(STREAM1_BY_LOD) & 1))
                D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 1, R32(lit + LIT_VB), 0, 4);
            else
                D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 1, R32(lit + LIT_VB), lod_first << 2, 4);
            D3DCALL(c, D3D_SET_MATERIAL, 1, lit + LIT_MATERIAL);
            if (R32(lit + LIT_COUNT) && !R8(NO_LIGHTS)) {
                uint32_t n = R32(lit + LIT_COUNT) & 0xff, i;
                for (i = 0; i < n; i++) {
                    D3DCALL(c, D3D_SET_LIGHT, 2, i, lit + LIT_LIGHTS + i * 0x68);
                    light_enable(c, i, 1);
                }
                for (; i < 3; i++)
                    light_enable(c, i, 0);
                set_rs(c, RS_LIGHTING, 1);
            } else
                set_rs(c, RS_LIGHTING, 0);
        } else {
            game_call(c, SET_VERTEX_FORMAT, 1, R32(buf + BUF_FORMAT));
            D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 0, R32(buf + BUF_VB), 0, R32(buf + BUF_STRIDE));
            set_rs(c, RS_LIGHTING, 0);
        }
    } else {
        /* the batch's own colours written into the vertex buffer once (ARGB at +0x18 of each
           0x24-byte vertex) [not reached on the lobby route] */
        if (R32(b + BATCH_COLOURS) && (R32(buf + BUF_FLAGS) & 0x40000000u)) {
            uint32_t F = dc_frame(c, 4), locked = F;
            uint32_t vb = R32(buf + BUF_VB);
            game_call(c, R32(R32(R32(buf + BUF_VB)) + VB_LOCK), 5, vb, 0, 0, locked, 0);
            for (uint32_t i = 0; i < R32(buf + BUF_VERTICES); i++) {
                uint32_t col = R32(b + BATCH_COLOURS) + i * 4;
                W8(col, R8(col));          /* (the original copies the first byte onto itself) */
                col = R32(b + BATCH_COLOURS) + i * 4;
                uint32_t argb = R8(col + 3) << 24 | R8(col) << 16 | R8(col + 1) << 8 | R8(col + 2);
                W32(R32(locked) + 0x18 + i * 0x24, argb);
            }
            vb = R32(buf + BUF_VB);
            game_call(c, R32(R32(vb) + VB_UNLOCK), 1, vb);
            W32(buf + BUF_FLAGS, R32(buf + BUF_FLAGS) & 0xbfffffffu);
            dc_unframe(c, F, 4);
        }
        D3DCALL(c, D3D_SET_MATERIAL, 1, lit + LIT_MATERIAL);
        game_call(c, SET_VERTEX_FORMAT, 1, R32(buf + BUF_FORMAT));
        D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 0, R32(buf + BUF_VB), 0, R32(buf + BUF_STRIDE));
        set_rs(c, RS_LIGHTING, 0);
    }
    D3DCALL(c, D3D_SET_INDICES, 1, R32(buf + BUF_IB));
    set_transform(c, D3DTS_WORLD, b);
    /* (the original keeps whether it set the bias, outside the translucent pass, in a local it
       never reads) */
    if ((R32(b + BATCH_FLAGS) & BF_DECAL) && !(R32(b + BATCH_FLAGS) & BF_DEFER))
        game_call(c, DEPTH_BIAS, 1, 0x3c23d70au);      /* 0.01 */
    else
        game_call(c, DEPTH_BIAS_OFF, 0);

    /* world x view x projection, transposed: vertex shader constants c0-c3 */
    uint32_t F = dc_frame(c, 0x220);
    uint32_t proj = F, wv = F + 0x40, view_m = F + 0x80, world = F + 0xc0, wvp = F + 0x100,
             cm = F + 0x140, k = F + 0x180;   /* (k: the 10 single constants c4-c13) */
    uint32_t r1 = game_callm(c, MAT_COPY, proj, 1, CAM_PROJECTION);
    uint32_t r2 = game_callm(c, MAT_COPY, view_m, 1, CAM_VIEW);
    uint32_t r3 = game_callm(c, MAT_COPY, world, 1, b);
    uint32_t r4 = game_callm(c, MAT_MUL, r3, 2, wv, r2);
    game_callm(c, MAT_MUL, r4, 2, wvp, r1);
    game_callm(c, MAT_ASSIGN, cm, 1, wvp);
    game_call(c, D3DX_TRANSPOSE, 2, cm, cm);
    static const uint32_t fixed[9][4] = {
        {0x3e4ccccdu, 0x3f333333u, 0x3e4ccccdu, 0x3f000000u},   /* c4 (0.2, 0.7, 0.2, 0.5) */
        {0}, {0},                                                 /* c5, c6 */
        {0x40000000u, 0, 0, 0},                                   /* c7 (2, 0, 0, 0) */
        {0}, {0}, {0},                                            /* c8, c9, c10 */
        {0x41c80000u, 0, 0, 0},                                   /* c11 (25, 0, 0, 0) */
        {0},                                                      /* c12 */
    };
    for (int i = 0; i < 9; i++)
        for (int j = 0; j < 4; j++)
            W32(k + 16u * i + 4u * j, fixed[i][j]);
    uint32_t c13 = k + 16u * 9;
    WF32(c13, (float)((f80)RF32(R32(LEVEL_PTR) + LEVEL_TIME) / (f80)RF32(K_8)));
    W32(c13 + 4, 0);
    W32(c13 + 8, 0);
    W32(c13 + 12, 0);
    set_vs_const(c, 0, cm, 4);
    for (uint32_t i = 0; i < 10; i++)
        set_vs_const(c, 4 + i, k + 16u * i, 1);
    dc_unframe(c, F, 0x220);

    /* the triangle strips, each with its material */
    uint32_t start = 0;
    if (R32(geom + GEOM_HAS_STRIPS)) {
        uint32_t mp = R32(geom + GEOM_STRIP_MATERIALS), sp = R32(geom + GEOM_STRIPS), last = 0;
        for (int32_t i = 0; i < (int32_t)R32(geom + GEOM_STRIP_COUNT); i++) {
            uint32_t mat = mats + R16(mp) * 0x6c;
            uint32_t len = R16(sp) + 2;     /* indices */
            sp += 2;
            if (mat != last) {
                last = mat;
                game_call(c, MATERIAL_SET, 1, R32(mat + MATERIAL_STATE));
            }
            D3DCALL(c, D3D_DRAW_INDEXED, 6, 5, 0, 0, R32(geom + GEOM_VERTEX_COUNT), start, len - 2);  /* strip */
            sp += len * 2;
            start += len;
            mp += 2;
        }
    }
    if ((R32(b + BATCH_FLAGS) & (BF_RECEIVER | BF_VISIBLE)) == (BF_RECEIVER | BF_VISIBLE))
        game_call(c, R32(SVC_QUEUE), 2, b, QUEUE_RECEIVER);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7a3783, dc_7a3783);

/* ------------------------------------------------------------------ the depth-sorted queue */

#define K_DEPTH_SCALE          0x8fce60u  /* .rdata double 0.2 */
#define K_2                    0x8fa794u  /* .rdata float 2 */
#define K_1999                 0x8fce5cu  /* .rdata float 1999 */
#define POOL_BYTES             0xbb80u    /* 4000 records */
#define QREC_KIND              0x0u       /* 1 by depth, 2 shadow receiver, 3 first */
#define QREC_BATCH             0x4u
#define QREC_NEXT              0x8u

/* 0x6ea51e (batch, depth) (SVC_QUEUE): queues a batch for the late pass (0x6ea603), while the pool
   has room: depth -2 (QUEUE_FIRST) into bucket 1 as kind 3, -1 (QUEUE_RECEIVER) into bucket 0 as
   kind 2, others as kind 1 into bucket depth / 5 kept within 2..1999 (so far ones are drawn
   first). Each bucket is a list, the newest first. */
static int dc_6ea51e(CPU *c)
{
    uint32_t e = c->esp, batch = ARG(0), key = ARG(1);
    uint32_t buckets = R32(CAM_BUCKETS), rec = R32(CAM_POOL_NEXT);
    if (rec >= R32(CAM_POOL_BASE) + POOL_BYTES)
        return decomp_ret(c, e, buckets, 0);
    if (key == QUEUE_RECEIVER) {
        W32(rec + QREC_KIND, 2);
        key = 0;
    } else if (key == QUEUE_FIRST) {
        W32(rec + QREC_KIND, 3);
        key = 0x3f800000u;                 /* 1 */
    } else {
        W32(rec + QREC_KIND, 1);
        key = f2u((float)((f80)u2f(key) * (f80)RF64(K_DEPTH_SCALE)));
        if ((f80)u2f(key) < (f80)RF32(K_2))
            key = 0x40000000u;             /* 2 */
        if ((f80)u2f(key) > (f80)RF32(K_1999))
            key = 0x44f9e000u;             /* 1999 */
    }
    FPUSH((f80)u2f(key));
    uint32_t bucket = buckets + game_call(c, FTOL, 0) * 4;
    W32(rec + QREC_NEXT, R32(bucket));
    W32(bucket, rec);
    W32(rec + QREC_BATCH, batch);
    W32(CAM_POOL_NEXT, rec + 0xc);
    return decomp_ret(c, e, batch, 0);
}
DECOMP(6ea51e, dc_6ea51e);

/* ------------------------------------------------------------------ the late pass */

#define CAMREC_VIEW            0x34u      /* CAM_VIEWREC's: the view the late pass draws for */
#define BATCH_RECEIVER_0x80    0x80u      /* short: cleared after the batch is drawn as a receiver */
#define DEBUG_PRINT            0x7c54ceu  /* (format, ...) */
#define FMT_RECEIVERS          0x93c600u  /* "NrShadowReceivers: %d\n" */

/* draws (SVC_DRAW_BATCH) the records of kind `kind` in one bucket and empties it; the renderer's
   default state (SVC_BEGIN) before the first one drawn since *first was set */
static void late_bucket(CPU *c, uint32_t slot, uint32_t kind, uint32_t view, uint32_t *first)
{
    if (!R32(slot))
        return;
    for (uint32_t r = R32(slot); r; r = R32(r + QREC_NEXT)) {
        if (R32(r + QREC_KIND) != kind)
            continue;
        uint32_t batch = R32(r + QREC_BATCH);
        if (view && R32(SVC_DRAW_BATCH)) {
            if (*first) {
                *first = 0;
                if (R32(SVC_BEGIN))
                    game_call(c, R32(SVC_BEGIN), 2, view, 0);
            }
            game_call(c, R32(SVC_DRAW_BATCH), 2, view, batch);
        }
    }
    W32(slot, 0);
}

/* 0x6ea603 (camera state) (SVC +0xcc): the late pass, what 0x6ea51e queued this view, for the view
   of CAM_VIEWREC (nothing without one): depth test and writes on, the batches queued to be drawn
   first (bucket 1, kind 3); then the translucent pass (depth writes off, TRANSLUCENT_PASS set)
   over buckets 1999 down to 1, kind 1, far to near; then the shadow receivers (bucket 0, kind 2:
   SVC_SHADOW_RECEIVER, the batch's receiver flag and +0x80 cleared), counted for a debug print.
   The state after the world (SVC_END) when anything was drawn since the translucent pass began.
   Every bucket walked is left empty. */
static int dc_6ea603(CPU *c)
{
    uint32_t e = c->esp, cam = ARG(0);
    uint32_t first = 1;
    uint32_t view = R32(R32(cam + (CAM_VIEWREC - CAM)) + CAMREC_VIEW);
    if (!view)
        return decomp_ret(c, e, c->eax, 0);
    if (R32(SVC_DEPTH))
        game_call(c, R32(SVC_DEPTH), 2, 1, 1);
    late_bucket(c, R32(cam + (CAM_BUCKETS - CAM)) + 4, 3, view, &first);
    W8(TRANSLUCENT_PASS, 1);
    first = 1;
    if (R32(SVC_DEPTH))
        game_call(c, R32(SVC_DEPTH), 2, 1, 0);
    if (R32(SVC_0xac))
        game_call(c, R32(SVC_0xac), 1, 0);
    uint32_t slot = R32(cam + (CAM_BUCKETS - CAM)) + 4 * 0x7cf;
    for (int32_t i = 0x7cf; i >= 1; i--, slot -= 4)
        late_bucket(c, slot, 1, view, &first);
    W8(TRANSLUCENT_PASS, 0);
    if (R32(SVC_0xac))
        game_call(c, R32(SVC_0xac), 1, 1);
    uint32_t receivers = 0;
    slot = R32(cam + (CAM_BUCKETS - CAM));
    if (R32(slot)) {
        for (uint32_t r = R32(slot); r; r = R32(r + QREC_NEXT)) {
            if (R32(r + QREC_KIND) != 2)
                continue;
            uint32_t batch = R32(r + QREC_BATCH);
            if (R32(SVC_SHADOW_RECEIVER))
                game_call(c, R32(SVC_SHADOW_RECEIVER), 2, view, batch);
            W32(batch + BATCH_FLAGS, R32(batch + BATCH_FLAGS) & ~BF_RECEIVER);
            W16(batch + BATCH_RECEIVER_0x80, 0);
            receivers++;
        }
        W32(slot, 0);
    }
    game_call(c, DEBUG_PRINT, 2, FMT_RECEIVERS, receivers);
    if (!first && R32(SVC_END))
        game_call(c, R32(SVC_END), 2, view, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(6ea603, dc_6ea603);

#define DEBUG_LEVEL            0xe7d3c8u  /* prints only below 0x20 */
#define SET_DEBUG_ON           0x4u       /* settings: debug prints wanted, */
#define SET_DEBUG_MUTE         0x18u      /* ... and not muted */
#define VSPRINTF               0x8aa57au  /* CRT vsprintf (buffer, format, arguments) */
#define DEBUG_TEXT             0x6302bbu  /* (0, text): empty in this build */

/* 0x7c54ce (format, ...): a debug print (into a 256-byte buffer, then 0x6302bb, which does nothing
   in this build), when the settings ask for it. */
static int dc_7c54ce(CPU *c)
{
    uint32_t e = c->esp, fmt = ARG(0);
    if (R32(DEBUG_LEVEL) >= 0x20 || !R32(R32(SETTINGS) + SET_DEBUG_ON) || R32(R32(SETTINGS) + SET_DEBUG_MUTE))
        return decomp_ret(c, e, c->eax, 0);
    uint32_t F = dc_frame(c, 0x100), buf = F;
    game_call(c, VSPRINTF, 3, buf, fmt, e + 8);     /* (the arguments after the format) */
    game_call(c, DEBUG_TEXT, 2, 0, buf);
    dc_unframe(c, F, 0x100);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(7c54ce, dc_7c54ce);
