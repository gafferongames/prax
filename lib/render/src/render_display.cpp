// The display draw: a view's color, scaled, tone mapped and encoded, in a pass of the application's.

#include "pr/render_display.h"

#include "pr/ascii.h"
#include "pr/assert.h"

#include "render/src/render.h"
#include "render/src/render_pipeline.h"

namespace pr_impl {

namespace {

// The display's shader names the tone maps by the values of PrRenderToneMap.
constexpr PrStringView kToneMapNames[PrRenderToneMap_Count] = {"neutral", "aces-channels", "aces-luminance"};
static_assert(kPrRenderToneMapNeutral == PrRenderToneMap_Neutral);
static_assert(kPrRenderToneMapAcesChannels == PrRenderToneMap_AcesChannels);
static_assert(kPrRenderToneMapAcesLuminance == PrRenderToneMap_AcesLuminance);

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrRenderToneMap prRenderToneMapFromName(PrStringView name) {
    for (PrRenderToneMap toneMap = 0; toneMap < PrRenderToneMap_Count; ++toneMap) {
        if (prAsciiEqualsFold(kToneMapNames[toneMap], name))
            return toneMap;
    }
    return PrRenderToneMap_Count;
}

PrStringView prRenderToneMapName(PrRenderToneMap toneMap) {
    return toneMap < PrRenderToneMap_Count ? kToneMapNames[toneMap] : PrStringView{};
}

PrRenderDisplayOptions prRenderDisplayOptionsDefault(void) {
    return PrRenderDisplayOptions{.exposure = 1, .headroom = 1, .toneMap = PrRenderToneMap_Neutral};
}

void prRenderDisplayDraw(PrRender* render, PrGpuPass* pass, PrGpuTexture color,
        const PrRenderDisplayOptions* PR_NULLABLE options) {
    Render* r = render;
    PrRenderDisplayOptions display = options != nullptr ? *options : prRenderDisplayOptionsDefault();
    PR_ASSERT(display.toneMap < PrRenderToneMap_Count);
    RenderPipelineKey key{.kind = RenderPipelineKind_Display, .samples = 1, .format = prGpuPassFormats(pass).color[0]};
    PrGpuPipeline pipeline = renderPipelineFor(r, key);
    if (pipeline.id == 0 || color.id == 0)
        return;
    PrGpuCommands* commands = prGpuPassCommands(pass);
    PrRenderDisplayPush push{
        .exposure = display.exposure,
        .headroom = display.headroom,
        .color = color.id,
        .toneMap = display.toneMap,
    };
    prGpuCommandsSetPipeline(commands, pipeline);
    prGpuCommandsSetPush(commands, &push, sizeof(push));
    if (!r->bindless)
        prGpuCommandsSetTexture(commands, r->bindings[RenderLibrary_Display].color, color, PrGpuSampler{});
    prGpuPassDraw(pass, 3, 1, 0, 0);
}
