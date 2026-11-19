// What lib/render gives a material type: the material's textures, sampled with explicit derivatives, and the operations
// that most materials share. The file also holds what the passes call of a material type.
//
// A material type implements IRenderMaterial (pr/render_surface.slang). Its `surface` computes the surface of the
// type's shading model from a point of a mesh, the attributes that the type declares, the type's parameter record and
// the material's textures, such as a RenderPbrSurface for the model that the library ships (pr/render_brdf.slang).
//
// A material samples only through the functions of this file. A technique that shades a pixel away from the triangle's
// own draw, such as a visibility buffer, has no implicit derivatives to sample with. A material declares no slots: its
// textures are the ones that its material names. A device without bindless binds them for each draw, in the slots that
// this file declares.

#pragma once

#include "pr/render_scene.slang"
#include "pr/render_surface.slang"

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// Returns the record of a texture of the material.
PrRenderMaterialTextureRecord renderMaterialTextureRecord(RenderTexture texture) {
    return renderMaterialLoad<PrRenderMaterialTextureRecord>(texture.record);
}

// Returns the texture of index `index` of a material whose type declares `slotIsIndex`, as the build fills a type's
// Textures.
RenderTexture renderMaterialTexture(PrRenderMaterialRecord material, uint index, bool slotIsIndex) {
    RenderTexture texture;
    texture.record = material.textures + index * kPrRenderMaterialTextureBytes;
    texture.index = index;
    texture.slotIsIndex = slotIsIndex;
    return texture;
}

#if defined(PR_RENDER_SLOTS)

// The number of the draw's material textures that a device without bindless samples, which is the number of the slots
// below. The renderer reads the slots by their names.
static const uint kRenderMaterialTextureSlots = 5;

// The textures of the draw's material that the draw samples, by their slot.
PR_GPU_TEXTURE(Texture2D<float4>, renderSlotMaterialTexture0, 1);
PR_GPU_TEXTURE(Texture2D<float4>, renderSlotMaterialTexture1, 2);
PR_GPU_TEXTURE(Texture2D<float4>, renderSlotMaterialTexture2, 3);
PR_GPU_TEXTURE(Texture2D<float4>, renderSlotMaterialTexture3, 4);
PR_GPU_TEXTURE(Texture2D<float4>, renderSlotMaterialTexture4, 5);

// Returns the value of a texture of the draw's material, whose record is `record`, at `uv`, filtered for the
// derivatives of `uv` across one pixel to the right (`dx`) and one pixel down (`dy`). A texture that no slot holds
// reads as its record's value.
float4 renderMaterialTextureValue(RenderTexture texture, PrRenderMaterialTextureRecord record, float2 uv, float2 dx,
    float2 dy) {
    uint slot = texture.slotIsIndex ? texture.index : record.slot;
    switch (slot) {
        case 0:
            return renderSlotMaterialTexture0.SampleGrad(renderSlotMaterialTexture0Sampler, uv, dx, dy);
        case 1:
            return renderSlotMaterialTexture1.SampleGrad(renderSlotMaterialTexture1Sampler, uv, dx, dy);
        case 2:
            return renderSlotMaterialTexture2.SampleGrad(renderSlotMaterialTexture2Sampler, uv, dx, dy);
        case 3:
            return renderSlotMaterialTexture3.SampleGrad(renderSlotMaterialTexture3Sampler, uv, dx, dy);
        case 4:
            return renderSlotMaterialTexture4.SampleGrad(renderSlotMaterialTexture4Sampler, uv, dx, dy);
        default:
            return record.value;
    }
}

#else

// A device with bindless samples every texture of a material.
static const uint kRenderMaterialTextureSlots = 0xffffffff;

float4 renderMaterialTextureValue(RenderTexture texture, PrRenderMaterialTextureRecord record, float2 uv, float2 dx,
    float2 dy) {
    Texture2D<float4> sampled = prGpuGet<Texture2D<float4>>(record.texture);
    return sampled.SampleGrad(prGpuGet<SamplerState>(record.sampler), uv, dx, dy);
}

#endif

// Returns true if the material names the texture, whether or not its texels have arrived.
bool renderMaterialHasTexture(RenderTexture texture) {
    return (renderMaterialTextureRecord(texture).flags & kPrRenderTextureNamed) != 0;
}

// Returns the value of a texture of the material at a set of coordinates, through the texture's transform, and its
// scale and bias: the texel times the scale plus the bias. A texture that the material does not name is white, and one
// whose texels have not arrived is the stand-in that the type declares for it, scaled and biased alike.
float4 renderMaterialSample(RenderTexture texture, RenderTexCoord coordinates) {
    PrRenderMaterialTextureRecord record = renderMaterialTextureRecord(texture);
    float4 row0 = record.transform[0];
    float4 row1 = record.transform[1];
    float2 uv = coordinates.value;
    float2 at = float2(dot(row0.xy, uv) + row0.z, dot(row1.xy, uv) + row1.z);
    float2 dx = float2(dot(row0.xy, coordinates.dx), dot(row1.xy, coordinates.dx));
    float2 dy = float2(dot(row0.xy, coordinates.dy), dot(row1.xy, coordinates.dy));
    return renderMaterialTextureValue(texture, record, at, dx, dy) * record.scale + record.bias;
}

// Returns the value of a texture of the material at the set of coordinates that the material names for it, of the
// type's `N` sets, as renderMaterialSample does. A set past the type's reads the last.
float4 renderMaterialSample<let N : int>(RenderTexture texture, RenderTexCoord sets[N]) {
    uint set = (renderMaterialTextureRecord(texture).flags >> kPrRenderTextureSetShift) & 0xff;
    // The set is chosen by a select of each, not by indexing the array with it: Metal's compiler reads a local array of
    // structures that a value indexes wrongly, where the sets interpolate with their derivatives.
    RenderTexCoord chosen = sets[0];
    [ForceUnroll]
    for (int i = 1; i < N; ++i)
        if (set >= uint(i))
            chosen = sets[i];
    return renderMaterialSample(texture, chosen);
}

// Returns the scalar that a texture gives an input: the channel of its value that the material names for it.
float renderMaterialScalar(RenderTexture texture, RenderTexCoord coordinates) {
    uint channel = (renderMaterialTextureRecord(texture).flags >> kPrRenderTextureChannelShift) & 3;
    return renderMaterialSample(texture, coordinates)[channel];
}

float renderMaterialScalar<let N : int>(RenderTexture texture, RenderTexCoord sets[N]) {
    uint channel = (renderMaterialTextureRecord(texture).flags >> kPrRenderTextureChannelShift) & 3;
    return renderMaterialSample(texture, sets)[channel];
}

// ---------------------------------------------------------------------------------------------------------------------
// Normals

// Returns the unit normal that a tangent-space normal gives a point of a surface, in the surface's frame, or the
// surface's own normal if the mesh has no tangents or the normal has no direction.
float3 renderMaterialFrameNormal(RenderSurfaceInputs inputs, float3 normal, float3 mapped) {
    float3 t = inputs.tangent.xyz - normal * dot(normal, inputs.tangent.xyz);
    float length2 = dot(t, t);
    if (length2 < 1e-12)
        return normal;
    t *= rsqrt(length2);
    float3 b = cross(normal, t) * inputs.tangent.w;
    float3 n = t * mapped.x + b * mapped.y + normal * mapped.z;
    return dot(n, n) > 1e-12 ? normalize(n) : normal;
}

// Returns the unit normal that a normal texture of the material gives a point of a surface: the texture's tangent-space
// normal, with its x and y scaled by `scale`, in the surface's frame. The texture holds each component of the normal
// mapped from [-1, 1] to [0, 1], as glTF's normal textures do. Returns the surface's own normal if the material lacks
// the texture or the mesh has no tangents.
float3 renderMaterialNormal<let N : int>(RenderTexture texture, float scale, RenderSurfaceInputs inputs,
    RenderTexCoord sets[N]) {
    if (!renderMaterialHasTexture(texture))
        return inputs.normal;
    float3 mapped = renderMaterialSample(texture, sets).xyz * 2 - 1;
    return renderMaterialFrameNormal(inputs, inputs.normal, float3(mapped.xy * scale, mapped.z));
}

// Returns the unit direction of the surface that the mesh's tangent gives, perpendicular to the unit normal `normal`
// and turned counterclockwise about it by `rotation` radians: the direction along which a material stretches an
// anisotropic lobe. A mesh without tangents gives a direction that is perpendicular to the normal and otherwise
// arbitrary.
float3 renderMaterialTangent(RenderSurfaceInputs inputs, float3 normal, float rotation) {
    float3 t = inputs.tangent.xyz - normal * dot(normal, inputs.tangent.xyz);
    float length2 = dot(t, t);
    if (length2 < 1e-12) {
        t = cross(normal, abs(normal.x) < 0.9 ? float3(1, 0, 0) : float3(0, 1, 0));
        length2 = dot(t, t);
    }
    t *= rsqrt(length2);
    float3 b = cross(normal, t) * (inputs.tangent.w < 0 ? -1.0 : 1.0);
    return t * cos(rotation) + b * sin(rotation);
}

// ---------------------------------------------------------------------------------------------------------------------
// A material type, as a pass calls it

// Returns the surface of `material`, a material of the type M, at a point of a mesh, with the attributes and the
// textures that the build filled for the type.
M.Surface renderMaterialSurface<M : IRenderMaterial>(PrRenderMaterialRecord material, RenderSurfaceInputs inputs,
    M.Attributes attributes, M.Textures textures) {
    return M.surface(inputs, attributes, renderMaterialLoad<M.Parameters>(material.parameters), textures);
}

// Returns the factor that takes what the surface of `material` emits to luminance in nits: 1 for an emission that is
// luminance, and the reciprocal of the view's exposure for one that is the view's (kPrRenderMaterialDisplayEmission),
// which the view's exposure then brings back to the value that the material gives.
float renderMaterialEmission(PrRenderMaterialRecord material) {
    return (material.flags & kPrRenderMaterialDisplayEmission) != 0 ? 1 / renderViewExposure() : 1;
}
