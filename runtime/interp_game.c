/* WHAT MOVES in Enter the Matrix, for frames drawn between updates (interp.c, decomp/frame.c):
   after each update the fields that place what the render pass draws are copied; interp.c blends
   them before the render pass and puts the latest back after it.
   Game units: the camera sits at thousands of units (the lobby camera at about 7400, 1450, 5140)
   [verified: a memory dump at update 4500]; a move of over 300 units in an update, or a matrix
   element changing by over 0.5 (a turn of about 30 degrees), is a cut, drawn unblended. */
#include <stdlib.h>
#include <string.h>
#include "runtime.h"
#include "interp.h"

/* the views [0x97f230] (next at +0x4f0), each copied to 0x97f234 for the render pass; what it
   reads of the camera [verified: the world rewrite, runtime/decomp/world.h and drawlist.c] */
#define VIEW_LIST        0x97f230u
#define VIEW_NEXT        0x4f0u
#define VIEW_VIEWPROJ    0xd4u      /* 4x4: view x projection */
#define VIEW_PLANES      0x114u     /* the frustum's 6 planes (normal, distance) */
#define VIEW_MATRIX      0x240u     /* 4x4: the view matrix */
#define VIEW_POSITION    0x2d4u     /* vec3: the camera's position */

void interp_game_update(void)
{
    interp_jump = 300.0f;
    interp_turn = 0.5f;
    interp_step_begin();
    for (uint32_t v = R32(VIEW_LIST); v; v = R32(v + VIEW_NEXT)) {
        interp_track_group(v + VIEW_VIEWPROJ, IK_MAT44N, v);
        interp_track_group(v + VIEW_MATRIX, IK_MAT44R, v);
        interp_track_group(v + VIEW_POSITION, IK_VEC3, v);
        for (uint32_t k = 0; k < 6; k++)
            interp_track_group(v + VIEW_PLANES + 16 * k, IK_VEC4, v);
    }
    interp_step_end();
}

/* CHARACTERS: posed at draw time (skin.c 0x6d0f2d, pose.c), and that code also writes game state
   (root motion, kept bone positions, held props), so it runs as the original does, from the true
   state, on every drawn frame (the same state poses the same: running it again on a frame without
   updates changes nothing [verified: tests/rate.sh, memory at update 4500 the same at 60 and 120]).
   What is blended is its output, the skinning matrices handed to the vertex shader: each
   character's last two poses (as of consecutive updates, by the state counter) by the frame's
   blend. A root bone moving over interp_jump, or a bone count change (level of detail), draws
   the latest. Held props likewise (their world matrices, from the same pose). */
#define STATE_COUNTER    0x97ec4cu  /* updates since the level began */
typedef struct { uint32_t ch, n; uint32_t stamp; int have_prev; float *prev, *cur; } Skin;
#define MAXSKIN 64
static Skin skins[MAXSKIN];

/* a 4x4 computed at draw time from the true state (so the same on every frame between two
   updates): blended with its value as of the previous update, put back after drawing */
typedef struct { uint32_t addr, stamp; int have_prev; float prev[16], cur[16]; } Drawn;
#define MAXDRAWN 512
static Drawn drawn[MAXDRAWN];
static void blend_drawn_mat(uint32_t addr)
{
    uint32_t stamp = R32(STATE_COUNTER);
    Drawn *d = NULL, *old = &drawn[0];
    for (int i = 0; i < MAXDRAWN && !d; i++) {
        if (drawn[i].addr == addr)
            d = &drawn[i];
        else if (drawn[i].stamp < old->stamp)
            old = &drawn[i];
    }
    if (!d) {
        d = old;
        d->addr = addr;
        d->stamp = 0;
    }
    if (stamp != d->stamp) {
        memcpy(d->prev, d->cur, sizeof d->prev);
        d->have_prev = d->stamp && stamp == d->stamp + 1;
        memcpy(d->cur, gptr(addr), sizeof d->cur);
        d->stamp = stamp;
    }
    if (!d->have_prev)
        return;
    float dx = d->cur[12] - d->prev[12], dy = d->cur[13] - d->prev[13], dz = d->cur[14] - d->prev[14];
    if (dx * dx + dy * dy + dz * dz > interp_jump * interp_jump)
        return;
    float u = (float)(frame_blend < 0 ? 0 : frame_blend > 1 ? 1 : frame_blend);
    interp_keep(addr, 64);
    float *out = (float *)gptr(addr);
    for (int k = 0; k < 16; k++)
        out[k] = d->prev[k] + (d->cur[k] - d->prev[k]) * u;
}

/* held props: placed at their bones by the character hook's notice 1 during the pose (charhook.c:
   each prop of the actor's list +0x380, next +0xd0, with an anchor +0x3bc; its world matrix +0x40) */
#define CHAR_ACTOR       0x164u
#define ACTOR_ATTACHED   0x1a4u
#define ACTOR_PROPS      0x380u
#define PROP_WORLD       0x40u
#define PROP_NEXT        0xd0u
#define PROP_ANCHOR      0x3bcu

void interp_skin(uint32_t ch, uint32_t buf, uint32_t nbones)
{
    if (!interp_active() || !nbones || nbones > 256)
        return;
    uint32_t stamp = R32(STATE_COUNTER);
    Skin *s = NULL, *old = &skins[0];
    for (int i = 0; i < MAXSKIN && !s; i++) {
        if (skins[i].ch == ch)
            s = &skins[i];
        else if (skins[i].stamp < old->stamp)
            old = &skins[i];
    }
    if (!s) {
        s = old;
        s->ch = ch;
        s->n = 0;
    }
    if (s->n != nbones) {
        free(s->prev);
        free(s->cur);
        s->prev = malloc(64u * nbones);
        s->cur = malloc(64u * nbones);
        s->n = nbones;
        s->stamp = 0;
        s->have_prev = 0;
    }
    if (stamp != s->stamp) {
        float *t = s->prev;
        s->prev = s->cur;
        s->cur = t;
        s->have_prev = s->stamp && stamp == s->stamp + 1;
        memcpy(s->cur, gptr(buf), 64u * nbones);
        s->stamp = stamp;
    }
    if (!s->have_prev)
        return;
    float dx = s->cur[12] - s->prev[12], dy = s->cur[13] - s->prev[13], dz = s->cur[14] - s->prev[14];
    if (dx * dx + dy * dy + dz * dz > interp_jump * interp_jump)
        return;
    float u = (float)(frame_blend < 0 ? 0 : frame_blend > 1 ? 1 : frame_blend);
    interp_keep(buf, 64u * nbones);                 /* (the true pose back after drawing) */
    float *out = (float *)gptr(buf);
    for (uint32_t k = 0; k < 16u * nbones; k++)
        out[k] = s->prev[k] + (s->cur[k] - s->prev[k]) * u;
    uint32_t actor = R32(ch + CHAR_ACTOR);
    if (actor && !R32(actor + ACTOR_ATTACHED))
        for (uint32_t prop = R32(actor + ACTOR_PROPS); prop; prop = R32(prop + PROP_NEXT))
            if (R32(prop + PROP_ANCHOR))
                blend_drawn_mat(prop + PROP_WORLD);
}
