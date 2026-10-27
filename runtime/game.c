/* What the shared runtime asks of the game-specific side. Enter the Matrix plays its movies
   through the DivX DLLs (translated with it) and has no game-specific controller handling yet. */
#include <math.h>
#include <string.h>
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

#include "options.h"
#include "win.h"

/* ------------------------------------------------------------------ resolution
   The settings loader (0x7b85e0) reads MatrixConfig.ini; [RENDERING] RESOLUTION 0-6 picks
   640x480 .. 2048x1536 into 0x947f60 / 0x947f64, anything else 640x480 (the repack's
   "1280x720", meant for its widescreen plugin, reads as 1280: 640x480) [verified: decompile].
   Option resolution: desktop (the default; test runs keep the game's own unless given),
   original, or WxH. The 3D view follows the shape by itself: 0x6eaf9a keeps
   (4/3) / (width / height) for the projection [verified: decompile; widescreen frames]. */
static int res_w, res_h;

static void resolution_choose(void)
{
    const char *r = option_str("resolution", deterministic ? "original" : "desktop");
    int w = 0, h = 0;
    if (!strcasecmp(r, "desktop"))
        host_desktop_size(&w, &h);
    else if (sscanf(r, "%dx%d", &w, &h) != 2)
        w = h = 0;
    if (w >= 320 && h >= 240) {
        res_w = w;
        res_h = h;
    }
}

static int hook_settings_load_fn(CPU *c)
{
    F_007b85e0_orig(c);
    if (res_w) {
        LOG(LOG_ALWAYS, "resolution: %dx%d (MatrixConfig.ini's: %ux%u)", res_w, res_h, R32(0x947f60), R32(0x947f64));
        W32(0x947f60, (uint32_t)res_w);
        W32(0x947f64, (uint32_t)res_h);
    }
    return 1;
}

/* ------------------------------------------------------------------ 2D layout
   The game's 2D (menus, text, HUD) is a "blit command list" (adders 0x5f21a8 .. 0x5f380e) the
   renderer 0x7a73db replays once a frame, a 640 x 448 design laid over the screen: x times
   [0x97eda0] (width / 640), y times height / 448 [verified: decompiles]. On a screen wider than
   4:3 that stretches everything sideways. Option ui = center (the default): before the replay
   the list is laid out for a centred 4:3 area (x scale height / 480, as on a 4:3 screen of that
   height): positions move by the area's offset; what spans the whole design width (fades,
   letterbox bars, backdrops) spans the whole screen. Commands (byte op, then data):
     2  set position: x u16, y u16, depth i32            (9 bytes)
     3, 4, 5  four bytes each (4 = colour)                (5)
     7  sprite at the position: rect 4 bytes, w i32, h i32 (13)
     8  texture: i32                                      (5)
     9, 10, 11  flags                                     (1)
     12 sprite: mode u8 (0: x u16, y from the position; 1: y u16, x from the position),
        rect 4 bytes, w i32, h i32                         (16)
     13 quad: 4 x (u, v as i16), then 4 x (x i16, y i16, i32) (49)
   ui = stretch: as the game draws it. Movies (0x7be3fb, 640 x 448 pictures) are pillarboxed the
   same way through the screen-space draw hook (d3d9.c ui2d_layout). */
static int ui_mode = -1;               /* 1 center, 0 stretch */
static float ui_s, ui_off_px;          /* x scale (pixels a design unit) and the area's offset */
static float ui_stretch_orig;          /* [0x97eda0] as the game set it */

static int ui_active(void)
{
    if (ui_mode < 0)
        ui_mode = strcasecmp(option_str("ui", "center"), "stretch") != 0;
    float w = (float)(int32_t)R32(0x947f60), h = (float)(int32_t)R32(0x947f64);
    if (!ui_mode || w <= 0 || h <= 0 || w / h < 4.0f / 3.0f + 0.01f)
        return 0;
    ui_s = h / 480.0f;
    ui_off_px = (w - 640.0f * ui_s) / 2;
    return 1;
}

static uint16_t rd16g(uint32_t a) { return R16(a); }
static void wr16g(uint32_t a, uint16_t v) { W16(a, v); }

static void blit_layout(void)
{
    uint32_t base = R32(0x92ed64), n = R32(0x979bcc);
    float off_d = ui_off_px / ui_s;                  /* the offset in design units */
    float full_d = 640.0f + 2 * off_d;               /* the screen's width in design units */
    int pos_x0 = -1;                                 /* the last position's x, for op 7 */
    uint32_t pos_at = 0;
    for (uint32_t i = 0; i < n; ) {
        uint32_t a = base + i;
        switch (R8(a)) {
        case 2: {
            uint16_t x = rd16g(a + 1);
            pos_x0 = x;
            pos_at = a + 1;
            if (x < 0x8000u)
                wr16g(a + 1, (uint16_t)lroundf((float)x + off_d));
            i += 9;
            break;
        }
        case 7: {
            int32_t w = (int32_t)R32(a + 5);
            if (pos_x0 == 0 && w >= 640) {          /* across the design: across the screen */
                wr16g(pos_at, 0);
                W32(a + 5, (uint32_t)lroundf((float)w * full_d / 640.0f));
            }
            i += 13;
            break;
        }
        case 12:
            if (R8(a + 1) == 0) {
                uint16_t x = rd16g(a + 2);
                int32_t w = (int32_t)R32(a + 8);
                if (x == 0 && w >= 640)
                    W32(a + 8, (uint32_t)lroundf((float)w * full_d / 640.0f));
                else if (x < 0x8000u)
                    wr16g(a + 2, (uint16_t)lroundf((float)x + off_d));
            }
            i += 16;
            break;
        case 13: {
            int16_t xs[4];
            int lo = 32767, hi = -32768;
            for (int k = 0; k < 4; k++) {
                xs[k] = (int16_t)rd16g(a + 1 + 16 + 8 * k);
                if (xs[k] < lo) lo = xs[k];
                if (xs[k] > hi) hi = xs[k];
            }
            int full = lo <= 0 && hi >= 640;
            for (int k = 0; k < 4; k++) {
                float x = full ? (float)xs[k] * full_d / 640.0f : (float)xs[k] + off_d;
                wr16g(a + 1 + 16 + 8 * k, (uint16_t)(int16_t)lroundf(x));
            }
            i += 49;
            break;
        }
        case 3: case 4: case 5: case 8: i += 5; break;
        case 0: case 9: case 10: case 11: i += 1; break;
        default:
            LOG(LOG_ALWAYS, "2D layout: unknown blit command %u at +%u: the rest drawn as is", R8(a), i);
            return;
        }
    }
}

static int hook_blit_draw_fn(CPU *c)
{
    if (!ui_active()) {
        F_007a73db_orig(c);
        return 1;
    }
    blit_layout();
    uint32_t keep = R32(0x97eda0);
    float s = ui_s;
    memcpy(&ui_stretch_orig, &keep, 4);
    uint32_t sv;
    memcpy(&sv, &s, 4);
    W32(0x97eda0, sv);
    F_007a73db_orig(c);
    W32(0x97eda0, keep);
    return 1;
}

/* (d3d9.c asks for each screen-space draw: 1 with a scale and offset for x, 0 to draw as is) */
int ui2d_layout(uint32_t caller, float minx, float maxx, float miny, float maxy, float *scale, float *offset)
{
    (void)minx; (void)maxx; (void)miny; (void)maxy;
    /* the movie picture (0x7be3fb draws it with x times [0x97eda0]): pillarboxed */
    if (caller >= 0x7be3fb && caller < 0x7beb44 && ui_active()) {
        float st;
        uint32_t v = R32(0x97eda0);
        memcpy(&st, &v, 4);
        if (st <= 0)
            return 0;
        *scale = ui_s / st;
        *offset = ui_off_px;
        return 1;
    }
    return 0;
}

/* (LA Rush hooks the shared runtime calls: none of them apply yet) */
int game_driving(void) { return 0; }
int game_front_end(void) { return 1; }
void frame_rate_reapply(void) {}

void game_init(void)
{
    resolution_choose();
    hook_settings_load = hook_settings_load_fn;
    hook_blit_draw = hook_blit_draw_fn;
}

void game_frame(unsigned long frame)
{
    (void)frame;
    extern void mss_frame(void);
    mss_frame();
}
