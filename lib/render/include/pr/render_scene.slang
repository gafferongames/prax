// lib/render's scene, as shaders read it: the root data of a draw or a dispatch, the records of the instances, draws,
// mesh parts and materials, a mesh's geometry and the formats of its attributes, and the transforms between a mesh, the
// world and a view.
//
// The shaders compile for two binding tiers. By default they reach the scene's resources by the handles in the uniforms
// and in the records. With PR_RENDER_SLOTS defined, for a device without bindless, they reach them through slots. Only
// the accessors differ, so the functions that call them serve both tiers.
//
// A file declares only the slots that its own functions read, and every entry point of a source has every slot that the
// source declares (pr/gpu.h). So each pass's entry points are a source of their own, which includes the files whose
// functions it calls. Each file numbers its own slots, and the renderer binds each by its name at the number that the
// build reflects (prGpuShaderLibrarySlots), so no number is written twice. The smallest device has 8 texture slots and
// 4 buffer slots, and the passes that shade use all of them.

#pragma once

#include "pr/gpu.slang"
#include "pr/render_records.h"
#include "pr/render_surface.slang"

PR_GPU_PUSH(PrRenderDrawPush, push);
PR_GPU_UNIFORMS(PrRenderFrameUniforms, frame);

// ---------------------------------------------------------------------------------------------------------------------
// Records

#if defined(PR_RENDER_SLOTS)

PR_GPU_BUFFER(ByteAddressBuffer, renderSlotInstances, 0);
PR_GPU_BUFFER(ByteAddressBuffer, renderSlotDraws, 1);
PR_GPU_BUFFER(ByteAddressBuffer, renderSlotParts, 2);
PR_GPU_BUFFER(ByteAddressBuffer, renderSlotMaterials, 3);

// Return the record at an index of the scene's instances and draws, and of the renderer's parts and materials.
PrRenderInstanceRecord renderInstance(uint index) {
    return renderSlotInstances.Load<PrRenderInstanceRecord>(index * kPrRenderInstanceBytes);
}

PrRenderDrawRecord renderDraw(uint index) {
    return renderSlotDraws.Load<PrRenderDrawRecord>(index * kPrRenderDrawBytes);
}

PrRenderPartRecord renderPart(uint index) {
    return renderSlotParts.Load<PrRenderPartRecord>(index * kPrRenderPartBytes);
}

PrRenderMaterialRecord renderMaterial(uint index) {
    return renderSlotMaterials.Load<PrRenderMaterialRecord>(index * kPrRenderMaterialBytes);
}

// Returns what the materials' buffer holds at an offset in bytes, which a material's record names: the material's
// parameter record, as the structure that its type defines for it, or one of its textures.
T renderMaterialLoad<T>(uint offset) {
    return renderSlotMaterials.Load<T>(offset);
}

#else

PrRenderInstanceRecord renderInstance(uint index) {
    return prGpuLoad<PrRenderInstanceRecord>(frame.instances, index * kPrRenderInstanceBytes);
}

PrRenderDrawRecord renderDraw(uint index) {
    return prGpuLoad<PrRenderDrawRecord>(frame.draws, index * kPrRenderDrawBytes);
}

PrRenderPartRecord renderPart(uint index) {
    return prGpuLoad<PrRenderPartRecord>(frame.parts, index * kPrRenderPartBytes);
}

PrRenderMaterialRecord renderMaterial(uint index) {
    return prGpuLoad<PrRenderMaterialRecord>(frame.materials, index * kPrRenderMaterialBytes);
}

T renderMaterialLoad<T>(uint offset) {
    return prGpuLoad<T>(frame.materials, offset);
}

#endif

// Returns the scene's time in seconds, which the application sets (prRenderSceneTimeSet). A material that changes with
// time reads it here, so that every pass of a frame computes the material at one time.
float renderSceneTime() {
    return frame.time;
}

// Returns the factor that scales the scene's luminance into the values of the view's color
// (PrRenderViewDesc::exposure), for a material whose values are the view's and not the scene's. It is 1 in a pass that
// stores no color.
float renderViewExposure() {
    return frame.exposure;
}

// ---------------------------------------------------------------------------------------------------------------------
// Vertices

// A vertex of a mesh's geometry, which a pipeline's vertex layout fetches from a buffer of PrRenderGeometryRecord. The
// attributes that a material reads follow it at the next locations, from a buffer of the layout that its type declares.
struct RenderVertex {
    PR_GPU_ATTRIBUTE(float3, position, 0);
    PR_GPU_ATTRIBUTE(float4, normal, 1);    // xyz, of unit length
    PR_GPU_ATTRIBUTE(float4, tangent, 2);   // xyz, of unit length or zero, and the handedness in w
};

// The first location of the attributes that a material reads.
static const uint kRenderAttributeLocation = 3;

// Return the values of words as a vertex layout reads them in each format of lib/gpu (PrGpuVertexFormat): signed
// normalized values map their largest to 1 and nothing below -1, unsigned normalized ones their largest to 1, and
// halves are IEEE 754 binary16.
float4 renderSnorm16x4(uint low, uint high) {
    int4 values = int4(int(low << 16) >> 16, int(low) >> 16, int(high << 16) >> 16, int(high) >> 16);
    return max(float4(values) / 32767.0, -1.0);
}

float2 renderSnorm16x2(uint word) {
    int2 values = int2(int(word << 16) >> 16, int(word) >> 16);
    return max(float2(values) / 32767.0, -1.0);
}

float4 renderUnorm16x4(uint low, uint high) {
    return float4(uint4(low, low >> 16, high, high >> 16) & 0xffff) / 65535.0;
}

float2 renderUnorm16x2(uint word) {
    return float2(uint2(word, word >> 16) & 0xffff) / 65535.0;
}

float4 renderUnorm8x4(uint word) {
    uint4 values = uint4(word, word >> 8, word >> 16, word >> 24) & 0xff;
    return float4(values) / 255.0;
}

float4 renderSnorm8x4(uint word) {
    int4 values = int4(int(word << 24) >> 24, int(word << 16) >> 24, int(word << 8) >> 24, int(word) >> 24);
    return max(float4(values) / 127.0, -1.0);
}

uint4 renderUint8x4(uint word) {
    return uint4(word, word >> 8, word >> 16, word >> 24) & 0xff;
}

float2 renderHalf2(uint word) {
    return f16tof32(uint2(word, word >> 16));
}

float4 renderHalf4(uint low, uint high) {
    return f16tof32(uint4(low, low >> 16, high, high >> 16));
}

// Returns a set of texture coordinates without derivatives, for a pass that samples no texture by them.
RenderTexCoord renderTexCoordAt(float2 value) {
    RenderTexCoord coordinates;
    coordinates.value = value;
    coordinates.dx = float2(0, 0);
    coordinates.dy = float2(0, 0);
    return coordinates;
}

#if !defined(PR_RENDER_SLOTS)

// Returns the index of the vertex that the index at `ordinal` of a part's indices names.
uint renderIndexPull(PrRenderPartRecord part, uint ordinal) {
    return prGpuLoad<uint>(part.indices, (part.firstIndex + ordinal) * kPrRenderIndexBytes);
}

// Returns the vertex of a part's geometry that the index `index` names, as a vertex layout would fetch it. A shader
// that pulls its vertices calls it, which needs a device with bindless.
RenderVertex renderVertexPull(PrRenderPartRecord part, uint index) {
    PrRenderGeometryRecord stored =
        prGpuLoad<PrRenderGeometryRecord>(part.geometry, (part.firstVertex + index) * kPrRenderGeometryBytes);
    RenderVertex vertex;
    vertex.position = float3(stored.position[0], stored.position[1], stored.position[2]);
    vertex.normal = renderSnorm16x4(stored.normal[0], stored.normal[1]);
    vertex.tangent = renderSnorm16x4(stored.tangent[0], stored.tangent[1]);
    return vertex;
}

// The corners of a triangle of a draw at a point of the triangle, from which a technique that shades away from the
// triangle's draw interpolates the attributes that a material reads: the indices of the corners' vertices, their
// weights at the point, and the derivatives of the weights across one pixel to the right and one pixel down.
struct RenderCorners {
    PrRenderDrawRecord draw;
    uint index[3];
    float3 weights;
    float3 weightsDx;
    float3 weightsDy;
};

// Returns the word at `offset` bytes into the vertex `index` of a draw's attributes, whose vertices are `stride` bytes.
uint renderAttributeWord(PrRenderDrawRecord draw, uint index, uint stride, uint offset) {
    return prGpuLoad<uint>(draw.attributes, (draw.firstAttribute + index) * stride + offset);
}

#endif

// ---------------------------------------------------------------------------------------------------------------------
// Transforms

// Returns a mesh's point in the world.
float3 renderWorldPoint(PrRenderInstanceRecord instance, float3 point) {
    float4 p = float4(point, 1);
    return float3(dot(instance.world[0], p), dot(instance.world[1], p), dot(instance.world[2], p));
}

// Returns a mesh's direction in the world, such as a tangent, not of unit length.
float3 renderWorldDirection(PrRenderInstanceRecord instance, float3 direction) {
    return float3(dot(instance.world[0].xyz, direction), dot(instance.world[1].xyz, direction),
        dot(instance.world[2].xyz, direction));
}

// Returns -1 if the instance's matrix reflects, which reverses the winding of its triangles, and 1 otherwise.
float renderWorldSign(PrRenderInstanceRecord instance) {
    return dot(cross(instance.world[0].xyz, instance.world[1].xyz), instance.world[2].xyz) < 0 ? -1.0 : 1.0;
}

// Returns a mesh's normal in the world, not of unit length. It is the normal times the cofactors of the instance's
// matrix, which keeps it perpendicular to its surface under any scale. The cofactors of a matrix that reflects turn the
// normal to the surface's inner side, so the sign of the matrix turns it back.
float3 renderWorldNormal(PrRenderInstanceRecord instance, float3 normal) {
    float3 r0 = instance.world[0].xyz;
    float3 r1 = instance.world[1].xyz;
    float3 r2 = instance.world[2].xyz;
    float3 cofactors = float3(dot(cross(r1, r2), normal), dot(cross(r2, r0), normal), dot(cross(r0, r1), normal));
    return cofactors * renderWorldSign(instance);
}

// Returns the largest factor by which the instance scales a length along one of the mesh's axes.
float renderWorldScale(PrRenderInstanceRecord instance) {
    float3 x = float3(instance.world[0].x, instance.world[1].x, instance.world[2].x);
    float3 y = float3(instance.world[0].y, instance.world[1].y, instance.world[2].y);
    float3 z = float3(instance.world[0].z, instance.world[1].z, instance.world[2].z);
    return sqrt(max(dot(x, x), max(dot(y, y), dot(z, z))));
}

// Returns the world's point in a view's clip space.
float4 renderClipPoint(uint view, float3 point) {
    float4 p = float4(point, 1);
    uint row = view * 4;
    return float4(dot(frame.viewProjection[row], p), dot(frame.viewProjection[row + 1], p),
        dot(frame.viewProjection[row + 2], p), dot(frame.viewProjection[row + 3], p));
}
