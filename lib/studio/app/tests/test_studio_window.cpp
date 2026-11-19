// Tests of Studio's windows that run a headless application on the system's GPU device.

#include "pr/atomic.h"
#include "pr/font_set.h"
#include "pr/gpu_graph.h"
#include "pr/kit_image.h"
#include "pr/mem.h"
#include "pr/studio_app.h"
#include "pr/studio_window.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace pr;

namespace {

// A window whose left part is a GPU view that renders red into a texture of its own, at the view's size, and the
// captures of two of its frames: one after the view first renders, and one in the frame that widens the view.
struct Run {
    PrStudioApp* app = nullptr;
    PrStudioWindowGpuView* view = nullptr;
    PrGpuDevice* PR_NULLABLE device = nullptr;
    PrGpuTexture texture{};
    PrUVec2 extent{};
    uint32_t frames = 0;
    uint32_t renders = 0;
    float width = 30;               // of the view, in percent of the window's width

    // What each capture saw: the extent of the frame, the extent that the view rendered at in that frame, and whether
    // each tenth of the frame's width is red at its middle row. Written on the device's thread before `captured` rises.
    struct Capture {
        PrUVec2 frame;
        PrUVec2 rendered;
        bool red[10];
    } captures[2] = {};
    Atomic<uint32_t> captured;
};

// Renders red into the view's texture, which it makes again at the extent that the window gives.
PrStudioWindowGpuOutput renderRed(void* PR_NULLABLE context, const PrStudioWindowRender* render) {
    Run& run = *static_cast<Run*>(context);
    if (render->extent.x != run.extent.x || render->extent.y != run.extent.y) {
        prGpuTextureRelease(render->device, run.texture);
        PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, render->extent,
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        REQUIRE(prGpuTextureCreate(render->device, &desc, &run.texture) == PrStatus_Ok);
        run.extent = render->extent;
        run.device = render->device;
    }
    ++run.renders;
    PrGpuGraphTexture texture = prGpuGraphImportTexture(render->graph, run.texture, PrGpuAccessFlag_ShaderRead);
    PrGpuGraphPassDesc pass{
        .name = "red",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = texture, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
        .clearColor = {PrColor{1, 0, 0, 1}},
    };
    prGpuGraphAddPass(render->graph, &pass, [](void* PR_NULLABLE, PrGpuPass*) {}, nullptr);
    return PrStudioWindowGpuOutput{.texture = texture, .alpha = PrTextureAlpha_None};
}

void captured(void* PR_NULLABLE context, PrPixelView rgba) {
    Run& run = *static_cast<Run*>(context);
    uint32_t index = run.captured.load();
    Run::Capture& capture = run.captures[index];
    capture.frame = rgba.extent;
    capture.rendered = run.extent;
    for (uint32_t i = 0; rgba.data != nullptr && i < 10; ++i) {
        const uint8_t* texel = rgba.data + (rgba.extent.y / 2) * rgba.stride + ((2 * i + 1) * rgba.extent.x / 20) * 4;
        capture.red[i] = texel[0] == 255 && texel[1] == 0 && texel[2] == 0;
    }
    run.captured.store(index + 1);
    prAppWake(prStudioAppApp(run.app));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a window renders a GPU view at its laid-out size in the frame that lays it out, and only when it changes") {
    static Run run;
    run.captured.store(0);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrStudioWindowDesc window{
        .title = "GPU view",
        .extent = {80, 40},
        .open = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Run& r = *static_cast<Run*>(context);
            PrStudioWindowGpuViewDesc desc{.render = renderRed, .context = &r};
            r.view = prStudioWindowGpuViewCreate(w, &desc);
            return true;
        },
        .build = [](void* PR_NULLABLE context, PrStudioWindow* w, PrLayoutNode root, PrVec2) {
            Run& r = *static_cast<Run*>(context);
            PrKitPlace place{.width = prKitPercent(r.width), .height = prKitPercent(100), .flex = PrKitFlex_Fixed};
            prKitEmbed(prStudioWindowKit(w), root, {.embed = prStudioWindowGpuViewEmbed(r.view)}, place);
        },
        .laidOut = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Run& r = *static_cast<Run*>(context);
            ++r.frames;
            // Frames that change nothing render nothing. The view widens in the round of frame 8, which the second
            // capture takes: a capture asked for once a frame is laid out takes that frame.
            if (r.frames == 4 || r.frames == 8)
                prStudioWindowCapture(w, captured, &r);
            if (r.frames == 7)
                r.width = 70;
            return r.frames < 10;
        },
        .close = [](void* PR_NULLABLE context, PrStudioWindow*) {
            Run& r = *static_cast<Run*>(context);
            prStudioWindowGpuViewDestroy(r.view);
            if (r.device != nullptr)
                prGpuTextureRelease(r.device, r.texture);
            r.texture = PrGpuTexture{};
        },
        .context = &run,
    };
    PrAppOptions options{.headless = true};
    PrStudioAppDesc desc{
        .fonts = fonts,
        .options = &options,
        .window = &window,
        .start = [](void* PR_NULLABLE context, PrStudioApp* app) {
            static_cast<Run*>(context)->app = app;
            return true;
        },
        .event = [](void* PR_NULLABLE context, PrStudioApp* app, const PrAppEvent*) {
            if (static_cast<Run*>(context)->captured.load() == 2)
                prStudioAppStop(app);
        },
        .context = &run,
    };
    PrStatus status = prStudioAppRun(&desc);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        return;
    REQUIRE(status == PrStatus_Ok);
    REQUIRE(run.captured.load() == 2);
    // The view rendered once at its first size, and once at its second, however many frames drew it.
    CHECK(run.renders == 2);
    for (uint32_t c = 0; c < 2; ++c) {
        CAPTURE(c);
        const Run::Capture& capture = run.captures[c];
        REQUIRE(capture.frame.x != 0);
        // The view rendered at its size in target pixels in the frame that the capture took.
        uint32_t tenths = c == 0 ? 3 : 7;
        CHECK(capture.rendered.x * 100 == capture.frame.x * (c == 0 ? 30 : 70));
        CHECK(capture.rendered.y == capture.frame.y);
        for (uint32_t i = 0; i < 10; ++i) {
            CAPTURE(i);
            CHECK(capture.red[i] == (i < tenths));
        }
    }
}

TEST_CASE("a window's GPU view frees what it made on a lost device, and renders again on the next") {
    // A view that renders red, and counts its renders and the devices that it lost.
    struct Lost {
        PrStudioWindowGpuView* view = nullptr;
        PrGpuDevice* PR_NULLABLE device = nullptr;
        PrGpuTexture texture{};
        uint32_t frames = 0;
        uint32_t renders = 0;
        uint32_t lost = 0;
        uint32_t rendersAfterLoss = 0;
    };
    static Lost run;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrStudioWindowDesc window{
        .title = "GPU view",
        .extent = {40, 20},
        .open = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Lost& r = *static_cast<Lost*>(context);
            PrStudioWindowGpuViewDesc desc{
                .render = [](void* PR_NULLABLE held, const PrStudioWindowRender* render) -> PrStudioWindowGpuOutput {
                    Lost& l = *static_cast<Lost*>(held);
                    if (l.device != render->device) {
                        PrGpuTextureDesc texture = prGpuTextureDesc2d(PrGpuFormat_Rgba8, render->extent,
                            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
                        REQUIRE(prGpuTextureCreate(render->device, &texture, &l.texture) == PrStatus_Ok);
                        l.device = render->device;
                    }
                    ++l.renders;
                    l.rendersAfterLoss += l.lost != 0 ? 1 : 0;
                    PrGpuGraphTexture shown =
                        prGpuGraphImportTexture(render->graph, l.texture, PrGpuAccessFlag_ShaderRead);
                    PrGpuGraphPassDesc pass{
                        .name = "red",
                        .viewCount = 1,
                        .colorCount = 1,
                        .color = {{.texture = shown, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
                    };
                    prGpuGraphAddPass(render->graph, &pass, [](void* PR_NULLABLE, PrGpuPass*) {}, nullptr);
                    return PrStudioWindowGpuOutput{.texture = shown, .alpha = PrTextureAlpha_None};
                },
                .lost = [](void* PR_NULLABLE held, PrGpuDevice* device) {
                    Lost& l = *static_cast<Lost*>(held);
                    CHECK(device == l.device);
                    prGpuTextureRelease(device, l.texture);
                    l.texture = PrGpuTexture{};
                    l.device = nullptr;
                    ++l.lost;
                },
                .context = &r,
            };
            r.view = prStudioWindowGpuViewCreate(w, &desc);
            return true;
        },
        .build = [](void* PR_NULLABLE context, PrStudioWindow* w, PrLayoutNode root, PrVec2) {
            Lost& r = *static_cast<Lost*>(context);
            PrKitPlace place{.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed};
            prKitEmbed(prStudioWindowKit(w), root, {.embed = prStudioWindowGpuViewEmbed(r.view)}, place);
        },
        .laidOut = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Lost& r = *static_cast<Lost*>(context);
            ++r.frames;
            if (r.frames == 3)
                prGpuDeviceSimulateLoss(prStudioAppDevice(prStudioWindowStudioApp(w)));
            if (r.rendersAfterLoss != 0 || r.frames == 200)
                prStudioAppStop(prStudioWindowStudioApp(w));
            return true;
        },
        .close = [](void* PR_NULLABLE context, PrStudioWindow*) {
            Lost& r = *static_cast<Lost*>(context);
            prStudioWindowGpuViewDestroy(r.view);
            if (r.device != nullptr)
                prGpuTextureRelease(r.device, r.texture);
        },
        .context = &run,
    };
    PrAppOptions options{.headless = true};
    PrStudioAppDesc desc{.fonts = fonts, .options = &options, .window = &window, .context = &run};
    PrStatus status = prStudioAppRun(&desc);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        return;
    REQUIRE(status == PrStatus_Ok);
    CHECK(run.lost == 1);
    CHECK(run.renders >= 2);
    CHECK(run.rendersAfterLoss >= 1);
}
