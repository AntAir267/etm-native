/* What the shared runtime asks of the game-specific side. Enter the Matrix plays its movies
   through the DivX DLLs (translated with it) and has no game-specific controller handling yet. */
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include "runtime.h"
#include "input.h"
#include "pad.h"
#include "funcs.h"

void video_skip_press(void) {}
int video_skip_taken(void) { return 0; }
/* CoCreateInstance for a class the runtime does not provide: CLASS_E_CLASSNOTAVAILABLE */
uint32_t video_cocreate(uint32_t clsid, uint32_t iid, uint32_t ppv) { (void)clsid; (void)iid; (void)ppv; return 0x80040111u; }
void padgame_frame(unsigned long frame) { (void)frame; }
int ifc_rumble_wanted(void) { return 0; }
void ifc_frame(unsigned long frame) { (void)frame; }

/* (LA Rush hooks the shared runtime calls: none of them apply yet) */
int game_driving(void) { return 0; }
void frame_rate_reapply(void) {}

void game_init(void)
{
}

void game_frame(unsigned long frame)
{
    (void)frame;
    extern void mss_frame(void);
    mss_frame();
}
