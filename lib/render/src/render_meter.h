// What a renderer holds of its meters (pr/render_meter.h), which the frame thread and the device's thread share.

#pragma once

#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/gpu_graph.h"
#include "pr/mutex.h"
#include "pr/render_meter.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

struct Render;

// A meter: its latest reading, which the device's thread writes as a frame's work completes, and any thread reads. The
// renderer's table holds one reference, and each reading in flight another, so that a reading that completes after the
// meter's destruction, or the renderer's, has somewhere to land.
struct RenderMeterState {
    Atomic<uint32_t> references;
    PrMutex lock;
    PrRenderMeterReading reading;   // guarded by `lock`
};

// Drops a reference to a meter, and frees it with the last.
void renderMeterRelease(RenderMeterState* meter);

// Adds the passes that read a view's color, of `extent` and exposed by `exposure`, into the meter with id `meter`, if
// it exists. Called on the frame thread.
void renderMeterAdd(Render* render, uint32_t meter, PrGpuGraphTexture color, PrUVec2 extent, float exposure);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
