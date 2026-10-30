/* BUTTON PROMPTS (DESIGN.md decision 13; Ant: "look into adding button prompts"). While a controller is
   the device in use, the game names the controller's buttons where it names its controls; with the
   keyboard and mouse everything is the game's own. Option button_prompts = auto (the default: the
   device used last, input_controller_last(), switching as the player changes device) | keyboard |
   controller; Options > Controls has the row (menu.c).

   WHERE THE PC GAME SHOWS ITS CONTROLS [verified: the disassembly of the functions named, the Text\
   tables decoded (UTF-16: dword 0, count, offsets from +8), frames of each]:
   - The Controls pages (Fighting 0x66ee7d, Driving 0x66fabe, Hovercraft 0x670a4d: the pages' draw
     functions, every frame) show each action's two bindings, the keyboard / mouse slot at x 220 and
     the joystick slot at x 420 (filled only when a joystick is present at start-up: the game's
     defaults), as names from 0x66c9c4(code) (through 0x66df54(action, slot), binding
     [0x97f844 + action x 8 + slot x 4]): a key's name, "Mouse Button 1..6" / "Wheel Up / Down" for
     -1..-8, "Joy Button 1..16" for -9..-24 (Text\TX<language>Stc.bin 551-566). Joystick buttons
     1-12 are -9..-20; 13-16 (-21..-24) are the D-pad's up, right, down, left (0x7b4b78 sets bytes
     +0x25c..+0x25f of the input object from POV 0 / 9000 / 18000 / 27000, +-2000). key_name below
     names them as the controller's buttons (pad.c's numbering: the repack's Xidi "Matrix" layout).
   - The tutorial tips (the popup in play and the pause menu's Tips list: Text\TX<language>IMT.bin) and
     IMF.bin's texts name controls by action in capitals ("Press FIRE to draw a weapon", "Use MOVEMENT
     to walk and turn", "Press PAUSE MENU..."): no key names, no placeholders, nothing in the program
     that substitutes them. text_get below adds the controller's button after each such word, "Press
     FIRE (RT) to draw a weapon", "MOVEMENT (left stick up)", from the binding table, so a remapped
     button shows as remapped. The texts come through the game's text lookup 0x65526b(text object, id,
     out) [verified: disassembly]; the HUD keeps IMT.bin's object at [0x97cef0]+0xfc, Hud2.bin's at
     +0xd64 and pointers to IM.bin's at [+0xf4], IMF.bin's at [+0xf8] (0x5ff8d5 loads them).
   - The HUD's context prompt near something to use: a hand icon and the action's name: see below.
   - Menus have no button hints on the PC (language, title, Options, Controls and pause pages
     [verified: frames]).
   - Xbox button art in the PC's data: Levels\frontend_XBOX.3d2_42 (loaded by the PC's front end)
     holds "xbox_buttons" (the D-pad and the A / B / X / Y buttons, 128 x 64 DXT1), the controller
     line drawing "icon_xbox_controller" and the green controller "hud_controller_xbox" (256 x 256
     DXT5) [verified: decoded, tools/tex3d2.py]; no trigger, bumper, Start or Back art, and in play the
     HUD's texture lookup does not have them [verified: called at run time]. The prompts are text in
     the game's own fonts (what the game draws its key names and tips with). ETM_LOG_TEXT=1 logs each
     text the game fetches (object, id, caller), 2 every fetch. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "decomp/decomp.h"

#define TEXT_GET        0x65526bu       /* (text object, id, out) -> UTF-16 text */
#define HUD             0x97cef0u       /* the in-game HUD object (its text objects) */
#define STC_TEXT        R32(0x97efc0)   /* Text\TX<language>Stc.bin's text object */
#define BINDINGS        0x97f844u       /* [action x 8 + slot x 4]: slot 0 keyboard / mouse, 1 joystick */
#define STC_LTHUMB      429             /* "Left thumbstick button" */
#define STC_RTHUMB      430             /* "Right thumbstick button" */

int levels_point_type(void);

/* ------------------------------------------------------------------ the mode */
static int mode = -1;                   /* 0 auto, 1 keyboard, 2 controller */

static void mode_read(void)
{
    const char *v = option_str("button_prompts", "auto");
    mode = !strcasecmp(v, "keyboard") ? 1 : !strcasecmp(v, "controller") ? 2 : 0;
}

/* the in-game option changed (menu.c) */
void prompts_option_changed(void) { mode = -1; }

int prompts_controller(void)
{
    if (mode < 0)
        mode_read();
    return mode == 2 || (mode == 0 && input_controller_last());
}

/* ------------------------------------------------------------------ texts in program memory */
/* our UTF-16 texts, each made once and kept (the game may keep a text's address while it shows it) */
typedef struct { uint32_t src, hash, g; } Made;
static Made made[2048];
static int nmade;

static uint32_t fnv(const uint16_t *s, size_t n, uint32_t h)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ s[i]) * 16777619u;
    return h;
}

static uint32_t wmake(uint32_t src, const uint16_t *s, size_t n)
{
    uint32_t h = fnv(s, n, 2166136261u);
    for (int i = 0; i < nmade; i++)
        if (made[i].src == src && made[i].hash == h)
            return made[i].g;
    uint32_t g = galloc(2 * (uint32_t)n + 2);
    for (size_t i = 0; i < n; i++)
        W16(g + 2 * (uint32_t)i, s[i]);
    W16(g + 2 * (uint32_t)n, 0);
    if (nmade < (int)(sizeof made / sizeof *made))
        made[nmade++] = (Made){src, h, g};
    return g;
}

static uint32_t wmake_ascii(const char *a)
{
    uint16_t w[64];
    size_t n = 0;
    for (; a[n] && n < 63; n++)
        w[n] = (uint8_t)a[n];
    return wmake(0, w, n);
}

/* ------------------------------------------------------------------ the controller's buttons */
/* by joystick code (-9 - n for button n + 1): pad.c's numbering (Xidi's "Matrix" layout plus LB / RB) */
static const char *const btn_short[16] = {
    "Y", "RT", "A", "B", "LB", "RB", "LT", "X", NULL, "Back", "LS click", "RS click",
    "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"};
static const char *const btn_long[16] = {
    "Y Button", "Right Trigger", "A Button", "B Button", "Left Bumper", "Right Bumper", "Left Trigger",
    "X Button", NULL, "Back Button", NULL /* the game's "Left thumbstick button" */,
    NULL /* "Right thumbstick button" */, "D-pad Up", "D-pad Right", "D-pad Down", "D-pad Left"};

static const char *code_short(int32_t code)
{
    return code <= -9 && code >= -24 ? btn_short[-9 - code] : NULL;
}

/* an action's controller binding: the joystick slot, or the keyboard slot if a button was put there */
static int32_t action_button(int action)
{
    int32_t j = (int32_t)R32(BINDINGS + 8u * (uint32_t)action + 4), k = (int32_t)R32(BINDINGS + 8u * (uint32_t)action);
    return code_short(j) ? j : code_short(k) ? k : 0;
}

/* ------------------------------------------------------------------ hook: a binding's name */
/* 0x66c9c4(code), cdecl, the name's UTF-16 text in eax */
static int hook_key_name_fn(CPU *c)
{
    int32_t code = (int32_t)ARG(0);
    if (code > -9 || code < -24 || !prompts_controller())
        return 0;
    int b = -9 - code;
    uint32_t text = 0;
    if (btn_long[b])
        text = wmake_ascii(btn_long[b]);
    else if (b == 10 || b == 11) {
        uint32_t entry = c->esp;
        text = game_callm(c, TEXT_GET, STC_TEXT, 2, b == 10 ? STC_LTHUMB : STC_RTHUMB, 0);
        c->esp = entry;
    }
    if (!text)
        return 0;                       /* (button 9: not on the controller; the game's name) */
    return decomp_ret(c, c->esp, text, 0);
}

/* ------------------------------------------------------------------ the tips' control words */
/* context: the load point's kind picks the vehicle's bindings for the words they share */
enum { ON_FOOT, DRIVING, HOVERCRAFT };
typedef struct {
    const char *word;           /* as the tips write it, capitals */
    const char *as;             /* the word written instead (NULL: kept) */
    short act[3][2];            /* the actions named on foot, driving, hovercraft (-1 none) */
    char join;                  /* between two actions: '+' together, '/' either */
    const char *fixed;          /* or a control that is no binding (sticks, Start, the D-pad) */
} Word;
#define A1(a)       {{a, -1}, {a, -1}, {a, -1}}
#define NONE        {{-1, -1}, {-1, -1}, {-1, -1}}
static const Word words[] = {   /* longest first where one begins another */
    {"ENTER/EXIT 1ST PERSON", NULL, {{14, 15}, {14, 15}, {14, 15}}, '/', NULL},
    {"ENTER/EXIT FIRST PERSON", NULL, {{14, 15}, {14, 15}, {14, 15}}, '/', NULL},
    {"1ST PERSON ZOOM IN/OUT", NULL, {{14, 15}, {14, 15}, {14, 15}}, '/', NULL},
    {"STRAFE LEFT/RIGHT", NULL, NONE, 0, "right stick"},
    {"STRAFE LEFT", NULL, NONE, 0, "right stick left"},
    {"STRAFE RIGHT", NULL, NONE, 0, "right stick right"},
    {"SECONDARY WEAPON", NULL, A1(5), 0, NULL},
    {"WEAPONS MENU", NULL, A1(18), 0, NULL},
    {"WEAPON SELECT", NULL, NONE, 0, "D-pad"},
    {"PAUSE MENU", NULL, NONE, 0, "Start"},
    {"EMERGENCY BRAKE", NULL, A1(0x1a), 0, NULL},
    {"SWITCH VIEW", NULL, {{0x1e, -1}, {0x1e, -1}, {0x24, -1}}, 0, NULL},
    {"ACCELERATE", NULL, {{0x18, -1}, {0x18, -1}, {0x22, -1}}, 0, NULL},
    {"BRAKE", NULL, {{0x19, -1}, {0x19, -1}, {0x23, -1}}, 0, NULL},
    {"ASSIST", NULL, A1(0x1f), 0, NULL},
    {"CANNON", NULL, A1(0x25), 0, NULL},
    {"FIRE", NULL, {{4, -1}, {0x26, -1}, {0x25, -1}}, 0, NULL},
    {"PUNCH", NULL, A1(7), 0, NULL},
    {"KICK", NULL, A1(8), 0, NULL},
    {"THROW", NULL, {{7, 8}, {7, 8}, {7, 8}}, '+', NULL},
    {"JUMP", NULL, A1(6), 0, NULL},
    {"ACTION", NULL, A1(10), 0, NULL},
    {"GRAB", NULL, A1(10), 0, NULL},
    {"FOCUS", NULL, A1(11), 0, NULL},
    {"MOVEMENT", NULL, NONE, 0, "left stick"},
    {"STEER", NULL, NONE, 0, "left stick"},
    {"AIM", NULL, NONE, 0, "left stick"},
    {"USE key", "USE button", A1(10), 0, NULL},
};
#define NWORDS ((int)(sizeof words / sizeof words[0]))

static int is_letter(uint16_t ch) { return (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch >= 0xc0; }

/* what a word's controls are called now, or 0 if it has none (left as written) */
static int word_buttons(const Word *w, int ctx, char *out, size_t n)
{
    if (w->fixed)
        return snprintf(out, n, "%s", w->fixed) > 0;
    const char *b0 = w->act[ctx][0] >= 0 ? code_short(action_button(w->act[ctx][0])) : NULL;
    const char *b1 = w->act[ctx][1] >= 0 ? code_short(action_button(w->act[ctx][1])) : NULL;
    if (b0 && b1 && strcmp(b0, b1))
        snprintf(out, n, w->join == '+' ? "%s+%s" : "%s / %s", b0, b1);
    else if (b0 || b1)
        snprintf(out, n, "%s", b0 ? b0 : b1);
    else
        return 0;
    return 1;
}

/* a tip with each control word followed by its buttons; 0 when nothing changes. A text all in
   capitals (a tip's title: "PUNCH", "FOCUS") stays as it is. The tip popup in play keeps 255
   characters (0x6089d0 copies the text with 0x657728(text, 0x100), a bounded append [verified:
   disassembly]): a tip that fits gets only the additions that still fit, so none of its own words is
   cut. */
#define TIP_MAX 2048
#define TIP_POPUP 255
static size_t tip_rewrite(const uint16_t *s, size_t n, uint16_t *out, size_t cap)
{
    if (n <= TIP_POPUP && cap > TIP_POPUP + 1)
        cap = TIP_POPUP + 1;
    int lower = 0;
    for (size_t i = 0; i < n && !lower; i++)
        lower = s[i] >= 'a' && s[i] <= 'z';
    if (!lower)
        return 0;
    int t = levels_point_type();
    int ctx = t == 1 || t == 2 ? DRIVING : t == 4 || t == 5 ? HOVERCRAFT : ON_FOOT;
    size_t o = 0;
    int changed = 0;
    for (size_t i = 0; i < n; ) {
        const Word *hit = NULL;
        size_t len = 0;
        if (!(i && is_letter(s[i - 1])))
            for (int k = 0; k < NWORDS && !hit; k++) {
                const char *w = words[k].word;
                size_t L = strlen(w), j = 0;
                while (j < L && i + j < n && s[i + j] == (uint8_t)w[j])
                    j++;
                if (j == L && !(i + L < n && is_letter(s[i + L]))) {
                    hit = &words[k];
                    len = L;
                }
            }
        char b[64];
        if (!hit || !word_buttons(hit, ctx, b, sizeof b)) {
            if (o + 1 < cap)
                out[o++] = s[i];
            i++;
            continue;
        }
        const char *as = hit->as ? hit->as : hit->word;
        /* "WORD (up)" -> "WORD (left stick up)"; else "WORD (RT)" */
        int inside = i + len + 1 < n && s[i + len] == ' ' && s[i + len + 1] == '(';
        char ins[80];
        snprintf(ins, sizeof ins, inside ? " (%s " : " (%s)", b);
        size_t grow = strlen(as) + strlen(ins) - len - (inside ? 2 : 0);
        if (o + grow + (n - i) + 1 > cap) {             /* (no room: the word as it is) */
            if (o + 1 < cap)
                out[o++] = s[i];
            i++;
            continue;
        }
        for (size_t k = 0; as[k]; k++)
            out[o++] = (uint8_t)as[k];
        i += len;
        for (size_t k = 0; ins[k]; k++)
            out[o++] = (uint8_t)ins[k];
        if (inside)
            i += 2;
        changed = 1;
    }
    return changed ? o : 0;
}

/* the tips' text objects: IMT.bin's (embedded in the HUD) and IMF.bin's */
static int tip_object(uint32_t obj)
{
    uint32_t hud = R32(HUD);
    return hud && obj && (obj == hud + 0xfc || obj == R32(hud + 0xf8));
}

/* THE HUD'S CONTEXT PROMPT: near something to use, 0x608128 (each frame) shows the hand icon
   ("icon_interact") with the action's name from Hud2.bin (the HUD's text object at +0xd64): ids 110-124
   (Activate, Deactivate, Turn On, Turn Off, Pick Up Body, Put Down Body, Press Button, Choke, Use,
   Plant Bomb, Answer Phone, Open, Close; 123 / 124 Locked, Currently Locked) and 161 Pick Up Parachute
   [verified: disassembly, the ids pushed before its text lookups]. The Action binding does them all
   (the manual: "Specific action is context-sensitive"): with a controller its button follows,
   "Open (X)"; Locked has none. */
#define PROMPT_FN_LO    0x608128u
#define PROMPT_FN_HI    0x608974u
static int context_prompt(uint32_t obj, uint32_t id, uint32_t caller)
{
    uint32_t hud = R32(HUD);
    return hud && obj == hud + 0xd64 && caller > PROMPT_FN_LO && caller < PROMPT_FN_HI &&
           ((id >= 110 && id <= 122) || id == 161);
}

/* ------------------------------------------------------------------ hook: the text lookup */
static int log_text = -1;

static void text_log(CPU *c, uint32_t obj, uint32_t id, uint32_t caller, uint32_t text)
{
    static struct { uint32_t obj, id; } seen[4096];
    static int nseen;
    for (int i = 0; i < nseen; i++)
        if (seen[i].obj == obj && seen[i].id == id) {
            if (log_text > 1)
                LOG(LOG_ALWAYS, "text: frame %lu obj %08x id %u from %08x again", frame_count, obj, id, caller);
            return;
        }
    if (nseen < 4096)
        seen[nseen++] = (typeof(seen[0])){obj, id};
    char a[100];
    size_t k = 0;
    for (; text && k + 1 < sizeof a && R16(text + 2 * (uint32_t)k); k++) {
        uint16_t ch = R16(text + 2 * (uint32_t)k);
        a[k] = ch == '\n' ? '|' : ch < 128 ? (char)ch : '?';
    }
    a[k] = 0;
    uint32_t hud = R32(HUD);
    LOG(LOG_ALWAYS, "text: frame %lu obj %08x (hud+%x) id %u from %08x count %u: \"%s\"", frame_count, obj,
        hud ? obj - hud : 0, id, caller, R32(obj + 4), a);
    (void)c;
}

/* THE TEXTS WE REWORD are kept live: each is a buffer of ours (made the first time the text is fetched
   with the controller in use, with room for the additions) holding a copy of the game's words; the game
   keeps the address it is given (the popup copies the words once, the pause menu's Tips list keeps the
   address of each tip it collected (0x61f957), the context prompt asks every frame), and when the
   device, the bindings or the kind of level change, prompts_frame writes each buffer again: the game's
   words for the keyboard, the buttons added for the controller. Until a controller is first used there
   are none, and the lookup is the game's own. */
typedef struct { uint32_t hash, g, cap; uint16_t *words; uint16_t n; uint8_t prompt; } Live;
#define LIVE_MAX 1024
#define LIVE_ROOM 320                   /* characters of additions a text has room for */
static Live live[LIVE_MAX];
static int nlive;

static void live_fill(const Live *l, int ctl)
{
    static uint16_t out[TIP_MAX + LIVE_ROOM];
    size_t m = 0, cap = l->cap + 1 < sizeof out / sizeof *out ? l->cap + 1 : sizeof out / sizeof *out;
    if (ctl && !l->prompt)
        m = tip_rewrite(l->words, l->n, out, cap);
    else if (ctl) {
        const char *b = code_short(action_button(10));
        char ins[40];
        snprintf(ins, sizeof ins, " (%s)", b ? b : "");
        if (b && l->n + strlen(ins) < cap) {
            memcpy(out, l->words, 2u * l->n);
            for (m = l->n; ins[m - l->n]; m++)
                out[m] = (uint8_t)ins[m - l->n];
        }
    }
    const uint16_t *w = m ? out : l->words;
    size_t k = m ? m : l->n;
    for (size_t i = 0; i < k; i++)
        W16(l->g + 2 * (uint32_t)i, w[i]);
    W16(l->g + 2 * (uint32_t)k, 0);
}

/* the live buffer for a text the game just fetched (made with the controller in use), or 0 */
static uint32_t live_text(uint32_t text, int prompt, int ctl)
{
    static uint16_t src[TIP_MAX];
    size_t n = 0;
    while (n < TIP_MAX - 1 && R16(text + 2 * (uint32_t)n))
        src[n] = R16(text + 2 * (uint32_t)n), n++;
    uint32_t h = fnv(src, n, 2166136261u);
    for (int i = 0; i < nlive; i++)     /* (by the words: a level load reads the tables to new addresses) */
        if (live[i].hash == h && live[i].n == n && live[i].prompt == prompt) {
            live_fill(&live[i], ctl);
            return live[i].g;
        }
    if (!ctl || nlive == LIVE_MAX)
        return 0;
    Live *l = &live[nlive];
    l->words = malloc(2 * n + 2);
    if (!l->words)
        return 0;
    memcpy(l->words, src, 2 * n);
    l->hash = h;
    l->n = (uint16_t)n;
    l->prompt = (uint8_t)prompt;
    l->cap = (uint32_t)n + LIVE_ROOM;
    l->g = galloc(2 * l->cap + 2);
    nlive++;
    live_fill(l, ctl);
    return l->g;
}

/* each frame (game.c): the live texts written again when what they say changed */
void prompts_frame(void)
{
    if (!nlive)
        return;
    int ctl = prompts_controller();
    uint32_t sig = 2166136261u;
    sig = (sig ^ (uint32_t)ctl) * 16777619u;
    sig = (sig ^ (uint32_t)levels_point_type()) * 16777619u;
    for (uint32_t a = BINDINGS; a < BINDINGS + 8 * 0x27; a += 4)
        sig = (sig ^ R32(a)) * 16777619u;
    static uint32_t last;
    if (sig == last)
        return;
    last = sig;
    for (int i = 0; i < nlive; i++)
        live_fill(&live[i], ctl);
}

/* 0x65526b(text object, id, out), thiscall, ret 8 */
static int hook_text_get_fn(CPU *c)
{
    if (log_text < 0)
        log_text = getenv("ETM_LOG_TEXT") ? atoi(getenv("ETM_LOG_TEXT")) : 0;
    int ctl = prompts_controller();
    if (!ctl && !log_text && !nlive)
        return 0;                       /* (the game's own until a controller is first used) */
    uint32_t obj = c->ecx, id = ARG(0), caller = R32(c->esp);
    F_0065526b_orig(c);
    uint32_t text = c->eax;
    if (log_text)
        text_log(c, obj, id, caller, text);
    if (!text)
        return 1;
    int tip = tip_object(obj), prompt = !tip && context_prompt(obj, id, caller);
    if (tip || prompt) {
        uint32_t g = live_text(text, prompt, ctl);
        if (g)
            c->eax = g;
    }
    return 1;
}

void prompts_install(void)
{
    hook_key_name = hook_key_name_fn;
    hook_text_get = hook_text_get_fn;
}
