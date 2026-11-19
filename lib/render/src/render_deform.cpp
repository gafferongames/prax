// A scene's deforming instances. An instance deforms once an update gives it joints or morph weights. While its mesh
// has a skin or morph targets, the instance has a range of the scene's deformed vertices and parts of its own that name
// that range, and a compute pass writes the range whenever the pose changes. Every other pass draws the instance's
// parts as it draws a mesh's, so nothing else knows of deformation.

#include "render/src/render_deform.h"

#include "pr/assert.h"
#include "pr/float.h"

#include "render/src/render_resource.h"
#include "render/src/render_scene.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Bounds

// A sphere that grows to hold other spheres: first their box, and then, about the box's center, the farthest that any
// of them reaches.
struct SphereUnion {
    PrVec3 low;
    PrVec3 high;
    bool any;
};

void addToBox(SphereUnion* box, PrVec3 center, float radius) {
    PrVec3 low = center - PrVec3{radius, radius, radius};
    PrVec3 high = center + PrVec3{radius, radius, radius};
    if (!box->any) {
        box->low = low;
        box->high = high;
        box->any = true;
    }
    box->low = {prF32Min(box->low.x, low.x), prF32Min(box->low.y, low.y), prF32Min(box->low.z, low.z)};
    box->high = {prF32Max(box->high.x, high.x), prF32Max(box->high.y, high.y), prF32Max(box->high.z, high.z)};
}

float reachFrom(PrVec3 from, PrVec3 center, float radius) {
    return prVec3Length(center - from) + radius;
}

// Returns a radius a little above `radius`, which holds what the device's arithmetic rounds otherwise than this file's.
float padded(float radius) {
    return radius * 1.001f + 1e-6f;
}

// Returns the farthest that the instance's morph targets move a vertex, at its weights.
float morphReach(const RenderMeshDeform& mesh, const RenderDeformation& d) {
    float reach = 0;
    for (uint32_t t = 0; t < mesh.morphTargetCount && t < d.weights.count; ++t)
        reach += prF32Abs(d.weights.data[t]) * mesh.morphReach[t];
    return reach;
}

// Returns a joint's matrix in the instance's pose, which is the identity for a joint that the pose does not hold.
PrAffine3 jointOf(const RenderDeformation& d, uint32_t joint) {
    return joint < d.joints.count ? d.joints.data[joint] : prAffine3Identity();
}

// Writes the bounds of the instance's parts in its pose.
//
// A skinned vertex lies at a weighted mean of where each of its joints would put it alone. A joint puts the vertices
// that it moves inside the image of their sphere, which the morph targets have first widened by their reach. So every
// vertex lies in a convex shape that holds the images of the joints' spheres, and one sphere about them all bounds
// every part of the instance. A mesh without a skin keeps each part's own sphere, widened by the morph targets' reach.
void writeBounds(Render* r, const RenderDeformation& d) {
    const RenderMeshState& mesh = r->meshes.data[d.mesh];
    const RenderMeshDeform& deform = mesh.deform;
    float reach = morphReach(deform, d);
    if (deform.jointCount == 0) {
        for (uint32_t i = 0; i < d.partCount; ++i) {
            PrRenderVec4 bounds = r->parts.data[mesh.firstPart + i].bounds;
            bounds.w = padded(bounds.w + reach);
            r->parts.data[d.firstPart + i].bounds = bounds;
        }
        renderPartsTouch(r, d.firstPart, d.partCount);
        return;
    }
    SphereUnion all{};
    for (uint32_t j = 0; j < deform.jointCount; ++j) {
        const PrRenderVec4& sphere = deform.jointBounds[j];
        if (sphere.w < 0)
            continue;
        PrAffine3 joint = jointOf(d, j);
        addToBox(&all, prAffine3MapPoint(&joint, PrVec3{sphere.x, sphere.y, sphere.z}),
            (sphere.w + reach) * prAffine3MaxScale(&joint));
    }
    PrVec3 center = (all.low + all.high) * 0.5f;
    float radius = 0;
    for (uint32_t j = 0; j < deform.jointCount; ++j) {
        const PrRenderVec4& sphere = deform.jointBounds[j];
        if (sphere.w < 0)
            continue;
        PrAffine3 joint = jointOf(d, j);
        PrVec3 at = prAffine3MapPoint(&joint, PrVec3{sphere.x, sphere.y, sphere.z});
        radius = prF32Max(radius, reachFrom(center, at, (sphere.w + reach) * prAffine3MaxScale(&joint)));
    }
    for (uint32_t i = 0; i < d.partCount; ++i) {
        // A vertex that no joint moves stays where it was bound, inside its part's own sphere.
        const PrRenderVec4& own = r->parts.data[mesh.firstPart + i].bounds;
        float whole = prF32Max(radius, reachFrom(center, PrVec3{own.x, own.y, own.z}, own.w + reach));
        r->parts.data[d.firstPart + i].bounds = PrRenderVec4{center.x, center.y, center.z, padded(whole)};
    }
    renderPartsTouch(r, d.firstPart, d.partCount);
}

// ---------------------------------------------------------------------------------------------------------------------
// Binding

// Returns the mesh of a deformation's instance if it has data that deforms, or null.
const RenderMeshState* PR_NULLABLE deformingMesh(const Render* r, const RenderSceneState* s, const RenderDeformation& d) {
    uint32_t id = s->records.data[d.instance].mesh;
    if (id == 0 || id >= r->meshes.count || !r->meshes.data[id].ready)
        return nullptr;
    const RenderMeshState& mesh = r->meshes.data[id];
    return mesh.deform.skin.id != 0 || mesh.deform.morphs.id != 0 ? &mesh : nullptr;
}

// Takes a deformation's vertices, poses and parts from it. Its instance then draws its mesh's own parts.
void unbind(Render* r, RenderSceneState* s, RenderDeformation* d) {
    if (!d->bound)
        return;
    renderRangesGive(&s->deformedRanges, d->firstVertex, d->vertexCount);
    renderRangesGive(&s->poseRanges, d->firstPose, d->poseCount);
    renderPartsGive(r, d->firstPart, d->partCount);
    d->bound = false;
    s->records.data[d->instance].previousPositions = 0;
    renderSceneTouchInstance(s, d->instance);
}

// Gives a deformation a range of the scene's vertices and poses for `mesh`, and parts that are the mesh's own until
// pointParts names the scene's vertices in them.
void bind(Render* r, RenderSceneState* s, RenderDeformation* d, const RenderMeshState& mesh) {
    const RenderMeshDeform& deform = mesh.deform;
    d->bound = true;
    d->mesh = s->records.data[d->instance].mesh;
    d->meshSerial = mesh.serial;
    d->vertexCount = deform.vertexCount;
    d->firstVertex = renderRangesTake(&s->deformedRanges, d->vertexCount);
    d->poseCount = deform.jointCount * 3 + (deform.morphTargetCount + 3) / 4;
    d->firstPose = renderRangesTake(&s->poseRanges, d->poseCount);
    renderArrayGrow(&s->poses, s->poseRanges.end);
    d->partCount = mesh.partCount;
    d->firstPart = renderPartsTake(r, d->partCount);
    // Taking parts may move them all, so the mesh's are found again.
    prMemCopy(r->parts.data + d->firstPart, r->parts.data + mesh.firstPart,
        size_t(d->partCount) * sizeof(PrRenderPartRecord));
    d->poseDirty = true;
    d->fresh = true;
    d->settling = false;
}

// Names the scene's deformed vertices in a deformation's parts, and their previous positions in its instance's record.
void pointParts(Render* r, RenderSceneState* s, const RenderDeformation& d) {
    for (uint32_t i = 0; i < d.partCount; ++i) {
        PrRenderPartRecord& part = r->parts.data[d.firstPart + i];
        part.geometry = s->deformedBuffer.buffer.id;
        part.firstVertex = d.firstVertex;
    }
    renderPartsTouch(r, d.firstPart, d.partCount);
    s->records.data[d.instance].previousPositions = s->previousBuffer.buffer.id;
    renderSceneTouchInstance(s, d.instance);
}

// Makes the scene's buffers of deformed vertices hold every bound deformation's range. A buffer that is made again
// holds nothing, so every deformation is then deformed again. Returns false if the device makes no such buffers.
bool reserveVertices(Render* r, RenderSceneState* s) {
    PrGpuBufferUsageFlags usage = PrGpuBufferUsageFlag_Vertex | PrGpuBufferUsageFlag_Storage;
    uint64_t count = s->deformedRanges.end;
    bool made = renderRecordBufferReserve(r, &s->deformedBuffer, count * kPrRenderGeometryBytes, usage,
        "render deformed");
    made |= renderRecordBufferReserve(r, &s->previousBuffer, count * kPrRenderPositionBytes,
        PrGpuBufferUsageFlag_Storage, "render previous positions");
    for (uint32_t i = 1; made && i < s->deformations.count; ++i) {
        RenderDeformation& d = s->deformations.data[i];
        d.fresh = true;
        d.poseDirty = true;
    }
    return s->deformedBuffer.buffer.id != 0 && s->previousBuffer.buffer.id != 0;
}

// Returns the deformation of an instance that is alive, which it makes if the instance has none.
RenderDeformation* PR_NULLABLE deformationOf(RenderSceneState* s, uint32_t id) {
    if (id == 0 || id >= s->instances.count || !s->instances.data[id].alive)
        return nullptr;
    RenderInstanceState& instance = s->instances.data[id];
    if (instance.deformation == 0) {
        // The first entry is no deformation.
        if (s->deformations.count == 0)
            s->deformations.push(RenderDeformation{});
        if (s->freeDeformations.count != 0) {
            instance.deformation = s->freeDeformations.last();
            s->freeDeformations.pop();
        } else {
            instance.deformation = s->deformations.count;
            s->deformations.push(RenderDeformation{});
        }
        s->deformations.data[instance.deformation] = RenderDeformation{.instance = id};
        s->drawsDirty = true;
    }
    return &s->deformations.data[instance.deformation];
}

// ---------------------------------------------------------------------------------------------------------------------
// The pass

// The deformation of one instance in a frame.
struct DeformItem {
    PrRenderDeformPush push;
    PrGpuBuffer vertices;
    PrGpuBuffer skin;
    PrGpuBuffer morphs;
};

// What the frame's deformation reads while the graph records it, in the graph's memory.
struct DeformWork {
    Render* render;
    RenderSceneState* scene;
    uint32_t count;
    DeformItem* items;
};

// Records a dispatch for each instance whose vertices are behind its pose.
void recordDeform(void* PR_NULLABLE context, PrGpuCommands* commands) {
    const DeformWork& work = *static_cast<const DeformWork*>(context);
    const Render* r = work.render;
    const RenderSceneState* s = work.scene;
    prGpuCommandsSetPipeline(commands, r->deform);
    uint32_t groupSize = prGpuPipelineGroupSize(r->device, r->deform).width;
    const RenderBindings& b = r->bindings[RenderLibrary_Deform];
    if (!r->bindless) {
        prGpuCommandsSetBuffer(commands, b.pose, s->poseBuffer.buffer, 0);
        prGpuCommandsSetRwBuffer(commands, b.deformed, s->deformedBuffer.buffer, 0);
        prGpuCommandsSetRwBuffer(commands, b.previous, s->previousBuffer.buffer, 0);
    }
    for (uint32_t i = 0; i < work.count; ++i) {
        const DeformItem& item = work.items[i];
        if (!r->bindless) {
            // A slot that the mesh has nothing for holds the vertices, which the dispatch then does not read there.
            prGpuCommandsSetBuffer(commands, b.vertices, item.vertices, 0);
            prGpuCommandsSetBuffer(commands, b.skin, item.skin.id != 0 ? item.skin : item.vertices, 0);
            prGpuCommandsSetBuffer(commands, b.morphs, item.morphs.id != 0 ? item.morphs : item.vertices, 0);
        }
        prGpuCommandsSetPush(commands, &item.push, sizeof(item.push));
        prGpuCommandsDispatch(commands, PrGpuExtent{(item.push.vertexCount + groupSize - 1) / groupSize, 1, 1});
    }
    if (!r->bindless) {
        prGpuCommandsSetRwBuffer(commands, b.deformed, PrGpuBuffer{}, 0);
        prGpuCommandsSetRwBuffer(commands, b.previous, PrGpuBuffer{}, 0);
    }
}

} // namespace

void renderDeformSetJoints(RenderSceneState* scene, uint32_t instance, const PrAffine3* joints, uint32_t count) {
    RenderDeformation* d = deformationOf(scene, instance);
    if (d == nullptr)
        return;
    d->joints.resize(count);
    if (count != 0)
        prMemCopy(d->joints.data, joints, size_t(count) * sizeof(PrAffine3));
    d->poseDirty = true;
}

void renderDeformSetWeights(RenderSceneState* scene, uint32_t instance, const float* weights, uint32_t count) {
    RenderDeformation* d = deformationOf(scene, instance);
    if (d == nullptr)
        return;
    d->weights.resize(count);
    if (count != 0)
        prMemCopy(d->weights.data, weights, size_t(count) * sizeof(float));
    d->poseDirty = true;
}

void renderDeformRemove(Render* render, RenderSceneState* scene, uint32_t instance) {
    uint32_t index = scene->instances.data[instance].deformation;
    if (index == 0)
        return;
    RenderDeformation& d = scene->deformations.data[index];
    unbind(render, scene, &d);
    d.joints.reset();
    d.weights.reset();
    d = RenderDeformation{};
    scene->freeDeformations.push(index);
}

void renderDeformBind(Render* render, RenderSceneState* scene) {
    Render* r = render;
    RenderSceneState* s = scene;
    for (uint32_t i = 1; i < s->deformations.count; ++i) {
        RenderDeformation& d = s->deformations.data[i];
        if (d.instance == 0)
            continue;
        const RenderMeshState* mesh = deformingMesh(r, s, d);
        bool same = mesh != nullptr && d.mesh == s->records.data[d.instance].mesh && d.meshSerial == mesh->serial;
        if (d.bound && !same)
            unbind(r, s, &d);
        if (mesh != nullptr && !d.bound)
            bind(r, s, &d, *mesh);
    }
    if (s->deformedRanges.end == 0)
        return;
    bool made = reserveVertices(r, s);
    for (uint32_t i = 1; i < s->deformations.count; ++i) {
        RenderDeformation& d = s->deformations.data[i];
        // Without the buffers, every instance draws its mesh as it was bound.
        if (d.bound && !made)
            unbind(r, s, &d);
        else if (d.bound && d.fresh)
            pointParts(r, s, d);
    }
}

void renderDeformUpload(Render* render, RenderSceneState* scene) {
    Render* r = render;
    RenderSceneState* s = scene;
    for (uint32_t i = 1; i < s->deformations.count; ++i) {
        const RenderDeformation& d = s->deformations.data[i];
        if (!d.bound || !d.poseDirty)
            continue;
        const RenderMeshDeform& mesh = r->meshes.data[d.mesh].deform;
        PrRenderVec4* pose = s->poses.data + d.firstPose;
        prMemZero(pose, size_t(d.poseCount) * sizeof(PrRenderVec4));
        for (uint32_t j = 0; j < mesh.jointCount; ++j) {
            PrAffine3 joint = jointOf(d, j);
            for (uint32_t row = 0; row < 3; ++row)
                pose[j * 3 + row] = {joint.m[row][0], joint.m[row][1], joint.m[row][2], joint.m[row][3]};
        }
        float* weights = &pose[mesh.jointCount * 3].x;
        for (uint32_t t = 0; t < mesh.morphTargetCount && t < d.weights.count; ++t)
            weights[t] = d.weights.data[t];
        s->poseDirtyFirst = d.firstPose < s->poseDirtyFirst ? d.firstPose : s->poseDirtyFirst;
        uint32_t last = d.firstPose + d.poseCount - 1;
        s->poseDirtyLast = last > s->poseDirtyLast ? last : s->poseDirtyLast;
        writeBounds(r, d);
    }
    if (s->poses.count == 0)
        return;
    uint64_t size = uint64_t(s->poses.count) * sizeof(PrRenderVec4);
    if (renderRecordBufferReserve(r, &s->poseBuffer, size, PrGpuBufferUsageFlag_Storage, "render poses")) {
        s->poseDirtyFirst = 0;
        s->poseDirtyLast = s->poses.count - 1;
    }
    if (s->poseBuffer.buffer.id != 0 && s->poseDirtyFirst <= s->poseDirtyLast) {
        uint64_t offset = uint64_t(s->poseDirtyFirst) * sizeof(PrRenderVec4);
        uint64_t bytes = uint64_t(s->poseDirtyLast - s->poseDirtyFirst + 1) * sizeof(PrRenderVec4);
        prGpuBufferUpload(r->device, s->poseBuffer.buffer, offset, s->poses.data + s->poseDirtyFirst, bytes);
    }
    s->poseDirtyFirst = 0xffffffffu;
    s->poseDirtyLast = 0;
}

void renderDeformAddPass(Render* render, RenderSceneState* scene, RenderSceneFrame* frame) {
    Render* r = render;
    RenderSceneState* s = scene;
    if (s->deformedRanges.end == 0 || s->deformedBuffer.buffer.id == 0 || s->poseBuffer.buffer.id == 0)
        return;
    frame->deformed = prGpuGraphImportBuffer(r->graph, s->deformedBuffer.buffer, PrGpuAccessFlag_ShaderRead);
    frame->previousPositions = prGpuGraphImportBuffer(r->graph, s->previousBuffer.buffer, PrGpuAccessFlag_ShaderRead);

    // An instance is deformed in the frame after its pose changes, and once more in the frame after that, which brings
    // its previous positions up to where it stopped.
    uint32_t count = 0;
    for (uint32_t i = 1; i < s->deformations.count; ++i) {
        const RenderDeformation& d = s->deformations.data[i];
        count += d.bound && (d.poseDirty || d.settling);
    }
    if (count == 0)
        return;
    DeformWork* work = static_cast<DeformWork*>(prGpuGraphAlloc(r->graph, sizeof(DeformWork)));
    DeformItem* items = static_cast<DeformItem*>(prGpuGraphAlloc(r->graph, size_t(count) * sizeof(DeformItem)));
    *work = DeformWork{.render = r, .scene = s, .count = count, .items = items};
    uint32_t deformedHandle = 0;
    uint32_t previousHandle = 0;
    if (r->bindless) {
        deformedHandle = prGpuBufferStorageHandle(r->device, s->deformedBuffer.buffer);
        previousHandle = prGpuBufferStorageHandle(r->device, s->previousBuffer.buffer);
    }
    uint32_t next = 0;
    for (uint32_t i = 1; i < s->deformations.count; ++i) {
        RenderDeformation& d = s->deformations.data[i];
        if (!d.bound || !(d.poseDirty || d.settling))
            continue;
        const RenderMeshState& mesh = r->meshes.data[d.mesh];
        items[next++] = DeformItem{
            .push = {
                .vertexCount = d.vertexCount,
                .firstDeformed = d.firstVertex,
                .jointCount = mesh.deform.jointCount,
                .morphTargetCount = mesh.deform.morphTargetCount,
                .firstPose = d.firstPose * uint32_t(sizeof(PrRenderVec4)),
                .keepsPrevious = d.fresh ? 0u : 1u,
                .vertices = mesh.geometry.id,
                .skin = mesh.deform.skin.id,
                .morphs = mesh.deform.morphs.id,
                .pose = s->poseBuffer.buffer.id,
                .deformed = deformedHandle,
                .previousPositions = previousHandle,
            },
            .vertices = mesh.geometry,
            .skin = mesh.deform.skin,
            .morphs = mesh.deform.morphs,
        };
        d.settling = d.poseDirty;
        d.poseDirty = false;
        d.fresh = false;
    }

    PrGpuGraphBuffer pose = prGpuGraphImportBuffer(r->graph, s->poseBuffer.buffer, PrGpuAccessFlag_ShaderRead);
    PrGpuGraphPass pass = prGpuGraphAddWork(r->graph, "render deform", recordDeform, work);
    prGpuGraphUseBuffer(r->graph, pass, pose, PrGpuAccessFlag_ComputeRead);
    prGpuGraphUseBuffer(r->graph, pass, frame->deformed, PrGpuAccessFlag_ComputeWrite);
    prGpuGraphUseBuffer(r->graph, pass, frame->previousPositions, PrGpuAccessFlag_ComputeWrite);
    // The vertices are a result of the frame whether or not a view draws them, and rest in the accesses of shader reads
    // between frames.
    prGpuGraphLeaveBuffer(r->graph, frame->deformed, PrGpuAccessFlag_ShaderRead);
    prGpuGraphLeaveBuffer(r->graph, frame->previousPositions, PrGpuAccessFlag_ShaderRead);
}

void renderDeformReset(Render* render, RenderSceneState* scene) {
    for (uint32_t i = 1; i < scene->deformations.count; ++i) {
        RenderDeformation& d = scene->deformations.data[i];
        if (d.bound)
            renderPartsGive(render, d.firstPart, d.partCount);
        d.joints.reset();
        d.weights.reset();
    }
    scene->deformations.reset();
    scene->freeDeformations.reset();
    renderRangesReset(&scene->deformedRanges);
    renderRangesReset(&scene->poseRanges);
    scene->poses.reset();
    prGpuBufferRelease(render->device, scene->deformedBuffer.buffer);
    prGpuBufferRelease(render->device, scene->previousBuffer.buffer);
    prGpuBufferRelease(render->device, scene->poseBuffer.buffer);
}

} // namespace pr_impl
