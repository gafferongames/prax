// Model sources: an asset of a scene format described in no format's terms, which a model draws (pr/model.h).
//
// A source describes its asset when it is created, and the description does not change: the nodes of its hierarchy, the
// meshes that they draw, the materials of the meshes' parts, the images that the materials sample, its cameras, lights
// and environment, its skins, its animations as tracks of keyed values, and what of the asset it does not describe. It
// loads on demand what the description only names: a mesh's vertices and triangles, and an image's bytes or texture. A
// format's adapter implements a source in its own library (pr/model_source_backend.h), so that what draws models
// depends on no format.
//
// The source's scene is in meters with +y up, and its triangles face front where their corners run counterclockwise.
// Texture coordinates have v down the image, as a renderer's do.
//
// Threads: a source's description is read by any thread, and its loaders are called from any thread at once, as a
// model's loading jobs call them.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/geom.h"
#include "pr/mesh.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"
#include "pr/texture.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Indices

// The indices of a source's nodes, meshes, parts, materials, textures, images, cameras, lights, skins, animations and
// variants, each an index of the array of its kind in the source's description, and the value of each that is none.
typedef uint32_t PrModelNodeIndex;
typedef uint32_t PrModelMeshIndex;
typedef uint32_t PrModelPartIndex;
typedef uint32_t PrModelMaterialIndex;
typedef uint32_t PrModelTextureIndex;
typedef uint32_t PrModelImageIndex;
typedef uint32_t PrModelCameraIndex;
typedef uint32_t PrModelLightIndex;
typedef uint32_t PrModelSkinIndex;
typedef uint32_t PrModelAnimationIndex;
typedef uint32_t PrModelVariantIndex;
PR_CONSTANT uint32_t kPrModelNodeNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelMeshNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelPartNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelMaterialNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelTextureNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelImageNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelCameraNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelLightNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelSkinNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelAnimationNone = 0xffffffffu;
PR_CONSTANT uint32_t kPrModelVariantNone = 0xffffffffu;

// ---------------------------------------------------------------------------------------------------------------------
// Nodes and draws

// What a node is beside its transform.
enum {
    // The rest transform is the node's translation, rotation and scale, which tracks of each replace. Without it, the
    // rest transform is the node's matrix, which a track of a matrix replaces.
    PrModelNodeFlag_Trs = 1u << 0,

    // The node is hidden at rest, and so is everything under it.
    PrModelNodeFlag_Hidden = 1u << 1,

    // The node's transform maps it to the source's scene, not to its parent's space (UsdGeomXformable's
    // resetXformStack). Visibility still follows the parent.
    PrModelNodeFlag_ResetsTransform = 1u << 2,
};
typedef uint8_t PrModelNodeFlags;

// A node of the source's hierarchy. A node draws nothing itself: draws name the nodes whose meshes they place.
typedef struct PrModelSourceNode {
    PrStringView name;
    PrModelNodeIndex parent;            // or kPrModelNodeNone for a node without one
    PrModelNodeFlags flags;

    // The node's children, a range of the description's `children`, in their order.
    uint32_t firstChild;
    uint32_t childCount;

    // The rest transform, as the flag PrModelNodeFlag_Trs says: a translation, a rotation as a unit quaternion (x, y,
    // z, w) and a scale, or a matrix.
    float translation[3];
    float rotation[4];
    float scale[3];
    PrMatrix4 matrix;

    PrModelCameraIndex camera;          // or kPrModelCameraNone
    PrModelLightIndex light;            // or kPrModelLightNone

    // The node's morph weights, a range of the description's `weights`, which stand for its meshes' where it has any.
    uint32_t firstWeight;
    uint32_t weightCount;
} PrModelSourceNode;

// A mesh drawn at a node: with the node's skin, once at the node, or once at each of several transforms within the
// node, as instancing (EXT_mesh_gpu_instancing) or a point instancer gives them.
typedef struct PrModelSourceDraw {
    PrModelNodeIndex node;
    PrModelMeshIndex mesh;
    PrModelSkinIndex skin;              // or kPrModelSkinNone

    // The transforms of the instances within the node, a range of the description's `instances`, or none for one draw
    // at the node itself.
    uint32_t firstInstance;
    uint32_t instanceCount;
} PrModelSourceDraw;

// A skin: its joints, a range of the description's `joints`, each with the inverse bind matrix at the same place of
// `inverseBinds`, which maps the mesh as it was bound into the joint's space.
typedef struct PrModelSourceSkin {
    uint32_t firstJoint;
    uint32_t jointCount;
} PrModelSourceSkin;

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

// What an attribute stream of a mesh's vertices means, beside its geometry.
enum {
    PrModelSemantic_TexCoord,           // a set of texture coordinates, v down the image
    PrModelSemantic_Color,              // a color in linear light, with an alpha where it has four values
    PrModelSemantic_Custom,             // a value that the source names, which only what reads the name knows
};
typedef uint8_t PrModelSemantic;

// An attribute stream that a part's vertices have: its name in the source, as glTF's TEXCOORD_0 or a primitive
// variable's name, what it means, which of the source's streams of the semantic it is, and the format that the source
// keeps it in. A mesh's reader gives it as floats.
typedef struct PrModelSourceStream {
    PrStringView name;
    PrModelSemantic semantic;
    uint8_t set;
    PrMeshFormat format;
} PrModelSourceStream;

// A part of a mesh: triangles that draw with one material.
typedef struct PrModelSourcePart {
    PrModelMaterialIndex material;      // or kPrModelMaterialNone for the default material
    PrBox3 bounds;                      // of the part's positions as they were bound

    // The part's morph targets, each the box of its displacements at a weight of 1 in the description's `morphBoxes`. A
    // part has fewer targets than its mesh where the rest displace nothing.
    uint32_t firstMorphBox;
    uint32_t morphTargetCount;

    // The part's attribute streams, a range of the description's `streams`.
    uint32_t firstStream;
    uint32_t streamCount;

    // The materials of the part in the source's variants, a range of the description's `variantMaterials`. A variant
    // that none of them names draws the part with its own material.
    uint32_t firstVariant;
    uint32_t variantCount;
} PrModelSourcePart;

// A variant's material of a part.
typedef struct PrModelSourceVariantMaterial {
    PrModelVariantIndex variant;
    PrModelMaterialIndex material;
} PrModelSourceVariantMaterial;

// A mesh: its parts, a range of the description's `parts`, its morph targets, and its morph weights at rest, a range of
// `weights`, which a node's own replace.
typedef struct PrModelSourceMesh {
    PrStringView name;
    PrModelPartIndex firstPart;
    uint32_t partCount;
    uint32_t morphTargetCount;          // the most of a part
    uint32_t firstWeight;
    uint32_t weightCount;
} PrModelSourceMesh;

// What a model asks of a mesh's reader: the attribute streams that it reads, by their names, and whether it reads the
// skin and the morph targets.
typedef struct PrModelMeshRequest {
    const PrStringView* PR_NULLABLE streams;
    uint32_t streamCount;
    bool skin;
    bool morphs;
} PrModelMeshRequest;

// What a part's vertices have, of what its mesh's data may hold.
enum {
    PrModelMeshPartFlag_Normals = 1u << 0,
    PrModelMeshPartFlag_Tangents = 1u << 1,

    // Each vertex is a corner of a triangle, so vertices that are equal may share one, whether or not they came from
    // one place of the source. Without it, the vertices are the source's, and two corners share a vertex only where the
    // source shares one.
    PrModelMeshPartFlag_Corners = 1u << 2,
};
typedef uint8_t PrModelMeshPartFlags;

// A part of a mesh's data: its triangles, a range of the data's triangles, and what its vertices have. Bit i of
// `streams` is set if its vertices have the request's stream i.
typedef struct PrModelMeshDataPart {
    uint32_t firstTriangle;
    uint32_t triangleCount;
    PrModelMeshPartFlags flags;
    uint32_t streams;
} PrModelMeshDataPart;

// The vertices and triangles of a mesh, as a source's reader writes them. Each array is null or has a value for each
// vertex, and is allocated with prMemoryAlloc, so that prModelMeshDataReset frees it.
typedef struct PrModelMeshData {
    uint32_t vertexCount;
    float* PR_NULLABLE positions;       // three floats each
    float* PR_NULLABLE normals;         // three, of unit length or of none for a part without normals
    float* PR_NULLABLE tangents;        // four: the tangent and the handedness of a bitangent pointing up the image

    // By the request's stream, its values, `streamWidths[i]` floats each, or null where no part has the stream. Each
    // array has the request's `streamCount` entries.
    float* PR_NULLABLE* PR_NULLABLE streams;
    uint32_t* PR_NULLABLE streamWidths;
    uint32_t streamCount;

    // `influenceCount` joints and their weights for each vertex, for a request of the skin.
    uint32_t* PR_NULLABLE joints;
    float* PR_NULLABLE weights;
    uint32_t influenceCount;

    // For a request of the morph targets, by vertex and then by target, the three floats of each target's displacement
    // of the position, the normal and the tangent. A displacement that a target lacks is zero.
    float* PR_NULLABLE morphPositions;
    float* PR_NULLABLE morphNormals;
    float* PR_NULLABLE morphTangents;
    uint32_t morphTargetCount;

    // The triangles, three vertices each, counterclockwise from their front, and the parts in the mesh's order.
    uint32_t* PR_NULLABLE triangles;
    uint32_t triangleCount;
    PrModelMeshDataPart* PR_NULLABLE parts;
    uint32_t partCount;
} PrModelMeshData;

// Frees the arrays of a mesh's data and empties it.
void prModelMeshDataReset(PrModelMeshData* data);

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// The shading model that a material's inputs are the inputs of, by its specification's names. A renderer's material
// type is chosen by it.
enum {
    PrModelShading_KhronosPbr,  // glTF's metallic-roughness model with Khronos's extensions, by their properties' names
    PrModelShading_OpenPbr,     // OpenPBR Surface, by its inputs' names
    PrModelShading_Preview,     // UsdPreviewSurface, by its inputs' names
    PrModelShading_Unlit,       // a color that no light changes (KHR_materials_unlit), its input `baseColorFactor`
    PrModelShading_Count,
};
typedef uint8_t PrModelShading;

// How a material's alpha is used.
enum {
    PrModelAlpha_Opaque,
    PrModelAlpha_Mask,          // absent where alpha is below the cutoff
    PrModelAlpha_Blend,
};
typedef uint8_t PrModelAlpha;

// What an input of a material is bound to.
enum {
    PrModelInputKind_Value,     // its value
    PrModelInputKind_Texture,   // a texture's texels, through a channel map
    PrModelInputKind_Attribute, // an attribute stream of the vertices, through a channel map
};
typedef uint8_t PrModelInputKind;

// A map of a texel's or an attribute's channels to an input's components: component k is the sum of each channel j
// times `weights[k][j]`, plus `offset[k]`.
typedef struct PrModelChannelMap {
    float weights[4][4];
    float offset[4];
} PrModelChannelMap;

// Returns the map that gives each component its own channel.
PrModelChannelMap prModelChannelMapIdentity(void);

// A named input of a material, by its shading model's name. A texture's coordinates are those of the stream of semantic
// TexCoord and set `texCoordSet`, through `transform`, after the translation, rotation and scale of its texture
// transform (`offset`, `rotation` and `scale`, as KHR_texture_transform orders them), which tracks of them replace. A
// texture's value may be tinted: multiplied by the attribute stream `tint`, component by component, where it is not
// empty. `value` is a value's, an attribute's where a vertex lacks it, and a tint's where a vertex lacks it.
typedef struct PrModelSourceInput {
    PrStringView name;
    PrModelInputKind kind;
    uint8_t width;              // its components, from 1 to 4
    float value[4];
    PrModelTextureIndex texture;
    uint8_t texCoordSet;
    float offset[2];
    float rotation;
    float scale[2];
    PrAffine2 transform;
    PrStringView attribute;     // the stream of an attribute, by its name
    PrStringView tint;
    PrModelChannelMap channels;
} PrModelSourceInput;

// Returns an input of `width` components named `name`, of `value`, with the identity's maps and transforms.
PrModelSourceInput prModelSourceInputValue(PrStringView name, uint32_t width, const float* value);

// A material: its shading model, its alpha and sidedness, and its inputs, a range of the description's `inputs`. An
// input that it lacks has its shading model's default.
typedef struct PrModelSourceMaterial {
    PrStringView name;
    PrModelShading shading;
    PrModelAlpha alpha;
    float alphaCutoff;
    bool doubleSided;
    uint32_t firstInput;
    uint32_t inputCount;
} PrModelSourceMaterial;

// How a texture is filtered and addressed. Default is the filter that the source leaves open, which is linear with
// anisotropy. A mip filter of None reads the first level alone.
enum {
    PrModelFilter_Default,
    PrModelFilter_None,
    PrModelFilter_Nearest,
    PrModelFilter_Linear,
};
typedef uint8_t PrModelFilter;

// How a texture's coordinates wrap past its edges.
enum {
    PrModelWrap_Repeat,
    PrModelWrap_Clamp,
    PrModelWrap_Mirror,
};
typedef uint8_t PrModelWrap;

// How a texture is sampled: its filters, and its wrapping along u and v.
typedef struct PrModelSampler {
    PrModelFilter magnify;
    PrModelFilter minify;
    PrModelFilter mip;
    PrModelWrap wrap[2];        // along u and v
} PrModelSampler;

// How a texture's texels are sampled: as color, through the Srgb form of their format, which decodes sRGB's transfer
// function, or as data, through the plain form, which reads values as they are stored. This is all that a model says of
// a texture's color: a color space that an asset declares for its textures, such as ACEScg, says only which of the two
// it is, linear values being data.
enum {
    PrModelTextureColor_Srgb,
    PrModelTextureColor_Data,
};
typedef uint8_t PrModelTextureColor;

// An image as a material samples it: the images that may show it, a range of the description's `textureImages`, of
// which a model draws the first whose media type its opener supports, the sampler, and how its texels are sampled.
typedef struct PrModelSourceTexture {
    uint32_t firstImage;
    uint32_t imageCount;
    PrModelSampler sampler;
    PrModelTextureColor color;
} PrModelSourceTexture;

// An image of the source: its name, its media type where the source states one, and where it is, as the source resolves
// it.
typedef struct PrModelSourceImage {
    PrStringView name;
    PrStringView mediaType;
    PrStringView location;
} PrModelSourceImage;

// What a source's image opens as: its bytes, which a model's texture opener reads, or a texture source that the source
// makes itself. One of them is set, with a reference that the caller releases.
typedef struct PrModelImageData {
    const PrBlobSource* PR_NULLABLE bytes;
    const PrTextureSource* PR_NULLABLE source;
} PrModelImageData;

// ---------------------------------------------------------------------------------------------------------------------
// Cameras, lights and environments

// How a camera projects the scene.
enum {
    PrModelProjection_Perspective,
    PrModelProjection_Orthographic,
};
typedef uint8_t PrModelProjection;

// A camera, looking down its node's -z with +y up. A perspective camera has a vertical field of view in radians, and an
// orthographic one half its extent along x and y in meters. The aspect ratio is zero where the source gives none, and
// the far distance infinity for none.
typedef struct PrModelSourceCamera {
    PrStringView name;
    PrModelProjection projection;
    float yfov;
    float aspectRatio;
    float xmag;
    float ymag;
    float znear;
    float zfar;
} PrModelSourceCamera;

// What a light is: a punctual light, or a light of an area, which emits along its node's -z.
enum {
    PrModelLightKind_Directional,
    PrModelLightKind_Point,
    PrModelLightKind_Spot,
    PrModelLightKind_Sphere,
    PrModelLightKind_Disk,
    PrModelLightKind_Rect,
    PrModelLightKind_Cylinder,
};
typedef uint8_t PrModelLightKind;

// The unit of a light's intensity: photometric, as KHR_lights_punctual gives it, which is candela for a light of a
// point and lux for a directional one, or the luminance of the light's surface in nits, as UsdLux gives it, which a
// directional light gives as its illuminance.
enum {
    PrModelLightUnit_Photometric,
    PrModelLightUnit_Luminance,
};
typedef uint8_t PrModelLightUnit;

// A light, with its shape in meters: a sphere's or a disk's radius, a rectangle's width along x and height along y, a
// cylinder's radius and its length along x. A light that is shaped emits within its cone, from the inner angle at full
// intensity to none at the outer, and a cone of an outer angle of pi does not shape it. A normalized light of an area
// emits as one of a unit area. The range is infinity for none.
typedef struct PrModelSourceLight {
    PrStringView name;
    PrModelLightKind kind;
    PrModelLightUnit unit;
    PrVec3 color;               // linear
    float intensity;
    float range;
    float innerConeAngle;
    float outerConeAngle;
    float radius;
    float width;
    float height;
    float length;
    bool normalized;
    bool treatAsPoint;          // a sphere of no radius, which emits as a point
} PrModelSourceLight;

// The environment that a source brings: radiance from every direction, the texels of an equirectangular image (its top
// pole along the frame's +y, the renderer's layout of such an image), or white without one, times a color and an
// intensity, at a node, through a frame within the node.
typedef struct PrModelSourceEnvironment {
    PrModelNodeIndex node;      // or kPrModelNodeNone for a source without one
    PrModelImageIndex image;    // or kPrModelImageNone
    PrVec3 color;
    float intensity;
    PrMatrix4 frame;            // a rotation
} PrModelSourceEnvironment;

// ---------------------------------------------------------------------------------------------------------------------
// Animations

// What a track sets: a property of a node, a mesh, a light, a camera or a material, which `object` names, and `element`
// within it: a morph target of one weight, or an input of a material, by its place among the material's.
enum {
    PrModelTrackTarget_Translation,
    PrModelTrackTarget_Rotation,
    PrModelTrackTarget_Scale,
    PrModelTrackTarget_Matrix,          // sixteen floats, by rows
    PrModelTrackTarget_Weights,         // a node's weights
    PrModelTrackTarget_Weight,          // one weight of a node
    PrModelTrackTarget_MeshWeights,     // a mesh's weights, which a node's own replace
    PrModelTrackTarget_MeshWeight,
    PrModelTrackTarget_Visible,         // nonzero to show the node
    PrModelTrackTarget_LightColor,
    PrModelTrackTarget_LightIntensity,
    PrModelTrackTarget_LightRange,
    PrModelTrackTarget_LightInnerConeAngle,
    PrModelTrackTarget_LightOuterConeAngle,
    PrModelTrackTarget_CameraYfov,
    PrModelTrackTarget_CameraAspectRatio,
    PrModelTrackTarget_CameraXmag,
    PrModelTrackTarget_CameraYmag,
    PrModelTrackTarget_CameraZnear,
    PrModelTrackTarget_CameraZfar,
    PrModelTrackTarget_Input,           // the value of a material's input
    PrModelTrackTarget_TextureOffset,   // the texture transform of a material's input
    PrModelTrackTarget_TextureRotation,
    PrModelTrackTarget_TextureScale,
};
typedef uint8_t PrModelTrackTarget;

// How a track's value moves between its keys: held, linearly, a rotation along the shorter arc, or along the cubic
// spline of each key's tangents, whose value is then its in-tangent, its value and its out-tangent.
enum {
    PrModelInterpolation_Step,
    PrModelInterpolation_Linear,
    PrModelInterpolation_CubicSpline,
};
typedef uint8_t PrModelInterpolation;

// A track: keyed values of one property. Its keys' times in seconds, ascending, are a range of the description's
// `keyTimes`, and its values, `width` floats a key, or three times that for a cubic spline, start at `firstValue` of
// `keyValues`. Before its first key a track holds the first key's value, and after its last the last's.
typedef struct PrModelSourceTrack {
    PrModelTrackTarget target;
    PrModelInterpolation interpolation;
    uint32_t object;
    uint32_t element;
    uint32_t width;
    uint32_t firstKey;
    uint32_t keyCount;
    uint32_t firstValue;
} PrModelSourceTrack;

// An animation: tracks that play together, a range of the description's `tracks`.
typedef struct PrModelSourceAnimation {
    PrStringView name;
    float duration;             // in seconds
    uint32_t firstTrack;
    uint32_t trackCount;
} PrModelSourceAnimation;

// ---------------------------------------------------------------------------------------------------------------------
// Descriptions

// Something of the asset that the source or a model does not draw as the asset means it: a feature's name, as an
// extension's (KHR_materials_volume) or a short name of the format's concept ("subdivision"), and a sentence about it.
typedef struct PrModelDiagnostic {
    PrStringView feature;
    PrStringView detail;
} PrModelDiagnostic;

// A source's description. Each array holds its count of elements, and stays valid and unchanged for the source's life.
typedef struct PrModelSourceInfo {
    const PrModelSourceNode* PR_NULLABLE nodes;
    uint32_t nodeCount;
    const PrModelNodeIndex* PR_NULLABLE roots;          // the scene's roots: a model draws them and what is under them
    uint32_t rootCount;
    const PrModelNodeIndex* PR_NULLABLE children;
    uint32_t childCount;
    const PrModelSourceDraw* PR_NULLABLE draws;
    uint32_t drawCount;
    const PrMatrix4* PR_NULLABLE instances;
    uint32_t instanceCount;
    const PrModelSourceMesh* PR_NULLABLE meshes;
    uint32_t meshCount;
    const PrModelSourcePart* PR_NULLABLE parts;
    uint32_t partCount;
    const PrModelSourceStream* PR_NULLABLE streams;
    uint32_t streamCount;
    const PrBox3* PR_NULLABLE morphBoxes;
    uint32_t morphBoxCount;
    const float* PR_NULLABLE weights;
    uint32_t weightCount;
    const PrModelSourceSkin* PR_NULLABLE skins;
    uint32_t skinCount;
    const PrModelNodeIndex* PR_NULLABLE joints;
    const PrMatrix4* PR_NULLABLE inverseBinds;
    uint32_t jointCount;
    const PrModelSourceMaterial* PR_NULLABLE materials;
    uint32_t materialCount;
    const PrModelSourceInput* PR_NULLABLE inputs;
    uint32_t inputCount;
    const PrModelSourceTexture* PR_NULLABLE textures;
    uint32_t textureCount;
    const PrModelImageIndex* PR_NULLABLE textureImages;
    uint32_t textureImageCount;
    const PrModelSourceImage* PR_NULLABLE images;
    uint32_t imageCount;
    const PrStringView* PR_NULLABLE variants;           // the variants' names
    uint32_t variantCount;
    const PrModelSourceVariantMaterial* PR_NULLABLE variantMaterials;
    uint32_t variantMaterialCount;
    const PrModelSourceCamera* PR_NULLABLE cameras;
    uint32_t cameraCount;
    const PrModelSourceLight* PR_NULLABLE lights;
    uint32_t lightCount;
    PrModelSourceEnvironment environment;
    const PrModelSourceAnimation* PR_NULLABLE animations;
    uint32_t animationCount;
    const PrModelSourceTrack* PR_NULLABLE tracks;
    uint32_t trackCount;
    const float* PR_NULLABLE keyTimes;
    uint32_t keyTimeCount;
    const float* PR_NULLABLE keyValues;
    uint32_t keyValueCount;
    const PrModelDiagnostic* PR_NULLABLE diagnostics;
    uint32_t diagnosticCount;
} PrModelSourceInfo;

// Appends a canonical text of a description to `out`: every element of every array, in order, with each string by its
// bytes and each float by its bits. Two descriptions are equal exactly when their texts are, which is how a test checks
// that an adapter's description of an asset converges.
void prModelSourceInfoWrite(const PrModelSourceInfo* info, PrTextBuffer* out);

// ---------------------------------------------------------------------------------------------------------------------
// Sources

// An asset described for a model, which a format's adapter creates (as pr/gltf_model.h and pr/usd_model.h do). A source
// has one owner: prModelCreate takes it, or prModelSourceDestroy frees it.
typedef struct PrModelSource PrModelSource;

// Frees a source that no model took.
void prModelSourceDestroy(PrModelSource* PR_NULLABLE source);

// Returns the source's description.
const PrModelSourceInfo* prModelSourceInfo(const PrModelSource* source);

// Reads the vertices and triangles of a mesh into `out`, which the caller resets (prModelMeshDataReset): its positions,
// its normals and tangents where its parts have them, the streams of the request where its parts have them, and the
// skin and the morph targets if the request asks for them. Each call reads the source again. Returns Io if a buffer
// that the mesh reads was not loaded, InvalidData for data that does not hold together, and Limit for a mesh whose
// counts do not fit 32 bits. On failure, `out` is empty.
PrStatus prModelSourceMeshRead(const PrModelSource* source, PrModelMeshIndex mesh, const PrModelMeshRequest* request,
    PrModelMeshData* out);

// Opens an image of the source into `out`, as its bytes or as a texture. Returns Io if the source has no bytes for it.
// On failure, `out` holds neither.
PrStatus prModelSourceImageOpen(const PrModelSource* source, PrModelImageIndex image, PrModelImageData* out);

// Appends to `out` the address of a node within the source's asset: the fragment that the format's addresses of the
// asset's parts end with, such as glTF's JSON Pointer `/nodes/3` or a USD prim's path `/World/Car`, which names the
// node of one instance of a prototype alone.
void prModelSourceNodeAddress(const PrModelSource* source, PrModelNodeIndex node, PrTextBuffer* out);

// Returns the node at an address within the source's asset, as prModelSourceNodeAddress writes it, or kPrModelNodeNone.
PrModelNodeIndex prModelSourceNodeFromAddress(const PrModelSource* source, PrStringView address);

// ---------------------------------------------------------------------------------------------------------------------
// Formats
//
// A model format makes sources of the resources of its format, as a texture format does textures. A program builds a
// set of formats from the adapters that it links and hands it to what opens models, which then depends on no format.

// Returns a provider of the resource at `location`, which a format resolved against the location of the resource that
// names it, or null where the host has no such resource. A format calls it on any thread.
typedef const PrBlobSource* PR_NULLABLE (*PrModelResolveFn)(void* PR_NULLABLE context, PrStringView location);

// What resolves the resources that an asset names, for a format: the function and its context.
typedef struct PrModelResolver {
    PrModelResolveFn resolve;
    void* PR_NULLABLE context;
} PrModelResolver;

// Called once with a source that the callee takes, or with the failure that left it null, on the thread that completed
// the format's reads, which may be the thread that opened it, during the call.
typedef void (*PrModelSourceOpenFn)(void* PR_NULLABLE context, PrStatus status, PrModelSource* PR_NULLABLE source);

// One format of models: its adapter's functions and their context, which outlives what it opens. `supportsTypeFn` says
// if it reads the media type that a resource was classified as (pr/mime_sniff.h). `openFn` reads the resource that
// `bytes` provides at `location`, and what it names through `resolver` at locations resolved against `location`, and
// calls `done` once with a source of it, or with NotSupported to leave the bytes to the next format, or with the
// failure that refuses them. The resolver outlives the call to `done`, and a source resolves what it reads later when
// it is made.
typedef struct PrModelFormat {
    bool (*supportsTypeFn)(void* PR_NULLABLE context, PrStringView type);
    void (*openFn)(void* PR_NULLABLE context, const PrBlobSource* bytes, PrStringView location,
        const PrModelResolver* resolver, PrModelSourceOpenFn done, void* PR_NULLABLE doneContext);
    void* PR_NULLABLE context;
} PrModelFormat;

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
