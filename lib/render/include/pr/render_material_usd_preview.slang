// The material type of OpenUSD's preview surface, as the "UsdPreviewSurface Specification" defines its inputs: a
// diffuse color under a specular reflection of a roughness, of a metal by the metallic input and of a dielectric by its
// index of refraction, or in the specular workflow of a specular color, with a clear coat, emission, opacity, occlusion
// and a normal map. It computes the surface of the Khronos PBR model (pr/render_brdf.slang).
//
// Each input reads a texture of its own, in the channel that its network reads, scaled and biased as UsdUVTexture
// scales it (PrRenderMaterialTextureRecord), so a network that reads roughness and metallic from two images, or one
// channel of an image of four, draws as it is written. A normal texture's value is a tangent-space normal after its
// scale and bias, which a network gives as 2 and -1 for a texture that maps [-1, 1] to [0, 1].
//
// The diffuse color and the opacity are times the vertex's color and its alpha, which is white where a mesh has none: a
// primitive variable that a network reads into either, such as displayColor, is a value of that attribute. The emissive
// color is times the vertex's optional attribute of that name, which a mesh carries only where a material binds it and
// which is white otherwise, so a primitive variable that a network reads into it is a value of that attribute.

#pragma once

#include "pr/render_brdf.slang"
#include "pr/render_material.slang"
#include "pr/render_material_khronos_pbr.slang"
#include "pr/render_shading.slang"

struct RenderMaterialUsdPreview : IRenderMaterial {
    typedef RenderPbrSurface Surface;

    // The values of the inputs, by the specification's names, with its fallbacks. Colors are linear, and their fourth
    // components are unused. `useSpecularWorkflow` is 1 for the specular workflow and 0 for the metallic. An input that
    // reads a texture takes the texture's value, through the texture's own scale and bias
    // (PrRenderMaterialTextureRecord), in place of its value here.
    struct Parameters {
        [RenderParameter(0.18, 0.18, 0.18, 0)] float4 diffuseColor;
        [RenderParameter(0, 0, 0, 0)] float4 emissiveColor;
        [RenderParameter(0, 0, 0, 0)] float4 specularColor;
        [RenderParameter(1, 0, 0, 0)] float opacity;
        [RenderParameter(1.5, 0, 0, 0)] float ior;
        [RenderParameter(0, 0, 0, 0)] float useSpecularWorkflow;
        [RenderParameter(0, 0, 0, 0)] float metallic;
        [RenderParameter(0.5, 0, 0, 0)] float roughness;
        [RenderParameter(0, 0, 0, 0)] float clearcoat;
        [RenderParameter(0.01, 0, 0, 0)] float clearcoatRoughness;
        [RenderParameter(1, 0, 0, 0)] float occlusion;
    };

    // A texture of each input, by the specification's names.
    struct Textures {
        [RenderColor(1, 1, 1, 1)] [RenderReads("rgb")] RenderTexture diffuseColor;
        [RenderData(0.5, 0.5, 1, 1)] [RenderReads("rgb")] RenderTexture normal;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture roughness;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture metallic;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture occlusion;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture emissiveColor;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture opacity;
        [RenderColor(0, 0, 0, 1)] [RenderReads("rgb")] RenderTexture specularColor;
        [RenderData(0, 0, 0, 1)] [RenderReads("chosen")] RenderTexture clearcoat;
        [RenderData(1, 1, 1, 1)] [RenderReads("chosen")] RenderTexture clearcoatRoughness;
    };

    // The attributes of the Khronos PBR types, and the emissive color of each vertex.
    struct Attributes {
        RENDER_KHRONOS_PBR_ATTRIBUTES
        [RenderVertex("custom", "half4", 1, 1, 1, 1)] [RenderOptional] float4 emissiveColor;
    };

    // Returns a color input: the texture's value if the material names one, and else `value`.
    static float3 color(RenderTexture texture, RenderTexCoord uv[2], float3 value) {
        if (!renderMaterialHasTexture(texture))
            return value;
        return renderMaterialSample(texture, uv).rgb;
    }

    // Returns a scalar input: the channel of the texture that it reads if the material names one, and else `value`.
    static float scalar(RenderTexture texture, RenderTexCoord uv[2], float value) {
        if (!renderMaterialHasTexture(texture))
            return value;
        return renderMaterialScalar(texture, uv);
    }

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexCoord uv[2] = {attributes.uv0, attributes.uv1};
        float3 diffuse = color(textures.diffuseColor, uv, parameters.diffuseColor.rgb) * attributes.color.rgb;
        float opacity = scalar(textures.opacity, uv, parameters.opacity) * attributes.color.a;

        // The normal texture's value is in the frame of the mesh's tangent, and a mesh without tangents keeps its own.
        float3 normal = inputs.normal;
        if (renderMaterialHasTexture(textures.normal))
            normal = renderMaterialFrameNormal(inputs, normal, renderMaterialSample(textures.normal, uv).xyz);

        RenderPbrSurface surface = renderPbrSurfaceDefault(normal);
        surface.baseColor = diffuse;
        surface.alpha = opacity;
        surface.roughness = saturate(scalar(textures.roughness, uv, parameters.roughness));
        surface.occlusion = saturate(scalar(textures.occlusion, uv, parameters.occlusion));
        float3 emissive = color(textures.emissiveColor, uv, parameters.emissiveColor.rgb);
        surface.emissive = emissive * attributes.emissiveColor.rgb;
        surface.specular = 1;
        surface.ior = parameters.ior;
        if (parameters.useSpecularWorkflow != 0) {
            // The specular workflow: the diffuse color under a lobe that reflects the specular color at normal
            // incidence, with no metal.
            surface.metallic = 0;
            surface.dielectricF0 = color(textures.specularColor, uv, parameters.specularColor.rgb);
        } else {
            surface.metallic = saturate(scalar(textures.metallic, uv, parameters.metallic));
            surface.dielectricF0 = renderPbrDielectricF0(surface.ior, float3(1, 1, 1));
        }

        // The coat is a white dielectric of the index of refraction 1.5 over the surface, with the surface's normal.
        surface.clearcoat = saturate(scalar(textures.clearcoat, uv, parameters.clearcoat));
        if (surface.clearcoat > 0) {
            surface.clearcoatRoughness =
                saturate(scalar(textures.clearcoatRoughness, uv, parameters.clearcoatRoughness));
            surface.clearcoatNormal = normal;
        }
        surface.position = inputs.position;
        return surface;
    }
}
