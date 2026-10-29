/* DIRECT3D INVENTORY (a measuring aid for our own Vulkan renderer, VULKAN.md;
   ETM_D3D_INV=from-to[,from-to...], up to 4 windows): every Direct3D 9 call the program makes,
   counted by method; the render, sampler and texture stage states it sets, each with the values
   seen; the resources it makes (type, format, pool, usage, and the sizes of render targets and
   depth buffers); its shaders (model, FNV-1a 64 hash, as d3d9.c's substitution names them) and which
   pairs draw; vertex declarations and FVFs; draws by primitive; locks by kind and flags; clears,
   StretchRects, queries; pipeline keys (what a Vulkan pipeline would have to hold at each draw:
   shaders, vertex layout, fixed states, target formats); and, for Enter the Matrix's fixed-function
   drawing (larush-native had none), the fixed-function state at each draw that has no shader: the
   vertex side (lighting, the enabled lights' types, material sources, texture coordinate sources
   and transforms, fog) and the texture stages' operations and arguments (FFVS / FFPS keys), the
   lights and materials set, the transforms.
   Counts are kept for the whole run and for each window of frames, also given per frame; written
   at exit to <user>/d3d_inventory_<from>-<to>.txt, one file a window. The shaders' bytecode the
   game makes (its D3DX assembles and compiles them at run time from text) is written to
   <user>/shaders/<hash>.vsh.bin / .psh.bin. Only the program's own calls are seen (run_thunk); the
   runtime's own (d3d9_msaa.c, the shots) call the device directly and are listed from their code in
   VULKAN.md.
   Off (the default) nothing here runs: run_thunk tests one flag. On, it only reads (the program's
   arguments and the objects' descriptions), so the run is the same. */
#define _GNU_SOURCE
#include <d3d9.h>
#undef LOG
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "runtime.h"
#ifndef COM_MAGIC
#define COM_MAGIC 0x4d4f4321u    /* "!COM" (com.c) */
#endif

int d3dinv_on;
#define NWIN 4
static unsigned long inv_from[NWIN], inv_to[NWIN];
static int nwin;
static pthread_mutex_t ilock = PTHREAD_MUTEX_INITIALIZER;

/* ------------------------------------------------------------------ counters by key */
typedef struct { char *key; unsigned long all, win[NWIN]; } Cnt;
#define CCAP (1 << 16)
static Cnt cnt[CCAP];
static int ncnt;

static uint32_t khash(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}

static void count(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void count(const char *fmt, ...)
{
    char k[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(k, sizeof k, fmt, ap);
    va_end(ap);
    for (uint32_t i = khash(k) & (CCAP - 1), n = 0; n < CCAP; i = (i + 1) & (CCAP - 1), n++) {
        if (!cnt[i].key) {
            if (ncnt > CCAP * 3 / 4) return;
            cnt[i].key = strdup(k);
            ncnt++;
        } else if (strcmp(cnt[i].key, k)) continue;
        cnt[i].all++;
        for (int w = 0; w < nwin; w++)
            cnt[i].win[w] += frame_count >= inv_from[w] && frame_count < inv_to[w];
        return;
    }
}

/* ------------------------------------------------------------------ names */
static const char *rs_name(uint32_t s)
{
    switch (s) {
    case 7: return "ZENABLE"; case 8: return "FILLMODE"; case 9: return "SHADEMODE";
    case 14: return "ZWRITEENABLE"; case 15: return "ALPHATESTENABLE"; case 16: return "LASTPIXEL";
    case 19: return "SRCBLEND"; case 20: return "DESTBLEND"; case 22: return "CULLMODE";
    case 23: return "ZFUNC"; case 24: return "ALPHAREF"; case 25: return "ALPHAFUNC";
    case 26: return "DITHERENABLE"; case 27: return "ALPHABLENDENABLE"; case 28: return "FOGENABLE";
    case 29: return "SPECULARENABLE"; case 34: return "FOGCOLOR"; case 35: return "FOGTABLEMODE";
    case 36: return "FOGSTART"; case 37: return "FOGEND"; case 38: return "FOGDENSITY";
    case 48: return "RANGEFOGENABLE"; case 52: return "STENCILENABLE"; case 53: return "STENCILFAIL";
    case 54: return "STENCILZFAIL"; case 55: return "STENCILPASS"; case 56: return "STENCILFUNC";
    case 57: return "STENCILREF"; case 58: return "STENCILMASK"; case 59: return "STENCILWRITEMASK";
    case 60: return "TEXTUREFACTOR"; case 128: return "WRAP0"; case 129: return "WRAP1";
    case 130: return "WRAP2"; case 131: return "WRAP3"; case 136: return "CLIPPING";
    case 137: return "LIGHTING"; case 139: return "AMBIENT"; case 140: return "FOGVERTEXMODE";
    case 141: return "COLORVERTEX"; case 142: return "LOCALVIEWER"; case 143: return "NORMALIZENORMALS";
    case 145: return "DIFFUSEMATERIALSOURCE"; case 146: return "SPECULARMATERIALSOURCE";
    case 147: return "AMBIENTMATERIALSOURCE"; case 148: return "EMISSIVEMATERIALSOURCE";
    case 151: return "VERTEXBLEND"; case 152: return "CLIPPLANEENABLE"; case 154: return "POINTSIZE";
    case 155: return "POINTSIZE_MIN"; case 156: return "POINTSPRITEENABLE"; case 157: return "POINTSCALEENABLE";
    case 161: return "MULTISAMPLEANTIALIAS"; case 162: return "MULTISAMPLEMASK"; case 166: return "POINTSIZE_MAX";
    case 167: return "INDEXEDVERTEXBLENDENABLE"; case 168: return "COLORWRITEENABLE"; case 170: return "TWEENFACTOR";
    case 171: return "BLENDOP"; case 174: return "SCISSORTESTENABLE"; case 175: return "SLOPESCALEDEPTHBIAS";
    case 176: return "ANTIALIASEDLINEENABLE"; case 185: return "TWOSIDEDSTENCILMODE";
    case 186: return "CCW_STENCILFAIL"; case 187: return "CCW_STENCILZFAIL"; case 188: return "CCW_STENCILPASS";
    case 189: return "CCW_STENCILFUNC"; case 190: return "COLORWRITEENABLE1"; case 191: return "COLORWRITEENABLE2";
    case 192: return "COLORWRITEENABLE3"; case 193: return "BLENDFACTOR"; case 194: return "SRGBWRITEENABLE";
    case 195: return "DEPTHBIAS"; case 206: return "SEPARATEALPHABLENDENABLE"; case 207: return "SRCBLENDALPHA";
    case 208: return "DESTBLENDALPHA"; case 209: return "BLENDOPALPHA";
    }
    return "?";
}

static const char *ss_name(uint32_t s)
{
    static const char *const n[] = {"?", "ADDRESSU", "ADDRESSV", "ADDRESSW", "BORDERCOLOR", "MAGFILTER",
        "MINFILTER", "MIPFILTER", "MIPMAPLODBIAS", "MAXMIPLEVEL", "MAXANISOTROPY", "SRGBTEXTURE",
        "ELEMENTINDEX", "DMAPOFFSET"};
    return s < 14 ? n[s] : "?";
}

static const char *tss_name(uint32_t s)
{
    switch (s) {
    case 1: return "COLOROP"; case 2: return "COLORARG1"; case 3: return "COLORARG2"; case 4: return "ALPHAOP";
    case 5: return "ALPHAARG1"; case 6: return "ALPHAARG2"; case 7: return "BUMPENVMAT00"; case 8: return "BUMPENVMAT01";
    case 9: return "BUMPENVMAT10"; case 10: return "BUMPENVMAT11"; case 11: return "TEXCOORDINDEX";
    case 22: return "BUMPENVLSCALE"; case 23: return "BUMPENVLOFFSET"; case 24: return "TEXTURETRANSFORMFLAGS";
    case 26: return "COLORARG0"; case 27: return "ALPHAARG0"; case 28: return "RESULTARG"; case 32: return "CONSTANT";
    }
    return "?";
}

static const char *fmt_name(uint32_t f)
{
    static char b[8][16];
    static int k;
    switch (f) {
    case 0: return "UNKNOWN"; case 20: return "R8G8B8"; case 21: return "A8R8G8B8"; case 22: return "X8R8G8B8";
    case 23: return "R5G6B5"; case 24: return "X1R5G5B5"; case 25: return "A1R5G5B5"; case 26: return "A4R4G4B4";
    case 28: return "A8"; case 31: return "A2B10G10R10"; case 32: return "A8B8G8R8"; case 33: return "X8B8G8R8";
    case 34: return "G16R16"; case 35: return "A2R10G10B10"; case 36: return "A16B16G16R16"; case 50: return "L8";
    case 51: return "A8L8"; case 60: return "V8U8"; case 63: return "Q8W8V8U8"; case 64: return "V16U16";
    case 70: return "D16_LOCKABLE"; case 71: return "D32"; case 73: return "D15S1"; case 75: return "D24S8";
    case 77: return "D24X8"; case 79: return "D24X4S4"; case 80: return "D16"; case 81: return "L16";
    case 82: return "D32F_LOCKABLE"; case 83: return "D24FS8"; case 100: return "VERTEXDATA";
    case 101: return "INDEX16"; case 102: return "INDEX32"; case 111: return "R16F"; case 112: return "G16R16F";
    case 113: return "A16B16G16R16F"; case 114: return "R32F"; case 115: return "G32R32F"; case 116: return "A32B32G32R32F";
    }
    char *s = b[k++ & 7];
    if (f > 0xffffff && ((f & 0xff) >= 32 && (f & 0xff) < 127))
        snprintf(s, 16, "%c%c%c%c", f & 0xff, (f >> 8) & 0xff, (f >> 16) & 0xff, f >> 24);
    else
        snprintf(s, 16, "fmt%u", f);
    return s;
}

static const char *prim_name(uint32_t p)
{
    static const char *const n[] = {"?", "POINTLIST", "LINELIST", "LINESTRIP", "TRIANGLELIST", "TRIANGLESTRIP", "TRIANGLEFAN"};
    return p < 7 ? n[p] : "?";
}

static const char *pool_name(uint32_t p)
{
    static const char *const n[] = {"DEFAULT", "MANAGED", "SYSTEMMEM", "SCRATCH"};
    return p < 4 ? n[p] : "?";
}

static const char *decl_type(uint8_t t)
{
    static const char *const n[] = {"FLOAT1", "FLOAT2", "FLOAT3", "FLOAT4", "D3DCOLOR", "UBYTE4", "SHORT2",
        "SHORT4", "UBYTE4N", "SHORT2N", "SHORT4N", "USHORT2N", "USHORT4N", "UDEC3", "DEC3N", "FLOAT16_2",
        "FLOAT16_4", "UNUSED"};
    return t < 18 ? n[t] : "?";
}

static const char *decl_usage(uint8_t u)
{
    static const char *const n[] = {"POSITION", "BLENDWEIGHT", "BLENDINDICES", "NORMAL", "PSIZE", "TEXCOORD",
        "TANGENT", "BINORMAL", "TESSFACTOR", "POSITIONT", "COLOR", "FOG", "DEPTH", "SAMPLE"};
    return u < 14 ? n[u] : "?";
}

/* ------------------------------------------------------------------ objects the program holds */
/* what the inventory knows of a program object: shaders (hash, version), declarations (text) */
typedef struct { uint32_t guest; int kind; uint64_t hash; uint32_t ver; char *text; } Obj;
#define OCAP (1 << 15)
static Obj objs[OCAP];

static Obj *obj(uint32_t guest, int add)
{
    for (uint32_t i = (guest >> 4) * 2654435761u & (OCAP - 1), n = 0; n < OCAP; i = (i + 1) & (OCAP - 1), n++) {
        if (objs[i].guest == guest) return &objs[i];
        if (!objs[i].guest) {
            if (!add) return NULL;
            objs[i].guest = guest;
            return &objs[i];
        }
    }
    return NULL;
}

/* a texture's or surface's description through the device's own object (host vtables) */
typedef struct { uint32_t type, fmt, usage, pool, w, h, ms; } Desc;
#define VTH(o) (*(void ***)(o))
static int desc_of(uint32_t guest, Desc *d)
{
    memset(d, 0, sizeof *d);
    if (!guest || R32(guest + 4) != COM_MAGIC) return 0;
    void *h = com_host(guest);
    if (!h) return 0;
    d->type = ((uint32_t (*)(void *))VTH(h)[10])(h);        /* IDirect3DResource9::GetType */
    D3DSURFACE_DESC sd;
    if (d->type == D3DRTYPE_SURFACE) {
        if (((HRESULT (*)(void *, D3DSURFACE_DESC *))VTH(h)[12])(h, &sd) < 0) return 0;
    } else if (d->type == D3DRTYPE_TEXTURE || d->type == D3DRTYPE_CUBETEXTURE) {
        if (((HRESULT (*)(void *, UINT, D3DSURFACE_DESC *))VTH(h)[17])(h, 0, &sd) < 0) return 0;
    } else if (d->type == D3DRTYPE_VOLUMETEXTURE) {
        D3DVOLUME_DESC vd;
        if (((HRESULT (*)(void *, UINT, D3DVOLUME_DESC *))VTH(h)[17])(h, 0, &vd) < 0) return 0;
        d->fmt = vd.Format; d->usage = vd.Usage; d->pool = vd.Pool; d->w = vd.Width; d->h = vd.Height;
        return 1;
    } else if (d->type == D3DRTYPE_VERTEXBUFFER) {
        D3DVERTEXBUFFER_DESC bd;
        ((HRESULT (*)(void *, D3DVERTEXBUFFER_DESC *))VTH(h)[13])(h, &bd);
        d->fmt = bd.Format; d->usage = bd.Usage; d->pool = bd.Pool; d->w = bd.Size;
        return 1;
    } else if (d->type == D3DRTYPE_INDEXBUFFER) {
        D3DINDEXBUFFER_DESC bd;
        ((HRESULT (*)(void *, D3DINDEXBUFFER_DESC *))VTH(h)[13])(h, &bd);
        d->fmt = bd.Format; d->usage = bd.Usage; d->pool = bd.Pool; d->w = bd.Size;
        return 1;
    } else return 0;
    d->fmt = sd.Format; d->usage = sd.Usage; d->pool = sd.Pool; d->w = sd.Width; d->h = sd.Height;
    d->ms = sd.MultiSampleType;
    return 1;
}

static const char *rtype_name(uint32_t t)
{
    static const char *const n[] = {"?", "SURFACE", "VOLUME", "TEXTURE", "VOLUMETEXTURE", "CUBETEXTURE", "VB", "IB"};
    return t < 8 ? n[t] : "?";
}

/* shader bytecode length in tokens (d3d9.c shader_tokens) */
static uint32_t sh_tokens(const uint32_t *w)
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

/* ------------------------------------------------------------------ state shadowed for the keys */
static uint32_t rs[256];
static uint32_t cur_vs, cur_ps, cur_decl, cur_fvf, strides[4];
static uint32_t tex_bound[16];
/* fixed function: the texture stages, the lights (type, enabled) by index */
static uint32_t tss[8][33];
#define NLIGHT 64
static uint32_t light_type[NLIGHT];
static uint8_t light_on[NLIGHT];
static const char *top_name(uint32_t o)
{
    static const char *const n[] = {"?", "DISABLE", "SELECTARG1", "SELECTARG2", "MODULATE", "MODULATE2X", "MODULATE4X",
        "ADD", "ADDSIGNED", "ADDSIGNED2X", "SUBTRACT", "ADDSMOOTH", "BLENDDIFFUSEALPHA", "BLENDTEXTUREALPHA",
        "BLENDFACTORALPHA", "BLENDTEXTUREALPHAPM", "BLENDCURRENTALPHA", "PREMODULATE", "MODULATEALPHA_ADDCOLOR",
        "MODULATECOLOR_ADDALPHA", "MODULATEINVALPHA_ADDCOLOR", "MODULATEINVCOLOR_ADDALPHA", "BUMPENVMAP",
        "BUMPENVMAPLUMINANCE", "DOTPRODUCT3", "MULTIPLYADD", "LERP"};
    return o < 27 ? n[o] : "?";
}
static void ta_text(char *b, size_t n, uint32_t a)
{
    static const char *const sel[] = {"DIFFUSE", "CURRENT", "TEXTURE", "TFACTOR", "SPECULAR", "TEMP", "CONSTANT", "?"};
    snprintf(b, n, "%s%s%s", (a & 0x10) ? "1-" : "", sel[a & 7], (a & 0x20) ? ".a" : "");
}
static uint32_t rt_fmt, rt_ms, ds_fmt, ds_ms;
/* the frame's distinct pipeline keys */
static uint64_t fkeys[8192];
static int nfkeys;
static unsigned long keyframes[NWIN], keysum[NWIN], keymax[NWIN], draws_win[NWIN];
static unsigned long last_frame = ~0ul;

static int in_win(int w, unsigned long f) { return f >= inv_from[w] && f < inv_to[w]; }

static void frame_turn(void)
{
    if (frame_count == last_frame) return;
    for (int w = 0; w < nwin; w++)
        if (last_frame != ~0ul && in_win(w, last_frame)) {
            keyframes[w]++;
            keysum[w] += (unsigned long)nfkeys;
            if ((unsigned long)nfkeys > keymax[w]) keymax[w] = (unsigned long)nfkeys;
        }
    nfkeys = 0;
    last_frame = frame_count;
}

static uint64_t fnv(uint64_t h, uint32_t v)
{
    for (int i = 0; i < 4; i++) h = (h ^ ((v >> (8 * i)) & 0xff)) * 0x100000001b3ull;
    return h;
}

/* the states a Vulkan pipeline holds (VULKAN.md "State"); the rest are dynamic or shader constants */
static const uint8_t pipe_rs[] = {7, 8, 14, 15, 19, 20, 22, 23, 25, 27, 28, 35, 52, 53, 54, 55, 56, 140, 152,
                                  161, 168, 171, 175, 185, 186, 187, 188, 189, 194, 195, 206, 207, 208, 209};

static void draw_key(int prim, uint32_t up_stride)
{
    Obj *v = cur_vs ? obj(cur_vs, 0) : NULL, *p = cur_ps ? obj(cur_ps, 0) : NULL;
    uint64_t h = 0xcbf29ce484222325ull;
    h = fnv(h, v ? (uint32_t)v->hash : 0); h = fnv(h, p ? (uint32_t)p->hash : 0);
    h = fnv(h, cur_decl); h = fnv(h, cur_fvf); h = fnv(h, up_stride ? up_stride : strides[0]);
    h = fnv(h, strides[1]); h = fnv(h, prim == 2 || prim == 3 ? 2 : prim == 1 ? 1 : 4);
    for (size_t i = 0; i < sizeof pipe_rs; i++) h = fnv(h, rs[pipe_rs[i]]);
    h = fnv(h, rt_fmt); h = fnv(h, rt_ms); h = fnv(h, ds_fmt); h = fnv(h, ds_ms);
    count("PIPEKEY %016llx", (unsigned long long)h);
    int i;
    for (i = 0; i < nfkeys && fkeys[i] != h; i++) ;
    if (i == nfkeys && nfkeys < 8192) fkeys[nfkeys++] = h;
    for (int w = 0; w < nwin; w++) draws_win[w] += in_win(w, frame_count);
    /* the shader pair and what its samplers have bound */
    char tb[200];
    int o = 0;
    for (int s = 0; s < 8; s++) {
        if (!tex_bound[s]) continue;
        Desc d;
        if (desc_of(tex_bound[s], &d))
            o += snprintf(tb + o, sizeof tb - o, " s%d=%s/%s%s", s, d.type == 5 ? "cube" : d.type == 4 ? "vol" : "2d",
                          fmt_name(d.fmt), d.usage & D3DUSAGE_RENDERTARGET ? "/rt" : d.usage & D3DUSAGE_DEPTHSTENCIL ? "/ds" : "");
    }
    tb[o] = 0;
    /* fixed function (no vertex shader, or pre-transformed vertices; no pixel shader) */
    int post = cur_fvf ? (cur_fvf & 0xe) == 4 : 0;
    if (!cur_fvf && cur_decl) {
        Obj *d = obj(cur_decl, 0);
        post = d && d->text && strstr(d->text, "POSITIONT") != NULL;
    }
    if (!v || post) {
        char lt[200], tc[200];
        int lo = 0, nl = 0, to = 0;
        for (int i = 0; i < NLIGHT && lo < 180; i++)
            if (light_on[i]) {
                nl++;
                lo += snprintf(lt + lo, sizeof lt - lo, "%s%s", lo ? "," : "",
                               light_type[i] == 1 ? "point" : light_type[i] == 2 ? "spot" : light_type[i] == 3 ? "dir" : "?");
            }
        lt[lo] = 0;
        for (int st = 0; st < 8 && to < 180; st++) {
            if (tss[st][1] == 1) break;                     /* (COLOROP DISABLE: the stages after it are off) */
            to += snprintf(tc + to, sizeof tc - to, " t%d=%x/%x", st, tss[st][11], tss[st][24]);
        }
        tc[to] = 0;
        count("FFVS %s%s lit %u lights %d [%s] colorvertex %u mcs %u/%u/%u/%u normalize %u localviewer %u specular %u "
              "fog %u vmode %u tmode %u range %u vblend %u pointscale %u tci/ttff%s",
              post ? "POSITIONT " : "", cur_fvf ? "fvf" : "decl", rs[137], nl, lt, rs[141], rs[145], rs[147], rs[146],
              rs[148], rs[143], rs[142], rs[29], rs[28], rs[140], rs[35], rs[48], rs[151], rs[157], tc);
        if (post || !v) count("FFVS lights enabled %d", nl);
    }
    if (!p) {
        char st[600];
        int o = 0;
        for (int i = 0; i < 8 && o < 520; i++) {
            if (tss[i][1] == 1) break;
            char c1[24], c2[24], c0[24], a1[24], a2[24], a0[24];
            ta_text(c1, sizeof c1, tss[i][2]); ta_text(c2, sizeof c2, tss[i][3]); ta_text(c0, sizeof c0, tss[i][26]);
            ta_text(a1, sizeof a1, tss[i][5]); ta_text(a2, sizeof a2, tss[i][6]); ta_text(a0, sizeof a0, tss[i][27]);
            o += snprintf(st + o, sizeof st - o, " [%d%s c %s(%s,%s,%s) a %s(%s,%s,%s)%s]", i, tex_bound[i] ? "" : " notex",
                          top_name(tss[i][1]), c1, c2, c0, top_name(tss[i][4]), a1, a2, a0, tss[i][28] == 5 ? " ->TEMP" : "");
        }
        st[o] = 0;
        count("FFPS alphatest %u/%u specular %u fog %u/%u%s", rs[15], rs[25], rs[29], rs[28], rs[35], st);
        for (int i = 0; i < 8; i++) {
            if (tss[i][1] == 1) break;
            count("FFPS op %s", top_name(tss[i][1]));
            count("FFPS op %s", top_name(tss[i][4]));
            for (int a = 0; a < 6; a++) {
                static const int ai[6] = {2, 3, 26, 5, 6, 27};
                char b[24];
                ta_text(b, sizeof b, tss[i][ai[a]]);
                count("FFPS arg %s", b);
            }
            if (tss[i][28] != 1) count("FFPS resultarg %u", tss[i][28]);
        }
    }
    count("DRAWPAIR vs %016llx ps %016llx fog %u/%u/%u at %u:%s",
          (unsigned long long)(v ? v->hash : 0), (unsigned long long)(p ? p->hash : 0), rs[28], rs[35], rs[140],
          rs[15], tb);
}

/* ------------------------------------------------------------------ the call hook */
static __thread uint32_t pend_out;      /* an object out-pointer to read after the call */
static __thread int pend_kind;          /* 1 vs, 2 ps, 3 declaration */
static __thread uint64_t pend_hash;
static __thread uint32_t pend_ver;
static __thread char *pend_text;

void d3dinv_call(CPU *c, const char *n)
{
    if (strncmp(n, "IDirect3D", 9)) return;
    pthread_mutex_lock(&ilock);
    frame_turn();
    count("M %s", n);
    const char *m = strstr(n, "::");
    m = m ? m + 2 : n;
    int dev = !strncmp(n, "IDirect3DDevice9::", 18);
    uint32_t a1 = ARG(1), a2 = ARG(2), a3 = ARG(3), a4 = ARG(4), a5 = ARG(5), a6 = ARG(6), a7 = ARG(7);
    if (dev && !strcmp(m, "SetRenderState")) {
        if (a1 < 256) rs[a1] = a2;
        if (a1 == 34 || a1 == 60 || a1 == 139 || a1 == 193)
            count("RS %3u %-24s = %08x", a1, rs_name(a1), a2);
        else if (a1 == 36 || a1 == 37 || a1 == 38 || a1 == 154 || a1 == 155 || a1 == 166 || a1 == 175 || a1 == 195) {
            float f; memcpy(&f, &a2, 4);
            count("RS %3u %-24s = %g (float)", a1, rs_name(a1), f);
        } else
            count("RS %3u %-24s = %u (0x%x)", a1, rs_name(a1), a2, a2);
    } else if (dev && !strcmp(m, "SetSamplerState")) {
        if (a2 == 8) { float f; memcpy(&f, &a3, 4); count("SS %-14s = %g (float)", ss_name(a2), f); }
        else count("SS %-14s = %u (0x%x)", ss_name(a2), a3, a3);
        count("SS sampler %u", a1);
    } else if (dev && !strcmp(m, "SetTextureStageState")) {
        if (a1 < 8 && a2 < 33) tss[a1][a2] = a3;
        if (a2 >= 7 && a2 <= 10) count("TSS stage %u %-22s = (float values)", a1, tss_name(a2));
        else count("TSS stage %u %-22s = %u (0x%x)", a1, tss_name(a2), a3, a3);
    } else if (dev && !strcmp(m, "SetTexture")) {
        Desc d;
        if (a1 < 16) tex_bound[a1] = a2;
        if (!a2) count("SETTEX stage %u NULL", a1);
        else if (desc_of(a2, &d))
            count("SETTEX stage %u %s %s usage %x pool %s", a1, rtype_name(d.type), fmt_name(d.fmt), d.usage, pool_name(d.pool));
    } else if (dev && (!strcmp(m, "SetVertexShader") || !strcmp(m, "SetPixelShader"))) {
        int ps = m[3] == 'P';
        if (ps) cur_ps = a1; else cur_vs = a1;
        Obj *o = a1 ? obj(a1, 0) : NULL;
        count("SETSHADER %s %s", ps ? "ps" : "vs", !a1 ? "NULL (fixed function)" : o ? "set" : "unknown");
    } else if (dev && !strcmp(m, "SetFVF")) {
        cur_fvf = a1;
        count("FVF %08x", a1);
    } else if (dev && !strcmp(m, "SetVertexDeclaration")) {
        cur_decl = a1;
        cur_fvf = 0;
        Obj *o = a1 ? obj(a1, 0) : NULL;
        count("SETDECL %s", o && o->text ? o->text : "?");
    } else if (dev && !strcmp(m, "SetStreamSource")) {
        if (a1 < 4) strides[a1] = a4;
        Desc d;
        if (a2 && desc_of(a2, &d))
            count("STREAM %u stride %u (VB usage %x pool %s)", a1, a4, d.usage, pool_name(d.pool));
        else
            count("STREAM %u stride %u (%s)", a1, a4, a2 ? "?" : "NULL");
    } else if (dev && !strcmp(m, "SetStreamSourceFreq")) {
        count("STREAMFREQ %u = %x", a1, a2);
    } else if (dev && !strcmp(m, "SetIndices")) {
        Desc d;
        if (a1 && desc_of(a1, &d)) count("INDICES %s usage %x pool %s", fmt_name(d.fmt), d.usage, pool_name(d.pool));
    } else if (dev && !strcmp(m, "DrawPrimitive")) {
        count("DRAW DrawPrimitive %s", prim_name(a1));
        draw_key((int)a1, 0);
    } else if (dev && !strcmp(m, "DrawIndexedPrimitive")) {
        count("DRAW DrawIndexedPrimitive %s", prim_name(a1));
        draw_key((int)a1, 0);
    } else if (dev && !strcmp(m, "DrawPrimitiveUP")) {
        count("DRAW DrawPrimitiveUP %s stride %u", prim_name(a1), a4);
        draw_key((int)a1, a4);
    } else if (dev && !strcmp(m, "DrawIndexedPrimitiveUP")) {
        count("DRAW DrawIndexedPrimitiveUP %s %s stride %u", prim_name(a1), fmt_name(a6), ARG(8));
        draw_key((int)a1, ARG(8));
    } else if (dev && !strcmp(m, "Clear")) {
        count("CLEAR flags %x rects %u", a3, a1);
    } else if (dev && !strcmp(m, "StretchRect")) {
        Desc s, d;
        desc_of(a1, &s);
        desc_of(a3, &d);
        count("STRETCHRECT %s %ux%u ms%u -> %s %ux%u ms%u, rects %s/%s, filter %u", fmt_name(s.fmt), s.w, s.h, s.ms,
              fmt_name(d.fmt), d.w, d.h, d.ms, a2 ? "src" : "-", a4 ? "dst" : "-", a5);
    } else if (dev && !strcmp(m, "SetRenderTarget")) {
        Desc d;
        if (a2 && desc_of(a2, &d)) {
            if (a1 == 0) rt_fmt = d.fmt, rt_ms = d.ms;
            count("SETRT %u %s %ux%u usage %x", a1, fmt_name(d.fmt), d.w, d.h, d.usage);
        } else count("SETRT %u NULL", a1);
    } else if (dev && !strcmp(m, "SetDepthStencilSurface")) {
        Desc d;
        if (a1 && desc_of(a1, &d)) {
            ds_fmt = d.fmt, ds_ms = d.ms;
            count("SETDS %s %ux%u", fmt_name(d.fmt), d.w, d.h);
        } else {
            ds_fmt = ds_ms = 0;
            count("SETDS NULL");
        }
    } else if (dev && !strcmp(m, "SetTransform")) {
        count("XFORM %u", a1);
    } else if (dev && !strcmp(m, "SetMaterial")) {
        float pw;
        memcpy(&pw, gptr(a1 + 64), 4);
        count("MATERIAL power %s", pw == 0 ? "0" : "nonzero");
    } else if (dev && !strcmp(m, "SetLight")) {
        uint32_t t = R32(a2);
        if (a1 < NLIGHT) light_type[a1] = t;
        count("LIGHT type %u index %s", t, a1 < 8 ? "0-7" : "8+");
    } else if (dev && !strcmp(m, "LightEnable")) {
        if (a1 < NLIGHT) light_on[a1] = a2 != 0;
        count("LIGHTENABLE %s", a2 ? "on" : "off");
    } else if (dev && (!strcmp(m, "SetVertexShaderConstantF") || !strcmp(m, "SetPixelShaderConstantF") ||
                       !strcmp(m, "SetVertexShaderConstantI") || !strcmp(m, "SetPixelShaderConstantI") ||
                       !strcmp(m, "SetVertexShaderConstantB") || !strcmp(m, "SetPixelShaderConstantB"))) {
        count("CONST %s c%u..c%u", m + 3, a1, a1 + a3 - 1);
    } else if (dev && !strcmp(m, "CreateTexture")) {
        count("CREATE TEXTURE %s usage %x pool %s levels %u", fmt_name(a5), a4, pool_name(a6), a3);
        if (a4 & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL | D3DUSAGE_DYNAMIC))
            count("CREATE TEXTURE %s usage %x %ux%u", fmt_name(a5), a4, a1, a2);
    } else if (dev && !strcmp(m, "CreateCubeTexture")) {
        count("CREATE CUBE %s usage %x pool %s edge %u levels %u", fmt_name(a4), a3, pool_name(a5), a1, a2);
    } else if (dev && !strcmp(m, "CreateVolumeTexture")) {
        count("CREATE VOLUME %s usage %x pool %s %ux%ux%u levels %u", fmt_name(a6), a5, pool_name(a7), a1, a2, a3, a4);
    } else if (dev && !strcmp(m, "CreateVertexBuffer")) {
        count("CREATE VB usage %x fvf %x pool %s%s", a2, a3, pool_name(a4), a1 >= 65536 ? " (64 KB+)" : "");
    } else if (dev && !strcmp(m, "CreateIndexBuffer")) {
        count("CREATE IB %s usage %x pool %s", fmt_name(a3), a2, pool_name(a4));
    } else if (dev && !strcmp(m, "CreateRenderTarget")) {
        count("CREATE RENDERTARGET %s %ux%u ms %u lockable %u", fmt_name(a3), a1, a2, a4, a6);
    } else if (dev && !strcmp(m, "CreateDepthStencilSurface")) {
        count("CREATE DEPTHSTENCIL %s %ux%u ms %u discard %u", fmt_name(a3), a1, a2, a4, a6);
    } else if (dev && !strcmp(m, "CreateOffscreenPlainSurface")) {
        count("CREATE OFFSCREEN %s %ux%u pool %s", fmt_name(a3), a1, a2, pool_name(a4));
    } else if (dev && !strcmp(m, "CreateQuery")) {
        count("CREATE QUERY type %u", a1);
    } else if (dev && !strcmp(m, "CreateStateBlock")) {
        count("CREATE STATEBLOCK type %u", a1);
    } else if (dev && (!strcmp(m, "CreateVertexShader") || !strcmp(m, "CreatePixelShader"))) {
        const uint32_t *w = (const uint32_t *)gptr(a1);
        uint32_t nt = a1 ? sh_tokens(w) : 0;
        uint64_t h = 0xcbf29ce484222325ull;
        for (uint32_t i = 0; i < nt * 4; i++) h = (h ^ ((const uint8_t *)w)[i]) * 0x100000001b3ull;
        pend_kind = m[6] == 'P' ? 2 : 1;
        pend_hash = h;
        pend_ver = nt ? w[0] : 0;
        pend_out = a2;
        count("CREATE SHADER %s_%u_%u %016llx", pend_kind == 2 ? "ps" : "vs", (pend_ver >> 8) & 0xff, pend_ver & 0xff,
              (unsigned long long)h);
        if (nt) {
            char path[1300];
            snprintf(path, sizeof path, "%s/shaders", user_dir);
            mkdir(path, 0755);
            snprintf(path, sizeof path, "%s/shaders/%016llx.%s.bin", user_dir, (unsigned long long)h, pend_kind == 2 ? "psh" : "vsh");
            FILE *sf = fopen(path, "wb");
            if (sf) { fwrite(w, 4, nt, sf); fclose(sf); }
        }
    } else if (dev && !strcmp(m, "CreateVertexDeclaration")) {
        char t[400];
        int o = 0;
        for (uint32_t p = a1; p && R16(p) != 0xff && o < 360; p += 8)
            o += snprintf(t + o, sizeof t - o, "%s%u+%u %s %s%u", o ? ", " : "", R16(p), R16(p + 2),
                          decl_type(R8(p + 4)), decl_usage(R8(p + 6)), R8(p + 7));
        t[o] = 0;
        count("CREATE DECL %s", t);
        pend_kind = 3;
        pend_text = strdup(t);
        pend_out = a2;
    } else if (!strcmp(m, "Lock") || !strcmp(m, "LockRect") || !strcmp(m, "LockBox")) {
        Desc d;
        uint32_t flags = !strcmp(m, "Lock") ? a4 : !strncmp(n, "IDirect3DSurface9", 17) ? a3 :
                         !strncmp(n, "IDirect3DCubeTexture9", 21) ? a5 : a4;
        if (desc_of(ARG(0), &d))
            count("LOCK %s %s usage %x pool %s flags %x", rtype_name(d.type), d.type >= 6 ? "" : fmt_name(d.fmt),
                  d.usage, pool_name(d.pool), flags);
    } else if (dev && (!strcmp(m, "GetRenderTargetData") || !strcmp(m, "UpdateSurface") || !strcmp(m, "UpdateTexture") ||
                       !strcmp(m, "ColorFill"))) {
        Desc s;
        desc_of(a1, &s);
        count("COPY %s %s %ux%u", m, fmt_name(s.fmt), s.w, s.h);
    } else if (!strncmp(n, "IDirect3D9::", 12) && !strcmp(m, "CheckDeviceFormat")) {
        count("CHECKFORMAT adapterfmt %s usage %x rtype %s fmt %s", fmt_name(a3), a4, rtype_name(a5), fmt_name(a6));
    } else if (!strncmp(n, "IDirect3D9::", 12) && !strcmp(m, "CheckDeviceMultiSampleType")) {
        count("CHECKMS fmt %s ms %u", fmt_name(a3), a5);
    } else if (!strncmp(n, "IDirect3DQuery9::", 17) && !strcmp(m, "Issue")) {
        count("QUERY Issue flags %x", a1);
    }
    (void)a6; (void)a7;
    pthread_mutex_unlock(&ilock);
}

void d3dinv_after(CPU *c)
{
    (void)c;
    if (!pend_kind) return;
    pthread_mutex_lock(&ilock);
    uint32_t g = pend_out ? R32(pend_out) : 0;
    Obj *o = g ? obj(g, 1) : NULL;
    if (o) {
        o->kind = pend_kind;
        o->hash = pend_hash;
        o->ver = pend_ver;
        free(o->text);
        o->text = pend_text;
        pend_text = NULL;
    }
    free(pend_text);
    pend_text = NULL;
    pend_kind = 0;
    pthread_mutex_unlock(&ilock);
}

/* ------------------------------------------------------------------ the report */
static int by_key(const void *a, const void *b)
{
    const Cnt *x = a, *y = b;
    return strcmp(x->key, y->key);
}

static void inv_write(void)
{
    pthread_mutex_lock(&ilock);
    frame_turn();
    last_frame = ~0ul;
    Cnt *all = malloc(sizeof(Cnt) * (size_t)(ncnt ? ncnt : 1));
    int k = 0;
    for (int i = 0; i < CCAP; i++)
        if (cnt[i].key) all[k++] = cnt[i];
    qsort(all, (size_t)k, sizeof *all, by_key);
    for (int w = 0; w < nwin; w++) {
        char path[1200];
        snprintf(path, sizeof path, "%s/d3d_inventory_%lu-%lu.txt", user_dir, inv_from[w], inv_to[w]);
        FILE *f = fopen(path, "w");
        if (!f) continue;
        unsigned long nw = inv_to[w] > inv_from[w] ? inv_to[w] - inv_from[w] : 1;
        if (frame_count < inv_to[w] && frame_count > inv_from[w]) nw = frame_count - inv_from[w];
        fprintf(f, "# Direct3D inventory (runtime/d3d9_inv.c): frames %lu-%lu of %lu; columns: whole run, window, window per frame\n",
                inv_from[w], inv_to[w], frame_count);
        fprintf(f, "# pipeline keys: %lu draws in the window; distinct keys a frame: mean %.1f, max %lu\n", draws_win[w],
                keyframes[w] ? (double)keysum[w] / keyframes[w] : 0.0, keymax[w]);
        unsigned long pk_all = 0, pk_win = 0;
        for (int i = 0; i < k; i++) {
            if (!strncmp(all[i].key, "PIPEKEY", 7)) { pk_all++; pk_win += all[i].win[w] > 0; continue; }
            fprintf(f, "%10lu %10lu %10.2f  %s\n", all[i].all, all[i].win[w], (double)all[i].win[w] / nw, all[i].key);
        }
        fprintf(f, "# distinct pipeline keys: %lu in the run, %lu in the window\n", pk_all, pk_win);
        fclose(f);
        LOG(LOG_ALWAYS, "d3d inventory written to %s", path);
    }
    free(all);
    pthread_mutex_unlock(&ilock);
}

__attribute__((constructor)) static void inv_init(void)
{
    const char *e = getenv("ETM_D3D_INV");
    if (!e || !*e) return;
    while (*e && nwin < NWIN) {
        inv_from[nwin] = strtoul(e, (char **)&e, 10);
        inv_to[nwin] = *e == '-' ? strtoul(e + 1, (char **)&e, 10) : inv_from[nwin] + 300;
        nwin++;
        while (*e && *e != ',') e++;
        if (*e == ',') e++;
    }
    d3dinv_on = 1;
    for (int i = 0; i < 256; i++) rs[i] = 0xdeadbeef;
    for (int t = 0; t < 8; t++) {           /* (Direct3D's defaults) */
        tss[t][1] = t ? 1 : 4; tss[t][4] = t ? 1 : 2;
        tss[t][2] = tss[t][5] = 2; tss[t][3] = tss[t][6] = tss[t][26] = tss[t][27] = tss[t][28] = 1;
        tss[t][11] = (uint32_t)t;
    }
    atexit(inv_write);
}
