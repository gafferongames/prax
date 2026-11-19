// A scene's deforming instances: the poses that updates give them, the vertices that they draw, and the pass that
// writes those vertices.

#pragma once

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Set the joints or the morph weights of an instance, which deforms from then on. Called as an update is applied.
void renderDeformSetJoints(RenderSceneState* scene, uint32_t instance, const PrAffine3* joints, uint32_t count);
void renderDeformSetWeights(RenderSceneState* scene, uint32_t instance, const float* weights, uint32_t count);

// Ends the deformation of an instance that is being removed.
void renderDeformRemove(Render* render, RenderSceneState* scene, uint32_t instance);

// Gives each deforming instance vertices and parts of its own for the mesh that it has now, and takes them from an
// instance whose mesh has no data that deforms. Called before a scene's draws are built again, which is whenever an
// instance or a mesh changes.
void renderDeformBind(Render* render, RenderSceneState* scene);

// Writes the poses that changed since they were deformed to the scene's poses, and bounds each such instance's parts in
// its pose. Called when a frame begins, after renderDeformBind.
void renderDeformUpload(Render* render, RenderSceneState* scene);

// Adds the scene's deformed vertices to the frame's graph, and the work that deforms the instances whose vertices are
// behind their pose. Called once for each frame that draws the scene.
void renderDeformAddPass(Render* render, RenderSceneState* scene, RenderSceneFrame* frame);

// Frees a scene's deformations and their buffers.
void renderDeformReset(Render* render, RenderSceneState* scene);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
