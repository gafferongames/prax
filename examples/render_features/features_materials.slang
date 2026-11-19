// The render features' own material types and shading model, which the example's shader set adds to the library's
// (examples/CMakeLists.txt): a checker that is computed and not sampled, a surface that changes with the scene's time,
// and a toon model with a surface of its own. Both techniques call each.

#pragma once

#include "pr/render_environment.slang"
#include "pr/render_material.slang"
#include "pr/render_shading.slang"

#include "features_materials.h"

// ---------------------------------------------------------------------------------------------------------------------
// Checker

// Returns the part of a pixel that the odd cells of a checker cover, from 0 to 1. `at` is the pixel's center in cells,
// and `width` the pixel's extent in cells along each axis. It is the checker averaged over the pixel's box, in closed
// form: far away, where a pixel holds many cells, it tends to one half and does not shimmer.
float checkerOdd(float2 at, float2 width) {
    // The integral of a wave that is 1 on even cells and -1 on odd ones is a triangle wave, so the wave's mean over the
    // pixel along one axis is the difference of the triangle wave at the pixel's two edges, over its width.
    float2 low = abs(frac((at - 0.5 * width) * 0.5) - 0.5);
    float2 high = abs(frac((at + 0.5 * width) * 0.5) - 0.5);
    float2 mean = 2 * (low - high) / width;
    // The product of the two axes' means is 1 on an even cell and -1 on an odd one.
    return 0.5 - 0.5 * mean.x * mean.y;
}

// The attributes of a type that reads one set of texture coordinates, the mesh's first.
struct FeaturesTexCoord {
    [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv0;
};

// What a type that samples no texture names, and a type that reads no attribute of the mesh's vertices.
struct FeaturesNoTextures {};
struct FeaturesNoAttributes {};

struct FeaturesChecker : IRenderMaterial {
    typedef FeaturesCheckerParameters Parameters;
    typedef RenderPbrSurface Surface;
    typedef FeaturesNoTextures Textures;
    typedef FeaturesTexCoord Attributes;

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        float2 at = attributes.uv0.value * parameters.cells;
        // The derivatives that the technique gives say how much of the checker one pixel holds.
        float2 width = max((abs(attributes.uv0.dx) + abs(attributes.uv0.dy)) * parameters.cells, 1e-5);
        float odd = checkerOdd(at, width);
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = lerp(parameters.even.rgb, parameters.odd.rgb, odd);
        surface.roughness = lerp(parameters.evenRoughness, parameters.oddRoughness, odd);
        surface.specular = 1;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Pulse

struct FeaturesPulse : IRenderMaterial {
    typedef FeaturesPulseParameters Parameters;
    typedef RenderPbrSurface Surface;
    typedef FeaturesNoTextures Textures;
    typedef FeaturesTexCoord Attributes;

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        // The bands move with the scene's time, which every pass of a frame reads alike.
        float phase = attributes.uv0.value.y * parameters.bands - renderSceneTime() * parameters.speed;
        float wave = 0.5 + 0.5 * cos(phase * 6.283185307);
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = parameters.color.rgb;
        surface.roughness = parameters.roughness;
        surface.specular = 1;
        surface.emissive = parameters.glow.rgb * (wave * wave * wave * wave);
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Toon

// The toon model: what it reads at a point, with the alpha that every surface has, and the radiance that leaves it.
struct FeaturesToonSurface : IRenderSimpleSurface {
    float3 color;       // linear sRGB
    float alpha;
    float3 normal;      // in the world, of unit length
    float bands;        // the steps of a light's angle
    float highlight;
    float highlightEdge;

    // One light's part: the base color in steps of the light's angle, with a shadow that has no penumbra, and a
    // highlight with a hard edge.
    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        float unshadowed = light.visibility > 0.5 ? 1.0 : 0.0;
        float facing = saturate(dot(normal, light.toLight));
        float level = ceil(facing * bands) / bands;
        float3 halfway = normalize(toEye + light.toLight);
        float edge = facing > 0 && dot(normal, halfway) > highlightEdge ? highlight : 0.0;
        return (color * (level * 0.3183098862) + edge) * (light.irradiance * unshadowed);
    }

    // The environment's part: its diffuse light from straight above, the same on every side of an object, so that the
    // steps of the lights are the only shading.
    float3 indirectRadiance(float3 toEye) {
        return color * renderEnvironmentIrradiance(float3(0, 1, 0));
    }
}

struct FeaturesToon : IRenderMaterial {
    typedef FeaturesToonParameters Parameters;
    typedef FeaturesToonSurface Surface;
    typedef FeaturesNoTextures Textures;
    typedef FeaturesNoAttributes Attributes;

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters, Textures textures) {
        FeaturesToonSurface surface;
        surface.color = parameters.color.rgb;
        surface.alpha = 1;
        surface.normal = inputs.normal;
        surface.bands = parameters.bands;
        surface.highlight = parameters.highlight;
        surface.highlightEdge = parameters.highlightEdge;
        return surface;
    }
}
