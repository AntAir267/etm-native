/* Controllers as DirectInput joysticks (pad.h): the mapping SDL's gamepad events go through
   (win.c), and ETM_PAD, a scripted controller for headless tests that goes through the
   same calls.

   Enter the Matrix reads a DirectInput joystick (0x7b4b78): the X/Y axes move, RotZ strafes
   (0x7b53c2), the D-pad works the menus, and buttons 1-12 and the D-pad's directions can be
   bound to actions; with a joystick present the game binds them itself [verified: the binding
   table at 0x97f844 read at run time]:
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
   In menus A also presses Enter and B Escape (the game's joystick accept is button 2, the
   right trigger).    Dead zone: a round one on both sticks against resting drift (controller_deadzone in
   options.txt, percent, default 6), the rest of the travel stretched over the range. */
#define _GNU_SOURCE
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "pad.h"

typedef struct {
    int connected;
    uint8_t button[PAD_BUTTONS];
    int axis[PAD_AXES];
} Pad;

static pthread_mutex_t pad_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[INPUT_MAX_JOYS];
static double stick_dead = 0.06 * 32767;
/* TWO PLAYERS (split.c): while a two-player race runs, the second controller connected is player
   2's: it reaches neither the game's joystick (player 1's) nor the menus' keys, and is read as an
   Xbox pad (pad_p2_xbox) */
static int split_slot = -1;

/* SDL button -> joystick button (0-based: "Button 1" is 0), -1 none */
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
#define TRIGGER_DOWN 8000      /* a trigger counts as its button from a quarter of its travel */
#define DIK_ESCAPE 0x01
#define LEFT_FULL 0.90         /* the left stick reaches full output at 90 % of its travel */

/* keys the controller presses in menus (DirectInput key codes) */
#define DIK_RETURN 0x1c
#define DIK_SPACE 0x39
#define DIK_UP 0xc8
#define DIK_LEFT 0xcb
#define DIK_RIGHT 0xcd
#define DIK_DOWN 0xd0
extern int game_driving(void);

/* the front end (menus) as against play: game.c */
extern int game_front_end(void);
static int front_end(void) { return game_front_end(); }
/* an in-engine cut scene (game.c: the cinema bars are up) */
extern int game_cutscene(void);


static int clamp16(double v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : (int)lround(v); }

static int last_controller;             /* the device used last was a controller */
void input_used(int controller) { last_controller = controller != 0; }
int input_controller_last(void) { return last_controller; }

/* ONE JOYSTICK FOR THE GAME: the game reads the first joystick it finds, so every controller is
   merged into one (buttons held on any, each stick from the controller pushing it furthest, rumble
   on all). Several devices for one controller are common: a handheld's own pad, its daemon's
   virtual one and the raw one it hides (the GPD Win Max 2 shows "Generic X-Box pad", "Microsoft
   X-Box 360 pad" and "Handheld Daemon Controller", some silent: the game read a silent one and its
   sticks and buttons did nothing [verified: Ant's run, 2026-10-27; /proc/bus/input/devices]).
   pads[] holds each controller; game_joy is the game's joystick. */
static int game_joy = -1;
int pad_game_joystick(void) { return game_joy; }

/* Plugging a controller in counts as using it (the prompts switch to it); unplugging the last
   one hands them back to the keyboard. */
int pad_attach(const char *id, const char *name)
{
    int slot = -1;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS && slot < 0; s++)
        if (!pads[s].connected)
            slot = s;
    if (slot >= 0) {
        memset(&pads[slot], 0, sizeof pads[slot]);
        pads[slot].connected = 1;
    }
    pthread_mutex_unlock(&pad_lock);
    if (slot < 0)
        return -1;
    (void)id;
    if (game_joy < 0) {
        game_joy = input_joy_attach("controllers", name, 6, 16, 1);
        if (game_joy >= 0)
            input_joy_set_ff(game_joy, ifc_rumble_wanted());    /* the game plays force feedback as rumble */
    }
    input_used(1);
    return slot;
}

static void send_all(void);
void pad_detach(int slot)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS)
        return;
    pthread_mutex_lock(&pad_lock);
    memset(&pads[slot], 0, sizeof pads[slot]);
    pthread_mutex_unlock(&pad_lock);
    if (!pad_count()) {
        if (game_joy >= 0)
            input_joy_disconnect(game_joy);
        game_joy = -1;
        input_used(0);
    } else {
        send_all();                 /* (what the unplugged one held lets go) */
    }
}

/* the left stick (round dead zone; full output at LEFT_FULL of its travel) on X / Y, from the
   controller pushing it furthest */
static void steer(void)
{
    double full = LEFT_FULL * 32767, best = -1;
    int sx = 0, sy = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        if (!pads[s].connected || s == split_slot)
            continue;
        int x = pads[s].axis[PAD_LX], y = pads[s].axis[PAD_LY];
        double r = hypot(x, y), k = r > stick_dead ? (r - stick_dead) / (full - stick_dead) * 32767 / r : 0;
        if (r > best) {
            best = r;
            sx = clamp16(x * k);
            sy = clamp16(y * k);
        }
    }
    pthread_mutex_unlock(&pad_lock);
    input_joy_axis(game_joy, INPUT_AXIS_X, sx);
    input_joy_axis(game_joy, INPUT_AXIS_Y, sy);
}

/* the left stick's X for the car's steering (game.c): -1 .. 1 past the round dead zone, the
   connected controller pushed furthest */
double pad_steer(void)
{
    double best = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        if (!pads[s].connected || s == split_slot)
            continue;
        double x = pads[s].axis[PAD_LX], y = pads[s].axis[PAD_LY], r = hypot(x, y);
        double v = r > stick_dead ? x * (r - stick_dead) / (32767 - stick_dead) / r : 0;
        if (fabs(v) > fabs(best))
            best = v;
    }
    pthread_mutex_unlock(&pad_lock);
    return best > 1 ? 1 : best < -1 ? -1 : best;
}

/* the right stick (round dead zone, the rest stretched over the range): across on RotZ, up and
   down on Z; from the controller pushing it furthest */
static void right_stick(void)
{
    double best = -1;
    int sx = 0, sy = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++) {
        if (!pads[s].connected || s == split_slot)
            continue;
        int x = pads[s].axis[PAD_RX], y = pads[s].axis[PAD_RY];
        double r = hypot(x, y), k = r > stick_dead ? (r - stick_dead) / (32767 - stick_dead) * 32767 / r : 0;
        if (r > best) {
            best = r;
            sx = clamp16(x * k);
            sy = clamp16(y * k);
        }
    }
    pthread_mutex_unlock(&pad_lock);
    input_joy_axis(game_joy, INPUT_AXIS_RZ, sx);
    input_joy_axis(game_joy, INPUT_AXIS_Z, sy);
}

/* a button held on any controller (player 2's apart) */
static int held_any(int button)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && s != split_slot && pads[s].button[button];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

static int trigger_any(int axis)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && s != split_slot && pads[s].axis[axis] > TRIGGER_DOWN;
    pthread_mutex_unlock(&pad_lock);
    return r;
}

/* the D-pad as the hat, from the D-pad buttons held on any controller */
static void send_pov(void)
{
    int u = held_any(PAD_DUP), d = held_any(PAD_DDOWN), lf = held_any(PAD_DLEFT), rg = held_any(PAD_DRIGHT);
    int v = INPUT_POV_CENTERED;
    if (u && rg) v = 4500; else if (rg && d) v = 13500;
    else if (d && lf) v = 22500; else if (lf && u) v = 31500;
    else if (u) v = 0; else if (rg) v = 9000; else if (d) v = 18000; else if (lf) v = 27000;
    input_joy_pov(game_joy, 0, v);
}

/* the Escape key while Start is held on any controller */
static void send_start(void)
{
    static int sent;
    int h = held_any(PAD_START);
    if (h != sent) {
        sent = h;
        input_key(DIK_ESCAPE, h);
    }
}

/* everything again (a controller unplugged) */
static void send_all(void)
{
    if (game_joy < 0)
        return;
    steer();
    right_stick();
    input_joy_button(game_joy, JOY_RT, trigger_any(PAD_RT));
    input_joy_button(game_joy, JOY_LT, trigger_any(PAD_LT));
    for (int b = 0; b < PAD_BUTTONS; b++)
        if (joy_button(b) >= 0)
            input_joy_button(game_joy, joy_button(b), held_any(b));
    send_pov();
    send_start();
}

void pad_axis(int slot, int axis, int value)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || axis < 0 || axis >= PAD_AXES)
        return;
    pthread_mutex_lock(&pad_lock);
    pads[slot].axis[axis] = value;
    pthread_mutex_unlock(&pad_lock);
    if (slot == split_slot || game_joy < 0)
        return;
    if (abs(value) > 12000)
        input_used(1);
    if (axis == PAD_LT || axis == PAD_RT) {
        input_joy_button(game_joy, axis == PAD_RT ? JOY_RT : JOY_LT, trigger_any(axis));
        return;
    }
    if (axis == PAD_LX || axis == PAD_LY) {
        steer();
        return;
    }
    right_stick();
}

void pad_button(int slot, int button, int down)
{
    if (slot < 0 || slot >= INPUT_MAX_JOYS || button < 0 || button >= PAD_BUTTONS)
        return;
    if (down) {
        input_used(1);
        video_skip_press();             /* (any button skips an intro video: video.c) */
    }
    pthread_mutex_lock(&pad_lock);
    pads[slot].button[button] = down != 0;
    pthread_mutex_unlock(&pad_lock);
    if (slot == split_slot || game_joy < 0)
        return;
    if (button == PAD_START) {                  /* the Escape key: the game's pause menu, skips films */
        send_start();
        return;
    }
    /* in menus A is Enter and B Escape too, and in cut scenes A is Enter (it skips them); the
       button pressed is the one released */
    if (button == PAD_A || button == PAD_B) {
        static uint8_t keyed[INPUT_MAX_JOYS][2];
        int k = button == PAD_B, dik = k ? DIK_ESCAPE : DIK_RETURN;
        if (down && (front_end() || (!k && game_cutscene()))) {
            keyed[slot][k] = 1;
            input_key(dik, 1);
        } else if (!down && keyed[slot][k]) {
            keyed[slot][k] = 0;
            input_key(dik, 0);
        }
    }
    if (button >= PAD_DUP && button <= PAD_DRIGHT) {
        send_pov();
        return;
    }
    int jb = joy_button(button);
    if (jb >= 0)
        input_joy_button(game_joy, jb, held_any(button));
}

int pad_held(int button)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        r |= pads[s].connected && s != split_slot && pads[s].button[button];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

int pad_axis_value(int axis)
{
    int r = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        if (pads[s].connected && s != split_slot && abs(pads[s].axis[axis]) > abs(r))
            r = pads[s].axis[axis];
    pthread_mutex_unlock(&pad_lock);
    return r;
}

/* two players on (split.c, each frame of a two-player race) or off: the second controller connected
   is player 2's; the game's joystick for it is left at rest */
void pad_split(int on)
{
    int want = -1;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0, seen = 0; on && s < INPUT_MAX_JOYS && want < 0; s++)
        if (pads[s].connected && seen++) want = s;
    int was = split_slot;
    split_slot = want;
    pthread_mutex_unlock(&pad_lock);
    if (want == was)
        return;
    if (want >= 0) {
        for (int a = 0; a < INPUT_JOY_AXES; a++) input_joy_axis(want, a, 0);
        for (int b = 0; b < INPUT_JOY_BUTTONS; b++) input_joy_button(want, b, 0);
        input_joy_pov(want, 0, INPUT_POV_CENTERED);
        LOG(LOG_ALWAYS, "two players: controller %d is player 2's", want);
    }
}

/* player 2's controller as the Xbox pad the game reads: buttons (1 up, 2 down, 4 left, 8 right,
   0x10 Start, 0x20 Back, 0x40/0x80 the sticks), the analog buttons 0-255 (A B X Y Black White LT
   RT; RB is Black, LB White), the sticks (+ up, round dead zone). 0 if there is none. */
int pad_p2_xbox(uint16_t *buttons, uint8_t analog[8], int16_t stick[4])
{
    pthread_mutex_lock(&pad_lock);
    int s = split_slot;
    if (s < 0 || !pads[s].connected) {
        pthread_mutex_unlock(&pad_lock);
        return 0;
    }
    const Pad *p = &pads[s];
    static const int bits[8][2] = {{PAD_DUP, 1}, {PAD_DDOWN, 2}, {PAD_DLEFT, 4}, {PAD_DRIGHT, 8},
                                   {PAD_START, 0x10}, {PAD_BACK, 0x20}, {PAD_LSTICK, 0x40}, {PAD_RSTICK, 0x80}};
    uint16_t b = 0;
    for (int i = 0; i < 8; i++)
        if (p->button[bits[i][0]]) b |= (uint16_t)bits[i][1];
    *buttons = b;
    static const int abtn[6] = {PAD_A, PAD_B, PAD_X, PAD_Y, PAD_RB, PAD_LB};
    for (int i = 0; i < 6; i++)
        analog[i] = p->button[abtn[i]] ? 255 : 0;
    analog[6] = (uint8_t)(p->axis[PAD_LT] > 0 ? p->axis[PAD_LT] * 255 / 32767 : 0);
    analog[7] = (uint8_t)(p->axis[PAD_RT] > 0 ? p->axis[PAD_RT] * 255 / 32767 : 0);
    for (int k = 0; k < 2; k++) {
        double x = p->axis[k ? PAD_RX : PAD_LX], y = p->axis[k ? PAD_RY : PAD_LY], r = hypot(x, y);
        double f = r > stick_dead ? (r - stick_dead) / (32767 - stick_dead) * 32767 / r : 0;
        stick[2 * k] = (int16_t)clamp16(x * f);
        stick[2 * k + 1] = (int16_t)clamp16(-y * f);
    }
    pthread_mutex_unlock(&pad_lock);
    return 1;
}

int pad_count(void)
{
    int n = 0;
    pthread_mutex_lock(&pad_lock);
    for (int s = 0; s < INPUT_MAX_JOYS; s++)
        n += pads[s].connected;
    pthread_mutex_unlock(&pad_lock);
    return n;
}

/* ------------------------------------------------------------------ ETM_PAD */
/* axis: 0 a button, 1 an axis, 2 plugging (index 0 unplug, 1 the controller, 2 another one);
   p2: player 2's controller ("P2." before the name; connected at its first event) */
typedef struct { unsigned long at, release; int axis, index, value, hold, state, p2, relative; } PadPress;
static PadPress *script;
static int nscript, script_slot = -1, script_slot2 = -1;
static const char *const plug_names[3] = {"UNPLUG", "PLUG", "PLUG2"};

static const struct { const char *name; int button; } button_names[] = {
    {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"BACK", PAD_BACK}, {"VIEW", PAD_BACK},
    {"SELECT", PAD_BACK}, {"GUIDE", PAD_GUIDE}, {"START", PAD_START}, {"MENU", PAD_START},
    {"LS", PAD_LSTICK}, {"L3", PAD_LSTICK}, {"RS", PAD_RSTICK}, {"R3", PAD_RSTICK},
    {"LB", PAD_LB}, {"RB", PAD_RB}, {"UP", PAD_DUP}, {"DOWN", PAD_DDOWN}, {"LEFT", PAD_DLEFT},
    {"RIGHT", PAD_DRIGHT}, {"MISC", PAD_MISC1},
};
static const char *const axis_names[PAD_AXES] = {"LX", "LY", "RX", "RY", "LT", "RT"};

void pad_init(void)
{
    const char *dz = option_str("controller_deadzone", NULL);
    if (dz) {
        double v = atof(dz);
        stick_dead = (v < 0 ? 0 : v > 90 ? 90 : v) / 100 * 32767;
    }
    const char *env = getenv("ETM_PAD");
    if (!env || !*env)
        return;
    char *copy = strdup(env), *save = NULL;
    for (char *tok = strtok_r(copy, ", \t\n", &save); tok; tok = strtok_r(NULL, ", \t\n", &save)) {
        char *colon = strchr(tok, ':'), *end;
        int relative = tok[0] == '+';           /* "+frames:": from the scripted input's mark (input.h) */
        unsigned long at = colon ? strtoul(tok + relative, &end, 10) : 0;
        if (!colon || end != colon) {
            LOG(LOG_ALWAYS, "ETM_PAD: \"%s\" is not frame:BUTTON or frame:AXIS=value", tok);
            continue;
        }
        char *name = colon + 1, *star = strchr(name, '*'), *eq = strchr(name, '=');
        int hold = 3, p2 = 0;
        if (!strncasecmp(name, "P2.", 3)) {
            p2 = 1;
            name += 3;
        }
        if (star) {
            *star = 0;
            hold = atoi(star + 1) > 0 ? atoi(star + 1) : 1;
        }
        PadPress p = {at, 0, 0, -1, 1, hold, 0, p2, relative};
        for (int k = 0; k < 3 && !eq; k++)
            if (!strcasecmp(name, plug_names[k])) {
                p.axis = 2;
                p.index = k;
            }
        if (p.axis == 2) {
        } else if (eq) {
            *eq = 0;
            p.axis = 1;
            p.value = atoi(eq + 1);
            for (int a = 0; a < PAD_AXES; a++)
                if (!strcasecmp(name, axis_names[a]))
                    p.index = a;
        } else {
            for (size_t k = 0; k < sizeof button_names / sizeof *button_names; k++)
                if (!strcasecmp(name, button_names[k].name))
                    p.index = button_names[k].button;
        }
        if (p.index < 0) {
            LOG(LOG_ALWAYS, "ETM_PAD: unknown %s \"%s\"", eq ? "axis" : "button", name);
            continue;
        }
        script = realloc(script, (nscript + 1) * sizeof *script);
        script[nscript++] = p;
    }
    free(copy);
    int first = -1;                 /* the first plugging event: if it plugs in, none at start-up */
    for (int k = 0; k < nscript; k++)
        if (script[k].axis == 2 && (first < 0 || script[k].at < script[first].at))
            first = k;
    if (first >= 0 && script[first].index) {
        LOG(LOG_ALWAYS, "ETM_PAD: %d scripted inputs, the controller not connected yet", nscript);
        return;
    }
    script_slot = pad_attach("insane-pad-script", "Scripted controller");
    LOG(LOG_ALWAYS, "ETM_PAD: controller connected as joystick %d, %d scripted inputs", script_slot, nscript);
}

static void script_plug(unsigned long frame, int which)
{
    if (script_slot >= 0) {
        LOG(LOG_ALWAYS, "ETM_PAD: frame %lu: joystick %d unplugged", frame, script_slot);
        pad_detach(script_slot);
        script_slot = -1;
    }
    if (which) {
        script_slot = which == 1 ? pad_attach("insane-pad-script", "Scripted controller")
                                 : pad_attach("insane-pad-script-2", "Another scripted controller");
        LOG(LOG_ALWAYS, "ETM_PAD: frame %lu: %s connected as joystick %d", frame,
            which == 1 ? "controller" : "another controller", script_slot);
    }
}

void pad_frame(unsigned long frame)
{
    for (int k = 0; k < nscript; k++) {
        PadPress *s = &script[k];
        int down = -1;
        long mark = s->relative ? input_script_mark_frame() : 0;
        if (s->state == 0 && mark >= 0 && frame >= s->at + (unsigned long)mark) {
            s->state = 1;
            s->release = frame + (unsigned long)s->hold;
            down = 1;
        } else if (s->state == 1 && frame >= s->release) {
            s->state = 2;
            down = 0;
        }
        if (down < 0)
            continue;
        if (s->p2 && script_slot2 < 0) {
            script_slot2 = pad_attach("insane-pad-script-p2", "Player 2's scripted controller");
            LOG(LOG_ALWAYS, "ETM_PAD: frame %lu: player 2's controller connected as joystick %d", frame, script_slot2);
        }
        int slot = s->p2 ? script_slot2 : script_slot;
        if (s->axis == 2) {
            if (down)
                script_plug(frame, s->index);
        } else if (s->axis) {
            LOG(LOG_ALWAYS, "ETM_PAD: frame %lu: %s%s = %d", frame, s->p2 ? "P2." : "", axis_names[s->index], down ? s->value : 0);
            pad_axis(slot, s->index, down ? s->value : 0);
        } else {
            const char *n = "?";
            for (size_t b = 0; b < sizeof button_names / sizeof *button_names; b++)
                if (button_names[b].button == s->index) {
                    n = button_names[b].name;
                    break;
                }
            LOG(LOG_ALWAYS, "ETM_PAD: frame %lu: %s%s %s", frame, s->p2 ? "P2." : "", n, down ? "down" : "up");
            pad_button(slot, s->index, down);
        }
    }
    padgame_frame(frame);
}
