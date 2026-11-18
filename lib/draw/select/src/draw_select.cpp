// The selector for lib/draw's backends.

#include "pr/draw_select.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/count_of.h"
#include "pr/float.h"

#if defined(PR_DRAW_HAS_CANVAS)
#include "pr/draw_canvas.h"
#endif
#if defined(PR_DRAW_HAS_CPU)
#include "pr/draw_cpu.h"
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
#include "pr/draw_triangles.h"
#endif
#if defined(PR_DRAW_HAS_FRAGMENT)
#include "pr/draw_fragment.h"
#endif
#if defined(PR_DRAW_HAS_PDF)
#include "pr/draw_pdf.h"
#endif
#if defined(PR_DRAW_HAS_SVG)
#include "pr/draw_svg.h"
#endif

namespace pr_impl {

namespace {

constexpr PrStringView kRendererNames[PrDrawRendererBackend_Count] = {
    "default", "cpu", "canvas", "triangles",
    "fragment",
};

// The renderers that the build includes, in order of preference. Default creates the first one that runs on the device.
constexpr PrDrawRendererBackend kRenderersBuilt[] = {
#if defined(PR_DRAW_HAS_FRAGMENT)
    PrDrawRendererBackend_Fragment,
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
    PrDrawRendererBackend_Triangles,
#endif
#if defined(PR_DRAW_HAS_CANVAS)
    PrDrawRendererBackend_Canvas,
#endif
#if defined(PR_DRAW_HAS_CPU)
    PrDrawRendererBackend_Cpu,
#endif
    PrDrawRendererBackend_Default,
};

constexpr uint64_t builtMask(const PrDrawRendererBackend* built, uint32_t count) {
    uint64_t mask = 0;
    for (uint32_t i = 0; i < count; ++i)
        mask |= uint64_t(1) << built[i];
    return mask & ~uint64_t(1);
}

constexpr PrStringView kWriterNames[PrDrawWriterBackend_Count] = {"default", "svg", "pdf"};

// The writers that the build includes, in order of preference. Default creates the first.
constexpr PrDrawWriterBackend kWritersBuilt[] = {
#if defined(PR_DRAW_HAS_PDF)
    PrDrawWriterBackend_Pdf,
#endif
#if defined(PR_DRAW_HAS_SVG)
    PrDrawWriterBackend_Svg,
#endif
    PrDrawWriterBackend_Default,
};

constexpr PrBackendInterface kRendererInterface{
    .name = "renderer",
    .names = kRendererNames,
    .count = PrDrawRendererBackend_Count,
    .built = builtMask(kRenderersBuilt, countOf(kRenderersBuilt)),
};

constexpr PrBackendInterface kWriterInterface{
    .name = "writer",
    .names = kWriterNames,
    .count = PrDrawWriterBackend_Count,
    .built = builtMask(kWritersBuilt, countOf(kWritersBuilt)),
};

// Creates a renderer of `backend`, which is not Default. Only the CPU renderer can create its own device.
PrStatus createRenderer(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device, const PrDrawRendererOptions& o,
        PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    if (backend != PrDrawRendererBackend_Cpu && device == nullptr)
        return PrStatus_NotSupported;
    switch (backend) {
#if defined(PR_DRAW_HAS_CPU)
        case PrDrawRendererBackend_Cpu: {
            PrDrawCpuOptions cpu = prDrawCpuOptionsDefault();
            cpu.glyphDecoder = o.glyphDecoder;
            cpu.device = device;
            *out = prDrawCpuCreate(&cpu);
            return *out != nullptr ? PrStatus_Ok : PrStatus_NotSupported;
        }
#endif
#if defined(PR_DRAW_HAS_CANVAS)
        case PrDrawRendererBackend_Canvas: {
            PrDrawCanvasOptions canvas = prDrawCanvasOptionsDefault();
            canvas.glyphDecoder = o.glyphDecoder;
            return prDrawCanvasCreate(device, &canvas, out);
        }
#endif
#if defined(PR_DRAW_HAS_TRIANGLES)
        case PrDrawRendererBackend_Triangles: {
            PrDrawTrianglesOptions triangles = prDrawTrianglesOptionsDefault();
            triangles.glyphDecoder = o.glyphDecoder;
            return prDrawTrianglesCreate(device, &triangles, out);
        }
#endif
#if defined(PR_DRAW_HAS_FRAGMENT)
        case PrDrawRendererBackend_Fragment: {
            PrDrawFragmentOptions fragment = prDrawFragmentOptionsDefault();
            fragment.glyphDecoder = o.glyphDecoder;
            return prDrawFragmentCreate(device, &fragment, out);
        }
#endif
        default: return PrStatus_NotSupported;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

const PrBackendInterface* prDrawRendererSelectInterface(void) {
    return &kRendererInterface;
}

PrStatus prDrawRendererCreate(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device,
        const PrDrawRendererOptions* PR_NULLABLE options, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDrawRendererOptions o = options != nullptr ? *options : prDrawRendererOptionsDefault();
    if (backend != PrDrawRendererBackend_Default)
        return createRenderer(backend, device, o, out);
    uint32_t chosen = 0;
    PrStatus status = prBackendFromEnvironment(&kRendererInterface, &chosen, nullptr);
    if (status != PrStatus_Ok)
        return status;
    if (chosen != PrDrawRendererBackend_Default)
        return createRenderer(PrDrawRendererBackend(chosen), device, o, out);
    // Without a choice, Default creates the first backend that runs on the device.
    for (PrDrawRendererBackend built : kRenderersBuilt) {
        if (built == PrDrawRendererBackend_Default)
            break;
        status = createRenderer(built, device, o, out);
        if (status != PrStatus_NotSupported)
            return status;
    }
    return PrStatus_NotSupported;
}

const PrBackendInterface* prDrawWriterSelectInterface(void) {
    return &kWriterInterface;
}

PrStatus prDrawWriterCreate(PrDrawWriterBackend backend, const PrDrawWriterOptions* PR_NULLABLE options,
        PrDrawWriter* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrDrawWriterOptions o = options != nullptr ? *options : prDrawWriterOptionsDefault();
    PR_ASSERT(o.rasterScale > 0 && prF32IsFinite(o.rasterScale));
    if (backend == PrDrawWriterBackend_Default) {
        uint32_t chosen = 0;
        PrStatus status = prBackendFromEnvironment(&kWriterInterface, &chosen, nullptr);
        if (status != PrStatus_Ok)
            return status;
        backend = chosen != PrDrawWriterBackend_Default ? PrDrawWriterBackend(chosen) : kWritersBuilt[0];
    }
    switch (backend) {
#if defined(PR_DRAW_HAS_PDF)
        case PrDrawWriterBackend_Pdf:
            *out = prDrawPdfCreate(&o);
            return PrStatus_Ok;
#endif
#if defined(PR_DRAW_HAS_SVG)
        case PrDrawWriterBackend_Svg:
            *out = prDrawSvgCreate(&o);
            return PrStatus_Ok;
#endif
        default: return PrStatus_NotSupported;
    }
}
