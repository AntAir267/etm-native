/* WHAT MOVES in Enter the Matrix, for frames drawn between updates (interp.c, decomp/frame.c):
   after each update the fields that place what the render pass draws are copied; interp.c blends
   them before the render pass and puts the latest back after it. */
#include "runtime.h"
#include "interp.h"

void interp_game_update(void)
{
    interp_step_begin();
    interp_step_end();
}
