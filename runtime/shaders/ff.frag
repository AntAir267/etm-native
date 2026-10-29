// Our own Vulkan renderer (VULKAN.md 2.3): Direct3D 9's fixed-function texture stages as one
// uber-shader, compiled to SPIR-V at build time (glslangValidator, the Makefile) and specialized per
// pipeline: the stages' operations and arguments, the samplers' kinds, projection, the alpha test, fog
// and the specular add are specialization constants from the state key (runtime/vk_ff.c ff_key_ps),
// so the driver keeps only the paths a draw uses. The texture factor, stage constants and bump
// matrices are in the pixel constants block (set 1 binding 1, after the extra block; vk_ff.c
// ff_data_ps); textures and samplers are the bindless set's, their indices in the push constants as
// for the programmable pixel shaders (tools/d3d2spv.py).
//
// ALTERED SOURCE: adapted from DXVK 3.1.1's src/d3d9/shaders/d3d9_fixed_function_frag.glsl and
// d3d9_fixed_function_common.glsl (the parity reference: our pictures are compared with DXVK's), the
// arithmetic kept as DXVK writes it; changed: DXVK's spec data buffer is specialization constants
// here, the inputs are at our renderer's locations (runtime/vk_abi.h), textures and samplers come from
// the bindless set (kinds: 2D, cube, 3D, 2D depth compare; an empty stage reads zero, as DXVK's null
// type), the alpha reference and fog values come from our extra block. DXVK's notice:
//
//                  Copyright (c) 2017 Philip Rebohle
//                  Copyright (c) 2019 Joshua Ashton
//                  Copyright (c) 2019 Robin Kertels
//                  Copyright (c) 2023 Jeffrey Ellison
//
//                          zlib/libpng license
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// - The origin of this software must not be misrepresented; you must not
//   claim that you wrote the original software. If you use this software
//   in a product, an acknowledgment in the product documentation would be
//   appreciated but is not required.
//
// - Altered source versions must be plainly marked as such, and must not
//   be misrepresented as being the original software.
//
// - This notice may not be removed or altered from any source distribution.
#version 450
#extension GL_EXT_spirv_intrinsics : require
#extension GL_EXT_demote_to_helper_invocation : require
#extension GL_EXT_control_flow_attributes : require
#extension GL_EXT_nonuniform_qualifier : require

// ---------------------------------------------------------------- the state key (vk_abi.h VK_SPEC_*, VK_FFP_*)
layout(constant_id = 0) const uint specAlpha = 8u;      // D3DCMP of the alpha test, 8 (always) = off
layout(constant_id = 1) const uint specFog = 0u;        // 0 off, 1 the vertex fog, 2 exp, 3 exp2, 4 linear
layout(constant_id = 2) const uint specKinds = 0u;      // 2 bits a stage: 0 2D, 1 cube, 2 3D, 3 2D depth compare
layout(constant_id = 3) const uint specProj = 0u;       // stages whose coordinates are projected
layout(constant_id = 5) const uint stageOps01 = 0x0101u | (0x0101u << 16u);
layout(constant_id = 6) const uint stageOps23 = 0x0101u | (0x0101u << 16u);
layout(constant_id = 7) const uint stageOps45 = 0x0101u | (0x0101u << 16u);
layout(constant_id = 8) const uint stageOps67 = 0x0101u | (0x0101u << 16u);
layout(constant_id = 9) const uint stageArgs0 = 0u;
layout(constant_id = 10) const uint stageArgs1 = 0u;
layout(constant_id = 11) const uint stageArgs2 = 0u;
layout(constant_id = 12) const uint stageArgs3 = 0u;
layout(constant_id = 13) const uint stageArgs4 = 0u;
layout(constant_id = 14) const uint stageArgs5 = 0u;
layout(constant_id = 15) const uint stageArgs6 = 0u;
layout(constant_id = 16) const uint stageArgs7 = 0u;
layout(constant_id = 17) const uint ffpFlags = 0u;      // 0 specular add, 1 point sprite, 2 fog from Z; 8-15 empty stages

const bool specularOn  = (ffpFlags & 1u) != 0u;
const bool pointSprite = (ffpFlags & 2u) != 0u;
const bool fogUseZ     = (ffpFlags & 4u) != 0u;

// ---------------------------------------------------------------- inputs (vk_abi.h varyings)
layout(location = 0) in vec4 in_Color0;
layout(location = 1) in vec4 in_Color1;
layout(location = 2) in vec4 in_Texcoord0;
layout(location = 3) in vec4 in_Texcoord1;
layout(location = 4) in vec4 in_Texcoord2;
layout(location = 5) in vec4 in_Texcoord3;
layout(location = 6) in vec4 in_Texcoord4;
layout(location = 7) in vec4 in_Texcoord5;
layout(location = 8) in vec4 in_Texcoord6;
layout(location = 9) in vec4 in_Texcoord7;
layout(location = 10) in float in_Fog;

layout(location = 0) out vec4 out_Color0;

// ---------------------------------------------------------------- resources (vk_abi.h)
layout(set = 0, binding = 0) uniform texture2D t2d[];
layout(set = 0, binding = 1) uniform textureCube tcube[];
layout(set = 0, binding = 2) uniform texture3D t3d[];
layout(set = 0, binding = 3) uniform texture2D tdepth[];
layout(set = 0, binding = 4) uniform sampler samplers[];

layout(push_constant) uniform Slots { uint slot[8]; } pc;   // texture index | sampler index << 16

layout(set = 1, binding = 1, std140) uniform PsData {
    vec4 extra[8];          // VK_PS_EXTRA: [0] fog colour, density; [1] fog end, fog scale, alpha ref, alpha scale
    uvec4 Colors[3];        // [0].x the texture factor, [0].y the fog colour (D3DCOLORs); [1], [2] stage constants 0-7
    vec4 BumpEnvMat[8];     // per stage: M00, M01, M10, M11
    vec4 BumpEnvL[4];       // stages 2k, 2k+1: luminance scale, offset
} psd;

spirv_instruction(set = "GLSL.std.450", id = 79) float spvNMin(float, float);
spirv_instruction(set = "GLSL.std.450", id = 80) float spvNMax(float, float);

vec4 decodeD3DColor(uint color) { return unpackUnorm4x8(color).bgra; }

// ---------------------------------------------------------------- texture stages
const uint D3DTOP_DISABLE                   = 1;
const uint D3DTOP_SELECTARG1                = 2;
const uint D3DTOP_SELECTARG2                = 3;
const uint D3DTOP_MODULATE                  = 4;
const uint D3DTOP_MODULATE2X                = 5;
const uint D3DTOP_MODULATE4X                = 6;
const uint D3DTOP_ADD                       = 7;
const uint D3DTOP_ADDSIGNED                 = 8;
const uint D3DTOP_ADDSIGNED2X               = 9;
const uint D3DTOP_SUBTRACT                  = 10;
const uint D3DTOP_ADDSMOOTH                 = 11;
const uint D3DTOP_BLENDDIFFUSEALPHA         = 12;
const uint D3DTOP_BLENDTEXTUREALPHA         = 13;
const uint D3DTOP_BLENDFACTORALPHA          = 14;
const uint D3DTOP_BLENDTEXTUREALPHAPM       = 15;
const uint D3DTOP_BLENDCURRENTALPHA         = 16;
const uint D3DTOP_PREMODULATE               = 17;
const uint D3DTOP_MODULATEALPHA_ADDCOLOR    = 18;
const uint D3DTOP_MODULATECOLOR_ADDALPHA    = 19;
const uint D3DTOP_MODULATEINVALPHA_ADDCOLOR = 20;
const uint D3DTOP_MODULATEINVCOLOR_ADDALPHA = 21;
const uint D3DTOP_BUMPENVMAP                = 22;
const uint D3DTOP_BUMPENVMAPLUMINANCE       = 23;
const uint D3DTOP_DOTPRODUCT3               = 24;
const uint D3DTOP_MULTIPLYADD               = 25;
const uint D3DTOP_LERP                      = 26;

// These differ from the actual D3D definitions. DXVK will shift
// the modifiers one to the right since only 7 args are defined.
const uint D3DTA_SELECTMASK                 = 0x07;

const uint D3DTA_DIFFUSE                    = 0x00;
const uint D3DTA_CURRENT                    = 0x01;
const uint D3DTA_TEXTURE                    = 0x02;
const uint D3DTA_TFACTOR                    = 0x03;
const uint D3DTA_SPECULAR                   = 0x04;
const uint D3DTA_TEMP                       = 0x05;
const uint D3DTA_CONSTANT                   = 0x06;

const uint D3DTA_COMPLEMENT                 = 0x08;
const uint D3DTA_ALPHAREPLICATE             = 0x10;

struct TextureStageArguments {
    uint arg0;
    uint arg1;
    uint arg2;
};

struct TextureStage {
    bool storeToTemp;
    uint colorOp;
    uint alphaOp;
    TextureStageArguments colorArgs;
    TextureStageArguments alphaArgs;
};

TextureStage getTextureStage(uint idx) {
    uint ops = 0u;
    uint args = 0u;

    switch (idx) {
        case 0: ops = stageOps01 >>  0; args = stageArgs0; break;
        case 1: ops = stageOps01 >> 16; args = stageArgs1; break;
        case 2: ops = stageOps23 >>  0; args = stageArgs2; break;
        case 3: ops = stageOps23 >> 16; args = stageArgs3; break;
        case 4: ops = stageOps45 >>  0; args = stageArgs4; break;
        case 5: ops = stageOps45 >> 16; args = stageArgs5; break;
        case 6: ops = stageOps67 >>  0; args = stageArgs6; break;
        case 7: ops = stageOps67 >> 16; args = stageArgs7; break;
    }

    TextureStage result;
    result.storeToTemp = bitfieldExtract(ops, 15, 1) != 0;
    result.colorOp = bitfieldExtract(ops, 0, 5);
    result.alphaOp = bitfieldExtract(ops, 8, 5);
    result.colorArgs.arg0 = bitfieldExtract(args,  0, 5);
    result.colorArgs.arg1 = bitfieldExtract(args,  5, 5);
    result.colorArgs.arg2 = bitfieldExtract(args, 10, 5);
    result.alphaArgs.arg0 = bitfieldExtract(args, 16, 5);
    result.alphaArgs.arg1 = bitfieldExtract(args, 21, 5);
    result.alphaArgs.arg2 = bitfieldExtract(args, 26, 5);
    return result;
}

vec3 calculateFog(vec3 oColor) {
    vec3 fogColor = unpackUnorm4x8(psd.Colors[0].y).bgr;
    float fogScale = psd.extra[1].y;
    float fogEnd = psd.extra[1].x;
    float fogDensity = psd.extra[0].w;

    if (specFog == 0u)
        return oColor;

    float depth = fogUseZ ? gl_FragCoord.z : (1.0 / gl_FragCoord.w);
    float fogFactor;

    switch (specFog) {
        default:
            fogFactor = in_Fog;
            break;

        // (end - d) / (end - start)
        case 4u:
            fogFactor = fogEnd - depth;
            fogFactor = fogFactor * fogScale;
            break;

        // 1 / (e^[d * density])^2
        case 3u:
        // 1 / (e^[d * density])
        case 2u:
            fogFactor = depth * fogDensity;

            if (specFog == 3u)
                fogFactor *= fogFactor;

            // Provides the rcp.
            fogFactor = -fogFactor;
            fogFactor = exp(fogFactor);
            break;
    }

    // Flush nan to 1 to "disable" fog, just to be safe
    fogFactor = spvNMax(spvNMin(fogFactor, 1.0f), 0.0f);
    return mix(fogColor, oColor, fogFactor.xxx);
}

vec4 calculateBumpmapCoords(uint stage, vec4 baseCoords, vec4 previousStageTextureVal) {
    uint previousStage = stage - 1;

    // (DXVK keeps BumpEnvMat[i][j] with [0][0] = M00, [1][0] = M01, [0][1] = M10, [1][1] = M11 and
    // dots row i: u += M00 du + M10 dv, v += M01 du + M11 dv)
    vec4 m = psd.BumpEnvMat[previousStage];
    vec2 bmr[2] = vec2[2](vec2(m.x, m.z), vec2(m.y, m.w));
    vec4 coords = baseCoords;
    [[unroll]]
    for (uint i = 0; i < 2; i++) {
        float tc_m_n = coords[i];
        vec2 bm = bmr[i];
        vec2 t = previousStageTextureVal.xy;
        float result = tc_m_n + dot(bm, t);
        coords[i] = result;
    }
    return coords;
}

vec4 sampleTexture(uint stage, vec4 texcoord, vec4 previousStageTextureVal) {
    bool projected = ((specProj >> stage) & 1u) != 0u;
    uint kind = (specKinds >> (2u * stage)) & 3u;
    bool empty = ((ffpFlags >> (8u + stage)) & 1u) != 0u;

    if (projected)
        texcoord /= texcoord.w;

    uint previousStageColorOp = 0;

    if (stage > 0) {
        uint previousStageColorOp = getTextureStage(stage - 1u).colorOp;

        if (previousStageColorOp == D3DTOP_BUMPENVMAP || previousStageColorOp == D3DTOP_BUMPENVMAPLUMINANCE)
            texcoord = calculateBumpmapCoords(stage, texcoord, previousStageTextureVal);
    }

    // We can encounter null textures in certain scenarios where only alpha
    // samples the texture, or the color op itself implicitly samples. For
    // some reason this appears to return a null vector on native.
    vec4 texVal = vec4(0.0f);

    uint s = pc.slot[stage];
    uint ti = s & 0xffffu, si = s >> 16u;
    if (!empty) {
        switch (kind) {
            case 0u:
                texVal = texture(sampler2D(t2d[ti], samplers[si]), texcoord.xy);
                break;
            case 3u:
                texVal = texture(sampler2DShadow(tdepth[ti], samplers[si]), texcoord.xyz).xxxx;
                break;
            case 1u:
                texVal = texture(samplerCube(tcube[ti], samplers[si]), texcoord.xyz);
                break;
            case 2u:
                texVal = texture(sampler3D(t3d[ti], samplers[si]), texcoord.xyz);
                break;
        }
    }

    if (stage != 0 && previousStageColorOp == D3DTOP_BUMPENVMAPLUMINANCE) {
        vec4 l = psd.BumpEnvL[(stage - 1u) / 2u];
        float lScale = ((stage - 1u) & 1u) != 0u ? l.z : l.x;
        float lOffset = ((stage - 1u) & 1u) != 0u ? l.w : l.y;
        float scale = texVal.z;
        scale *= lScale;
        scale += lOffset;
        scale = clamp(scale, 0.0, 1.0);
        texVal *= scale;
    }

    return texVal;
}

vec4 readArgValue(uint stage, uint arg, vec4 current, vec4 temp, vec4 textureVal, bool premodulate) {
    vec4 reg = vec4(1.0);
    switch (arg & D3DTA_SELECTMASK) {
        case D3DTA_CONSTANT:
            reg = decodeD3DColor(psd.Colors[1u + stage / 4u][stage % 4u]);
            break;
        case D3DTA_CURRENT:
            reg = mix(current, current * textureVal, premodulate.xxxx);
            break;
        case D3DTA_DIFFUSE:
            reg = in_Color0;
            break;
        case D3DTA_SPECULAR:
            reg = in_Color1;
            break;
        case D3DTA_TEMP:
            reg = temp;
            break;
        case D3DTA_TEXTURE:
            reg = textureVal;
            break;
        case D3DTA_TFACTOR:
            reg = decodeD3DColor(psd.Colors[0].x);
            break;
    }

    // reg = 1 - reg
    if ((arg & D3DTA_COMPLEMENT) != 0)
        reg = vec4(1.0) - reg;

    // reg = reg.wwww
    if ((arg & D3DTA_ALPHAREPLICATE) != 0)
        reg = reg.aaaa;

    return reg;
}

struct TextureStageArgumentValues {
    vec4 arg0;
    vec4 arg1;
    vec4 arg2;
};

TextureStageArgumentValues readArgValues(uint stage, const TextureStageArguments args, vec4 current, vec4 temp, vec4 textureVal, bool premodulate) {
    TextureStageArgumentValues argVals;
    argVals.arg0 = readArgValue(stage, args.arg0, current, temp, textureVal, premodulate);
    argVals.arg1 = readArgValue(stage, args.arg1, current, temp, textureVal, premodulate);
    argVals.arg2 = readArgValue(stage, args.arg2, current, temp, textureVal, premodulate);
    return argVals;
}

vec4 complement(vec4 val) {
    return vec4(1.0) - val;
}

vec4 saturate(vec4 val) {
    return clamp(val, vec4(0.0), vec4(1.0));
}

vec4 calculateTextureStage(uint op, vec4 dst, const TextureStageArgumentValues arg, vec4 current, vec4 textureVal) {
    switch (op) {
        case D3DTOP_SELECTARG1:
            return arg.arg1;

        case D3DTOP_SELECTARG2:
            return arg.arg2;

        case D3DTOP_MODULATE4X:
            return saturate(arg.arg1 * arg.arg2 * 4.0);

        case D3DTOP_MODULATE2X:
            return saturate(arg.arg1 * arg.arg2 * 2.0);

        case D3DTOP_MODULATE:
            return saturate(arg.arg1 * arg.arg2);

        case D3DTOP_ADDSIGNED2X:
            return saturate(2.0 * (arg.arg1 + (arg.arg2 - vec4(0.5))));

        case D3DTOP_ADDSIGNED:
            return saturate(arg.arg1 + (arg.arg2 - vec4(0.5)));

        case D3DTOP_ADD:
            return saturate(arg.arg1 + arg.arg2);

        case D3DTOP_SUBTRACT:
            return saturate(arg.arg1 - arg.arg2);

        case D3DTOP_ADDSMOOTH:
            return saturate(fma(complement(arg.arg1), arg.arg2, arg.arg1));

        case D3DTOP_BLENDDIFFUSEALPHA:
            return mix(arg.arg2, arg.arg1, in_Color0.aaaa);

        case D3DTOP_BLENDTEXTUREALPHA:
            return mix(arg.arg2, arg.arg1, textureVal.aaaa);

        case D3DTOP_BLENDFACTORALPHA:
            return mix(arg.arg2, arg.arg1, decodeD3DColor(psd.Colors[0].x).aaaa);

        case D3DTOP_BLENDTEXTUREALPHAPM:
            return saturate(fma(arg.arg2, complement(textureVal.aaaa), arg.arg1));

        case D3DTOP_BLENDCURRENTALPHA:
            return mix(arg.arg2, arg.arg1, current.aaaa);

        case D3DTOP_PREMODULATE:
            return arg.arg1;

        case D3DTOP_MODULATEALPHA_ADDCOLOR:
            return saturate(fma(arg.arg1.aaaa, arg.arg2, arg.arg1));

        case D3DTOP_MODULATECOLOR_ADDALPHA:
            return saturate(fma(arg.arg1, arg.arg2, arg.arg1.aaaa));

        case D3DTOP_MODULATEINVALPHA_ADDCOLOR:
            return saturate(fma(complement(arg.arg1.aaaa), arg.arg2, arg.arg1));

        case D3DTOP_MODULATEINVCOLOR_ADDALPHA:
            return saturate(fma(complement(arg.arg1), arg.arg2, arg.arg1.aaaa));

        case D3DTOP_BUMPENVMAPLUMINANCE:
        case D3DTOP_BUMPENVMAP:
            // Load texture for the next stage...
            return dst;

        case D3DTOP_DOTPRODUCT3:
            return saturate(vec4(dot(arg.arg1.rgb - vec3(0.5), arg.arg2.rgb - vec3(0.5)) * 4.0));

        case D3DTOP_MULTIPLYADD:
            return saturate(fma(arg.arg1, arg.arg2, arg.arg0));

        case D3DTOP_LERP:
            return mix(arg.arg2, arg.arg1, arg.arg0);

        default:
            // Unhandled texture op!
            return dst;

    }

    return vec4(0.0);
}

// the alpha test as DXVK's for 8-bit targets (extra[1].w 255: compared in 8 bits, the alpha rounded to
// nearest even) and float ones (extra[1].w 0) [16-bit targets: as float, DXVK compares in 16 bits]
bool alphaTest(float alpha) {
    if (specAlpha < 8u) {
        float alphaRef = psd.extra[1].z;
        float scale = psd.extra[1].w;
        if (scale > 0.0)
            alpha = roundEven(alpha * scale);

        switch (specAlpha) {
            case 2u: return alpha <  alphaRef;
            case 3u: return alpha == alphaRef;
            case 4u: return alpha <= alphaRef;
            case 5u: return alpha >  alphaRef;
            case 6u: return alpha != alphaRef;
            case 7u: return alpha >= alphaRef;
            default: return false;  // NEVER
        }
    }

    return true;
}

struct TextureStageState {
    vec4 current;
    vec4 temp;
    vec4 previousStageTextureVal;
    bool premodulateColor;
    bool premodulateAlpha;
};

vec4 getTexCoord(uint stage) {
    // If point sprites are enabled, we need to replace the
    // input texture coordinate with the point coordinate
    if (pointSprite)
        return vec4(gl_PointCoord, 0.0f, 0.0f);

    switch (stage) {
        case 0: return in_Texcoord0;
        case 1: return in_Texcoord1;
        case 2: return in_Texcoord2;
        case 3: return in_Texcoord3;
        case 4: return in_Texcoord4;
        case 5: return in_Texcoord5;
        case 6: return in_Texcoord6;
        case 7: return in_Texcoord7;
    }

    return vec4(0.0f);
}

TextureStageState runTextureStage(uint stage, TextureStageState state) {
    TextureStage info = getTextureStage(stage);

    // This cancels all subsequent stages.
    if (info.colorOp == D3DTOP_DISABLE)
        return state;

    vec4 prev = info.storeToTemp ? state.temp : state.current;
    vec4 next = prev;

    vec4 textureVal = sampleTexture(stage, getTexCoord(stage), state.previousStageTextureVal);

    // Fast path if alpha/color path is identical.
    // D3DTOP_DOTPRODUCT3 also has special quirky behaviour here.
    bool fastPath = info.colorOp == info.alphaOp &&
        info.colorArgs.arg0 == info.alphaArgs.arg0 &&
        info.colorArgs.arg1 == info.alphaArgs.arg1 &&
        info.colorArgs.arg2 == info.alphaArgs.arg2 &&
        state.premodulateColor == state.premodulateAlpha;

    if (fastPath || info.colorOp == D3DTOP_DOTPRODUCT3) {
        TextureStageArgumentValues colorArgVals = readArgValues(stage, info.colorArgs, state.current, state.temp, textureVal, state.premodulateColor);
        next = calculateTextureStage(info.colorOp, prev, colorArgVals, state.current, textureVal);
    } else {
        TextureStageArgumentValues colorArgVals = readArgValues(stage, info.colorArgs, state.current, state.temp, textureVal, state.premodulateColor);
        next.rgb = calculateTextureStage(info.colorOp, prev, colorArgVals, state.current, textureVal).rgb;

        if (info.alphaOp != D3DTOP_DISABLE) {
            TextureStageArgumentValues alphaArgVals = readArgValues(stage, info.alphaArgs, state.current, state.temp, textureVal, state.premodulateAlpha);
            next.a = calculateTextureStage(info.alphaOp, prev, alphaArgVals, state.current, textureVal).a;
        }
    }

    state.temp = info.storeToTemp ? next : state.temp;
    state.current = info.storeToTemp ? state.current : next;
    state.previousStageTextureVal = textureVal;
    state.premodulateColor = info.colorOp == D3DTOP_PREMODULATE;
    state.premodulateAlpha = info.alphaOp == D3DTOP_PREMODULATE;
    return state;
}

void main() {
    // in_Color0 is diffuse
    // in_Color1 is specular

    TextureStageState state;
    // Current starts of as equal to diffuse.
    state.current = in_Color0;
    // Temp starts off as equal to vec4(0)
    state.temp = vec4(0.0);
    state.previousStageTextureVal = vec4(0.0);
    state.premodulateColor = false;
    state.premodulateAlpha = false;

    // If we turn this into a loop, performance becomes very poor on the proprietary Nvidia driver
    // because it fails to unroll it.
    state = runTextureStage(0, state);
    state = runTextureStage(1, state);
    state = runTextureStage(2, state);
    state = runTextureStage(3, state);
    state = runTextureStage(4, state);
    state = runTextureStage(5, state);
    state = runTextureStage(6, state);
    state = runTextureStage(7, state);

    if (specularOn)
        state.current.xyz += in_Color1.xyz;

    state.current.xyz = calculateFog(state.current.xyz);

    out_Color0 = state.current;

    if (!alphaTest(state.current.a))
        demote;
}
