// Renderers: the shaders, the queues that other threads fill, and the frame boundary that applies them.

#include "pr/render.h"
#include "render/src/render.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/byte_view.h"
#include "pr/float16.h"

#include "render/src/kRenderBackgroundShaders.h"
#include "render/src/kRenderBackgroundSlotShaders.h"
#include "render/src/kRenderCullShaders.h"
#include "render/src/kRenderCullSlotShaders.h"
#include "render/src/kRenderDeformShaders.h"
#include "render/src/kRenderDeformSlotShaders.h"
#include "render/src/kRenderDepthShaders.h"
#include "render/src/kRenderDepthSlotShaders.h"
#include "render/src/kRenderDisplayShaders.h"
#include "render/src/kRenderDisplaySlotShaders.h"
#include "render/src/kRenderEnvironmentFilterShaders.h"
#include "render/src/kRenderEnvironmentFilterSlotShaders.h"
#include "render/src/kRenderMeterShaders.h"
#include "render/src/kRenderMeterSlotShaders.h"
#include "render/src/render_deform.h"
#include "render/src/render_environment.h"
#include "render/src/render_meter.h"
#include "render/src/render_pipeline.h"
#include "render/src/render_resource.h"
#include "render/src/render_scene.h"
#include "render/src/render_tables.h"
#include "render/src/render_view.h"
#include "render_shaders/RenderLibraryShaders.h"

namespace pr_impl {

uint32_t renderIdsTake(RenderIds* ids) {
    if (ids->free.count != 0)
        return ids->free.data[--ids->free.count];
    return ++ids->count;
}

void renderIdsGive(RenderIds* ids, uint32_t id) {
    ids->free.push(id);
}

void renderIdsReset(RenderIds* ids) {
    ids->free.reset();
    ids->count = 0;
}

uint32_t renderRangesTake(RenderRanges* ranges, uint32_t count) {
    for (uint32_t i = 0; i < ranges->free.count; ++i) {
        RenderRange& range = ranges->free.data[i];
        if (range.count < count)
            continue;
        uint32_t first = range.first;
        range.first += count;
        range.count -= count;
        if (range.count == 0)
            ranges->free.erase(i);
        return first;
    }
    uint32_t first = ranges->end;
    ranges->end += count;
    return first;
}

void renderRangesGive(RenderRanges* ranges, uint32_t first, uint32_t count) {
    if (count == 0)
        return;
    uint32_t at = 0;
    while (at < ranges->free.count && ranges->free.data[at].first < first)
        ++at;
    ranges->free.insert(at, RenderRange{first, count});
    // The range joins the free range after it, and then the one before it, where they touch.
    if (at + 1 < ranges->free.count && first + count == ranges->free.data[at + 1].first) {
        ranges->free.data[at].count += ranges->free.data[at + 1].count;
        ranges->free.erase(at + 1);
    }
    if (at != 0 && ranges->free.data[at - 1].first + ranges->free.data[at - 1].count == first) {
        ranges->free.data[at - 1].count += ranges->free.data[at].count;
        ranges->free.erase(at);
    }
    // A free range at the end is no range at all.
    const RenderRange& last = ranges->free.last();
    if (last.first + last.count == ranges->end) {
        ranges->end = last.first;
        ranges->free.pop();
    }
}

void renderRangesReset(RenderRanges* ranges) {
    ranges->free.reset();
    ranges->end = 0;
}

bool renderRecordBufferReserve(Render* render, RenderRecordBuffer* buffer, uint64_t size, PrGpuBufferUsageFlags usage,
        PrStringView name) {
    if (buffer->buffer.id != 0 && size <= buffer->capacity)
        return false;
    // A buffer at least doubles, so that records that are added one at a time are not uploaded again each time.
    uint64_t capacity = buffer->capacity * 2;
    if (capacity < size)
        capacity = size;
    if (capacity < 1024)
        capacity = 1024;
    prGpuBufferRelease(render->device, buffer->buffer);
    PrGpuBufferDesc desc{.size = capacity, .usage = usage, .memory = PrGpuMemory_Device, .name = name};
    buffer->capacity = 0;
    if (prGpuBufferCreate(render->device, &desc, &buffer->buffer) == PrStatus_Ok)
        buffer->capacity = capacity;
    return true;
}

RenderSceneState* PR_NULLABLE renderSceneFind(const Render* render, uint32_t id) {
    if (id == 0 || id >= render->frameScenes.count)
        return nullptr;
    return render->frameScenes.data[id];
}

namespace {

// A library's bytes for a device with bindless, and for the slots alone.
struct LibraryBytes {
    PrByteView handles;
    PrByteView slots;
};

// The names of the library's types, by RenderLibraryType, as the shader set names them.
constexpr PrStringView kLibraryTypeNames[RenderLibraryType_Count] = {"KhronosPbr", "Unlit", "KhronosPbrLayered", "UsdPreview"};

// Reads the slots that a library's source declares, by the names that the renderer binds.
RenderBindings bindingsOf(PrByteView code) {
    RenderBindings b{};
    uint32_t* fields[] = {
        &b.instances, &b.draws, &b.parts, &b.materials, &b.arguments, &b.shadowMap, &b.environment, &b.tables,
        &b.color, &b.source, &b.vertices, &b.skin, &b.morphs, &b.pose, &b.deformed, &b.previous,
    };
    for (uint32_t* field : fields)
        *field = kRenderSlotNone;
    for (uint32_t& slot : b.materialTextures)
        slot = kRenderSlotNone;
    struct Named {
        PrStringView name;
        uint32_t* slot;
    };
    const Named named[] = {
        {"renderSlotInstances", &b.instances},
        {"renderSlotDraws", &b.draws},
        {"renderSlotParts", &b.parts},
        {"renderSlotMaterials", &b.materials},
        {"renderSlotArguments", &b.arguments},
        {"renderSlotShadowMap", &b.shadowMap},
        {"renderSlotEnvironment", &b.environment},
        {"renderSlotTables", &b.tables},
        {"renderSlotColor", &b.color},
        {"renderSlotSource", &b.source},
        {"deformSlotVertices", &b.vertices},
        {"deformSlotSkin", &b.skin},
        {"deformSlotMorphs", &b.morphs},
        {"deformSlotPose", &b.pose},
        {"deformSlotDeformed", &b.deformed},
        {"deformSlotPrevious", &b.previous},
    };
    uint32_t count = prGpuShaderLibrarySlots(code, nullptr, 0);
    RawArray<PrGpuShaderSlot> slots{};
    slots.resize(count);
    prGpuShaderLibrarySlots(code, slots.data, count);
    PrStringView prefix = "renderSlotMaterialTexture";
    for (uint32_t i = 0; i < count; ++i) {
        const PrGpuShaderSlot& slot = slots.data[i];
        for (const Named& n : named) {
            if (slot.name == n.name)
                *n.slot = slot.slot;
        }
        // A material's texture slots are named for their place among the material's slots.
        if (slot.name.size == prefix.size + 1 && PrStringView{slot.name.data, prefix.size} == prefix) {
            uint32_t place = uint32_t(slot.name.data[prefix.size] - '0');
            if (place < kRenderMaxMaterialSlots)
                b.materialTextures[place] = slot.slot;
        }
    }
    slots.reset();
    while (b.materialTextureCount < kRenderMaxMaterialSlots &&
            b.materialTextures[b.materialTextureCount] != kRenderSlotNone)
        ++b.materialTextureCount;
    return b;
}

// The libraries that are the renderer's own, whatever its shader set.
constexpr LibraryBytes kLibraries[kRenderLibraryFirstOfSet] = {
    {{kRenderCullShaders, sizeof(kRenderCullShaders)}, {kRenderCullSlotShaders, sizeof(kRenderCullSlotShaders)}},
    {
        {kRenderDeformShaders, sizeof(kRenderDeformShaders)},
        {kRenderDeformSlotShaders, sizeof(kRenderDeformSlotShaders)},
    },
    {{kRenderDepthShaders, sizeof(kRenderDepthShaders)}, {kRenderDepthSlotShaders, sizeof(kRenderDepthSlotShaders)}},
    {
        {kRenderBackgroundShaders, sizeof(kRenderBackgroundShaders)},
        {kRenderBackgroundSlotShaders, sizeof(kRenderBackgroundSlotShaders)},
    },
    {
        {kRenderDisplayShaders, sizeof(kRenderDisplayShaders)},
        {kRenderDisplaySlotShaders, sizeof(kRenderDisplaySlotShaders)},
    },
    {
        {kRenderEnvironmentFilterShaders, sizeof(kRenderEnvironmentFilterShaders)},
        {kRenderEnvironmentFilterSlotShaders, sizeof(kRenderEnvironmentFilterSlotShaders)},
    },
    {{kRenderMeterShaders, sizeof(kRenderMeterShaders)}, {kRenderMeterSlotShaders, sizeof(kRenderMeterSlotShaders)}},
};

// Sets `out` to the texel that stands in for a texture of `value`, which the first call for the value makes.
PrStatus standInFor(Render* r, PrRenderVec4 value, PrGpuTexture* out) {
    for (uint32_t i = 0; i < r->standIns.count; ++i) {
        const PrRenderVec4& held = r->standIns.data[i].value;
        if (held.x == value.x && held.y == value.y && held.z == value.z && held.w == value.w) {
            *out = r->standIns.data[i].texture;
            return PrStatus_Ok;
        }
    }
    // Half floats hold one half exactly, so that a flat normal decodes to the surface's own.
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, PrUVec2{1, 1}, PrGpuTextureUsageFlag_Sampled);
    desc.name = "render stand-in";
    RenderStandIn made{.value = value};
    PrStatus status = prGpuTextureCreate(r->device, &desc, &made.texture);
    if (status != PrStatus_Ok)
        return status;
    const PrFloat16 texel[4] = {
        prFloat16FromFloat(value.x),
        prFloat16FromFloat(value.y),
        prFloat16FromFloat(value.z),
        prFloat16FromFloat(value.w),
    };
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{1, 1});
    prGpuTextureUpload(r->device, made.texture, &region, texel, sizeof(texel), sizeof(texel));
    r->standIns.push(made);
    *out = made.texture;
    return PrStatus_Ok;
}

// Makes the texel that each texture of each material type of the shader set reads before its data arrives.
PrStatus createStandIns(Render* r) {
    const PrRenderShaderSet& set = *r->shaders;
    for (uint32_t type = 0; type < set.materialTypeCount; ++type) {
        r->pendingFirst.push(r->pendingTextures.count);
        for (uint32_t i = 0; i < set.materialTypes[type].textureCount; ++i) {
            PrGpuTexture texture{};
            PrStatus status = standInFor(r, set.materialTypes[type].textures[i].standIn, &texture);
            if (status != PrStatus_Ok)
                return status;
            r->pendingTextures.push(texture);
        }
    }
    return PrStatus_Ok;
}

// Makes what every frame needs: the shader libraries, the pipelines of the dispatches, the white texel, and what
// environments are made with.
PrStatus createDeviceObjects(Render* r) {
    for (uint32_t i = 0; i < RenderLibrary_Count; ++i) {
        PrByteView code{};
        if (i < kRenderLibraryFirstOfSet) {
            code = r->bindless ? kLibraries[i].handles : kLibraries[i].slots;
        } else {
            uint32_t pass = i - kRenderLibraryFirstOfSet;
            code = r->bindless ? r->shaders->handles[pass] : r->shaders->slots[pass];
        }
        if (code.size == 0)
            continue;
        r->bindings[i] = bindingsOf(code);
        PrStatus status = prGpuShaderLibraryCreate(r->device, code.data, code.size, &r->libraries[i]);
        if (status != PrStatus_Ok)
            return status == PrStatus_InvalidData ? PrStatus_NotSupported : status;
    }
    PrStatus status =
        prGpuPipelineCreateCompute(r->device, r->libraries[RenderLibrary_Cull], "cullDraws", "render cull", &r->cull);
    if (status != PrStatus_Ok)
        return status;
    status = prGpuPipelineCreateCompute(r->device, r->libraries[RenderLibrary_Deform], "deformVertices",
        "render deform", &r->deform);
    if (status != PrStatus_Ok)
        return status;

    PrGpuTextureDesc whiteDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, PrUVec2{1, 1}, PrGpuTextureUsageFlag_Sampled);
    whiteDesc.name = "render white";
    status = prGpuTextureCreate(r->device, &whiteDesc, &r->white);
    if (status != PrStatus_Ok)
        return status;
    const uint8_t white[4] = {255, 255, 255, 255};
    PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, PrUVec2{1, 1});
    prGpuTextureUpload(r->device, r->white, &region, white, 4, 4);
    PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Repeat);
    status = prGpuSamplerIntern(r->device, &sampler, &r->whiteSampler);
    if (status != PrStatus_Ok)
        return status;
    PrGpuSamplerDesc behind = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Clamp);
    behind.mip = PrGpuFilter_Linear;
    status = prGpuSamplerIntern(r->device, &behind, &r->behindSampler);
    if (status != PrStatus_Ok)
        return status;
    status = createStandIns(r);
    if (status != PrStatus_Ok)
        return status;
    status = renderTablesCreate(r);
    if (status != PrStatus_Ok)
        return status;
    status = renderEnvironmentObjectsCreate(r);
    // The sources of the tables are the caller's, and are read only here.
    r->options.tables = nullptr;
    r->options.tableCount = 0;
    return status;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRenderOptions prRenderOptionsDefault(void) {
    return PrRenderOptions{.shadowMapSize = 2048, .environmentSize = kPrRenderEnvironmentDefaultSize};
}

PrStatus prRenderCreate(PrGpuDevice* device, const PrRenderOptions* PR_NULLABLE options,
        PrRender* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrRender* r = allocZeroed<PrRender>();
    r->device = device;
    r->options = options != nullptr ? *options : prRenderOptionsDefault();
    PR_ASSERT(r->options.shadowMapSize != 0 && r->options.environmentSize <= kPrRenderEnvironmentMaxSize);
    r->shaders = r->options.shaders != nullptr ? r->options.shaders : prRenderShaderSetLibrary();
    // Every set that pr_render_shaders builds has the library's types, and the neutral material is of glTF's.
    for (uint32_t i = 0; i < RenderLibraryType_Count; ++i) {
        r->libraryTypes[i] = prRenderMaterialTypeFromName(r->shaders, kLibraryTypeNames[i]);
        PR_ASSERT(r->libraryTypes[i] != kPrRenderMaterialTypeMissing);
    }
    {
        const PrRenderMaterialType& gltf = r->shaders->materialTypes[r->libraryTypes[RenderLibraryType_KhronosPbr]];
        r->neutralParameters.resize(gltf.parameterBytes);
        prRenderMaterialParametersDefault(&gltf, r->neutralParameters.data);
        const float gray[4] = {0.5f, 0.5f, 0.5f, 1};
        const float zero = 0;
        prRenderMaterialParameterSet(&gltf, r->neutralParameters.data,
            prRenderMaterialParameterFromName(&gltf, "baseColor"), gray, 4);
        prRenderMaterialParameterSet(&gltf, r->neutralParameters.data,
            prRenderMaterialParameterFromName(&gltf, "metallic"), &zero, 1);
    }
    // Every such set has the library's table too, which the library's shading model reads.
    PR_ASSERT(r->shaders->tableCount > 0);
    if (r->options.environmentSize == 0)
        r->options.environmentSize = kPrRenderEnvironmentDefaultSize;
    r->bindless = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_Bindless) != 0;
    prMutexInit(&r->lock);
    prMutexInit(&r->pipelineLock);
    PrStatus status = createDeviceObjects(r);
    if (status != PrStatus_Ok) {
        prRenderDestroy(r);
        return status;
    }
    // Every table has an entry for no id, and the materials' is the material that stands in for a missing one.
    renderTableGrow(&r->meshes, 0);
    renderTableGrow(&r->textures, 0);
    renderTableGrow(&r->materials, 0);
    renderTableGrow(&r->environments, 0);
    renderTableGrow(&r->frameScenes, 0);
    r->scenes.push(nullptr);
    r->materialsDirty = true;
    r->partsDirtyFirst = 0xffffffffu;
    *out = r;
    return PrStatus_Ok;
}

void prRenderDestroy(PrRender* PR_NULLABLE render) {
    if (render == nullptr)
        return;
    Render* r = render;
    for (uint32_t i = 0; i < r->ops.count; ++i)
        renderOpDiscard(r, r->ops.data[i]);
    r->ops.reset();
    for (uint32_t i = 0; i < r->committed.count; ++i)
        renderUpdateDestroy(r->committed.data[i]);
    r->committed.reset();
    for (uint32_t i = 0; i < r->scenes.count; ++i)
        renderSceneStateDestroy(r, r->scenes.data[i]);
    r->scenes.reset();
    r->frameScenes.reset();
    renderResourcesReset(r);
    renderIdsReset(&r->meshIds);
    renderIdsReset(&r->textureIds);
    renderIdsReset(&r->materialIds);
    renderIdsReset(&r->environmentIds);
    renderIdsReset(&r->sceneIds);
    for (uint32_t i = 0; i < r->meters.count; ++i) {
        if (r->meters.data[i] != nullptr)
            renderMeterRelease(r->meters.data[i]);
    }
    r->meters.reset();
    renderIdsReset(&r->meterIds);

    prGpuPipelineRelease(r->device, r->cull);
    prGpuPipelineRelease(r->device, r->deform);
    renderPipelinesReset(r);
    prGpuTextureRelease(r->device, r->white);
    for (uint32_t i = 0; i < r->standIns.count; ++i)
        prGpuTextureRelease(r->device, r->standIns.data[i].texture);
    r->standIns.reset();
    r->pendingTextures.reset();
    r->pendingFirst.reset();
    prGpuTextureRelease(r->device, r->tables);
    renderEnvironmentObjectsDestroy(r);
    for (PrGpuShaderLibrary* library : r->libraries)
        prGpuShaderLibraryDestroy(library);
    prMutexReset(&r->lock);
    prMutexReset(&r->pipelineLock);
    prMemoryFree(render);
}

PrGpuDevice* prRenderDevice(const PrRender* render) {
    return render->device;
}

const PrRenderShaderSet* prRenderShaderSet(const PrRender* render) {
    return render->shaders;
}

PrRenderInfo prRenderInfo(const PrRender* render) {
    // The lock guards the ids, and locking it changes nothing that the caller can observe.
    Render* r = const_cast<PrRender*>(render);
    prMutexLock(&r->lock);
    PrRenderInfo info{
        .meshCount = r->meshIds.count - r->meshIds.free.count,
        .textureCount = r->textureIds.count - r->textureIds.free.count,
        .materialCount = r->materialIds.count - r->materialIds.free.count,
        .environmentCount = r->environmentIds.count - r->environmentIds.free.count,
        .sceneCount = r->sceneIds.count - r->sceneIds.free.count,
    };
    prMutexUnlock(&r->lock);
    return info;
}

PrRenderPipelineInfo prRenderPipelineInfo(const PrRender* render) {
    // Locking changes nothing that the caller can observe.
    Render* r = const_cast<PrRender*>(render);
    prMutexLock(&r->pipelineLock);
    PrRenderPipelineInfo info{.count = r->pipelines.count, .seconds = r->pipelineSeconds};
    prMutexUnlock(&r->pipelineLock);
    return info;
}

void prRenderFrameBegin(PrRender* render, PrGpuGraph* graph) {
    Render* r = render;
    // The queues are taken whole, so that what a thread adds while they are applied waits for the next frame.
    RawArray<RenderOp> ops{};
    RawArray<RenderUpdate*> updates{};
    prMutexLock(&r->lock);
    ops = r->ops;
    updates = r->committed;
    r->ops = RawArray<RenderOp>{};
    r->committed = RawArray<RenderUpdate*>{};
    uint32_t meshCount = r->meshIds.count;
    uint32_t textureCount = r->textureIds.count;
    uint32_t materialCount = r->materialIds.count;
    uint32_t environmentCount = r->environmentIds.count;
    renderTableGrow(&r->frameScenes, r->sceneIds.count);
    for (uint32_t i = 0; i < r->scenes.count; ++i)
        r->frameScenes.data[i] = r->scenes.data[i];
    prMutexUnlock(&r->lock);

    renderTableGrow(&r->meshes, meshCount);
    renderTableGrow(&r->textures, textureCount);
    if (materialCount + 1 > r->materials.count)
        r->materialsDirty = true;
    renderTableGrow(&r->materials, materialCount);
    renderTableGrow(&r->environments, environmentCount);
    renderResourcesApply(r, &ops);
    ops.reset();
    renderScenesApply(r, &updates);
    updates.reset();
    // The scenes write the parts of their deforming instances, so the parts are uploaded after them.
    renderPartsUpload(r);

    ++r->frameSerial;
    r->graph = graph;
    r->frameParts = prGpuGraphImportBuffer(graph, r->partBuffer.buffer, PrGpuAccessFlag_ShaderRead);
    r->frameMaterials = prGpuGraphImportBuffer(graph, r->materialBuffer.buffer, PrGpuAccessFlag_ShaderRead);
}
