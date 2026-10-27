/* Interpolation of what moves, for frames drawn between simulation steps (interp.c). */
#pragma once
#include <stdint.h>

/* IK_MAT34: three rows of four, the translation in the last column (the renderer's instance
   matrices). IK_KEEP*: copied after a step and put back after drawing, not blended (fields
   rebuilt from blended ones before drawing) */
enum { IK_FLOAT, IK_VEC3, IK_QUAT, IK_MAT43, IK_MAT44, IK_KEEP3, IK_KEEP16, IK_MAT34, IK_VEC4,
       IK_MAT44R,  /* a 4x4 whose jumps show in its rotation only (a view matrix: its translation is
                      the position turned into view space) */
       IK_MAT44N   /* a 4x4 not checked for jumps (left to its group) */ };
void interp_step_begin(void);
void interp_track(uint32_t addr, int kind);     /* after a step: copy this field */
/* the same for a field of a group (nonzero id): a jump in any of a group's fields draws all of
   them at their latest (a camera cut moves the matrices and the position together) */
void interp_track_group(uint32_t addr, int kind, uint32_t group);
void interp_step_end(void);
void interp_apply(double blend);                /* before drawing */
void interp_restore(void);                      /* after drawing */
extern float interp_jump;                       /* game units a step that count as a jump (not blended) */
extern float interp_turn;                       /* a matrix element changing more than this in a step:
                                                   a jump (a cut to another angle), 0 = not checked */

/* interp_game.c: this game's fields, after each update (frame.c) */
void interp_game_update(void);
extern double frame_blend;
