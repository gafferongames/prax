// The render tests' own material types and shading models, as an application writes its own: a procedural pattern with
// parameters, a texture and the scene's time, lit by the library's model, a type that reads custom attributes of a
// mesh's vertices, and a toon model and a wrap model, each with its own surface. The tests compare what they draw with
// the formulas here.

#pragma once

#include "pr/render_environment.slang"
#include "pr/render_material.slang"
#include "pr/render_shading.slang"

#include "test_render_materials.h"

// ---------------------------------------------------------------------------------------------------------------------
// Rings: a pattern, lit by the library's model

struct TestRings : IRenderMaterial {
    typedef TestRingsParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {
        [RenderData(0.25, 0.5, 0.75, 1)] RenderTexture pattern;
    };

    struct Attributes {
        [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv;
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        float2 uv = attributes.uv.value;
        float phase = parameters.frequencyU * uv.x + parameters.rate * renderSceneTime();
        float part = 0.5 + 0.5 * sin(phase) * cos(parameters.frequencyV * uv.y);
        float4 color =
            lerp(parameters.low, parameters.high, part) * renderMaterialSample(textures.pattern, attributes.uv);
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.baseColor = color.rgb * parameters.lit;
        surface.emissive = color.rgb * (1 - parameters.lit);
        surface.alpha = color.a;
        surface.specular = parameters.lit;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Quilt: more textures than a draw binds through slots, and a large parameter record

struct TestQuilt : IRenderMaterial {
    typedef TestQuiltParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {
        [RenderData(0.125, 0, 0, 1)] RenderTexture patch0;
        [RenderData(0.25, 0, 0, 1)] RenderTexture patch1;
        [RenderData(0.375, 0, 0, 1)] RenderTexture patch2;
        [RenderData(0.5, 0, 0, 1)] RenderTexture patch3;
        [RenderData(0.625, 0, 0, 1)] RenderTexture patch4;
        [RenderData(0.75, 0, 0, 1)] RenderTexture patch5;
        [RenderData(0.875, 0, 0, 1)] RenderTexture patch6;
        [RenderData(0.0625, 0, 0, 1)] RenderTexture patch7;
    };

    struct Attributes {
        [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv;
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderTexture patches[kTestQuiltTextures] = {textures.patch0, textures.patch1, textures.patch2,
            textures.patch3, textures.patch4, textures.patch5, textures.patch6, textures.patch7};
        float3 sum = parameters.tint.rgb;
        for (uint i = 0; i < kTestQuiltTextures; ++i)
            sum += parameters.weights[i].rgb * renderMaterialSample(patches[i], attributes.uv).r;
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        surface.emissive = sum;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Toon: bands of diffuse light under a highlight with a hard edge

// The toon model.
struct TestToonSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;
    float3 normal;          // in the world, of unit length
    float bands;
    float ambient;
    float3 highlight;
    float highlightEdge;    // the cosine of the angle between the normal and the half vector at the highlight's edge

    // The light of one light: the base color in the band that the light's angle falls in, which is the same over the
    // band, and the highlight wherever the half vector lies within its edge of the normal.
    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        float nl = saturate(dot(normal, light.toLight));
        float band = ceil(nl * bands) / bands;
        float3 halfway = normalize(toEye + light.toLight);
        float edge = nl > 0 && dot(normal, halfway) > highlightEdge ? 1.0 : 0.0;
        return (color * band + highlight * edge) * (light.irradiance * light.visibility);
    }

    float3 indirectRadiance(float3 toEye) {
        return color * ambient;
    }
}

struct TestToonPaint : IRenderMaterial {
    typedef TestToonParameters Parameters;
    typedef TestToonSurface Surface;

    struct Textures {};
    struct Attributes {};

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        TestToonSurface surface;
        surface.color = parameters.color.rgb;
        surface.alpha = parameters.color.a;
        surface.normal = inputs.normal;
        surface.bands = parameters.bands;
        surface.ambient = parameters.ambient;
        surface.highlight = parameters.highlight.rgb;
        surface.highlightEdge = parameters.highlight.w;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Probe: a model with a context, which shows what the library gives a shading model

// What the probe model prepares for a pixel.
struct TestProbeContext {
    float facing;           // the cosine of the normal with the eye
};

// The probe model.
struct TestProbeSurface : IRenderSurface {
    typedef TestProbeContext Context;

    float3 color;
    float alpha;
    float3 normal;          // in the world, of unit length
    float3 geometricNormal;
    float2 uv;
    uint shows;

    Context prepare(float3 toEye) {
        TestProbeContext context;
        context.facing = dot(normal, toEye);
        return context;
    }

    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) {
        return shows == kTestProbeLightSize ? float3(light.angularRadius, 0, 0) : float3(0, 0, 0);
    }

    float3 indirectRadiance(Context context, float3 toEye) {
        return float3(0, 0, 0);
    }

    // The emission reads the context and the direction to the eye, as a coated emitter's does.
    override float3 emittedRadiance(Context context, float3 toEye) {
        if (shows == kTestProbeFacing)
            return color * context.facing * dot(normal, toEye);
        if (shows == kTestProbeGeometric)
            return geometricNormal * 0.5 + 0.5;
        if (shows == kTestProbeSplitSum)
            return float3(renderSplitSum(uv.x, uv.y), 0);
        if (shows == kTestProbeRamp)
            return renderTableSample(kRenderTableRamp, uv).rgb;
        if (shows == kTestProbeFlat)
            return renderTableSample(kRenderTableFlat, uv).rgb;
        return float3(0, 0, 0);
    }
}

struct TestProbe : IRenderMaterial {
    typedef TestProbeParameters Parameters;
    typedef TestProbeSurface Surface;

    struct Textures {};

    struct Attributes {
        [RenderVertex("uv", "float2", 0, 0, 0, 0)] RenderTexCoord uv;
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        TestProbeSurface surface;
        surface.color = parameters.color.rgb;
        surface.alpha = 1;
        surface.normal = inputs.normal;
        surface.geometricNormal = inputs.geometricNormal;
        surface.uv = attributes.uv.value;
        surface.shows = parameters.shows;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Wrap: diffuse light that reaches around to the side that faces away

// The wrap model.
struct TestWrapSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;
    float3 normal;          // in the world, of unit length

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        float wrapped = dot(normal, light.toLight) * 0.5 + 0.5;
        return color * (wrapped * wrapped) * (light.irradiance * light.visibility);
    }

    float3 indirectRadiance(float3 toEye) {
        return color * renderEnvironmentIrradiance(normal);
    }
}

struct TestFelt : IRenderMaterial {
    typedef TestFeltParameters Parameters;
    typedef TestWrapSurface Surface;

    struct Textures {};
    struct Attributes {};

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        TestWrapSurface surface;
        surface.color = parameters.color.rgb;
        surface.alpha = parameters.color.a;
        surface.normal = inputs.normal;
        return surface;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Heat: attributes of a mesh's vertices that only this type knows

// A surface that emits the vertices' temperature in red, scaled, and their band in green, as a share of the bands, and
// reflects nothing. A mesh's producer converts a temperature of doubles into floats and bands of 64-bit integers into
// 32-bit ones, so the shader reads the layout that it declares, whatever the source held. A vertex without them is at a
// temperature of 0.5 in band 3.
struct TestHeat : IRenderMaterial {
    typedef TestHeatParameters Parameters;
    typedef RenderPbrSurface Surface;

    struct Textures {};

    struct Attributes {
        [RenderVertex("custom", "float", 0.5, 0, 0, 0)] float temperature;
        [RenderVertex("custom", "uint", 3, 0, 0, 0)] uint band;
    };

    static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
        Textures textures) {
        RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
        float band = float(attributes.band) / parameters.scale.y;
        surface.emissive = float3(attributes.temperature * parameters.scale.x, band, 0);
        return surface;
    }
}
