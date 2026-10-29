/* The interface between the shaders tools/d3d2spv.py compiles (gen/spirv_gen.c) and our Vulkan
   renderer (runtime/vk_*.c; VULKAN.md "Shaders"). Each value here is mirrored in d3d2spv.py's
   header: change both. */
#pragma once
#include <stdint.h>

/* vertex input locations by declaration usage and index (d3d2spv.py INPUT_LOC); -1: none */
static inline int vk_input_loc(unsigned usage, unsigned index)
{
    switch (usage) {
    case 0: case 9: return index == 0 ? 0 : index == 1 && usage == 0 ? 19 : -1;   /* POSITION(T) */
    case 1: return index == 0 ? 1 : -1;                     /* BLENDWEIGHT */
    case 2: return index == 0 ? 2 : -1;                     /* BLENDINDICES */
    case 3: return index == 0 ? 3 : index == 1 ? 20 : -1;   /* NORMAL */
    case 4: return index == 0 ? 4 : -1;                     /* PSIZE */
    case 5: return index < 8 ? 5 + (int)index : -1;         /* TEXCOORD */
    case 6: return index == 0 ? 13 : -1;                    /* TANGENT */
    case 7: return index == 0 ? 14 : -1;                    /* BINORMAL */
    case 10: return index < 2 ? 15 + (int)index : -1;       /* COLOR */
    case 11: return index == 0 ? 17 : -1;                   /* FOG */
    case 12: return index == 0 ? 18 : -1;                   /* DEPTH */
    }
    return -1;
}
#define VK_INPUT_LOCS       21

/* descriptor sets: 0 bindless (bound once a command buffer), 1 the shaders' float constants */
#define VK_B_TEX2D          0
#define VK_B_TEXCUBE        1
#define VK_B_TEX3D          2
#define VK_B_TEXSHADOW      3
#define VK_B_SAMPLER        4
#define VK_MAX_TEXTURES     16384
#define VK_MAX_SAMPLERS     4096
#define VK_VS_CONSTS        256     /* vec4s in the vertex shader's buffer */
#define VK_VS_CHUNK         32      /* ... as VK_VS_CHUNKS buffers of this many each (set 1 binding 0's array: */
#define VK_VS_CHUNKS        8       /*     c[n] is chunk n / 32's n % 32; relative addressing reads chunk 0's 256) */
#define VK_PS_CONSTS        224     /* the pixel shader's, after its PS_EXTRA vec4s */
#define VK_PS_EXTRA         8       /* 0 fog rgb + density; 1 fog end, fog scale, alpha ref, alpha scale;
                                       2-5 bump matrices of stages 0-3; 6-7 bump luminance scale/offset */
#define VK_MAX_SAMPLER_SLOTS 8      /* push constants: per slot (texture index | sampler index << 16) */

/* specialization constants of the pixel shaders: the programmable ones (tools/d3d2spv.py) read 0-4,
   the fixed-function uber-shader (runtime/shaders/ff.frag) 0-3 and 5-17 */
#define VK_SPEC_ALPHA       0       /* D3DCMP of the alpha test, 8 (always) = off */
#define VK_SPEC_FOG         1       /* 0 off, 1 the vertex shader's fog, 2 exp, 3 exp2, 4 linear */
#define VK_SPEC_KINDS       2       /* 2 bits a sampler slot: 0 2D, 1 cube, 2 3D, 3 2D depth compare */
#define VK_SPEC_PROJ        3       /* ps_1_x / fixed function: slots whose coordinates are projected (D3DTTFF_PROJECTED) */
#define VK_SPEC_SHADOWF     4       /* the shadow filter (3, 5, 7; 0 off) [not applied yet] */
#define VK_FFP_OPS          5       /* 4: stages 2k (low 16 bits) and 2k + 1: colour op bits 0-4, alpha op 8-12,
                                       bit 15 the result to TEMP (DXVK's D3D9SpecData stageOps) */
#define VK_FFP_ARGS         9       /* 8: a stage's arguments, 5 bits each (DXVK's stageArgs: colour 0-2, alpha 0-2 at 16) */
#define VK_FFP_FLAGS        17      /* bit 0 specular added, 1 point sprites, 2 fog from Z; bits 8-15 stages
                                       with no texture (they sample zero, as DXVK's null type) */
#define VK_SPEC_COUNT       18

/* specialization constants of the fixed-function vertex uber-shader (runtime/shaders/ff.vert) */
#define VK_FFV_FLAGS        0       /* bits: 0 POSITIONT, 1 COLOR0, 2 COLOR1, 3 PSIZE, 4 FOG in the declaration;
                                       5 lighting, 6 normalize normals, 7 local viewer, 8 range fog, 9 specular,
                                       10 point scale, 11 tweening, 12 fog on, 13 pixel shader model 3, 14 points;
                                       16-17 diffuse, 18-19 ambient, 20-21 specular, 22-23 emissive material
                                       source (D3DMCS_*) */
#define VK_FFV_LIGHTS       1       /* 2 bits a light, the enabled ones in index order: 1 point, 2 spot, 3 directional */
#define VK_FFV_TCI          2       /* 3 bits a stage: the texture coordinate set it reads */
#define VK_FFV_TCIF         3       /* 3 bits a stage: D3DTSS_TCI_* >> 16 */
#define VK_FFV_TTFF         4       /* 3 bits a stage: D3DTTFF_COUNT* (without PROJECTED) */
#define VK_FFV_TCMASK       5       /* 3 bits a coordinate set: its components in the declaration */
#define VK_FFV_MISC         6       /* bits 0-7 projected stages, 8-9 the vertex fog mode */
#define VK_FFV_COUNT        7

/* the fixed-function shaders in pipeline keys (in place of a bytecode hash) */
#define VK_FF_VS_HASH       1ull
#define VK_FF_PS_HASH       2ull
#define VK_FF_PS_CONSTS     15      /* vec4s of the pixel block after the extra ones: texture factor and fog
                                       colour, stage constants, bump matrices, luminance (ff.frag PsData) */

/* a compiled shader (gen/spirv_gen.c) */
typedef struct {
    uint64_t hash;          /* FNV-1a 64 of the Direct3D bytecode the device is given */
    uint32_t off, nwords;   /* in vk_spirv_words */
    uint8_t major, minor;
    uint8_t flags;          /* VK_SH_* */
    uint32_t inputs;        /* vertex shaders: the input locations read */
    uint32_t samplers;      /* pixel shaders: the slots read */
    uint32_t kinds;         /* 2 bits a slot: 0 any (ps_1_x), 1 2D, 2 cube, 3 volume (its dcl) */
    uint32_t proj;          /* ps_1_x slots whose tex honours D3DTTFF_PROJECTED */
    uint32_t cmax;          /* float constants read: highest + 1 (all with relative addressing) */
    uint32_t def0, ndef;    /* its def'd float constants in vk_shader_defs (copied into the buffer too,
                               so relative reads see them) */
    uint32_t noff, nnwords; /* pixel shaders: the narrow variant (0 words: none; d3d2spv.py NARROW) */
    uint32_t nkinds;        /* ... for these kinds: 4 bits a slot, bit k kind k (VK_SPEC_KINDS' values) */
    uint32_t nfilters;      /* ... and filters: bit 0 none, 1 3, 2 5, 3 7 (VK_SPEC_SHADOWF) */
} VkShaderMeta;
#define VK_SH_PIXEL         1
#define VK_SH_RELATIVE      2
#define VK_SH_DEPTH         4
#define VK_SH_KILL          8

typedef struct { uint32_t reg; uint32_t v[4]; } VkShaderDef;

extern const uint32_t vk_spirv_words[];
extern const VkShaderDef vk_shader_defs[];
extern const VkShaderMeta vk_shader_meta[];
extern const int vk_shader_count;
