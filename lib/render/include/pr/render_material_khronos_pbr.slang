// The material type of glTF's core material: base color, metallic and roughness, a normal map, occlusion and emission,
// each a factor times a texture, with the vertex's color times the base color. It computes the surface of the Khronos
// PBR model.

#pragma once

#include "pr/render_brdf.slang"
#include "pr/render_material.slang"
#include "pr/render_shading.slang"

// The attributes that glTF's materials read of a mesh's vertices: two sets of texture coordinates, TEXCOORD_0 and
// TEXCOORD_1, and the color COLOR_0, linear, which multiplies the base color and its alpha. A vertex without a color is
// white. The library's types that serve glTF's materials declare these alike, so they share one layout.
#define RENDER_KHRONOS_PBR_ATTRIBUTES                                                     \
    [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv0;                 \
    [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv1;                 \
    [RenderVertex("color", "unorm8x4", 1, 1, 1, 1)] float4 color;

struct RenderMaterialKhronosPbr : IRenderMaterial {
    typedef RenderPbrSurface Surface;

    // The core material's properties, by their names in glTF 2.0's "Materials" without their Factor, and with glTF's
    // defaults: the base color and its alpha, linear, the emission, linear, and KHR_materials_emissive_strength's
    // strength, which multiplies it. A color's fourth component is unused, but for the base color's alpha.
    struct Parameters {
        [RenderParameter(1, 1, 1, 1)] float4 baseColor;
        [RenderParameter(0, 0, 0, 0)] float4 emissive;
        [RenderParameter(1, 0, 0, 0)] float emissiveStrength;
        [RenderParameter(1, 0, 0, 0)] float metallic;
        [RenderParameter(1, 0, 0, 0)] float roughness;
        [RenderParameter(1, 0, 0, 0)] float normalScale;
        [RenderParameter(1, 0, 0, 0)] float occlusionStrength;
        float _pad[3];
    };

    // The stand-ins are a white base color, a rough dielectric (roughness 1 and metallic 0, in the channels that glTF
    // reads), the flat normal, no occlusion and no emission.
    struct Textures {
        [RenderColor(1, 1, 1, 1)] RenderTexture baseColor;
        [RenderData(1, 1, 0, 1)] [RenderReads("gb")] RenderTexture metallicRoughness;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture normal;
        [RenderData(1, 1, 1, 1)] [RenderReads("r")] RenderTexture occlusion;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture emissive;
    };

    struct Attributes {
        RENDER_KHRONOS_PBR_ATTRIBUTES
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexCoord uv[2] = {attributes.uv0, attributes.uv1};
        float4 baseColor = parameters.baseColor * attributes.color * renderMaterialSample(textures.baseColor, uv);
        float4 metallicRoughness = renderMaterialSample(textures.metallicRoughness, uv);
        float occlusion = renderMaterialSample(textures.occlusion, uv).r;

        float3 normal = renderMaterialNormal(textures.normal, parameters.normalScale, inputs, uv);
        RenderPbrSurface surface = renderPbrSurfaceDefault(normal);
        surface.baseColor = baseColor.rgb;
        surface.alpha = baseColor.a;
        surface.metallic = parameters.metallic * metallicRoughness.b;
        surface.roughness = parameters.roughness * metallicRoughness.g;
        surface.specular = 1;
        // The strength moves the occlusion from none at 0 to the texture's at 1.
        surface.occlusion = 1 + parameters.occlusionStrength * (occlusion - 1);
        surface.emissive = parameters.emissive.rgb * parameters.emissiveStrength *
            renderMaterialSample(textures.emissive, uv).rgb;
        return surface;
    }
}
