// The parameter records of the render features' own material types (features_materials.slang). C++ and Slang both
// compile this file, so a page fills the structure that the material type reads.

#pragma once

#include "pr/render_records.h"

#if !defined(__SLANG__)
PR_ASSUME_NONNULL_BEGIN
#endif

// A checker of two kinds of cell, `cells` of them to one unit of the mesh's texture coordinates.
struct FeaturesCheckerParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    PrRenderVec4 even;          // the base color of the cells whose column and row add to an even number, linear
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 odd;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float cells;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float evenRoughness;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float oddRoughness;
    float _pad;
};

// A surface that bands of light run over: `bands` of them to one unit of the texture coordinates' v, moving `speed`
// bands a second.
struct FeaturesPulseParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 color;         // the base color, linear
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 glow;          // the radiance of a band at its brightest
    PR_RENDER_PARAMETER(0, 0, 0, 0) float bands;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float speed;
    PR_RENDER_PARAMETER(0, 0, 0, 0) float roughness;
    float _pad;
};

// A surface of the toon shading model.
struct FeaturesToonParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 color;         // the base color, linear
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float bands;                // the number of steps of light between the dark side and the brightest
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float highlight;            // the radiance of the highlight for a unit of the light's irradiance
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float highlightEdge;        // the cosine of the angle from the normal to the half vector at the highlight's edge
    float _pad;
};

#if !defined(__SLANG__)
PR_ASSUME_NONNULL_END
#endif
