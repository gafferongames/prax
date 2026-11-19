// glTF 2.0: an asset read into a document of its scenes, nodes, meshes, materials, animations and binary data.
//
// A document is the asset as the specification describes it, with every default filled in. It decodes no images and
// builds nothing for a GPU. Objects are records in arrays, and one object names another by its index in the array of
// its kind, so an index of the asset's JSON is the same index here.
//
// What a document guarantees. prGltfCreate checks the asset against the specification and makes a document only if
// every check passes, so a consumer uses a document without checking it again:
//
// - Every index names an object of its kind, or is kPrGltfNone where the specification lets the property be absent.
// - Every enumeration holds one of its members, and every number is finite and within the range that the
//   specification's schema gives its property.
// - A buffer view lies within its buffer, and an accessor within its buffer view, as do the indices and the values of
//   a sparse accessor. Their offsets are multiples of the size of a component. A buffer that has bytes has at least
//   `byteLength` of them.
// - The attributes of a primitive, and of each of its morph targets, have the element type and component type that the
//   specification gives their semantic, and each has `vertexCount` elements.
// - A primitive's number of vertices suits its mode. Each primitive of a mesh has the mesh's number of morph targets
//   or none, and each list of weights has that number.
// - The nodes form trees: a node has at most one parent and is not its own ancestor. A scene lists only roots.
// - A node with a skin has a mesh whose primitives all have joints and weights.
// - A channel's output accessor has the element type that the channel's path needs, and holds exactly the values of
//   its sampler's keyframes. An animation has one channel for a property, at most, and a channel of a pointer names a
//   property that the asset defines.
// - A node's instances have one element of each of their attributes each. A variant is in one mapping of a primitive at
//   most.
//
// Four guarantees depend on the bytes of buffers, and hold wherever the buffers that they read have bytes:
//
// - The indices of a primitive are less than its `vertexCount`.
// - The indices of a sparse accessor increase strictly and are less than the accessor's `count`.
// - The joints of a vertex are less than the number of joints of every skin that a node draws the vertex's mesh with.
// - The times of an animation sampler are finite, are not negative, and increase strictly.
//
// A compressed buffer view's stream decodes to exactly its elements, which are its bytes, and a fallback buffer is
// named only by compressed views. A primitive's Draco stream decodes to a mesh of exactly the elements of the accessors
// that it fills, each of its accessor's component type and number of components.
//
// A document does not check what the values mean: that a normal or a rotation has unit length, that weights sum to one,
// that an accessor's `min` and `max` bound its data, that floats in a buffer are finite, that a node's matrix
// decomposes, that a skin's skeleton is an ancestor of its joints, or that an image's bytes are an image. Nor does it
// check which kinds of data share a buffer view.
//
// A document is immutable, so any number of threads may read it.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/byte_view.h"
#include "pr/geom.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Documents

// A glTF asset, read and checked.
typedef struct PrGltf PrGltf;

// An object of a document, by its position in the array of its kind.
typedef uint32_t PrGltfBufferIndex;
typedef uint32_t PrGltfBufferViewIndex;
typedef uint32_t PrGltfAccessorIndex;
typedef uint32_t PrGltfImageIndex;
typedef uint32_t PrGltfSamplerIndex;
typedef uint32_t PrGltfTextureIndex;
typedef uint32_t PrGltfMaterialIndex;
typedef uint32_t PrGltfMeshIndex;
typedef uint32_t PrGltfPrimitiveIndex;
typedef uint32_t PrGltfCameraIndex;
typedef uint32_t PrGltfLightIndex;
typedef uint32_t PrGltfNodeIndex;
typedef uint32_t PrGltfSkinIndex;
typedef uint32_t PrGltfSceneIndex;
typedef uint32_t PrGltfAnimationIndex;
typedef uint32_t PrGltfAnimationSamplerIndex;
typedef uint32_t PrGltfAnimationChannelIndex;

// The index of a property that the asset leaves undefined.
PR_CONSTANT uint32_t kPrGltfNone = 0xffffffffu;

// A list of indices or of numbers that an object holds: `count` items from `first` in the document's storage for lists.
// The field's comment says which kind the list holds. prGltfListIndices and prGltfListFloats return the items.
typedef struct PrGltfList {
    uint32_t first;
    uint32_t count;
} PrGltfList;

// What an asset names by a URI outside its own file.
enum {
    PrGltfExternal_Buffer,
    PrGltfExternal_Image,
};
typedef uint8_t PrGltfExternal;

// Loads the bytes that the asset names by `uri`, exactly as the asset gives it: a relative path, which may be
// percent-encoded, unless the asset goes beyond the specification (glTF-2.0/#uris-overview). The host resolves it
// against the asset's own location. Returns a blob that the document takes over and releases when it is destroyed, or
// null if the bytes cannot be had or are not wanted.
typedef PrBlob* PR_NULLABLE (*PrGltfLoadFn)(void* PR_NULLABLE context, PrGltfExternal kind, PrStringView uri);

// The host services that reading uses.
typedef struct PrGltfOptions {
    // Loads the buffers and images that the asset names by URI. Without it, they have no bytes.
    PrGltfLoadFn PR_NULLABLE load;
    void* PR_NULLABLE loadContext;

    // Receives a sentence that says which property of the asset failed which check, if reading fails.
    PrTextBuffer* PR_NULLABLE diagnostics;
} PrGltfOptions;

// Returns options with no services.
PrGltfOptions prGltfOptionsDefault(void);

// Reads the glTF asset in the file at `path`, as glTF JSON or binary glTF, into a document at `*out`, which the caller
// destroys (prGltfDestroy). The buffers and images that the asset names by URI are read from the files that the URIs
// name relative to the asset's folder. One that cannot be read has no bytes. The call reads every file before it
// returns. Returns Io if the file cannot be read, and otherwise the status of prGltfCreate, whose sentence about a
// failure goes to `diagnostics` if it is given. On failure, `*out` is null.
PrStatus prGltfFileRead(PrStringView path, PrTextBuffer* PR_NULLABLE diagnostics, PrGltf* PR_NULLABLE* PR_NONNULL out);

// Reads the asset in `file`, which is glTF JSON or binary glTF, as its first bytes say. The document keeps a reference
// to `file` if it is binary glTF with a binary chunk, and otherwise keeps nothing of it. The options are used only
// during the call, and null options mean the defaults.
//
// A buffer or an image that the asset embeds, in the binary chunk or in a data: URI, always has its bytes. One that the
// asset names by URI has bytes only if `load` returns them. A document whose external buffers have no bytes still
// describes the whole asset, so a host that cannot load during the call reads the asset once to learn the URIs
// (PrGltfBuffer::uri, PrGltfImage::uri) and again once it has their bytes.
//
// Returns Syntax for text that is not JSON, InvalidData for an asset that breaks a rule of the specification, and
// NotSupported for an asset of another major version of glTF, or one that requires a later minor version or an
// extension that prGltfExtensionIsSupported does not accept. On failure, `*out` is null.
PrStatus prGltfCreate(PrBlob* file, const PrGltfOptions* PR_NULLABLE options, PrGltf* PR_NULLABLE* PR_NONNULL out);

// Frees the document, and releases the blobs that it holds.
void prGltfDestroy(PrGltf* PR_NULLABLE gltf);

// What the asset says about itself (glTF-2.0/#asset). A string that the asset omits is the null view.
typedef struct PrGltfAsset {
    PrStringView version;
    PrStringView minVersion;
    PrStringView generator;
    PrStringView copyright;
} PrGltfAsset;

// Returns the asset's description of itself. Its strings are valid until the document is destroyed, as is every string
// and every array that a document returns.
PrGltfAsset prGltfAsset(const PrGltf* gltf);

// Returns the indices of `list`, or its numbers: `list.count` of them.
const uint32_t* PR_NULLABLE prGltfListIndices(const PrGltf* gltf, PrGltfList list);
const float* PR_NULLABLE prGltfListFloats(const PrGltf* gltf, PrGltfList list);

// ---------------------------------------------------------------------------------------------------------------------
// Extensions

// Returns true if the module reads the extension `name`, so that an asset may require it. Each extension that the
// module reads adds to the records that the core defines, and the fields that it adds name it.
bool prGltfExtensionIsSupported(PrStringView name);

// Return the number of extensions that the asset says it uses (`extensionsUsed`), and the name of each. The document
// holds nothing of an extension that prGltfExtensionIsSupported does not accept, so these names tell a consumer what it
// is not shown.
uint32_t prGltfUsedExtensionCount(const PrGltf* gltf);
PrStringView prGltfUsedExtension(const PrGltf* gltf, uint32_t index);

// ---------------------------------------------------------------------------------------------------------------------
// Binary data (glTF-2.0/#binary-data-storage)

// A run of bytes that buffer views divide.
typedef struct PrGltfBuffer {
    PrStringView name;
    uint64_t byteLength;

    // The URI that names the buffer's bytes. Empty for a data: URI, whose bytes the document holds instead. The null
    // view if the asset gives the buffer no URI, as it does for the binary chunk of binary glTF.
    PrStringView uri;

    // The buffer's bytes, of which the first `byteLength` are the buffer. The null view if the buffer has no bytes: its
    // URI was not loaded, or the asset gives it no source.
    PrByteView bytes;

    // True if the asset marks the buffer as the fallback of compressed buffer views (EXT_meshopt_compression,
    // KHR_meshopt_compression). Only those views name it, and a reader of the extension needs none of its bytes, so the
    // module loads none.
    bool fallback;
} PrGltfBuffer;

// What a buffer view's bytes are for, if the asset says.
enum {
    PrGltfBufferTarget_None,
    PrGltfBufferTarget_Vertices,        // ARRAY_BUFFER
    PrGltfBufferTarget_Indices,         // ELEMENT_ARRAY_BUFFER
};
typedef uint8_t PrGltfBufferTarget;

// How meshopt compression encodes a buffer view's bytes: as a sequence of elements, as the indices of triangles, or as
// other indices.
enum {
    PrGltfMeshoptMode_Attributes,
    PrGltfMeshoptMode_Triangles,
    PrGltfMeshoptMode_Indices,
};
typedef uint8_t PrGltfMeshoptMode;

// What meshopt compression applies to each element after it decodes the attributes: nothing, unit vectors in octahedral
// form, quaternions without their largest component, floats as exponents and mantissas, or colors in YCoCg.
enum {
    PrGltfMeshoptFilter_None,
    PrGltfMeshoptFilter_Octahedral,
    PrGltfMeshoptFilter_Quaternion,
    PrGltfMeshoptFilter_Exponential,
    PrGltfMeshoptFilter_Color,
};
typedef uint8_t PrGltfMeshoptFilter;

// The compressed form of a buffer view's bytes (EXT_meshopt_compression, KHR_meshopt_compression): `count` elements of
// `byteStride` bytes, compressed into a range of another buffer.
typedef struct PrGltfMeshopt {
    PrGltfBufferIndex buffer;   // kPrGltfNone if the view is not compressed
    PrGltfMeshoptMode mode;
    PrGltfMeshoptFilter filter;
    bool v1;                    // true for KHR_meshopt_compression, whose attributes may be in either version
    uint16_t byteStride;
    uint32_t count;
    uint64_t byteOffset;
    uint64_t byteLength;
} PrGltfMeshopt;

// A range of a buffer.
typedef struct PrGltfBufferView {
    PrStringView name;
    PrGltfBufferIndex buffer;
    PrGltfBufferTarget target;
    uint8_t byteStride;         // bytes from one vertex to the next, or zero if elements are tightly packed
    uint64_t byteOffset;
    uint64_t byteLength;

    // The compressed form of the view's bytes, which the module decodes when it reads the asset. The view's own buffer
    // is then a fallback that a reader of the extension does not read, and prGltfBufferViewBytes returns the decoded
    // bytes.
    PrGltfMeshopt meshopt;
} PrGltfBufferView;

// The type of an accessor's components.
enum {
    PrGltfComponentType_Byte,
    PrGltfComponentType_UnsignedByte,
    PrGltfComponentType_Short,
    PrGltfComponentType_UnsignedShort,
    PrGltfComponentType_UnsignedInt,
    PrGltfComponentType_Float,
};
typedef uint8_t PrGltfComponentType;

// The shape of an accessor's elements.
enum {
    PrGltfElementType_Scalar,
    PrGltfElementType_Vec2,
    PrGltfElementType_Vec3,
    PrGltfElementType_Vec4,
    PrGltfElementType_Mat2,
    PrGltfElementType_Mat3,
    PrGltfElementType_Mat4,
};
typedef uint8_t PrGltfElementType;

// The elements of a sparse accessor that replace elements of its source (glTF-2.0/#sparse-accessors). The indices are
// `count` tightly packed components of `indicesComponentType`, and the values are `count` tightly packed elements of
// the accessor's own type.
typedef struct PrGltfSparse {
    uint32_t count;             // zero if the accessor is not sparse
    PrGltfComponentType indicesComponentType;
    PrGltfBufferViewIndex indicesBufferView;
    PrGltfBufferViewIndex valuesBufferView;
    uint64_t indicesByteOffset;
    uint64_t valuesByteOffset;
} PrGltfSparse;

// A sequence of typed elements (glTF-2.0/#accessors).
typedef struct PrGltfAccessor {
    PrStringView name;
    PrGltfBufferViewIndex bufferView;   // kPrGltfNone if the source of the elements is zeros

    // True if a primitive's Draco stream gives the elements (KHR_draco_mesh_compression). The document then reads the
    // elements that the stream decodes to, tightly packed, and neither `bufferView`, which the asset may give for a
    // reader that does not read the extension, nor zeros. The sparse elements replace elements of those.
    bool draco;
    uint32_t count;                     // the number of elements, at least one
    uint64_t byteOffset;                // of the first element within the buffer view
    PrGltfComponentType componentType;
    PrGltfElementType type;
    bool normalized;                    // true if integer components stand for numbers in [0, 1] or [-1, 1]

    // The least and greatest value of each component, as the asset states them: floats, one per component, or empty.
    // The bounds of a normalized accessor are those of its integers.
    PrGltfList min;
    PrGltfList max;

    PrGltfSparse sparse;
} PrGltfAccessor;

// Return the number of buffers, buffer views or accessors, and the array of them.
uint32_t prGltfBufferCount(const PrGltf* gltf);
const PrGltfBuffer* PR_NULLABLE prGltfBuffers(const PrGltf* gltf);
uint32_t prGltfBufferViewCount(const PrGltf* gltf);
const PrGltfBufferView* PR_NULLABLE prGltfBufferViews(const PrGltf* gltf);
uint32_t prGltfAccessorCount(const PrGltf* gltf);
const PrGltfAccessor* PR_NULLABLE prGltfAccessors(const PrGltf* gltf);

// Returns the bytes of a buffer view, or the null view if its buffer has no bytes. The bytes of a compressed view are
// those that its compressed form decodes to, or the null view if the buffer of that form has no bytes.
PrByteView prGltfBufferViewBytes(const PrGltf* gltf, PrGltfBufferViewIndex bufferView);

// Decodes `source`, a stream of meshopt compression of the form that `meshopt` describes, into `out`, which holds its
// `count` elements of `byteStride` bytes, and applies its filter. Returns false if the stream is not one that the
// extension defines, or does not hold exactly that many elements, and then `out` holds no meaning. Reading an asset
// decodes its compressed views with this, so a host calls it only for a stream of its own.
bool prGltfMeshoptDecode(PrByteView source, const PrGltfMeshopt* meshopt, uint8_t* out);

// Return the size of a component in bytes, and the number of components in an element.
uint32_t prGltfComponentTypeSize(PrGltfComponentType type);
uint32_t prGltfElementTypeComponentCount(PrGltfElementType type);

// Returns the size of an element in bytes. Each column of a matrix starts at a multiple of four bytes, so a matrix of
// small components holds padding (glTF-2.0/#accessor-element-size).
uint32_t prGltfElementSize(PrGltfElementType type, PrGltfComponentType componentType);

// Writes to `min` and `max` the bounds that an accessor states, one float for each component, as the values that the
// elements stand for: a normalized integer converts as prGltfAccessorReadFloats converts it, so the bounds of quantized
// data (KHR_mesh_quantization) are in the space of its floats. Returns false, and writes nothing, if the accessor
// states no bounds.
bool prGltfAccessorBounds(const PrGltf* gltf, PrGltfAccessorIndex accessor, float* min, float* max);

// Returns the number of bytes from one element of an accessor to the next in its buffer view: the buffer view's stride,
// or the size of an element.
uint32_t prGltfAccessorStride(const PrGltf* gltf, PrGltfAccessorIndex accessor);

// Unpack `count` elements of an accessor, from element `first`, into `out`, which holds a number for each component of
// each element. Stride, the padding of matrices and sparse substitution are applied, so `out` is the elements as the
// specification defines them, a matrix in column-major order. The range must lie within the accessor.
//
// As floats, a normalized integer is converted as the specification gives (glTF-2.0/#accessor-data-types), and any
// other integer becomes the nearest float. As integers, an unsigned component is its value, a signed component is its
// value in two's complement, which a cast to int32_t recovers, and a float is truncated toward zero and clamped to what
// a uint32_t holds.
//
// Returns false, and fills `out` with zeros, if a buffer that the accessor reads has no bytes.
bool prGltfAccessorReadFloats(const PrGltf* gltf, PrGltfAccessorIndex accessor, uint32_t first, uint32_t count, float* out);
bool prGltfAccessorReadIntegers(
    const PrGltf* gltf, PrGltfAccessorIndex accessor, uint32_t first, uint32_t count, uint32_t* out);

// ---------------------------------------------------------------------------------------------------------------------
// Images, samplers and textures (glTF-2.0/#texture-data)

// An image in its encoded form. The module does not decode it.
typedef struct PrGltfImage {
    PrStringView name;
    PrStringView uri;                   // the null view for a buffer view, and empty for a data: URI

    // The image's media type: the asset's `mimeType`, or else the type of a data: URI, or else "image/png",
    // "image/jpeg", "image/webp" or "image/ktx2" if the bytes begin as that format does (glTF-2.0/#images). The null
    // view if none of these says.
    PrStringView mimeType;

    PrGltfBufferViewIndex bufferView;   // kPrGltfNone for an image that a URI gives

    // The encoded image. The null view if its URI was not loaded, or its buffer has no bytes.
    PrByteView bytes;
} PrGltfImage;

// How a texture is filtered when it is magnified or minified. The mipmap filters are for minification only.
enum {
    PrGltfFilter_None,                  // the asset leaves the filter to the implementation
    PrGltfFilter_Nearest,
    PrGltfFilter_Linear,
    PrGltfFilter_NearestMipmapNearest,
    PrGltfFilter_LinearMipmapNearest,
    PrGltfFilter_NearestMipmapLinear,
    PrGltfFilter_LinearMipmapLinear,
};
typedef uint8_t PrGltfFilter;

// How a texture coordinate outside [0, 1] is wrapped.
enum {
    PrGltfWrap_Repeat,
    PrGltfWrap_ClampToEdge,
    PrGltfWrap_MirroredRepeat,
};
typedef uint8_t PrGltfWrap;

// How a texture is filtered and wrapped (glTF-2.0/#samplers).
typedef struct PrGltfSampler {
    PrStringView name;
    PrGltfFilter magFilter;
    PrGltfFilter minFilter;
    PrGltfWrap wrapS;
    PrGltfWrap wrapT;
} PrGltfSampler;

// An image and the sampler that it is read with.
typedef struct PrGltfTexture {
    PrStringView name;
    PrGltfSamplerIndex sampler;         // kPrGltfNone for repeat wrapping and filters left to the implementation
    PrGltfImageIndex source;            // the image of the core property, or kPrGltfNone if the asset gives none

    // Every image that the texture may be drawn from, image indices: the image of each extension that gives the texture
    // another format (KHR_texture_basisu, then EXT_texture_webp), and then `source`. Each is the same picture, so a
    // consumer draws the first whose media type it decodes. Empty if the texture has no image.
    PrGltfList sources;
} PrGltfTexture;

// Return the number of images, samplers or textures, and the array of them.
uint32_t prGltfImageCount(const PrGltf* gltf);
const PrGltfImage* PR_NULLABLE prGltfImages(const PrGltf* gltf);
uint32_t prGltfSamplerCount(const PrGltf* gltf);
const PrGltfSampler* PR_NULLABLE prGltfSamplers(const PrGltf* gltf);
uint32_t prGltfTextureCount(const PrGltf* gltf);
const PrGltfTexture* PR_NULLABLE prGltfTextures(const PrGltf* gltf);

// ---------------------------------------------------------------------------------------------------------------------
// Materials (glTF-2.0/#materials)

// A transform of texture coordinates, from KHR_texture_transform. A coordinate is scaled, then rotated by `rotation`
// radians counter-clockwise as the texture is seen, with its v axis pointing down, then offset. The identity is an
// offset of zero, no rotation and a scale of one.
typedef struct PrGltfTextureTransform {
    float offset[2];
    float rotation;
    float scale[2];
} PrGltfTextureTransform;

// A material's use of a texture.
typedef struct PrGltfTextureInfo {
    PrGltfTextureIndex texture;         // kPrGltfNone if the material has no such texture
    uint32_t texCoord;                  // the set n of the TEXCOORD_n attribute that addresses the texture
    PrGltfTextureTransform transform;   // the identity unless the asset gives one
    bool hasTransform;                  // true if the asset gives the texture a KHR_texture_transform
} PrGltfTextureInfo;

// How a material's alpha is used (glTF-2.0/#alpha-coverage).
enum {
    PrGltfAlphaMode_Opaque,
    PrGltfAlphaMode_Mask,
    PrGltfAlphaMode_Blend,
};
typedef uint8_t PrGltfAlphaMode;

// The extensions of Khronos's material model that a material may have, as bits. Each adds to the metallic-roughness
// material, except unlit, which replaces its lighting, and specular-glossiness, an archived extension that replaces its
// parameters. A material without an extension's bit holds that extension's defaults, which leave the material as the
// core defines it.
enum {
    PrGltfMaterialExtensionFlag_Clearcoat = 1u << 0,            // KHR_materials_clearcoat
    PrGltfMaterialExtensionFlag_Sheen = 1u << 1,                // KHR_materials_sheen
    PrGltfMaterialExtensionFlag_Transmission = 1u << 2,         // KHR_materials_transmission
    PrGltfMaterialExtensionFlag_Volume = 1u << 3,               // KHR_materials_volume
    PrGltfMaterialExtensionFlag_Ior = 1u << 4,                  // KHR_materials_ior
    PrGltfMaterialExtensionFlag_Specular = 1u << 5,             // KHR_materials_specular
    PrGltfMaterialExtensionFlag_Iridescence = 1u << 6,          // KHR_materials_iridescence
    PrGltfMaterialExtensionFlag_Anisotropy = 1u << 7,           // KHR_materials_anisotropy
    PrGltfMaterialExtensionFlag_Dispersion = 1u << 8,           // KHR_materials_dispersion
    PrGltfMaterialExtensionFlag_DiffuseTransmission = 1u << 9,  // KHR_materials_diffuse_transmission
    PrGltfMaterialExtensionFlag_EmissiveStrength = 1u << 10,    // KHR_materials_emissive_strength
    PrGltfMaterialExtensionFlag_Unlit = 1u << 11,               // KHR_materials_unlit
    PrGltfMaterialExtensionFlag_SpecularGlossiness = 1u << 12,  // KHR_materials_pbrSpecularGlossiness
};
typedef uint32_t PrGltfMaterialExtensionFlags;

// Returns the flag of the material extension `name`, or zero if it names no extension of the material model.
PrGltfMaterialExtensionFlags prGltfMaterialExtensionFromName(PrStringView name);

// A clear coat over the material (KHR_materials_clearcoat). The intensity is read from red, and the roughness from
// green.
typedef struct PrGltfClearcoat {
    float factor;                       // the layer's intensity, from 0 for none to 1
    PrGltfTextureInfo texture;
    float roughnessFactor;
    PrGltfTextureInfo roughnessTexture;
    PrGltfTextureInfo normalTexture;    // the layer's own normals, or none for the material's
    float normalScale;
} PrGltfClearcoat;

// A sheen of fibers over the material (KHR_materials_sheen). The color texture is sRGB, and roughness is read from
// alpha.
typedef struct PrGltfSheen {
    float colorFactor[3];               // linear RGB, black for no sheen
    PrGltfTextureInfo colorTexture;
    float roughnessFactor;
    PrGltfTextureInfo roughnessTexture;
} PrGltfSheen;

// Light that passes through the surface (KHR_materials_transmission), with the fraction read from red.
typedef struct PrGltfTransmission {
    float factor;
    PrGltfTextureInfo texture;
} PrGltfTransmission;

// The volume that the surface encloses (KHR_materials_volume). The thickness is read from green.
typedef struct PrGltfVolume {
    float thicknessFactor;              // in the mesh's space; zero for a thin wall
    PrGltfTextureInfo thicknessTexture;
    float attenuationDistance;          // in the world's space, or infinity for no attenuation
    float attenuationColor[3];          // linear RGB
} PrGltfVolume;

// The strength and the color of the specular reflection of a dielectric (KHR_materials_specular). The strength is read
// from alpha, and the color texture is sRGB.
typedef struct PrGltfSpecular {
    float factor;
    PrGltfTextureInfo texture;
    float colorFactor[3];               // linear RGB, which may exceed 1
    PrGltfTextureInfo colorTexture;
} PrGltfSpecular;

// A thin film over the material (KHR_materials_iridescence). The intensity is read from red, and the thickness from
// green, between the minimum and the maximum.
typedef struct PrGltfIridescence {
    float factor;
    PrGltfTextureInfo texture;
    float ior;                          // of the film
    float thicknessMinimum;             // in nanometers
    float thicknessMaximum;
    PrGltfTextureInfo thicknessTexture;
} PrGltfIridescence;

// Roughness that differs along one direction of the surface (KHR_materials_anisotropy). The texture holds a direction
// in red and green, and a strength in blue.
typedef struct PrGltfAnisotropy {
    float strength;
    float rotation;                     // radians counter-clockwise from the tangent
    PrGltfTextureInfo texture;
} PrGltfAnisotropy;

// Light that the surface scatters through itself (KHR_materials_diffuse_transmission). The fraction is read from alpha,
// and the color texture is sRGB.
typedef struct PrGltfDiffuseTransmission {
    float factor;
    PrGltfTextureInfo texture;
    float colorFactor[3];               // linear RGB
    PrGltfTextureInfo colorTexture;
} PrGltfDiffuseTransmission;

// The parameters of the specular-glossiness model (KHR_materials_pbrSpecularGlossiness), which a material with the
// extension is drawn with in place of its metallic-roughness parameters. The diffuse texture is sRGB, as is the
// specular color in the RGB of the second texture, whose alpha is glossiness.
typedef struct PrGltfSpecularGlossiness {
    float diffuseFactor[4];             // linear RGB and alpha
    PrGltfTextureInfo diffuseTexture;
    float specularFactor[3];            // linear RGB
    float glossinessFactor;
    PrGltfTextureInfo specularGlossinessTexture;
} PrGltfSpecularGlossiness;

// A metallic-roughness material, with the extensions that add to it. A factor multiplies the value read from its
// texture, and stands alone if there is no texture.
typedef struct PrGltfMaterial {
    PrStringView name;

    float baseColorFactor[4];           // linear RGB and alpha
    float metallicFactor;
    float roughnessFactor;
    PrGltfTextureInfo baseColorTexture;
    PrGltfTextureInfo metallicRoughnessTexture; // roughness in green and metalness in blue

    PrGltfTextureInfo normalTexture;
    float normalScale;                  // scales the x and y of the normal read from `normalTexture`
    PrGltfTextureInfo occlusionTexture; // occlusion in red
    float occlusionStrength;            // how much of the occlusion applies, from 0 to 1

    PrGltfTextureInfo emissiveTexture;
    float emissiveFactor[3];            // linear RGB
    float emissiveStrength;             // multiplies the emission; 1 unless KHR_materials_emissive_strength gives it

    PrGltfAlphaMode alphaMode;
    float alphaCutoff;                  // in mask mode, the alpha below which a surface is not drawn
    bool doubleSided;

    // The extensions that the asset gives the material. With Unlit, the base color is the surface's color.
    PrGltfMaterialExtensionFlags extensions;

    // The index of refraction (KHR_materials_ior), 1.5 by default. Zero stands for an infinite index, which makes the
    // Fresnel term one at every angle.
    float ior;

    // The strength of dispersion, 20 over the Abbe number, or zero for none (KHR_materials_dispersion).
    float dispersion;

    PrGltfClearcoat clearcoat;
    PrGltfSheen sheen;
    PrGltfTransmission transmission;
    PrGltfVolume volume;
    PrGltfSpecular specular;
    PrGltfIridescence iridescence;
    PrGltfAnisotropy anisotropy;
    PrGltfDiffuseTransmission diffuseTransmission;
    PrGltfSpecularGlossiness specularGlossiness;
} PrGltfMaterial;

// Return the number of materials, and the array of them.
uint32_t prGltfMaterialCount(const PrGltf* gltf);
const PrGltfMaterial* PR_NULLABLE prGltfMaterials(const PrGltf* gltf);

// Returns the affine transform that `transform` applies to a texture coordinate.
PrAffine2 prGltfTextureTransformAffine(const PrGltfTextureTransform* transform);

// The textures of a material: the core's five, and those of the extensions of the material model, which a material
// reads only if it has the extension (prGltfMaterialTextureExtension).
enum {
    PrGltfMaterialTexture_BaseColor,
    PrGltfMaterialTexture_MetallicRoughness,
    PrGltfMaterialTexture_Normal,
    PrGltfMaterialTexture_Occlusion,
    PrGltfMaterialTexture_Emissive,
    PrGltfMaterialTexture_Clearcoat,
    PrGltfMaterialTexture_ClearcoatRoughness,
    PrGltfMaterialTexture_ClearcoatNormal,
    PrGltfMaterialTexture_SheenColor,
    PrGltfMaterialTexture_SheenRoughness,
    PrGltfMaterialTexture_Transmission,
    PrGltfMaterialTexture_Thickness,
    PrGltfMaterialTexture_Specular,
    PrGltfMaterialTexture_SpecularColor,
    PrGltfMaterialTexture_Iridescence,
    PrGltfMaterialTexture_IridescenceThickness,
    PrGltfMaterialTexture_Anisotropy,
    PrGltfMaterialTexture_DiffuseTransmission,
    PrGltfMaterialTexture_DiffuseTransmissionColor,
    PrGltfMaterialTexture_Diffuse,
    PrGltfMaterialTexture_SpecularGlossiness,
    PrGltfMaterialTexture_Count,
};
typedef uint8_t PrGltfMaterialTexture;

// Returns the material's use of one of its textures.
const PrGltfTextureInfo* prGltfMaterialTextureInfo(const PrGltfMaterial* material, PrGltfMaterialTexture texture);

// Returns the extension that gives a material the texture, or zero for a texture of the core.
PrGltfMaterialExtensionFlags prGltfMaterialTextureExtension(PrGltfMaterialTexture texture);

// Returns true if the values of the texture are colors in sRGB, as its specification says, and false if they are linear
// data. The alpha of a texture is linear in both cases.
bool prGltfMaterialTextureIsSrgb(PrGltfMaterialTexture texture);

// ---------------------------------------------------------------------------------------------------------------------
// Meshes (glTF-2.0/#meshes)

// What a vertex attribute holds. Semantics that take a set are numbered: TEXCOORD_0, TEXCOORD_1. Custom is any name
// that the specification does not define, such as an application's own, which begins with an underscore.
enum {
    PrGltfSemantic_Position,
    PrGltfSemantic_Normal,
    PrGltfSemantic_Tangent,
    PrGltfSemantic_TexCoord,
    PrGltfSemantic_Color,
    PrGltfSemantic_Joints,
    PrGltfSemantic_Weights,
    PrGltfSemantic_Custom,
};
typedef uint8_t PrGltfSemantic;

// One attribute of a primitive or of a morph target.
typedef struct PrGltfAttribute {
    PrStringView name;                  // as the asset spells it, such as "TEXCOORD_1"
    PrGltfSemantic semantic;
    uint32_t set;                       // the n of a numbered semantic, and zero for any other
    PrGltfAccessorIndex accessor;
} PrGltfAttribute;

// The displacements that one morph target adds to a primitive's attributes: a range of the document's attributes
// (prGltfAttributes).
typedef struct PrGltfMorphTarget {
    uint32_t attributeFirst;
    uint32_t attributeCount;
} PrGltfMorphTarget;

// How a primitive's vertices are assembled (glTF-2.0/#meshes-topology).
enum {
    PrGltfMode_Points,
    PrGltfMode_Lines,
    PrGltfMode_LineLoop,
    PrGltfMode_LineStrip,
    PrGltfMode_Triangles,
    PrGltfMode_TriangleStrip,
    PrGltfMode_TriangleFan,
};
typedef uint8_t PrGltfMode;

// Geometry drawn with one material.
typedef struct PrGltfPrimitive {
    // The primitive's attributes: a range of the document's attributes (prGltfAttributes).
    uint32_t attributeFirst;
    uint32_t attributeCount;

    // The primitive's morph targets: a range of the document's morph targets (prGltfMorphTargets).
    uint32_t morphTargetFirst;
    uint32_t morphTargetCount;

    PrGltfAccessorIndex indices;        // kPrGltfNone if the vertices are drawn in order
    PrGltfMaterialIndex material;       // kPrGltfNone for the default material (glTF-2.0/#default-material)
    PrGltfMode mode;
    uint32_t vertexCount;               // the number of elements of every attribute

    // The buffer view of the primitive's Draco stream (KHR_draco_mesh_compression), or kPrGltfNone. The stream holds
    // the primitive's indices and those of its attributes that the extension names, whose accessors the document fills
    // from it when the asset is read (PrGltfAccessor::draco). Morph targets, and attributes that the extension does not
    // name, are read from their accessors as they are in any primitive.
    PrGltfBufferViewIndex draco;

    // The materials that the primitive takes in variants of the asset (KHR_materials_variants): a range of the
    // document's variant mappings (prGltfVariantMappings). Each variant is in one mapping at most.
    uint32_t mappingFirst;
    uint32_t mappingCount;
} PrGltfPrimitive;

// A material that a primitive takes in some of the asset's variants.
typedef struct PrGltfVariantMapping {
    PrGltfMaterialIndex material;
    PrGltfList variants;                // variant indices, at least one
    PrStringView name;
} PrGltfVariantMapping;

// Return the number of variants of the asset's materials, and the name of each (KHR_materials_variants).
uint32_t prGltfVariantCount(const PrGltf* gltf);
PrStringView prGltfVariantName(const PrGltf* gltf, uint32_t variant);

// Return the number of variant mappings, and the array of them.
uint32_t prGltfVariantMappingCount(const PrGltf* gltf);
const PrGltfVariantMapping* PR_NULLABLE prGltfVariantMappings(const PrGltf* gltf);

// Returns the material that a primitive is drawn with in `variant`: the material of its mapping for the variant, or
// else its own. kPrGltfNone for `variant` asks for its own.
PrGltfMaterialIndex prGltfPrimitiveMaterial(const PrGltf* gltf, PrGltfPrimitiveIndex primitive, uint32_t variant);

// A set of primitives that are drawn together.
typedef struct PrGltfMesh {
    PrStringView name;

    // The mesh's primitives: a range of the document's primitives (prGltfPrimitives).
    PrGltfPrimitiveIndex primitiveFirst;
    uint32_t primitiveCount;

    // The weight of each morph target, floats, used by a node that gives none. Empty means weights of zero.
    PrGltfList weights;
} PrGltfMesh;

// Return the number of meshes, primitives, attributes or morph targets, and the array of them.
uint32_t prGltfMeshCount(const PrGltf* gltf);
const PrGltfMesh* PR_NULLABLE prGltfMeshes(const PrGltf* gltf);
uint32_t prGltfPrimitiveCount(const PrGltf* gltf);
const PrGltfPrimitive* PR_NULLABLE prGltfPrimitives(const PrGltf* gltf);
uint32_t prGltfAttributeCount(const PrGltf* gltf);
const PrGltfAttribute* PR_NULLABLE prGltfAttributes(const PrGltf* gltf);
uint32_t prGltfMorphTargetCount(const PrGltf* gltf);
const PrGltfMorphTarget* PR_NULLABLE prGltfMorphTargets(const PrGltf* gltf);

// Returns the accessor of the primitive's attribute with `semantic` and `set`, or kPrGltfNone if it has none.
PrGltfAccessorIndex prGltfPrimitiveAttribute(
    const PrGltf* gltf, PrGltfPrimitiveIndex primitive, PrGltfSemantic semantic, uint32_t set);

// Writes the primitive's triangles to `out` as a triangle list, three vertex indices for each, up to `capacity`
// indices, and returns the number of indices in the whole list. Every index is less than the primitive's `vertexCount`.
// A strip or a fan is unrolled with the winding that the specification gives each triangle (glTF-2.0/#meshes-topology),
// and vertices beyond the last whole triangle are dropped. A primitive of points or lines has no triangles. Returns
// zero if the primitive's indices are in a buffer that has no bytes.
uint32_t prGltfPrimitiveTriangles(
    const PrGltf* gltf, PrGltfPrimitiveIndex primitive, uint32_t* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

// The kinds of projection.
enum {
    PrGltfCameraType_Perspective,
    PrGltfCameraType_Orthographic,
};
typedef uint8_t PrGltfCameraType;

// A projection (glTF-2.0/#cameras). A camera looks along its node's negative z axis, with positive y up.
typedef struct PrGltfCamera {
    PrStringView name;
    PrGltfCameraType type;

    // A perspective camera's vertical field of view in radians, and its width over its height. An aspect ratio of zero
    // means that of the viewport.
    float yfov;
    float aspectRatio;

    // An orthographic camera's half width and half height.
    float xmag;
    float ymag;

    // The distances to the near and far planes. A perspective camera with no far plane has a `zfar` of infinity.
    float znear;
    float zfar;
} PrGltfCamera;

// The kinds of punctual light.
enum {
    PrGltfLightType_Directional,
    PrGltfLightType_Point,
    PrGltfLightType_Spot,
};
typedef uint8_t PrGltfLightType;

// A light of KHR_lights_punctual. It shines from its node's origin, a directional or spot light along the node's
// negative z axis.
typedef struct PrGltfLight {
    PrStringView name;
    PrGltfLightType type;
    float color[3];                     // linear RGB
    float intensity;                    // candela for a point or spot light, and lux for a directional light
    float range;                        // the distance at which the light ends, or infinity

    // A spot light's cone, in radians from its axis: full intensity within the inner angle, none beyond the outer.
    float innerConeAngle;
    float outerConeAngle;
} PrGltfLight;

// Return the number of cameras or lights, and the array of them.
uint32_t prGltfCameraCount(const PrGltf* gltf);
const PrGltfCamera* PR_NULLABLE prGltfCameras(const PrGltf* gltf);
uint32_t prGltfLightCount(const PrGltf* gltf);
const PrGltfLight* PR_NULLABLE prGltfLights(const PrGltf* gltf);

// ---------------------------------------------------------------------------------------------------------------------
// Nodes, skins and scenes (glTF-2.0/#nodes-and-hierarchy)

// The instances of a node's mesh. The three transforms are accessors of `count` elements, or kPrGltfNone for the
// identity: a VEC3 of floats for the translation and the scale, and a VEC4 of floats or of normalized signed integers
// for the rotation, a unit quaternion. The attributes are the others that the asset gives, by name, each of `count`
// elements: a range of the document's attributes (prGltfAttributes), of semantic Custom.
typedef struct PrGltfInstancing {
    uint32_t count;                     // the number of instances, or zero if the node's mesh is drawn once
    PrGltfAccessorIndex translation;
    PrGltfAccessorIndex rotation;
    PrGltfAccessorIndex scale;
    uint32_t attributeFirst;
    uint32_t attributeCount;
} PrGltfInstancing;

// A node of the hierarchy. Its transform is either `matrix`, or `translation`, `rotation` and `scale`, which are the
// identity if the node has a matrix.
typedef struct PrGltfNode {
    PrStringView name;
    PrGltfNodeIndex parent;             // kPrGltfNone for a root
    PrGltfList children;                // node indices
    PrGltfMeshIndex mesh;               // each of these four is kPrGltfNone if the node has none
    PrGltfSkinIndex skin;
    PrGltfCameraIndex camera;
    PrGltfLightIndex light;

    // The weight of each morph target of the node's mesh, floats. Empty means the mesh's own weights.
    PrGltfList weights;

    bool hasMatrix;
    PrMatrix4 matrix;                   // meaningful only if `hasMatrix`
    float translation[3];
    float rotation[4];                  // a unit quaternion, as x, y, z, w
    float scale[3];

    // False if KHR_node_visibility hides the node, which hides its descendants too, whatever they say.
    bool visible;

    // The instances of the node's mesh (EXT_mesh_gpu_instancing), or none. Each is drawn at its own translation,
    // rotation and scale within the node, and the attributes give each instance a value of their own.
    PrGltfInstancing instancing;
} PrGltfNode;

// The joints that deform a skinned mesh (glTF-2.0/#skins).
typedef struct PrGltfSkin {
    PrStringView name;
    PrGltfList joints;                  // node indices, at least one

    // A MAT4 accessor of floats with a matrix for each joint, at least. kPrGltfNone if every matrix is the identity.
    PrGltfAccessorIndex inverseBindMatrices;

    PrGltfNodeIndex skeleton;           // kPrGltfNone if the asset names no root for the joints
} PrGltfSkin;

// A set of root nodes to draw.
typedef struct PrGltfScene {
    PrStringView name;
    PrGltfList nodes;                   // node indices, each a root
} PrGltfScene;

// Return the number of nodes, skins or scenes, and the array of them.
uint32_t prGltfNodeCount(const PrGltf* gltf);
const PrGltfNode* PR_NULLABLE prGltfNodes(const PrGltf* gltf);
uint32_t prGltfSkinCount(const PrGltf* gltf);
const PrGltfSkin* PR_NULLABLE prGltfSkins(const PrGltf* gltf);
uint32_t prGltfSceneCount(const PrGltf* gltf);
const PrGltfScene* PR_NULLABLE prGltfScenes(const PrGltf* gltf);

// Returns the scene to show when the asset is loaded, or kPrGltfNone if the asset names none.
PrGltfSceneIndex prGltfDefaultScene(const PrGltf* gltf);

// Returns the matrix of a translation of three floats, a rotation by a unit quaternion of four (x, y, z, w) and a scale
// of three, which scales, then rotates, then translates (glTF-2.0/#transformations).
PrMatrix4 prGltfMatrixFromTrs(const float* translation, const float* rotation, const float* scale);

// Returns the node's transform relative to its parent: its matrix, or the matrix of its translation, rotation and
// scale.
PrMatrix4 prGltfNodeLocalMatrix(const PrGltfNode* node);

// Returns the transform from a node to the scene, given its transform relative to its parent and its parent's transform
// to the scene. A root's world matrix is its local matrix.
PrMatrix4 prGltfNodeWorldMatrix(const PrMatrix4* local, const PrMatrix4* parentWorld);

// Writes the transform from each node to the scene to `out`, which holds a matrix for each node of the document, with
// every node at the transform that the asset gives it.
void prGltfWorldMatrices(const PrGltf* gltf, PrMatrix4* out);

// Writes to `out` the transform of `count` instances of a node's mesh from instance `first`, each relative to the node,
// which the node's world matrix maps to the scene. The range must lie within the node's instances. Returns false, and
// writes the identity for each, if a buffer of the instances' transforms has no bytes.
bool prGltfInstanceMatrices(const PrGltf* gltf, PrGltfNodeIndex node, uint32_t first, uint32_t count, PrMatrix4* out);

// ---------------------------------------------------------------------------------------------------------------------
// Animations (glTF-2.0/#animations)

// How a sampler's value is computed between two keyframes (glTF-2.0/#animations-interpolations).
enum {
    PrGltfInterpolation_Linear,
    PrGltfInterpolation_Step,
    PrGltfInterpolation_CubicSpline,
};
typedef uint8_t PrGltfInterpolation;

// The property that a channel animates. The core's targets are the first four, of a node. KHR_animation_pointer names
// any property of the asset that the Asset Object Model makes animatable by a JSON pointer, and a pointer reads as the
// path of its property, with the object that holds it (PrGltfAnimationChannel). A pointer to the translation, the
// rotation, the scale or the weights of a node reads as the core's target.
enum {
    PrGltfPath_None,                    // nothing that the specification defines
    PrGltfPath_Translation,
    PrGltfPath_Rotation,
    PrGltfPath_Scale,
    PrGltfPath_Weights,

    // A pointer into `extras` or into an extension that the module does not read, which animates nothing that the
    // module knows of.
    PrGltfPath_Pointer,

    // A node's KHR_node_visibility, as 0 or 1, and one weight of its morph targets.
    PrGltfPath_NodeVisible,
    PrGltfPath_NodeWeight,

    // The default weights of a mesh's morph targets, and one of them.
    PrGltfPath_MeshWeights,
    PrGltfPath_MeshWeight,

    // A camera's projection. The near and far planes are those of its type.
    PrGltfPath_CameraAspectRatio,
    PrGltfPath_CameraYfov,
    PrGltfPath_CameraXmag,
    PrGltfPath_CameraYmag,
    PrGltfPath_CameraZnear,
    PrGltfPath_CameraZfar,

    // A punctual light.
    PrGltfPath_LightColor,
    PrGltfPath_LightIntensity,
    PrGltfPath_LightRange,
    PrGltfPath_LightInnerConeAngle,
    PrGltfPath_LightOuterConeAngle,

    // A factor of a material, named as its field of PrGltfMaterial.
    PrGltfPath_MaterialBaseColorFactor,
    PrGltfPath_MaterialMetallicFactor,
    PrGltfPath_MaterialRoughnessFactor,
    PrGltfPath_MaterialAlphaCutoff,
    PrGltfPath_MaterialEmissiveFactor,
    PrGltfPath_MaterialNormalScale,
    PrGltfPath_MaterialOcclusionStrength,
    PrGltfPath_MaterialEmissiveStrength,
    PrGltfPath_MaterialClearcoatFactor,
    PrGltfPath_MaterialClearcoatRoughnessFactor,
    PrGltfPath_MaterialClearcoatNormalScale,
    PrGltfPath_MaterialSheenColorFactor,
    PrGltfPath_MaterialSheenRoughnessFactor,
    PrGltfPath_MaterialTransmissionFactor,
    PrGltfPath_MaterialThicknessFactor,
    PrGltfPath_MaterialAttenuationDistance,
    PrGltfPath_MaterialAttenuationColor,
    PrGltfPath_MaterialIor,
    PrGltfPath_MaterialSpecularFactor,
    PrGltfPath_MaterialSpecularColorFactor,
    PrGltfPath_MaterialIridescenceFactor,
    PrGltfPath_MaterialIridescenceIor,
    PrGltfPath_MaterialIridescenceThicknessMinimum,
    PrGltfPath_MaterialIridescenceThicknessMaximum,
    PrGltfPath_MaterialAnisotropyStrength,
    PrGltfPath_MaterialAnisotropyRotation,
    PrGltfPath_MaterialDispersion,
    PrGltfPath_MaterialDiffuseTransmissionFactor,
    PrGltfPath_MaterialDiffuseTransmissionColorFactor,

    // The KHR_texture_transform of a texture of a material.
    PrGltfPath_TextureOffset,
    PrGltfPath_TextureRotation,
    PrGltfPath_TextureScale,
};
typedef uint8_t PrGltfPath;

// Keyframes: a time in seconds for each, in `input`, and the values at those times, in `output`.
typedef struct PrGltfAnimationSampler {
    PrGltfAccessorIndex input;
    PrGltfAccessorIndex output;
    PrGltfInterpolation interpolation;
} PrGltfAnimationSampler;

// A sampler applied to one property of one object.
typedef struct PrGltfAnimationChannel {
    PrGltfAnimationSamplerIndex sampler;
    PrGltfPath path;                    // PrGltfPath_None for a channel with no target, which animates nothing

    // The node whose property the channel animates, or kPrGltfNone for a property of another object.
    PrGltfNodeIndex node;

    // The object that holds the property: the index of a node, a mesh, a camera, a light or a material, as the path
    // says, or kPrGltfNone for a channel with no target or an unresolved pointer.
    uint32_t object;

    // The morph target of one weight, or the PrGltfMaterialTexture of a texture transform, and zero for other paths.
    uint32_t element;

    // The number of floats in the channel's value: 3 for a translation or a scale, 4 for a rotation, the number of
    // morph targets for weights, that of the property for a pointer, and zero for a channel with no target or an
    // unresolved pointer.
    uint32_t width;

    // The JSON pointer that names the property (KHR_animation_pointer), or the null view for a target of the core.
    PrStringView pointer;
} PrGltfAnimationChannel;

// A set of channels that play together.
typedef struct PrGltfAnimation {
    PrStringView name;

    // The animation's channels and samplers: ranges of the document's (prGltfAnimationChannels,
    // prGltfAnimationSamplers).
    PrGltfAnimationChannelIndex channelFirst;
    uint32_t channelCount;
    PrGltfAnimationSamplerIndex samplerFirst;
    uint32_t samplerCount;

    float duration;                     // the last keyframe's time among the channels that have a target, in seconds
} PrGltfAnimation;

// Return the number of animations, animation samplers or animation channels, and the array of them.
uint32_t prGltfAnimationCount(const PrGltf* gltf);
const PrGltfAnimation* PR_NULLABLE prGltfAnimations(const PrGltf* gltf);
uint32_t prGltfAnimationSamplerCount(const PrGltf* gltf);
const PrGltfAnimationSampler* PR_NULLABLE prGltfAnimationSamplers(const PrGltf* gltf);
uint32_t prGltfAnimationChannelCount(const PrGltf* gltf);
const PrGltfAnimationChannel* PR_NULLABLE prGltfAnimationChannels(const PrGltf* gltf);

// Writes the value of a channel at `seconds` to `out`, which holds the channel's `width` floats. Before the first
// keyframe the value is the first keyframe's, and after the last it is the last keyframe's
// (glTF-2.0/#animations-usage). Between two keyframes it is interpolated as the sampler says: held, interpolated
// linearly, or along the cubic spline of the keyframes' tangents. A rotation is interpolated spherically along the
// shorter arc when the interpolation is linear, and is a unit quaternion in every case.
//
// Returns false, and fills `out` with zeros, if a buffer that the sampler reads has no bytes.
bool prGltfAnimationChannelEvaluate(const PrGltf* gltf, PrGltfAnimationChannelIndex channel, float seconds, float* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
