// A renderer's state, which the sources that implement pr/render.h and its topic headers share.
//
// The state has two parts. The ids and the queues are guarded by the renderer's lock, and any thread reaches them. The
// tables, the records and the GPU objects belong to the thread that builds frames, which applies the queues to them
// when a frame begins (prRenderFrameBegin).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/mem.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_records.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// ---------------------------------------------------------------------------------------------------------------------
// Ids

// The ids of one kind of object. An id is taken when its object is created, and given back once the object's
// destruction has been applied, so that an id names one object at a time.
struct RenderIds {
    uint32_t count;             // the ids ever taken, which are 1 to `count`
    RawArray<uint32_t> free;
};

// Take an id, give one back, and free the storage. The caller holds the renderer's lock for the first two.
uint32_t renderIdsTake(RenderIds* ids);
void renderIdsGive(RenderIds* ids, uint32_t id);
void renderIdsReset(RenderIds* ids);

// ---------------------------------------------------------------------------------------------------------------------
// Ranges

// A range of the items of an array or a buffer.
struct RenderRange {
    uint32_t first;
    uint32_t count;
};

// The ranges of an array or a buffer that are in use: everything below `end` except the free ranges, which are in the
// order of their firsts, with something in use between any two.
struct RenderRanges {
    uint32_t end;
    RawArray<RenderRange> free;
};

// Take a range of `count` items, which is the first free range that holds it, or new items at the end. Give a range
// back. Free the storage.
uint32_t renderRangesTake(RenderRanges* ranges, uint32_t count);
void renderRangesGive(RenderRanges* ranges, uint32_t first, uint32_t count);
void renderRangesReset(RenderRanges* ranges);

// ---------------------------------------------------------------------------------------------------------------------
// Resources

// What deforms a mesh: its vertices' skin and morph targets on the device, and what bounds a pose of it. The owner of
// the structure frees the bounds.
struct RenderMeshDeform {
    PrGpuBuffer skin;           // PrRenderSkinVertex by vertex, or none
    PrGpuBuffer morphs;         // PrRenderMorphDelta by vertex and target, or none
    uint32_t vertexCount;
    uint32_t jointCount;
    uint32_t morphTargetCount;

    // By joint, the sphere that holds every vertex that the joint moves, as the mesh was bound, with a negative radius
    // for a joint that moves none. By morph target, the farthest that the target moves a vertex at a weight of 1.
    PrRenderVec4* PR_NULLABLE jointBounds;
    float* PR_NULLABLE morphReach;
};

// The attributes of a mesh's vertices in one layout of the shader set's. A stream of the fallbacks is the renderer's,
// made for a draw of a material whose layout the mesh's data did not give.
struct RenderAttributeStream {
    uint32_t layout;            // by its index in the shader set
    PrGpuBuffer buffer;
    bool fallback;
};

// A mesh as the frame thread holds it. Its parts are a range of the renderer's parts.
struct RenderMeshState {
    bool ready;                 // the mesh has data
    uint32_t serial;            // changes whenever the mesh's data does
    uint32_t vertexCount;
    PrGpuBuffer geometry;
    PrGpuBuffer indices;
    RawArray<RenderAttributeStream> streams;
    uint32_t firstPart;
    uint32_t partCount;
    RenderMeshDeform deform;
};

// A texture as the frame thread holds it.
struct RenderTextureState {
    bool ready;                 // the texture has texels
    PrGpuTexture texture;
};

// A material as the frame thread holds it: what it was last set to, or null if it was not set. The description owns its
// textures, the samplers that they read through, by texture, and its parameters, which follow it in one allocation.
struct RenderMaterialState {
    PrRenderMaterialDesc* PR_NULLABLE desc;
    const PrGpuSampler* PR_NULLABLE samplers;
};

// The most texture slots of a material that a pass may declare.
constexpr uint32_t kRenderMaxMaterialSlots = 8;

// The textures that a draw of a material binds on a device without bindless, by slot.
struct RenderMaterialSlots {
    PrGpuTexture textures[kRenderMaxMaterialSlots];
    PrGpuSampler samplers[kRenderMaxMaterialSlots];
    bool behind;    // the last slot holds the scene behind the surface, which the pass binds
};

// An environment as the frame thread holds it.
struct RenderEnvironmentState {
    bool ready;                 // the environment has light
    PrGpuTexture cube;          // the radiance, filtered for a rougher surface at each level
    uint32_t faceSize;
    uint32_t levels;

    // The diffuse light, as PrRenderFrameUniforms holds it, before a scene's intensity.
    PrRenderVec4 irradiance[kPrRenderEnvironmentHarmonics];

    // The sun's disc, as PrRenderFrameUniforms holds it, before a scene's intensity.
    PrRenderVec4 sun;
    PrRenderVec4 sunRadiance;
};

// A change to a resource that a thread made and the frame thread has yet to apply.
enum {
    RenderOpKind_MeshData,
    RenderOpKind_MeshDestroy,
    RenderOpKind_TextureCreate,
    RenderOpKind_TextureData,
    RenderOpKind_TextureDestroy,
    RenderOpKind_MaterialSet,
    RenderOpKind_MaterialDestroy,
    RenderOpKind_EnvironmentData,
    RenderOpKind_EnvironmentDestroy,
    RenderOpKind_MeterDestroy,
};
typedef uint8_t RenderOpKind;

// A queued change, and what it holds for its kind.
struct RenderOp {
    RenderOpKind kind;
    uint32_t id;

    // The data of a mesh: its buffers, its streams and its parts, which the op owns.
    PrGpuBuffer geometry;
    PrGpuBuffer indices;
    uint32_t vertexCount;
    RenderAttributeStream* PR_NULLABLE streams;
    uint32_t streamCount;
    PrRenderPartRecord* PR_NULLABLE parts;
    uint32_t partCount;
    RenderMeshDeform deform;

    PrGpuTexture texture;
    RenderMaterialState material;                   // owned by the op

    // The light of an environment, owned by the op with its cube.
    RenderEnvironmentState* PR_NULLABLE environment;
};

// A buffer of records on the device, which is made again, larger, when the records outgrow it.
struct RenderRecordBuffer {
    PrGpuBuffer buffer;
    uint64_t capacity;          // in bytes
};

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

struct Render;
struct RenderSceneFrame;

// An instance as the frame thread holds it, beside its record. Its materials are a range of the scene's pool.
struct RenderInstanceState {
    bool alive;
    bool moved;                 // its previous transform differs from its transform
    bool mirrored;              // its matrix reverses the winding of its triangles
    uint32_t firstMaterial;
    uint32_t materialCount;
    uint32_t deformation;       // the index of its deformation among the scene's, or zero if it does not deform
};

// The deformation of one instance: the pose that updates gave it, and, while its mesh has data that deforms, the
// vertices that it draws and the parts that name them.
struct RenderDeformation {
    uint32_t instance;          // or zero for an entry that no instance uses
    RawArray<PrAffine3> joints;
    RawArray<float> weights;

    // The mesh that the rest was made for, if `bound`.
    bool bound;
    uint32_t mesh;
    uint32_t meshSerial;
    uint32_t firstVertex;       // in the scene's deformed vertices
    uint32_t vertexCount;
    uint32_t firstPart;         // the instance's own parts, among the renderer's
    uint32_t partCount;
    uint32_t firstPose;         // in the scene's poses, in 4-vectors
    uint32_t poseCount;

    bool poseDirty;             // the vertices are behind the pose
    bool fresh;                 // the vertices hold nothing yet, so they have no previous positions
    bool settling;              // the vertices changed in the frame before, so their previous positions are behind
};

// A light as the frame thread holds it.
struct RenderLightState {
    bool alive;
    PrRenderLightDesc desc;
};

// The draws of a scene are sorted into buckets, each drawn with one set of states: three alpha modes, by four ways of
// culling. A way of culling is RenderCullFlags. Within a bucket the draws are in the order of their materials' types,
// since the forward technique draws each variant of a type with a pipeline of its own.
enum {
    RenderCullFlag_Mirrored = 1u << 0,      // front faces run clockwise, as a mirrored instance's do
    RenderCullFlag_DoubleSided = 1u << 1,   // no face is culled
};
typedef uint32_t RenderCullFlags;

// The numbers of ways of culling, of alpha modes (opaque, masked and blended), and of buckets. A draw's bucket is its
// alpha mode times kRenderCullCount, plus its way of culling.
constexpr uint32_t kRenderCullCount = 4;
constexpr uint32_t kRenderAlphaCount = 3;
constexpr uint32_t kRenderBucketCount = kRenderAlphaCount * kRenderCullCount;

// The buffers of indirect arguments of a scene: the first for the light's view, and one for each view of a frame.
constexpr uint32_t kRenderArgumentBuffers = 1 + kPrRenderMaxViews;

// A scene: its ids, which any thread takes, and its instances, lights, draws and buffers, which the frame thread keeps.
struct RenderSceneState {
    uint32_t id;

    // Guarded by the renderer's lock.
    RenderIds instanceIds;
    RenderIds lightIds;
    bool destroyed;             // the scene's destruction waits for the next frame

    // The rest belongs to the frame thread.
    RawArray<RenderInstanceState> instances;        // by instance id
    RawArray<PrRenderInstanceRecord> records;       // by instance id
    RawArray<uint32_t> materials;                   // the instances' materials, each instance's a range
    uint32_t materialsWasted;                       // the materials of removed instances that the pool still holds
    RawArray<uint32_t> moved;                       // the ids of the instances that are `moved`
    RawArray<RenderLightState> lights;              // by light id
    PrRenderSceneEnvironment environment;
    float time;                                     // in seconds, as the application set it
    uint32_t instanceCount;
    uint32_t lightCount;

    // The records that changed since they were uploaded, as a range of instance ids, empty if first > last.
    uint32_t dirtyFirst;
    uint32_t dirtyLast;

    // The scene's draws in the order of their buckets, and where each bucket begins. They are built again when
    // `drawsDirty` is set: when an instance, a mesh, or a material's type or render state changes.
    bool drawsDirty;
    RawArray<PrRenderDrawRecord> draws;
    uint32_t bucketFirst[kRenderBucketCount + 1];   // by bucket, and the number of draws
    uint32_t behindDraws;                           // the number of draws whose material reads the scene behind it
    uint32_t fallbackDraws;                         // the number of draws that read their layout's fallbacks

    // The deformations, the first of which is no deformation, and the entries that no instance uses.
    RawArray<RenderDeformation> deformations;
    RawArray<uint32_t> freeDeformations;

    // The vertices that the deforming instances draw, where each was a frame before, and the poses that deform them,
    // which are kept here as they are uploaded. Each bound deformation has a range of each.
    RenderRanges deformedRanges;
    RenderRanges poseRanges;
    RawArray<PrRenderVec4> poses;
    uint32_t poseDirtyFirst;        // the poses that changed since they were uploaded, empty if first > last
    uint32_t poseDirtyLast;
    RenderRecordBuffer deformedBuffer;
    RenderRecordBuffer previousBuffer;
    RenderRecordBuffer poseBuffer;

    RenderRecordBuffer instanceBuffer;
    RenderRecordBuffer drawBuffer;
    RenderRecordBuffer arguments[kRenderArgumentBuffers];
    PrGpuTexture shadowMap;

    // What the frame that is being built holds for the scene, in the graph's memory, or null before the frame's first
    // use of the scene.
    uint64_t frameSerial;
    RenderSceneFrame* PR_NULLABLE frame;
};

// What a frame holds for a scene, in the graph's memory: the scene's resources in the frame's graph, and the uniforms
// that every view of the scene shares, which include the light's view.
struct RenderSceneFrame {
    PrGpuGraphBuffer instances;
    PrGpuGraphBuffer draws;
    PrGpuGraphBuffer deformed;      // none, with the next, if no instance of the scene deforms
    PrGpuGraphBuffer previousPositions;
    PrGpuGraphTexture shadowMap;    // none if no light casts the scene's shadows
    uint32_t viewCount;             // the views of the scene that the frame has added
    PrRenderFrameUniforms uniforms;
};

// A set of changes to a scene, as commands of 32-bit words.
struct RenderUpdate {
    Render* render;
    RenderSceneState* scene;
    RawArray<uint32_t> words;

    // The ids that the update took, which a discarded update gives back.
    RawArray<uint32_t> addedInstances;
    RawArray<uint32_t> addedLights;
};

struct RenderMeterState;

// ---------------------------------------------------------------------------------------------------------------------
// Renderers

// One texel of a value, which stands in for a texture whose data has not arrived.
struct RenderStandIn {
    PrRenderVec4 value;
    PrGpuTexture texture;
};

// The slots that a library's source declares, by the names that the renderer binds, as the build reflects them
// (prGpuShaderLibrarySlots). A slot that the source does not declare is kRenderSlotNone, and is not bound.
struct RenderBindings {
    uint32_t instances;
    uint32_t draws;
    uint32_t parts;
    uint32_t materials;
    uint32_t arguments;
    uint32_t shadowMap;
    uint32_t environment;
    uint32_t tables;
    uint32_t color;
    uint32_t source;
    uint32_t vertices;
    uint32_t skin;
    uint32_t morphs;
    uint32_t pose;
    uint32_t deformed;
    uint32_t previous;
    uint32_t materialTextures[kRenderMaxMaterialSlots];
    uint32_t materialTextureCount;  // the slots of a material's textures, renderSlotMaterialTexture0 and after
};

// The slot of a name that a library's source does not declare.
constexpr uint32_t kRenderSlotNone = 0xffffffffu;

// The material types of the library's that every shader set has, which the renderer finds by their names.
enum {
    RenderLibraryType_KhronosPbr,
    RenderLibraryType_Unlit,
    RenderLibraryType_KhronosPbrLayered,
    RenderLibraryType_UsdPreview,
    RenderLibraryType_Count,
};
typedef uint8_t RenderLibraryType;

// The shader libraries, one for each pass's entry points. The first are the library's own. The last are those of the
// renderer's shader set, in the order of PrRenderShaderPass: the passes that call the materials and the shading models.
enum {
    RenderLibrary_Cull,
    RenderLibrary_Deform,
    RenderLibrary_Depth,
    RenderLibrary_Background,
    RenderLibrary_Display,
    RenderLibrary_EnvironmentFilter,
    RenderLibrary_Meter,
    RenderLibrary_DepthMask,
    RenderLibrary_Forward,
    RenderLibrary_Visibility,   // made only on a device with bindless
    RenderLibrary_Count,
};
typedef uint8_t RenderLibrary;
constexpr uint32_t kRenderLibraryFirstOfSet = RenderLibrary_DepthMask;

// What a pipeline draws.
enum {
    RenderPipelineKind_Depth,       // depth alone, of materials that are opaque everywhere
    RenderPipelineKind_DepthMask,   // depth alone, of masked materials
    RenderPipelineKind_Forward,     // the surfaces of one material type, shaded as they are drawn
    RenderPipelineKind_Background,
    RenderPipelineKind_Visibility,
    RenderPipelineKind_Shade,
    RenderPipelineKind_Display,
    RenderPipelineKind_Meter,
};
typedef uint8_t RenderPipelineKind;

// The states that a pipeline is made for.
struct RenderPipelineKey {
    RenderPipelineKind kind;
    RenderCullFlags cull;
    uint8_t samples;
    bool shadow;                // draws the light's view, whose depth grows with distance
    bool blend;                 // blends over the target, and tests depth without writing it
    bool mask;                  // a visibility pipeline that tests a masked material's alpha
    PrGpuFormat format;         // of the display's target
    uint32_t variant;           // the variant of a material type that a forward pipeline shades

    // The layout of the attributes that a pipeline fetches, by its index in the shader set, plus one: zero fetches the
    // geometry alone. A forward pipeline's is its variant's, and a masked depth's is the layout of the variants that it
    // draws.
    uint32_t layout;
};

// A pipeline that the renderer made, and the states that it was made for, also as one number.
struct RenderPipeline {
    RenderPipelineKey key;
    uint64_t packed;
    PrGpuPipeline pipeline;
};

// A renderer: its device objects, its queues, which any thread fills, and its resources and scenes.
struct Render {
    PrGpuDevice* device;
    PrRenderOptions options;
    const PrRenderShaderSet* shaders;                               // the material types and the shading models
    bool bindless;                                                  // the shaders reach resources by handle, not
                                                                    // through slots
    PrGpuShaderLibrary* PR_NULLABLE libraries[RenderLibrary_Count]; // by library
    RenderBindings bindings[RenderLibrary_Count];                   // by library, on a device without bindless
    uint32_t libraryTypes[RenderLibraryType_Count];                 // the ids of the library's types in the set

    // The parameters of the material that stands in for one that is missing or not set: a rough gray dielectric of the
    // library's glTF type.
    RawArray<uint8_t> neutralParameters;
    PrGpuPipeline cull;
    PrGpuPipeline deform;

    // The pipelines of the draws, which any thread makes (render_pipeline.h), and the time that making them took.
    // Guarded by `pipelineLock`.
    PrMutex pipelineLock;
    RawArray<RenderPipeline> pipelines;
    double pipelineSeconds;

    PrGpuTexture white;         // one white texel, which a material reads for a texture that it does not name
    PrGpuSampler whiteSampler;
    PrGpuSampler behindSampler;     // filters the scene behind a surface between texels and between levels

    // The texels that stand in for textures whose data has not arrived: one for each value that a material type of the
    // shader set declares. `pendingTextures` holds the texel of each texture of each type: a type's are consecutive,
    // from `pendingFirst` of the type's id.
    RawArray<RenderStandIn> standIns;
    RawArray<PrGpuTexture> pendingTextures;
    RawArray<uint32_t> pendingFirst;

    PrGpuTexture tables;        // the tables of the shader set, in one texture (render_tables.h)

    // What environments are made and read with: a black cube, which stands in for an environment without light, the
    // sampler of every environment's cube, and the pipelines that fill a cube's levels.
    PrGpuTexture blackCube;
    PrGpuSampler environmentSampler;
    PrGpuPipeline environmentReduce;
    PrGpuPipeline environmentSpecular;

    // Guarded by `lock`.
    PrMutex lock;
    RenderIds meshIds;
    RenderIds textureIds;
    RenderIds materialIds;
    RenderIds environmentIds;
    RenderIds sceneIds;
    RenderIds meterIds;
    RawArray<RenderMeterState*> meters;             // by meter id, null for none
    RawArray<RenderSceneState*> scenes;             // by scene id, null for none
    RawArray<RenderOp> ops;
    RawArray<RenderUpdate*> committed;

    // The rest belongs to the frame thread.
    RawArray<RenderMeshState> meshes;                 // by mesh id
    RawArray<RenderTextureState> textures;            // by texture id
    RawArray<RenderMaterialState> materials;          // by material id
    RawArray<PrRenderMaterialRecord> materialRecords; // by material id
    RawArray<RenderMaterialSlots> materialSlots;      // by material id, on a device without bindless
    RawArray<uint8_t> materialBytes;                  // the materials' buffer: the records, and what they name
    RawArray<RenderEnvironmentState> environments;    // by environment id
    RawArray<PrRenderPartRecord> parts;
    RenderRanges partRanges;
    RawArray<RenderSceneState*> frameScenes;        // by scene id, as of the frame that began last
    uint32_t partsDirtyFirst;                       // the parts that changed since they were uploaded, empty if
    uint32_t partsDirtyLast;                        // first > last
    bool materialsDirty;
    RenderRecordBuffer partBuffer;
    RenderRecordBuffer materialBuffer;

    // The frame that is being built.
    uint64_t frameSerial;
    PrGpuGraph* PR_NULLABLE graph;
    PrGpuGraphBuffer frameParts;
    PrGpuGraphBuffer frameMaterials;
};

// Makes `buffer` hold at least `size` bytes of records. Returns true if it made a new buffer, whose contents are zero,
// so that the caller uploads every record again.
bool renderRecordBufferReserve(Render* render, RenderRecordBuffer* buffer, uint64_t size, PrGpuBufferUsageFlags usage,
    PrStringView name);

// Returns the scene by an id as of the frame that began last, or null. Called on the frame thread.
RenderSceneState* PR_NULLABLE renderSceneFind(const Render* render, uint32_t id);

// Grows an array to hold `count` items, with zeroed bytes in the new ones.
template <typename T>
void renderArrayGrow(RawArray<T>* PR_NONNULL array, uint32_t count) {
    uint32_t old = array->count;
    if (count <= old)
        return;
    array->resize(count);
    prMemZero(array->data + old, size_t(count - old) * sizeof(T));
}

// Grows a table that an id indexes to hold `count` ids and none, with zeroed entries for the new ids.
template <typename T>
void renderTableGrow(RawArray<T>* PR_NONNULL table, uint32_t count) {
    uint32_t old = table->count;
    if (count + 1 <= old)
        return;
    table->resize(count + 1);
    prMemZero(table->data + old, size_t(count + 1 - old) * sizeof(T));
}

} // namespace pr_impl

// C opaque types
struct PrRender : pr_impl::Render {};
struct PrRenderUpdate : pr_impl::RenderUpdate {};

PR_ASSUME_NONNULL_END
