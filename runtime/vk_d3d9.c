/* Our own Vulkan renderer: the Direct3D 9 the game uses (VULKAN.md 2.1), as host COM objects with
   Direct3D's vtables, so the generated wrappers, d3d9.c, d3d9_msaa.c and the shots call these exactly
   as they called DXVK's. Brought over from larush-native's renderer (its round 4, 2026-10-28) on
   2026-10-29; its native passes (bloom, AO) left out, Enter the Matrix has none.
   - IDirect3D9: the answers DXVK 3.1.1 gives (its caps, adapter identity, format and multisample
     checks, mode list: ETM_DUMP_CAPS compares them), so the game takes the same paths under both
     renderers and its memory stays the same.
   - Resources: textures and cube textures (one image each, every level and face a surface object
     sharing the texture's references), standalone surfaces (render targets, depth buffers, the back
     buffer, system-memory surfaces), vertex and index buffers (ranges of large host-visible
     buffers, the program's own copy as the host copy for locks: d3d9.c vk_buf_host; written in place,
     or into a new range when draws recorded since still read the old bytes), declarations (SetFVF's
     made from the FVF as DXVK makes them), shaders (their SPIR-V from gen/spirv_gen.c by the
     bytecode's hash), state blocks.
   - The device: Direct3D's state; at a draw, the pipeline for the state (cached, listed on disk and
     made again at the start), dynamic state, the constants into the frame's ring, the textures' and
     samplers' bindless indices as push constants; render passes begun at the first drawing or clear
     after the targets change and ended when they change (dynamic rendering), copies and resolves
     between them, barriers by need.
   - Fixed function (VULKAN.md 2.3; the game draws everything so): two uber-shaders specialized by a
     state key ("fixed function" below).
   Not here yet: vertex blending, clip planes, flat shading, depth StretchRect, mipmap generation,
   volume textures, gamma ramps (VULKAN.md 4). */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "vk.h"
#include "options.h"
#include "win.h"
#include "d3d9_gen.h"

void vk_present(VkImage bb, uint32_t bw, uint32_t bh);

/* the real clock in milliseconds (now_ms follows --test's virtual clock) */
static double real_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int once_(const char *what)
{
    static const char *seen[256];
    static int n;
    for (int i = 0; i < n; i++) if (seen[i] == what) return 0;
    if (n < 256) seen[n++] = what;
    return 1;
}
static VkShaderModule module_of(uint64_t h, const VkShaderMeta **meta);
#define UNSUPPORTED(name) do { if (once_(name)) LOG(LOG_ALWAYS, "vulkan renderer: %s is not supported", name); } while (0)

/* ------------------------------------------------------------------ formats */
typedef struct {
    D3DFORMAT d3d;
    VkFormat vk;
    uint8_t bpb, block;          /* bytes per block (texel), block size (4: BC) */
    uint8_t depth, stencil, alpha, shadow;   /* shadow: sampling it compares (Direct3D's depth textures) */
    VkComponentSwizzle r, g, b, a;
} Fmt;
#define I VK_COMPONENT_SWIZZLE_IDENTITY
#define ONE VK_COMPONENT_SWIZZLE_ONE
#define ZERO VK_COMPONENT_SWIZZLE_ZERO
#define SR VK_COMPONENT_SWIZZLE_R
#define SG VK_COMPONENT_SWIZZLE_G
static Fmt fmts[] = {
    {D3DFMT_A8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_X8R8G8B8, VK_FORMAT_B8G8R8A8_UNORM, 4, 1, 0, 0, 0, 0, I, I, I, ONE},
    {D3DFMT_A8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_X8B8G8R8, VK_FORMAT_R8G8B8A8_UNORM, 4, 1, 0, 0, 0, 0, I, I, I, ONE},
    {D3DFMT_R5G6B5, VK_FORMAT_R5G6B5_UNORM_PACK16, 2, 1, 0, 0, 0, 0, I, I, I, ONE},
    {D3DFMT_A1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_X1R5G5B5, VK_FORMAT_A1R5G5B5_UNORM_PACK16, 2, 1, 0, 0, 0, 0, I, I, I, ONE},
    {D3DFMT_A4R4G4B4, VK_FORMAT_A4R4G4B4_UNORM_PACK16, 2, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_A2R10G10B10, VK_FORMAT_A2R10G10B10_UNORM_PACK32, 4, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_A2B10G10R10, VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_G16R16, VK_FORMAT_R16G16_UNORM, 4, 1, 0, 0, 0, 0, I, I, ONE, ONE},
    {D3DFMT_A16B16G16R16, VK_FORMAT_R16G16B16A16_UNORM, 8, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_A8, VK_FORMAT_R8_UNORM, 1, 1, 0, 0, 1, 0, ZERO, ZERO, ZERO, SR},
    {D3DFMT_L8, VK_FORMAT_R8_UNORM, 1, 1, 0, 0, 0, 0, SR, SR, SR, ONE},
    {D3DFMT_A8L8, VK_FORMAT_R8G8_UNORM, 2, 1, 0, 0, 1, 0, SR, SR, SR, SG},
    {D3DFMT_L16, VK_FORMAT_R16_UNORM, 2, 1, 0, 0, 0, 0, SR, SR, SR, ONE},
    {D3DFMT_V8U8, VK_FORMAT_R8G8_SNORM, 2, 1, 0, 0, 0, 0, I, I, ONE, ONE},
    {D3DFMT_V16U16, VK_FORMAT_R16G16_SNORM, 4, 1, 0, 0, 0, 0, I, I, ONE, ONE},
    {D3DFMT_Q8W8V8U8, VK_FORMAT_R8G8B8A8_SNORM, 4, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_DXT1, VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 8, 4, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_DXT2, VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_DXT3, VK_FORMAT_BC2_UNORM_BLOCK, 16, 4, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_DXT4, VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_DXT5, VK_FORMAT_BC3_UNORM_BLOCK, 16, 4, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_R16F, VK_FORMAT_R16_SFLOAT, 2, 1, 0, 0, 0, 0, I, ONE, ONE, ONE},
    {D3DFMT_G16R16F, VK_FORMAT_R16G16_SFLOAT, 4, 1, 0, 0, 0, 0, I, I, ONE, ONE},
    {D3DFMT_A16B16G16R16F, VK_FORMAT_R16G16B16A16_SFLOAT, 8, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_R32F, VK_FORMAT_R32_SFLOAT, 4, 1, 0, 0, 0, 0, I, ONE, ONE, ONE},
    {D3DFMT_G32R32F, VK_FORMAT_R32G32_SFLOAT, 8, 1, 0, 0, 0, 0, I, I, ONE, ONE},
    {D3DFMT_A32B32G32R32F, VK_FORMAT_R32G32B32A32_SFLOAT, 16, 1, 0, 0, 1, 0, I, I, I, I},
    {D3DFMT_D16, VK_FORMAT_D16_UNORM, 2, 1, 1, 0, 0, 1, I, I, I, I},
    {D3DFMT_D16_LOCKABLE, VK_FORMAT_D16_UNORM, 2, 1, 1, 0, 0, 1, I, I, I, I},
    {D3DFMT_D24S8, VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 1, 0, 1, I, I, I, I},
    {D3DFMT_D24X8, VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 0, 0, 1, I, I, I, I},
    {D3DFMT_D24X4S4, VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 1, 0, 1, I, I, I, I},
    {D3DFMT_D32, VK_FORMAT_D32_SFLOAT, 4, 1, 1, 0, 0, 1, I, I, I, I},
    {(D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z'), VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 1, 0, 0, SR, SR, SR, SR},
    {(D3DFORMAT)MAKEFOURCC('D', 'F', '2', '4'), VK_FORMAT_D24_UNORM_S8_UINT, 4, 1, 1, 0, 0, 0, SR, SR, SR, SR},
    {(D3DFORMAT)MAKEFOURCC('D', 'F', '1', '6'), VK_FORMAT_D16_UNORM, 2, 1, 1, 0, 0, 0, SR, SR, SR, SR},
};
#undef I
#undef ONE
#undef ZERO
#undef SR
#undef SG

static const Fmt *fmt_of(D3DFORMAT f)
{
    for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++)
        if (fmts[i].d3d == f) return &fmts[i];
    return NULL;
}

/* D24S8 where the GPU can sample and render it (NVIDIA), else D32_SFLOAT_S8_UINT (AMD, as DXVK) */
static void fmts_init(void)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(R.phys, VK_FORMAT_D24_UNORM_S8_UINT, &p);
    VkFormatFeatureFlags need = VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if ((p.optimalTilingFeatures & need) != need)
        for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++)
            if (fmts[i].vk == VK_FORMAT_D24_UNORM_S8_UINT) fmts[i].vk = VK_FORMAT_D32_SFLOAT_S8_UINT;
    vkGetPhysicalDeviceFormatProperties(R.phys, VK_FORMAT_A4R4G4B4_UNORM_PACK16, &p);
    if (!(p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT))
        for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++)
            if (fmts[i].d3d == D3DFMT_A4R4G4B4) fmts[i].vk = VK_FORMAT_UNDEFINED;
}

const char *vk_format_name(D3DFORMAT f)
{
    static char b[16];
    snprintf(b, sizeof b, "%u", (unsigned)f);
    return b;
}

/* ------------------------------------------------------------------ objects: references, private data */
static IDirect3D9Vtbl vt_d3d9;
static IDirect3DDevice9Vtbl vt_dev;
static IDirect3DTexture9Vtbl vt_tex;
static IDirect3DCubeTexture9Vtbl vt_cube;
static IDirect3DSurface9Vtbl vt_surf;
static IDirect3DVertexBuffer9Vtbl vt_vb;
static IDirect3DIndexBuffer9Vtbl vt_ib;
static IDirect3DVertexDeclaration9Vtbl vt_decl;
static IDirect3DVertexShader9Vtbl vt_vs;
static IDirect3DPixelShader9Vtbl vt_ps;
static IDirect3DStateBlock9Vtbl vt_sb;
static IDirect3DQuery9Vtbl vt_q;
typedef struct { VkObj o; D3DQUERYTYPE type; int slot; int issued; } VkQuery;   /* (timestamps: dev_CreateQuery) */
static int qslots_used;
static int qfree[4096], nqfree;

static VkObj d3d9_obj, dev_obj;          /* (one of each) */
static void destroy(VkObj *o);

__attribute__((noinline)) static VkObj *owner(VkObj *o)
{
    if (o->kind == DK_IDirect3DSurface9) {
        VkSurf *s = (VkSurf *)o;
        if (!s->standalone && s->tex) return &s->tex->o;
    }
    return o;
}

static void obj_init(VkObj *o, void *vtbl, int kind)
{
    o->vtbl = vtbl;
    o->refs = 1;
    o->irefs = 0;
    o->kind = kind;
    o->zombie = 0;
    o->priv = NULL;
}

static void release_last(VkObj *o);

/* the device's own reference (state blocks, the targets), so an object released while held lives on */
static void iref(VkObj *o)
{
    if (o) __atomic_add_fetch(&owner(o)->irefs, 1, __ATOMIC_ACQ_REL);
}

static void iunref(VkObj *o)
{
    if (!o) return;
    VkObj *w = owner(o);
    if (__atomic_sub_fetch(&w->irefs, 1, __ATOMIC_ACQ_REL) == 0 && w->refs == 0) release_last(w);
}

static ULONG com_AddRef(void *This)
{
    VkObj *o = owner(This);
    return (ULONG)__atomic_add_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
}

static ULONG com_Release(void *This)
{
    VkObj *o = owner(This);
    long r = __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL);
    if (r == 0 && o->irefs == 0 && o != &d3d9_obj && o != &dev_obj) release_last(o);
    if (r < 0) o->refs = 0;
    return (ULONG)(r < 0 ? 0 : r);
}

static const IID *iid_of(int kind)
{
    switch (kind) {
    case DK_IDirect3D9: return &IID_IDirect3D9;
    case DK_IDirect3DDevice9: return &IID_IDirect3DDevice9;
    case DK_IDirect3DTexture9: return &IID_IDirect3DTexture9;
    case DK_IDirect3DCubeTexture9: return &IID_IDirect3DCubeTexture9;
    case DK_IDirect3DSurface9: return &IID_IDirect3DSurface9;
    case DK_IDirect3DVertexBuffer9: return &IID_IDirect3DVertexBuffer9;
    case DK_IDirect3DIndexBuffer9: return &IID_IDirect3DIndexBuffer9;
    case DK_IDirect3DVertexDeclaration9: return &IID_IDirect3DVertexDeclaration9;
    case DK_IDirect3DVertexShader9: return &IID_IDirect3DVertexShader9;
    case DK_IDirect3DPixelShader9: return &IID_IDirect3DPixelShader9;
    case DK_IDirect3DStateBlock9: return &IID_IDirect3DStateBlock9;
    case DK_IDirect3DQuery9: return &IID_IDirect3DQuery9;
    }
    return NULL;
}

static HRESULT com_QueryInterface(void *This, REFIID riid, void **ppv)
{
    VkObj *o = This;
    if (!ppv) return E_POINTER;
    *ppv = NULL;
    const IID *mine = iid_of(o->kind);
    int ok = !memcmp(riid, &IID_IUnknown, sizeof(IID)) || (mine && !memcmp(riid, mine, sizeof(IID)));
    int res = o->kind == DK_IDirect3DTexture9 || o->kind == DK_IDirect3DCubeTexture9 || o->kind == DK_IDirect3DSurface9 ||
              o->kind == DK_IDirect3DVertexBuffer9 || o->kind == DK_IDirect3DIndexBuffer9;
    if (res && !memcmp(riid, &IID_IDirect3DResource9, sizeof(IID))) ok = 1;
    if ((o->kind == DK_IDirect3DTexture9 || o->kind == DK_IDirect3DCubeTexture9) &&
        !memcmp(riid, &IID_IDirect3DBaseTexture9, sizeof(IID))) ok = 1;
    if (!ok) return E_NOINTERFACE;
    com_AddRef(This);
    *ppv = This;
    return S_OK;
}

static void priv_free(VkObj *o)
{
    VkPriv *p = o->priv;
    o->priv = NULL;
    while (p) {
        VkPriv *n = p->next;
        if (p->iunk && p->data) ((ULONG (*)(void *))(*(void ***)p->data)[2])(p->data);
        else free(p->data);
        free(p);
        p = n;
    }
}

static HRESULT res_SetPrivateData(void *This, REFGUID guid, const void *data, DWORD size, DWORD flags)
{
    VkObj *o = This;
    VkPriv **pp = &o->priv;
    for (; *pp; pp = &(*pp)->next)
        if (!memcmp(&(*pp)->guid, guid, sizeof(GUID))) break;
    if (*pp) {
        VkPriv *p = *pp;
        *pp = p->next;
        if (p->iunk && p->data) ((ULONG (*)(void *))(*(void ***)p->data)[2])(p->data);
        else free(p->data);
        free(p);
    }
    VkPriv *p = calloc(1, sizeof *p);
    memcpy(&p->guid, guid, sizeof(GUID));
    if (flags & D3DSPD_IUNKNOWN) {
        p->iunk = 1;
        p->data = (void *)data;
        if (data) ((ULONG (*)(const void *))(*(void *const *const *)data)[1])(data);
        p->size = sizeof(void *);
    } else {
        p->data = malloc(size ? size : 1);
        if (size) memcpy(p->data, data, size);
        p->size = size;
    }
    p->next = o->priv;
    o->priv = p;
    return D3D_OK;
}

static HRESULT res_GetPrivateData(void *This, REFGUID guid, void *data, DWORD *size)
{
    VkObj *o = This;
    for (VkPriv *p = o->priv; p; p = p->next)
        if (!memcmp(&p->guid, guid, sizeof(GUID))) {
            if (!size) return D3DERR_INVALIDCALL;
            if (!data) { *size = p->size; return D3D_OK; }
            if (*size < p->size) { *size = p->size; return D3DERR_MOREDATA; }
            if (p->iunk) {
                memcpy(data, &p->data, sizeof(void *));
                if (p->data) ((ULONG (*)(void *))(*(void ***)p->data)[1])(p->data);
            } else memcpy(data, p->data, p->size);
            *size = p->size;
            return D3D_OK;
        }
    return D3DERR_NOTFOUND;
}

static HRESULT res_FreePrivateData(void *This, REFGUID guid)
{
    VkObj *o = This;
    for (VkPriv **pp = &o->priv; *pp; pp = &(*pp)->next)
        if (!memcmp(&(*pp)->guid, guid, sizeof(GUID))) {
            VkPriv *p = *pp;
            *pp = p->next;
            if (p->iunk && p->data) ((ULONG (*)(void *))(*(void ***)p->data)[2])(p->data);
            else free(p->data);
            free(p);
            return D3D_OK;
        }
    return D3DERR_NOTFOUND;
}

static HRESULT res_GetDevice(void *This, IDirect3DDevice9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    com_AddRef(&dev_obj);
    *out = (IDirect3DDevice9 *)&dev_obj;
    return D3D_OK;
}

static DWORD res_SetPriority(void *This, DWORD p) { (void)This; (void)p; return 0; }
static DWORD res_GetPriority(void *This) { (void)This; return 0; }
static void res_PreLoad(void *This) { (void)This; }

/* ------------------------------------------------------------------ the device's state */
#define NSAMP 21                         /* 0-15 pixel, 16 displacement map, 17-20 vertex */
#define VK_LIGHTS 64                     /* light indices kept (Direct3D allows any) */
typedef struct {
    DWORD rs[256];
    DWORD ss[NSAMP][14];
    DWORD tss[8][33];
    VkTex *tex[NSAMP];
    VkBuf *vb[16];
    UINT vb_off[16], vb_stride[16], freq[16];
    VkBuf *ib;
    VkDecl *decl;
    DWORD fvf;
    VkShader *vs, *ps;
    float vsc[VK_VS_CONSTS][4];
    int vsi[16][4];
    BOOL vsb[16];
    float psc[VK_PS_CONSTS][4];
    int psi[16][4];
    BOOL psb[16];
    D3DVIEWPORT9 vp;
    RECT scissor;
    float clip[6][4];
    /* fixed function (VULKAN.md 2.3) */
    D3DMATERIAL9 mat;
    D3DLIGHT9 light[VK_LIGHTS];         /* by index (the game uses 0-7: VULKAN.md 1) */
    uint8_t light_on[VK_LIGHTS], light_set[VK_LIGHTS];
} St;

static St S;                             /* the device's state */
static VkSurf *rts[4], *dsurf;           /* bound targets (the device's references) */
static VkSurf *backbuf, *autods;         /* the device's own */
/* the frame's back buffer is in its swapchain image (vk_core.c vk_sc_target): 1 drawn there, 2 copied
   back into the back buffer and drawn there from then on (something else wrote or read it) */
static int sc_frame;
static D3DPRESENT_PARAMETERS cur_pp;
static D3DMATRIX xforms[512];
static unsigned long creator_thread;

/* what is bound at the Vulkan side in the command buffer being recorded (reset with each new one) */
static struct {
    VkCommandBuffer cb;
    VkPipeline pipe;
    int pass;                            /* rendering begun */
    VkSurf *prt, *pds;                   /* the pass's attachments */
    uint32_t pw, ph;
    uint32_t slots[VK_MAX_SAMPLER_SLOTS];
    int slots_valid;
    VkDescriptorSet cset;                /* the constants: the ring chunk's set, the vertex shader's */
    uint32_t coff[VK_VS_CHUNKS + 1];     /* chunks' offsets and the pixel shader's block's (last) */
    int consts_valid;                    /* cset and coff hold something of this command buffer */
    uint32_t vvalid;                     /* chunks uploaded (into cset's buffer) */
    uint32_t vdef[VK_VS_CHUNKS];         /* ... with that shader's def'd constants in them (0: none) */
    int vcontig;                         /* ... all at once, one after the other (relative addressing) */
    int pvalid;
    uint32_t c_ps;                       /* the pixel shader its block was written for (id) */
    int c_unorm8;                        /* ... and the target kind the alpha reference was for */
    int dyn_valid;                       /* the values below were set in this command buffer */
    VkViewport vp;
    VkRect2D sc;
    VkCullModeFlags cull;
    struct { uint32_t test, write, op, bias_on; float bias, slope; } depth;
    struct { uint32_t on, ops[2][4], cmask, wmask, ref; } stencil;
    DWORD blendf;
    VkPrimitiveTopology topo;
    VkBuffer vbuf[16];
    VkDeviceSize voff[16], vstride[16];
    VkBuffer ibuf;
    VkDeviceSize ioff;
    VkIndexType itype;
    int zero_bound;
    int center;                          /* the pass's draws use centred sample locations */
    int ffv_valid;                       /* coff[0] holds the fixed-function vertex block of now */
} B;

/* what changed since the last draw (draw_prep): the float constants' changed registers [lo, hi)
   (empty: lo > hi), the pixel shader's extra block (fog, alpha reference, bump matrices, the
   target's kind), the pipeline's key, dynamic state by group, sampler states by slot */
enum { DYN_VP = 1, DYN_CULL = 2, DYN_DEPTH = 4, DYN_STENCIL = 8, DYN_BLEND = 16, DYN_ALL = 31 };
static uint32_t vs_dirty = ~0u;          /* the vertex constants' chunks (VK_VS_CHUNK registers) changed */
static uint32_t psc_lo, psc_hi = VK_PS_CONSTS;
static int dirty_psx = 1, dirty_pipe = 1, dirty_dyn = DYN_ALL;
static int dirty_ffv = 1;                /* the fixed-function vertex block's values (ff_data_vs) */
static uint32_t dirty_samp = ~0u;
#define DIRTY_CONSTS() (vs_dirty = ~0u, psc_lo = 0, psc_hi = VK_PS_CONSTS, dirty_psx = 1)
#define DIRTY_ALL() (DIRTY_CONSTS(), dirty_pipe = 1, dirty_dyn = DYN_ALL, dirty_samp = ~0u, dirty_ffv = 1)

/* ------------------------------------------------------------------ bound objects
   The state's slots (textures, streams, indices, declaration, shaders) hold no reference: a bind
   touches only the slot, never the object (thousands of binds a frame over ~27,000 objects
   scattered in memory: round 1's atomic reference counts on their cold cache lines were 0.4 ms of
   the game thread a frame on the Win Max). Direct3D keeps an object the program releases alive
   while it is bound: such an object waits in `zombies` until no slot holds it (state blocks and
   the targets still count references: iref). */
static VkObj **zombies;
static int nzombies, capzombies;

static int bound(const VkObj *o)
{
    const void *p = o;
    for (int i = 0; i < NSAMP; i++) if ((const void *)S.tex[i] == p) return 1;
    for (int i = 0; i < 16; i++) if ((const void *)S.vb[i] == p) return 1;
    return (const void *)S.ib == p || (const void *)S.decl == p || (const void *)S.vs == p || (const void *)S.ps == p;
}

/* its last reference gone: destroyed now, or once no slot holds it */
static void release_last(VkObj *o)
{
    if (!bound(o)) { destroy(o); return; }
    if (o->zombie) return;
    if (nzombies == capzombies) {
        capzombies = capzombies ? capzombies * 2 : 64;
        zombies = realloc(zombies, (size_t)capzombies * sizeof *zombies);
    }
    o->zombie = 1;
    zombies[nzombies++] = o;
}

/* the waiting objects no slot holds are destroyed (one the program took a reference to again,
   GetTexture say, is alive again) */
static void zombies_sweep(void)
{
    for (int i = 0; i < nzombies; ) {
        VkObj *z = zombies[i];
        if (z->refs > 0 || z->irefs > 0) { z->zombie = 0; zombies[i] = zombies[--nzombies]; continue; }
        if (bound(z)) { i++; continue; }
        zombies[i] = zombies[--nzombies];
        destroy(z);
    }
}

/* a slot let go of old */
static inline void unbound(const void *old)
{
    if (__builtin_expect(nzombies != 0, 0) && old)
        for (int i = 0; i < nzombies; i++)
            if ((const void *)zombies[i] == old) { zombies_sweep(); return; }
}

static void rs_defaults(void)
{
    memset(&S, 0, sizeof S);
    DWORD *rs = S.rs;
    float f1 = 1.0f, f64 = 64.0f;
    rs[D3DRS_ZENABLE] = cur_pp.EnableAutoDepthStencil ? D3DZB_TRUE : D3DZB_FALSE;
    rs[D3DRS_FILLMODE] = D3DFILL_SOLID;
    rs[D3DRS_SHADEMODE] = D3DSHADE_GOURAUD;
    rs[D3DRS_ZWRITEENABLE] = TRUE;
    rs[D3DRS_LASTPIXEL] = TRUE;
    rs[D3DRS_SRCBLEND] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLEND] = D3DBLEND_ZERO;
    rs[D3DRS_CULLMODE] = D3DCULL_CCW;
    rs[D3DRS_ZFUNC] = D3DCMP_LESSEQUAL;
    rs[D3DRS_ALPHAFUNC] = D3DCMP_ALWAYS;
    memcpy(&rs[D3DRS_FOGEND], &f1, 4);
    memcpy(&rs[D3DRS_FOGDENSITY], &f1, 4);
    rs[D3DRS_STENCILFAIL] = rs[D3DRS_STENCILZFAIL] = rs[D3DRS_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_STENCILMASK] = rs[D3DRS_STENCILWRITEMASK] = 0xffffffff;
    rs[D3DRS_TEXTUREFACTOR] = 0xffffffff;
    rs[D3DRS_CLIPPING] = TRUE;
    rs[D3DRS_LIGHTING] = TRUE;
    rs[D3DRS_COLORVERTEX] = TRUE;
    rs[D3DRS_LOCALVIEWER] = TRUE;
    rs[D3DRS_DIFFUSEMATERIALSOURCE] = D3DMCS_COLOR1;
    rs[D3DRS_SPECULARMATERIALSOURCE] = D3DMCS_COLOR2;
    memcpy(&rs[D3DRS_POINTSIZE], &f1, 4);
    memcpy(&rs[D3DRS_POINTSIZE_MIN], &f1, 4);
    memcpy(&rs[D3DRS_POINTSCALE_A], &f1, 4);
    memcpy(&rs[D3DRS_POINTSIZE_MAX], &f64, 4);
    rs[D3DRS_MULTISAMPLEANTIALIAS] = TRUE;
    rs[D3DRS_MULTISAMPLEMASK] = 0xffffffff;
    rs[D3DRS_COLORWRITEENABLE] = rs[D3DRS_COLORWRITEENABLE1] = rs[D3DRS_COLORWRITEENABLE2] = rs[D3DRS_COLORWRITEENABLE3] = 0xf;
    rs[D3DRS_BLENDOP] = rs[D3DRS_BLENDOPALPHA] = D3DBLENDOP_ADD;
    rs[D3DRS_POSITIONDEGREE] = D3DDEGREE_CUBIC;
    rs[D3DRS_NORMALDEGREE] = D3DDEGREE_LINEAR;
    memcpy(&rs[D3DRS_MINTESSELLATIONLEVEL], &f1, 4);
    memcpy(&rs[D3DRS_MAXTESSELLATIONLEVEL], &f1, 4);
    memcpy(&rs[D3DRS_ADAPTIVETESS_W], &f1, 4);
    rs[D3DRS_CCW_STENCILFAIL] = rs[D3DRS_CCW_STENCILZFAIL] = rs[D3DRS_CCW_STENCILPASS] = D3DSTENCILOP_KEEP;
    rs[D3DRS_CCW_STENCILFUNC] = D3DCMP_ALWAYS;
    rs[D3DRS_BLENDFACTOR] = 0xffffffff;
    rs[D3DRS_SRCBLENDALPHA] = D3DBLEND_ONE;
    rs[D3DRS_DESTBLENDALPHA] = D3DBLEND_ZERO;
    for (int s = 0; s < NSAMP; s++) {
        S.ss[s][D3DSAMP_ADDRESSU] = S.ss[s][D3DSAMP_ADDRESSV] = S.ss[s][D3DSAMP_ADDRESSW] = D3DTADDRESS_WRAP;
        S.ss[s][D3DSAMP_MAGFILTER] = S.ss[s][D3DSAMP_MINFILTER] = D3DTEXF_POINT;
        S.ss[s][D3DSAMP_MIPFILTER] = D3DTEXF_NONE;
        S.ss[s][D3DSAMP_MAXANISOTROPY] = 1;
    }
    for (int t = 0; t < 8; t++) {
        S.tss[t][D3DTSS_COLOROP] = t ? D3DTOP_DISABLE : D3DTOP_MODULATE;
        S.tss[t][D3DTSS_ALPHAOP] = t ? D3DTOP_DISABLE : D3DTOP_SELECTARG1;
        S.tss[t][D3DTSS_COLORARG1] = S.tss[t][D3DTSS_ALPHAARG1] = D3DTA_TEXTURE;
        S.tss[t][D3DTSS_COLORARG2] = S.tss[t][D3DTSS_ALPHAARG2] = D3DTA_CURRENT;
        S.tss[t][D3DTSS_TEXCOORDINDEX] = (DWORD)t;
        S.tss[t][D3DTSS_COLORARG0] = S.tss[t][D3DTSS_ALPHAARG0] = S.tss[t][D3DTSS_RESULTARG] = D3DTA_CURRENT;
    }
    for (int i = 0; i < VK_LIGHTS; i++) {   /* (Direct3D's light a GetLight of an unset index gives: DXVK's DefaultLight) */
        S.light[i].Type = D3DLIGHT_DIRECTIONAL;
        S.light[i].Diffuse.r = S.light[i].Diffuse.g = S.light[i].Diffuse.b = 1.0f;
        S.light[i].Direction.z = 1.0f;
    }
    for (int i = 0; i < 16; i++) S.freq[i] = 1;
    memset(xforms, 0, sizeof xforms);
    for (int i = 0; i < 512; i++) xforms[i]._11 = xforms[i]._22 = xforms[i]._33 = xforms[i]._44 = 1.0f;
    DIRTY_ALL();
}

/* ------------------------------------------------------------------ textures and surfaces */
static VkImageView make_view(VkTex *t, VkImageViewType type, VkImageAspectFlags aspect, uint32_t level, uint32_t nlevels,
                             uint32_t layer, uint32_t nlayers, int swizzle)
{
    const Fmt *f = fmt_of(t->fmt);
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, t->image, type, t->vkfmt,
        swizzle && f ? (VkComponentMapping){f->r, f->g, f->b, f->a} : (VkComponentMapping){0, 0, 0, 0},
        {aspect, level, nlevels, layer, nlayers}};
    VkImageView v = VK_NULL_HANDLE;
    if (vkCreateImageView(R.dev, &vi, NULL, &v) != VK_SUCCESS) LOG(LOG_ALWAYS, "vulkan: image view failed");
    return v;
}

static void end_pass(void);

/* ------------------------------------------------------------------ barriers (VULKAN.md "Render passes")
   Every image lives in GENERAL. What render passes and transfers did since the last barrier towards
   graphics waits for the next render pass (one barrier before it, scoped to the stages that read and
   write images there), what they did since the last barrier towards transfers for the next copy,
   resolve or blit; reads count as sources too (a later write waits for them). Round 1 had a full
   barrier (every stage, all memory) after each pass and around every copy. */
#define GFX_STAGES (VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | \
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT | \
    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT)
#define GFX_ACCESS (VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | \
    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | \
    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define PASS_WRITES (VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT)
#define XFER_STAGES VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT
#define XFER_ACCESS (VK_ACCESS_2_TRANSFER_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT)

static struct { VkPipelineStageFlags2 stage; VkAccessFlags2 access; } pend_gfx, pend_xfer;
/* the work since the last barrier towards graphics (xfer 0) or transfers (1) before what comes next */
static void barrier_to(VkCommandBuffer cb, int xfer)
{
    typeof(pend_gfx) *p = xfer ? &pend_xfer : &pend_gfx;
    if (!p->stage) return;
    VkMemoryBarrier2 mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, p->stage, p->access,
        xfer ? XFER_STAGES : GFX_STAGES, xfer ? XFER_ACCESS : GFX_ACCESS};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
    p->stage = 0;
    p->access = 0;
}

/* work recorded that later work must wait for (stages; the writes among its accesses) */
static void did(VkPipelineStageFlags2 stage, VkAccessFlags2 writes)
{
    pend_gfx.stage |= stage, pend_gfx.access |= writes;
    pend_xfer.stage |= stage, pend_xfer.access |= writes;
}

/* a full barrier recorded (every stage, all memory): nothing pending */
static void barrier_all(VkCommandBuffer cb)
{
    vk_barrier_all(cb);
    pend_gfx.stage = pend_gfx.access = pend_xfer.stage = pend_xfer.access = 0;
}

/* a new image (and its views, slot) for t; t's fields describe it */
static int tex_image(VkTex *t)
{
    const Fmt *f = fmt_of(t->fmt);
    if (!f || f->vk == VK_FORMAT_UNDEFINED) {
        LOG(LOG_ALWAYS, "vulkan renderer: texture format %u is not supported", (unsigned)t->fmt);
        return 0;
    }
    t->vkfmt = f->vk;
    t->depth = f->depth;
    t->stencil = f->stencil && (t->vkfmt == VK_FORMAT_D24_UNORM_S8_UINT || t->vkfmt == VK_FORMAT_D32_SFLOAT_S8_UINT);
    t->has_alpha = f->alpha;
    VkImageUsageFlags use = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t->samples <= 1) use |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (f->depth) use |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else if ((t->usage & D3DUSAGE_RENDERTARGET) || t->type == D3DRTYPE_SURFACE)
        use |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (f->block > 1) use &= ~VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkImageCreateFlags cflags = t->faces == 6 ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
    if (f->depth && t->samples > 1 && R.sample_locations) cflags |= VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cflags,
        VK_IMAGE_TYPE_2D, t->vkfmt, {t->w, t->h, 1}, t->levels, t->faces, (VkSampleCountFlagBits)(t->samples ? t->samples : 1),
        VK_IMAGE_TILING_OPTIMAL, use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(R.dev, &ii, NULL, &t->image) != VK_SUCCESS) {
        LOG(LOG_ALWAYS, "vulkan: image %ux%u format %d failed", t->w, t->h, t->vkfmt);
        return 0;
    }
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(R.dev, t->image, &mr);
    if (!vkm_alloc(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &t->mem)) {
        vkDestroyImage(R.dev, t->image, NULL);
        t->image = VK_NULL_HANDLE;
        return 0;
    }
    vkBindImageMemory(R.dev, t->image, t->mem.mem, t->mem.off);
    VkImageAspectFlags aspect = f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    /* every image lives in GENERAL (VULKAN.md "Render passes"): there from the start */
    end_pass();
    VkCommandBuffer cb = vk_cmd();
    /* (a depth format without stencil may sit in a depth/stencil Vulkan format, D24X8 on AMD's
       D32_SFLOAT_S8_UINT: the layout covers both aspects of such an image) */
    int vk_stencil = t->vkfmt == VK_FORMAT_D24_UNORM_S8_UINT || t->vkfmt == VK_FORMAT_D32_SFLOAT_S8_UINT;
    vk_image_layout(cb, t->image, aspect | (vk_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_GENERAL, t->faces);
    if (t->samples <= 1) {
        t->slot = vk_tex_slot();
        if (t->faces == 6) {
            t->kind = 1;
            t->view = make_view(t, VK_IMAGE_VIEW_TYPE_CUBE, aspect, 0, t->levels, 0, 6, 1);
            if (t->slot) vk_tex_write(t->slot, VK_NULL_HANDLE, t->view, VK_NULL_HANDLE, VK_NULL_HANDLE);
        } else {
            t->kind = f->shadow ? 3 : 0;
            t->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, aspect, 0, t->levels, 0, 1, !f->depth || !f->shadow);
            if (f->depth && f->shadow) t->view_depth = t->view;
            if (t->slot) vk_tex_write(t->slot, t->view, VK_NULL_HANDLE, VK_NULL_HANDLE, t->view_depth);
        }
    }
    return 1;
}

static VkSurf *surf_new(VkTex *t, uint32_t level, uint32_t face, int standalone)
{
    VkSurf *s = calloc(1, sizeof *s);
    obj_init(&s->o, &vt_surf, DK_IDirect3DSurface9);
    s->tex = t;
    s->level = level;
    s->face = face;
    s->standalone = standalone;
    if (t->image) {
        const Fmt *f = fmt_of(t->fmt);
        int attach = f && (f->depth || (t->usage & D3DUSAGE_RENDERTARGET) || t->type == D3DRTYPE_SURFACE) && f->block == 1;
        if (attach)
            s->view = make_view(t, VK_IMAGE_VIEW_TYPE_2D, f->depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (t->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                                                 : VK_IMAGE_ASPECT_COLOR_BIT, level, 1, face, 1, 0);
    }
    return s;
}

static uint32_t lvl_dim(uint32_t d, uint32_t level) { d >>= level; return d ? d : 1; }

/* the program's bytes of one level and face: pitch and rows */
static void sub_layout(VkTex *t, uint32_t level, uint32_t *pitch, uint32_t *rows)
{
    const Fmt *f = fmt_of(t->fmt);
    uint32_t w = lvl_dim(t->w, level), h = lvl_dim(t->h, level);
    uint32_t bw = (w + f->block - 1) / f->block, bh = (h + f->block - 1) / f->block;
    *pitch = (bw * f->bpb + 3) & ~3u;   /* (aligned to 4 as DXVK's locks: the program sees the same pitch) */
    *rows = bh;
}

/* a texture, cube texture or standalone surface */
static VkTex *tex_new(D3DRESOURCETYPE type, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                      UINT samples)
{
    VkTex *t = calloc(1, sizeof *t);
    int kind = type == D3DRTYPE_CUBETEXTURE ? DK_IDirect3DCubeTexture9 : type == D3DRTYPE_TEXTURE ? DK_IDirect3DTexture9 : DK_IDirect3DSurface9;
    obj_init(&t->o, type == D3DRTYPE_CUBETEXTURE ? (void *)&vt_cube : (void *)&vt_tex, kind);
    t->type = type;
    t->fmt = fmt;
    t->usage = usage;
    t->pool = pool;
    t->w = w ? w : 1;
    t->h = h ? h : 1;
    t->samples = samples > 1 ? samples : 1;
    t->faces = type == D3DRTYPE_CUBETEXTURE ? 6 : 1;
    if (!levels) {
        uint32_t m = t->w > t->h ? t->w : t->h;
        levels = 1;
        while (m > 1) { m >>= 1; levels++; }
    }
    t->levels = levels;
    if (pool == D3DPOOL_SYSTEMMEM || pool == D3DPOOL_SCRATCH) {
        if (!fmt_of(fmt)) { free(t); return NULL; }
    } else if (!tex_image(t)) {
        free(t);
        return NULL;
    }
    t->surfs = calloc(t->levels * t->faces, sizeof *t->surfs);
    if (type != D3DRTYPE_SURFACE)
        for (uint32_t f = 0; f < t->faces; f++)
            for (uint32_t l = 0; l < t->levels; l++)
                t->surfs[f * t->levels + l] = surf_new(t, l, f, 0);
    return t;
}

/* a standalone surface: a texture of its own, the surface object holding the references */
static VkSurf *surf_standalone(UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, DWORD usage, UINT samples)
{
    VkTex *t = tex_new(D3DRTYPE_SURFACE, w, h, 1, usage, fmt, pool, samples);
    if (!t) return NULL;
    VkSurf *s = surf_new(t, 0, 0, 1);
    t->surfs[0] = s;
    return s;
}

static void tex_destroy(VkTex *t)
{
    if (t->surfs)
        for (uint32_t i = 0; i < t->levels * t->faces; i++) {
            VkSurf *s = t->surfs[i];
            if (!s) continue;
            priv_free(&s->o);
            if (s->view) vk_later(LATER_VIEW, s->view, NULL);
            free(s->shadow);
            if (!s->standalone) free(s);
        }
    priv_free(&t->o);
    if (t->view) vk_later(LATER_VIEW, t->view, NULL);
    if (t->view_depth && t->view_depth != t->view) vk_later(LATER_VIEW, t->view_depth, NULL);
    if (t->slot) vk_tex_slot_free(t->slot);
    if (t->image) vk_later(LATER_IMAGE, t->image, NULL);
    if (t->mem.blk) vk_later_alloc(&t->mem);
    free(t->surfs);
    free(t->sysmem);
    free(t);
}

/* bytes into one level and face of the image (recorded in order with the drawing) */
static void upload(VkTex *t, uint32_t level, uint32_t face, const uint8_t *data, uint32_t pitch, uint32_t rows)
{
    if (!t->image) return;
    const Fmt *f = fmt_of(t->fmt);
    VkRing r;
    uint32_t bytes = pitch * rows;
    if (!vk_ring(bytes, 16, &r)) return;
    memcpy(r.map, data, bytes);
    end_pass();
    VkCommandBuffer cb = vk_cmd();
    barrier_to(cb, 1);
    VkBufferImageCopy c = {r.off, (pitch / f->bpb) * f->block, 0,
        {f->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, level, face, 1},
        {0, 0, 0}, {lvl_dim(t->w, level), lvl_dim(t->h, level), 1}};
    vkCmdCopyBufferToImage(cb, r.buf, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    did(XFER_STAGES, VK_ACCESS_2_TRANSFER_WRITE_BIT);
}

static HRESULT surf_lock(VkSurf *s, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    VkTex *t = s->tex;
    if (!lr) return D3DERR_INVALIDCALL;
    uint32_t pitch, rows;
    sub_layout(t, s->level, &pitch, &rows);
    if (!s->shadow) {
        if (t->sysmem && s->standalone) s->shadow = t->sysmem, t->sysmem = NULL;
        else s->shadow = calloc(1, (size_t)pitch * rows + 16);
    }
    const Fmt *f = fmt_of(t->fmt);
    uint32_t off = 0;
    if (rect) off = (uint32_t)(rect->top / f->block) * pitch + (uint32_t)(rect->left / f->block) * f->bpb;
    lr->Pitch = (INT)pitch;
    lr->pBits = s->shadow + off;
    s->locked = 1;
    s->lock_flags = (int)flags;
    return D3D_OK;
}

static HRESULT surf_unlock(VkSurf *s)
{
    if (!s->locked) return D3DERR_INVALIDCALL;
    s->locked = 0;
    if (!(s->lock_flags & D3DLOCK_READONLY) && s->tex->image) {
        uint32_t pitch, rows;
        sub_layout(s->tex, s->level, &pitch, &rows);
        upload(s->tex, s->level, s->face, s->shadow, pitch, rows);
    }
    return D3D_OK;
}

/* IDirect3DSurface9 */
static D3DRESOURCETYPE surf_GetType(void *This) { (void)This; return D3DRTYPE_SURFACE; }
static HRESULT surf_GetContainer(void *This, REFIID riid, void **out)
{
    VkSurf *s = This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = NULL;
    if (!s->standalone && s->tex) return com_QueryInterface(&s->tex->o, riid, out);
    if (memcmp(riid, &IID_IDirect3DDevice9, sizeof(IID))) return E_NOINTERFACE;
    return res_GetDevice(This, (IDirect3DDevice9 **)out);
}
static HRESULT surf_GetDesc(void *This, D3DSURFACE_DESC *d)
{
    VkSurf *s = This;
    VkTex *t = s->tex;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = t->fmt;
    d->Type = D3DRTYPE_SURFACE;
    d->Usage = t->usage;
    d->Pool = t->pool;
    d->MultiSampleType = t->samples > 1 ? (D3DMULTISAMPLE_TYPE)t->samples : D3DMULTISAMPLE_NONE;
    d->MultiSampleQuality = 0;
    d->Width = lvl_dim(t->w, s->level);
    d->Height = lvl_dim(t->h, s->level);
    return D3D_OK;
}
static HRESULT surf_LockRect(void *This, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags) { return surf_lock(This, lr, rect, flags); }
static HRESULT surf_UnlockRect(void *This) { return surf_unlock(This); }
static HRESULT surf_GetDC(void *This, HDC *dc) { (void)This; (void)dc; UNSUPPORTED("GetDC"); return D3DERR_INVALIDCALL; }
static HRESULT surf_ReleaseDC(void *This, HDC dc) { (void)This; (void)dc; return D3DERR_INVALIDCALL; }

/* IDirect3DTexture9 / IDirect3DCubeTexture9 */
static D3DRESOURCETYPE tex_GetType(void *This) { return ((VkTex *)This)->type; }
static DWORD tex_SetLOD(void *This, DWORD lod) { (void)This; (void)lod; return 0; }
static DWORD tex_GetLOD(void *This) { (void)This; return 0; }
static DWORD tex_GetLevelCount(void *This) { return ((VkTex *)This)->levels; }
static HRESULT tex_SetAutoGenFilterType(void *This, D3DTEXTUREFILTERTYPE f) { (void)This; (void)f; return D3D_OK; }
static D3DTEXTUREFILTERTYPE tex_GetAutoGenFilterType(void *This) { (void)This; return D3DTEXF_LINEAR; }
static void tex_GenerateMipSubLevels(void *This) { (void)This; UNSUPPORTED("GenerateMipSubLevels"); }
static HRESULT tex_GetLevelDesc(void *This, UINT level, D3DSURFACE_DESC *d)
{
    VkTex *t = This;
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    HRESULT r = surf_GetDesc(t->surfs[level], d);
    if (r == D3D_OK) d->Type = D3DRTYPE_SURFACE;
    return r;
}
static HRESULT tex_GetSurfaceLevel(void *This, UINT level, IDirect3DSurface9 **out)
{
    VkTex *t = This;
    if (!out || level >= t->levels) return D3DERR_INVALIDCALL;
    com_AddRef(&t->o);
    *out = (IDirect3DSurface9 *)t->surfs[level];
    return D3D_OK;
}
static HRESULT tex_LockRect(void *This, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    VkTex *t = This;
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    return surf_lock(t->surfs[level], lr, rect, flags);
}
static HRESULT tex_UnlockRect(void *This, UINT level)
{
    VkTex *t = This;
    if (level >= t->levels) return D3DERR_INVALIDCALL;
    return surf_unlock(t->surfs[level]);
}
static HRESULT tex_AddDirtyRect(void *This, const RECT *r) { (void)This; (void)r; return D3D_OK; }
static HRESULT cube_GetCubeMapSurface(void *This, D3DCUBEMAP_FACES face, UINT level, IDirect3DSurface9 **out)
{
    VkTex *t = This;
    if (!out || level >= t->levels || (unsigned)face >= 6) return D3DERR_INVALIDCALL;
    com_AddRef(&t->o);
    *out = (IDirect3DSurface9 *)t->surfs[face * t->levels + level];
    return D3D_OK;
}
static HRESULT cube_LockRect(void *This, D3DCUBEMAP_FACES face, UINT level, D3DLOCKED_RECT *lr, const RECT *rect, DWORD flags)
{
    VkTex *t = This;
    if (level >= t->levels || (unsigned)face >= 6) return D3DERR_INVALIDCALL;
    return surf_lock(t->surfs[face * t->levels + level], lr, rect, flags);
}
static HRESULT cube_UnlockRect(void *This, D3DCUBEMAP_FACES face, UINT level)
{
    VkTex *t = This;
    if (level >= t->levels || (unsigned)face >= 6) return D3DERR_INVALIDCALL;
    return surf_unlock(t->surfs[face * t->levels + level]);
}
static HRESULT cube_AddDirtyRect(void *This, D3DCUBEMAP_FACES face, const RECT *r) { (void)This; (void)face; (void)r; return D3D_OK; }

/* ------------------------------------------------------------------ vertex and index buffers */
/* A buffer's bytes: the program locks a host copy (shadow), and an unlock writes the range into the
   GPU's copy (a range of an arena, host-visible). Draws already recorded in the command buffer being
   built, or in submitted work not yet finished, may still read the GPU copy: then the buffer moves to
   a new range first (the whole host copy written into it), unless the lock said NOOVERWRITE; DISCARD
   always moves it. The old range goes once that work is done.
   The host copy is the program's own (round 3): d3d9.c keeps one in the program's memory for every
   buffer the program locks (its locks hand that out) and gives it to us at its first lock
   (vk_buf_host), so its unlock's copy into ours is a copy onto itself (skipped there) and the bytes
   go from the program's copy to the GPU's once: 1.6 MB a race frame less copied, and no second
   host copy of the ~27,000 buffers (they start as zeros in both). ETM_VK_BUF_HOST=0: our own
   copy as before (before/after measurements). */
static VkBuf *buf_new(UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, D3DFORMAT fmt, int index)
{
    VkBuf *b = calloc(1, sizeof *b);
    obj_init(&b->o, index ? (void *)&vt_ib : (void *)&vt_vb, index ? DK_IDirect3DIndexBuffer9 : DK_IDirect3DVertexBuffer9);
    b->size = len;
    b->usage = usage;
    b->fvf = fvf;
    b->pool = pool;
    b->fmt = index ? fmt : D3DFMT_VERTEXDATA;
    b->shadow = calloc(1, len + 16);
    b->own_shadow = 1;
    if (!vkb_sub(len ? len : 16, &b->sub)) {
        free(b->shadow);
        free(b);
        return NULL;
    }
    return b;
}

/* d3d9.c: the program's copy of the buffer (as many bytes, zeros like ours) is its host copy from
   now on (the program's lock, before it writes anything) */
void vk_buf_host(void *buf, void *host)
{
    VkBuf *b = buf;
    static int on = -1;
    if (on < 0) on = !(getenv("ETM_VK_BUF_HOST") && *getenv("ETM_VK_BUF_HOST") == '0');
    if (!on || !b->own_shadow || b->lock_depth) return;
    free(b->shadow);
    b->shadow = host;
    b->own_shadow = 0;
}

static void buf_destroy(VkBuf *b)
{
    priv_free(&b->o);
    vk_later_sub(&b->sub);
    if (b->own_shadow) free(b->shadow);
    free(b);
}

static int in_flight(uint64_t used) { return used > R.done; }

static void buf_write(VkBuf *b, uint32_t off, uint32_t len, DWORD flags)
{
    uint64_t cur = R.serial + 1;
    int busy = b->used == cur ? off < b->used_hi && off + len > b->used_lo : in_flight(b->used);
    if ((flags & D3DLOCK_DISCARD) || (busy && !(flags & D3DLOCK_NOOVERWRITE))) {
        VkSub n;
        if (vkb_sub(b->size ? b->size : 16, &n)) {
            vk_later_sub(&b->sub);
            b->sub = n;
            memcpy(b->sub.map, b->shadow, b->size);
            b->used = 0;
            b->used_lo = b->used_hi = 0;
            return;
        }
    }
    memcpy(b->sub.map + off, b->shadow + off, len);
}

static HRESULT buf_Lock(void *This, UINT off, UINT len, void **data, DWORD flags)
{
    VkBuf *b = This;
    if (!data) return D3DERR_INVALIDCALL;
    if (off > b->size) off = b->size;
    if (!len || off + len > b->size) len = b->size - off;
    if (b->lock_depth < 4) {
        b->lock_off[b->lock_depth] = off;
        b->lock_len[b->lock_depth] = len;
        b->lock_flags[b->lock_depth] = flags;
    }
    b->lock_depth++;
    *data = b->shadow + off;
    return D3D_OK;
}

static HRESULT buf_Unlock(void *This)
{
    VkBuf *b = This;
    if (b->lock_depth <= 0) return D3D_OK;
    b->lock_depth--;
    if (b->lock_depth < 4) {
        int d = b->lock_depth;
        if (!(b->lock_flags[d] & D3DLOCK_READONLY))
            buf_write(b, b->lock_off[d], b->lock_len[d], b->lock_flags[d]);
    } else {
        buf_write(b, 0, b->size, 0);
    }
    return D3D_OK;
}

static D3DRESOURCETYPE vb_GetType(void *This) { (void)This; return D3DRTYPE_VERTEXBUFFER; }
static D3DRESOURCETYPE ib_GetType(void *This) { (void)This; return D3DRTYPE_INDEXBUFFER; }
static HRESULT vb_GetDesc(void *This, D3DVERTEXBUFFER_DESC *d)
{
    VkBuf *b = This;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = D3DFMT_VERTEXDATA;
    d->Type = D3DRTYPE_VERTEXBUFFER;
    d->Usage = b->usage;
    d->Pool = b->pool;
    d->Size = b->size;
    d->FVF = b->fvf;
    return D3D_OK;
}
static HRESULT ib_GetDesc(void *This, D3DINDEXBUFFER_DESC *d)
{
    VkBuf *b = This;
    if (!d) return D3DERR_INVALIDCALL;
    d->Format = b->fmt;
    d->Type = D3DRTYPE_INDEXBUFFER;
    d->Usage = b->usage;
    d->Pool = b->pool;
    d->Size = b->size;
    return D3D_OK;
}

/* ------------------------------------------------------------------ declarations and shaders */
static uint32_t next_id = 1;

static HRESULT decl_GetDeclaration(void *This, D3DVERTEXELEMENT9 *el, UINT *n)
{
    VkDecl *d = This;
    if (!n) return D3DERR_INVALIDCALL;
    if (el) memcpy(el, d->el, sizeof(D3DVERTEXELEMENT9) * (size_t)(d->nel + 1));
    *n = (UINT)d->nel + 1;
    return D3D_OK;
}

static uint32_t shader_tokens(const DWORD *w)
{
    if (w[0] >> 16 != 0xfffe && w[0] >> 16 != 0xffff) return 0;
    uint32_t major = (w[0] >> 8) & 0xff, i = 1;
    while (i < 0x10000) {
        uint32_t t = w[i], op = t & 0xffff, n;
        if (t == 0xffff) return i + 1;
        if (op == 0xfffe) n = (t >> 16) & 0x7fff;
        else if (major >= 2) n = (t >> 24) & 15;
        else if (op == 81) n = 5;
        else for (n = 0; w[i + 1 + n] & 0x80000000u; n++) ;
        i += 1 + n;
    }
    return 0;
}

static VkShader *shader_new(const DWORD *code, int ps)
{
    uint32_t n = code ? shader_tokens(code) : 0;
    if (!n) return NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (uint32_t i = 0; i < n * 4; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    VkShader *s = calloc(1, sizeof *s);
    obj_init(&s->o, ps ? (void *)&vt_ps : (void *)&vt_vs, ps ? DK_IDirect3DPixelShader9 : DK_IDirect3DVertexShader9);
    s->hash = h;
    s->id = next_id++;
    s->module = module_of(h, &s->meta);
    if (!s->meta) {
        LOG(LOG_ALWAYS, "vulkan renderer: %s shader %016llx (%u tokens) has no SPIR-V: draws with it are skipped",
            ps ? "pixel" : "vertex", (unsigned long long)h, n);
        return s;
    }
    for (uint32_t d = 0; d < s->meta->ndef; d++)
        s->defmask |= 1u << (vk_shader_defs[s->meta->def0 + d].reg / VK_VS_CHUNK);
    return s;
}

static HRESULT shader_GetFunction(void *This, void *data, UINT *size)
{
    (void)This; (void)data;
    if (size) *size = 0;
    UNSUPPORTED("GetFunction");
    return D3DERR_INVALIDCALL;
}

/* ------------------------------------------------------------------ destruction */
static void destroy(VkObj *o)
{
    switch (o->kind) {
    case DK_IDirect3DTexture9: case DK_IDirect3DCubeTexture9:
        tex_destroy((VkTex *)o);
        break;
    case DK_IDirect3DSurface9: {
        VkSurf *s = (VkSurf *)o;
        VkTex *t = s->tex;
        if (t) tex_destroy(t);              /* (frees its surfaces' views and private data) */
        free(s);
        break;
    }
    case DK_IDirect3DVertexBuffer9: case DK_IDirect3DIndexBuffer9:
        buf_destroy((VkBuf *)o);
        break;
    case DK_IDirect3DQuery9: {
        VkQuery *q = (VkQuery *)o;
        if (q->slot >= 0 && nqfree < 4096) qfree[nqfree++] = q->slot;
        priv_free(o);
        free(q);
        break;
    }
    case DK_IDirect3DVertexShader9: case DK_IDirect3DPixelShader9: {
        VkShader *s = (VkShader *)o;
        priv_free(o);                       /* (its module stays: shared, module_of) */
        free(s);
        break;
    }
    default:
        priv_free(o);
        free(o);
        break;
    }
}

/* ------------------------------------------------------------------ state blocks */
typedef struct { VkObj o; St st; } VkSB;

static void st_refs(St *st, int add)
{
    void (*f)(VkObj *) = add ? iref : iunref;
    for (int i = 0; i < NSAMP; i++) if (st->tex[i]) f(&st->tex[i]->o);
    for (int i = 0; i < 16; i++) if (st->vb[i]) f(&st->vb[i]->o);
    if (st->ib) f(&st->ib->o);
    if (st->decl) f(&st->decl->o);
    if (st->vs) f(&st->vs->o);
    if (st->ps) f(&st->ps->o);
}

static HRESULT sb_Capture(void *This)
{
    VkSB *b = This;
    st_refs(&S, 1);
    st_refs(&b->st, 0);
    b->st = S;
    return D3D_OK;
}

static HRESULT sb_Apply(void *This)
{
    VkSB *b = This;
    S = b->st;                              /* (the block keeps its references; the slots take none) */
    zombies_sweep();
    DIRTY_ALL();
    B.slots_valid = 0;
    return D3D_OK;
}

/* ------------------------------------------------------------------ render passes */
/* a Clear of the whole target before the pass's first draw: the pass's load operation instead
   (begin_pass), so the GPU clears as it starts (fast clears) rather than drawing the clear */
static struct { DWORD flags; float color[4]; float z; uint32_t stencil; } pclear;
static int begin_pass(void);

static void end_pass(void)
{
    if (!B.pass) {
        if (!pclear.flags || !begin_pass()) {   /* (a clear waiting: an empty pass that clears) */
            pclear.flags = 0;
            return;
        }
    }
    vkCmdEndRendering(B.cb);
    did(GFX_STAGES, PASS_WRITES);
    B.pass = 0;
}

static void new_cb_check(void)
{
    VkCommandBuffer cb = vk_cmd();
    if (cb != B.cb) {
        B.cb = cb;
        B.pipe = VK_NULL_HANDLE;
        B.pass = 0;
        B.slots_valid = B.consts_valid = B.dyn_valid = 0;
        B.vvalid = 0;
        B.pvalid = 0;
        B.vcontig = 0;
        memset(B.vbuf, 0, sizeof B.vbuf);
        B.ibuf = VK_NULL_HANDLE;
        B.zero_bound = 0;
        B.topo = VK_PRIMITIVE_TOPOLOGY_MAX_ENUM;
        dirty_dyn = DYN_ALL;
    }
}

static int begin_pass(void)
{
    new_cb_check();
    if (B.pass) return 1;
    VkSurf *rt = rts[0], *ds = dsurf;
    if (!rt || !rt->view) return 0;
    uint32_t w = lvl_dim(rt->tex->w, rt->level), h = lvl_dim(rt->tex->h, rt->level);
    if (ds && (!ds->view || lvl_dim(ds->tex->w, ds->level) < w || lvl_dim(ds->tex->h, ds->level) < h ||
               ds->tex->samples != rt->tex->samples)) {
        if (once_("depth buffer mismatch"))
            LOG(LOG_ALWAYS, "vulkan renderer: a depth buffer smaller than its target or of another sample count: left out");
        ds = NULL;
    }
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, rt->view, VK_IMAGE_LAYOUT_GENERAL,
        VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
    VkRenderingAttachmentInfo da = ca, sa = ca;
    if (ds) da.imageView = sa.imageView = ds->view;
    if (pclear.flags & D3DCLEAR_TARGET) {
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        memcpy(ca.clearValue.color.float32, pclear.color, 16);
    }
    if (ds && (pclear.flags & D3DCLEAR_ZBUFFER)) {
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.clearValue.depthStencil.depth = pclear.z;
    }
    if (ds && ds->tex->stencil && (pclear.flags & D3DCLEAR_STENCIL)) {
        sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        sa.clearValue.depthStencil.stencil = pclear.stencil;
    }
    if (rt == backbuf && sc_frame != 2) {
        /* (the back buffer's passes straight into the swapchain image; the frame's first starts from
           nothing unless it clears: the swap effect is DISCARD) */
        VkImage sci;
        VkImageView scv;
        if (vk_sc_target(w, h, rt->tex->vkfmt, &sci, &scv)) {
            new_cb_check();             /* (vk_sc_target may have begun a new command buffer) */
            ca.imageView = scv;
            if (!sc_frame && !(pclear.flags & D3DCLEAR_TARGET)) ca.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            sc_frame = 1;
        }
    }
    pclear.flags = 0;
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, 1, &ca,
                          ds ? &da : NULL, ds && ds->tex->stencil ? &sa : NULL};
    barrier_to(B.cb, 0);
    vkCmdBeginRendering(B.cb, &ri);
    B.pass = 1;
    B.prt = rt;
    B.pds = ds;
    B.pw = w;
    B.ph = h;
    B.center = 0;
    /* (bound pipeline and dynamic state carry over: command buffer state; what depends on the
       pass, the scissor's clamp, depth and stencil with or without a depth buffer, the formats in
       the pipeline's key, is looked at again) */
    dirty_dyn |= DYN_VP | DYN_DEPTH | DYN_STENCIL;
    dirty_pipe = 1;
    return 1;
}

/* ------------------------------------------------------------------ pipelines
   A pipeline's key is what it is made from, by content: the shaders' bytecode hashes, the
   declaration's elements' hash (the elements kept with the entry), the fixed states, the target
   formats, the specialization constants; so a key means the same in every run. The pipelines made
   are listed in the user folder (vk_pipelines.bin, with the declarations' elements) beside the
   driver's pipeline cache data (vk_pipeline_cache.bin, vk_core.c), and at the next start worker
   threads make every listed pipeline from the SPIR-V table while the game loads (VULKAN.md "State
   and pipelines"): a draw that needs one being made waits for it, one not listed is made at once. */
typedef struct {
    uint64_t vs, ps, decl;
    uint8_t topo, fill, samples, blend;
    uint8_t src, dst, op, srca, dsta, opa, wmask, center;
    uint32_t rtfmt, dsfmt;
    uint32_t spec[VK_SPEC_COUNT];        /* the pixel shader's (programmable 0-4, fixed function all) */
    uint32_t vspec[VK_FFV_COUNT];        /* the fixed-function vertex shader's (0 with a programmable one) */
} PipeKey;

#define PCAP 4096
typedef struct {
    PipeKey k;
    VkPipeline p;
    int state;                           /* 0 empty, 1 being made, 2 made (p VK_NULL_HANDLE: it failed) */
    int nel;
    D3DVERTEXELEMENT9 *el;               /* the declaration's elements (for the list) */
} PipeEnt;
static PipeEnt pipes[PCAP];
static int pipe_order[PCAP], npipes;    /* the entries in the order first asked for */
static pthread_mutex_t pipe_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t pipe_made = PTHREAD_COND_INITIALIZER;
static double pipe_ms_game, pipe_ms_worst, pipe_ms_workers;
static int pipes_game, pipes_workers, pipes_listed, pipes_saved;

static VkBlendFactor blend_factor(DWORD b, int alpha_target)
{
    switch (b) {
    case D3DBLEND_ZERO: return VK_BLEND_FACTOR_ZERO;
    case D3DBLEND_ONE: return VK_BLEND_FACTOR_ONE;
    case D3DBLEND_SRCCOLOR: return VK_BLEND_FACTOR_SRC_COLOR;
    case D3DBLEND_INVSRCCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case D3DBLEND_SRCALPHA: return VK_BLEND_FACTOR_SRC_ALPHA;
    case D3DBLEND_INVSRCALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case D3DBLEND_DESTALPHA: return alpha_target ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_ONE;
    case D3DBLEND_INVDESTALPHA: return alpha_target ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ZERO;
    case D3DBLEND_DESTCOLOR: return VK_BLEND_FACTOR_DST_COLOR;
    case D3DBLEND_INVDESTCOLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case D3DBLEND_SRCALPHASAT: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case D3DBLEND_BLENDFACTOR: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case D3DBLEND_INVBLENDFACTOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    default: return VK_BLEND_FACTOR_ONE;
    }
}

static VkBlendOp blend_op(DWORD o)
{
    switch (o) {
    case D3DBLENDOP_SUBTRACT: return VK_BLEND_OP_SUBTRACT;
    case D3DBLENDOP_REVSUBTRACT: return VK_BLEND_OP_REVERSE_SUBTRACT;
    case D3DBLENDOP_MIN: return VK_BLEND_OP_MIN;
    case D3DBLENDOP_MAX: return VK_BLEND_OP_MAX;
    default: return VK_BLEND_OP_ADD;
    }
}

static VkFormat decl_format(BYTE t)
{
    switch (t) {
    case D3DDECLTYPE_FLOAT1: return VK_FORMAT_R32_SFLOAT;
    case D3DDECLTYPE_FLOAT2: return VK_FORMAT_R32G32_SFLOAT;
    case D3DDECLTYPE_FLOAT3: return VK_FORMAT_R32G32B32_SFLOAT;
    case D3DDECLTYPE_FLOAT4: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case D3DDECLTYPE_D3DCOLOR: return VK_FORMAT_B8G8R8A8_UNORM;
    case D3DDECLTYPE_UBYTE4: return VK_FORMAT_R8G8B8A8_USCALED;
    case D3DDECLTYPE_SHORT2: return VK_FORMAT_R16G16_SSCALED;
    case D3DDECLTYPE_SHORT4: return VK_FORMAT_R16G16B16A16_SSCALED;
    case D3DDECLTYPE_UBYTE4N: return VK_FORMAT_R8G8B8A8_UNORM;
    case D3DDECLTYPE_SHORT2N: return VK_FORMAT_R16G16_SNORM;
    case D3DDECLTYPE_SHORT4N: return VK_FORMAT_R16G16B16A16_SNORM;
    case D3DDECLTYPE_USHORT2N: return VK_FORMAT_R16G16_UNORM;
    case D3DDECLTYPE_USHORT4N: return VK_FORMAT_R16G16B16A16_UNORM;
    case D3DDECLTYPE_UDEC3: return VK_FORMAT_A2B10G10R10_USCALED_PACK32;
    case D3DDECLTYPE_DEC3N: return VK_FORMAT_A2B10G10R10_SNORM_PACK32;
    case D3DDECLTYPE_FLOAT16_2: return VK_FORMAT_R16G16_SFLOAT;
    case D3DDECLTYPE_FLOAT16_4: return VK_FORMAT_R16G16B16A16_SFLOAT;
    }
    return VK_FORMAT_UNDEFINED;
}

/* shader modules by bytecode hash, made once (the device's shader objects and the workers share them) */
#define MCAP 512
static struct { uint64_t hash; VkShaderModule m; const VkShaderMeta *meta; int narrow; } mods[MCAP];
static int nmods;
static pthread_mutex_t mod_lock = PTHREAD_MUTEX_INITIALIZER;

static VkShaderModule module_get(uint64_t h, int narrow, const VkShaderMeta **meta);
static VkShaderModule module_of(uint64_t h, const VkShaderMeta **meta) { return module_get(h, 0, meta); }

/* the narrow variant of pixel shader m fits pipeline key k: every slot's kind and the filter among
   those it keeps (d3d2spv.py NARROW; ETM_VK_NARROW=0: never) */
static int narrow_fits(const VkShaderMeta *m, const PipeKey *k)
{
    static int on = -1;
    if (on < 0) on = !(getenv("ETM_VK_NARROW") && *getenv("ETM_VK_NARROW") == '0');
    if (!on || !m || !m->nnwords) return 0;
    for (uint32_t b = m->samplers & ((1u << VK_MAX_SAMPLER_SLOTS) - 1); b; b &= b - 1) {
        int s = __builtin_ctz(b), kind = (int)(k->spec[VK_SPEC_KINDS] >> (2 * s)) & 3;
        if (!((m->nkinds >> (4 * s + kind)) & 1)) return 0;
    }
    uint32_t f = k->spec[VK_SPEC_SHADOWF], fb = f == 0 ? 1 : f == 3 ? 2 : f == 5 ? 4 : f == 7 ? 8 : 0;
    return (m->nfilters & fb) != 0;
}

/* the fixed-function uber-shaders (runtime/shaders/ff.vert, ff.frag; SPIR-V from the Makefile, gen/ff_*.h):
   their "metadata" as for a programmable shader of the table (every fixed-function input; the pixel
   shader's samplers are the stages in use, decided per draw) */
#include "ff_vert.h"
#include "ff_frag.h"
#define FF_INPUTS (0x1fffffu & ~((1u << 13) | (1u << 14) | (1u << 18)))
static const VkShaderMeta ff_vs_meta = {VK_FF_VS_HASH, 0, 0, 2, 0, 0, FF_INPUTS, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
static const VkShaderMeta ff_ps_meta = {VK_FF_PS_HASH, 0, 0, 2, 0, VK_SH_PIXEL, 0, 0xff, 0, 0, VK_FF_PS_CONSTS, 0, 0, 0, 0, 0, 0};

static const VkShaderMeta *meta_of(uint64_t h)
{
    if (h == VK_FF_VS_HASH) return &ff_vs_meta;
    if (h == VK_FF_PS_HASH) return &ff_ps_meta;
    for (int i = 0; i < vk_shader_count; i++)
        if (vk_shader_meta[i].hash == h) return &vk_shader_meta[i];
    return NULL;
}

static VkShaderModule module_get(uint64_t h, int narrow, const VkShaderMeta **meta)
{
    pthread_mutex_lock(&mod_lock);
    for (int i = 0; i < nmods; i++)
        if (mods[i].hash == h && mods[i].narrow == narrow) {
            *meta = mods[i].meta;
            VkShaderModule m = mods[i].m;
            pthread_mutex_unlock(&mod_lock);
            return m;
        }
    const VkShaderMeta *mt = NULL;
    for (int i = 0; i < vk_shader_count; i++)
        if (vk_shader_meta[i].hash == h) { mt = &vk_shader_meta[i]; break; }
    VkShaderModule m = VK_NULL_HANDLE;
    if (h == VK_FF_VS_HASH || h == VK_FF_PS_HASH) {
        /* (glslang's SPIR-V as it is: DXVK hands its own GLSL-built fixed-function shaders over likewise) */
        mt = meta_of(h);
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0,
            h == VK_FF_VS_HASH ? sizeof vk_ff_vert : sizeof vk_ff_frag, h == VK_FF_VS_HASH ? vk_ff_vert : vk_ff_frag};
        if (vkCreateShaderModule(R.dev, &ci, NULL, &m) != VK_SUCCESS) {
            LOG(LOG_ALWAYS, "vulkan: the fixed-function %s shader's module failed", h == VK_FF_VS_HASH ? "vertex" : "pixel");
            m = VK_NULL_HANDLE;
            mt = NULL;
        }
    } else if (mt) {
        /* the device's variant of the module (d3d2spv.py variant, the same rules), as DXVK decides: R.fcon
           keeps RTE and flush-to-zero, R.fc2 the default fast-math mode with the precise operations'
           NoContraction as FPFastMathMode 0; otherwise those go and NoContraction stays */
        const uint32_t *w = vk_spirv_words + (narrow ? mt->noff : mt->off), nw = narrow ? mt->nnwords : mt->nwords;
        uint32_t *v = malloc(nw * 4 + nw * 2), nv = 5;    /* (NoContraction: 3 words -> 4) */
        memcpy(v, w, 20);
        static const uint32_t fc2name[6] = {0x5f565053, 0x5f52484b, 0x616f6c66, 0x6f635f74, 0x6f72746e, 0x0032736c};
        for (uint32_t i = 5; i < nw; ) {
            uint32_t n = w[i] >> 16, op = w[i] & 0xffff, drop = 0;
            if (!n) break;
            if (op == 71 && n == 3 && w[i + 2] == 42 && R.fc2) {                       /* OpDecorate NoContraction */
                uint32_t d[4] = {(4u << 16) | 71, w[i + 1], 40, 0};                    /* -> FPFastMathMode 0 */
                memcpy(v + nv, d, 16), nv += 4;
                i += n;
                continue;
            }
            if (op == 17) drop = (w[i + 1] == 6029 && !R.fc2) || ((w[i + 1] == 4467 || w[i + 1] == 4465) && !R.fcon);
            else if (op == 10) drop = !R.fc2 && n == 7 && !memcmp(w + i + 1, fc2name, sizeof fc2name);   /* OpExtension */
            else if (op == 331) drop = w[i + 2] == 6028 && !R.fc2;                      /* OpExecutionModeId */
            else if (op == 16) drop = (w[i + 2] == 4462 || w[i + 2] == 4460) && !R.fcon; /* OpExecutionMode */
            if (!drop) memcpy(v + nv, w + i, n * 4), nv += n;
            i += n;
        }
        VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nv * 4, v};
        VkResult cr = vkCreateShaderModule(R.dev, &ci, NULL, &m);
        free(v);
        if (cr != VK_SUCCESS) {
            LOG(LOG_ALWAYS, "vulkan: shader module %016llx failed", (unsigned long long)h);
            m = VK_NULL_HANDLE;
            mt = NULL;
        }
    }
    if (nmods < MCAP) mods[nmods++] = (typeof(mods[0])){h, m, mt, narrow};
    pthread_mutex_unlock(&mod_lock);
    *meta = mt;
    return m;
}

/* the pipeline for k from its parts (any thread) */
static VkPipeline make_pipeline(const PipeKey *k, const D3DVERTEXELEMENT9 *el, int nel)
{
    const VkShaderMeta *vm, *pmeta;
    VkShaderModule vmod = module_of(k->vs, &vm), pmod = module_get(k->ps, narrow_fits(meta_of(k->ps), k), &pmeta);
    if (!vm || !pmeta || (vm->flags & VK_SH_PIXEL) || !(pmeta->flags & VK_SH_PIXEL)) return VK_NULL_HANDLE;
    VkVertexInputBindingDescription vb[17];
    VkVertexInputAttributeDescription va[VK_INPUT_LOCS];
    uint32_t nvb = 0, nva = 0, have = 0, streams = 0;
    for (int e = 0; e < nel; e++) {
        int loc = vk_input_loc(el[e].Usage, el[e].UsageIndex);
        if (loc < 0 || !(vm->inputs & (1u << loc)) || (have & (1u << loc))) continue;
        have |= 1u << loc;
        va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)loc, el[e].Stream, decl_format(el[e].Type), el[e].Offset};
        streams |= 1u << el[e].Stream;
    }
    for (int s = 0; s < 16; s++)
        if (streams & (1u << s)) vb[nvb++] = (VkVertexInputBindingDescription){(uint32_t)s, 0, VK_VERTEX_INPUT_RATE_VERTEX};
    uint32_t missing = vm->inputs & ~have;
    if (missing) {
        vb[nvb++] = (VkVertexInputBindingDescription){16, 0, VK_VERTEX_INPUT_RATE_VERTEX};
        for (int l = 0; l < VK_INPUT_LOCS; l++)
            if (missing & (1u << l)) va[nva++] = (VkVertexInputAttributeDescription){(uint32_t)l, 16, VK_FORMAT_R32G32B32A32_SFLOAT, 0};
    }
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0, nvb, vb, nva, va};
    static const VkPrimitiveTopology topo[3] = {VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                                VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, topo[k->topo], VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
        k->fill == D3DFILL_WIREFRAME ? VK_POLYGON_MODE_LINE : k->fill == D3DFILL_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL,
        VK_CULL_MODE_NONE, VK_FRONT_FACE_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
        (VkSampleCountFlagBits)k->samples, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    /* Direct3D's MULTISAMPLEANTIALIAS = FALSE on a multisampled target: every sample at the pixel's
       centre (as DXVK), so coverage is decided at the centre and edges are not partly covered */
    static const VkSampleLocationEXT centre[16] = {{.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f},
        {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}, {.5f, .5f}};
    VkPipelineSampleLocationsStateCreateInfoEXT sls = {VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT, NULL,
        VK_TRUE, {VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, NULL, (VkSampleCountFlagBits)k->samples, {1, 1}, k->samples, centre}};
    if (k->center) ms.pNext = &sls;
    VkPipelineDepthStencilStateCreateInfo dss = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    int alpha_target = 1;
    for (size_t f = 0; f < sizeof fmts / sizeof *fmts; f++)
        if (fmts[f].d3d == (D3DFORMAT)k->rtfmt) alpha_target = fmts[f].alpha;
    VkPipelineColorBlendAttachmentState cba = {k->blend, blend_factor(k->src, alpha_target), blend_factor(k->dst, alpha_target),
        blend_op(k->op), blend_factor(k->srca, alpha_target), blend_factor(k->dsta, alpha_target), blend_op(k->opa), k->wmask};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE,
        VK_LOGIC_OP_COPY, 1, &cba, {0, 0, 0, 0}};
    static const VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_FRONT_FACE, VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP, VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
        VK_DYNAMIC_STATE_DEPTH_BIAS, VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP,
        VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE};
    VkPipelineDynamicStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0,
                                           sizeof dyn / sizeof *dyn, dyn};
    VkSpecializationMapEntry me[VK_SPEC_COUNT];
    for (int s = 0; s < VK_SPEC_COUNT; s++) me[s] = (VkSpecializationMapEntry){(uint32_t)s, (uint32_t)(4 * s), 4};
    VkSpecializationInfo spec = {VK_SPEC_COUNT, me, sizeof k->spec, k->spec};
    VkSpecializationInfo vspec = {VK_FFV_COUNT, me, sizeof k->vspec, k->vspec};
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vmod, "main",
         k->vs == VK_FF_VS_HASH ? &vspec : NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, pmod, "main", &spec}};
    const Fmt *df = k->dsfmt ? fmt_of((D3DFORMAT)k->dsfmt) : NULL;
    const Fmt *cf = fmt_of((D3DFORMAT)k->rtfmt);
    VkFormat cfmt = cf ? cf->vk : VK_FORMAT_B8G8R8A8_UNORM;
    VkPipelineRenderingCreateInfo pr = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &cfmt,
        df ? df->vk : VK_FORMAT_UNDEFINED,
        df && df->stencil && (df->vk == VK_FORMAT_D24_UNORM_S8_UINT || df->vk == VK_FORMAT_D32_SFLOAT_S8_UINT) ? df->vk : VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo gi = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &pr, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms,
        &dss, &cb, &ds, R.layout, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(R.dev, R.pcache, 1, &gi, NULL, &pipe);
    if (r != VK_SUCCESS) {
        LOG(LOG_ALWAYS, "vulkan: pipeline failed (%d): vs %016llx ps %016llx", r, (unsigned long long)k->vs,
            (unsigned long long)k->ps);
        pipe = VK_NULL_HANDLE;
    }
    return pipe;
}

/* the pipeline for k: made (by this thread, unless a worker is making it: then waited for), or as
   it was; worker: only made if nobody has asked for it yet */
static VkPipeline pipeline(const PipeKey *k, const D3DVERTEXELEMENT9 *el, int nel, int worker)
{
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *p = (const uint8_t *)k;
    for (size_t i = 0; i < sizeof *k; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    uint32_t i = (uint32_t)h & (PCAP - 1);
    pthread_mutex_lock(&pipe_lock);
    for (int n = 0; n < PCAP; n++, i = (i + 1) & (PCAP - 1)) {
        if (!pipes[i].state) break;
        if (!memcmp(&pipes[i].k, k, sizeof *k)) {
            while (!worker && pipes[i].state == 1) pthread_cond_wait(&pipe_made, &pipe_lock);
            VkPipeline r = pipes[i].p;
            pthread_mutex_unlock(&pipe_lock);
            return r;
        }
    }
    if (pipes[i].state || npipes >= PCAP * 3 / 4) {
        pthread_mutex_unlock(&pipe_lock);
        return VK_NULL_HANDLE;
    }
    PipeEnt *e = &pipes[i];
    e->k = *k;
    e->state = 1;
    e->nel = nel;
    e->el = malloc(sizeof *el * (size_t)(nel + 1));
    memcpy(e->el, el, sizeof *el * (size_t)nel);
    pipe_order[npipes++] = (int)i;
    pthread_mutex_unlock(&pipe_lock);
    double t0 = real_ms();
    VkPipeline pipe = make_pipeline(k, el, nel);
    double t1 = real_ms(), dt = t1 - t0;
    pthread_mutex_lock(&pipe_lock);
    e->p = pipe;
    e->state = 2;
    if (worker) pipe_ms_workers += dt, pipes_workers++;
    else {
        pipe_ms_game += dt, pipes_game++;
        if (dt > pipe_ms_worst) pipe_ms_worst = dt;
    }
    int n = npipes, ng = pipes_game, nw = pipes_workers;
    double mg = pipe_ms_game, mw = pipe_ms_workers, worst = pipe_ms_worst;
    pthread_cond_broadcast(&pipe_made);
    pthread_mutex_unlock(&pipe_lock);
    if (!worker && (once_("first pipeline") || (n % 32) == 0 || dt >= 4.0 || getenv("ETM_VK_LOG_PIPES")))
        LOG(LOG_ALWAYS, "vulkan renderer: frame %lu, %d pipelines (this one %.2f ms; on the game thread %d, %.1f ms in all, "
            "the longest %.2f; by the workers %d, %.1f ms)", frame_count, n, dt, ng, mg, worst, nw, mw);
    return pipe;
}

/* ---- the list of pipelines made (vk_pipelines.bin in the user folder): made again at the start */
#define PLIST_MAGIC "ETVKPL01"
typedef struct { PipeKey k; uint32_t nel; D3DVERTEXELEMENT9 el[32]; } ListEnt;
static ListEnt *plist;
static int nplist, plist_next, nworkers, workers_done;
static double workers_t0;

static void *pipe_worker(void *arg)
{
    (void)arg;
    for (;;) {
        int i = __atomic_fetch_add(&plist_next, 1, __ATOMIC_RELAXED);
        if (i >= nplist) break;
        pipeline(&plist[i].k, plist[i].el, (int)plist[i].nel, 1);
    }
    if (__atomic_add_fetch(&workers_done, 1, __ATOMIC_ACQ_REL) == nworkers) {
        pthread_mutex_lock(&pipe_lock);
        int nw = pipes_workers;
        double mw = pipe_ms_workers;
        pthread_mutex_unlock(&pipe_lock);
        LOG(LOG_ALWAYS, "vulkan renderer: the workers made %d of the %d listed pipelines in %.1f ms (%.1f ms of their time; "
            "frame %lu)", nw, nplist, real_ms() - workers_t0, mw, frame_count);
    }
    return NULL;
}

/* at the device's creation: the listed pipelines made by worker threads */
static void pipes_preload(void)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/vk_pipelines.bin", user_dir);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char magic[8];
    uint32_t keysize = 0, n = 0;
    if (fread(magic, 8, 1, f) == 1 && !memcmp(magic, PLIST_MAGIC, 8) && fread(&keysize, 4, 1, f) == 1 &&
        keysize == sizeof(PipeKey) && fread(&n, 4, 1, f) == 1 && n <= PCAP) {
        plist = calloc(n ? n : 1, sizeof *plist);
        for (uint32_t i = 0; i < n; i++) {
            ListEnt *e = &plist[nplist];
            if (fread(&e->k, sizeof e->k, 1, f) != 1 || fread(&e->nel, 4, 1, f) != 1 || e->nel > 31 ||
                fread(e->el, sizeof *e->el, e->nel, f) != e->nel)
                break;
            nplist++;
        }
    }
    fclose(f);
    pipes_listed = nplist;
    if (!nplist) return;
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    int nt = cpus >= 8 ? 4 : cpus >= 4 ? 2 : 1;
    nworkers = nt;
    workers_t0 = real_ms();
    for (int t = 0; t < nt; t++) {
        pthread_t th;
        if (pthread_create(&th, NULL, pipe_worker, NULL) == 0) pthread_detach(th);
    }
    LOG(LOG_ALWAYS, "vulkan renderer: %d pipelines listed from the last runs, made by %d worker threads", nplist, nt);
}

/* the list and the driver's cache data written (a thread of their own: the cache data can be
   megabytes) when there are new pipelines, at most every 30 frames (at a Present) */
static int saving;
static unsigned long last_save;

static void *pipes_save_thread(void *arg)
{
    struct { size_t len; uint8_t *buf; } *job = arg;
    char path[1200], tmp[1300];
    snprintf(path, sizeof path, "%s/vk_pipelines.bin", user_dir);
    snprintf(tmp, sizeof tmp, "%s.new", path);
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(job->buf, 1, job->len, f) == job->len;
        if (fclose(f) == 0 && ok) rename(tmp, path);
        else remove(tmp);
    }
    free(job->buf);
    free(job);
    vk_pipeline_cache_save();
    __atomic_store_n(&saving, 0, __ATOMIC_RELEASE);
    return NULL;
}

static void pipes_save_maybe(void)
{
    if (__atomic_load_n(&saving, __ATOMIC_ACQUIRE)) return;
    pthread_mutex_lock(&pipe_lock);
    int n = npipes;
    if (n == pipes_saved || frame_count - last_save < 30) {
        pthread_mutex_unlock(&pipe_lock);
        return;
    }
    size_t len = 16;
    for (int j = 0; j < n; j++) len += sizeof(PipeKey) + 4 + sizeof(D3DVERTEXELEMENT9) * (size_t)pipes[pipe_order[j]].nel;
    uint8_t *buf = malloc(len), *w = buf;
    uint32_t keysize = sizeof(PipeKey), cnt = 0;
    memcpy(w, PLIST_MAGIC, 8), w += 8;
    memcpy(w, &keysize, 4), w += 4;
    uint8_t *cntp = w;
    w += 4;
    for (int j = 0; j < n; j++) {
        PipeEnt *e = &pipes[pipe_order[j]];
        if (e->state != 2 || !e->p) continue;           /* (failed or not yet made: left out) */
        uint32_t nel = (uint32_t)e->nel;
        memcpy(w, &e->k, sizeof e->k), w += sizeof e->k;
        memcpy(w, &nel, 4), w += 4;
        memcpy(w, e->el, sizeof *e->el * nel), w += sizeof *e->el * nel;
        cnt++;
    }
    memcpy(cntp, &cnt, 4);
    pipes_saved = n;
    last_save = frame_count;
    pthread_mutex_unlock(&pipe_lock);
    struct { size_t len; uint8_t *buf; } *job = malloc(sizeof *job);
    job->len = (size_t)(w - buf);
    job->buf = buf;
    __atomic_store_n(&saving, 1, __ATOMIC_RELEASE);
    pthread_t th;
    if (pthread_create(&th, NULL, pipes_save_thread, job) == 0) pthread_detach(th);
    else { free(buf); free(job); __atomic_store_n(&saving, 0, __ATOMIC_RELEASE); }
}

/* ------------------------------------------------------------------ draws */
static int topo_class(D3DPRIMITIVETYPE p) { return p == D3DPT_POINTLIST ? 0 : p <= D3DPT_LINESTRIP ? 1 : 2; }

static VkPrimitiveTopology topo_vk(D3DPRIMITIVETYPE p)
{
    switch (p) {
    case D3DPT_POINTLIST: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
    case D3DPT_LINELIST: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    case D3DPT_LINESTRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
    case D3DPT_TRIANGLESTRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
    case D3DPT_TRIANGLEFAN: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
    default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}

static uint32_t prim_verts(D3DPRIMITIVETYPE p, UINT n)
{
    switch (p) {
    case D3DPT_POINTLIST: return n;
    case D3DPT_LINELIST: return n * 2;
    case D3DPT_LINESTRIP: return n + 1;
    case D3DPT_TRIANGLELIST: return n * 3;
    default: return n + 2;
    }
}

static VkCompareOp cmp_op(DWORD c)
{
    return c >= D3DCMP_NEVER && c <= D3DCMP_ALWAYS ? (VkCompareOp)(c - 1) : VK_COMPARE_OP_ALWAYS;
}

static VkStencilOp stencil_op(DWORD o)
{
    switch (o) {
    case D3DSTENCILOP_ZERO: return VK_STENCIL_OP_ZERO;
    case D3DSTENCILOP_REPLACE: return VK_STENCIL_OP_REPLACE;
    case D3DSTENCILOP_INCRSAT: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    case D3DSTENCILOP_DECRSAT: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
    case D3DSTENCILOP_INVERT: return VK_STENCIL_OP_INVERT;
    case D3DSTENCILOP_INCR: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
    case D3DSTENCILOP_DECR: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
    default: return VK_STENCIL_OP_KEEP;
    }
}

static float f_of(DWORD d) { float f; memcpy(&f, &d, 4); return f; }

/* the dynamic state groups that changed (DYN_*), each set only when its values differ from what
   this command buffer last set */
static void set_dynamic(int groups)
{
    VkCommandBuffer cb = B.cb;
    int all = !B.dyn_valid;
    if (all) groups = DYN_ALL;
    int has_ds = B.pds != NULL;
    if (groups & DYN_VP) {
        const D3DVIEWPORT9 *vp = &S.vp;
        float zb = vp->MinZ >= 0.5f ? 0.0f : 0.001f;
        float minz = vp->MinZ < 0 ? 0 : vp->MinZ > 1 ? 1 : vp->MinZ;
        float maxz = vp->MaxZ > vp->MinZ + zb ? vp->MaxZ : vp->MinZ + zb;
        if (maxz > 1) maxz = 1;
        VkViewport v = {(float)vp->X + 0.5f, (float)(vp->Height + vp->Y) + 0.5f, (float)vp->Width, -(float)vp->Height, minz, maxz};
        if (all || memcmp(&v, &B.vp, sizeof v)) {
            vkCmdSetViewport(cb, 0, 1, &v);
            B.vp = v;
        }
        int32_t x0 = (int32_t)vp->X, y0 = (int32_t)vp->Y, x1 = x0 + (int32_t)vp->Width, y1 = y0 + (int32_t)vp->Height;
        if (S.rs[D3DRS_SCISSORTESTENABLE]) {
            if (S.scissor.left > x0) x0 = S.scissor.left;
            if (S.scissor.top > y0) y0 = S.scissor.top;
            if (S.scissor.right < x1) x1 = S.scissor.right;
            if (S.scissor.bottom < y1) y1 = S.scissor.bottom;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)B.pw) x1 = (int32_t)B.pw;
        if (y1 > (int32_t)B.ph) y1 = (int32_t)B.ph;
        VkRect2D sc = {{x0, y0}, {(uint32_t)(x1 > x0 ? x1 - x0 : 0), (uint32_t)(y1 > y0 ? y1 - y0 : 0)}};
        if (all || memcmp(&sc, &B.sc, sizeof sc)) {
            vkCmdSetScissor(cb, 0, 1, &sc);
            B.sc = sc;
        }
    }
    if (groups & DYN_CULL) {
        DWORD c = S.rs[D3DRS_CULLMODE];
        VkCullModeFlags cull = c == D3DCULL_CW ? VK_CULL_MODE_FRONT_BIT : c == D3DCULL_CCW ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
        if (all || cull != B.cull) {
            vkCmdSetCullMode(cb, cull);
            B.cull = cull;
        }
        if (all) vkCmdSetFrontFace(cb, VK_FRONT_FACE_CLOCKWISE);
    }
    if (groups & DYN_DEPTH) {
        uint32_t test = has_ds && S.rs[D3DRS_ZENABLE] != D3DZB_FALSE;
        uint32_t write = test && S.rs[D3DRS_ZWRITEENABLE];
        uint32_t op = (uint32_t)cmp_op(S.rs[D3DRS_ZFUNC]);
        float bias = f_of(S.rs[D3DRS_DEPTHBIAS]), slope = f_of(S.rs[D3DRS_SLOPESCALEDEPTHBIAS]);
        float scale = has_ds && B.pds->tex->fmt == D3DFMT_D16 ? 65536.0f : 16777216.0f;
        uint32_t bias_on = bias != 0 || slope != 0;
        bias *= scale;
        if (all || test != B.depth.test) vkCmdSetDepthTestEnable(cb, test);
        if (all || write != B.depth.write) vkCmdSetDepthWriteEnable(cb, write);
        if (all || op != B.depth.op) vkCmdSetDepthCompareOp(cb, (VkCompareOp)op);
        if (all || bias_on != B.depth.bias_on) vkCmdSetDepthBiasEnable(cb, bias_on);
        if (all || bias != B.depth.bias || slope != B.depth.slope) vkCmdSetDepthBias(cb, bias, 0, slope);
        B.depth.test = test, B.depth.write = write, B.depth.op = op, B.depth.bias_on = bias_on;
        B.depth.bias = bias, B.depth.slope = slope;
    }
    if (groups & DYN_STENCIL) {
        uint32_t on = has_ds && B.pds->tex->stencil && S.rs[D3DRS_STENCILENABLE];
        if (all || on != B.stencil.on) vkCmdSetStencilTestEnable(cb, on);
        B.stencil.on = on;
        int two = S.rs[D3DRS_TWOSIDEDSTENCILMODE] != 0;
        uint32_t ops[2][4] = {
            {stencil_op(S.rs[D3DRS_STENCILFAIL]), stencil_op(S.rs[D3DRS_STENCILPASS]), stencil_op(S.rs[D3DRS_STENCILZFAIL]),
             cmp_op(S.rs[D3DRS_STENCILFUNC])},
            {stencil_op(S.rs[two ? D3DRS_CCW_STENCILFAIL : D3DRS_STENCILFAIL]), stencil_op(S.rs[two ? D3DRS_CCW_STENCILPASS : D3DRS_STENCILPASS]),
             stencil_op(S.rs[two ? D3DRS_CCW_STENCILZFAIL : D3DRS_STENCILZFAIL]), cmp_op(S.rs[two ? D3DRS_CCW_STENCILFUNC : D3DRS_STENCILFUNC])}};
        for (int f = 0; f < 2; f++)
            if (all || memcmp(ops[f], B.stencil.ops[f], sizeof ops[f])) {
                vkCmdSetStencilOp(cb, f ? VK_STENCIL_FACE_BACK_BIT : VK_STENCIL_FACE_FRONT_BIT, (VkStencilOp)ops[f][0],
                                  (VkStencilOp)ops[f][1], (VkStencilOp)ops[f][2], (VkCompareOp)ops[f][3]);
                memcpy(B.stencil.ops[f], ops[f], sizeof ops[f]);
            }
        VkStencilFaceFlags both = VK_STENCIL_FACE_FRONT_AND_BACK;
        uint32_t cm = S.rs[D3DRS_STENCILMASK] & 0xff, wm = S.rs[D3DRS_STENCILWRITEMASK] & 0xff, ref = S.rs[D3DRS_STENCILREF] & 0xff;
        if (all || cm != B.stencil.cmask) vkCmdSetStencilCompareMask(cb, both, cm);
        if (all || wm != B.stencil.wmask) vkCmdSetStencilWriteMask(cb, both, wm);
        if (all || ref != B.stencil.ref) vkCmdSetStencilReference(cb, both, ref);
        B.stencil.cmask = cm, B.stencil.wmask = wm, B.stencil.ref = ref;
    }
    if (groups & DYN_BLEND) {
        DWORD bf = S.rs[D3DRS_BLENDFACTOR];
        if (all || bf != B.blendf) {
            float bc[4] = {((bf >> 16) & 255) / 255.0f, ((bf >> 8) & 255) / 255.0f, (bf & 255) / 255.0f, (bf >> 24) / 255.0f};
            vkCmdSetBlendConstants(cb, bc);
            B.blendf = bf;
        }
    }
    B.dyn_valid = 1;
}

/* the sampler for slot s with what is bound there (as DXVK: cubes clamp; one-level textures no
   anisotropy, LOD 0-1; the option anisotropic on linear filtering of textures with mipmaps) */
static int aniso_opt = -2;
/* (larush-native's shadow_filter: Enter the Matrix's DXVK runs d3d9.shadowFilter = 0, d3d9.c; the
   specialization constant stays 0) */
static const int shadow_filter_opt = 0;

/* the option anisotropic (16 by default, as d3d9.c gives DXVK: d3d9.samplerAnisotropy), read at the
   start; the Video page changes it for the next start (menu.c) */
static void options_read(void)
{
    int n = atoi(option_str("anisotropic", "16"));
    int an = n == 2 || n == 4 || n == 8 || n == 16 ? n : -1;
    if (an != aniso_opt) dirty_samp = ~0u;
    aniso_opt = an;
}

static int aniso_option(void)
{
    if (aniso_opt == -2) options_read();
    return aniso_opt;
}

static int sampler_for(int s, VkTex *t)
{
    const DWORD *ss = S.ss[s];
    VkSamplerKey k;
    memset(&k, 0, sizeof k);
    int cube = t && t->faces == 6;
    k.addr[0] = cube ? D3DTADDRESS_CLAMP : (uint8_t)ss[D3DSAMP_ADDRESSU];
    k.addr[1] = cube ? D3DTADDRESS_CLAMP : (uint8_t)ss[D3DSAMP_ADDRESSV];
    k.addr[2] = cube ? D3DTADDRESS_CLAMP : (uint8_t)ss[D3DSAMP_ADDRESSW];
    k.mag = (uint8_t)ss[D3DSAMP_MAGFILTER];
    k.min = (uint8_t)ss[D3DSAMP_MINFILTER];
    k.mip = (uint8_t)ss[D3DSAMP_MIPFILTER];
    k.compare = t && t->kind == 3;
    if (k.addr[0] == D3DTADDRESS_BORDER || k.addr[1] == D3DTADDRESS_BORDER || k.addr[2] == D3DTADDRESS_BORDER)
        k.border = ss[D3DSAMP_BORDERCOLOR];
    if (!t || t->levels <= 1) {
        /* (one level: LOD 0..1 so the minification filter still applies, no anisotropy: as DXVK) */
        k.mip = D3DTEXF_POINT;
        k.minlod = 0;
        k.maxlod = 1;
    } else if (k.mip) {
        int an = k.min == D3DTEXF_ANISOTROPIC ? (int)ss[D3DSAMP_MAXANISOTROPY] : 0;
        if (aniso_option() != -1 && k.min > D3DTEXF_POINT) an = aniso_option();
        k.aniso = (uint8_t)an;
        k.bias = f_of(ss[D3DSAMP_MIPMAPLODBIAS]);
        k.minlod = (float)ss[D3DSAMP_MAXMIPLEVEL];
        k.maxlod = 16;
    } else {
        k.mip = D3DTEXF_POINT;
        k.minlod = (float)ss[D3DSAMP_MAXMIPLEVEL];
        k.maxlod = k.minlod + 0.25f;
    }
    return vk_sampler(&k);
}

/* the constants into the frame's ring: the vertex shader's (its def'd ones over the program's) and
   the pixel shader's extra block then its constants (VULKAN.md "Shaders") */
static uint32_t ubo_align;

/* a new ring chunk under the constants: what was uploaded is in another buffer (the offsets stay
   valid placeholders in the new one until replaced) */
static unsigned c_rechunks;

static void consts_rechunk(const VkRing *r)
{
    c_rechunks++;
    B.cset = r->cset;
    B.vvalid = 0;
    B.vcontig = 0;
    B.pvalid = 0;
    B.ffv_valid = 0;
    for (int k = 0; k <= VK_VS_CHUNKS; k++) B.coff[k] = (uint32_t)r->off;
}

/* ------------------------------------------------------------------ fixed function
   FIXED FUNCTION (VULKAN.md 2.3). Enter the Matrix draws everything through Direct3D's fixed-function
   pipeline (no shader on the test route: VULKAN.md 1), which larush-native never needed. A draw with no
   vertex shader (or pre-transformed vertices) takes the vertex uber-shader runtime/shaders/ff.vert, one
   with no pixel shader the pixel uber-shader ff.frag, both SPIR-V from the build; what they do is
   chosen by specialization constants from the state key built here (ff_key_vs, ff_key_ps: the
   pipeline's key holds them), the values per draw go into the frame's ring (ff_data_vs: matrices,
   lights, material, viewport; ff_data_ps: texture factor, stage constants, bump matrices). Meaning and
   arithmetic follow DXVK 3.1.1 (d3d9_device.cpp UpdateFixedFunctionVS / PS, UpdateFog, UpdatePointMode,
   d3d9_state.h D3D9Light and D3D9SpecData, util_matrix.cpp): the CPU-side matrices are multiplied and
   inverted in DXVK's order of operations, so the GPU gets the same bits. */

/* DXVK's Classify (d3d9_vertex_declaration.cpp): what the fixed-function vertex shader reads */
static void decl_classify(VkDecl *d)
{
    static const uint8_t comps[] = {1, 2, 3, 4, 4, 4, 2, 4, 4, 2, 4, 2, 4, 3, 3, 2, 4, 0};
    d->ffflags = d->tcmask = 0;
    for (int e = 0; e < d->nel; e++) {
        const D3DVERTEXELEMENT9 *el = &d->el[e];
        if (el->Usage == D3DDECLUSAGE_COLOR && el->UsageIndex == 0) d->ffflags |= DECL_COLOR0;
        else if (el->Usage == D3DDECLUSAGE_COLOR && el->UsageIndex == 1) d->ffflags |= DECL_COLOR1;
        else if (el->Usage == D3DDECLUSAGE_POSITIONT) d->ffflags |= DECL_POSITIONT;
        else if (el->Usage == D3DDECLUSAGE_PSIZE) d->ffflags |= DECL_PSIZE;
        else if (el->Usage == D3DDECLUSAGE_FOG) d->ffflags |= DECL_FOG;
        else if (el->Usage == D3DDECLUSAGE_BLENDWEIGHT) d->ffflags |= DECL_BLENDWEIGHT;
        else if (el->Usage == D3DDECLUSAGE_BLENDINDICES) d->ffflags |= DECL_BLENDINDICES;
        if (el->Usage == D3DDECLUSAGE_TEXCOORD && el->UsageIndex < 8 && el->Type < sizeof comps)
            d->tcmask |= (uint32_t)comps[el->Type] << (el->UsageIndex * 3);
    }
}

static HRESULT dev_CreateVertexDeclaration(void *This, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **out);

/* SetFVF's declaration (DXVK's D3D9VertexDecl::SetFVF), made once per FVF and kept by the device */
static VkDecl *fvf_decl(DWORD fvf)
{
    static struct { DWORD fvf; VkDecl *d; } cache[64];
    static int ncache;
    for (int i = 0; i < ncache; i++)
        if (cache[i].fvf == fvf) return cache[i].d;
    static const uint8_t tsize[] = {4, 4, 12, 16, 4, 4, 4, 8, 4, 4, 8, 4, 8, 4, 4, 4, 8, 0};
    D3DVERTEXELEMENT9 el[17];
    int n = 0;
    uint32_t betas = 0;
    int beta_idx = -1;
#define EL(type, usage, index) (el[n++] = (D3DVERTEXELEMENT9){0, 0, (BYTE)(type), D3DDECLMETHOD_DEFAULT, (BYTE)(usage), (BYTE)(index)})
    switch (fvf & D3DFVF_POSITION_MASK) {
    case D3DFVF_XYZ: case D3DFVF_XYZB1: case D3DFVF_XYZB2: case D3DFVF_XYZB3: case D3DFVF_XYZB4: case D3DFVF_XYZB5:
        EL(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_POSITION, 0);
        if ((fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZ) break;
        betas = (((fvf & D3DFVF_XYZB5) - D3DFVF_XYZB1) >> 1) + 1;
        if (fvf & D3DFVF_LASTBETA_D3DCOLOR) beta_idx = D3DDECLTYPE_D3DCOLOR;
        else if (fvf & D3DFVF_LASTBETA_UBYTE4) beta_idx = D3DDECLTYPE_UBYTE4;
        else if ((fvf & D3DFVF_XYZB5) == D3DFVF_XYZB5) beta_idx = D3DDECLTYPE_FLOAT1;
        if (beta_idx >= 0) betas--;
        if (betas > 0 && betas <= 4) EL(D3DDECLTYPE_FLOAT1 + betas - 1, D3DDECLUSAGE_BLENDWEIGHT, 0);
        if (beta_idx >= 0) EL(beta_idx, D3DDECLUSAGE_BLENDINDICES, 0);
        break;
    case D3DFVF_XYZW: case D3DFVF_XYZRHW:
        EL(D3DDECLTYPE_FLOAT4, (fvf & D3DFVF_POSITION_MASK) == D3DFVF_XYZW ? D3DDECLUSAGE_POSITION : D3DDECLUSAGE_POSITIONT, 0);
        break;
    }
    if (fvf & D3DFVF_NORMAL) EL(D3DDECLTYPE_FLOAT3, D3DDECLUSAGE_NORMAL, 0);
    if (fvf & D3DFVF_PSIZE) EL(D3DDECLTYPE_FLOAT1, D3DDECLUSAGE_PSIZE, 0);
    if (fvf & D3DFVF_DIFFUSE) EL(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 0);
    if (fvf & D3DFVF_SPECULAR) EL(D3DDECLTYPE_D3DCOLOR, D3DDECLUSAGE_COLOR, 1);
    uint32_t ntex = (fvf & D3DFVF_TEXCOUNT_MASK) >> D3DFVF_TEXCOUNT_SHIFT;
    if (ntex > 8) ntex = 8;
    for (uint32_t i = 0; i < ntex; i++) {
        static const uint8_t ft[4] = {D3DDECLTYPE_FLOAT2, D3DDECLTYPE_FLOAT3, D3DDECLTYPE_FLOAT4, D3DDECLTYPE_FLOAT1};
        EL(ft[(fvf >> (16 + i * 2)) & 3], D3DDECLUSAGE_TEXCOORD, i);
    }
#undef EL
    for (int i = 1; i < n; i++) el[i].Offset = (WORD)(el[i - 1].Offset + tsize[el[i - 1].Type]);
    el[n] = (D3DVERTEXELEMENT9)D3DDECL_END();
    IDirect3DVertexDeclaration9 *o = NULL;
    dev_CreateVertexDeclaration(NULL, el, &o);
    VkDecl *d = (VkDecl *)o;
    d->fvf = fvf;
    if (ncache < 64) cache[ncache++] = (typeof(cache[0])){fvf, d};
    else if (once_("FVF cache")) LOG(LOG_ALWAYS, "vulkan renderer: more than 64 FVFs (declarations made again each time)");
    return d;
}

/* DXVK's matrices are column-major with Direct3D's rows as their columns: its Matrix4 a * b, with a and
   b holding Direct3D's rows, is Direct3D's b x a, each element summed left to right */
static void ff_mul(float r[4][4], const float b[4][4], const float a[4][4])  /* r = b x a (Direct3D), as DXVK's a * b */
{
    float t[4][4];
    for (int j = 0; j < 4; j++)
        for (int c = 0; c < 4; c++)
            t[j][c] = a[0][c] * b[j][0] + a[1][c] * b[j][1] + a[2][c] * b[j][2] + a[3][c] * b[j][3];
    memcpy(r, t, sizeof t);
}

/* DXVK's tryInverse (util_matrix.cpp) on its data, Direct3D's rows; identity when it has none */
static void ff_inverse(float out[4][4], const float m[4][4])
{
    float coef00 = m[2][2] * m[3][3] - m[3][2] * m[2][3];
    float coef02 = m[1][2] * m[3][3] - m[3][2] * m[1][3];
    float coef03 = m[1][2] * m[2][3] - m[2][2] * m[1][3];
    float coef04 = m[2][1] * m[3][3] - m[3][1] * m[2][3];
    float coef06 = m[1][1] * m[3][3] - m[3][1] * m[1][3];
    float coef07 = m[1][1] * m[2][3] - m[2][1] * m[1][3];
    float coef08 = m[2][1] * m[3][2] - m[3][1] * m[2][2];
    float coef10 = m[1][1] * m[3][2] - m[3][1] * m[1][2];
    float coef11 = m[1][1] * m[2][2] - m[2][1] * m[1][2];
    float coef12 = m[2][0] * m[3][3] - m[3][0] * m[2][3];
    float coef14 = m[1][0] * m[3][3] - m[3][0] * m[1][3];
    float coef15 = m[1][0] * m[2][3] - m[2][0] * m[1][3];
    float coef16 = m[2][0] * m[3][2] - m[3][0] * m[2][2];
    float coef18 = m[1][0] * m[3][2] - m[3][0] * m[1][2];
    float coef19 = m[1][0] * m[2][2] - m[2][0] * m[1][2];
    float coef20 = m[2][0] * m[3][1] - m[3][0] * m[2][1];
    float coef22 = m[1][0] * m[3][1] - m[3][0] * m[1][1];
    float coef23 = m[1][0] * m[2][1] - m[2][0] * m[1][1];
    const float fac0[4] = {coef00, coef00, coef02, coef03}, fac1[4] = {coef04, coef04, coef06, coef07};
    const float fac2[4] = {coef08, coef08, coef10, coef11}, fac3[4] = {coef12, coef12, coef14, coef15};
    const float fac4[4] = {coef16, coef16, coef18, coef19}, fac5[4] = {coef20, coef20, coef22, coef23};
    const float vec0[4] = {m[1][0], m[0][0], m[0][0], m[0][0]}, vec1[4] = {m[1][1], m[0][1], m[0][1], m[0][1]};
    const float vec2[4] = {m[1][2], m[0][2], m[0][2], m[0][2]}, vec3[4] = {m[1][3], m[0][3], m[0][3], m[0][3]};
    float inv[4][4];
    for (int i = 0; i < 4; i++) {
        float sa = (i & 1) ? -1.0f : 1.0f;
        inv[0][i] = (vec1[i] * fac0[i] - vec2[i] * fac1[i] + vec3[i] * fac2[i]) * sa;
        inv[1][i] = (vec0[i] * fac0[i] - vec2[i] * fac3[i] + vec3[i] * fac4[i]) * -sa;
        inv[2][i] = (vec0[i] * fac1[i] - vec1[i] * fac3[i] + vec3[i] * fac5[i]) * sa;
        inv[3][i] = (vec0[i] * fac2[i] - vec1[i] * fac4[i] + vec2[i] * fac5[i]) * -sa;
    }
    const float row0[4] = {inv[0][0], inv[1][0], inv[2][0], inv[3][0]};
    float d0 = m[0][0] * row0[0], d1 = m[0][1] * row0[1], d2 = m[0][2] * row0[2], d3 = m[0][3] * row0[3];
    float dot1 = (d0 + d1) + (d2 + d3);
    if (fabsf(dot1) <= 1.17549435e-38f * 10) {
        memset(out, 0, 64);
        out[0][0] = out[1][1] = out[2][2] = out[3][3] = 1.0f;
        return;
    }
    float s = 1.0f / dot1;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = inv[i][j] * s;
}

/* DXVK's Matrix4 * Vector4 with Direct3D's rows: v x M, summed (x + y) + (z + w) */
static void ff_xform(float r[4], const float m[4][4], float x, float y, float z, float w)
{
    for (int c = 0; c < 4; c++) r[c] = (m[0][c] * x + m[1][c] * y) + (m[2][c] * z + m[3][c] * w);
}

/* the vertex block (ff.vert FFVsData: std140, everything vec4 sized) */
typedef struct {
    float wv[16], nrm[16], proj[16], wvp[16], tex[8][16];
    float vp_off[4], vp_ext[4];
    struct { float diffuse[4], specular[4], ambient[4], position[4], direction[4], rfa[4], atp[4]; } light[8];
    float mdiff[4], mamb[4], mspec[4], memis[4];
    uint32_t gamb[4];
    float ptf[4], fdp[4], pscale[4];
} FFVsData;

/* DXVK's EncodePointSize and the shader's decoding: 13.3 fixed point */
static float ff_point(DWORD v) { float f = f_of(v) * 8.0f; f = f < 0 ? 0 : f > 65535.0f ? 65535.0f : f; return (float)(uint16_t)f / 8.0f; }

static int ff_ztest(void) { return S.rs[D3DRS_ZENABLE] != D3DZB_FALSE && dsurf != NULL; }

static void ff_data_vs(FFVsData *d)
{
    memset(d, 0, sizeof *d);
    const float (*view)[4] = (const float (*)[4])xforms[D3DTS_VIEW].m;
    const float (*world)[4] = (const float (*)[4])xforms[D3DTS_WORLD].m;
    float wv[4][4], nrm[4][4], wvp[4][4];
    ff_mul(wv, world, view);                /* World x View */
    ff_inverse(nrm, wv);
    ff_mul(wvp, wv, (const float (*)[4])xforms[D3DTS_PROJECTION].m);
    memcpy(d->wv, wv, 64);
    memcpy(d->nrm, nrm, 64);
    memcpy(d->proj, xforms[D3DTS_PROJECTION].m, 64);
    memcpy(d->wvp, wvp, 64);
    for (int i = 0; i < 8; i++) memcpy(d->tex[i], xforms[D3DTS_TEXTURE0 + i].m, 64);
    /* pre-transformed vertices: window to clip coordinates for the viewport (DXVK's m_viewportInfo) */
    const D3DVIEWPORT9 *vp = &S.vp;
    d->vp_ext[0] = 2.0f / (float)vp->Width;
    d->vp_ext[1] = -2.0f / (float)vp->Height;
    d->vp_ext[2] = ff_ztest() ? 1.0f : 0.0f;
    d->vp_ext[3] = 1.0f;
    d->vp_off[0] = -(float)vp->X * d->vp_ext[0] + -1.0f;
    d->vp_off[1] = -(float)vp->Y * d->vp_ext[1] + 1.0f;
    d->vp_off[2] = 0.0f * d->vp_ext[2] + 0.0f;
    d->vp_off[3] = 0.0f * d->vp_ext[3] + 0.0f;
    /* the enabled lights in index order, at most 8, in view space (DXVK's D3D9Light) */
    int n = 0;
    for (int i = 0; i < VK_LIGHTS && n < 8; i++) {
        const D3DLIGHT9 *l = &S.light[i];
        if (!S.light_on[i] || !l->Type || l->Type > D3DLIGHT_DIRECTIONAL) continue;
        typeof(d->light[0]) *o = &d->light[n++];
        memcpy(o->diffuse, &l->Diffuse, 16);
        memcpy(o->specular, &l->Specular, 16);
        memcpy(o->ambient, &l->Ambient, 16);
        ff_xform(o->position, view, l->Position.x, l->Position.y, l->Position.z, 1.0f);
        float dir[4];
        ff_xform(dir, view, l->Direction.x, l->Direction.y, l->Direction.z, 0.0f);
        float len = sqrtf(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2] + dir[3] * dir[3]), inv = 1.0f / len;
        for (int c = 0; c < 4; c++) o->direction[c] = dir[c] * inv;
        o->rfa[0] = l->Range, o->rfa[1] = l->Falloff, o->rfa[2] = l->Attenuation0, o->rfa[3] = l->Attenuation1;
        o->atp[0] = l->Attenuation2, o->atp[1] = cosf(l->Theta / 2.0f), o->atp[2] = cosf(l->Phi / 2.0f);
    }
    memcpy(d->mdiff, &S.mat.Diffuse, 16);
    memcpy(d->mamb, &S.mat.Ambient, 16);
    memcpy(d->mspec, &S.mat.Specular, 16);
    memcpy(d->memis, &S.mat.Emissive, 16);
    d->gamb[0] = S.rs[D3DRS_AMBIENT];
    float fs = f_of(S.rs[D3DRS_FOGSTART]), fe = f_of(S.rs[D3DRS_FOGEND]);
    d->ptf[0] = S.mat.Power;
    d->ptf[1] = f_of(S.rs[D3DRS_TWEENFACTOR]);
    d->ptf[2] = fe;
    d->ptf[3] = fe != fs ? 1.0f / (fe - fs) : 3.402823466e38f;
    d->fdp[0] = f_of(S.rs[D3DRS_FOGDENSITY]);
    d->fdp[1] = ff_point(S.rs[D3DRS_POINTSIZE]);
    d->fdp[2] = ff_point(S.rs[D3DRS_POINTSIZE_MIN]);
    d->fdp[3] = ff_point(S.rs[D3DRS_POINTSIZE_MAX]);
    float ps = 1.0f / (float)(vp->Height * vp->Height);
    d->pscale[0] = ps * f_of(S.rs[D3DRS_POINTSCALE_A]);
    d->pscale[1] = ps * f_of(S.rs[D3DRS_POINTSCALE_B]);
    d->pscale[2] = ps * f_of(S.rs[D3DRS_POINTSCALE_C]);
}

/* the projected stages among those with a texture (DXVK: projected & used textures) */
static uint32_t ff_proj(uint32_t used)
{
    uint32_t m = 0;
    for (int s = 0; s < 8; s++)
        if ((used >> s & 1) && S.tex[s] && (S.tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED)) m |= 1u << s;
    return m;
}

/* DXVK's GetTextureStageArgMask: the arguments an operation reads (bit 0 arg 0, 1 arg 1, 2 arg 2) */
static uint32_t ff_argmask(DWORD op)
{
    switch (op) {
    case D3DTOP_DISABLE: case D3DTOP_BUMPENVMAP: case D3DTOP_BUMPENVMAPLUMINANCE: return 0;
    case D3DTOP_SELECTARG1: case D3DTOP_PREMODULATE: return 2;
    case D3DTOP_SELECTARG2: return 4;
    case D3DTOP_MULTIPLYADD: case D3DTOP_LERP: return 7;
    default: return 6;
    }
}

/* DXVK's getTextureArg: the selector and its two modifiers in 5 bits */
static uint32_t ff_arg(DWORD a) { return ((a & D3DTA_SELECTMASK) | ((a & ~(DWORD)D3DTA_SELECTMASK) >> 1)) & 0x1f; }

/* the vertex shader's key (DXVK's UpdateFixedFunctionVS, UpdateFog, UpdatePointMode) */
static void ff_key_vs(PipeKey *k, const VkDecl *decl, const VkShader *ps, int topo, uint32_t proj)
{
    const DWORD *rs = S.rs;
    uint32_t df = decl->ffflags, f = 0;
    int post = (df & DECL_POSITIONT) != 0;
    f |= post ? 1u : 0;
    f |= (df & DECL_COLOR0) ? 2u : 0;
    f |= (df & DECL_COLOR1) ? 4u : 0;
    f |= (df & DECL_PSIZE) ? 8u : 0;
    f |= (df & DECL_FOG) ? 16u : 0;
    int lighting = rs[D3DRS_LIGHTING] && !post;
    uint32_t mask = lighting && rs[D3DRS_COLORVERTEX] ? ((df & DECL_COLOR0) ? D3DMCS_COLOR1 : 0) | ((df & DECL_COLOR1) ? D3DMCS_COLOR2 : 0) : 0;
    f |= lighting ? 0x20u : 0;
    f |= rs[D3DRS_NORMALIZENORMALS] ? 0x40u : 0;
    f |= rs[D3DRS_LOCALVIEWER] && lighting ? 0x80u : 0;
    f |= rs[D3DRS_RANGEFOGENABLE] ? 0x100u : 0;
    f |= rs[D3DRS_SPECULARENABLE] ? 0x200u : 0;
    f |= topo == 0 && rs[D3DRS_POINTSCALEENABLE] ? 0x400u : 0;
    if (rs[D3DRS_VERTEXBLEND] != D3DVBF_DISABLE && !post) {
        if (rs[D3DRS_VERTEXBLEND] == D3DVBF_TWEENING) { if (df & DECL_BLENDWEIGHT) f |= 0x800u; }
        else if (once_("vertex blending"))
            LOG(LOG_ALWAYS, "vulkan renderer: fixed-function vertex blending (%u) is not done", (unsigned)rs[D3DRS_VERTEXBLEND]);
    }
    int fog = rs[D3DRS_FOGENABLE] != 0;
    f |= fog ? 0x1000u : 0;
    f |= ps && ps->meta && ps->meta->major >= 3 ? 0x2000u : 0;
    f |= topo == 0 ? 0x4000u : 0;
    f |= (rs[D3DRS_DIFFUSEMATERIALSOURCE] & mask & 3) << 16;
    f |= (rs[D3DRS_AMBIENTMATERIALSOURCE] & mask & 3) << 18;
    f |= (rs[D3DRS_SPECULARMATERIALSOURCE] & mask & 3) << 20;
    f |= (rs[D3DRS_EMISSIVEMATERIALSOURCE] & mask & 3) << 22;
    k->vspec[VK_FFV_FLAGS] = f;
    uint32_t lights = 0;
    if (lighting)
        for (int i = 0, n = 0; i < VK_LIGHTS && n < 8; i++) {
            if (!S.light_on[i] || !S.light[i].Type || S.light[i].Type > D3DLIGHT_DIRECTIONAL) continue;
            lights |= (uint32_t)S.light[i].Type << (2 * n++);
        }
    k->vspec[VK_FFV_LIGHTS] = lights;
    uint32_t tci = 0, tcif = 0, ttff = 0;
    for (int i = 0; i < 8; i++) {
        DWORD idx = S.tss[i][D3DTSS_TEXCOORDINDEX];
        tci |= (idx & 7) << (3 * i);
        tcif |= ((idx >> 16) & 7) << (3 * i);
        ttff |= (S.tss[i][D3DTSS_TEXTURETRANSFORMFLAGS] & ~(DWORD)D3DTTFF_PROJECTED & 7) << (3 * i);
    }
    k->vspec[VK_FFV_TCI] = tci;
    k->vspec[VK_FFV_TCIF] = tcif;
    k->vspec[VK_FFV_TTFF] = ttff;
    k->vspec[VK_FFV_TCMASK] = decl->tcmask;
    uint32_t vmode = fog && !rs[D3DRS_FOGTABLEMODE] ? rs[D3DRS_FOGVERTEXMODE] & 3 : 0;
    k->vspec[VK_FFV_MISC] = proj | vmode << 8;
}

/* the pixel shader's key (DXVK's UpdateFixedFunctionPS): the stages in use and their operations and
   arguments; returns the stages that sample (the samplers' mask) */
static uint32_t ff_key_ps(PipeKey *k)
{
    int active = 0;
    for (int i = 0; i < 8; i++) {
        DWORD op = S.tss[i][D3DTSS_COLOROP];
        if (op == D3DTOP_DISABLE) break;
        /* (a stage without a texture whose colour reads TEXTURE ends the stages: alpha and implicit
           sampling do not count) */
        if (!S.tex[i]) {
            uint32_t m = ff_argmask(op);
            DWORD a0 = ((m & 1) ? S.tss[i][D3DTSS_COLORARG0] : D3DTA_CURRENT) & D3DTA_SELECTMASK;
            DWORD a1 = ((m & 2) ? S.tss[i][D3DTSS_COLORARG1] : D3DTA_CURRENT) & D3DTA_SELECTMASK;
            DWORD a2 = ((m & 4) ? S.tss[i][D3DTSS_COLORARG2] : D3DTA_CURRENT) & D3DTA_SELECTMASK;
            if (a0 == D3DTA_TEXTURE || a1 == D3DTA_TEXTURE || a2 == D3DTA_TEXTURE) break;
        }
        active++;
    }
    uint32_t ops[8], args[8];
    for (int i = 0; i < 8; i++) {
        if (i >= active) {
            ops[i] = D3DTOP_DISABLE | (D3DTOP_DISABLE << 5);    /* (DXVK's disableTextureStage, as written) */
            args[i] = 0;
            continue;
        }
        const DWORD *t = S.tss[i];
        DWORD result = t[D3DTSS_RESULTARG];
        int temp = result == D3DTA_TEMP;
        DWORD cop = t[D3DTSS_COLOROP], aop = t[D3DTSS_ALPHAOP];
        uint32_t cm = ff_argmask(cop), am = ff_argmask(aop);
        DWORD c0 = (cm & 1) ? t[D3DTSS_COLORARG0] : 0, c1 = (cm & 2) ? t[D3DTSS_COLORARG1] : 0, c2 = (cm & 4) ? t[D3DTSS_COLORARG2] : 0;
        DWORD a0 = (am & 1) ? t[D3DTSS_ALPHAARG0] : 0, a1 = (am & 2) ? t[D3DTSS_ALPHAARG1] : 0, a2 = (am & 4) ? t[D3DTSS_ALPHAARG2] : 0;
        if (!i && temp && aop == D3DTOP_DISABLE) {     /* (the first stage's alpha from diffuse if left undefined) */
            aop = D3DTOP_SELECTARG1;
            a0 = 0, a1 = D3DTA_DIFFUSE, a2 = 0;
        }
        if (i + 1 == active) result = D3DTA_CURRENT;    /* (the last stage always writes current) */
        ops[i] = (cop & 0x1f) | (aop & 0x1f) << 8 | ((result & D3DTA_SELECTMASK) == D3DTA_TEMP ? 1u << 15 : 0);
        args[i] = 0;
        if (cop != D3DTOP_DISABLE) args[i] |= ff_arg(c0) | ff_arg(c1) << 5 | ff_arg(c2) << 10;
        if (aop != D3DTOP_DISABLE) args[i] |= ff_arg(a0) << 16 | ff_arg(a1) << 21 | ff_arg(a2) << 26;
    }
    for (int j = 0; j < 4; j++) k->spec[VK_FFP_OPS + j] = (ops[2 * j] & 0xffff) | (ops[2 * j + 1] & 0xffff) << 16;
    for (int i = 0; i < 8; i++) k->spec[VK_FFP_ARGS + i] = args[i];
    return (1u << active) - 1;
}

/* the pixel block after the extra vec4s (ff.frag PsData): texture factor and fog colour, stage
   constants, bump matrices, luminance scale and offset */
static void ff_data_ps(float *e)
{
    uint32_t *u = (uint32_t *)e;
    memset(e, 0, VK_FF_PS_CONSTS * 16);
    u[0] = S.rs[D3DRS_TEXTUREFACTOR];
    u[1] = S.rs[D3DRS_FOGCOLOR];
    for (int i = 0; i < 8; i++) u[4 + i] = S.tss[i][D3DTSS_CONSTANT];
    for (int i = 0; i < 8; i++) {
        e[12 + 4 * i] = f_of(S.tss[i][D3DTSS_BUMPENVMAT00]);
        e[13 + 4 * i] = f_of(S.tss[i][D3DTSS_BUMPENVMAT01]);
        e[14 + 4 * i] = f_of(S.tss[i][D3DTSS_BUMPENVMAT10]);
        e[15 + 4 * i] = f_of(S.tss[i][D3DTSS_BUMPENVMAT11]);
        e[44 + 2 * i] = f_of(S.tss[i][D3DTSS_BUMPENVLSCALE]);
        e[45 + 2 * i] = f_of(S.tss[i][D3DTSS_BUMPENVLOFFSET]);
    }
}

/* the vertex block into the ring for chunk 0 of the vertex constants (draw_prep) */
static int consts_ffv(void)
{
    VkRing r;
    if (!vk_ring(sizeof(FFVsData), ubo_align, &r)) return 0;
    if (r.cset != B.cset || !B.consts_valid) consts_rechunk(&r);
    ff_data_vs((FFVsData *)r.map);
    B.coff[0] = (uint32_t)r.off;
    B.vvalid = 0;                           /* (chunk 0 is ours now: a programmable shader uploads again) */
    B.vcontig = 0;
    B.ffv_valid = 1;
    dirty_ffv = 0;
    return 1;
}

/* the fixed-function shaders as shader objects of ours (draw_prep uses them when none is bound) */
static VkShader ff_vs, ff_ps;

static void ff_init(void)
{
    if (ff_vs.meta) return;
    ff_vs.hash = VK_FF_VS_HASH;
    ff_vs.id = next_id++;
    ff_vs.module = module_of(VK_FF_VS_HASH, &ff_vs.meta);
    ff_ps.hash = VK_FF_PS_HASH;
    ff_ps.id = next_id++;
    ff_ps.module = module_of(VK_FF_PS_HASH, &ff_ps.meta);
    if (!ff_vs.meta || !ff_ps.meta) LOG(LOG_ALWAYS, "vulkan renderer: no fixed-function shaders: such draws are skipped");
}

/* the vertex shader's chunks k0..k1-1 (one after the other), its def'd constants over the program's */
static int consts_vs(const VkShader *vs, int k0, int k1)
{
    VkRing r;
    uint32_t bytes = (uint32_t)(k1 - k0) * VK_VS_CHUNK * 16;
    if (!vk_ring(bytes, ubo_align, &r)) return 0;
    if (r.cset != B.cset || !B.consts_valid) consts_rechunk(&r);
    memcpy(r.map, S.vsc[k0 * VK_VS_CHUNK], bytes);
    uint32_t lo = (uint32_t)k0 * VK_VS_CHUNK, hi = (uint32_t)k1 * VK_VS_CHUNK;
    for (uint32_t d = 0; d < vs->meta->ndef; d++) {
        const VkShaderDef *df = &vk_shader_defs[vs->meta->def0 + d];
        if (df->reg >= lo && df->reg < hi) memcpy(r.map + 16 * (df->reg - lo), df->v, 16);
    }
    for (int k = k0; k < k1; k++) {
        B.coff[k] = (uint32_t)r.off + (uint32_t)(k - k0) * VK_VS_CHUNK * 16;
        B.vdef[k] = (vs->defmask >> k) & 1 ? vs->id : 0;
    }
    uint32_t m = ((1u << (k1 - k0)) - 1) << k0;
    if (k0 == 0) B.ffv_valid = 0;           /* (chunk 0 was the fixed-function block) */
    B.vvalid |= m;
    vs_dirty &= ~m;
    B.vcontig = k0 == 0 && k1 == VK_VS_CHUNKS;
    return 1;
}

static int consts_ps(const VkShader *ps, int unorm8, VkRing *r)
{
    const VkShaderMeta *pm = ps->meta;
    uint32_t pn = pm->cmax;
    if (!vk_ring((VK_PS_EXTRA + pn) * 16, ubo_align, r)) return 0;
    if (r->cset != B.cset || !B.consts_valid) consts_rechunk(r);
    float *e = (float *)r->map;
    DWORD fc = S.rs[D3DRS_FOGCOLOR];
    float fs = f_of(S.rs[D3DRS_FOGSTART]), fe = f_of(S.rs[D3DRS_FOGEND]);
    e[0] = ((fc >> 16) & 255) / 255.0f;
    e[1] = ((fc >> 8) & 255) / 255.0f;
    e[2] = (fc & 255) / 255.0f;
    e[3] = f_of(S.rs[D3DRS_FOGDENSITY]);
    e[4] = fe;
    e[5] = fe != fs ? 1.0f / (fe - fs) : 3.402823e38f;
    e[6] = unorm8 ? (float)(S.rs[D3DRS_ALPHAREF] & 255) : (float)(S.rs[D3DRS_ALPHAREF] & 255) / 255.0f;
    e[7] = unorm8 ? 255.0f : 0.0f;
    /* (Direct3D's texbem: u += M00 du + M10 dv, v += M01 du + M11 dv; larush-native found DXVK 3.1.1
       applying the matrix transposed in programmable shaders and kept Direct3D's: its VULKAN.md 3.2) */
    for (int t = 0; t < 4; t++) {
        e[8 + 4 * t] = f_of(S.tss[t][D3DTSS_BUMPENVMAT00]);
        e[9 + 4 * t] = f_of(S.tss[t][D3DTSS_BUMPENVMAT01]);
        e[10 + 4 * t] = f_of(S.tss[t][D3DTSS_BUMPENVMAT10]);
        e[11 + 4 * t] = f_of(S.tss[t][D3DTSS_BUMPENVMAT11]);
        e[24 + 2 * t] = f_of(S.tss[t][D3DTSS_BUMPENVLSCALE]);
        e[25 + 2 * t] = f_of(S.tss[t][D3DTSS_BUMPENVLOFFSET]);
    }
    if (ps->hash == VK_FF_PS_HASH)
        ff_data_ps(e + 4 * VK_PS_EXTRA);
    else
        memcpy(e + 4 * VK_PS_EXTRA, S.psc, pn * 16);
    for (uint32_t d = 0; d < pm->ndef; d++) {
        const VkShaderDef *df = &vk_shader_defs[pm->def0 + d];
        if (df->reg < pn) memcpy(e + 4 * (VK_PS_EXTRA + df->reg), df->v, 16);
    }
    B.coff[VK_VS_CHUNKS] = (uint32_t)r->off;
    B.pvalid = 1;
    return 1;
}

/* per sampler slot: the bindless sampler index for the slot's states and what kind of texture is
   there (sampler_for), kept until those change (dirty_samp, the texture's signature) */
static int samp_idx[VK_MAX_SAMPLER_SLOTS];
static uint8_t samp_sig[VK_MAX_SAMPLER_SLOTS];

static uint8_t tex_sig(const VkTex *t)
{
    return t ? (uint8_t)(1 | (t->faces == 6) << 1 | (t->levels <= 1) << 2 | (t->kind == 3) << 3) : 0x40;
}

/* the pipeline for the current state (dirty_pipe: something in its key may have changed) */
static PipeKey cur_key;
static VkPipeline cur_pipe;
static struct { uint32_t decl, vs, streams; } scache;      /* the streams a declaration and shader read */

/* everything a draw needs, recorded; 0: skip the draw */
static int draw_prep(D3DPRIMITIVETYPE prim, uint32_t vfirst, uint32_t vcount)
{
    VkShader *vs = S.vs, *ps = S.ps;
    const VkDecl *decl = S.decl;
    if (!decl) {
        if (once_("draw without a declaration")) LOG(LOG_ALWAYS, "vulkan renderer: a draw without a declaration or FVF: skipped");
        return 0;
    }
    /* fixed function: no vertex shader or pre-transformed vertices (DXVK's UseProgrammableVS), no pixel
       shader */
    int ffv = !vs || (decl->ffflags & DECL_POSITIONT), ffp = !ps;
    if (ffv) vs = &ff_vs;
    if (ffp) ps = &ff_ps;
    if (!vs->meta || !ps->meta) {
        if (once_("draw without SPIR-V")) LOG(LOG_ALWAYS, "vulkan renderer: a draw with a shader that has no SPIR-V: skipped");
        return 0;
    }
    if (!begin_pass()) return 0;
    VkCommandBuffer cb = B.cb;
    VkTex *rt = B.prt->tex;
    const VkShaderMeta *pm = ps->meta;
    uint64_t cur = R.serial + 1;
    uint8_t topo = (uint8_t)topo_class(prim);
    /* the fixed-function parts of the key, every draw (a few dozen operations; the stages in use decide
       the samplers) */
    PipeKey fk;
    uint32_t smask = pm->samplers & ((1u << VK_MAX_SAMPLER_SLOTS) - 1);
    if (ffp) smask = ff_key_ps(&fk);
    /* textures and samplers: the slots' words, the kinds bound (in the pipeline's key) */
    uint32_t slots[VK_MAX_SAMPLER_SLOTS] = {0}, kinds = 0, nulls = 0;
    for (uint32_t m = smask; m; m &= m - 1) {
        int s = __builtin_ctz(m);
        VkTex *t = S.tex[s];
        uint8_t sig = tex_sig(t);
        if (t) {
            t->used = cur;
            kinds |= (uint32_t)t->kind << (2 * s);
        } else nulls |= 1u << s;
        if (sig != samp_sig[s] || (dirty_samp & (1u << s))) {
            samp_idx[s] = sampler_for(s, t);
            samp_sig[s] = sig;
            dirty_samp &= ~(1u << s);
        }
        slots[s] = (uint32_t)(t && t->slot ? t->slot : 0) | ((uint32_t)samp_idx[s] << 16);
    }
    uint32_t proj = 0;
    if (ffp || ffv) {
        if (ffp) {
            proj = ff_proj(smask);
            uint32_t fl = (S.rs[D3DRS_SPECULARENABLE] ? 1u : 0) | (topo == 0 && S.rs[D3DRS_POINTSPRITEENABLE] ? 2u : 0) | nulls << 8;
            if (S.rs[D3DRS_FOGENABLE] && ffv && !(decl->ffflags & DECL_POSITIONT)) {
                /* (DXVK's UpdateFog: Z fog when the projection is not a perspective one) */
                const float (*m)[4] = (const float (*)[4])xforms[D3DTS_PROJECTION].m;
                float wn = 1, wf = 1;
                if (!(m[2][2] == 0.0f || m[2][2] == m[2][3])) {
                    wn = m[3][3] - m[3][2] / m[2][2] * m[2][3];
                    wf = (m[3][3] - m[3][2]) / (m[2][2] - m[2][3]) * m[2][3] + m[3][3];
                }
                if (wn == 1.0f && wf == 1.0f) fl |= 4u;
            }
            fk.spec[VK_FFP_FLAGS] = fl;
        } else {
            for (uint32_t m = pm->proj & smask; m; m &= m - 1) {
                int s = __builtin_ctz(m);
                if (S.tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) proj |= 1u << s;
            }
        }
        if (ffv) ff_key_vs(&fk, decl, ffp ? NULL : ps, topo, ffp ? proj : ff_proj(smask));
        if ((ffp && memcmp(&fk.spec[VK_FFP_OPS], &cur_key.spec[VK_FFP_OPS], 4 * (VK_SPEC_COUNT - VK_FFP_OPS))) ||
            (ffv && memcmp(fk.vspec, cur_key.vspec, sizeof fk.vspec)))
            dirty_pipe = 1;
    }
    /* the pipeline */
    if (dirty_pipe || kinds != cur_key.spec[VK_SPEC_KINDS] || topo != cur_key.topo || vs->hash != cur_key.vs ||
        ps->hash != cur_key.ps) {
        PipeKey k;
        memset(&k, 0, sizeof k);
        k.vs = vs->hash;
        k.ps = ps->hash;
        k.decl = S.decl->hash;
        k.topo = topo;
        k.fill = (uint8_t)S.rs[D3DRS_FILLMODE];
        k.samples = (uint8_t)rt->samples;
        k.blend = S.rs[D3DRS_ALPHABLENDENABLE] ? 1 : 0;
        DWORD src = S.rs[D3DRS_SRCBLEND], dst = S.rs[D3DRS_DESTBLEND];
        if (src == D3DBLEND_BOTHSRCALPHA) src = D3DBLEND_SRCALPHA, dst = D3DBLEND_INVSRCALPHA;
        if (src == D3DBLEND_BOTHINVSRCALPHA) src = D3DBLEND_INVSRCALPHA, dst = D3DBLEND_SRCALPHA;
        k.src = (uint8_t)src;
        k.dst = (uint8_t)dst;
        k.op = (uint8_t)S.rs[D3DRS_BLENDOP];
        if (S.rs[D3DRS_SEPARATEALPHABLENDENABLE]) {
            k.srca = (uint8_t)S.rs[D3DRS_SRCBLENDALPHA];
            k.dsta = (uint8_t)S.rs[D3DRS_DESTBLENDALPHA];
            k.opa = (uint8_t)S.rs[D3DRS_BLENDOPALPHA];
        } else {
            k.srca = k.src;
            k.dsta = k.dst;
            k.opa = k.op;
        }
        if (!k.blend) k.src = k.dst = k.op = k.srca = k.dsta = k.opa = 0;
        k.wmask = (uint8_t)(S.rs[D3DRS_COLORWRITEENABLE] & 15);
        k.center = rt->samples > 1 && !S.rs[D3DRS_MULTISAMPLEANTIALIAS] && R.sample_locations &&
                   (R.sample_location_counts & rt->samples);
        if (!R.variable_sample_locations && B.center != k.center) {
            /* (fixed sample locations within a render pass on this device: a new pass) */
            end_pass();
            if (!begin_pass()) return 0;
            cb = B.cb;
        }
        B.center = k.center;
        k.rtfmt = rt->fmt;
        k.dsfmt = B.pds ? B.pds->tex->fmt : 0;
        k.spec[VK_SPEC_ALPHA] = S.rs[D3DRS_ALPHATESTENABLE] ? S.rs[D3DRS_ALPHAFUNC] : D3DCMP_ALWAYS;
        if (k.spec[VK_SPEC_ALPHA] < D3DCMP_NEVER || k.spec[VK_SPEC_ALPHA] > D3DCMP_ALWAYS) k.spec[VK_SPEC_ALPHA] = D3DCMP_ALWAYS;
        k.spec[VK_SPEC_FOG] = S.rs[D3DRS_FOGENABLE] && pm->major < 3 ? 1 + (S.rs[D3DRS_FOGTABLEMODE] & 3) : 0;
        if (!ffp && !ffv)
            for (uint32_t m = pm->proj & smask; m; m &= m - 1) {
                int s = __builtin_ctz(m);
                if (S.tss[s][D3DTSS_TEXTURETRANSFORMFLAGS] & D3DTTFF_PROJECTED) proj |= 1u << s;
            }
        if (ffp) memcpy(&k.spec[VK_FFP_OPS], &fk.spec[VK_FFP_OPS], 4 * (VK_SPEC_COUNT - VK_FFP_OPS));
        if (ffv) memcpy(k.vspec, fk.vspec, sizeof k.vspec);
        k.spec[VK_SPEC_KINDS] = kinds;
        k.spec[VK_SPEC_PROJ] = proj;
        k.spec[VK_SPEC_SHADOWF] = (kinds & 0xaaaau) ? (uint32_t)shadow_filter_opt : 0;   /* (only with a depth compare) */
        if (!cur_pipe || memcmp(&k, &cur_key, sizeof k)) {
            cur_pipe = pipeline(&k, decl->el, decl->nel, 0);
            cur_key = k;
        }
        dirty_pipe = 0;
    }
    VkPipeline p = cur_pipe;
    if (!p) return 0;
    if (p != B.pipe) {
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
        B.pipe = p;
    }
    if (dirty_dyn || !B.dyn_valid) {
        set_dynamic(dirty_dyn);
        dirty_dyn = 0;
    }
    VkPrimitiveTopology tv = topo_vk(prim);
    if (tv != B.topo) {
        vkCmdSetPrimitiveTopology(cb, tv);
        B.topo = tv;
    }
    if (!B.slots_valid || memcmp(slots, B.slots, sizeof slots)) {
        vkCmdPushConstants(cb, R.layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof slots, slots);
        memcpy(B.slots, slots, sizeof slots);
        B.slots_valid = 1;
    }
    /* constants (VULKAN.md "Shaders"): the vertex shader's in chunks of VK_VS_CHUNK registers, each
       uploaded again only when registers in it changed or it holds another shader's def'd constants
       (a shader with relative addressing: all of them in one piece); the pixel shader's block when
       what it reads changed; all in the same ring chunk (one descriptor set, its dynamic offsets) */
    int unorm8 = rt->fmt == D3DFMT_A8R8G8B8 || rt->fmt == D3DFMT_X8R8G8B8 || rt->fmt == D3DFMT_R5G6B5 ||
                 rt->fmt == D3DFMT_A8B8G8R8 || rt->fmt == D3DFMT_X8B8G8R8;
    if (!ubo_align) {
        ubo_align = (uint32_t)R.props.limits.minUniformBufferOffsetAlignment;
        if (ubo_align < 16) ubo_align = 16;
    }
    int rebind = !B.consts_valid;
    for (int tries = 0; tries < 2; tries++) {
        unsigned rc0 = c_rechunks;
        int was_valid = B.consts_valid;
        uint32_t vn = vs->meta->cmax ? vs->meta->cmax : 1;
        int nk = (int)((vn + VK_VS_CHUNK - 1) / VK_VS_CHUNK);
        if (nk > VK_VS_CHUNKS) nk = VK_VS_CHUNKS;
        if (ffv) {
            if (!B.consts_valid || !B.ffv_valid || dirty_ffv) {
                if (!consts_ffv()) return 0;
                rebind = 1;
            }
        } else if (vs->meta->flags & VK_SH_RELATIVE) {
            int ok = B.consts_valid && B.vcontig && !(vs_dirty & 0xffu);
            for (int k = 0; ok && k < VK_VS_CHUNKS; k++)
                if (B.vdef[k] != ((vs->defmask >> k) & 1 ? vs->id : 0)) ok = 0;
            if (!ok) {
                if (!consts_vs(vs, 0, VK_VS_CHUNKS)) return 0;
                rebind = 1;
            }
        } else {
            for (int k = 0; k < nk; k++) {
                if (B.consts_valid && ((B.vvalid >> k) & 1) && !((vs_dirty >> k) & 1) &&
                    B.vdef[k] == ((vs->defmask >> k) & 1 ? vs->id : 0))
                    continue;
                int k1 = k + 1;                 /* (neighbours needing it too: one piece) */
                while (k1 < nk && !(B.consts_valid && ((B.vvalid >> k1) & 1) && !((vs_dirty >> k1) & 1) &&
                                    B.vdef[k1] == ((vs->defmask >> k1) & 1 ? vs->id : 0)))
                    k1++;
                if (!consts_vs(vs, k, k1)) return 0;
                B.consts_valid = 1;
                rebind = 1;
                k = k1 - 1;
            }
        }
        B.consts_valid = 1;
        if (!B.pvalid || ps->id != B.c_ps || dirty_psx || unorm8 != B.c_unorm8 || (psc_lo < pm->cmax && psc_lo < psc_hi)) {
            VkRing r;
            if (!consts_ps(ps, unorm8, &r)) return 0;
            B.c_ps = ps->id;
            B.c_unorm8 = unorm8;
            psc_lo = VK_PS_CONSTS, psc_hi = 0;
            dirty_psx = 0;
            rebind = 1;
        }
        /* (a new ring chunk under uploads kept from before: everything again into it; the first upload
           of a command buffer always starts one, nothing kept) */
        if (c_rechunks - rc0 <= (was_valid ? 0u : 1u)) break;
        B.vvalid = 0;
        B.pvalid = 0;
        B.vcontig = 0;
        B.ffv_valid = 0;
    }
    if (rebind)
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, R.layout, 1, 1, &B.cset, VK_VS_CHUNKS + 1, B.coff);
    /* vertex buffers (the ones the declaration reads), and the zero buffer for the rest */
    if (scache.decl != decl->id || scache.vs != vs->id) {
        uint32_t streams = 0;
        for (int e = 0; e < decl->nel; e++) {
            int loc = vk_input_loc(decl->el[e].Usage, decl->el[e].UsageIndex);
            if (loc >= 0 && (vs->meta->inputs & (1u << loc))) streams |= 1u << decl->el[e].Stream;
        }
        scache.decl = decl->id;
        scache.vs = vs->id;
        scache.streams = streams & 0xffff;
    }
    for (uint32_t m = scache.streams; m; m &= m - 1) {
        int s = __builtin_ctz(m);
        VkBuf *b = S.vb[s];
        if (!b) {
            if (once_("draw from no stream")) LOG(LOG_ALWAYS, "vulkan renderer: a draw reads a stream with no buffer: skipped");
            return 0;
        }
        uint32_t lo = S.vb_off[s] + vfirst * S.vb_stride[s], hi = lo + vcount * S.vb_stride[s] + 64;
        if (b->used != cur) b->used_lo = lo, b->used_hi = hi;
        else {
            if (lo < b->used_lo) b->used_lo = lo;
            if (hi > b->used_hi) b->used_hi = hi;
        }
        b->used = cur;
        VkDeviceSize off = b->sub.off + S.vb_off[s], stride = S.vb_stride[s];
        if (B.vbuf[s] != b->sub.buf || B.voff[s] != off || B.vstride[s] != stride) {
            vkCmdBindVertexBuffers2(cb, (uint32_t)s, 1, &b->sub.buf, &off, NULL, &stride);
            B.vbuf[s] = b->sub.buf;
            B.voff[s] = off;
            B.vstride[s] = stride;
        }
    }
    if (!B.zero_bound) {
        /* (binding 16: the zero buffer for inputs the declaration does not give; pipeline()) */
        VkDeviceSize zoff = R.zero.off, zs = 0;
        vkCmdBindVertexBuffers2(cb, 16, 1, &R.zero.buf, &zoff, NULL, &zs);
        B.zero_bound = 1;
    }
    return 1;
}

static HRESULT dev_DrawPrimitive(void *This, D3DPRIMITIVETYPE prim, UINT start, UINT count)
{
    (void)This;
    uint32_t n = prim_verts(prim, count);
    if (!count || !draw_prep(prim, start, n)) return D3D_OK;
    vkCmdDraw(B.cb, n, 1, start, 0);
    return D3D_OK;
}

static HRESULT dev_DrawIndexedPrimitive(void *This, D3DPRIMITIVETYPE prim, INT base, UINT minv, UINT numv, UINT starti, UINT count)
{
    (void)This;
    if (!count || !S.ib) return D3D_OK;
    if (!draw_prep(prim, (uint32_t)(base + (INT)minv), numv)) return D3D_OK;
    VkBuf *ib = S.ib;
    VkIndexType it = ib->fmt == D3DFMT_INDEX32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    uint32_t isz = it == VK_INDEX_TYPE_UINT32 ? 4 : 2, n = prim_verts(prim, count);
    uint64_t cur = R.serial + 1;
    uint32_t lo = starti * isz, hi = lo + n * isz;
    if (ib->used != cur) ib->used_lo = lo, ib->used_hi = hi;
    else {
        if (lo < ib->used_lo) ib->used_lo = lo;
        if (hi > ib->used_hi) ib->used_hi = hi;
    }
    ib->used = cur;
    if (B.ibuf != ib->sub.buf || B.ioff != ib->sub.off || B.itype != it) {
        vkCmdBindIndexBuffer(B.cb, ib->sub.buf, ib->sub.off, it);
        B.ibuf = ib->sub.buf;
        B.ioff = ib->sub.off;
        B.itype = it;
    }
    vkCmdDrawIndexed(B.cb, n, 1, starti, base, 0);
    return D3D_OK;
}

/* drawing from memory: the vertices (and indices) into the ring; stream 0 and the indices are unset
   afterwards, as Direct3D does */
static void unset_stream0(void)
{
    VkBuf *old = S.vb[0];
    S.vb[0] = NULL;
    S.vb_off[0] = S.vb_stride[0] = 0;
    unbound(old);
}

static void unset_indices(void)
{
    VkBuf *old = S.ib;
    S.ib = NULL;
    unbound(old);
}

static HRESULT dev_DrawPrimitiveUP(void *This, D3DPRIMITIVETYPE prim, UINT count, const void *data, UINT stride)
{
    (void)This;
    uint32_t n = prim_verts(prim, count);
    if (!count || !data) return D3D_OK;
    VkRing r;
    if (!vk_ring(n * stride, 16, &r)) return D3D_OK;
    memcpy(r.map, data, n * stride);
    /* (a temporary buffer object standing for the ring's range as stream 0) */
    static VkBuf tmp;
    tmp.sub.buf = r.buf;
    tmp.sub.off = r.off;
    tmp.size = n * stride;
    unset_stream0();
    S.vb[0] = &tmp;
    S.vb_stride[0] = stride;
    tmp.o.refs = 1;
    tmp.o.irefs = 1000;
    int ok = draw_prep(prim, 0, n);
    S.vb[0] = NULL;
    S.vb_stride[0] = 0;
    if (ok) vkCmdDraw(B.cb, n, 1, 0, 0);
    unset_indices();
    return D3D_OK;
}

static HRESULT dev_DrawIndexedPrimitiveUP(void *This, D3DPRIMITIVETYPE prim, UINT minv, UINT nv, UINT count, const void *idx,
                                          D3DFORMAT ifmt, const void *data, UINT stride)
{
    (void)This;
    if (!count || !data || !idx) return D3D_OK;
    uint32_t n = prim_verts(prim, count), isz = ifmt == D3DFMT_INDEX32 ? 4 : 2;
    uint32_t vbytes = (minv + nv) * stride;
    VkRing r;
    if (!vk_ring(vbytes + n * isz + 16, 16, &r)) return D3D_OK;
    memcpy(r.map, data, vbytes);
    uint32_t ioff = (vbytes + 15) & ~15u;
    memcpy(r.map + ioff, idx, n * isz);
    static VkBuf tmp;
    tmp.sub.buf = r.buf;
    tmp.sub.off = r.off;
    tmp.size = vbytes;
    unset_stream0();
    S.vb[0] = &tmp;
    S.vb_stride[0] = stride;
    tmp.o.irefs = 1000;
    int ok = draw_prep(prim, minv, nv);
    S.vb[0] = NULL;
    S.vb_stride[0] = 0;
    if (ok) {
        VkIndexType it = isz == 4 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
        vkCmdBindIndexBuffer(B.cb, r.buf, r.off + ioff, it);
        B.ibuf = r.buf;
        B.ioff = r.off + ioff;
        B.itype = it;
        vkCmdDrawIndexed(B.cb, n, 1, 0, 0, 0);
    }
    unset_indices();
    return D3D_OK;
}

/* ------------------------------------------------------------------ clears, copies */
static HRESULT dev_Clear(void *This, DWORD n, const D3DRECT *rects, DWORD flags, D3DCOLOR color, float z, DWORD stencil)
{
    (void)This;
    /* the whole target before the pass's first draw: its load operation (pclear) */
    VkSurf *rt0 = rts[0];
    if (!B.pass && rt0 && rt0->view) {
        LONG tw = (LONG)lvl_dim(rt0->tex->w, rt0->level), th = (LONG)lvl_dim(rt0->tex->h, rt0->level);
        int full = S.vp.X == 0 && S.vp.Y == 0 && (LONG)S.vp.Width >= tw && (LONG)S.vp.Height >= th &&
                   (!S.rs[D3DRS_SCISSORTESTENABLE] || (S.scissor.left <= 0 && S.scissor.top <= 0 &&
                                                        S.scissor.right >= tw && S.scissor.bottom >= th)) &&
                   (!rects || !n || (n == 1 && rects[0].x1 <= 0 && rects[0].y1 <= 0 && rects[0].x2 >= tw && rects[0].y2 >= th));
        if (full) {
            if (flags & D3DCLEAR_TARGET) {
                pclear.color[0] = ((color >> 16) & 255) / 255.0f;
                pclear.color[1] = ((color >> 8) & 255) / 255.0f;
                pclear.color[2] = (color & 255) / 255.0f;
                pclear.color[3] = (color >> 24) / 255.0f;
            }
            if (flags & D3DCLEAR_ZBUFFER) pclear.z = z < 0 ? 0 : z > 1 ? 1 : z;
            if (flags & D3DCLEAR_STENCIL) pclear.stencil = stencil & 0xff;
            pclear.flags |= flags & (D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER | D3DCLEAR_STENCIL);
            return D3D_OK;
        }
    }
    if (!begin_pass()) return D3D_OK;
    VkClearAttachment a[2];
    uint32_t na = 0;
    if (flags & D3DCLEAR_TARGET) {
        VkClearAttachment c = {VK_IMAGE_ASPECT_COLOR_BIT, 0, {.color = {.float32 = {((color >> 16) & 255) / 255.0f,
            ((color >> 8) & 255) / 255.0f, (color & 255) / 255.0f, (color >> 24) / 255.0f}}}};
        a[na++] = c;
    }
    VkImageAspectFlags da = 0;
    if (B.pds && (flags & D3DCLEAR_ZBUFFER)) da |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (B.pds && B.pds->tex->stencil && (flags & D3DCLEAR_STENCIL)) da |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (da) {
        VkClearAttachment d = {da, 0, {.depthStencil = {z < 0 ? 0 : z > 1 ? 1 : z, stencil & 0xff}}};
        a[na++] = d;
    }
    if (!na) return D3D_OK;
    /* each rectangle (or the viewport) within the viewport and the target */
    int32_t vx0 = (int32_t)S.vp.X, vy0 = (int32_t)S.vp.Y, vx1 = vx0 + (int32_t)S.vp.Width, vy1 = vy0 + (int32_t)S.vp.Height;
    if (S.rs[D3DRS_SCISSORTESTENABLE]) {
        if (S.scissor.left > vx0) vx0 = S.scissor.left;
        if (S.scissor.top > vy0) vy0 = S.scissor.top;
        if (S.scissor.right < vx1) vx1 = S.scissor.right;
        if (S.scissor.bottom < vy1) vy1 = S.scissor.bottom;
    }
    VkClearRect cr[16];
    uint32_t nr = 0;
    for (DWORD i = 0; i < (rects && n ? n : 1) && nr < 16; i++) {
        int32_t x0 = vx0, y0 = vy0, x1 = vx1, y1 = vy1;
        if (rects && n) {
            if (rects[i].x1 > x0) x0 = rects[i].x1;
            if (rects[i].y1 > y0) y0 = rects[i].y1;
            if (rects[i].x2 < x1) x1 = rects[i].x2;
            if (rects[i].y2 < y1) y1 = rects[i].y2;
        }
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int32_t)B.pw) x1 = (int32_t)B.pw;
        if (y1 > (int32_t)B.ph) y1 = (int32_t)B.ph;
        if (x1 <= x0 || y1 <= y0) continue;
        cr[nr++] = (VkClearRect){{{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}}, 0, 1};
    }
    if (nr) vkCmdClearAttachments(B.cb, na, a, nr, cr);
    return D3D_OK;
}

/* the back buffer touched by something other than its passes (a copy into or out of it, a lock): what
   the frame drew into the swapchain image copied back into it, and the rest of the frame drawn there
   (Present then blits it, as without vk_sc_target) */
static void bb_materialize(const VkSurf *s)
{
    if (s != backbuf || sc_frame != 1) return;
    end_pass();
    new_cb_check();
    barrier_all(B.cb);
    VkImageCopy c = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0},
                     {backbuf->tex->w, backbuf->tex->h, 1}};
    vkCmdCopyImage(B.cb, R.sc_images[R.sc_idx], VK_IMAGE_LAYOUT_GENERAL, backbuf->tex->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier_all(B.cb);
    sc_frame = 2;
    if (once_("back buffer materialized"))
        LOG(LOG_ALWAYS, "vulkan renderer: the back buffer copied or locked: that frame drawn into it, not the swapchain image");
}

static HRESULT dev_StretchRect(void *This, IDirect3DSurface9 *srcp, const RECT *sr, IDirect3DSurface9 *dstp, const RECT *dr,
                               D3DTEXTUREFILTERTYPE filter)
{
    (void)This;
    VkSurf *src = (VkSurf *)srcp, *dst = (VkSurf *)dstp;
    bb_materialize(src);
    bb_materialize(dst);
    if (!src || !dst || !src->tex->image || !dst->tex->image) return D3DERR_INVALIDCALL;
    VkTex *st = src->tex, *dt = dst->tex;
    end_pass();
    new_cb_check();
    VkCommandBuffer cb = B.cb;
    RECT s = sr ? *sr : (RECT){0, 0, (LONG)lvl_dim(st->w, src->level), (LONG)lvl_dim(st->h, src->level)};
    RECT d = dr ? *dr : (RECT){0, 0, (LONG)lvl_dim(dt->w, dst->level), (LONG)lvl_dim(dt->h, dst->level)};
    if (st->depth || dt->depth) {
        /* a depth buffer into a depth texture of the same Vulkan format and size (the ambient
           occlusion's INTZ copy; third_party/dxvk-depth-texture.patch did this for DXVK): a copy, or
           from a multisampled one a resolve of sample 0 (no mix of a near and a far surface at an
           edge), the depth alone (the stencil is never read) */
        uint32_t w = lvl_dim(st->w, src->level), h = lvl_dim(st->h, src->level);
        if (!st->depth || !dt->depth || st->vkfmt != dt->vkfmt || w != lvl_dim(dt->w, dst->level) ||
            h != lvl_dim(dt->h, dst->level) || sr || dr || dt->samples > 1) {
            if (once_("depth StretchRect kind")) LOG(LOG_ALWAYS, "vulkan renderer: a depth StretchRect of this kind is not done");
            return D3D_OK;
        }
        if (st->samples > 1) {
            /* (a resolve at the end of an empty render pass: graphics stages) */
            VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, src->view, VK_IMAGE_LAYOUT_GENERAL,
                VK_RESOLVE_MODE_SAMPLE_ZERO_BIT, dst->view, VK_IMAGE_LAYOUT_GENERAL, VK_ATTACHMENT_LOAD_OP_LOAD,
                VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
            VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {w, h}}, 1, 0, 0, NULL, &da, NULL};
            barrier_to(cb, 0);
            vkCmdBeginRendering(cb, &ri);
            vkCmdEndRendering(cb);
            did(GFX_STAGES, PASS_WRITES);
        } else {
            VkImageCopy c = {{VK_IMAGE_ASPECT_DEPTH_BIT, src->level, src->face, 1}, {0, 0, 0},
                             {VK_IMAGE_ASPECT_DEPTH_BIT, dst->level, dst->face, 1}, {0, 0, 0}, {w, h, 1}};
            barrier_to(cb, 1);
            vkCmdCopyImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
            did(XFER_STAGES, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        }
        return D3D_OK;
    }
    VkImageSubresourceLayers sl = {VK_IMAGE_ASPECT_COLOR_BIT, src->level, src->face, 1};
    VkImageSubresourceLayers dl = {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, dst->face, 1};
    int same = s.right - s.left == d.right - d.left && s.bottom - s.top == d.bottom - d.top;
    /* A whole multisampled level into a single-sampled one of its size and format (the twins'
       resolves, the passes' scene copies): an empty render pass's resolve attachment (DXVK's
       resolveImageRp) with both images in COLOR_ATTACHMENT_OPTIMAL for it, put back in GENERAL
       after. RADV resolves faster that way: on the 890M a 2560x1440 resolve took 0.33 ms with
       vkCmdResolveImage or the same render pass in GENERAL, 0.23-0.26 like this (DXVK 0.26), a
       night frame 0.3-0.5 ms less (VULKAN.md 3.4.5); the pictures identical [verified]. The
       destination's old contents are not kept (the resolve writes all of it). A part of an image
       (a split screen's view) takes vkCmdResolveImage below. ETM_VK_RP_RESOLVE=0: every resolve
       vkCmdResolveImage. */
    static int rp_resolve = -1;
    if (rp_resolve < 0) rp_resolve = !(getenv("ETM_VK_RP_RESOLVE") && *getenv("ETM_VK_RP_RESOLVE") == '0');
    uint32_t sw = lvl_dim(st->w, src->level), sh = lvl_dim(st->h, src->level);
    if (rp_resolve && st->samples > 1 && dt->samples == 1 && st->vkfmt == dt->vkfmt && src->view && dst->view &&
        s.left == 0 && s.top == 0 && s.right == (LONG)sw && s.bottom == (LONG)sh && d.left == 0 && d.top == 0 &&
        d.right == (LONG)sw && d.bottom == (LONG)sh && lvl_dim(dt->w, dst->level) == sw && lvl_dim(dt->h, dst->level) == sh) {
        const VkImageLayout att = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier_to(cb, 0);              /* (the work pending; then the layouts, chained on its stages) */
        VkImageMemoryBarrier2 ib[2] = {
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_IMAGE_LAYOUT_GENERAL, att, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, st->image,
             {VK_IMAGE_ASPECT_COLOR_BIT, src->level, 1, src->face, 1}},
            {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
             VK_IMAGE_LAYOUT_UNDEFINED, att, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dt->image,
             {VK_IMAGE_ASPECT_COLOR_BIT, dst->level, 1, dst->face, 1}}};
        VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 2, ib};
        vkCmdPipelineBarrier2(cb, &di);
        VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, src->view, att,
            VK_RESOLVE_MODE_AVERAGE_BIT, dst->view, att, VK_ATTACHMENT_LOAD_OP_LOAD, VK_ATTACHMENT_STORE_OP_STORE, {{{0}}}};
        VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, {sw, sh}}, 1, 0, 1, &ca, NULL, NULL};
        vkCmdBeginRendering(cb, &ri);
        vkCmdEndRendering(cb);
        for (int i = 0; i < 2; i++) {   /* (back to GENERAL, visible to whatever comes next) */
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
            ib[i].srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
            ib[i].dstStageMask = GFX_STAGES | XFER_STAGES;
            ib[i].dstAccessMask = GFX_ACCESS | XFER_ACCESS;
            ib[i].oldLayout = att;
            ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        }
        vkCmdPipelineBarrier2(cb, &di);
        return D3D_OK;
    }
    barrier_to(cb, 1);
    if (st->samples > 1 && dt->samples == 1 && same && st->vkfmt == dt->vkfmt) {
        VkImageResolve r = {sl, {s.left, s.top, 0}, dl, {d.left, d.top, 0}, {(uint32_t)(s.right - s.left), (uint32_t)(s.bottom - s.top), 1}};
        vkCmdResolveImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else if (same && st->vkfmt == dt->vkfmt && st->samples == dt->samples) {
        VkImageCopy c = {sl, {s.left, s.top, 0}, dl, {d.left, d.top, 0}, {(uint32_t)(s.right - s.left), (uint32_t)(s.bottom - s.top), 1}};
        vkCmdCopyImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    } else if (st->samples == 1 && dt->samples == 1) {
        VkImageBlit b = {sl, {{s.left, s.top, 0}, {s.right, s.bottom, 1}}, dl, {{d.left, d.top, 0}, {d.right, d.bottom, 1}}};
        vkCmdBlitImage(cb, st->image, VK_IMAGE_LAYOUT_GENERAL, dt->image, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       filter == D3DTEXF_POINT || filter == D3DTEXF_NONE ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
    } else {
        if (once_("StretchRect kind")) LOG(LOG_ALWAYS, "vulkan renderer: a StretchRect of this kind is not done yet");
        return D3D_OK;
    }
    did(XFER_STAGES, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    return D3D_OK;
}

static int device_live;                  /* (the device made) */

static HRESULT dev_GetRenderTargetData(void *This, IDirect3DSurface9 *rtp, IDirect3DSurface9 *dstp)
{
    (void)This;
    VkSurf *rt = (VkSurf *)rtp, *dst = (VkSurf *)dstp;
    if (!rt || !dst || !rt->tex->image || rt->tex->samples > 1) return D3DERR_INVALIDCALL;
    VkTex *t = rt->tex;
    const Fmt *f = fmt_of(t->fmt);
    uint32_t w = lvl_dim(t->w, rt->level), h = lvl_dim(t->h, rt->level), pitch = w * f->bpb;
    VkDeviceSize bytes = (VkDeviceSize)pitch * h;
    VkBuffer buf;
    VkBufferCreateInfo bi = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    if (vkCreateBuffer(R.dev, &bi, NULL, &buf) != VK_SUCCESS) return D3DERR_INVALIDCALL;
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(R.dev, buf, &mr);
    VkAlloc al;
    if (!vkm_alloc(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT, 0, &al) &&
        !vkm_alloc(&mr, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT, 0, &al)) {
        vkDestroyBuffer(R.dev, buf, NULL);
        return D3DERR_INVALIDCALL;
    }
    vkBindBufferMemory(R.dev, buf, al.mem, al.off);
    end_pass();
    new_cb_check();
    barrier_all(B.cb);
    VkBufferImageCopy c = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, rt->level, rt->face, 1}, {0, 0, 0}, {w, h, 1}};
    /* (the back buffer drawn into the frame's swapchain image: read there) */
    vkCmdCopyImageToBuffer(B.cb, rt == backbuf && sc_frame == 1 ? R.sc_images[R.sc_idx] : t->image, VK_IMAGE_LAYOUT_GENERAL, buf, 1, &c);
    barrier_all(B.cb);
    vk_submit(1);
    B.cb = VK_NULL_HANDLE;
    D3DLOCKED_RECT lr;
    if (surf_lock(dst, &lr, NULL, 0) == D3D_OK) {
        uint32_t dp = (uint32_t)lr.Pitch;
        for (uint32_t y = 0; y < h; y++) memcpy((uint8_t *)lr.pBits + (size_t)y * dp, al.map + (size_t)y * pitch, pitch < dp ? pitch : dp);
        dst->locked = 0;
    }
    vkDestroyBuffer(R.dev, buf, NULL);
    vkm_free(&al);
    return D3D_OK;
}

/* ------------------------------------------------------------------ the device: targets */
static void set_rt(int i, VkSurf *s)
{
    if (rts[i] == s) return;
    end_pass();
    if (s) iref(&s->o);
    if (rts[i]) iunref(&rts[i]->o);
    rts[i] = s;
}

static void set_ds(VkSurf *s)
{
    if (dsurf == s) return;
    end_pass();
    dirty_ffv = 1;                          /* (pre-transformed vertices' depth: with a depth buffer only) */
    if (s) iref(&s->o);
    if (dsurf) iunref(&dsurf->o);
    dsurf = s;
}

static HRESULT dev_SetRenderTarget(void *This, DWORD i, IDirect3DSurface9 *s)
{
    (void)This;
    if (i >= 4 || (i == 0 && !s)) return D3DERR_INVALIDCALL;
    if (i > 0 && s && once_("MRT")) LOG(LOG_ALWAYS, "vulkan renderer: more than one render target is not done yet");
    set_rt((int)i, (VkSurf *)s);
    if (i == 0 && s) {
        VkTex *t = ((VkSurf *)s)->tex;
        S.vp = (D3DVIEWPORT9){0, 0, lvl_dim(t->w, ((VkSurf *)s)->level), lvl_dim(t->h, ((VkSurf *)s)->level), 0, 1};
        S.scissor = (RECT){0, 0, (LONG)S.vp.Width, (LONG)S.vp.Height};
        dirty_dyn |= DYN_VP;
        dirty_ffv = 1;
    }
    return D3D_OK;
}

static HRESULT dev_GetRenderTarget(void *This, DWORD i, IDirect3DSurface9 **out)
{
    (void)This;
    if (i >= 4 || !out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface9 *)rts[i];
    if (!rts[i]) return D3DERR_NOTFOUND;
    com_AddRef(rts[i]);
    return D3D_OK;
}

static HRESULT dev_SetDepthStencilSurface(void *This, IDirect3DSurface9 *s) { (void)This; set_ds((VkSurf *)s); return D3D_OK; }

static HRESULT dev_GetDepthStencilSurface(void *This, IDirect3DSurface9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DSurface9 *)dsurf;
    if (!dsurf) return D3DERR_NOTFOUND;
    com_AddRef(dsurf);
    return D3D_OK;
}

static HRESULT dev_GetBackBuffer(void *This, UINT sc, UINT i, D3DBACKBUFFER_TYPE type, IDirect3DSurface9 **out)
{
    (void)This; (void)type;
    if (sc || i || !out || !backbuf) return D3DERR_INVALIDCALL;
    com_AddRef(backbuf);
    *out = (IDirect3DSurface9 *)backbuf;
    return D3D_OK;
}

/* ------------------------------------------------------------------ slot 0: nothing bound */
/* A sampler with no texture reads (0, 0, 0, 1) in Direct3D (the front end's fade quad multiplies by
   it): slot 0 of every bindless array holds a 1x1 texture of that colour (the depth one 1.0) */
static void dummies_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    static const uint8_t black[6][4] = {{0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}, {0, 0, 0, 255}};
    VkTex *d2 = tex_new(D3DRTYPE_TEXTURE, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, 1);
    VkTex *dc = tex_new(D3DRTYPE_CUBETEXTURE, 1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, 1);
    VkTex *dd = tex_new(D3DRTYPE_TEXTURE, 1, 1, 1, D3DUSAGE_DEPTHSTENCIL, D3DFMT_D16, D3DPOOL_DEFAULT, 1);
    if (!d2 || !dc || !dd) { LOG(LOG_ALWAYS, "vulkan renderer: no dummy textures"); return; }
    upload(d2, 0, 0, black[0], 4, 1);
    for (uint32_t f = 0; f < 6; f++) upload(dc, 0, f, black[f], 4, 1);
    /* a 3D one, made here (textures of ours are 2D and cube) */
    VkImage im3;
    VkAlloc al3;
    VkImageView v3 = VK_NULL_HANDLE;
    VkImageCreateInfo ii = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_3D, VK_FORMAT_R8G8B8A8_UNORM, {1, 1, 1}, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    VkCommandBuffer cb = vk_cmd();
    if (vkCreateImage(R.dev, &ii, NULL, &im3) == VK_SUCCESS) {
        VkMemoryRequirements mr;
        vkGetImageMemoryRequirements(R.dev, im3, &mr);
        if (vkm_alloc(&mr, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, &al3)) {
            vkBindImageMemory(R.dev, im3, al3.mem, al3.off);
            vk_image_layout(cb, im3, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 1);
            VkClearColorValue cv = {.float32 = {0, 0, 0, 1}};
            VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdClearColorImage(cb, im3, VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &rr);
            VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, im3, VK_IMAGE_VIEW_TYPE_3D,
                VK_FORMAT_R8G8B8A8_UNORM, {0, 0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
            vkCreateImageView(R.dev, &vi, NULL, &v3);
        }
    }
    VkClearDepthStencilValue dv = {1.0f, 0};
    VkImageSubresourceRange dr = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
    vkCmdClearDepthStencilImage(cb, dd->image, VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &dr);
    barrier_all(cb);
    vk_tex_write(0, d2->view, dc->view, v3, dd->view_depth);
}

/* ------------------------------------------------------------------ the device: creation and reset */
static int make_buffers(const D3DPRESENT_PARAMETERS *pp)
{
    cur_pp = *pp;
    UINT w = pp->BackBufferWidth ? pp->BackBufferWidth : 640, h = pp->BackBufferHeight ? pp->BackBufferHeight : 480;
    cur_pp.BackBufferWidth = w;
    cur_pp.BackBufferHeight = h;
    D3DFORMAT bf = pp->BackBufferFormat == D3DFMT_UNKNOWN ? D3DFMT_X8R8G8B8 : pp->BackBufferFormat;
    cur_pp.BackBufferFormat = bf;
    backbuf = surf_standalone(w, h, bf, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, 1);
    if (!backbuf) return 0;
    backbuf->o.refs = 0;                    /* (the device's alone: iref below) */
    autods = NULL;
    if (pp->EnableAutoDepthStencil) {
        autods = surf_standalone(w, h, pp->AutoDepthStencilFormat, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, 1);
        if (autods) autods->o.refs = 0;
    }
    iref(&backbuf->o);                      /* (kept: the device's own) */
    if (autods) iref(&autods->o);
    rs_defaults();
    rts[0] = rts[1] = rts[2] = rts[3] = NULL;
    dsurf = NULL;
    set_rt(0, backbuf);
    set_ds(autods);
    S.vp = (D3DVIEWPORT9){0, 0, w, h, 0, 1};
    S.scissor = (RECT){0, 0, (LONG)w, (LONG)h};
    R.vsync = pp->PresentationInterval == D3DPRESENT_INTERVAL_ONE;
    if (R.surface && (R.sc_w != w || R.sc_h != h || !R.swapchain)) {
        int ww = 0, wh = 0;
        SDL_GetWindowSizeInPixels(host_sdl_window(), &ww, &wh);
        vk_swapchain(ww > 0 ? ww : (int)w, wh > 0 ? wh : (int)h);
    }
    return 1;
}

static void drop_buffers(void)
{
    end_pass();
    for (int i = 0; i < 4; i++) set_rt(i, NULL);
    set_ds(NULL);
    memset(&S, 0, sizeof S);                /* (the slots hold no references) */
    zombies_sweep();
    if (backbuf) { iunref(&backbuf->o); backbuf = NULL; }
    if (autods) { iunref(&autods->o); autods = NULL; }
}

static HRESULT dev_Reset(void *This, D3DPRESENT_PARAMETERS *pp)
{
    (void)This;
    if (!pp) return D3DERR_INVALIDCALL;
    drop_buffers();
    if (R.cb) vk_submit(1);
    B.cb = VK_NULL_HANDLE;
    sc_frame = 0;                           /* (the new back buffer: drawn into the image again only if it fits) */
    vkDeviceWaitIdle(R.dev);
    if (!make_buffers(pp)) return D3DERR_INVALIDCALL;
    return D3D_OK;
}

static HRESULT dev_Present(void *This, const RECT *sr, const RECT *dr, HWND w, const RGNDATA *rg)
{
    (void)This; (void)sr; (void)dr; (void)w; (void)rg;
    end_pass();
    new_cb_check();
    barrier_to(B.cb, 1);                    /* (the back buffer blitted to the screen) */
    vk_present(sc_frame == 1 ? VK_NULL_HANDLE : backbuf->tex->image, backbuf->tex->w, backbuf->tex->h);
    sc_frame = 0;
    did(XFER_STAGES, 0);
    B.cb = VK_NULL_HANDLE;
    options_read();
    pipes_save_maybe();
    return D3D_OK;
}

/* ------------------------------------------------------------------ the device: state setters */
static void check_thread(void)
{
    if (__builtin_expect(pthread_self() != creator_thread, 0) && once_("thread"))
        LOG(LOG_ALWAYS, "vulkan renderer: a device call from another thread (%lx; the device's %lx) [not safe yet]",
            (unsigned long)pthread_self(), creator_thread);
}

static HRESULT dev_SetRenderState(void *This, D3DRENDERSTATETYPE st, DWORD v)
{
    (void)This;
    if ((unsigned)st >= 256) return D3DERR_INVALIDCALL;
    if (S.rs[st] == v) return D3D_OK;
    S.rs[st] = v;
    switch (st) {
    case D3DRS_FOGSTART: case D3DRS_FOGEND: case D3DRS_FOGDENSITY:
        dirty_ffv = 1;
        /* fall through */
    case D3DRS_FOGCOLOR: case D3DRS_ALPHAREF: case D3DRS_TEXTUREFACTOR:
        dirty_psx = 1;
        break;
    /* (fixed function: the key's states are looked at every draw, draw_prep; these are the values) */
    case D3DRS_AMBIENT: case D3DRS_TWEENFACTOR: case D3DRS_POINTSIZE: case D3DRS_POINTSIZE_MIN: case D3DRS_POINTSIZE_MAX:
    case D3DRS_POINTSCALE_A: case D3DRS_POINTSCALE_B: case D3DRS_POINTSCALE_C:
        dirty_ffv = 1;
        break;
    case D3DRS_SHADEMODE:
        if (v == D3DSHADE_FLAT && once_("flat shading"))
            LOG(LOG_ALWAYS, "vulkan renderer: flat shading is not done (colours interpolated)");
        break;
    case D3DRS_FILLMODE: case D3DRS_ALPHABLENDENABLE: case D3DRS_SRCBLEND: case D3DRS_DESTBLEND: case D3DRS_BLENDOP:
    case D3DRS_SEPARATEALPHABLENDENABLE: case D3DRS_SRCBLENDALPHA: case D3DRS_DESTBLENDALPHA: case D3DRS_BLENDOPALPHA:
    case D3DRS_COLORWRITEENABLE: case D3DRS_MULTISAMPLEANTIALIAS: case D3DRS_ALPHATESTENABLE: case D3DRS_ALPHAFUNC:
    case D3DRS_FOGENABLE: case D3DRS_FOGTABLEMODE:
        dirty_pipe = 1;
        break;
    case D3DRS_SCISSORTESTENABLE:
        dirty_dyn |= DYN_VP;
        break;
    case D3DRS_CULLMODE:
        dirty_dyn |= DYN_CULL;
        break;
    case D3DRS_ZENABLE:
        dirty_ffv = 1;                      /* (pre-transformed vertices' depth: with the depth test only) */
        /* fall through */
    case D3DRS_ZWRITEENABLE: case D3DRS_ZFUNC: case D3DRS_DEPTHBIAS: case D3DRS_SLOPESCALEDEPTHBIAS:
        dirty_dyn |= DYN_DEPTH;
        break;
    case D3DRS_STENCILENABLE: case D3DRS_STENCILFAIL: case D3DRS_STENCILZFAIL: case D3DRS_STENCILPASS: case D3DRS_STENCILFUNC:
    case D3DRS_STENCILREF: case D3DRS_STENCILMASK: case D3DRS_STENCILWRITEMASK: case D3DRS_TWOSIDEDSTENCILMODE:
    case D3DRS_CCW_STENCILFAIL: case D3DRS_CCW_STENCILZFAIL: case D3DRS_CCW_STENCILPASS: case D3DRS_CCW_STENCILFUNC:
        dirty_dyn |= DYN_STENCIL;
        break;
    case D3DRS_BLENDFACTOR:
        dirty_dyn |= DYN_BLEND;
        break;
    default:                                /* (the rest: fixed function, not read with shaders) */
        break;
    }
    return D3D_OK;
}

static HRESULT dev_GetRenderState(void *This, D3DRENDERSTATETYPE st, DWORD *v)
{
    (void)This;
    if ((unsigned)st >= 256 || !v) return D3DERR_INVALIDCALL;
    *v = S.rs[st];
    return D3D_OK;
}

static int samp_index(DWORD s)
{
    if (s < 16) return (int)s;
    if (s == D3DDMAPSAMPLER) return 16;
    if (s >= D3DVERTEXTEXTURESAMPLER0 && s <= D3DVERTEXTEXTURESAMPLER3) return 17 + (int)(s - D3DVERTEXTEXTURESAMPLER0);
    return -1;
}

static HRESULT dev_SetSamplerState(void *This, DWORD s, D3DSAMPLERSTATETYPE t, DWORD v)
{
    (void)This;
    int i = samp_index(s);
    if (i < 0 || (unsigned)t >= 14) return D3DERR_INVALIDCALL;
    if (S.ss[i][t] == v) return D3D_OK;
    S.ss[i][t] = v;
    dirty_samp |= 1u << i;
    return D3D_OK;
}

static HRESULT dev_GetSamplerState(void *This, DWORD s, D3DSAMPLERSTATETYPE t, DWORD *v)
{
    (void)This;
    int i = samp_index(s);
    if (i < 0 || (unsigned)t >= 14 || !v) return D3DERR_INVALIDCALL;
    *v = S.ss[i][t];
    return D3D_OK;
}

static HRESULT dev_SetTextureStageState(void *This, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD v)
{
    (void)This;
    if (stage >= 8 || (unsigned)t >= 33) return D3DERR_INVALIDCALL;
    if (S.tss[stage][t] == v) return D3D_OK;
    S.tss[stage][t] = v;
    if ((t >= D3DTSS_BUMPENVMAT00 && t <= D3DTSS_BUMPENVMAT11) || t == D3DTSS_BUMPENVLSCALE || t == D3DTSS_BUMPENVLOFFSET ||
        t == D3DTSS_CONSTANT)
        dirty_psx = 1;
    else if (t == D3DTSS_TEXTURETRANSFORMFLAGS) dirty_pipe = 1;
    return D3D_OK;
}

static HRESULT dev_GetTextureStageState(void *This, DWORD stage, D3DTEXTURESTAGESTATETYPE t, DWORD *v)
{
    (void)This;
    if (stage >= 8 || (unsigned)t >= 33 || !v) return D3DERR_INVALIDCALL;
    *v = S.tss[stage][t];
    return D3D_OK;
}

static HRESULT dev_SetTexture(void *This, DWORD stage, IDirect3DBaseTexture9 *tp)
{
    (void)This;
    int i = samp_index(stage);
    if (i < 0) return D3DERR_INVALIDCALL;
    VkTex *t = (VkTex *)tp, *old = S.tex[i];
    if (old == t) return D3D_OK;
    S.tex[i] = t;
    unbound(old);
    return D3D_OK;
}

static HRESULT dev_GetTexture(void *This, DWORD stage, IDirect3DBaseTexture9 **out)
{
    (void)This;
    int i = samp_index(stage);
    if (i < 0 || !out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DBaseTexture9 *)S.tex[i];
    if (S.tex[i]) com_AddRef(S.tex[i]);
    return D3D_OK;
}

static HRESULT dev_SetStreamSource(void *This, UINT s, IDirect3DVertexBuffer9 *vb, UINT off, UINT stride)
{
    (void)This;
    if (s >= 16) return D3DERR_INVALIDCALL;
    VkBuf *b = (VkBuf *)vb, *old = S.vb[s];
    if (old != b) {
        S.vb[s] = b;
        unbound(old);
    }
    S.vb_off[s] = off;
    S.vb_stride[s] = stride;
    return D3D_OK;
}

static HRESULT dev_GetStreamSource(void *This, UINT s, IDirect3DVertexBuffer9 **vb, UINT *off, UINT *stride)
{
    (void)This;
    if (s >= 16 || !vb || !off || !stride) return D3DERR_INVALIDCALL;
    *vb = (IDirect3DVertexBuffer9 *)S.vb[s];
    if (S.vb[s]) com_AddRef(S.vb[s]);
    *off = S.vb_off[s];
    *stride = S.vb_stride[s];
    return D3D_OK;
}

static HRESULT dev_SetStreamSourceFreq(void *This, UINT s, UINT d)
{
    (void)This;
    if (s >= 16) return D3DERR_INVALIDCALL;
    S.freq[s] = d;
    if (d != 1 && once_("instancing")) LOG(LOG_ALWAYS, "vulkan renderer: stream frequencies (instancing) are not done");
    return D3D_OK;
}

static HRESULT dev_GetStreamSourceFreq(void *This, UINT s, UINT *d)
{
    (void)This;
    if (s >= 16 || !d) return D3DERR_INVALIDCALL;
    *d = S.freq[s];
    return D3D_OK;
}

static HRESULT dev_SetIndices(void *This, IDirect3DIndexBuffer9 *ibp)
{
    (void)This;
    VkBuf *b = (VkBuf *)ibp, *old = S.ib;
    if (old == b) return D3D_OK;
    S.ib = b;
    unbound(old);
    return D3D_OK;
}

static HRESULT dev_GetIndices(void *This, IDirect3DIndexBuffer9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DIndexBuffer9 *)S.ib;
    if (S.ib) com_AddRef(S.ib);
    return D3D_OK;
}

static HRESULT dev_SetVertexDeclaration(void *This, IDirect3DVertexDeclaration9 *d)
{
    (void)This;
    VkDecl *n = (VkDecl *)d, *old = S.decl;
    S.fvf = n ? n->fvf : 0;
    if (old == n) return D3D_OK;
    S.decl = n;
    dirty_pipe = 1;
    unbound(old);
    return D3D_OK;
}

static HRESULT dev_GetVertexDeclaration(void *This, IDirect3DVertexDeclaration9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DVertexDeclaration9 *)S.decl;
    if (S.decl) com_AddRef(S.decl);
    return D3D_OK;
}

/* an FVF is a declaration of its own (fvf_decl, kept by the device); a declaration set clears the FVF */
static HRESULT dev_SetFVF(void *This, DWORD fvf)
{
    (void)This;
    if (!fvf) return D3D_OK;
    VkDecl *d = fvf_decl(fvf), *old = S.decl;
    S.fvf = fvf;
    if (old == d) return D3D_OK;
    S.decl = d;
    dirty_pipe = 1;
    unbound(old);
    return D3D_OK;
}

static HRESULT dev_GetFVF(void *This, DWORD *fvf) { (void)This; if (!fvf) return D3DERR_INVALIDCALL; *fvf = S.fvf; return D3D_OK; }

static HRESULT set_shader(VkShader **slot, VkShader *s)
{
    VkShader *old = *slot;
    if (old == s) return D3D_OK;
    *slot = s;
    dirty_pipe = 1;                         /* (the constants follow the shader's id: draw_prep) */
    unbound(old);
    return D3D_OK;
}

static HRESULT dev_SetVertexShader(void *This, IDirect3DVertexShader9 *s) { (void)This; return set_shader(&S.vs, (VkShader *)s); }
static HRESULT dev_SetPixelShader(void *This, IDirect3DPixelShader9 *s) { (void)This; return set_shader(&S.ps, (VkShader *)s); }
static HRESULT dev_GetVertexShader(void *This, IDirect3DVertexShader9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DVertexShader9 *)S.vs;
    if (S.vs) com_AddRef(S.vs);
    return D3D_OK;
}
static HRESULT dev_GetPixelShader(void *This, IDirect3DPixelShader9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DPixelShader9 *)S.ps;
    if (S.ps) com_AddRef(S.ps);
    return D3D_OK;
}

static HRESULT dev_SetVertexShaderConstantF(void *This, UINT r, const float *d, UINT n)
{
    (void)This;
    if (!d || r >= VK_VS_CONSTS) return D3DERR_INVALIDCALL;
    if (r + n > VK_VS_CONSTS) n = VK_VS_CONSTS - r;
    if (!n || !memcmp(S.vsc[r], d, n * 16)) return D3D_OK;  /* (the same values again: nothing to upload) */
    memcpy(S.vsc[r], d, n * 16);
    uint32_t k0 = r / VK_VS_CHUNK, k1 = (r + n - 1) / VK_VS_CHUNK;
    vs_dirty |= ((2u << k1) - 1) & ~((1u << k0) - 1);
    return D3D_OK;
}
static HRESULT dev_GetVertexShaderConstantF(void *This, UINT r, float *d, UINT n)
{
    (void)This;
    if (!d || r + n > VK_VS_CONSTS) return D3DERR_INVALIDCALL;
    memcpy(d, S.vsc[r], n * 16);
    return D3D_OK;
}
static HRESULT dev_SetVertexShaderConstantI(void *This, UINT r, const int *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(S.vsi[r], d, n * 16);
    return D3D_OK;
}
static HRESULT dev_GetVertexShaderConstantI(void *This, UINT r, int *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d, S.vsi[r], n * 16);
    return D3D_OK;
}
static HRESULT dev_SetVertexShaderConstantB(void *This, UINT r, const WINBOOL *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(&S.vsb[r], d, n * sizeof(BOOL));
    return D3D_OK;
}
static HRESULT dev_GetVertexShaderConstantB(void *This, UINT r, WINBOOL *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d, &S.vsb[r], n * sizeof(BOOL));
    return D3D_OK;
}
static HRESULT dev_SetPixelShaderConstantF(void *This, UINT r, const float *d, UINT n)
{
    (void)This;
    if (!d || r >= VK_PS_CONSTS) return D3DERR_INVALIDCALL;
    if (r + n > VK_PS_CONSTS) n = VK_PS_CONSTS - r;
    if (!memcmp(S.psc[r], d, n * 16)) return D3D_OK;
    memcpy(S.psc[r], d, n * 16);
    if (r < psc_lo) psc_lo = r;
    if (r + n > psc_hi) psc_hi = r + n;
    return D3D_OK;
}
static HRESULT dev_GetPixelShaderConstantF(void *This, UINT r, float *d, UINT n)
{
    (void)This;
    if (!d || r + n > VK_PS_CONSTS) return D3DERR_INVALIDCALL;
    memcpy(d, S.psc[r], n * 16);
    return D3D_OK;
}
static HRESULT dev_SetPixelShaderConstantI(void *This, UINT r, const int *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(S.psi[r], d, n * 16);
    return D3D_OK;
}
static HRESULT dev_GetPixelShaderConstantI(void *This, UINT r, int *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d, S.psi[r], n * 16);
    return D3D_OK;
}
static HRESULT dev_SetPixelShaderConstantB(void *This, UINT r, const WINBOOL *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(&S.psb[r], d, n * sizeof(BOOL));
    return D3D_OK;
}
static HRESULT dev_GetPixelShaderConstantB(void *This, UINT r, WINBOOL *d, UINT n)
{
    (void)This;
    if (!d || r + n > 16) return D3DERR_INVALIDCALL;
    memcpy(d, &S.psb[r], n * sizeof(BOOL));
    return D3D_OK;
}

static HRESULT dev_SetViewport(void *This, const D3DVIEWPORT9 *vp)
{
    (void)This;
    if (!vp) return D3DERR_INVALIDCALL;
    S.vp = *vp;
    dirty_dyn |= DYN_VP;
    dirty_ffv = 1;
    return D3D_OK;
}
static HRESULT dev_GetViewport(void *This, D3DVIEWPORT9 *vp) { (void)This; if (!vp) return D3DERR_INVALIDCALL; *vp = S.vp; return D3D_OK; }
static HRESULT dev_SetScissorRect(void *This, const RECT *r)
{
    (void)This;
    if (!r) return D3DERR_INVALIDCALL;
    S.scissor = *r;
    dirty_dyn |= DYN_VP;
    return D3D_OK;
}
static HRESULT dev_GetScissorRect(void *This, RECT *r) { (void)This; if (!r) return D3DERR_INVALIDCALL; *r = S.scissor; return D3D_OK; }
static HRESULT dev_SetClipPlane(void *This, DWORD i, const float *p)
{
    (void)This;
    if (i >= 6 || !p) return D3DERR_INVALIDCALL;
    memcpy(S.clip[i], p, 16);
    return D3D_OK;
}
static HRESULT dev_GetClipPlane(void *This, DWORD i, float *p)
{
    (void)This;
    if (i >= 6 || !p) return D3DERR_INVALIDCALL;
    memcpy(p, S.clip[i], 16);
    return D3D_OK;
}
static HRESULT dev_SetTransform(void *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m)
{
    (void)This;
    if ((unsigned)t >= 512 || !m) return D3DERR_INVALIDCALL;
    xforms[t] = *m;
    if (t == D3DTS_VIEW || t == D3DTS_PROJECTION || t == D3DTS_WORLD || (t >= D3DTS_TEXTURE0 && t <= D3DTS_TEXTURE7))
        dirty_ffv = 1;
    else if (t > D3DTS_WORLD && once_("world matrices"))
        LOG(LOG_ALWAYS, "vulkan renderer: world matrix %u set (vertex blending is not done)", (unsigned)t);
    return D3D_OK;
}
static HRESULT dev_GetTransform(void *This, D3DTRANSFORMSTATETYPE t, D3DMATRIX *m)
{
    (void)This;
    if ((unsigned)t >= 512 || !m) return D3DERR_INVALIDCALL;
    *m = xforms[t];
    return D3D_OK;
}
static HRESULT dev_MultiplyTransform(void *This, D3DTRANSFORMSTATETYPE t, const D3DMATRIX *m) { (void)This; (void)t; (void)m; UNSUPPORTED("MultiplyTransform"); return D3D_OK; }

/* ------------------------------------------------------------------ the device: creation of objects */
static HRESULT dev_CreateTexture(void *This, UINT w, UINT h, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                 IDirect3DTexture9 **out, HANDLE *shared)
{
    (void)This; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkTex *t = tex_new(D3DRTYPE_TEXTURE, w, h, levels, usage, fmt, pool, 1);
    LOG(LOG_D3D, "vulkan renderer: texture %ux%u, %u levels, usage %x, format %u, pool %u -> %p", w, h, levels,
        (unsigned)usage, (unsigned)fmt, (unsigned)pool, (void *)t);
    *out = (IDirect3DTexture9 *)t;
    return t ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateCubeTexture(void *This, UINT edge, UINT levels, DWORD usage, D3DFORMAT fmt, D3DPOOL pool,
                                     IDirect3DCubeTexture9 **out, HANDLE *shared)
{
    (void)This; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkTex *t = tex_new(D3DRTYPE_CUBETEXTURE, edge, edge, levels, usage, fmt, pool, 1);
    *out = (IDirect3DCubeTexture9 *)t;
    return t ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateVolumeTexture(void *This, UINT w, UINT h, UINT d, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p,
                                       IDirect3DVolumeTexture9 **out, HANDLE *sh)
{
    (void)This; (void)w; (void)h; (void)d; (void)l; (void)u; (void)f; (void)p; (void)sh;
    if (out) *out = NULL;
    UNSUPPORTED("CreateVolumeTexture");
    return D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateRenderTarget(void *This, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q, WINBOOL lockable,
                                      IDirect3DSurface9 **out, HANDLE *shared)
{
    (void)This; (void)q; (void)lockable; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkSurf *s = surf_standalone(w, h, fmt, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET, ms > 1 ? (UINT)ms : 1);
    *out = (IDirect3DSurface9 *)s;
    return s ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateDepthStencilSurface(void *This, UINT w, UINT h, D3DFORMAT fmt, D3DMULTISAMPLE_TYPE ms, DWORD q,
                                             WINBOOL discard, IDirect3DSurface9 **out, HANDLE *shared)
{
    (void)This; (void)q; (void)discard; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkSurf *s = surf_standalone(w, h, fmt, D3DPOOL_DEFAULT, D3DUSAGE_DEPTHSTENCIL, ms > 1 ? (UINT)ms : 1);
    *out = (IDirect3DSurface9 *)s;
    return s ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateOffscreenPlainSurface(void *This, UINT w, UINT h, D3DFORMAT fmt, D3DPOOL pool, IDirect3DSurface9 **out,
                                               HANDLE *shared)
{
    (void)This; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkSurf *s = surf_standalone(w, h, fmt, pool, 0, 1);
    *out = (IDirect3DSurface9 *)s;
    return s ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateVertexBuffer(void *This, UINT len, DWORD usage, DWORD fvf, D3DPOOL pool, IDirect3DVertexBuffer9 **out,
                                      HANDLE *shared)
{
    (void)This; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkBuf *b = buf_new(len, usage, fvf, pool, D3DFMT_VERTEXDATA, 0);
    *out = (IDirect3DVertexBuffer9 *)b;
    return b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

static HRESULT dev_CreateIndexBuffer(void *This, UINT len, DWORD usage, D3DFORMAT fmt, D3DPOOL pool, IDirect3DIndexBuffer9 **out,
                                     HANDLE *shared)
{
    (void)This; (void)shared;
    if (!out) return D3DERR_INVALIDCALL;
    VkBuf *b = buf_new(len, usage, 0, pool, fmt, 1);
    *out = (IDirect3DIndexBuffer9 *)b;
    return b ? D3D_OK : D3DERR_OUTOFVIDEOMEMORY;
}

static HRESULT dev_CreateVertexDeclaration(void *This, const D3DVERTEXELEMENT9 *el, IDirect3DVertexDeclaration9 **out)
{
    (void)This;
    if (!el || !out) return D3DERR_INVALIDCALL;
    VkDecl *d = calloc(1, sizeof *d);
    obj_init(&d->o, &vt_decl, DK_IDirect3DVertexDeclaration9);
    while (d->nel < 31 && el[d->nel].Stream != 0xff) {
        d->el[d->nel] = el[d->nel];
        d->nel++;
    }
    d->el[d->nel] = (D3DVERTEXELEMENT9)D3DDECL_END();
    d->id = next_id++;
    decl_classify(d);
    d->hash = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)d->el;
    for (size_t i = 0; i < sizeof *d->el * (size_t)d->nel; i++) d->hash = (d->hash ^ b[i]) * 0x100000001b3ull;
    *out = (IDirect3DVertexDeclaration9 *)d;
    return D3D_OK;
}

static HRESULT dev_CreateVertexShader(void *This, const DWORD *code, IDirect3DVertexShader9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DVertexShader9 *)shader_new(code, 0);
    return *out ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreatePixelShader(void *This, const DWORD *code, IDirect3DPixelShader9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    *out = (IDirect3DPixelShader9 *)shader_new(code, 1);
    return *out ? D3D_OK : D3DERR_INVALIDCALL;
}

static HRESULT dev_CreateStateBlock(void *This, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out)
{
    (void)This; (void)type;
    if (!out) return D3DERR_INVALIDCALL;
    VkSB *b = calloc(1, sizeof *b);
    obj_init(&b->o, &vt_sb, DK_IDirect3DStateBlock9);
    b->st = S;
    st_refs(&b->st, 1);
    *out = (IDirect3DStateBlock9 *)b;
    return D3D_OK;
}

/* TIMESTAMP and TIMESTAMPFREQ queries (d3d9_gputime.c, the passes' timers; VULKAN.md "GPU timing"):
   each a slot of the timestamp pool, written in order with the drawing at Issue (reset on the host
   first), read without waiting at GetData (S_FALSE until its work has run) */

static HRESULT dev_CreateQuery(void *This, D3DQUERYTYPE type, IDirect3DQuery9 **out)
{
    (void)This;
    if (!R.ttimes || (type != D3DQUERYTYPE_TIMESTAMP && type != D3DQUERYTYPE_TIMESTAMPFREQ)) {
        if (out) *out = NULL;
        return D3DERR_NOTAVAILABLE;
    }
    if (!out) return D3D_OK;                 /* (asking whether the type is there) */
    int slot = -1;
    if (type == D3DQUERYTYPE_TIMESTAMP) {
        if (nqfree) slot = qfree[--nqfree];
        else if (qslots_used < 4096) slot = qslots_used++;
        else { *out = NULL; return D3DERR_NOTAVAILABLE; }
    }
    VkQuery *q = calloc(1, sizeof *q);
    obj_init(&q->o, &vt_q, DK_IDirect3DQuery9);
    q->type = type;
    q->slot = slot;
    *out = (IDirect3DQuery9 *)q;
    return D3D_OK;
}

static D3DQUERYTYPE q_GetType(void *This) { return ((VkQuery *)This)->type; }
static DWORD q_GetDataSize(void *This) { (void)This; return sizeof(UINT64); }
static HRESULT q_Issue(void *This, DWORD flags)
{
    VkQuery *q = This;
    if (!(flags & D3DISSUE_END) || q->slot < 0) return D3D_OK;
    vkResetQueryPool(R.dev, R.tpool, (uint32_t)q->slot, 1);
    new_cb_check();
    vkCmdWriteTimestamp2(B.cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, R.tpool, (uint32_t)q->slot);
    q->issued = 1;
    return D3D_OK;
}
static HRESULT q_GetData(void *This, void *data, DWORD size, DWORD flags)
{
    (void)flags;
    VkQuery *q = This;
    UINT64 v = 0;
    if (q->type == D3DQUERYTYPE_TIMESTAMPFREQ) {
        v = (UINT64)(1e9 / (double)R.props.limits.timestampPeriod);
    } else {
        if (!q->issued) return S_FALSE;
        if (vkGetQueryPoolResults(R.dev, R.tpool, (uint32_t)q->slot, 1, sizeof v, &v, sizeof v, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return S_FALSE;
    }
    if (data && size >= sizeof v) memcpy(data, &v, sizeof v);
    return S_OK;
}

/* ------------------------------------------------------------------ the device: the rest */
static HRESULT dev_TestCooperativeLevel(void *This) { (void)This; return D3D_OK; }
static UINT dev_GetAvailableTextureMem(void *This) { (void)This; return 2047u << 20; }
static HRESULT dev_EvictManagedResources(void *This) { (void)This; return D3D_OK; }
static HRESULT dev_GetDirect3D(void *This, IDirect3D9 **out)
{
    (void)This;
    if (!out) return D3DERR_INVALIDCALL;
    com_AddRef(&d3d9_obj);
    *out = (IDirect3D9 *)&d3d9_obj;
    return D3D_OK;
}
static HRESULT d3d9_GetDeviceCaps(void *This, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps);
static HRESULT dev_GetDeviceCaps(void *This, D3DCAPS9 *caps) { (void)This; return d3d9_GetDeviceCaps(&d3d9_obj, 0, D3DDEVTYPE_HAL, caps); }
static HRESULT dev_GetDisplayMode(void *This, UINT sc, D3DDISPLAYMODE *m)
{
    (void)This; (void)sc;
    if (!m) return D3DERR_INVALIDCALL;
    m->Width = cur_pp.BackBufferWidth;
    m->Height = cur_pp.BackBufferHeight;
    m->RefreshRate = 60;
    m->Format = D3DFMT_X8R8G8B8;
    return D3D_OK;
}
static HRESULT dev_GetCreationParameters(void *This, D3DDEVICE_CREATION_PARAMETERS *p)
{
    (void)This;
    if (!p) return D3DERR_INVALIDCALL;
    p->AdapterOrdinal = 0;
    p->DeviceType = D3DDEVTYPE_HAL;
    p->hFocusWindow = NULL;
    p->BehaviorFlags = D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE;
    return D3D_OK;
}
static HRESULT dev_SetCursorProperties(void *This, UINT x, UINT y, IDirect3DSurface9 *s) { (void)This; (void)x; (void)y; (void)s; return D3D_OK; }
static void dev_SetCursorPosition(void *This, int x, int y, DWORD f) { (void)This; (void)x; (void)y; (void)f; }
static WINBOOL dev_ShowCursor(void *This, WINBOOL b) { (void)This; (void)b; return FALSE; }
static HRESULT dev_CreateAdditionalSwapChain(void *This, D3DPRESENT_PARAMETERS *pp, IDirect3DSwapChain9 **out)
{
    (void)This; (void)pp;
    if (out) *out = NULL;
    UNSUPPORTED("CreateAdditionalSwapChain");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_GetSwapChain(void *This, UINT i, IDirect3DSwapChain9 **out)
{
    (void)This; (void)i;
    if (out) *out = NULL;
    UNSUPPORTED("GetSwapChain");
    return D3DERR_INVALIDCALL;
}
static UINT dev_GetNumberOfSwapChains(void *This) { (void)This; return 1; }
static HRESULT dev_GetRasterStatus(void *This, UINT sc, D3DRASTER_STATUS *rs)
{
    (void)This; (void)sc;
    if (!rs) return D3DERR_INVALIDCALL;
    rs->InVBlank = TRUE;
    rs->ScanLine = 0;
    return D3D_OK;
}
static HRESULT dev_SetDialogBoxMode(void *This, WINBOOL b) { (void)This; (void)b; return D3D_OK; }
static void dev_SetGammaRamp(void *This, UINT sc, DWORD f, const D3DGAMMARAMP *r) { (void)This; (void)sc; (void)f; (void)r; }
static void dev_GetGammaRamp(void *This, UINT sc, D3DGAMMARAMP *r)
{
    (void)This; (void)sc;
    if (r) for (int i = 0; i < 256; i++) r->red[i] = r->green[i] = r->blue[i] = (WORD)(i * 257);
}
static HRESULT dev_UpdateSurface(void *This, IDirect3DSurface9 *s, const RECT *sr, IDirect3DSurface9 *d, const POINT *dp)
{
    (void)This; (void)s; (void)sr; (void)d; (void)dp;
    UNSUPPORTED("UpdateSurface");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_UpdateTexture(void *This, IDirect3DBaseTexture9 *s, IDirect3DBaseTexture9 *d)
{
    (void)This; (void)s; (void)d;
    UNSUPPORTED("UpdateTexture");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_GetFrontBufferData(void *This, UINT sc, IDirect3DSurface9 *d) { (void)This; (void)sc; (void)d; return D3DERR_INVALIDCALL; }
static HRESULT dev_ColorFill(void *This, IDirect3DSurface9 *s, const RECT *r, D3DCOLOR c)
{
    (void)This; (void)s; (void)r; (void)c;
    UNSUPPORTED("ColorFill");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_BeginScene(void *This) { (void)This; check_thread(); return D3D_OK; }
static HRESULT dev_EndScene(void *This) { (void)This; return D3D_OK; }
/* fixed function: the material and the lights (VULKAN.md 2.3) */
static HRESULT dev_SetMaterial(void *This, const D3DMATERIAL9 *m)
{
    (void)This;
    if (!m) return D3DERR_INVALIDCALL;
    S.mat = *m;
    dirty_ffv = 1;
    return D3D_OK;
}
static HRESULT dev_GetMaterial(void *This, D3DMATERIAL9 *m) { (void)This; if (!m) return D3DERR_INVALIDCALL; *m = S.mat; return D3D_OK; }
static HRESULT dev_SetLight(void *This, DWORD i, const D3DLIGHT9 *l)
{
    (void)This;
    if (!l) return D3DERR_INVALIDCALL;
    if (i >= VK_LIGHTS) {
        if (once_("light index")) LOG(LOG_ALWAYS, "vulkan renderer: light %u is past the %d kept: ignored", (unsigned)i, VK_LIGHTS);
        return D3D_OK;
    }
    S.light[i] = *l;
    S.light_set[i] = 1;
    dirty_ffv = 1;
    return D3D_OK;
}
static HRESULT dev_GetLight(void *This, DWORD i, D3DLIGHT9 *l)
{
    (void)This;
    if (!l || i >= VK_LIGHTS || !S.light_set[i]) return D3DERR_INVALIDCALL;
    *l = S.light[i];
    return D3D_OK;
}
static HRESULT dev_LightEnable(void *This, DWORD i, WINBOOL e)
{
    (void)This;
    if (i >= VK_LIGHTS) return D3D_OK;
    S.light_set[i] = 1;                     /* (enabling an unset light sets the default one, as Direct3D) */
    S.light_on[i] = e != 0;
    dirty_ffv = 1;
    return D3D_OK;
}
static HRESULT dev_GetLightEnable(void *This, DWORD i, WINBOOL *e)
{
    (void)This;
    if (!e || i >= VK_LIGHTS || !S.light_set[i]) return D3DERR_INVALIDCALL;
    *e = S.light_on[i] ? 128 : 0;           /* (Direct3D 9 answers 128 for enabled, as DXVK) */
    return D3D_OK;
}
/* BeginStateBlock / EndStateBlock: the game records one at the start and never applies it (VULKAN.md 1:
   no Apply on the route); the block holds the whole state as at EndStateBlock [Direct3D's holds only
   the states set in between: enough while nothing applies it] */
static int recording;
static HRESULT dev_CreateStateBlock(void *This, D3DSTATEBLOCKTYPE type, IDirect3DStateBlock9 **out);
static HRESULT dev_BeginStateBlock(void *This)
{
    (void)This;
    if (recording) return D3DERR_INVALIDCALL;
    recording = 1;
    return D3D_OK;
}
static HRESULT dev_EndStateBlock(void *This, IDirect3DStateBlock9 **out)
{
    if (!out || !recording) return D3DERR_INVALIDCALL;
    recording = 0;
    if (once_("recorded state block"))
        LOG(LOG_ALWAYS, "vulkan renderer: a recorded state block (BeginStateBlock / EndStateBlock) holds the whole state");
    return dev_CreateStateBlock(This, D3DSBT_ALL, out);
}
static HRESULT dev_SetClipStatus(void *This, const D3DCLIPSTATUS9 *c) { (void)This; (void)c; return D3D_OK; }
static HRESULT dev_GetClipStatus(void *This, D3DCLIPSTATUS9 *c) { (void)This; if (c) memset(c, 0, sizeof *c); return D3D_OK; }
static HRESULT dev_ValidateDevice(void *This, DWORD *n) { (void)This; if (n) *n = 1; return D3D_OK; }
static HRESULT dev_SetPaletteEntries(void *This, UINT p, const PALETTEENTRY *e) { (void)This; (void)p; (void)e; return D3D_OK; }
static HRESULT dev_GetPaletteEntries(void *This, UINT p, PALETTEENTRY *e) { (void)This; (void)p; (void)e; return D3DERR_INVALIDCALL; }
static HRESULT dev_SetCurrentTexturePalette(void *This, UINT p) { (void)This; (void)p; return D3D_OK; }
static HRESULT dev_GetCurrentTexturePalette(void *This, UINT *p) { (void)This; if (p) *p = 0; return D3D_OK; }
static HRESULT dev_SetSoftwareVertexProcessing(void *This, WINBOOL b) { (void)This; (void)b; return D3D_OK; }
static WINBOOL dev_GetSoftwareVertexProcessing(void *This) { (void)This; return FALSE; }
static HRESULT dev_SetNPatchMode(void *This, float n) { (void)This; (void)n; return D3D_OK; }
static float dev_GetNPatchMode(void *This) { (void)This; return 0; }
static HRESULT dev_ProcessVertices(void *This, UINT a, UINT b, UINT c, IDirect3DVertexBuffer9 *d, IDirect3DVertexDeclaration9 *e, DWORD f)
{
    (void)This; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
    UNSUPPORTED("ProcessVertices");
    return D3DERR_INVALIDCALL;
}
static HRESULT dev_DrawRectPatch(void *This, UINT h, const float *s, const D3DRECTPATCH_INFO *i) { (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT dev_DrawTriPatch(void *This, UINT h, const float *s, const D3DTRIPATCH_INFO *i) { (void)This; (void)h; (void)s; (void)i; return D3DERR_INVALIDCALL; }
static HRESULT dev_DeletePatch(void *This, UINT h) { (void)This; (void)h; return D3DERR_INVALIDCALL; }

/* ------------------------------------------------------------------ IDirect3D9 */
/* DXVK 3.1.1's D3DCAPS9 on the Win Max (ETM_DUMP_CAPS, work/inv/caps_winmax.hex): its values do
   not depend on the GPU but for MaxPointSize, set from this device's limits */
static const char caps_hex[] =
    "01000000000000000000020000000260a00300000f00008001000000f0be1b00f2cf3f009121730fff000000ff3f0000ff3f0000"
    "ff00000008420800c5ec01000007030700070307000703073f0000003f0000003f00000000400000004000000020000000200000"
    "0020000010000000f9021550000000c7000000c7000000470000004700000000ff01000008001000ffffff030800000008000000"
    "7b0100000800000006000000040000000000000000ffff4555555500ffffff0010000000fc0100000003feff000100000003ffff"
    "ffff7f7f510000000000000000000000000000000000000001000000ff0300000400000000030003010000001800000020000000"
    "040000001f0000001800000020000000040000000002000000030003ffffffffffffffff0080000000800000";
static D3DCAPS9 caps_dxvk;
static int caps_ready;

static void caps_load(void)
{
    if (caps_ready) return;
    caps_ready = 1;
    uint8_t *b = (uint8_t *)&caps_dxvk;
    for (size_t i = 0; i < sizeof caps_dxvk && caps_hex[2 * i] && caps_hex[2 * i + 1]; i++) {
        char t[3] = {caps_hex[2 * i], caps_hex[2 * i + 1], 0};
        b[i] = (uint8_t)strtoul(t, NULL, 16);
    }
    caps_dxvk.MaxPointSize = R.props.limits.pointSizeRange[1];
}

static HRESULT d3d9_RegisterSoftwareDevice(void *This, void *init) { (void)This; (void)init; return D3DERR_INVALIDCALL; }
static UINT d3d9_GetAdapterCount(void *This) { (void)This; return 1; }
static HRESULT d3d9_GetAdapterIdentifier(void *This, UINT a, DWORD flags, D3DADAPTER_IDENTIFIER9 *id)
{
    (void)This; (void)flags;
    if (a || !id) return D3DERR_INVALIDCALL;
    /* (only up to WHQLLevel: the program's structure is 1100 bytes, the host's padded to 1104; the
       padding written cost the program its return address) */
    memset(id, 0, offsetof(D3DADAPTER_IDENTIFIER9, WHQLLevel) + sizeof id->WHQLLevel);
    uint32_t vendor = R.props.vendorID;
    snprintf(id->Driver, sizeof id->Driver, "%s", vendor == 0x10de ? "nvd3dum.dll" : vendor == 0x8086 ? "igdumd64.dll" : "aticfx64.dll");
    snprintf(id->Description, sizeof id->Description, "%s", R.props.deviceName);
    snprintf(id->DeviceName, sizeof id->DeviceName, "\\\\.\\DISPLAY%u", (unsigned)SDL_GetPrimaryDisplay());
    id->VendorId = vendor;
    id->DeviceId = R.props.deviceID;
    memcpy(&id->DeviceIdentifier, R.uuid, 16);
    id->WHQLLevel = 0;
    id->DriverVersion.QuadPart = INT64_MAX;
    return D3D_OK;
}

static int desktop_mode(D3DDISPLAYMODE *m)
{
    const SDL_DisplayMode *dm = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay());
    m->Width = dm ? (UINT)dm->w : 1920;
    m->Height = dm ? (UINT)dm->h : 1080;
    m->RefreshRate = dm && dm->refresh_rate > 0 ? (UINT)(dm->refresh_rate + 0.5f) : 60;
    m->Format = D3DFMT_X8R8G8B8;
    return 1;
}

/* the display's modes as DXVK 3.1.1 lists them (d3d9_adapter.cpp CacheModes, its SDL3 WSI), so the
   game sees the same list under both renderers (its memory and choices the same): the first display
   SDL lists, modes of the format's bits per pixel (X8R8G8B8 32, R5G6B5 16: none on today's screens),
   the refresh rate truncated to whole hertz, exact duplicates dropped (SDL can list a mode twice:
   VULKAN.md 1), sorted by width, height, then refresh rate highest first */
static int modes_of(D3DFORMAT f, D3DDISPLAYMODE *out, int max)
{
    int bpp = f == D3DFMT_X8R8G8B8 || f == D3DFMT_A8R8G8B8 ? 32 : f == D3DFMT_R5G6B5 || f == D3DFMT_X1R5G5B5 ? 16 :
              f == D3DFMT_A2R10G10B10 ? 32 : 0;
    if (!bpp) return 0;
    int nd = 0, n = 0, k = 0;
    SDL_DisplayID *ds = SDL_GetDisplays(&nd);
    SDL_DisplayMode **m = ds && nd > 0 ? SDL_GetFullscreenDisplayModes(ds[0], &n) : NULL;
    SDL_free(ds);
    for (int i = 0; m && i < n && k < max; i++) {
        int b = SDL_BITSPERPIXEL(m[i]->format);
        b = b <= 8 ? 8 : b <= 16 ? 16 : 32;     /* (DXVK: a power of two, as Windows counts padding) */
        if (b != bpp) continue;
        UINT rr = m[i]->refresh_rate_numerator ? (UINT)m[i]->refresh_rate_numerator / (UINT)m[i]->refresh_rate_denominator :
                  m[i]->refresh_rate > 0 ? (UINT)(m[i]->refresh_rate * 1000.0f) / 1000 : 60;
        D3DDISPLAYMODE d = {(UINT)m[i]->w, (UINT)m[i]->h, rr, f};
        int dup = 0;
        for (int j = 0; j < k; j++) dup |= !memcmp(&out[j], &d, sizeof d);
        if (!dup) out[k++] = d;
    }
    SDL_free(m);
    for (int i = 1; i < k; i++)                 /* (insertion sort: a few dozen) */
        for (int j = i; j > 0; j--) {
            D3DDISPLAYMODE *a = &out[j - 1], *b = &out[j];
            int after = a->Width > b->Width || (a->Width == b->Width && (a->Height > b->Height ||
                        (a->Height == b->Height && a->RefreshRate < b->RefreshRate)));
            if (!after) break;
            D3DDISPLAYMODE t = *a; *a = *b; *b = t;
        }
    return k;
}

static UINT d3d9_GetAdapterModeCount(void *This, UINT a, D3DFORMAT f)
{
    (void)This;
    if (a) return 0;
    D3DDISPLAYMODE m[256];
    return (UINT)modes_of(f, m, 256);
}

static HRESULT d3d9_EnumAdapterModes(void *This, UINT a, D3DFORMAT f, UINT i, D3DDISPLAYMODE *out)
{
    (void)This;
    if (a || !out) return D3DERR_INVALIDCALL;
    D3DDISPLAYMODE m[256];
    int n = modes_of(f, m, 256);
    if ((int)i >= n) return D3DERR_INVALIDCALL;
    *out = m[i];
    return D3D_OK;
}

static HRESULT d3d9_GetAdapterDisplayMode(void *This, UINT a, D3DDISPLAYMODE *m)
{
    (void)This;
    if (a || !m) return D3DERR_INVALIDCALL;
    desktop_mode(m);
    return D3D_OK;
}

static HRESULT d3d9_CheckDeviceType(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT bf, WINBOOL w)
{
    (void)This; (void)t; (void)bf; (void)w;
    if (a) return D3DERR_INVALIDCALL;
    return af == D3DFMT_X8R8G8B8 || af == D3DFMT_R5G6B5 ? D3D_OK : D3DERR_NOTAVAILABLE;
}

/* as DXVK 3.1.1 answers (d3d9_adapter.cpp CheckDeviceFormat; both renderers' answers compared with
   ETM_DUMP_CAPS, VULKAN.md 1): D32 and D24X4S4 never (DXVK maps them nowhere: given here, the game took
   D32 for its depth buffer where DXVK's gave D24X8), a depth format as a plain surface only lockable,
   a depth format as a render target no, a colour format as a depth buffer no, else yes when the format
   is one we have */
static HRESULT d3d9_CheckDeviceFormat(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, DWORD usage, D3DRESOURCETYPE rt, D3DFORMAT f)
{
    (void)This; (void)t; (void)af;
    if (a) return D3DERR_INVALIDCALL;
    if (rt == D3DRTYPE_VERTEXBUFFER || rt == D3DRTYPE_INDEXBUFFER) return D3DERR_INVALIDCALL;
    if (f == D3DFMT_D32 || f == D3DFMT_D24X4S4) return D3DERR_NOTAVAILABLE;
    const Fmt *x = fmt_of(f);
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && (!x || !x->depth)) return D3DERR_NOTAVAILABLE;
    if (x && x->depth && rt == D3DRTYPE_SURFACE && !(usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) &&
        f != D3DFMT_D16_LOCKABLE && f != D3DFMT_D32F_LOCKABLE)
        return D3DERR_NOTAVAILABLE;
    if (!x || x->vk == VK_FORMAT_UNDEFINED) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_RENDERTARGET) && (x->depth || x->block > 1)) return D3DERR_NOTAVAILABLE;
    if ((usage & D3DUSAGE_DEPTHSTENCIL) && !x->depth) return D3DERR_NOTAVAILABLE;
    return D3D_OK;
}

static HRESULT d3d9_CheckDeviceMultiSampleType(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT f, WINBOOL w, D3DMULTISAMPLE_TYPE ms, DWORD *q)
{
    (void)This; (void)t; (void)w;
    if (q) *q = 1;
    if (a || ms > D3DMULTISAMPLE_16_SAMPLES) return D3DERR_INVALIDCALL;
    const Fmt *x = fmt_of(f);
    if (!x) return D3DERR_NOTAVAILABLE;
    uint32_t n = ms > 1 ? (uint32_t)ms : 1;
    if (n & (n - 1)) return D3DERR_NOTAVAILABLE;
    if (n > 1 && (x->block > 1 || f == (D3DFORMAT)MAKEFOURCC('I', 'N', 'T', 'Z') || f == D3DFMT_D16_LOCKABLE)) return D3DERR_NOTAVAILABLE;
    VkSampleCountFlags av = x->depth ? R.props.limits.framebufferDepthSampleCounts : R.props.limits.framebufferColorSampleCounts;
    if (!(av & n)) return D3DERR_NOTAVAILABLE;
    /* (DXVK: NONMASKABLE's quality levels are the sample counts there are) */
    if (q && ms == D3DMULTISAMPLE_NONMASKABLE) *q = 32 - (DWORD)__builtin_clz(av);
    return D3D_OK;
}

static HRESULT d3d9_CheckDepthStencilMatch(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT af, D3DFORMAT rf, D3DFORMAT df)
{
    (void)This; (void)t; (void)af; (void)rf; (void)df;
    return a ? D3DERR_INVALIDCALL : D3D_OK;
}

static HRESULT d3d9_CheckDeviceFormatConversion(void *This, UINT a, D3DDEVTYPE t, D3DFORMAT s, D3DFORMAT d)
{
    (void)This; (void)t; (void)s; (void)d;
    return a ? D3DERR_INVALIDCALL : D3D_OK;
}

static HRESULT d3d9_GetDeviceCaps(void *This, UINT a, D3DDEVTYPE t, D3DCAPS9 *caps)
{
    (void)This;
    if (a || !caps) return D3DERR_INVALIDCALL;
    caps_load();
    *caps = caps_dxvk;
    caps->DeviceType = t;
    return D3D_OK;
}

static HMONITOR d3d9_GetAdapterMonitor(void *This, UINT a) { (void)This; (void)a; return (HMONITOR)(uintptr_t)0x00120001u; }

static HRESULT d3d9_CreateDevice(void *This, UINT a, D3DDEVTYPE t, HWND focus, DWORD flags, D3DPRESENT_PARAMETERS *pp,
                                 IDirect3DDevice9 **out)
{
    (void)This; (void)t; (void)focus; (void)flags;
    if (a || !pp || !out) return D3DERR_INVALIDCALL;
    *out = NULL;
    creator_thread = (unsigned long)pthread_self();
    fmts_init();
    options_read();
    dummies_init();
    ff_init();
    if (!make_buffers(pp)) return D3DERR_NOTAVAILABLE;
    static int preloaded;
    if (!preloaded++) pipes_preload();
    obj_init(&dev_obj, &vt_dev, DK_IDirect3DDevice9);
    device_live = 1;
    *out = (IDirect3DDevice9 *)&dev_obj;
    LOG(LOG_ALWAYS, "vulkan renderer: device %ux%u, %d shaders in the table", cur_pp.BackBufferWidth, cur_pp.BackBufferHeight,
        vk_shader_count);
    return D3D_OK;
}

/* ------------------------------------------------------------------ the vtables */
#define RES_COMMON(vt) \
    vt.QueryInterface = com_QueryInterface; vt.AddRef = com_AddRef; vt.Release = com_Release; \
    vt.GetDevice = res_GetDevice; vt.SetPrivateData = res_SetPrivateData; vt.GetPrivateData = res_GetPrivateData; \
    vt.FreePrivateData = res_FreePrivateData;
#define RES_PRIORITY(vt) vt.SetPriority = res_SetPriority; vt.GetPriority = res_GetPriority; vt.PreLoad = res_PreLoad;
#define TEX_COMMON(vt) RES_COMMON(vt) RES_PRIORITY(vt) vt.GetType = tex_GetType; vt.SetLOD = tex_SetLOD; vt.GetLOD = tex_GetLOD; \
    vt.GetLevelCount = tex_GetLevelCount; vt.SetAutoGenFilterType = tex_SetAutoGenFilterType; \
    vt.GetAutoGenFilterType = tex_GetAutoGenFilterType; vt.GenerateMipSubLevels = tex_GenerateMipSubLevels; \
    vt.GetLevelDesc = tex_GetLevelDesc;

static void vtables_init(void)
{
    static int done;
    if (done) return;
    done = 1;
    vt_d3d9 = (IDirect3D9Vtbl){com_QueryInterface, com_AddRef, com_Release, d3d9_RegisterSoftwareDevice, d3d9_GetAdapterCount,
        d3d9_GetAdapterIdentifier, d3d9_GetAdapterModeCount, d3d9_EnumAdapterModes, d3d9_GetAdapterDisplayMode, d3d9_CheckDeviceType,
        d3d9_CheckDeviceFormat, d3d9_CheckDeviceMultiSampleType, d3d9_CheckDepthStencilMatch, d3d9_CheckDeviceFormatConversion,
        d3d9_GetDeviceCaps, d3d9_GetAdapterMonitor, d3d9_CreateDevice};
    vt_dev = (IDirect3DDevice9Vtbl){com_QueryInterface, com_AddRef, com_Release, dev_TestCooperativeLevel, dev_GetAvailableTextureMem,
        dev_EvictManagedResources, dev_GetDirect3D, dev_GetDeviceCaps, dev_GetDisplayMode, dev_GetCreationParameters,
        dev_SetCursorProperties, dev_SetCursorPosition, dev_ShowCursor, dev_CreateAdditionalSwapChain, dev_GetSwapChain,
        dev_GetNumberOfSwapChains, dev_Reset, dev_Present, dev_GetBackBuffer, dev_GetRasterStatus, dev_SetDialogBoxMode,
        dev_SetGammaRamp, dev_GetGammaRamp, dev_CreateTexture, dev_CreateVolumeTexture, dev_CreateCubeTexture,
        dev_CreateVertexBuffer, dev_CreateIndexBuffer, dev_CreateRenderTarget, dev_CreateDepthStencilSurface, dev_UpdateSurface,
        dev_UpdateTexture, dev_GetRenderTargetData, dev_GetFrontBufferData, dev_StretchRect, dev_ColorFill,
        dev_CreateOffscreenPlainSurface, dev_SetRenderTarget, dev_GetRenderTarget, dev_SetDepthStencilSurface,
        dev_GetDepthStencilSurface, dev_BeginScene, dev_EndScene, dev_Clear, dev_SetTransform, dev_GetTransform,
        dev_MultiplyTransform, dev_SetViewport, dev_GetViewport, dev_SetMaterial, dev_GetMaterial, dev_SetLight, dev_GetLight,
        dev_LightEnable, dev_GetLightEnable, dev_SetClipPlane, dev_GetClipPlane, dev_SetRenderState, dev_GetRenderState,
        dev_CreateStateBlock, dev_BeginStateBlock, dev_EndStateBlock, dev_SetClipStatus, dev_GetClipStatus, dev_GetTexture,
        dev_SetTexture, dev_GetTextureStageState, dev_SetTextureStageState, dev_GetSamplerState, dev_SetSamplerState,
        dev_ValidateDevice, dev_SetPaletteEntries, dev_GetPaletteEntries, dev_SetCurrentTexturePalette,
        dev_GetCurrentTexturePalette, dev_SetScissorRect, dev_GetScissorRect, dev_SetSoftwareVertexProcessing,
        dev_GetSoftwareVertexProcessing, dev_SetNPatchMode, dev_GetNPatchMode, dev_DrawPrimitive, dev_DrawIndexedPrimitive,
        dev_DrawPrimitiveUP, dev_DrawIndexedPrimitiveUP, dev_ProcessVertices, dev_CreateVertexDeclaration,
        dev_SetVertexDeclaration, dev_GetVertexDeclaration, dev_SetFVF, dev_GetFVF, dev_CreateVertexShader, dev_SetVertexShader,
        dev_GetVertexShader, dev_SetVertexShaderConstantF, dev_GetVertexShaderConstantF, dev_SetVertexShaderConstantI,
        dev_GetVertexShaderConstantI, dev_SetVertexShaderConstantB, dev_GetVertexShaderConstantB, dev_SetStreamSource,
        dev_GetStreamSource, dev_SetStreamSourceFreq, dev_GetStreamSourceFreq, dev_SetIndices, dev_GetIndices,
        dev_CreatePixelShader, dev_SetPixelShader, dev_GetPixelShader, dev_SetPixelShaderConstantF, dev_GetPixelShaderConstantF,
        dev_SetPixelShaderConstantI, dev_GetPixelShaderConstantI, dev_SetPixelShaderConstantB, dev_GetPixelShaderConstantB,
        dev_DrawRectPatch, dev_DrawTriPatch, dev_DeletePatch, dev_CreateQuery};
    TEX_COMMON(vt_tex)
    vt_tex.GetSurfaceLevel = tex_GetSurfaceLevel;
    vt_tex.LockRect = tex_LockRect;
    vt_tex.UnlockRect = tex_UnlockRect;
    vt_tex.AddDirtyRect = tex_AddDirtyRect;
    TEX_COMMON(vt_cube)
    vt_cube.GetCubeMapSurface = cube_GetCubeMapSurface;
    vt_cube.LockRect = cube_LockRect;
    vt_cube.UnlockRect = cube_UnlockRect;
    vt_cube.AddDirtyRect = cube_AddDirtyRect;
    RES_COMMON(vt_surf)
    RES_PRIORITY(vt_surf)
    vt_surf.GetType = surf_GetType;
    vt_surf.GetContainer = surf_GetContainer;
    vt_surf.GetDesc = surf_GetDesc;
    vt_surf.LockRect = surf_LockRect;
    vt_surf.UnlockRect = surf_UnlockRect;
    vt_surf.GetDC = surf_GetDC;
    vt_surf.ReleaseDC = surf_ReleaseDC;
    RES_COMMON(vt_vb)
    RES_PRIORITY(vt_vb)
    vt_vb.GetType = vb_GetType;
    vt_vb.Lock = buf_Lock;
    vt_vb.Unlock = buf_Unlock;
    vt_vb.GetDesc = vb_GetDesc;
    RES_COMMON(vt_ib)
    RES_PRIORITY(vt_ib)
    vt_ib.GetType = ib_GetType;
    vt_ib.Lock = buf_Lock;
    vt_ib.Unlock = buf_Unlock;
    vt_ib.GetDesc = ib_GetDesc;
    vt_decl = (IDirect3DVertexDeclaration9Vtbl){com_QueryInterface, com_AddRef, com_Release, res_GetDevice, decl_GetDeclaration};
    vt_vs = (IDirect3DVertexShader9Vtbl){com_QueryInterface, com_AddRef, com_Release, res_GetDevice, shader_GetFunction};
    vt_ps = (IDirect3DPixelShader9Vtbl){com_QueryInterface, com_AddRef, com_Release, res_GetDevice, shader_GetFunction};
    vt_sb = (IDirect3DStateBlock9Vtbl){com_QueryInterface, com_AddRef, com_Release, res_GetDevice, sb_Capture, sb_Apply};
    vt_q = (IDirect3DQuery9Vtbl){com_QueryInterface, com_AddRef, com_Release, res_GetDevice, q_GetType, q_GetDataSize, q_Issue, q_GetData};
}

/* Direct3DCreate9 under renderer = vulkan (d3d9.c) */
void *vk_create9(void)
{
    if (!vk_init()) return NULL;
    vtables_init();
    obj_init(&d3d9_obj, &vt_d3d9, DK_IDirect3D9);
    return &d3d9_obj;
}
