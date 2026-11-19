// lib/render's deformation: a dispatch that writes the geometry that one deforming instance draws, from its mesh's
// geometry as it was bound, its morph targets and skin, and the instance's pose. Every pass then draws that geometry as
// it draws a mesh's own, so no other shader knows of deformation. Deformation moves the geometry alone: the attributes
// that a material reads are the mesh's, whatever the pose.
//
// With PR_RENDER_SLOTS defined, for a device without bindless, the dispatch reaches its buffers through slots:
//
//     Buffer 0 to 3       the mesh's geometry, its skin and its morph targets, and the scene's poses
//     RwBuffer 0 and 1    the scene's deformed geometry, and its previous positions

#include "pr/gpu.slang"
#include "pr/render_records.h"

PR_GPU_PUSH(PrRenderDeformPush, push);

// ---------------------------------------------------------------------------------------------------------------------
// Buffers

#if defined(PR_RENDER_SLOTS)

PR_GPU_BUFFER(ByteAddressBuffer, deformSlotVertices, 0);
PR_GPU_BUFFER(ByteAddressBuffer, deformSlotSkin, 1);
PR_GPU_BUFFER(ByteAddressBuffer, deformSlotMorphs, 2);
PR_GPU_BUFFER(ByteAddressBuffer, deformSlotPose, 3);
PR_GPU_RW_BUFFER(RWByteAddressBuffer, deformSlotDeformed, 0);
PR_GPU_RW_BUFFER(RWByteAddressBuffer, deformSlotPrevious, 1);

// Return the mesh's vertex, its skin and one of its morph targets at an index, and a 4-vector and a float of the pose
// at a byte offset.
PrRenderGeometryRecord deformVertex(uint index) {
    return deformSlotVertices.Load<PrRenderGeometryRecord>(index * kPrRenderGeometryBytes);
}

PrRenderSkinRecord deformSkin(uint index) {
    return deformSlotSkin.Load<PrRenderSkinRecord>(index * kPrRenderSkinBytes);
}

PrRenderMorphRecord deformMorph(uint index) {
    return deformSlotMorphs.Load<PrRenderMorphRecord>(index * kPrRenderMorphBytes);
}

float4 deformPoseVector(uint offset) {
    return deformSlotPose.Load<float4>(offset);
}

float deformPoseFloat(uint offset) {
    return deformSlotPose.Load<float>(offset);
}

// Return the buffers that the dispatch writes.
RWByteAddressBuffer deformDeformed() {
    return deformSlotDeformed;
}

RWByteAddressBuffer deformPrevious() {
    return deformSlotPrevious;
}

#else

PrRenderGeometryRecord deformVertex(uint index) {
    return prGpuLoad<PrRenderGeometryRecord>(push.vertices, index * kPrRenderGeometryBytes);
}

PrRenderSkinRecord deformSkin(uint index) {
    return prGpuLoad<PrRenderSkinRecord>(push.skin, index * kPrRenderSkinBytes);
}

PrRenderMorphRecord deformMorph(uint index) {
    return prGpuLoad<PrRenderMorphRecord>(push.morphs, index * kPrRenderMorphBytes);
}

float4 deformPoseVector(uint offset) {
    return prGpuLoad<float4>(push.pose, offset);
}

float deformPoseFloat(uint offset) {
    return prGpuLoad<float>(push.pose, offset);
}

RWByteAddressBuffer deformDeformed() {
    return prGpuGet<RWByteAddressBuffer>(push.deformed);
}

RWByteAddressBuffer deformPrevious() {
    return prGpuGet<RWByteAddressBuffer>(push.previousPositions);
}

#endif

// ---------------------------------------------------------------------------------------------------------------------
// Packing

// Returns the three signed 16-bit values of two words as floats, as a vertex layout reads them.
float3 deformUnpack(uint low, uint high) {
    int3 values = int3(int(low << 16) >> 16, int(low) >> 16, int(high << 16) >> 16);
    return max(float3(values) / 32767.0, -1.0);
}

// Returns a float in [-1, 1] as the 16 bits that a vertex stores for it.
uint deformPack(float value) {
    return uint(int(round(clamp(value, -1.0, 1.0) * 32767.0))) & 0xffff;
}

// Returns the unit vector along `v`, or `fallback` if `v` has no direction.
float3 deformUnit(float3 v, float3 fallback) {
    float length2 = dot(v, v);
    return length2 > 1e-20 ? v * rsqrt(length2) : fallback;
}

// ---------------------------------------------------------------------------------------------------------------------
// The dispatch

// A vertex's frame while it deforms.
struct DeformFrame {
    float3 position;
    float3 normal;
    float3 tangent;
};

// Adds the instance's morph targets to a vertex, each times its weight.
DeformFrame deformByMorphs(DeformFrame frame, uint index) {
    uint weights = push.firstPose + push.jointCount * kPrRenderJointBytes;
    for (uint target = 0; target < push.morphTargetCount; ++target) {
        float weight = deformPoseFloat(weights + target * 4);
        if (weight == 0)
            continue;
        PrRenderMorphRecord morph = deformMorph(index * push.morphTargetCount + target);
        frame.position += weight * float3(morph.position[0], morph.position[1], morph.position[2]);
        frame.normal += weight * float3(morph.normal[0], morph.normal[1], morph.normal[2]);
        frame.tangent += weight * float3(morph.tangent[0], morph.tangent[1], morph.tangent[2]);
    }
    return frame;
}

// Moves a vertex by its joints: by the sum of their matrices, each times its weight. The normal is mapped by that sum's
// cofactors, which keep it perpendicular to the surface under any scale, turned back if the sum reflects.
DeformFrame deformBySkin(DeformFrame frame, uint index) {
    PrRenderSkinRecord skin = deformSkin(index);
    uint4 joints = uint4(skin.joints[0] & 0xffff, skin.joints[0] >> 16, skin.joints[1] & 0xffff, skin.joints[1] >> 16);
    float4 weights =
        float4(uint4(skin.weights[0] & 0xffff, skin.weights[0] >> 16, skin.weights[1] & 0xffff, skin.weights[1] >> 16));
    float total = weights.x + weights.y + weights.z + weights.w;
    if (total == 0)
        return frame;
    weights /= total;
    float4 r0 = float4(0, 0, 0, 0);
    float4 r1 = float4(0, 0, 0, 0);
    float4 r2 = float4(0, 0, 0, 0);
    for (uint i = 0; i < kPrRenderSkinInfluences; ++i) {
        if (weights[i] == 0)
            continue;
        uint joint = push.firstPose + min(joints[i], push.jointCount - 1) * kPrRenderJointBytes;
        r0 += weights[i] * deformPoseVector(joint);
        r1 += weights[i] * deformPoseVector(joint + 16);
        r2 += weights[i] * deformPoseVector(joint + 32);
    }
    float4 p = float4(frame.position, 1);
    float3 n = frame.normal;
    float3 t = frame.tangent;
    float3 c0 = cross(r1.xyz, r2.xyz);
    float sign = dot(r0.xyz, c0) < 0 ? -1.0 : 1.0;
    frame.position = float3(dot(r0, p), dot(r1, p), dot(r2, p));
    frame.normal = float3(dot(c0, n), dot(cross(r2.xyz, r0.xyz), n), dot(cross(r0.xyz, r1.xyz), n)) * sign;
    frame.tangent = float3(dot(r0.xyz, t), dot(r1.xyz, t), dot(r2.xyz, t));
    return frame;
}

// Writes one vertex of the instance: the mesh's vertex with its morph targets added and then its joints applied, as
// glTF orders them, and where the vertex was a frame before.
void deformOne(uint index) {
    PrRenderGeometryRecord vertex = deformVertex(index);
    DeformFrame bound;
    bound.position = float3(vertex.position[0], vertex.position[1], vertex.position[2]);
    bound.normal = deformUnpack(vertex.normal[0], vertex.normal[1]);
    bound.tangent = deformUnpack(vertex.tangent[0], vertex.tangent[1]);

    DeformFrame frame = bound;
    if (push.morphTargetCount != 0)
        frame = deformByMorphs(frame, index);
    if (push.jointCount != 0)
        frame = deformBySkin(frame, index);
    // A mesh without tangents stores zeros, which stay zeros.
    float3 normal = deformUnit(frame.normal, bound.normal);
    float3 tangent = deformUnit(frame.tangent, float3(0, 0, 0));

    // A vertex of the geometry lies at a multiple of 4 bytes, so it is written a word at a time.
    uint at = (push.firstDeformed + index) * kPrRenderGeometryBytes;
    uint previousAt = (push.firstDeformed + index) * kPrRenderPositionBytes;
    RWByteAddressBuffer deformed = deformDeformed();
    float3 previous = frame.position;
    if (push.keepsPrevious != 0)
        previous = asfloat(uint3(deformed.Load(at), deformed.Load(at + 4), deformed.Load(at + 8)));
    RWByteAddressBuffer previousPositions = deformPrevious();
    previousPositions.Store(previousAt, asuint(previous.x));
    previousPositions.Store(previousAt + 4, asuint(previous.y));
    previousPositions.Store(previousAt + 8, asuint(previous.z));
    deformed.Store(at, asuint(frame.position.x));
    deformed.Store(at + 4, asuint(frame.position.y));
    deformed.Store(at + 8, asuint(frame.position.z));
    deformed.Store(at + 12, deformPack(normal.x) | deformPack(normal.y) << 16);
    deformed.Store(at + 16, deformPack(normal.z));
    deformed.Store(at + 20, deformPack(tangent.x) | deformPack(tangent.y) << 16);
    deformed.Store(at + 24, deformPack(tangent.z) | (vertex.tangent[1] & 0xffff0000));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void deformVertices(uint3 id : SV_DispatchThreadID) {
    if (id.x < push.vertexCount)
        deformOne(id.x);
}
