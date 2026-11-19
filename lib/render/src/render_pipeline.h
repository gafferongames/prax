// The pipelines that a renderer's passes draw with. The renderer makes each one when something first needs it: a
// material when it is set, on the thread that sets it, for the states that the frames have drawn with, or else a pass
// when it draws.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/render_resource.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The formats of a view's color, its depth and its visibility buffer.
constexpr PrGpuFormat kRenderColorFormat = PrGpuFormat_Rgba16F;
constexpr PrGpuFormat kRenderDepthFormat = PrGpuFormat_D32F;
constexpr PrGpuFormat kRenderVisibilityFormat = PrGpuFormat_Rg32Uint;

// Returns the pipeline for `key`, which the call makes if the renderer has none. Returns zero if the device makes no
// such pipeline. Any thread calls it.
PrGpuPipeline renderPipelineFor(Render* render, const RenderPipelineKey& key);

// Makes the pipelines that draws of a material of `desc` need, in the states that the renderer's pipelines were made
// for so far: the sample counts and the passes that its frames have drawn with. The thread that sets a material calls
// it before it queues the material, so that the frame that first draws the material finds its pipelines made. A state
// that only a draw knows, such as a mirrored instance's, is left to the pass.
void renderPipelinesWarm(Render* render, const PrRenderMaterialDesc& desc);

// Returns the layout of the attributes of a variant of a material type as a pipeline's key holds it
// (RenderPipelineKey::layout).
inline uint32_t renderPipelineLayout(const Render* render, uint32_t variant) {
    return render->shaders->variants[variant].layout + 1;
}

// Frees the pipelines.
void renderPipelinesReset(Render* render);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
