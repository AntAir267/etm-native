/* WHAT MOVES in Enter the Matrix, for frames drawn between updates (interp.c, decomp/frame.c):
   after each update the fields that place what the render pass draws are copied; interp.c blends
   them before the render pass and puts the latest back after it.
   Game units: the camera sits at thousands of units (the lobby camera at about 7400, 1450, 5140)
   [verified: a memory dump at update 4500]; a move of over 300 units in an update, or a matrix
   element changing by over 0.5 (a turn of about 30 degrees), is a cut, drawn unblended. */
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
