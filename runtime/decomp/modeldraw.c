/* AN OBJECT'S MODEL DRAWN (object submission, DESIGN.md "The drawing code's rewrite"): 0x7b1808
   draws the parts of one actor's model for one pass (the 4x4s, the bone palette, each part's
   material, buffers and DrawIndexedPrimitive), with the matrix and vector helpers it uses and the
   bone palette loader (0x7ae7b8, 0x7ae788). Its caller is 0x7b1838 (objects.c). */
#include "objsubmit.h"

/* ---------------------------------------------------------------- matrix and vector helpers */

/* 0x7a68d0: the 4x4 matrix's constructor, which does nothing (returns this). The rewrites here
   don't call it. */
static int dc_7a68d0(CPU *c)
{
    return decomp_ret(c, c->esp, c->ecx, 0);
}
DECOMP(7a68d0, dc_7a68d0);

/* 0x7a6280 (this = 4x4; src): this = src (16 dwords); returns this. */
static int dc_7a6280(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, src = ARG(0);
    for (uint32_t i = 0; i < 64; i += 4)
        W32(m + i, R32(src + i));
    return decomp_ret(c, e, m, 1);
}
DECOMP(7a6280, dc_7a6280);

/* 0x7a6430 (this = A; out, B): out = A x B (row vectors: A applied first), through a temporary,
   so out may be A or B; returns out. */
static int dc_7a6430(CPU *c)
{
    uint32_t e = c->esp, a = c->ecx, out = ARG(0), b = ARG(1);
    uint32_t t[16];
    for (uint32_t i = 0; i < 4; i++)
        for (uint32_t j = 0; j < 4; j++) {
            uint32_t r = a + 16 * i, k = b + 4 * j;
            f80 x = (f80)RF32(r) * RF32(k) + (f80)RF32(r + 4) * RF32(k + 0x10);
            x = x + (f80)RF32(r + 8) * RF32(k + 0x20);
            x = x + (f80)RF32(r + 12) * RF32(k + 0x30);
            t[4 * i + j] = f2u((float)x);
        }
    for (uint32_t i = 0; i < 16; i++)
        W32(out + 4 * i, t[i]);
    return decomp_ret(c, e, out, 2);
}
DECOMP(7a6430, dc_7a6430);

/* 0x7b40c4 (this = vec3; x, y, z): sets the vector; returns this. */
static int dc_7b40c4(CPU *c)
{
    uint32_t e = c->esp, v = c->ecx;
    W32(v, ARG(0));
    W32(v + 4, ARG(1));
    W32(v + 8, ARG(2));
    return decomp_ret(c, e, v, 3);
}
DECOMP(7b40c4, dc_7b40c4);

/* 0x7b3ef8 (this = 4x4; v): v = v rotated by the matrix's 3x3 (a direction: no translation), in
   place; returns v. */
static int dc_7b3ef8(CPU *c)
{
    uint32_t e = c->esp, m = c->ecx, v = ARG(0);
    float x = RF32(v), y = RF32(v + 4), z = RF32(v + 8);
    for (uint32_t i = 0; i < 3; i++) {
        f80 r = (f80)RF32(m + 4 * i) * x + (f80)RF32(m + 0x10 + 4 * i) * y;
        WF32(v + 4 * i, (float)(r + (f80)RF32(m + 0x20 + 4 * i) * z));
    }
    return decomp_ret(c, e, v, 1);
}
DECOMP(7b3ef8, dc_7b3ef8);

#define VEC3_SET         0x7b40c4u  /* (this = vec3; x, y, z) */

/* 0x7b3fd4 (out, v, m): out = v transformed by the 4x4 m as a point; returns out. */
static int dc_7b3fd4(CPU *c)
{
    uint32_t e = c->esp, out = ARG(0), v = ARG(1), m = ARG(2);
    uint32_t r[3];
    for (uint32_t i = 0; i < 3; i++) {
        f80 x = (f80)RF32(m + 4 * i) * RF32(v) + (f80)RF32(m + 0x10 + 4 * i) * RF32(v + 4);
        x = x + (f80)RF32(m + 0x20 + 4 * i) * RF32(v + 8);
        r[i] = f2u((float)(x + RF32(m + 0x30 + 4 * i)));
    }
    game_callm(c, VEC3_SET, out, 3, r[0], r[1], r[2]);
    return decomp_ret(c, e, out, 0);
}
DECOMP(7b3fd4, dc_7b3fd4);

/* ---------------------------------------------------------------- the bone palette */

#define BONE_PALETTE     0xe80f60u  /* the bone matrices the parts are skinned with, 64 bytes each (x87 version) */

/* 0x7ae788 (first, src, count): copies count 4x4s to the bone palette from entry first. The SSE
   version (another palette, plus a constant splat) stays with the translation (objsubmit.h). */
static int dc_7ae788(CPU *c)
{
    if (cpu_sse_version(c))
        return 0;
    uint32_t e = c->esp, first = ARG(0), src = ARG(1), count = ARG(2);
    uint32_t eax = game_call(c, MEMCPY, 3, BONE_PALETTE + first * 64, src, count * 64);
    return decomp_ret(c, e, eax, 0);
}
DECOMP(7ae788, dc_7ae788);

#define PALETTE_LOAD     0x7ae788u
/* the actor's skeleton instance (actor +0x188) */
#define SKI_DEF          0x38cu     /* the skeleton definition: */
#define SKEL_BONES       4u         /*   its bone count */
#define SKEL_GROUPS      0x10u      /*   per palette group: short first matrix, short count */
#define SKI_GROUP_MASK   0x390u     /* bit per palette group: the group has bones of its own */
#define SKI_GROUP_BASE   0x394u     /* per group: an index offset added to the group's first matrix */
#define PART_GROUP       0x24u      /* short: the part's palette group (negative: none) */
#define PART_EXTRA_N     0x26u      /* short: extra matrices the part adds after its group's */
#define PART_EXTRA       0x28u      /* the extra matrices' indices (dwords) */

/* 0x7ae7b8 (part, skeleton instance, bone matrices, &skinned, &loaded group): loads the bone
   palette a part is drawn with, from the actor's bone matrices (64 bytes each): its group's run
   (when the group has bones of its own) plus the part's extra matrices after it, and notes the
   group; a part of a group without bones has the whole skeleton loaded, once (loaded group -1). */
static int dc_7ae7b8(CPU *c)
{
    uint32_t e = c->esp, part = ARG(0), ski = ARG(1), mats = ARG(2), skinned = ARG(3), loaded = ARG(4);
    int32_t g = (int16_t)R16(part + PART_GROUP);
    if ((1u << (g & 31)) & R32(ski + SKI_GROUP_MASK)) {
        uint32_t grp = R32(R32(ski + SKI_DEF) + SKEL_GROUPS) + 4 * g;
        game_call(c, PALETTE_LOAD, 3, 0,
                  (int16_t)R16(grp) * 64 + mats + R32(ski + SKI_GROUP_BASE + 4 * g) * 64,
                  (uint32_t)(int16_t)R16(grp + 2));
        for (int32_t j = 0; j < (int16_t)R16(part + PART_EXTRA_N); j++) {
            uint32_t grp2 = R32(R32(ski + SKI_DEF) + SKEL_GROUPS) + 4 * g;
            game_call(c, PALETTE_LOAD, 3, (uint32_t)((int16_t)R16(grp2 + 2) + j),
                      R32(R32(part + PART_EXTRA) + 4 * j) * 64 + mats, 1);
        }
        W32(loaded, (uint32_t)g);
        W8(skinned, 1);
    } else {
        if (R32(loaded) != 0xffffffffu) {
            game_call(c, PALETTE_LOAD, 3, 0, mats, R32(R32(ski + SKI_DEF) + SKEL_BONES));
            W32(loaded, 0xffffffffu);
        }
        W8(skinned, 0);
    }
    return decomp_ret(c, e, skinned, 0);
}
DECOMP(7ae7b8, dc_7ae7b8);

/* ---------------------------------------------------------------- the parts of a model */

#define VERTEX_BUFFER    0x994a14u  /* IDirect3DVertexBuffer9 *: the dynamic buffer every part's vertices are written to */
#define VB_LOCK          0x2cu      /* its vtable: Lock (offset, size, &data, flags) */
#define VB_UNLOCK        0x30u
#define LOCK_DISCARD     0x2000u
#define VERTEX_SIZE      0x20u      /* written: position, normal, u, v (floats) */
#define TRIANGLESTRIP    5u
#define ACTOR_MODEL      0x154u     /* -> +0x14 -> +0x40: the mesh */
#define ACTOR_MATRIX     0x40u      /* the actor's 4x4 (a model without a skeleton is drawn with it) */
#define ACTOR_SKEL       0x188u     /* the actor's skeleton instance, or 0 (0x7ae7b8's second argument) */
#define ACTOR_BONES      0x18cu     /* the actor's bone matrices, 64 bytes each (posed by the characters' code, 0x6d0f2d) */
#define MESH_MATERIALS   0x28u      /* the mesh's materials, 0x48 bytes each (0x7ad508's argument) */
#define CFG_PARTS_N      4u         /* a configuration: its parts' count */
#define CFG_PARTS        8u         /*   and their pointers */
#define PART_LODS        0x2cu      /* a part: its levels of detail, 0x1c bytes each: */
#define LOD_VERTS_N      8u         /*   vertex count */
#define LOD_WEIGHTS_N    0xcu       /*   bones per vertex (0 or 1: one, by index; more: weighted) */
#define LOD_VERTS        0x10u      /*   the source vertices */
#define LOD_GROUPS       0x14u      /*   the strips, 0x18 each, ending with count -1; or 0: nothing to draw */
#define GROUP_IB         0u         /* (the first group's) IDirect3DIndexBuffer9 * */
#define GROUP_MATERIAL   4u
#define GROUP_COUNT      0xcu       /* the strip's index count */
/* a source vertex: position (3 floats), normal at +0xc, u, v at +0x18, then by the part's kind
   the bone index (dword) at +0x24, or bone indices (floats) from +0x2c followed by the weights;
   0x2c bytes, or 0x2c + 8 per bone when weighted */
#define SV_NORMAL        0xcu
#define SV_UV            0x18u
#define SV_BONE          0x24u
#define SV_BONES         0x2cu

#define MATERIAL         0x7ad508u
#define MAT_MUL          0x7a6430u  /* (this = A; out, B) out = A x B */
#define ROTATE           0x7b3ef8u  /* (this = 4x4; v) v rotated in place */
#define TRANSFORM        0x7b3fd4u  /* (out, v, 4x4) out = v as a point */
#define PALETTE_GROUP    0x7ae7b8u

/* the locals the callees are handed (one frame for the whole call) */
#define F_LOCKED         0x00u      /* the vertex buffer's data pointer */
#define F_LOADED         0x04u      /* the palette group loaded (0x7ae7b8's), -2 at first */
#define F_NORMAL         0x10u
#define F_POS            0x20u
#define F_MATRIX         0x40u      /* the actor's 4x4 copied */
#define F_COMBINED       0x80u      /* with the given world matrix, or a bone's */
#define F_BLEND          0xc0u      /* a weighted vertex's blended bone matrix (3x4 of it) */
#define F_BONE_K         0x100u     /* bone k with the world matrix */
#define F_SIZE           0x140u

/* writes one vertex: the position and normal computed, the source's u, v */
static void put_vertex(uint32_t dst, uint32_t pos, uint32_t nrm, uint32_t src)
{
    for (uint32_t k = 0; k < 12; k += 4)
        W32(dst + k, R32(pos + k));
    for (uint32_t k = 0; k < 12; k += 4)
        W32(dst + 0xc + k, R32(nrm + k));
    W32(dst + 0x18, R32(src + SV_UV));
    W32(dst + 0x1c, R32(src + SV_UV + 4));
}

static uint32_t vb_lock(CPU *c, uint32_t F)
{
    uint32_t vb = R32(VERTEX_BUFFER);
    return game_call(c, R32(R32(vb) + VB_LOCK), 5, vb, 0, 0, F + F_LOCKED, LOCK_DISCARD);
}

/* the part's strips: the vertex buffer unlocked and set with the part's index buffer, then each
   strip with its material (set when it changes) */
static void draw_strips(CPU *c, uint32_t mesh, uint32_t lod)
{
    uint32_t vb = R32(VERTEX_BUFFER);
    game_call(c, R32(R32(vb) + VB_UNLOCK), 1, vb);
    uint32_t last = 0xffffffffu, g = R32(lod + LOD_GROUPS), start = 0;
    D3DCALL(c, D3D_SET_STREAM_SOURCE, 4, 0, R32(VERTEX_BUFFER), 0, VERTEX_SIZE);
    D3DCALL(c, D3D_SET_INDICES, 1, R32(g + GROUP_IB));
    for (; R32(g + GROUP_COUNT) != 0xffffffffu; g += 0x18) {
        if (R32(g + GROUP_MATERIAL) != last) {
            game_call(c, MATERIAL, 1, R32(mesh + MESH_MATERIALS) + R32(g + GROUP_MATERIAL) * 0x48);
            last = R32(g + GROUP_MATERIAL);
        }
        D3DCALL(c, D3D_DRAW_INDEXED, 6, TRIANGLESTRIP, 0, 0, R32(lod + LOD_VERTS_N), start,
                R32(g + GROUP_COUNT) - 2);
        start += R32(g + GROUP_COUNT);
    }
}

static uint32_t src_stride(uint32_t lod)
{
    int32_t n = (int32_t)R32(lod + LOD_WEIGHTS_N);
    return n > 1 ? (uint32_t)n * 8 + 0x2c : 0x2c;
}

/* a model without a skeleton: every vertex through the actor's 4x4 (with the world matrix
   given: after it, or before it in a second pass) */
static void draw_rigid(CPU *c, uint32_t F, uint32_t cfg, uint32_t lodi, uint32_t a, uint32_t world,
                       uint32_t second, uint32_t mesh)
{
    game_call(c, MEMCPY, 3, F + F_MATRIX, a + ACTOR_MATRIX, 0x40);
    uint32_t m = F + F_MATRIX;
    if (world && !second) {
        game_callm(c, MAT_MUL, m, 2, F + F_COMBINED, world);
        m = F + F_COMBINED;
    } else if (world) {
        game_callm(c, MAT_MUL, world, 2, F + F_COMBINED, m);
        m = F + F_COMBINED;
    }
    for (int32_t i = 0; i < (int32_t)R32(cfg + CFG_PARTS_N); i++) {
        uint32_t part = R32(R32(cfg + CFG_PARTS) + 4 * i);
        uint32_t lod = R32(part + PART_LODS) + lodi * 0x1c;
        if (!R32(lod + LOD_GROUPS))
            continue;
        vb_lock(c, F);
        uint32_t stride = src_stride(lod), src = R32(lod + LOD_VERTS);
        for (uint32_t j = 0; j < R32(lod + LOD_VERTS_N); j++, src += stride) {
            for (uint32_t k = 0; k < 12; k += 4)
                W32(F + F_NORMAL + k, R32(src + SV_NORMAL + k));
            game_callm(c, ROTATE, m, 1, F + F_NORMAL);
            game_call(c, TRANSFORM, 3, F + F_POS, src, m);
            put_vertex(R32(F + F_LOCKED) + 32 * j, F + F_POS, F + F_NORMAL, src);
        }
        draw_strips(c, mesh, lod);
    }
}

/* a model with a skeleton: per part the bone palette (0x7ae7b8, or the whole skeleton for a part
   of no group), then every vertex through its bone (with the world matrix given: the bone then
   the world; normals by the bone alone), or through its bones blended by their weights */
static void draw_skinned(CPU *c, uint32_t F, uint32_t cfg, uint32_t lodi, uint32_t a, uint32_t world,
                         uint32_t skinned, uint32_t mesh)
{
    int32_t lastg = -2;
    for (int32_t i = 0; i < (int32_t)R32(cfg + CFG_PARTS_N); i++) {
        uint32_t part = R32(R32(cfg + CFG_PARTS) + 4 * i);
        uint32_t lod = R32(part + PART_LODS) + lodi * 0x1c;
        if (!R32(lod + LOD_GROUPS))
            continue;
        vb_lock(c, F);
        if ((int16_t)R16(part + PART_GROUP) != lastg) {
            if ((int16_t)R16(part + PART_GROUP) >= 0)
                game_call(c, PALETTE_GROUP, 5, part, R32(a + ACTOR_SKEL), R32(a + ACTOR_BONES), skinned,
                          F + F_LOADED);
            else {
                W8(skinned, 1);
                if (R32(a + ACTOR_SKEL)) {
                    if (R32(F + F_LOADED) != 0xffffffffu) {
                        game_call(c, PALETTE_LOAD, 3, 0, R32(a + ACTOR_BONES),
                                  R32(R32(R32(a + ACTOR_SKEL) + SKI_DEF) + SKEL_BONES));
                        W32(F + F_LOADED, 0xffffffffu);
                    }
                } else
                    game_call(c, PALETTE_LOAD, 3, 0, R32(a + ACTOR_BONES), 1);
            }
            lastg = (int16_t)R16(part + PART_GROUP);
        }
        uint32_t stride = src_stride(lod), src = R32(lod + LOD_VERTS);
        if (!R8(skinned) || (int32_t)R32(lod + LOD_WEIGHTS_N) <= 1) {
            for (uint32_t j = 0; j < R32(lod + LOD_VERTS_N); j++, src += stride) {
                for (uint32_t k = 0; k < 12; k += 4)
                    W32(F + F_NORMAL + k, R32(src + SV_NORMAL + k));
                game_callm(c, ROTATE, BONE_PALETTE + R32(src + SV_BONE) * 64, 1, F + F_NORMAL);
                if (world) {
                    game_callm(c, MAT_MUL, BONE_PALETTE + R32(src + SV_BONE) * 64, 2, F + F_COMBINED, world);
                    game_call(c, TRANSFORM, 3, F + F_POS, src, F + F_COMBINED);
                } else
                    game_call(c, TRANSFORM, 3, F + F_POS, src, BONE_PALETTE + R32(src + SV_BONE) * 64);
                put_vertex(R32(F + F_LOCKED) + 32 * j, F + F_POS, F + F_NORMAL, src);
            }
        } else {
            uint32_t dst = R32(F + F_LOCKED), mb = F + F_BLEND;
            for (uint32_t j = 0; j < R32(lod + LOD_VERTS_N); j++, src += stride, dst += 32) {
                uint32_t bones = src + SV_BONES, wts = bones + 4 * R32(lod + LOD_WEIGHTS_N);
                uint32_t b = BONE_PALETTE + (uint32_t)fpu_ist64(RF32(bones)) * 64;
                if (world) {
                    game_callm(c, MAT_MUL, b, 2, F + F_COMBINED, world);
                    b = F + F_COMBINED;
                }
                float w = RF32(wts);
                for (uint32_t r = 0; r < 64; r += 16)
                    for (uint32_t k = 0; k < 12; k += 4)
                        WF32(mb + r + k, f32((f80)RF32(b + r + k) * w));
                for (uint32_t n = 1; n < R32(lod + LOD_WEIGHTS_N); n++) {
                    uint32_t bk = BONE_PALETTE + (uint32_t)fpu_ist64(RF32(bones + 4 * n)) * 64;
                    if (world) {
                        game_callm(c, MAT_MUL, bk, 2, F + F_BONE_K, world);
                        bk = F + F_BONE_K;
                    }
                    w = RF32(wts + 4 * n);
                    for (uint32_t r = 0; r < 64; r += 16)
                        for (uint32_t k = 0; k < 12; k += 4)
                            WF32(mb + r + k, f32((f80)RF32(bk + r + k) * w + RF32(mb + r + k)));
                }
                for (uint32_t k = 0; k < 12; k += 4)
                    W32(F + F_NORMAL + k, R32(src + SV_NORMAL + k));
                game_callm(c, ROTATE, mb, 1, F + F_NORMAL);
                for (uint32_t k = 0; k < 12; k += 4) {
                    f80 x = (f80)RF32(mb + k) * RF32(src) + (f80)RF32(mb + 0x10 + k) * RF32(src + 4);
                    x = x + (f80)RF32(mb + 0x20 + k) * RF32(src + 8);
                    WF32(dst + k, f32(x + RF32(mb + 0x30 + k)));
                }
                for (uint32_t k = 0; k < 12; k += 4)
                    W32(dst + 0xc + k, R32(F + F_NORMAL + k));
                W32(dst + 0x18, R32(src + SV_UV));
                W32(dst + 0x1c, R32(src + SV_UV + 4));
            }
        }
        draw_strips(c, mesh, lod);
    }
}

/* 0x7b1808 (config, lod, actor, skinned flag, world or 0, second pass): draws the parts of one
   configuration of the actor's model at one level of detail: each part's vertices transformed on
   the CPU into the dynamic vertex buffer (by the actor's 4x4, or skinned by its bone matrices)
   and its strips drawn (Direct3D's world matrix is the identity 0x7b1838 set). The skinned flag
   (a byte in the argument's own slot) is 0x7ae7b8's output for the parts after. The SSE version
   stays with the translation (objsubmit.h). */
static int dc_7b1808(CPU *c)
{
    if (cpu_sse_version(c))
        return 0;
    uint32_t e = c->esp, cfg = ARG(0), lodi = ARG(1), a = ARG(2), world = ARG(4), second = ARG(5);
    uint32_t skinned = e + 4 + 4 * 3;               /* the flag argument's slot */
    uint32_t mesh = R32(R32(R32(a + ACTOR_MODEL) + 0x14) + 0x40);
    uint32_t F = dc_frame(c, F_SIZE);
    W32(F + F_LOADED, 0xfffffffeu);
    if (!R32(a + ACTOR_SKEL))
        draw_rigid(c, F, cfg, lodi, a, world, second, mesh);
    else
        draw_skinned(c, F, cfg, lodi, a, world, skinned, mesh);
    dc_unframe(c, F, F_SIZE);
    return decomp_ret(c, e, R32(cfg + CFG_PARTS_N), 0);
}
/* Not switched in yet: never checked alone (switched in with 0x7b1838 and the others, which
   differ before it runs). */
DECOMP_PENDING(7b1808, dc_7b1808);
