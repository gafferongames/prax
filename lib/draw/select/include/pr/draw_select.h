// The selector for lib/draw's backends. It reports which renderers and writers the build includes, and creates one of a
// given backend. Programs and composition libraries link it.

#pragma once

#include "pr/backend.h"
#include "pr/base.h"
#include "pr/draw_renderer.h"
#include "pr/draw_writer.h"
#include "pr/gpu.h"
#include "pr/status.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Renderers

// The renderer backends. The fragment and triangles renderers run on a device that runs their shaders. The Canvas 2D
// renderer runs on a device whose textures are a web page's canvases, and the CPU renderer runs on a memory device.
// Default is the backend that PR_BACKEND_RENDERER names (pr/backend.h). If the variable chooses nothing, Default is the
// first of these that the build includes and the device runs, in the order fragment, triangles, Canvas 2D and CPU.
enum {
    PrDrawRendererBackend_Default,
    PrDrawRendererBackend_Cpu,
    PrDrawRendererBackend_Canvas,
    PrDrawRendererBackend_Triangles,
    PrDrawRendererBackend_Fragment,
    PrDrawRendererBackend_Count,
};
typedef uint8_t PrDrawRendererBackend;

// Returns the descriptor of the renderer interface.
const PrBackendInterface* prDrawRendererSelectInterface(void);

// Creates a renderer of `backend` on `device`. Null options mean the defaults. Without a device, the CPU renderer
// creates its own memory device (pr/draw_cpu.h), and the other backends return NotSupported. Returns NotSupported if
// the build does not include the backend or the device cannot run it, and Lost if the device is lost. For Default, it
// also fails as prBackendFromEnvironment does. On failure, `*out` is null.
PrStatus prDrawRendererCreate(PrDrawRendererBackend backend, PrGpuDevice* PR_NULLABLE device,
    const PrDrawRendererOptions* PR_NULLABLE options, PrDrawRenderer* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Writers

// The writer backends. Default is the backend that PR_BACKEND_WRITER names (pr/backend.h). If the variable chooses
// nothing, Default is the first backend that the build includes.
enum {
    PrDrawWriterBackend_Default,
    PrDrawWriterBackend_Svg,
    PrDrawWriterBackend_Pdf,
    PrDrawWriterBackend_Count,
};
typedef uint8_t PrDrawWriterBackend;

// Returns the descriptor of the writer interface.
const PrBackendInterface* prDrawWriterSelectInterface(void);

// Creates a writer of `backend`. Null options mean the defaults (prDrawWriterOptionsDefault). Returns NotSupported if
// the build does not include the backend. For Default, it also fails as prBackendFromEnvironment does. On failure,
// `*out` is null.
PrStatus prDrawWriterCreate(PrDrawWriterBackend backend, const PrDrawWriterOptions* PR_NULLABLE options,
    PrDrawWriter* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
