// The Dear ImGui draw backend on a lib/gpu device, which draws a prepared frame with the renderer of imgui_gpu.h.

#include "draw/comparisons/imgui/src/draw_imgui_gpu.h"

#include "pr/alloc.h"
#include "pr/draw_renderer_backend.h"
#include "pr/mem.h"
#include "pr/pixels.h"
#include "pr/raw_array.h"
#include "pr/texture.h"

#include "draw/comparisons/imgui/config/imgui_gpu.h"

#include "imgui.h"

namespace pr_impl {

namespace {

// An image uploaded for the renderer to sample. It has straight alpha, which is what the renderer blends.
struct CachedTexture {
    uint64_t id;
    PrGpuTexture texture;
    PrUVec2 extent;
    uint64_t lastUsed;
};

struct ImGuiGpu : ImGuiBackend {
    PrGpuDevice* device;
    RawArray<CachedTexture> textures;
};

PrGpuDevice* rendererDevice(const PrDrawRenderer* renderer) {
    return static_cast<const ImGuiGpu*>(renderer)->device;
}

void destroyFrame(PrDrawFrame* handle) {
    ImGui::SetCurrentContext(static_cast<ImGuiFrame*>(handle)->renderer->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    imguiFrameClear(frame);
    prMemoryFree(frame);
}

// Drops the images that the render did not use. The device keeps a destroyed texture until the work that reads it is
// done.
void dropUnusedTextures(ImGuiGpu* g) {
    uint32_t kept = 0;
    for (uint32_t i = 0; i < g->textures.count; ++i) {
        if (g->textures.data[i].lastUsed == g->render)
            g->textures.data[kept++] = g->textures.data[i];
        else
            prGpuTextureRelease(g->device, g->textures.data[i].texture);
    }
    g->textures.count = kept;
}

PrStatus prepare(PrDrawFrame* handle, const PrDrawList* list, const PrDrawPart* part, const PrDrawTarget* target) {
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    ImGuiGpu* g = static_cast<ImGuiGpu*>(frame->renderer);
    ImGui::SetCurrentContext(g->context);
    imguiFrameClear(frame);
    ++g->render;
    imguiRecord(g, frame, list, *part, target);
    dropUnusedTextures(g);
    return PrStatus_Ok;
}

// Draws the lists with the render's pixel (0, 0) at `origin`. Their projection starts at the negated origin over the
// whole attachments. Every draw has a clip at the render's bounds, which keeps it within the render.
void render(PrDrawFrame* handle, PrGpuPass* pass, PrIVec2 origin) {
    ImGui::SetCurrentContext(static_cast<ImGuiFrame*>(handle)->renderer->context);
    ImGuiFrame* frame = static_cast<ImGuiFrame*>(handle);
    PrUVec2 extent = prGpuPassExtent(pass);
    frame->data.DisplayPos = ImVec2(-float(origin.x), -float(origin.y));
    frame->data.DisplaySize = ImVec2(float(extent.x), float(extent.y));
    frame->data.FramebufferScale = ImVec2(1, 1);
    imguiGpuRender(&frame->data, pass);
}

PrDrawFrame* createFrame(PrDrawRenderer* renderer) {
    ImGuiFrame* frame = allocZeroed<ImGuiFrame>();
    frame->destroyFn = destroyFrame;
    frame->prepareFn = prepare;
    frame->renderFn = render;
    frame->renderer = static_cast<ImGuiGpu*>(renderer);
    return frame;
}

void dropResources(PrDrawRenderer* renderer) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    for (uint32_t i = 0; i < g->textures.count; ++i)
        prGpuTextureRelease(g->device, g->textures.data[i].texture);
    g->textures.clear();
}

void destroyBackend(PrDrawRenderer* renderer) {
    ImGuiGpu* g = static_cast<ImGuiGpu*>(renderer);
    ImGui::SetCurrentContext(g->context);
    dropResources(renderer);
    g->textures.reset();
    imguiGpuShutdown();
    imguiContextDestroy(g);
    prMemoryFree(g);
}

} // namespace

ImTextureID imguiHostTexture(ImGuiBackend*, PrGpuTexture texture) {
    return ImTextureID(texture.id);
}

ImGuiBackend* PR_NULLABLE imguiDeviceCreate(PrGpuDevice* device) {
    if (!imguiGpuInit(device))
        return nullptr;
    ImGuiGpu* g = allocZeroed<ImGuiGpu>();
    g->device = device;
    g->destroyFn = destroyBackend;
    g->frameCreateFn = createFrame;
    g->name = "imgui";
    g->deviceFn = rendererDevice;
    g->dropResourcesFn = dropResources;
    return g;
}

} // namespace pr_impl
