// What a scene asks of the frame that is being built (pr/render_view.h).

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns what the frame that is being built holds for a scene. The first call of a frame for a scene adds the scene's
// resources to the frame's graph, and the passes that every view of the scene shares.
RenderSceneFrame* renderSceneFrame(Render* render, RenderSceneState* scene);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
