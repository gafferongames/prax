// The records that lib/render keeps on the GPU and its shaders read: instances, draws, mesh parts, materials and
// lights, a mesh's geometry, and the root data of the library's draws and dispatches. C, C++ and Slang all compile this
// file and read each structure as it is written here, so an application's own shaders and compute passes read and write
// the same records.
//
// Every field lies at a multiple of its alignment: 16 bytes for a PrRenderVec4 and 4 for a scalar. No field is a
// 3-vector, and every record's size is a multiple of 16 bytes, so each language lays a record out alike (pr/gpu.slang).
// The parameter records of the library's material types are declared with their types, in Slang alone, and a producer
// fills them by their parameters' names (PrRenderMaterialType::parameters).
//
// An id is a record's index in its buffer. Index zero of the instances is no instance, and index zero of the materials
// is the material that stands in for one that is missing.

#pragma once

#if defined(__SLANG__)

// Declares a constant that C and the shaders share, in the spelling of the language that compiles it.
#define PR_RENDER_CONSTANT(name, value) static const uint name = value;
#define PR_RENDER_CONSTANT_U32(name, value) static const uint name = value;

// Four floats, aligned to 16 bytes.
typedef float4 PrRenderVec4;

// Marks a field of a material type's parameter record that C compiles too with its value where a material does not set
// it (RenderParameter, pr/render_surface.slang). C reads no mark.
#define PR_RENDER_PARAMETER(x, y, z, w) [RenderParameter(x, y, z, w)]

#else

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

// C++ declares each constant as 32 unsigned bits, as the shaders do. C declares an enumerator, which sizes an array,
// and a constant of 32 unsigned bits for a value that does not fit in an int.
#if defined(__cplusplus)
#define PR_RENDER_CONSTANT(name, value) constexpr uint32_t name = value;
#else
#define PR_RENDER_CONSTANT(name, value) enum { name = value };
#endif
#define PR_RENDER_CONSTANT_U32(name, value) PR_CONSTANT uint32_t name = value;
#define PR_RENDER_PARAMETER(x, y, z, w)

// Aligns a PrRenderVec4 to 16 bytes, as the shaders align a 4-vector.
#if defined(__cplusplus)
#define PR_RENDER_VEC4_ALIGN alignas(16)
#else
#define PR_RENDER_VEC4_ALIGN _Alignas(16)
#endif

// Four floats, aligned to 16 bytes.
typedef struct PrRenderVec4 {
    PR_RENDER_VEC4_ALIGN float x;
    float y;
    float z;
    float w;
} PrRenderVec4;

// The records below, which C names without `struct` as the shaders do.
typedef struct PrRenderInstanceRecord PrRenderInstanceRecord;
typedef struct PrRenderDrawRecord PrRenderDrawRecord;
typedef struct PrRenderPartRecord PrRenderPartRecord;
typedef struct PrRenderGeometryRecord PrRenderGeometryRecord;
typedef struct PrRenderSkinRecord PrRenderSkinRecord;
typedef struct PrRenderMorphRecord PrRenderMorphRecord;
typedef struct PrRenderDeformPush PrRenderDeformPush;
typedef struct PrRenderMaterialRecord PrRenderMaterialRecord;
typedef struct PrRenderMaterialTextureRecord PrRenderMaterialTextureRecord;
typedef struct PrRenderLightRecord PrRenderLightRecord;
typedef struct PrRenderFrameUniforms PrRenderFrameUniforms;
typedef struct PrRenderDrawPush PrRenderDrawPush;
typedef struct PrRenderDisplayPush PrRenderDisplayPush;
typedef struct PrRenderMeterPush PrRenderMeterPush;
typedef struct PrRenderEnvironmentPush PrRenderEnvironmentPush;

#endif

// ---------------------------------------------------------------------------------------------------------------------
// Limits and sizes

// The number of lights of a scene that shade a surface. A scene's lights past this number, in the order of their ids,
// light nothing.
PR_RENDER_CONSTANT(kPrRenderMaxLights, 32)

// The size in bytes of each record, of a vertex of a mesh's geometry and of an index, and of the indirect arguments of
// an indexed draw (PrGpuDrawIndexedIndirect).
PR_RENDER_CONSTANT(kPrRenderInstanceBytes, 112)
PR_RENDER_CONSTANT(kPrRenderDrawBytes, 32)
PR_RENDER_CONSTANT(kPrRenderPartBytes, 48)
PR_RENDER_CONSTANT(kPrRenderMaterialBytes, 32)
PR_RENDER_CONSTANT(kPrRenderMaterialTextureBytes, 96)
PR_RENDER_CONSTANT(kPrRenderLightBytes, 64)
PR_RENDER_CONSTANT(kPrRenderGeometryBytes, 28)
PR_RENDER_CONSTANT(kPrRenderIndexBytes, 4)
PR_RENDER_CONSTANT(kPrRenderArgumentsBytes, 20)

// The size in bytes of what deforms a mesh: a vertex's skin, what one morph target adds to one vertex, a joint's
// matrix, and a position of the frame before.
PR_RENDER_CONSTANT(kPrRenderSkinBytes, 16)
PR_RENDER_CONSTANT(kPrRenderMorphBytes, 36)
PR_RENDER_CONSTANT(kPrRenderJointBytes, 48)
PR_RENDER_CONSTANT(kPrRenderPositionBytes, 12)

// The number of joints that move one vertex.
PR_RENDER_CONSTANT(kPrRenderSkinInfluences, 4)

// The views of a draw or a dispatch: the camera's view, and the view of the light that casts the scene's shadows.
PR_RENDER_CONSTANT(kPrRenderViewCamera, 0)
PR_RENDER_CONSTANT(kPrRenderViewLight, 1)
PR_RENDER_CONSTANT(kPrRenderViewCount, 2)

// The number of 4-vectors that hold an environment's diffuse light: the coefficients of the spherical harmonics of
// bands 0, 1 and 2.
PR_RENDER_CONSTANT(kPrRenderEnvironmentHarmonics, 9)

// The number of planes that bound a view's frustum. A point is inside a plane if dot(plane.xyz, point) + plane.w >= 0.
PR_RENDER_CONSTANT(kPrRenderViewPlanes, 6)

// ---------------------------------------------------------------------------------------------------------------------
// Scene records

// The bit of an instance's visibility mask that makes the instance cast shadows. The other bits are the application's:
// a view sees an instance if the view's mask and the instance's share a bit.
PR_RENDER_CONSTANT_U32(kPrRenderVisibilityShadow, 0x80000000u)

// An instance of a mesh in the world. `world` is the top three rows of the matrix that maps the mesh's column vectors
// (x, y, z, 1) to the world, and `previousWorld` is that matrix one frame earlier.
//
// An instance that deforms draws vertices of its own, which its parts name (PrRenderPartRecord). `previousPositions` is
// then the handle of the buffer that holds where each of those vertices was one frame earlier, in the mesh's space, as
// three floats at kPrRenderPositionBytes times the vertex's place in the part's buffer: its index plus the part's
// `firstVertex`. It is zero for an instance that does not deform, whose vertices are where they were.
struct PrRenderInstanceRecord {
    PrRenderVec4 world[3];
    PrRenderVec4 previousWorld[3];
    uint32_t mesh;
    uint32_t mobility;          // PrRenderMobilityFlags
    uint32_t visibility;        // the views that see the instance, and kPrRenderVisibilityShadow
    uint32_t previousPositions;
};

// What a draw's flags say.
PR_RENDER_CONSTANT(kPrRenderDrawMask, 1u << 0)      // the material cuts the surface out where its alpha is low
PR_RENDER_CONSTANT(kPrRenderDrawBlend, 1u << 1)     // the material blends over what lies behind it
PR_RENDER_CONSTANT(kPrRenderDrawDoubleSided, 1u << 2)
PR_RENDER_CONSTANT(kPrRenderDrawMirrored, 1u << 3)  // the instance's matrix reverses the winding of its triangles

// The material reads the scene behind its surfaces (kPrRenderMaterialBehind). Such a draw has kPrRenderDrawBlend too,
// whatever its alpha mode: it is drawn with the blended draws, after the surfaces that it shows.
PR_RENDER_CONSTANT(kPrRenderDrawBehind, 1u << 4)

// One part of one instance, which is one draw. A scene's draws are in the order that its passes issue them.
//
// `attributes` is the handle of the buffer that holds the part's vertex attributes in the layout that the material's
// type declares (PrRenderVertexLayout), and `firstAttribute` the vertex of that buffer that the part's index zero
// names. The geometry is the part's, and the attributes the draw's, because a material decides the layout that it reads
// and a deforming instance moves its geometry alone.
struct PrRenderDrawRecord {
    uint32_t instance;
    uint32_t part;
    uint32_t material;
    uint32_t flags;
    uint32_t attributes;
    uint32_t firstAttribute;
    uint32_t _pad[2];
};

// A part of a mesh: a range of the mesh's indices, the buffers that hold the mesh's geometry and indices, and the
// sphere that bounds the part's vertices, as a center and a radius.
//
// A draw names the part that it draws. An instance that deforms has parts of its own, which name the buffer that holds
// its deformed vertices and bound them in its pose, so that a pass draws a deformed instance as it draws any other.
struct PrRenderPartRecord {
    PrRenderVec4 bounds;
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t geometry;          // the handle of a buffer of PrRenderGeometryRecord
    uint32_t indices;           // the handle of a buffer of 32-bit indices
    uint32_t firstVertex;       // the vertex of `geometry` that the index zero names
    uint32_t _pad[3];
};

// A vertex of a mesh's geometry, which every pass reads and deformation writes, as a shader that pulls its vertices
// loads it. PrRenderGeometryVertex (pr/render_resource.h) is the same bytes with the packed fields named. What else a
// vertex holds is in the layout that a material's type declares.
struct PrRenderGeometryRecord {
    float position[3];
    uint32_t normal[2];         // four signed 16-bit values: x and y in the first word, then z and nothing
    uint32_t tangent[2];        // four signed 16-bit values: x and y, then z and the handedness
};

// ---------------------------------------------------------------------------------------------------------------------
// Deformation

// The joints that move a vertex of a skinned mesh, and the part of the vertex that each moves. PrRenderSkinVertex
// (pr/render_resource.h) is the same bytes.
struct PrRenderSkinRecord {
    uint32_t joints[2];         // four 16-bit indices of joints: two in each word, the first in the low half
    uint32_t weights[2];        // four unsigned 16-bit weights, laid out alike, which sum to 65535
};

// What one morph target adds to a vertex at a weight of 1. PrRenderMorphDelta (pr/render_resource.h) is the same bytes.
struct PrRenderMorphRecord {
    float position[3];
    float normal[3];
    float tangent[3];
};

// The push data of the dispatch that deforms one instance's vertices. It reads the mesh's vertices, their skin and
// their morph targets, and the instance's pose, and writes the vertices that the instance draws.
//
// The pose is 4-vectors at `firstPose` bytes into its buffer: three rows for each joint, which map a vertex of the mesh
// as it was bound to where the joint puts it, in the mesh's space, and then the weights of the morph targets, four to a
// vector. A vertex's morph targets are consecutive records, at its index times `morphTargetCount`.
struct PrRenderDeformPush {
    uint32_t vertexCount;
    uint32_t firstDeformed;         // the instance's first vertex in the buffers that the dispatch writes
    uint32_t jointCount;            // of the mesh's skin, or zero for a mesh without one
    uint32_t morphTargetCount;

    uint32_t firstPose;

    // Nonzero if the instance's vertices hold the frame before, whose positions become its previous positions. If zero,
    // the previous positions are the new ones.
    uint32_t keepsPrevious;

    // Handles, which a device with bindless reads: the mesh's vertices, skin and morph targets, the scene's poses, and
    // the storage handles of the scene's deformed vertices and of their previous positions.
    uint32_t vertices;
    uint32_t skin;
    uint32_t morphs;
    uint32_t pose;
    uint32_t deformed;
    uint32_t previousPositions;
};

// ---------------------------------------------------------------------------------------------------------------------
// Materials

// What a material's flags say. The low two bits are its alpha mode.
PR_RENDER_CONSTANT(kPrRenderMaterialAlphaMode, 3u)
PR_RENDER_CONSTANT(kPrRenderMaterialOpaque, 0)
PR_RENDER_CONSTANT(kPrRenderMaterialMask, 1)
PR_RENDER_CONSTANT(kPrRenderMaterialBlend, 2)
PR_RENDER_CONSTANT(kPrRenderMaterialDoubleSided, 1u << 2)
PR_RENDER_CONSTANT(kPrRenderMaterialBehind, 1u << 3)          // the material reads the scene behind its surfaces
PR_RENDER_CONSTANT(kPrRenderMaterialDisplayEmission, 1u << 4) // its emission is the view's (PrRenderEmission_Display)

// A material: the variant of its type that draws it and its render states, and where the materials' buffer holds its
// textures and its parameters.
//
// The materials' buffer begins with these records, one for each material id. The rest of the buffer holds what the
// records name by an offset in bytes from the buffer's start. A material's size is therefore its type's: the type's
// number of textures times kPrRenderMaterialTextureBytes, plus the size of the type's parameter record.
//
// `textures` is the offset of `textureCount` consecutive PrRenderMaterialTextureRecord, by the type's texture index.
// `parameters` is the offset of the parameter record of the material's type, the structure that the type names as its
// Parameters. Both are multiples of 16.
//
// The renderer writes the whole buffer again when a material or a texture changes, and an offset may then change. A
// pass of the application's that writes a material's parameters on the GPU reads the offset from this record in the
// same pass.
struct PrRenderMaterialRecord {
    uint32_t variant;           // of the material's type, by its index among the set's (PrRenderShaderSet::variants)
    uint32_t flags;
    float alphaCutoff;
    uint32_t textureCount;
    uint32_t textures;
    uint32_t parameters;
    uint32_t parameterBytes;
    uint32_t _pad;
};

// What a texture's flags say: the material names a texture here, and in the bits above kPrRenderTextureSetShift the set
// of texture coordinates that it reads, by the index among the type's attributes of that semantic, and above
// kPrRenderTextureChannelShift the channel that a scalar input reads, 0 to 3 for red to alpha.
PR_RENDER_CONSTANT(kPrRenderTextureNamed, 1u << 0)
PR_RENDER_CONSTANT(kPrRenderTextureSetShift, 8)
PR_RENDER_CONSTANT(kPrRenderTextureChannelShift, 16)

// The slot of a texture that no slot holds.
PR_RENDER_CONSTANT_U32(kPrRenderMaterialNoSlot, 0xffffffffu)

// One texture of a material.
//
// `texture` and `sampler` are handles, which a device with bindless samples. A texture that the material does not name
// is a white texel, so a material type multiplies by a texture without asking whether the material has it. A texture
// that the material names and whose texels have not arrived, or that was destroyed, is one texel of the stand-in value
// that the material's type declares for it.
//
// A device without bindless samples the texture slot `slot` of the draw's material textures, which the pass sources
// declare. A texture with kPrRenderMaterialNoSlot is not sampled there and reads as `value`: white for a texture that
// the material does not name, and the type's stand-in value for one that it names past the slots that a draw has.
//
// The texture's coordinates pass through a transform of two rows: (u, v) maps to (dot(row0.xy, uv) + row0.z,
// dot(row1.xy, uv) + row1.z). What a sample returns is the texel times `scale` plus `bias`, by component, as
// UsdUVTexture and a MaterialX image scale and bias the texels of a channel: the mapping of a texture's values to an
// input belongs to the texture's use, so no material type holds one of its own.
struct PrRenderMaterialTextureRecord {
    PrRenderVec4 transform[2];
    PrRenderVec4 value;
    PrRenderVec4 scale;
    PrRenderVec4 bias;
    uint32_t texture;
    uint32_t sampler;
    uint32_t slot;
    uint32_t flags;
};

// ---------------------------------------------------------------------------------------------------------------------
// Lights

// The kinds of light, which a light's record holds in `direction.w`.
PR_RENDER_CONSTANT(kPrRenderLightDirectional, 0)
PR_RENDER_CONSTANT(kPrRenderLightPoint, 1)
PR_RENDER_CONSTANT(kPrRenderLightSpot, 2)

// A light, in the units of glTF's KHR_lights_punctual.
struct PrRenderLightRecord {
    PrRenderVec4 position;      // in the world, and in w the range past which the light is dark, or 0 for no range
    PrRenderVec4 direction;     // the unit direction in which the light shines, and in w its kind
    PrRenderVec4 color;         // linear color times intensity: lux for a directional light, candela for the others

    // A spot's cone in x and y: the light's part at the cosine c of the angle from its axis is saturate(c * x + y)
    // squared. In z, the size of the light's source: for a directional light the angle from its middle to its edge, in
    // radians, and for a point or spot light its radius in the world.
    PrRenderVec4 cone;
};

// ---------------------------------------------------------------------------------------------------------------------
// Root data

// The uniforms of every draw and dispatch of one view of a scene. A device with bindless reads the resources by the
// handles here, and any other device reads them from slots.
struct PrRenderFrameUniforms {
    // By view, the four rows of the matrix that maps the world to the view's clip space, and the planes of the view's
    // frustum, with unit normals.
    PrRenderVec4 viewProjection[kPrRenderViewCount * 4];
    PrRenderVec4 planes[kPrRenderViewCount * kPrRenderViewPlanes];

    PrRenderVec4 cameraPosition;
    PrRenderVec4 targetExtent;      // the width and height of the camera's targets in pixels, in x and y

    // The three rows of the matrix that maps (x, y, 1) to the direction in the world that the camera sees at a point of
    // its target, where x and y run from -1 to 1 across the target, with y up. The direction is not of unit length.
    PrRenderVec4 backgroundRay[3];

    // The three rows of the matrix that turns a direction in the world into the direction of the scene's environment
    // that it shows.
    PrRenderVec4 environmentRotation[3];

    // The environment's diffuse light: the radiance that a white surface without gloss leaves toward any direction, as
    // a function of its unit normal n in the environment's space. It is the sum of each coefficient times its term of
    // n, in this order: 1, y, z, x, xy, yz, 3z^2 - 1, xz, x^2 - y^2. The scene's intensity is in the coefficients.
    PrRenderVec4 environmentIrradiance[kPrRenderEnvironmentHarmonics];

    // The sun's disc of the environment, which a view's background shows and which lights no surface: the unit
    // direction toward its middle in the environment's space, with the sine of the angle from its middle to its edge in
    // w, and its mean radiance, with the scene's intensity in it. An environment without a disc has a radiance of zero.
    PrRenderVec4 environmentSun;
    PrRenderVec4 environmentSunRadiance;

    float shadowSize;               // the shadow map's width and height in texels
    float shadowTexel;              // the width of one texel of the shadow map in the world
    uint32_t shadowLight;           // the index of the light whose shadows the map holds, or kPrRenderMaxLights
    uint32_t lightCount;

    float shadowDepth;              // the distance in the world between the map's depths of 0 and 1
    float shadowSpread;             // the tangent of the shadowing light's angular radius
    float pixelSpread;              // the tangent of the angle that a pixel spans at the middle of the camera's target
    uint32_t tables;                // the texture that holds the tables of the shader set (pr/render_table.slang)

    uint32_t drawCount;
    uint32_t cameraMask;            // the visibility bits of the instances that the camera's view sees
    uint32_t lightMask;             // the visibility bits of the instances that the light's view sees
    uint32_t cameraPulls;           // nonzero if the camera's draws that do not blend pull their vertices

    uint32_t instances;
    uint32_t draws;
    uint32_t parts;
    uint32_t materials;

    uint32_t arguments;             // by view, the indirect arguments that the view's cull writes
    uint32_t shadowMap;
    uint32_t visibility;            // the draw and the triangle that each pixel of the camera's view shows
    uint32_t environment;           // the cube of the environment's radiance, filtered more at each level

    uint32_t environmentSampler;
    float environmentIntensity;     // the factor that scales the cube's radiance
    float environmentLastLevel;     // the cube's last level, which holds the radiance filtered for a roughness of 1
    float time;                     // the scene's time, in seconds (prRenderSceneTimeSet)

    // The scene behind the surfaces of the materials that read it (pr/render_behind.slang): the texture that holds the
    // camera's view of the surfaces that do not blend, with its background, each level a blur of the one before, its
    // sampler, and its last level. The last level is below zero in a pass that has no such texture.
    uint32_t behind;
    uint32_t behindSampler;
    float behindLastLevel;

    // The factor that scales the scene's luminance into the values of the view's color (PrRenderViewDesc::exposure).
    float exposure;

    PrRenderLightRecord lights[kPrRenderMaxLights];
};

// The push data. A cull reads `view` and writes the arguments of each draw, starting at `argumentsOffset`. A draw reads
// `view` and `draw`.
struct PrRenderDrawPush {
    uint32_t draw;
    uint32_t view;
    uint32_t argumentsOffset;
    uint32_t _pad;
};

// The tone maps of the display draw (PrRenderToneMap).
PR_RENDER_CONSTANT(kPrRenderToneMapNeutral, 0)
PR_RENDER_CONSTANT(kPrRenderToneMapAcesChannels, 1)
PR_RENDER_CONSTANT(kPrRenderToneMapAcesLuminance, 2)

// The push data of the display draw.
struct PrRenderDisplayPush {
    float exposure;                 // the factor that scales scene-referred color before the tone map
    float headroom;                 // the display's headroom, as a multiple of SDR white, at least 1
    uint32_t color;                 // the handle of the texture that holds the view's color
    uint32_t toneMap;
};

// The cells of a meter's grid along each side of a view (PrRenderMeter), and the pixels that a cell reads along each of
// its sides.
PR_RENDER_CONSTANT(kPrRenderMeterGrid, 64)
PR_RENDER_CONSTANT(kPrRenderMeterSamples, 4)

// The push data of a meter's draw, which writes one texel for each cell of its grid: in red, the mean of the base-2
// logarithm of the luminance of the cell's pixels that have light, and in green the share of its pixels that have it.
struct PrRenderMeterPush {
    uint32_t color;                 // the handle of the texture that holds the view's color
    float exposure;                 // the view's exposure, which the luminance is divided by
    uint32_t width;                 // the extent of the view's color
    uint32_t height;
};

// The push data of the draws that fill the levels of an environment's cube. Each draw covers one face of one level.
struct PrRenderEnvironmentPush {
    uint32_t source;                // the handle of the cube that the draw samples, and of its sampler
    uint32_t sampler;
    uint32_t face;                  // the face that the draw covers, as lib/gpu orders a cube's faces
    float targetSize;               // the width of the face that the draw covers, in texels

    float roughness;                // the roughness that the draw filters the source for
    float sourceSize;               // the width of a face of the source's first level, in texels
    uint32_t sampleCount;           // the number of directions that the draw samples for each texel
    uint32_t _pad;
};

#if !defined(__SLANG__)

PR_STATIC_ASSERT(sizeof(PrRenderInstanceRecord) == kPrRenderInstanceBytes);
PR_STATIC_ASSERT(sizeof(PrRenderDrawRecord) == kPrRenderDrawBytes);
PR_STATIC_ASSERT(sizeof(PrRenderPartRecord) == kPrRenderPartBytes);
PR_STATIC_ASSERT(sizeof(PrRenderGeometryRecord) == kPrRenderGeometryBytes);
PR_STATIC_ASSERT(sizeof(PrRenderSkinRecord) == kPrRenderSkinBytes);
PR_STATIC_ASSERT(sizeof(PrRenderMorphRecord) == kPrRenderMorphBytes);
PR_STATIC_ASSERT(sizeof(PrRenderDeformPush) % 16 == 0);
PR_STATIC_ASSERT(sizeof(PrRenderMaterialRecord) == kPrRenderMaterialBytes);
PR_STATIC_ASSERT(sizeof(PrRenderMaterialTextureRecord) == kPrRenderMaterialTextureBytes);
PR_STATIC_ASSERT(sizeof(PrRenderLightRecord) == kPrRenderLightBytes);
PR_STATIC_ASSERT(sizeof(PrRenderFrameUniforms) % 16 == 0);

PR_ASSUME_NONNULL_END

#endif
