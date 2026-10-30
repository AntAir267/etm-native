/* THE LEVEL LOADER (DESIGN.md decision 12; Ant: "a debugger that loads you into those levels").
   Straight into any level of the game's own level table, the way the game's Load page starts a saved
   game: from the command line (--level NAME, ETM_LEVEL=NAME in --test runs, --list-levels) and from
   the title menu's "Level Select" (the game's own text, Text\TX<language>FE.bin id 2, unused by the
   PC's menus).

   THE GAME'S LEVELS [verified: disassembly of the functions named; the table and the names records
   read at run time (ETM_LEVELS_DEBUG=1 logs both); the New Game route watched with ETM_WATCH on the
   world's level field and ETM_OPEN_BT on the level files' opens]:
   - The world (the game-flow object, [[0x97f230]+0x4ec] = [0x97f720]) keeps the level table, built
     once at start-up by 0x5d1138 (from 0x6ebbfc): [+0x194] levels, [+0x1a0] for each {count, load
     points}, a load point 0x30 bytes: +0 type, +0xc the file ("levels\X.3d2", opened as
     Levels\X.3d2_42), +0x10 "Beat_N_Name\X", +0x18 the animation set, +0x28 a 16-bit id (level << 10,
     load point << 4, the low two bits the character: 0 Niobe, 1 Ghost). The game calls the two
     numbers "level" and "loadpoint" ("Illegal level number %d", "Illegal loadpoint %d for level %s").
     18 levels: 0 multiplayer arenas and developers' test rooms, 1 the Post Office, 2 city driving,
     3 city rooftops, 4 the airport, 5 the aqueduct, 6 the sewers, 7 the chateau, 8 under the city,
     9 the zen garden, 10 the freeway (driving), 11 the freeway on foot, 12 the power plant,
     13 Chinatown, 14 the skyscraper, 15 the Logos (hovercraft), 16 "secret" (Sparks' training
     construct and AI test rooms), 17 the in-engine cut scenes (New Game starts 17.20 / 17.19).
   - Names: 0x986678 + level x 0x170 (filled at start-up; the texts are Text\TXeStc.bin's, the code
     that fills them not traced): the level's UTF-16 name ("THE POST OFFICE"), a count, then {UTF-16
     name, load point, character} for the load points a save can name ("Closing Time", 1 = Ghost;
     room for 30: the cut scenes' 35 overrun it, the last five taken from the id) [guess: what the
     Load page shows]. The id's low bits and the records' character disagree in four cut scenes
     (17.1, 17.3, 17.10, 17.22): the records tell a file's two copies apart (17.10 / 17.28 the
     airport's opening, Ghost's and Niobe's; the ids make both Niobe), so the records win [guess].
   - World [+0x20] level, [+0x24] load point, [+4] the updates since the load point began: the game's
     in-game handler (0x5392ad -> 0x539311) loads the load point when it finds [+4] = 0 (0x5d6c14:
     entry 0x74864b(world, level, loadpoint), file 0x741330) and runs it from then on.
   - Only 0x65f335 (a front-end method) sets a real level: world [+0x20] / [+0x24] from the front
     end's [+0xc80] / [+0xc84], [0x97ff10] = character from [+0xc88], screen [0x97ed80] = 2, ...
     The front end [0x98d08c] (0x7028 bytes) runs page [+0x20] in state [+0x24] (0x65c246's table):
     1 logos, 6/7 language, 2 title menu (0x65e161; list +0x214 built by 0x65dec8), 3 the opening
     film, 4 character select and the jack-in (0x65f40e), 8 options, 10 load (0x65f09d).
     New Game: character select sets [+0xc80..+0xc88] (17, 19 or 20, Niobe 0 / Ghost 1), state 2
     plays the jack-in film, state 3 calls 0x7b768d(1) (mode 1) and 0x65f335. Load: the save's level,
     load point and character into [+0xc80..+0xc88], [0x98c92c] = 0, [+0xc8c] = 1, 0x65d14c (the
     front end's films let go), the jack-in film opened into slot 0xd (Ghost) / 0xc (Niobe) of
     +0x1cc and made the one playing ([+0x6540]), page 4 state 2: when the film ends, 0x65f335.
   - The developers' own level select (0x6784c3, "uidebug": reads Levels\LevelList.txt) is cut: its
     per-frame state prints "Unsupported", and nothing calls it.

   THE LOADER does what the Load page does, with the level, load point and character it is given and
   no save applied (a new game's state otherwise), difficulty Normal ([0x97f804]: 0 Easy, 1 Normal,
   2 Hard; New Game's default 1) unless asked. It acts on the title menu (page 2, idle in state 1
   with its backdrop film, slot 5, up): command-line requests wait for it (the language page is
   answered with Enter on the way), the menu entry is part of it.

   The title menu's Level Select (option level_select, on by default; off gives the original menu):
   the second row of the game's own list, under Load; accepting it fills the same list with the levels (their names
   from the records; the cut scenes, the freeway on foot and the test rooms, which have none, ours),
   then a level's load points ("Closing Time - Ghost", the file's name where the records have none),
   seven rows at a time ("..." where more follow), up / down / accept / back as the game's menus
   (sounds 0x35d, 0x359); accepting a load point starts it. Load points whose file is not on disk
   (developers' test rooms, the freeway on foot, AITutorial2) are left out.

   Command line / environment (tests): --level NAME (or ETM_LEVEL=NAME): NAME a load point's file
   name (B1_DC_01_N_CITY_SECTION_A), case-insensitive, NAME@L to pick level L where several share a
   file, or L.P; character from the table (--character / ETM_LEVEL_CHAR ghost|niobe overrides),
   --difficulty / ETM_LEVEL_DIFFICULTY easy|normal|hard. Once the load point runs, the scripted
   input's mark is set (ETM_KEYS / ETM_PAD / ETM_SHOT "+frames" count from it) and a status line is
   logged every 100 frames (level, screen, cinema bars, film, camera position, view matrix hash). ETM_LEVEL_FRAMES=N
   ends the run N frames after the mark; ETM_LEVEL_SKIP=1 skips a film or in-engine scene at the
   start (Escape). --list-levels prints the table. ETM_LEVELS_DEBUG=1 logs the table and the
   front end's and world's state as it changes (how the above was found). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include "runtime.h"
#include "input.h"
#include "options.h"
#include "decomp/decomp.h"

/* ------------------------------------------------------------------ the game's data */
#define VIEWS           0x97f230u       /* the view list (+0x4ec the world, +0x2d4 the camera position) */
#define W_UPDATES(w)    ((w) + 0x04)    /* updates since the load point began (0: load it) */
#define W_LEVEL(w)      ((w) + 0x20)
#define W_LOADPOINT(w)  ((w) + 0x24)
#define W_NLEVELS(w)    ((w) + 0x194)
#define W_LEVELS(w)     ((w) + 0x1a0)   /* {count, load points} per level */
#define LP_SIZE         0x30u
#define LP_TYPE         0x00            /* 0 on foot, 1 / 2 driving, 4 / 5 hovercraft, 7 multiplayer */
#define LP_FILE         0x0c
#define LP_ID           0x28
#define NAMES           0x986678u       /* per level: UTF-16 name, count, {name, load point, character} x 30 */
#define NAMES_SIZE      0x170u
#define CUTSCENES       17              /* the level of the in-engine cut scenes */

#define FRONT_END       0x98d08cu
#define FE_PAGE(f)      ((f) + 0x20)
#define FE_STATE(f)     ((f) + 0x24)
#define FE_TEXT(f)      ((f) + 0x1c0)
#define FE_MOVIES(f)    ((f) + 0x1cc)   /* film handles by slot */
#define FE_LIST(f)      ((f) + 0x214)   /* the title menu's list */
#define FE_INPUT(f)     R32((f) + 0x994)
#define FE_LEVEL(f)     ((f) + 0xc80)
#define FE_LOADPOINT(f) ((f) + 0xc84)
#define FE_GHOST(f)     ((f) + 0xc88)
#define FE_LOADED(f)    ((f) + 0xc8c)   /* a loaded game: no film after the jack-in */
#define FE_MOVIE(f)     ((f) + 0x6540)  /* the slot playing (5: the title menu's backdrop) */
#define PAGE_TITLE      2
#define PAGE_CHARACTER  4
#define MOVIE_BACKDROP  5

#define DIFFICULTY      0x97f804u
#define SAVE_PROMPT     0x98c92cu       /* 1 after New Game's character select, 0 from Load [guess: a save prompt] */
#define SCREEN          0x97ed80u
#define CINEMA_BARS     0x97f238u

/* the game's functions (their stubs' targets) */
#define FE_RELEASE      0x65d14cu       /* (front end) its films and lists let go */
#define FE_SET_PAGE     0x65c532u       /* (front end, page) */
#define TITLE_BUILD     0x65dec8u       /* (front end) the title menu's list */
#define TITLE_PULSE     0x65bd67u       /* (front end) the highlight's pulse, a step (the page's state 1 first) */
#define MOVIE_OPEN      R32(0x97f9b8)   /* (name, 0, 0) -> handle */
#define JACKIN_GHOST    0x933468u       /* the game's "movies\jackin_ghost.avi" */
#define JACKIN_NIOBE    0x933480u       /* "movies\jackin_niobe.avi" */
#define TEXT_GET        0x65526bu       /* (text object, id, 0) -> UTF-16 */
#define TEXT_LEVEL_SELECT 2u            /* TX<language>FE.bin: "Level Select" (No, Yes, Level Select, Load, ...) */
#define MENU_KEY        0x5f69bdu       /* (input object, action) -> al: 0 up, 1 down, 4 accept, 7 back */
#define JOY_DIR         0x98f048u       /* the joystick's direction as a menu key (0x10 up, 0x12 down) */
#define SOUND_PLAY      R32(0x97eee8)   /* (1, sound, 0, 0, 0, 0, 0) */
#define SOUND_MOVE      0x35du
#define SOUND_BACK      0x359u
#define LIST_CLEAR      0x657401u
#define LIST_ADD        0x6554fcu
#define LIST_SELECT     0x6577afu
#define L_SEL(l)        ((l) + 0x04)
#define L_LOOK(l, k)    ((l) + 0x0c + (k))
#define L_COUNT(l)      ((l) + 0x88)

/* ------------------------------------------------------------------ the table, read from the game */
typedef struct {
    int level, point, type, ghost, listed, exists;
    char file[64];              /* the file's name, as the table has it (without levels\ and .3d2) */
    char title[64];             /* the load point's name where the names records have it */
} Level;
#define MAX_LEVELS 512
static Level lv[MAX_LEVELS];
static int nlv, nlevels;
static char level_name[64][48];

static uint32_t world(void)
{
    uint32_t v = R32(VIEWS);
    return v ? R32(v + 0x4ec) : 0;
}

static void gstr8(uint32_t a, char *out, size_t n)
{
    size_t k = 0;
    for (; a && k + 1 < n && R8(a + (uint32_t)k); k++)
        out[k] = (char)R8(a + (uint32_t)k);
    out[k] = 0;
}

static void wstr(uint32_t a, char *out, size_t n)
{
    size_t k = 0;
    for (; a && k + 1 < n && R16(a + 2 * (uint32_t)k); k++) {
        uint16_t ch = R16(a + 2 * (uint32_t)k);
        out[k] = ch < 128 ? (char)ch : '?';
    }
    out[k] = 0;
}

static int table_ready(void)
{
    uint32_t w = world();
    if (!w || !R32(W_NLEVELS(w)) || !R32(W_LEVELS(w)))
        return 0;
    if (nlv)
        return 1;
    nlevels = (int)R32(W_NLEVELS(w));
    if (nlevels > 64)
        nlevels = 64;
    for (int g = 0; g < nlevels; g++) {
        uint32_t rec = NAMES + NAMES_SIZE * (uint32_t)g;
        wstr(R32(rec), level_name[g], sizeof level_name[g]);
        uint32_t n = R32(R32(W_LEVELS(w)) + 8u * (uint32_t)g), e0 = R32(R32(W_LEVELS(w)) + 8u * (uint32_t)g + 4);
        for (uint32_t i = 0; i < n && nlv < MAX_LEVELS; i++) {
            uint32_t e = e0 + LP_SIZE * i;
            Level *l = &lv[nlv++];
            memset(l, 0, sizeof *l);
            l->level = g;
            l->point = (int)i;
            l->type = (int)R32(e + LP_TYPE);
            l->ghost = (R16(e + LP_ID) & 3) == 1;
            char f[128];
            gstr8(R32(e + LP_FILE), f, sizeof f);
            const char *s = strncasecmp(f, "levels\\", 7) ? f : f + 7;
            snprintf(l->file, sizeof l->file, "%s", s);
            char *dot = strrchr(l->file, '.');
            if (dot && !strcasecmp(dot, ".3d2"))
                *dot = 0;
            char win[256], host[2048];
            snprintf(win, sizeof win, "%s\\Levels\\%s.3d2_42", virtual_game_root(), l->file);
            l->exists = path_to_host(win, host, sizeof host, 0);
            uint32_t nn = R32(rec + 4);
            for (uint32_t k = 0; k < nn && k < 30; k++)
                if (R32(rec + 12 + 12 * k) == i) {
                    wstr(R32(rec + 8 + 12 * k), l->title, sizeof l->title);
                    for (size_t e = strlen(l->title); e && l->title[e - 1] == ' '; e--)
                        l->title[e - 1] = 0;
                    l->listed = 1;
                    l->ghost = R32(rec + 16 + 12 * k) == 1;
                    break;
                }
        }
    }
    LOG(LOG_ALWAYS, "levels: the game's table read: %d levels, %d load points", nlevels, nlv);
    return 1;
}

static const char *kind_of(const Level *l)
{
    if (l->level == CUTSCENES)
        return "cut scene";
    switch (l->type) {
    case 1: return "driving";
    case 2: return "driving (gunner)";
    case 4: return "hovercraft (pilot)";
    case 5: return "hovercraft (gunner)";
    case 7: return "multiplayer arena";
    }
    return l->level == 0 || l->level == 16 ? "test / training" : "on foot";
}

static const char *who(const Level *l) { return l->type == 7 ? "-" : l->ghost ? "Ghost" : "Niobe"; }

/* a level's name for the menu and the list (the names records', or ours where they have none) */
static void level_title(int g, char *out, size_t n)
{
    if (level_name[g][0])
        snprintf(out, n, "%s", level_name[g]);
    else if (g == CUTSCENES)
        snprintf(out, n, "CUT SCENES");
    else if (g == 11)
        snprintf(out, n, "THE FREEWAY ON FOOT");
    else
        snprintf(out, n, "LEVEL %d", g);
}

static void point_title(const Level *l, char *out, size_t n)
{
    char f[64];                         /* (the file's name: the menus' font has no '_' or '\') */
    snprintf(f, sizeof f, "%s", l->file);
    for (char *q = f; *q; q++)
        if (*q == '_' || *q == '\\')
            *q = ' ';
    if (l->type == 7)
        snprintf(out, n, "%s", l->title[0] ? l->title : f);
    else
        snprintf(out, n, "%s - %s", l->title[0] ? l->title : f, who(l));
}

/* NAME, NAME@L or L.P -> the entry, -1 if none */
static int find_level(const char *name)
{
    int g, p;
    char tail;
    if (sscanf(name, "%d.%d%c", &g, &p, &tail) == 2) {
        for (int k = 0; k < nlv; k++)
            if (lv[k].level == g && lv[k].point == p)
                return k;
        return -1;
    }
    char want[96];
    snprintf(want, sizeof want, "%s", name);
    int only = -1;
    char *at = strchr(want, '@');
    if (at) {
        *at = 0;
        only = atoi(at + 1);
    }
    size_t wl = strlen(want);
    if (wl > 7 && !strcasecmp(want + wl - 7, ".3d2_42"))
        want[wl - 7] = 0;
    /* the story's levels first, then the test rooms, the cut scenes last */
    static const int pass_of[3] = {0, 1, 2};
    for (int pass = 0; pass < 3; pass++)
        for (int k = 0; k < nlv; k++) {
            const Level *l = &lv[k];
            int lp = l->level == CUTSCENES ? 2 : l->level == 0 || l->level == 16 ? 1 : 0;
            if (lp != pass_of[pass] || (only >= 0 && l->level != only))
                continue;
            const char *base = strrchr(l->file, '\\');
            if (!strcasecmp(l->file, want) || (base && !strcasecmp(base + 1, want)))
                return k;
        }
    return -1;
}

/* ------------------------------------------------------------------ requests */
enum { REQ_NONE, REQ_PENDING, REQ_STARTED, REQ_PLAYING };
static struct {
    char name[96];
    int ghost;                  /* -1: the table's */
    int difficulty;             /* 0 Easy, 1 Normal, 2 Hard */
    int cli;                    /* from the command line (status lines, the language page answered) */
    int state, k;
    unsigned long mark;
} req = {"", -1, 1, 0, REQ_NONE, -1, 0};
static int list_mode;
static const char *cli_level, *cli_char, *cli_diff;

void levels_cli(const char *level, const char *character, const char *difficulty, int list)
{
    if (level) cli_level = level;
    if (character) cli_char = character;
    if (difficulty) cli_diff = difficulty;
    if (list) {
        list_mode = 1;
        setenv("DXVK_LOG_LEVEL", "none", 0);    /* (the list alone on the output) */
    }
}

static void list_print(void)
{
    printf("Enter the Matrix: the game's level table (level.load point, kind, character, file, name)\n");
    for (int g = 0; g < nlevels; g++) {
        char t[64];
        level_title(g, t, sizeof t);
        printf("%2d %s\n", g, t);
        for (int k = 0; k < nlv; k++) {
            const Level *l = &lv[k];
            if (l->level != g)
                continue;
            char id[16];
            snprintf(id, sizeof id, "%d.%d", l->level, l->point);
            printf("   %-6s %-20s %-6s %-36s %s%s%s%s\n", id, kind_of(l), who(l), l->file,
                   l->title[0] ? "\"" : "", l->title, l->title[0] ? "\"" : "", l->exists ? "" : " (no file)");
        }
    }
    fflush(stdout);
}

/* ------------------------------------------------------------------ helpers (as menu.c's) */
static uint32_t wtext(const char *s)
{
    static struct { char s[64]; uint32_t g; } made[256];
    static int nmade;
    for (int i = 0; i < nmade; i++)
        if (!strcmp(made[i].s, s))
            return made[i].g;
    size_t n = strlen(s);
    uint32_t g = galloc(2 * (uint32_t)n + 2);
    for (size_t i = 0; i <= n; i++)
        W16(g + 2 * (uint32_t)i, (uint16_t)(unsigned char)s[i]);
    if (nmade < 256 && n < sizeof made[0].s) {
        snprintf(made[nmade].s, sizeof made[nmade].s, "%s", s);
        made[nmade++].g = g;
    }
    return g;
}

static void list_add(CPU *c, uint32_t list, uint32_t text)
{
    uint32_t f = dc_frame(c, 8);
    W32(f, 0);
    W32(f + 4, text);
    game_callm(c, LIST_ADD, list, 2, f, 0xffffffffu);
    dc_unframe(c, f, 8);
}

static int menu_key(CPU *c, uint32_t fe, int action)
{
    return (game_callm(c, MENU_KEY, FE_INPUT(fe), 1, (uint32_t)action) & 0xff) != 0;
}

static void sound(CPU *c, uint32_t id)
{
    game_call(c, SOUND_PLAY, 7, 1, id, 0, 0, 0, 0, 0);
}

/* ------------------------------------------------------------------ starting a load point */
static void start(CPU *c, uint32_t fe, int k, int ghost, int difficulty)
{
    const Level *l = &lv[k];
    W32(FE_LEVEL(fe), (uint32_t)l->level);
    W32(FE_LOADPOINT(fe), (uint32_t)l->point);
    W32(FE_GHOST(fe), (uint32_t)ghost);
    W32(SAVE_PROMPT, 0);
    W32(DIFFICULTY, (uint32_t)difficulty);
    W32(FE_LOADED(fe), 1);
    game_callm(c, FE_RELEASE, fe, 0);
    uint32_t slot = ghost ? 0xd : 0xc;
    W32(FE_MOVIES(fe) + 4 * slot, game_call(c, MOVIE_OPEN, 3, ghost ? JACKIN_GHOST : JACKIN_NIOBE, 0, 0));
    W32(FE_MOVIE(fe), slot);
    game_callm(c, FE_SET_PAGE, fe, 1, PAGE_CHARACTER);
    W32(FE_STATE(fe), 2);
    req.k = k;
    req.state = REQ_STARTED;
    LOG(LOG_ALWAYS, "level: starting %d.%d %s (%s, %s, difficulty %d) at frame %lu", l->level, l->point, l->file,
        kind_of(l), ghost ? "Ghost" : "Niobe", difficulty, frame_count);
}

/* ------------------------------------------------------------------ the title menu's Level Select */
#define TITLE_ROWS       6              /* with Level Select */
#define TITLE_ROW_LEVELS 1              /* its row */
#define TITLE_DISPATCH   0x65e421u      /* the title menu's read of the accepted row (0x65e41c's return) */
#define LOGOS_NEXT       0x65ce7fu      /* the logos page's next step (what the end of each calls) */
#define LOGOS_LEGAL      5u             /* its state showing the legal notices: fades in, 5 s, fades out,
                                           no input read, unlike the logos [verified: 0x65ca66 disassembly] */
#define LOGOS_FADE(f)    ((f) + 0x9a0)  /* the fade: down to -100 when that state ends */
#define LS_ROWS     7                   /* rows shown, in the title menu's place under the logo: the */
#define LS_Y        272                 /* menu's own list (y 280, 24 apart) moved up a little and */
#define LS_SPACING  22                  /* closer, so seven fit the 448-line design (TITLE_BUILD puts it back) */
static struct {
    int active, depth, level;           /* depth 0: the levels, 1: a level's load points */
    int sel, top, n;
    int item[MAX_LEVELS];               /* depth 0: level numbers; 1: entries */
} ls;

static void ls_fill(CPU *c, uint32_t fe)
{
    uint32_t list = FE_LIST(fe);
    W32(L_LOOK(list, 0x34), LS_Y);
    W32(L_LOOK(list, 0x68), LS_SPACING);
    if (ls.sel < ls.top)
        ls.top = ls.sel;
    if (ls.sel >= ls.top + LS_ROWS)
        ls.top = ls.sel - LS_ROWS + 1;
    game_callm(c, LIST_CLEAR, list, 0);
    for (int r = ls.top; r < ls.n && r < ls.top + LS_ROWS; r++) {
        char t[96], s[112];
        if (ls.depth == 0)
            level_title(ls.item[r], t, sizeof t);
        else
            point_title(&lv[ls.item[r]], t, sizeof t);
        int more_up = r == ls.top && ls.top > 0, more_down = r == ls.top + LS_ROWS - 1 && r < ls.n - 1;
        snprintf(s, sizeof s, "%s%s%s", more_up ? "... " : "", t, more_down ? " ..." : "");
        list_add(c, list, wtext(s));
    }
    game_callm(c, LIST_SELECT, list, 1, (uint32_t)(ls.sel - ls.top));
}

static void ls_open(CPU *c, uint32_t fe, int depth, int level, int sel)
{
    ls.active = 1;
    ls.depth = depth;
    ls.level = level;
    ls.n = 0;
    if (depth == 0) {
        /* the story first (1-15), then the cut scenes, the secret and the multiplayer / test rooms;
           levels with no load point on disk left out */
        static const int order[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, CUTSCENES, 16, 0};
        for (size_t o = 0; o < sizeof order / sizeof *order; o++) {
            int g = order[o], any = 0;
            if (g >= nlevels)
                continue;
            for (int k = 0; k < nlv; k++)
                any |= lv[k].level == g && lv[k].exists;
            if (any)
                ls.item[ls.n++] = g;
        }
    } else {
        for (int k = 0; k < nlv; k++)
            if (lv[k].level == level && lv[k].exists)
                ls.item[ls.n++] = k;
    }
    ls.sel = sel < ls.n ? sel : 0;
    ls.top = 0;
    ls_fill(c, fe);
}

static void ls_close(CPU *c, uint32_t fe)
{
    ls.active = 0;
    game_callm(c, TITLE_BUILD, fe, 0);  /* (the build hook adds Level Select again) */
    game_callm(c, LIST_SELECT, FE_LIST(fe), 1, TITLE_ROW_LEVELS);
}

static void ls_update(CPU *c, uint32_t fe)
{
    game_callm(c, TITLE_PULSE, fe, 0);  /* (as the page's own state 1 does each frame) */
    int dir = 0;
    if (menu_key(c, fe, 0) || R32(JOY_DIR) == 0x10) dir = -1;
    if (menu_key(c, fe, 1) || R32(JOY_DIR) == 0x12) dir = 1;
    if (dir && ls.n) {
        ls.sel = (ls.sel + dir + ls.n) % ls.n;
        if (getenv("ETM_LEVELS_DEBUG"))
            LOG(LOG_ALWAYS, "levels: menu row %d of %d (frame %lu)", ls.sel, ls.n, frame_count);
        sound(c, SOUND_MOVE);
        ls_fill(c, fe);
        return;
    }
    if (menu_key(c, fe, 7)) {
        sound(c, SOUND_BACK);
        if (ls.depth == 1) {
            int back = 0, from = ls.level;
            ls_open(c, fe, 0, 0, 0);
            for (int r = 0; r < ls.n; r++)
                if (ls.item[r] == from)
                    back = r;
            ls.sel = back;
            ls_fill(c, fe);
        } else
            ls_close(c, fe);
        return;
    }
    if (menu_key(c, fe, 4) && ls.n) {
        sound(c, SOUND_MOVE);
        if (ls.depth == 0)
            ls_open(c, fe, 1, ls.item[ls.sel], 0);
        else {
            int k = ls.item[ls.sel];
            ls.active = 0;
            req.cli = 0;
            start(c, fe, k, lv[k].ghost, 1);
        }
    }
}

/* ------------------------------------------------------------------ hooks */
static int ls_accepted;                 /* Level Select accepted some way only the list sees */

static int hook_title_build_fn(CPU *c)
{
    uint32_t fe = c->ecx;
    F_0065dec8_orig(c);
    uint32_t ret = c->ret, eax = c->eax;
    /* (on in play, off by default under --test: the scripted routes count the original menu's rows) */
    if (option_bool("level_select", !deterministic) && table_ready() && R32(L_COUNT(FE_LIST(fe))) == 5) { /* (5: not the demo mode's one) */
        /* the same texts in the same order (0x65dec8: Load, New Game, Options, Hacking, Quit), with
           Level Select under Load; hook_list_selected_fn gives the game its own row numbers back */
        static const uint32_t rows[TITLE_ROWS] = {3, TEXT_LEVEL_SELECT, 4, 5, 0x24, 0x6c};
        game_callm(c, LIST_CLEAR, FE_LIST(fe), 0);
        for (int r = 0; r < TITLE_ROWS; r++)
            list_add(c, FE_LIST(fe), game_callm(c, TEXT_GET, FE_TEXT(fe), 2, rows[r], 0));
    }
    c->ret = ret;
    c->eax = eax;
    return 1;
}

static int hook_title_update_fn(CPU *c)
{
    uint32_t entry = c->esp, fe = c->ecx;
    if (R32(FE_STATE(fe)) != 1 || R32(FE_MOVIE(fe)) != MOVIE_BACKDROP || !table_ready())
        return 0;
    if (req.state == REQ_PENDING) {
        int k = find_level(req.name);
        if (k < 0 || !lv[k].exists) {
            LOG(LOG_ALWAYS, "level: \"%s\" is %s (--list-levels lists them)", req.name, k < 0 ? "not in the game's table" : "a load point without its file");
            if (deterministic)
                process_exit(3);
            req.state = REQ_NONE;
            return 0;
        }
        start(c, fe, k, req.ghost >= 0 ? req.ghost : lv[k].ghost, req.difficulty);
        return decomp_ret(c, entry, c->eax, 0);
    }
    if (ls.active) {
        ls_update(c, fe);
        return decomp_ret(c, entry, c->eax, 0);
    }
    uint32_t eax = c->eax, edx = c->edx;
    if (R32(L_COUNT(FE_LIST(fe))) == TITLE_ROWS &&
        (ls_accepted || (R32(L_SEL(FE_LIST(fe))) == TITLE_ROW_LEVELS && menu_key(c, fe, 4)))) {
        ls_accepted = 0;
        sound(c, SOUND_MOVE);
        ls_open(c, fe, 0, 0, 0);
        return decomp_ret(c, entry, c->eax, 0);
    }
    c->eax = eax;                       /* (then the page's own, with the registers it was called with) */
    c->ecx = fe;
    c->edx = edx;
    return 0;
}

/* 0x61dac0: a list's selected row. For the title menu's dispatch, with Level Select in the list:
   the rows below it as the game numbers them; Level Select itself past the game's five, where the
   dispatch does nothing (accepted some way hook_title_update_fn's test did not see: opened next frame) */
static int hook_list_selected_fn(CPU *c)
{
    uint32_t e = c->esp;
    if (R32(e) != TITLE_DISPATCH || R32(L_COUNT(c->ecx)) != TITLE_ROWS)
        return 0;
    uint32_t row = R32(L_SEL(c->ecx));
    if (row == TITLE_ROW_LEVELS) {
        ls_accepted = 1;
        return decomp_ret(c, e, TITLE_ROWS, 0);
    }
    return decomp_ret(c, e, row > TITLE_ROW_LEVELS ? row - 1 : row, 0);
}

/* 0x65ca66: the logos page. The legal notices can be skipped with accept, the logos' skip or back. */
static int hook_logos_update_fn(CPU *c)
{
    uint32_t e = c->esp, fe = c->ecx, eax = c->eax, edx = c->edx;
    if (R32(FE_STATE(fe)) != LOGOS_LEGAL)
        return 0;
    if (menu_key(c, fe, 4) || menu_key(c, fe, 6) || menu_key(c, fe, 7)) {
        W32(LOGOS_FADE(fe), (uint32_t)-100);
        game_callm(c, LOGOS_NEXT, fe, 0);
        return decomp_ret(c, e, c->eax, 0);
    }
    c->eax = eax;
    c->ecx = fe;
    c->edx = edx;
    return 0;
}

void levels_install(void)
{
    hook_title_build = hook_title_build_fn;
    hook_title_update = hook_title_update_fn;
    hook_list_selected = hook_list_selected_fn;
    hook_logos_update = hook_logos_update_fn;
    const char *name = cli_level ? cli_level : getenv("ETM_LEVEL");
    const char *ch = cli_char ? cli_char : getenv("ETM_LEVEL_CHAR");
    const char *df = cli_diff ? cli_diff : getenv("ETM_LEVEL_DIFFICULTY");
    if (ch && *ch)
        req.ghost = tolower((unsigned char)ch[0]) == 'g' ? 1 : 0;
    if (df && *df)
        req.difficulty = !strncasecmp(df, "e", 1) ? 0 : !strncasecmp(df, "h", 1) ? 2 : 1;
    if (name && *name) {
        snprintf(req.name, sizeof req.name, "%s", name);
        req.cli = 1;
        req.state = REQ_PENDING;
        LOG(LOG_ALWAYS, "level: %s asked for (character %s, difficulty %d)", req.name,
            req.ghost < 0 ? "from the table" : req.ghost ? "Ghost" : "Niobe", req.difficulty);
    }
}

/* the type of the load point running (LP_TYPE: 0 on foot, 1 / 2 driving, 4 / 5 hovercraft, 7 arena),
   read from the game's table at the world's level and load point; -1 when there is none (prompts.c:
   which bindings the tips' FIRE, ACCELERATE... name) */
int levels_point_type(void)
{
    uint32_t w = world();
    if (!w || !R32(W_LEVELS(w)))
        return -1;
    uint32_t g = R32(W_LEVEL(w)), p = R32(W_LOADPOINT(w));
    if (g >= R32(W_NLEVELS(w)) || g >= 64)
        return -1;
    uint32_t rec = R32(W_LEVELS(w)) + 8u * g;
    if (p >= R32(rec) || !R32(rec + 4))
        return -1;
    return (int)R32(R32(rec + 4) + LP_SIZE * p + LP_TYPE);
}

/* ------------------------------------------------------------------ each frame */
static void debug_frame(unsigned long frame);

void levels_frame(unsigned long frame)
{
    debug_frame(frame);
    if (list_mode && table_ready()) {
        list_print();
        process_exit(0);
    }
    uint32_t fe = R32(FRONT_END);
    /* a command-line request answers the language page (page 6 / 7, state 1) with Enter */
    static unsigned long lang_since, enter_up;
    if (enter_up && frame >= enter_up) {
        input_key(DIK_RETURN, 0);
        enter_up = 0;
    }
    if (req.state == REQ_PENDING && req.cli && fe && (R32(FE_PAGE(fe)) == 6 || R32(FE_PAGE(fe)) == 7) &&
        R32(FE_STATE(fe)) == 1) {
        if (!lang_since)
            lang_since = frame;
        if (frame - lang_since >= 60 && (frame - lang_since) % 60 == 0 && !enter_up) {
            LOG(LOG_ALWAYS, "level: the language page answered (Enter) at frame %lu", frame);
            input_key(DIK_RETURN, 1);
            enter_up = frame + 3;
        }
    } else
        lang_since = 0;
    /* the load point running: the scripted input's mark */
    uint32_t w = world();
    if (req.state == REQ_STARTED && !fe && w && R32(W_UPDATES(w)) > 0 &&
        (int)R32(W_LEVEL(w)) == lv[req.k].level && (int)R32(W_LOADPOINT(w)) == lv[req.k].point) {
        req.state = REQ_PLAYING;
        req.mark = frame;
        input_script_mark(frame);
        LOG(LOG_ALWAYS, "level: %d.%d %s running from frame %lu", lv[req.k].level, lv[req.k].point, lv[req.k].file, frame);
    }
    static long stop_after = -2;        /* ETM_LEVEL_FRAMES=N (tests): the run ends N frames after the mark */
    if (stop_after == -2)
        stop_after = getenv("ETM_LEVEL_FRAMES") ? atol(getenv("ETM_LEVEL_FRAMES")) : -1;
    if (req.state == REQ_PLAYING && req.cli && stop_after > 0 && frame >= req.mark + (unsigned long)stop_after) {
        LOG(LOG_ALWAYS, "level: %ld frames played from the load point's start (frame %lu): done", stop_after, frame);
        process_exit(0);
    }
    /* ETM_LEVEL_SKIP=1 (tests): in the first 1500 frames a story film and, but in a cut scene's own
       load point, an in-engine scene are skipped with Escape, as a player would, once each has shown
       for 20 frames; three presses at most (Escape skips both: Enter skips neither [verified: 17.20,
       six Enters from +600 changed nothing, Escape at +600 brought the save prompt at +700]) */
    static int skip = -1, presses;
    static unsigned long film_for, scene_for, key_up;
    static int key_dik = DIK_ESCAPE;
    if (skip < 0)
        skip = getenv("ETM_LEVEL_SKIP") && atoi(getenv("ETM_LEVEL_SKIP")) > 0;
    if (key_up && frame >= key_up) {
        input_key(key_dik, 0);
        key_up = 0;
    }
    if (skip && req.state == REQ_PLAYING && req.cli && frame < req.mark + 1500 && !key_up && presses < 3) {
        extern int game_film_playing(void), game_cutscene(void);
        film_for = game_film_playing() ? film_for + 1 : 0;
        scene_for = lv[req.k].level != CUTSCENES && game_cutscene() ? scene_for + 1 : 0;
        if (film_for >= 20 || scene_for >= 20) {
            LOG(LOG_ALWAYS, "level: %s skipped (Escape) at frame %lu (+%lu)", film_for >= 20 ? "a film" : "an in-engine scene",
                frame, frame - req.mark);
            presses++;
            input_key(key_dik, 1);
            key_up = frame + 3;
            film_for = scene_for = 0;
        }
    }
    if (req.state == REQ_PLAYING && req.cli && (frame - req.mark) % 100 == 0) {
        uint32_t v = R32(VIEWS);
        float p[3] = {0, 0, 0};
        for (int i = 0; v && i < 3; i++) {
            uint32_t b = R32(v + 0x2d4 + 4 * (uint32_t)i);
            memcpy(&p[i], &b, 4);
        }
        uint32_t look = 2166136261u;    /* the view matrix (+0x240), hashed: turning shows */
        for (uint32_t i = 0; v && i < 64; i++)
            look = (look ^ R8(v + 0x240 + i)) * 16777619u;
        extern int game_film_playing(void);
        LOG(LOG_ALWAYS, "level: frame %lu (+%lu) level %d.%d screen %d bars %d film %d camera %.1f %.1f %.1f view %08x",
            frame, frame - req.mark, w ? (int)R32(W_LEVEL(w)) : -1, w ? (int)R32(W_LOADPOINT(w)) : -1, (int)R32(SCREEN),
            (int)R32(CINEMA_BARS), game_film_playing(), p[0], p[1], p[2], look);
    }
}

/* ------------------------------------------------------------------ ETM_LEVELS_DEBUG=1 */
static void debug_frame(unsigned long frame)
{
    static int dbg = -1;
    if (dbg < 0)
        dbg = getenv("ETM_LEVELS_DEBUG") != NULL;
    if (!dbg)
        return;
    uint32_t w = world(), fe = R32(FRONT_END);
    static int dumped;
    if (!dumped && table_ready()) {
        dumped = 1;
        for (int g = 0; g < nlevels; g++)
            LOG(LOG_ALWAYS, "levels: %d \"%s\"", g, level_name[g]);
        for (int k = 0; k < nlv; k++)
            LOG(LOG_ALWAYS, "levels: %d.%d type %d %s %s \"%s\"%s", lv[k].level, lv[k].point, lv[k].type, who(&lv[k]),
                lv[k].file, lv[k].title, lv[k].exists ? "" : " (no file)");
    }
    static uint32_t last[12];
    uint32_t now[12] = {w, w ? R32(W_LEVEL(w)) : 0, w ? R32(W_LOADPOINT(w)) : 0, R32(DIFFICULTY), R32(SAVE_PROMPT),
                        R32(0x947f08), R32(SCREEN), fe, fe ? R32(FE_PAGE(fe)) : 0, fe ? R32(FE_STATE(fe)) : 0,
                        fe ? R32(FE_MOVIE(fe)) : 0, R32(0x97ff10)};
    if (memcmp(now, last, sizeof now)) {
        memcpy(last, now, sizeof now);
        LOG(LOG_ALWAYS, "levels: frame %lu world %08x level %d.%d difficulty %d c92c %d | mode %d screen %d | "
            "front end %08x page %d state %d film %d | ghost %d",
            frame, now[0], (int32_t)now[1], (int32_t)now[2], (int32_t)now[3], (int32_t)now[4], (int32_t)now[5],
            (int32_t)now[6], now[7], (int32_t)now[8], (int32_t)now[9], (int32_t)now[10], (int32_t)now[11]);
    }
}
