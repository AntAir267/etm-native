/* The Direct3D 9 bridge: the program's 32-bit D3D9 calls go to DXVK (native Linux build,
   Vulkan) through COM objects of our own (runtime/d3d9.c, generated gen/d3d9_gen.c). */
#pragma once
#include <d3d9.h>
#undef LOG
#include "runtime.h"
#include "d3d9_gen.h"

#define VT(o) (*(void ***)(o))

/* One program thread at a time in the device: the game draws its loading frames from a second thread
   while its main thread makes the level's textures and buffers [verified: frame logs], and our
   renderer records both into one command buffer (a crash in the driver at a level's start, 2026-10-30).
   Every Direct3D call from the program comes through the macros below. */
void d3d_lock(void);
void d3d_unlock(void);
#undef HOSTFN
#define HOSTFN(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    void fname(CPU *c) { d3d_lock(); uint32_t _r = fname##_body(c); d3d_unlock(); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)
#undef METHOD
#define METHOD(fname, nargs) \
    static uint32_t fname##_body(CPU *c); \
    static void fname(CPU *c) { d3d_lock(); uint32_t _r = fname##_body(c); d3d_unlock(); api_return(c, _r, nargs); } \
    static uint32_t fname##_body(CPU *c)

void *d3d_host(uint32_t guest);              /* the DXVK object behind a program object */
void *d3d_host_or_null(uint32_t guest);
uint32_t d3d_wrap(int kind, void *host);     /* the program object for a DXVK object (one each) */
uint32_t d3d_wrap_new(int kind, void *host); /* the same for an object just created */
void *d3d_hwnd(uint32_t hwnd);               /* a window handle as DXVK takes it (SDL_Window *) */
float d3d_argf(CPU *c, int n);               /* a float argument */
void d3d_unsupported(CPU *c, const char *name, int nargs);
extern ComClass d3d_classes[DK_COUNT];
void d3d_classes_init(void);
void msaa_device(void *dev);                 /* d3d9_msaa.c: after CreateDevice / Reset */
void msaa_reset(void);                       /* before Reset */
uint32_t msaa_render_state(uint32_t state, uint32_t value);   /* MULTISAMPLEANTIALIAS set */
