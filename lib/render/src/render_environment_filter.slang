// lib/render's environment filter: the draws that fill the levels of an environment's cubes when the environment is
// set. Each draw covers one face of one level of a cube, and samples another cube.

#include "pr/gpu.slang"
#include "pr/render_records.h"
#include "pr/render_whole.slang"

PR_GPU_PUSH(PrRenderEnvironmentPush, push);

static const float kFilterPi = 3.14159265358979323846;

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(TextureCube<float4>, renderSlotSource, 0);

// Returns the radiance that the source holds along a direction, at a level of detail.
float3 sourceRadiance(float3 direction, float level) {
    return renderSlotSource.SampleLevel(renderSlotSourceSampler, direction, level).rgb;
}

#else

float3 sourceRadiance(float3 direction, float level) {
    TextureCube<float4> cube = prGpuGet<TextureCube<float4>>(push.source);
    return cube.SampleLevel(prGpuGet<SamplerState>(push.sampler), direction, level).rgb;
}

#endif

// Returns the direction, not of unit length, through a point of a face of a cube. `at` runs from -1 to 1 along the
// face's columns and rows, as lib/gpu lays out a cube's faces.
float3 faceDirection(uint face, float2 at) {
    switch (face) {
        case 0:
            return float3(1, -at.y, -at.x);
        case 1:
            return float3(-1, -at.y, at.x);
        case 2:
            return float3(at.x, 1, at.y);
        case 3:
            return float3(at.x, -1, -at.y);
        case 4:
            return float3(at.x, -at.y, 1);
        default:
            return float3(-at.x, -at.y, -1);
    }
}

// Returns the direction through the texel of the draw's face at a pixel.
float3 texelDirection(float2 pixel) {
    return faceDirection(push.face, pixel / push.targetSize * 2 - 1);
}

// Returns the solid angle of a small square about a direction through a face, relative to that of a square of the same
// size at the middle of the face: it falls with the cube of the direction's length.
float relativeSolidAngle(float3 direction) {
    float length2 = dot(direction, direction);
    return 1 / (length2 * sqrt(length2));
}

// Returns point `index` of `count` points of the Hammersley set, which cover the unit square evenly.
float2 hammersley(uint index, uint count) {
    return float2((float(index) + 0.5) / float(count), float(reversebits(index)) * 2.3283064365386963e-10);
}

[shader("vertex")]
float4 filterVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns the mean radiance of the four texels of the source that the pixel's texel covers, each weighted by its solid
// angle, so that a level holds the light of the level before it: a texel's radiance times its solid angle is the sum of
// that of its four. The source is the level before the draw's, alone. Each sample lies at a texel's center, where the
// filter returns the texel itself.
[shader("fragment")]
float4 reducePixel(float4 position : SV_Position) : SV_Target {
    float3 sum = float3(0, 0, 0);
    float weights = 0;
    for (uint i = 0; i < 4; ++i) {
        float2 offset = float2(i & 1, i >> 1) * 0.5 - 0.25;
        float3 direction = texelDirection(position.xy + offset);
        float weight = relativeSolidAngle(direction);
        sum += sourceRadiance(direction, 0) * weight;
        weights += weight;
    }
    return float4(sum / weights, 1);
}

// Returns the source's radiance as the GGX lobe of the draw's roughness gathers it about the texel's direction, with
// the eye along the lobe's axis, which is the assumption of the split-sum approximation. The lobe is sampled by its own
// distribution, and each sample is weighted by the cosine of its angle to the axis.
//
// Each sample reads the source at the level whose texels are as large as the solid angle that the sample stands for, so
// that a small bright light is gathered by every texel near it and not by the few that a sample happens to hit.
[shader("fragment")]
float4 specularPixel(float4 position : SV_Position) : SV_Target {
    float3 n = normalize(texelDirection(position.xy));
    float3 up = abs(n.z) < 0.999 ? float3(0, 0, 1) : float3(1, 0, 0);
    float3 tangent = normalize(cross(up, n));
    float3 bitangent = cross(n, tangent);
    float alpha = push.roughness * push.roughness;
    float alpha2 = alpha * alpha;
    float texelSolidAngle = 4 * kFilterPi / (6 * push.sourceSize * push.sourceSize);

    float3 sum = float3(0, 0, 0);
    float weight = 0;
    for (uint i = 0; i < push.sampleCount; ++i) {
        float2 xi = hammersley(i, push.sampleCount);
        float phi = 2 * kFilterPi * xi.x;
        float cosTheta = sqrt((1 - xi.y) / (1 + (alpha2 - 1) * xi.y));
        float sinTheta = sqrt(1 - cosTheta * cosTheta);
        float3 h = tangent * (sinTheta * cos(phi)) + bitangent * (sinTheta * sin(phi)) + n * cosTheta;
        float3 l = 2 * cosTheta * h - n;
        float nl = dot(n, l);
        if (nl <= 0)
            continue;
        // With the eye along the normal, the density of the sample's direction is the distribution over 4.
        float d = cosTheta * cosTheta * (alpha2 - 1) + 1;
        float density = alpha2 / (kFilterPi * d * d) / 4;
        float sampleSolidAngle = 1 / (float(push.sampleCount) * density);
        float level = max(0.5 * log2(sampleSolidAngle / texelSolidAngle) + 1, 0);
        sum += sourceRadiance(l, level) * nl;
        weight += nl;
    }
    return float4(sum / max(weight, 1e-6), 1);
}
