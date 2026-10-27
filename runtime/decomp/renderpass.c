/* THE RENDER PASS's small steps (the block at the start of each frame's view loop in 0x646e8b
   that draws the state the last updates left: DESIGN.md "The drawing code's rewrite").
   Worked example of the rewrite's form: plain C over the game's memory, every call through
   game_call / game_callm (so each callee, rewritten or not, can be switched off alone with
   ETM_DECOMP_OFF), locals a callee is handed in a dc_frame. */
#include "decomp.h"

#define LEVEL_PTR        0x97f720u  /* the level (its actor list at +0x50) [verified: 0x646c9e] */
#define LEVEL_ACTORS     0x50u      /* first actor; each actor's next at +0xd0 */
#define ACTOR_NEXT       0xd0u
#define ACTOR_EXTRA      0x1a4u     /* an object drawn with the actor, or 0 [guess: attachment] */

#define EXTRA_DRAW       0x77eee5u  /* (this = the actor's +0x1a4 object; the actor) draws it [verified: 0x646cd4..ce1] */

/* 0x646c9e: every actor of the level with an object at +0x1a4 has it drawn (0x77eee5, a method
   of that object, the actor as its argument). */
static int dc_646c9e(CPU *c)
{
    uint32_t e = c->esp;
    for (uint32_t a = R32(R32(LEVEL_PTR) + LEVEL_ACTORS); a; a = R32(a + ACTOR_NEXT))
        if (R32(a + ACTOR_EXTRA))
            game_callm(c, EXTRA_DRAW, R32(a + ACTOR_EXTRA), 1, a);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(646c9e, dc_646c9e);

#define HOOK_BEFORE      0x97ee78u  /* the service table's (cdecl, two arguments) hooks round the */
#define HOOK_AROUND      0x97ee98u  /* drawing of a view: before (0, 0), around ((0, 0) then (1, 1)), */
#define HOOK_AFTER       0x97ee7cu  /* after (0, 0); each may be 0 [verified: 0x646c33] */
#define VIEW_DRAW        0x5fef62u  /* (view, 0): the view's scene [guess: from its use here] */

/* 0x646c33 (view): the view's scene (0x5fef62) between the service table's optional hooks. */
static int dc_646c33(CPU *c)
{
    uint32_t e = c->esp, view = ARG(0);
    if (R32(HOOK_BEFORE))
        game_call(c, R32(HOOK_BEFORE), 2, 0, 0);
    if (R32(HOOK_AROUND))
        game_call(c, R32(HOOK_AROUND), 2, 0, 0);
    game_call(c, VIEW_DRAW, 2, view, 0);
    if (R32(HOOK_AROUND))
        game_call(c, R32(HOOK_AROUND), 2, 1, 1);
    if (R32(HOOK_AFTER))
        game_call(c, R32(HOOK_AFTER), 2, 0, 0);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(646c33, dc_646c33);

/* Two optional placed things (0x97ff2c and 0x97ff30: an object whose 4x4 at +0x40 places them,
   with a position 0x97ff14 / 0x97ff20 each), each drawn by up to two drawers (0x97ff34, 0x97ff3c
   for the first; 0x97ff38, 0x97ff40 for the second) [verified: 0x646cec; what they are: guess] */
#define PLACED_A         0x97ff2cu
#define PLACED_B         0x97ff30u
#define PLACED_A_POS     0x97ff14u
#define PLACED_B_POS     0x97ff20u
#define PLACED_A_DRAW1   0x97ff34u
#define PLACED_A_DRAW2   0x97ff3cu
#define PLACED_B_DRAW1   0x97ff38u
#define PLACED_B_DRAW2   0x97ff40u
#define OBJ_MATRIX       0x40u

#define VEC_BY_MATRIX    0x41f470u  /* (this = vec3; matrix) */
#define MAT_INIT         0x47f630u  /* (this = 4x4 record, 0x44 bytes) */
#define MAT_FROM         0x41f400u  /* (this = record; matrix) */
#define MAT_WITH_VEC     0x41f450u  /* (this = record; vec3) -> an object for MAT_FINISH */
#define MAT_FINISH       0x41f100u  /* (this = what MAT_WITH_VEC returned) */
#define DRAWER_PLACE     0x7528e5u  /* (this = drawer; vec3, 1) */
#define DRAWER_DRAW      0x7530e1u  /* (this = drawer; record, 0x8000000) */

static void placed(CPU *c, uint32_t obj, uint32_t pos, uint32_t d1, uint32_t d2)
{
    /* the record (0x44) then the position (12), as the original's frame had them */
    uint32_t F = dc_frame(c, 0x50), rec = F, v = F + 0x44;
    W32(v, R32(pos));
    W32(v + 4, R32(pos + 4));
    W32(v + 8, R32(pos + 8));
    game_callm(c, VEC_BY_MATRIX, v, 1, R32(obj) + OBJ_MATRIX);
    game_callm(c, MAT_INIT, rec, 0);
    game_callm(c, MAT_FROM, rec, 1, R32(obj) + OBJ_MATRIX);
    uint32_t r = game_callm(c, MAT_WITH_VEC, rec, 1, v);
    game_callm(c, MAT_FINISH, r, 0);
    if (R32(d1)) {
        game_callm(c, DRAWER_PLACE, R32(d1), 2, v, 1);
        game_callm(c, DRAWER_DRAW, R32(d1), 2, rec, 0x8000000);
    }
    if (R32(d2)) {
        game_callm(c, DRAWER_PLACE, R32(d2), 2, v, 1);
        game_callm(c, DRAWER_DRAW, R32(d2), 2, rec, 0x8000000);
    }
    dc_unframe(c, F, 0x50);
}

/* 0x646cec: the two optional placed things, each placed by its object's matrix and drawn. */
static int dc_646cec(CPU *c)
{
    uint32_t e = c->esp;
    if (R32(PLACED_A))
        placed(c, PLACED_A, PLACED_A_POS, PLACED_A_DRAW1, PLACED_A_DRAW2);
    if (R32(PLACED_B))
        placed(c, PLACED_B, PLACED_B_POS, PLACED_B_DRAW1, PLACED_B_DRAW2);
    return decomp_ret(c, e, c->eax, 0);
}
DECOMP(646cec, dc_646cec);
