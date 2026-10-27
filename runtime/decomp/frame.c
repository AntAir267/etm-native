/* THE FRAME: one in-game frame (0x646e8b, called by the frame function 0x6465c8 while the game
   screen [0x97ed80] is 2 or 3) and its update count (0x6469f1). DESIGN.md 9b.
   A frame first draws, for each view, the state the last updates left (the render pass); then
   runs the frame's updates at the fixed 60 a second, the last one preparing the next frame's
   views (field of view, frustum, projection, visibility); then the world's per-frame step and the
   frame's tail. The interpolation hooks in here: fields copied after each update, blended before
   the render pass, put back after it.
   Transcribed for identical behaviour: the original's frame layout (0x7c bytes of locals, esi and
   edi saved below them; the update count's out-argument at ebp-0x20), its ebp/esi/edi as callees
   see them (rep movsd leaves esi/edi past the copy), byte arguments pushed in a whole register
   whose upper bytes are the previous callee's. */
#include <string.h>
#include <strings.h>
#include "decomp.h"
#include "../interp.h"
#include "../options.h"

/* game state */
#define STATE_FLAGS      0x97ecccu  /* bits 1, 4, 8, 0x20, 0x40, 0x80 tested here (meanings unknown) */
#define GAME_SCREEN      0x97ed80u  /* 1 front end, 2-3 in game (3: playing) [verified: 0x6465c8] */
#define GAME             0x97f800u  /* the game's state object (+0xf8 a mode 1..7, +0x1b4 / +0x1b8
                                       switches for the extra drawing and the sound service,
                                       +0x1d4 / +0x1d8 / +0x1cc / +0x26c / +0x274 / +0x1e0) */
#define FE_SCREEN        0x97cef0u  /* the front end's screen object, or 0 */
#define FE_BLOCKED       0x97cf00u  /* (byte) */
#define MODE_TABLE       0x97ed88u  /* function table: +0xfc before the world is drawn, +0xcc (view)
                                       after the objects; +0x10 the aspect divisor, +0x20 an argument
                                       of the field of view and the projection */
#define STATE_COUNTER    0x97ec4cu  /* updates since the level started ("statecounter %d") */
#define PAD1             0xe7c5d8u  /* the two players' input objects (thiscall 0x5f6690 tests a */
#define PAD2             0xe7c5dcu  /* control: (1) / (0)) [guess: from 0x6465c8's use] */

/* timing [verified: DESIGN.md 9b] */
#define TARGET_RATE      0x97ec60u  /* 60 or 30 (50 / 25 in the PAL mode) */
#define DESIGN_RATE      0x97ec58u  /* 60 (50) */
#define RATE_SCALE       0x97ec54u  /* 1.0 (1.2) */
#define UPDATES_PER_FRAME 0x97ec5cu /* float: 60 / the target rate plus catch-up */
#define CARRY            0x97ec64u  /* float: the fraction of an update carried to the next frame */
#define MEASURED_RATE    0x97ec6cu  /* 1 / the frame end's quantized step */
#define UPDATE_TIME      0x97f570u  /* float: one update's time [guess: 1/60] */
#define UPDATE_INDEX     0x97ec68u  /* 1..n while the frame's updates run */
#define UPDATES_THIS_FRAME 0x97ff4cu
#define FRAME_COUNTER    0x97ff54u
#define UPDATE_ARG       0x97ff50u  /* (byte) handed to the update count and 0x646af7, cleared each frame */

/* views: a list of records [0x97f230] (next at +0x4f0), each copied to VIEW to be drawn or updated
   and copied back */
#define VIEW_LIST        0x97f230u
#define VIEW             0x97f234u
#define VIEW_DWORDS      0x173u     /* 0x5cc bytes */
#define VIEW_NEXT        0x4f0u
#define VIEW_LEVEL       0x4ecu     /* the view's level: VIEW + 0x4ec is 0x97f720 */
#define LEVEL_PTR        0x97f720u
#define LEVEL_UPDATES    4u         /* updates run in the level */
#define LEVEL_TIME       8u         /* float: the level's time */
#define LEVEL_WORLD      0x34u      /* the world drawn (0x747b66) and stepped */
#define LEVEL_WORLD2     0x38u      /* a second world, or 0 (or the same) */

/* functions */
#define CRT_FTOL         0x8a9ef4u  /* __ftol: st0 truncated, popped */
#define LOG_WARNING      0x6303eeu
#define LOG_DEBUG        0x4acc70u  /* printf-like */
#define LOG_STATE        0x7c54ceu  /* printf-like */
#define FE_ACTIVE        0x612463u  /* -> al */
#define PAD_HELD         0x5f6690u  /* (this = input object; which) -> al */
#define PROFILE_MARK     0x653e66u  /* (colour): a profiler's section marker [guess: the arguments are
                                       colours, one per section] */
#define UPDATE_COUNT     0x6469f1u
#define WORLD_DRAW       0x747b66u  /* (world) */
#define OBJECTS_DRAW     0x6f309cu  /* (0) objects and characters */
#define LEVEL_DRAW       0x646c33u  /* (level): renderpass.c */
#define PLACED_DRAW      0x646cecu  /* renderpass.c */
#define ACTOR_EXTRAS_DRAW 0x646c9eu /* renderpass.c */
#define VIEW_FOV         0x62b547u  /* (fov, view, mode arg) */
#define VIEW_FRUSTUM     0x62bf7au  /* (view) */
#define PROJECTION       0x62a236u  /* (matrix, w, h, aspect, near?, mode arg): game.c hooks it */
#define VIEW_PROJ        0x628c89u  /* (out, projection, view matrix) */
#define VISIBLE          0x649f80u  /* (view, node, a, b, 0) -> node [guess: visibility] */
#define WORLD_STEP_A     0x745e07u  /* (world), on the updates that run 0x646367 */
#define WORLD_STEP_B     0x745826u  /* (world) */
#define WORLD_POST       0x7475c8u  /* (world), once a frame after the updates */
#define SND_EEF0         0x97eef0u  /* function pointers of the sound service [guess: called round */
#define SND_EEF4         0x97eef4u  /* 0x75c4f9 and given the frame's update count]; EEF4 when */
#define SND_EED8         0x97eed8u  /* sound is on (STATE_FLAGS bit 1 and GAME+0x1d8 clear) */
#define SND_SERVICE      0x75c4f9u

static uint32_t f2u(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
/* a byte pushed as `mov al, b; push eax`: the register's upper bytes go with it */
static uint32_t lo_byte(uint32_t reg, int b) { return (reg & 0xffffff00u) | (uint8_t)b; }
/* rep movsd: the copy, and esi/edi left past it, ecx 0 */
static void copy_dwords(CPU *c, uint32_t dst, uint32_t src, uint32_t n)
{
    memmove(gptr(dst), gptr(src), 4u * n);
    c->esi = src + 4u * n;
    c->edi = dst + 4u * n;
    c->ecx = 0;
}

/* FRAMES BETWEEN UPDATES (option frame_rate = uncapped; DESIGN.md 9b). In game, a frame no
   longer waits for its 1/60 s: the updates it runs follow the clock (0 or more, the fraction
   carried), and the render pass draws what moves blended between the last two updates by the
   previous frame's carry (the render pass comes before the frame's updates, so it shows the
   updates the previous frame ran, as of the previous frame's time: a constant one update and one
   frame behind, as the original is one frame behind). Frames run at the screen's rate (vsync) or
   frame_limit's. The game's frame-counting code sees whole updates: the measured rate it divides
   60 by is 60 / the frame's updates (0x71c1d5, the first update's, gets the frame's count).
   Front-end frames and in-game frames that stop before the updates stay one a 1/60 s. */
static int uncapped = -1;           /* the option, read on first use and on changes */
static int frame_free;              /* this frame's updates follow the clock; no frame-end wait */
static int frame_free_updates;
static int was_free;                /* the previous frame was */
static double carry;                /* updates due, carried (0..1) */
static double free_last_ms;
double frame_blend;                 /* where the render pass draws between the last two updates */

void frame_rate_reapply(void) { uncapped = -1; }
int frame_rate_uncapped(void)
{
    if (uncapped < 0)
        uncapped = !strcasecmp(option_str("frame_rate", "60"), "uncapped");
    return uncapped;
}
/* main.c, at each frame's end: the frame's length in the game's own frames (a free frame's
   updates; any other frame is one), for scripted input and test checkpoints */
int frame_ticks_end(void)
{
    int t = frame_free ? frame_free_updates : 1;
    was_free = frame_free;
    frame_free = 0;
    return t;
}

static uint32_t free_count(CPU *c, uint32_t out)
{
    double rate = RF32(DESIGN_RATE), due;
    double now = deterministic ? 0 : host_ms();
    if (!was_free)
        due = 1;                                    /* (the first: one update, as the original) */
    else
        due = (deterministic ? test_frame_ms : now - free_last_ms) * rate / 1000.0;
    free_last_ms = now;
    if (due < 0) due = 0;
    frame_blend = carry;
    carry += due;
    uint32_t n = (uint32_t)carry;
    if (n > 4) {                                    /* (too slow: the time beyond is dropped) */
        n = 4;
        carry = 4;
    }
    carry -= n;
    W32(out, n);
    WF32(UPDATES_PER_FRAME, (float)n);
    W32(CARRY, 0);
    WF32(MEASURED_RATE, n ? (float)(rate / n) : 1e9f);
    frame_free = 1;
    frame_free_updates = (int)n;
    return n;
}

/* the frame timer (0x677a90 reads it, 0x7c1b30 advances it by a step in cut scenes): in a free
   frame the frame end (0x7c0915) reads one update's step, already passed (so it picks that step
   and does not wait), and restarts the timer where it would advance it */
static int timer_elapsed(CPU *c)
{
    uint32_t ret = R32(c->esp);
    if (!frame_free || (ret != 0x7c0ac0u && ret != 0x7c0b3eu))
        return 0;
    return decomp_ret_st0(c, c->esp, (f80)RF32(0x8fbde4), 0);
}
static int timer_advance(CPU *c)
{
    uint32_t e = c->esp;
    if (!frame_free || R32(e) != 0x7c0b8fu)
        return 0;
    game_callm(c, 0x677ae0u, c->ecx, 0);
    return decomp_ret(c, e, c->eax, 1);
}
void frame_install(void)
{
    hook_timer_elapsed = timer_elapsed;
    hook_timer_advance = timer_advance;
}

/* 0x6469f1 (unused, out): the number of updates this frame: 60 / the target rate, one more for
   each 60 a second the measured rate falls short (up to 5 more), plus the carried fraction
   (snapped to 0 below 0.01, to 1 above 0.99). *out: the updates that run 0x646367 (1, 2 below
   31 a second, one more for each catch-up update). */
static int dc_6469f1(CPU *c)
{
    uint32_t e = c->esp, fp = e - 4, out = ARG(1), ebp = c->ebp;
    if (frame_rate_uncapped())
        return decomp_ret(c, e, free_count(c, out), 0);
    W32(fp, ebp);
    c->ebp = fp;
    c->esp = fp - 8;
    WF32(UPDATES_PER_FRAME, (float)((f80)RF32(0x8fa0a4) / RF32(TARGET_RATE)));       /* 60 / rate */
    W32(out, 1);
    if (RF32(TARGET_RATE) < RF32(0x8fbdbc))                                         /* 31 */
        W32(out, 2);
    for (int32_t i = 0; W32(fp - 8, (uint32_t)i), i < 5; i++) {
        f80 x = ((f80)RF32(DESIGN_RATE) - RF32(0x8fa05c)) / (f80)(int32_t)R32(out);   /* (60 - 1) / out */
        if (!(x > (f80)RF32(MEASURED_RATE)))
            break;
        WF32(UPDATES_PER_FRAME, (float)((f80)RF32(UPDATES_PER_FRAME) + RF32(RATE_SCALE)));
        W32(out, R32(out) + 1);
    }
    f80 carry = (f80)RF32(CARRY) + RF32(UPDATES_PER_FRAME);
    WF32(CARRY, (float)carry);
    FPUSH(carry);
    uint32_t n = game_call(c, CRT_FTOL, 0);
    W32(fp - 4, n);
    WF32(CARRY, (float)((f80)RF32(CARRY) - (f80)(int32_t)n));
    if ((f80)RF32(CARRY) < (f80)RF64(0x8fbd90))                                     /* 0.01 */
        W32(CARRY, 0);
    if ((f80)RF32(CARRY) > (f80)RF64(0x8fbdb0))                                     /* 0.99 */
        W32(CARRY, 0x3f800000u);
    c->ebp = ebp;
    return decomp_ret(c, e, n, 0);
}
DECOMP(6469f1, dc_6469f1);

/* 0x646e8b: the in-game frame (see the file's header). */
static int dc_646e8b(CPU *c)
{
    uint32_t e = c->esp, fp = e - 4, ebp = c->ebp, esi = c->esi, edi = c->edi;
    uint32_t n2 = fp - 0x20;    /* the update count's out-argument: updates that run 0x646367 */
    W32(fp, ebp);
    c->ebp = fp;
    W32(fp - 0x80, esi);
    W32(fp - 0x84, edi);
    c->esp = fp - 0x84;

    W32(fp - 0x1c, 0x3fc147aeu);                /* the floating-point check: 1.51 truncates to 1 */
    FPUSH((f80)RF32(fp - 0x1c));
    if (game_call(c, CRT_FTOL, 0) != 1)
        game_call(c, LOG_WARNING, 1, 0x932750u);
    int sound_on = 1;
    if ((R32(STATE_FLAGS) & 1) || R32(R32(GAME) + 0x1d8))
        sound_on = 0;
    W32(R32(LEVEL_PTR) + 0x1e8, 0);

    /* a front-end screen over the game */
    if ((game_call(c, FE_ACTIVE, 0) & 0xff) && !R8(FE_BLOCKED)) {
        if (!R32(R32(FE_SCREEN) + 0xe0)) {
            game_call(c, 0x5f5dd2u, 0);
            game_call(c, 0x601c3au, 0);
            W8(R32(FE_SCREEN) + 0xdc, 0);
            game_call(c, R32(0x97f9ccu), 0);
            game_call(c, R32(0x97f9b4u), 0);
        } else {
            if (R32(R32(GAME) + 0x1b8)) {
                game_call(c, R32(SND_EEF0), 0);
                game_call(c, R32(SND_EED8), 2, 1, 0x3f800000u);
                game_call(c, SND_SERVICE, 0);
            }
            game_call(c, 0x612490u, 0);
            if (R32(R32(FE_SCREEN) + 0xe4) == 0x2c && !(game_call(c, FE_ACTIVE, 0) & 0xff))
                game_call(c, 0x613672u, 0);
            if (R32(R32(FE_SCREEN) + 0xe4) == 0x2d && game_call(c, 0x6139feu, 0))
                game_call(c, 0x613f2cu, 0);
            goto out;
        }
    } else if (R32(R32(GAME) + 0x1b8) && sound_on) {
        game_call(c, R32(SND_EEF4), 0);
    }
    if (game_call(c, 0x646905u, 0))
        goto out;

    /* the level's start */
    uint32_t lvl0 = R32(LEVEL_PTR);
    if ((int32_t)R32(lvl0 + LEVEL_UPDATES) <= 0) {
        W32(lvl0, R32(lvl0) & ~2u);
        uint32_t s = 0;
        if (R32(lvl0 + 0x20)) {
            uint32_t r = game_call(c, 0x74864bu, 4, lvl0, R32(lvl0 + 0x20), R32(lvl0 + 0x24), 1);
            if (r && R16(r + 0x20))
                s = R16(r + 0x20);
        }
        if (R32(0x97ecf0u)) {
            if (R32(STATE_FLAGS) & 0x20) {
                game_call(c, 0x646b9du, 1, s);
                W32(0x97ecf0u, 0);
                if (R32(STATE_FLAGS) & 0x80) {
                    game_call(c, 0x64606bu, 0);
                    game_call(c, 0x677fcau, 1, 0);
                    game_call(c, 0x65bfdcu, 1, 1);
                    game_call(c, 0x658a62u, 1, 0x15);
                    W32(GAME_SCREEN, 1);
                    goto out;
                }
            } else {
                game_call(c, 0x646bebu, 1, R32(0x97ecf0u));
                goto out;
            }
        }
        if (s) {
            if (R32(STATE_FLAGS) & 0x20) {
                game_call(c, 0x646b9du, 1, 0);
            } else {
                game_call(c, 0x646bebu, 1, s);
                goto out;
            }
        }
    }
    if (!R32(STATE_COUNTER) && !(R32(STATE_FLAGS) & 4)) {
        if (!(game_call(c, 0x64615du, 0) & 0xff))
            goto out;
        if (R32(R32(R32(VIEW_LIST) + VIEW_LEVEL) + LEVEL_UPDATES) && R32(0x97ff00u))
            game_call(c, R32(0x97ff00u), 1, 0);
    }
    game_call(c, 0x653e40u, 0);

    /* the updates due */
    uint32_t views = R32(VIEW_LIST);
    uint32_t n = game_call(c, UPDATE_COUNT, 2, lo_byte(c->eax, (int)R8(UPDATE_ARG)), n2);
    W32(FRAME_COUNTER, R32(FRAME_COUNTER) + 1);
    if (!(R32(FRAME_COUNTER) & 0x1f)) {
        double fps = RF32(MEASURED_RATE);
        uint64_t b;
        memcpy(&b, &fps, 8);
        game_call(c, LOG_DEBUG, 5, 0x9327acu, (uint32_t)b, (uint32_t)(b >> 32), n, R32(n2));
    }
    if (R32(FE_SCREEN))
        W32(R32(FE_SCREEN) + 0x1658, R32(R32(FE_SCREEN) + 0x1658) + n);
    if (R32(R32(GAME) + 0x1e0)) {
        n = 1;
        W32(n2, 1);
    }
    if (R32(GAME_SCREEN) == 3 || R32(R32(GAME) + 0xf8) == 5 || R32(R32(GAME) + 0xf8) == 4)
        W32(n2, n);
    W32(UPDATES_THIS_FRAME, n);
    W8(UPDATE_ARG, 0);

    /* the render pass: each view as the last updates left it */
    if (frame_free)
        interp_apply(frame_blend);
    uint32_t level = 0;
    for (uint32_t v = views; v; v = R32(v + VIEW_NEXT)) {
        copy_dwords(c, VIEW, v, VIEW_DWORDS);
        level = R32(VIEW + VIEW_LEVEL);
        c->ecx = VIEW;
        c->edx = level;
        game_call(c, PROFILE_MARK, 1, 0xffff00);
        if (R32(STATE_COUNTER) && R32(level + LEVEL_WORLD)) {
            copy_dwords(c, VIEW + 0x94, VIEW + 0x54, 16);          /* the projection drawn with */
            copy_dwords(c, 0x97f4b4u, 0x97f474u, 16);              /* view x projection */
            if (R32(MODE_TABLE + 0xfc)) {
                c->edx = MODE_TABLE;
                game_call(c, R32(MODE_TABLE + 0xfc), 0);
            }
            game_call(c, WORLD_DRAW, 1, R32(level + LEVEL_WORLD));
            game_call(c, PROFILE_MARK, 1, 0xff00ff);
            game_call(c, OBJECTS_DRAW, 1, 0);
            c->eax = MODE_TABLE;
            game_call(c, R32(MODE_TABLE + 0xcc), 1, VIEW);
            game_call(c, LEVEL_DRAW, 1, level);
            if (R32(R32(GAME) + 0x1b4)) {
                game_call(c, PLACED_DRAW, 0);
                game_call(c, ACTOR_EXTRAS_DRAW, 0);
                game_call(c, PROFILE_MARK, 1, 0xffff);
                float t = (float)((f80)(int32_t)R32(n2) * RF32(UPDATE_TIME));
                W32(fp - 0x3c, f2u(t));
                game_call(c, R32(0x97ef30u), 3, (uint32_t)sound_on, 1, f2u(t));
            }
        }
        copy_dwords(c, v, VIEW, VIEW_DWORDS);
    }
    interp_restore();

    /* a control switching what the player controls [guess: 0x601171 (1) / (2)] */
    if (!(R32(STATE_FLAGS) & 0xe) && !R32(R32(LEVEL_PTR) + 0x170) &&
        (!R32(0x992becu) || (R32(VIEW + 0x238) & 2)) && R32(R32(LEVEL_PTR) + 0x3c) &&
        (R32(R32(GAME) + 0xf8) == 7 || !(R32(R32(R32(LEVEL_PTR) + 0x3c) + 0xd8) & 0x10)) &&
        R32(R32(R32(LEVEL_PTR) + 0x3c) + 0x178) && !(R32(R32(R32(R32(LEVEL_PTR) + 0x3c) + 0x178)) & 1)) {
        if ((game_callm(c, PAD_HELD, R32(PAD1), 1, 1) & 0xff) ||
            (R32(R32(GAME) + 0xf8) == 7 && (game_callm(c, PAD_HELD, R32(PAD2), 1, 1) & 0xff))) {
            W32(0x97cee0u, (game_callm(c, PAD_HELD, R32(PAD1), 1, 1) & 0xff) ? 0 : 1);
            uint32_t a = R32(R32(LEVEL_PTR) + 0x3c), b = R32(R32(LEVEL_PTR) + 0x40);
            W32(fp - 0x40, a);
            W32(fp - 0x44, b);
            if (R32(R32(GAME) + 0xf8) != 7 ||
                (!(R32(R32(a + 0x188) + 0x2ec) & 0x1000200) &&
                 (!b || !(R32(R32(b + 0x188) + 0x2ec) & 0x1000200))))
                game_call(c, 0x601171u, 1, 1);
        } else if ((game_callm(c, PAD_HELD, R32(PAD1), 1, 0) & 0xff) &&
                   R32(R32(LEVEL_PTR) + 0x20) != 0xf && R32(R32(LEVEL_PTR) + 0x20) != 2 &&
                   R32(R32(LEVEL_PTR) + 0x20) != 5 && R32(R32(LEVEL_PTR) + 0x20) != 8 &&
                   R32(R32(LEVEL_PTR) + 0x20) != 0xa && R32(R32(GAME) + 0xf8) != 7) {
            game_call(c, 0x601171u, 1, 2);
        }
    }

    /* the updates */
    int sound_done = 0, views_done = 0;        /* this frame's sound service / 0x652b4f ran */
    if (R32(STATE_FLAGS) & 8) {
        n = 1;
        W32(n2, 1);
    }
restart:
    for (W32(UPDATE_INDEX, 1); (int32_t)R32(UPDATE_INDEX) <= (int32_t)n; W32(UPDATE_INDEX, R32(UPDATE_INDEX) + 1)) {
        if (!(R32(STATE_FLAGS) & 1) && !R32(R32(LEVEL_PTR) + 0x170))
            W32(R32(LEVEL_PTR) + 0x24c, R32(R32(LEVEL_PTR) + 0x24c) + 1);
        int extra = (int32_t)R32(UPDATE_INDEX) > (int32_t)R32(n2);   /* beyond *out: no 0x646367 */
        if (!extra)
            game_call(c, 0x646367u, 2, R32(0x97f500u), 0);
        copy_dwords(c, VIEW, VIEW, VIEW_DWORDS);
        for (uint32_t v = R32(VIEW_LIST); v; v = R32(v + VIEW_NEXT)) {
            copy_dwords(c, VIEW, v, VIEW_DWORDS);
            level = R32(VIEW + VIEW_LEVEL);
            if (R32(UPDATE_INDEX) == 1) {
                if ((R32(R32(GAME) + 0xf8) == 1 || R32(R32(GAME) + 0xf8) == 2) && R32(level + LEVEL_WORLD)) {
                    game_call(c, 0x7bf160u, 4, 0, 0x40, 0x40, 0x40);
                    if (!(R32(STATE_FLAGS) & 1))
                        game_call(c, 0x71c1d5u, 0);
                }
                game_call(c, 0x7bf160u, 4, 0, 0x20, 0x40, 0xc0);
                if (R32(level + 0x1cc) && !(R32(STATE_FLAGS) & 1)) {
                    float t = (float)((f80)RF32(UPDATES_PER_FRAME) * RF32(0x8fbde4) * RF32(UPDATE_TIME) /
                                      RF32(RATE_SCALE));                /* the frame's time */
                    game_callm(c, 0x728aa0u, R32(level + 0x1cc), 1, f2u(t));
                    game_call(c, 0x7bf160u, 4, 0, 0, 0, 0);
                }
            }
            game_call(c, PROFILE_MARK, 1, 0xff);
            if (R32(GAME_SCREEN) == 3) {
                W32(R32(GAME) + 0x1d8, 0);
                if (!(R32(STATE_FLAGS) & 1)) {
                    if (!R32(R32(GAME) + 0x26c) || R32(R32(GAME) + 0x274)) {
                        game_call(c, 0x706bcfu, 1, VIEW);
                        W32(R32(GAME) + 0x1d8, 0);
                        W32(R32(GAME) + 0x274, 0);
                    } else {
                        W32(R32(GAME) + 0x1d8, 1);
                    }
                }
                if (!R32(R32(GAME) + 0x1d8)) {
                    W32(R32(GAME) + 0x1d4, R32(R32(GAME) + 0x1d4) - 1);
                    if ((int32_t)R32(R32(GAME) + 0x1d4) <= 0) {
                        game_call(c, 0x6f201fu, 0);
                        game_call(c, 0x6f2128u, 2, 1, lo_byte(c->edx, extra));
                        game_call(c, 0x6f2128u, 2, 0, lo_byte(c->eax, extra));
                        uint32_t L = R32(LEVEL_PTR);
                        if (R32(L + 0x50) && R32(L + 0x170) && R32(R32(L + 0x50) + 0x188)) {
                            f80 a = (f80)RF32(R32(R32(L + 0x170) + 0x40) + 0xc) - RF32(0x8fa05c);
                            if (a > (f80)RF32(R32(R32(R32(L + 0x50) + 0x188) + 0x380) + 0x28)) {
                                game_call(c, 0x6f201fu, 0);
                                game_call(c, 0x6f2128u, 2, 1, lo_byte(c->ecx, extra));
                                game_call(c, 0x6f2128u, 2, 0, lo_byte(c->edx, extra));
                            }
                        }
                    }
                }
            } else {
                int paused = R32(STATE_FLAGS) & 1;
                game_call(c, 0x6f201fu, 0);
                game_call(c, 0x6f2128u, 2, 1, lo_byte(paused ? c->edx : c->ecx, extra));
                game_call(c, PROFILE_MARK, 1, 0xff00);
                if (!extra)
                    game_callm(c, 0x6532eeu, VIEW + 0x234, 0);
                game_call(c, PROFILE_MARK, 1, 0);
                WF32(VIEW + 0x40, (float)((f80)RF32(VIEW + 0x2d0) * RF32(0x8fa038)));   /* x 0.5 */
            }
            if (R32(STATE_FLAGS) & 0x40) {
                W32(STATE_FLAGS, R32(STATE_FLAGS) & ~0x40u);
                goto out;
            }
            if (R32(STATE_FLAGS) & 4)
                goto out;

            if (R32(UPDATE_INDEX) == n) {          /* the last update prepares the next frame's view */
                game_call(c, VIEW_FOV, 3, R32(VIEW + 0x40), VIEW, R32(MODE_TABLE + 0x20));
                game_call(c, VIEW_FRUSTUM, 1, VIEW);
                float aspect = (float)((f80)RF32(VIEW + 0x4c) / RF32(MODE_TABLE + 0x10));
                W32(fp - 0x50, f2u(aspect));
                game_call(c, PROJECTION, 6, VIEW + 0x54, f2u((float)(int32_t)R32(VIEW + 8)),
                          f2u((float)(int32_t)R32(VIEW + 0xc)), f2u(aspect), R32(VIEW + 0x34),
                          R32(MODE_TABLE + 0x20));
                game_call(c, VIEW_PROJ, 3, 0x97f474u, 0x97f288u, 0x97f308u);
                for (int32_t i = 0; i < (int32_t)R32(VIEW + 0x354); i++) {
                    uint32_t p = R32(VIEW + 0x35c + 4u * (uint32_t)i);
                    W32(p, R32(p) & 0xfffffffau);
                }
                if (R32(level + 0x3c) && (int32_t)R32(R32(level + LEVEL_WORLD) + 0x4c) >= 1) {
                    if (R32(VIEW + 0x338)) {
                        uint32_t vis = VIEW + 0x234, node = R32(VIEW + 0x338), r;
                        if (!(R32(VIEW + 0x238) & 1))
                            r = game_call(c, VISIBLE, 5, VIEW, node, vis + 0xa0, vis + 0xb8, 0);
                        else
                            r = game_call(c, VISIBLE, 5, VIEW, node, vis + 0xb8, vis + 0xa0, 0);
                        W32(VIEW + 0x358, r);
                        W32(VIEW + 0x338, r);
                        W32(v + 0x338, r);
                        uint32_t q = R32(R32(level + 0x3c) + 0x1b4);
                        W32(q, R32(q) | 1);
                    } else {
                        W32(VIEW + 0x358, R32(R32(level + 0x3c) + 0x1b4));
                        W32(VIEW + 0x358, game_call(c, VISIBLE, 5, VIEW, R32(VIEW + 0x358),
                                                    R32(level + 0x3c) + 0xfc, VIEW + 0x2d4, 0));
                    }
                    game_call(c, 0x7458d6u, 0);
                    game_call(c, 0x64b2b8u, 3, VIEW, R32(R32(level + LEVEL_WORLD) + 0xa4),
                              R32(R32(level + LEVEL_WORLD) + 0x4c));
                } else if (R32(level + LEVEL_WORLD)) {
                    uint32_t p = R32(R32(level + LEVEL_WORLD) + 0xa4);
                    W32(VIEW + 0x354, R32(R32(level + LEVEL_WORLD) + 0x4c));
                    int32_t cnt = (int32_t)R32(R32(level + LEVEL_WORLD) + 0x4c);
                    if (cnt > 100)
                        cnt = 100;
                    for (int32_t i = 0; i < cnt; i++, p += 0xd0)
                        W32(VIEW + 0x35c + 4u * (uint32_t)i, p);
                }
            }
            if (R32(GAME_SCREEN) != 3) {
                game_call(c, PROFILE_MARK, 1, 0xff);
                int paused = R32(STATE_FLAGS) & 1;
                game_call(c, 0x6f2128u, 2, 0, lo_byte(paused ? c->ecx : c->eax, extra));
            }

            /* the world's step and the sound */
            if (R32(level + LEVEL_WORLD)) {
                game_call(c, PROFILE_MARK, 1, 0xff0000);
                if (!(R32(STATE_FLAGS) & 1)) {
                    if (!extra)
                        game_call(c, WORLD_STEP_A, 1, R32(level + LEVEL_WORLD));
                    game_call(c, WORLD_STEP_B, 1, R32(level + LEVEL_WORLD));
                }
                game_call(c, PROFILE_MARK, 1, 0xff00);
                if (R32(level + LEVEL_WORLD2) && R32(level + LEVEL_WORLD2) != R32(level + LEVEL_WORLD))
                    game_call(c, 0x746de6u, 1, R32(level + LEVEL_WORLD2));
                if (R32(R32(GAME) + 0x1b4)) {
                    game_call(c, PROFILE_MARK, 1, 0x3f3f);
                    if (R32(level + LEVEL_WORLD2) && R32(level + LEVEL_WORLD2) != R32(level + LEVEL_WORLD))
                        game_call(c, 0x746f7bu, 1, R32(level + LEVEL_WORLD2));
                    if (!(R32(STATE_FLAGS) & 8))
                        game_call(c, 0x746f7bu, 1, R32(level + LEVEL_WORLD));
                }
                game_call(c, PROFILE_MARK, 1, 0x7f7f7f);
                if (R32(UPDATE_INDEX) == n) {
                    views_done = 1;
                    game_call(c, 0x652b4fu, 0);
                }
                if (R32(R32(GAME) + 0x1b8)) {
                    game_call(c, PROFILE_MARK, 1, 0);
                    if ((int32_t)R32(level + LEVEL_UPDATES) > 0) {
                        if (R32(GAME_SCREEN) == 3) {
                            game_call(c, 0x75cad1u, 0);
                            game_call(c, 0x75b3f5u, 1, 0);
                            W32(fp - 0x70, f2u((float)ST(0)));
                            FPOP();
                        } else {
                            if (R32(level + LEVEL_WORLD2) && R32(level + LEVEL_WORLD2) != R32(level + LEVEL_WORLD))
                                game_call(c, 0x7465a5u, 1, R32(level + LEVEL_WORLD2));
                            if (!(R32(STATE_FLAGS) & 8))
                                game_call(c, 0x7465a5u, 1, R32(level + LEVEL_WORLD));
                            if (!(R32(STATE_FLAGS) & 1)) {
                                if (R32(0x93ed3cu) != 0xffffffffu || R32(0x992df0u) == 1) {
                                    game_call(c, 0x75b3f5u, 1, f2u((float)(int32_t)R32(0x93ed3cu)));
                                    FPOP();
                                } else {
                                    game_call(c, 0x75a7e6u, 1, R32(0x992decu));
                                    game_call(c, 0x75ab02u, 1, R32(0x992decu));
                                }
                            }
                        }
                    }
                    if (R32(UPDATE_INDEX) == n) {
                        game_call(c, R32(sound_on ? SND_EEF4 : SND_EEF0), 0);
                        W32(fp - 0x74, f2u((float)((f80)(int32_t)R32(n2) * RF32(UPDATE_TIME))));
                        game_call(c, R32(SND_EED8), 2, 1, f2u((float)(int32_t)n));
                        sound_done = 1;
                    }
                    game_call(c, SND_SERVICE, 0);
                }
                if (R32(level + LEVEL_WORLD2) && R32(level + LEVEL_WORLD2) != R32(level + LEVEL_WORLD))
                    game_call(c, 0x746e33u, 1, R32(level + LEVEL_WORLD2));
            }
            copy_dwords(c, v, VIEW, VIEW_DWORDS);
        }

        /* the update's tail */
        W32(R32(LEVEL_PTR) + LEVEL_WORLD2, 0);
        game_call(c, PROFILE_MARK, 1, 0xff0080);
        W32(STATE_COUNTER, R32(STATE_COUNTER) + 1);
        game_call(c, LOG_STATE, 2, 0x9327c8u, R32(STATE_COUNTER));
        if (R32(GAME_SCREEN) == 3) {
            W32(fp - 0x78, 1);
            W32(R32(GAME) + 0x1cc, 0x3f800000u);
            if ((int32_t)R32(R32(GAME) + 0x1d4) <= 0)
                W32(R32(GAME) + 0x1d4, 1);
        }
        game_call(c, 0x6016e1u, 0);
        if (game_call(c, FE_ACTIVE, 0) & 0xff)
            goto out;
        if (!(R32(STATE_FLAGS) & 1) || !R32(level + LEVEL_UPDATES)) {
            W32(level + LEVEL_UPDATES, R32(level + LEVEL_UPDATES) + 1);
            WF32(level + LEVEL_TIME, (float)((f80)RF32(UPDATE_TIME) / RF32(RATE_SCALE) + RF32(level + LEVEL_TIME)));
        }
        if (R32(GAME_SCREEN) != 3 && R32(GAME_SCREEN) != 2)
            goto out;
        if (R32(UPDATE_INDEX) == 1 && !(R32(STATE_FLAGS) & 4))
            game_call(c, 0x5d76e9u, 1, R32(LEVEL_PTR));
        if (R32(UPDATE_INDEX) != n) {
            game_call(c, 0x5f5e15u, 1, R32(PAD1));
            game_call(c, 0x5f5e15u, 1, R32(PAD2));
        }
        game_call(c, 0x646af7u, 3, UPDATE_ARG, R32(UPDATE_INDEX), n);
        if (frame_rate_uncapped())
            interp_game_update();           /* what moves, copied (interp_game.c) */
        if (R32(R32(GAME) + 0x1e0))
            W8(UPDATE_ARG, 0);
        if (R32(lvl0) & 2)
            break;
        if ((int32_t)R32(0x97ece4u) >= 0) {
            if (!views_done)
                game_call(c, 0x652b4fu, 0);
            if (!sound_done) {
                game_call(c, R32(sound_on ? SND_EEF4 : SND_EEF0), 0);
                W32(fp - 0x7c, f2u((float)((f80)(int32_t)R32(n2) * RF32(UPDATE_TIME))));
                game_call(c, R32(SND_EED8), 2, 1, f2u((float)(int32_t)n));
                game_call(c, SND_SERVICE, 0);
            }
            break;
        }
    }
    if (R32(STATE_FLAGS) & 8)
        goto restart;

    /* the frame's tail: the world's per-frame step for each view */
    if (R32(GAME_SCREEN) == 3)
        game_call(c, 0x70629fu, 1, R32(R32(level + 0x170) + 0x40));
    copy_dwords(c, VIEW, VIEW, VIEW_DWORDS);
    for (uint32_t v = R32(VIEW_LIST); v; v = R32(v + VIEW_NEXT)) {
        copy_dwords(c, VIEW, v, VIEW_DWORDS);
        level = R32(VIEW + VIEW_LEVEL);
        game_call(c, PROFILE_MARK, 1, 0xffff00);
        if (R32(level + LEVEL_WORLD))
            game_call(c, WORLD_POST, 1, R32(level + LEVEL_WORLD));
        copy_dwords(c, v, VIEW, VIEW_DWORDS);
    }
    game_call(c, PROFILE_MARK, 1, 0xffffff);
    if (R32(GAME_SCREEN) == 3) {
        int call = RF32(R32(GAME) + 0x1cc) == 0.0f;
        if (!call && R32(level + 0x170) && R32(R32(level + 0x170) + 0x40))
            call = RF32(R32(R32(level + 0x170) + 0x40) + 0xc) <= 3.0f;
        if (call)
            game_call(c, 0x602458u, 0);
    }
    game_call(c, 0x6030e0u, 0);
    game_call(c, 0x6eb540u, 0);
    if (RF32(TARGET_RATE) < RF32(0x8fbdbc)) {      /* at 30 a second: wait for [0x97fee4]() >= 1.3 */
        for (;;) {
            game_call(c, R32(0x97fee4u), 0);
            f80 t = ST(0);
            FPOP();
            if (!(t < (f80)RF32(0x8fbde0)))
                break;
        }
    }
    game_call(c, 0x6e8469u, 0);
out:
    c->esi = esi;
    c->edi = edi;
    c->ebp = ebp;
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(646e8b, dc_646e8b);
