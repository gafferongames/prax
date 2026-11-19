// The scenes of a renderer (pr/render.h): instances of meshes, lights and an environment, and the updates that change
// them.
//
// A scene is flat records in world space, with no hierarchy. A producer, such as a loader or a game, owns the structure
// and writes the records through updates, from any thread.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/gpu_graph.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_records.h"
#include "pr/render_resource.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

// A scene: instances of meshes, lights and an environment. Zero is no scene.
typedef struct PrRenderScene {
    uint32_t id;
} PrRenderScene;

// An instance of a mesh in a scene, or a light of a scene. The id belongs to its scene. Zero is none.
typedef struct PrRenderInstance {
    uint32_t id;
} PrRenderInstance;
typedef struct PrRenderLight {
    uint32_t id;
} PrRenderLight;

// How an instance moves, as a mask, so that an instance keeps its id when its mobility changes.
enum {
    PrRenderMobilityFlag_Static = 1u << 0,      // does not move
    PrRenderMobilityFlag_Dynamic = 1u << 1,     // moves as the application sets its transform
    PrRenderMobilityFlag_Kinematic = 1u << 2,   // moves along a path that the application knows ahead
};
typedef uint32_t PrRenderMobilityFlags;

// An instance: a mesh, a material for each of the mesh's parts, and where the mesh is in the world. A part past
// `materialCount` has no material.
typedef struct PrRenderInstanceDesc {
    PrRenderMesh mesh;
    const PrRenderMaterial* PR_NULLABLE materials;      // by part
    uint32_t materialCount;
    PrAffine3 world;
    PrRenderMobilityFlags mobility;

    // The views that see the instance: a view sees it if the view's mask shares a bit with this one. The instance casts
    // shadows if the mask has kPrRenderVisibilityShadow.
    uint32_t visibility;
} PrRenderInstanceDesc;

// Returns an instance of `mesh` at the identity, static, seen by every view, that casts shadows.
PrRenderInstanceDesc prRenderInstanceDescDefault(PrRenderMesh mesh);

// The kinds of light, as glTF's KHR_lights_punctual defines them.
enum {
    PrRenderLightKind_Directional,
    PrRenderLightKind_Point,
    PrRenderLightKind_Spot,
};
typedef uint8_t PrRenderLightKind;

// A light. A directional light shines along `direction` from infinitely far, with `intensity` in lux. A point light
// shines from `position` in every direction, and a spot light within a cone about `direction`, with `intensity` in
// candela. Both fall off with the square of the distance.
typedef struct PrRenderLightDesc {
    PrRenderLightKind kind;
    PrVec3 color;               // linear
    float intensity;
    PrVec3 position;
    PrVec3 direction;           // the direction in which the light shines, of any length above zero
    float range;                // the distance at which a point or spot light reaches zero, or 0 for no limit
    float innerConeAngle;       // the angle from a spot's axis within which it is at full strength, in radians
    float outerConeAngle;       // the angle from a spot's axis at which it reaches zero, in radians

    // A directional light with `castsShadows` casts the scene's shadows, through one shadow map that every view of a
    // frame shares. The first such light, in the order of the lights' ids, casts them. The map covers a sphere of the
    // world: the one given, or, with a radius that is not above zero, the bounds of the scene's instances that cast
    // shadows.
    bool castsShadows;
    PrSphere shadowBounds;

    // The angle from the middle of a directional light's source to its edge, in radians, as the scene sees it. The edge
    // of a shadow widens with it and with the distance from what casts the shadow, as the penumbra of a disc of that
    // size does. Zero is a source without size, whose shadows are sharp. The sun's is kPrRenderSunAngularRadius.
    float angularRadius;

    // The radius of a point or spot light's source, in the world's units. Zero is a source without size, as glTF's
    // lights are. A shading model reads the angle that it spans at a surface (RenderShadingLight::angularRadius). The
    // light's falloff and the library's own shading model do not depend on it.
    float radius;
} PrRenderLightDesc;

// Returns a white directional light of one lux that shines down, the size of the sun, with a spot's cone of a quarter
// turn, and no shadows.
PrRenderLightDesc prRenderLightDescDefault(void);

// A scene's environment: the environment that lights the scene's surfaces and is the background of its views, turned
// and scaled for the scene.
typedef struct PrRenderSceneEnvironment {
    PrRenderEnvironment environment;    // none for a black environment

    // Turns the environment's directions into the world's. Only its three columns of directions are read, and they must
    // be a rotation.
    PrAffine3 rotation;

    float intensity;                    // the factor that scales the environment's radiance
} PrRenderSceneEnvironment;

// Returns `environment` as it is: not turned, with an intensity of 1.
PrRenderSceneEnvironment prRenderSceneEnvironmentDefault(PrRenderEnvironment environment);

// Creates an empty scene with a black environment.
PrRenderScene prRenderSceneCreate(PrRender* render);

// Destroys the scene, with its instances and lights. The frames that begin after the call have no such scene. No update
// of the scene may be open.
void prRenderSceneDestroy(PrRender* render, PrRenderScene scene);

// Return the number of the scene's instances and lights, as of the frame that began last. Called on the thread that
// builds frames.
uint32_t prRenderSceneInstanceCount(const PrRender* render, PrRenderScene scene);
uint32_t prRenderSceneLightCount(const PrRender* render, PrRenderScene scene);

// Returns the illuminance in lux that an incident-light meter with a flat receptor reads at `point`, in the frame that
// began last, turned toward what lights it most: the greatest illuminance of a plane at the point among the planes that
// face each light that shades surfaces (kPrRenderMaxLights) and each direction along an axis of the world. The lights
// fall off as the shaders compute, and the environment gives what its spherical harmonics give a surface. Nothing is
// shadowed, and each color counts by its luminance. prRenderEv100FromIlluminance turns the reading into an exposure.
// Called on the thread that builds frames.
float prRenderSceneIlluminance(const PrRender* render, PrRenderScene scene, PrVec3 point);

// Returns the buffer that holds the scene's instances (PrRenderInstanceRecord, by instance id) in the graph of the
// frame that began last, for a pass of the application's that writes instances on the GPU. The pass is added before the
// views that draw the scene. What the renderer decides on the CPU, such as the order of blended draws and the extent of
// the shadow map, follows the transforms set through updates, not those that a pass writes. Called on the thread that
// builds frames.
PrGpuGraphBuffer prRenderSceneInstanceBuffer(PrRender* render, PrRenderScene scene);

// The vertices that a scene's deforming instances draw, in the graph of a frame.
typedef struct PrRenderDeformedBuffers {
    // PrRenderGeometryRecord records, as the frame's deformation writes them. A part of a deforming instance names its
    // range of them (PrRenderPartRecord).
    PrGpuGraphBuffer vertices;

    // Where each of those vertices was one frame earlier, as three floats at the vertex's own index
    // (PrRenderInstanceRecord).
    PrGpuGraphBuffer previousPositions;
} PrRenderDeformedBuffers;

// Returns the buffers that hold the vertices of the scene's deforming instances in the graph of the frame that began
// last, for a pass of the application's that reads them, such as one that draws motion. Both are none for a scene in
// which nothing deforms. The call adds the pass that deforms the scene's instances to the graph, as a view of the scene
// does. Called on the thread that builds frames.
PrRenderDeformedBuffers prRenderSceneDeformedBuffers(PrRender* render, PrRenderScene scene);

// What the frame that began last draws for one part of an instance.
typedef struct PrRenderInstancePart {
    // The sphere of the world that the part is culled by. It holds every vertex of the part, in the instance's pose if
    // it deforms.
    PrSphere bounds;

    // The part's range of indices in its mesh, and the first of its vertices in the scene's deformed vertices, which is
    // zero for an instance that does not deform.
    uint32_t firstIndex;
    uint32_t indexCount;
    uint32_t firstVertex;
    bool deforms;
} PrRenderInstancePart;

// Writes what the frame that began last draws for part `part` of an instance. Returns false, and writes zeros, for an
// instance that does not exist, a mesh without data, or a part that the mesh lacks. Called on the thread that builds
// frames.
bool prRenderSceneInstancePart(const PrRender* render, PrRenderScene scene, PrRenderInstance instance, uint32_t part,
    PrRenderInstancePart* out);

// Returns the number of the scene's draws, as of the frame that began last, whose mesh has no attributes in the layout
// of their material's type (PrRenderMeshStream), and which therefore read the fallback of each of the layout's
// attributes. A producer that gave each mesh the layouts of its materials has none. Called on the thread that builds
// frames.
uint32_t prRenderSceneFallbackDrawCount(const PrRender* render, PrRenderScene scene);

// ---------------------------------------------------------------------------------------------------------------------
// Updates

// A set of changes to one scene, which take effect together. A producer begins an update, fills it, and commits it. The
// frames that begin after the commit draw every change of the update, and no frame draws only some of them. Updates
// that are committed before a frame begins take effect in the order of their commits.
//
// One thread at a time fills an update. Several updates of one scene may be open at once, on several threads.
typedef struct PrRenderUpdate PrRenderUpdate;

// Begins an empty update of `scene`.
PrRenderUpdate* prRenderUpdateBegin(PrRender* render, PrRenderScene scene);

// Commits the update's changes, and frees the update.
void prRenderUpdateCommit(PrRenderUpdate* update);

// Frees the update without its changes. The instances and lights that it added never exist.
void prRenderUpdateDiscard(PrRenderUpdate* PR_NULLABLE update);

// Adds an instance. Its id is valid at once, in this update and in others, and the instance exists once the update is
// committed. The update copies the description's materials.
PrRenderInstance prRenderInstanceAdd(PrRenderUpdate* update, const PrRenderInstanceDesc* desc);

// Removes an instance. Its id may name another instance once a frame has begun after the commit.
void prRenderInstanceRemove(PrRenderUpdate* update, PrRenderInstance instance);

// Set where an instance is, or where each of `count` instances is. The transform that an instance had in the frame
// before becomes its previous transform.
void prRenderInstanceSetTransform(PrRenderUpdate* update, PrRenderInstance instance, const PrAffine3* world);
void prRenderInstanceSetTransforms(PrRenderUpdate* update, const PrRenderInstance* instances,
    const PrAffine3* worlds, uint32_t count);

// Sets the pose of an instance of a skinned mesh: `count` matrices, by joint of the mesh's skin. A joint's matrix maps
// a vertex of the mesh as it was bound to where the joint puts it, in the mesh's space, which the instance's transform
// then maps to the world. For a glTF skin, it is the joint's matrix to the mesh's space times its inverse bind matrix.
// A joint past `count` keeps the vertices that it moves where they were bound. The update copies the matrices.
//
// An instance deforms from the first update that sets its joints or its morph weights, while its mesh has a skin or
// morph targets. The renderer then keeps vertices of the instance's own, which a compute pass of the frame writes
// whenever the pose changes, and every other pass draws as it draws a mesh's. An instance that was given no pose draws
// its mesh as it was bound.
void prRenderInstanceSetJoints(PrRenderUpdate* update, PrRenderInstance instance, const PrAffine3* joints, uint32_t count);

// Sets the weights of the morph targets of an instance's mesh: `count` weights, by target. A target past `count` has a
// weight of zero. The update copies the weights.
void prRenderInstanceSetMorphWeights(PrRenderUpdate* update, PrRenderInstance instance, const float* weights, uint32_t count);

// Sets the material of one part of an instance.
void prRenderInstanceSetMaterial(PrRenderUpdate* update, PrRenderInstance instance, uint32_t part, PrRenderMaterial material);

// Sets an instance's mobility and its visibility mask.
void prRenderInstanceSetMasks(PrRenderUpdate* update, PrRenderInstance instance, PrRenderMobilityFlags mobility,
    uint32_t visibility);

// Add a light, whose id is valid at once, replace what a light is, and remove a light.
PrRenderLight prRenderLightAdd(PrRenderUpdate* update, const PrRenderLightDesc* desc);
void prRenderLightSet(PrRenderUpdate* update, PrRenderLight light, const PrRenderLightDesc* desc);
void prRenderLightRemove(PrRenderUpdate* update, PrRenderLight light);

// Sets the scene's environment.
void prRenderSceneEnvironmentSet(PrRenderUpdate* update, const PrRenderSceneEnvironment* environment);

// Sets the scene's time, in seconds, which the material types that change with time read (pr/render_scene.slang's
// renderSceneTime). It is the application's own number and zero until it is set: the renderer has no clock. It takes
// effect with the rest of the update, so a frame draws materials at the time that it draws transforms of. A float holds
// a time to a tenth of a millisecond for under half an hour, so an application whose materials repeat wraps the time at
// their period.
void prRenderSceneTimeSet(PrRenderUpdate* update, float seconds);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
