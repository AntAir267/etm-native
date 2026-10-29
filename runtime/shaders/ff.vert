// Our own Vulkan renderer (VULKAN.md 2.3): Direct3D 9's fixed-function vertex processing as one
// uber-shader, compiled to SPIR-V at build time (glslangValidator, the Makefile) and specialized per
// pipeline: every path is chosen by specialization constants from the state key (runtime/vk_d3d9.c
// ff_key_vs), so the driver drops what a draw does not use. The per-draw values (matrices, lights,
// material, viewport, point size) are a block in the frame's constants ring (set 1 binding 0, the
// vertex shader constants' first chunk; vk_d3d9.c ff_data_vs).
//
// ALTERED SOURCE: adapted from DXVK 3.1.1's src/d3d9/shaders/d3d9_fixed_function_vert.vert and
// d3d9_fixed_function_common.glsl (the parity reference: our pictures are compared with DXVK's), the
// arithmetic kept as DXVK writes it; changed: the state DXVK reads from its data buffer and spec data
// buffer is specialization constants here, the inputs and outputs are at our renderer's locations
// (runtime/vk_abi.h), the per-draw data is one std140 block, vertex blending and clip planes are left
// out (Enter the Matrix uses neither: VULKAN.md 1), the normal output goes (no fixed-function pixel
// shader reads it). DXVK's notice:
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

// ---------------------------------------------------------------- the state key (vk_abi.h VK_FFV_*)
layout(constant_id = 0) const uint ffFlags = 0u;      // VK_FFV_FLAGS
layout(constant_id = 1) const uint ffLights = 0u;     // 2 bits a light: 0 none, 1 point, 2 spot, 3 directional
layout(constant_id = 2) const uint ffTci = 0u;        // 3 bits a stage: its texture coordinate set
layout(constant_id = 3) const uint ffTciFlags = 0u;   // 3 bits a stage: D3DTSS_TCI_* >> 16
layout(constant_id = 4) const uint ffTtff = 0u;       // 3 bits a stage: D3DTTFF_COUNT* (PROJECTED apart)
layout(constant_id = 5) const uint ffTcMask = 0u;     // 3 bits a coordinate set: its components in the declaration
layout(constant_id = 6) const uint ffMisc = 0u;       // bits 0-7 projected stages; 8-9 fog vertex mode

const bool hasPositionT  = (ffFlags & 0x1u) != 0u;
const bool hasColor0     = (ffFlags & 0x2u) != 0u;
const bool hasColor1     = (ffFlags & 0x4u) != 0u;
const bool hasPointSize  = (ffFlags & 0x8u) != 0u;
const bool hasFog        = (ffFlags & 0x10u) != 0u;
const bool useLighting   = (ffFlags & 0x20u) != 0u;
const bool normalizeNrm  = (ffFlags & 0x40u) != 0u;
const bool localViewer   = (ffFlags & 0x80u) != 0u;
const bool rangeFog      = (ffFlags & 0x100u) != 0u;
const bool specularOn    = (ffFlags & 0x200u) != 0u;
const bool pointScaleOn  = (ffFlags & 0x400u) != 0u;
const bool tween         = (ffFlags & 0x800u) != 0u;
const bool fogOn         = (ffFlags & 0x1000u) != 0u;
const bool psIsSM3       = (ffFlags & 0x2000u) != 0u;
const bool points        = (ffFlags & 0x4000u) != 0u;
const uint diffuseSource  = (ffFlags >> 16u) & 3u;
const uint ambientSource  = (ffFlags >> 18u) & 3u;
const uint specularSource = (ffFlags >> 20u) & 3u;
const uint emissiveSource = (ffFlags >> 22u) & 3u;
const uint fogVertexMode  = (ffMisc >> 8u) & 3u;

// ---------------------------------------------------------------- inputs (vk_abi.h vk_input_loc)
layout(location = 0) in vec4 in_Position0;
layout(location = 1) in vec4 in_BlendWeight;
layout(location = 2) in vec4 in_BlendIndices;
layout(location = 3) in vec4 in_Normal0;
layout(location = 4) in float in_PointSize;
layout(location = 5) in vec4 in_Texcoord0;
layout(location = 6) in vec4 in_Texcoord1;
layout(location = 7) in vec4 in_Texcoord2;
layout(location = 8) in vec4 in_Texcoord3;
layout(location = 9) in vec4 in_Texcoord4;
layout(location = 10) in vec4 in_Texcoord5;
layout(location = 11) in vec4 in_Texcoord6;
layout(location = 12) in vec4 in_Texcoord7;
layout(location = 15) in vec4 in_Color0;
layout(location = 16) in vec4 in_Color1;
layout(location = 17) in float in_Fog;
layout(location = 19) in vec4 in_Position1;
layout(location = 20) in vec4 in_Normal1;

invariant gl_Position;

// ---------------------------------------------------------------- outputs (vk_abi.h varyings)
layout(location = 0) out vec4 out_Color0;
layout(location = 1) out vec4 out_Color1;
layout(location = 2) out vec4 out_Texcoord0;
layout(location = 3) out vec4 out_Texcoord1;
layout(location = 4) out vec4 out_Texcoord2;
layout(location = 5) out vec4 out_Texcoord3;
layout(location = 6) out vec4 out_Texcoord4;
layout(location = 7) out vec4 out_Texcoord5;
layout(location = 8) out vec4 out_Texcoord6;
layout(location = 9) out vec4 out_Texcoord7;
layout(location = 10) out float out_Fog;

const float FloatMaxValue = 340282346638528859811704183484516925440.0;
const uint D3DFOG_NONE = 0u, D3DFOG_EXP = 1u, D3DFOG_EXP2 = 2u, D3DFOG_LINEAR = 3u;
const uint D3DLIGHT_POINT = 1u, D3DLIGHT_SPOT = 2u, D3DLIGHT_DIRECTIONAL = 3u;
const uint D3DMCS_MATERIAL = 0u, D3DMCS_COLOR1 = 1u, D3DMCS_COLOR2 = 2u;
const uint D3DTTFF_COUNT1 = 1u, D3DTTFF_COUNT4 = 4u;
const uint DXVK_TSS_TCI_PASSTHRU = 0u, DXVK_TSS_TCI_CAMERASPACENORMAL = 1u, DXVK_TSS_TCI_CAMERASPACEPOSITION = 2u,
           DXVK_TSS_TCI_CAMERASPACEREFLECTIONVECTOR = 3u, DXVK_TSS_TCI_SPHEREMAP = 4u;

spirv_instruction(set = "GLSL.std.450", id = 79) float spvNMin(float, float);
spirv_instruction(set = "GLSL.std.450", id = 81) float spvNClamp(float, float, float);

// ---------------------------------------------------------------- the per-draw block (vk_d3d9.c FFVsData)
struct Light {
    vec4 Diffuse;
    vec4 Specular;
    vec4 Ambient;
    vec4 Position;      // in view space
    vec4 Direction;     // in view space, normalized
    vec4 RangeFalloffAtt01;     // range, falloff, attenuation 0, attenuation 1
    vec4 Att2ThetaPhi;          // attenuation 2, cos(theta / 2), cos(phi / 2), -
};

layout(set = 1, binding = 0, std140, row_major) uniform FFVsData {
    mat4 WorldView;
    mat4 NormalMatrix;
    mat4 Projection;
    mat4 WorldViewProj;
    mat4 TexcoordMatrices[8];
    vec4 ViewportInverseOffset;
    vec4 ViewportInverseExtent;
    Light Lights[8];
    vec4 MatDiffuse;
    vec4 MatAmbient;
    vec4 MatSpecular;
    vec4 MatEmissive;
    uvec4 GlobalAmbient;        // x: D3DRS_AMBIENT (a D3DCOLOR, decoded here as DXVK does)
    vec4 PowerTweenFog;         // material power, tween factor, fog end, fog scale
    vec4 FogDensityPoint;       // fog density, point size, point size min, point size max
    vec4 PointScale;            // A, B, C (already over the viewport's height squared, as DXVK)
} ffd[8];                       // (the vertex constants' chunk array: the block is in chunk 0)

#define data ffd[0]

uint lightType(uint i) { return (ffLights >> (2u * i)) & 3u; }
uint texcoordIndex(uint i) { return (ffTci >> (3u * i)) & 7u; }
uint texcoordFlags(uint i) { return (ffTciFlags >> (3u * i)) & 7u; }
uint texcoordTransformFlags(uint i) { return (ffTtff >> (3u * i)) & 7u; }
bool isSamplerProjected(uint i) { return ((ffMisc >> i) & 1u) != 0u; }

vec4 decodeD3DColor(uint color) { return unpackUnorm4x8(color).bgra; }

float calculateFog(vec4 vPos) {
    vec4 specular = in_Color1;
    bool hasSpecular = hasColor1;

    float fogScale = data.PowerTweenFog.w;
    float fogEnd = data.PowerTweenFog.z;
    float fogDensity = data.FogDensityPoint.x;

    if (!fogOn)
        return 0.0;

    float w = vPos.w;
    float z = vPos.z;
    float depth;
    if (rangeFog) {
        vec3 pos3 = vPos.xyz;
        depth = length(pos3);
    } else {
        depth = hasFog ? in_Fog : abs(z);
    }
    float fogFactor;
    if (hasPositionT) {
        fogFactor = hasSpecular ? specular.w : 1.0;
    } else {
        switch (fogVertexMode) {
            default:
            case D3DFOG_NONE:
                fogFactor = hasSpecular ? specular.w : 1.0;
                break;

            // (end - d) / (end - start)
            case D3DFOG_LINEAR:
                fogFactor = fogEnd - depth;
                fogFactor = fogFactor * fogScale;
                fogFactor = spvNClamp(fogFactor, 0.0, 1.0);
                break;

            // 1 / (e^[d * density])^2
            case D3DFOG_EXP2:
            // 1 / (e^[d * density])
            case D3DFOG_EXP:
                fogFactor = depth * fogDensity;

                if (fogVertexMode == D3DFOG_EXP2)
                    fogFactor *= fogFactor;

                // Provides the rcp.
                fogFactor = -fogFactor;
                fogFactor = exp(fogFactor);
                break;
        }
    }

    return fogFactor;
}

float calculatePointSize(vec4 vtx) {
    vec3 pointSizeArgs = data.FogDensityPoint.yzw;

    float value = hasPointSize ? in_PointSize : pointSizeArgs.x;

    float scaleC = data.PointScale.z;
    float scaleB = data.PointScale.y;
    float scaleA = data.PointScale.x;

    vec3 vtx3 = vtx.xyz;

    float DeSqr = dot(vtx3, vtx3);
    float De    = sqrt(DeSqr);
    float scaleValue = scaleC * DeSqr;
    scaleValue = fma(scaleB, De, scaleValue);
    scaleValue += scaleA;
    scaleValue = sqrt(scaleValue);
    scaleValue = value / scaleValue;

    value = pointScaleOn ? scaleValue : value;

    float pointSizeMin = pointSizeArgs.y;
    float pointSizeMax = pointSizeArgs.z;

    return clamp(value, pointSizeMin, pointSizeMax);
}

// Precise dot product and matrix-vector product helpers. This needs to match
// programmable shaders since some games rely on fixed-function and programmable
// VS producing identical vertex positions (e.g. Railroad Tycoon 3).
precise float dp4(vec4 a, vec4 b) {
    return fma(a.w, b.w, fma(a.z, b.z, fma(a.y, b.y, a.x * b.x)));
}

precise vec4 vecTimesMat4(vec4 a, mat4 b) {
    return vec4(dp4(a, b[0]), dp4(a, b[1]),
                dp4(a, b[2]), dp4(a, b[3]));
}

float mul_legacy(float a, float b) {
    return (b == 0.0f ? 0.0f : a) * (a == 0.0f ? 0.0f : b);
}

vec4 pickMaterialSource(uint source, vec4 material) {
    if (source == D3DMCS_COLOR1 && hasColor0)
        return in_Color0;
    else if (source == D3DMCS_COLOR2 && hasColor1)
        return in_Color1;
    else
        return material;
}

struct Vertex {
    vec4 coord;
    vec4 transformed;
    vec3 normal;
};

Vertex transformVertex() {
    Vertex result;
    result.coord = in_Position0;
    result.normal = in_Normal0.xyz;

    if (tween) {
        result.coord = mix(result.coord, in_Position1, data.PowerTweenFog.y);
        result.normal = mix(result.normal, in_Normal1.xyz, data.PowerTweenFog.y);
    }

    if (!hasPositionT) {
        // Apply pre-multiplied world-view-projection matrix, Railroad Tycoon 3
        // relies on this and will break if we apply matrices one by one.
        result.transformed = vecTimesMat4(result.coord, data.WorldViewProj);
        result.coord = vecTimesMat4(result.coord, data.WorldView);
        result.normal = mat3(data.NormalMatrix) * result.normal;

        if (normalizeNrm) {
            float normalScale = inversesqrt(dot(result.normal, result.normal));
            result.normal.x = mul_legacy(result.normal.x, normalScale);
            result.normal.y = mul_legacy(result.normal.y, normalScale);
            result.normal.z = mul_legacy(result.normal.z, normalScale);
        }

        return result;
    } else {
        // We still need to account for perspective correction here...
        result.transformed = fma(result.coord, data.ViewportInverseExtent, data.ViewportInverseOffset);

        float rhw = result.transformed.w == 0.0 ? 1.0 : 1.0 / result.transformed.w;
        result.transformed.xyz *= rhw;
        result.transformed.w = rhw;
        return result;
    }
}

vec4 loadTexcoord(uint idx) {
    vec4 result = vec4(0.0f);
    result = mix(result, in_Texcoord0, bvec4(idx == 0u));
    result = mix(result, in_Texcoord1, bvec4(idx == 1u));
    result = mix(result, in_Texcoord2, bvec4(idx == 2u));
    result = mix(result, in_Texcoord3, bvec4(idx == 3u));
    result = mix(result, in_Texcoord4, bvec4(idx == 4u));
    result = mix(result, in_Texcoord5, bvec4(idx == 5u));
    result = mix(result, in_Texcoord6, bvec4(idx == 6u));
    result = mix(result, in_Texcoord7, bvec4(idx == 7u));
    return result;
}

float vectorExtract(vec4 vector, uint idx) {
    float result = vector.x;
    result = mix(result, vector.y, idx == 1u);
    result = mix(result, vector.z, idx == 2u);
    result = mix(result, vector.w, idx == 3u);
    return result;
}

vec4 transformTexCoord(uint idx, vec4 vertex, vec3 normal) {
    uint inputIndex = texcoordIndex(idx);
    uint inputFlags = texcoordFlags(idx);
    uint texcoordCount = (ffTcMask >> (inputIndex * 3u)) & 7u;

    vec4 transformed = vec4(0.0f);

    uint flags = texcoordTransformFlags(idx);

    // Passing 0xffffffff results in it getting clamped to the dimensions of the texture coords and getting treated as PROJECTED
    // but D3D9 does not apply the transformation matrix.
    bool applyTransform = flags > D3DTTFF_COUNT1 && flags <= D3DTTFF_COUNT4;

    uint count = min(flags, 4u);

    // A projection component index of 4 means we won't do projection
    uint projIndex = count != 0u ? count - 1u : 4u;

    switch (inputFlags) {
        default:
        case DXVK_TSS_TCI_PASSTHRU:
            transformed = loadTexcoord(inputIndex & 0xffu);

            if (texcoordCount < 4u) {
                // Vulkan sets the w component to 1.0 if that's not provided by the vertex buffer, D3D9 expects 0 here
                transformed.w = 0.0;
            }

            if (applyTransform && !hasPositionT) {
                // The first component after the last one thats backed by a vertex buffer gets padded to 1 for some reason.
                if (texcoordCount >= 1u)
                    transformed = mix(transformed, vec4(1.0f), equal(uvec4(0u, 1u, 2u, 3u), texcoordCount.xxxx));
            } else if (texcoordCount != 0u && !applyTransform) {
                // COUNT0, COUNT1, COUNT > 4 => take count from vertex decl if that's not zero
                count = texcoordCount;
            }

            projIndex = count != 0u ? count - 1u : 4u;
            break;

        case DXVK_TSS_TCI_CAMERASPACENORMAL:
            transformed = vec4(normal, 1.0f);

            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }
            break;

        case DXVK_TSS_TCI_CAMERASPACEPOSITION:
            transformed = vertex;
            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }
            break;

        case DXVK_TSS_TCI_CAMERASPACEREFLECTIONVECTOR: {
            vec3 reflection = reflect(normalize(vertex.xyz), normal);
            transformed = vec4(reflection, 1.0f);

            if (!applyTransform) {
                count = 3u;
                projIndex = 4u;
            }

            break;
        }

        case DXVK_TSS_TCI_SPHEREMAP: {
            vec3 reflection = reflect(normalize(vertex.xyz), normal);

            float m = length(reflection + vec3(0.0f, 0.0f, 1.0f)) * 2.0f;
            transformed = vec4(reflection.xy / m + 0.5f, 0.0f, 1.0f);
            break;
        }
    }

    if (applyTransform && !hasPositionT)
        transformed = transformed * data.TexcoordMatrices[idx];

    // Discard the components that exceed the specified D3DTTFF_COUNT
    vec4 result = mix(transformed, vec4(0.0f), lessThan(count.xxxx, uvec4(1u, 2u, 3u, 4u)));

    if (isSamplerProjected(idx) && projIndex < 4u) {
        // The projection idx is always based on the flags, even when the input
        // mode is not DXVK_TSS_TCI_PASSTHRU. The w component is only used for
        // projection or unused, so always insert the divisor there. The pixel
        // shader will then decide whether to project or not.
        result.w = vectorExtract(transformed, projIndex);
    }

    return result;
}

struct Lighting {
    vec4 diffuse;
    vec4 specular;
};

Lighting computeLighting(vec4 vertex, vec3 normal) {
    Lighting result;
    result.diffuse = hasColor0 ? in_Color0 : vec4(1.0, 1.0, 1.0, 1.0);
    result.specular = hasColor1 ? in_Color1 : vec4(0.0, 0.0, 0.0, psIsSM3 ? 0.0 : 1.0);

    if (useLighting) {
        vec4 diffuseValue = vec4(0.0);
        vec4 specularValue = vec4(0.0);
        vec4 ambientValue = vec4(0.0);

        for (uint i = 0; i < 8u; i++) {
            if (lightType(i) == 0u)
                break;
            Light light = data.Lights[i];
            uint lType = lightType(i);

            vec3 delta = light.Position.xyz - vertex.xyz;
            float dist = length(delta);

            // Directional light properties
            vec3 hitDir = -light.Direction.xyz;
            float atten = 1.0f;

            if (lType != D3DLIGHT_DIRECTIONAL) {
                // Range-based attenuation
                atten = fma(dist, light.Att2ThetaPhi.x, light.RangeFalloffAtt01.w);
                atten = fma(dist, atten, light.RangeFalloffAtt01.z);
                atten = 1.0 / atten;
                atten = spvNMin(atten, FloatMaxValue);
                atten = dist > light.RangeFalloffAtt01.x ? 0.0 : atten;

                hitDir = normalize(delta);
            }

            if (lType == D3DLIGHT_SPOT) {
                // Angle-based attenuation
                float rho = dot(-hitDir, light.Direction.xyz);
                float spotAtten = rho - light.Att2ThetaPhi.z;
                      spotAtten = spotAtten / (light.Att2ThetaPhi.y - light.Att2ThetaPhi.z);
                      spotAtten = pow(spotAtten, light.RangeFalloffAtt01.y);

                bool insideThetaAndPhi = rho <= light.Att2ThetaPhi.y;
                bool insidePhi = rho > light.Att2ThetaPhi.z;
                     spotAtten = insidePhi ? spotAtten : 0.0;
                     spotAtten = insideThetaAndPhi ? spotAtten : 1.0;
                     spotAtten = clamp(spotAtten, 0.0, 1.0);

                atten *= spotAtten;
            }

            // Ambient + Diffuse
            float hitDot = dot(normal, hitDir);
                  hitDot = clamp(hitDot, 0.0, 1.0);

            float diffuseness = hitDot * atten;
            ambientValue += light.Ambient * atten;
            diffuseValue += light.Diffuse * diffuseness;

            // Specular
            vec3 mid;

            if (localViewer) {
                mid = normalize(vertex.xyz);
                mid = hitDir - mid;
            } else {
                mid = hitDir - vec3(0.0, 0.0, 1.0);
            }

            float midDot = dot(normal, normalize(mid));
                  midDot = clamp(midDot, 0.0, 1.0);

            if (midDot > 0.0 && hitDot > 0.0) {
                float specularness = pow(midDot, data.PowerTweenFog.x) * atten;
                specularValue += light.Specular * specularness;
            }
        }

        vec4 matDiffuse  = pickMaterialSource(diffuseSource, data.MatDiffuse);
        vec4 matAmbient  = pickMaterialSource(ambientSource, data.MatAmbient);
        vec4 matEmissive = pickMaterialSource(emissiveSource, data.MatEmissive);
        vec4 matSpecular = pickMaterialSource(specularSource, data.MatSpecular);

        vec4 finalColor0 = fma(matAmbient, decodeD3DColor(data.GlobalAmbient.x), matEmissive);
             finalColor0 = fma(matAmbient, ambientValue, finalColor0);
             finalColor0 = fma(matDiffuse, diffuseValue, finalColor0);
             finalColor0.a = matDiffuse.a;

        vec4 finalColor1 = matSpecular * specularValue;

        // Saturate
        finalColor0 = clamp(finalColor0, vec4(0.0), vec4(1.0));
        finalColor1 = clamp(finalColor1, vec4(0.0), vec4(1.0));

        result.diffuse = finalColor0;

        if (specularOn)
            result.specular = finalColor1;
    }

    return result;
}

void main() {
    Vertex vtx = transformVertex();

    gl_Position = vtx.transformed;
    gl_PointSize = points ? calculatePointSize(vtx.coord) : 1.0;

    out_Texcoord0 = transformTexCoord(0u, vtx.coord, vtx.normal);
    out_Texcoord1 = transformTexCoord(1u, vtx.coord, vtx.normal);
    out_Texcoord2 = transformTexCoord(2u, vtx.coord, vtx.normal);
    out_Texcoord3 = transformTexCoord(3u, vtx.coord, vtx.normal);
    out_Texcoord4 = transformTexCoord(4u, vtx.coord, vtx.normal);
    out_Texcoord5 = transformTexCoord(5u, vtx.coord, vtx.normal);
    out_Texcoord6 = transformTexCoord(6u, vtx.coord, vtx.normal);
    out_Texcoord7 = transformTexCoord(7u, vtx.coord, vtx.normal);

    Lighting lighting = computeLighting(vtx.coord, vtx.normal);
    out_Color0 = lighting.diffuse;
    out_Color1 = lighting.specular;

    out_Fog = calculateFog(vtx.coord);
}
