// The unlit material type: a surface whose color is its base color times its texture and the vertex's color, which no
// light changes. It is a surface of the Khronos PBR model that reflects nothing and emits its color, by glTF's names
// (KHR_materials_unlit). That color is what the surface shows, and not a luminance of the scene's, so a material of the
// type has the view's emission (PrRenderEmission_Display), which the view's color holds as it was authored at any
// exposure.

#pragma once

#include "pr/render_brdf.slang"
#include "pr/render_material.slang"
#include "pr/render_material_khronos_pbr.slang"
#include "pr/render_shading.slang"

struct RenderMaterialUnlit : IRenderMaterial {
    typedef RenderPbrSurface Surface;

    // The color and its alpha, linear.
    struct Parameters {
        [RenderParameter(1, 1, 1, 1)] float4 baseColor;
    };

    struct Textures {
        [RenderColor(1, 1, 1, 1)] RenderTexture baseColor;
    };

    struct Attributes {
        RENDER_KHRONOS_PBR_ATTRIBUTES
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexCoord uv[2] = {attributes.uv0, attributes.uv1};
        float4 color = parameters.baseColor * attributes.color * renderMaterialSample(textures.baseColor, uv);
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.alpha = color.a;
        surface.emissive = color.rgb;
        return surface;
    }
}
