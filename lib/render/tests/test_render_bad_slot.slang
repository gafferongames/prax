// A material type whose source declares a texture of its own, which takes a slot. The build of the tests' broken set of
// it reports the declaration (test_render_shaders.cmake).

#pragma once

#include "pr/render_material.slang"
#include "pr/render_shading.slang"

Texture2D<float4> badNoise;

struct SlotParameters {
    [RenderParameter(1, 1, 1, 1)] float4 color;
};

struct SlotMaterial : IRenderMaterial {
    typedef SlotParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {};
    struct Attributes {};

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = badNoise.Load(int3(0, 0, 0)).rgb * parameters.color.rgb;
        return surface;
    }
}
