// MaterialX's surfaces as OpenPBR Surface's inputs: what each input of OpenPBR is on the surfaces of MaterialX's
// standard library that mean the same, and how a surface's inputs, once each is folded to a value or a map of one
// source's channels, become OpenPBR's.
//
// The surfaces are OpenPBR Surface (its specification v1.1, "Model"), Autodesk Standard Surface (its specification
// v1.0.1) and UsdPreviewSurface ("UsdPreviewSurface Specification"), each with its specification's defaults.

#pragma once

#include "pr/base.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A surface of MaterialX's standard library that a material's network reaches.
enum {
    PrMaterialxSurface_OpenPbr,
    PrMaterialxSurface_Standard,
    PrMaterialxSurface_Preview,
    PrMaterialxSurface_Count,
};
typedef uint8_t PrMaterialxSurface;

// Returns the surface of a node definition's category, as `open_pbr_surface`, `standard_surface` or
// `UsdPreviewSurface`, or Count for a category of no such surface.
PrMaterialxSurface prMaterialxSurfaceFromCategory(PrStringView category);

// An input of OpenPBR as one surface has it: OpenPBR's name and its width, the input's name on the surface, which is
// empty where the surface has none and the input is then `fallback`, the surface's default of it, and the factor that
// brings the surface's unit of it to OpenPBR's. OpenPBR names two inputs that its specification states as tangents,
// `geometry_tangent_rotation` and `geometry_coat_tangent_rotation`, by the angles in radians by which the tangents turn
// about the normal, which Standard Surface gives as a fraction of a turn.
typedef struct PrMaterialxOpenPbrInput {
    PrStringView openPbr;
    uint32_t width;
    PrStringView name;
    float fallback[3];
    float factor;
} PrMaterialxOpenPbrInput;

// Return the number of OpenPBR's inputs that the surfaces map, and each as `surface` has it.
uint32_t prMaterialxOpenPbrInputCount(void);
PrMaterialxOpenPbrInput prMaterialxOpenPbrInput(PrMaterialxSurface surface, uint32_t index);

// An input of a surface, folded: a value, or a map of a source's channels, element i the sum over channels j of
// `weights[i][j]` times channel j, plus `offset[i]`, for an input that `varies`.
typedef struct PrMaterialxFolded {
    float value[4];
    float weights[4][4];
    float offset[4];
    bool varies;
} PrMaterialxFolded;

// What a translation approximates.
enum {
    // Standard Surface's specular weight scales its dielectric alone, and OpenPBR's its metal too. A surface between a
    // dielectric and a metal keeps Standard Surface's weight, which darkens its metal.
    PrMaterialxApproximation_SpecularWeight = 1u << 0,
};
typedef uint8_t PrMaterialxApproximations;

// Translates `inputs`, by OpenPBR's input, each folded as `surface` has it, into OpenPBR's in place: each scaled by its
// factor, a metal of Standard Surface given OpenPBR's specular weight of 1 (OpenPBR Surface v1.1, "Metal"), and a film
// of Standard Surface given wherever it has a thickness. Returns what it approximates.
PrMaterialxApproximations prMaterialxOpenPbrTranslate(PrMaterialxSurface surface, PrMaterialxFolded* inputs);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
