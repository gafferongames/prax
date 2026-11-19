// A shader of the render tests' own, which puts a surface of the Khronos PBR model with one layer of an extension in a
// white furnace, and sums the layer's lobes over the directions of light. It includes the library's lobes as an
// application's shader does, and reads no table: it sums the sheen's albedo itself.

#include "pr/gpu.slang"
#include "pr/render_brdf.slang"
#include "pr/render_whole.slang"

struct LayerPush {
    float layer;            // which layer the surface has: one of the kLayer constants
    float extent;           // the width and height of the target in pixels
    float lowRoughness;     // the roughness at the target's left edge, which rises to 1 at its right edge
    float metallic;
    float value;            // the layer's parameter: a film's thickness in nanometers, or an anisotropy's strength
    float _pad0;
    float _pad1;
    float _pad2;
};
PR_GPU_PUSH(LayerPush, push);

static const uint kLayerNone = 0;
static const uint kLayerClearcoat = 1;
static const uint kLayerSheen = 2;
static const uint kLayerIridescence = 3;
static const uint kLayerAnisotropy = 4;
static const uint kLayerDiffuseTransmission = 5;
static const uint kLayerTransmission = 6;

// The steps of the sum over the light's directions: in the cosine of the angle to the normal, and around the normal.
static const uint kPolarSteps = 64;
static const uint kAroundSteps = 128;

[shader("vertex")]
float4 layerVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns a white surface of the roughness `roughness` with one layer.
RenderPbrSurface layerSurface(uint layer, float roughness, float metallic, float value) {
    RenderPbrSurface surface = renderPbrSurfaceDefault(float3(0, 0, 1));
    surface.baseColor = float3(1, 1, 1);
    surface.metallic = metallic;
    surface.specular = 1;
    surface.roughness = roughness;
    if (layer == kLayerClearcoat) {
        surface.clearcoat = 1;
        surface.clearcoatRoughness = roughness;
    } else if (layer == kLayerSheen) {
        surface.sheenColor = float3(1, 1, 1);
        surface.sheenRoughness = roughness;
    } else if (layer == kLayerIridescence) {
        surface.iridescence = 1;
        surface.iridescenceThickness = value;
    } else if (layer == kLayerAnisotropy) {
        surface.anisotropy = value;
        surface.anisotropyDirection = float3(1, 0, 0);
    } else if (layer == kLayerDiffuseTransmission) {
        surface.diffuseTransmission = 0.5;
    } else if (layer == kLayerTransmission) {
        surface.transmission = 1;
    }
    return surface;
}

// Returns, for a white surface with one layer in a white furnace that surrounds it:
// - in red, the part of the furnace's light that the surface sends toward the eye, summed over every direction of
//   light, above the surface and below it;
// - in green, the directional albedo of the sheen lobe of the surface's roughness toward the eye, summed alike;
// - in blue, the sum over the half vectors of the specular lobe's distribution times the half vector's cosine, which is
//   1 for a distribution that is normalized;
// - in alpha, the anisotropic lobe without its Fresnel term at the left edge's roughness, for a light 45 degrees from
//   the normal, whose angle around the normal is the pixel's place across the target, and an eye along the normal.
//
// The surface's roughness rises across the target, and the cosine of the eye's angle to the normal rises down it.
[shader("fragment")]
float4 layerPixel(float4 position : SV_Position) : SV_Target {
    uint layer = uint(push.layer);
    float across = position.x / push.extent;
    float roughness = lerp(push.lowRoughness, 1, across);
    RenderPbrSurface surface = layerSurface(layer, roughness, push.metallic, push.value);
    float nv = position.y / push.extent;
    float3 toEye = float3(sqrt(1 - nv * nv), 0, nv);

    // The sheen's albedo toward the eye, which the layering reads: the lobe summed over the light's directions.
    float albedo = 0;
    for (uint i = 0; i < kPolarSteps; ++i) {
        float nl = (i + 0.5) / kPolarSteps;
        float sine = sqrt(1 - nl * nl);
        for (uint j = 0; j < kAroundSteps; ++j) {
            float around = (j + 0.5) / kAroundSteps * 2 * kRenderPi;
            float3 toLight = float3(sine * cos(around), sine * sin(around), nl);
            float nh = saturate(normalize(toEye + toLight).z);
            albedo += renderSheenLobe(roughness, nh, nl, max(nv, 1e-4)) * nl;
        }
    }
    albedo *= 2 * kRenderPi / (kPolarSteps * kAroundSteps);

    RenderPbrContext context = renderBrdfPrepare(surface, toEye, albedo);
    float3 sum = float3(0, 0, 0);
    float normalized = 0;
    for (uint i = 0; i < kPolarSteps * 2; ++i) {
        float nl = (i + 0.5) / kPolarSteps - 1;
        float sine = sqrt(1 - nl * nl);
        for (uint j = 0; j < kAroundSteps; ++j) {
            float around = (j + 0.5) / kAroundSteps * 2 * kRenderPi;
            float3 direction = float3(sine * cos(around), sine * sin(around), nl);
            sum += renderBrdfDirect(surface, context, toEye, direction);
            if (nl > 0 && context.layered) {
                // The distribution of the lobe's two widths at the half vector `direction`.
                float at = context.anisotropic.alphaT;
                float ab = context.anisotropic.alphaB;
                float3 f = float3(ab * direction.x, at * direction.y, at * ab * nl);
                float w2 = at * ab / dot(f, f);
                normalized += at * ab * w2 * w2 / kRenderPi * nl;
            }
        }
    }
    float scale = 2 * kRenderPi / (kPolarSteps * kAroundSteps);

    float turn = across * 2 * kRenderPi;
    float3 up = float3(0, 0, 1);
    float3 slanted = float3(cos(turn), sin(turn), 1) * sqrt(0.5);
    // The lobe's two widths as KHR_materials_anisotropy gives them for a strength, which a strength of zero makes
    // equal.
    float alpha = push.lowRoughness * push.lowRoughness;
    RenderBrdfAnisotropicLobe shaped = renderBrdfAnisotropicLobe(up, float3(1, 0, 0), float3(0, 1, 0),
        lerp(alpha, 1, push.value * push.value), clamp(alpha, 0.001, 1), up);
    float lobe = renderBrdfGgxAnisotropic(shaped, up, normalize(up + slanted), slanted);
    return float4(sum.g * scale, albedo, normalized * scale, lobe);
}
