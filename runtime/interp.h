/* Interpolation of what moves, for frames drawn between simulation steps (interp.c). */
#pragma once
#include <stdint.h>

/* IK_MAT34: three rows of four, the translation in the last column (the renderer's instance
   matrices). IK_KEEP*: copied after a step and put back after drawing, not blended (fields
   rebuilt from blended ones before drawing) */
enum { IK_FLOAT, IK_VEC3, IK_QUAT, IK_MAT43, IK_MAT44, IK_KEEP3, IK_KEEP16, IK_MAT34 };
void interp_step_begin(void);
void interp_track(uint32_t addr, int kind);     /* after a step: copy this field */
void interp_step_end(void);
void interp_apply(double blend);                /* before drawing */
void interp_restore(void);                      /* after drawing */
extern float interp_jump;                       /* game units a step that count as a jump (not blended) */
