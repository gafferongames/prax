// A renderer's resources: meshes, textures and materials. Any thread makes a resource's GPU objects and queues the
// change, and the frame thread applies the queue to its tables when a frame begins.

#include "pr/render_resource.h"
#include "render/src/render_resource.h"

#include "pr/align.h"
#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mesh.h"

#include "render/src/render.h"
#include "render/src/render_meter.h"
#include "render/src/render_pipeline.h"
#include "render/src/render_scene.h"
#include "render/src/render_view.h"
#include "render_shaders/RenderLibraryShaders.h"

namespace pr_impl {

namespace {

// The names of the units of emission, by PrRenderEmission.
constexpr PrStringView kEmissionNames[PrRenderEmission_Count] = {"luminance", "display"};

// The shaders read a vertex's skin and a morph target's delta as the records that have their bytes.
static_assert(sizeof(PrRenderSkinVertex) == sizeof(PrRenderSkinRecord));
static_assert(sizeof(PrRenderMorphDelta) == sizeof(PrRenderMorphRecord));

int16_t packSnorm16(float value) {
    return int16_t(prF32Round(prF32Clamp(value, -1, 1) * 32767));
}

PrVec3 positionOf(const PrRenderGeometryVertex& vertex) {
    return PrVec3{vertex.position[0], vertex.position[1], vertex.position[2]};
}

// Returns a sphere that bounds the vertices that a part's indices name: the center of their box, and the distance to
// the farthest of them.
PrRenderVec4 partBounds(const PrRenderMeshData& data, const PrRenderMeshPart& part) {
    if (part.indexCount == 0)
        return PrRenderVec4{};
    PrVec3 low = positionOf(data.geometry[data.indices[part.firstIndex]]);
    PrVec3 high = low;
    for (uint32_t i = 1; i < part.indexCount; ++i) {
        PrVec3 p = positionOf(data.geometry[data.indices[part.firstIndex + i]]);
        low = {prF32Min(low.x, p.x), prF32Min(low.y, p.y), prF32Min(low.z, p.z)};
        high = {prF32Max(high.x, p.x), prF32Max(high.y, p.y), prF32Max(high.z, p.z)};
    }
    PrVec3 center = (low + high) * 0.5f;
    float radius2 = 0;
    for (uint32_t i = 0; i < part.indexCount; ++i) {
        PrVec3 d = positionOf(data.geometry[data.indices[part.firstIndex + i]]) - center;
        radius2 = prF32Max(radius2, prVec3Dot(d, d));
    }
    return PrRenderVec4{center.x, center.y, center.z, prF32Sqrt(radius2)};
}

// Returns, for each joint of a mesh's skin, the sphere that holds every vertex that the joint moves: the center of
// their box, and the distance to the farthest of them. A joint that moves no vertex has a radius of -1.
PrRenderVec4* jointBounds(const PrRenderMeshData& data) {
    uint32_t count = data.jointCount;
    PrVec3* low = static_cast<PrVec3*>(prMemoryAlloc(size_t(count) * sizeof(PrVec3)));
    PrVec3* high = static_cast<PrVec3*>(prMemoryAlloc(size_t(count) * sizeof(PrVec3)));
    PrRenderVec4* bounds = static_cast<PrRenderVec4*>(prMemoryAlloc(size_t(count) * sizeof(PrRenderVec4)));
    for (uint32_t j = 0; j < count; ++j)
        bounds[j] = PrRenderVec4{0, 0, 0, -1};
    for (uint32_t i = 0; i < data.vertexCount; ++i) {
        PrVec3 p = positionOf(data.geometry[i]);
        for (uint32_t k = 0; k < kPrRenderSkinInfluences; ++k) {
            uint32_t j = data.skin[i].joints[k];
            if (data.skin[i].weights[k] == 0 || j >= count)
                continue;
            if (bounds[j].w < 0) {
                low[j] = p;
                high[j] = p;
                bounds[j].w = 0;
            }
            low[j] = {prF32Min(low[j].x, p.x), prF32Min(low[j].y, p.y), prF32Min(low[j].z, p.z)};
            high[j] = {prF32Max(high[j].x, p.x), prF32Max(high[j].y, p.y), prF32Max(high[j].z, p.z)};
        }
    }
    for (uint32_t j = 0; j < count; ++j) {
        PrVec3 center = (low[j] + high[j]) * 0.5f;
        if (bounds[j].w >= 0)
            bounds[j] = PrRenderVec4{center.x, center.y, center.z, 0};
    }
    for (uint32_t i = 0; i < data.vertexCount; ++i) {
        PrVec3 p = positionOf(data.geometry[i]);
        for (uint32_t k = 0; k < kPrRenderSkinInfluences; ++k) {
            uint32_t j = data.skin[i].joints[k];
            if (data.skin[i].weights[k] == 0 || j >= count)
                continue;
            PrVec3 d = p - PrVec3{bounds[j].x, bounds[j].y, bounds[j].z};
            bounds[j].w = prF32Max(bounds[j].w, prVec3Length(d));
        }
    }
    prMemoryFree(low);
    prMemoryFree(high);
    return bounds;
}

// Returns, for each morph target of a mesh, the farthest that the target moves a vertex at a weight of 1.
float* morphReach(const PrRenderMeshData& data) {
    float* reach = static_cast<float*>(prMemoryAllocZeroed(size_t(data.morphTargetCount) * sizeof(float)));
    for (uint32_t i = 0; i < data.vertexCount; ++i) {
        for (uint32_t t = 0; t < data.morphTargetCount; ++t) {
            const float* d = data.morphs[size_t(i) * data.morphTargetCount + t].position;
            reach[t] = prF32Max(reach[t], prF32Sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
        }
    }
    return reach;
}

// Frees what deforms a mesh.
void releaseDeform(Render* r, RenderMeshDeform* deform) {
    prGpuBufferRelease(r->device, deform->skin);
    prGpuBufferRelease(r->device, deform->morphs);
    prMemoryFree(deform->jointBounds);
    prMemoryFree(deform->morphReach);
    *deform = RenderMeshDeform{};
}

// Creates a buffer that holds `size` bytes of `data`.
PrStatus createBufferOf(Render* r, const void* data, uint64_t size, PrGpuBufferUsageFlags usage, PrStringView name,
        PrGpuBuffer* out) {
    PrGpuBufferDesc desc{.size = size, .usage = usage, .memory = PrGpuMemory_Device, .name = name};
    PrStatus status = prGpuBufferCreate(r->device, &desc, out);
    if (status == PrStatus_Ok)
        prGpuBufferUpload(r->device, *out, 0, data, size);
    return status;
}

uint32_t levelSide(uint32_t side, uint32_t level) {
    uint32_t reduced = side >> level;
    return reduced != 0 ? reduced : 1;
}

// Frees a mesh's streams of attributes.
void releaseStreams(Render* r, RenderMeshState* mesh) {
    for (uint32_t i = 0; i < mesh->streams.count; ++i)
        prGpuBufferRelease(r->device, mesh->streams.data[i].buffer);
    mesh->streams.reset();
}

// Frees a mesh's data: its buffers and its parts. The mesh's serial changes, so that what was made for the data is made
// again.
void releaseMeshData(Render* r, RenderMeshState* mesh) {
    uint32_t serial = mesh->serial + 1;
    if (mesh->ready) {
        prGpuBufferRelease(r->device, mesh->geometry);
        prGpuBufferRelease(r->device, mesh->indices);
        releaseDeform(r, &mesh->deform);
        renderPartsGive(r, mesh->firstPart, mesh->partCount);
    }
    releaseStreams(r, mesh);
    *mesh = RenderMeshState{.serial = serial};
}

void markDrawsDirty(Render* r) {
    for (uint32_t i = 0; i < r->frameScenes.count; ++i) {
        if (r->frameScenes.data[i] != nullptr)
            r->frameScenes.data[i]->drawsDirty = true;
    }
}

void applyOp(Render* r, const RenderOp& op) {
    switch (op.kind) {
        case RenderOpKind_MeshData: {
            RenderMeshState* mesh = &r->meshes.data[op.id];
            releaseMeshData(r, mesh);
            mesh->ready = true;
            mesh->vertexCount = op.vertexCount;
            mesh->geometry = op.geometry;
            mesh->indices = op.indices;
            mesh->streams.append(op.streams, op.streamCount);
            prMemoryFree(op.streams);
            mesh->partCount = op.partCount;
            mesh->deform = op.deform;
            mesh->firstPart = renderPartsTake(r, op.partCount);
            prMemCopy(r->parts.data + mesh->firstPart, op.parts, size_t(op.partCount) * sizeof(PrRenderPartRecord));
            prMemoryFree(op.parts);
            markDrawsDirty(r);
            break;
        }
        case RenderOpKind_MeshDestroy:
            releaseMeshData(r, &r->meshes.data[op.id]);
            renderIdGive(r, &r->meshIds, op.id);
            markDrawsDirty(r);
            break;
        case RenderOpKind_TextureCreate:
            r->textures.data[op.id] = RenderTextureState{};
            break;
        case RenderOpKind_TextureData: {
            RenderTextureState* texture = &r->textures.data[op.id];
            prGpuTextureRelease(r->device, texture->texture);
            texture->texture = op.texture;
            texture->ready = true;
            r->materialsDirty = true;
            break;
        }
        case RenderOpKind_TextureDestroy:
            prGpuTextureRelease(r->device, r->textures.data[op.id].texture);
            r->textures.data[op.id] = RenderTextureState{};
            renderIdGive(r, &r->textureIds, op.id);
            r->materialsDirty = true;
            break;
        case RenderOpKind_MaterialSet:
            prMemoryFree(r->materials.data[op.id].desc);
            r->materials.data[op.id] = op.material;
            r->materialsDirty = true;
            break;
        case RenderOpKind_MaterialDestroy:
            prMemoryFree(r->materials.data[op.id].desc);
            r->materials.data[op.id] = RenderMaterialState{};
            renderIdGive(r, &r->materialIds, op.id);
            r->materialsDirty = true;
            break;
        case RenderOpKind_EnvironmentData:
            prGpuTextureRelease(r->device, r->environments.data[op.id].cube);
            r->environments.data[op.id] = *op.environment;
            prMemoryFree(op.environment);
            break;
        case RenderOpKind_EnvironmentDestroy:
            prGpuTextureRelease(r->device, r->environments.data[op.id].cube);
            r->environments.data[op.id] = RenderEnvironmentState{};
            renderIdGive(r, &r->environmentIds, op.id);
            break;
        case RenderOpKind_MeterDestroy: {
            prMutexLock(&r->lock);
            RenderMeterState* meter = r->meters.data[op.id];
            r->meters.data[op.id] = nullptr;
            renderIdsGive(&r->meterIds, op.id);
            prMutexUnlock(&r->lock);
            renderMeterRelease(meter);
            break;
        }
    }
}

// Returns a copy of a material's description that owns its textures, the samplers that they read through, which are
// interned on the device, and its parameters, in one allocation that prMemoryFree frees.
RenderMaterialState materialCopy(Render* r, const PrRenderMaterialDesc& desc) {
    size_t textureBytes = size_t(desc.textureCount) * sizeof(PrRenderTextureUse);
    size_t samplersAt = sizeof(PrRenderMaterialDesc) + textureBytes;
    size_t parametersAt = alignUp(samplersAt + size_t(desc.textureCount) * sizeof(PrGpuSampler), 16);
    uint8_t* memory = static_cast<uint8_t*>(prMemoryAlloc(parametersAt + desc.parameterBytes));
    PrRenderMaterialDesc* copy = reinterpret_cast<PrRenderMaterialDesc*>(memory);
    *copy = desc;
    copy->textures = nullptr;
    PrGpuSampler* samplers = reinterpret_cast<PrGpuSampler*>(memory + samplersAt);
    if (desc.textureCount != 0) {
        prMemCopy(memory + sizeof(PrRenderMaterialDesc), desc.textures, textureBytes);
        copy->textures = reinterpret_cast<const PrRenderTextureUse*>(memory + sizeof(PrRenderMaterialDesc));
    }
    // A device that has no sampler left gives none, and the texture then reads through the white texel's.
    for (uint32_t i = 0; i < desc.textureCount; ++i) {
        if (prGpuSamplerIntern(r->device, &desc.textures[i].sampler, &samplers[i]) != PrStatus_Ok)
            samplers[i] = r->whiteSampler;
    }
    prMemCopy(memory + parametersAt, desc.parameters, desc.parameterBytes);
    copy->parameters = memory + parametersAt;
    return RenderMaterialState{.desc = copy, .samplers = samplers};
}

// Returns the size in bytes of what a material of a type holds in the materials' buffer after its record.
uint32_t materialDataBytes(const PrRenderMaterialType& type) {
    return type.textureCount * kPrRenderMaterialTextureBytes + alignUp(type.parameterBytes, 16);
}

// Returns the value that a shader samples of a texel of half floats that holds `value`.
PrRenderVec4 halfTexel(PrRenderVec4 value) {
    return PrRenderVec4{
        prFloat16ToFloat(prFloat16FromFloat(value.x)),
        prFloat16ToFloat(prFloat16FromFloat(value.y)),
        prFloat16ToFloat(prFloat16FromFloat(value.z)),
        prFloat16ToFloat(prFloat16FromFloat(value.w)),
    };
}

// Writes a material: its record at `record`, and its textures and its parameters at `offset` bytes into the materials'
// buffer, whose bytes are at `bytes`. A material that is not set writes the neutral one. It also writes the textures
// that a draw of the material binds on a device without bindless. Returns the offset past what it wrote.
uint32_t writeMaterial(const Render* r, const RenderMaterialState& state, PrRenderMaterialRecord* record,
        RenderMaterialSlots* slots, uint8_t* bytes, uint32_t offset) {
    PrRenderMaterialDesc neutral = prRenderMaterialDescDefault(r->libraryTypes[RenderLibraryType_KhronosPbr],
        r->neutralParameters.data, r->neutralParameters.count);
    const PrRenderMaterialDesc& desc = state.desc != nullptr ? *state.desc : neutral;
    const PrRenderMaterialType& type = r->shaders->materialTypes[desc.type];
    bool behind = (desc.flags & PrRenderMaterialFlag_ReadsBehind) != 0;
    *record = PrRenderMaterialRecord{
        .variant = type.firstVariant + desc.attributes,
        .flags = uint32_t(desc.alphaMode) & kPrRenderMaterialAlphaMode,
        .alphaCutoff = desc.alphaCutoff,
        .textureCount = type.textureCount,
        .textures = offset,
        .parameters = offset + type.textureCount * kPrRenderMaterialTextureBytes,
        .parameterBytes = type.parameterBytes,
    };
    if ((desc.flags & PrRenderMaterialFlag_DoubleSided) != 0)
        record->flags |= kPrRenderMaterialDoubleSided;
    if (behind)
        record->flags |= kPrRenderMaterialBehind;
    if (desc.emission == PrRenderEmission_Display)
        record->flags |= kPrRenderMaterialDisplayEmission;
    // The scene behind a surface takes the last of a draw's slots from the material's textures.
    uint32_t slotCount = r->bindings[RenderLibrary_Forward].materialTextureCount;
    slots->behind = behind;
    if (behind && slotCount != 0)
        --slotCount;
    for (uint32_t i = 0; i < kRenderMaxMaterialSlots; ++i) {
        slots->textures[i] = r->white;
        slots->samplers[i] = r->whiteSampler;
    }

    uint32_t slot = 0;
    for (uint32_t i = 0; i < type.textureCount; ++i) {
        PrRenderTextureUse use =
            i < desc.textureCount ? desc.textures[i] : prRenderTextureUseDefault(PrRenderTexture{});
        const PrAffine2& t = use.transform;
        PrRenderMaterialTextureRecord texture{
            .transform = {{t.a, t.c, t.e, 0}, {t.b, t.d, t.f, 0}},
            .value = {1, 1, 1, 1},
            .scale = {use.scale[0], use.scale[1], use.scale[2], use.scale[3]},
            .bias = {use.bias[0], use.bias[1], use.bias[2], use.bias[3]},
            .texture = r->white.id,
            .sampler = r->whiteSampler.id,
            .slot = kPrRenderMaterialNoSlot,
            .flags = uint32_t(use.texCoord) << kPrRenderTextureSetShift |
                uint32_t(use.channel & 3) << kPrRenderTextureChannelShift,
        };
        uint32_t id = use.texture.id;
        if (id != 0) {
            // A texture that the material names and that has no texels is the value that the type declares for it, not
            // the white of a texture that the material does not name.
            texture.flags |= kPrRenderTextureNamed;
            if (id < r->textures.count && r->textures.data[id].ready) {
                texture.texture = r->textures.data[id].texture.id;
                texture.sampler = state.samplers != nullptr ? state.samplers[i].id : r->whiteSampler.id;
            } else {
                texture.texture = r->pendingTextures.data[r->pendingFirst.data[desc.type] + i].id;
            }
            // A type whose textures fit a draw's slots has each in the slot of its index, which its shaders name. A
            // larger type's material has the textures that it names in the slots, in the order of their indices, and
            // the indices that name one texture share its slot: an image that packs occlusion, roughness and metallic,
            // or a layer's strength and its roughness, costs one.
            bool indexed = type.textureCount <= r->bindings[RenderLibrary_Forward].materialTextureCount;
            uint32_t shared = kPrRenderMaterialNoSlot;
            for (uint32_t k = 0; !indexed && k < slot; ++k) {
                if (slots->textures[k].id == texture.texture && slots->samplers[k].id == texture.sampler)
                    shared = k;
            }
            if (shared != kPrRenderMaterialNoSlot) {
                texture.slot = shared;
            } else if (indexed ? i < slotCount : slot < slotCount) {
                texture.slot = indexed ? i : slot;
                slots->textures[texture.slot] = PrGpuTexture{texture.texture};
                slots->samplers[texture.slot] = PrGpuSampler{texture.sampler};
                ++slot;
            } else {
                // The stand-in's texel is of half floats, and the value here is what a shader would sample of it.
                texture.value = halfTexel(type.textures[i].standIn);
            }
        }
        prMemCopy(bytes + offset + i * kPrRenderMaterialTextureBytes, &texture, sizeof(texture));
    }
    prMemCopy(bytes + record->parameters, desc.parameters, type.parameterBytes);
    return offset + materialDataBytes(type);
}

// The flags of a material's record that decide which bucket its draws are in.
constexpr uint32_t kMaterialDrawFlags =
    kPrRenderMaterialAlphaMode | kPrRenderMaterialDoubleSided | kPrRenderMaterialBehind;

// Writes every material's record again and uploads them. A change to a material's render states changes its draws.
void uploadMaterials(Render* r) {
    uint32_t count = r->materials.count;
    uint32_t oldCount = r->materialRecords.count;
    bool drawsChanged = oldCount != count;
    uint32_t gltf = r->libraryTypes[RenderLibraryType_KhronosPbr];
    uint32_t size = count * kPrRenderMaterialBytes;
    for (uint32_t i = 0; i < count; ++i) {
        const PrRenderMaterialDesc* desc = r->materials.data[i].desc;
        size += materialDataBytes(r->shaders->materialTypes[desc != nullptr ? desc->type : gltf]);
    }
    r->materialBytes.resize(size);
    prMemZero(r->materialBytes.data, size);
    r->materialRecords.resize(count);
    r->materialSlots.resize(count);

    uint32_t offset = count * kPrRenderMaterialBytes;
    for (uint32_t i = 0; i < count; ++i) {
        PrRenderMaterialRecord record;
        offset = writeMaterial(r, r->materials.data[i], &record, &r->materialSlots.data[i], r->materialBytes.data, offset);
        if (i < oldCount) {
            const PrRenderMaterialRecord& old = r->materialRecords.data[i];
            if (((record.flags ^ old.flags) & kMaterialDrawFlags) != 0 || record.variant != old.variant)
                drawsChanged = true;
        }
        r->materialRecords.data[i] = record;
        prMemCopy(r->materialBytes.data + size_t(i) * kPrRenderMaterialBytes, &record, sizeof(record));
    }
    if (drawsChanged)
        markDrawsDirty(r);
    renderRecordBufferReserve(r, &r->materialBuffer, size, PrGpuBufferUsageFlag_Storage, "render materials");
    if (r->materialBuffer.buffer.id != 0)
        prGpuBufferUpload(r->device, r->materialBuffer.buffer, 0, r->materialBytes.data, size);
}

// The most bytes of the parameter record of a library's type.
constexpr uint32_t kRenderMaxParameterBytes = 256;

// A parameter record of a type of the renderer's set, written by its parameters' names.
struct NamedRecord {
    const PrRenderMaterialType* type;
    uint8_t bytes[kRenderMaxParameterBytes];

    void init(const Render* r, uint32_t id) {
        type = &r->shaders->materialTypes[id];
        PR_ASSERT(type->parameterBytes <= sizeof(bytes));
        prRenderMaterialParametersDefault(type, bytes);
    }

    // Sets the parameter `name`, which the type has, to `count` numbers.
    void set(PrStringView name, const float* values, uint32_t count) {
        uint32_t parameter = prRenderMaterialParameterFromName(type, name);
        PR_ASSERT(parameter != kPrRenderMaterialParameterMissing);
        prRenderMaterialParameterSet(type, bytes, parameter, values, count);
    }

    void set(PrStringView name, float value) {
        set(name, &value, 1);
    }

    void set(PrStringView name, PrVec3 value) {
        const float values[3] = {value.x, value.y, value.z};
        set(name, values, 3);
    }

    void set(PrStringView name, PrColor value) {
        const float values[4] = {value.r, value.g, value.b, value.a};
        set(name, values, 4);
    }
};

// Returns the description of a glTF material, of the type `type`, whose parameters and textures are at `parameters` and
// `textures`.
PrRenderMaterialDesc gltfDesc(uint32_t type, const PrRenderKhronosPbrMaterial& m, const NamedRecord& parameters,
        const PrRenderTextureUse* textures, uint32_t textureCount) {
    PrRenderMaterialDesc desc = prRenderMaterialDescDefault(type, parameters.bytes, parameters.type->parameterBytes);
    desc.alphaMode = m.alphaMode;
    desc.flags = m.flags;
    desc.alphaCutoff = m.alphaCutoff;
    desc.emission = m.emission;
    desc.textures = textures;
    desc.textureCount = textureCount;
    return desc;
}

// Writes the core material's parameters of a glTF material, which both of glTF's types have.
void gltfCore(const PrRenderKhronosPbrMaterial& m, NamedRecord* record) {
    record->set("baseColor", m.baseColor);
    record->set("emissive", m.emissive);
    record->set("emissiveStrength", m.emissiveStrength);
    record->set("metallic", m.metallic);
    record->set("roughness", m.roughness);
    record->set("normalScale", m.normalScale);
    record->set("occlusionStrength", m.occlusionStrength);
}

// Writes the textures of a glTF material by the layered type's indices, of which the core type's are the first.
void gltfTextures(const PrRenderKhronosPbrMaterial& m, PrRenderTextureUse* textures) {
    textures[kRenderLibraryKhronosPbrLayeredTextureBaseColor] = m.baseColorTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureMetallicRoughness] = m.metallicRoughnessTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureNormal] = m.normalTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureOcclusion] = m.occlusionTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureEmissive] = m.emissiveTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureClearcoat] = m.clearcoatTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureClearcoatRoughness] = m.clearcoatRoughnessTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureClearcoatNormal] = m.clearcoatNormalTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureSheenColor] = m.sheenColorTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureSheenRoughness] = m.sheenRoughnessTexture;
    PrRenderTextureUse none = prRenderTextureUseDefault(PrRenderTexture{});
    textures[kRenderLibraryKhronosPbrLayeredTextureSpecular] = m.specularGlossiness ? none : m.specularTexture;
    PrRenderTextureUse specularColor = m.specularGlossiness ? none : m.specularColorTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureSpecularColor] = specularColor;
    textures[kRenderLibraryKhronosPbrLayeredTextureIridescence] = m.iridescenceTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureIridescenceThickness] = m.iridescenceThicknessTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureAnisotropy] = m.anisotropyTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureTransmission] = m.transmissionTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureThickness] = m.thicknessTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureDiffuseTransmission] = m.diffuseTransmissionTexture;
    textures[kRenderLibraryKhronosPbrLayeredTextureDiffuseTransmissionColor] = m.diffuseTransmissionColorTexture;
}

// The core type's textures are the first of the layered type's, by the same indices.
static_assert(kRenderLibraryKhronosPbrTextureBaseColor == kRenderLibraryKhronosPbrLayeredTextureBaseColor);
static_assert(kRenderLibraryKhronosPbrTextureEmissive == kRenderLibraryKhronosPbrLayeredTextureEmissive);
static_assert(kRenderLibraryKhronosPbrTextureCount == kRenderLibraryKhronosPbrLayeredTextureClearcoat);

// Writes the fallbacks of a layout's attributes into each of `count` vertices of the layout at `out`.
void writeFallbacks(const PrRenderVertexLayout& layout, uint32_t count, uint8_t* out) {
    for (uint32_t a = 0; a < layout.attributeCount; ++a) {
        const PrRenderVertexAttribute& attribute = layout.attributes[a];
        const float fallback[4] = {
            attribute.fallback.x, attribute.fallback.y, attribute.fallback.z,
            attribute.fallback.w,
        };
        prMeshAttributeConvert(PrMeshAttributeView{}, fallback,
            PrMeshAttribute{out + attribute.offset, count, layout.stride, attribute.format});
    }
}

} // namespace

uint32_t renderPartsTake(Render* render, uint32_t count) {
    uint32_t first = renderRangesTake(&render->partRanges, count);
    renderArrayGrow(&render->parts, render->partRanges.end);
    renderPartsTouch(render, first, count);
    return first;
}

void renderPartsGive(Render* render, uint32_t first, uint32_t count) {
    if (count == 0)
        return;
    // A part that nothing uses is empty, so that a stale draw of it draws nothing.
    prMemZero(render->parts.data + first, size_t(count) * sizeof(PrRenderPartRecord));
    renderPartsTouch(render, first, count);
    renderRangesGive(&render->partRanges, first, count);
}

void renderPartsTouch(Render* render, uint32_t first, uint32_t count) {
    if (count == 0)
        return;
    if (first < render->partsDirtyFirst)
        render->partsDirtyFirst = first;
    if (first + count - 1 > render->partsDirtyLast)
        render->partsDirtyLast = first + count - 1;
}

void renderPartsUpload(Render* render) {
    Render* r = render;
    uint64_t size = uint64_t(r->parts.count) * sizeof(PrRenderPartRecord);
    if (renderRecordBufferReserve(r, &r->partBuffer, size, PrGpuBufferUsageFlag_Storage, "render parts")) {
        r->partsDirtyFirst = 0;
        r->partsDirtyLast = r->parts.count - 1;
    }
    if (r->partBuffer.buffer.id != 0 && r->parts.count != 0 && r->partsDirtyFirst <= r->partsDirtyLast) {
        uint64_t offset = uint64_t(r->partsDirtyFirst) * sizeof(PrRenderPartRecord);
        uint64_t bytes = uint64_t(r->partsDirtyLast - r->partsDirtyFirst + 1) * sizeof(PrRenderPartRecord);
        prGpuBufferUpload(r->device, r->partBuffer.buffer, offset, r->parts.data + r->partsDirtyFirst, bytes);
    }
    r->partsDirtyFirst = 0xffffffffu;
    r->partsDirtyLast = 0;
}

PrGpuBuffer renderMeshStream(Render* render, uint32_t mesh, uint32_t layout, bool* fallback) {
    Render* r = render;
    RenderMeshState& state = r->meshes.data[mesh];
    *fallback = false;
    for (uint32_t i = 0; i < state.streams.count; ++i) {
        if (state.streams.data[i].layout == layout) {
            *fallback = state.streams.data[i].fallback;
            return state.streams.data[i].buffer;
        }
    }
    const PrRenderVertexLayout& described = r->shaders->layouts[layout];
    if (described.attributeCount == 0)
        return PrGpuBuffer{};
    *fallback = true;
    uint64_t size = uint64_t(state.vertexCount) * described.stride;
    uint8_t* bytes = static_cast<uint8_t*>(prMemoryAlloc(size_t(size)));
    writeFallbacks(described, state.vertexCount, bytes);
    RenderAttributeStream stream{.layout = layout, .fallback = true};
    createBufferOf(r, bytes, size, PrGpuBufferUsageFlag_Vertex, "render fallbacks", &stream.buffer);
    prMemoryFree(bytes);
    state.streams.push(stream);
    return stream.buffer;
}

void renderResourcesApply(Render* render, RawArray<RenderOp>* ops) {
    for (uint32_t i = 0; i < ops->count; ++i)
        applyOp(render, ops->data[i]);
    if (render->materialsDirty)
        uploadMaterials(render);
    render->materialsDirty = false;
}

void renderOpQueue(Render* render, const RenderOp& op) {
    prMutexLock(&render->lock);
    render->ops.push(op);
    prMutexUnlock(&render->lock);
}

uint32_t renderIdTake(Render* render, RenderIds* ids) {
    prMutexLock(&render->lock);
    uint32_t id = renderIdsTake(ids);
    prMutexUnlock(&render->lock);
    return id;
}

void renderIdGive(Render* render, RenderIds* ids, uint32_t id) {
    prMutexLock(&render->lock);
    renderIdsGive(ids, id);
    prMutexUnlock(&render->lock);
}

void renderOpDiscard(Render* render, const RenderOp& op) {
    RenderMeshDeform deform = op.deform;
    releaseDeform(render, &deform);
    prGpuBufferRelease(render->device, op.geometry);
    prGpuBufferRelease(render->device, op.indices);
    for (uint32_t i = 0; op.streams != nullptr && i < op.streamCount; ++i)
        prGpuBufferRelease(render->device, op.streams[i].buffer);
    prGpuTextureRelease(render->device, op.texture);
    if (op.environment != nullptr)
        prGpuTextureRelease(render->device, op.environment->cube);
    prMemoryFree(op.parts);
    prMemoryFree(op.streams);
    prMemoryFree(op.material.desc);
    prMemoryFree(op.environment);
}

void renderResourcesReset(Render* render) {
    Render* r = render;
    for (uint32_t i = 0; i < r->meshes.count; ++i) {
        prGpuBufferRelease(r->device, r->meshes.data[i].geometry);
        prGpuBufferRelease(r->device, r->meshes.data[i].indices);
        releaseStreams(r, &r->meshes.data[i]);
        releaseDeform(r, &r->meshes.data[i].deform);
    }
    for (uint32_t i = 0; i < r->textures.count; ++i)
        prGpuTextureRelease(r->device, r->textures.data[i].texture);
    for (uint32_t i = 0; i < r->environments.count; ++i)
        prGpuTextureRelease(r->device, r->environments.data[i].cube);
    prGpuBufferRelease(r->device, r->partBuffer.buffer);
    prGpuBufferRelease(r->device, r->materialBuffer.buffer);
    r->meshes.reset();
    r->textures.reset();
    for (uint32_t i = 0; i < r->materials.count; ++i)
        prMemoryFree(r->materials.data[i].desc);
    r->materials.reset();
    r->materialRecords.reset();
    r->materialSlots.reset();
    r->materialBytes.reset();
    r->environments.reset();
    r->parts.reset();
    renderRangesReset(&r->partRanges);
}

} // namespace pr_impl

using namespace pr_impl;

PrRenderGeometryVertex prRenderGeometryVertexPack(PrVec3 position, PrVec3 normal, PrVec3 tangent, float handedness) {
    return PrRenderGeometryVertex{
        .position = {position.x, position.y, position.z},
        .normal = {packSnorm16(normal.x), packSnorm16(normal.y), packSnorm16(normal.z), 0},
        .tangent = {packSnorm16(tangent.x), packSnorm16(tangent.y), packSnorm16(tangent.z), packSnorm16(handedness)},
    };
}

PrRenderSkinVertex prRenderSkinVertexPack(const uint16_t* joints, const float* weights) {
    float total = 0;
    for (uint32_t i = 0; i < kPrRenderSkinInfluences; ++i)
        total += weights[i] > 0 ? weights[i] : 0;
    PrRenderSkinVertex vertex{};
    if (!(total > 0) || !prF32IsFinite(total))
        return vertex;
    uint32_t sum = 0;
    uint32_t largest = 0;
    for (uint32_t i = 0; i < kPrRenderSkinInfluences; ++i) {
        float weight = weights[i] > 0 ? weights[i] / total : 0;
        vertex.joints[i] = joints[i];
        vertex.weights[i] = uint16_t(prF32Round(weight * 65535));
        sum += vertex.weights[i];
        if (vertex.weights[i] > vertex.weights[largest])
            largest = i;
    }
    // The rounding of four weights leaves their sum within two of 65535, which the largest takes up.
    vertex.weights[largest] = uint16_t(vertex.weights[largest] + 65535 - sum);
    return vertex;
}

PrRenderMesh prRenderMeshCreate(PrRender* render) {
    return PrRenderMesh{renderIdTake(render, &render->meshIds)};
}

PrStatus prRenderMeshSetData(PrRender* render, PrRenderMesh mesh, const PrRenderMeshData* data) {
    Render* r = render;
    PR_ASSERT(mesh.id != 0 && data->vertexCount != 0 && data->indexCount != 0 && data->partCount != 0);
    RenderOp op{
        .kind = RenderOpKind_MeshData,
        .id = mesh.id,
        .vertexCount = data->vertexCount,
        .partCount = data->partCount,
    };
    bool skinned = data->skin != nullptr && data->jointCount != 0;
    bool morphed = data->morphs != nullptr && data->morphTargetCount != 0;
    // A device without bindless binds what the deformation reads to slots, which take a buffer with the storage usage.
    PrGpuBufferUsageFlags geometryUsage = PrGpuBufferUsageFlag_Vertex;
    if (skinned || morphed)
        geometryUsage |= PrGpuBufferUsageFlag_Storage;
    PrStatus status = createBufferOf(r, data->geometry, uint64_t(data->vertexCount) * sizeof(PrRenderGeometryVertex),
        geometryUsage, "render geometry", &op.geometry);
    if (status == PrStatus_Ok) {
        status = createBufferOf(r, data->indices, uint64_t(data->indexCount) * sizeof(uint32_t),
            PrGpuBufferUsageFlag_Index, "render indices", &op.indices);
    }
    if (data->streamCount != 0) {
        op.streams = static_cast<RenderAttributeStream*>(prMemoryAllocZeroed(size_t(data->streamCount) *
            sizeof(RenderAttributeStream)));
    }
    for (uint32_t i = 0; status == PrStatus_Ok && i < data->streamCount; ++i) {
        const PrRenderMeshStream& stream = data->streams[i];
        uint32_t layout = 0;
        while (layout < r->shaders->layoutCount && &r->shaders->layouts[layout] != stream.layout)
            ++layout;
        PR_ASSERT(layout < r->shaders->layoutCount);
        op.streams[op.streamCount] = RenderAttributeStream{.layout = layout};
        if (stream.layout->stride != 0) {
            status = createBufferOf(r, stream.vertices, uint64_t(data->vertexCount) * stream.layout->stride,
                PrGpuBufferUsageFlag_Vertex, "render attributes", &op.streams[op.streamCount].buffer);
        }
        ++op.streamCount;
    }
    if (status == PrStatus_Ok && skinned) {
        status = createBufferOf(r, data->skin, uint64_t(data->vertexCount) * sizeof(PrRenderSkinVertex),
            PrGpuBufferUsageFlag_Storage, "render skin", &op.deform.skin);
    }
    if (status == PrStatus_Ok && morphed) {
        uint64_t size = uint64_t(data->vertexCount) * data->morphTargetCount * sizeof(PrRenderMorphDelta);
        status = createBufferOf(r, data->morphs, size, PrGpuBufferUsageFlag_Storage, "render morph targets",
            &op.deform.morphs);
    }
    if (status != PrStatus_Ok) {
        renderOpDiscard(r, op);
        return status;
    }
    op.deform.vertexCount = data->vertexCount;
    if (skinned) {
        op.deform.jointCount = data->jointCount;
        op.deform.jointBounds = jointBounds(*data);
    }
    if (morphed) {
        op.deform.morphTargetCount = data->morphTargetCount;
        op.deform.morphReach = morphReach(*data);
    }
    op.parts = static_cast<PrRenderPartRecord*>(prMemoryAlloc(size_t(data->partCount) * sizeof(PrRenderPartRecord)));
    for (uint32_t i = 0; i < data->partCount; ++i) {
        const PrRenderMeshPart& part = data->parts[i];
        PR_ASSERT(part.indexCount % 3 == 0 && part.firstIndex <= data->indexCount &&
            part.indexCount <= data->indexCount - part.firstIndex);
        PrRenderVec4 bounds{part.bounds.center.x, part.bounds.center.y, part.bounds.center.z, part.bounds.radius};
        if (!(part.bounds.radius > 0))
            bounds = partBounds(*data, part);
        op.parts[i] = PrRenderPartRecord{
            .bounds = bounds,
            .firstIndex = part.firstIndex,
            .indexCount = part.indexCount,
            .geometry = op.geometry.id,
            .indices = op.indices.id,
        };
    }
    renderOpQueue(r, op);
    return PrStatus_Ok;
}

void prRenderMeshDestroy(PrRender* render, PrRenderMesh mesh) {
    if (mesh.id != 0)
        renderOpQueue(render, RenderOp{.kind = RenderOpKind_MeshDestroy, .id = mesh.id});
}

bool prRenderMeshIsReady(const PrRender* render, PrRenderMesh mesh) {
    return mesh.id != 0 && mesh.id < render->meshes.count && render->meshes.data[mesh.id].ready;
}

PrRenderTexture prRenderTextureCreate(PrRender* render) {
    Render* r = render;
    RenderOp op{.kind = RenderOpKind_TextureCreate};
    prMutexLock(&r->lock);
    op.id = renderIdsTake(&r->textureIds);
    r->ops.push(op);
    prMutexUnlock(&r->lock);
    return PrRenderTexture{op.id};
}

PrStatus prRenderTextureSetData(PrRender* render, PrRenderTexture texture, const PrRenderTextureData* data) {
    Render* r = render;
    PR_ASSERT(texture.id != 0 && data->levelCount >= 1 && data->levelCount <= kPrRenderTextureMaxLevels);
    PR_ASSERT(data->extent.x != 0 && data->extent.y != 0);
    PrGpuFormat format = data->format;
    if (!prGpuDeviceSupportsFormat(r->device, format, PrGpuTextureUsageFlag_Sampled))
        return prGpuDeviceIsLost(r->device) ? PrStatus_Lost : PrStatus_NotSupported;

    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled;
    uint8_t levels = data->levelCount;
    PrGpuTextureUsageFlags filled = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    bool fills = levels == 1 && prGpuMipLevels(data->extent) > 1 && prGpuFormatFilters(format) &&
        prGpuDeviceSupportsFormat(r->device, format, filled);
    if (fills) {
        usage = filled;
        levels = prGpuMipLevels(data->extent);
    }
    PrGpuTextureDesc desc = prGpuTextureDesc2d(format, data->extent, usage);
    desc.levels = levels;
    desc.name = "render texture";
    RenderOp op{.kind = RenderOpKind_TextureData, .id = texture.id};
    PrStatus status = prGpuTextureCreate(r->device, &desc, &op.texture);
    if (status != PrStatus_Ok)
        return status;

    PrGpuFormatBlock block = prGpuFormatBlock(format);
    for (uint32_t level = 0; level < data->levelCount; ++level) {
        PrUVec2 extent{levelSide(data->extent.x, level), levelSide(data->extent.y, level)};
        uint32_t rowBytes = data->levels[level].rowBytes;
        if (rowBytes == 0)
            rowBytes = (extent.x + block.width - 1) / block.width * block.bytes;
        PR_ASSERT(data->levels[level].data != nullptr && rowBytes % block.bytes == 0);
        uint32_t rows = (extent.y + block.height - 1) / block.height;
        PrGpuTextureRegion region = prGpuTextureRegion2d(PrUVec2{0, 0}, extent);
        region.level = uint8_t(level);
        prGpuTextureUpload(r->device, op.texture, &region, data->levels[level].data, rowBytes, rowBytes * rows);
    }
    PrGpuCommands* commands = nullptr;
    if (fills && prGpuCommandsBegin(r->device, nullptr, &commands)) {
        prGpuCommandsGenerateMipmaps(commands, op.texture, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmit(commands);
    }
    renderOpQueue(r, op);
    return PrStatus_Ok;
}

void prRenderTextureSetGpu(PrRender* render, PrRenderTexture texture, PrGpuTexture gpu) {
    PR_ASSERT(texture.id != 0 && gpu.id != 0);
    prGpuTextureRetain(render->device, gpu);
    renderOpQueue(render, RenderOp{.kind = RenderOpKind_TextureData, .id = texture.id, .texture = gpu});
}

void prRenderTextureDestroy(PrRender* render, PrRenderTexture texture) {
    if (texture.id != 0)
        renderOpQueue(render, RenderOp{.kind = RenderOpKind_TextureDestroy, .id = texture.id});
}

bool prRenderTextureIsReady(const PrRender* render, PrRenderTexture texture) {
    return texture.id != 0 && texture.id < render->textures.count && render->textures.data[texture.id].ready;
}

PrRenderEmission prRenderEmissionFromName(PrStringView name) {
    for (PrRenderEmission emission = 0; emission < PrRenderEmission_Count; ++emission) {
        if (prAsciiEqualsFold(kEmissionNames[emission], name))
            return emission;
    }
    return PrRenderEmission_Count;
}

PrStringView prRenderEmissionName(PrRenderEmission emission) {
    return emission < PrRenderEmission_Count ? kEmissionNames[emission] : PrStringView{};
}

PrRenderTextureUse prRenderTextureUseDefault(PrRenderTexture texture) {
    return PrRenderTextureUse{
        .texture = texture,
        .transform = prAffine2Identity(),
        .sampler = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Repeat),
        .scale = {1, 1, 1, 1},
    };
}

PrAffine2 prRenderTextureTransform(PrVec2 offset, float rotation, PrVec2 scale) {
    // KHR_texture_transform's matrix is translation times rotation times scale, with its rotation's sine above the
    // diagonal.
    float c = prF32Cos(rotation);
    float s = prF32Sin(rotation);
    return PrAffine2{
        .a = c * scale.x,
        .b = -s * scale.x,
        .c = s * scale.y,
        .d = c * scale.y,
        .e = offset.x,
        .f = offset.y,
    };
}

PrRenderMaterialDesc prRenderMaterialDescDefault(uint32_t type, const void* parameters, size_t parameterBytes) {
    return PrRenderMaterialDesc{
        .type = type,
        .alphaMode = PrRenderAlphaMode_Opaque,
        .alphaCutoff = 0.5f,
        .parameters = parameters,
        .parameterBytes = parameterBytes,
    };
}

PrRenderKhronosPbrMaterial prRenderKhronosPbrMaterialDefault(void) {
    PrRenderTextureUse none = prRenderTextureUseDefault(PrRenderTexture{});
    return PrRenderKhronosPbrMaterial{
        .alphaMode = PrRenderAlphaMode_Opaque,
        .alphaCutoff = 0.5f,
        .baseColor = {1, 1, 1, 1},
        .metallic = 1,
        .roughness = 1,
        .normalScale = 1,
        .occlusionStrength = 1,
        .emissiveStrength = 1,
        .baseColorTexture = none,
        .metallicRoughnessTexture = none,
        .normalTexture = none,
        .occlusionTexture = none,
        .emissiveTexture = none,
        .ior = 1.5f,
        .specular = 1,
        .specularColor = {1, 1, 1},
        .specularTexture = none,
        .specularColorTexture = none,
        .clearcoatNormalScale = 1,
        .clearcoatTexture = none,
        .clearcoatRoughnessTexture = none,
        .clearcoatNormalTexture = none,
        .sheenColorTexture = none,
        .sheenRoughnessTexture = none,
        .iridescenceIor = 1.3f,
        .iridescenceThicknessMinimum = 100,
        .iridescenceThicknessMaximum = 400,
        .iridescenceTexture = none,
        .iridescenceThicknessTexture = none,
        .anisotropyTexture = none,
        .transmissionTexture = none,
        .thicknessTexture = none,
        .attenuationColor = {1, 1, 1},
        .diffuseTransmissionColor = {1, 1, 1},
        .diffuseTransmissionTexture = none,
        .diffuseTransmissionColorTexture = none,
        .glossiness = 1,
    };
}

PrRenderUnlitMaterial prRenderUnlitMaterialDefault(void) {
    return PrRenderUnlitMaterial{
        .alphaMode = PrRenderAlphaMode_Opaque,
        .alphaCutoff = 0.5f,
        .color = {1, 1, 1, 1},
        .colorTexture = prRenderTextureUseDefault(PrRenderTexture{}),
    };
}

PrRenderUsdPreviewMaterial prRenderUsdPreviewMaterialDefault(void) {
    PrRenderUsdPreviewMaterial m{
        .diffuseColor = {0.18f, 0.18f, 0.18f},
        .roughness = 0.5f,
        .clearcoatRoughness = 0.01f,
        .opacity = 1,
        .ior = 1.5f,
        .occlusion = 1,
    };
    PrRenderTextureUse none = prRenderTextureUseDefault(PrRenderTexture{});
    PrRenderTextureUse* uses[] = {
        &m.diffuseColorTexture, &m.normalTexture, &m.roughnessTexture, &m.metallicTexture, &m.occlusionTexture,
        &m.emissiveColorTexture, &m.opacityTexture, &m.specularColorTexture, &m.clearcoatTexture,
        &m.clearcoatRoughnessTexture,
    };
    for (PrRenderTextureUse* use : uses)
        *use = none;
    return m;
}

PrRenderMaterial prRenderMaterialCreate(PrRender* render) {
    return PrRenderMaterial{renderIdTake(render, &render->materialIds)};
}

void prRenderMaterialSet(PrRender* render, PrRenderMaterial material, const PrRenderMaterialDesc* desc) {
    PR_ASSERT(material.id != 0);
    PR_ASSERT(desc->type < render->shaders->materialTypeCount);
    PR_ASSERT(desc->parameterBytes == render->shaders->materialTypes[desc->type].parameterBytes);
    PR_ASSERT(desc->textureCount <= render->shaders->materialTypes[desc->type].textureCount);
    PR_ASSERT(desc->attributes < render->shaders->materialTypes[desc->type].variantCount);
    // The pipelines are made before the material is queued, so that no frame draws it without them.
    renderPipelinesWarm(render, *desc);
    RenderOp op{.kind = RenderOpKind_MaterialSet, .id = material.id};
    op.material = materialCopy(render, *desc);
    renderOpQueue(render, op);
}

void prRenderMaterialSetKhronosPbr(PrRender* render, PrRenderMaterial material, const PrRenderKhronosPbrMaterial* gltf) {
    const PrRenderKhronosPbrMaterial& m = *gltf;
    uint32_t type = render->libraryTypes[RenderLibraryType_KhronosPbr];
    NamedRecord parameters;
    parameters.init(render, type);
    gltfCore(m, &parameters);
    PrRenderTextureUse textures[kRenderLibraryKhronosPbrLayeredTextureCount];
    gltfTextures(m, textures);
    // The core material's flags have no layer to read the scene behind it.
    PrRenderMaterialDesc desc = gltfDesc(type, m, parameters, textures, kRenderLibraryKhronosPbrTextureCount);
    desc.flags &= ~PrRenderMaterialFlag_ReadsBehind;
    prRenderMaterialSet(render, material, &desc);
}

void prRenderMaterialSetKhronosPbrLayered(PrRender* render, PrRenderMaterial material, const PrRenderKhronosPbrMaterial* gltf) {
    const PrRenderKhronosPbrMaterial& m = *gltf;
    uint32_t type = render->libraryTypes[RenderLibraryType_KhronosPbrLayered];
    NamedRecord parameters;
    parameters.init(render, type);
    gltfCore(m, &parameters);
    parameters.set("ior", m.ior);
    parameters.set("specular", m.specularGlossiness ? 1.0f : m.specular);
    parameters.set("specularColor", m.specularColor);
    parameters.set("clearcoat", m.clearcoat);
    parameters.set("clearcoatRoughness", m.clearcoatRoughness);
    parameters.set("clearcoatNormalScale", m.clearcoatNormalScale);
    parameters.set("sheenColor", m.sheenColor);
    parameters.set("sheenRoughness", m.sheenRoughness);
    parameters.set("iridescence", m.iridescence);
    parameters.set("iridescenceIor", m.iridescenceIor);
    parameters.set("iridescenceThicknessMinimum", m.iridescenceThicknessMinimum);
    parameters.set("iridescenceThicknessMaximum", m.iridescenceThicknessMaximum);
    parameters.set("anisotropyStrength", m.anisotropy);
    parameters.set("anisotropyRotation", m.anisotropyRotation);
    parameters.set("transmission", m.transmission);
    parameters.set("thickness", m.thickness);
    parameters.set("attenuationDistance", m.attenuationDistance);
    parameters.set("attenuationColor", m.attenuationColor);
    parameters.set("dispersion", m.dispersion);
    parameters.set("diffuseTransmission", m.diffuseTransmission);
    parameters.set("diffuseTransmissionColor", m.diffuseTransmissionColor);
    parameters.set("specularGlossiness", m.specularGlossiness ? 1.0f : 0.0f);
    parameters.set("glossiness", m.glossiness);
    PrRenderTextureUse textures[kRenderLibraryKhronosPbrLayeredTextureCount];
    gltfTextures(m, textures);
    PrRenderMaterialDesc desc = gltfDesc(type, m, parameters, textures, kRenderLibraryKhronosPbrLayeredTextureCount);
    // A surface that transmits light reads the scene behind it.
    if (m.transmission > 0)
        desc.flags |= PrRenderMaterialFlag_ReadsBehind;
    prRenderMaterialSet(render, material, &desc);
}

void prRenderMaterialSetUnlit(PrRender* render, PrRenderMaterial material, const PrRenderUnlitMaterial* unlit) {
    const PrRenderUnlitMaterial& m = *unlit;
    uint32_t type = render->libraryTypes[RenderLibraryType_Unlit];
    NamedRecord parameters;
    parameters.init(render, type);
    parameters.set("baseColor", m.color);
    PrRenderMaterialDesc desc = prRenderMaterialDescDefault(type, parameters.bytes, parameters.type->parameterBytes);
    desc.alphaMode = m.alphaMode;
    desc.flags = m.flags;
    desc.alphaCutoff = m.alphaCutoff;
    desc.emission = PrRenderEmission_Display;
    desc.textures = &m.colorTexture;
    desc.textureCount = 1;
    prRenderMaterialSet(render, material, &desc);
}

void prRenderMaterialSetUsdPreview(PrRender* render, PrRenderMaterial material, const PrRenderUsdPreviewMaterial* preview) {
    const PrRenderUsdPreviewMaterial& m = *preview;
    uint32_t type = render->libraryTypes[RenderLibraryType_UsdPreview];
    NamedRecord parameters;
    parameters.init(render, type);
    parameters.set("diffuseColor", m.diffuseColor);
    parameters.set("opacity", m.opacity);
    parameters.set("emissiveColor", m.emissiveColor);
    parameters.set("ior", m.ior);
    parameters.set("specularColor", m.specularColor);
    parameters.set("useSpecularWorkflow", m.useSpecularWorkflow ? 1.0f : 0.0f);
    parameters.set("metallic", m.metallic);
    parameters.set("roughness", m.roughness);
    parameters.set("clearcoat", m.clearcoat);
    parameters.set("clearcoatRoughness", m.clearcoatRoughness);
    parameters.set("occlusion", m.occlusion);
    PrRenderTextureUse uses[kRenderLibraryUsdPreviewTextureCount];
    uses[kRenderLibraryUsdPreviewTextureDiffuseColor] = m.diffuseColorTexture;
    uses[kRenderLibraryUsdPreviewTextureNormal] = m.normalTexture;
    uses[kRenderLibraryUsdPreviewTextureRoughness] = m.roughnessTexture;
    uses[kRenderLibraryUsdPreviewTextureMetallic] = m.metallicTexture;
    uses[kRenderLibraryUsdPreviewTextureOcclusion] = m.occlusionTexture;
    uses[kRenderLibraryUsdPreviewTextureEmissiveColor] = m.emissiveColorTexture;
    uses[kRenderLibraryUsdPreviewTextureOpacity] = m.opacityTexture;
    uses[kRenderLibraryUsdPreviewTextureSpecularColor] = m.specularColorTexture;
    uses[kRenderLibraryUsdPreviewTextureClearcoat] = m.clearcoatTexture;
    uses[kRenderLibraryUsdPreviewTextureClearcoatRoughness] = m.clearcoatRoughnessTexture;
    PrRenderMaterialDesc desc = prRenderMaterialDescDefault(type, parameters.bytes, parameters.type->parameterBytes);
    desc.flags = m.flags;
    desc.emission = m.emission;
    desc.attributes = m.attributes;
    if (m.opacityThreshold > 0) {
        desc.alphaMode = PrRenderAlphaMode_Mask;
        desc.alphaCutoff = m.opacityThreshold;
    } else if (m.opacity < 1 || m.opacityTexture.texture.id != 0) {
        desc.alphaMode = PrRenderAlphaMode_Blend;
    }
    desc.textures = uses;
    desc.textureCount = kRenderLibraryUsdPreviewTextureCount;
    prRenderMaterialSet(render, material, &desc);
}

bool prRenderKhronosPbrMaterialIsLayered(const PrRenderKhronosPbrMaterial* gltf) {
    const PrRenderKhronosPbrMaterial& m = *gltf;
    return m.specularGlossiness || m.ior != 1.5f || m.specular != 1 || m.specularColor.x != 1 || m.specularColor.y != 1 ||
        m.specularColor.z != 1 || m.specularTexture.texture.id != 0 || m.specularColorTexture.texture.id != 0 ||
        m.clearcoat > 0 || m.sheenColor.x > 0 || m.sheenColor.y > 0 || m.sheenColor.z > 0 || m.iridescence > 0 ||
        m.anisotropy > 0 || m.transmission > 0 || m.diffuseTransmission > 0;
}

uint32_t prRenderMaterialTextureLimit(const PrRender* render, PrRenderMaterialFlags flags) {
    if (render->bindless)
        return UINT32_MAX;
    uint32_t slots = render->bindings[RenderLibrary_Forward].materialTextureCount;
    bool behind = (flags & PrRenderMaterialFlag_ReadsBehind) != 0;
    return behind && slots != 0 ? slots - 1 : slots;
}

void prRenderMaterialDestroy(PrRender* render, PrRenderMaterial material) {
    if (material.id != 0)
        renderOpQueue(render, RenderOp{.kind = RenderOpKind_MaterialDestroy, .id = material.id});
}
