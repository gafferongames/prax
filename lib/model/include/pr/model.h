// Models: an asset of a scene format made ready to draw with a renderer (pr/render.h), through a source that describes
// it in no format's terms (pr/model_source.h).
//
// A model owns what the renderer has no notion of: the asset's node hierarchy, its cameras, its lights and its
// animations. It creates the asset's meshes, textures and materials as the renderer's resources (prModelPopulate), and
// a placement of the model (PrModelPlacement) puts an instance of each of its meshes into a scene and keeps their
// transforms in step with the hierarchy and an animation. Where a call says "the asset", it means what the source
// describes.
//
// Loading never blocks. Populating a renderer returns at once, with every resource created and none loaded. The model
// then decodes each image and prepares each mesh as a job of its own, on the task pool that it was given, or one job at
// each prModelUpdate without a pool. A model is placed and drawn from the first frame, and each part appears when its
// data arrives, as the renderer draws a resource without data. A resource that fails to load fails alone: the model
// reports it (prModelImageInfo, prModelMeshInfo) and draws the rest.
//
// Threads: one thread at a time calls a model and its placements, and the model's jobs run beside it. That thread is
// usually the one that builds frames, although no call needs it to be.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/mesh_bvh.h"
#include "pr/model_source.h"
#include "pr/model_texture.h"
#include "pr/render.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/task.h"
#include "pr/text_buffer.h"
#include "pr/texture.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Models

// An asset's source and the renderer's resources that draw it.
typedef struct PrModel PrModel;

// The host services that a model loads with, and its policy.
typedef struct PrModelOptions {
    // Makes the sources of the asset's images, in the formats that the program built it with (pr/texture.h). It
    // outlives the model. Without it, every image fails as NotSupported, and the materials draw with their factors
    // alone. An image whose format the opener lacks fails as NotSupported too.
    const PrTextureOpener* PR_NULLABLE textures;

    // Makes the chain of sources of each texture of an image for the use that the materials make of it, and its
    // context, which outlives the model (pr/model_texture.h). Null means prModelTextureChainDefault.
    PrModelTextureChainFn PR_NULLABLE chain;
    void* PR_NULLABLE chainContext;

    // Runs the jobs that load the model. It outlives the model. Without it, each prModelUpdate runs one job.
    PrTaskPool* PR_NULLABLE pool;

    // The unit of the emission of the asset's materials (pr/render_resource.h). glTF's core material and
    // UsdPreviewSurface state luminance in nits, which is the default, and an asset authored for a fully exposed pixel
    // at an emission of 1 draws as it was meant in the view's unit. An unlit material's color is the view's whatever
    // this says.
    PrRenderEmission emission;
} PrModelOptions;

// Returns options with no services, with emission in nits.
PrModelOptions prModelOptionsDefault(void);

// Creates a model of `source`, which it takes and destroys with the model. The model reads the source's description
// when it is created, and reads its meshes and images when it populates a renderer. The model has no renderer until
// prModelPopulate gives it one. Null `options` means the defaults.
PrModel* prModelCreate(PrModelSource* source, const PrModelOptions* PR_NULLABLE options);

// Opens a model of the resource that `bytes` provides at `location`, which names it, and returns it at once, without a
// source. `resolver` gives the formats what the resource names, and outlives the opening, until prModelProgress reports
// the source as other than pending. The model classifies the resource's first bytes and its location (pr/mime_sniff.h),
// and the first of the `formatCount` formats of `formats`, which it copies, that supports the resource's media type and
// does not refuse it makes its source. Each step may complete later, on any thread. The prModelUpdate after the source
// arrives describes the model: its nodes, meshes, materials, cameras, lights and animations appear, its placements
// enter their scenes, and in a renderer its resources are created and begin loading. Until then the model has none of
// them, and its calls return what an empty asset has. A model whose resource does not read, or that no format opens,
// stays empty, and prModelProgress reports the failure.
PrModel* prModelOpen(const PrBlobSource* bytes, PrStringView location, const PrModelResolver* resolver,
    const PrModelFormat* PR_NULLABLE formats, uint32_t formatCount, const PrModelOptions* PR_NULLABLE options);

// Destroys the model and its placements, as prModelPopulate with no renderer does first.
void prModelDestroy(PrModel* PR_NULLABLE model);

// Returns the model's source, which describes the asset (prModelSourceInfo), or null before it is known.
const PrModelSource* PR_NULLABLE prModelSource(const PrModel* model);

// Returns a number that grows each time the model changes in a way that a consumer shows: its description arrives, its
// opening fails, a resource finishes loading, or it populates a renderer. A view that shows a model polls it once a
// frame, after prModelUpdate, and refreshes what it shows when the number changes.
uint64_t prModelRevision(const PrModel* model);

// Return the number of the asset's nodes, and a node's parent, or kPrModelNodeNone for a root, and its name.
uint32_t prModelNodeCount(const PrModel* model);
PrModelNodeIndex prModelNodeParent(const PrModel* model, PrModelNodeIndex node);
PrStringView prModelNodeName(const PrModel* model, PrModelNodeIndex node);

// Append a node's address within the asset, as its format's addresses end, such as `/nodes/3` or `/World/Car`, and
// return the node at such an address, or kPrModelNodeNone (prModelSourceNodeAddress).
void prModelNodeAddress(const PrModel* model, PrModelNodeIndex node, PrTextBuffer* out);
PrModelNodeIndex prModelNodeFromAddress(const PrModel* model, PrStringView address);

// Return the number of the asset's animations, and an animation's name and duration in seconds.
uint32_t prModelAnimationCount(const PrModel* model);
PrStringView prModelAnimationName(const PrModel* model, PrModelAnimationIndex animation);
float prModelAnimationDuration(const PrModel* model, PrModelAnimationIndex animation);

// Makes `render` the model's renderer: creates the model's meshes, textures and materials in it, and starts loading
// them. Returns without waiting for any of them. This one call serves a first load and the recovery from a lost device,
// for which the application makes a renderer on a new device and populates it.
//
// The call first ends the model's use of the renderer that it had. It waits for a job that is running, which is at most
// one image or one mesh for each thread of the pool. It removes every placement from its scene, and destroys the
// resources that the model created, all of which is safe on a lost device. The placements remain, in no scene, with
// their animations (prModelPlacementSetScene). So the renderer that a model has outlives the model's use of it: the
// application populates another renderer, or none, before it destroys the renderer.
//
// A null `render` leaves the model with no renderer.
void prModelPopulate(PrModel* model, PrRender* PR_NULLABLE render);

// Returns the renderer that the model populates, or null.
PrRender* PR_NULLABLE prModelRender(const PrModel* model);

// Sets the unit of the emission of the asset's materials (PrModelOptions::emission), and returns it. The model sets its
// materials in its renderer again, and the frames that begin after the call draw them in the new unit.
void prModelSetEmission(PrModel* model, PrRenderEmission emission);
PrRenderEmission prModelEmission(const PrModel* model);

// What an asset brings that decides how a host lights and exposes it, so that a host that shows many assets can choose
// its defaults by asset. The answer depends on the asset alone.
enum {
    // The model's scene holds a light (prModelLightCount).
    PrModelLightingFlag_Lights = 1u << 0,

    // The asset brings an environment (prModelEnvironmentNode).
    PrModelLightingFlag_Environment = 1u << 1,

    // A surface that the model's scene draws has a material that emits: an emission above zero, or from a texture.
    PrModelLightingFlag_Emission = 1u << 2,

    // The model's scene draws surfaces, and every one of them is unlit (PrModelShading_Unlit), so no light or
    // environment changes what it shows.
    PrModelLightingFlag_Unlit = 1u << 3,
};
typedef uint32_t PrModelLightingFlags;

// Returns what the asset brings of its own lighting.
PrModelLightingFlags prModelLighting(const PrModel* model);

// Brings the model's scenes up to date: writes the transforms of the placements that moved, and the lights that moved
// with them, each scene's in one update of its own (prRenderUpdateBegin), which the call commits. Without a pool, the
// call also runs one loading job, so an application that calls it once a frame loads a model over several frames.
void prModelUpdate(PrModel* model);

// Returns once nothing of the model is loading. With a pool, the call waits for the jobs. Without one, it runs them.
void prModelWait(PrModel* model);

// ---------------------------------------------------------------------------------------------------------------------
// Loading

// How far a resource of a model has loaded.
enum {
    PrModelLoad_None,       // the model does not load it: no material uses the image, or the mesh has no triangles
    PrModelLoad_Pending,    // its job has not finished, or the model has no renderer
    PrModelLoad_Ready,      // the renderer has its data
    PrModelLoad_Failed,     // the renderer has no data for it, and draws without it
};
typedef uint8_t PrModelLoad;

// How one image or one mesh has loaded.
typedef struct PrModelResourceInfo {
    PrModelLoad load;

    // Of an image that is ready: whether its source is of a format's Srgb form, as a KTX 2.0 file whose descriptor
    // states sRGB's transfer function is, and a material samples it as data all the same. The material's use decides
    // how a texture is sampled either way.
    bool colorMismatch;

    // Why a failed resource failed. NotSupported if no format of the opener opens the image, the model has no opener,
    // or the device samples no chain of the texture that the policy makes, InvalidData for an image that does not
    // decode or a mesh that lib/mesh refuses, Io for data whose buffer or URI was not loaded with the asset, Limit for
    // an image of more than 8192 squared texels or data that is larger than the device takes, and Lost for a lost
    // device.
    PrStatus status;
} PrModelResourceInfo;

// How far a model has opened its source, the numbers of the images and meshes that a model loads, and of those that are
// ready and that failed, and the bytes of the texels of the renderer's textures that show the images that are ready:
// every mip level of each, whether the image brought it or the renderer made it.
typedef struct PrModelProgress {
    PrModelLoad source;         // the source: Pending while it is opened, Ready once the model is described
    PrStatus sourceStatus;      // why the source failed: the read's failure, or NotSupported for no format
    uint32_t imageCount;
    uint32_t imagesReady;
    uint32_t imagesFailed;
    uint64_t textureBytes;
    uint32_t meshCount;
    uint32_t meshesReady;
    uint32_t meshesFailed;
} PrModelProgress;

// Returns how far the model has loaded.
PrModelProgress prModelProgress(const PrModel* model);

// Returns true if the model's opening failed, or the model is described, has a renderer and none of its resources is
// pending. Some may have failed.
bool prModelIsLoaded(const PrModel* model);

// Return how one image or one mesh of the asset has loaded.
PrModelResourceInfo prModelImageInfo(const PrModel* model, PrModelImageIndex image);
PrModelResourceInfo prModelMeshInfo(const PrModel* model, PrModelMeshIndex mesh);

// Return the number of what the asset has that the model does not draw as the asset means it, and each of them: what
// the source reports (PrModelSourceInfo::diagnostics), and then what the model's materials lose to the renderer's
// types, such as an input that a type has no texture, parameter or attribute for, which the model finds when it
// populates a renderer. Each feature is reported once.
uint32_t prModelDiagnosticCount(const PrModel* model);
PrModelDiagnostic prModelDiagnostic(const PrModel* model, uint32_t index);

// Returns true if the model reports `feature` (prModelDiagnostic).
bool prModelHasDiagnostic(const PrModel* model, PrStringView feature);

// Returns the number of textures of the model's materials that the renderer's device does not sample, or zero for a
// model that is in no renderer. A device without bindless samples a limited number of different textures for one
// material (prRenderMaterialTextureLimit), and a material that names more draws the rest as the factors alone: without
// that layer, or with it even across the surface. The count is of the textures past the limit, over the materials.
uint32_t prModelUnsampledTextureCount(const PrModel* model);

// Return the renderer's mesh for a mesh of the asset, and its material for a material of the asset, or none if the
// model has no renderer. A mesh without parts has none. A part of the renderer's mesh is a part of the source's mesh,
// in their order. The ids are valid until the model is populated again or destroyed.
PrRenderMesh prModelMesh(const PrModel* model, PrModelMeshIndex mesh);
PrRenderMaterial prModelMaterial(const PrModel* model, PrModelMaterialIndex material);

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

// Return the number of cameras that the model's scene holds, and the node of each. A camera of the asset counts once
// for each node of the scene that carries it.
uint32_t prModelCameraCount(const PrModel* model);
PrModelNodeIndex prModelCameraNode(const PrModel* model, uint32_t index);

// Return the number of lights that the model's scene holds, and the node of each, counted as the cameras are.
uint32_t prModelLightCount(const PrModel* model);
PrModelNodeIndex prModelLightNode(const PrModel* model, uint32_t index);

// Returns the node of the environment that the asset brings, or kPrModelNodeNone for an asset without one
// (PrModelSourceInfo::environment).
//
// The environment is the model's resource in its renderer: made when the model populates a renderer, from its texture,
// which loads as one of the model's images, or from its color alone. It is black until its texture loads. A scene's
// environment is the application's, which takes the model's for a placement's scene where it chooses to
// (prModelPlacementEnvironment).
PrModelNodeIndex prModelEnvironmentNode(const PrModel* model);

// ---------------------------------------------------------------------------------------------------------------------
// Placements

// One placement of a model in a scene: an instance of each mesh that the model's scene draws, under a root transform,
// in a pose that is the placement's own. A model is placed any number of times.
typedef struct PrModelPlacement PrModelPlacement;

// Where a placement is, and what it adds to its scene.
typedef struct PrModelPlacementDesc {
    PrRenderScene scene;        // a scene of the model's renderer, or none
    PrMatrix4 root;             // maps the asset's scene to the world
    uint32_t visibility;        // the visibility mask of the placement's instances (PrRenderInstanceDesc)

    // The placement adds the asset's lights to the scene, and moves them with their nodes. None of them casts shadows.
    bool addsLights;
} PrModelPlacementDesc;

// Returns a placement in `scene` at the identity, that every view sees, that casts shadows, and that adds the asset's
// lights.
PrModelPlacementDesc prModelPlacementDescDefault(PrRenderScene scene);

// Creates a placement of the model in its rest pose: every node as the asset gives it, and no animation. If the
// description names a scene, the model has a renderer, and the placement's instances exist in the scene's frames from
// the next one on.
PrModelPlacement* prModelPlacementCreate(PrModel* model, const PrModelPlacementDesc* desc);

// Removes the placement's instances and lights from its scene, and frees the placement.
void prModelPlacementDestroy(PrModelPlacement* PR_NULLABLE placement);

// Moves the placement to another scene of the model's renderer, or to none. A placement is in no scene after its model
// is populated again, and this call puts it in a scene of the new renderer, in the pose that it had.
void prModelPlacementSetScene(PrModelPlacement* placement, PrRenderScene scene);

// Sets the transform that maps the asset's scene to the world. The next prModelUpdate moves the instances.
void prModelPlacementSetRoot(PrModelPlacement* placement, const PrMatrix4* root);

// Draws the placement's parts with the materials of a variant of the asset (PrModelSourceInfo::variants): each part
// with the material that the variant maps it to, or else with its own. kPrModelVariantNone draws every part with its
// own, as a placement does from its creation. The next prModelUpdate changes the materials.
void prModelPlacementSetVariant(PrModelPlacement* placement, PrModelVariantIndex variant);

// Returns the placement's variant, or kPrModelVariantNone.
PrModelVariantIndex prModelPlacementVariant(const PrModelPlacement* placement);

// Draws the meshes of `node`, and of the nodes under it, with `material`, a material of the model's renderer, in place
// of their own, as a tool marks what is selected. kPrModelNodeNone or no material draws every mesh with its own. The
// next prModelUpdate changes the materials. The material is the caller's, and outlives its use here.
void prModelPlacementSetHighlight(PrModelPlacement* placement, PrModelNodeIndex node, PrRenderMaterial material);

// ---------------------------------------------------------------------------------------------------------------------
// Animation
//
// A placement plays one of the asset's animations at a time of its own. The animation sets the properties that it has
// tracks for: the transforms and the visibility of nodes, the weights of morph targets, and the properties of lights,
// cameras and materials, and everything else keeps what the asset gives it at rest. The next prModelUpdate after a
// change writes the pose to the scene: the transforms of the instances, the joints of the skinned ones, which follow
// the nodes of their skins, and the morph weights.

// What a placement shows at a time past the end of its animation.
enum {
    PrModelLoop_Once,       // the pose at the animation's end
    PrModelLoop_Repeat,     // the animation again from its start
};
typedef uint8_t PrModelLoop;

// Plays an animation of the asset from its start, or returns the placement to its rest pose with kPrModelAnimationNone.
// It also ends a blend.
//
// An animation that changes an input of a material or a transform of one of its textures changes it for this placement
// alone. The placement draws such a material with a material of the renderer's that it owns, which it makes when it
// first plays a channel of the material in a scene, and which it keeps until it leaves the scene; prModelMaterial still
// returns the material at rest.
void prModelPlacementSetAnimation(PrModelPlacement* placement, PrModelAnimationIndex animation, PrModelLoop loop);

// Return the placement's animation, or kPrModelAnimationNone, and its time in the animation, in seconds.
PrModelAnimationIndex prModelPlacementAnimation(const PrModelPlacement* placement);
float prModelPlacementTime(const PrModelPlacement* placement);

// Set the time in the placement's animation, in seconds, and move it by `delta` seconds, which may be negative. A time
// outside the animation is brought into it as the placement's loop says.
void prModelPlacementSetTime(PrModelPlacement* placement, float seconds);
void prModelPlacementAdvance(PrModelPlacement* placement, float delta);

// Blends a second animation into the pose: each node is at the mix of what the placement's animation gives it and what
// `animation` gives it at `seconds`, by `weight` from 0 for the first alone to 1 for the second alone. Translations,
// scales and morph weights mix linearly, and rotations along the shorter arc. A node that only one of the two animates
// mixes with what the asset gives it. The second animation's time is brought into it as the placement's loop says, and
// prModelPlacementAdvance does not move it. kPrModelAnimationNone ends the blend.
void prModelPlacementSetBlend(PrModelPlacement* placement, PrModelAnimationIndex animation, float seconds, float weight);

// ---------------------------------------------------------------------------------------------------------------------
// What a placement holds in the world

// Returns the matrix that maps a node of the asset to the world, in the placement's pose.
PrMatrix4 prModelPlacementNodeWorld(PrModelPlacement* placement, PrModelNodeIndex node);

// Writes to `out` a box of the world that holds the placement's meshes in its pose, for framing a camera. The box holds
// the box of each part that the placement draws, as the source states it (PrModelSourcePart::bounds), so it is known
// before anything loads, and it is as true as the source. A mesh with morph targets counts with the farthest that its
// targets move it at their weights, by the bounds that the source states for them. A skinned mesh counts in its pose,
// by the box of the positions that each joint moves, under that joint. Returns false, and writes an empty box at the
// origin, if the placement draws nothing.
bool prModelPlacementBounds(PrModelPlacement* placement, PrBox3* out);

// What a ray meets of a placement's meshes.
typedef struct PrModelPick {
    PrModelNodeIndex node;              // the node that draws the mesh
    PrModelMeshIndex mesh;
    PrModelPartIndex part;              // the source's part that holds the triangle
    uint32_t instance;                  // the draw's instance of the mesh, or 0
    float t;                            // the hit is at the ray's origin + t * its direction
    PrVec3 position;                    // the hit, in the world
} PrModelPick;

// Finds the nearest triangle of the placement's meshes that `ray`, in the world, meets, on either side, and writes what
// it meets to `*out`. Returns false if the ray meets none. A hidden node's meshes are not met.
//
// Every instance of a mesh is tested in the placement's pose, in the shape that the renderer draws: a mesh with morph
// targets at the pose's weights (what the animation gives them, or else the node's, or else the mesh's), and a skinned
// mesh where its joints then put it.
//
// Cost: the first pick that reaches a mesh prepares its triangles and builds a bounding volume hierarchy over them
// (pr/mesh_bvh.h), which the model keeps until it is destroyed. A mesh that does not deform is then tested in time
// proportional to the depth of its hierarchy. A mesh that deforms is deformed on the CPU at each pick, once for all of
// a node's instances, and its hierarchy refitted to the shape, in time proportional to its vertices times its targets
// and to its triangles. A shape whose refitted hierarchy costs much more than a build's is built over again, which is
// then the tree that later picks refit.
bool prModelPlacementPick(PrModelPlacement* placement, PrMeshRay ray, PrModelPick* out);

// Returns the camera of the model's camera `index`, in the placement's pose, for a target whose width over its height
// is `aspect`. The camera keeps the asset's field of view, or its orthographic extent, whatever aspect ratio the asset
// states. A perspective camera sees from the asset's near distance on, with no far plane.
PrRenderCamera prModelPlacementCamera(PrModelPlacement* placement, uint32_t index, float aspect);

// Returns the model's light `index` as a light of a scene, in the placement's pose, at the placement's scale of it,
// without shadows. A light of an area is drawn as the punctual light of its intensity along its axis: a sphere or a
// cylinder as a point, and a disk or a rectangle as a spot of a half-space, within its shaping cone where it has one. A
// placement that adds the asset's lights adds these.
PrRenderLightDesc prModelPlacementLight(PrModelPlacement* placement, uint32_t index);

// Set and return the factor that scales the intensity of the model's light `index` in the placement, which is 1 when
// the placement is created, so that a host rebalances an asset's lights against each other, against the asset's
// environment, or against light of the host's own. The next prModelUpdate changes the light in the scene.
void prModelPlacementSetLightScale(PrModelPlacement* placement, uint32_t index, float scale);
float prModelPlacementLightScale(const PrModelPlacement* placement, uint32_t index);

// Writes the environment that the asset brings as a scene's environment, in the placement's pose: the model's
// environment in its renderer, turned by its node's transform in the world, and scaled by the light's intensity, which
// is zero while its node is hidden. Returns false, with no environment, for an asset without one or a model in no
// renderer.
bool prModelPlacementEnvironment(PrModelPlacement* placement, PrRenderSceneEnvironment* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
