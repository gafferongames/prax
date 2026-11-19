// What a pass that draws a mesh's surface carries from its vertex shader to its fragment shader of the mesh's geometry,
// and the point of the surface that a fragment offers a material. The passes that evaluate a material where a mesh is
// drawn share them: the forward pass, and the passes that test a masked material's alpha. The attributes that a
// material's type declares travel beside them, in varyings that the build generates for each layout of a vertex
// (render_shaders_check.cmake), which the hardware interpolates.

#pragma once

#include "pr/render_scene.slang"
#include "pr/render_surface.slang"

struct RenderVaryings {
    float4 clip : SV_Position;
    float3 position : TEXCOORD0;    // in the world
    float3 normal : TEXCOORD1;      // in the world, not of unit length
    float4 tangent : TEXCOORD2;     // in the world, not of unit length, and the handedness in the world in w
    nointerpolation uint instance : TEXCOORD3;
    nointerpolation uint material : TEXCOORD4;
    nointerpolation uint flags : TEXCOORD5;     // the draw's flags
};

// The first TEXCOORD of the attributes that a material's type declares.
static const uint kRenderAttributeVarying = 8;

// Returns what a vertex of the draw `index` carries of its geometry, at its position in `view`.
RenderVaryings renderVaryings(RenderVertex vertex, uint index, uint view) {
    PrRenderDrawRecord draw = renderDraw(index);
    PrRenderInstanceRecord instance = renderInstance(draw.instance);
    RenderVaryings out;
    out.position = renderWorldPoint(instance, vertex.position);
    out.normal = renderWorldNormal(instance, vertex.normal.xyz);
    float handedness = vertex.tangent.w * renderWorldSign(instance);
    out.tangent = float4(renderWorldDirection(instance, vertex.tangent.xyz), handedness);
    out.instance = draw.instance;
    out.material = draw.material;
    out.flags = draw.flags;
    out.clip = renderClipPoint(view, out.position);
    return out;
}

// Returns the point of a surface that a fragment shows. `isFront` is false on a triangle's back face, which only a
// double-sided material draws, and whose frame is reversed. The function takes derivatives of the fragment's position,
// so a fragment shader calls it before any branch. They are the fine derivatives, which every device computes alike.
RenderSurfaceInputs renderVaryingInputs(RenderVaryings input, bool isFront) {
    float facing = isFront ? 1.0 : -1.0;
    RenderSurfaceInputs inputs;
    inputs.position = input.position;
    inputs.normal = normalize(input.normal) * facing;
    // The triangle is flat, so the differences of its position between neighboring pixels lie in its plane.
    float3 across = cross(prGpuDdxFine(input.position), prGpuDdyFine(input.position));
    inputs.geometricNormal = normalize(dot(across, inputs.normal) < 0 ? -across : across);
    inputs.tangent = float4(input.tangent.xyz * facing, input.tangent.w);
    inputs.instance = input.instance;
    inputs.material = input.material;
    return inputs;
}

// Returns a set of texture coordinates that a fragment interpolates, with the fine derivatives that a material samples
// by. A fragment shader calls it before any branch.
RenderTexCoord renderVaryingTexCoord(float2 value) {
    RenderTexCoord coordinates;
    coordinates.value = value;
    coordinates.dx = prGpuDdxFine(value);
    coordinates.dy = prGpuDdyFine(value);
    return coordinates;
}
