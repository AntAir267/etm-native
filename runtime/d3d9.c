/* The Direct3D 9 bridge to DXVK (design: the program's D3D9 runs on DXVK's native Linux build).
   The program's COM objects are ours (com.c): their vtables point at runtime functions, mostly
   generated (gen/d3d9_gen.c), which call DXVK's object behind them. Written here: the objects'
   identity and reference counts, the methods whose arguments differ between the program's
   32-bit layout and DXVK's 64-bit one (present parameters, locked rectangles, window handles),
   and locks: the program cannot address DXVK's memory (above 4 GB), so a lock hands it a copy in
   its own memory (persistent for vertex and index buffers, per lock for textures and surfaces),
   copied into DXVK's at unlock. */
#define _GNU_SOURCE
#define INITGUID
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>
#include "d3d9_bridge.h"
#include "win.h"
#include "options.h"
#include <strings.h>

static void ui_screen(uint32_t w, uint32_t h);

/* ------------------------------------------------------------------ objects */
typedef struct {
    void *host;
    uint32_t guest;
    int kind;
    /* vertex/index buffers: the program's copy */
    uint32_t shadow, size;
    int nlocks;
    struct { uint32_t off, len; void *hp; } locks[4];
    int watched;                         /* a death watcher is attached (ent_watch) */
} Ent;

#define HCAP 65536
static Ent *ents[HCAP];                  /* open addressing on the host pointer */
static pthread_mutex_t dlock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t hslot(void *h) { return (uint32_t)(((uintptr_t)h >> 4) * 2654435761u) & (HCAP - 1); }

static Ent *ent_find(void *h)
{
    for (uint32_t i = hslot(h), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return NULL;
        if (ents[i] != (Ent *)1 && ents[i]->host == h) return ents[i];
    }
    return NULL;
}

static void ent_put(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++)
        if (!ents[i] || ents[i] == (Ent *)1) { ents[i] = e; return; }
    fatal("too many Direct3D objects");
}

static void ent_del(Ent *e)
{
    for (uint32_t i = hslot(e->host), n = 0; n < HCAP; i = (i + 1) & (HCAP - 1), n++) {
        if (!ents[i]) return;
        if (ents[i] == e) { ents[i] = (Ent *)1; return; }
    }
}

void *d3d_host(uint32_t guest) { return com_host(guest); }
void *d3d_host_or_null(uint32_t guest) { return guest ? com_host(guest) : NULL; }

static Ent *ent_of_guest(uint32_t guest)
{
    void *h = com_host(guest);
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(h);
    pthread_mutex_unlock(&dlock);
    return e;
}

/* a resource's own class from its type (the program may hold it as a base texture) */
static int kind_of_resource(void *h)
{
    uint32_t t = ((uint32_t (*)(void *))VT(h)[10])(h);       /* IDirect3DResource9::GetType */
    switch (t) {
    case 1: return DK_IDirect3DSurface9;
    case 2: return DK_IDirect3DVolume9;
    case 3: return DK_IDirect3DTexture9;
    case 4: return DK_IDirect3DVolumeTexture9;
    case 5: return DK_IDirect3DCubeTexture9;
    case 6: return DK_IDirect3DVertexBuffer9;
    case 7: return DK_IDirect3DIndexBuffer9;
    }
    fatal("Direct3D resource of unknown type %u", t);
}

/* DEATH WATCHERS. A resource can die without the program releasing it (a texture's surfaces go
   with the texture). Each resource carries a small object of ours as private data
   (D3DSPD_IUNKNOWN); DXVK releases it when the resource is destroyed, on the thread whose release
   did it, and the entry goes then. Before, a dead entry was only noticed when DXVK put a new
   object at the same address (below), which happened or not depending on DXVK's allocator, so
   the program's memory differed from run to run. */
static void ent_free_locked(Ent *e);
typedef struct { void **vt; Ent *e; volatile long ref; } Watcher;
static long __stdcall watcher_qi(Watcher *w, const GUID *g, void **o) { (void)g; *o = w; w->ref++; return 0; }
static unsigned long __stdcall watcher_addref(Watcher *w) { return (unsigned long)++w->ref; }
static unsigned long __stdcall watcher_release(Watcher *w)
{
    long r = --w->ref;
    if (r == 0) {
        pthread_mutex_lock(&dlock);
        if (w->e) ent_free_locked(w->e);
        pthread_mutex_unlock(&dlock);
        free(w);
    }
    return (unsigned long)r;
}
static void *watcher_vt[3] = {(void *)watcher_qi, (void *)watcher_addref, (void *)watcher_release};
static const GUID watcher_guid = {0x4c615275, 0x7368, 0x4e61, {0x74, 0x69, 0x76, 0x65, 0x44, 0x65, 0x61, 0x64}};

static void ent_watch(Ent *e)
{
    switch (e->kind) {
    case DK_IDirect3DSurface9: case DK_IDirect3DVolume9: case DK_IDirect3DTexture9:
    case DK_IDirect3DVolumeTexture9: case DK_IDirect3DCubeTexture9: case DK_IDirect3DVertexBuffer9:
    case DK_IDirect3DIndexBuffer9: break;
    default: return;
    }
    Watcher *w = calloc(1, sizeof *w);
    w->vt = watcher_vt;
    w->e = e;
    w->ref = 1;
    HRESULT r = ((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(e->host)[4])(
        e->host, &watcher_guid, w, sizeof(void *), 1 /* D3DSPD_IUNKNOWN: the data is the object */);
    if (r >= 0) e->watched = 1;
    else w->e = NULL;
    watcher_release(w);                  /* DXVK holds the only reference now */
}

/* under dlock: the entry goes (released, destroyed, or found dead) */
static void ent_free_locked(Ent *e)
{
    ent_del(e);
    LOG(LOG_D3D, "%s %08x gone", d3d_classes[e->kind].iface, e->guest);
    if (e->shadow) gfree(e->shadow);
    W32(e->guest, 0);                    /* a use after it finds no vtable */
    free(e);
}

/* An entry left for a host object that died without the program releasing it (a texture's
   surfaces go with the texture) must not be handed out for a new object DXVK puts at the same
   address: objects from Create* calls are always new (d3d_wrap_new), and an entry of another
   kind is stale. (Seen as a new texture coming back as an old surface: the game then called a
   texture method on it and crashed, now and then, loading a race.) */
static uint32_t wrap(int kind, void *host, int is_new)
{
    if (!host) return 0;
    pthread_mutex_lock(&dlock);
    Ent *e = ent_find(host);
    if (e && (is_new || (kind != DK_IDirect3DResource9 && kind != DK_IDirect3DBaseTexture9 &&
                         kind != DK_IUnknown && e->kind != kind))) {
        LOG(LOG_D3D, "%s %p: the old %s %08x there is gone", d3d_classes[kind].iface, host,
            d3d_classes[e->kind].iface, e->guest);
        ent_free_locked(e);
        e = NULL;
    }
    if (e) {
        pthread_mutex_unlock(&dlock);
        return e->guest;
    }
    pthread_mutex_unlock(&dlock);
    if (kind == DK_IDirect3DResource9 || kind == DK_IDirect3DBaseTexture9)
        kind = kind_of_resource(host);
    e = calloc(1, sizeof *e);
    e->host = host;
    e->kind = kind;
    e->guest = com_new(&d3d_classes[kind], host);
    pthread_mutex_lock(&dlock);
    Ent *again = ent_find(host);         /* another thread wrapped it meanwhile */
    if (again) {
        pthread_mutex_unlock(&dlock);
        free(e);
        return again->guest;
    }
    ent_put(e);
    pthread_mutex_unlock(&dlock);
    ent_watch(e);
    LOG(LOG_D3D, "%s %p -> %08x", d3d_classes[kind].iface, host, e->guest);
    return e->guest;
}

uint32_t d3d_wrap(int kind, void *host) { return wrap(kind, host, 0); }
uint32_t d3d_wrap_new(int kind, void *host) { return wrap(kind, host, 1); }

void *d3d_hwnd(uint32_t hwnd)
{
    (void)hwnd;                          /* every window is the host window */
    return host_sdl_window();
}

float d3d_argf(CPU *c, int n)
{
    uint32_t u = ARG(n);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

void d3d_unsupported(CPU *c, const char *name, int nargs)
{
    LOG(LOG_ALWAYS, "%s is not supported (called from %08x)", name, R32(c->esp));
    api_return(c, 0x8876086cu, nargs);   /* D3DERR_INVALIDCALL */
}

/* ------------------------------------------------------------------ IUnknown */
static const struct { const GUID *iid; int kind; } iids[] = {
    {&IID_IDirect3D9, DK_IDirect3D9}, {&IID_IDirect3DDevice9, DK_IDirect3DDevice9},
    {&IID_IDirect3DResource9, DK_IDirect3DResource9}, {&IID_IDirect3DVertexBuffer9, DK_IDirect3DVertexBuffer9},
    {&IID_IDirect3DVolume9, DK_IDirect3DVolume9}, {&IID_IDirect3DSwapChain9, DK_IDirect3DSwapChain9},
    {&IID_IDirect3DSurface9, DK_IDirect3DSurface9}, {&IID_IDirect3DIndexBuffer9, DK_IDirect3DIndexBuffer9},
    {&IID_IDirect3DBaseTexture9, DK_IDirect3DBaseTexture9}, {&IID_IDirect3DTexture9, DK_IDirect3DTexture9},
    {&IID_IDirect3DCubeTexture9, DK_IDirect3DCubeTexture9}, {&IID_IDirect3DVolumeTexture9, DK_IDirect3DVolumeTexture9},
    {&IID_IDirect3DVertexDeclaration9, DK_IDirect3DVertexDeclaration9},
    {&IID_IDirect3DVertexShader9, DK_IDirect3DVertexShader9}, {&IID_IDirect3DPixelShader9, DK_IDirect3DPixelShader9},
    {&IID_IDirect3DStateBlock9, DK_IDirect3DStateBlock9}, {&IID_IDirect3DQuery9, DK_IDirect3DQuery9},
};

HOSTFN(sp_any_QueryInterface, 3)
{
    void *self = d3d_host(ARG(0));
    Ent *me = ent_of_guest(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[0])(self, &g, &o);
    if ((int32_t)r < 0 || !o) {
        if (ARG(2)) W32(ARG(2), 0);
        LOG(LOG_D3D, "QueryInterface(" GUID_FMT ") on %s -> %08x", GUID_ARGS(ARG(1)), me ? d3d_classes[me->kind].iface : "?", r);
        return r;
    }
    int kind = me ? me->kind : DK_IDirect3DResource9;
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++)
        if (!memcmp(&g, iids[i].iid, sizeof g)) { kind = iids[i].kind; break; }
    if (o == self && me)
        kind = me->kind;                 /* the same object: keep its class */
    W32(ARG(2), d3d_wrap(kind, o));
    return r;
}

HOSTFN(sp_any_AddRef, 1)
{
    void *self = d3d_host(ARG(0));
    return ((uint32_t (*)(void *))VT(self)[1])(self);
}

HOSTFN(sp_any_Release, 1)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    int watched = e && e->watched;       /* (its watcher frees it as the object dies) */
    uint32_t r = ((uint32_t (*)(void *))VT(self)[2])(self);
    if (r == 0 && e && !watched) {
        pthread_mutex_lock(&dlock);
        ent_free_locked(e);
        pthread_mutex_unlock(&dlock);
    }
    return r;
}

/* SetPrivateData with D3DSPD_IUNKNOWN would hand DXVK a program object to reference count: kept
   out of DXVK (the data is stored as plain bytes instead, the pointer included) */
HOSTFN(sp_any_SetPrivateData, 5)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    uint32_t flags = ARG(4) & ~1u;
    return (uint32_t)((HRESULT (*)(void *, const GUID *, const void *, DWORD, DWORD))VT(self)[4])(
        self, &g, gptr(ARG(2)), ARG(3), flags);
}

HOSTFN(sp_any_GetPrivateData, 4)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    DWORD n = ARG(3) ? R32(ARG(3)) : 0;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void *, DWORD *))VT(self)[5])(
        self, &g, ARG(2) ? gptr(ARG(2)) : NULL, &n);
    if (ARG(3)) W32(ARG(3), n);
    return r;
}

/* ------------------------------------------------------------------ present parameters */
/* the program's D3DPRESENT_PARAMETERS: 14 dwords, hDeviceWindow at +28 */
static void pp_in(uint32_t g, D3DPRESENT_PARAMETERS *pp, uint32_t focus)
{
    memset(pp, 0, sizeof *pp);
    pp->BackBufferWidth = R32(g);
    pp->BackBufferHeight = R32(g + 4);
    pp->BackBufferFormat = (D3DFORMAT)R32(g + 8);
    pp->BackBufferCount = R32(g + 12);
    pp->MultiSampleType = (D3DMULTISAMPLE_TYPE)R32(g + 16);
    pp->MultiSampleQuality = R32(g + 20);
    pp->SwapEffect = (D3DSWAPEFFECT)R32(g + 24);
    pp->hDeviceWindow = d3d_hwnd(R32(g + 28) ? R32(g + 28) : focus);
    pp->Windowed = R32(g + 32);
    pp->EnableAutoDepthStencil = R32(g + 36);
    pp->AutoDepthStencilFormat = (D3DFORMAT)R32(g + 40);
    pp->Flags = R32(g + 44);
    pp->FullScreen_RefreshRateInHz = R32(g + 48);
    pp->PresentationInterval = R32(g + 52);
    /* option vsync (off by default): frames wait for the screen's refresh (the game asks for
       immediate presents); with render_rate = screen the drawn frames then follow the screen */
    if (option_bool("vsync", 0))
        pp->PresentationInterval = 1;           /* D3DPRESENT_INTERVAL_ONE */
    else if (pp->PresentationInterval == 1)
        pp->PresentationInterval = 0x80000000u; /* D3DPRESENT_INTERVAL_IMMEDIATE */
    LOG(LOG_ALWAYS, "present parameters: %ux%u format %u x%u, ms %u, swap %u, %s, depth %u (format %u), "
        "flags %x, %u Hz, interval %x", pp->BackBufferWidth, pp->BackBufferHeight, pp->BackBufferFormat,
        pp->BackBufferCount, pp->MultiSampleType, pp->SwapEffect, pp->Windowed ? "windowed" : "full screen",
        pp->EnableAutoDepthStencil, pp->AutoDepthStencilFormat, pp->Flags, pp->FullScreen_RefreshRateInHz,
        pp->PresentationInterval);
    /* the desktop's mode is never changed: full screen becomes a window of the back buffer's size
       (the host window goes full screen on its own, borderless, with Alt+Enter) */
    if (!pp->Windowed) {
        pp->Windowed = TRUE;
        pp->FullScreen_RefreshRateInHz = 0;
    }
    if (pp->BackBufferWidth && pp->BackBufferHeight)
        host_window_resize((int)pp->BackBufferWidth, (int)pp->BackBufferHeight);
    ui_screen(pp->BackBufferWidth, pp->BackBufferHeight);
}

static void pp_out(uint32_t g, const D3DPRESENT_PARAMETERS *pp)
{
    W32(g, pp->BackBufferWidth);
    W32(g + 4, pp->BackBufferHeight);
    W32(g + 8, pp->BackBufferFormat);
    W32(g + 12, pp->BackBufferCount);
}

/* ------------------------------------------------------------------ IDirect3D9 */
static void *dxvk;
static void *(*dxvk_create9)(UINT);

API(D3D9, Direct3DCreate9, 1)
{
    if (!dxvk) {
        char dir[1024], path[1200];
        ssize_t n = readlink("/proc/self/exe", dir, sizeof dir - 1);
        dir[n > 0 ? n : 0] = 0;
        char *slash = strrchr(dir, '/');
        if (slash) *slash = 0;
        snprintf(path, sizeof path, "%s/lib/libdxvk_d3d9.so.0", dir);
        setenv("DXVK_WSI_DRIVER", "SDL3", 0);
        static char logdir[1100];
        snprintf(logdir, sizeof logdir, "%s", user_dir);
        setenv("DXVK_LOG_PATH", logdir, 0);
        setenv("DXVK_STATE_CACHE_PATH", logdir, 0);
        /* option anisotropic = off | 2 | 4 | 8 | 16 (default): textures stay sharp at a slant (the
           road ahead); neither the PC nor the Xbox version filtered so.
           option shadow_filter = original | 3 | 5 | 7 (default): the shadow map read through a
           smooth filter that many texels square (our DXVK's d3d9.shadowFilter,
           third_party/dxvk-soft-shadows.patch) where the game's shaders take one hardware 2x2
           comparison sample: hard, stepped edges */
        {
            static char conf[160];
            int an = atoi(option_str("anisotropic", "16"));
            int sf = atoi(option_str("shadow_filter", "7"));
            if (an != 2 && an != 4 && an != 8 && an != 16) an = -1;
            if (sf != 3 && sf != 5 && sf != 7) sf = 0;
            /* (a hidden window, --hidden, is never shown: presenting without waiting for the
               screen keeps a compositor that never draws it from holding the frame up) */
            snprintf(conf, sizeof conf, "d3d9.samplerAnisotropy = %d; d3d9.shadowFilter = %d;%s", an, sf,
                     hidden_window ? " d3d9.presentInterval = 0;" : "");
            setenv("DXVK_CONFIG", conf, 0);
        }
        dxvk = dlopen(path, RTLD_NOW | RTLD_LOCAL);
        if (!dxvk)
            fatal("cannot load DXVK (%s): %s", path, dlerror());
        dxvk_create9 = (void *(*)(UINT))dlsym(dxvk, "Direct3DCreate9");
        if (!dxvk_create9)
            fatal("DXVK has no Direct3DCreate9");
        d3d_classes_init();
    }
    if (!host_sdl_window())
        LOG(LOG_ALWAYS, "Direct3DCreate9 without a host window (headless run): DXVK cannot present");
    void *d = dxvk_create9(ARG(0));
    LOG(LOG_ALWAYS, "Direct3DCreate9(%u) -> %p", ARG(0), d);
    return d3d_wrap_new(DK_IDirect3D9, d);
}

HOSTFN(sp_IDirect3D9_CreateDevice, 7)
{
    void *self = d3d_host(ARG(0));
    uint32_t flags = ARG(4);
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(5), &pp, ARG(3));
    /* Direct3D 9 sets the FPU to single precision unless told to preserve it: so does this, on the
       program's control word; DXVK always preserves (the host FPU is not its to change) */
    if (!(flags & D3DCREATE_FPU_PRESERVE))
        fpu_setcw(c, c->fcw & ~0x300u);
    void *dev = NULL;
    HRESULT (*create)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **) =
        (HRESULT (*)(void *, UINT, D3DDEVTYPE, HWND, DWORD, D3DPRESENT_PARAMETERS *, void **))VT(self)[16];
    uint32_t r = (uint32_t)create(self, ARG(1), (D3DDEVTYPE)ARG(2), d3d_hwnd(ARG(3)), flags | D3DCREATE_FPU_PRESERVE, &pp, &dev);
    if ((int32_t)r >= 0) msaa_device(dev);
    LOG(LOG_ALWAYS, "CreateDevice(adapter %u, type %u, flags %x) -> %08x", ARG(1), ARG(2), flags, r);
    pp_out(ARG(5), &pp);
    if (ARG(6)) W32(ARG(6), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DDevice9, dev) : 0);
    return r;
}

HOSTFN(sp_IDirect3D9_GetAdapterMonitor, 2) { return 0x00120001u; }
HOSTFN(sp_IDirect3D9_RegisterSoftwareDevice, 2) { return 0x8876086cu; }

/* ------------------------------------------------------------------ IDirect3DDevice9 */
HOSTFN(sp_IDirect3DDevice9_Reset, 2)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    msaa_reset();                               /* (its targets go before the device's reset) */
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *))VT(self)[16])(self, &pp);
    if ((int32_t)r >= 0) msaa_device(self);
    LOG(LOG_ALWAYS, "Reset -> %08x", r);
    pp_out(ARG(1), &pp);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_CreateAdditionalSwapChain, 3)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    pp_in(ARG(1), &pp, 0);
    void *sc = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *, void **))VT(self)[13])(self, &pp, &sc);
    pp_out(ARG(1), &pp);
    if (ARG(2)) W32(ARG(2), (int32_t)r >= 0 ? d3d_wrap_new(DK_IDirect3DSwapChain9, sc) : 0);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_GetCreationParameters, 2)
{
    void *self = d3d_host(ARG(0));
    D3DDEVICE_CREATION_PARAMETERS p;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DDEVICE_CREATION_PARAMETERS *))VT(self)[9])(self, &p);
    W32(ARG(1), p.AdapterOrdinal);
    W32(ARG(1) + 4, p.DeviceType);
    W32(ARG(1) + 8, main_hwnd);
    W32(ARG(1) + 12, p.BehaviorFlags & ~(uint32_t)D3DCREATE_FPU_PRESERVE);
    return r;
}

/* ETM_SHOT="frame,frame,...": the back buffer of those presented frames saved as
   shot_<frame>.png in the user folder (checking the picture without capturing the desktop) */
#include <zlib.h>
static void png_chunk(FILE *f, const char *type, const uint8_t *d, uint32_t n)
{
    uint8_t h[8] = {n >> 24, n >> 16, n >> 8, n, type[0], type[1], type[2], type[3]};
    fwrite(h, 1, 8, f);
    if (n) fwrite(d, 1, n, f);
    uint32_t crc = crc32(0, (const uint8_t *)type, 4);
    if (n) crc = crc32(crc, d, n);         /* (zlib's crc32 of a null buffer is its initial value, 0) */
    uint8_t t[4] = {crc >> 24, crc >> 16, crc >> 8, crc};
    fwrite(t, 1, 4, f);
}

static void shot_maybe(void *dev)
{
    static int parsed;
    static unsigned long want[1024];
    static int nwant;
    if (!parsed) {
        parsed = 1;
        const char *e = getenv("ETM_SHOT");
        for (const char *p = e; p && *p && nwant < 1024; ) {
            want[nwant++] = strtoul(p, (char **)&p, 10);
            while (*p == ',' || *p == ' ') p++;
        }
    }
    int hit = 0;
    for (int i = 0; i < nwant; i++) hit |= want[i] == frame_count + 1;
    if (!hit) return;
    void *bb = NULL, *sys = NULL;
    if (((HRESULT (*)(void *, UINT, UINT, D3DBACKBUFFER_TYPE, void **))VT(dev)[18])(dev, 0, 0, D3DBACKBUFFER_TYPE_MONO, &bb) < 0)
        return;
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(bb)[12])(bb, &d);
    if (((HRESULT (*)(void *, UINT, UINT, D3DFORMAT, D3DPOOL, void **, HANDLE *))VT(dev)[36])(
            dev, d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, NULL) >= 0 &&
        ((HRESULT (*)(void *, void *, void *))VT(dev)[32])(dev, bb, sys) >= 0) {
        D3DLOCKED_RECT lr;
        if (((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(sys)[13])(sys, &lr, NULL, D3DLOCK_READONLY) >= 0) {
            uint32_t w = d.Width, h = d.Height;
            size_t rowb = 1 + 3 * (size_t)w;
            uint8_t *raw = malloc(rowb * h);
            for (uint32_t y = 0; y < h; y++) {
                const uint8_t *src = (const uint8_t *)lr.pBits + (size_t)y * lr.Pitch;
                uint8_t *dst = raw + y * rowb;
                dst[0] = 0;
                for (uint32_t x = 0; x < w; x++) {          /* X8R8G8B8 / A8R8G8B8: B G R A */
                    dst[1 + 3 * x] = src[4 * x + 2];
                    dst[2 + 3 * x] = src[4 * x + 1];
                    dst[3 + 3 * x] = src[4 * x];
                }
            }
            ((HRESULT (*)(void *))VT(sys)[14])(sys);
            uLongf zn = compressBound(rowb * h);
            uint8_t *z = malloc(zn);
            compress2(z, &zn, raw, rowb * h, 6);
            char path[1200];
            snprintf(path, sizeof path, "%s/shot_%lu.png", user_dir, frame_count + 1);
            FILE *f = fopen(path, "wb");
            if (f) {
                static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
                fwrite(sig, 1, 8, f);
                uint8_t ih[13] = {w >> 24, w >> 16, w >> 8, w, h >> 24, h >> 16, h >> 8, h, 8, 2, 0, 0, 0};
                png_chunk(f, "IHDR", ih, 13);
                png_chunk(f, "IDAT", z, (uint32_t)zn);
                png_chunk(f, "IEND", NULL, 0);
                fclose(f);
                LOG(LOG_ALWAYS, "frame %lu saved to %s (%ux%u, format %u)", frame_count + 1, path, w, h, d.Format);
            }
            free(z);
            free(raw);
        }
    }
    if (sys) ((ULONG (*)(void *))VT(sys)[2])(sys);
    ((ULONG (*)(void *))VT(bb)[2])(bb);
}

HOSTFN(sp_IDirect3DDevice9_Present, 5)
{
    void *self = d3d_host(ARG(0));
    shot_maybe(self);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *))VT(self)[17])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)));
    frame_presented();
    return r;
}

HOSTFN(sp_IDirect3DDevice9_GetNPatchMode, 1)
{
    LOG(LOG_ALWAYS, "GetNPatchMode: returns 0 (N-patches are never on)");
    return 0;
}

/* ------------------------------------------------------------------ shader substitution */
/* Option shaders = original (the default) | improved. The game's shaders are fixed bytecode from
   vshaders.pak / pshaders.pak. With improved, a shader whose bytecode's FNV-1a 64 hash names a
   replacement in data/shaders/ (<hash>.vsh / .psh, shader assembly; tools/d3dasm.py assembles them
   at build time into gen/shaders_gen.c) is created from ours instead; the program gets the object
   as before (its shader table holds whatever CreateVertexShader returns), so nothing above the
   device sees a difference. original never looks. */
extern const int shader_sub_count;
extern const uint64_t shader_sub_hash[];
extern const uint32_t shader_sub_len[];
extern const uint32_t *const shader_sub_code[];
extern const char *const shader_sub_name[];

static int shaders_improved(void)
{
    static int improved = -1;
    if (improved < 0)
        improved = !strcasecmp(option_str("shaders", "improved"), "improved");
    return improved;
}

/* the bytecode's length in tokens, end token included (0: not a shader). Shader model 2 and up
   give each instruction's length; in 1.x an instruction's parameters are the tokens after it with
   bit 31 set (def's four floats aside). */
static uint32_t shader_tokens(const uint32_t *w)
{
    if (w[0] >> 16 != 0xfffe && w[0] >> 16 != 0xffff)
        return 0;
    uint32_t major = (w[0] >> 8) & 0xff, i = 1;
    while (i < 0x10000) {
        uint32_t t = w[i], op = t & 0xffff, n;
        if (t == 0xffff)
            return i + 1;
        if (op == 0xfffe) n = (t >> 16) & 0x7fff;
        else if (major >= 2) n = (t >> 24) & 15;
        else if (op == 81) n = 5;
        else for (n = 0; w[i + 1 + n] & 0x80000000u; n++) ;
        i += 1 + n;
    }
    return 0;
}

static const DWORD *shader_code(uint32_t guest, int ps)
{
    static uint8_t logged[256];
    const DWORD *code = (const DWORD *)gptr(guest);
    if (!shaders_improved() || !guest)
        return code;
    uint32_t n = shader_tokens(code);
    uint64_t h = 0xcbf29ce484222325ull;
    const uint8_t *b = (const uint8_t *)code;
    for (uint32_t i = 0; i < n * 4; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    for (int i = 0; i < shader_sub_count; i++)
        if (shader_sub_hash[i] == h) {
            if (i < 256 && !logged[i]) {
                logged[i] = 1;
                LOG(LOG_ALWAYS, "shaders: %s %016llx replaced by data/shaders/%s", ps ? "pixel shader" : "vertex shader",
                    (unsigned long long)h, shader_sub_name[i]);
            }
            return (const DWORD *)shader_sub_code[i];
        }
    LOG(LOG_D3D, "shaders: %s %016llx (%u tokens) kept", ps ? "pixel shader" : "vertex shader", (unsigned long long)h, n);
    return code;
}

HOSTFN(sp_IDirect3DDevice9_CreateVertexShader, 3)
{
    void *self = d3d_host(ARG(0));
    void *o = NULL;
    const DWORD *code = shader_code(ARG(1), 0);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const DWORD *, void **))VT(self)[91])(self, code, ARG(2) ? &o : NULL);
    if (ARG(2)) W32(ARG(2), o ? d3d_wrap_new(DK_IDirect3DVertexShader9, o) : 0);
    if ((int32_t)r < 0 && (r != 0x8876086au || code != (const DWORD *)gptr(ARG(1))))
        LOG(code != (const DWORD *)gptr(ARG(1)) ? LOG_ALWAYS : LOG_D3D, "IDirect3DDevice9::CreateVertexShader -> %08x", r);
    return r;
}

HOSTFN(sp_IDirect3DDevice9_CreatePixelShader, 3)
{
    void *self = d3d_host(ARG(0));
    void *o = NULL;
    const DWORD *code = shader_code(ARG(1), 1);
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const DWORD *, void **))VT(self)[106])(self, code, ARG(2) ? &o : NULL);
    if (ARG(2)) W32(ARG(2), o ? d3d_wrap_new(DK_IDirect3DPixelShader9, o) : 0);
    if ((int32_t)r < 0 && (r != 0x8876086au || code != (const DWORD *)gptr(ARG(1))))
        LOG(code != (const DWORD *)gptr(ARG(1)) ? LOG_ALWAYS : LOG_D3D, "IDirect3DDevice9::CreatePixelShader -> %08x", r);
    return r;
}

/* ------------------------------------------------------------------ 2D at other screen shapes */
/* The game draws its menus, text and HUD as quads already in screen space (-1..1, from a 640x480
   layout) through a vertex shader whose c12-c15 matrix is then the identity. At a screen that is
   not 4:3 those stretch; option ui = center (the default) draws a copy of each such quad laid out
   in a centred 4:3 area, except that corners on the screen's edge stay there: backdrops, the menus'
   bands, fades and movies keep filling the screen, text and pictures keep their shape. Not in a
   race: its HUD is in pixels, already in shape, and stays at the screen's edges (game.c fixes
   its text's width). ui = stretch is the game's own. */
static int ui_center = -1;
static float ui_sx = 1, ui_sy = 1;          /* the 4:3 area's share of the screen */
static float vs_c12[16];                    /* the program's c12-c15 */
static int vs_c12_identity;
static uint32_t ss0_vb, ss0_off, ss0_stride; /* stream 0: program object, offset, stride */

static void ui_screen(uint32_t w, uint32_t h)
{
    if (ui_center < 0)
        ui_center = strcasecmp(option_str("ui", "center"), "stretch") != 0;
    ui_sx = ui_sy = 1;
    if (!ui_center || !w || !h)
        return;
    float a = (float)w / (float)h;
    if (a > 4.0f / 3.0f + 0.01f)
        ui_sx = (4.0f / 3.0f) / a;
    else if (a < 4.0f / 3.0f - 0.01f)
        ui_sy = a / (4.0f / 3.0f);
}

HOSTFN(sp_IDirect3DDevice9_SetStreamSource, 5)
{
    void *self = d3d_host(ARG(0));
    if (ARG(1) == 0) {
        ss0_vb = ARG(2);
        ss0_off = ARG(3);
        ss0_stride = ARG(4);
    }
    return (uint32_t)((HRESULT (*)(void *, UINT, void *, UINT, UINT))VT(self)[100])(
        self, (UINT)ARG(1), (void *)d3d_host_or_null(ARG(2)), (UINT)ARG(3), (UINT)ARG(4));
}

/* HEMISPHERIC AMBIENT for the world (shaders = improved; data/shaders, DESIGN.md 2026-10-26).
   The game lights everything with one flat ambient colour, c35 (the mode record's colour 2; also
   pixel constant 6, the ground's), plus the sun c34 along c32. The replaced world shaders take
   instead an ambient that depends on the surface's facing: sky above, a bounce from the ground
   below, amb(n) = mid + n.y x half, with
     sky    = c35 x kS x the fog colour's hue (the mode's own sky, record +0x70, the game's
              current fog colour at 0x6afc14, its brightness divided out, half strength)
     ground = c35 x kG + c34 x max(c32.y, 0) x kB (the sunlit street's bounce)
     mid = (sky + ground) / 2 -> vertex c100, half = (sky - ground) / 2 -> vertex c101
   (no game shader reads c96-c159 but the bones' c93 and the shaders' own defs c109/c110). The
   cars' per-pixel paint (ps 0x3e/0x3f) and plain lit parts (vs 0x3b) get the same shape scaled
   to their own ambient c62 (0.6 x c35 by day) and kC: pixel c8 and vertex c102 = c62 x mid / c35
   x kC, pixel c9 and vertex c103 = c62 x half / c35 x kC (no game pixel shader reads c8, c9).
   kS, kG, kB and kC depend on option sun (original 1.08, 0.55, 0.2, 1: about the original's
   brightness; true 1.55, 0.35, 0.3, 1.6): the true sun lights level ground at 30 degrees where the
   original's lit it from 81, its long shadows put more of the street in shade, which the sky must
   light, and the cars are often backlit. Neutral (mid = c35, half = 0: the original's ambient exactly) at night (the
   mode record's +0x9c) and whenever c35 is not the mode's colour (the HUD's grey pass, the front
   end's scenes). Set as the program sets c32, c34, c35 or c62, beneath its calls (the draw
   stream and game memory are the same). */
#define HEMI_MODE       0x8fdc0c    /* the game mode (view.c) */
#define HEMI_RECORDS    0x8fd260    /* + mode x 0xa8: +0x38 colour 2 (RRGGBBAA), +0x9c night */
#define HEMI_FOG        0x6afc14    /* the fog colour last set (0x53f5e0: ARGB) */
static float hemi_c32[4], hemi_c34[4], hemi_c35[4], hemi_c62[4];
static float hemi_mid[3] = {1, 1, 1}, hemi_half[3];     /* per channel, as factors of c35 */
static float hemi_kcar = 1;                             /* the cars' share, on top */

static void hemi_cars(void *self)
{
    float p[8] = {0};
    for (int i = 0; i < 3; i++) {
        p[i] = hemi_c62[i] * hemi_mid[i] * hemi_kcar;
        p[4 + i] = hemi_c62[i] * hemi_half[i] * hemi_kcar;
    }
    p[3] = hemi_c62[3];
    ((HRESULT (*)(void *, UINT, const float *, UINT))VT(self)[109])(self, 8, p, 2);
    ((HRESULT (*)(void *, UINT, const float *, UINT))VT(self)[94])(self, 102, p, 2);
}

static void hemi_update(void *self)
{
    uint32_t mode = R32(HEMI_MODE), rec = HEMI_RECORDS + (mode < 6 ? mode : 0) * 0xa8;
    uint32_t col = R32(rec + 0x38);
    int neutral = mode >= 6 || R32(rec + 0x9c) != 0;
    for (int i = 0; i < 3 && !neutral; i++)
        if (fabsf(hemi_c35[i] - (float)((col >> (24 - 8 * i)) & 0xff) / 256.0f) > 1e-6f)
            neutral = 1;
    float sky[3], gnd[3], mid[4] = {0}, half[4] = {0};
    if (neutral) {
        for (int i = 0; i < 3; i++)
            hemi_mid[i] = 1, hemi_half[i] = 0;
        hemi_kcar = 1;
    } else {
        const int t = option_sun_true();
        const float kS = t ? 1.55f : 1.08f, kG = t ? 0.35f : 0.55f, kB = t ? 0.30f : 0.20f;
        hemi_kcar = t ? 1.60f : 1.0f;
        uint32_t fog = R32(HEMI_FOG);
        float f[3] = {(float)(fog >> 16 & 0xff) / 255.0f, (float)(fog >> 8 & 0xff) / 255.0f,
                      (float)(fog & 0xff) / 255.0f};
        float lum = 0.3f * f[0] + 0.59f * f[1] + 0.11f * f[2];
        float up = hemi_c32[1] > 0 ? hemi_c32[1] : 0;
        for (int i = 0; i < 3; i++) {
            float hue = lum > 0.02f ? f[i] / lum : 1.0f;
            hue = hue < 0.5f ? 0.5f : hue > 1.6f ? 1.6f : hue;
            sky[i] = hemi_c35[i] * kS * (1.0f + (hue - 1.0f) * 0.5f);
            gnd[i] = hemi_c35[i] * kG + hemi_c34[i] * up * kB;
            float a = hemi_c35[i] > 1e-4f ? hemi_c35[i] : 1e-4f;
            hemi_mid[i] = (sky[i] + gnd[i]) * 0.5f / a;
            hemi_half[i] = (sky[i] - gnd[i]) * 0.5f / a;
        }
    }
    for (int i = 0; i < 3; i++) {
        mid[i] = hemi_c35[i] * hemi_mid[i];
        half[i] = hemi_c35[i] * hemi_half[i];
    }
    mid[3] = hemi_c35[3];
    float m[8] = {mid[0], mid[1], mid[2], mid[3], half[0], half[1], half[2], half[3]};
    ((HRESULT (*)(void *, UINT, const float *, UINT))VT(self)[94])(self, 100, m, 2);
    hemi_cars(self);
}

HOSTFN(sp_IDirect3DDevice9_SetVertexShaderConstantF, 4)
{
    void *self = d3d_host(ARG(0));
    uint32_t start = ARG(1), n = ARG(3);
    const float *v = (const float *)gptr(ARG(2));
    if (start < 16 && start + n > 12) {
        for (uint32_t i = start; i < start + n; i++)
            if (i >= 12 && i < 16)
                memcpy(&vs_c12[(i - 12) * 4], v + (i - start) * 4, 16);
        static const float id[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
        vs_c12_identity = !memcmp(vs_c12, id, sizeof id);
    }
    uint32_t rr = (uint32_t)((HRESULT (*)(void *, UINT, const float *, UINT))VT(self)[94])(self, start, v, n);
    /* With shaders = improved the per-pixel car shaders (data/shaders: ps 0x3e, 0x3f) need some
       of the vertex shaders' constants, which a pixel shader cannot read: each is copied, as the
       program sets it, into a pixel shader constant no game shader reads (c24-c31; the game's
       ps_2_0 shaders read at most c22, ps_1_1 c0-c7). The program's own calls are unchanged. */
    if (shaders_improved() && start <= 92 && start + n > 32) {
        static const uint8_t from[8] = {32, 37, 58, 61, 62, 90, 91, 92};   /* -> c24-c31 */
        for (int k = 0; k < 8; k++)
            if (from[k] >= start && from[k] < start + n)
                ((HRESULT (*)(void *, UINT, const float *, UINT))VT(self)[109])(self, 24 + k, v + (from[k] - start) * 4, 1);
    }
    /* the hemispheric ambient (above) follows c32, c34, c35 and the cars' c62 */
    if (shaders_improved() && start <= 62 && start + n > 32) {
        int world = 0, cars = 0;
        static float *const keep[4] = {hemi_c32, hemi_c34, hemi_c35, hemi_c62};
        static const uint8_t reg[4] = {32, 34, 35, 62};
        for (int k = 0; k < 4; k++)
            if (reg[k] >= start && reg[k] < start + n) {
                memcpy(keep[k], v + (reg[k] - start) * 4, 16);
                if (k < 3) world = 1; else cars = 1;
            }
        if (world)
            hemi_update(self);
        else if (cars)
            hemi_cars(self);
    }
    return rr;
}

/* DRAW DISTANCE: what the game shows ends at its linear table fog's end (FOGSTART 1), which the
   device setup picks from VIEW DISTANCE (0x6efc9c: 2000, 3000, 4000; the city's geometry reaches
   much further) and a device reset puts back to 3000 whatever the setting (0x540cf5). Option
   draw_distance (original 1, far 2, max 4) multiplies the setting's end, on every setting of it
   (game.c scales the cars' and the city's level-of-detail distances to match).
   FOG: the game's starts at 1, so everything is fogged in proportion to its distance (a car 400
   away 10 % at end 4000). Option fog = haze moves FOGSTART to a quarter of the end: the near
   field clear, the haze thickening to the same full fog at the end (where the cars and the
   city's meshes stop), in the game's colour. Both are set with every FOGSTART or FOGEND the game
   sets (device setup, reset), from the setting, so they hold whatever it changes. */
static double draw_distance = -1;
static int fog_haze = -1;
#define HAZE_START 0.25f

static float fog_end(void)
{
    if (draw_distance < 0) {
        draw_distance = option_draw_distance();
        const char *f = option_str("fog", "haze");
        fog_haze = !strcasecmp(f, "haze");
        LOG(LOG_ALWAYS, "draw distance x%g, fog %s", draw_distance, fog_haze ? "haze" : "original");
    }
    static const float ends[3] = {2000, 3000, 4000};
    uint32_t v = R32(0x6efc9c);
    return ends[v < 3 ? v : 2] * (float)draw_distance;
}

HOSTFN(sp_IDirect3DDevice9_SetRenderState, 3)
{
    void *self = d3d_host(ARG(0));
    uint32_t state = ARG(1), value = ARG(2);
    HRESULT (*set)(void *, D3DRENDERSTATETYPE, DWORD) = (HRESULT (*)(void *, D3DRENDERSTATETYPE, DWORD))VT(self)[57];
    if (state == D3DRS_FOGEND || state == D3DRS_FOGSTART) {
        float end = fog_end();
        if (state == D3DRS_FOGEND) {
            memcpy(&value, &end, 4);
            if (fog_haze) {             /* (the game sets FOGSTART first: it follows this end) */
                float start = end * HAZE_START;
                DWORD sv;
                memcpy(&sv, &start, 4);
                set(self, D3DRS_FOGSTART, sv);
            }
        } else if (fog_haze) {
            float start = end * HAZE_START;
            memcpy(&value, &start, 4);
        }
    }
    if (state == D3DRS_MULTISAMPLEANTIALIAS)
        value = msaa_render_state(state, value);
    return (uint32_t)set(self, (D3DRENDERSTATETYPE)state, value);
}

/* one coordinate into the centred area; on the screen's edge it stays */
static float ui_map(float x, float s)
{
    if (x <= -0.99f && x >= -1.01f) return -1;
    if (x >= 0.99f && x <= 1.01f) return 1;
    return x * s;
}

HOSTFN(sp_IDirect3DDevice9_DrawPrimitive, 4)
{
    void *self = d3d_host(ARG(0));
    uint32_t prim = ARG(1), first = ARG(2), count = ARG(3);
    HRESULT (*draw)(void *, D3DPRIMITIVETYPE, UINT, UINT) = (HRESULT (*)(void *, D3DPRIMITIVETYPE, UINT, UINT))VT(self)[81];
    /* (in a race, screen 0x38, the HUD is laid out in pixels and is drawn as is: game.c) */
    if ((ui_sx < 1 || ui_sy < 1) && vs_c12_identity && ss0_vb && ss0_stride >= 12 && ss0_stride <= 64 &&
        R32(0x8d0980) != 0x38 &&
        (prim == D3DPT_TRIANGLEFAN || prim == D3DPT_TRIANGLESTRIP || prim == D3DPT_TRIANGLELIST)) {
        Ent *e = ent_of_guest(ss0_vb);
        uint32_t nv = prim == D3DPT_TRIANGLELIST ? count * 3 : count + 2;
        uint32_t at = ss0_off + first * ss0_stride;
        static uint8_t copy[256 * 64];
        if (e && e->shadow && nv <= 256 && at + nv * ss0_stride <= e->size) {
            memcpy(copy, gptr(e->shadow + at), nv * ss0_stride);
            for (uint32_t i = 0; i < nv; i++) {
                float *xy = (float *)(copy + i * ss0_stride);
                xy[0] = ui_map(xy[0], ui_sx);
                xy[1] = ui_map(xy[1], ui_sy);
            }
            HRESULT r = ((HRESULT (*)(void *, D3DPRIMITIVETYPE, UINT, const void *, UINT))VT(self)[83])(
                self, (D3DPRIMITIVETYPE)prim, count, copy, ss0_stride);
            /* a drawing from memory unsets stream 0: the program's comes back */
            ((HRESULT (*)(void *, UINT, void *, UINT, UINT))VT(self)[100])(
                self, 0, (void *)d3d_host_or_null(ss0_vb), ss0_off, ss0_stride);
            return (uint32_t)r;
        }
    }
    return (uint32_t)draw(self, (D3DPRIMITIVETYPE)prim, first, count);
}

/* ------------------------------------------------------------------ IDirect3DSwapChain9 */
HOSTFN(sp_IDirect3DSwapChain9_Present, 6)
{
    void *self = d3d_host(ARG(0));
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const RECT *, const RECT *, HWND, const RGNDATA *, DWORD))VT(self)[3])(
        self, (const RECT *)gptr(ARG(1)), (const RECT *)gptr(ARG(2)), NULL, (const RGNDATA *)gptr(ARG(4)), ARG(5));
    frame_presented();
    return r;
}

HOSTFN(sp_IDirect3DSwapChain9_GetPresentParameters, 2)
{
    void *self = d3d_host(ARG(0));
    D3DPRESENT_PARAMETERS pp;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, D3DPRESENT_PARAMETERS *))VT(self)[9])(self, &pp);
    uint32_t g = ARG(1);
    W32(g, pp.BackBufferWidth); W32(g + 4, pp.BackBufferHeight); W32(g + 8, pp.BackBufferFormat);
    W32(g + 12, pp.BackBufferCount); W32(g + 16, pp.MultiSampleType); W32(g + 20, pp.MultiSampleQuality);
    W32(g + 24, pp.SwapEffect); W32(g + 28, main_hwnd); W32(g + 32, pp.Windowed);
    W32(g + 36, pp.EnableAutoDepthStencil); W32(g + 40, pp.AutoDepthStencilFormat); W32(g + 44, pp.Flags);
    W32(g + 48, pp.FullScreen_RefreshRateInHz); W32(g + 52, pp.PresentationInterval);
    return r;
}

/* ------------------------------------------------------------------ buffer locks */
static uint32_t buffer_lock(CPU *c, int desc_index_size)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    uint32_t off = ARG(1), len = ARG(2), flags = ARG(4);
    if (!e) fatal("buffer lock on an unknown object");
    if (!e->shadow) {
        /* D3DVERTEXBUFFER_DESC / D3DINDEXBUFFER_DESC: Size at 16 */
        uint8_t desc[32];
        ((HRESULT (*)(void *, void *))VT(self)[13])(self, desc);
        memcpy(&e->size, desc + desc_index_size, 4);
        e->shadow = galloc(e->size + 16);
        memset(gptr(e->shadow), 0, e->size);
    }
    if (off > e->size) off = e->size;
    if (!len || off + len > e->size) len = e->size - off;
    void *hp = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, UINT, UINT, void **, DWORD))VT(self)[11])(self, off, len, &hp, flags);
    if ((int32_t)r < 0 || !hp) {
        W32(ARG(3), 0);
        return r;
    }
    if (flags & D3DLOCK_READONLY)
        memcpy(gptr(e->shadow + off), hp, len);
    if (e->nlocks < 4)
        e->locks[e->nlocks] = (typeof(e->locks[0])){off, (flags & D3DLOCK_READONLY) ? 0 : len, hp};
    e->nlocks++;
    W32(ARG(3), e->shadow + off);
    return r;
}

static uint32_t buffer_unlock(CPU *c)
{
    void *self = d3d_host(ARG(0));
    Ent *e = ent_of_guest(ARG(0));
    if (e && e->nlocks > 0) {
        e->nlocks--;
        if (e->nlocks < 4 && e->locks[e->nlocks].len) {
            memcpy(e->locks[e->nlocks].hp, gptr(e->shadow + e->locks[e->nlocks].off), e->locks[e->nlocks].len);
            if (draw_hashing)
                draw_hash_bytes(gptr(e->shadow + e->locks[e->nlocks].off), e->locks[e->nlocks].len);
            if (dev_recording)
                devrec_upload(e->shadow + e->locks[e->nlocks].off, e->locks[e->nlocks].len);
        }
    }
    return (uint32_t)((HRESULT (*)(void *))VT(self)[12])(self);
}

HOSTFN(sp_IDirect3DVertexBuffer9_Lock, 5) { return buffer_lock(c, 16); }
HOSTFN(sp_IDirect3DVertexBuffer9_Unlock, 1) { return buffer_unlock(c); }
HOSTFN(sp_IDirect3DIndexBuffer9_Lock, 5) { return buffer_lock(c, 16); }
HOSTFN(sp_IDirect3DIndexBuffer9_Unlock, 1) { return buffer_unlock(c); }

/* ------------------------------------------------------------------ texture and surface locks */
/* bytes per block and block size of a format (4 for the DXT formats, else 1) */
static int fmt_info(uint32_t f, int *block)
{
    *block = 1;
    switch (f) {
    case D3DFMT_DXT1: *block = 4; return 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: *block = 4; return 16;
    case D3DFMT_A8R8G8B8: case D3DFMT_X8R8G8B8: case D3DFMT_A8B8G8R8: case D3DFMT_X8B8G8R8:
    case D3DFMT_A2R10G10B10: case D3DFMT_A2B10G10R10: case D3DFMT_G16R16: case D3DFMT_Q8W8V8U8:
    case D3DFMT_V16U16: case D3DFMT_X8L8V8U8: case D3DFMT_R32F: case D3DFMT_G16R16F:
    case D3DFMT_D32: case D3DFMT_D24S8: case D3DFMT_D24X8: case D3DFMT_D24X4S4: case D3DFMT_D32F_LOCKABLE:
    case D3DFMT_A2W10V10U10: case D3DFMT_INDEX32:
        return 4;
    case D3DFMT_R8G8B8: return 3;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4:
    case D3DFMT_X4R4G4B4: case D3DFMT_A8R3G3B2: case D3DFMT_A8L8: case D3DFMT_V8U8: case D3DFMT_L6V5U5:
    case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_D15S1: case D3DFMT_L16: case D3DFMT_R16F:
    case D3DFMT_A8P8: case D3DFMT_INDEX16:
        return 2;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4:
        return 1;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_Q16W16V16U16: case D3DFMT_G32R32F:
        return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    }
    return 4;
}

typedef struct Lk { struct Lk *next; void *host; uint32_t sub; uint32_t shadow, bytes; void *hp; int readonly; } Lk;
static Lk *lks;

static uint32_t rect_lock(CPU *c, void *self, uint32_t sub, int lock_i, D3DSURFACE_DESC *d, uint32_t lr,
                          uint32_t rect, uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_RECT *, const RECT *, DWORD))
{
    D3DLOCKED_RECT hl;
    /* the whole surface on the host: the program's copy covers it, and a rectangle is an offset */
    HRESULT r = lock(self, sub, &hl, NULL, flags & ~(uint32_t)D3DLOCK_NO_DIRTY_UPDATE);
    (void)lock_i;
    if (r < 0) { W32(lr, 0); W32(lr + 4, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t rows = (d->Height + block - 1) / block;
    uint32_t bytes = (uint32_t)hl.Pitch * rows;
    uint32_t sh = galloc(bytes + 16);
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(gptr(sh), hl.pBits, bytes);
    uint32_t off = 0;
    if (rect) {
        int32_t x = (int32_t)R32(rect), y = (int32_t)R32(rect + 4);
        off = (uint32_t)(y / block) * (uint32_t)hl.Pitch + (uint32_t)(x / block) * (uint32_t)bpb;
    }
    Lk *k = malloc(sizeof *k);
    *k = (Lk){NULL, self, sub, sh, bytes, hl.pBits, (flags & D3DLOCK_READONLY) != 0};
    pthread_mutex_lock(&dlock);
    k->next = lks;
    lks = k;
    pthread_mutex_unlock(&dlock);
    W32(lr, (uint32_t)hl.Pitch);
    W32(lr + 4, sh + off);
    return (uint32_t)r;
}

static void rect_unlock_copy(void *self, uint32_t sub)
{
    pthread_mutex_lock(&dlock);
    Lk **pk = &lks, *k = NULL;
    for (; *pk; pk = &(*pk)->next)
        if ((*pk)->host == self && (*pk)->sub == sub) { k = *pk; *pk = k->next; break; }
    pthread_mutex_unlock(&dlock);
    if (!k) {
        LOG(LOG_ALWAYS, "unlock of %p (%u) that was not locked", self, sub);
        return;
    }
    if (getenv("ETM_LOG_LOCKS")) {
        static int n;
        if (k->bytes >= 262144 && n++ < 40) {
            uint32_t nz = 0;
            for (uint32_t i = 0; i < k->bytes; i += 4) nz += (R32(k->shadow + i) & 0xffffffu) != 0;
            LOG(LOG_ALWAYS, "unlock %p sub %u: %u bytes, %u nonzero dwords", self, sub, k->bytes, nz);
        }
    }
    if (!k->readonly)
        memcpy(k->hp, gptr(k->shadow), k->bytes);
    gfree(k->shadow);
    free(k);
}

/* the per-interface lock functions, with the subresource in a common form */
static HRESULT tex_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[19])(s, sub, l, r, f); }
static HRESULT cube_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ return ((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[19])(s, (D3DCUBEMAP_FACES)(sub >> 16), sub & 0xffff, l, r, f); }
static HRESULT surf_lock(void *s, uint32_t sub, D3DLOCKED_RECT *l, const RECT *r, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_RECT *, const RECT *, DWORD))VT(s)[13])(s, l, r, f); }

HOSTFN(sp_IDirect3DTexture9_LockRect, 5)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[17])(self, ARG(1), &d);
    return rect_lock(c, self, ARG(1), 19, &d, ARG(2), ARG(3), ARG(4), tex_lock);
}

HOSTFN(sp_IDirect3DTexture9_UnlockRect, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[20])(self, ARG(1));
}

HOSTFN(sp_IDirect3DCubeTexture9_LockRect, 6)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VT(self)[17])(self, ARG(2), &d);
    return rect_lock(c, self, (ARG(1) << 16) | ARG(2), 19, &d, ARG(3), ARG(4), ARG(5), cube_lock);
}

HOSTFN(sp_IDirect3DCubeTexture9_UnlockRect, 3)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, (ARG(1) << 16) | ARG(2));
    return (uint32_t)((HRESULT (*)(void *, D3DCUBEMAP_FACES, UINT))VT(self)[20])(self, (D3DCUBEMAP_FACES)ARG(1), ARG(2));
}

HOSTFN(sp_IDirect3DSurface9_LockRect, 4)
{
    void *self = d3d_host(ARG(0));
    D3DSURFACE_DESC d;
    ((HRESULT (*)(void *, D3DSURFACE_DESC *))VT(self)[12])(self, &d);
    return rect_lock(c, self, 0, 13, &d, ARG(1), ARG(2), ARG(3), surf_lock);
}

HOSTFN(sp_IDirect3DSurface9_UnlockRect, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[14])(self);
}

/* volumes: boxes of slices */
static uint32_t box_lock(CPU *c, void *self, uint32_t sub, const D3DVOLUME_DESC *d, uint32_t lb, uint32_t box,
                         uint32_t flags, HRESULT (*lock)(void *, uint32_t, D3DLOCKED_BOX *, const D3DBOX *, DWORD))
{
    D3DLOCKED_BOX hb;
    HRESULT r = lock(self, sub, &hb, NULL, flags);
    if (r < 0) { W32(lb, 0); W32(lb + 4, 0); W32(lb + 8, 0); return (uint32_t)r; }
    int block, bpb = fmt_info(d->Format, &block);
    uint32_t bytes = (uint32_t)hb.SlicePitch * d->Depth;
    uint32_t sh = galloc(bytes + 16);
    if (!(flags & D3DLOCK_DISCARD))
        memcpy(gptr(sh), hb.pBits, bytes);
    uint32_t off = 0;
    if (box)
        off = R32(box + 16) * (uint32_t)hb.SlicePitch + (R32(box + 4) / block) * (uint32_t)hb.RowPitch +
              (R32(box) / block) * (uint32_t)bpb;
    Lk *k = malloc(sizeof *k);
    *k = (Lk){NULL, self, sub, sh, bytes, hb.pBits, (flags & D3DLOCK_READONLY) != 0};
    pthread_mutex_lock(&dlock);
    k->next = lks;
    lks = k;
    pthread_mutex_unlock(&dlock);
    W32(lb, (uint32_t)hb.RowPitch);
    W32(lb + 4, (uint32_t)hb.SlicePitch);
    W32(lb + 8, sh + off);
    return (uint32_t)r;
}

static HRESULT voltex_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ return ((HRESULT (*)(void *, UINT, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[19])(s, sub, l, b, f); }
static HRESULT vol_lock(void *s, uint32_t sub, D3DLOCKED_BOX *l, const D3DBOX *b, DWORD f)
{ (void)sub; return ((HRESULT (*)(void *, D3DLOCKED_BOX *, const D3DBOX *, DWORD))VT(s)[9])(s, l, b, f); }

HOSTFN(sp_IDirect3DVolumeTexture9_LockBox, 5)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, UINT, D3DVOLUME_DESC *))VT(self)[17])(self, ARG(1), &d);
    return box_lock(c, self, ARG(1), &d, ARG(2), ARG(3), ARG(4), voltex_lock);
}

HOSTFN(sp_IDirect3DVolumeTexture9_UnlockBox, 2)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, ARG(1));
    return (uint32_t)((HRESULT (*)(void *, UINT))VT(self)[20])(self, ARG(1));
}

HOSTFN(sp_IDirect3DVolume9_LockBox, 4)
{
    void *self = d3d_host(ARG(0));
    D3DVOLUME_DESC d;
    ((HRESULT (*)(void *, D3DVOLUME_DESC *))VT(self)[8])(self, &d);
    return box_lock(c, self, 0, &d, ARG(1), ARG(2), ARG(3), vol_lock);
}

HOSTFN(sp_IDirect3DVolume9_UnlockBox, 1)
{
    void *self = d3d_host(ARG(0));
    rect_unlock_copy(self, 0);
    return (uint32_t)((HRESULT (*)(void *))VT(self)[10])(self);
}

/* ------------------------------------------------------------------ the rest */
static uint32_t get_container(CPU *c, int index)
{
    void *self = d3d_host(ARG(0));
    GUID g;
    memcpy(&g, gptr(ARG(1)), sizeof g);
    void *o = NULL;
    uint32_t r = (uint32_t)((HRESULT (*)(void *, const GUID *, void **))VT(self)[index])(self, &g, &o);
    int kind = DK_IDirect3DResource9;
    for (size_t i = 0; i < sizeof iids / sizeof *iids; i++)
        if (!memcmp(&g, iids[i].iid, sizeof g)) { kind = iids[i].kind; break; }
    W32(ARG(2), (int32_t)r >= 0 && o ? d3d_wrap(kind, o) : 0);
    return r;
}

HOSTFN(sp_IDirect3DSurface9_GetContainer, 3) { return get_container(c, 11); }
HOSTFN(sp_IDirect3DVolume9_GetContainer, 3) { return get_container(c, 7); }

HOSTFN(sp_IDirect3DSurface9_GetDC, 2)
{
    LOG(LOG_ALWAYS, "IDirect3DSurface9::GetDC is not supported");
    if (ARG(1)) W32(ARG(1), 0);
    return 0x8876086cu;
}

HOSTFN(sp_IDirect3DSurface9_ReleaseDC, 2) { return 0x8876086cu; }
