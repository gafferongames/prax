// A model's state, which the sources that implement pr/model.h share.
//
// The state has three parts. What describes the asset is made once, when the source's description arrives, and is read
// by any thread after that. What a renderer holds for the model is made again by each prModelPopulate. A job reads the
// first part, sets data in the renderer, and writes only its own resource's load state. Until the description arrives
// the model has no nodes, meshes or materials, and its placements wait for them.

#pragma once

#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/model.h"
#include "pr/model_source.h"
#include "pr/mutex.h"
#include "pr/raw_array.h"
#include "pr/render_shaders.h"
#include "pr/thread.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Model;

// How a resource has loaded: a PrModelLoad and the PrStatus of a failure, which a job writes and any thread reads.
struct ModelLoadState {
    Atomic<uint8_t> load;
    Atomic<uint8_t> status;
};

// A part of the source's meshes, as the model prepares it.
struct ModelPart {
    bool wantsTangents;         // its material reads a normal map or a direction of its surface, which need tangents
    uint8_t tangentSet;         // the set of texture coordinates that the tangents follow
    bool blends;                // its material blends, so the order of its triangles shows
};

// A mesh of the source.
struct ModelMesh {
    uint32_t firstPart;         // of the model's parts, which are the source's
    uint32_t partCount;
    PrRenderMesh mesh;          // the renderer's mesh, or none without a renderer or without a part
    ModelLoadState state;

    // The most joints of a skin that a draw of the scene draws the mesh with, or zero if none draws it with a skin, and
    // the mesh's morph targets.
    uint32_t jointCount;
    uint32_t morphTargetCount;

    // By joint, the box of the positions that the joint moves, which is empty, with its least corner above its
    // greatest, for a joint that moves none. `jointBoxesRead` is false if the mesh does not read.
    PrBox3* PR_NULLABLE jointBoxes;
    bool jointBoxesRead;
};

// A mesh's triangles as picks test them (prModelPlacementPick), as the renderer draws them: its positions as it was
// bound, as three floats a vertex, its indices, the joints of each vertex of a mesh that a draw has a skin for, what
// each morph target adds to each position, and the first index and the source's part of each of its parts.
//
// The hierarchy is over the triangles at `positions` for a mesh that does not deform. A mesh that deforms is tested in
// each pick's shape, which refits the hierarchy into the model's scratch (prMeshBvhRefit), and builds it again over
// that shape when the refit costs too much more than the build did (prMeshBvhCost). Its boxes are then those of the
// shape it was last built over, and only its tree and triangle order are read.
struct ModelPickMesh {
    bool prepared;              // the fields below are read; a mesh whose data does not read has no triangles
    bool deforms;               // a draw has a skin for the mesh, or the mesh has morph targets
    uint32_t morphTargetCount;  // the targets of each vertex in `morphs`
    RawArray<float> positions;
    RawArray<uint32_t> indices;
    RawArray<PrRenderSkinVertex> skin;
    RawArray<float> morphs;     // by vertex and then by target, the three floats that the target adds to the position
    RawArray<uint32_t> partFirstIndex;
    RawArray<PrModelPartIndex> partSources;
    RawArray<PrMeshBvhNode> nodes;
    RawArray<uint32_t> triangles;
    PrMeshBvhView bvh;
    float builtCost;            // the hierarchy's cost when it was built (prMeshBvhCost)
};

// The storage that a pick deforms a mesh into: the positions of its vertices in the shape that a draw gives them, the
// hierarchy refitted to them, and the joints of the draw's skin. Each pick reuses it.
struct ModelPickScratch {
    RawArray<float> positions;
    RawArray<PrMeshBvhNode> nodes;
    RawArray<PrAffine3> joints;
};

// An image of the source.
struct ModelImage {
    bool used;                  // a texture that a material reads has this image, or the environment does
    ModelLoadState state;
    Atomic<uint64_t> textureBytes;  // the bytes of its texels in the renderer's textures, once it is ready
    Atomic<uint8_t> colorMismatch;  // nonzero if a texture samples it as data where its source is of a Srgb form
};

// One of the renderer's textures: an image of the source, for one use of it by the materials, which its chain of
// sources is made for (pr/model_texture.h). The materials' textures of an image that agree in the kind that their types
// declare and in how the source samples their texels share one, and its channels are those that any of them reads, so
// an image that materials read both as color and as data is two of these.
struct ModelTexture {
    PrModelImageIndex image;            // or kPrModelImageNone for a texture without an image, which stays white
    PrRenderTextureKind kind;
    PrModelTextureColor color;          // through the Srgb form of its format or the plain one
    PrRenderChannelFlags channels;      // the channels that the materials read, red to alpha
    PrRenderTexture texture;
};

// The absent index of a model's texture.
constexpr uint32_t kModelTextureNone = 0xffffffffu;

// The environment that the asset brings (prModelEnvironmentNode), in the model's renderer.
struct ModelEnvironment {
    PrRenderEnvironment environment;    // or none
};

// The work of loading one resource.
enum {
    ModelJobKind_Mesh,
    ModelJobKind_Image,
};
typedef uint8_t ModelJobKind;

// A job, which a thread of the pool or prModelUpdate runs once. A job finishes when its last asynchronous step does.
struct ModelJob {
    Model* model;
    ModelJobKind kind;
    uint32_t index;             // of the mesh or the image in the source
};

// A draw of the source in the model's scene.
struct ModelDraw {
    PrModelNodeIndex node;
    PrModelMeshIndex mesh;
    PrModelSkinIndex skin;      // or kPrModelSkinNone
    bool animated;              // an animation moves the node or one of its ancestors, or the mesh deforms
    uint32_t firstWeight;       // the first of the draw's morph weights among the weights of every draw
    uint32_t weightCount;       // the number of morph targets of the draw's mesh

    // The draw's instances among the instances of every draw, one for a draw at its node. Each is the renderer's
    // instance of the mesh at `Model::instanceLocal` of its own within the node.
    uint32_t firstInstance;
    uint32_t instanceCount;
};

// A placement's use of an animation.
struct ModelPlayback {
    PrModelAnimationIndex animation;    // or kPrModelAnimationNone
    float seconds;
};

// A node's translation, rotation and scale, or its matrix where a track gives it one.
struct ModelTrs {
    float translation[3];
    float rotation[4];
    float scale[3];
    bool hasMatrix;
    PrMatrix4 matrix;
};

// One placement of a model: where it is, what it plays, its pose, and its instances in its scene. Its arrays are made
// once its model is described.
struct ModelPlacement {
    Model* model;
    PrModelPlacementDesc desc;
    PrModelLoop loop;
    ModelPlayback playback;
    ModelPlayback blend;
    float blendWeight;
    bool described;                     // the arrays below are made

    bool poseDirty;                     // `worlds` is behind the root, the animation or its time
    bool sceneDirty;                    // the scene's instances and lights are behind `worlds`
    ModelTrs* pose;                     // by node: scratch for the pose that `worlds` is computed from
    ModelTrs* blendPose;                // by node: scratch for the pose of the animation that is blended in
    PrMatrix4* scenes;                  // by node: the matrix that maps the node to the asset's scene
    PrMatrix4* worlds;                  // by node: the matrix that maps the node to the world
    bool* visible;                      // by node: the node and its ancestors are visible
    bool* blendVisible;                 // by node: scratch for the visibility of the animation that is blended in
    PrModelSourceLight* lights;         // by light of the source: the light in the pose
    PrModelSourceLight* blendLights;    // by light of the source: scratch for the animation that is blended in
    PrModelSourceCamera* cameras;       // by camera of the source: the camera in the pose
    float* weights;                     // the morph weights of every draw, each draw's a range
    float* blendWeights;                // scratch for the weights of the animation that is blended in
    float* trackValues;                 // scratch for the value of one track
    PrAffine3* transforms;              // by the model's instance: scratch for the transforms that an update takes
    PrAffine3* joints;                  // scratch for the joints of one skin that an update takes

    // The variant of the asset's materials that the placement draws, or kPrModelVariantNone, and the renderer's
    // material of each part in it, by part.
    PrModelVariantIndex variant;
    PrRenderMaterial* partMaterials;
    bool materialsDirty;                // the instances' materials are behind `partMaterials`

    // The node whose meshes, and those of the nodes under it, draw with `highlight` in place of their materials
    // (prModelPlacementSetHighlight), or kPrModelNodeNone.
    PrModelNodeIndex highlightNode;
    PrRenderMaterial highlight;

    // For a model whose animations change materials: by input of the source, the inputs in the pose and scratch for
    // those of the animation that is blended in; by material, whether the pose's animation has a track of it, and the
    // renderer's material that the placement draws it with, which it makes when it first plays such a track in a scene,
    // or none.
    PrModelSourceInput* inputPose;
    PrModelSourceInput* blendInputs;
    bool* materialPlayed;
    bool* blendMaterialPlayed;
    PrRenderMaterial* ownMaterials;

    // The placement's instances in its scene, by the model's instance, whether each draw is shown, by the model's draw,
    // and its lights, by the model's light. Each is none while the placement is in no scene.
    PrRenderInstance* instances;
    bool* shown;
    PrRenderLight* sceneLights;
    float* lightScales;                 // by the model's light: the factor of its intensity
};

// A material of the source as the model's renderer draws it: the renderer's type that the binder chose for it, which is
// none without a renderer, and the type's optional attributes that the material binds, which choose its variant.
struct ModelBinding {
    uint32_t type;                      // of the renderer's shader set, or kPrRenderMaterialTypeMissing
    uint32_t attributes;                // by the type's optional attributes, a bit for each
};

// A model: what it read of its source, its placements, and what it holds in its renderer.
struct Model {
    // The source, once it is known, and its description. The counts are the description's.
    PrModelSource* PR_NULLABLE source;
    const PrModelSourceInfo* PR_NULLABLE info;
    PrModelOptions options;
    bool described;                     // the description is read into the arrays below

    // The opening of a model from bytes (prModelOpen): its formats, and the source or the failure that the pipeline
    // gave, under `lock`, which prModelUpdate takes to describe the model.
    RawArray<PrModelFormat> formats;
    PrModelSource* PR_NULLABLE opened;
    ModelLoadState sourceState;
    Atomic<uint64_t> revision;          // counts the changes that a consumer sees (prModelRevision)

    RawArray<PrModelDiagnostic> diagnostics;
    uint32_t unsampledTextures;         // the materials' textures past what the renderer's device samples

    // The description, as the model reads it.
    ModelPart* parts;                   // by part of the source
    ModelMesh* meshes;                  // by mesh of the source
    ModelImage* images;                 // by image of the source
    PrModelImageIndex* textureImages;   // by texture of the source: the image that it draws, or kPrModelImageNone

    // The renderer's textures, and by input of the source, the index among them of the input's texture, or
    // kModelTextureNone. Both are made again by each prModelPopulate.
    RawArray<ModelTexture> textures;
    uint32_t* inputTextures;
    PrRenderMaterial* materials;        // by material of the source
    ModelBinding* bindings;             // by material of the source
    bool animatesMaterials;             // an animation changes a material
    PrRenderMaterial defaultMaterial;   // the material of parts without one, or none if every part has a material
    PrRenderMaterial* partMaterials;    // by part: the material of each part
    RawArray<PrModelNodeIndex> order;   // the scene's nodes, each after its parent
    bool* inScene;                      // by node of the source
    RawArray<ModelDraw> draws;
    uint32_t* nodeDraws;                // by node: one more than the index of the node's first draw, or zero
    PrMatrix4* instanceLocal;           // by instance: its transform within its node
    uint32_t instanceCount;             // the instances of every draw
    uint32_t weightCount;               // the morph weights of every draw
    uint32_t jointCount;                // the most joints of a skin
    uint32_t trackWidth;                // the most values of a track
    RawArray<PrModelNodeIndex> cameras;
    RawArray<PrModelNodeIndex> lights;
    ModelEnvironment environment;

    RawArray<ModelPlacement*> placements;

    // The renderer that the model populates, and the jobs that load it. `jobsNext` is the next job that prModelUpdate
    // runs without a pool. `jobsPending` counts the jobs that have not finished, and the opening of the source, under
    // `lock`, and `idle` is signaled when it reaches zero. A job that finds `cancelled` set does nothing.
    PrRender* PR_NULLABLE render;
    RawArray<const PrRenderVertexLayout*> layouts;  // of the variants that the model's materials draw with
    RawArray<ModelJob> jobs;
    uint32_t jobsNext;
    PrMutex lock;
    PrCondition idle;
    uint32_t jobsPending;
    Atomic<bool> cancelled;

    // By mesh of the source, what picks test, made at the first pick that reaches a mesh, or null before any pick, and
    // the storage of the picks of meshes that deform.
    ModelPickMesh* PR_NULLABLE picks;
    ModelPickScratch pickScratch;
};

// Ends a job, or the opening of the source: counts it as finished, and signals `idle` once nothing is pending.
void modelJobFinish(Model* m);

// Adds a diagnostic of `feature`, a string of the program or of the source, unless the model has one of it.
void modelDiagnose(Model* m, PrStringView feature, PrStringView detail);

// Returns the input of a material that `name` names among `inputs`, the inputs of every material, or null.
const PrModelSourceInput* PR_NULLABLE modelMaterialInput(const Model* m, PrModelMaterialIndex material,
    const PrModelSourceInput* inputs, PrStringView name);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
