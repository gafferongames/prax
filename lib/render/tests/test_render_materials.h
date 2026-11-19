// The parameter records of the render tests' own material types (test_render_materials.slang), which C++ and Slang both
// compile, as an application's are.

#pragma once

#include "pr/render_records.h"

#if !defined(__SLANG__)
PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {
#endif

// A pattern of two colors: the part of `high` at the texture coordinates (u, v) and the scene's time t is 0.5 + 0.5 *
// sin(frequencyU * u + rate * t) * cos(frequencyV * v), and the rest is `low`. The pattern's color is the surface's
// base color by the part `lit`, and its emission by the rest.
struct TestRingsParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 low;           // linear, and alpha
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 high;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float frequencyU;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float frequencyV;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float rate;                 // radians a second
    PR_RENDER_PARAMETER(0, 0, 0, 0) float lit;
};

// A surface of the toon shading model: a color that each light shades in flat bands, under a highlight with a hard
// edge.
struct TestToonParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 color;
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    PrRenderVec4 highlight;     // the highlight's color, and in w the cosine of the angle at its edge
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float bands;                // the number of bands between the dark side and the brightest
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float ambient;              // the part of the color that leaves the surface under no light
    float _pad[2];
};

// A surface of the wrap shading model, of one color.
struct TestFeltParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 color;
};

// The number of textures of the quilt type, which is more than a draw binds on a device without bindless.
PR_RENDER_CONSTANT(kTestQuiltTextures, 8)

// A surface that emits `tint` plus the sum of each of its textures times the texture's weight, and reflects nothing.
// The record is larger than those of the library's types.
struct TestQuiltParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 tint;
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 weights[kTestQuiltTextures];
};

// What a surface of the probe model shows.
PR_RENDER_CONSTANT(kTestProbeFacing, 0)     // emits its color times the cosine of its normal with the eye
PR_RENDER_CONSTANT(kTestProbeLightSize, 1)  // reflects each light's angular radius, in red
PR_RENDER_CONSTANT(kTestProbeGeometric, 2)  // emits its triangle's normal, mapped from [-1, 1] to [0, 1]
PR_RENDER_CONSTANT(kTestProbeSplitSum, 3)   // emits the library's split sum at the roughness u and the cosine v
PR_RENDER_CONSTANT(kTestProbeRamp, 4)       // emits the set's ramp table at its texture coordinates
PR_RENDER_CONSTANT(kTestProbeFlat, 5)       // emits the set's flat table at its texture coordinates

// A surface of the probe shading model.
struct TestProbeParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 color;
    PR_RENDER_PARAMETER(0, 0, 0, 0) uint32_t shows;             // one of the kTestProbe constants
    uint32_t _pad[3];
};

// A surface of the heat type: x scales its temperature, and y is its number of bands.
struct TestHeatParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 scale;
};

#if !defined(__SLANG__)
} // namespace pr_impl

PR_ASSUME_NONNULL_END
#endif
