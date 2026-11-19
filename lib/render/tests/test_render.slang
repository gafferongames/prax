// A shader of the render tests' own, which includes the library's shading functions as an application's shader does. It
// puts a white surface in a white furnace: light of unit radiance from every direction above the surface.

#include "pr/gpu.slang"
#include "pr/render_brdf.slang"
#include "pr/render_whole.slang"

struct FurnacePush {
    float metallic;
    float extent;           // the width and height of the target in pixels
    float lowRoughness;     // the roughness at the target's left edge, which rises to 1 at its right edge
    float _pad;
};
PR_GPU_PUSH(FurnacePush, push);

// The steps of the sum over the light's directions: in the cosine of the angle to the normal, and around the normal.
static const uint kPolarSteps = 96;
static const uint kAroundSteps = 192;

[shader("vertex")]
float4 furnaceVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns the part of the furnace's light that a white surface sends toward the eye: the direct lobes summed over the
// hemisphere of light directions. The surface's roughness rises across the target, and the cosine of the eye's angle to
// the normal rises down it. A surface that conserves energy returns no more than 1.
[shader("fragment")]
float4 furnacePixel(float4 position : SV_Position) : SV_Target {
    RenderPbrSurface surface = renderPbrSurfaceDefault(float3(0, 0, 1));
    surface.baseColor = float3(1, 1, 1);
    surface.metallic = push.metallic;
    surface.specular = 1;
    surface.roughness = lerp(push.lowRoughness, 1, position.x / push.extent);
    float nv = position.y / push.extent;
    float3 toEye = float3(sqrt(1 - nv * nv), 0, nv);

    // The directions are spaced evenly in the cosine of their angle to the normal and in their angle around it, so each
    // stands for the same solid angle.
    RenderPbrContext context = renderBrdfPrepare(surface, toEye, 0);
    float3 sum = float3(0, 0, 0);
    for (uint i = 0; i < kPolarSteps; ++i) {
        float nl = (i + 0.5) / kPolarSteps;
        float sine = sqrt(1 - nl * nl);
        for (uint j = 0; j < kAroundSteps; ++j) {
            float around = (j + 0.5) / kAroundSteps * 2 * kRenderPi;
            sum += renderBrdfDirect(surface, context, toEye, float3(sine * cos(around), sine * sin(around), nl));
        }
    }
    return float4(sum * (2 * kRenderPi / (kPolarSteps * kAroundSteps)), 1);
}
