// The environment of lib/render's scene, as shaders read it: the light that arrives from every direction around the
// scene. A cube holds its radiance, filtered at each level for the specular lobe of a rougher surface, and the uniforms
// hold its diffuse light as spherical harmonics. A shading model lights surfaces with both, and a technique draws the
// first level as a view's background.

#pragma once

#include "pr/render_scene.slang"

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(TextureCube<float4>, renderSlotEnvironment, 6);

// Returns the radiance that the environment's cube holds along a direction of the environment's space, at a level of
// detail.
float3 renderEnvironmentSample(float3 direction, float level) {
    return renderSlotEnvironment.SampleLevel(renderSlotEnvironmentSampler, direction, level).rgb;
}

#else

float3 renderEnvironmentSample(float3 direction, float level) {
    TextureCube<float4> cube = prGpuGet<TextureCube<float4>>(frame.environment);
    return cube.SampleLevel(prGpuGet<SamplerState>(frame.environmentSampler), direction, level).rgb;
}

#endif

// Returns the direction of the environment's space that a direction in the world shows.
float3 renderEnvironmentDirection(float3 direction) {
    return float3(dot(frame.environmentRotation[0].xyz, direction), dot(frame.environmentRotation[1].xyz, direction),
        dot(frame.environmentRotation[2].xyz, direction));
}

// Returns the environment's radiance from a direction in the world, as a specular lobe of `roughness` gathers it about
// that direction. A roughness of 0 gives the environment itself.
float3 renderEnvironmentRadiance(float3 direction, float roughness) {
    float level = saturate(roughness) * frame.environmentLastLevel;
    return renderEnvironmentSample(renderEnvironmentDirection(direction), level) * frame.environmentIntensity;
}

// Returns the radiance that a white surface without gloss leaves under the environment: the irradiance of a plane with
// the unit normal `normal`, in the world, divided by pi. The harmonics of three bands hold a light with one bright spot
// only roughly, and their sum can fall below zero opposite the spot, where the result is zero.
float3 renderEnvironmentIrradiance(float3 normal) {
    float3 n = renderEnvironmentDirection(normal);
    float3 sum = frame.environmentIrradiance[0].rgb + frame.environmentIrradiance[1].rgb * n.y +
        frame.environmentIrradiance[2].rgb * n.z + frame.environmentIrradiance[3].rgb * n.x +
        frame.environmentIrradiance[4].rgb * (n.x * n.y) + frame.environmentIrradiance[5].rgb * (n.y * n.z) +
        frame.environmentIrradiance[6].rgb * (3 * n.z * n.z - 1) + frame.environmentIrradiance[7].rgb * (n.x * n.z) +
        frame.environmentIrradiance[8].rgb * (n.x * n.x - n.y * n.y);
    return max(sum, 0);
}

// How much darker the sun's disc is at its edge than at its middle, as a part of the middle's radiance. The radiance at
// the cosine c of the angle between the sun's surface and the line of sight is the middle's times 1 - k (1 - c): the
// linear law of limb darkening, with one constant for every color. The constant gives the disc the mean that the Sun's
// has at 550 nm, 0.8 of its middle, by the law of degree 2 that Allen's Astrophysical Quantities gives (Cox, 2000):
// 0.30 + 0.93 c - 0.23 c^2, whose mean is 0.805. That law's edge is darker, 0.30 of the middle against this one's 0.4.
static const float kRenderSunLimbDarkening = 0.6;

// The largest radiance that a pixel of the disc is given, which a half float holds.
static const float kRenderSunMaxRadiance = 60000;

// Returns the radiance of the sun's disc of the environment along a unit direction of the environment's space, as a
// pixel that spans the angle `pixelAngle` shows it: the part of the pixel that the disc covers, to within the pixel's
// width at the disc's edge. Nothing below the horizon shows the disc.
float3 renderEnvironmentSun(float3 direction, float pixelAngle) {
    float3 sun = frame.environmentSun.xyz;
    float size = frame.environmentSun.w;
    if (size <= 0 || direction.y < 0 || dot(direction, sun) <= 0)
        return float3(0, 0, 0);
    // The sine of the angle from the disc's middle, which is the angle itself this near to it.
    float away = length(cross(direction, sun));
    float covered = saturate((size - away) / max(pixelAngle, 1e-9) + 0.5);
    if (covered <= 0)
        return float3(0, 0, 0);
    float cosine = sqrt(saturate(1 - (away * away) / (size * size)));
    // The law's mean over the disc is 1 - k / 3, and the radiance in the uniforms is the disc's mean.
    float limb = (1 - kRenderSunLimbDarkening * (1 - cosine)) / (1 - kRenderSunLimbDarkening / 3);
    return min(frame.environmentSunRadiance.rgb * (limb * covered), kRenderSunMaxRadiance);
}

// Returns the environment's radiance behind a pixel of the camera's view, which is the view's background, with the
// sun's disc if the environment has one. `pixel` is the pixel's center in the target.
float3 renderEnvironmentBackground(float2 pixel) {
    // The target's y runs down, and the ray's runs up.
    float3 p = float3(pixel.x / frame.targetExtent.x * 2 - 1, 1 - pixel.y / frame.targetExtent.y * 2, 1);
    float3 ray =
        float3(dot(frame.backgroundRay[0].xyz, p), dot(frame.backgroundRay[1].xyz, p), dot(frame.backgroundRay[2].xyz, p));
    float3 direction = renderEnvironmentDirection(ray);
    float3 radiance = renderEnvironmentSample(direction, 0) * frame.environmentIntensity;
    return radiance + renderEnvironmentSun(normalize(direction), frame.pixelSpread);
}
