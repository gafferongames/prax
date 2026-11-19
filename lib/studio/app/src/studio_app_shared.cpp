// What Studio's applications and windows share: the device that the windows render on, and the windows' closing.

#include "studio/app/src/studio_app_shared.h"

#include "pr/alloc.h"
#include "pr/draw_select.h"
#include "pr/gpu_select.h"
#include "pr/svg_glyph.h"
#include "pr/texture_cache.h"

namespace pr_impl {

bool studioAppMakeDevice(StudioAppState* app) {
    if (app->device != nullptr && prGpuDeviceIsLost(app->device))
        studioAppCloseDevice(app);
    if (app->device == nullptr) {
        PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
        options.pool = app->desc.pool;
        app->device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
    }
    if (app->device == nullptr || !prGpuDeviceIsReady(app->device) || prGpuDeviceIsLost(app->device))
        return false;
    if (app->textures == nullptr) {
        PrTextureCacheDesc desc = prTextureCacheDescDefault(app->device);
        desc.tasks = app->desc.pool;
        desc.values = PrTextureCacheValues_Encoded;
        // An image that arrives after a frame drew without it wakes the application, which asks every window for a
        // frame.
        desc.arrived = [](void* PR_NULLABLE context) { prAppWake(static_cast<StudioAppState*>(context)->app); };
        desc.arrivedContext = app;
        app->textures = prTextureCacheCreate(&desc);
        app->glyphs = app->images != nullptr ? prSvgGlyphDecoderCreate(app->images, app->compress, app->textures) : nullptr;
    } else if (prTextureCacheDevice(app->textures) != app->device) {
        prTextureCacheSetDevice(app->textures, app->device);
    }
    if (app->renderer == nullptr) {
        PrDrawRendererOptions options = prDrawRendererOptionsDefault();
        options.glyphDecoder = app->glyphs;
        if (prDrawRendererCreate(PrDrawRendererBackend_Default, app->device, &options, &app->renderer) != PrStatus_Ok)
            return false;
        ++app->generation;
    }
    return true;
}

bool studioAppCreatingDevice(const StudioAppState* app) {
    return app->device != nullptr && !prGpuDeviceIsReady(app->device);
}

PrDrawDisplay* PR_NULLABLE studioAppDisplay(StudioAppState* app) {
    if (app->display == nullptr && app->device != nullptr)
        prDrawDisplayCreate(app->device, &app->display);
    return app->display;
}

void studioAppCloseDevice(StudioAppState* app) {
    for (uint32_t i = 0; i < app->windows.count; ++i)
        studioWindowCloseDevice(app->windows.data[i]);
    prDrawDisplayDestroy(app->display);
    prDrawRendererDestroy(app->renderer);
    // The cache frees its textures on the device before the device goes, and moves to the next device.
    if (app->textures != nullptr)
        prTextureCacheSetDevice(app->textures, app->device);
    prGpuDeviceDestroy(app->device);
    app->display = nullptr;
    app->renderer = nullptr;
    app->device = nullptr;
}

void studioAppWindowClosed(StudioAppState* app, StudioWindowState* window) {
    for (uint32_t i = 0; i < app->windows.count; ++i) {
        if (app->windows.data[i] == window) {
            app->windows.erase(i);
            break;
        }
    }
    if (window->initial && window->status != PrStatus_Ok)
        app->status = window->status;
    if (app->windows.count == 0 && !app->stopping && app->desc.lastWindow == PrStudioAppLastWindow_Stop)
        prAppStop(app->app);
}

void studioWindowTargetClose(StudioAppState* app, StudioWindowTarget* target) {
    if (app->device != nullptr)
        prGpuTextureRelease(app->device, target->rendered);
    prDrawFrameDestroy(target->frame);
    prGpuGraphDestroy(target->graph);
    prGpuSwapChainDestroy(target->swapChain);
    target->rendered = PrGpuTexture{};
    target->frame = nullptr;
    target->graph = nullptr;
    target->swapChain = nullptr;
}

void studioWindowCloseDevice(StudioWindowState* window) {
    // A GPU view frees what it made on the device, and renders again on the next device.
    for (PrStudioWindowGpuView* view : window->gpuViews.span()) {
        if (view->lost != nullptr && window->studio->device != nullptr)
            view->lost(view->context, window->studio->device);
        prGpuTextureRelease(window->studio->device, view->shown.texture);
        view->shown = PrTexture{};
        view->invalid = true;
    }
    studioWindowTargetClose(window->studio, &window->target);
    // A portal's window draws again on the next device, whether or not its root's drawing changed.
    for (uint32_t i = 0; i < window->portals.count; ++i) {
        studioWindowTargetClose(window->studio, &window->portals.data[i]->target);
        window->portals.data[i]->drawn = 0;
    }
}

} // namespace pr_impl
