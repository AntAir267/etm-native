/* THE OBJECTS' DRAW LIST and visibility (DESIGN.md "The drawing code's rewrite"): the small steps
   of 0x6f309c (the objects' drawing, rewritten by the object submission batch) that decide which
   objects are drawn this view and list them by model: the visibility test per object (0x6f242f:
   the cell it is in, its bounding sphere against the view's frustum or the cell's portal,
   0x64b4f1 and 0x628922), the list itself ("Too many items in draw list": 0x6f2e67, 0x6f2e8b,
   0x6f2fab, 0x6f36d9), a culled character's stand-in bone positions (0x6d1b6a) and the profiler's
   colour marks (0x7bf160).

   What moves between updates [verified: the reads below]: the objects' bounding spheres
   (OBJ_SPHERE, from their positions), the frustum planes (CAM_PLANES, from the camera), a culled
   character's matrix (its visual's +0x40). */
#include "world.h"

/* an object (a level actor: renderpass.c's actor list) */
#define OBJ_NEXT_ATTACHED      0xd0u      /* in its owner's attachment list */
#define OBJ_FLAGS              0xd8u      /* 4 hidden, 0x40000 seen this view */
#define OF_HIDDEN              0x4u
#define OF_SEEN                0x40000u
#define OBJ_PLACED             0xe4u      /* -1: not placed in the level yet */
#define OBJ_SPHERE             0x144u     /* vec3: its bounding sphere's centre, */
#define OBJ_RADIUS             0x150u     /* ... radius */
#define OBJ_TYPE               0x154u     /* its type record: */
#define TYPE_MODEL             0x14u      /* ... the model (its +0x40: has something to draw) */
#define TYPE_KIND              0x1cu
#define TYPE_ID                0x20u
#define OBJ_VISUAL             0x188u     /* a character's visual (0x6d0f2d draws it), or 0 */
#define OBJ_0x18c              0x18cu     /* copied to 0x9920b0 when the camera's own character is drawn */
#define OBJ_0x1a0              0x1a0u
#define OBJ_CELL               0x1b4u     /* the cell (room) it is in: flags +0 (1 seen, 4 not) */
#define OBJ_ATTACHED           0x380u     /* its first attached object */

/* a view (what the render pass passes) */
#define VIEW_OWNER             0x3cu      /* the character the camera belongs to [guess: the player] */
#define VIEW_FRUSTUM_ONLY      0x170u     /* objects tested against the frustum, not cells [guess] */

#define CAM_PLANES             (CAM + 0x114)  /* the frustum's 6 planes (16 bytes: normal, distance) */
#define CAM_FAR_PLANE          (CAM + 0x154)  /* (the fifth, used with a cell's portal) */
#define CAM_CELL               0x358u     /* (offset) the camera's cell */
#define CELL_PORTAL            0x54u      /* a cell's 4 planes, as seen this view */

/* the draw list (16-byte records: model, object, the next with the same model, next) */
#define DL_COUNT               0x0u
#define DL_CAPACITY            0x4u
#define DL_RECORDS             0x8u
#define DL_FREE                0xcu
#define DL_HEAD                0x10u
#define DR_MODEL               0x0u
#define DR_OBJECT              0x4u
#define DR_SAME                0x8u
#define DR_NEXT                0xcu
#define DRAWLIST_ADD           0x6f2e8bu
#define ERROR_PRINT            0x6303eeu  /* (message) */
#define MSG_DRAWLIST_FULL      0x93c948u  /* "Too many items in draw list. Tell Saxs\n" */

#define PLAYER                 0x97f560u  /* the player's state [guess]: +0x2d4, +0x2d8 modes, */
#define PL_MODE                0x2d4u
#define PL_MODE_NEXT           0x2d8u
#define PL_BLEND               0x2ecu     /* ... a 0..1 blend between them [guess] */
#define GAME_STATE_97ed80      0x97ed80u  /* 3: the camera's character not drawn first-person */
#define FLAG_992bec            0x992becu
#define OWNER_0x9920b0         0x9920b0u
#define K_06                   0x8fcb6cu  /* .rdata floats 0.6, 0.4 */
#define K_04                   0x8fcbd4u

#define PLANE_DIST             0x628922u
#define SPHERE_VISIBLE         0x64b4f1u
#define OBJ_UNPLACED           0x6ee140u  /* (object) */
#define OBJ_HIDDEN_DONE        0x6edbfd   /* (object, 0) */
#define CHAR_CULLED            0x6d1b6au
#define CHAR_DRAW              0x6d0f2du  /* (this = visual; 0, 100): the characters batch */
#define CELL_TEST              0x6ebbb0u
#define MAT_TRANSLATION        0x41f450u  /* (this = 4x4) -> its translation (+0x30) */
#define VEC3_COPY              0x41f100u  /* (this = vec3; src) */

/* ------------------------------------------------------------------ the draw list */

/* 0x6f2e67 (list): empties the draw list. */
static int dc_6f2e67(CPU *c)
{
    uint32_t e = c->esp, list = ARG(0);
    W32(list + DL_FREE, R32(list + DL_RECORDS));
    W32(list + DL_COUNT, 0);
    W32(list + DL_HEAD, 0);
    return decomp_ret(c, e, list, 0);
}
DECOMP(6f2e67, dc_6f2e67);

/* 0x6f2e8b (list, object, model, at_end): lists an object to be drawn with its model: in front of
   the record of the same model when there is one (that record linked from it), else at the end
   (at_end, a byte) or the front. A full list is reported. */
static int dc_6f2e8b(CPU *c)
{
    uint32_t e = c->esp, list = ARG(0), obj = ARG(1), model = ARG(2), at_end = ARG(3) & 0xff;
    uint32_t rec = R32(list + DL_FREE), prev = 0;
    W32(rec + DR_OBJECT, obj);
    W32(rec + DR_MODEL, model);
    uint32_t p = R32(list + DL_HEAD);
    while (p && R32(p + DR_MODEL) != model) {
        prev = p;
        p = R32(p + DR_NEXT);
    }
    if (p) {
        W32(rec + DR_SAME, p);
        W32(rec + DR_NEXT, R32(p + DR_NEXT));
        if (prev)
            W32(prev + DR_NEXT, rec);
        else
            W32(list + DL_HEAD, rec);
    } else {
        if (!R32(list + DL_COUNT)) {
            W32(list + DL_HEAD, rec);
            W32(rec + DR_NEXT, 0);
        } else if (at_end) {
            W32(prev + DR_NEXT, rec);
            W32(rec + DR_NEXT, 0);
        } else {
            W32(rec + DR_NEXT, R32(list + DL_HEAD));
            W32(list + DL_HEAD, rec);
        }
        W32(rec + DR_SAME, 0);
    }
    W32(list + DL_FREE, R32(list + DL_FREE) + 0x10);
    W32(list + DL_COUNT, R32(list + DL_COUNT) + 1);
    uint32_t r = list;
    if (R32(list + DL_COUNT) == R32(list + DL_CAPACITY))
        r = game_call(c, ERROR_PRINT, 1, MSG_DRAWLIST_FULL);
    return decomp_ret(c, e, r, 0);
}
DECOMP(6f2e8b, dc_6f2e8b);

/* 0x6f2fab (list, object): lists an object (at the end for kinds 0xd and 0x11) and, when the
   settings' +0x104 is set, each of its attached objects that is not hidden and has something to
   draw (at the end). Returns how many were listed. */
static int dc_6f2fab(CPU *c)
{
    uint32_t e = c->esp, list = ARG(0), obj = ARG(1);
    uint32_t kind = R32(R32(obj + OBJ_TYPE) + TYPE_KIND);
    uint32_t at_end = kind == 0xd || kind == 0x11;
    /* (the flag's upper bytes are the type record's in the original; 0x6f2e8b reads the byte) */
    game_call(c, DRAWLIST_ADD, 4, list, obj, R32(R32(obj + OBJ_TYPE) + TYPE_MODEL), at_end);
    uint32_t n = 1;
    if (R32(R32(SETTINGS) + 0x104))
        for (uint32_t a = R32(obj + OBJ_ATTACHED); a; a = R32(a + OBJ_NEXT_ATTACHED)) {
            if (R32(a + OBJ_FLAGS) & OF_HIDDEN)
                continue;
            if (R32(R32(R32(a + OBJ_TYPE) + TYPE_MODEL) + 0x40)) {
                game_call(c, DRAWLIST_ADD, 4, list, a, R32(R32(a + OBJ_TYPE) + TYPE_MODEL), 1);
                n++;
            }
        }
    return decomp_ret(c, e, n, 0);
}
DECOMP(6f2fab, dc_6f2fab);

/* 0x6f36d9 (object) -> al: its type is of kind 1, 6 or 8, or id 0xfb (0x6f309c lists these in a
   pass of their own). */
static int dc_6f36d9(CPU *c)
{
    uint32_t e = c->esp, obj = ARG(0);
    uint32_t kind = R32(R32(obj + OBJ_TYPE) + TYPE_KIND);
    uint32_t r = kind == 1 || kind == 6 || kind == 8;
    if (R32(R32(obj + OBJ_TYPE) + TYPE_ID) == 0xfb)
        r = 1;
    return decomp_ret(c, e, (obj & ~0xffu) | r, 0);
}
DECOMP(6f36d9, dc_6f36d9);

/* ------------------------------------------------------------------ visibility */

/* 0x628922 (vec3 point, plane) -> st(0): the point's distance to the plane (normal . point + d). */
static int dc_628922(CPU *c)
{
    uint32_t e = c->esp, p = ARG(0), q = ARG(1);
    f80 d = (f80)RF32(p) * (f80)RF32(q);
    d = d + (f80)RF32(p + 4) * (f80)RF32(q + 4);
    d = d + (f80)RF32(p + 8) * (f80)RF32(q + 8);
    d = d + (f80)RF32(q + 12);
    c->eax = q;
    return decomp_ret_st0(c, e, d, 0);
}
DECOMP(628922, dc_628922);

/* 0x64b4f1 (cell, vec3 centre, float radius, camera state) -> al: a sphere is in view: in front of
   (or less than its radius behind) each of the frustum's six planes when it is in the camera's
   cell (or no cell is given), else each of the cell's four portal planes and the frustum's fifth.
   Each distance is rounded to a float before the test. */
static int dc_64b4f1(CPU *c)
{
    uint32_t e = c->esp, cell = ARG(0), centre = ARG(1), cam = ARG(3);
    uint32_t planes[6], n;
    if (!cell || cell == R32(cam + CAM_CELL)) {
        for (n = 0; n < 6; n++)
            planes[n] = cam + (CAM_PLANES - CAM) + 16 * n;
    } else {
        uint32_t portal = R32(cell + CELL_PORTAL);
        for (n = 0; n < 4; n++)
            planes[n] = portal + 16 * n;
        planes[n++] = cam + (CAM_FAR_PLANE - CAM);
    }
    float behind = (float)-(f80)u2f(ARG(2));
    for (uint32_t i = 0; i < n; i++) {
        float d = (float)game_call_st0(c, PLANE_DIST, 2, centre, planes[i]);
        if (!((f80)d > (f80)behind)) {
            /* eax: the compare's C0/C3 bits (al 0), as the original leaves it */
            uint32_t sw = d != d || behind != behind ? 0x4100 : d < behind ? 0x100 : 0x4000;
            return decomp_ret(c, e, sw, 0);
        }
    }
    return decomp_ret(c, e, 1, 0);
}
DECOMP(64b4f1, dc_64b4f1);

/* 0x6ebbb0 (object) -> 0 when it has no +0x1a0, is in a cell, and that cell is not flagged 1 or 4;
   else 1. */
static int dc_6ebbb0(CPU *c)
{
    uint32_t e = c->esp, obj = ARG(0);
    if (R32(obj + OBJ_0x1a0))
        return decomp_ret(c, e, 1, 0);
    uint32_t r = !R32(obj + OBJ_CELL) || (R32(R32(obj + OBJ_CELL)) & 5) ? 1 : 0;
    return decomp_ret(c, e, r, 0);
}
DECOMP(6ebbb0, dc_6ebbb0);

/* 0x6d1b6a (this = a character's visual): when the character is not drawn, its 14 bone positions
   (+0x3f4, 12 bytes each) become its position (its matrix at +0x40) raised by each bone's height
   in its skeleton's rest table, +0x4c8 = -10000 and +0x4d3 = 1; not in view modes other than 0. */
static int dc_6d1b6a(CPU *c)
{
    uint32_t e = c->esp, vis = c->ecx;
    if (R32(R32(SETTINGS) + 0xf8))
        return decomp_ret(c, e, R32(SETTINGS), 0);
    W32(vis + 0x4c8, 0xc61c4000u);         /* -10000 */
    W8(vis + 0x4d3, 1);
    for (uint32_t i = 0; i < 14; i++) {
        uint32_t t = game_callm(c, MAT_TRANSLATION, vis + 0x40, 0);
        game_callm(c, VEC3_COPY, vis + 0x3f4 + i * 0xc, 1, t);
        uint32_t bone = R32(R32(vis + 0x168) + 0x30 + i * 4);
        uint32_t rest = R32(R32(vis + 0x2c0) + 0x20);
        uint32_t y = vis + i * 0xc + 0x3f8;
        WF32(y, (float)((f80)RF32(y) + (f80)RF32(rest + bone * 12 + 4)));
    }
    return decomp_ret(c, e, 14, 0);
}
DECOMP(6d1b6a, dc_6d1b6a);

/* 0x6f242f (view, object) -> al: whether an object is drawn this view (OF_SEEN set). Not when not
   placed yet (0x6ee140) or hidden (0x6edbfd); the camera's own character in the player modes
   that show it (3, 4, 6, by PL_BLEND) is drawn here directly (0x6d0f2d) and not listed; others
   are tested by their cell (view modes other than 1 and 2) and their bounding sphere (0x64b4f1:
   against the view's own character's cell in modes 1 and 2). A character not drawn gets its
   stand-in bone positions (0x6d1b6a). */
static int dc_6f242f(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0), obj = ARG(1);
    if (R32(obj + OBJ_PLACED) == 0xffffffffu) {
        game_call(c, OBJ_UNPLACED, 1, obj);
        return decomp_ret(c, e, c->eax & ~0xffu, 0);
    }
    W32(obj + OBJ_FLAGS, R32(obj + OBJ_FLAGS) & ~OF_SEEN);
    if (R32(obj + OBJ_FLAGS) & OF_HIDDEN) {
        if (R32(obj + OBJ_VISUAL))
            game_callm(c, CHAR_CULLED, R32(obj + OBJ_VISUAL), 0);
        game_call(c, OBJ_HIDDEN_DONE, 2, obj, 0);
        return decomp_ret(c, e, c->eax & ~0xffu, 0);
    }
    if (obj == R32(view + VIEW_OWNER)) {
        uint32_t m1 = R32(R32(PLAYER) + PL_MODE), m2 = R32(R32(PLAYER) + PL_MODE_NEXT), show = 0;
        if (R32(GAME_STATE_97ed80) != 3 && !R32(FLAG_992bec)) {
            int in1 = m1 == 4 || m1 == 3 || m1 == 6, in2 = m2 == 4 || m2 == 3 || m2 == 6;
            f80 blend = (f80)RF32(R32(PLAYER) + PL_BLEND);
            if (in1) {
                /* (unordered counts as below) */
                if (!(blend < (f80)RF32(K_06) || blend != blend) || in2)
                    show = 1;
            } else if (in2 && blend < (f80)RF32(K_04))
                show = 1;
        }
        if (show) {
            W32(OWNER_0x9920b0, R32(obj + OBJ_0x18c));
            if (R32(obj + OBJ_VISUAL))
                game_callm(c, CHAR_DRAW, R32(obj + OBJ_VISUAL), 2, 0, 0x64);
            return decomp_ret(c, e, c->eax & ~0xffu, 0);
        }
    } else if (R32(R32(SETTINGS) + 0xf8) != 2 && R32(R32(SETTINGS) + 0xf8) != 1) {
        if ((R32(R32(obj + OBJ_CELL)) & 4) || !game_call(c, CELL_TEST, 1, obj)) {
            if (R32(obj + OBJ_VISUAL))
                game_callm(c, CHAR_CULLED, R32(obj + OBJ_VISUAL), 0);
            return decomp_ret(c, e, c->eax & ~0xffu, 0);
        }
    }
    /* (the original reads the type record into a local it never uses) */
    if (!R32(view + VIEW_FRUSTUM_ONLY) && obj != R32(view + VIEW_OWNER)) {
        uint32_t cell = R32(R32(SETTINGS) + 0xf8) == 1 || R32(R32(SETTINGS) + 0xf8) == 2
                        ? R32(R32(view + VIEW_OWNER) + OBJ_CELL) : R32(obj + OBJ_CELL);
        if (!(game_call(c, SPHERE_VISIBLE, 4, cell, obj + OBJ_SPHERE, R32(obj + OBJ_RADIUS), CAM) & 0xff)) {
            if (R32(obj + OBJ_VISUAL))
                game_callm(c, CHAR_CULLED, R32(obj + OBJ_VISUAL), 0);
            return decomp_ret(c, e, c->eax & ~0xffu, 0);
        }
    } else if (R32(view + VIEW_FRUSTUM_ONLY)) {
        if (!(game_call(c, SPHERE_VISIBLE, 4, 0, obj + OBJ_SPHERE, R32(obj + OBJ_RADIUS), CAM) & 0xff)) {
            if (R32(obj + OBJ_VISUAL))
                game_callm(c, CHAR_CULLED, R32(obj + OBJ_VISUAL), 0);
            return decomp_ret(c, e, c->eax & ~0xffu, 0);
        }
    }
    W32(obj + OBJ_FLAGS, R32(obj + OBJ_FLAGS) | OF_SEEN);
    return decomp_ret(c, e, (obj & ~0xffu) | 1, 0);
}
DECOMP(6f242f, dc_6f242f);

/* ------------------------------------------------------------------ the profiler */

#define PROF_THREAD            0x97da58u  /* the marks are kept for this id only */
#define PROF_COUNT             0x97da3cu  /* marks this frame (at most 16) */
#define PROF_MARKS             0x949becu  /* pointer: 4 bytes each (r, g, b, time) */
#define PROF_CLOCK             0x97fee4u  /* () -> st(0): the frame's time so far [guess: 0..2] */
#define K_128                  0x8fa350u  /* .rdata float 128 */

/* 0x7bf160 (id, r, g, b): a profiler mark in colour (r, g, b) at the clock's time x 128, for the
   profiled id. */
static int dc_7bf160(CPU *c)
{
    uint32_t e = c->esp, id = ARG(0), r = ARG(1), g = ARG(2), b = ARG(3);
    if (R32(PROF_THREAD) != id || R32(PROF_COUNT) == 0x10)
        return decomp_ret(c, e, R32(PROF_THREAD), 0);
    game_call(c, R32(PROF_CLOCK), 0);     /* (its result left on the x87 stack) */
    ST(0) = ST(0) * (f80)RF32(K_128);
    uint32_t t = game_call(c, FTOL, 0);
    uint32_t p = R32(PROF_MARKS) + R32(PROF_COUNT) * 4;
    W32(PROF_COUNT, R32(PROF_COUNT) + 1);
    W8(p, r);
    W8(p + 1, g);
    W8(p + 2, b);
    W8(p + 3, t);
    return decomp_ret(c, e, (p & ~0xffu) | (t & 0xff), 0);
}
DECOMP(7bf160, dc_7bf160);
