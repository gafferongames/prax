// The glTF document as it is stored.

#include "gltf/src/gltf_document.h"

#include "pr/mem.h"

namespace pr_impl {

PrGltfTextureInfo* gltfMaterialTexture(PrGltfMaterial* material, PrGltfMaterialTexture texture) {
    switch (texture) {
        case PrGltfMaterialTexture_BaseColor:
            return &material->baseColorTexture;
        case PrGltfMaterialTexture_MetallicRoughness:
            return &material->metallicRoughnessTexture;
        case PrGltfMaterialTexture_Normal:
            return &material->normalTexture;
        case PrGltfMaterialTexture_Occlusion:
            return &material->occlusionTexture;
        case PrGltfMaterialTexture_Emissive:
            return &material->emissiveTexture;
        case PrGltfMaterialTexture_Clearcoat:
            return &material->clearcoat.texture;
        case PrGltfMaterialTexture_ClearcoatRoughness:
            return &material->clearcoat.roughnessTexture;
        case PrGltfMaterialTexture_ClearcoatNormal:
            return &material->clearcoat.normalTexture;
        case PrGltfMaterialTexture_SheenColor:
            return &material->sheen.colorTexture;
        case PrGltfMaterialTexture_SheenRoughness:
            return &material->sheen.roughnessTexture;
        case PrGltfMaterialTexture_Transmission:
            return &material->transmission.texture;
        case PrGltfMaterialTexture_Thickness:
            return &material->volume.thicknessTexture;
        case PrGltfMaterialTexture_Specular:
            return &material->specular.texture;
        case PrGltfMaterialTexture_SpecularColor:
            return &material->specular.colorTexture;
        case PrGltfMaterialTexture_Iridescence:
            return &material->iridescence.texture;
        case PrGltfMaterialTexture_IridescenceThickness:
            return &material->iridescence.thicknessTexture;
        case PrGltfMaterialTexture_Anisotropy:
            return &material->anisotropy.texture;
        case PrGltfMaterialTexture_DiffuseTransmission:
            return &material->diffuseTransmission.texture;
        case PrGltfMaterialTexture_DiffuseTransmissionColor:
            return &material->diffuseTransmission.colorTexture;
        case PrGltfMaterialTexture_Diffuse:
            return &material->specularGlossiness.diffuseTexture;
        default:
            return &material->specularGlossiness.specularGlossinessTexture;
    }
}

PrStringView gltfCopyString(Gltf* gltf, PrStringView text) {
    if (text.data == nullptr)
        return text;
    // An empty string still needs storage, so that it does not read as the null view.
    char* copy = static_cast<char*>(gltf->storage.allocBytes(text.size, 1));
    prMemCopy(copy, text.data, text.size);
    return PrStringView{copy, text.size};
}

void gltfReset(Gltf* gltf) {
    gltf->buffers.reset();
    gltf->bufferViews.reset();
    gltf->decodedViews.reset();
    gltf->decodedAccessors.reset();
    gltf->accessors.reset();
    gltf->images.reset();
    gltf->samplers.reset();
    gltf->textures.reset();
    gltf->materials.reset();
    gltf->meshes.reset();
    gltf->primitives.reset();
    gltf->attributes.reset();
    gltf->morphTargets.reset();
    gltf->cameras.reset();
    gltf->lights.reset();
    gltf->nodes.reset();
    gltf->skins.reset();
    gltf->scenes.reset();
    gltf->animations.reset();
    gltf->animationSamplers.reset();
    gltf->animationChannels.reset();
    gltf->usedExtensions.reset();
    gltf->variants.reset();
    gltf->variantMappings.reset();
    gltf->listIndices.reset();
    gltf->listFloats.reset();
    for (uint32_t i = 0; i < gltf->blobs.count; ++i)
        prBlobRelease(gltf->blobs.data[i]);
    gltf->blobs.reset();
    gltf->storage.reset();
}

} // namespace pr_impl
