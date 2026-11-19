// What --stats prints in the examples that draw with lib/render: the frames' average times on the CPU and on the GPU,
// every couple of seconds, and the pipelines that the renderer has made.
//
// An example holds a FrameStats in the structure of its window, which is static, so the counters start at zero. It
// calls frameStatsSubmit with each frame's command buffer before it submits it, and frameStatsReport after each frame.

#pragma once

#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/print.h"
#include "pr/render.h"
#include "pr/string_view.h"
#include "pr/time.h"

PR_ASSUME_NONNULL_BEGIN

namespace frame_stats {

using namespace pr;

// The frames since the last report. The device's thread adds each frame's time on the GPU once its work is done.
struct FrameStats {
    PrTimePerf reported;
    bool began;
    uint32_t frames;
    double cpuMilliseconds;
    Atomic<uint64_t> gpuNanoseconds;
    Atomic<uint32_t> gpuFrames;
};

// The seconds between reports.
constexpr double kFrameStatsSeconds = 2;

// Adds a frame to the report: the milliseconds that the thread spent on it, and its time on the GPU once the work of
// `commands` is done. `stats` must outlive that work.
inline void frameStatsSubmit(FrameStats& stats, PrGpuCommands* commands, double cpuMilliseconds) {
    stats.cpuMilliseconds += cpuMilliseconds;
    ++stats.frames;
    prGpuCommandsOnDone(commands, [](void* PR_NULLABLE context, double seconds) {
        FrameStats& held = *static_cast<FrameStats*>(context);
        held.gpuNanoseconds.fetchAdd(uint64_t(seconds * 1e9));
        held.gpuFrames.fetchAdd(1);
    }, &stats);
}

// Prints the frames' average times since the last report, once kFrameStatsSeconds have passed, with the number of
// pipelines that `render` has made and the time that making them took. A device without timestamps reports no time on
// the GPU. `name` is the example's.
inline void frameStatsReport(FrameStats& stats, PrStringView name, const PrRender* render) {
    PrTimePerf now = prTimePerfNow();
    if (!stats.began) {
        stats.began = true;
        stats.reported = now;
    }
    if (prTimePerfSeconds(stats.reported, now) < kFrameStatsSeconds || stats.frames == 0)
        return;
    uint64_t gpuNanoseconds = stats.gpuNanoseconds.exchange(0);
    uint32_t gpuFrames = stats.gpuFrames.exchange(0);
    PrGpuDeviceInfo info = prGpuDeviceInfo(prRenderDevice(render));
    PrRenderPipelineInfo pipelines = prRenderPipelineInfo(render);
    double cpu = stats.cpuMilliseconds / stats.frames;
    double gpu = gpuFrames != 0 ? double(gpuNanoseconds) / 1e6 / gpuFrames : 0;
    if ((info.features & PrGpuFeatureFlag_Timestamps) != 0 && gpuFrames != 0) {
        print("{}: cpu {:.2f} ms, gpu {:.2f} ms a frame over {} frames, {} pipelines made in {:.1f} ms\n", name,
            cpu, gpu, stats.frames, pipelines.count, pipelines.seconds * 1e3);
    } else {
        print("{}: cpu {:.2f} ms a frame over {} frames, {} pipelines made in {:.1f} ms\n", name, cpu,
            stats.frames, pipelines.count, pipelines.seconds * 1e3);
    }
    stats.reported = now;
    stats.frames = 0;
    stats.cpuMilliseconds = 0;
}

} // namespace frame_stats

PR_ASSUME_NONNULL_END
