/* THE WORLD BATCH, shared definitions (world.c, surface.c, shadow.c, staticlib.c): the Direct3D
   device and its methods as the game calls them, and the renderer's globals these files share.
   Direct3D calls go through the device's vtable as the original's do (HOWTO.md); the offsets are
   IDirect3DDevice9's method order times 4 [verified: each matches the method tools/calltree.sh
   names for the call, e.g. 0x7a3783's +0x148 calls are its 449 DrawIndexedPrimitive]. */
#pragma once
#include "decomp.h"

/* ------------------------------------------------------------------ Direct3D 9 */
#define D3D_DEVICE             0x994a10u  /* IDirect3DDevice9 * [verified: every call here] */

#define D3D_SET_TRANSFORM      0xb0u
#define D3D_SET_MATERIAL       0xc4u
#define D3D_SET_LIGHT          0xccu
#define D3D_LIGHT_ENABLE       0xd4u
#define D3D_SET_RENDER_STATE   0xe4u
#define D3D_SET_TEXTURE        0x104u
#define D3D_SET_TSS            0x10cu     /* SetTextureStageState */
#define D3D_SET_SAMPLER_STATE  0x114u
#define D3D_DRAW_INDEXED       0x148u     /* DrawIndexedPrimitive */
#define D3D_DRAW_PRIMITIVE_UP  0x14cu
#define D3D_SET_VS_CONST       0x178u     /* SetVertexShaderConstantF */
#define D3D_SET_STREAM_SOURCE  0x190u
#define D3D_SET_INDICES        0x1a0u
#define D3D_SET_PIXEL_SHADER   0x1acu
#define D3D_SET_PS_CONST       0x1b4u     /* SetPixelShaderConstantF */
#define VB_LOCK                0x2cu      /* IDirect3DVertexBuffer9::Lock, Unlock */
#define VB_UNLOCK              0x30u

/* transform states */
#define D3DTS_VIEW             2u
#define D3DTS_PROJECTION       3u
#define D3DTS_TEXTURE0         16u
#define D3DTS_TEXTURE1         17u
#define D3DTS_WORLD            256u

/* render states (D3DRENDERSTATETYPE) */
#define RS_ZENABLE             7u
#define RS_ZWRITEENABLE        14u
#define RS_ALPHATESTENABLE     15u
#define RS_SRCBLEND            19u
#define RS_DESTBLEND           20u
#define RS_CULLMODE            22u
#define RS_ZFUNC               23u
#define RS_ALPHAREF            24u
#define RS_ALPHAFUNC           25u
#define RS_ALPHABLENDENABLE    27u
#define RS_FOGENABLE           28u
#define RS_TEXTUREFACTOR       60u
#define RS_LIGHTING            137u
#define RS_AMBIENT             139u
#define RS_FOGVERTEXMODE       140u
#define RS_COLORVERTEX         141u
#define RS_DIFFUSEMATERIALSOURCE  145u
#define RS_SPECULARMATERIALSOURCE 146u
#define RS_AMBIENTMATERIALSOURCE  147u
#define RS_EMISSIVEMATERIALSOURCE 148u
#define RS_DEPTHBIAS           195u

/* texture stage states and sampler states */
#define TSS_COLOROP            1u
#define TSS_COLORARG1          2u
#define TSS_COLORARG2          3u
#define TSS_ALPHAOP            4u
#define TSS_ALPHAARG1          5u
#define TSS_ALPHAARG2          6u
#define TSS_TEXCOORDINDEX      11u
#define TSS_TEXTURETRANSFORMFLAGS 24u
#define SAMP_ADDRESSU          1u
#define SAMP_ADDRESSV          2u
#define SAMP_MINFILTER         5u
#define SAMP_MAGFILTER         6u
#define SAMP_MIPFILTER         7u
#define SAMP_MAXANISOTROPY     10u

/* a device method: game_call through the device's vtable, the device as the first argument */
#define D3DCALL(c, off, n, ...) \
    game_call(c, R32(R32(R32(D3D_DEVICE)) + (off)), (n) + 1, R32(D3D_DEVICE), __VA_ARGS__)
static inline uint32_t set_rs(CPU *c, uint32_t s, uint32_t v) { return D3DCALL(c, D3D_SET_RENDER_STATE, 2, s, v); }
static inline uint32_t set_tss(CPU *c, uint32_t st, uint32_t t, uint32_t v) { return D3DCALL(c, D3D_SET_TSS, 3, st, t, v); }
static inline uint32_t set_ss(CPU *c, uint32_t st, uint32_t t, uint32_t v) { return D3DCALL(c, D3D_SET_SAMPLER_STATE, 3, st, t, v); }
static inline uint32_t light_enable(CPU *c, uint32_t i, uint32_t on) { return D3DCALL(c, D3D_LIGHT_ENABLE, 2, i, on); }
static inline uint32_t set_transform(CPU *c, uint32_t t, uint32_t m) { return D3DCALL(c, D3D_SET_TRANSFORM, 2, t, m); }
static inline uint32_t set_vs_const(CPU *c, uint32_t r, uint32_t p, uint32_t n) { return D3DCALL(c, D3D_SET_VS_CONST, 3, r, p, n); }

/* floats as the game's dwords */
static inline float u2f(uint32_t u) { float f; memcpy(&f, &u, 4); return f; }
static inline uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }

/* ------------------------------------------------------------------ the renderer's globals */
/* The renderer's service table: function pointers (cdecl) the engine calls the Direct3D renderer
   through, each may be 0 [verified: the calls through them in 0x747b66, 0x6ea603, 0x646c33; what
   each holds: 0x7c0f3c fills them (+0x9c 0x401cf3 -> 0x7a2f26, +0xa0 0x404908 -> 0x7a3472, +0xa4,
   +0xa8, +0xb0, +0x110), 0x6eaf9a (+0xc8, +0xcc)] */
#define SVC                    0x97ed88u
#define SVC_FOCAL_DIV          (SVC + 0x10)   /* float: the camera's focal value is divided by it */
#define SVC_ASPECT             (SVC + 0x20)   /* float: the projection's last argument (aspect factor) */
#define SVC_BEGIN              (SVC + 0x9c)   /* (view, 1): 0x7a2f26, the scene's default state */
#define SVC_END                (SVC + 0xa0)   /* (view, 1): 0x7a3472, the state after the world */
#define SVC_DRAW_BATCH         (SVC + 0xa4)   /* (view, batch): 0x7a3783 */
#define SVC_SHADOW_RECEIVER    (SVC + 0xa8)   /* (view, batch): 0x7a4a5e */
#define SVC_0xac               (SVC + 0xac)   /* (0 before, 1 after the far-to-near batches); not set by 0x7c0f3c */
#define SVC_DRAW_MESH          (SVC + 0xb0)   /* (view, mesh): 0x7a3531 */
#define SVC_DRAW_WORLD         (SVC + 0xb4)   /* (view): draws the whole world instead; not set by 0x7c0f3c */
#define SVC_QUEUE              (SVC + 0xc8)   /* (batch, depth): 0x6ea51e, the depth-sorted queue */
#define SVC_DEPTH              (SVC + 0x110)  /* (test, write): 0x7c1317, the depth buffer's use */

/* The camera state (game.c: its +4 the cinema bars' height): the 3D view's size, projection and
   view matrices, and the depth-sorted queue's buckets [verified: the uses below] */
#define CAM                    0x97f234u
#define CAM_WIDTH              (CAM + 0x8)    /* int: the 3D viewport's width, height */
#define CAM_HEIGHT             (CAM + 0xc)
#define CAM_FAR_WIDE           (CAM + 0x38)   /* float: far distance for meshes with flag 0x10 */
#define CAM_FAR                (CAM + 0x3c)   /* float: far distance, at least 1000 */
#define CAM_FOCAL              (CAM + 0x4c)   /* float: focal value (over SVC_FOCAL_DIV) */
#define CAM_PROJECTION         (CAM + 0x54)   /* 4x4: the projection matrix (0x97f288) */
#define CAM_VIEW               (CAM + 0x240)  /* 4x4: the view matrix (0x97f474) */
#define CAM_POOL_BASE          (CAM + 0x348)  /* the queue's records (12 bytes: kind, batch, next) */
#define CAM_POOL_NEXT          (CAM + 0x34c)  /* ... the next free one */
#define CAM_BUCKETS            (CAM + 0x350)  /* the queue's 2000 buckets (lists), by depth */
#define CAM_MESH_COUNT         (CAM + 0x354)  /* the world's meshes to draw this view */
#define CAM_MESHES             (CAM + 0x35c)  /* ... their pointers */
#define CAM_VIEWREC            (CAM + 0x4ec)  /* a record whose +0x34 is the view the hooks get */

#define SETTINGS               0x97f800u  /* pointer: the engine's frame settings (fields by offset) */
#define LEVEL_PTR              0x97f720u  /* the level (renderpass.c) */
#define LEVEL_TIME             0x8u       /* float: the level's clock, seconds [guess: grows each update] */

#define USE_W_BUFFER           0xe7c4e5u  /* byte: depth as W (ZENABLE 2) this mesh */
#define USE_W_BUFFER_DEFAULT   0xe7c4e8u  /* byte: ... the setting it returns to */
#define TRANSLUCENT_PASS       0xe75ea8u  /* byte: drawing the far-to-near batches (depth writes off) */
#define FOG_ON                 0x97f7dcu  /* fog in use (meshes with flag 0x200 are drawn without) */
#define FVF_CACHE              0x94234cu  /* the vertex format 0x78ad3c last set, -1 none */

/* the other batches' functions this one calls (their translation, or their rewrite) */
#define SET_VERTEX_FORMAT      0x78ad3cu  /* (index): the vertex format (FVF) from its table */
#define DEPTH_BIAS_OFF         0x78ba81u  /* depth bias 0 */
#define FOG_OFF                0x78c6a0u
#define MAT_COPY               0x7a6280u  /* (this = 4x4; src) -> this */
#define MAT_MUL                0x7a6430u  /* (this = a; out, b): out = a b -> out */
#define VEC_BY_MATRIX_TO       0x62981cu  /* (vec3, out vec3, 4x4) */
#define MEMCPY                 0x8a9ae0u  /* CRT memcpy */
#define FTOL                   0x8a9ef4u  /* CRT _ftol: st(0) truncated (popped) -> edx:eax */
