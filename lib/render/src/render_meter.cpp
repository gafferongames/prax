// A renderer's meters (pr/render_meter.h): the passes that read what a view shows, and the readings that the device's
// thread makes of them once a frame's work completes.

#include "pr/render_meter.h"
#include "render/src/render_meter.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/render_exposure.h"
#include "pr/render_view.h"
#include "pr/sort.h"
#include "pr/span.h"

#include "render/src/render.h"
#include "render/src/render_pipeline.h"
#include "render/src/render_resource.h"

namespace pr_impl {

namespace {

// The shares of the cells' weight below which a reading starts and above which it ends.
constexpr float kLowShare = 0.1f;
constexpr float kHighShare = 0.9f;

// What a meter's passes read while the graph records.
struct MeterFrame {
    Render* render;
    RenderMeterState* meter;
    PrGpuGraph* graph;
    PrGpuGraphTexture color;
    PrGpuGraphTexture grid;
    PrRenderMeterPush push;
};

// A reading in flight, which the device's thread completes.
struct MeterRead {
    RenderMeterState* meter;
    float exposure;
};

// One cell of a meter's grid: the mean of the logarithm of its luminance, and the share of its pixels that have light.
struct MeterCell {
    float logLuminance;
    float weight;
};

void drawMeter(void* PR_NULLABLE context, PrGpuPass* pass) {
    MeterFrame& m = *static_cast<MeterFrame*>(context);
    RenderPipelineKey key{.kind = RenderPipelineKind_Meter, .samples = 1};
    PrGpuPipeline pipeline = renderPipelineFor(m.render, key);
    PrGpuTexture color = prGpuGraphTexture(m.graph, m.color);
    if (pipeline.id == 0 || color.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    m.push.color = color.id;
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &m.push, sizeof(m.push));
    if (!m.render->bindless)
        prGpuCommandsSetTexture(commands, m.render->bindings[RenderLibrary_Meter].color, color, PrGpuSampler{});
    prGpuPassDraw(pass, 3, 1, 0, 0);
}

// Takes a reading of the grid's texels, of four half floats each, into the meter. Called on the device's thread.
void finishRead(void* PR_NULLABLE context, PrPixelView texels) {
    MeterRead* read = static_cast<MeterRead*>(context);
    RenderMeterState* meter = read->meter;
    uint32_t count = texels.extent.x * texels.extent.y;
    if (count != 0) {
        RawArray<MeterCell> held;
        held.resize(count);
        MeterCell* cells = held.data;
        float total = 0;
        for (uint32_t y = 0; y < texels.extent.y; ++y) {
            const uint8_t* row = texels.data + size_t(y) * texels.stride;
            for (uint32_t x = 0; x < texels.extent.x; ++x) {
                PrFloat16 halves[2];
                prMemCopy(halves, row + size_t(x) * 8, sizeof(halves));
                MeterCell& cell = cells[y * texels.extent.x + x];
                cell = MeterCell{prFloat16ToFloat(halves[0]), prFloat16ToFloat(halves[1])};
                total += cell.weight;
            }
        }
        sortItems(Span<MeterCell>(cells, count), [](const MeterCell& a, const MeterCell& b) {
            return a.logLuminance < b.logLuminance;
        });
        // The part of each cell's weight that lies between the low and the high shares of the total counts.
        double sum = 0;
        double weights = 0;
        float low = total * kLowShare;
        float high = total * kHighShare;
        float before = 0;
        for (uint32_t i = 0; i < count; ++i) {
            float after = before + cells[i].weight;
            float inside = prF32Min(after, high) - prF32Max(before, low);
            if (inside > 0) {
                sum += double(cells[i].logLuminance) * double(inside);
                weights += double(inside);
            }
            before = after;
        }
        held.reset();
        PrRenderMeterReading reading{.exposure = read->exposure, .coverage = total / float(count)};
        if (weights > 0) {
            reading.luminance = prF32Exp2(float(sum / weights));
            reading.ev100 = prRenderEv100FromLuminance(reading.luminance);
        }
        prMutexLock(&meter->lock);
        reading.serial = meter->reading.serial + 1;
        meter->reading = reading;
        prMutexUnlock(&meter->lock);
    }
    prMemoryFree(read);
    renderMeterRelease(meter);
}

void recordRead(void* PR_NULLABLE context, PrGpuCommands* commands) {
    const MeterFrame& m = *static_cast<const MeterFrame*>(context);
    PrGpuTexture grid = prGpuGraphTexture(m.graph, m.grid);
    if (grid.id == 0)
        return;
    m.meter->references.fetchAdd(1);
    MeterRead* read = alloc<MeterRead>();
    *read = MeterRead{m.meter, m.push.exposure};
    prGpuCommandsReadTexture(commands, grid, PrGpuAccessFlag_ShaderRead, finishRead, read);
}

} // namespace

void renderMeterRelease(RenderMeterState* meter) {
    if (meter->references.fetchSub(1) != 1)
        return;
    prMutexReset(&meter->lock);
    prMemoryFree(meter);
}

void renderMeterAdd(Render* render, uint32_t meter, PrGpuGraphTexture color, PrUVec2 extent, float exposure) {
    Render* r = render;
    // A meter's destruction takes effect at the next frame's boundary, after this frame's graph has recorded, so the
    // table's reference keeps the meter through the recording.
    prMutexLock(&r->lock);
    RenderMeterState* state = meter < r->meters.count ? r->meters.data[meter] : nullptr;
    prMutexUnlock(&r->lock);
    if (state == nullptr || color.id == 0)
        return;
    PrGpuGraph* graph = r->graph;
    PrGpuTextureDesc gridDesc = prGpuTextureDesc2d(kRenderColorFormat, PrUVec2{kPrRenderMeterGrid, kPrRenderMeterGrid},
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    MeterFrame* m = static_cast<MeterFrame*>(prGpuGraphAlloc(graph, sizeof(MeterFrame)));
    *m = MeterFrame{
        .render = r,
        .meter = state,
        .graph = graph,
        .color = color,
        .grid = prGpuGraphAddTexture(graph, &gridDesc),
        .push = {.exposure = exposure, .width = extent.x, .height = extent.y},
    };
    PrGpuGraphPassDesc desc{
        .name = "render meter",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = m->grid, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphPass pass = prGpuGraphAddPass(graph, &desc, drawMeter, m);
    prGpuGraphUseTexture(graph, pass, color, PrGpuAccessFlag_FragmentRead);
    PrGpuGraphPass read = prGpuGraphAddWork(graph, "render meter read", recordRead, m);
    prGpuGraphUseTexture(graph, read, m->grid, PrGpuAccessFlag_ShaderRead);
}

} // namespace pr_impl

using namespace pr_impl;

PrRenderMeter prRenderMeterCreate(PrRender* render) {
    Render* r = render;
    RenderMeterState* meter = allocZeroed<RenderMeterState>();
    meter->references.store(1);
    prMutexInit(&meter->lock);
    prMutexLock(&r->lock);
    uint32_t id = renderIdsTake(&r->meterIds);
    renderTableGrow(&r->meters, id);
    r->meters.data[id] = meter;
    prMutexUnlock(&r->lock);
    return PrRenderMeter{id};
}

void prRenderMeterDestroy(PrRender* render, PrRenderMeter meter) {
    if (meter.id != 0)
        renderOpQueue(render, RenderOp{.kind = RenderOpKind_MeterDestroy, .id = meter.id});
}

PrRenderMeterReading prRenderMeterRead(const PrRender* render, PrRenderMeter meter) {
    Render* r = const_cast<Render*>(static_cast<const Render*>(render));
    prMutexLock(&r->lock);
    RenderMeterState* state = meter.id < r->meters.count ? r->meters.data[meter.id] : nullptr;
    PrRenderMeterReading reading{};
    if (state != nullptr) {
        prMutexLock(&state->lock);
        reading = state->reading;
        prMutexUnlock(&state->lock);
    }
    prMutexUnlock(&r->lock);
    return reading;
}
