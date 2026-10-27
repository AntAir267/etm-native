/* INTERPOLATION (from larush-native; ETM: decomp/frame.c): with frames drawn between simulation
   steps, what moves is drawn where it would be at that moment: after every step the fields that
   place things (interp_fields) are copied; before a frame is drawn each one gets the blend of its
   last two copies (frame_blend: 0 the previous step, 1 the latest), and after it the latest is
   put back, so the simulation never sees a blended value. A field that jumped (a respawn, a
   reset camera) is drawn at its latest value. */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "interp.h"

#define MAXF 4096
typedef struct { uint32_t addr; int kind; float prev[16], cur[16]; int have; unsigned long step; } Field;
static Field fields[MAXF];
static int nfields;
static unsigned long step_no;
static int applied;
float interp_jump = 50.0f;                       /* a move longer than this in a step is a jump */

static int kind_floats(int kind)
{
    switch (kind) {
    case IK_VEC3: case IK_KEEP3: return 3;
    case IK_QUAT: return 4;
    case IK_MAT43: case IK_MAT34: return 12;
    case IK_MAT44: case IK_KEEP16: return 16;
    }
    return 1;
}

/* fields by address: open addressing over indices into fields[], rebuilt after each step */
#define HSIZE 16384
static int16_t hidx[HSIZE];
static int hash_built;
static uint32_t hslot_of(uint32_t addr) { return (addr * 2654435761u) >> 18; }
static void hash_rebuild(void)
{
    memset(hidx, 0xff, sizeof hidx);
    for (int i = 0; i < nfields; i++) {
        uint32_t h = hslot_of(fields[i].addr);
        while (hidx[h] >= 0) h = (h + 1) & (HSIZE - 1);
        hidx[h] = (int16_t)i;
    }
    hash_built = 1;
}

static Field *field_of(uint32_t addr, int kind)
{
    if (!hash_built) hash_rebuild();
    uint32_t h = hslot_of(addr);
    for (; hidx[h] >= 0; h = (h + 1) & (HSIZE - 1))
        if (fields[hidx[h]].addr == addr && fields[hidx[h]].kind == kind) return &fields[hidx[h]];
    if (nfields == MAXF) return NULL;
    Field *f = &fields[nfields];
    memset(f, 0, sizeof *f);
    f->addr = addr;
    f->kind = kind;
    hidx[h] = (int16_t)nfields++;
    return f;
}

/* after a step: each field the lists name, copied (the previous copy kept) */
void interp_track(uint32_t addr, int kind)
{
    Field *f = field_of(addr, kind);
    if (!f) return;
    int n = kind_floats(kind);
    if (f->have && f->step == step_no - 1) memcpy(f->prev, f->cur, 4u * (uint32_t)n);
    else f->have = 0;
    memcpy(f->cur, gptr(addr), 4u * (uint32_t)n);
    if (!f->have) { memcpy(f->prev, f->cur, 4u * (uint32_t)n); f->have = 1; }
    f->step = step_no;
}

void interp_step_begin(void) { step_no++; }

/* fields not seen in the last step are dropped (their objects went) */
void interp_step_end(void)
{
    int k = 0;
    for (int i = 0; i < nfields; i++)
        if (fields[i].step == step_no) fields[k++] = fields[i];
    if (k != nfields) hash_built = 0;           /* (indices moved) */
    nfields = k;
}

static float lerp(float a, float b, float t) { return a + (b - a) * t; }

static int jumped(const Field *f)
{
    float dx, dy, dz;
    if (f->kind == IK_VEC3 || f->kind == IK_MAT43 || f->kind == IK_MAT44) {
        int o = f->kind == IK_VEC3 ? 0 : f->kind == IK_MAT43 ? 9 : 12;   /* the translation */
        dx = f->cur[o] - f->prev[o], dy = f->cur[o + 1] - f->prev[o + 1], dz = f->cur[o + 2] - f->prev[o + 2];
    } else if (f->kind == IK_MAT34) {
        dx = f->cur[3] - f->prev[3], dy = f->cur[7] - f->prev[7], dz = f->cur[11] - f->prev[11];
    } else {
        return 0;
    }
    return dx * dx + dy * dy + dz * dz > interp_jump * interp_jump;  /* (over interp_jump units in a step) */
}

/* before drawing: the blends written */
void interp_apply(double t)
{
    if (applied) return;
    applied = 1;
    float u = (float)(t < 0 ? 0 : t > 1 ? 1 : t);
    for (int i = 0; i < nfields; i++) {
        Field *f = &fields[i];
        if (f->step != step_no || jumped(f) || f->kind == IK_KEEP3 || f->kind == IK_KEEP16) continue;
        int n = kind_floats(f->kind);
        float v[16];
        if (f->kind == IK_QUAT) {                       /* normalised lerp, the short way round */
            float d = f->prev[0] * f->cur[0] + f->prev[1] * f->cur[1] + f->prev[2] * f->cur[2] + f->prev[3] * f->cur[3];
            float s = d < 0 ? -1.0f : 1.0f, len = 0;
            for (int k = 0; k < 4; k++) { v[k] = lerp(f->prev[k] * s, f->cur[k], u); len += v[k] * v[k]; }
            len = len > 0 ? 1.0f / sqrtf(len) : 1.0f;
            for (int k = 0; k < 4; k++) v[k] *= len;
        } else {
            for (int k = 0; k < n; k++) v[k] = lerp(f->prev[k], f->cur[k], u);
        }
        memcpy(gptr(f->addr), v, 4u * (uint32_t)n);
    }
}

/* after drawing: the latest step's values back */
void interp_restore(void)
{
    if (!applied) return;
    applied = 0;
    for (int i = 0; i < nfields; i++) {
        Field *f = &fields[i];
        if (f->step == step_no)
            memcpy(gptr(f->addr), f->cur, 4u * (uint32_t)kind_floats(f->kind));
    }
}
