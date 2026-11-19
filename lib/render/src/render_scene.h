// What the frame boundary and a renderer's destruction ask of the scenes (pr/render_scene.h).

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Notes that an instance's record changed since it was uploaded.
void renderSceneTouchInstance(RenderSceneState* scene, uint32_t id);

// Applies committed updates to their scenes, in order, then brings each scene's records and draws up to date and
// uploads what changed. Frees the updates, and the scenes whose destruction was waiting. Called when a frame begins,
// after renderResourcesApply.
void renderScenesApply(Render* render, RawArray<RenderUpdate*>* updates);

// Frees a scene, or an update that was not applied.
void renderSceneStateDestroy(Render* render, RenderSceneState* PR_NULLABLE scene);
void renderUpdateDestroy(RenderUpdate* PR_NULLABLE update);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
