// A model's material system: how the source's materials, textures and attribute streams become the renderer's.

#include "model/src/model_bind.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/model_texture.h"
#include "pr/render_environment.h"
#include "pr/render_shaders.h"
#include "pr/string_ops.h"
#include "pr/texture.h"

#include "model/src/model_mesh.h"

namespace pr_impl {

namespace {

template <typename T>
T* allocArray(uint32_t count) {
    return static_cast<T*>(prMemoryAllocZeroed(size_t(count != 0 ? count : 1) * sizeof(T)));
}

// ---------------------------------------------------------------------------------------------------------------------
// The names of the shading models' inputs

// The inputs of the Khronos PBR model are the paths of their properties within a glTF material, as
// KHR_animation_pointer names them. The extension of the archived specular-glossiness workflow, whose properties
// replace the metallic-roughness parameters (KHR_materials_pbrSpecularGlossiness, "Best Practices": a client that draws
// the extension draws it in place of the core material).
constexpr PrStringView kSpecularGlossiness = "extensions/KHR_materials_pbrSpecularGlossiness/";

// The names of the renderer's parameters and textures that the properties of the specular-glossiness workflow set,
// which the rule of the Khronos model's names does not give.
struct NameAlias {
    PrStringView input;
    PrStringView name;
};

constexpr NameAlias kSpecularGlossinessNames[] = {
    {"diffuseFactor", "baseColor"},
    {"diffuseTexture", "baseColor"},
    {"specularFactor", "specularColor"},
    {"glossinessFactor", "glossiness"},
    {"specularGlossinessTexture", "metallicRoughness"},
};

// The parameter of the renderer's Khronos types that a material of the specular-glossiness workflow sets to 1.
constexpr PrStringView kSpecularGlossinessParameter = "specularGlossiness";

// The inputs whose textures are normal maps, or a direction of the surface, which need tangents.
constexpr PrStringView kTangentInputs[] = {
    "normalTexture",
    "extensions/KHR_materials_clearcoat/clearcoatNormalTexture",
    "extensions/KHR_materials_anisotropy/anisotropyTexture",
    "normal",
    "geometry_normal",
    "geometry_coat_normal",
};

// The inputs of a shading model that the color attribute of the renderer's library types multiplies: the color, whose
// red, green and blue it gives, and the opacity, whose value its alpha gives, which a model of a color with alpha gives
// with the color.
struct ColorInputs {
    PrStringView color;
    PrStringView opacity;
};

constexpr ColorInputs kColorInputs[PrModelShading_Count] = {
    {"pbrMetallicRoughness/baseColorFactor", ""},
    {"base_color", "geometry_opacity"},
    {"diffuseColor", "opacity"},
    {"pbrMetallicRoughness/baseColorFactor", ""},
};

// Returns true if `name` is the name of the shading model's color input or of its opacity.
bool isColorInput(PrModelShading shading, PrStringView name) {
    return name == kColorInputs[shading].color || (kColorInputs[shading].opacity.size != 0 &&
        name == kColorInputs[shading].opacity);
}

// The most bytes of a name of a renderer's parameter or texture.
constexpr size_t kMaxNameBytes = 96;

// A name of a renderer's parameter or texture, as the binder spells an input's.
struct TypeName {
    char bytes[kMaxNameBytes];
    size_t size;

    PrStringView view() const {
        return PrStringView{bytes, size};
    }

    void append(PrStringView text) {
        for (size_t i = 0; i < text.size && size < kMaxNameBytes; ++i)
            bytes[size++] = text.data[i];
    }
};

// Returns `text` without `suffix`, if it ends with it.
PrStringView withoutSuffix(PrStringView text, PrStringView suffix) {
    if (text.size > suffix.size && prStringEndsWith(text, suffix))
        return PrStringView{text.data, text.size - suffix.size};
    return text;
}

// Returns the name in the renderer's types of an input of `shading`, the vocabulary that maps a shading model's names
// onto the names that the library's types declare:
//
// - The Khronos model's property is its last name without Factor or Texture, and a texture's scale or strength is the
//   texture's name with Scale or Strength: "pbrMetallicRoughness/baseColorFactor" is "baseColor", and
//   "normalTexture/scale" is "normalScale". The specular-glossiness workflow's properties name the parameters that
//   they replace.
// - OpenPBR's input is its name run together: "base_color" is "baseColor".
// - UsdPreviewSurface's input is its own name.
TypeName typeName(PrModelShading shading, PrStringView input) {
    TypeName name{};
    if (shading == PrModelShading_OpenPbr) {
        bool upper = false;
        for (size_t i = 0; i < input.size && name.size < kMaxNameBytes; ++i) {
            if (input.data[i] == '_') {
                upper = true;
                continue;
            }
            name.bytes[name.size++] = upper ? prAsciiUpper(input.data[i]) : input.data[i];
            upper = false;
        }
        return name;
    }
    if (shading == PrModelShading_Preview) {
        name.append(input);
        return name;
    }
    if (prStringStartsWith(input, kSpecularGlossiness)) {
        PrStringView property{input.data + kSpecularGlossiness.size, input.size - kSpecularGlossiness.size};
        for (const NameAlias& alias : kSpecularGlossinessNames) {
            if (alias.input == property) {
                name.append(alias.name);
                return name;
            }
        }
    }
    // The last name, and the texture before it for a texture's own property.
    size_t last = input.size;
    while (last > 0 && input.data[last - 1] != '/')
        --last;
    PrStringView property{input.data + last, input.size - last};
    if (last > 0 && (property == "scale" || property == "strength")) {
        size_t before = last - 1;
        size_t start = before;
        while (start > 0 && input.data[start - 1] != '/')
            --start;
        name.append(withoutSuffix(PrStringView{input.data + start, before - start}, "Texture"));
        if (property == "scale")
            name.append("Scale");
        else
            name.append("Strength");
        return name;
    }
    name.append(withoutSuffix(withoutSuffix(property, "Factor"), "Texture"));
    return name;
}

const PrModelSourceMaterial* materialOf(const Model* m, PrModelMaterialIndex material) {
    return material != kPrModelMaterialNone ? &m->info->materials[material] : nullptr;
}

PrModelShading shadingOf(const Model* m, PrModelMaterialIndex material) {
    const PrModelSourceMaterial* found = materialOf(m, material);
    return found != nullptr ? found->shading : PrModelShading(PrModelShading_KhronosPbr);
}

// Returns the value of an input, or `fallback` for a material that lacks it or binds it otherwise.
float scalarOf(const Model* m, PrModelMaterialIndex material, const PrModelSourceInput* inputs, PrStringView name,
        float fallback) {
    const PrModelSourceInput* input = modelMaterialInput(m, material, inputs, name);
    return input != nullptr && input->kind == PrModelInputKind_Value ? input->value[0] : fallback;
}

PrVec3 colorOf(const Model* m, PrModelMaterialIndex material, const PrModelSourceInput* inputs, PrStringView name,
        PrVec3 fallback) {
    const PrModelSourceInput* input = modelMaterialInput(m, material, inputs, name);
    if (input == nullptr || input->kind != PrModelInputKind_Value)
        return fallback;
    return PrVec3{input->value[0], input->value[1], input->value[2]};
}

// Returns true if any of `count` values is above zero.
bool positive(const float* value, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        if (value[i] > 0)
            return true;
    }
    return false;
}

// Returns true if an input is above zero or read from a texture or the vertices.
bool inputPositive(const Model* m, PrModelMaterialIndex material, PrStringView name, uint32_t width) {
    const PrModelSourceInput* input = modelMaterialInput(m, material, m->info->inputs, name);
    if (input == nullptr)
        return false;
    return input->kind != PrModelInputKind_Value || positive(input->value, width);
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// Returns a source's sampler as the device takes it. A filter that the source leaves open is linear between texels and
// between levels, with anisotropy.
PrGpuSamplerDesc samplerDescOf(const PrModelSampler& sampler) {
    PrGpuSamplerDesc desc = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Repeat);
    for (uint32_t axis = 0; axis < 2; ++axis) {
        desc.address[axis] = PrGpuAddress_Repeat;
        if (sampler.wrap[axis] == PrModelWrap_Clamp)
            desc.address[axis] = PrGpuAddress_Clamp;
        else if (sampler.wrap[axis] == PrModelWrap_Mirror)
            desc.address[axis] = PrGpuAddress_Mirror;
    }
    desc.magnify = sampler.magnify == PrModelFilter_Nearest ? PrGpuFilter_Nearest : PrGpuFilter_Linear;
    if (sampler.minify == PrModelFilter_Nearest)
        desc.minify = PrGpuFilter_Nearest;
    if (sampler.mip == PrModelFilter_None) {
        // The filter reads the first level alone.
        desc.mip = PrGpuFilter_Nearest;
        desc.maxLod = 0;
    } else if (sampler.mip == PrModelFilter_Nearest) {
        desc.mip = PrGpuFilter_Nearest;
    }
    bool open = sampler.minify == PrModelFilter_Default && sampler.mip == PrModelFilter_Default;
    if (open && desc.magnify == PrGpuFilter_Linear)
        desc.maxAnisotropy = 8;
    return desc;
}

// Returns the image that a source's texture is drawn from: the first of its images whose media type the model's opener
// supports, or else the first, which fails to open, or kPrModelImageNone for a texture without an image.
PrModelImageIndex textureImage(const Model* m, const PrModelSourceTexture& texture) {
    if (texture.imageCount == 0)
        return kPrModelImageNone;
    const PrModelImageIndex* images = m->info->textureImages + texture.firstImage;
    for (uint32_t i = 0; m->options.textures != nullptr && i < texture.imageCount; ++i) {
        PrStringView type = m->info->images[images[i]].mediaType;
        if (type.size != 0 && prTextureOpenerSupportsType(m->options.textures, type))
            return images[i];
    }
    return images[0];
}

// Returns the index of the model's texture of an image for a use, which it adds if the model has none, and adds the
// use's channels to the texture's.
uint32_t internTexture(Model* m, PrModelImageIndex image, PrRenderTextureKind kind, PrModelTextureColor color,
        PrRenderChannelFlags channels) {
    for (uint32_t i = 0; i < m->textures.count; ++i) {
        ModelTexture& known = m->textures.data[i];
        if (known.image == image && known.kind == kind && known.color == color) {
            known.channels |= channels;
            return i;
        }
    }
    m->textures.push(ModelTexture{.image = image, .kind = kind, .color = color, .channels = channels});
    return m->textures.count - 1;
}

// Returns the use of the input's texture by a texture of a renderer's type, `declared`: its coordinates and their
// transform, the sampler, and the input's map of the texel's channels. Sets `*texture` to the model's texture that it
// reads, which a model that populates its renderer interns, with the channels that the type reads.
PrRenderTextureUse textureUse(Model* m, uint32_t inputIndex, const PrModelSourceInput& input,
        const PrRenderMaterialTexture& declared, bool interns, uint32_t* texture) {
    *texture = kModelTextureNone;
    PrRenderTextureUse use = prRenderTextureUseDefault(PrRenderTexture{});
    if (input.kind != PrModelInputKind_Texture || input.texture == kPrModelTextureNone)
        return use;
    const PrModelSourceTexture& source = m->info->textures[input.texture];
    use.sampler = samplerDescOf(source.sampler);
    use.texCoord = input.texCoordSet < 2 ? input.texCoordSet : 0;
    PrAffine2 trs = prRenderTextureTransform({input.offset[0], input.offset[1]}, input.rotation,
        {input.scale[0], input.scale[1]});
    use.transform = prAffine2Concat(trs, input.transform);
    const PrModelChannelMap& map = input.channels;
    if (input.width == 1) {
        // A scalar reads the channel that it weighs most.
        uint8_t channel = 0;
        for (uint8_t j = 1; j < 4; ++j) {
            if (prF32Abs(map.weights[0][j]) > prF32Abs(map.weights[0][channel]))
                channel = j;
        }
        use.channel = channel;
        use.scale[channel] = map.weights[0][channel];
        use.bias[channel] = map.offset[0];
    } else {
        for (uint32_t k = 0; k < 4; ++k) {
            use.scale[k] = k < input.width ? map.weights[k][k] : 1;
            use.bias[k] = k < input.width ? map.offset[k] : 0;
        }
    }
    if (interns) {
        PrModelImageIndex image = m->textureImages[input.texture];
        PrRenderChannelFlags channels = declared.channels;
        if ((channels & PrRenderChannelFlag_Chosen) != 0)
            channels = PrRenderChannelFlags(1u << use.channel);
        m->inputTextures[inputIndex] = internTexture(m, image, declared.kind, source.color, channels);
    }
    *texture = m->inputTextures[inputIndex];
    return use;
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// A material of a renderer's type, as the binder builds it from a source's material: its type, its optional attributes,
// its render states, its parameter record, and its textures, each with the model's texture that it reads.
struct MaterialBuild {
    uint32_t type;
    const PrRenderMaterialType* info;
    uint32_t attributes;
    PrRenderAlphaMode alphaMode;
    PrRenderMaterialFlags flags;
    float alphaCutoff;
    PrRenderEmission emission;
    uint8_t parameters[kPrRenderMaxParameterBytes];
    PrRenderTextureUse uses[32];        // by the type's texture index
    uint32_t textures[32];              // by the type's texture index: the model's texture, or kModelTextureNone
    uint32_t textureCount;
};

// The most textures of a type that the binder fills.
constexpr uint32_t kMaxTypeTextures = 32;

// Starts a build of a material of `type`, with the type's values for every parameter and no texture.
void buildBegin(const Model* m, uint32_t type, MaterialBuild* build) {
    const PrRenderShaderSet* set = prRenderShaderSet(m->render);
    build->type = type;
    build->info = &set->materialTypes[type];
    build->attributes = 0;
    build->alphaMode = PrRenderAlphaMode_Opaque;
    build->flags = 0;
    build->alphaCutoff = 0.5f;
    build->emission = m->options.emission;
    PR_ASSERT(build->info->parameterBytes <= sizeof(build->parameters));
    prRenderMaterialParametersDefault(build->info, build->parameters);
    build->textureCount = build->info->textureCount < kMaxTypeTextures ? build->info->textureCount : kMaxTypeTextures;
    for (uint32_t i = 0; i < build->textureCount; ++i) {
        build->uses[i] = prRenderTextureUseDefault(PrRenderTexture{});
        build->textures[i] = kModelTextureNone;
    }
}

// Sets parameter `name` of a build, if its type has one, to `count` values.
void buildSet(MaterialBuild* build, PrStringView name, const float* values, uint32_t count) {
    uint32_t parameter = prRenderMaterialParameterFromName(build->info, name);
    if (parameter != kPrRenderMaterialParameterMissing)
        prRenderMaterialParameterSet(build->info, build->parameters, parameter, values, count);
}

// Returns the float value of component `component` of parameter `name` of a build, or `fallback` for a type without it.
float buildValue(const MaterialBuild& build, PrStringView name, uint32_t component, float fallback) {
    uint32_t parameter = prRenderMaterialParameterFromName(build.info, name);
    if (parameter == kPrRenderMaterialParameterMissing)
        return fallback;
    float value = 0;
    prMemCopy(&value, build.parameters + build.info->parameters[parameter].offset + component * 4, 4);
    return value;
}

// Returns true if a build names a texture of its type `name`.
bool buildHasTexture(const MaterialBuild& build, PrStringView name) {
    uint32_t texture = prRenderMaterialTextureFromName(build.info, name);
    return texture < build.textureCount && build.textures[texture] != kModelTextureNone;
}

PrRenderAlphaMode alphaModeOf(const PrModelSourceMaterial* PR_NULLABLE material) {
    if (material == nullptr)
        return PrRenderAlphaMode_Opaque;
    if (material->alpha == PrModelAlpha_Mask)
        return PrRenderAlphaMode_Mask;
    return material->alpha == PrModelAlpha_Blend ? PrRenderAlphaMode_Blend : PrRenderAlphaMode_Opaque;
}

// Returns true if an input sets what a type would read of it: a texture, an attribute, or a value other than the one
// that `type` declares for its parameter.
bool inputMatters(const PrRenderMaterialType& type, const PrModelSourceInput& input, PrStringView name) {
    if (input.kind != PrModelInputKind_Value)
        return true;
    uint32_t parameter = prRenderMaterialParameterFromName(&type, name);
    if (parameter == kPrRenderMaterialParameterMissing)
        return false;
    const PrRenderMaterialParameter& p = type.parameters[parameter];
    const float declared[4] = {p.value.x, p.value.y, p.value.z, p.value.w};
    for (uint32_t k = 0; k < input.width && k < p.componentCount; ++k) {
        if (input.value[k] != declared[k])
            return true;
    }
    return false;
}

// Returns true if `type` has a parameter or a texture of `name`.
bool typeHas(const PrRenderMaterialType& type, PrStringView name) {
    return prRenderMaterialParameterFromName(&type, name) != kPrRenderMaterialParameterMissing ||
        prRenderMaterialTextureFromName(&type, name) != kPrRenderMaterialTextureMissing;
}

// The renderer's types that each shading model draws as, by name, in the order of preference: the binder chooses the
// first that the renderer's set has and that holds what the material sets, of which the last that the set has holds the
// most. None of them is named for a format: UsdPreview is the type of the UsdPreviewSurface shading model. `lost` names
// the diagnostic of a material that sets what the type that draws it does not hold. A set without any of them draws the
// material as `fallback`, another shading model's type, whose inputs the binder converts to.
struct ShadingTypes {
    PrStringView names[2];
    PrStringView lost;
    PrStringView fallback;
};

constexpr ShadingTypes kShadingTypes[PrModelShading_Count] = {
    {{"KhronosPbr", "KhronosPbrLayered"}, "layers", "KhronosPbr"},
    {{"OpenPbr", ""}, "inputs", "UsdPreview"},
    {{"UsdPreview", ""}, "inputs", "KhronosPbr"},
    {{"Unlit", ""}, "inputs", "KhronosPbr"},
};

// Returns the number of the inputs of a material that a type does not hold and the richest type of its shading model
// does: a texture or an attribute of a name that it lacks, or a value of one other than the richest type's value.
uint32_t lostInputs(const Model* m, PrModelMaterialIndex material, const PrRenderMaterialType& type,
        const PrRenderMaterialType& richest) {
    const PrModelSourceMaterial* found = materialOf(m, material);
    uint32_t lost = 0;
    bool specularGlossiness = false;
    for (uint32_t i = 0; found != nullptr && i < found->inputCount; ++i) {
        const PrModelSourceInput& input = m->info->inputs[found->firstInput + i];
        specularGlossiness = specularGlossiness || (found->shading == PrModelShading_KhronosPbr &&
            input.name.size > kSpecularGlossiness.size && prStringStartsWith(input.name, kSpecularGlossiness));
        TypeName name = typeName(found->shading, input.name);
        if (!typeHas(type, name.view()) && typeHas(richest, name.view()) && inputMatters(richest, input, name.view()))
            ++lost;
    }
    if (specularGlossiness && !typeHas(type, kSpecularGlossinessParameter) &&
            typeHas(richest, kSpecularGlossinessParameter))
        ++lost;
    return lost;
}

// Chooses the renderer's type of a material, by its shading model, among the types of the renderer's set: the first
// that holds every input that the richest holds, or else the richest. Reports what the chosen type loses.
uint32_t chooseType(Model* m, PrModelMaterialIndex material, bool diagnoses) {
    const PrRenderShaderSet* set = prRenderShaderSet(m->render);
    PrModelShading shading = shadingOf(m, material);
    const ShadingTypes& candidates = kShadingTypes[shading];
    uint32_t types[2];
    uint32_t count = 0;
    for (PrStringView name : candidates.names) {
        uint32_t type = name.size != 0 ? prRenderMaterialTypeFromName(set, name) : kPrRenderMaterialTypeMissing;
        if (type != kPrRenderMaterialTypeMissing)
            types[count++] = type;
    }
    if (count == 0) {
        if (diagnoses && shading == PrModelShading_OpenPbr)
            modelDiagnose(m, "openpbr", "the renderer has no OpenPBR type, and OpenPBR draws as UsdPreviewSurface");
        return prRenderMaterialTypeFromName(set, candidates.fallback);
    }
    const PrRenderMaterialType& richest = set->materialTypes[types[count - 1]];
    for (uint32_t i = 0; i + 1 < count; ++i) {
        if (lostInputs(m, material, set->materialTypes[types[i]], richest) == 0)
            return types[i];
    }
    uint32_t chosen = types[count - 1];
    // A set without a richer type draws a material that sets what only that type holds as the poorer: an input of a
    // name that the poorer lacks, other than a value of zero.
    bool lacks = candidates.names[1].size != 0 && count == 1;
    const PrModelSourceMaterial* found = materialOf(m, material);
    bool lost = false;
    for (uint32_t i = 0; diagnoses && lacks && found != nullptr && i < found->inputCount; ++i) {
        const PrModelSourceInput& input = m->info->inputs[found->firstInput + i];
        TypeName name = typeName(shading, input.name);
        if (!typeHas(richest, name.view()))
            lost = lost || input.kind != PrModelInputKind_Value || positive(input.value, input.width);
    }
    if (lost)
        modelDiagnose(m, candidates.lost, "the renderer has no type that holds every input of the material, which "
            "draws as a type that holds fewer");
    return chosen;
}

// Fills a build of the material `material` of a shading model by the names of its inputs: each value input sets the
// parameter of its name, each texture input the texture of its name, and an input that an attribute of the vertices
// gives sets the parameter to 1, which the attribute multiplies. Sets the optional attributes of the type that the
// material binds. An input of a name that the type lacks sets nothing, and a texture of one is reported.
void bindNamed(Model* m, PrModelMaterialIndex material, const PrModelSourceInput* inputs, bool interns, MaterialBuild* build) {
    const PrModelSourceMaterial* found = materialOf(m, material);
    if (found == nullptr)
        return;
    const PrRenderMaterialType& type = *build->info;
    for (uint32_t i = 0; i < found->inputCount; ++i) {
        uint32_t index = found->firstInput + i;
        const PrModelSourceInput& input = inputs[index];
        TypeName name = typeName(found->shading, input.name);
        if (found->shading == PrModelShading_KhronosPbr && input.name.size > kSpecularGlossiness.size &&
                prStringStartsWith(input.name, kSpecularGlossiness)) {
            const float on = 1;
            buildSet(build, kSpecularGlossinessParameter, &on, 1);
        }
        if (input.kind == PrModelInputKind_Value) {
            buildSet(build, name.view(), input.value, input.width);
        } else if (input.kind == PrModelInputKind_Attribute) {
            const float ones[4] = {1, 1, 1, 1};
            buildSet(build, name.view(), ones, input.width);
        }
        bool tinted = input.kind == PrModelInputKind_Texture && input.tint.size != 0;
        for (uint32_t k = 0; k < type.optionalAttributeCount; ++k) {
            if (type.optionalAttributes[k] == name.view() && (input.kind == PrModelInputKind_Attribute || tinted))
                build->attributes |= 1u << k;
        }
        if (input.kind != PrModelInputKind_Texture)
            continue;
        uint32_t at = prRenderMaterialTextureFromName(&type, name.view());
        if (at >= build->textureCount) {
            if (interns)
                modelDiagnose(m, "inputs", "a texture of an input that the renderer's type does not sample");
            continue;
        }
        build->uses[at] = textureUse(m, index, input, type.textures[at], interns, &build->textures[at]);
    }
}

// The preview type's input that draws each input of OpenPBR where the renderer has no OpenPBR type.
struct OpenPbrPreview {
    PrStringView openPbr;
    PrStringView preview;
};

constexpr OpenPbrPreview kOpenPbrPreviews[] = {
    {"base_color", "diffuseColor"},
    {"geometry_normal", "normal"},
    {"specular_roughness", "roughness"},
    {"base_metalness", "metallic"},
    {"geometry_opacity", "opacity"},
    {"emission_color", "emissiveColor"},
    {"coat_weight", "clearcoat"},
    {"coat_roughness", "clearcoatRoughness"},
};

// Fills a build of the preview type with the part of an OpenPBR material that the preview surface has, for a renderer
// whose shader set has no OpenPBR type: its base, metalness, roughness, normal, emission, opacity and coat.
void bindPreviewOfOpenPbr(Model* m, PrModelMaterialIndex material, const PrModelSourceInput* inputs, bool interns,
        MaterialBuild* build) {
    float base = scalarOf(m, material, inputs, "base_weight", 1);
    float emission = scalarOf(m, material, inputs, "emission_luminance", 0);
    PrVec3 diffuse = colorOf(m, material, inputs, "base_color", PrVec3{0.8f, 0.8f, 0.8f}) * base;
    PrVec3 emissive = colorOf(m, material, inputs, "emission_color", PrVec3{1, 1, 1}) * emission;
    const PrModelSourceInput* color = modelMaterialInput(m, material, inputs, "base_color");
    if (color != nullptr && color->kind == PrModelInputKind_Attribute)
        diffuse = PrVec3{base, base, base};
    float opacity = scalarOf(m, material, inputs, "geometry_opacity", 1);
    const PrModelSourceInput* opacityInput = modelMaterialInput(m, material, inputs, "geometry_opacity");
    if (opacityInput != nullptr && opacityInput->kind == PrModelInputKind_Attribute)
        opacity = 1;
    const float diffuseValues[3] = {diffuse.x, diffuse.y, diffuse.z};
    const float emissiveValues[3] = {emissive.x, emissive.y, emissive.z};
    const float metallic = scalarOf(m, material, inputs, "base_metalness", 0);
    const float roughness = scalarOf(m, material, inputs, "specular_roughness", 0.3f);
    const float ior = scalarOf(m, material, inputs, "specular_ior", 1.5f);
    const float clearcoat = scalarOf(m, material, inputs, "coat_weight", 0);
    const float clearcoatRoughness = scalarOf(m, material, inputs, "coat_roughness", 0);
    buildSet(build, "diffuseColor", diffuseValues, 3);
    buildSet(build, "emissiveColor", emissiveValues, 3);
    buildSet(build, "metallic", &metallic, 1);
    buildSet(build, "roughness", &roughness, 1);
    buildSet(build, "ior", &ior, 1);
    buildSet(build, "clearcoat", &clearcoat, 1);
    buildSet(build, "clearcoatRoughness", &clearcoatRoughness, 1);
    buildSet(build, "opacity", &opacity, 1);
    const PrModelSourceMaterial& found = m->info->materials[material];
    for (const OpenPbrPreview& shown : kOpenPbrPreviews) {
        const PrModelSourceInput* input = modelMaterialInput(m, material, inputs, shown.openPbr);
        uint32_t at = prRenderMaterialTextureFromName(build->info, shown.preview);
        if (input == nullptr || input->kind != PrModelInputKind_Texture || at >= build->textureCount)
            continue;
        uint32_t index = uint32_t(input - inputs);
        PR_ASSERT(index >= found.firstInput && index < found.firstInput + found.inputCount);
        PrRenderTextureUse& out = build->uses[at];
        out = textureUse(m, index, *input, build->info->textures[at], interns, &build->textures[at]);
        // A color's texture is scaled by its weight, as the preview surface has none.
        float factor = 1;
        if (shown.openPbr == "base_color")
            factor = base;
        else if (shown.openPbr == "emission_color")
            factor = emission;
        for (uint32_t k = 0; input->width != 1 && k < 3; ++k) {
            out.scale[k] *= factor;
            out.bias[k] *= factor;
        }
    }
}

// Builds the renderer's material of the source's material `material` with `inputs`, the inputs of every material, or
// the default material for kPrModelMaterialNone. The type of a source's material is its binding's.
void buildMaterial(Model* m, PrModelMaterialIndex material, const PrModelSourceInput* inputs, bool interns,
        MaterialBuild* build) {
    const PrModelSourceMaterial* found = materialOf(m, material);
    uint32_t type = material != kPrModelMaterialNone ? m->bindings[material].type : chooseType(m, material, false);
    const PrRenderShaderSet* set = prRenderShaderSet(m->render);
    PrModelShading shading = shadingOf(m, material);
    buildBegin(m, type, build);
    build->alphaMode = alphaModeOf(found);
    build->flags = found != nullptr && found->doubleSided ? PrRenderMaterialFlag_DoubleSided : 0;
    build->alphaCutoff = found != nullptr ? found->alphaCutoff : 0.5f;
    bool previewOfOpenPbr = shading == PrModelShading_OpenPbr && set->materialTypes[type].name != "OpenPbr";
    if (previewOfOpenPbr)
        bindPreviewOfOpenPbr(m, material, inputs, interns, build);
    else
        bindNamed(m, material, inputs, interns, build);
    switch (shading) {
        case PrModelShading_Unlit:
            // An unlit color is what the surface shows, which is the view's.
            build->emission = PrRenderEmission_Display;
            break;
        case PrModelShading_Preview: {
            // The opacity decides the alpha mode, as the specification has it: a threshold above zero cuts the surface
            // away below it, and otherwise an opacity below 1, or from a texture, blends.
            float threshold = scalarOf(m, material, inputs, "opacityThreshold", 0);
            build->alphaMode = PrRenderAlphaMode_Opaque;
            if (threshold > 0) {
                build->alphaMode = PrRenderAlphaMode_Mask;
                build->alphaCutoff = threshold;
            } else if (buildValue(*build, "opacity", 0, 1) < 1 || buildHasTexture(*build, "opacity")) {
                build->alphaMode = PrRenderAlphaMode_Blend;
            }
            break;
        }
        case PrModelShading_OpenPbr:
            // A surface that transmits light shows the scene behind it.
            if (!previewOfOpenPbr && (buildValue(*build, "transmissionWeight", 0, 0) > 0 ||
                    buildHasTexture(*build, "transmissionWeight")))
                build->flags |= PrRenderMaterialFlag_ReadsBehind;
            break;
        default:
            // A layer that transmits light reads the scene behind it.
            if (buildValue(*build, "transmission", 0, 0) > 0)
                build->flags |= PrRenderMaterialFlag_ReadsBehind;
            break;
    }
    if (previewOfOpenPbr) {
        build->alphaMode = alphaModeOf(found);
        if (buildValue(*build, "opacity", 0, 1) < 1 || buildHasTexture(*build, "opacity"))
            build->alphaMode = PrRenderAlphaMode_Blend;
    }
}

// Returns the number of the different textures of a build that the renderer's device does not sample: those past the
// device's limit.
uint32_t unsampledTextures(PrRender* render, const MaterialBuild& build) {
    uint32_t seen[kMaxTypeTextures];
    uint32_t count = 0;
    for (uint32_t i = 0; i < build.textureCount; ++i) {
        uint32_t texture = build.textures[i];
        bool known = texture == kModelTextureNone;
        for (uint32_t k = 0; !known && k < count; ++k)
            known = seen[k] == texture;
        if (!known)
            seen[count++] = texture;
    }
    uint32_t limit = prRenderMaterialTextureLimit(render, build.flags);
    return count > limit ? count - limit : 0;
}

// Sets a material of the renderer to a build, whose textures are the model's renderer's.
void setBuilt(Model* m, PrRenderMaterial target, MaterialBuild* build) {
    for (uint32_t i = 0; i < build->textureCount; ++i) {
        if (build->textures[i] != kModelTextureNone)
            build->uses[i].texture = m->textures.data[build->textures[i]].texture;
    }
    PrRenderMaterialDesc desc =
        prRenderMaterialDescDefault(build->type, build->parameters, build->info->parameterBytes);
    desc.attributes = build->attributes;
    desc.alphaMode = build->alphaMode;
    desc.flags = build->flags;
    desc.alphaCutoff = build->alphaCutoff;
    desc.emission = build->emission;
    desc.textures = build->uses;
    desc.textureCount = build->textureCount;
    prRenderMaterialSet(m->render, target, &desc);
}

void addLayout(Model* m, const PrRenderVertexLayout* layout) {
    for (uint32_t k = 0; k < m->layouts.count; ++k) {
        if (m->layouts.data[k] == layout)
            return;
    }
    m->layouts.push(layout);
}

// Returns true if a layout has an attribute that serves the material's input: the color attribute for the shading
// model's color and opacity, or a custom attribute of the input's name in the renderer's types.
bool layoutServes(const PrRenderVertexLayout* layout, PrModelShading shading, PrStringView input) {
    TypeName name = typeName(shading, input);
    for (uint32_t i = 0; i < layout->attributeCount; ++i) {
        const PrRenderVertexAttribute& attribute = layout->attributes[i];
        if (attribute.semantic == PrRenderSemantic_Color && attribute.semanticIndex == 0 && isColorInput(shading, input))
            return true;
        if (attribute.semantic == PrRenderSemantic_Custom && (attribute.name == input || attribute.name == name.view()))
            return true;
    }
    return false;
}

// Reports the inputs of the material that its variant's layout has no attribute for: their attributes are not drawn,
// and each input is its value where a vertex lacks the attribute.
void diagnoseAttributes(Model* m, PrModelMaterialIndex material, const PrRenderVertexLayout* layout) {
    const PrModelSourceMaterial& found = m->info->materials[material];
    for (uint32_t i = 0; i < found.inputCount; ++i) {
        const PrModelSourceInput& input = m->info->inputs[found.firstInput + i];
        bool attribute = input.kind == PrModelInputKind_Attribute ||
            (input.kind == PrModelInputKind_Texture && input.tint.size != 0);
        if (attribute && !layoutServes(layout, found.shading, input.name))
            modelDiagnose(m, "attribute-inputs",
                "an input that an attribute of the vertices gives, which the renderer's type has no attribute for");
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Images

void setLoad(ModelLoadState* state, PrModelLoad load, PrStatus status) {
    state->status.store(status);
    state->load.store(load);
}

// Records how an image's job went, unless the model's loading was cancelled, and ends the job.
void finishImage(Model* m, uint32_t index, PrStatus status) {
    if (!m->cancelled.load()) {
        if (status != PrStatus_Ok && status != PrStatus_NotSupported && status != PrStatus_Limit &&
                status != PrStatus_Io && status != PrStatus_Lost)
            status = PrStatus_InvalidData;
        setLoad(&m->images[index].state, status == PrStatus_Ok ? PrModelLoad_Ready : PrModelLoad_Failed, status);
    }
    m->revision.fetchAdd(1);
    modelJobFinish(m);
}

// The steps of an image's job that are under way: the uploads of its textures and the read of the environment. The last
// step to complete finishes the job with the first failure, or Ok.
struct ImageLoad {
    ModelJob* job;
    Atomic<uint32_t> pending;
    Atomic<uint8_t> status;
    Atomic<uint64_t> bytes;
};

// Completes one step of an image's job.
void imageStep(ImageLoad* load, PrStatus status) {
    if (status != PrStatus_Ok) {
        uint8_t expected = PrStatus_Ok;
        load->status.cas(expected, status);
    }
    if (load->pending.fetchSub(1) != 1)
        return;
    Model* m = load->job->model;
    uint32_t index = load->job->index;
    m->images[index].textureBytes.store(load->bytes.load());
    PrStatus finished = PrStatus(load->status.load());
    prMemoryFree(load);
    finishImage(m, index, finished);
}

// An upload of one of the model's textures, which is a step of its image's job.
struct TextureUpload {
    ImageLoad* load;
    uint32_t texture;           // of the model's textures
    uint64_t bytes;             // of the texels of the GPU texture
};

// Gives the renderer's texture the uploaded GPU texture.
void textureUploaded(void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
    TextureUpload upload = *static_cast<TextureUpload*>(context);
    prMemoryFree(context);
    Model* m = upload.load->job->model;
    if (status == PrStatus_Ok) {
        PrGpuDevice* device = prRenderDevice(m->render);
        if (!m->cancelled.load()) {
            prRenderTextureSetGpu(m->render, m->textures.data[upload.texture].texture, texture.texture);
            upload.load->bytes.fetchAdd(upload.bytes);
        }
        prGpuTextureRelease(device, texture.texture);
    }
    imageStep(upload.load, status);
}

// Gives the renderer's environment the radiance of the texels that `read` read, tinted by the environment's color.
void environmentRead(void* PR_NULLABLE context, PrTextureRead* read) {
    ImageLoad* load = static_cast<ImageLoad*>(context);
    Model* m = load->job->model;
    PrStatus status = prTextureReadStatus(read);
    const PrTextureDesc& desc = *prTextureSourceDesc(prTextureReadSource(read));
    if (status == PrStatus_Ok && !m->cancelled.load()) {
        size_t pixels = size_t(desc.extent.x) * desc.extent.y;
        float* rgba = reinterpret_cast<float*>(prTextureReadStorage(read));
        // The radiance is red, green and blue alone, in place.
        PrVec3 tint = m->info->environment.color;
        for (size_t i = 0; i < pixels; ++i) {
            rgba[i * 3] = rgba[i * 4] * tint.x;
            rgba[i * 3 + 1] = rgba[i * 4 + 1] * tint.y;
            rgba[i * 3 + 2] = rgba[i * 4 + 2] * tint.z;
        }
        PrRenderRadianceImage image{
            .pixels = {reinterpret_cast<const uint8_t*>(rgba), size_t(desc.extent.x) * 3 * sizeof(float), desc.extent},
            .channels = 3,
        };
        status = prRenderEnvironmentSetEquirect(m->render, m->environment.environment, &image);
    }
    prTextureReadDestroy(read);
    imageStep(load, status);
}

// Starts reading the environment's texture as radiance: floats of linear light. A texture of floats holds them, and one
// of 8 bits is color, whose values are sRGB's.
void loadEnvironment(ImageLoad* load, const PrTextureSource* source) {
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    bool floatsStored = desc.format == PrGpuFormat_Rgba16F || desc.format == PrGpuFormat_Rgba32F;
    PrGpuFormat form = PrGpuFormat_Rgba32F;
    if (!floatsStored)
        form = prTextureSourceProduces(source, PrGpuFormat_Rgba8Srgb) ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8;
    const PrTextureSource* decoded = nullptr;
    PrStatus status = prTextureSourceReformat(source, form, &decoded);
    if (status != PrStatus_Ok) {
        imageStep(load, status);
        return;
    }
    // Pixels of 8 bits take the Srgb form, whose values a conversion to floats decodes.
    const PrTextureSource* encoded = decoded;
    if (!floatsStored) {
        PrTextureConvertDesc relabel = prTextureConvertDescDefault(PrGpuFormat_Rgba8Srgb);
        relabel.alpha = prTextureSourceDesc(decoded)->alpha;
        encoded = prTextureSourceCreateConvert(decoded, &relabel);
        prTextureSourceRelease(decoded);
    }
    PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba32F);
    const PrTextureSource* radiance = prTextureSourceCreateConvert(encoded, &convert);
    prTextureSourceRelease(encoded);
    PrTextureReadDesc read = prTextureReadDescAll(prTextureSourceDesc(radiance));
    read.levelCount = 1;
    read.layerCount = 1;
    read.done = environmentRead;
    read.context = load;
    prTextureReadCreate(radiance, &read);
    prTextureSourceRelease(radiance);
}

// The image's source, which the opener made of its bytes or the source gave: makes the chain of each of the model's
// textures of the image for its use, and uploads each, and reads the environment.
void imageOpened(void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
    ModelJob* job = static_cast<ModelJob*>(context);
    Model* m = job->model;
    if (status != PrStatus_Ok || source == nullptr || m->cancelled.load()) {
        prTextureSourceRelease(source);
        finishImage(m, job->index, status != PrStatus_Ok ? status : PrStatus_InvalidData);
        return;
    }
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    bool srgbSource = prGpuFormatIsSrgb(desc.format);
    bool environment = job->index == m->info->environment.image && m->environment.environment.id != 0;
    uint32_t steps = environment ? 1 : 0;
    bool mismatch = false;
    for (uint32_t i = 0; i < m->textures.count; ++i) {
        const ModelTexture& texture = m->textures.data[i];
        if (texture.image != job->index)
            continue;
        ++steps;
        mismatch = mismatch || (srgbSource && texture.color == PrModelTextureColor_Data);
    }
    m->images[job->index].colorMismatch.store(mismatch ? 1 : 0);
    if (steps == 0) {
        prTextureSourceRelease(source);
        finishImage(m, job->index, PrStatus_Ok);
        return;
    }
    ImageLoad* load = allocZeroed<ImageLoad>();
    load->job = job;
    load->pending.store(steps);
    load->status.store(PrStatus_Ok);
    PrGpuDevice* device = prRenderDevice(m->render);
    PrModelTextureChainFn chain = m->options.chain != nullptr ? m->options.chain : prModelTextureChainDefault;
    for (uint32_t i = 0; i < m->textures.count; ++i) {
        const ModelTexture& texture = m->textures.data[i];
        if (texture.image != job->index)
            continue;
        PrModelTextureUse use{.kind = texture.kind, .channels = texture.channels, .color = texture.color};
        PrModelTextureChain made{};
        PrStatus chained = chain(m->options.chainContext, source, &use, device, &made);
        if (chained != PrStatus_Ok || made.source == nullptr) {
            prTextureSourceRelease(made.source);
            imageStep(load, chained != PrStatus_Ok ? chained : PrStatus_NotSupported);
            continue;
        }
        TextureUpload* upload = static_cast<TextureUpload*>(prMemoryAlloc(sizeof(TextureUpload)));
        *upload = TextureUpload{
            .load = load,
            .texture = i,
            .bytes = prTextureUploadSize(device, prTextureSourceDesc(made.source), made.flags),
        };
        prTextureUpload(device, made.source, made.flags, m->info->images[job->index].name, textureUploaded, upload);
        prTextureSourceRelease(made.source);
    }
    if (environment)
        loadEnvironment(load, source);
    prTextureSourceRelease(source);
}

// Gives the renderer's environment the radiance of a light without a texture: its color from every direction.
PrStatus setEvenEnvironment(Model* m) {
    PrVec3 color = m->info->environment.color;
    float texel[3] = {color.x, color.y, color.z};
    PrRenderRadianceImage faces[6];
    for (PrRenderRadianceImage& face : faces)
        face =
            PrRenderRadianceImage{.pixels = {reinterpret_cast<const uint8_t*>(texel), sizeof(texel), {1, 1}}, .channels = 3};
    return prRenderEnvironmentSetCube(m->render, m->environment.environment, faces);
}

} // namespace

uint32_t ModelStreamNames::add(PrStringView name) {
    for (uint32_t i = 0; i < names.count; ++i) {
        if (names.data[i] == name)
            return i;
    }
    names.push(name);
    return names.count - 1;
}

void modelBindDescribe(Model* m) {
    const PrModelSourceInfo& info = *m->info;
    m->textureImages = allocArray<PrModelImageIndex>(info.textureCount);
    for (uint32_t i = 0; i < info.textureCount; ++i) {
        PrModelImageIndex image = textureImage(m, info.textures[i]);
        m->textureImages[i] = image;
        if (image != kPrModelImageNone)
            m->images[image].used = true;
    }
    if (info.environment.node != kPrModelNodeNone && info.environment.image != kPrModelImageNone)
        m->images[info.environment.image].used = true;
    for (uint32_t i = 0; i < info.inputCount; ++i) {
        const PrModelSourceInput& input = info.inputs[i];
        if (input.kind == PrModelInputKind_Texture && input.texCoordSet >= 2)
            modelDiagnose(m, "texture-coordinate-sets", "a texture reads a set of texture coordinates past the second, "
                "and reads the first instead");
    }
    // What each part's preparation needs of its material.
    for (uint32_t p = 0; p < info.partCount; ++p) {
        PrModelMaterialIndex material = info.parts[p].material;
        ModelPart& part = m->parts[p];
        const PrModelSourceMaterial* found = materialOf(m, material);
        part.blends = found != nullptr && found->alpha == PrModelAlpha_Blend;
        if (found == nullptr || found->shading == PrModelShading_Unlit)
            continue;
        for (PrStringView name : kTangentInputs) {
            const PrModelSourceInput* input = modelMaterialInput(m, material, info.inputs, name);
            if (input == nullptr || input->kind != PrModelInputKind_Texture || part.wantsTangents)
                continue;
            part.wantsTangents = true;
            part.tangentSet = input->texCoordSet;
        }
        // Anisotropy is a direction of the frame of the texture coordinates, which a material with it needs without any
        // normal map, by the first set where it has no texture.
        if (!part.wantsTangents && modelMaterialInput(m, material, info.inputs,
                "extensions/KHR_materials_anisotropy/anisotropyStrength") != nullptr) {
            part.wantsTangents = true;
            part.tangentSet = 0;
        }
    }
}

void modelBindPopulate(Model* m) {
    PrRender* render = m->render;
    const PrModelSourceInfo& info = *m->info;
    const PrRenderShaderSet* set = prRenderShaderSet(render);
    m->textures.clear();
    prMemoryFree(m->inputTextures);
    m->inputTextures = allocArray<uint32_t>(info.inputCount);
    for (uint32_t i = 0; i < info.inputCount; ++i)
        m->inputTextures[i] = kModelTextureNone;
    // The meshes have the attributes of each layout of the variants that the model's materials draw with, and the
    // default material's.
    m->layouts.clear();
    uint32_t fallback = chooseType(m, kPrModelMaterialNone, false);
    bool defaulted = false;
    for (uint32_t i = 0; i < info.partCount; ++i)
        defaulted = defaulted || info.parts[i].material == kPrModelMaterialNone;
    if (fallback != kPrRenderMaterialTypeMissing && (defaulted || info.materialCount == 0))
        addLayout(m, prRenderMaterialTypeLayout(set, fallback, 0));
    // Each material is built first, which makes the model's textures with what the materials read of them, and set once
    // the renderer's textures exist.
    MaterialBuild* builds = allocArray<MaterialBuild>(info.materialCount);
    for (uint32_t i = 0; i < info.materialCount; ++i) {
        m->bindings[i].type = chooseType(m, i, true);
        if (m->bindings[i].type == kPrRenderMaterialTypeMissing)
            continue;
        buildMaterial(m, i, info.inputs, true, &builds[i]);
        m->bindings[i].attributes = builds[i].attributes;
    }
    for (uint32_t i = 0; i < m->textures.count; ++i)
        m->textures.data[i].texture = prRenderTextureCreate(render);
    m->unsampledTextures = 0;
    for (uint32_t i = 0; i < info.materialCount; ++i) {
        m->materials[i] = prRenderMaterialCreate(render);
        if (m->bindings[i].type == kPrRenderMaterialTypeMissing)
            continue;
        const PrRenderVertexLayout* layout = prRenderMaterialTypeLayout(set, m->bindings[i].type, builds[i].attributes);
        addLayout(m, layout);
        diagnoseAttributes(m, i, layout);
        m->unsampledTextures += unsampledTextures(render, builds[i]);
        setBuilt(m, m->materials[i], &builds[i]);
    }
    prMemoryFree(builds);
    // An environment without a texture is one color, which costs nothing to set here.
    if (info.environment.node != kPrModelNodeNone) {
        m->environment.environment = prRenderEnvironmentCreate(render);
        if (info.environment.image == kPrModelImageNone)
            setEvenEnvironment(m);
    }
}

uint32_t modelBindMaterial(Model* m, PrRenderMaterial target, PrModelMaterialIndex material,
        const PrModelSourceInput* PR_NULLABLE inputs) {
    if (inputs == nullptr)
        inputs = m->info->inputs;
    if (material != kPrModelMaterialNone && m->bindings[material].type == kPrRenderMaterialTypeMissing)
        return 0;
    MaterialBuild* build = allocZeroed<MaterialBuild>();
    buildMaterial(m, material, inputs, false, build);
    // A material's optional attributes are those of the meshes that the model prepared for it.
    if (material != kPrModelMaterialNone)
        build->attributes = m->bindings[material].attributes;
    uint32_t unsampled = unsampledTextures(m->render, *build);
    setBuilt(m, target, build);
    prMemoryFree(build);
    return unsampled;
}

bool modelBindEmits(const Model* m, PrModelMaterialIndex material) {
    if (material == kPrModelMaterialNone)
        return false;
    switch (m->info->materials[material].shading) {
        case PrModelShading_KhronosPbr: {
            float strength = scalarOf(m, material, m->info->inputs,
                "extensions/KHR_materials_emissive_strength/emissiveStrength", 1);
            const PrModelSourceInput* factor = modelMaterialInput(m, material, m->info->inputs, "emissiveFactor");
            return strength > 0 && factor != nullptr && positive(factor->value, 3);
        }
        case PrModelShading_Preview:
            return inputPositive(m, material, "emissiveColor", 3);
        case PrModelShading_OpenPbr:
            return inputPositive(m, material, "emission_luminance", 1) && (inputPositive(m, material, "emission_color", 3) ||
                modelMaterialInput(m, material, m->info->inputs, "emission_color") == nullptr);
        default:
            return false;
    }
}

bool modelBindIsUnlit(const Model* m, PrModelMaterialIndex material) {
    return material != kPrModelMaterialNone && m->info->materials[material].shading == PrModelShading_Unlit;
}

void modelBindAttribute(const Model* m, PrModelPartIndex part, const PrRenderVertexAttribute& attribute,
        ModelStreamNames* names, ModelComponentSource* out) {
    const PrModelSourceInfo& info = *m->info;
    const PrModelSourcePart& given = info.parts[part];
    for (uint32_t k = 0; k < 4; ++k)
        out[k] = ModelComponentSource{.stream = kModelStreamNone};
    auto identity = [&](uint32_t stream, uint32_t components) {
        for (uint32_t k = 0; k < components && k < 4; ++k) {
            out[k].stream = stream;
            out[k].weights[k] = 1;
        }
    };
    auto partStream = [&](PrModelSemantic semantic, uint32_t set, PrStringView name) -> const PrModelSourceStream* {
        for (uint32_t i = 0; i < given.streamCount; ++i) {
            const PrModelSourceStream& stream = info.streams[given.firstStream + i];
            if (name.size != 0) {
                bool underscored = stream.name.size == name.size + 1 && stream.name.data[0] == '_' &&
                    prAsciiEqualsFold(PrStringView{stream.name.data + 1, name.size}, name);
                if (stream.name == name || underscored)
                    return &stream;
            } else if (stream.semantic == semantic && stream.set == set) {
                return &stream;
            }
        }
        return nullptr;
    };
    // An input bound to an attribute gives each of its components, by its map of the stream's channels.
    auto mapped = [&](const PrModelSourceInput& input, uint32_t first, uint32_t count) {
        PrStringView name = input.kind == PrModelInputKind_Attribute ? input.attribute : input.tint;
        uint32_t stream = names->add(name);
        for (uint32_t k = 0; k < count && first + k < 4; ++k) {
            out[first + k].stream = stream;
            if (input.kind == PrModelInputKind_Attribute) {
                prMemCopy(out[first + k].weights, input.channels.weights[k], sizeof(out[first + k].weights));
                out[first + k].offset = input.channels.offset[k];
            } else {
                out[first + k].weights[k] = 1;
            }
        }
    };
    if (attribute.semantic == PrRenderSemantic_Uv) {
        const PrModelSourceStream* stream = partStream(PrModelSemantic_TexCoord, attribute.semanticIndex, {});
        if (stream != nullptr)
            identity(names->add(stream->name), 2);
        return;
    }
    PrModelMaterialIndex material = given.material;
    PrModelShading shading = shadingOf(m, material);
    if (attribute.semantic == PrRenderSemantic_Color) {
        if (attribute.semanticIndex != 0)
            return;
        const ColorInputs& inputs = kColorInputs[shading];
        const PrModelSourceInput* color = modelMaterialInput(m, material, info.inputs, inputs.color);
        const PrModelSourceInput* opacity =
            inputs.opacity.size != 0 ? modelMaterialInput(m, material, info.inputs, inputs.opacity) : nullptr;
        bool colorBound = color != nullptr && (color->kind == PrModelInputKind_Attribute ||
            (color->kind == PrModelInputKind_Texture && color->tint.size != 0));
        if (colorBound)
            mapped(*color, 0, color->kind == PrModelInputKind_Attribute ? color->width : 3);
        if (opacity != nullptr && opacity->kind == PrModelInputKind_Attribute)
            mapped(*opacity, 3, 1);
        // The Khronos model's color is the vertices' first color where its material binds none (glTF-2.0/#meshes).
        bool khronos = shading == PrModelShading_KhronosPbr || shading == PrModelShading_Unlit;
        const PrModelSourceStream* stream = khronos && !colorBound ? partStream(PrModelSemantic_Color, 0, {}) : nullptr;
        if (stream != nullptr)
            identity(names->add(stream->name), 4);
        return;
    }
    // A custom attribute is the input of its name, in the renderer's types or the shading model's own.
    const PrModelSourceMaterial* found = materialOf(m, material);
    for (uint32_t i = 0; found != nullptr && i < found->inputCount; ++i) {
        const PrModelSourceInput& input = info.inputs[found->firstInput + i];
        if (input.kind == PrModelInputKind_Attribute &&
                (input.name == attribute.name || typeName(shading, input.name).view() == attribute.name)) {
            mapped(input, 0, input.width);
            return;
        }
    }
    const PrModelSourceStream* stream = partStream(PrModelSemantic_Custom, 0, attribute.name);
    if (stream != nullptr)
        identity(names->add(stream->name), 4);
}

uint32_t modelBindTangentStream(const Model* m, PrModelPartIndex part, ModelStreamNames* names) {
    const ModelPart& prepared = m->parts[part];
    if (!prepared.wantsTangents)
        return kModelStreamNone;
    const PrModelSourcePart& given = m->info->parts[part];
    for (uint32_t i = 0; i < given.streamCount; ++i) {
        const PrModelSourceStream& stream = m->info->streams[given.firstStream + i];
        if (stream.semantic == PrModelSemantic_TexCoord && stream.set == prepared.tangentSet)
            return names->add(stream.name);
    }
    return kModelStreamNone;
}

void modelBindLoadImage(ModelJob* job) {
    Model* m = job->model;
    if (m->cancelled.load()) {
        modelJobFinish(m);
        return;
    }
    if (m->options.textures == nullptr) {
        finishImage(m, job->index, PrStatus_NotSupported);
        return;
    }
    PrModelImageData data{};
    PrStatus status = prModelSourceImageOpen(m->source, job->index, &data);
    if (status != PrStatus_Ok) {
        finishImage(m, job->index, status == PrStatus_InvalidData ? PrStatus_Io : status);
        return;
    }
    if (data.source != nullptr) {
        imageOpened(job, PrStatus_Ok, data.source);
        return;
    }
    const PrModelSourceImage& image = m->info->images[job->index];
    PrStringView name = image.location.size != 0 ? image.location : image.name;
    PrTextureLimits limits = prTextureLimitsDefault();
    prTextureOpenerOpen(m->options.textures, data.bytes, name, &limits, imageOpened, job);
    prBlobSourceRelease(data.bytes);
}

} // namespace pr_impl
