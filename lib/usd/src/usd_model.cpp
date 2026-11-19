// USD's model source: a composed stage described for a model, and the model format of USD layers and packages.

#include "pr/usd_model.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/materialx_openpbr.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mesh.h"
#include "pr/mesh_normals.h"
#include "pr/mesh_polygons.h"
#include "pr/mesh_shapes.h"
#include "pr/model_source_backend.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/usd_geom.h"

namespace pr_impl {

namespace {

constexpr float kPi = 3.14159265358979323846f;

// The diffuse color of a surface that binds no material and authors no display color: the preview surface's fallback.
constexpr float kFallbackGray = 0.18f;

// The number of edges around the axis of a round implicit surface's mesh.
constexpr uint32_t kShapeSegments = 48;

// The most keys of a track: the time samples of the values that it holds, the context's start and end, and the time
// codes between them.
constexpr uint32_t kMaxSamples = 4096;

constexpr uint32_t kNone = 0xffffffffu;

template <typename T>
T* allocArray(uint32_t count) {
    return static_cast<T*>(prMemoryAllocZeroed(size_t(count ? count : 1) * sizeof(T)));
}

PrMatrix4 matrixOf(const PrDMatrix4& m) {
    return prMatrix4FromDMatrix4(&m);
}

// A map from 64-bit keys to the indices of the entries that they name, which only grows.
struct KeyMap {
    RawArray<uint64_t> keys;
    KeyIndex index;

    uint32_t find(uint64_t key) const {
        uint32_t at = index.find(key, [&](uint32_t i) { return keys.data[i]; });
        return at == kHashMissing ? kNone : at;
    }

    // Adds `key` as the next entry, which the map must not hold.
    void add(uint64_t key) {
        index.insert(key, keys.count);
        keys.push(key);
    }

    void reset() {
        keys.reset();
        index.reset();
    }
};

// The material that a prim's binding and its ancestors' resolve to, and whether that binding is stronger than the
// bindings under it.
struct Binding {
    PrUsdPrimIndex material;        // or kPrUsdNone
    bool strong;
};

// A node of the source, beside its description: the prim, which a prototype's prim is for each of its instances' nodes,
// its first child and next sibling, and whether its transform varies.
struct UsdNode {
    PrUsdPrimIndex prim;
    uint32_t firstChild;
    uint32_t nextSibling;
    bool animated;
};

// A mesh of the source: a Mesh prim or an implicit surface, and the subsets that are its parts, in order, after which
// the faces that no subset holds are the last part where `hasRest`.
struct UsdMesh {
    PrUsdPrimIndex prim;
    uint32_t firstSubset;
    uint32_t subsetCount;
    bool hasRest;
};

// A stage's source: the stage, which it owns if a format opened it, the description, and what reading the meshes needs.
struct UsdModelSource : PrModelSource {
    const PrUsdStage* stage;
    PrUsdStage* PR_NULLABLE owned;
    PrUsdGeomStageInfo stageInfo;
    double restTime;                // the time code of the rest pose: the context's start
    double endTime;                 // the time code where the context's animation ends, or restTime for none
    double perSecond;               // the stage's time codes per second
    PrMatrix4 stageToScene;         // maps the stage's units and up axis to the source's scene: meters, +y up
    PrStringView texCoordNames[2];  // the primitive variable of each set of texture coordinates
    uint32_t texCoordCount;
    PrModelSourceInfo info;
    RawArray<UsdNode> usdNodes;
    RawArray<UsdMesh> usdMeshes;
    RawArray<PrUsdPrimIndex> subsets;
    RawArray<PrModelSourceNode> nodes;
    RawArray<PrModelNodeIndex> roots;
    RawArray<PrModelNodeIndex> children;
    RawArray<PrModelSourceDraw> draws;
    RawArray<PrMatrix4> instances;
    RawArray<PrModelSourceMesh> meshes;
    RawArray<PrModelSourcePart> parts;
    RawArray<PrModelSourceStream> streams;
    RawArray<PrModelSourceMaterial> materials;
    RawArray<PrModelSourceInput> inputs;
    RawArray<PrModelSourceTexture> textures;
    RawArray<PrModelImageIndex> textureImages;
    RawArray<PrModelSourceImage> images;
    RawArray<PrRawString> locations;                // by image
    RawArray<const PrBlobSource*> imageBytes;       // by image: its provider, or null where it does not open
    RawArray<PrModelSourceCamera> cameras;
    RawArray<PrModelSourceLight> lights;
    RawArray<PrModelSourceAnimation> animations;
    RawArray<PrModelSourceTrack> tracks;
    RawArray<float> keyTimes;
    RawArray<float> keyValues;
    RawArray<PrModelDiagnostic> diagnostics;
};

// What the walk of a stage keeps while it reads, beside the source's state.
struct Reader {
    UsdModelSource* s;
    const PrUsdStage* stage;
    const PrUsdPrim* prims;
    PrUsdScratch* scratch;
    PrUsdModelOptions options;
    KeyMap meshKeys;
    KeyMap materialKeys;                // by material: its prim and its sidedness
    KeyMap imageKeys;
    RawArray<uint32_t> lastChild;       // by node
    RawArray<PrUsdPrimIndex> instancers;
    uint32_t lastRoot;                  // the last node without a parent, or kNone
    PrUsdPrimIndex dome;                // the dome light that is the environment, or kPrUsdNone
    bool environmentRead;

    // Set while the walk reads a material at another time than the rest pose's, to compare its values: the reading then
    // adds no texture, image or set of texture coordinates to the source.
    bool sampling;
};

// A prim that the walk reaches: the node that it is under, and what it inherits.
struct Visit {
    PrUsdPrimIndex prim;
    uint32_t parent;
    Binding binding;
    PrUsdGeomPurpose purpose;
};

void addDiagnostic(UsdModelSource* s, PrStringView feature, PrStringView detail) {
    for (uint32_t i = 0; i < s->diagnostics.count; ++i) {
        if (s->diagnostics.data[i].feature == feature)
            return;
    }
    s->diagnostics.push(PrModelDiagnostic{feature, detail});
}

// Returns what a prim binds, given what its ancestors bind: theirs if it is stronger than its descendants', and else
// the prim's own if it authors one (UsdShadeMaterialBindingAPI::ComputeBoundMaterial).
Binding combine(Binding inherited, const PrUsdShadeBinding& own) {
    if (inherited.strong || !own.authored)
        return inherited;
    return Binding{own.material, own.strongerThanDescendants};
}

// Returns true if the walk draws the prim and what is under it: it is active, loaded and defined, and not a class.
bool walks(const PrUsdPrim& prim) {
    PrUsdPrimFlags wanted = PrUsdPrimFlag_Active | PrUsdPrimFlag_Loaded | PrUsdPrimFlag_Defined;
    return (prim.flags & wanted) == wanted && (prim.flags & PrUsdPrimFlag_Abstract) == 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Images and textures

// Returns the set of texture coordinates that a texture's primitive variable is read into, adding it if there is room.
// A variable past the second set reads the first, and the source says so.
uint8_t texCoordOf(Reader& r, PrStringView name) {
    UsdModelSource* s = r.s;
    for (uint32_t i = 0; i < s->texCoordCount; ++i) {
        if (s->texCoordNames[i] == name)
            return uint8_t(i);
    }
    if (r.sampling)
        return 0;
    if (s->texCoordCount < 2) {
        s->texCoordNames[s->texCoordCount] = name;
        return uint8_t(s->texCoordCount++);
    }
    addDiagnostic(s, "texture-coordinate-sets",
        "a material reads texture coordinates past the second primitive variable of them, and reads the first instead");
    return 0;
}

PrModelWrap wrapOf(PrUsdTextureWrap wrap) {
    switch (wrap) {
        case PrUsdTextureWrap_Mirror:
            return PrModelWrap_Mirror;
        case PrUsdTextureWrap_Clamp:
        case PrUsdTextureWrap_Black:
            // The device has no border of black, and an edge's texels are the nearest.
            return PrModelWrap_Clamp;
        default:
            return PrModelWrap_Repeat;
    }
}

// Returns the source's image for a location, which it adds if the source has none, with the provider of its bytes.
uint32_t internImage(Reader& r, PrStringView location) {
    uint64_t key = prHashBytes(location.data, location.size, 0);
    uint32_t found = r.imageKeys.find(key);
    if (found != kNone && prRawStringView(&r.s->locations.data[found]) == location)
        return found;
    r.imageKeys.add(key);
    r.s->locations.push(prRawStringCopy(location));
    PrUsdOptions package = prUsdOptionsDefault();
    package.codec = r.options.codec;
    const PrBlobSource* bytes = nullptr;
    prUsdResourceSource(location, r.options.open, r.options.openContext, &package, &bytes);
    r.s->imageBytes.push(bytes);
    return r.s->locations.count - 1;
}

// Returns true if a texture that an input reads holds color, encoded as sRGB: where the network says so, and for a
// color input whose space is `auto`. A linear space, raw or one such as ACEScg, is read as its values are stored.
bool isSrgbOf(const PrUsdTexture& texture, bool colorInput) {
    return texture.colorSpace == PrUsdTextureColorSpace_Srgb ||
        (colorInput && texture.colorSpace == PrUsdTextureColorSpace_Auto);
}

// Returns the source's texture of an image through a texture's wrapping, read as color if `srgb`, which it adds if the
// source has none.
uint32_t internTexture(Reader& r, uint32_t image, const PrUsdTexture& texture, bool srgb) {
    PrModelSampler sampler{.wrap = {wrapOf(texture.wrapS), wrapOf(texture.wrapT)}};
    PrModelTextureColor color = srgb ? PrModelTextureColor_Srgb : PrModelTextureColor_Data;
    RawArray<PrModelSourceTexture>& textures = r.s->textures;
    for (uint32_t i = 0; i < textures.count; ++i) {
        const PrModelSourceTexture& known = textures.data[i];
        if (r.s->textureImages.data[known.firstImage] == image && known.sampler.wrap[0] == sampler.wrap[0] &&
                known.sampler.wrap[1] == sampler.wrap[1] && known.color == color)
            return i;
    }
    r.s->textureImages.push(image);
    textures.push(PrModelSourceTexture{
        .firstImage = r.s->textureImages.count - 1,
        .imageCount = 1,
        .sampler = sampler,
        .color = color,
    });
    return textures.count - 1;
}

// Returns a texture input of a USD texture at `time`, `width` components wide, read through `channels`, or false if the
// texture names no file. While the walk samples, the input names no texture.
bool textureInput(Reader& r, PrStringView name, uint32_t width, const PrUsdTexture& texture, bool srgb,
        const PrModelChannelMap& channels, double time, PrModelSourceInput* out) {
    PrRawString location = prUsdTextureLocation(r.stage, &texture, time);
    bool found = prRawStringView(&location).size != 0;
    if (found) {
        const float white[4] = {1, 1, 1, 1};
        *out = prModelSourceInputValue(name, width, white);
        out->kind = PrModelInputKind_Texture;
        out->texture = kPrModelTextureNone;
        if (!r.sampling)
            out->texture = internTexture(r, internImage(r, prRawStringView(&location)), texture, srgb);
        out->texCoordSet = texCoordOf(r, texture.primvar);
        // USD's coordinates have t up the image, and the renderer's v runs down it: v = 1 - t, which is its own
        // inverse. The texture's transform of st is between the two flips.
        PrAffine2 flip{.a = 1, .d = -1, .f = 1};
        out->transform = prAffine2Concat(prAffine2Concat(flip, texture.transform), flip);
        out->channels = channels;
    }
    prRawStringReset(&location);
    return found;
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// The preview surface's inputs, by their names in the specification, and their widths.
struct PreviewInput {
    PrStringView name;
    uint32_t width;
    bool color;                 // its texture of `auto` color space is sRGB
};

constexpr PreviewInput kPreviewInputs[PrUsdPreviewInput_Count] = {
    {"diffuseColor", 3, true},
    {"emissiveColor", 3, true},
    {"specularColor", 3, true},
    {"metallic", 1, false},
    {"roughness", 1, false},
    {"clearcoat", 1, false},
    {"clearcoatRoughness", 1, false},
    {"opacity", 1, false},
    {"opacityThreshold", 1, false},
    {"ior", 1, false},
    {"normal", 3, false},
    {"displacement", 1, false},
    {"occlusion", 1, false},
};

// Returns the map of a UsdUVTexture's channels to an input of `width` components: its scale and bias, of the channel
// that a scalar reads.
PrModelChannelMap previewChannels(const PrUsdTexture& texture, uint32_t width) {
    PrModelChannelMap map{};
    if (width == 1) {
        uint32_t channel = texture.channel < PrUsdTextureChannel_Rgb ? texture.channel : 0;
        map.weights[0][channel] = texture.scale[channel];
        map.offset[0] = texture.bias[channel];
        return map;
    }
    for (uint32_t k = 0; k < 4; ++k) {
        map.weights[k][k] = texture.scale[k];
        map.offset[k] = texture.bias[k];
    }
    return map;
}

// Returns an input that a primitive variable gives, its components its own, with `fallback` where a mesh lacks it.
PrModelSourceInput attributeInput(PrStringView name, uint32_t width, PrStringView primvar, const float* fallback) {
    PrModelSourceInput input = prModelSourceInputValue(name, width, fallback);
    input.kind = PrModelInputKind_Attribute;
    input.attribute = primvar;
    return input;
}

// Reads a material's preview surface at `time` as inputs of UsdPreviewSurface. Returns whether it blends.
bool readPreview(Reader& r, const PrUsdPreviewSurface& surface, double time, RawArray<PrModelSourceInput>* out) {
    for (uint32_t i = 0; i < PrUsdPreviewInput_Count; ++i) {
        const PrUsdPreviewInputValue& value = surface.inputs[i];
        const PreviewInput& info = kPreviewInputs[i];
        PrModelSourceInput input{};
        if (value.textured && textureInput(r, info.name, info.width, value.texture,
                isSrgbOf(value.texture, info.color), previewChannels(value.texture, info.width), time, &input))
            out->push(input);
        else if (value.primvar.size != 0 && !value.textured)
            out->push(attributeInput(info.name, info.width, value.primvar, value.value));
        else
            out->push(prModelSourceInputValue(info.name, info.width, value.value));
    }
    const float workflow = surface.useSpecularWorkflow ? 1.0f : 0.0f;
    out->push(prModelSourceInputValue("useSpecularWorkflow", 1, &workflow));
    const PrUsdPreviewInputValue& opacity = surface.inputs[PrUsdPreviewInput_Opacity];
    if (surface.inputs[PrUsdPreviewInput_OpacityThreshold].value[0] > 0)
        return false;
    return opacity.value[0] < 1 || opacity.textured || opacity.primvar.size != 0;
}

// Reads a material's MaterialX network at `time` as OpenPBR's inputs, as lib/materialx maps its surface's, each folded
// by lib/usd. Returns false if the network reaches no surface that lib/materialx maps, and writes whether it blends.
bool readMtlx(Reader& r, PrUsdPrimIndex prim, double time, RawArray<PrModelSourceInput>* out, bool* blends) {
    PrStringView id{};
    PrUsdPrimIndex shader = prUsdMtlxSurface(r.stage, prim, r.scratch, &id);
    PrMaterialxSurface surface = prMaterialxSurfaceFromCategory(prUsdMtlxNodeCategory(id));
    if (shader == kPrUsdNone || surface == PrMaterialxSurface_Count)
        return false;
    uint32_t count = prMaterialxOpenPbrInputCount();
    PrUsdMtlxInput* read = allocArray<PrUsdMtlxInput>(count);
    PrMaterialxFolded* folded = allocArray<PrMaterialxFolded>(count);
    for (uint32_t i = 0; i < count; ++i) {
        PrMaterialxOpenPbrInput info = prMaterialxOpenPbrInput(surface, i);
        PrUsdMtlxInput& input = read[i];
        prUsdMtlxInputRead(r.stage, shader, info.name, info.width, info.fallback, time, r.scratch, &input);
        if (info.name.size == 0) {
            for (uint32_t k = 0; k < info.width; ++k)
                input.value[k] = info.fallback[k];
        }
        if (input.approximated)
            addDiagnostic(r.s, "material-networks", "a MaterialX network computes an input with a node that is not "
                "folded, which draws as its first input or its first image");
        PrMaterialxFolded& value = folded[i];
        prMemCopy(value.value, input.value, sizeof(value.value));
        prMemCopy(value.weights, input.weights, sizeof(value.weights));
        prMemCopy(value.offset, input.offset, sizeof(value.offset));
        value.varies = input.textured || input.primvar.size != 0;
    }
    if (prMaterialxOpenPbrTranslate(surface, folded) != 0)
        addDiagnostic(r.s, "material-networks", "a surface that is partly metal draws with Standard Surface's specular "
            "weight, which darkens its metal");
    *blends = false;
    for (uint32_t i = 0; i < count; ++i) {
        PrMaterialxOpenPbrInput info = prMaterialxOpenPbrInput(surface, i);
        const PrUsdMtlxInput& input = read[i];
        const PrMaterialxFolded& value = folded[i];
        PrModelChannelMap channels{};
        prMemCopy(channels.weights, value.weights, sizeof(channels.weights));
        prMemCopy(channels.offset, value.offset, sizeof(channels.offset));
        bool normal = info.openPbr == "geometry_normal" || info.openPbr == "geometry_coat_normal";
        PrModelSourceInput made = prModelSourceInputValue(info.openPbr, info.width, value.value);
        // A normal is drawn where a normal map makes it, and the mesh's own normal stands for any other.
        if (input.textured && normal == input.normalMap &&
                textureInput(r, info.openPbr, info.width, input.texture, isSrgbOf(input.texture, false), channels, time, &made)) {
            made.tint = input.tint;
            if (input.tint.size != 0)
                prMemCopy(made.value, input.tintFallback, sizeof(made.value));
        } else if (!input.textured && input.primvar.size != 0) {
            made = attributeInput(info.openPbr, info.width, input.primvar, input.primvarFallback);
            made.channels = channels;
        }
        if (info.openPbr == "geometry_opacity")
            *blends = made.kind != PrModelInputKind_Value || made.value[0] < 1;
        out->push(made);
    }
    prMemoryFree(read);
    prMemoryFree(folded);
    return true;
}

// Reads the inputs of a material prim at `time` into `out`, and writes its shading model and whether it blends. A
// material whose network has a MaterialX surface draws it, one without draws its preview surface, and a prim that binds
// none draws its display color and opacity.
void readMaterial(Reader& r, PrUsdPrimIndex prim, double time, RawArray<PrModelSourceInput>* out,
        PrModelShading* shading, bool* blends) {
    *shading = PrModelShading_Preview;
    *blends = false;
    PrUsdPreviewSurface surface{};
    if (prim != kPrUsdNone && readMtlx(r, prim, time, out, blends)) {
        *shading = PrModelShading_OpenPbr;
    } else if (prim != kPrUsdNone && prUsdPreviewSurfaceRead(r.stage, prim, time, r.scratch, &surface)) {
        *blends = readPreview(r, surface, time, out);
    } else {
        const float gray[3] = {kFallbackGray, kFallbackGray, kFallbackGray};
        const float one = 1;
        out->push(attributeInput("diffuseColor", 3, "displayColor", gray));
        out->push(attributeInput("opacity", 1, "displayOpacity", &one));
    }
}

// Returns the source's material for a material prim drawn with a sidedness, which it adds if the source has none, with
// the images and textures that it reads at rest.
uint32_t internMaterial(Reader& r, PrUsdPrimIndex prim, bool doubleSided) {
    uint64_t key = (uint64_t(prim) << 1) | (doubleSided ? 1 : 0);
    uint32_t found = r.materialKeys.find(key);
    if (found != kNone)
        return found;
    r.materialKeys.add(key);
    UsdModelSource* s = r.s;
    PrModelSourceMaterial material{
        .name = prim != kPrUsdNone ? r.prims[prim].name : PrStringView{},
        .alphaCutoff = 0.5f,
        .doubleSided = doubleSided,
        .firstInput = s->inputs.count,
    };
    bool blends = false;
    readMaterial(r, prim, s->restTime, &s->inputs, &material.shading, &blends);
    material.alpha = blends ? PrModelAlpha_Blend : PrModelAlpha_Opaque;
    material.inputCount = s->inputs.count - material.firstInput;
    s->materials.push(material);
    return s->materials.count - 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

// Writes the part of each face of a mesh to `parts`: the first of its subsets that holds it, by its place among
// `subsets`, or `subsetCount` for a face that no subset holds, or kNone for a hole.
void partsOfFaces(const PrUsdGeomMesh& mesh, const PrUsdGeomSubset* subsets, uint32_t subsetCount, uint32_t* parts) {
    for (uint32_t f = 0; f < mesh.faceCount; ++f)
        parts[f] = subsetCount;
    for (uint32_t s = subsetCount; s-- > 0;) {
        for (uint32_t i = 0; i < subsets[s].indexCount; ++i)
            parts[subsets[s].indices[i]] = s;
    }
    for (uint32_t i = 0; i < mesh.holeCount; ++i)
        parts[mesh.holeIndices[i]] = kNone;
}

// A prim's surface as faces: a Mesh's own, or the polygons that approximate an implicit surface, which the surface
// owns. A shape's faces have the normals of the surface at their corners.
struct UsdSurface {
    PrUsdGeomMesh mesh;
    RawArray<PrVec3> points;
    RawArray<uint32_t> faceCounts;
    RawArray<uint32_t> corners;
    RawArray<PrVec3> normals;

    void reset() {
        points.reset();
        faceCounts.reset();
        corners.reset();
        normals.reset();
    }
};

// Returns the mesh that approximates an implicit surface.
PrMeshShapeDesc shapeDescOf(const PrUsdGeomShape& shape) {
    PrMeshShapeDesc desc{
        .kind = PrMeshShapeKind_Sphere,
        .axis = shape.axis,
        .radius = float(shape.radius),
        .radiusBottom = float(shape.radiusBottom),
        .radiusTop = float(shape.radiusTop),
        .height = float(shape.height),
        .segments = kShapeSegments,
    };
    float size = float(shape.size);
    switch (shape.kind) {
        case PrUsdGeomShapeKind_Cube:
            desc.kind = PrMeshShapeKind_Box;
            desc.size = {size, size, size};
            break;
        case PrUsdGeomShapeKind_Cylinder:
            desc.kind = PrMeshShapeKind_Cylinder;
            break;
        case PrUsdGeomShapeKind_Cone:
            desc.kind = PrMeshShapeKind_Cylinder;
            desc.radiusBottom = float(shape.radius);
            desc.radiusTop = 0;
            break;
        case PrUsdGeomShapeKind_Capsule:
            desc.kind = PrMeshShapeKind_Capsule;
            break;
        case PrUsdGeomShapeKind_Plane:
            // The mesh's plane spans its x and y, which an axis of x or y turns to the plane's length and width.
            desc.kind = PrMeshShapeKind_Plane;
            desc.size = {float(shape.width), float(shape.length), 0};
            if (shape.axis != 2)
                desc.size = {float(shape.length), float(shape.width), 0};
            break;
        default:
            break;
    }
    return desc;
}

// Reads the surface of a Mesh or of an implicit surface at `time`, as prUsdGeomMeshRead reads a mesh.
PrStatus surfaceRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch, UsdSurface* out) {
    PrUsdGeomShape shape{};
    if (prUsdGeomPrimKind(stage, prim) != PrUsdGeomKind_Shape)
        return prUsdGeomMeshRead(stage, prim, time, scratch, &out->mesh);
    if (!prUsdGeomShapeRead(stage, prim, time, scratch, &shape))
        return PrStatus_InvalidData;
    PrMeshShapeDesc desc = shapeDescOf(shape);
    PrMeshShapeCounts counts = prMeshShapeCounts(&desc);
    out->points.resize(counts.pointCount);
    out->faceCounts.resize(counts.faceCount);
    out->corners.resize(counts.cornerCount);
    out->normals.resize(counts.cornerCount);
    prMeshShapeWrite(&desc, out->points.data, out->faceCounts.data, out->corners.data, out->normals.data);
    // A left-handed shape's front is its inside, which its faces then wind counterclockwise toward.
    for (uint32_t i = 0; shape.leftHanded && i < out->normals.count; ++i)
        out->normals.data[i] = -out->normals.data[i];
    // The counts and corners are far below the range where unsigned and signed integers differ.
    out->mesh = PrUsdGeomMesh{
        .points = out->points.data,
        .pointCount = counts.pointCount,
        .faceVertexCounts = reinterpret_cast<const int32_t*>(out->faceCounts.data),
        .faceCount = counts.faceCount,
        .faceVertexIndices = reinterpret_cast<const int32_t*>(out->corners.data),
        .cornerCount = counts.cornerCount,
        .leftHanded = shape.leftHanded,
        .doubleSided = shape.doubleSided,
        .subdivision = PrUsdGeomSubdivision_None,
        .normals = {
            .value =
                {.type = PrUsdType_Float3, .isArray = true, .count = counts.cornerCount, .data = out->normals.data},
            .interpolation = PrUsdGeomInterpolation_FaceVarying,
            .elementSize = 1,
        },
    };
    return PrStatus_Ok;
}

// Adds the streams that a part's vertices may have, from `first` of the source's streams: the source's texture
// coordinates, and the primitive variables that its material's inputs read, each once.
void addStreams(UsdModelSource* s, const PrModelSourceMaterial& material, uint32_t first) {
    for (uint32_t i = 0; i < s->texCoordCount; ++i)
        s->streams.push(PrModelSourceStream{
            s->texCoordNames[i], PrModelSemantic_TexCoord, uint8_t(i),
            prMeshFormatFloat32(2),
        });
    for (uint32_t i = 0; i < material.inputCount; ++i) {
        const PrModelSourceInput& input = s->inputs.data[material.firstInput + i];
        PrStringView name = input.kind == PrModelInputKind_Attribute ? input.attribute : input.tint;
        bool known = name.size == 0;
        for (uint32_t k = first; k < s->streams.count && !known; ++k)
            known = s->streams.data[k].name == name;
        if (!known)
            s->streams.push(PrModelSourceStream{name, PrModelSemantic_Custom, 0, prMeshFormatFloat32(4)});
    }
}

// Returns the source's mesh of a Mesh prim or an implicit surface with the binding that its ancestors give it, which it
// adds if the source has none: its parts, each part's material, and the box of its points. A mesh whose schema does not
// hold together has no part.
uint32_t internMesh(Reader& r, PrUsdPrimIndex prim, Binding inherited) {
    uint64_t key = (uint64_t(prim) << 32) | (uint64_t(inherited.material + 1) << 1) | (inherited.strong ? 1 : 0);
    uint32_t found = r.meshKeys.find(key);
    if (found != kNone)
        return found;
    r.meshKeys.add(key);
    UsdModelSource* s = r.s;
    UsdMesh mesh{.prim = prim, .firstSubset = s->subsets.count};
    PrModelSourceMesh model{.name = r.prims[prim].name, .firstPart = s->parts.count};
    UsdSurface surface{};
    const PrUsdGeomMesh& geometry = surface.mesh;
    if (surfaceRead(r.stage, prim, s->restTime, r.scratch, &surface) == PrStatus_Ok && geometry.faceCount != 0) {
        uint32_t subsetCount =
            prUsdGeomSubsets(r.stage, prim, geometry.faceCount, "materialBind", s->restTime, r.scratch, nullptr, 0);
        PrUsdGeomSubset* subsets = allocArray<PrUsdGeomSubset>(subsetCount);
        prUsdGeomSubsets(r.stage, prim, geometry.faceCount, "materialBind", s->restTime, r.scratch, subsets,
            subsetCount);
        uint32_t* parts = allocArray<uint32_t>(geometry.faceCount);
        partsOfFaces(geometry, subsets, subsetCount, parts);
        uint32_t* faces = allocArray<uint32_t>(subsetCount + 1);
        for (uint32_t f = 0; f < geometry.faceCount; ++f) {
            if (parts[f] != kNone)
                ++faces[parts[f]];
        }
        PrBox3 box{};
        prMeshBox(PrMeshAttributeView{geometry.points, geometry.pointCount, sizeof(PrVec3), prMeshFormatFloat32(3)}, &box);
        Binding own = combine(inherited, prUsdShadeDirectBinding(r.stage, prim, r.scratch));
        for (uint32_t k = 0; k <= subsetCount; ++k) {
            if (faces[k] == 0)
                continue;
            Binding binding = own;
            if (k < subsetCount) {
                s->subsets.push(subsets[k].prim);
                binding = combine(own, prUsdShadeDirectBinding(r.stage, subsets[k].prim, r.scratch));
            } else {
                mesh.hasRest = true;
            }
            uint32_t material = internMaterial(r, binding.material, geometry.doubleSided);
            PrModelSourcePart part{.material = material, .bounds = box, .firstStream = s->streams.count};
            addStreams(s, s->materials.data[material], part.firstStream);
            part.streamCount = s->streams.count - part.firstStream;
            s->parts.push(part);
        }
        mesh.subsetCount = s->subsets.count - mesh.firstSubset;
        prMemoryFree(subsets);
        prMemoryFree(parts);
        prMemoryFree(faces);
        if (geometry.subdivision != PrUsdGeomSubdivision_None)
            addDiagnostic(s, "subdivision", "a mesh has a subdivision scheme, and its faces are drawn as they are, with "
                "the smooth normals of its points");
    }
    surface.reset();
    model.partCount = s->parts.count - model.firstPart;
    s->usdMeshes.push(mesh);
    s->meshes.push(model);
    return s->meshes.count - 1;
}

// Adds a draw of a source's mesh at a node, with the instances whose transforms within the node `locals` gives, or one
// at the node's own place if it is null.
void addDraw(Reader& r, uint32_t node, uint32_t mesh, const PrMatrix4* PR_NULLABLE locals, uint32_t count) {
    if (r.s->meshes.data[mesh].partCount == 0 || count == 0)
        return;
    PrModelSourceDraw draw{.node = node, .mesh = mesh, .skin = kPrModelSkinNone};
    if (locals != nullptr) {
        draw.firstInstance = r.s->instances.count;
        draw.instanceCount = count;
        r.s->instances.append(locals, count);
    }
    r.s->draws.push(draw);
}

// ---------------------------------------------------------------------------------------------------------------------
// Lights and cameras

// Reads a UsdLux light at `time` as the source's: its kind and shape in meters, its luminance, and its shaping cone.
// Returns false for a dome light, which is the environment, and for a light of a kind that the source has none of,
// which a diagnostic names.
bool readLight(Reader& r, PrUsdPrimIndex prim, double time, PrModelSourceLight* out) {
    PrUsdLuxLight lux{};
    if (!prUsdLuxLightRead(r.stage, prim, time, r.scratch, &lux) || lux.kind == PrUsdLightKind_Dome)
        return false;
    if (lux.kind == PrUsdLightKind_Other) {
        addDiagnostic(r.s, "lights", "a light of a kind that the source has none of, as a GeometryLight or a "
            "PortalLight, lights nothing");
        return false;
    }
    double meters = r.s->stageInfo.metersPerUnit;
    PrModelLightKind kind = PrModelLightKind_Sphere;
    switch (lux.kind) {
        case PrUsdLightKind_Distant:
            kind = PrModelLightKind_Directional;
            break;
        case PrUsdLightKind_Disk:
            kind = PrModelLightKind_Disk;
            break;
        case PrUsdLightKind_Rect:
            kind = PrModelLightKind_Rect;
            break;
        case PrUsdLightKind_Cylinder:
            kind = PrModelLightKind_Cylinder;
            break;
        default:
            break;
    }
    PrModelSourceLight light{
        .name = r.prims[prim].name,
        .kind = kind,
        .unit = PrModelLightUnit_Luminance,
        .color = lux.color,
        .intensity = lux.intensity * prF32Exp2(lux.exposure),
        .range = kPrF32Infinity,
        .innerConeAngle = 0,
        .outerConeAngle = kPi,
        .radius = float(double(lux.radius) * meters),
        .width = float(double(lux.width) * meters),
        .height = float(double(lux.height) * meters),
        .length = float(double(lux.length) * meters),
        .normalized = lux.normalize,
        .treatAsPoint = lux.treatAsPoint,
    };
    if (lux.coneAngle < 90 && kind != PrModelLightKind_Directional) {
        light.outerConeAngle = lux.coneAngle * kPi / 180;
        light.innerConeAngle = light.outerConeAngle * (1 - prF32Clamp(lux.coneSoftness, 0, 1));
    }
    *out = light;
    return true;
}

// Adds the source's light of a UsdLux light at rest, or returns kPrModelLightNone for a light that readLight does not
// read.
uint32_t addLight(Reader& r, PrUsdPrimIndex prim) {
    PrModelSourceLight light{};
    if (!readLight(r, prim, r.s->restTime, &light))
        return kPrModelLightNone;
    r.s->lights.push(light);
    return r.s->lights.count - 1;
}

// Makes a dome light at a node the source's environment. Its radiance is its color times its intensity times 2 to its
// exposure, times its texture's where it has one of the latlong format. A texture of another format is not read, and
// the dome lights evenly.
void readDome(Reader& r, PrUsdPrimIndex prim, uint32_t node) {
    PrUsdLuxLight lux{};
    if (!prUsdLuxLightRead(r.stage, prim, r.s->restTime, r.scratch, &lux) || lux.kind != PrUsdLightKind_Dome)
        return;
    r.environmentRead = true;
    r.dome = prim;
    PrModelSourceEnvironment& environment = r.s->info.environment;
    environment.node = node;
    environment.color = lux.color;
    environment.intensity = lux.intensity * prF32Exp2(lux.exposure);
    // The renderer's equirectangular image has +x at its middle and +z a quarter turn to the right, and OpenEXR's
    // latlong map, which a dome's frame holds, has +z at its middle and -x a quarter turn to the right: the renderer's
    // x is the dome's z, and its z the dome's -x.
    PrMatrix4 frame{{{0, 0, -1, 0}, {0, 1, 0, 0}, {1, 0, 0, 0}, {0, 0, 0, 1}}};
    // A pole along z turns the dome's y to z, and its z to -y.
    bool zUp = lux.poleAxis == PrUsdLuxPoleAxis_Z ||
        (lux.poleAxis == PrUsdLuxPoleAxis_Scene && r.s->stageInfo.upAxis == PrUsdUpAxis_Z);
    PrMatrix4 pole{{{1, 0, 0, 0}, {0, 0, -1, 0}, {0, 1, 0, 0}, {0, 0, 0, 1}}};
    environment.frame = zUp ? prMatrix4Concat(&frame, &pole) : frame;
    bool latlong = lux.textureFormat == PrUsdLuxTextureFormat_Automatic ||
        lux.textureFormat == PrUsdLuxTextureFormat_Latlong;
    PrRawString location = prUsdLuxTextureLocation(r.stage, prim, &lux, r.s->restTime);
    if (prRawStringView(&location).size != 0 && latlong)
        environment.image = internImage(r, prRawStringView(&location));
    else if (prRawStringView(&location).size != 0)
        addDiagnostic(r.s, "environment-format", "a dome light's texture maps directions otherwise than a latitude-"
            "longitude map, which UsdLux does not define, and the dome lights evenly");
    prRawStringReset(&location);
}

// Reads a UsdGeomCamera at `time` as the source's: the field of view that its focal length and vertical aperture make,
// its aspect, and its clipping range in meters. Returns false for a camera that the schema does not read.
bool readCamera(Reader& r, PrUsdPrimIndex prim, double time, PrModelSourceCamera* out) {
    PrUsdGeomCamera camera{};
    if (!prUsdGeomCameraRead(r.stage, prim, time, r.scratch, &camera))
        return false;
    float meters = float(r.s->stageInfo.metersPerUnit);
    PrModelProjection projection = camera.orthographic ? PrModelProjection_Orthographic : PrModelProjection_Perspective;
    *out = PrModelSourceCamera{
        .name = r.prims[prim].name,
        .projection = projection,
        .yfov = 2 * prF32Atan(camera.verticalAperture / (2 * camera.focalLength)),
        .aspectRatio = camera.horizontalAperture / camera.verticalAperture,
        // An orthographic camera's apertures are in tenths of the stage's unit.
        .xmag = camera.horizontalAperture * 0.05f * meters,
        .ymag = camera.verticalAperture * 0.05f * meters,
        .znear = camera.nearClip * meters,
        .zfar = camera.farClip * meters,
    };
    return true;
}

// Adds the source's camera of a UsdGeomCamera at rest, or returns kPrModelCameraNone for one that readCamera does not
// read.
uint32_t addCamera(Reader& r, PrUsdPrimIndex prim) {
    PrModelSourceCamera camera{};
    if (!readCamera(r, prim, r.s->restTime, &camera))
        return kPrModelCameraNone;
    r.s->cameras.push(camera);
    return r.s->cameras.count - 1;
}

// ---------------------------------------------------------------------------------------------------------------------
// The walk

// Pushes the children of a prim to `stack`, in reverse so that they are visited in their order: an instance's are its
// prototype's.
void pushChildren(Reader& r, RawArray<Visit>* stack, PrUsdPrimIndex prim, uint32_t parent, Binding binding,
        PrUsdGeomPurpose purpose) {
    const PrUsdPrim& p = r.prims[prim];
    PrUsdPrimIndex first = p.prototype != kPrUsdNone ? r.prims[p.prototype].firstChild : p.firstChild;
    uint32_t begin = stack->count;
    for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = r.prims[child].nextSibling)
        stack->push(Visit{child, parent, binding, purpose});
    for (uint32_t a = begin, b = stack->count; a + 1 < b; ++a, --b) {
        Visit swap = stack->data[a];
        stack->data[a] = stack->data[b - 1];
        stack->data[b - 1] = swap;
    }
}

// Returns the purpose that a prim has: its own, or else what it inherits.
PrUsdGeomPurpose purposeOf(Reader& r, PrUsdPrimIndex prim, PrUsdGeomPurpose inherited) {
    bool authored = false;
    PrUsdGeomPurpose purpose = prUsdGeomPurpose(r.stage, prim, r.scratch, &authored);
    return authored ? purpose : inherited;
}

// Returns true if the context draws a prim of `purpose`.
bool drawsPurpose(const Reader& r, PrUsdGeomPurpose purpose) {
    const PrUsdModelContext& context = r.options.context;
    switch (purpose) {
        case PrUsdGeomPurpose_Render:
            return !context.proxy;
        case PrUsdGeomPurpose_Proxy:
            return context.proxy;
        case PrUsdGeomPurpose_Guide:
            return context.guides;
        default:
            return true;
    }
}

// Adds the draws of a point instancer's prototypes at its node: each mesh under each prototype, at every visible
// instance of that prototype, through the transforms from the prototype's root to the mesh at rest.
void addInstancer(Reader& r, uint32_t node, PrUsdPrimIndex prim, Binding binding) {
    PrUsdGeomPointInstancer instancer{};
    double rest = r.s->restTime;
    if (prUsdGeomPointInstancerRead(r.stage, prim, rest, r.scratch, &instancer) != PrStatus_Ok)
        return;
    // The instancer's own arrays live in the scratch, which the walk below reads into too, so they are copied.
    uint32_t instanceCount = instancer.instanceCount;
    PrMatrix4* placed = allocArray<PrMatrix4>(instanceCount);
    int32_t* protoIndex = allocArray<int32_t>(instanceCount);
    for (uint32_t k = 0; k < instanceCount; ++k) {
        PrDMatrix4 m = prUsdGeomInstanceTransform(&instancer, k);
        placed[k] = matrixOf(m);
        protoIndex[k] = prUsdGeomInstanceIsVisible(&instancer, k) ? instancer.protoIndices[k] : -1;
    }
    uint32_t prototypeCount = instancer.prototypeCount;
    PrUsdPrimIndex* prototypes = allocArray<PrUsdPrimIndex>(prototypeCount);
    prMemCopy(prototypes, instancer.prototypes, size_t(prototypeCount) * sizeof(PrUsdPrimIndex));
    // A prim under a prototype, with the transform from the instancer to it and what it binds.
    struct Inner {
        PrUsdPrimIndex prim;
        PrMatrix4 within;
        Binding binding;
        PrUsdGeomPurpose purpose;
    };
    RawArray<Inner> stack{};
    RawArray<PrMatrix4> locals{};
    for (uint32_t p = 0; p < prototypeCount; ++p) {
        if (prototypes[p] == kPrUsdNone)
            continue;
        stack.push(Inner{prototypes[p], prMatrix4Identity(), binding, PrUsdGeomPurpose_Default});
        while (stack.count != 0) {
            Inner at = stack.last();
            stack.pop();
            const PrUsdPrim& record = r.prims[at.prim];
            PrUsdGeomKind kind = prUsdGeomPrimKind(r.stage, at.prim);
            PrUsdGeomPurpose purpose = purposeOf(r, at.prim, at.purpose);
            if (!walks(record) || kind == PrUsdGeomKind_Material || kind == PrUsdGeomKind_Shader ||
                    !drawsPurpose(r, purpose) || prUsdGeomIsInvisible(r.stage, at.prim, rest, r.scratch))
                continue;
            PrMatrix4 within = at.within;
            if (prUsdGeomKindIsXformable(kind)) {
                PrDMatrix4 local{};
                bool resets = false;
                prUsdGeomLocalTransform(r.stage, at.prim, rest, r.scratch, &local, &resets);
                PrMatrix4 single = matrixOf(local);
                within = resets ? single : prMatrix4Concat(&single, &within);
            }
            if (kind == PrUsdGeomKind_Mesh || kind == PrUsdGeomKind_Shape) {
                uint32_t mesh = internMesh(r, at.prim, at.binding);
                locals.clear();
                for (uint32_t k = 0; k < instanceCount; ++k) {
                    if (protoIndex[k] == int32_t(p))
                        locals.push(prMatrix4Concat(&within, &placed[k]));
                }
                addDraw(r, node, mesh, locals.data, locals.count);
            }
            Binding inner = combine(at.binding, prUsdShadeDirectBinding(r.stage, at.prim, r.scratch));
            PrUsdPrimIndex first =
                record.prototype != kPrUsdNone ? r.prims[record.prototype].firstChild : record.firstChild;
            uint32_t begin = stack.count;
            for (PrUsdPrimIndex child = first; child != kPrUsdNone; child = r.prims[child].nextSibling)
                stack.push(Inner{child, within, inner, purpose});
            for (uint32_t a = begin, b = stack.count; a + 1 < b; ++a, --b) {
                Inner swap = stack.data[a];
                stack.data[a] = stack.data[b - 1];
                stack.data[b - 1] = swap;
            }
        }
    }
    stack.reset();
    locals.reset();
    prMemoryFree(placed);
    prMemoryFree(protoIndex);
    prMemoryFree(prototypes);
}

// Reads the stage's prims into nodes, depth first, each after its parent.
void readNodes(Reader& r) {
    UsdModelSource* s = r.s;
    RawArray<Visit> stack{};
    pushChildren(r, &stack, kPrUsdPseudoRoot, kNone, Binding{kPrUsdNone, false}, PrUsdGeomPurpose_Default);
    while (stack.count != 0) {
        Visit at = stack.last();
        stack.pop();
        const PrUsdPrim& record = r.prims[at.prim];
        PrUsdGeomKind kind = prUsdGeomPrimKind(r.stage, at.prim);
        if (!walks(record) || kind == PrUsdGeomKind_Material || kind == PrUsdGeomKind_Shader ||
                kind == PrUsdGeomKind_Subset)
            continue;
        PrUsdGeomPurpose purpose = purposeOf(r, at.prim, at.purpose);
        if (!drawsPurpose(r, purpose))
            continue;
        if (record.typeName == "SkelRoot" || record.typeName == "Skeleton" || record.typeName == "BlendShape")
            addDiagnostic(s, "skeletons", "UsdSkel's skeletons, their animation and blend shapes are not read, and the "
                "meshes that they bind draw as they are authored");
        uint32_t index = s->nodes.count;
        bool hidden = prUsdGeomIsInvisible(r.stage, at.prim, s->restTime, r.scratch);
        PrModelSourceNode node{
            .name = record.name,
            .parent = at.parent,
            .flags = PrModelNodeFlags(hidden ? PrModelNodeFlag_Hidden : 0),
            .rotation = {0, 0, 0, 1},
            .scale = {1, 1, 1},
            .matrix = prMatrix4Identity(),
            .camera = kPrModelCameraNone,
            .light = kPrModelLightNone,
        };
        UsdNode usd{.prim = at.prim, .firstChild = kNone, .nextSibling = kNone};
        bool resets = false;
        if (prUsdGeomKindIsXformable(kind)) {
            PrDMatrix4 local{};
            prUsdGeomLocalTransform(r.stage, at.prim, s->restTime, r.scratch, &local, &resets);
            node.matrix = matrixOf(local);
            usd.animated = prUsdGeomTransformVaries(r.stage, at.prim, r.scratch);
        }
        // A root's transform, and one that resets the stack, maps to the stage, which maps to the source's scene.
        if (resets)
            node.flags |= PrModelNodeFlag_ResetsTransform;
        if (resets || at.parent == kNone)
            node.matrix = prMatrix4Concat(&node.matrix, &s->stageToScene);
        uint32_t* last = at.parent != kNone ? &r.lastChild.data[at.parent] : &r.lastRoot;
        if (*last != kNone)
            s->usdNodes.data[*last].nextSibling = index;
        else if (at.parent != kNone)
            s->usdNodes.data[at.parent].firstChild = index;
        *last = index;
        if (at.parent == kNone)
            s->roots.push(index);
        s->nodes.push(node);
        s->usdNodes.push(usd);
        r.lastChild.push(kNone);
        Binding binding = combine(at.binding, prUsdShadeDirectBinding(r.stage, at.prim, r.scratch));
        if (kind == PrUsdGeomKind_Mesh || kind == PrUsdGeomKind_Shape)
            addDraw(r, index, internMesh(r, at.prim, at.binding), nullptr, 1);
        else if (kind == PrUsdGeomKind_Gprim)
            addDiagnostic(s, "primitives", "a geometric primitive that is neither a mesh nor an implicit surface, as "
                "Points or BasisCurves, draws nothing");
        if (kind == PrUsdGeomKind_Camera)
            s->nodes.data[index].camera = addCamera(r, at.prim);
        if (kind == PrUsdGeomKind_Light && !r.environmentRead)
            readDome(r, at.prim, index);
        if (kind == PrUsdGeomKind_Light)
            s->nodes.data[index].light = addLight(r, at.prim);
        // An instancer draws its prototypes, which are usually prims under it that draw nothing of their own.
        if (kind == PrUsdGeomKind_PointInstancer) {
            r.instancers.push(at.prim);
            addInstancer(r, index, at.prim, binding);
            continue;
        }
        pushChildren(r, &stack, at.prim, index, binding, purpose);
    }
    stack.reset();
    // Each node's children, in their order.
    for (uint32_t i = 0; i < s->nodes.count; ++i) {
        s->nodes.data[i].firstChild = s->children.count;
        for (uint32_t child = s->usdNodes.data[i].firstChild; child != kNone; child =
                s->usdNodes.data[child].nextSibling)
            s->children.push(child);
        s->nodes.data[i].childCount = s->children.count - s->nodes.data[i].firstChild;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Animation

// Adds to `times` the times of the samples, from the context's start to its end, of each attribute of a prim that
// varies and whose name `admits` accepts. Returns true if any such attribute varies.
template <typename Admits>
bool attributeTimes(Reader& r, PrUsdPrimIndex prim, Admits admits, RawArray<double>* times) {
    const PrUsdPrim& record = r.prims[prim];
    const PrUsdProperty* properties = prUsdStageProperties(r.stage);
    bool varies = false;
    for (uint32_t i = 0; i < record.propertyCount; ++i) {
        uint32_t property = record.firstProperty + i;
        const PrUsdProperty& p = properties[property];
        if (p.form != PrUsdSpecForm_Attribute || !admits(p.name) || !prUsdStageAttributeVaries(r.stage, prim, property))
            continue;
        varies = true;
        const double* sampled = nullptr;
        uint32_t count = prUsdStageAttributeTimes(r.stage, prim, property, r.s->restTime, r.s->endTime, r.scratch, &sampled);
        times->append(sampled, count);
    }
    return varies;
}

// Returns true for the name of an attribute that a transform reads.
bool isTransformName(PrStringView name) {
    return prStringStartsWith(name, "xformOp:") || name == "xformOpOrder";
}

// Returns true for the name of an attribute that no transform or visibility reads: one of a prim's own schema.
bool isOwnName(PrStringView name) {
    return !isTransformName(name) && name != "visibility";
}

// Adds to `times` the times of the samples of the attributes of a material's prim and of the prims under it, which are
// its network. Returns true if any varies.
bool networkTimes(Reader& r, PrUsdPrimIndex material, RawArray<double>* times) {
    bool varies = false;
    for (PrUsdPrimIndex prim = material; prim < r.prims[material].end; ++prim)
        varies = attributeTimes(r, prim, [](PrStringView) { return true; }, times) || varies;
    return varies;
}

// Makes `times` the keys of a track: its times with the context's start and end, sorted and each once, and the time
// codes between each two of them, so that a value that is not linear in its attributes, as a transform's operations or
// a field of view, is keyed where a frame reads it. A track has at most kMaxSamples keys.
void keyTimesOf(const UsdModelSource* s, RawArray<double>* times) {
    times->push(s->restTime);
    times->push(s->endTime);
    sortItems(times->span(), [](double a, double b) { return a < b; });
    RawArray<double> keys{};
    for (uint32_t i = 0; i < times->count; ++i) {
        double t = times->data[i];
        if (t < s->restTime || t > s->endTime || (keys.count != 0 && keys.last() == t))
            continue;
        if (keys.count != 0) {
            for (double between = prF64Floor(keys.last()) + 1; between < t && keys.count < kMaxSamples; between += 1)
                keys.push(between);
        }
        if (keys.count < kMaxSamples)
            keys.push(t);
    }
    times->reset();
    *times = keys;
}

// Adds a track of `width` floats a key, one key at each of `times`, of `values`, if a key differs from the first.
void addTrack(UsdModelSource* s, PrModelTrackTarget target, PrModelInterpolation interpolation, uint32_t object,
        uint32_t element, uint32_t width, const RawArray<double>& times, const float* values) {
    bool varies = false;
    for (uint32_t k = 1; k < times.count && !varies; ++k)
        varies = !prMemEquals(values, values + size_t(k) * width, width * sizeof(float));
    if (!varies)
        return;
    s->tracks.push(PrModelSourceTrack{
        .target = target,
        .interpolation = interpolation,
        .object = object,
        .element = element,
        .width = width,
        .firstKey = s->keyTimes.count,
        .keyCount = times.count,
        .firstValue = s->keyValues.count,
    });
    for (uint32_t k = 0; k < times.count; ++k)
        s->keyTimes.push(float((times.data[k] - s->restTime) / s->perSecond));
    s->keyValues.append(values, times.count * width);
}

// Adds the track of a node's local transform: its matrix at each key, mapped to the scene at a root.
void animateTransform(Reader& r, uint32_t index, RawArray<double>* times, RawArray<float>* values) {
    UsdModelSource* s = r.s;
    const UsdNode& node = s->usdNodes.data[index];
    uint32_t count = prUsdGeomTransformTimes(r.stage, node.prim, s->restTime, s->endTime, r.scratch, nullptr, 0);
    times->resize(count);
    prUsdGeomTransformTimes(r.stage, node.prim, s->restTime, s->endTime, r.scratch, times->data, count);
    keyTimesOf(s, times);
    const PrModelSourceNode& described = s->nodes.data[index];
    bool root = described.parent == kNone || (described.flags & PrModelNodeFlag_ResetsTransform) != 0;
    values->clear();
    for (uint32_t k = 0; k < times->count; ++k) {
        PrDMatrix4 local{};
        bool resets = false;
        prUsdGeomLocalTransform(r.stage, node.prim, times->data[k], r.scratch, &local, &resets);
        PrMatrix4 matrix = matrixOf(local);
        if (root)
            matrix = prMatrix4Concat(&matrix, &s->stageToScene);
        values->append(&matrix.m[0][0], 16);
        prUsdScratchClear(r.scratch);
    }
    addTrack(s, PrModelTrackTarget_Matrix, PrModelInterpolation_Linear, index, 0, 16, *times, values->data);
}

// Adds the track of a node's visibility, which holds from each key to the next.
void animateVisibility(Reader& r, uint32_t index, RawArray<double>* times, RawArray<float>* values) {
    UsdModelSource* s = r.s;
    keyTimesOf(s, times);
    values->clear();
    for (uint32_t k = 0; k < times->count; ++k)
        values->push(prUsdGeomIsInvisible(r.stage, s->usdNodes.data[index].prim, times->data[k], r.scratch) ? 0 : 1);
    addTrack(s, PrModelTrackTarget_Visible, PrModelInterpolation_Step, index, 0, 1, *times, values->data);
}

// Adds the tracks of a light's color, intensity and cone. A light whose kind or shape varies keeps them at rest.
void animateLight(Reader& r, PrUsdPrimIndex prim, uint32_t light, RawArray<double>* times, RawArray<float>* values) {
    UsdModelSource* s = r.s;
    keyTimesOf(s, times);
    const PrModelSourceLight rest = s->lights.data[light];
    uint32_t count = times->count;
    // By key: the color, the intensity, and the inner and outer angles of the cone.
    values->resize(count * 6);
    bool shaped = true;
    for (uint32_t k = 0; k < count; ++k) {
        PrModelSourceLight at = rest;
        readLight(r, prim, times->data[k], &at);
        shaped = shaped && at.kind == rest.kind && at.radius == rest.radius && at.width == rest.width &&
            at.height == rest.height && at.length == rest.length && at.normalized == rest.normalized &&
            at.treatAsPoint == rest.treatAsPoint;
        float* key = values->data + size_t(k) * 6;
        key[0] = at.color.x;
        key[1] = at.color.y;
        key[2] = at.color.z;
        key[3] = at.intensity;
        key[4] = at.innerConeAngle;
        key[5] = at.outerConeAngle;
        prUsdScratchClear(r.scratch);
    }
    if (!shaped)
        addDiagnostic(s, "light-animation", "a light's kind or shape varies over time, and the light keeps the kind and "
            "the shape that it has at the start");
    const PrModelTrackTarget targets[] = {
        PrModelTrackTarget_LightColor, PrModelTrackTarget_LightIntensity, PrModelTrackTarget_LightInnerConeAngle,
        PrModelTrackTarget_LightOuterConeAngle,
    };
    const uint32_t widths[] = {3, 1, 1, 1};
    RawArray<float> track{};
    for (uint32_t t = 0, first = 0; t < 4; first += widths[t], ++t) {
        track.clear();
        for (uint32_t k = 0; k < count; ++k)
            track.append(values->data + size_t(k) * 6 + first, widths[t]);
        addTrack(s, targets[t], PrModelInterpolation_Linear, light, 0, widths[t], *times, track.data);
    }
    track.reset();
}

// Adds the tracks of a camera's projection. A camera whose kind of projection varies keeps its kind at rest.
void animateCamera(Reader& r, PrUsdPrimIndex prim, uint32_t camera, RawArray<double>* times, RawArray<float>* values) {
    UsdModelSource* s = r.s;
    keyTimesOf(s, times);
    const PrModelSourceCamera rest = s->cameras.data[camera];
    uint32_t count = times->count;
    // By key: the field of view, the aspect, the magnifications and the clipping distances.
    values->resize(count * 6);
    for (uint32_t k = 0; k < count; ++k) {
        PrModelSourceCamera at = rest;
        readCamera(r, prim, times->data[k], &at);
        if (at.projection != rest.projection)
            addDiagnostic(s, "camera-animation", "a camera's projection varies between perspective and orthographic "
                "over time, and the camera keeps the projection that it has at the start");
        float* key = values->data + size_t(k) * 6;
        key[0] = at.yfov;
        key[1] = at.aspectRatio;
        key[2] = at.xmag;
        key[3] = at.ymag;
        key[4] = at.znear;
        key[5] = at.zfar;
        prUsdScratchClear(r.scratch);
    }
    const PrModelTrackTarget targets[] = {
        PrModelTrackTarget_CameraYfov, PrModelTrackTarget_CameraAspectRatio, PrModelTrackTarget_CameraXmag,
        PrModelTrackTarget_CameraYmag, PrModelTrackTarget_CameraZnear, PrModelTrackTarget_CameraZfar,
    };
    RawArray<float> track{};
    for (uint32_t t = 0; t < 6; ++t) {
        track.clear();
        for (uint32_t k = 0; k < count; ++k)
            track.push(values->data[size_t(k) * 6 + t]);
        addTrack(s, targets[t], PrModelInterpolation_Linear, camera, 0, 1, *times, track.data);
    }
    track.reset();
}

// Returns true if two inputs read alike, apart from the value of an input of a value.
bool readsAlike(const PrModelSourceInput& a, const PrModelSourceInput& b) {
    if (a.kind != b.kind || a.width != b.width)
        return false;
    if (a.kind == PrModelInputKind_Value)
        return true;
    return prMemEquals(a.value, b.value, sizeof(a.value)) &&
        prMemEquals(&a.transform, &b.transform, sizeof(a.transform)) &&
        prMemEquals(&a.channels, &b.channels, sizeof(a.channels)) && a.attribute == b.attribute && a.tint == b.tint;
}

// Adds the tracks of the values of a material's inputs, read at each key. A material whose textures, primitive
// variables or alpha vary keeps them at rest, and its values too.
void animateMaterial(Reader& r, uint32_t index, RawArray<double>* times, RawArray<float>* values) {
    UsdModelSource* s = r.s;
    keyTimesOf(s, times);
    const PrModelSourceMaterial material = s->materials.data[index];
    PrUsdPrimIndex prim = PrUsdPrimIndex(r.materialKeys.keys.data[index] >> 1);
    uint32_t count = times->count;
    // By input, then by key: its value, four floats.
    values->resize(material.inputCount * count * 4);
    RawArray<PrModelSourceInput> inputs{};
    bool alike = true;
    r.sampling = true;
    for (uint32_t k = 0; k < count && alike; ++k) {
        inputs.clear();
        PrModelShading shading = PrModelShading_Preview;
        bool blends = false;
        readMaterial(r, prim, times->data[k], &inputs, &shading, &blends);
        alike = inputs.count == material.inputCount && shading == material.shading &&
            (blends ? PrModelAlpha_Blend : PrModelAlpha_Opaque) == material.alpha;
        for (uint32_t i = 0; alike && i < material.inputCount; ++i) {
            alike = readsAlike(inputs.data[i], s->inputs.data[material.firstInput + i]);
            prMemCopy(values->data + (size_t(i) * count + k) * 4, inputs.data[i].value, 4 * sizeof(float));
        }
        prUsdScratchClear(r.scratch);
    }
    r.sampling = false;
    inputs.reset();
    if (!alike) {
        addDiagnostic(s, "material-animation", "a material's textures, primitive variables or alpha vary over time, and "
            "the material is drawn as it is at the start");
        return;
    }
    for (uint32_t i = 0; i < material.inputCount; ++i) {
        const PrModelSourceInput& input = s->inputs.data[material.firstInput + i];
        if (input.kind != PrModelInputKind_Value)
            continue;
        // The keys of a track are its input's width apart.
        float* key = values->data + size_t(i) * count * 4;
        for (uint32_t k = 0; k < count; ++k)
            prMemMove(key + size_t(k) * input.width, key + size_t(k) * 4, input.width * sizeof(float));
        addTrack(s, PrModelTrackTarget_Input, PrModelInterpolation_Linear, index, i, input.width, *times, key);
    }
}

// Reifies the context's time codes as one animation, of a track of each value that varies and that a track holds: a
// node's transform and visibility, a light's color, intensity and cone, a camera's projection, and the value of a
// material's input. A value that varies and that no track holds is described at rest, and a diagnostic names it.
void readAnimation(Reader& r) {
    UsdModelSource* s = r.s;
    bool animates = s->endTime > s->restTime;
    PrModelSourceAnimation animation{
        .name = "Time codes",
        .duration = float((s->endTime - s->restTime) / s->perSecond),
        .firstTrack = s->tracks.count,
    };
    RawArray<double> times{};
    RawArray<float> values{};
    bool varies = false;
    for (uint32_t i = 0; i < s->usdNodes.count; ++i) {
        const UsdNode& node = s->usdNodes.data[i];
        const PrModelSourceNode& described = s->nodes.data[i];
        varies = varies || node.animated;
        if (animates && node.animated)
            animateTransform(r, i, &times, &values);
        times.clear();
        if (attributeTimes(r, node.prim, [](PrStringView name) { return name == "visibility"; }, &times)) {
            varies = true;
            if (animates)
                animateVisibility(r, i, &times, &values);
        }
        times.clear();
        if (described.light != kPrModelLightNone && attributeTimes(r, node.prim, isOwnName, &times)) {
            varies = true;
            if (animates)
                animateLight(r, node.prim, described.light, &times, &values);
        }
        times.clear();
        if (described.camera != kPrModelCameraNone && attributeTimes(r, node.prim, isOwnName, &times)) {
            varies = true;
            if (animates)
                animateCamera(r, node.prim, described.camera, &times, &values);
        }
        times.clear();
        prUsdScratchClear(r.scratch);
    }
    for (uint32_t i = 0; i < s->materials.count; ++i) {
        PrUsdPrimIndex prim = PrUsdPrimIndex(r.materialKeys.keys.data[i] >> 1);
        times.clear();
        if (prim == kPrUsdNone || !networkTimes(r, prim, &times))
            continue;
        varies = true;
        if (animates)
            animateMaterial(r, i, &times, &values);
    }
    // What varies of what no track holds: a mesh's points, normals, primitive variables, shape or subsets, an
    // instancer's instances, and the environment.
    bool meshes = false;
    for (uint32_t i = 0; i < s->usdMeshes.count && !meshes; ++i) {
        const UsdMesh& mesh = s->usdMeshes.data[i];
        meshes = attributeTimes(r, mesh.prim, isOwnName, &times);
        for (uint32_t k = 0; k < mesh.subsetCount && !meshes; ++k)
            meshes = attributeTimes(r, s->subsets.data[mesh.firstSubset + k], isOwnName, &times);
    }
    bool instancers = false;
    for (uint32_t i = 0; i < r.instancers.count && !instancers; ++i)
        instancers = attributeTimes(r, r.instancers.data[i], isOwnName, &times);
    bool environment = r.dome != kPrUsdNone && attributeTimes(r, r.dome, isOwnName, &times);
    times.reset();
    values.reset();
    prUsdScratchClear(r.scratch);
    varies = varies || meshes || instancers || environment;
    if (!animates) {
        if (varies)
            addDiagnostic(s, "time-range", "values of the stage vary over time, and the context spans no time codes, so "
                "the source shows them at its start");
        return;
    }
    if (meshes)
        addDiagnostic(s, "mesh-animation", "a mesh's points, normals, primitive variables, shape or subsets vary over "
            "time, and the mesh is drawn as it is at the start");
    if (instancers)
        addDiagnostic(s, "instancer-animation", "a point instancer's instances vary over time, and are drawn as they "
            "are at the start");
    if (environment)
        addDiagnostic(s, "environment-animation", "the dome light that is the environment varies over time, and lights "
            "the scene as it does at the start");
    animation.trackCount = s->tracks.count - animation.firstTrack;
    if (animation.trackCount != 0)
        s->animations.push(animation);
}

// Reports each variant selection of the context that the stage does not make.
void checkSelections(Reader& r) {
    const PrUsdModelContext& context = r.options.context;
    for (uint32_t i = 0; i < context.variantSelectionCount; ++i) {
        const PrUsdPrimVariantSelection& selection = context.variantSelections[i];
        PrUsdPrimIndex prim = prUsdStageFindProxy(r.stage, selection.prim);
        PrStringView made{};
        if (prim != kPrUsdNone)
            made = prUsdStagePrimVariantSelection(r.stage, prim, selection.variantSet);
        if (made != selection.variant)
            addDiagnostic(r.s, "variant-selections", "the stage makes another variant selection than the context's, "
                "and the source shows the stage's");
    }
}

// Reifies the stage in the options' context into the source's description.
void describe(UsdModelSource* s, const PrUsdModelOptions& options) {
    const PrUsdStage* stage = s->stage;
    const PrUsdModelContext& context = options.context;
    s->stageInfo = prUsdGeomStageInfo(stage);
    s->restTime = context.hasTimeRange ? context.startTimeCode : s->stageInfo.startTimeCode;
    s->endTime = context.hasTimeRange ? context.endTimeCode : s->stageInfo.endTimeCode;
    if (!(s->endTime > s->restTime))
        s->endTime = s->restTime;
    s->perSecond = s->stageInfo.timeCodesPerSecond > 0 ? s->stageInfo.timeCodesPerSecond : 24;
    // The stage's unit in meters, with its up axis turned to +y: +z up becomes +y, and +y becomes -z.
    float m = float(s->stageInfo.metersPerUnit);
    s->stageToScene = PrMatrix4{{{m, 0, 0, 0}, {0, m, 0, 0}, {0, 0, m, 0}, {0, 0, 0, 1}}};
    if (s->stageInfo.upAxis == PrUsdUpAxis_Z)
        s->stageToScene = PrMatrix4{{{m, 0, 0, 0}, {0, 0, m, 0}, {0, -m, 0, 0}, {0, 0, 0, 1}}};
    s->info.environment = PrModelSourceEnvironment{.node = kPrModelNodeNone, .image = kPrModelImageNone};
    Reader r{
        .s = s,
        .stage = stage,
        .prims = prUsdStagePrims(stage),
        .scratch = prUsdScratchCreate(),
        .options = options,
        .lastRoot = kNone,
        .dome = kPrUsdNone,
    };
    checkSelections(r);
    readNodes(r);
    readAnimation(r);
    for (uint32_t i = 0; i < s->locations.count; ++i) {
        PrStringView location = prRawStringView(&s->locations.data[i]);
        s->images.push(PrModelSourceImage{.name = location, .location = location});
    }
    prUsdScratchDestroy(r.scratch);
    r.meshKeys.reset();
    r.materialKeys.reset();
    r.imageKeys.reset();
    r.lastChild.reset();
    r.instancers.reset();
    PrModelSourceEnvironment environment = s->info.environment;
    s->info = PrModelSourceInfo{
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
        .cameras = s->cameras.data,
        .cameraCount = s->cameras.count,
        .lights = s->lights.data,
        .lightCount = s->lights.count,
        .environment = environment,
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
// Primitive variables

// The kinds of number of a primitive variable's components.
enum {
    PrimvarNumber_None,
    PrimvarNumber_Float,
    PrimvarNumber_Double,
    PrimvarNumber_Half,
};
typedef uint8_t PrimvarNumber;

// Returns the number of components of a value of `type`, a scalar or a vector of floats, doubles or halves, and their
// kind in `*number`, or zero for a type of another kind.
uint32_t componentsOf(PrUsdType type, PrimvarNumber* number) {
    struct Shape {
        PrUsdType type;
        uint32_t components;
        PrimvarNumber number;
    };
    constexpr Shape kShapes[] = {
        {PrUsdType_Float, 1, PrimvarNumber_Float}, {PrUsdType_Float2, 2, PrimvarNumber_Float},
        {PrUsdType_Float3, 3, PrimvarNumber_Float}, {PrUsdType_Float4, 4, PrimvarNumber_Float},
        {PrUsdType_Double, 1, PrimvarNumber_Double}, {PrUsdType_Double2, 2, PrimvarNumber_Double},
        {PrUsdType_Double3, 3, PrimvarNumber_Double}, {PrUsdType_Double4, 4, PrimvarNumber_Double},
        {PrUsdType_Half, 1, PrimvarNumber_Half}, {PrUsdType_Half2, 2, PrimvarNumber_Half},
        {PrUsdType_Half3, 3, PrimvarNumber_Half}, {PrUsdType_Half4, 4, PrimvarNumber_Half},
    };
    for (const Shape& shape : kShapes) {
        if (shape.type == type) {
            *number = shape.number;
            return shape.components;
        }
    }
    *number = PrimvarNumber_None;
    return 0;
}

// Reads `width` floats of a value of a primitive variable, of a type of floats, doubles or halves. The components that
// the variable lacks are left as they are. Returns false for a variable of another type, or an element that it lacks.
bool primvarFloats(const PrUsdGeomPrimvar& primvar, uint32_t width, uint32_t element, float* out) {
    uint32_t at = primvar.indices != nullptr ? uint32_t(primvar.indices[element]) : element;
    at *= primvar.elementSize;
    PrimvarNumber number = PrimvarNumber_None;
    uint32_t components = componentsOf(primvar.value.type, &number);
    if (at >= primvar.value.count || components == 0)
        return false;
    for (uint32_t i = 0; i < width && i < components; ++i) {
        size_t k = size_t(at) * components + i;
        if (number == PrimvarNumber_Float)
            out[i] = static_cast<const float*>(primvar.value.data)[k];
        else if (number == PrimvarNumber_Double)
            out[i] = float(static_cast<const double*>(primvar.value.data)[k]);
        else
            out[i] = prFloat16ToFloat(static_cast<const PrFloat16*>(primvar.value.data)[k]);
    }
    return true;
}

// Returns the element of a primitive variable that the corner `corner`, of the face `face` at the point `point`, reads.
uint32_t elementOf(PrUsdGeomInterpolation interpolation, uint32_t face, uint32_t corner, uint32_t point) {
    switch (interpolation) {
        case PrUsdGeomInterpolation_Constant:
            return 0;
        case PrUsdGeomInterpolation_Uniform:
            return face;
        case PrUsdGeomInterpolation_FaceVarying:
            return corner;
        default:
            return point;
    }
}

// Writes `width` floats of a primitive variable for each corner of a mesh to `out`, or `fallback` where it has none.
void cornerValues(const PrUsdGeomMesh& mesh, const PrUsdGeomPrimvar* PR_NULLABLE primvar, const uint32_t* faceOf,
        uint32_t width, const float* fallback, float* out) {
    bool fits = primvar != nullptr && prUsdGeomPrimvarFits(&mesh, primvar);
    for (uint32_t c = 0; c < mesh.cornerCount; ++c) {
        float* value = out + size_t(c) * width;
        uint32_t element = fits ? elementOf(primvar->interpolation, faceOf[c], c, uint32_t(mesh.faceVertexIndices[c])) : 0;
        prMemCopy(value, fallback, width * sizeof(float));
        if (fits)
            primvarFloats(*primvar, width, element, value);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The loaders

void sourceDestroy(PrModelSource* source) {
    UsdModelSource* s = static_cast<UsdModelSource*>(source);
    for (uint32_t i = 0; i < s->locations.count; ++i)
        prRawStringReset(&s->locations.data[i]);
    for (uint32_t i = 0; i < s->imageBytes.count; ++i)
        prBlobSourceRelease(s->imageBytes.data[i]);
    s->usdNodes.reset();
    s->usdMeshes.reset();
    s->subsets.reset();
    s->nodes.reset();
    s->roots.reset();
    s->children.reset();
    s->draws.reset();
    s->instances.reset();
    s->meshes.reset();
    s->parts.reset();
    s->streams.reset();
    s->materials.reset();
    s->inputs.reset();
    s->textures.reset();
    s->textureImages.reset();
    s->images.reset();
    s->locations.reset();
    s->imageBytes.reset();
    s->cameras.reset();
    s->lights.reset();
    s->animations.reset();
    s->tracks.reset();
    s->keyTimes.reset();
    s->keyValues.reset();
    s->diagnostics.reset();
    prUsdStageDestroy(s->owned);
    prMemoryFree(s);
}

const PrModelSourceInfo* sourceInfo(const PrModelSource* source) {
    return &static_cast<const UsdModelSource*>(source)->info;
}

// Reads a mesh at the rest time: a vertex for each corner of its faces, with what each requested primitive variable
// gives the corner, its faces triangulated, and its parts the triangles of its subsets, then of the faces that no
// subset holds.
PrStatus meshRead(const PrModelSource* source, PrModelMeshIndex index, const PrModelMeshRequest* request,
        PrModelMeshData* out) {
    const UsdModelSource* s = static_cast<const UsdModelSource*>(source);
    const UsdMesh& mesh = s->usdMeshes.data[index];
    const PrModelSourceMesh& model = s->meshes.data[index];
    PrUsdScratch* scratch = prUsdScratchCreate();
    UsdSurface surface{};
    const PrUsdGeomMesh& geometry = surface.mesh;
    PrStatus status = surfaceRead(s->stage, mesh.prim, s->restTime, scratch, &surface);
    if (status != PrStatus_Ok) {
        surface.reset();
        prUsdScratchDestroy(scratch);
        return PrStatus_InvalidData;
    }
    uint32_t faceCount = geometry.faceCount;
    uint32_t cornerCount = geometry.cornerCount;
    // The schema's reading checked that each count is at least three and each corner names a point.
    const uint32_t* counts = reinterpret_cast<const uint32_t*>(geometry.faceVertexCounts);
    PrMeshIndexView corners{geometry.faceVertexIndices, cornerCount, PrMeshIndexType_U32};
    PrMeshAttributeView points{geometry.points, geometry.pointCount, sizeof(PrVec3), prMeshFormatFloat32(3)};
    uint32_t triangleCount = prMeshPolygonTriangleCount(counts, faceCount);
    uint32_t* triangles = allocArray<uint32_t>(triangleCount * 3);
    uint32_t* faceOfTriangle = allocArray<uint32_t>(triangleCount);
    uint32_t* faceOf = allocArray<uint32_t>(cornerCount);
    for (uint32_t f = 0, c = 0; f < faceCount; ++f) {
        for (uint32_t k = 0; k < counts[f]; ++k)
            faceOf[c++] = f;
    }
    status = prMeshPolygonsTriangulate(points, corners, counts, faceCount, triangles, faceOfTriangle);
    // A left-handed mesh's faces wind clockwise seen from their front, and the renderer's counterclockwise.
    for (uint32_t t = 0; geometry.leftHanded && t < triangleCount; ++t) {
        uint32_t swap = triangles[t * 3 + 1];
        triangles[t * 3 + 1] = triangles[t * 3 + 2];
        triangles[t * 3 + 2] = swap;
    }
    out->vertexCount = cornerCount;
    out->positions = allocArray<float>(cornerCount * 3);
    out->normals = allocArray<float>(cornerCount * 3);
    for (uint32_t c = 0; c < cornerCount; ++c)
        prMemCopy(out->positions + c * 3, &geometry.points[geometry.faceVertexIndices[c]], 3 * sizeof(float));
    // A mesh that a scheme subdivides has the normals of its smooth surface, which the scheme decides, and the stage's
    // are ignored (UsdGeomMesh, "normals"). Without the scheme's refinement, they are its cage's smooth normals.
    bool authored = geometry.normals.value.count != 0 && geometry.subdivision == PrUsdGeomSubdivision_None;
    if (authored) {
        const float up[3] = {0, 0, 1};
        cornerValues(geometry, &geometry.normals, faceOf, 3, up, out->normals);
    } else if (status == PrStatus_Ok) {
        uint32_t* pointTriangles = allocArray<uint32_t>(triangleCount * 3);
        for (uint32_t k = 0; k < triangleCount * 3; ++k)
            pointTriangles[k] = uint32_t(geometry.faceVertexIndices[triangles[k]]);
        float* pointNormals = allocArray<float>(geometry.pointCount * 3);
        status = prMeshNormals(points, PrMeshIndexView{pointTriangles, triangleCount * 3, PrMeshIndexType_U32},
            PrMeshAttribute{pointNormals, geometry.pointCount, 3 * sizeof(float), prMeshFormatFloat32(3)});
        for (uint32_t c = 0; c < cornerCount; ++c)
            prMemCopy(out->normals + c * 3, pointNormals + size_t(geometry.faceVertexIndices[c]) * 3,
                3 * sizeof(float));
        prMemoryFree(pointTriangles);
        prMemoryFree(pointNormals);
    }
    // Each requested primitive variable, for every corner, as floats: texture coordinates with t turned to v down the
    // image.
    uint32_t streamMask = 0;
    out->streamCount = request->streamCount;
    out->streams = allocArray<float*>(request->streamCount);
    out->streamWidths = allocArray<uint32_t>(request->streamCount);
    for (uint32_t i = 0; i < request->streamCount && i < 32; ++i) {
        PrUsdGeomPrimvar primvar{};
        PrimvarNumber number = PrimvarNumber_None;
        if (!prUsdGeomPrimvarRead(s->stage, mesh.prim, request->streams[i], s->restTime, scratch, &primvar) ||
                !prUsdGeomPrimvarFits(&geometry, &primvar))
            continue;
        uint32_t width = componentsOf(primvar.value.type, &number);
        if (width == 0)
            continue;
        bool texCoords = false;
        for (uint32_t set = 0; set < s->texCoordCount; ++set)
            texCoords = texCoords || s->texCoordNames[set] == request->streams[i];
        const float zero[4] = {0, 0, 0, 0};
        out->streams[i] = allocArray<float>(cornerCount * width);
        out->streamWidths[i] = width;
        cornerValues(geometry, &primvar, faceOf, width, zero, out->streams[i]);
        for (uint32_t c = 0; texCoords && width >= 2 && c < cornerCount; ++c)
            out->streams[i][c * width + 1] = 1 - out->streams[i][c * width + 1];
        streamMask |= 1u << i;
    }

    // The faces of each part, which the source read in the same order: its subsets, then the faces that none holds.
    PrUsdGeomSubset* subsets = allocArray<PrUsdGeomSubset>(mesh.subsetCount);
    for (uint32_t k = 0; k < mesh.subsetCount; ++k) {
        subsets[k] = PrUsdGeomSubset{.prim = s->subsets.data[mesh.firstSubset + k]};
        const int32_t* indices = nullptr;
        PrUsdValue value{};
        uint32_t property = prUsdStageFindProperty(s->stage, subsets[k].prim, "indices");
        if (property != kPrUsdNone && prUsdStageAttributeValue(s->stage, subsets[k].prim, property, s->restTime,
                scratch, &value) && value.type == PrUsdType_Int) {
            indices = static_cast<const int32_t*>(value.data);
            subsets[k].indices = indices;
            subsets[k].indexCount = value.count;
        }
        for (uint32_t i = 0; i < subsets[k].indexCount; ++i) {
            if (indices[i] < 0 || uint32_t(indices[i]) >= faceCount)
                subsets[k].indexCount = 0;
        }
    }
    uint32_t* partOfFace = allocArray<uint32_t>(faceCount);
    partsOfFaces(geometry, subsets, mesh.subsetCount, partOfFace);
    out->triangles = allocArray<uint32_t>(triangleCount * 3);
    out->parts = allocArray<PrModelMeshDataPart>(model.partCount);
    out->partCount = model.partCount;
    uint32_t partIndex = 0;
    for (uint32_t k = 0; status == PrStatus_Ok && k <= mesh.subsetCount && partIndex < model.partCount; ++k) {
        if (k == mesh.subsetCount && !mesh.hasRest)
            break;
        PrModelMeshDataPart& part = out->parts[partIndex++];
        part.firstTriangle = out->triangleCount;
        part.flags = PrModelMeshPartFlag_Normals | PrModelMeshPartFlag_Corners;
        part.streams = streamMask;
        // The stage changed none of the faces that the source read, so a part without triangles is a subset that names
        // only holes, which stays empty.
        for (uint32_t t = 0; t < triangleCount; ++t) {
            if (partOfFace[faceOfTriangle[t]] != k)
                continue;
            prMemCopy(out->triangles + size_t(out->triangleCount) * 3, triangles + size_t(t) * 3, 3 * sizeof(uint32_t));
            ++out->triangleCount;
        }
        part.triangleCount = out->triangleCount - part.firstTriangle;
    }
    prMemoryFree(partOfFace);
    prMemoryFree(subsets);
    prMemoryFree(triangles);
    prMemoryFree(faceOfTriangle);
    prMemoryFree(faceOf);
    surface.reset();
    prUsdScratchDestroy(scratch);
    return status == PrStatus_Ok ? PrStatus_Ok : PrStatus_InvalidData;
}

PrStatus imageOpen(const PrModelSource* source, PrModelImageIndex image, PrModelImageData* out) {
    const UsdModelSource* s = static_cast<const UsdModelSource*>(source);
    const PrBlobSource* bytes = s->imageBytes.data[image];
    if (bytes == nullptr)
        return PrStatus_Io;
    prBlobSourceRetain(bytes);
    out->bytes = bytes;
    return PrStatus_Ok;
}

// Appends the names from the root down, which the parents give from the node up.
void nodeAddress(const PrModelSource* source, PrModelNodeIndex node, PrTextBuffer* out) {
    const UsdModelSource* s = static_cast<const UsdModelSource*>(source);
    uint32_t chain[256];
    uint32_t depth = 0;
    for (uint32_t at = node; at != kNone && depth < 256; at = s->nodes.data[at].parent)
        chain[depth++] = at;
    while (depth-- > 0) {
        prTextBufferAppendChar(out, '/');
        prTextBufferAppend(out, s->nodes.data[chain[depth]].name);
    }
}

// Returns the node at a prim's path: each name of the path is one child of the node before it, from a root.
PrModelNodeIndex nodeFromAddress(const PrModelSource* source, PrStringView path) {
    const UsdModelSource* s = static_cast<const UsdModelSource*>(source);
    if (path.size < 2 || path.data[0] != '/' || s->nodes.count == 0)
        return kPrModelNodeNone;
    uint32_t parent = kNone;
    size_t at = 1;
    while (at <= path.size) {
        size_t end = at;
        while (end < path.size && path.data[end] != '/')
            ++end;
        PrStringView name{path.data + at, end - at};
        const PrModelNodeIndex* candidates =
            parent == kNone ? s->roots.data : s->children.data + s->nodes.data[parent].firstChild;
        uint32_t count = parent == kNone ? s->roots.count : s->nodes.data[parent].childCount;
        uint32_t child = kNone;
        for (uint32_t i = 0; i < count && child == kNone; ++i) {
            if (s->nodes.data[candidates[i]].name == name)
                child = candidates[i];
        }
        if (child == kNone)
            return kPrModelNodeNone;
        parent = child;
        at = end + 1;
    }
    return parent;
}

UsdModelSource* sourceMake(const PrUsdStage* stage, PrUsdStage* PR_NULLABLE owned, const PrUsdModelOptions& options) {
    UsdModelSource* s = allocZeroed<UsdModelSource>();
    s->destroyFn = sourceDestroy;
    s->infoFn = sourceInfo;
    s->meshReadFn = meshRead;
    s->imageOpenFn = imageOpen;
    s->nodeAddressFn = nodeAddress;
    s->nodeFromAddressFn = nodeFromAddress;
    s->stage = stage;
    s->owned = owned;
    describe(s, options);
    return s;
}

// ---------------------------------------------------------------------------------------------------------------------
// The format

// A stage being composed: the format's description, the root's location and its bytes, and the callback that receives
// the source.
struct UsdOpen {
    const PrModelResolver* resolver;
    PrRawString location;
    const PrBlobSource* root;
};

// Opens what the stage's layers name: the root's own bytes at its location, and anything else through the resolver.
const PrBlobSource* PR_NULLABLE openResource(void* PR_NULLABLE context, PrStringView location) {
    const UsdOpen* open = static_cast<const UsdOpen*>(context);
    if (location == prRawStringView(&open->location)) {
        prBlobSourceRetain(open->root);
        return open->root;
    }
    return open->resolver->resolve(open->resolver->context, location);
}

bool formatSupportsType(void* PR_NULLABLE, PrStringView type) {
    return type == "model/vnd.usda" || type == "model/x-usdc" || type == "model/vnd.usdz+zip";
}

// Composes the stage in the format's context, which reads the root's bytes and the layers that it names, and makes its
// source in the same context.
void formatOpen(void* PR_NULLABLE context, const PrBlobSource* bytes, PrStringView location,
        const PrModelResolver* resolver, PrModelSourceOpenFn done, void* PR_NULLABLE doneContext) {
    const PrUsdModelFormatDesc* desc = static_cast<const PrUsdModelFormatDesc*>(context);
    UsdOpen open{resolver, prRawStringCopy(location), bytes};
    PrUsdStageOptions options = prUsdStageOptionsDefault();
    options.layer.codec = desc->codec;
    options.open = openResource;
    options.openContext = &open;
    options.variantSelectionCount = desc->context.variantSelectionCount;
    options.variantSelections = desc->context.variantSelections;
    PrUsdStage* stage = nullptr;
    PrStatus status = prUsdStageCreate(location, &options, &stage);
    PrModelSource* source = nullptr;
    if (status == PrStatus_Ok) {
        PrUsdModelOptions model{
            .context = desc->context,
            .open = openResource,
            .openContext = &open,
            .codec = desc->codec,
        };
        source = sourceMake(stage, stage, model);
    }
    prRawStringReset(&open.location);
    done(doneContext, status, source);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrUsdModelOptions prUsdModelOptionsDefault(void) {
    return PrUsdModelOptions{};
}

PrModelSource* prUsdModelSourceCreate(const PrUsdStage* stage, const PrUsdModelOptions* PR_NULLABLE options) {
    PrUsdModelOptions given = options != nullptr ? *options : prUsdModelOptionsDefault();
    return sourceMake(stage, nullptr, given);
}

PrModelFormat prUsdModelFormat(const PrUsdModelFormatDesc* desc) {
    // The format reads its description and changes none of it.
    return PrModelFormat{
        .supportsTypeFn = formatSupportsType,
        .openFn = formatOpen,
        .context = const_cast<PrUsdModelFormatDesc*>(desc),
    };
}
