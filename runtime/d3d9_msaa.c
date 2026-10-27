/* ANTI-ALIASING (option antialiasing = off | 2x | 4x | 8x, 4x by default): the game draws its 3D
   scene into a texture of its own (then post effects and the HUD into the same texture, then one
   copy to the screen), with the device's own depth buffer. Direct3D 9 textures cannot be
   multisampled, so every render target the game draws to with that depth buffer is drawn instead
   into a multisampled twin (same size and format), with a multisampled twin of the depth buffer;
   the twin is resolved into the game's target when the game reads the target's texture (the post
   pass reads the scene while drawing over it; the copy to the screen reads it last) or draws to the
   target without its twin: three full-size resolves a frame, not one at every change of target.
   The game never sees the twins: it is told its own targets are bound. Targets with other depth
   buffers (the reflection cube, the shadow map) and the screen itself are left as they were. */
#include <stdlib.h>
#include "d3d9_bridge.h"
#include "options.h"

/* a twin: the game's target, its multisampled twin, the target's texture (pointer only), and
   whether the twin may hold drawing not yet resolved into the target */
typedef struct { void *orig, *ms, *tex; int dirty; unsigned gen; volatile int dead; } Twin;
static Twin twins[16];          /* (slots: ms NULL is free) */
static int level;               /* the samples (0: off, or off after the device refused) */
static void *dev_ds, *dev_bb;   /* the device's own depth buffer and back buffer (pointers only) */
static void *ms_ds;             /* dev_ds's twin */
static void *want_rt, *want_ds; /* what the game has bound (referenced while bound) */
static void *host_rt;           /* the target bound at the device */
static Twin *bound;             /* the twin bound (NULL: none) */
static uint32_t game_msaa = TRUE;   /* the game's D3DRS_MULTISAMPLEANTIALIAS */
static int overlay;             /* drawing the overlay (below) */

#define REL(o) ((ULONG (*)(void *))VT(o)[2])(o)
#define ADDREF(o) ((ULONG (*)(void *))VT(o)[1])(o)

static void desc_of(void *surf, D3DSURFACE_DESC *d)
{
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(surf)[12])(surf, d);
}

/* after CreateDevice or Reset: the device's own buffers noted (its depth buffer is what is bound) */
void msaa_device(void *dev)
{
    int n = atoi(option_str("antialiasing", "4x"));  /* ("4x": 4) */
    level = n == 2 || n == 4 || n == 8 ? n : 0;
    void *o = NULL;
    dev_ds = dev_bb = NULL;
    game_msaa = TRUE;                           /* (a device's starting state) */
    overlay = 0;
    if (((HRESULT (*)(void *, void **))VT(dev)[40])(dev, &o) >= 0 && o) {
        dev_ds = o;
        want_ds = o;                            /* (the reference Get gave) */
    }
    o = NULL;
    if (((HRESULT (*)(void *, UINT, UINT, D3DBACKBUFFER_TYPE, void **))VT(dev)[18])(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &o) >= 0 && o) {
        dev_bb = o;
        REL(o);
    }
}

static void twin_free(Twin *t)
{
    REL(t->ms);
    t->ms = t->orig = t->tex = NULL;
    t->dead = t->dirty = 0;
    t->gen++;                                   /* (its target's watcher, if it dies later, no longer counts) */
}

/* A twin goes with its target: the target carries a small object of ours as private data
   (D3DSPD_IUNKNOWN, as d3d9.c's death watchers), released by DXVK as the target is destroyed;
   the twin is then freed at the next binding. */
typedef struct { void **vt; Twin *t; unsigned gen; volatile long ref; } Watch;
static long __stdcall watch_qi(Watch *w, const GUID *g, void **o) { (void)g; *o = w; w->ref++; return 0; }
static unsigned long __stdcall watch_addref(Watch *w) { return (unsigned long)++w->ref; }
static unsigned long __stdcall watch_release(Watch *w)
{
    long r = --w->ref;
    if (r == 0) {
        if (w->t->gen == w->gen) w->t->dead = 1;
        free(w);
    }
    return (unsigned long)r;
}
static void *watch_vt[3] = {(void *)watch_qi, (void *)watch_addref, (void *)watch_release};
static const GUID watch_guid = {0x4c615275, 0x7368, 0x4d53, {0x41, 0x41, 0x54, 0x77, 0x69, 0x6e, 0x21, 0x21}};

static void watch(Twin *t)
{
    Watch *w = calloc(1, sizeof *w);
    w->vt = watch_vt;
    w->t = t;
    w->gen = t->gen;
    w->ref = 1;
    ((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(t->orig)[4])(
        t->orig, &watch_guid, w, sizeof(void *), 1 /* D3DSPD_IUNKNOWN */);
    watch_release(w);                           /* (DXVK holds the only reference now) */
}

/* before Reset: everything of ours released (a reset fails while any is held) */
void msaa_reset(void)
{
    for (int i = 0; i < (int)(sizeof twins / sizeof *twins); i++)
        if (twins[i].ms) twin_free(&twins[i]);
    if (ms_ds) REL(ms_ds);
    if (want_rt) REL(want_rt);
    if (want_ds) REL(want_ds);
    ms_ds = want_rt = want_ds = host_rt = NULL;
    bound = NULL;
}

static void off_for_good(const char *what, HRESULT r)
{
    LOG(LOG_ALWAYS, "antialiasing %dx: %s failed (%08x): off", level, what, (unsigned)r);
    level = 0;
}

/* the twin of a target drawn with the device's depth buffer, made on first use (NULL: none) */
static Twin *twin_of(void *dev, void *rt)
{
    if (!level || rt == dev_bb) return NULL;
    D3DSURFACE_DESC d, dd;
    desc_of(rt, &d);
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE || !(d.Usage & D3DUSAGE_RENDERTARGET)) return NULL;
    if (!ms_ds) {
        desc_of(dev_ds, &dd);
        HRESULT r = ((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, void **, HANDLE *))VT(dev)[29])(
            dev, dd.Width, dd.Height, dd.Format, (D3DMULTISAMPLE_TYPE)level, 0, FALSE, &ms_ds, NULL);
        if (r < 0) { ms_ds = NULL; off_for_good("the depth buffer", r); return NULL; }
        LOG(LOG_ALWAYS, "antialiasing %dx: depth buffer %ux%u", level, dd.Width, dd.Height);
    }
    Twin *t = NULL;
    for (int i = 0; i < (int)(sizeof twins / sizeof *twins); i++) {
        if (twins[i].ms && twins[i].dead) {
            LOG(LOG_ALWAYS, "antialiasing %dx: a target gone, its twin freed", level);
            twin_free(&twins[i]);
        }
        if (twins[i].ms && twins[i].orig == rt) return &twins[i];
        if (!twins[i].ms && !t) t = &twins[i];
    }
    if (!t) return NULL;
    HRESULT r = ((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, void **, HANDLE *))VT(dev)[28])(
        dev, d.Width, d.Height, d.Format, (D3DMULTISAMPLE_TYPE)level, 0, FALSE, &t->ms, NULL);
    if (r < 0) { t->ms = NULL; off_for_good("a target", r); return NULL; }
    t->orig = rt;
    void *tex = NULL;
    if (((HRESULT (*)(void *, const GUID *, void **))VT(rt)[11])(rt, &IID_IDirect3DTexture9, &tex) >= 0 && tex) {
        t->tex = tex;
        REL(tex);
    }
    watch(t);
    LOG(LOG_ALWAYS, "antialiasing %dx: target %ux%u (format %u)", level, d.Width, d.Height, d.Format);
    return t;
}

/* The overlay (the HUD, menus, text) is drawn as it always was, without multisampling (DXVK then
   puts every sample at the pixel's centre): multisampled, a quad's border pixels are shaded with
   texture coordinates from past its edge (a line along the minimap's square). The game starts that
   pass by clearing only the depth and stencil buffers (its 2D view, 0x55e030 with view flag
   0x2000); it lasts until another target is set. */
static void overlay_set(void *dev, int on)
{
    overlay = on;
    ((HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD))VT(dev)[57])(dev, D3DRS_MULTISAMPLEANTIALIAS, on ? FALSE : game_msaa);
}

/* the twin's drawing into the game's target (still dirty while bound: drawing goes on) */
static void resolve(void *dev, Twin *t)
{
    ((HRESULT (*)(void *, void *, const RECT *, void *, const RECT *, D3DTEXTUREFILTERTYPE))VT(dev)[34])(
        dev, t->ms, NULL, t->orig, NULL, D3DTEXF_NONE);
    t->dirty = t == bound;
}

static Twin *twin_with(void *rt)
{
    for (int i = 0; i < (int)(sizeof twins / sizeof *twins); i++)
        if (twins[i].ms && twins[i].orig == rt && !twins[i].dead) return &twins[i];
    return NULL;
}

/* what the game has bound, put at the device: with t, its twin and the depth buffer's twin. The
   twin is chosen when the depth buffer is set (the game sets a target, then its depth buffer: a
   target set while the last pass's depth buffer is still bound is not taken as drawn with it). */
static HRESULT bind(void *dev, Twin *t, int rt_set)
{
    void *rt = t ? t->ms : want_rt, *ds = t ? ms_ds : want_ds;
    if (t != bound && overlay) overlay_set(dev, 0);
    if (!t) {                                   /* (drawn to without its twin: the twin's first) */
        Twin *o = twin_with(want_rt);
        if (o && o->dirty) resolve(dev, o);
    }
    bound = t;
    if (t) t->dirty = 1;
    HRESULT r = 0;
    if (rt_set || rt != host_rt) {
        /* (setting a target resets the viewport: kept when the game did not set one) */
        D3DVIEWPORT9 vp;
        int keep = !rt_set && ((HRESULT (*)(void *, D3DVIEWPORT9 *))VT(dev)[48])(dev, &vp) >= 0;
        r = ((HRESULT (*)(void *, DWORD, void *))VT(dev)[37])(dev, 0, rt);
        if (keep) ((HRESULT (*)(void *, const D3DVIEWPORT9 *))VT(dev)[47])(dev, &vp);
        host_rt = rt;
    }
    HRESULT r2 = ((HRESULT (*)(void *, void *))VT(dev)[39])(dev, ds);
    return rt_set ? r : r2;
}

uint32_t msaa_render_state(uint32_t state, uint32_t value)
{
    (void)state;
    game_msaa = value;
    return overlay ? FALSE : value;
}

HOSTFN(sp_IDirect3DDevice9_Clear, 7)
{
    void *self = d3d_host(ARG(0));
    if (bound && !(ARG(3) & D3DCLEAR_TARGET) && !overlay)
        overlay_set(self, 1);
    return (uint32_t)((HRESULT (*)(void *, DWORD, const D3DRECT *, DWORD, D3DCOLOR, float, DWORD))VT(self)[43])(
        self, ARG(1), (const D3DRECT *)(uintptr_t)ARG(2), ARG(3), ARG(4), d3d_argf(c, 5), ARG(6));
}

static void want(void **slot, void *o)
{
    if (o) ADDREF(o);
    if (*slot) REL(*slot);
    *slot = o;
}

HOSTFN(sp_IDirect3DDevice9_SetRenderTarget, 3)
{
    void *self = d3d_host(ARG(0));
    void *rt = d3d_host_or_null(ARG(2));
    if (!level || ARG(1) != 0 || !rt)           /* (off, more targets at once, or none: as asked) */
        return (uint32_t)((HRESULT (*)(void *, DWORD, void *))VT(self)[37])(self, ARG(1), rt);
    want(&want_rt, rt);
    /* (the same target again, as the HUD does for each of its parts: its twin kept, unresolved) */
    return (uint32_t)bind(self, bound && bound->orig == rt && want_ds == dev_ds ? bound : NULL, 1);
}

HOSTFN(sp_IDirect3DDevice9_SetDepthStencilSurface, 2)
{
    void *self = d3d_host(ARG(0));
    void *ds = d3d_host_or_null(ARG(1));
    if (!level)
        return (uint32_t)((HRESULT (*)(void *, void *))VT(self)[39])(self, ds);
    want(&want_ds, ds);
    if (!want_rt)                               /* (no target set yet: the device's own is bound) */
        return (uint32_t)((HRESULT (*)(void *, void *))VT(self)[39])(self, want_ds);
    return (uint32_t)bind(self, want_ds && want_ds == dev_ds ? twin_of(self, want_rt) : NULL, 0);
}

HOSTFN(sp_IDirect3DDevice9_GetRenderTarget, 3)
{
    void *self = d3d_host(ARG(0));
    void *o = NULL;
    uint32_t r;
    if (ARG(1) == 0 && bound) {                 /* (the game's own, not its twin) */
        o = want_rt;
        ADDREF(o);
        r = 0;
    } else {
        r = (uint32_t)((HRESULT (*)(void *, DWORD, void **))VT(self)[38])(self, ARG(1), ARG(2) ? &o : NULL);
    }
    if (ARG(2)) W32(ARG(2), o ? d3d_wrap(DK_IDirect3DSurface9, o) : 0);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_GetDepthStencilSurface, 2)
{
    void *self = d3d_host(ARG(0));
    void *o = NULL;
    uint32_t r;
    if (bound) {
        o = want_ds;
        ADDREF(o);
        r = 0;
    } else {
        r = (uint32_t)((HRESULT (*)(void *, void **))VT(self)[40])(self, ARG(1) ? &o : NULL);
    }
    if (ARG(1)) W32(ARG(1), o ? d3d_wrap(DK_IDirect3DSurface9, o) : 0);
    return r;
}

/* reading a target's texture: what its twin holds resolved into it first */
HOSTFN(sp_IDirect3DDevice9_SetTexture, 3)
{
    void *self = d3d_host(ARG(0));
    void *tex = d3d_host_or_null(ARG(2));
    if (tex)
        for (int i = 0; i < (int)(sizeof twins / sizeof *twins); i++)
            if (twins[i].dirty && twins[i].tex == tex) { resolve(self, &twins[i]); break; }
    return (uint32_t)((HRESULT (*)(void *, DWORD, void *))VT(self)[65])(self, ARG(1), tex);
}
