// The glTF document as it is stored.

#pragma once

#include "pr/arena.h"
#include "pr/gltf.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A glTF document. Each array is the array of the asset's JSON with the same name, except those that the asset nests:
// the primitives of every mesh are one array, as are the attributes of every primitive and morph target, the morph
// targets of every primitive, and the samplers and channels of every animation.
struct Gltf {
    PrGltfAsset asset;
    PrGltfSceneIndex scene;

    RawArray<PrGltfBuffer> buffers;
    RawArray<PrGltfBufferView> bufferViews;
    RawArray<const uint8_t*> decodedViews;              // by buffer view: the bytes of a compressed view, or null
    RawArray<PrGltfAccessor> accessors;
    RawArray<const uint8_t*> decodedAccessors;          // by accessor: the elements that a Draco stream gave, or null
    RawArray<PrGltfImage> images;
    RawArray<PrGltfSampler> samplers;
    RawArray<PrGltfTexture> textures;
    RawArray<PrGltfMaterial> materials;
    RawArray<PrGltfMesh> meshes;
    RawArray<PrGltfPrimitive> primitives;
    RawArray<PrGltfAttribute> attributes;
    RawArray<PrGltfMorphTarget> morphTargets;
    RawArray<PrGltfCamera> cameras;
    RawArray<PrGltfLight> lights;
    RawArray<PrGltfNode> nodes;
    RawArray<PrGltfSkin> skins;
    RawArray<PrGltfScene> scenes;
    RawArray<PrGltfAnimation> animations;
    RawArray<PrGltfAnimationSampler> animationSamplers;
    RawArray<PrGltfAnimationChannel> animationChannels;
    RawArray<PrStringView> usedExtensions;
    RawArray<PrStringView> variants;                    // the name of each, of KHR_materials_variants
    RawArray<PrGltfVariantMapping> variantMappings;

    // The storage that every PrGltfList is a range of.
    RawArray<uint32_t> listIndices;
    RawArray<float> listFloats;

    // The blobs that the document's views read: the asset's own file, and what the host loaded.
    RawArray<PrBlob*> blobs;

    // The document's strings and the bytes that it decodes, which stay in place as long as the document.
    Arena storage;
};

// The alignment of the bytes that a document decodes, which is the size of the largest component type, so that an
// accessor's elements are aligned in them.
constexpr size_t kGltfDecodedAlignment = 4;

// What the specifications say of a texture of a material: the extension that gives it, or zero for the core, whether
// its colors are sRGB, and the JSON pointer to its texture info from the material, as an animation's pointer names it.
struct GltfMaterialTextureInfo {
    PrGltfMaterialExtensionFlags extension;
    bool srgb;
    PrStringView pointer;
};

// By PrGltfMaterialTexture.
inline constexpr GltfMaterialTextureInfo kGltfMaterialTextures[PrGltfMaterialTexture_Count] = {
    {0, true, "/pbrMetallicRoughness/baseColorTexture"},
    {0, false, "/pbrMetallicRoughness/metallicRoughnessTexture"},
    {0, false, "/normalTexture"},
    {0, false, "/occlusionTexture"},
    {0, true, "/emissiveTexture"},
    {PrGltfMaterialExtensionFlag_Clearcoat, false, "/extensions/KHR_materials_clearcoat/clearcoatTexture"},
    {PrGltfMaterialExtensionFlag_Clearcoat, false, "/extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture"},
    {PrGltfMaterialExtensionFlag_Clearcoat, false, "/extensions/KHR_materials_clearcoat/clearcoatNormalTexture"},
    {PrGltfMaterialExtensionFlag_Sheen, true, "/extensions/KHR_materials_sheen/sheenColorTexture"},
    {PrGltfMaterialExtensionFlag_Sheen, false, "/extensions/KHR_materials_sheen/sheenRoughnessTexture"},
    {PrGltfMaterialExtensionFlag_Transmission, false, "/extensions/KHR_materials_transmission/transmissionTexture"},
    {PrGltfMaterialExtensionFlag_Volume, false, "/extensions/KHR_materials_volume/thicknessTexture"},
    {PrGltfMaterialExtensionFlag_Specular, false, "/extensions/KHR_materials_specular/specularTexture"},
    {PrGltfMaterialExtensionFlag_Specular, true, "/extensions/KHR_materials_specular/specularColorTexture"},
    {PrGltfMaterialExtensionFlag_Iridescence, false, "/extensions/KHR_materials_iridescence/iridescenceTexture"},
    {
        PrGltfMaterialExtensionFlag_Iridescence, false,
        "/extensions/KHR_materials_iridescence/iridescenceThicknessTexture",
    },
    {PrGltfMaterialExtensionFlag_Anisotropy, false, "/extensions/KHR_materials_anisotropy/anisotropyTexture"},
    {
        PrGltfMaterialExtensionFlag_DiffuseTransmission, false,
        "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
    },
    {
        PrGltfMaterialExtensionFlag_DiffuseTransmission, true,
        "/extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
    },
    {
        PrGltfMaterialExtensionFlag_SpecularGlossiness, true,
        "/extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
    },
    {
        PrGltfMaterialExtensionFlag_SpecularGlossiness, true,
        "/extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",
    },
};

// Returns the material's use of one of its textures.
PrGltfTextureInfo* gltfMaterialTexture(PrGltfMaterial* material, PrGltfMaterialTexture texture);

// Returns `size` uninitialized bytes for decoded data, at kGltfDecodedAlignment, that live as long as the document.
inline uint8_t* PR_NONNULL gltfAllocDecoded(Gltf* gltf, size_t size) {
    return static_cast<uint8_t*>(gltf->storage.allocBytes(size, kGltfDecodedAlignment));
}

// Returns a copy of `text` that lives as long as the document. A null view stays null.
PrStringView gltfCopyString(Gltf* gltf, PrStringView text);

// Frees everything that the document holds, and releases its blobs.
void gltfReset(Gltf* gltf);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
