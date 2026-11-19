// A renderer's scenes: the updates that producers fill on any thread, and the instances, lights and draws that the
// frame thread keeps and uploads when a frame begins.

#include "pr/render_scene.h"
#include "render/src/render_scene.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/sort.h"
#include "pr/span.h"

#include "render/src/render.h"
#include "render/src/render_deform.h"
#include "render/src/render_resource.h"
#include "render/src/render_view.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Commands

// The changes that an update holds. Each is a structure of 32-bit words, which the words that its comment names follow.
enum {
    CommandKind_InstanceAdd,
    CommandKind_InstanceRemove,
    CommandKind_InstanceTransforms,
    CommandKind_InstanceJoints,
    CommandKind_InstanceWeights,
    CommandKind_InstanceMaterial,
    CommandKind_InstanceMasks,
    CommandKind_LightSet,
    CommandKind_LightRemove,
    CommandKind_Environment,
    CommandKind_Time,
};
typedef uint32_t CommandKind;

struct CommandHeader {
    CommandKind kind;
    uint32_t words;             // the command's length, with this header and what follows the structure
};

// Followed by the instance's materials.
struct InstanceAddCommand {
    CommandHeader header;
    uint32_t id;
    uint32_t mesh;
    uint32_t mobility;
    uint32_t visibility;
    uint32_t materialCount;
    PrAffine3 world;
};

// Followed by `count` instance ids, and then `count` transforms.
struct InstanceTransformsCommand {
    CommandHeader header;
    uint32_t count;
};

// Sets an instance's joints or its morph weights. Followed by `count` transforms, or `count` floats.
struct InstancePoseCommand {
    CommandHeader header;
    uint32_t id;
    uint32_t count;
};

struct InstanceMaterialCommand {
    CommandHeader header;
    uint32_t id;
    uint32_t part;
    uint32_t material;
};

struct InstanceMasksCommand {
    CommandHeader header;
    uint32_t id;
    uint32_t mobility;
    uint32_t visibility;
};

// Removes an instance or a light.
struct RemoveCommand {
    CommandHeader header;
    uint32_t id;
};

struct LightSetCommand {
    CommandHeader header;
    uint32_t id;
    PrRenderLightDesc desc;
};

struct EnvironmentCommand {
    CommandHeader header;
    PrRenderSceneEnvironment environment;
};

struct TimeCommand {
    CommandHeader header;
    float seconds;
};

// Appends a command of type T and `extraWords` more words to an update, and returns it with its header filled in.
template <typename T>
T* appendCommand(RenderUpdate* update, CommandKind kind, uint32_t extraWords = 0) {
    static_assert(sizeof(T) % 4 == 0 && alignof(T) <= 4);
    uint32_t words = uint32_t(sizeof(T) / 4) + extraWords;
    uint32_t at = update->words.count;
    update->words.resize(at + words);
    T* command = reinterpret_cast<T*>(update->words.data + at);
    prMemZero(command, sizeof(T));
    command->header = CommandHeader{.kind = kind, .words = words};
    return command;
}

// ---------------------------------------------------------------------------------------------------------------------
// Applying updates

void setWorld(PrRenderVec4* rows, const PrAffine3& world) {
    for (uint32_t row = 0; row < 3; ++row)
        rows[row] = {world.m[row][0], world.m[row][1], world.m[row][2], world.m[row][3]};
}

// Returns true if a matrix reflects, which reverses the winding of the triangles that it maps.
bool reflects(const PrRenderVec4* w) {
    float determinant = w[0].x * (w[1].y * w[2].z - w[1].z * w[2].y) - w[0].y * (w[1].x * w[2].z - w[1].z * w[2].x) +
        w[0].z * (w[1].x * w[2].y - w[1].y * w[2].x);
    return determinant < 0;
}

RenderInstanceState* aliveInstance(RenderSceneState* s, uint32_t id) {
    if (id == 0 || id >= s->instances.count || !s->instances.data[id].alive)
        return nullptr;
    return &s->instances.data[id];
}

void addInstance(RenderSceneState* s, const InstanceAddCommand& command, const uint32_t* materials) {
    if (command.id == 0 || command.id >= s->instances.count || s->instances.data[command.id].alive)
        return;
    PrRenderInstanceRecord& record = s->records.data[command.id];
    record = PrRenderInstanceRecord{
        .mesh = command.mesh,
        .mobility = command.mobility,
        .visibility = command.visibility,
    };
    setWorld(record.world, command.world);
    setWorld(record.previousWorld, command.world);
    s->instances.data[command.id] = RenderInstanceState{
        .alive = true,
        .mirrored = reflects(record.world),
        .firstMaterial = s->materials.count,
        .materialCount = command.materialCount,
    };
    s->materials.append(materials, command.materialCount);
    ++s->instanceCount;
    s->drawsDirty = true;
    renderSceneTouchInstance(s, command.id);
}

void removeInstance(Render* r, RenderSceneState* s, uint32_t id, RawArray<uint32_t>* freed) {
    RenderInstanceState* instance = aliveInstance(s, id);
    if (instance == nullptr)
        return;
    renderDeformRemove(r, s, id);
    s->materialsWasted += instance->materialCount;
    *instance = RenderInstanceState{};
    s->records.data[id] = PrRenderInstanceRecord{};
    --s->instanceCount;
    s->drawsDirty = true;
    renderSceneTouchInstance(s, id);
    freed->push(id);
}

void setTransform(RenderSceneState* s, uint32_t id, const PrAffine3& world) {
    RenderInstanceState* instance = aliveInstance(s, id);
    if (instance == nullptr)
        return;
    PrRenderInstanceRecord& record = s->records.data[id];
    // The first change of a frame keeps the transform of the frame before. A later one replaces only the new transform.
    if (!instance->moved) {
        instance->moved = true;
        s->moved.push(id);
        for (uint32_t row = 0; row < 3; ++row)
            record.previousWorld[row] = record.world[row];
    }
    setWorld(record.world, world);
    bool mirrored = reflects(record.world);
    if (mirrored != instance->mirrored) {
        instance->mirrored = mirrored;
        s->drawsDirty = true;
    }
    renderSceneTouchInstance(s, id);
}

void setMaterial(RenderSceneState* s, const InstanceMaterialCommand& command) {
    RenderInstanceState* instance = aliveInstance(s, command.id);
    if (instance == nullptr)
        return;
    if (command.part >= instance->materialCount) {
        // The instance's materials move to the end of the pool, where they can grow.
        uint32_t first = s->materials.count;
        s->materials.resize(first + command.part + 1);
        for (uint32_t i = 0; i <= command.part; ++i)
            s->materials.data[first + i] = i < instance->materialCount ? s->materials.data[instance->firstMaterial + i] : 0;
        s->materialsWasted += instance->materialCount;
        instance->firstMaterial = first;
        instance->materialCount = command.part + 1;
    }
    s->materials.data[instance->firstMaterial + command.part] = command.material;
    s->drawsDirty = true;
}

void setLight(RenderSceneState* s, const LightSetCommand& command) {
    if (command.id == 0 || command.id >= s->lights.count)
        return;
    RenderLightState& light = s->lights.data[command.id];
    if (!light.alive)
        ++s->lightCount;
    light.alive = true;
    light.desc = command.desc;
}

void removeLight(RenderSceneState* s, uint32_t id, RawArray<uint32_t>* freed) {
    if (id == 0 || id >= s->lights.count || !s->lights.data[id].alive)
        return;
    s->lights.data[id] = RenderLightState{};
    --s->lightCount;
    freed->push(id);
}

// Applies an update's commands to its scene, in order. The ids of the instances and lights that it removes are appended
// to `freedInstances` and `freedLights`.
void applyUpdate(const RenderUpdate& update, RawArray<uint32_t>* freedInstances, RawArray<uint32_t>* freedLights) {
    RenderSceneState* s = update.scene;
    const uint32_t* words = update.words.data;
    for (uint32_t at = 0; at < update.words.count;) {
        const CommandHeader* header = reinterpret_cast<const CommandHeader*>(words + at);
        const void* command = header;
        switch (header->kind) {
            case CommandKind_InstanceAdd: {
                const InstanceAddCommand* add = static_cast<const InstanceAddCommand*>(command);
                addInstance(s, *add, words + at + sizeof(InstanceAddCommand) / 4);
                break;
            }
            case CommandKind_InstanceRemove:
                removeInstance(update.render, s, static_cast<const RemoveCommand*>(command)->id, freedInstances);
                break;
            case CommandKind_InstanceJoints: {
                const InstancePoseCommand* pose = static_cast<const InstancePoseCommand*>(command);
                const uint32_t* joints = words + at + sizeof(InstancePoseCommand) / 4;
                renderDeformSetJoints(s, pose->id, reinterpret_cast<const PrAffine3*>(joints), pose->count);
                break;
            }
            case CommandKind_InstanceWeights: {
                const InstancePoseCommand* pose = static_cast<const InstancePoseCommand*>(command);
                const uint32_t* weights = words + at + sizeof(InstancePoseCommand) / 4;
                renderDeformSetWeights(s, pose->id, reinterpret_cast<const float*>(weights), pose->count);
                break;
            }
            case CommandKind_InstanceTransforms: {
                uint32_t count = static_cast<const InstanceTransformsCommand*>(command)->count;
                const uint32_t* ids = words + at + sizeof(InstanceTransformsCommand) / 4;
                const PrAffine3* worlds = reinterpret_cast<const PrAffine3*>(ids + count);
                for (uint32_t i = 0; i < count; ++i)
                    setTransform(s, ids[i], worlds[i]);
                break;
            }
            case CommandKind_InstanceMaterial:
                setMaterial(s, *static_cast<const InstanceMaterialCommand*>(command));
                break;
            case CommandKind_InstanceMasks: {
                const InstanceMasksCommand* masks = static_cast<const InstanceMasksCommand*>(command);
                if (aliveInstance(s, masks->id) != nullptr) {
                    s->records.data[masks->id].mobility = masks->mobility;
                    s->records.data[masks->id].visibility = masks->visibility;
                    renderSceneTouchInstance(s, masks->id);
                }
                break;
            }
            case CommandKind_LightSet:
                setLight(s, *static_cast<const LightSetCommand*>(command));
                break;
            case CommandKind_LightRemove:
                removeLight(s, static_cast<const RemoveCommand*>(command)->id, freedLights);
                break;
            case CommandKind_Environment:
                s->environment = static_cast<const EnvironmentCommand*>(command)->environment;
                break;
            case CommandKind_Time:
                s->time = static_cast<const TimeCommand*>(command)->seconds;
                break;
        }
        at += header->words;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Draws

// A draw, the bucket that it is drawn in, and the type of its material.
struct BucketDraw {
    PrRenderDrawRecord draw;
    uint32_t bucket;
    uint32_t variant;
};

// Orders draws by bucket, and within a bucket so that a pass changes as little as it can between draws: the pipeline of
// a variant of a material type in the forward technique, then a material's textures on a device without bindless, and
// then a mesh's buffers.
bool drawPrecedes(const BucketDraw& a, const BucketDraw& b) {
    if (a.bucket != b.bucket)
        return a.bucket < b.bucket;
    if (a.variant != b.variant)
        return a.variant < b.variant;
    if (a.draw.material != b.draw.material)
        return a.draw.material < b.draw.material;
    if (a.draw.part != b.draw.part)
        return a.draw.part < b.draw.part;
    return a.draw.instance < b.draw.instance;
}

// Drops the materials of removed instances from the pool, once they are most of it.
void compactMaterials(RenderSceneState* s) {
    if (s->materialsWasted < 64 || s->materialsWasted * 2 < s->materials.count)
        return;
    RawArray<uint32_t> kept{};
    kept.reserve(s->materials.count - s->materialsWasted);
    for (uint32_t id = 1; id < s->instances.count; ++id) {
        RenderInstanceState& instance = s->instances.data[id];
        if (!instance.alive)
            continue;
        uint32_t first = kept.count;
        kept.append(s->materials.data + instance.firstMaterial, instance.materialCount);
        instance.firstMaterial = first;
    }
    s->materials.reset();
    s->materials = kept;
    s->materialsWasted = 0;
}

// Returns the first of the parts that an instance draws: its own if it deforms, or its mesh's.
uint32_t firstPartOf(const RenderSceneState* s, const RenderInstanceState& instance, const RenderMeshState& mesh) {
    if (instance.deformation != 0 && s->deformations.data[instance.deformation].bound)
        return s->deformations.data[instance.deformation].firstPart;
    return mesh.firstPart;
}

// Builds the scene's draws again: one for each part of each instance whose mesh has data, in the order of the buckets.
// Each draw reads the mesh's attributes in the layout of its material's type, or the layout's fallbacks if the mesh's
// data has no stream of that layout.
void buildDraws(Render* r, RenderSceneState* s) {
    RawArray<BucketDraw> sorted{};
    s->fallbackDraws = 0;
    for (uint32_t id = 1; id < s->instances.count; ++id) {
        const RenderInstanceState& instance = s->instances.data[id];
        uint32_t meshId = s->records.data[id].mesh;
        if (!instance.alive || meshId == 0 || meshId >= r->meshes.count || !r->meshes.data[meshId].ready)
            continue;
        uint32_t firstPart = firstPartOf(s, instance, r->meshes.data[meshId]);
        uint32_t partCount = r->meshes.data[meshId].partCount;
        for (uint32_t part = 0; part < partCount; ++part) {
            uint32_t material = part < instance.materialCount ? s->materials.data[instance.firstMaterial + part] : 0;
            if (material >= r->materialRecords.count)
                material = 0;
            uint32_t materialFlags = r->materialRecords.data[material].flags;
            uint32_t alpha = materialFlags & kPrRenderMaterialAlphaMode;
            bool doubleSided = (materialFlags & kPrRenderMaterialDoubleSided) != 0;
            uint32_t flags = 0;
            if (alpha == kPrRenderMaterialMask)
                flags |= kPrRenderDrawMask;
            else if (alpha == kPrRenderMaterialBlend)
                flags |= kPrRenderDrawBlend;
            // A material that reads the scene behind it is drawn after that scene, with the blended draws.
            bool behind = (materialFlags & kPrRenderMaterialBehind) != 0;
            if (behind) {
                flags |= kPrRenderDrawBlend | kPrRenderDrawBehind;
                alpha = kPrRenderMaterialBlend;
            }
            if (doubleSided)
                flags |= kPrRenderDrawDoubleSided;
            if (instance.mirrored)
                flags |= kPrRenderDrawMirrored;
            RenderCullFlags cull = 0;
            if (doubleSided)
                cull |= RenderCullFlag_DoubleSided;
            if (instance.mirrored)
                cull |= RenderCullFlag_Mirrored;
            uint32_t alphaClass = alpha <= kPrRenderMaterialBlend ? alpha : 0;
            uint32_t variant = r->materialRecords.data[material].variant;
            bool fallback = false;
            PrGpuBuffer attributes = renderMeshStream(r, meshId, r->shaders->variants[variant].layout, &fallback);
            s->fallbackDraws += fallback ? 1 : 0;
            sorted.push(BucketDraw{
                .draw = {
                    .instance = id,
                    .part = firstPart + part,
                    .material = material,
                    .flags = flags,
                    .attributes = attributes.id,
                },
                .bucket = alphaClass * kRenderCullCount + cull,
                .variant = variant,
            });
        }
    }
    sortItems(Span<BucketDraw>(sorted.data, sorted.count), drawPrecedes);
    s->draws.resize(sorted.count);
    uint32_t bucket = 0;
    s->bucketFirst[0] = 0;
    s->behindDraws = 0;
    for (uint32_t i = 0; i < sorted.count; ++i) {
        while (bucket < sorted.data[i].bucket)
            s->bucketFirst[++bucket] = i;
        s->draws.data[i] = sorted.data[i].draw;
        if ((sorted.data[i].draw.flags & kPrRenderDrawBehind) != 0)
            ++s->behindDraws;
    }
    while (bucket < kRenderBucketCount)
        s->bucketFirst[++bucket] = sorted.count;
    sorted.reset();
}

// Brings a scene's buffers up to date with its records and draws.
void uploadScene(Render* r, RenderSceneState* s) {
    compactMaterials(s);
    if (s->drawsDirty) {
        renderDeformBind(r, s);
        buildDraws(r, s);
        uint64_t size = uint64_t(s->draws.count) * sizeof(PrRenderDrawRecord);
        renderRecordBufferReserve(r, &s->drawBuffer, size, PrGpuBufferUsageFlag_Storage, "render draws");
        if (s->drawBuffer.buffer.id != 0 && size != 0)
            prGpuBufferUpload(r->device, s->drawBuffer.buffer, 0, s->draws.data, size);
        s->drawsDirty = false;
    }
    renderDeformUpload(r, s);
    uint64_t size = uint64_t(s->records.count) * sizeof(PrRenderInstanceRecord);
    if (renderRecordBufferReserve(r, &s->instanceBuffer, size, PrGpuBufferUsageFlag_Storage, "render instances")) {
        s->dirtyFirst = 0;
        s->dirtyLast = s->records.count - 1;
    }
    if (s->instanceBuffer.buffer.id != 0 && s->dirtyFirst <= s->dirtyLast) {
        uint64_t offset = uint64_t(s->dirtyFirst) * sizeof(PrRenderInstanceRecord);
        uint64_t bytes = uint64_t(s->dirtyLast - s->dirtyFirst + 1) * sizeof(PrRenderInstanceRecord);
        prGpuBufferUpload(r->device, s->instanceBuffer.buffer, offset, s->records.data + s->dirtyFirst, bytes);
    }
    s->dirtyFirst = 0xffffffffu;
    s->dirtyLast = 0;
}

// Ends the frame in which each moved instance moved: its previous transform becomes its transform.
void settleMoved(RenderSceneState* s) {
    for (uint32_t i = 0; i < s->moved.count; ++i) {
        uint32_t id = s->moved.data[i];
        if (!s->instances.data[id].alive)
            continue;
        s->instances.data[id].moved = false;
        PrRenderInstanceRecord& record = s->records.data[id];
        for (uint32_t row = 0; row < 3; ++row)
            record.previousWorld[row] = record.world[row];
        renderSceneTouchInstance(s, id);
    }
    s->moved.count = 0;
}

void giveIds(RenderIds* ids, const RawArray<uint32_t>& freed) {
    for (uint32_t i = 0; i < freed.count; ++i)
        renderIdsGive(ids, freed.data[i]);
}

// Returns the luminance of a linear sRGB color (ITU-R BT.709's weights).
float luminanceOf(PrVec3 color) {
    return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

// Returns the luminance of the illuminance that a light gives a plane that faces it at `point`, and writes the unit
// direction from the point toward the light, with the falloff that the shaders give it (renderLightIrradiance).
float lightIlluminance(const PrRenderLightDesc& light, PrVec3 point, PrVec3* toLight) {
    float intensity = luminanceOf(light.color) * light.intensity;
    float length = prVec3Length(light.direction);
    PrVec3 direction = length > 0 ? light.direction / length : PrVec3{0, -1, 0};
    if (light.kind == PrRenderLightKind_Directional) {
        *toLight = -direction;
        return intensity;
    }
    PrVec3 offset = light.position - point;
    float distance2 = prF32Max(prVec3Dot(offset, offset), 1e-8f);
    float distance = prF32Sqrt(distance2);
    *toLight = offset / distance;
    float falloff = 1 / distance2;
    if (light.range > 0) {
        float ratio2 = distance2 / (light.range * light.range);
        falloff *= prF32Clamp(1 - ratio2 * ratio2, 0, 1);
    }
    if (light.kind == PrRenderLightKind_Spot) {
        float outer = prF32Cos(light.outerConeAngle);
        float scale = 1 / prF32Max(0.001f, prF32Cos(light.innerConeAngle) - outer);
        float cone = prF32Clamp((prVec3Dot(direction, -*toLight) - outer) * scale, 0, 1);
        falloff *= cone * cone;
    }
    return intensity * falloff;
}

} // namespace

void renderSceneTouchInstance(RenderSceneState* scene, uint32_t id) {
    if (id < scene->dirtyFirst)
        scene->dirtyFirst = id;
    if (id > scene->dirtyLast)
        scene->dirtyLast = id;
}

void renderScenesApply(Render* render, RawArray<RenderUpdate*>* updates) {
    Render* r = render;
    // Each scene's tables grow to hold the ids that updates have taken, before any update is applied.
    for (uint32_t i = 1; i < r->frameScenes.count; ++i) {
        RenderSceneState* s = r->frameScenes.data[i];
        if (s == nullptr)
            continue;
        prMutexLock(&r->lock);
        uint32_t instanceCount = s->instanceIds.count;
        uint32_t lightCount = s->lightIds.count;
        prMutexUnlock(&r->lock);
        renderTableGrow(&s->instances, instanceCount);
        renderTableGrow(&s->records, instanceCount);
        renderTableGrow(&s->lights, lightCount);
        settleMoved(s);
    }

    RawArray<uint32_t> freedInstances{};
    RawArray<uint32_t> freedLights{};
    for (uint32_t i = 0; i < updates->count; ++i) {
        RenderUpdate* update = updates->data[i];
        applyUpdate(*update, &freedInstances, &freedLights);
        // An id is given back only once its removal is applied, so that it names one object at a time.
        prMutexLock(&r->lock);
        giveIds(&update->scene->instanceIds, freedInstances);
        giveIds(&update->scene->lightIds, freedLights);
        prMutexUnlock(&r->lock);
        freedInstances.count = 0;
        freedLights.count = 0;
        renderUpdateDestroy(update);
    }
    freedInstances.reset();
    freedLights.reset();

    for (uint32_t i = 1; i < r->frameScenes.count; ++i) {
        RenderSceneState* s = r->frameScenes.data[i];
        if (s == nullptr)
            continue;
        prMutexLock(&r->lock);
        bool destroyed = s->destroyed;
        if (destroyed) {
            r->scenes.data[i] = nullptr;
            renderIdsGive(&r->sceneIds, i);
        }
        prMutexUnlock(&r->lock);
        if (destroyed) {
            r->frameScenes.data[i] = nullptr;
            renderSceneStateDestroy(r, s);
        } else {
            uploadScene(r, s);
        }
    }
}

void renderSceneStateDestroy(Render* render, RenderSceneState* PR_NULLABLE scene) {
    if (scene == nullptr)
        return;
    renderDeformReset(render, scene);
    prGpuBufferRelease(render->device, scene->instanceBuffer.buffer);
    prGpuBufferRelease(render->device, scene->drawBuffer.buffer);
    for (const RenderRecordBuffer& arguments : scene->arguments)
        prGpuBufferRelease(render->device, arguments.buffer);
    prGpuTextureRelease(render->device, scene->shadowMap);
    renderIdsReset(&scene->instanceIds);
    renderIdsReset(&scene->lightIds);
    scene->instances.reset();
    scene->records.reset();
    scene->materials.reset();
    scene->moved.reset();
    scene->lights.reset();
    scene->draws.reset();
    prMemoryFree(scene);
}

void renderUpdateDestroy(RenderUpdate* PR_NULLABLE update) {
    if (update == nullptr)
        return;
    update->words.reset();
    update->addedInstances.reset();
    update->addedLights.reset();
    prMemoryFree(static_cast<PrRenderUpdate*>(update));
}

} // namespace pr_impl

using namespace pr_impl;

PrRenderInstanceDesc prRenderInstanceDescDefault(PrRenderMesh mesh) {
    return PrRenderInstanceDesc{
        .mesh = mesh,
        .world = prAffine3Identity(),
        .mobility = PrRenderMobilityFlag_Static,
        .visibility = 0xffffffffu,
    };
}

PrRenderLightDesc prRenderLightDescDefault(void) {
    return PrRenderLightDesc{
        .kind = PrRenderLightKind_Directional,
        .color = {1, 1, 1},
        .intensity = 1,
        .direction = {0, -1, 0},
        .outerConeAngle = 0.78539816f,
        .angularRadius = kPrRenderSunAngularRadius,
    };
}

PrRenderSceneEnvironment prRenderSceneEnvironmentDefault(PrRenderEnvironment environment) {
    return PrRenderSceneEnvironment{.environment = environment, .rotation = prAffine3Identity(), .intensity = 1};
}

PrRenderScene prRenderSceneCreate(PrRender* render) {
    Render* r = render;
    RenderSceneState* scene = allocZeroed<RenderSceneState>();
    scene->environment = prRenderSceneEnvironmentDefault(PrRenderEnvironment{});
    scene->dirtyFirst = 0xffffffffu;
    scene->poseDirtyFirst = 0xffffffffu;
    scene->drawsDirty = true;
    prMutexLock(&r->lock);
    scene->id = renderIdsTake(&r->sceneIds);
    renderTableGrow(&r->scenes, scene->id);
    r->scenes.data[scene->id] = scene;
    prMutexUnlock(&r->lock);
    return PrRenderScene{scene->id};
}

void prRenderSceneDestroy(PrRender* render, PrRenderScene scene) {
    Render* r = render;
    prMutexLock(&r->lock);
    if (scene.id != 0 && scene.id < r->scenes.count && r->scenes.data[scene.id] != nullptr)
        r->scenes.data[scene.id]->destroyed = true;
    prMutexUnlock(&r->lock);
}

uint32_t prRenderSceneInstanceCount(const PrRender* render, PrRenderScene scene) {
    const RenderSceneState* s = renderSceneFind(render, scene.id);
    return s != nullptr ? s->instanceCount : 0;
}

uint32_t prRenderSceneLightCount(const PrRender* render, PrRenderScene scene) {
    const RenderSceneState* s = renderSceneFind(render, scene.id);
    return s != nullptr ? s->lightCount : 0;
}

float prRenderSceneIlluminance(const PrRender* render, PrRenderScene scene, PrVec3 point) {
    const RenderSceneState* s = renderSceneFind(render, scene.id);
    if (s == nullptr)
        return 0;

    // The lights that shade surfaces, as the frame's uniforms hold them (setLights).
    PrVec3 directions[kPrRenderMaxLights];
    float illuminances[kPrRenderMaxLights];
    uint32_t count = 0;
    for (uint32_t id = 1; id < s->lights.count && count < kPrRenderMaxLights; ++id) {
        if (!s->lights.data[id].alive)
            continue;
        illuminances[count] = lightIlluminance(s->lights.data[id].desc, point, &directions[count]);
        ++count;
    }

    // The planes that face each light and each direction along an axis.
    PrVec3 normals[kPrRenderMaxLights + 6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (uint32_t i = 0; i < count; ++i)
        normals[6 + i] = directions[i];
    const PrRenderSceneEnvironment& environment = s->environment;
    const float (*m)[4] = environment.rotation.m;
    float most = 0;
    for (uint32_t n = 0; n < 6 + count; ++n) {
        PrVec3 normal = normals[n];
        float sum = 0;
        for (uint32_t i = 0; i < count; ++i)
            sum += illuminances[i] * prF32Max(prVec3Dot(normal, directions[i]), 0);
        if (environment.environment.id != 0) {
            // The rotation maps the environment's directions to the world's, so its transpose maps them back.
            PrVec3 local{
                m[0][0] * normal.x + m[1][0] * normal.y + m[2][0] * normal.z,
                m[0][1] * normal.x + m[1][1] * normal.y + m[2][1] * normal.z,
                m[0][2] * normal.x + m[1][2] * normal.y + m[2][2] * normal.z,
            };
            PrVec3 radiance = prRenderEnvironmentIrradiance(render, environment.environment, local);
            sum += kPrF32Pi * environment.intensity * luminanceOf(radiance);
        }
        most = prF32Max(most, sum);
    }
    return most;
}

PrGpuGraphBuffer prRenderSceneInstanceBuffer(PrRender* render, PrRenderScene scene) {
    RenderSceneState* s = renderSceneFind(render, scene.id);
    if (s == nullptr || render->graph == nullptr)
        return PrGpuGraphBuffer{};
    return renderSceneFrame(render, s)->instances;
}

PrRenderDeformedBuffers prRenderSceneDeformedBuffers(PrRender* render, PrRenderScene scene) {
    RenderSceneState* s = renderSceneFind(render, scene.id);
    if (s == nullptr || render->graph == nullptr)
        return PrRenderDeformedBuffers{};
    const RenderSceneFrame* frame = renderSceneFrame(render, s);
    return PrRenderDeformedBuffers{.vertices = frame->deformed, .previousPositions = frame->previousPositions};
}

bool prRenderSceneInstancePart(const PrRender* render, PrRenderScene scene, PrRenderInstance instance, uint32_t part,
        PrRenderInstancePart* out) {
    const Render* r = render;
    *out = PrRenderInstancePart{};
    const RenderSceneState* s = renderSceneFind(r, scene.id);
    if (s == nullptr || instance.id == 0 || instance.id >= s->instances.count || !s->instances.data[instance.id].alive)
        return false;
    const PrRenderInstanceRecord& record = s->records.data[instance.id];
    if (record.mesh == 0 || record.mesh >= r->meshes.count || !r->meshes.data[record.mesh].ready)
        return false;
    const RenderMeshState& mesh = r->meshes.data[record.mesh];
    if (part >= mesh.partCount)
        return false;
    const RenderInstanceState& state = s->instances.data[instance.id];
    const PrRenderPartRecord& drawn = r->parts.data[firstPartOf(s, state, mesh) + part];
    // The sphere in the world, as the cull computes it: the center under the instance's matrix, and the radius times
    // the longest of the matrix's axes.
    const PrRenderVec4* w = record.world;
    float scale2 = 0;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        float x = (&w[0].x)[axis];
        float y = (&w[1].x)[axis];
        float z = (&w[2].x)[axis];
        scale2 = prF32Max(scale2, x * x + y * y + z * z);
    }
    const PrRenderVec4& b = drawn.bounds;
    *out = PrRenderInstancePart{
        .bounds = {
            .center = {
                w[0].x * b.x + w[0].y * b.y + w[0].z * b.z + w[0].w,
                w[1].x * b.x + w[1].y * b.y + w[1].z * b.z + w[1].w,
                w[2].x * b.x + w[2].y * b.y + w[2].z * b.z + w[2].w,
            },
            .radius = b.w * prF32Sqrt(scale2),
        },
        .firstIndex = drawn.firstIndex,
        .indexCount = drawn.indexCount,
        .firstVertex = drawn.firstVertex,
        .deforms = state.deformation != 0 && s->deformations.data[state.deformation].bound,
    };
    return true;
}

uint32_t prRenderSceneFallbackDrawCount(const PrRender* render, PrRenderScene scene) {
    const RenderSceneState* s = renderSceneFind(render, scene.id);
    return s != nullptr ? s->fallbackDraws : 0;
}
PrRenderUpdate* prRenderUpdateBegin(PrRender* render, PrRenderScene scene) {
    Render* r = render;
    PrRenderUpdate* update = allocZeroed<PrRenderUpdate>();
    update->render = r;
    prMutexLock(&r->lock);
    PR_ASSERT(scene.id != 0 && scene.id < r->scenes.count && r->scenes.data[scene.id] != nullptr);
    update->scene = r->scenes.data[scene.id];
    prMutexUnlock(&r->lock);
    return update;
}

void prRenderUpdateCommit(PrRenderUpdate* update) {
    Render* r = update->render;
    prMutexLock(&r->lock);
    r->committed.push(update);
    prMutexUnlock(&r->lock);
}

void prRenderUpdateDiscard(PrRenderUpdate* PR_NULLABLE update) {
    if (update == nullptr)
        return;
    Render* r = update->render;
    prMutexLock(&r->lock);
    giveIds(&update->scene->instanceIds, update->addedInstances);
    giveIds(&update->scene->lightIds, update->addedLights);
    prMutexUnlock(&r->lock);
    renderUpdateDestroy(update);
}

PrRenderInstance prRenderInstanceAdd(PrRenderUpdate* update, const PrRenderInstanceDesc* desc) {
    Render* r = update->render;
    prMutexLock(&r->lock);
    uint32_t id = renderIdsTake(&update->scene->instanceIds);
    prMutexUnlock(&r->lock);
    update->addedInstances.push(id);
    InstanceAddCommand* command =
        appendCommand<InstanceAddCommand>(update, CommandKind_InstanceAdd, desc->materialCount);
    command->id = id;
    command->mesh = desc->mesh.id;
    command->mobility = desc->mobility;
    command->visibility = desc->visibility;
    command->materialCount = desc->materialCount;
    command->world = desc->world;
    uint32_t* materials = reinterpret_cast<uint32_t*>(command + 1);
    for (uint32_t i = 0; i < desc->materialCount; ++i)
        materials[i] = desc->materials[i].id;
    return PrRenderInstance{id};
}

void prRenderInstanceRemove(PrRenderUpdate* update, PrRenderInstance instance) {
    appendCommand<RemoveCommand>(update, CommandKind_InstanceRemove)->id = instance.id;
}

void prRenderInstanceSetTransform(PrRenderUpdate* update, PrRenderInstance instance, const PrAffine3* world) {
    prRenderInstanceSetTransforms(update, &instance, world, 1);
}

void prRenderInstanceSetTransforms(PrRenderUpdate* update, const PrRenderInstance* instances,
        const PrAffine3* worlds, uint32_t count) {
    static_assert(sizeof(PrRenderInstance) == 4 && sizeof(PrAffine3) == 48);
    uint32_t extraWords = count * (1 + uint32_t(sizeof(PrAffine3) / 4));
    InstanceTransformsCommand* command =
        appendCommand<InstanceTransformsCommand>(update, CommandKind_InstanceTransforms, extraWords);
    command->count = count;
    uint32_t* ids = reinterpret_cast<uint32_t*>(command + 1);
    prMemCopy(ids, instances, size_t(count) * sizeof(PrRenderInstance));
    prMemCopy(ids + count, worlds, size_t(count) * sizeof(PrAffine3));
}

void prRenderInstanceSetJoints(PrRenderUpdate* update, PrRenderInstance instance, const PrAffine3* joints, uint32_t count) {
    uint32_t extraWords = count * uint32_t(sizeof(PrAffine3) / 4);
    InstancePoseCommand* command = appendCommand<InstancePoseCommand>(update, CommandKind_InstanceJoints, extraWords);
    command->id = instance.id;
    command->count = count;
    if (count != 0)
        prMemCopy(command + 1, joints, size_t(count) * sizeof(PrAffine3));
}

void prRenderInstanceSetMorphWeights(PrRenderUpdate* update, PrRenderInstance instance, const float* weights, uint32_t count) {
    InstancePoseCommand* command = appendCommand<InstancePoseCommand>(update, CommandKind_InstanceWeights, count);
    command->id = instance.id;
    command->count = count;
    if (count != 0)
        prMemCopy(command + 1, weights, size_t(count) * sizeof(float));
}

void prRenderInstanceSetMaterial(PrRenderUpdate* update, PrRenderInstance instance, uint32_t part,
        PrRenderMaterial material) {
    InstanceMaterialCommand* command = appendCommand<InstanceMaterialCommand>(update, CommandKind_InstanceMaterial);
    command->id = instance.id;
    command->part = part;
    command->material = material.id;
}

void prRenderInstanceSetMasks(PrRenderUpdate* update, PrRenderInstance instance, PrRenderMobilityFlags mobility,
        uint32_t visibility) {
    InstanceMasksCommand* command = appendCommand<InstanceMasksCommand>(update, CommandKind_InstanceMasks);
    command->id = instance.id;
    command->mobility = mobility;
    command->visibility = visibility;
}

PrRenderLight prRenderLightAdd(PrRenderUpdate* update, const PrRenderLightDesc* desc) {
    Render* r = update->render;
    prMutexLock(&r->lock);
    uint32_t id = renderIdsTake(&update->scene->lightIds);
    prMutexUnlock(&r->lock);
    update->addedLights.push(id);
    prRenderLightSet(update, PrRenderLight{id}, desc);
    return PrRenderLight{id};
}

void prRenderLightSet(PrRenderUpdate* update, PrRenderLight light, const PrRenderLightDesc* desc) {
    LightSetCommand* command = appendCommand<LightSetCommand>(update, CommandKind_LightSet);
    command->id = light.id;
    command->desc = *desc;
}

void prRenderLightRemove(PrRenderUpdate* update, PrRenderLight light) {
    appendCommand<RemoveCommand>(update, CommandKind_LightRemove)->id = light.id;
}

void prRenderSceneEnvironmentSet(PrRenderUpdate* update, const PrRenderSceneEnvironment* environment) {
    appendCommand<EnvironmentCommand>(update, CommandKind_Environment)->environment = *environment;
}

void prRenderSceneTimeSet(PrRenderUpdate* update, float seconds) {
    appendCommand<TimeCommand>(update, CommandKind_Time)->seconds = seconds;
}

