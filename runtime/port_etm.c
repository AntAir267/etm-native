/* Enter the Matrix's hooks into the AntiStatic runtime (docs/MOVING.md): what this port's own copy
   of the runtime did that is the game's, not Windows'. */
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

extern int frame_rate_uncapped(void);     /* decomp/frame.c */
extern int frame_ticks_end(void);
extern int frame_thirty_level(void);
extern int game_front_end(void);          /* game.c */
extern int game_cutscene(void);
extern void levels_cli(const char *level, const char *character, const char *difficulty, int list);

/* ------------------------------------------------------------------ frames
   With frame_rate = uncapped a drawn frame runs 0 or more of the game's updates (decomp/frame.c):
   scripted input, --hash, --dump-memory, --frames and game_frame count the updates, so runs at
   different drawn rates meet the same input on the same update (tests/rate.sh). */
int port_frame_ticks(void)
{
    int t = frame_ticks_end();          /* (every frame: it ends the frame's update count) */
    return frame_rate_uncapped() ? t : 1;
}

/* the limiter: driving and hovercraft at 30 whatever the option says (decomp/frame.c); with
   frames between updates and no limit set, the screen's rate (3 under it) */
double port_frame_limit_ms(double ms)
{
    if (frame_thirty_level())
        return 1000.0 / 30;
    if (ms <= 0 && frame_rate_uncapped())
        return 1000.0 / frame_limit_screen();
    return ms;
}

/* ------------------------------------------------------------------ command line: the level loader */
int port_arg(int argc, char **argv, int i)
{
    const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
    if (!strcmp(a, "--level") && v) { levels_cli(v, NULL, NULL, 0); return 2; }
    if (!strcmp(a, "--character") && v) { levels_cli(NULL, v, NULL, 0); return 2; }
    if (!strcmp(a, "--difficulty") && v) { levels_cli(NULL, NULL, v, 0); return 2; }
    if (!strcmp(a, "--list-levels")) { levels_cli(NULL, NULL, NULL, 1); return 1; }
    return 0;
}

void port_usage(void)
{
    fprintf(stderr,
            "              [--level NAME [--character ghost|niobe] [--difficulty easy|normal|hard]] [--list-levels]\n"
            "  --level  start at a load point (a level file's name, NAME@LEVEL or LEVEL.POINT; add --window\n"
            "           to play it); --list-levels prints the table\n");
}

/* ------------------------------------------------------------------ files: which movie plays, skip_intro
   game.c follows which film is opened (movie_opened); options.txt skip_intro = on (for tests)
   hides the start-up logo and legal films, so the game goes on to its menus at once */
extern void movie_opened(const char *winpath);
int port_file_hidden(const char *winpath)
{
    static int on = -1;
    if (!winpath)
        return 0;
    movie_opened(winpath);
    if (on < 0) on = option_bool("skip_intro", 0);
    if (!on)
        return 0;
    const char *b = strrchr(winpath, '\\');
    b = b ? b + 1 : winpath;
    static const char *const intro[] = {"atari.avi", "wb.avi", "shiny.avi", "bandai.avi"};
    for (size_t i = 0; i < sizeof intro / sizeof *intro; i++)
        if (!strcasecmp(b, intro[i]))
            return 1;
    return 0;
}

/* ------------------------------------------------------------------ sound
   Miles' 3D providers in the order the game opens them; the first that opens sets its EAX level
   (3..0 at 0x9955a8) */
const char *const *port_miles_providers(void)
{
    static const char *const names[] = {"Creative Labs EAX 3 (TM)", "Creative Labs EAX 2 (TM)",
                                        "Creative Labs EAX (TM)", "Miles Fast 2D Positional Audio", NULL};
    return names;
}

/* ------------------------------------------------------------------ controllers
   Enter the Matrix reads a DirectInput joystick (0x7b4b78): the X/Y axes move, RotZ strafes
   (0x7b53c2), the D-pad works the menus, and buttons 1-12 and the D-pad's directions can be bound
   to actions; with a joystick present the game binds them itself [verified: the binding table at
   0x97f844 read at run time]:
     Button 1 punch / fire (on foot), 2 primary action and the menus' accept, 3 jump, 4 kick,
     5 switch shooting / hand-to-hand, 6 (right mouse's other action), 7 Focus, 8 action /
     defend, 10 weapon menu, 12 first person; D-pad up / down weapons, right zoom out;
     driving: 3 accelerate, 7 brake, 8 emergency brake, 2 view, 4 Ghost assist, D-pad looks;
     hovercraft: 3 / 4 speed, 2 view, 1 fire.
   The controller's buttons are numbered as the 2004 repack's Xidi "Matrix" layout numbers them
   (Xidi.ini), which with those defaults gives the Xbox version's layout (its manual): A jump,
   B kick, X action / defend, Y punch, LT Focus, RT fire, Back weapon menu, right stick click
   first person, Start pause; LB and RB (no button in Xidi's layout) get buttons 5 and 6:
     left stick   X, Y (full at 90 % of its travel, Xidi's SaturationPercentStickLeft)
     right stick  RotZ (across: strafe), Z (up and down)
     D-pad        POV 0
     Y 1, RT 2, A 3, B 4, LB 5, RB 6, LT 7, X 8, Back 10, left stick click 11, right stick click 12
     Start        the Escape key (the pause menu)
   In menus A also presses Enter and B Escape (the game's joystick accept is button 2, the right
   trigger); in cut scenes A is Escape, which skips them (Enter doesn't [verified: the level
   loader's sweep, 2026-10-29: six Enter presses left the New Game scene running, Escape ended it]). */
#define DIK_ESCAPE 0x01
#define DIK_RETURN 0x1c
#define TRIGGER_DOWN 8000      /* a trigger counts as its button from a quarter of its travel */
#define LEFT_FULL 0.90         /* the left stick reaches full output at 90 % of its travel */

static int joy_button(int b)
{
    switch (b) {
    case PAD_Y: return 0;
    case PAD_A: return 2;
    case PAD_B: return 3;
    case PAD_LB: return 4;
    case PAD_RB: return 5;
    case PAD_X: return 7;
    case PAD_BACK: return 9;
    case PAD_LSTICK: return 10;
    case PAD_RSTICK: return 11;
    }
    return -1;
}
#define JOY_LT 6
#define JOY_RT 1

static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

const char *port_pad_name(int joy) { (void)joy; return "Controller (Matrix)"; }

/* st: every controller merged, past the toolkit's round dead zone (its rest stretched to full at
   the stick's edge) */
int port_pad_send(int joy, const PadState *st)
{
    const char *dzs = option_str("controller_deadzone", NULL);
    double dzp = dzs ? atof(dzs) : 6;
    double dz = (dzp < 0 ? 0 : dzp > 90 ? 90 : dzp) / 100 * 32767;
    double left = (32767 - dz) / (LEFT_FULL * 32767 - dz);     /* full at 90 % instead */
    input_joy_axis(joy, INPUT_AXIS_X, clamp16(st->axis[PAD_LX] * left));
    input_joy_axis(joy, INPUT_AXIS_Y, clamp16(st->axis[PAD_LY] * left));
    input_joy_axis(joy, INPUT_AXIS_RZ, st->axis[PAD_RX]);
    input_joy_axis(joy, INPUT_AXIS_Z, st->axis[PAD_RY]);
    input_joy_button(joy, JOY_RT, st->axis[PAD_RT] > TRIGGER_DOWN);
    input_joy_button(joy, JOY_LT, st->axis[PAD_LT] > TRIGGER_DOWN);
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (joy_button(b) >= 0)
            input_joy_button(joy, joy_button(b), st->button[b]);
    int u = st->button[PAD_DUP], d = st->button[PAD_DDOWN], lf = st->button[PAD_DLEFT], rg = st->button[PAD_DRIGHT];
    int v = INPUT_POV_CENTERED;
    if (u && rg) v = 4500; else if (rg && d) v = 13500;
    else if (d && lf) v = 22500; else if (lf && u) v = 31500;
    else if (u) v = 0; else if (rg) v = 9000; else if (d) v = 18000; else if (lf) v = 27000;
    input_joy_pov(joy, 0, v);

    /* keys: Start is Escape while held; A and B press the key the moment calls for when pressed,
       and that key is the one let go */
    static int start_sent;
    static uint8_t keyed[2];
    if (st->button[PAD_START] != start_sent) {
        start_sent = st->button[PAD_START];
        input_key(DIK_ESCAPE, start_sent);
    }
    for (int k = 0; k < 2; k++) {
        int down = st->button[k ? PAD_B : PAD_A];
        if (down && !keyed[k]) {
            int dik = game_front_end() ? (k ? DIK_ESCAPE : DIK_RETURN) : !k && game_cutscene() ? DIK_ESCAPE : 0;
            if (dik) {
                keyed[k] = (uint8_t)dik;
                input_key(dik, 1);
            } else
                keyed[k] = 0xff;        /* (held with nothing pressed: nothing until let go) */
        } else if (!down && keyed[k]) {
            if (keyed[k] != 0xff)
                input_key(keyed[k], 0);
            keyed[k] = 0;
        }
    }
    return 1;
}

/* ------------------------------------------------------------------ options.txt's comments */
static const char *const help[][2] = {
    {"display", "display: window or fullscreen (Alt+Enter switches too)"},
    {"vsync", "vsync: on (frames wait for the screen) or off"},
    {"antialiasing", "antialiasing: off, 2x, 4x or 8x (multisampling)"},
    {"rumble", "rumble: on or off (controller vibration)"},
    {"frame_limit", "frame_limit: off (the game's own pacing: up to 60 a second, the Xbox version's rate), screen (3 under the screen's refresh) or a rate (30 = the repack's cap)"},
    {"frame_rate", "frame_rate: 60 (the default: the game's own, one frame per 60 Hz update) or uncapped (in game, frames "
                   "drawn between updates with what moves blended, at the screen's rate (3 under it) or frame_limit's; "
                   "the game itself still runs 60 updates a second); driving and hovercraft always draw at 30"},
    {"resolution", "resolution: desktop (the default: the screen's size), original (MatrixConfig.ini's RESOLUTION, "
                   "0-6 = 640x480 .. 2048x1536) or WxH"},
    {"anisotropic", "anisotropic: 16, 8, 4, 2 or off (texture filtering)"},
    {"draw_distance", "draw_distance: how far you see in the driving levels, the fog and the far plane scaled: 1 (the default: the original), 1.5, 2 or 3"},
    {"renderer", "renderer: dxvk (the default) or vulkan (our own Vulkan renderer, VULKAN.md; DXVK if it cannot start); "
                 "read at the start"},
    {"hud", "hud: corners (the default: on wide screens the HUD sits at the screen's edges, as far from them as on a 4:3 "
            "screen) or center (in the centred 4:3 area); with ui = center"},
    {"movies", "movies: fill (the default: the story's cinematics, films letterboxed in their 4:3 picture, drawn across a wide "
               "screen) or fit (every movie pillarboxed in 4:3)"},
    {"cutscenes", "cutscenes: fill (the default: the in-engine scenes, framed for about 16:9 inside black bars, fill a wide "
                  "screen with their original framing) or bars (the game's bars over the wider view)"},
    {"ui", "ui: center (the default: menus, text, HUD and movies keep their 4:3 shape on wide screens, fades and "
           "bars still fill the screen) or stretch (the game's own)"},
    {NULL, NULL}};

const char *port_option_help(const char *key)
{
    for (int i = 0; help[i][0]; i++)
        if (!strcasecmp(help[i][0], key))
            return help[i][1];
    return NULL;
}
