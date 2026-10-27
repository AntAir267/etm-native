/* What the shared runtime asks of the game-specific side. Enter the Matrix plays its movies
   through the DivX DLLs (translated with it) and has no game-specific controller handling yet. */
#include <ctype.h>
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
int game_front_end(void);
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

/* IN-ENGINE SCENES (option cutscenes = fill, the default; ui = center). In cinema mode the
   camera state's [0x97f238] holds the bars' height in pixels (an eighth of the screen's: 135 at
   1080), the 3D is drawn into the band between them (viewport y = bars, height = screen - 2
   bars; the projection 0x62a236 gets that height) and 0x6023cf draws black 2D bars 58 high at
   the top and bottom of the 448-line design [verified: decompiles; projection arguments and the
   view state read at run time]. The band on a 4:3 screen is about 16:9: the shape the scenes
   were framed for. On a wide screen the band is shown at the screen's size: the viewport and
   the projection's height become the screen's and its focal length grows by s, so the picture
   keeps the band's framing:
     s = min(screen / band height, (width / height) / (4/3))
   (16:9: s = 4/3, the band's height exactly, no bars; 16:10: s = 1.2 keeps the band's width and
   slim bars cover the rest, (1 - s x band / screen) / 2 each). Culling keeps the game's own
   frustum (wider than what shows). cutscenes = bars: the game's bars over the wider view. */
static int cutscenes_fill = -1;
static float cinema_s, cinema_bar;     /* (this frame's) focal factor, and each bar's share */

static int cinema_on(void)
{
    if (cutscenes_fill < 0)
        cutscenes_fill = strcasecmp(option_str("cutscenes", "fill"), "bars") != 0;
    int32_t bar = (int32_t)R32(0x97f238);
    float w = (float)(int32_t)R32(0x947f60), h = (float)(int32_t)R32(0x947f64);
    if (!cutscenes_fill || !ui_active() || bar <= 0 || bar * 4 >= h)
        return 0;
    float band = h - 2 * (float)bar;
    float s1 = h / band, s2 = (w / h) / (4.0f / 3.0f);
    cinema_s = s1 < s2 ? s1 : s2;
    cinema_bar = (1 - cinema_s * band / h) / 2;
    if (cinema_bar < 0.004f)
        cinema_bar = 0;
    return 1;
}

void ui_viewport(uint32_t *x, uint32_t *y, uint32_t *w, uint32_t *h)
{
    if (!cinema_on())
        return;
    uint32_t sw = R32(0x947f60), sh = R32(0x947f64), bar = R32(0x97f238);
    if (*x == 0 && *y == bar && *w == sw && *h == sh - 2 * bar) {
        *y = 0;
        *h = sh;
    }
}

static uint16_t rd16g(uint32_t a) { return R16(a); }
static void wr16g(uint32_t a, uint16_t v) { W16(a, v); }

/* HUD corners (option hud = corners, the default; in play only, menus stay centred): items are
   anchored as wholes to the left edge, the centre or the right edge, so the HUD's distance from
   the screen's edges is the 4:3 layout's. A line of text is many items (each glyph is its own
   position and sprite): glyphs on the same row, each starting near the last one's end, are one
   group, and a group is anchored by its extent: left when it lies in the left part (starts in
   the first quarter, ends before 40 %), right when it lies in the right part, centred otherwise
   (subtitles, tips, the crosshair). */
enum { ANCHOR_CENTER, ANCHOR_LEFT, ANCHOR_RIGHT, ANCHOR_FULL };
typedef struct {
    uint32_t at;                    /* the command's address */
    uint8_t op;
    uint32_t xat;                   /* where its x is (op 2, op 12 mode 0), 0 none */
    float x0, x1, y0, y1;           /* its extent in design units (op 2 + 7: the sprite) */
    int group, anchor;
} BlitItem;
#define BLIT_MAX 8192
static BlitItem items[BLIT_MAX];
static int hud_corners = -1;

static int anchor_of(float x0, float x1)
{
    if (x0 <= 1 && x1 >= 639)
        return ANCHOR_FULL;
    if (x0 < 160 && x1 < 256)
        return ANCHOR_LEFT;
    if (x1 > 480 && x0 > 384)
        return ANCHOR_RIGHT;
    return ANCHOR_CENTER;
}

static void blit_layout(void)
{
    uint32_t base = R32(0x92ed64), n = R32(0x979bcc);
    float off_d = ui_off_px / ui_s;                  /* the offset in design units */
    float full_d = 640.0f + 2 * off_d;               /* the screen's width in design units */
    if (hud_corners < 0)
        hud_corners = strcasecmp(option_str("hud", "corners"), "center") != 0;
    int corners = hud_corners && !game_front_end();
    /* 1: the items, with the extent each covers */
    int ni = 0;
    float px = 0, py = 0;                            /* the last position */
    int pos_item = -1;                               /* the item of the last position command */
    for (uint32_t i = 0; i < n && ni < BLIT_MAX; ) {
        uint32_t a = base + i;
        BlitItem *it = &items[ni];
        memset(it, 0, sizeof *it);
        it->at = a;
        it->op = R8(a);
        it->group = -1;
        it->x0 = 1e9f;
        switch (it->op) {
        case 2:
            px = rd16g(a + 1);
            py = rd16g(a + 3);
            it->xat = a + 1;
            it->x0 = it->x1 = px;
            it->y0 = it->y1 = py;
            pos_item = ni;
            ni++;
            i += 9;
            continue;
        case 7:
            if (pos_item >= 0) {                     /* the sprite widens its position's item */
                BlitItem *p = &items[pos_item];
                int32_t w = (int32_t)R32(a + 5), h = (int32_t)R32(a + 9);
                p->x1 = px + (float)w;
                p->y1 = py + (float)h;
            }
            i += 13;
            continue;                                /* (no item of its own) */
        case 12: {
            int32_t w = (int32_t)R32(a + 8), h = (int32_t)R32(a + 12);
            if (R8(a + 1) == 0) {
                it->xat = a + 2;
                it->x0 = rd16g(a + 2);
                it->y0 = py;
            } else {
                it->x0 = px;
                it->y0 = rd16g(a + 2);
            }
            it->x1 = it->x0 + (float)w;
            it->y1 = it->y0 + (float)h;
            ni++;
            i += 16;
            continue;
        }
        case 13: {
            float lo = 1e9f, hi = -1e9f, ylo = 1e9f, yhi = -1e9f;
            for (int k = 0; k < 4; k++) {
                float x = (int16_t)rd16g(a + 1 + 16 + 8 * k), y = (int16_t)rd16g(a + 1 + 16 + 8 * k + 2);
                if (x < lo) lo = x;
                if (x > hi) hi = x;
                if (y < ylo) ylo = y;
                if (y > yhi) yhi = y;
            }
            it->x0 = lo; it->x1 = hi; it->y0 = ylo; it->y1 = yhi;
            ni++;
            i += 49;
            continue;
        }
        case 3: case 4: case 5: case 8: i += 5; continue;
        case 0: case 9: case 10: case 11: i += 1; continue;
        default:
            LOG(LOG_ALWAYS, "2D layout: unknown blit command %u at +%u: the rest drawn as is", R8(a), i);
            n = i;
            break;
        }
        break;
    }
    /* 2: groups (text lines) and their anchors */
    int ngroups = 0;
    for (int k = 0; k < ni; k++) {
        BlitItem *it = &items[k];
        if (it->op == 2 && k > 0 && items[k - 1].op == 2 && items[k - 1].group >= 0) {
            BlitItem *pv = &items[k - 1];
            float h = pv->y1 - pv->y0 > 4 ? pv->y1 - pv->y0 : 8;
            if (fabsf(it->y0 - pv->y0) <= 1.5f && it->x0 >= pv->x0 - 1 && it->x0 <= pv->x1 + h * 1.5f) {
                it->group = pv->group;
                continue;
            }
        }
        it->group = ngroups++;
    }
    static float gx0[BLIT_MAX], gx1[BLIT_MAX];
    static int ganchor[BLIT_MAX];
    for (int g = 0; g < ngroups; g++) { gx0[g] = 1e9f; gx1[g] = -1e9f; }
    for (int k = 0; k < ni; k++) {
        int g = items[k].group;
        if (items[k].x0 < gx0[g]) gx0[g] = items[k].x0;
        if (items[k].x1 > gx1[g]) gx1[g] = items[k].x1;
    }
    for (int g = 0; g < ngroups; g++) {
        int an = anchor_of(gx0[g], gx1[g]);
        if (!corners && (an == ANCHOR_LEFT || an == ANCHOR_RIGHT))
            an = ANCHOR_CENTER;
        ganchor[g] = an;
    }
    static int log_blit = -1;
    if (log_blit < 0)
        log_blit = getenv("ETM_LOG_BLIT") != NULL;
    if (log_blit)
        for (int g = 0; g < ngroups; g++)
            if (ganchor[g] == ANCHOR_FULL)
                for (int k = 0; k < ni; k++)
                    if (items[k].group == g)
                        LOG(LOG_ALWAYS, "blit: frame %lu full-width op %u x %.0f..%.0f y %.0f..%.0f", frame_count,
                            items[k].op, items[k].x0, items[k].x1, items[k].y0, items[k].y1);
    /* the in-engine scenes' bars (op 2 at x 0, y 0 or 392, then op 7 641 x 58) shrink to what the
       zoomed picture leaves */
    if (cinema_on()) {
        float bar = cinema_bar * 448;
        int32_t bh = bar < 0.5f ? 0 : (int32_t)lroundf(bar);
        for (uint32_t i = 0; i + 22 <= n; ) {
            uint32_t a = base + i;
            uint8_t op = R8(a);
            if (op == 2 && R8(a + 9) == 7 && rd16g(a + 1) == 0 && (int32_t)R32(a + 9 + 5) >= 640 &&
                (int32_t)R32(a + 9 + 9) == 58) {
                uint16_t y = rd16g(a + 3);
                if (y == 0 || y == 392) {
                    if (y == 392)
                        wr16g(a + 3, (uint16_t)(448 - bh));
                    W32(a + 9 + 9, (uint32_t)(bh ? bh + (y == 392 ? 2 : 0) : 0));
                }
            }
            switch (op) {
            case 2: i += 9; break;
            case 7: i += 13; break;
            case 12: i += 16; break;
            case 13: i += 49; break;
            case 3: case 4: case 5: case 8: i += 5; break;
            default: i += 1; break;
            }
        }
    }
    /* 3: the new positions */
    for (int k = 0; k < ni; k++) {
        BlitItem *it = &items[k];
        int an = ganchor[it->group];
        float shift = an == ANCHOR_LEFT ? 0 : an == ANCHOR_RIGHT ? 2 * off_d : off_d;
        uint32_t a = it->at;
        switch (it->op) {
        case 2: {
            uint16_t x = rd16g(a + 1);
            if (an == ANCHOR_FULL && x == 0) {       /* across the design: across the screen */
                uint32_t s7 = a + 9;                 /* (its sprite follows: op 7) */
                if (s7 < base + n && R8(s7) == 7) {
                    int32_t w = (int32_t)R32(s7 + 5);
                    W32(s7 + 5, (uint32_t)lroundf((float)w * full_d / 640.0f));
                }
            } else if (x < 0x8000u)
                wr16g(a + 1, (uint16_t)lroundf((float)x + shift));
            break;
        }
        case 12:
            if (it->xat) {
                uint16_t x = rd16g(it->xat);
                if (an == ANCHOR_FULL && x == 0)
                    W32(a + 8, (uint32_t)lroundf((float)(int32_t)R32(a + 8) * full_d / 640.0f));
                else if (x < 0x8000u)
                    wr16g(it->xat, (uint16_t)lroundf((float)x + shift));
            }
            break;
        case 13:
            for (int q = 0; q < 4; q++) {
                float x = (int16_t)rd16g(a + 1 + 16 + 8 * q);
                x = an == ANCHOR_FULL ? x * full_d / 640.0f : x + shift;
                wr16g(a + 1 + 16 + 8 * q, (uint16_t)(int16_t)lroundf(x));
            }
            break;
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

/* CINEMATICS (option movies = fill, the default; ui = center): the story's movies
   (movies\fmv\*.avi) are films letterboxed inside their 640 x 448 picture (g01_s: rows 112-367
   of the 480-line screen, about 2.5:1 [verified: frame dump]). On a wide screen they are drawn
   across its whole width, centred: the picture grows by the screen's shape over 4:3 and its own
   black bars still cover the top and bottom (a 4:3 picture would lose 12 % at each; the films'
   bars are wider than that). The other movies (logos, menu backdrops, character select, whose
   menus are laid over them in 4:3) are pillarboxed. movies = fit pillarboxes all. */
static int movie_fmv, movies_fill = -1;

static void movie_opened(const char *win)
{
    size_t n = strlen(win);
    if (n < 4 || strcasecmp(win + n - 4, ".avi"))
        return;
    char low[512];
    snprintf(low, sizeof low, "%s", win);
    for (char *p = low; *p; p++)
        *p = (char)tolower((unsigned char)*p);
    movie_fmv = strstr(low, "\\fmv\\") != NULL;
}

/* (d3d9.c asks for each screen-space draw: 1 with a scale and offset for x and y, 0 to draw as is) */
int ui2d_layout(uint32_t caller, float minx, float maxx, float miny, float maxy, float *scale, float *offset,
                float *yscale, float *yoffset)
{
    (void)minx; (void)maxx; (void)miny; (void)maxy;
    /* the movie picture (0x7be3fb draws it with x times [0x97eda0]): pillarboxed, or across the
       screen (the films) */
    if (caller >= 0x7be3fb && caller < 0x7beb44 && ui_active()) {
        float st;
        uint32_t v = R32(0x97eda0);
        memcpy(&st, &v, 4);
        if (st <= 0)
            return 0;
        if (movies_fill < 0)
            movies_fill = strcasecmp(option_str("movies", "fill"), "fit") != 0;
        if (movie_fmv && movies_fill) {
            float h = (float)(int32_t)R32(0x947f64), k = st / ui_s;    /* (width / 640) / (height / 480) */
            *scale = 1;
            *offset = 0;
            *yscale = k;
            *yoffset = h / 2 * (1 - k);
            return 1;
        }
        *scale = ui_s / st;
        *offset = ui_off_px;
        return 1;
    }
    return 0;
}

/* (m, width, height, focal, far, aspect factor) */
static int hook_projection_fn(CPU *c)
{
    static int log = -1;
    if (log < 0)
        log = getenv("ETM_LOG_PROJ") != NULL;
    float a[5];
    for (int k = 0; k < 5; k++) { uint32_t v = R32(c->esp + 8 + 4 * k); memcpy(&a[k], &v, 4); }
    if (log)
        LOG(LOG_ALWAYS, "projection: frame %lu w %.0f h %.0f focal %.1f far %.0f k %.3f bars %d", frame_count,
            a[0], a[1], a[2], a[3], a[4], (int32_t)R32(0x97f238));
    /* the band (its height the screen's less the bars) shown at the screen's height */
    float sh = (float)(int32_t)R32(0x947f64), bar = (float)(int32_t)R32(0x97f238);
    if (cinema_on() && fabsf(a[1] - (sh - 2 * bar)) < 1.5f) {
        float nh = sh, nf = a[2] * cinema_s;
        uint32_t v;
        memcpy(&v, &nh, 4); W32(c->esp + 12, v);
        memcpy(&v, &nf, 4); W32(c->esp + 16, v);
    }
    return 0;
}

/* the frame end's wait (0x7c0915: until the frame's step has passed, QueryPerformanceCounter and
   Sleep(0) from 0x7c0b4a) [verified: disassembly]: its Sleep(0) sleeps 0.2 ms a turn (sync.c) */
int sleep0_spin_site(uint32_t ret) { return ret == 0x7c0b50u; }

/* (LA Rush hooks the shared runtime calls: none of them apply yet) */
int game_driving(void) { return 0; }
/* the game's menus (the input update 0x7b53c2 reads the menu controls under this test): mode
   [0x947f08] 0 or 9, [0x97ed80] == 1, or a front-end screen [0x97cef0] in state 2 / flagged */
int game_front_end(void)
{
    int32_t mode = (int32_t)R32(0x947f08);
    uint32_t fe = R32(0x97cef0);
    return mode == 0 || mode == 9 || R32(0x97ed80) == 1 ||
           (fe && (R32(fe + 0x6c) == 2 || R8(fe + 0x7c) != 0));
}

void game_init(void)
{
    extern void decomp_install(void), stackfix_install(void);
    decomp_install();                   /* the drawing code's rewrites (runtime/decomp/) */
    stackfix_install();                 /* frames cleared in both modes (runtime/decomp/stackfix.c) */
    extern void frame_install(void);
    frame_install();                    /* frames between updates (runtime/decomp/frame.c) */
    resolution_choose();
    hook_settings_load = hook_settings_load_fn;
    hook_blit_draw = hook_blit_draw_fn;
    hook_projection = hook_projection_fn;
    extern void (*file_open_notify)(const char *winpath);
    file_open_notify = movie_opened;
}

void game_frame(unsigned long frame)
{
    static long dump_at = -2;
    if (dump_at == -2) {
        const char *e = getenv("ETM_DUMP_BINDINGS");
        dump_at = e ? atol(e) : -1;
    }
    if ((long)frame == dump_at) {                   /* (debug: the key bindings 0x97f840..) */
        char line[2048];
        int n = 0;
        for (uint32_t a = 0x97f844; a < 0x97f9a4; a += 8)
            n += snprintf(line + n, sizeof line - n, " %x:%d,%d", a & 0xfff, (int32_t)R32(a), (int32_t)R32(a + 4));
        LOG(LOG_ALWAYS, "bindings at frame %lu:%s mode %d", frame, line, (int32_t)R32(0x947f08));
    }
    extern void mss_frame(void);
    mss_frame();
}
