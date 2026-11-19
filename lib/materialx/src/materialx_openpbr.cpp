// MaterialX's surfaces as OpenPBR Surface's inputs.

#include "pr/materialx_openpbr.h"

#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/string_ops.h"

namespace pr_impl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// An input of OpenPBR: its name and width, the name and the default of the input that means the same on each surface,
// and the factor that brings a Standard Surface's unit of it to OpenPBR's.
struct OpenPbrInput {
    PrStringView openPbr;
    PrStringView standard;
    PrStringView preview;
    uint32_t width;
    float openPbrFallback[3];
    float standardFallback[3];
    float previewFallback[3];
    float standardFactor;
};

constexpr OpenPbrInput kInputs[] = {
    {"base_weight", "base", "", 1, {1}, {0.8f}, {1}, 1},
    {"base_color", "base_color", "diffuseColor", 3, {0.8f, 0.8f, 0.8f}, {1, 1, 1}, {0.18f, 0.18f, 0.18f}, 1},
    {"base_metalness", "metalness", "metallic", 1, {0}, {0}, {0}, 1},
    {"base_diffuse_roughness", "diffuse_roughness", "", 1, {0}, {0}, {0}, 1},
    {"specular_weight", "specular", "", 1, {1}, {1}, {1}, 1},
    {"specular_color", "specular_color", "", 3, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 1},
    {"specular_roughness", "specular_roughness", "roughness", 1, {0.3f}, {0.2f}, {0.5f}, 1},
    {"specular_ior", "specular_IOR", "ior", 1, {1.5f}, {1.5f}, {1.5f}, 1},
    {"specular_roughness_anisotropy", "specular_anisotropy", "", 1, {0}, {0}, {0}, 1},
    {"transmission_weight", "transmission", "", 1, {0}, {0}, {0}, 1},
    {"transmission_color", "transmission_color", "", 3, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 1},
    {"transmission_depth", "transmission_depth", "", 1, {0}, {0}, {0}, 1},
    {"subsurface_weight", "subsurface", "", 1, {0}, {0}, {0}, 1},
    {"subsurface_color", "subsurface_color", "", 3, {0.8f, 0.8f, 0.8f}, {1, 1, 1}, {1, 1, 1}, 1},
    {"coat_weight", "coat", "clearcoat", 1, {0}, {0}, {0}, 1},
    {"coat_color", "coat_color", "", 3, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 1},
    {"coat_roughness", "coat_roughness", "clearcoatRoughness", 1, {0}, {0.1f}, {0.01f}, 1},
    {"coat_ior", "coat_IOR", "", 1, {1.6f}, {1.5f}, {1.5f}, 1},
    {"fuzz_weight", "sheen", "", 1, {0}, {0}, {0}, 1},
    {"fuzz_color", "sheen_color", "", 3, {1, 1, 1}, {1, 1, 1}, {1, 1, 1}, 1},
    {"fuzz_roughness", "sheen_roughness", "", 1, {0.5f}, {0.3f}, {0.5f}, 1},
    {"thin_film_weight", "", "", 1, {0}, {0}, {0}, 1},
    // Standard Surface gives the film's thickness in nanometers, and OpenPBR in micrometers.
    {"thin_film_thickness", "thin_film_thickness", "", 1, {0.5f}, {0}, {0.5f}, 0.001f},
    {"thin_film_ior", "thin_film_IOR", "", 1, {1.4f}, {1.5f}, {1.4f}, 1},
    {"emission_luminance", "emission", "", 1, {0}, {0}, {1}, 1},
    {"emission_color", "emission_color", "emissiveColor", 3, {1, 1, 1}, {1, 1, 1}, {0, 0, 0}, 1},
    {"geometry_opacity", "opacity", "opacity", 1, {1}, {1}, {1}, 1},
    {"geometry_thin_walled", "thin_walled", "", 1, {0}, {0}, {0}, 1},
    {"geometry_normal", "normal", "normal", 3, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, 1},
    {"geometry_coat_normal", "coat_normal", "", 3, {0, 0, 1}, {0, 0, 1}, {0, 0, 1}, 1},
    {"coat_roughness_anisotropy", "coat_anisotropy", "", 1, {0}, {0}, {0}, 1},
    // Standard Surface turns its tangents about the normals by a fraction of a whole turn (its definition's
    // `tangent_rotate_degree`, 360 times `specular_rotation`). OpenPBR's own surface has no rotation to read.
    {"geometry_tangent_rotation", "specular_rotation", "", 1, {0}, {0}, {0}, 2 * kPi},
    {"geometry_coat_tangent_rotation", "coat_rotation", "", 1, {0}, {0}, {0}, 2 * kPi},
};

// Returns the place of an input among kInputs.
uint32_t indexOf(PrStringView openPbr) {
    for (uint32_t i = 0; i < countOf(kInputs); ++i) {
        if (kInputs[i].openPbr == openPbr)
            return i;
    }
    PR_ASSERT(false);
    return 0;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMaterialxSurface prMaterialxSurfaceFromCategory(PrStringView category) {
    if (category == "open_pbr_surface")
        return PrMaterialxSurface_OpenPbr;
    if (category == "standard_surface")
        return PrMaterialxSurface_Standard;
    if (category == "UsdPreviewSurface")
        return PrMaterialxSurface_Preview;
    return PrMaterialxSurface_Count;
}

uint32_t prMaterialxOpenPbrInputCount(void) {
    return uint32_t(countOf(kInputs));
}

PrMaterialxOpenPbrInput prMaterialxOpenPbrInput(PrMaterialxSurface surface, uint32_t index) {
    PR_ASSERT(index < countOf(kInputs));
    const OpenPbrInput& input = kInputs[index];
    PrMaterialxOpenPbrInput out{.openPbr = input.openPbr, .width = input.width, .factor = 1};
    const float* fallback = input.openPbrFallback;
    out.name = input.openPbr;
    // OpenPBR's own surface states tangents, not the rotations that the other surfaces give.
    if (prStringEndsWith(input.openPbr, "_rotation"))
        out.name = PrStringView{};
    if (surface == PrMaterialxSurface_Standard) {
        out.name = input.standard;
        fallback = input.standardFallback;
        out.factor = input.standardFactor;
    } else if (surface == PrMaterialxSurface_Preview) {
        out.name = input.preview;
        fallback = input.previewFallback;
    }
    for (uint32_t k = 0; k < 3; ++k)
        out.fallback[k] = fallback[k];
    return out;
}

PrMaterialxApproximations prMaterialxOpenPbrTranslate(PrMaterialxSurface surface, PrMaterialxFolded* inputs) {
    PrMaterialxApproximations approximated = 0;
    for (uint32_t i = 0; surface == PrMaterialxSurface_Standard && i < countOf(kInputs); ++i) {
        float factor = kInputs[i].standardFactor;
        PrMaterialxFolded& input = inputs[i];
        for (uint32_t k = 0; factor != 1 && k < kInputs[i].width; ++k) {
            input.value[k] *= factor;
            input.offset[k] *= factor;
            for (uint32_t j = 0; j < 4; ++j)
                input.weights[k][j] *= factor;
        }
    }
    if (surface != PrMaterialxSurface_Standard)
        return approximated;
    // Standard Surface's specular weight scales the dielectric's reflection alone, and OpenPBR's scales the metal's too
    // (OpenPBR Surface v1.1, "Metal"). The two agree on a dielectric, and on a metal where OpenPBR's weight is 1.
    PrMaterialxFolded& metalness = inputs[indexOf("base_metalness")];
    PrMaterialxFolded& specular = inputs[indexOf("specular_weight")];
    if (!metalness.varies && metalness.value[0] >= 1) {
        specular = PrMaterialxFolded{.value = {1}};
    } else if ((metalness.varies || metalness.value[0] > 0) && (specular.varies || specular.value[0] != 1)) {
        approximated |= PrMaterialxApproximation_SpecularWeight;
    }
    // Standard Surface has a film wherever it has a thickness.
    const PrMaterialxFolded& thickness = inputs[indexOf("thin_film_thickness")];
    if (thickness.value[0] > 0 || thickness.varies)
        inputs[indexOf("thin_film_weight")].value[0] = 1;
    return approximated;
}
