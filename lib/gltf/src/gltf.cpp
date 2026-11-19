// glTF 2.0: an asset read into a document of its scenes, nodes, meshes, materials, animations and binary data.

#include "pr/gltf.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/string_ops.h"

#include "gltf/src/gltf_accessor.h"
#include "gltf/src/gltf_animation.h"
#include "gltf/src/gltf_document.h"
#include "gltf/src/gltf_meshopt.h"
#include "gltf/src/gltf_parse.h"

namespace pr_impl {

namespace {

// The header of binary glTF: its magic, the version of the container, and the length of the whole file
// (glTF-2.0/#binary-gltf-layout).
constexpr uint32_t kGlbMagic = 0x46546c67;
constexpr uint32_t kGlbVersion = 2;
constexpr size_t kGlbHeaderSize = 12;

// The header of a chunk, its length and its type, and the types of the two chunks (glTF-2.0/#chunks-types).
constexpr size_t kGlbChunkHeaderSize = 8;
constexpr uint32_t kGlbChunkJson = 0x4e4f534a;
constexpr uint32_t kGlbChunkBinary = 0x004e4942;

// The number of indices that unrolling a strip or a fan reads at a time.
constexpr uint32_t kIndexWindow = 256;

bool isGlb(PrByteView bytes) {
    return bytes.size >= 4 && prMemReadLeU32(bytes.data) == kGlbMagic;
}

// Reads the chunk at `*at` in the `length` bytes of a binary glTF file, and moves `*at` past it. Returns false if the
// chunk does not lie within the file, or its length is not a multiple of four (glTF-2.0/#chunks-v2).
bool glbChunk(PrByteView bytes, size_t length, size_t* at, uint32_t* type, PrByteView* data) {
    if (length - *at < kGlbChunkHeaderSize)
        return false;
    uint32_t chunkLength = prMemReadLeU32(bytes.data + *at);
    *type = prMemReadLeU32(bytes.data + *at + 4);
    *at += kGlbChunkHeaderSize;
    if (chunkLength % 4 != 0 || chunkLength > length - *at)
        return false;
    *data = PrByteView{bytes.data + *at, chunkLength};
    *at += chunkLength;
    return true;
}

// Finds the JSON chunk and the binary chunk of binary glTF. The JSON is the first chunk, and the binary chunk, if there
// is one, is the second. `*binary` is the null view if there is none. Returns InvalidData for a malformed file, and
// NotSupported for a container of another version.
PrStatus glbRead(PrByteView bytes, PrStringView* json, PrByteView* binary, const char** message) {
    if (bytes.size < kGlbHeaderSize) {
        *message = "the binary glTF header is cut short";
        return PrStatus_InvalidData;
    }
    if (prMemReadLeU32(bytes.data + 4) != kGlbVersion) {
        *message = "the binary glTF container is not version 2";
        return PrStatus_NotSupported;
    }
    size_t length = prMemReadLeU32(bytes.data + 8);
    if (length < kGlbHeaderSize || length > bytes.size) {
        *message = "the binary glTF length is not the length of the file";
        return PrStatus_InvalidData;
    }
    size_t at = kGlbHeaderSize;
    uint32_t type = 0;
    PrByteView data = {};
    if (!glbChunk(bytes, length, &at, &type, &data) || type != kGlbChunkJson) {
        *message = "the first chunk of the binary glTF is not a whole JSON chunk";
        return PrStatus_InvalidData;
    }
    *json = prStringFromBytes(data);
    *binary = PrByteView{nullptr, 0};
    if (at == length)
        return PrStatus_Ok;
    if (!glbChunk(bytes, length, &at, &type, &data)) {
        *message = "the second chunk of the binary glTF is malformed";
        return PrStatus_InvalidData;
    }
    // A chunk of a type that the specification does not define is ignored.
    if (type == kGlbChunkBinary)
        *binary = data;
    return PrStatus_Ok;
}

// The number of indices in the triangle list of `count` vertices assembled in `mode`.
uint32_t triangleIndexCount(PrGltfMode mode, uint32_t count) {
    switch (mode) {
        case PrGltfMode_Triangles:
            return count / 3 * 3;
        case PrGltfMode_TriangleStrip:
        case PrGltfMode_TriangleFan: {
            uint32_t triangles = count >= 3 ? count - 2 : 0;
            return (triangles < 0xffffffffu / 3 ? triangles : 0xffffffffu / 3) * 3;
        }
        default:
            return 0;
    }
}

// The position, among a primitive's assembled vertices, of corner `corner` of triangle `triangle`
// (glTF-2.0/#meshes-topology).
uint32_t trianglePosition(PrGltfMode mode, uint32_t triangle, uint32_t corner) {
    switch (mode) {
        case PrGltfMode_TriangleStrip: {
            // Every other triangle swaps its last two corners, which keeps the winding of the first.
            static const uint32_t kEven[3] = {0, 1, 2};
            static const uint32_t kOdd[3] = {0, 2, 1};
            return triangle + (triangle % 2 == 0 ? kEven[corner] : kOdd[corner]);
        }
        case PrGltfMode_TriangleFan:
            return corner == 2 ? 0 : triangle + 1 + corner;
        default:
            return triangle * 3 + corner;
    }
}

// A run of a primitive's indices, read as triangles ask for them.
struct IndexWindow {
    const Gltf* gltf;
    const PrGltfAccessor* PR_NULLABLE indices;  // null if the vertices are drawn in order
    uint32_t first;
    uint32_t count;
    uint32_t values[kIndexWindow];
};

// Returns the vertex at `position` among the primitive's assembled vertices.
uint32_t vertexAt(IndexWindow* window, uint32_t position) {
    if (window->indices == nullptr)
        return position;
    if (position < window->first || position - window->first >= window->count) {
        uint32_t left = window->indices->count - position;
        window->first = position;
        window->count = left < kIndexWindow ? left : kIndexWindow;
        gltfAccessorRead(window->gltf, *window->indices, position, window->count, GltfNumber_Integer, window->values);
    }
    return window->values[position - window->first];
}

// Loads the file that `uri`, a relative path that may be percent-encoded, names in the folder that `context` points to.
PrBlob* PR_NULLABLE gltfLoadBeside(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
    const PrStringView* folder = static_cast<const PrStringView*>(context);
    PrTextBuffer decoded = {};
    for (size_t i = 0; i < uri.size; ++i) {
        bool escape = uri.data[i] == '%' && i + 2 < uri.size && prAsciiIsHexDigit(uri.data[i + 1]) &&
            prAsciiIsHexDigit(uri.data[i + 2]);
        if (escape) {
            prTextBufferAppendChar(&decoded, char(prAsciiHexValue(uri.data[i + 1]) * 16 + prAsciiHexValue(uri.data[i + 2])));
            i += 2;
        } else {
            prTextBufferAppendChar(&decoded, uri.data[i]);
        }
    }
    PrTextBuffer path = {};
    prTextBufferAppend(&path, *folder);
    prFilePathAppend(&path, prTextBufferView(&decoded));
    PrBlob* blob = prFileOpen(prTextBufferView(&path));
    prTextBufferReset(&decoded);
    prTextBufferReset(&path);
    return blob;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrGltf : Gltf {};

// ---------------------------------------------------------------------------------------------------------------------
// Documents

PrGltfOptions prGltfOptionsDefault(void) {
    PrGltfOptions options{};
    return options;
}

PrStatus prGltfFileRead(PrStringView path, PrTextBuffer* PR_NULLABLE diagnostics, PrGltf* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrBlob* file = prFileOpen(path);
    if (file == nullptr)
        return PrStatus_Io;
    PrStringView folder = prFilePathParent(path);
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = gltfLoadBeside;
    options.loadContext = &folder;
    options.diagnostics = diagnostics;
    PrStatus status = prGltfCreate(file, &options, out);
    prBlobRelease(file);
    return status;
}

PrStatus prGltfCreate(PrBlob* file, const PrGltfOptions* PR_NULLABLE options, PrGltf* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrGltfOptions resolved = options != nullptr ? *options : prGltfOptionsDefault();
    PrByteView bytes = prBlobBytes(file);
    PrStringView json = prStringFromBytes(bytes);
    PrByteView binary = {nullptr, 0};
    if (isGlb(bytes)) {
        const char* message = nullptr;
        PrStatus status = glbRead(bytes, &json, &binary, &message);
        if (status != PrStatus_Ok) {
            if (resolved.diagnostics != nullptr)
                prTextBufferAppend(resolved.diagnostics, message);
            return status;
        }
    }

    PrGltf* gltf = allocZeroed<PrGltf>();
    if (binary.data != nullptr) {
        // The first buffer views the binary chunk in place.
        prBlobRetain(file);
        gltf->blobs.push(file);
    }
    PrStatus status = gltfParse(gltf, json, binary, resolved);
    if (status != PrStatus_Ok) {
        prGltfDestroy(gltf);
        return status;
    }
    *out = gltf;
    return PrStatus_Ok;
}

void prGltfDestroy(PrGltf* PR_NULLABLE gltf) {
    if (gltf == nullptr)
        return;
    gltfReset(gltf);
    prMemoryFree(gltf);
}

PrGltfAsset prGltfAsset(const PrGltf* gltf) {
    return gltf->asset;
}

const uint32_t* PR_NULLABLE prGltfListIndices(const PrGltf* gltf, PrGltfList list) {
    PR_ASSERT(list.first <= gltf->listIndices.count && list.count <= gltf->listIndices.count - list.first);
    return gltf->listIndices.data + list.first;
}

const float* PR_NULLABLE prGltfListFloats(const PrGltf* gltf, PrGltfList list) {
    PR_ASSERT(list.first <= gltf->listFloats.count && list.count <= gltf->listFloats.count - list.first);
    return gltf->listFloats.data + list.first;
}

// ---------------------------------------------------------------------------------------------------------------------
// Extensions

bool prGltfExtensionIsSupported(PrStringView name) {
    static const PrStringView kSupported[] = {
        "EXT_mesh_gpu_instancing",
        "EXT_meshopt_compression",
        "EXT_texture_webp",
        "KHR_animation_pointer",
        "KHR_draco_mesh_compression",
        "KHR_lights_punctual",
        "KHR_materials_anisotropy",
        "KHR_materials_clearcoat",
        "KHR_materials_diffuse_transmission",
        "KHR_materials_dispersion",
        "KHR_materials_emissive_strength",
        "KHR_materials_ior",
        "KHR_materials_iridescence",
        "KHR_materials_pbrSpecularGlossiness",
        "KHR_materials_sheen",
        "KHR_materials_specular",
        "KHR_materials_transmission",
        "KHR_materials_unlit",
        "KHR_materials_variants",
        "KHR_materials_volume",
        "KHR_mesh_quantization",
        "KHR_meshopt_compression",
        "KHR_node_visibility",
        "KHR_texture_basisu",
        "KHR_texture_transform",
    };
    for (PrStringView supported : kSupported) {
        if (name == supported)
            return true;
    }
    return false;
}

uint32_t prGltfUsedExtensionCount(const PrGltf* gltf) {
    return gltf->usedExtensions.count;
}

PrStringView prGltfUsedExtension(const PrGltf* gltf, uint32_t index) {
    PR_ASSERT(index < gltf->usedExtensions.count);
    return gltf->usedExtensions.data[index];
}

// ---------------------------------------------------------------------------------------------------------------------
// Binary data

uint32_t prGltfBufferCount(const PrGltf* gltf) {
    return gltf->buffers.count;
}

const PrGltfBuffer* PR_NULLABLE prGltfBuffers(const PrGltf* gltf) {
    return gltf->buffers.data;
}

uint32_t prGltfBufferViewCount(const PrGltf* gltf) {
    return gltf->bufferViews.count;
}

const PrGltfBufferView* PR_NULLABLE prGltfBufferViews(const PrGltf* gltf) {
    return gltf->bufferViews.data;
}

uint32_t prGltfAccessorCount(const PrGltf* gltf) {
    return gltf->accessors.count;
}

const PrGltfAccessor* PR_NULLABLE prGltfAccessors(const PrGltf* gltf) {
    return gltf->accessors.data;
}

PrByteView prGltfBufferViewBytes(const PrGltf* gltf, PrGltfBufferViewIndex bufferView) {
    return gltfBufferViewBytes(gltf, bufferView);
}

bool prGltfMeshoptDecode(PrByteView source, const PrGltfMeshopt* meshopt, uint8_t* out) {
    return gltfMeshoptDecode(source, *meshopt, out);
}

uint32_t prGltfComponentTypeSize(PrGltfComponentType type) {
    return gltfComponentSize(type);
}

uint32_t prGltfElementTypeComponentCount(PrGltfElementType type) {
    return gltfComponentCount(type);
}

uint32_t prGltfElementSize(PrGltfElementType type, PrGltfComponentType componentType) {
    return gltfElementSize(type, componentType);
}

bool prGltfAccessorBounds(const PrGltf* gltf, PrGltfAccessorIndex accessor, float* min, float* max) {
    PR_ASSERT(accessor < gltf->accessors.count);
    const PrGltfAccessor& record = gltf->accessors.data[accessor];
    if (record.min.count == 0 || record.max.count == 0)
        return false;
    // A normalized integer maps onto [0, 1] or [-1, 1] (glTF-2.0/#accessor-data-types).
    float scale = 1;
    float least = -kPrF32Infinity;
    if (record.normalized) {
        switch (record.componentType) {
            case PrGltfComponentType_Byte:
                scale = 127;
                least = -1;
                break;
            case PrGltfComponentType_UnsignedByte:
                scale = 255;
                break;
            case PrGltfComponentType_Short:
                scale = 32767;
                least = -1;
                break;
            default:
                scale = 65535;
                break;
        }
    }
    const float* low = gltf->listFloats.data + record.min.first;
    const float* high = gltf->listFloats.data + record.max.first;
    for (uint32_t c = 0; c < record.min.count; ++c) {
        min[c] = prF32Max(low[c] / scale, least);
        max[c] = prF32Max(high[c] / scale, least);
    }
    return true;
}

uint32_t prGltfAccessorStride(const PrGltf* gltf, PrGltfAccessorIndex accessor) {
    PR_ASSERT(accessor < gltf->accessors.count);
    const PrGltfAccessor& record = gltf->accessors.data[accessor];
    if (!record.draco && record.bufferView != kPrGltfNone && gltf->bufferViews.data[record.bufferView].byteStride != 0)
        return gltf->bufferViews.data[record.bufferView].byteStride;
    return gltfElementSize(record.type, record.componentType);
}

bool prGltfAccessorReadFloats(const PrGltf* gltf, PrGltfAccessorIndex accessor, uint32_t first, uint32_t count, float* out) {
    PR_ASSERT(accessor < gltf->accessors.count);
    return gltfAccessorRead(gltf, gltf->accessors.data[accessor], first, count, GltfNumber_Float, out);
}

bool prGltfAccessorReadIntegers(
        const PrGltf* gltf, PrGltfAccessorIndex accessor, uint32_t first, uint32_t count, uint32_t* out) {
    PR_ASSERT(accessor < gltf->accessors.count);
    return gltfAccessorRead(gltf, gltf->accessors.data[accessor], first, count, GltfNumber_Integer, out);
}

// ---------------------------------------------------------------------------------------------------------------------
// Images, samplers and textures

uint32_t prGltfImageCount(const PrGltf* gltf) {
    return gltf->images.count;
}

const PrGltfImage* PR_NULLABLE prGltfImages(const PrGltf* gltf) {
    return gltf->images.data;
}

uint32_t prGltfSamplerCount(const PrGltf* gltf) {
    return gltf->samplers.count;
}

const PrGltfSampler* PR_NULLABLE prGltfSamplers(const PrGltf* gltf) {
    return gltf->samplers.data;
}

uint32_t prGltfTextureCount(const PrGltf* gltf) {
    return gltf->textures.count;
}

const PrGltfTexture* PR_NULLABLE prGltfTextures(const PrGltf* gltf) {
    return gltf->textures.data;
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials

PrGltfMaterialExtensionFlags prGltfMaterialExtensionFromName(PrStringView name) {
    struct Named {
        PrStringView name;
        PrGltfMaterialExtensionFlags flag;
    };
    static const Named kNamed[] = {
        {"KHR_materials_clearcoat", PrGltfMaterialExtensionFlag_Clearcoat},
        {"KHR_materials_sheen", PrGltfMaterialExtensionFlag_Sheen},
        {"KHR_materials_transmission", PrGltfMaterialExtensionFlag_Transmission},
        {"KHR_materials_volume", PrGltfMaterialExtensionFlag_Volume},
        {"KHR_materials_ior", PrGltfMaterialExtensionFlag_Ior},
        {"KHR_materials_specular", PrGltfMaterialExtensionFlag_Specular},
        {"KHR_materials_iridescence", PrGltfMaterialExtensionFlag_Iridescence},
        {"KHR_materials_anisotropy", PrGltfMaterialExtensionFlag_Anisotropy},
        {"KHR_materials_dispersion", PrGltfMaterialExtensionFlag_Dispersion},
        {"KHR_materials_diffuse_transmission", PrGltfMaterialExtensionFlag_DiffuseTransmission},
        {"KHR_materials_emissive_strength", PrGltfMaterialExtensionFlag_EmissiveStrength},
        {"KHR_materials_unlit", PrGltfMaterialExtensionFlag_Unlit},
        {"KHR_materials_pbrSpecularGlossiness", PrGltfMaterialExtensionFlag_SpecularGlossiness},
    };
    for (const Named& named : kNamed) {
        if (named.name == name)
            return named.flag;
    }
    return 0;
}

uint32_t prGltfMaterialCount(const PrGltf* gltf) {
    return gltf->materials.count;
}

const PrGltfMaterial* PR_NULLABLE prGltfMaterials(const PrGltf* gltf) {
    return gltf->materials.data;
}

PrAffine2 prGltfTextureTransformAffine(const PrGltfTextureTransform* transform) {
    float cosine = prF32Cos(transform->rotation);
    float sine = prF32Sin(transform->rotation);
    return PrAffine2{
        .a = cosine * transform->scale[0],
        .b = -sine * transform->scale[0],
        .c = sine * transform->scale[1],
        .d = cosine * transform->scale[1],
        .e = transform->offset[0],
        .f = transform->offset[1],
    };
}

const PrGltfTextureInfo* prGltfMaterialTextureInfo(const PrGltfMaterial* material, PrGltfMaterialTexture texture) {
    PR_ASSERT(texture < PrGltfMaterialTexture_Count);
    return gltfMaterialTexture(const_cast<PrGltfMaterial*>(material), texture);
}

PrGltfMaterialExtensionFlags prGltfMaterialTextureExtension(PrGltfMaterialTexture texture) {
    PR_ASSERT(texture < PrGltfMaterialTexture_Count);
    return kGltfMaterialTextures[texture].extension;
}

bool prGltfMaterialTextureIsSrgb(PrGltfMaterialTexture texture) {
    PR_ASSERT(texture < PrGltfMaterialTexture_Count);
    return kGltfMaterialTextures[texture].srgb;
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

uint32_t prGltfVariantCount(const PrGltf* gltf) {
    return gltf->variants.count;
}

PrStringView prGltfVariantName(const PrGltf* gltf, uint32_t variant) {
    PR_ASSERT(variant < gltf->variants.count);
    return gltf->variants.data[variant];
}

uint32_t prGltfVariantMappingCount(const PrGltf* gltf) {
    return gltf->variantMappings.count;
}

const PrGltfVariantMapping* PR_NULLABLE prGltfVariantMappings(const PrGltf* gltf) {
    return gltf->variantMappings.data;
}

PrGltfMaterialIndex prGltfPrimitiveMaterial(const PrGltf* gltf, PrGltfPrimitiveIndex primitive, uint32_t variant) {
    PR_ASSERT(primitive < gltf->primitives.count);
    PR_ASSERT(variant == kPrGltfNone || variant < gltf->variants.count);
    const PrGltfPrimitive& record = gltf->primitives.data[primitive];
    for (uint32_t i = 0; variant != kPrGltfNone && i < record.mappingCount; ++i) {
        const PrGltfVariantMapping& mapping = gltf->variantMappings.data[record.mappingFirst + i];
        const uint32_t* variants = gltf->listIndices.data + mapping.variants.first;
        for (uint32_t k = 0; k < mapping.variants.count; ++k) {
            if (variants[k] == variant)
                return mapping.material;
        }
    }
    return record.material;
}

uint32_t prGltfMeshCount(const PrGltf* gltf) {
    return gltf->meshes.count;
}

const PrGltfMesh* PR_NULLABLE prGltfMeshes(const PrGltf* gltf) {
    return gltf->meshes.data;
}

uint32_t prGltfPrimitiveCount(const PrGltf* gltf) {
    return gltf->primitives.count;
}

const PrGltfPrimitive* PR_NULLABLE prGltfPrimitives(const PrGltf* gltf) {
    return gltf->primitives.data;
}

uint32_t prGltfAttributeCount(const PrGltf* gltf) {
    return gltf->attributes.count;
}

const PrGltfAttribute* PR_NULLABLE prGltfAttributes(const PrGltf* gltf) {
    return gltf->attributes.data;
}

uint32_t prGltfMorphTargetCount(const PrGltf* gltf) {
    return gltf->morphTargets.count;
}

const PrGltfMorphTarget* PR_NULLABLE prGltfMorphTargets(const PrGltf* gltf) {
    return gltf->morphTargets.data;
}

PrGltfAccessorIndex prGltfPrimitiveAttribute(
        const PrGltf* gltf, PrGltfPrimitiveIndex primitive, PrGltfSemantic semantic, uint32_t set) {
    PR_ASSERT(primitive < gltf->primitives.count);
    const PrGltfPrimitive& record = gltf->primitives.data[primitive];
    for (uint32_t i = 0; i < record.attributeCount; ++i) {
        const PrGltfAttribute& attribute = gltf->attributes.data[record.attributeFirst + i];
        if (attribute.semantic == semantic && attribute.set == set)
            return attribute.accessor;
    }
    return kPrGltfNone;
}

uint32_t prGltfPrimitiveTriangles(
        const PrGltf* gltf, PrGltfPrimitiveIndex primitive, uint32_t* PR_NULLABLE out, uint32_t capacity) {
    PR_ASSERT(primitive < gltf->primitives.count);
    const PrGltfPrimitive& record = gltf->primitives.data[primitive];
    IndexWindow window = {};
    window.gltf = gltf;
    uint32_t assembled = record.vertexCount;
    if (record.indices != kPrGltfNone) {
        window.indices = &gltf->accessors.data[record.indices];
        if (!gltfAccessorHasBytes(gltf, *window.indices))
            return 0;
        assembled = window.indices->count;
    }
    uint32_t total = triangleIndexCount(record.mode, assembled);
    uint32_t written = (total < capacity ? total : capacity) / 3;
    // Every triangle of a fan ends at the first vertex, which is read once so that the window only moves forward.
    uint32_t center = written != 0 && record.mode == PrGltfMode_TriangleFan ? vertexAt(&window, 0) : 0;
    for (uint32_t triangle = 0; triangle < written; ++triangle) {
        for (uint32_t corner = 0; corner < 3; ++corner) {
            uint32_t position = trianglePosition(record.mode, triangle, corner);
            bool isCenter = record.mode == PrGltfMode_TriangleFan && corner == 2;
            out[triangle * 3 + corner] = isCenter ? center : vertexAt(&window, position);
        }
    }
    return total;
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

uint32_t prGltfCameraCount(const PrGltf* gltf) {
    return gltf->cameras.count;
}

const PrGltfCamera* PR_NULLABLE prGltfCameras(const PrGltf* gltf) {
    return gltf->cameras.data;
}

uint32_t prGltfLightCount(const PrGltf* gltf) {
    return gltf->lights.count;
}

const PrGltfLight* PR_NULLABLE prGltfLights(const PrGltf* gltf) {
    return gltf->lights.data;
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes, skins and scenes

uint32_t prGltfNodeCount(const PrGltf* gltf) {
    return gltf->nodes.count;
}

const PrGltfNode* PR_NULLABLE prGltfNodes(const PrGltf* gltf) {
    return gltf->nodes.data;
}

uint32_t prGltfSkinCount(const PrGltf* gltf) {
    return gltf->skins.count;
}

const PrGltfSkin* PR_NULLABLE prGltfSkins(const PrGltf* gltf) {
    return gltf->skins.data;
}

uint32_t prGltfSceneCount(const PrGltf* gltf) {
    return gltf->scenes.count;
}

const PrGltfScene* PR_NULLABLE prGltfScenes(const PrGltf* gltf) {
    return gltf->scenes.data;
}

PrGltfSceneIndex prGltfDefaultScene(const PrGltf* gltf) {
    return gltf->scene;
}

PrMatrix4 prGltfMatrixFromTrs(const float* translation, const float* rotation, const float* scale) {
    float x = rotation[0];
    float y = rotation[1];
    float z = rotation[2];
    float w = rotation[3];
    float basis[3][3] = {
        {1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)},
        {2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)},
        {2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)},
    };
    PrMatrix4 matrix = prMatrix4Identity();
    for (uint32_t row = 0; row < 3; ++row) {
        for (uint32_t column = 0; column < 3; ++column)
            matrix.m[row][column] = basis[row][column] * scale[column];
        matrix.m[row][3] = translation[row];
    }
    return matrix;
}

PrMatrix4 prGltfNodeLocalMatrix(const PrGltfNode* node) {
    if (node->hasMatrix)
        return node->matrix;
    return prGltfMatrixFromTrs(node->translation, node->rotation, node->scale);
}

PrMatrix4 prGltfNodeWorldMatrix(const PrMatrix4* local, const PrMatrix4* parentWorld) {
    return prMatrix4Concat(local, parentWorld);
}

void prGltfWorldMatrices(const PrGltf* gltf, PrMatrix4* out) {
    // A parent is visited before its children, and the nodes form trees, so each node is visited once.
    RawArray<uint32_t> stack;
    for (uint32_t i = 0; i < gltf->nodes.count; ++i) {
        if (gltf->nodes.data[i].parent == kPrGltfNone)
            stack.push(i);
    }
    while (stack.count != 0) {
        uint32_t index = stack.last();
        stack.pop();
        const PrGltfNode& node = gltf->nodes.data[index];
        out[index] = prGltfNodeLocalMatrix(&node);
        if (node.parent != kPrGltfNone)
            out[index] = prMatrix4Concat(&out[index], &out[node.parent]);
        stack.append(gltf->listIndices.data + node.children.first, node.children.count);
    }
    stack.reset();
}

bool prGltfInstanceMatrices(const PrGltf* gltf, PrGltfNodeIndex node, uint32_t first, uint32_t count, PrMatrix4* out) {
    PR_ASSERT(node < gltf->nodes.count);
    const PrGltfInstancing& instancing = gltf->nodes.data[node].instancing;
    PR_ASSERT(first <= instancing.count && count <= instancing.count - first);
    // Each transform is read in batches of instances, and one that the asset does not give is the identity.
    constexpr uint32_t kBatch = 64;
    float translations[kBatch * 3];
    float rotations[kBatch * 4];
    float scales[kBatch * 3];
    bool read = true;
    for (uint32_t at = 0; at < count; at += kBatch) {
        uint32_t batch = count - at < kBatch ? count - at : kBatch;
        for (uint32_t i = 0; i < batch; ++i) {
            for (uint32_t c = 0; c < 3; ++c) {
                translations[i * 3 + c] = 0;
                scales[i * 3 + c] = 1;
                rotations[i * 4 + c] = 0;
            }
            rotations[i * 4 + 3] = 1;
        }
        if (instancing.translation != kPrGltfNone)
            read = gltfAccessorRead(gltf, gltf->accessors.data[instancing.translation], first + at, batch,
                GltfNumber_Float, translations) && read;
        if (instancing.rotation != kPrGltfNone)
            read = gltfAccessorRead(gltf, gltf->accessors.data[instancing.rotation], first + at, batch,
                GltfNumber_Float, rotations) && read;
        if (instancing.scale != kPrGltfNone)
            read = gltfAccessorRead(gltf, gltf->accessors.data[instancing.scale], first + at, batch, GltfNumber_Float,
                scales) && read;
        for (uint32_t i = 0; i < batch; ++i)
            out[at + i] = prGltfMatrixFromTrs(translations + i * 3, rotations + i * 4, scales + i * 3);
    }
    if (!read) {
        for (uint32_t i = 0; i < count; ++i)
            out[i] = prMatrix4Identity();
    }
    return read;
}

// ---------------------------------------------------------------------------------------------------------------------
// Animations

uint32_t prGltfAnimationCount(const PrGltf* gltf) {
    return gltf->animations.count;
}

const PrGltfAnimation* PR_NULLABLE prGltfAnimations(const PrGltf* gltf) {
    return gltf->animations.data;
}

uint32_t prGltfAnimationSamplerCount(const PrGltf* gltf) {
    return gltf->animationSamplers.count;
}

const PrGltfAnimationSampler* PR_NULLABLE prGltfAnimationSamplers(const PrGltf* gltf) {
    return gltf->animationSamplers.data;
}

uint32_t prGltfAnimationChannelCount(const PrGltf* gltf) {
    return gltf->animationChannels.count;
}

const PrGltfAnimationChannel* PR_NULLABLE prGltfAnimationChannels(const PrGltf* gltf) {
    return gltf->animationChannels.data;
}

bool prGltfAnimationChannelEvaluate(const PrGltf* gltf, PrGltfAnimationChannelIndex channel, float seconds, float* out) {
    PR_ASSERT(channel < gltf->animationChannels.count);
    return gltfAnimationChannelEvaluate(gltf, gltf->animationChannels.data[channel], seconds, out);
}
