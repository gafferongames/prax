// glTF's model source: a glTF document described for a model, and the model format of glTF assets.

#include "pr/gltf_model.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/file.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/model_source_backend.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/text_buffer.h"

namespace pr_impl {

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// The input of each texture of a material, by PrGltfMaterialTexture.
constexpr PrStringView kTextureInputs[PrGltfMaterialTexture_Count] = {
    "pbrMetallicRoughness/baseColorTexture",
    "pbrMetallicRoughness/metallicRoughnessTexture",
    "normalTexture",
    "occlusionTexture",
    "emissiveTexture",
    "extensions/KHR_materials_clearcoat/clearcoatTexture",
    "extensions/KHR_materials_clearcoat/clearcoatRoughnessTexture",
    "extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
    "extensions/KHR_materials_sheen/sheenColorTexture",
    "extensions/KHR_materials_sheen/sheenRoughnessTexture",
    "extensions/KHR_materials_transmission/transmissionTexture",
    "extensions/KHR_materials_volume/thicknessTexture",
    "extensions/KHR_materials_specular/specularTexture",
    "extensions/KHR_materials_specular/specularColorTexture",
    "extensions/KHR_materials_iridescence/iridescenceTexture",
    "extensions/KHR_materials_iridescence/iridescenceThicknessTexture",
    "extensions/KHR_materials_anisotropy/anisotropyTexture",
    "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionTexture",
    "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorTexture",
    "extensions/KHR_materials_pbrSpecularGlossiness/diffuseTexture",
    "extensions/KHR_materials_pbrSpecularGlossiness/specularGlossinessTexture",
};

// A value of a material: its input's name, the extension that has it, or zero for the core, its field of the document's
// material, its width, and the path of a pointer that animates it.
struct ValueInput {
    PrStringView name;
    PrGltfMaterialExtensionFlags extension;
    size_t offset;
    uint32_t width;
    PrGltfPath path;
};

#define GLTF_FIELD(field) offsetof(PrGltfMaterial, field)

constexpr ValueInput kValueInputs[] = {
    {"pbrMetallicRoughness/baseColorFactor", 0, GLTF_FIELD(baseColorFactor), 4, PrGltfPath_MaterialBaseColorFactor},
    {"pbrMetallicRoughness/metallicFactor", 0, GLTF_FIELD(metallicFactor), 1, PrGltfPath_MaterialMetallicFactor},
    {"pbrMetallicRoughness/roughnessFactor", 0, GLTF_FIELD(roughnessFactor), 1, PrGltfPath_MaterialRoughnessFactor},
    {"normalTexture/scale", 0, GLTF_FIELD(normalScale), 1, PrGltfPath_MaterialNormalScale},
    {"occlusionTexture/strength", 0, GLTF_FIELD(occlusionStrength), 1, PrGltfPath_MaterialOcclusionStrength},
    {"emissiveFactor", 0, GLTF_FIELD(emissiveFactor), 3, PrGltfPath_MaterialEmissiveFactor},
    {
        "extensions/KHR_materials_emissive_strength/emissiveStrength", 0, GLTF_FIELD(emissiveStrength), 1,
        PrGltfPath_MaterialEmissiveStrength,
    },
    {"extensions/KHR_materials_ior/ior", PrGltfMaterialExtensionFlag_Ior, GLTF_FIELD(ior), 1, PrGltfPath_MaterialIor},
    {
        "extensions/KHR_materials_specular/specularFactor", PrGltfMaterialExtensionFlag_Specular,
        GLTF_FIELD(specular.factor), 1, PrGltfPath_MaterialSpecularFactor,
    },
    {
        "extensions/KHR_materials_specular/specularColorFactor", PrGltfMaterialExtensionFlag_Specular,
        GLTF_FIELD(specular.colorFactor), 3, PrGltfPath_MaterialSpecularColorFactor,
    },
    {
        "extensions/KHR_materials_clearcoat/clearcoatFactor", PrGltfMaterialExtensionFlag_Clearcoat,
        GLTF_FIELD(clearcoat.factor), 1, PrGltfPath_MaterialClearcoatFactor,
    },
    {
        "extensions/KHR_materials_clearcoat/clearcoatRoughnessFactor", PrGltfMaterialExtensionFlag_Clearcoat,
        GLTF_FIELD(clearcoat.roughnessFactor), 1, PrGltfPath_MaterialClearcoatRoughnessFactor,
    },
    {
        "extensions/KHR_materials_clearcoat/clearcoatNormalTexture/scale", PrGltfMaterialExtensionFlag_Clearcoat,
        GLTF_FIELD(clearcoat.normalScale), 1, PrGltfPath_MaterialClearcoatNormalScale,
    },
    {
        "extensions/KHR_materials_sheen/sheenColorFactor", PrGltfMaterialExtensionFlag_Sheen,
        GLTF_FIELD(sheen.colorFactor), 3, PrGltfPath_MaterialSheenColorFactor,
    },
    {
        "extensions/KHR_materials_sheen/sheenRoughnessFactor", PrGltfMaterialExtensionFlag_Sheen,
        GLTF_FIELD(sheen.roughnessFactor), 1, PrGltfPath_MaterialSheenRoughnessFactor,
    },
    {
        "extensions/KHR_materials_iridescence/iridescenceFactor", PrGltfMaterialExtensionFlag_Iridescence,
        GLTF_FIELD(iridescence.factor), 1, PrGltfPath_MaterialIridescenceFactor,
    },
    {
        "extensions/KHR_materials_iridescence/iridescenceIor", PrGltfMaterialExtensionFlag_Iridescence,
        GLTF_FIELD(iridescence.ior), 1, PrGltfPath_MaterialIridescenceIor,
    },
    {
        "extensions/KHR_materials_iridescence/iridescenceThicknessMinimum", PrGltfMaterialExtensionFlag_Iridescence,
        GLTF_FIELD(iridescence.thicknessMinimum), 1, PrGltfPath_MaterialIridescenceThicknessMinimum,
    },
    {
        "extensions/KHR_materials_iridescence/iridescenceThicknessMaximum", PrGltfMaterialExtensionFlag_Iridescence,
        GLTF_FIELD(iridescence.thicknessMaximum), 1, PrGltfPath_MaterialIridescenceThicknessMaximum,
    },
    {
        "extensions/KHR_materials_anisotropy/anisotropyStrength", PrGltfMaterialExtensionFlag_Anisotropy,
        GLTF_FIELD(anisotropy.strength), 1, PrGltfPath_MaterialAnisotropyStrength,
    },
    {
        "extensions/KHR_materials_anisotropy/anisotropyRotation", PrGltfMaterialExtensionFlag_Anisotropy,
        GLTF_FIELD(anisotropy.rotation), 1, PrGltfPath_MaterialAnisotropyRotation,
    },
    {
        "extensions/KHR_materials_transmission/transmissionFactor", PrGltfMaterialExtensionFlag_Transmission,
        GLTF_FIELD(transmission.factor), 1, PrGltfPath_MaterialTransmissionFactor,
    },
    {
        "extensions/KHR_materials_volume/thicknessFactor", PrGltfMaterialExtensionFlag_Volume,
        GLTF_FIELD(volume.thicknessFactor), 1, PrGltfPath_MaterialThicknessFactor,
    },
    {
        "extensions/KHR_materials_volume/attenuationDistance", PrGltfMaterialExtensionFlag_Volume,
        GLTF_FIELD(volume.attenuationDistance), 1, PrGltfPath_MaterialAttenuationDistance,
    },
    {
        "extensions/KHR_materials_volume/attenuationColor", PrGltfMaterialExtensionFlag_Volume,
        GLTF_FIELD(volume.attenuationColor), 3, PrGltfPath_MaterialAttenuationColor,
    },
    {
        "extensions/KHR_materials_dispersion/dispersion", PrGltfMaterialExtensionFlag_Dispersion,
        GLTF_FIELD(dispersion), 1,
        PrGltfPath_MaterialDispersion,
    },
    {
        "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionFactor",
        PrGltfMaterialExtensionFlag_DiffuseTransmission, GLTF_FIELD(diffuseTransmission.factor), 1,
        PrGltfPath_MaterialDiffuseTransmissionFactor,
    },
    {
        "extensions/KHR_materials_diffuse_transmission/diffuseTransmissionColorFactor",
        PrGltfMaterialExtensionFlag_DiffuseTransmission, GLTF_FIELD(diffuseTransmission.colorFactor), 3,
        PrGltfPath_MaterialDiffuseTransmissionColorFactor,
    },
    {
        "extensions/KHR_materials_pbrSpecularGlossiness/diffuseFactor", PrGltfMaterialExtensionFlag_SpecularGlossiness,
        GLTF_FIELD(specularGlossiness.diffuseFactor), 4, PrGltfPath_None,
    },
    {
        "extensions/KHR_materials_pbrSpecularGlossiness/specularFactor", PrGltfMaterialExtensionFlag_SpecularGlossiness,
        GLTF_FIELD(specularGlossiness.specularFactor), 3, PrGltfPath_None,
    },
    {
        "extensions/KHR_materials_pbrSpecularGlossiness/glossinessFactor",
        PrGltfMaterialExtensionFlag_SpecularGlossiness,
        GLTF_FIELD(specularGlossiness.glossinessFactor), 1, PrGltfPath_None,
    },
};

#undef GLTF_FIELD

// The inputs of one material while the source is made, which animations may add to.
struct MaterialInputs {
    RawArray<PrModelSourceInput> inputs;
};

// ---------------------------------------------------------------------------------------------------------------------
// The source

// A glTF document's source: the document, which it owns if a format opened it, and the description.
struct GltfModelSource : PrModelSource {
    const PrGltf* gltf;
    PrGltf* PR_NULLABLE owned;
    PrModelSourceInfo info;
    RawArray<PrModelSourceNode> nodes;
    RawArray<PrModelNodeIndex> roots;
    RawArray<PrModelNodeIndex> children;
    RawArray<PrModelSourceDraw> draws;
    RawArray<PrMatrix4> instances;
    RawArray<PrModelSourceMesh> meshes;
    RawArray<PrModelSourcePart> parts;
    RawArray<PrGltfPrimitiveIndex> partPrimitives;     // by part
    RawArray<PrModelSourceStream> streams;
    RawArray<PrBox3> morphBoxes;
    RawArray<float> weights;
    RawArray<PrModelSourceSkin> skins;
    RawArray<PrModelNodeIndex> joints;
    RawArray<PrMatrix4> inverseBinds;
    RawArray<PrModelSourceMaterial> materials;
    RawArray<PrModelSourceInput> inputs;
    RawArray<PrModelSourceTexture> textures;
    RawArray<PrGltfTextureIndex> textureSources;       // by texture of the source: the document's texture
    RawArray<PrModelImageIndex> textureImages;
    RawArray<PrModelSourceImage> images;
    RawArray<PrStringView> variants;
    RawArray<PrModelSourceVariantMaterial> variantMaterials;
    RawArray<PrModelSourceCamera> cameras;
    RawArray<PrModelSourceLight> lights;
    RawArray<PrModelSourceAnimation> animations;
    RawArray<PrModelSourceTrack> tracks;
    RawArray<float> keyTimes;
    RawArray<float> keyValues;
    RawArray<PrModelDiagnostic> diagnostics;
};

void addDiagnostic(GltfModelSource* s, PrStringView feature, PrStringView detail) {
    for (uint32_t i = 0; i < s->diagnostics.count; ++i) {
        if (s->diagnostics.data[i].feature == feature)
            return;
    }
    s->diagnostics.push(PrModelDiagnostic{feature, detail});
}

PrModelWrap wrapOf(PrGltfWrap wrap) {
    switch (wrap) {
        case PrGltfWrap_ClampToEdge:
            return PrModelWrap_Clamp;
        case PrGltfWrap_MirroredRepeat:
            return PrModelWrap_Mirror;
        default:
            return PrModelWrap_Repeat;
    }
}

// Returns a sampler of the document as a model takes it (glTF-2.0/#samplers). A filter that the document leaves to the
// implementation, and trilinear filtering, are the model's default.
PrModelSampler samplerOf(const PrGltf* gltf, PrGltfSamplerIndex index) {
    PrModelSampler out{};
    if (index == kPrGltfNone)
        return out;
    const PrGltfSampler& sampler = prGltfSamplers(gltf)[index];
    out.wrap[0] = wrapOf(sampler.wrapS);
    out.wrap[1] = wrapOf(sampler.wrapT);
    if (sampler.magFilter == PrGltfFilter_Nearest)
        out.magnify = PrModelFilter_Nearest;
    else if (sampler.magFilter == PrGltfFilter_Linear)
        out.magnify = PrModelFilter_Linear;
    switch (sampler.minFilter) {
        case PrGltfFilter_Nearest:
        case PrGltfFilter_Linear:
            // These filters read the first level alone.
            out.minify = sampler.minFilter == PrGltfFilter_Nearest ? PrModelFilter_Nearest : PrModelFilter_Linear;
            out.mip = PrModelFilter_None;
            break;
        case PrGltfFilter_NearestMipmapNearest:
        case PrGltfFilter_NearestMipmapLinear:
            out.minify = PrModelFilter_Nearest;
            out.mip =
                sampler.minFilter == PrGltfFilter_NearestMipmapNearest ? PrModelFilter_Nearest : PrModelFilter_Linear;
            break;
        case PrGltfFilter_LinearMipmapNearest:
            out.minify = PrModelFilter_Linear;
            out.mip = PrModelFilter_Nearest;
            break;
        default:
            break;
    }
    return out;
}

// Returns the source's texture of a document's texture read as color if `srgb`, which it adds if the source has none.
PrModelTextureIndex internTexture(GltfModelSource* s, PrGltfTextureIndex index, bool srgb) {
    PrModelTextureColor color = srgb ? PrModelTextureColor_Srgb : PrModelTextureColor_Data;
    for (uint32_t i = 0; i < s->textures.count; ++i) {
        if (s->textureSources.data[i] == index && s->textures.data[i].color == color)
            return i;
    }
    const PrGltfTexture& texture = prGltfTextures(s->gltf)[index];
    // The images that may show the texture, in the document's order of preference.
    uint32_t first = s->textureImages.count;
    s->textureImages.append(prGltfListIndices(s->gltf, texture.sources), texture.sources.count);
    s->textures.push(PrModelSourceTexture{
        .firstImage = first,
        .imageCount = texture.sources.count,
        .sampler = samplerOf(s->gltf, texture.sampler),
        .color = color,
    });
    s->textureSources.push(index);
    return s->textures.count - 1;
}

// Returns the input of a texture of a material.
PrModelSourceInput textureInput(GltfModelSource* s, PrGltfMaterialTexture slot, const PrGltfTextureInfo& info) {
    const float white[4] = {1, 1, 1, 1};
    PrModelSourceInput input = prModelSourceInputValue(kTextureInputs[slot], 4, white);
    input.kind = PrModelInputKind_Texture;
    input.texture = internTexture(s, info.texture, prGltfMaterialTextureIsSrgb(slot));
    input.texCoordSet = uint8_t(info.texCoord < 255 ? info.texCoord : 255);
    input.offset[0] = info.transform.offset[0];
    input.offset[1] = info.transform.offset[1];
    input.rotation = info.transform.rotation;
    input.scale[0] = info.transform.scale[0];
    input.scale[1] = info.transform.scale[1];
    return input;
}

// Reads a material's inputs: its core's, and those of each extension that it has.
void readMaterial(GltfModelSource* s, const PrGltfMaterial& material, MaterialInputs* out) {
    bool unlit = (material.extensions & PrGltfMaterialExtensionFlag_Unlit) != 0;
    const uint8_t* fields = reinterpret_cast<const uint8_t*>(&material);
    for (const ValueInput& value : kValueInputs) {
        bool core = value.name == "pbrMetallicRoughness/baseColorFactor";
        if (unlit ? !core : (value.extension != 0 && (material.extensions & value.extension) == 0))
            continue;
        out->inputs.push(prModelSourceInputValue(value.name, value.width,
            reinterpret_cast<const float*>(fields + value.offset)));
    }
    for (uint32_t slot = 0; slot < PrGltfMaterialTexture_Count; ++slot) {
        const PrGltfTextureInfo& info = *prGltfMaterialTextureInfo(&material, PrGltfMaterialTexture(slot));
        PrGltfMaterialExtensionFlags extension = prGltfMaterialTextureExtension(PrGltfMaterialTexture(slot));
        if (info.texture == kPrGltfNone || (unlit && slot != PrGltfMaterialTexture_BaseColor) ||
                (extension != 0 && (material.extensions & extension) == 0))
            continue;
        out->inputs.push(textureInput(s, PrGltfMaterialTexture(slot), info));
    }
}

// Returns the place among a material's inputs of the input `name`, adding it with the document's value of it if the
// material lacks it, as a pointer to an extension's property gives the material the extension.
uint32_t inputOf(MaterialInputs* material, const PrGltfMaterial& document, const ValueInput& value) {
    for (uint32_t i = 0; i < material->inputs.count; ++i) {
        if (material->inputs.data[i].name == value.name)
            return i;
    }
    const uint8_t* fields = reinterpret_cast<const uint8_t*>(&document);
    material->inputs.push(prModelSourceInputValue(value.name, value.width,
        reinterpret_cast<const float*>(fields + value.offset)));
    return material->inputs.count - 1;
}

// Returns the place among a material's inputs of its texture `slot`, or UINT32_MAX if it has none.
uint32_t textureInputOf(const MaterialInputs& material, uint32_t slot) {
    if (slot >= PrGltfMaterialTexture_Count)
        return 0xffffffffu;
    for (uint32_t i = 0; i < material.inputs.count; ++i) {
        if (material.inputs.data[i].name == kTextureInputs[slot])
            return i;
    }
    return 0xffffffffu;
}

// ---------------------------------------------------------------------------------------------------------------------
// The description

void readNodes(GltfModelSource* s, PrGltfSceneIndex scene) {
    const PrGltf* gltf = s->gltf;
    const PrGltfNode* nodes = prGltfNodes(gltf);
    for (uint32_t i = 0; i < prGltfNodeCount(gltf); ++i) {
        const PrGltfNode& node = nodes[i];
        PrModelSourceNode out{
            .name = node.name,
            .parent = node.parent,
            .flags = PrModelNodeFlags((node.hasMatrix ? 0 : PrModelNodeFlag_Trs) |
                (node.visible ? 0 : PrModelNodeFlag_Hidden)),
            .firstChild = s->children.count,
            .childCount = node.children.count,
            .matrix = node.hasMatrix ? node.matrix : prMatrix4Identity(),
            .camera = node.camera,
            .light = node.light,
            .firstWeight = s->weights.count,
            .weightCount = node.weights.count,
        };
        prMemCopy(out.translation, node.translation, sizeof(out.translation));
        prMemCopy(out.rotation, node.rotation, sizeof(out.rotation));
        prMemCopy(out.scale, node.scale, sizeof(out.scale));
        s->children.append(prGltfListIndices(gltf, node.children), node.children.count);
        s->weights.append(prGltfListFloats(gltf, node.weights), node.weights.count);
        s->nodes.push(out);
        if (node.mesh == kPrGltfNone)
            continue;
        PrModelSourceDraw draw{
            .node = i,
            .mesh = node.mesh,
            .skin = node.skin,
            .firstInstance = s->instances.count,
            .instanceCount = node.instancing.count,
        };
        // An instance whose buffer has no bytes stays at the node.
        uint32_t first = s->instances.count;
        for (uint32_t k = 0; k < node.instancing.count; ++k)
            s->instances.push(prMatrix4Identity());
        if (node.instancing.count != 0)
            prGltfInstanceMatrices(gltf, i, 0, node.instancing.count, s->instances.data + first);
        s->draws.push(draw);
    }
    if (scene == kPrGltfNone)
        scene = prGltfDefaultScene(gltf);
    if (scene == kPrGltfNone && prGltfSceneCount(gltf) != 0)
        scene = 0;
    if (scene != kPrGltfNone) {
        PrGltfList roots = prGltfScenes(gltf)[scene].nodes;
        s->roots.append(prGltfListIndices(gltf, roots), roots.count);
    } else {
        for (uint32_t i = 0; i < prGltfNodeCount(gltf); ++i) {
            if (nodes[i].parent == kPrGltfNone)
                s->roots.push(i);
        }
    }
}

// Reads the document's meshes: the primitives that are parts, their bounds and morph targets, their streams, and their
// variants' materials.
void readMeshes(GltfModelSource* s) {
    const PrGltf* gltf = s->gltf;
    const PrGltfMesh* meshes = prGltfMeshes(gltf);
    const PrGltfPrimitive* primitives = prGltfPrimitives(gltf);
    const PrGltfAttribute* attributes = prGltfAttributes(gltf);
    const PrGltfAccessor* accessors = prGltfAccessors(gltf);
    for (uint32_t i = 0; i < prGltfMeshCount(gltf); ++i) {
        PrModelSourceMesh mesh{
            .name = meshes[i].name,
            .firstPart = s->parts.count,
            .firstWeight = s->weights.count,
            .weightCount = meshes[i].weights.count,
        };
        s->weights.append(prGltfListFloats(gltf, meshes[i].weights), meshes[i].weights.count);
        for (uint32_t k = 0; k < meshes[i].primitiveCount; ++k) {
            PrGltfPrimitiveIndex index = meshes[i].primitiveFirst + k;
            const PrGltfPrimitive& p = primitives[index];
            if (p.mode == PrGltfMode_Points)
                addDiagnostic(s, "points", "a mesh has a primitive of points, which is not drawn");
            else if (p.mode < PrGltfMode_Triangles)
                addDiagnostic(s, "lines", "a mesh has a primitive of lines, which is not drawn");
            PrGltfAccessorIndex positions = prGltfPrimitiveAttribute(gltf, index, PrGltfSemantic_Position, 0);
            if (p.mode < PrGltfMode_Triangles || positions == kPrGltfNone)
                continue;
            PrModelSourcePart part{
                .material = p.material,
                .firstMorphBox = s->morphBoxes.count,
                .morphTargetCount = p.morphTargetCount,
                .firstStream = s->streams.count,
                .firstVariant = s->variantMaterials.count,
            };
            // A document states the bounds of every accessor of positions (glTF-2.0/#accessor-bounds), which are those
            // of the integers of quantized positions.
            float low[3] = {};
            float high[3] = {};
            prGltfAccessorBounds(gltf, positions, low, high);
            part.bounds = PrBox3{{low[0], low[1], low[2]}, {high[0], high[1], high[2]}};
            // A target without stated bounds counts as moving nothing.
            const PrGltfMorphTarget* targets = prGltfMorphTargets(gltf);
            for (uint32_t t = 0; t < p.morphTargetCount; ++t) {
                PrBox3 box{};
                const PrGltfMorphTarget& target = targets[p.morphTargetFirst + t];
                for (uint32_t a = 0; a < target.attributeCount; ++a) {
                    const PrGltfAttribute& attribute = attributes[target.attributeFirst + a];
                    if (attribute.semantic == PrGltfSemantic_Position &&
                            prGltfAccessorBounds(gltf, attribute.accessor, low, high))
                        box = PrBox3{{low[0], low[1], low[2]}, {high[0], high[1], high[2]}};
                }
                s->morphBoxes.push(box);
            }
            for (uint32_t a = 0; a < p.attributeCount; ++a) {
                const PrGltfAttribute& attribute = attributes[p.attributeFirst + a];
                PrModelSemantic semantic = PrModelSemantic_Custom;
                if (attribute.semantic == PrGltfSemantic_TexCoord)
                    semantic = PrModelSemantic_TexCoord;
                else if (attribute.semantic == PrGltfSemantic_Color)
                    semantic = PrModelSemantic_Color;
                else if (attribute.name.size == 0 || attribute.name.data[0] != '_')
                    continue;
                const PrGltfAccessor& accessor = accessors[attribute.accessor];
                s->streams.push(PrModelSourceStream{
                    .name = attribute.name,
                    .semantic = semantic,
                    .set = uint8_t(attribute.set < 255 ? attribute.set : 255),
                    .format = prMeshFormatFloat32(prGltfElementTypeComponentCount(accessor.type)),
                });
            }
            part.streamCount = s->streams.count - part.firstStream;
            for (uint32_t v = 0; v < prGltfVariantCount(gltf); ++v) {
                PrGltfMaterialIndex material = prGltfPrimitiveMaterial(gltf, index, v);
                if (material != p.material)
                    s->variantMaterials.push(PrModelSourceVariantMaterial{v, material});
            }
            part.variantCount = s->variantMaterials.count - part.firstVariant;
            if (p.morphTargetCount > mesh.morphTargetCount)
                mesh.morphTargetCount = p.morphTargetCount;
            s->parts.push(part);
            s->partPrimitives.push(index);
        }
        mesh.partCount = s->parts.count - mesh.firstPart;
        s->meshes.push(mesh);
    }
}

// Reads the inverse bind matrix of each joint of each skin. A skin without them, or whose buffer has no bytes, has the
// identity for each.
void readSkins(GltfModelSource* s) {
    const PrGltf* gltf = s->gltf;
    const PrGltfSkin* skins = prGltfSkins(gltf);
    RawArray<float> columns;
    for (uint32_t i = 0; i < prGltfSkinCount(gltf); ++i) {
        uint32_t count = skins[i].joints.count;
        uint32_t first = s->joints.count;
        s->skins.push(PrModelSourceSkin{first, count});
        s->joints.append(prGltfListIndices(gltf, skins[i].joints), count);
        for (uint32_t j = 0; j < count; ++j)
            s->inverseBinds.push(prMatrix4Identity());
        if (skins[i].inverseBindMatrices == kPrGltfNone)
            continue;
        // An accessor holds a matrix by columns.
        columns.resize(count * 16);
        if (prGltfAccessorReadFloats(gltf, skins[i].inverseBindMatrices, 0, count, columns.data)) {
            for (uint32_t j = 0; j < count; ++j) {
                for (uint32_t k = 0; k < 16; ++k)
                    s->inverseBinds.data[first + j].m[k % 4][k / 4] = columns.data[j * 16 + k];
            }
        }
    }
    columns.reset();
}

void readCamerasAndLights(GltfModelSource* s) {
    const PrGltf* gltf = s->gltf;
    for (uint32_t i = 0; i < prGltfCameraCount(gltf); ++i) {
        const PrGltfCamera& camera = prGltfCameras(gltf)[i];
        PrModelProjection projection = PrModelProjection_Orthographic;
        if (camera.type == PrGltfCameraType_Perspective)
            projection = PrModelProjection_Perspective;
        s->cameras.push(PrModelSourceCamera{
            .name = camera.name,
            .projection = projection,
            .yfov = camera.yfov,
            .aspectRatio = camera.aspectRatio,
            .xmag = camera.xmag,
            .ymag = camera.ymag,
            .znear = camera.znear,
            .zfar = camera.zfar,
        });
    }
    for (uint32_t i = 0; i < prGltfLightCount(gltf); ++i) {
        const PrGltfLight& light = prGltfLights(gltf)[i];
        PrModelLightKind kind = PrModelLightKind_Directional;
        if (light.type == PrGltfLightType_Point)
            kind = PrModelLightKind_Point;
        else if (light.type == PrGltfLightType_Spot)
            kind = PrModelLightKind_Spot;
        s->lights.push(PrModelSourceLight{
            .name = light.name,
            .kind = kind,
            .unit = PrModelLightUnit_Photometric,
            .color = {light.color[0], light.color[1], light.color[2]},
            .intensity = light.intensity,
            .range = light.range,
            .innerConeAngle = light.innerConeAngle,
            .outerConeAngle = light.outerConeAngle,
        });
    }
}

// Returns the target of a channel's path, or false for a path that the source does not animate.
bool targetOf(PrGltfPath path, PrModelTrackTarget* out) {
    switch (path) {
        case PrGltfPath_Translation:
            *out = PrModelTrackTarget_Translation;
            return true;
        case PrGltfPath_Rotation:
            *out = PrModelTrackTarget_Rotation;
            return true;
        case PrGltfPath_Scale:
            *out = PrModelTrackTarget_Scale;
            return true;
        case PrGltfPath_Weights:
            *out = PrModelTrackTarget_Weights;
            return true;
        case PrGltfPath_NodeWeight:
            *out = PrModelTrackTarget_Weight;
            return true;
        case PrGltfPath_NodeVisible:
            *out = PrModelTrackTarget_Visible;
            return true;
        case PrGltfPath_MeshWeights:
            *out = PrModelTrackTarget_MeshWeights;
            return true;
        case PrGltfPath_MeshWeight:
            *out = PrModelTrackTarget_MeshWeight;
            return true;
        case PrGltfPath_CameraAspectRatio:
            *out = PrModelTrackTarget_CameraAspectRatio;
            return true;
        case PrGltfPath_CameraYfov:
            *out = PrModelTrackTarget_CameraYfov;
            return true;
        case PrGltfPath_CameraXmag:
            *out = PrModelTrackTarget_CameraXmag;
            return true;
        case PrGltfPath_CameraYmag:
            *out = PrModelTrackTarget_CameraYmag;
            return true;
        case PrGltfPath_CameraZnear:
            *out = PrModelTrackTarget_CameraZnear;
            return true;
        case PrGltfPath_CameraZfar:
            *out = PrModelTrackTarget_CameraZfar;
            return true;
        case PrGltfPath_LightColor:
            *out = PrModelTrackTarget_LightColor;
            return true;
        case PrGltfPath_LightIntensity:
            *out = PrModelTrackTarget_LightIntensity;
            return true;
        case PrGltfPath_LightRange:
            *out = PrModelTrackTarget_LightRange;
            return true;
        case PrGltfPath_LightInnerConeAngle:
            *out = PrModelTrackTarget_LightInnerConeAngle;
            return true;
        case PrGltfPath_LightOuterConeAngle:
            *out = PrModelTrackTarget_LightOuterConeAngle;
            return true;
        case PrGltfPath_TextureOffset:
            *out = PrModelTrackTarget_TextureOffset;
            return true;
        case PrGltfPath_TextureRotation:
            *out = PrModelTrackTarget_TextureRotation;
            return true;
        case PrGltfPath_TextureScale:
            *out = PrModelTrackTarget_TextureScale;
            return true;
        default:
            if (path >= PrGltfPath_MaterialBaseColorFactor && path <= PrGltfPath_MaterialDiffuseTransmissionColorFactor) {
                *out = PrModelTrackTarget_Input;
                return true;
            }
            return false;
    }
}

// Reads each animation's channels as tracks, with their keys. A channel whose buffers have no bytes animates nothing.
void readAnimations(GltfModelSource* s, MaterialInputs* materials) {
    const PrGltf* gltf = s->gltf;
    const PrGltfAnimationChannel* channels = prGltfAnimationChannels(gltf);
    const PrGltfAnimationSampler* samplers = prGltfAnimationSamplers(gltf);
    const PrGltfAccessor* accessors = prGltfAccessors(gltf);
    const PrGltfMaterial* documentMaterials = prGltfMaterials(gltf);
    for (uint32_t a = 0; a < prGltfAnimationCount(gltf); ++a) {
        const PrGltfAnimation& animation = prGltfAnimations(gltf)[a];
        PrModelSourceAnimation out{
            .name = animation.name,
            .duration = animation.duration,
            .firstTrack = s->tracks.count,
        };
        for (uint32_t c = 0; c < animation.channelCount; ++c) {
            const PrGltfAnimationChannel& channel = channels[animation.channelFirst + c];
            PrModelTrackTarget target = PrModelTrackTarget_Translation;
            if (channel.path == PrGltfPath_None || channel.width == 0 || !targetOf(channel.path, &target))
                continue;
            PrModelSourceTrack track{
                .target = target,
                .object = channel.node != kPrGltfNone ? channel.node : channel.object,
                .element = channel.element,
                .width = channel.width,
            };
            if (channel.path == PrGltfPath_MeshWeights || channel.path == PrGltfPath_MeshWeight ||
                    target >= PrModelTrackTarget_LightColor)
                track.object = channel.object;
            if (target == PrModelTrackTarget_Input) {
                if (channel.object >= prGltfMaterialCount(gltf))
                    continue;
                const ValueInput* value = nullptr;
                for (const ValueInput& known : kValueInputs) {
                    if (known.path == channel.path)
                        value = &known;
                }
                if (value == nullptr)
                    continue;
                track.element = inputOf(&materials[channel.object], documentMaterials[channel.object], *value);
            } else if (target >= PrModelTrackTarget_TextureOffset) {
                if (channel.object >= prGltfMaterialCount(gltf))
                    continue;
                track.element = textureInputOf(materials[channel.object], channel.element);
                if (track.element == 0xffffffffu)
                    continue;
            }
            const PrGltfAnimationSampler& sampler = samplers[channel.sampler];
            if (sampler.interpolation == PrGltfInterpolation_Step)
                track.interpolation = PrModelInterpolation_Step;
            else if (sampler.interpolation == PrGltfInterpolation_CubicSpline)
                track.interpolation = PrModelInterpolation_CubicSpline;
            else
                track.interpolation = PrModelInterpolation_Linear;
            const PrGltfAccessor& input = accessors[sampler.input];
            const PrGltfAccessor& output = accessors[sampler.output];
            uint32_t keys = input.count;
            uint32_t values = output.count * prGltfElementTypeComponentCount(output.type);
            uint32_t needed = keys * channel.width * (track.interpolation == PrModelInterpolation_CubicSpline ? 3 : 1);
            if (keys == 0 || values < needed)
                continue;
            track.firstKey = s->keyTimes.count;
            track.keyCount = keys;
            track.firstValue = s->keyValues.count;
            s->keyTimes.resize(track.firstKey + keys);
            s->keyValues.resize(track.firstValue + values);
            bool read = prGltfAccessorReadFloats(gltf, sampler.input, 0, keys, s->keyTimes.data + track.firstKey) &&
                prGltfAccessorReadFloats(gltf, sampler.output, 0, output.count, s->keyValues.data + track.firstValue);
            if (!read) {
                s->keyTimes.resize(track.firstKey);
                s->keyValues.resize(track.firstValue);
                continue;
            }
            s->tracks.push(track);
        }
        out.trackCount = s->tracks.count - out.firstTrack;
        s->animations.push(out);
    }
}

// Reads the description of the document into the source.
void describe(GltfModelSource* s, PrGltfSceneIndex scene) {
    const PrGltf* gltf = s->gltf;
    readNodes(s, scene);
    readMeshes(s);
    readSkins(s);
    readCamerasAndLights(s);
    for (uint32_t i = 0; i < prGltfImageCount(gltf); ++i) {
        const PrGltfImage& image = prGltfImages(gltf)[i];
        s->images.push(PrModelSourceImage{.name = image.name, .mediaType = image.mimeType, .location = image.uri});
    }
    for (uint32_t i = 0; i < prGltfVariantCount(gltf); ++i)
        s->variants.push(prGltfVariantName(gltf, i));
    uint32_t materialCount = prGltfMaterialCount(gltf);
    MaterialInputs* materials = static_cast<MaterialInputs*>(prMemoryAllocZeroed(sizeof(MaterialInputs) *
        (materialCount != 0 ? materialCount : 1)));
    const PrGltfMaterial* documentMaterials = prGltfMaterials(gltf);
    for (uint32_t i = 0; i < materialCount; ++i)
        readMaterial(s, documentMaterials[i], &materials[i]);
    readAnimations(s, materials);
    for (uint32_t i = 0; i < materialCount; ++i) {
        const PrGltfMaterial& material = documentMaterials[i];
        PrModelAlpha alpha = PrModelAlpha_Opaque;
        if (material.alphaMode == PrGltfAlphaMode_Mask)
            alpha = PrModelAlpha_Mask;
        else if (material.alphaMode == PrGltfAlphaMode_Blend)
            alpha = PrModelAlpha_Blend;
        bool unlit = (material.extensions & PrGltfMaterialExtensionFlag_Unlit) != 0;
        s->materials.push(PrModelSourceMaterial{
            .name = material.name,
            .shading = PrModelShading(unlit ? PrModelShading_Unlit : PrModelShading_KhronosPbr),
            .alpha = alpha,
            .alphaCutoff = material.alphaCutoff,
            .doubleSided = material.doubleSided,
            .firstInput = s->inputs.count,
            .inputCount = materials[i].inputs.count,
        });
        s->inputs.append(materials[i].inputs.span());
        materials[i].inputs.reset();
    }
    prMemoryFree(materials);

    PrModelSourceInfo& info = s->info;
    info = PrModelSourceInfo{
        .nodes = s->nodes.data,
        .nodeCount = s->nodes.count,
        .roots = s->roots.data,
        .rootCount = s->roots.count,
        .children = s->children.data,
        .childCount = s->children.count,
        .draws = s->draws.data,
        .drawCount = s->draws.count,
        .instances = s->instances.data,
        .instanceCount = s->instances.count,
        .meshes = s->meshes.data,
        .meshCount = s->meshes.count,
        .parts = s->parts.data,
        .partCount = s->parts.count,
        .streams = s->streams.data,
        .streamCount = s->streams.count,
        .morphBoxes = s->morphBoxes.data,
        .morphBoxCount = s->morphBoxes.count,
        .weights = s->weights.data,
        .weightCount = s->weights.count,
        .skins = s->skins.data,
        .skinCount = s->skins.count,
        .joints = s->joints.data,
        .inverseBinds = s->inverseBinds.data,
        .jointCount = s->joints.count,
        .materials = s->materials.data,
        .materialCount = s->materials.count,
        .inputs = s->inputs.data,
        .inputCount = s->inputs.count,
        .textures = s->textures.data,
        .textureCount = s->textures.count,
        .textureImages = s->textureImages.data,
        .textureImageCount = s->textureImages.count,
        .images = s->images.data,
        .imageCount = s->images.count,
        .variants = s->variants.data,
        .variantCount = s->variants.count,
        .variantMaterials = s->variantMaterials.data,
        .variantMaterialCount = s->variantMaterials.count,
        .cameras = s->cameras.data,
        .cameraCount = s->cameras.count,
        .lights = s->lights.data,
        .lightCount = s->lights.count,
        // glTF's specification and its ratified extensions define no environment.
        .environment = {.node = kPrModelNodeNone, .image = kPrModelImageNone},
        .animations = s->animations.data,
        .animationCount = s->animations.count,
        .tracks = s->tracks.data,
        .trackCount = s->tracks.count,
        .keyTimes = s->keyTimes.data,
        .keyTimeCount = s->keyTimes.count,
        .keyValues = s->keyValues.data,
        .keyValueCount = s->keyValues.count,
        .diagnostics = s->diagnostics.data,
        .diagnosticCount = s->diagnostics.count,
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// The loaders

void sourceDestroy(PrModelSource* source) {
    GltfModelSource* s = static_cast<GltfModelSource*>(source);
    s->nodes.reset();
    s->roots.reset();
    s->children.reset();
    s->draws.reset();
    s->instances.reset();
    s->meshes.reset();
    s->parts.reset();
    s->partPrimitives.reset();
    s->streams.reset();
    s->morphBoxes.reset();
    s->weights.reset();
    s->skins.reset();
    s->joints.reset();
    s->inverseBinds.reset();
    s->materials.reset();
    s->inputs.reset();
    s->textures.reset();
    s->textureSources.reset();
    s->textureImages.reset();
    s->images.reset();
    s->variants.reset();
    s->variantMaterials.reset();
    s->cameras.reset();
    s->lights.reset();
    s->animations.reset();
    s->tracks.reset();
    s->keyTimes.reset();
    s->keyValues.reset();
    s->diagnostics.reset();
    prGltfDestroy(s->owned);
    prMemoryFree(s);
}

const PrModelSourceInfo* sourceInfo(const PrModelSource* source) {
    return &static_cast<const GltfModelSource*>(source)->info;
}

float* allocFloats(uint64_t count) {
    return static_cast<float*>(prMemoryAllocZeroed(size_t(count != 0 ? count : 1) * sizeof(float)));
}

// Reads an accessor's elements as floats into `out`, `width` floats of each element, the accessor's first components.
// Returns false if its buffer has no bytes.
bool readInto(const PrGltf* gltf, PrGltfAccessorIndex accessor, uint32_t count, uint32_t width, float* out) {
    const PrGltfAccessor& a = prGltfAccessors(gltf)[accessor];
    uint32_t components = prGltfElementTypeComponentCount(a.type);
    if (components == width)
        return prGltfAccessorReadFloats(gltf, accessor, 0, count, out);
    float* values = allocFloats(uint64_t(count) * components);
    bool read = prGltfAccessorReadFloats(gltf, accessor, 0, count, values);
    for (uint32_t v = 0; read && v < count; ++v) {
        for (uint32_t k = 0; k < width && k < components; ++k)
            out[size_t(v) * width + k] = values[size_t(v) * components + k];
    }
    prMemoryFree(values);
    return read;
}

// Returns the accessor of a morph target's displacements of an attribute, or kPrGltfNone.
PrGltfAccessorIndex targetAttribute(const PrGltf* gltf, const PrGltfMorphTarget& target, PrGltfSemantic semantic) {
    const PrGltfAttribute* attributes = prGltfAttributes(gltf);
    for (uint32_t i = 0; i < target.attributeCount; ++i) {
        const PrGltfAttribute& attribute = attributes[target.attributeFirst + i];
        if (attribute.semantic == semantic && attribute.set == 0)
            return attribute.accessor;
    }
    return kPrGltfNone;
}

// Returns the attribute of a primitive that a request's stream names, or kPrGltfNone.
PrGltfAccessorIndex streamAttribute(const PrGltf* gltf, PrGltfPrimitiveIndex primitive, PrStringView name) {
    const PrGltfPrimitive& p = prGltfPrimitives(gltf)[primitive];
    const PrGltfAttribute* attributes = prGltfAttributes(gltf);
    for (uint32_t i = 0; i < p.attributeCount; ++i) {
        if (attributes[p.attributeFirst + i].name == name)
            return attributes[p.attributeFirst + i].accessor;
    }
    return kPrGltfNone;
}

// The number of sets of joints and weights that a skinned vertex is read from.
constexpr uint32_t kSkinSets = 2;

// Reads a mesh's primitives that are parts, one after another, their vertices apart.
PrStatus meshRead(const PrModelSource* source, PrModelMeshIndex index, const PrModelMeshRequest* request,
        PrModelMeshData* out) {
    const GltfModelSource* s = static_cast<const GltfModelSource*>(source);
    const PrGltf* gltf = s->gltf;
    const PrModelSourceMesh& mesh = s->meshes.data[index];
    const PrGltfPrimitive* primitives = prGltfPrimitives(gltf);
    uint64_t vertices = 0;
    uint64_t indices = 0;
    for (uint32_t k = 0; k < mesh.partCount; ++k) {
        PrGltfPrimitiveIndex primitive = s->partPrimitives.data[mesh.firstPart + k];
        vertices += primitives[primitive].vertexCount;
        indices += prGltfPrimitiveTriangles(gltf, primitive, nullptr, 0);
    }
    uint32_t targets = request->morphs ? mesh.morphTargetCount : 0;
    if (vertices * (targets != 0 ? targets : 1) * 3 > 0xffffffffu || indices > 0xffffffffu)
        return PrStatus_Limit;
    out->vertexCount = uint32_t(vertices);
    out->triangleCount = uint32_t(indices / 3);
    out->partCount = mesh.partCount;
    out->positions = allocFloats(vertices * 3);
    out->normals = allocFloats(vertices * 3);
    out->tangents = allocFloats(vertices * 4);
    out->triangles = static_cast<uint32_t*>(prMemoryAllocZeroed(size_t(indices + 1) * sizeof(uint32_t)));
    out->parts = static_cast<PrModelMeshDataPart*>(prMemoryAllocZeroed(sizeof(PrModelMeshDataPart) * (mesh.partCount + 1)));
    out->streamCount = request->streamCount;
    out->streams = static_cast<float**>(prMemoryAllocZeroed(sizeof(float*) * (request->streamCount + 1)));
    out->streamWidths = static_cast<uint32_t*>(prMemoryAllocZeroed(sizeof(uint32_t) * (request->streamCount + 1)));
    // A stream's width is the widest that a primitive has of it.
    for (uint32_t i = 0; i < request->streamCount; ++i) {
        for (uint32_t k = 0; k < mesh.partCount; ++k) {
            PrGltfAccessorIndex accessor = streamAttribute(gltf, s->partPrimitives.data[mesh.firstPart + k],
                request->streams[i]);
            if (accessor == kPrGltfNone)
                continue;
            uint32_t width = prGltfElementTypeComponentCount(prGltfAccessors(gltf)[accessor].type);
            out->streamWidths[i] = width > out->streamWidths[i] ? width : out->streamWidths[i];
        }
        if (out->streamWidths[i] != 0)
            out->streams[i] = allocFloats(vertices * out->streamWidths[i]);
    }
    if (request->skin) {
        out->influenceCount = kSkinSets * 4;
        out->joints = static_cast<uint32_t*>(prMemoryAllocZeroed(size_t(vertices * out->influenceCount + 1) * 4));
        out->weights = allocFloats(vertices * out->influenceCount);
    }
    if (targets != 0) {
        out->morphTargetCount = targets;
        out->morphPositions = allocFloats(vertices * targets * 3);
        out->morphNormals = allocFloats(vertices * targets * 3);
        out->morphTangents = allocFloats(vertices * targets * 3);
    }
    bool read = true;
    uint32_t base = 0;
    uint32_t corner = 0;
    float* scratch = nullptr;
    for (uint32_t k = 0; read && k < mesh.partCount; ++k) {
        PrGltfPrimitiveIndex primitive = s->partPrimitives.data[mesh.firstPart + k];
        const PrGltfPrimitive& p = primitives[primitive];
        uint32_t count = p.vertexCount;
        PrModelMeshDataPart& part = out->parts[k];
        part.firstTriangle = corner / 3;
        PrGltfAccessorIndex position = prGltfPrimitiveAttribute(gltf, primitive, PrGltfSemantic_Position, 0);
        PrGltfAccessorIndex normal = prGltfPrimitiveAttribute(gltf, primitive, PrGltfSemantic_Normal, 0);
        PrGltfAccessorIndex tangent = prGltfPrimitiveAttribute(gltf, primitive, PrGltfSemantic_Tangent, 0);
        read = readInto(gltf, position, count, 3, out->positions + size_t(base) * 3);
        if (read && normal != kPrGltfNone) {
            read = readInto(gltf, normal, count, 3, out->normals + size_t(base) * 3);
            part.flags |= PrModelMeshPartFlag_Normals;
        }
        if (read && tangent != kPrGltfNone) {
            read = readInto(gltf, tangent, count, 4, out->tangents + size_t(base) * 4);
            part.flags |= PrModelMeshPartFlag_Tangents;
        }
        for (uint32_t i = 0; read && i < request->streamCount; ++i) {
            PrGltfAccessorIndex accessor = streamAttribute(gltf, primitive, request->streams[i]);
            if (accessor == kPrGltfNone || i >= 32)
                continue;
            uint32_t width = out->streamWidths[i];
            read = readInto(gltf, accessor, count, width, out->streams[i] + size_t(base) * width);
            part.streams |= 1u << i;
        }
        for (uint32_t set = 0; read && request->skin && set < kSkinSets; ++set) {
            PrGltfAccessorIndex jointAccessor = prGltfPrimitiveAttribute(gltf, primitive, PrGltfSemantic_Joints, set);
            PrGltfAccessorIndex weightAccessor = prGltfPrimitiveAttribute(gltf, primitive, PrGltfSemantic_Weights, set);
            if (jointAccessor == kPrGltfNone || weightAccessor == kPrGltfNone)
                continue;
            uint32_t* joints = static_cast<uint32_t*>(prMemoryAlloc(size_t(count) * 4 * sizeof(uint32_t) + 4));
            scratch = allocFloats(uint64_t(count) * 4);
            read = prGltfAccessorReadIntegers(gltf, jointAccessor, 0, count, joints) &&
                prGltfAccessorReadFloats(gltf, weightAccessor, 0, count, scratch);
            for (uint32_t v = 0; read && v < count; ++v) {
                for (uint32_t j = 0; j < 4; ++j) {
                    size_t at = (size_t(base) + v) * out->influenceCount + set * 4 + j;
                    out->joints[at] = joints[v * 4 + j];
                    out->weights[at] = scratch[v * 4 + j];
                }
            }
            prMemoryFree(joints);
            prMemoryFree(scratch);
            scratch = nullptr;
        }
        const PrGltfMorphTarget* morphTargets = prGltfMorphTargets(gltf);
        for (uint32_t t = 0; read && t < p.morphTargetCount && t < targets; ++t) {
            const PrGltfMorphTarget& target = morphTargets[p.morphTargetFirst + t];
            float* arrays[3] = {out->morphPositions, out->morphNormals, out->morphTangents};
            const PrGltfSemantic semantics[3] =
                {PrGltfSemantic_Position, PrGltfSemantic_Normal, PrGltfSemantic_Tangent};
            for (uint32_t a = 0; read && a < 3; ++a) {
                PrGltfAccessorIndex accessor = targetAttribute(gltf, target, semantics[a]);
                if (accessor == kPrGltfNone)
                    continue;
                scratch = allocFloats(uint64_t(count) * 3);
                read = readInto(gltf, accessor, count, 3, scratch);
                for (uint32_t v = 0; read && v < count; ++v)
                    prMemCopy(arrays[a] + ((size_t(base) + v) * targets + t) * 3, scratch + size_t(v) * 3,
                        3 * sizeof(float));
                prMemoryFree(scratch);
                scratch = nullptr;
            }
        }
        uint32_t triangles = prGltfPrimitiveTriangles(gltf, primitive, nullptr, 0);
        if (read)
            read = prGltfPrimitiveTriangles(gltf, primitive, out->triangles + corner, triangles) == triangles &&
                triangles != 0;
        for (uint32_t i = 0; read && i < triangles; ++i)
            out->triangles[corner + i] += base;
        part.triangleCount = triangles / 3;
        corner += triangles;
        base += count;
    }
    return read ? PrStatus_Ok : PrStatus_Io;
}

// Leaves the bytes of an image with its document, which outlives the source's readers.
void keepDocumentBytes(void* PR_NULLABLE, PrByteView) {}

PrStatus imageOpen(const PrModelSource* source, PrModelImageIndex image, PrModelImageData* out) {
    const GltfModelSource* s = static_cast<const GltfModelSource*>(source);
    // A glTF document holds its images' bytes.
    PrByteView held = prGltfImages(s->gltf)[image].bytes;
    if (held.data == nullptr)
        return PrStatus_Io;
    PrBlob* blob = prBlobCreate(held, keepDocumentBytes, nullptr);
    out->bytes = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    return PrStatus_Ok;
}

void nodeAddress(const PrModelSource*, PrModelNodeIndex node, PrTextBuffer* out) {
    prTextBufferAppend(out, "/nodes/");
    prTextBufferAppendU64(out, node, 10);
}

PrModelNodeIndex nodeFromAddress(const PrModelSource* source, PrStringView address) {
    const GltfModelSource* s = static_cast<const GltfModelSource*>(source);
    PrStringView prefix = "/nodes/";
    if (address.size <= prefix.size || PrStringView{address.data, prefix.size} != prefix)
        return kPrModelNodeNone;
    uint64_t node = 0;
    for (size_t i = prefix.size; i < address.size; ++i) {
        if (!prAsciiIsDigit(address.data[i]) || node > 0xffffffffu)
            return kPrModelNodeNone;
        node = node * 10 + uint64_t(address.data[i] - '0');
    }
    return node < s->nodes.count ? PrModelNodeIndex(node) : kPrModelNodeNone;
}

GltfModelSource* sourceMake(const PrGltf* gltf, PrGltf* PR_NULLABLE owned, PrGltfSceneIndex scene) {
    GltfModelSource* s = allocZeroed<GltfModelSource>();
    s->destroyFn = sourceDestroy;
    s->infoFn = sourceInfo;
    s->meshReadFn = meshRead;
    s->imageOpenFn = imageOpen;
    s->nodeAddressFn = nodeAddress;
    s->nodeFromAddressFn = nodeFromAddress;
    s->gltf = gltf;
    s->owned = owned;
    describe(s, scene);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// The format

// An asset being opened: the format's resolver, the asset's location, and the callback that receives its source.
struct GltfOpen {
    const PrModelResolver* resolver;
    PrRawString location;
    PrModelSourceOpenFn done;
    void* PR_NULLABLE context;
};

bool formatSupportsType(void* PR_NULLABLE, PrStringView type) {
    return type == "model/gltf+json" || type == "model/gltf-binary";
}

// Reads the file that `uri`, a relative path that may be percent-encoded, names beside the asset.
PrBlob* PR_NULLABLE loadBeside(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
    const GltfOpen* open = static_cast<const GltfOpen*>(context);
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
    prTextBufferAppend(&path, prFilePathParent(prRawStringView(&open->location)));
    prFilePathAppend(&path, prTextBufferView(&decoded));
    const PrBlobSource* bytes = open->resolver->resolve(open->resolver->context, prTextBufferView(&path));
    PrBlob* blob = nullptr;
    if (bytes != nullptr)
        prBlobSourceRead(bytes, prBlobRangeWhole(), &blob);
    prBlobSourceRelease(bytes);
    prTextBufferReset(&decoded);
    prTextBufferReset(&path);
    return blob;
}

void formatOpen(void* PR_NULLABLE, const PrBlobSource* bytes, PrStringView location, const PrModelResolver* resolver,
        PrModelSourceOpenFn done, void* PR_NULLABLE doneContext) {
    GltfOpen* open = allocZeroed<GltfOpen>();
    *open = GltfOpen{resolver, prRawStringCopy(location), done, doneContext};
    PrBlobReadDesc read = prBlobReadDescDefault();
    read.context = open;
    read.done = [](void* PR_NULLABLE held, PrBlobRead* bytesRead) {
        GltfOpen* opened = static_cast<GltfOpen*>(held);
        PrStatus status = prBlobReadStatus(bytesRead);
        PrGltf* gltf = nullptr;
        if (status == PrStatus_Ok) {
            PrGltfOptions options = prGltfOptionsDefault();
            options.load = loadBeside;
            options.loadContext = opened;
            status = prGltfCreate(prBlobReadBlob(bytesRead), &options, &gltf);
        }
        prBlobReadDestroy(bytesRead);
        PrModelSource* source = status == PrStatus_Ok ? sourceMake(gltf, gltf, kPrGltfNone) : nullptr;
        PrModelSourceOpenFn finish = opened->done;
        void* finishContext = opened->context;
        prRawStringReset(&opened->location);
        prMemoryFree(opened);
        finish(finishContext, status, source);
    };
    prBlobReadCreate(bytes, &read);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrGltfModelOptions prGltfModelOptionsDefault(void) {
    return PrGltfModelOptions{.scene = kPrGltfNone};
}

PrModelSource* prGltfModelSourceCreate(const PrGltf* gltf, const PrGltfModelOptions* PR_NULLABLE options) {
    PrGltfModelOptions given = options != nullptr ? *options : prGltfModelOptionsDefault();
    return sourceMake(gltf, nullptr, given.scene);
}

PrModelFormat prGltfModelFormat(void) {
    return PrModelFormat{.supportsTypeFn = formatSupportsType, .openFn = formatOpen};
}
