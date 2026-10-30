/* IN-GAME OPTIONS (DESIGN.md "In-game options"; GAME-GOALS.md: every setting the port adds lives in
   the game's own options menu, drawn in its style). The port's settings (options.txt) are rows of
   the front end's own Options pages, built, drawn and navigated by the game's menu code:

   - Video (title menu > Options > Video): Subtitles, then Display, Resolution, V-Sync, Frame Rate,
     Frame Limit, Anti-Aliasing, Anisotropic Filtering, HUD, Movies, Cut Scenes, Menus, Renderer
     (DXVK / our own Vulkan renderer: VULKAN.md; on restart).
   - Controls: Vibration and Button Prompts (prompts.c) after Fighting, Driving, Hovercraft, Mouse.

   THE GAME'S MENUS [verified: disassembly of the functions named, frames of the pages]. The Options
   screen (a 0x7028-byte object, constructor 0x65c06a; the front end's per-frame 0x65c246 runs state
   [+0x24] of it through 0x65fc55) keeps its pages' rows in list widgets, members 0xc0 bytes apart:
   +0x214 (a page's labels), +0x2d4, +0x394 (the Video page's values) ... A list widget:
     +0x04 the selected item, +0x0c..+0x83 its look (a 0x78-byte block copied in by 0x5f9ca0: +0x30 x,
     +0x34 y on the 640 x 448 design, +0x3c text height 22, +0x40 20, +0x4c..+0x58 colour 160,160,160,
     255, +0x5c width, +0x64 the spacing of a row laid out across (0: a column), +0x68 the spacing of
     a column's rows), +0x78 flags (the look's +0x6c; 0x6574ca(n, on) sets bit 2, 0x10, 1, 0x22, 0x40,
     0x100, 0x200 for n = 0..6 (0x65548e's table) [verified: the table read, 2026-10-30]: n = 2, bit 1,
     places the items by the spacings above (without it they follow each other along the line) and
     highlights the selection when the state is 0), +0x84 the items (a linked list of 0x18-byte nodes; 0x6554fc
     adds {look or 0, UTF-16 text}, copying the text), +0x88 their count, +0x8c the state
     (0x5f9ce0: 0 shown with its highlight, 2 leaving, 3 shown without). 0x657401 empties it,
     0x6577af selects, 0x656962 draws it, 0x6559be runs its own input (a row across: left and right).
   The texts come from Text\TX<language>FE.bin through 0x65526b (the screen's text object at +0x1c0;
   ids: 23 Subtitles, 26 On, 27 Off, 28 Vibration); 0x5f69bd(n) on the screen's input object
   ([+0x994]) tells a menu action pressed this frame: 0 up, 1 down, 2 left, 3 right, 7 back
   ([0x98f048], the input slot 0x10-0x13 pressed this frame (0x65c1cf), taken by the pages as
   0x10 up, 0x11 right, 0x12 down, 0x13 left too [guess: the joystick's directions]).

   The Video page (0x660a16 builds it, 0x6617a7 runs it, 0x661b8a draws +0x2d4, +0x214, +0x394):
   one label, Subtitles (the setup still loops over Gamma, Contrast and Brightness, ids 38-40, and
   adds only the first), and a row across of On / Off at x 300 whose selection is the setting
   [0x97f810] (written back as the page is left with back; the left / right of rows 1-3 that would
   change the gamma never happen on the PC). Here (hooks video_setup, video_update): the setup is
   left to run, then its two lists are laid out again as two columns, the labels' (no highlight) and
   the values' (one item per row, the selected row's value highlighted, as the Audio page's Output
   row); the page's update is replaced by the same in C, plus up / down between the rows (as the
   Audio page 0x6628b3 does it: its sound 0x35d, wrapping) and left / right changing the selected
   row's value. Subtitles keeps its own behaviour: the choice goes to [0x97f810] as the page is
   left with back, and left / right on it applies the gamma as the original's left / right did.

   The Controls page (0x66c752 builds +0x214: Fighting, Driving, Hovercraft, Mouse; 0x66e9bf runs it
   through the list's own input, 0x6559be, Enter opening a sub-page; 0x66eab0 draws +0x214 in the
   front end): Vibration and Button Prompts are a fifth and sixth label (the labels' rows 56 apart
   instead of 70, so six fit), their values (On / Off; Auto / Keyboard / Controller) a two-item
   column in +0x394 (not used by the Controls pages) drawn after the page; left / right on them
   changes option rumble / button_prompts (Enter on them does nothing, as on any row past the
   page's four).
   The pause menu's Controls (the same class, made by 0x61db35 as [0x97d250]) is left as it is.

   Each change is set (option_set), saved (options_save) and applied where the code allows (the
   readers' re-read paths); options read once at start-up show "(on restart)" while the choice
   differs from the one in force. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "options.h"
#include "win.h"
#include "decomp/decomp.h"

/* the game's functions (their incremental-linking stubs' targets) */
#define LIST_CLEAR      0x657401u       /* (this) */
#define LIST_LOOK       0x5f9ca0u       /* (this, look*) */
#define LIST_FLAG       0x6574cau       /* (this, n, on) */
#define LIST_STATE      0x5f9ce0u       /* (this, state) */
#define LIST_ADD        0x6554fcu       /* (this, item*, -1) */
#define LIST_SELECT     0x6577afu       /* (this, index) */
#define LIST_INPUT      0x6559beu       /* (this) */
#define LIST_DRAW       0x656962u       /* (this, -1) */
#define TEXT_GET        0x65526bu       /* (text object, id, 0) -> UTF-16 text */
#define MENU_KEY        0x5f69bdu       /* (input object, action) -> al */
#define GAMMA_APPLY     0x6eb545u       /* (gamma, contrast, brightness) [guess: from the page's rows] */
#define GAMMA_SET       0x78a24du
#define F_00671660      0x671660u
#define SOUND_PLAY      R32(0x97eee8)   /* (1, sound, 0, 0, 0, 0, handle*) */

/* the Options screen's members */
#define SCR_STATE(s)    ((s) + 0x24)
#define SCR_TEXT(s)     ((s) + 0x1c0)
#define SCR_LABELS(s)   ((s) + 0x214)
#define SCR_LIST2(s)    ((s) + 0x2d4)
#define SCR_VALUES(s)   ((s) + 0x394)
#define SCR_INPUT(s)    R32((s) + 0x994)
/* a list widget's members */
#define L_SEL(l)        ((l) + 0x04)
#define L_LOOK(l, k)    ((l) + 0x0c + (k))
#define L_COUNT(l)      ((l) + 0x88)

/* settings the game keeps [verified: 0x6617a7] */
#define SUBTITLES       0x97f810u       /* byte: on */
#define VIDEO_2D4       0x97f80cu       /* the +0x2d4 list's choice (no rows on the PC's Video page) */
#define GAMMA3          0x97f828u       /* three floats, kept at +0x6510 of the screen */
#define JOY_DIR         0x98f048u       /* the joystick's direction as a menu key */

extern int host_fullscreen(void);
extern void win_options(void), win_rumble_option(void);
extern void frame_rate_reapply(void);
void game_options_reapply(void);

/* ------------------------------------------------------------------ texts in program memory */
/* UTF-16 copies of our texts, made once each (the game copies an item's text as it is added) */
static uint32_t wtext(const char *s)
{
    static struct { char s[48]; uint32_t g; } made[96];
    static int nmade;
    for (int i = 0; i < nmade; i++)
        if (!strcmp(made[i].s, s))
            return made[i].g;
    size_t n = strlen(s);
    uint32_t g = galloc(2 * (uint32_t)n + 2);
    for (size_t i = 0; i <= n; i++)
        W16(g + 2 * (uint32_t)i, (uint16_t)(unsigned char)s[i]);
    if (nmade < 96 && n < sizeof made[0].s) {
        snprintf(made[nmade].s, sizeof made[nmade].s, "%s", s);
        made[nmade++].g = g;
    }
    return g;
}

/* a game text as ASCII (its UTF-16 below 256 kept) */
static void game_text(CPU *c, uint32_t scr, int id, char *out, size_t n)
{
    uint32_t p = game_callm(c, TEXT_GET, SCR_TEXT(scr), 2, (uint32_t)id, 0);
    size_t k = 0;
    for (; k + 1 < n && p && R16(p + 2 * (uint32_t)k); k++) {
        uint16_t ch = R16(p + 2 * (uint32_t)k);
        out[k] = ch < 256 ? (char)ch : '?';
    }
    out[k] = 0;
}

/* ------------------------------------------------------------------ the port's settings */
typedef struct {
    const char *label;
    const char *key;
    const char *vals[16];       /* as written to options.txt (canonical), NULL-terminated */
    const char *names[16];      /* as shown; NULL: On / Off from the game's texts (vals off, on) */
    int restart;                /* read once at start-up */
    void (*apply)(void);
} Setting;

static void apply_display(void) { if (!deterministic) win_options(); }
extern void draw_distance_option(void);
static void apply_frame_rate(void) { frame_rate_reapply(); frame_limit_options(); }
static void apply_layout(void) { game_options_reapply(); }

static const Setting video_rows[] = {
    {"Display", "display", {"window", "fullscreen"}, {"Window", "Full Screen"}, 0, apply_display},
    {"Resolution", "resolution",
     {"desktop", "original", "640x480", "800x600", "1024x768", "1280x720", "1280x800", "1366x768", "1600x900",
      "1920x1080", "1920x1200", "2560x1440", "2560x1600"},
     {"Desktop", "Original", "640x480", "800x600", "1024x768", "1280x720", "1280x800", "1366x768", "1600x900",
      "1920x1080", "1920x1200", "2560x1440", "2560x1600"}, 1, NULL},
    {"V-Sync", "vsync", {"off", "on"}, {NULL}, 1, NULL},
    {"Frame Rate", "frame_rate", {"60", "uncapped"}, {"60", "Uncapped"}, 0, apply_frame_rate},
    {"Frame Limit", "frame_limit", {"off", "screen", "30", "60", "120", "144", "165", "240"},
     {"Off", "Screen", "30", "60", "120", "144", "165", "240"}, 0, frame_limit_options},
    {"Anti-Aliasing", "antialiasing", {"off", "2x", "4x", "8x"}, {"Off", "2x", "4x", "8x"}, 1, NULL},
    {"Anisotropic Filtering", "anisotropic", {"off", "2", "4", "8", "16"}, {"Off", "2x", "4x", "8x", "16x"}, 1, NULL},
    {"HUD", "hud", {"corners", "center"}, {"Corners", "Center"}, 0, apply_layout},
    {"Movies", "movies", {"fill", "fit"}, {"Fill", "Fit"}, 0, apply_layout},
    {"Cut Scenes", "cutscenes", {"fill", "bars"}, {"Fill", "Bars"}, 0, apply_layout},
    {"Menus", "ui", {"center", "stretch"}, {"Center", "Stretch"}, 0, apply_layout},
    {"Driving Draw Distance", "draw_distance", {"1", "1.5", "2", "3"}, {"Original", "1.5x", "2x", "3x"}, 0,
     draw_distance_option},
    {"Renderer", "renderer", {"dxvk", "vulkan"}, {"DXVK", "Vulkan"}, 1, NULL},
};
#define NVIDEO ((int)(sizeof video_rows / sizeof video_rows[0]))

static const Setting vibration_row =
    {"Vibration", "rumble", {"off", "on"}, {NULL}, 0, win_rumble_option};
extern void prompts_option_changed(void);
static const Setting prompts_row =      /* prompts.c, DESIGN.md decision 13 */
    {"Button Prompts", "button_prompts", {"auto", "keyboard", "controller"}, {"Auto", "Keyboard", "Controller"}, 0,
     prompts_option_changed};

/* a setting's value in force, canonical (as its readers take it: see each reader) */
static void canonical(const Setting *s, const char *v, char *out, size_t n)
{
    const char *k = s->key;
    int i = v ? atoi(v) : 0;
    if (!strcmp(k, "display")) {                /* win.c: fullscreen, else a window */
        if (host_sdl_window() && !deterministic)    /* (test runs never switch: apply_display) */
            v = host_fullscreen() ? "fullscreen" : "window";   /* (Alt+Enter switches too) */
        snprintf(out, n, "%s", v && !strcasecmp(v, "fullscreen") ? "fullscreen" : "window");
    } else if (!strcmp(k, "resolution")) {      /* game.c resolution_choose */
        int w = 0, h = 0;
        if (!v)
            v = deterministic ? "original" : "desktop";
        if (!strcasecmp(v, "desktop") || !strcasecmp(v, "original"))
            snprintf(out, n, "%s", !strcasecmp(v, "desktop") ? "desktop" : "original");
        else if (sscanf(v, "%dx%d", &w, &h) == 2 && w >= 320 && h >= 240)
            snprintf(out, n, "%dx%d", w, h);
        else
            snprintf(out, n, "%s", deterministic ? "original" : "desktop");
    } else if (!strcmp(k, "vsync") || !strcmp(k, "rumble")) {  /* option_bool */
        int on = !strcmp(k, "rumble");
        if (v)
            on = !strcasecmp(v, "on") || !strcasecmp(v, "yes") || !strcasecmp(v, "true") || !strcmp(v, "1");
        snprintf(out, n, "%s", on ? "on" : "off");
    } else if (!strcmp(k, "frame_rate"))        /* decomp/frame.c */
        snprintf(out, n, "%s", v && !strcasecmp(v, "uncapped") ? "uncapped" : "60");
    else if (!strcmp(k, "frame_limit")) {       /* main.c frame_limit: screen, a rate from 10, else off */
        if (v && !strcasecmp(v, "screen"))
            snprintf(out, n, "screen");
        else if (i >= 10)
            snprintf(out, n, "%d", i);
        else
            snprintf(out, n, "off");
    } else if (!strcmp(k, "antialiasing")) {    /* d3d9_msaa.c: 2, 4 or 8, else off; 4x by default */
        if (!v) i = 4;
        if (i == 2 || i == 4 || i == 8)
            snprintf(out, n, "%dx", i);
        else
            snprintf(out, n, "off");
    } else if (!strcmp(k, "anisotropic")) {     /* d3d9.c: 2, 4, 8 or 16, else off; 16 by default */
        if (!v) i = 16;
        if (i == 2 || i == 4 || i == 8 || i == 16)
            snprintf(out, n, "%d", i);
        else
            snprintf(out, n, "off");
    } else if (!strcmp(k, "hud"))               /* game.c: center, else corners */
        snprintf(out, n, "%s", v && !strcasecmp(v, "center") ? "center" : "corners");
    else if (!strcmp(k, "movies"))              /* fit, else fill */
        snprintf(out, n, "%s", v && !strcasecmp(v, "fit") ? "fit" : "fill");
    else if (!strcmp(k, "cutscenes"))           /* bars, else fill */
        snprintf(out, n, "%s", v && !strcasecmp(v, "bars") ? "bars" : "fill");
    else if (!strcmp(k, "ui"))                  /* stretch, else center */
        snprintf(out, n, "%s", v && !strcasecmp(v, "stretch") ? "stretch" : "center");
    else if (!strcmp(k, "renderer"))            /* d3d9.c Direct3DCreate9: vulkan, else DXVK */
        snprintf(out, n, "%s", v && !strcasecmp(v, "vulkan") ? "vulkan" : "dxvk");
    else if (!strcmp(k, "draw_distance")) {     /* decomp/frame.c: 1.5, 2 or 3, else the original */
        double d = v ? atof(v) : 1;
        snprintf(out, n, "%s", d == 1.5 ? "1.5" : d == 2 ? "2" : d == 3 ? "3" : "1");
    } else if (!strcmp(k, "button_prompts"))      /* prompts.c: keyboard, controller, else auto */
        snprintf(out, n, "%s", v && !strcasecmp(v, "keyboard") ? "keyboard" : v && !strcasecmp(v, "controller") ? "controller" : "auto");
    else
        snprintf(out, n, "%s", v ? v : "");
}

static void current(const Setting *s, char *out, size_t n)
{
    canonical(s, option_str(s->key, NULL), out, n);
}

/* the values the restart rows started with */
static char started[NVIDEO][32];

void menu_init(void)
{
    for (int r = 0; r < NVIDEO; r++)
        if (video_rows[r].restart)
            current(&video_rows[r], started[r], sizeof started[r]);
}

static int nvals(const Setting *s)
{
    int k = 0;
    while (k < 16 && s->vals[k]) k++;
    return k;
}

/* the shown text of a setting's value */
static void shown(CPU *c, uint32_t scr, const Setting *s, int row, char *out, size_t n)
{
    char v[32];
    current(s, v, sizeof v);
    const char *name = v;                       /* (a value not in the list: shown as written) */
    char onoff[32];
    for (int k = 0; k < nvals(s); k++)
        if (!strcmp(s->vals[k], v)) {
            if (s->names[0])
                name = s->names[k];
            else {
                game_text(c, scr, k ? 26 : 27, onoff, sizeof onoff);
                name = onoff;
            }
        }
    if (row >= 0 && s->restart && strcmp(v, started[row]))
        snprintf(out, n, "%s (on restart)", name);
    else
        snprintf(out, n, "%s", name);
}

/* the next (dir 1) or previous value; from a value not in the list (a WxH or rate of the user's
   own in options.txt), the list's first or last */
static void step(const Setting *s, int dir)
{
    char v[32];
    current(s, v, sizeof v);
    int nv = nvals(s), at = -1;
    for (int k = 0; k < nv; k++)
        if (!strcmp(s->vals[k], v))
            at = k;
    at = at < 0 ? (dir > 0 ? 0 : nv - 1) : (at + dir + nv) % nv;
    option_set(s->key, s->vals[at]);
    options_save();
    LOG(LOG_ALWAYS, "options menu: %s = %s%s", s->key, s->vals[at], s->restart ? " (on restart)" : "");
    if (s->apply)
        s->apply();
}

/* ------------------------------------------------------------------ list helpers */
static void list_add(CPU *c, uint32_t list, uint32_t text)
{
    uint32_t f = dc_frame(c, 8);
    W32(f, 0);                                  /* (the list's own look) */
    W32(f + 4, text);
    game_callm(c, LIST_ADD, list, 2, f, 0xffffffffu);
    dc_unframe(c, f, 8);
}

static int menu_key(CPU *c, uint32_t scr, int action)
{
    return (game_callm(c, MENU_KEY, SCR_INPUT(scr), 1, (uint32_t)action) & 0xff) != 0;
}

static void sound(CPU *c, uint32_t id)
{
    game_call(c, SOUND_PLAY, 7, 1, id, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ the Video page */
/* layout: thirteen rows from y 64, 25 apart (27 for the twelve before Renderer; the game's own rows are 70 apart for four; its key
   binding pages set nineteen 17 apart), labels at x 70, values at x 330; a text wider than its
   list's width (+0x5c, 180 on the game's page) wraps onto a second line, so the widths are wider */
#define V_Y0        64
#define V_SPACING   25
#define V_LABEL_X   70
#define V_LABEL_W   256
#define V_VALUE_X   330
#define V_VALUE_W   290

static int sub_on;                              /* the Subtitles choice while the page is up */

static void video_values(CPU *c, uint32_t scr, int sel)
{
    uint32_t vl = SCR_VALUES(scr);
    char t[64];
    game_callm(c, LIST_CLEAR, vl, 0);
    game_text(c, scr, sub_on ? 26 : 27, t, sizeof t);
    list_add(c, vl, wtext(t));
    for (int r = 0; r < NVIDEO; r++) {
        shown(c, scr, &video_rows[r], r, t, sizeof t);
        list_add(c, vl, wtext(t));
    }
    game_callm(c, LIST_SELECT, vl, 1, (uint32_t)sel);
}

static int hook_video_setup_fn(CPU *c)
{
    uint32_t scr = c->ecx;
    F_00660a16_orig(c);
    uint32_t ret = c->ret, eax = c->eax;
    uint32_t lb = SCR_LABELS(scr), vl = SCR_VALUES(scr);
    sub_on = R8(SUBTITLES) != 0;
    /* the labels: a column (the setup's look, laid out for thirteen rows) */
    W32(L_LOOK(lb, 0x30), V_LABEL_X);
    W32(L_LOOK(lb, 0x34), V_Y0);
    W32(L_LOOK(lb, 0x5c), V_LABEL_W);
    W32(L_LOOK(lb, 0x68), V_SPACING);
    uint32_t sub = game_callm(c, TEXT_GET, SCR_TEXT(scr), 2, 23, 0);
    game_callm(c, LIST_CLEAR, lb, 0);
    list_add(c, lb, sub);
    for (int r = 0; r < NVIDEO; r++)
        list_add(c, lb, wtext(video_rows[r].label));
    game_callm(c, LIST_SELECT, lb, 1, 0);
    /* the values: a column beside it, its selection highlighted (the setup left it active, state 0) */
    W32(L_LOOK(vl, 0x30), V_VALUE_X);
    W32(L_LOOK(vl, 0x34), V_Y0);
    W32(L_LOOK(vl, 0x5c), V_VALUE_W);
    W32(L_LOOK(vl, 0x64), 0);
    W32(L_LOOK(vl, 0x68), V_SPACING);
    video_values(c, scr, 0);
    c->ret = ret;
    c->eax = eax;
    return 1;
}

/* 0x6617a7 in C (the order of its calls kept), with the rows */
static int hook_video_update_fn(CPU *c)
{
    uint32_t entry = c->esp, scr = c->ecx;
    uint32_t lb = SCR_LABELS(scr), l2 = SCR_LIST2(scr), vl = SCR_VALUES(scr);
    game_callm(c, LIST_INPUT, l2, 0);
    /* (the original runs the values' own input here: a row across, left / right moving its
       selection; the column's selection is the page's row, moved below) */
    if (menu_key(c, scr, 7) || menu_key(c, scr, 4)) {
        if (menu_key(c, scr, 7)) {              /* back: the choices kept */
            sound(c, 0x359);
            W32(SCR_STATE(scr), 0);
            W32(VIDEO_2D4, R32(L_SEL(l2)));
            game_callm(c, LIST_STATE, l2, 1, 2);
            W8(SUBTITLES, (uint8_t)(sub_on != 0));
            game_callm(c, LIST_STATE, vl, 1, 2);
        }
        /* (the original's cancel branch tests key 7 again where it was not pressed: never runs) */
        W32(scr + 0x6510, R32(GAMMA3));
        W32(scr + 0x6514, R32(GAMMA3 + 4));
        W32(scr + 0x6518, R32(GAMMA3 + 8));
        game_call(c, GAMMA_APPLY, 3, R32(GAMMA3), R32(GAMMA3 + 4), R32(GAMMA3 + 8));
    }
    /* up / down: the row (as the Audio page, 0x6628b3) */
    int dir = 0;
    if (menu_key(c, scr, 0) || R32(JOY_DIR) == 0x10) dir = -1;
    if (menu_key(c, scr, 1) || R32(JOY_DIR) == 0x12) dir = 1;
    if (dir && R32(SCR_STATE(scr)) == 4) {
        int n = (int)R32(L_COUNT(lb)), row = (int)R32(L_SEL(lb)) + dir;
        row = row < 0 ? n - 1 : row >= n ? 0 : row;
        sound(c, 0x35d);
        game_callm(c, LIST_SELECT, lb, 1, (uint32_t)row);
        game_callm(c, LIST_SELECT, vl, 1, (uint32_t)row);
    }
    /* left / right */
    int lr = 0;
    if (menu_key(c, scr, 2) || R32(JOY_DIR) == 0x13) lr = -1;
    if (menu_key(c, scr, 3) || R32(JOY_DIR) == 0x11) lr = 1;
    if (lr) {
        int row = (int)R32(L_SEL(lb));
        if (row == 0) {
            /* Subtitles; and as the original's left / right on its one row: the gamma applied again
               (rows 1-3, Gamma, Contrast and Brightness, are not on the PC's page) */
            if (R32(SCR_STATE(scr)) == 4) {
                sub_on = !sub_on;
                sound(c, 0x35d);
                video_values(c, scr, row);
            }
            game_call(c, GAMMA_APPLY, 3, R32(GAMMA3), R32(GAMMA3 + 4), R32(GAMMA3 + 8));
            W32(0x947f84, R32(GAMMA3));
            W32(0x947f88, R32(GAMMA3 + 8));
            W32(0x947f8c, R32(GAMMA3 + 4));
            game_call(c, GAMMA_SET, 3, R32(0x947f84), R32(0x947f8c), R32(0x947f88));
            game_call(c, F_00671660, 0);
        } else if (row <= NVIDEO && R32(SCR_STATE(scr)) == 4) {
            sound(c, 0x35d);
            step(&video_rows[row - 1], lr);
            video_values(c, scr, row);
        }
    }
    return decomp_ret(c, entry, c->eax, 0);
}

/* ------------------------------------------------------------------ the Controls page */
/* Vibration and Button Prompts after Fighting, Driving, Hovercraft, Mouse: the labels' rows closer
   together (the game's are 70 apart, which leaves room for five), the two values a column of their
   own at x 300 */
#define C_ROW       4                           /* Vibration; Button Prompts is the next */
#define C_ROWS      (C_ROW + 2)
#define C_SPACING   56
#define C_VALUE_X   300

static int controls_ours(uint32_t scr)
{
    return scr != R32(0x97d250) && R32(L_COUNT(SCR_LABELS(scr))) == C_ROWS;
}

static void controls_value(CPU *c, uint32_t scr)
{
    uint32_t vl = SCR_VALUES(scr);
    char t[64];
    game_callm(c, LIST_CLEAR, vl, 0);
    shown(c, scr, &vibration_row, -1, t, sizeof t);
    list_add(c, vl, wtext(t));
    shown(c, scr, &prompts_row, -1, t, sizeof t);
    list_add(c, vl, wtext(t));
    game_callm(c, LIST_SELECT, vl, 1, 0);
}

static int hook_controls_setup_fn(CPU *c)
{
    uint32_t scr = c->ecx;
    F_0066c752_orig(c);
    if (scr == R32(0x97d250) || R32(L_COUNT(SCR_LABELS(scr))) != C_ROW)
        return 1;                               /* (the pause menu's: as it is) */
    uint32_t ret = c->ret, eax = c->eax;
    uint32_t lb = SCR_LABELS(scr), vl = SCR_VALUES(scr);
    list_add(c, lb, game_callm(c, TEXT_GET, SCR_TEXT(scr), 2, 28, 0));
    list_add(c, lb, wtext(prompts_row.label));
    W32(L_LOOK(lb, 0x68), C_SPACING);
    /* the values: the labels' look at the value column, from the fifth row, never highlighted */
    uint32_t f = dc_frame(c, 0x78);
    memcpy(gptr(f), gptr(L_LOOK(lb, 0)), 0x78);
    W32(f + 0x30, C_VALUE_X);
    W32(f + 0x34, R32(f + 0x34) + C_ROW * C_SPACING);
    W32(f + 0x2c, 0);
    W32(f + 0x64, 0);                           /* (a column) */
    W32(f + 0x68, C_SPACING);
    game_callm(c, LIST_CLEAR, vl, 0);
    game_callm(c, LIST_LOOK, vl, 1, f);
    dc_unframe(c, f, 0x78);
    /* (flag 1 of +0x78, the look's +0x6c, kept from the labels': items placed by the row spacing, a
       column [verified: 0x656962 tests it; without it items follow each other along the line]; state
       3 draws no highlight) */
    game_callm(c, LIST_STATE, vl, 1, 3);
    controls_value(c, scr);
    c->ret = ret;
    c->eax = eax;
    return 1;
}

static int hook_controls_update_fn(CPU *c)
{
    uint32_t scr = c->ecx;
    int row = (int)R32(L_SEL(SCR_LABELS(scr)));
    if (controls_ours(scr) && row >= C_ROW && R32(SCR_STATE(scr)) == 0xe) {
        int lr = menu_key(c, scr, 2) || R32(JOY_DIR) == 0x13 ? -1 : menu_key(c, scr, 3) || R32(JOY_DIR) == 0x11 ? 1 : 0;
        if (lr) {
            sound(c, 0x35d);
            step(row == C_ROW ? &vibration_row : &prompts_row, lr);
            controls_value(c, scr);
        }
        c->ecx = scr;
    }
    return 0;                                   /* (then the page's own) */
}

static int hook_controls_draw_fn(CPU *c)
{
    uint32_t scr = c->ecx;
    F_0066eab0_orig(c);
    if (!controls_ours(scr) || R32(0x97ed80) != 1)
        return 1;
    uint32_t ret = c->ret, eax = c->eax;
    game_callm(c, LIST_DRAW, SCR_VALUES(scr), 1, 0xffffffffu);
    c->ret = ret;
    c->eax = eax;
    return 1;
}

void menu_install(void)
{
    menu_init();
    hook_video_setup = hook_video_setup_fn;
    hook_video_update = hook_video_update_fn;
    hook_controls_setup = hook_controls_setup_fn;
    hook_controls_update = hook_controls_update_fn;
    hook_controls_draw = hook_controls_draw_fn;
}
