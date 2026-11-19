// What the frame boundary and a renderer's destruction ask of the resources (pr/render_resource.h).

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

#include "render/src/render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Take a range of `count` of the renderer's parts, which the caller writes, and give one back, which empties its parts.
// Both note that the parts changed.
uint32_t renderPartsTake(Render* render, uint32_t count);
void renderPartsGive(Render* render, uint32_t first, uint32_t count);

// Notes that `count` parts from `first` changed since they were uploaded.
void renderPartsTouch(Render* render, uint32_t first, uint32_t count);

// Uploads the parts that changed. Called when a frame begins, after everything that writes parts.
void renderPartsUpload(Render* render);

// Returns the buffer of a ready mesh's attributes in the layout of index `layout` of the shader set, and sets
// `*fallback` to whether it holds the layout's fallbacks: a mesh whose data did not give the layout gets a stream of
// them, made at the first call, so a draw of the mesh reads what the layout declares a vertex without the attributes
// reads. Returns no buffer for a layout without attributes, or if the device makes none. Called on the frame thread.
PrGpuBuffer renderMeshStream(Render* render, uint32_t mesh, uint32_t layout, bool* fallback);

// Applies the queued changes to resources, and uploads the materials that changed. Sets `drawsDirty` on every scene if
// a change alters which draws a scene has or how they are drawn. Called when a frame begins.
void renderResourcesApply(Render* render, RawArray<RenderOp>* ops);

// Adds a change to the queue that the next frame applies. Any thread calls it.
void renderOpQueue(Render* render, const RenderOp& op);

// Take an id of a kind of resource, and give one back, under the renderer's lock.
uint32_t renderIdTake(Render* render, RenderIds* ids);
void renderIdGive(Render* render, RenderIds* ids, uint32_t id);

// Frees what a queued change holds, without applying it.
void renderOpDiscard(Render* render, const RenderOp& op);

// Frees every resource's GPU objects and tables.
void renderResourcesReset(Render* render);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
