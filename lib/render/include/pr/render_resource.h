// The resources of a renderer (pr/render.h), which every scene shares: meshes, textures and materials. Environments are
// resources too, with a header of their own (pr/render_environment.h).
//
// Creating, setting and destroying a resource is thread-safe. A resource may be referenced as soon as it is created.
// What is set takes effect when the next frame begins (prRenderFrameBegin).

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/render.h"
#include "pr/render_records.h"
#include "pr/render_shaders.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

// A mesh: the geometry of its vertices, the attributes that its materials read of them, indices, and parts that each
// draw with a material of their own. Zero is no mesh.
typedef struct PrRenderMesh {
    uint32_t id;
} PrRenderMesh;

// A vertex of a mesh's geometry, which the renderer owns: every pass reads it and deformation writes it, whatever a
// material reads. Positions are floats. The normal and the tangent are signed 16-bit values that map [-32767, 32767] to
// [-1, 1], so a unit vector is within 1/32767 of the stored one in each component. It is laid out as
// prRenderGeometryLayout describes it, so a producer converts its geometry as it converts its other attributes.
//
// The space is right-handed, and a triangle faces front if its vertices run counterclockwise. The tangent points along
// increasing u, and `tangent[3]` is the handedness of the bitangent, 32767 or -32767: the bitangent is the cross
// product of the normal and the tangent, times the handedness. A mesh without tangents stores zeros, and normal maps
// then leave its normals unchanged.
typedef struct PrRenderGeometryVertex {
    float position[3];
    int16_t normal[4];          // x, y, z and zero
    int16_t tangent[4];         // x, y, z and the handedness
} PrRenderGeometryVertex;

// Returns the geometry of a vertex at `position` with a unit `normal`, and a unit `tangent` or zero for no tangent,
// with the bitangent's `handedness`, 1 or -1, rounded to the vertex's precision.
PrRenderGeometryVertex prRenderGeometryVertexPack(PrVec3 position, PrVec3 normal, PrVec3 tangent, float handedness);

// The attributes of a mesh's vertices in one layout that a material type declares (PrRenderMaterialType::layout): a
// vertex of `layout->stride` bytes for each vertex of the mesh, its attributes at their offsets in their formats. A
// producer writes them with prMeshAttributeConvert, which converts what its source holds into each attribute's format,
// or the attribute's fallback where the source lacks it.
typedef struct PrRenderMeshStream {
    const PrRenderVertexLayout* layout;     // one of the renderer's shader set (PrRenderShaderSet::layouts)
    const void* vertices;
} PrRenderMeshStream;

// A part of a mesh: a range of the mesh's indices that form triangles, and the sphere that bounds their vertices. A
// radius that is not above zero asks the renderer to compute the sphere.
typedef struct PrRenderMeshPart {
    uint32_t firstIndex;
    uint32_t indexCount;        // a multiple of 3
    PrSphere bounds;
} PrRenderMeshPart;

// The joints that move a vertex of a skinned mesh: the indices of up to four joints, and the part of the vertex that
// each moves, as 16 unsigned bits in which 65535 is all of it. The weights sum to 65535, and a joint with a weight of
// zero moves nothing. A vertex whose weights are all zero does not move.
typedef struct PrRenderSkinVertex {
    uint16_t joints[kPrRenderSkinInfluences];
    uint16_t weights[kPrRenderSkinInfluences];
} PrRenderSkinVertex;

// Returns a vertex's skin for four joints and their weights, which are scaled to sum to one and rounded so that the
// stored ones sum to 65535. Weights that are not above zero count as zero.
PrRenderSkinVertex prRenderSkinVertexPack(const uint16_t* joints, const float* weights);

// What one morph target adds to one vertex at a weight of 1: to its position, and to its normal and its tangent before
// each is brought back to unit length.
typedef struct PrRenderMorphDelta {
    float position[3];
    float normal[3];
    float tangent[3];
} PrRenderMorphDelta;

// The data of a mesh. An index names a vertex of the mesh, and every part's indices lie within `indices`.
//
// The mesh's vertices have a geometry each, and the attributes of each layout of `streams`, at most one stream for a
// layout. A part that draws with a material whose type's layout the mesh has no stream of reads the fallback of each of
// the layout's attributes, which the renderer makes a stream of, and a scene reports the draws that do
// (prRenderSceneFallbackDrawCount).
//
// A mesh may deform. Deformation moves the geometry alone. With `skin`, it has a skin of `jointCount` joints: each
// vertex moves with the joints that its skin names, by the matrices that an instance gives them
// (prRenderInstanceSetJoints). With `morphs`, it has `morphTargetCount` morph targets: each adds its delta to a vertex,
// times the weight that an instance gives it (prRenderInstanceSetMorphWeights), before the joints move the vertex. The
// vertices are the mesh as it was bound, which an instance that sets neither draws.
typedef struct PrRenderMeshData {
    const PrRenderGeometryVertex* geometry;     // by vertex
    uint32_t vertexCount;
    const PrRenderMeshStream* PR_NULLABLE streams;
    uint32_t streamCount;
    const uint32_t* indices;
    uint32_t indexCount;
    const PrRenderMeshPart* parts;
    uint32_t partCount;         // at least one

    const PrRenderSkinVertex* PR_NULLABLE skin;     // by vertex, or null for a mesh without a skin
    uint32_t jointCount;                            // every joint that `skin` names is below it

    // The deltas of the vertex i for the target t are at i * morphTargetCount + t. Null for a mesh without targets.
    const PrRenderMorphDelta* PR_NULLABLE morphs;
    uint32_t morphTargetCount;
} PrRenderMeshData;

// Creates a mesh without data. Instances may refer to it at once, and are drawn once it has data.
PrRenderMesh prRenderMeshCreate(PrRender* render);

// Sets or replaces the mesh's data, which the renderer copies before the call returns. The frames that begin after the
// call draw it. Returns Limit if the device has no memory for the data, and Lost if the device is lost. On failure the
// mesh keeps the data that it had.
PrStatus prRenderMeshSetData(PrRender* render, PrRenderMesh mesh, const PrRenderMeshData* data);

// Destroys the mesh. Instances that still refer to it are not drawn.
void prRenderMeshDestroy(PrRender* render, PrRenderMesh mesh);

// Returns true if the frame that began last draws the mesh's data. Called on the thread that builds frames.
bool prRenderMeshIsReady(const PrRender* render, PrRenderMesh mesh);

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// A 2D texture with its mip levels. Zero is no texture. A texture holds texels alone: how a material samples them is
// the material's (PrRenderTextureUse), so one image that two materials read with two samplers is one texture.
typedef struct PrRenderTexture {
    uint32_t id;
} PrRenderTexture;

// The maximum number of mip levels of a texture's data.
enum { kPrRenderTextureMaxLevels = 16 };

// One mip level of a texture's data: rows of texels, or of blocks, `rowBytes` apart. `rowBytes` is a multiple of the
// size of a texel or a block, or zero for rows with nothing between them.
typedef struct PrRenderTextureLevel {
    const void* PR_NULLABLE data;
    uint32_t rowBytes;
} PrRenderTextureLevel;

// The texels of a texture. `format` names their layout and their encoding: a Srgb format is read as sRGB-encoded color,
// which a shader reads as linear light, and any other is read as it is stored. A loader chooses the encoding by what
// the texture holds, which a material type declares for each of its textures (PrRenderMaterialTexture::kind).
//
// With one level in a format that the device filters and renders to, the renderer makes the rest of the mip chain. With
// more than one, the levels given are the whole chain. A compressed format passes through as it is, on a device that
// has it.
typedef struct PrRenderTextureData {
    PrGpuFormat format;
    PrUVec2 extent;                                         // of the first level
    uint8_t levelCount;                                     // at least one, at most kPrRenderTextureMaxLevels
    PrRenderTextureLevel levels[kPrRenderTextureMaxLevels]; // by mip level
} PrRenderTextureData;

// Creates a texture without data. Materials may refer to it at once: each reads the stand-in value that its type
// declares for the texture's place until the texels arrive (PrRenderMaterialTexture::standIn). For the library's glTF
// type that is a white base color, a rough dielectric, a flat normal, no occlusion and no emission, so a surface never
// draws as metal, or glows, for want of texels.
PrRenderTexture prRenderTextureCreate(PrRender* render);

// Sets or replaces the texture's texels, which the renderer copies before the call returns. The frames that begin after
// the call sample them. Returns NotSupported for a format that the device does not sample, Limit if the device has no
// memory or handle for the texture or the extent is larger than it allows, and Lost if the device is lost. On failure
// the texture keeps the texels that it had.
PrStatus prRenderTextureSetData(PrRender* render, PrRenderTexture texture, const PrRenderTextureData* data);

// Sets or replaces the texture's texels with those of `gpu`, a sampled 2D texture of the renderer's device, which the
// renderer retains (prGpuTextureRetain). The frames that begin after the call sample it. Its format says how a shader
// reads its values, as PrRenderTextureData::format does, and it has the levels that it is sampled with.
void prRenderTextureSetGpu(PrRender* render, PrRenderTexture texture, PrGpuTexture gpu);

// Destroys the texture. Materials that still refer to it read what they read before its texels arrived.
void prRenderTextureDestroy(PrRender* render, PrRenderTexture texture);

// Returns true if the frame that began last samples the texture's data. Called on the thread that builds frames.
bool prRenderTextureIsReady(const PrRender* render, PrRenderTexture texture);

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// A material: a material type, that type's parameters and textures, and render states. Zero is no material, and a part
// without a material draws with a neutral one.
typedef struct PrRenderMaterial {
    uint32_t id;
} PrRenderMaterial;

// How a material's alpha is used, as glTF defines its alpha modes.
enum {
    PrRenderAlphaMode_Opaque,   // alpha is ignored
    PrRenderAlphaMode_Mask,     // the surface is absent where alpha is below the material's cutoff
    PrRenderAlphaMode_Blend,    // the surface is blended over what lies behind it, by its alpha
};
typedef uint8_t PrRenderAlphaMode;

// What the unit of a material's emission is. glTF's core material defines its emission as luminance in nits where a
// physical unit is needed, and leaves the conversion to a pixel to the camera's exposure; many engines read an emissive
// factor of 1 as a fully exposed pixel instead (glTF 2.0, "Additional Textures", the note on emissive). Neither reading
// suits every asset, so the unit is the material's, and a producer chooses it for each asset.
enum {
    // Luminance in nits, which a view's exposure scales as it scales the light of the scene.
    PrRenderEmission_Luminance,

    // A share of a view's diffuse white, at any exposure: an emission of 1 is a value of 1 in the view's color, as an
    // unlit color is (KHR_materials_unlit).
    PrRenderEmission_Display,

    PrRenderEmission_Count,
};
typedef uint8_t PrRenderEmission;

// Returns the unit that `name` names, as prRenderEmissionName spells it, ignoring case, or Count if it names none.
PrRenderEmission prRenderEmissionFromName(PrStringView name);

// Returns a unit's name in lowercase: "luminance" or "display".
PrStringView prRenderEmissionName(PrRenderEmission emission);

// A texture as a material uses it: the texture, the set of texture coordinates that it reads, the transform that takes
// those coordinates to the texture's, any affine map of the plane, the sampler that reads its texels, and how its
// values become an input's: each texel times `scale` plus `bias`, by component, of which an input of one number reads
// the component `channel`.
typedef struct PrRenderTextureUse {
    PrRenderTexture texture;    // none for no texture
    uint8_t texCoord;           // the type's attribute of the semantic uv that it reads, by its semantic index
    uint8_t channel;            // 0 to 3, red to alpha
    PrAffine2 transform;
    PrGpuSamplerDesc sampler;
    float scale[4];
    float bias[4];
} PrRenderTextureUse;

// Returns the use of `texture` with the first set of coordinates, unchanged, through a sampler that filters linearly
// between texels and levels and repeats, with a scale of 1, a bias of 0, and the red channel.
PrRenderTextureUse prRenderTextureUseDefault(PrRenderTexture texture);

// Returns the transform of glTF's KHR_texture_transform: the coordinates scaled by `scale`, then rotated by `rotation`
// radians, counterclockwise as the extension draws an image with v down, then moved by `offset`.
PrAffine2 prRenderTextureTransform(PrVec2 offset, float rotation, PrVec2 scale);

// The render states of a material that are on or off.
enum {
    // Draws both faces, and lights a back face by its reversed normal.
    PrRenderMaterialFlag_DoubleSided = 1u << 0,

    // The material's type reads the scene behind the material's surfaces (pr/render_behind.slang), as a surface that
    // transmits light does. The material is then drawn after the surfaces that do not blend, with the blended draws and
    // in their order, whatever its alpha mode, and it casts no shadows. On a device without bindless it samples one
    // texture fewer (prRenderMaterialTextureLimit). The renderer makes the texture of the scene behind for the views of
    // a scene that has an instance with such a material, and for no other.
    PrRenderMaterialFlag_ReadsBehind = 1u << 1,
};
typedef uint8_t PrRenderMaterialFlags;

// A material of any type. `type` is the id of a type of the renderer's shader set (pr/render_shaders.h): the id that
// the set's header names, or the one that prRenderMaterialTypeFromName finds by the type's name. The type gives
// `textures` and `parameters` their meaning, and a producer fills the parameter record by the names of the type's
// parameters (PrRenderMaterialType::parameters). prRenderMaterialDescDefault describes a material of any type, and
// prRenderMaterialSetKhronosPbr and the functions after it set a material of the library's types from a description of
// their own, for an application that knows those types.
//
// The description points at the caller's textures and parameters, which prRenderMaterialSet copies.
typedef struct PrRenderMaterialDesc {
    uint32_t type;

    // The type's optional attributes that the material's meshes carry, a bit for each by its index among them
    // (PrRenderMaterialType::optionalAttributes), which choose the variant of the type that draws the material and the
    // layout of the attributes that its meshes hold. The others read as their values where a vertex lacks them.
    uint32_t attributes;

    PrRenderAlphaMode alphaMode;
    PrRenderMaterialFlags flags;
    float alphaCutoff;          // for PrRenderAlphaMode_Mask
    PrRenderEmission emission;  // the unit of what the surface of the type's shading model emits

    // The material's textures, by the type's texture index: `textureCount` of them, at most the number that the type
    // declares. The type's textures past them are none. Null if `textureCount` is zero.
    const PrRenderTextureUse* PR_NULLABLE textures;
    uint32_t textureCount;

    // The type's parameter record: the structure that the type names as its Parameters, which C and Slang both compile,
    // and its size.
    const void* parameters;
    size_t parameterBytes;
} PrRenderMaterialDesc;

// Returns the description of a material of `type` whose parameter record is the `parameterBytes` bytes at `parameters`:
// opaque, without flags, with a cutoff of one half, no textures, and none of the type's optional attributes. The
// description points at `parameters`.
PrRenderMaterialDesc prRenderMaterialDescDefault(uint32_t type, const void* parameters, size_t parameterBytes);

// A material of glTF's metallic-roughness model, with the layers of Khronos's material extensions. Each factor
// multiplies its texture's value, as glTF defines them: the base color texture is sRGB, the metallic-roughness texture
// holds roughness in green and metallic in blue, the occlusion texture holds occlusion in red, and the normal texture
// holds a tangent-space normal.
//
// The fields after the core material's are the extensions', by the names and with the defaults of their specifications,
// and each extension's textures hold what the extension reads of them (pr/render_material_khronos_pbr_layered.slang).
// The sheen's color, the specular color and the diffuse transmission's color are sRGB textures. A material whose every
// such field is at its default is glTF's core material.
typedef struct PrRenderKhronosPbrMaterial {
    PrRenderAlphaMode alphaMode;
    PrRenderMaterialFlags flags;
    float alphaCutoff;
    PrColor baseColor;          // linear, with the alpha
    float metallic;
    float roughness;
    float normalScale;
    float occlusionStrength;
    PrVec3 emissive;            // linear
    float emissiveStrength;     // KHR_materials_emissive_strength
    PrRenderEmission emission;  // the unit of the emissive factor times its strength and its texture
    PrRenderTextureUse baseColorTexture;
    PrRenderTextureUse metallicRoughnessTexture;
    PrRenderTextureUse normalTexture;
    PrRenderTextureUse occlusionTexture;
    PrRenderTextureUse emissiveTexture;

    float ior;                          // KHR_materials_ior, 1.5 by default; 0 stands for an infinite index

    float specular;                     // KHR_materials_specular
    PrVec3 specularColor;               // linear, and may exceed 1
    PrRenderTextureUse specularTexture;
    PrRenderTextureUse specularColorTexture;

    float clearcoat;                    // KHR_materials_clearcoat
    float clearcoatRoughness;
    float clearcoatNormalScale;
    PrRenderTextureUse clearcoatTexture;
    PrRenderTextureUse clearcoatRoughnessTexture;
    PrRenderTextureUse clearcoatNormalTexture;

    PrVec3 sheenColor;                  // KHR_materials_sheen, linear; black is no sheen
    float sheenRoughness;
    PrRenderTextureUse sheenColorTexture;
    PrRenderTextureUse sheenRoughnessTexture;

    float iridescence;                  // KHR_materials_iridescence
    float iridescenceIor;
    float iridescenceThicknessMinimum;  // in nanometers
    float iridescenceThicknessMaximum;
    PrRenderTextureUse iridescenceTexture;
    PrRenderTextureUse iridescenceThicknessTexture;

    float anisotropy;                   // KHR_materials_anisotropy: the strength
    float anisotropyRotation;           // radians counterclockwise from the tangent
    PrRenderTextureUse anisotropyTexture;

    float transmission;                 // KHR_materials_transmission
    PrRenderTextureUse transmissionTexture;

    // KHR_materials_volume: the thickness in the mesh's space, which an instance's scale brings to the world, and the
    // color that white light has after `attenuationDistance` in the world. A distance of zero, or an infinite one, is
    // no attenuation.
    float thickness;
    PrRenderTextureUse thicknessTexture;
    float attenuationDistance;
    PrVec3 attenuationColor;            // linear

    float dispersion;                   // KHR_materials_dispersion: 20 over the Abbe number, or zero for none

    float diffuseTransmission;          // KHR_materials_diffuse_transmission
    PrVec3 diffuseTransmissionColor;    // linear
    PrRenderTextureUse diffuseTransmissionTexture;
    PrRenderTextureUse diffuseTransmissionColorTexture;

    // The archived KHR_materials_pbrSpecularGlossiness. If `specularGlossiness` is set, the material is of that
    // workflow: `baseColor` and its texture are the diffuse color, `specularColor` is the specular color, `glossiness`
    // is the glossiness, and `metallicRoughnessTexture` is the specular-glossiness texture, which is sRGB with the
    // glossiness in alpha. `metallic`, `roughness`, `specular` and the specular textures are then not read.
    bool specularGlossiness;
    float glossiness;
} PrRenderKhronosPbrMaterial;

// Returns glTF's default material: opaque, white, fully metallic and rough, without emission or textures, with a cutoff
// of one half, and every extension's default.
PrRenderKhronosPbrMaterial prRenderKhronosPbrMaterialDefault(void);

// A material that no light changes (glTF's KHR_materials_unlit): it emits its color times its texture as the view's
// (PrRenderEmission_Display).
typedef struct PrRenderUnlitMaterial {
    PrRenderAlphaMode alphaMode;
    PrRenderMaterialFlags flags;
    float alphaCutoff;
    PrColor color;              // linear, with the alpha
    PrRenderTextureUse colorTexture;
} PrRenderUnlitMaterial;

// Returns an opaque, white unlit material without a texture, with a cutoff of one half.
PrRenderUnlitMaterial prRenderUnlitMaterialDefault(void);

// A material of OpenUSD's preview surface (UsdPreviewSurface), by the specification's inputs, whose colors are linear.
// Each input that has a texture takes the texture's value in place of its own: of a color input, the red, green and
// blue of the texture's scaled and biased texel (UsdUVTexture), and of a scalar input, its channel. The diffuse color
// and the opacity are times the color attribute of the mesh's vertices, so a primitive variable that a network reads
// into them, such as displayColor, is a value of that attribute. The emissive color is times the optional attribute of
// its name where the material's meshes carry it.
//
// The opacity decides the alpha mode, as the specification has it: a threshold above zero cuts the surface away where
// the opacity is below it and draws the rest opaque, and otherwise a surface whose opacity is below 1, or comes from a
// texture, blends.
typedef struct PrRenderUsdPreviewMaterial {
    PrRenderMaterialFlags flags;
    PrVec3 diffuseColor;
    PrVec3 emissiveColor;
    PrRenderEmission emission;  // the unit of the emissive color
    bool useSpecularWorkflow;
    PrVec3 specularColor;
    float metallic;
    float roughness;
    float clearcoat;
    float clearcoatRoughness;
    float opacity;
    float opacityThreshold;
    float ior;
    float occlusion;
    PrRenderTextureUse diffuseColorTexture;
    PrRenderTextureUse normalTexture;
    PrRenderTextureUse roughnessTexture;
    PrRenderTextureUse metallicTexture;
    PrRenderTextureUse occlusionTexture;
    PrRenderTextureUse emissiveColorTexture;
    PrRenderTextureUse opacityTexture;
    PrRenderTextureUse specularColorTexture;
    PrRenderTextureUse clearcoatTexture;
    PrRenderTextureUse clearcoatRoughnessTexture;

    // The type's optional attributes that the material's meshes carry (PrRenderMaterialDesc::attributes): 1 for the
    // emissive color, the type's one optional attribute, or 0 for none.
    uint32_t attributes;
} PrRenderUsdPreviewMaterial;

// Returns the preview surface of the specification's fallbacks, without textures or flags. A texture's use is of no
// texture, with a scale of 1, a bias of 0 and the red channel.
PrRenderUsdPreviewMaterial prRenderUsdPreviewMaterialDefault(void);

// Creates a material that is not set, which draws as a neutral gray. Instances may refer to it at once.
PrRenderMaterial prRenderMaterialCreate(PrRender* render);

// Sets or replaces what the material is. The frames that begin after the call draw with it. The description's type is
// one of the renderer's shader set, its parameter record has the size of that type's, and it names no more textures
// than the type has.
//
// The call makes the pipelines that draws of the material need and the renderer lacks, for the passes and the sample
// counts that the renderer has drawn with. The first material of a type, or of an alpha mode or a sidedness of a type,
// therefore costs the calling thread a pipeline's creation, and the frame that first draws it does not pay it.
void prRenderMaterialSet(PrRender* render, PrRenderMaterial material, const PrRenderMaterialDesc* desc);

// Set a material to one of the library's types, as prRenderMaterialSet does, from the library's description of it:
// glTF's core material, whose layers are not drawn; glTF's material with its layers; an unlit material; and a preview
// surface. Every shader set has these types, which the renderer finds by their names. An application that sets one of
// glTF's chooses between the two (prRenderKhronosPbrMaterialIsLayered).
void prRenderMaterialSetKhronosPbr(PrRender* render, PrRenderMaterial material, const PrRenderKhronosPbrMaterial* gltf);
void prRenderMaterialSetKhronosPbrLayered(PrRender* render, PrRenderMaterial material, const PrRenderKhronosPbrMaterial* gltf);
void prRenderMaterialSetUnlit(PrRender* render, PrRenderMaterial material, const PrRenderUnlitMaterial* unlit);
void prRenderMaterialSetUsdPreview(PrRender* render, PrRenderMaterial material, const PrRenderUsdPreviewMaterial* preview);

// Returns true if a glTF material has what only the layered type draws: a layer of an extension, the
// specular-glossiness workflow, or an index of refraction or a specular reflection that is not the default's.
bool prRenderKhronosPbrMaterialIsLayered(const PrRenderKhronosPbrMaterial* gltf);

// Returns the number of different textures that a material whose description has `flags` samples on the renderer's
// device: the material texture slots that the passes declare on a device without bindless, less the slot that the scene
// behind a surface takes from a material that reads it (PrRenderMaterialFlag_ReadsBehind), and UINT32_MAX, which is no
// limit, on a device with bindless. A glTF material reads the scene behind it if it transmits and is layered. A texture
// that a material names at several of its type's indices counts once. A material that names more textures than this
// draws with the first of them, in the order of its type's texture indices, and reads each of the others as the
// stand-in value that its type declares (PrRenderMaterialTexture::standIn). A loader that must not lose a texture packs
// several into one, or chooses a type with fewer, when its material names more than this number.
uint32_t prRenderMaterialTextureLimit(const PrRender* render, PrRenderMaterialFlags flags);

// Destroys the material. Parts that still refer to it draw with a neutral one.
void prRenderMaterialDestroy(PrRender* render, PrRenderMaterial material);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
