// 3D rendering as a library of parts: meshes, textures, materials and environments on a GPU device, scenes of instances
// and lights that refer to them, and techniques that add the passes of a view to the application's render graph
// (pr/gpu_graph.h).
//
// The application owns its frame. Each frame it begins its graph, begins the renderer's frame (prRenderFrameBegin),
// adds each view's passes (pr/render_view.h), adds passes of its own, and records the graph. The renderer owns no
// window, swap chain or camera controls, and no model of the application's data: a scene is flat records in world
// space, which a producer writes.
//
// Everything is named by a small integer id, as lib/gpu names its resources. Zero is none. An id stays valid until its
// object is destroyed or removed, and names a later object only after that.
//
// Rendering never waits. A mesh, a texture, a material or an environment may be referenced as soon as it is created,
// before its data is set. An instance whose mesh has no data is not drawn, a texture without data reads as the value
// that the material's type declares for it, a material that is not set is a neutral one, and an environment without
// data is black. Populating an empty renderer piece by piece is therefore the one path for start-up, for streaming, and
// for recovery after a device is lost: the application makes a renderer on the new device and fills it again.
//
// Threads: creating, setting and destroying meshes, textures, materials, environments and scenes is thread-safe, and so
// are beginning and committing updates. One thread at a time fills an update. One thread at a time builds frames: it
// begins them, adds their views, and calls the queries that say so. What other threads set and commit takes effect when
// a frame begins, whole.
//
// A mesh may have a skin and morph targets, and an instance of it a pose (pr/render_scene.h). The frame then deforms
// the instance's vertices on the GPU, in a compute pass that the renderer adds with the frame's first view of the
// scene.
//
// The records on the GPU, a mesh's geometry and the shaders' functions are published for an application's own shaders
// (pr/render_records.h, pr/render_*.slang).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/render_shaders.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Renderers

// A renderer on one device: its shaders and pipelines, its meshes, textures and materials (pr/render_resource.h), its
// environments (pr/render_environment.h), and its scenes (pr/render_scene.h).
typedef struct PrRender PrRender;

// The default and the largest width of a face of an environment's cube, in texels.
enum {
    kPrRenderEnvironmentDefaultSize = 256,
    kPrRenderEnvironmentMaxSize = 2048,
};

// How a renderer is created.
typedef struct PrRenderOptions {
    uint32_t shadowMapSize;     // the width and height in texels of a scene's shadow map

    // The width in texels of a face of the cube that holds an environment made from an equirectangular image or from a
    // sky, at most kPrRenderEnvironmentMaxSize. Zero means kPrRenderEnvironmentDefaultSize.
    uint32_t environmentSize;

    // The material types and the shading models that the renderer draws with, which outlive it. Null means the
    // library's own (prRenderShaderSetLibrary). An application builds a set with its own types and models with
    // pr_render_shaders (pr/render_shaders.h).
    const PrRenderShaderSet* PR_NULLABLE shaders;

    // What fills each table that the application's shader set declares (pr/render_shaders.h): `tableCount` sources, one
    // for each table of the set but the library's own. prRenderCreate calls each source's function and keeps nothing of
    // the array.
    const PrRenderTableSource* PR_NULLABLE tables;
    uint32_t tableCount;
} PrRenderOptions;

// Returns the default options: a shadow map of 2048 texels, environments of kPrRenderEnvironmentDefaultSize, and the
// library's own shader set.
PrRenderOptions prRenderOptionsDefault(void);

// Creates a renderer on `device`, which is ready and outlives it. A null `options` means the defaults. Returns
// NotSupported if the device takes none of the renderer's shaders, and the status of the device's failure if it cannot
// make what the renderer needs, such as Lost. On failure, `*out` is null.
PrStatus prRenderCreate(PrGpuDevice* device, const PrRenderOptions* PR_NULLABLE options,
    PrRender* PR_NULLABLE* PR_NONNULL out);

// Frees the renderer and everything made with it. The device's work that uses it may still be running. No update of its
// scenes may be open.
void prRenderDestroy(PrRender* PR_NULLABLE render);

// Returns the device that the renderer was created on.
PrGpuDevice* prRenderDevice(const PrRender* render);

// Returns the shader set that the renderer draws with.
const PrRenderShaderSet* prRenderShaderSet(const PrRender* render);

// The numbers of a renderer's objects that hold an id: those created, less those whose destruction a frame has applied.
// An application that loads and unloads content checks with them that it frees what it creates.
typedef struct PrRenderInfo {
    uint32_t meshCount;
    uint32_t textureCount;
    uint32_t materialCount;
    uint32_t environmentCount;
    uint32_t sceneCount;
} PrRenderInfo;

// Returns the numbers of the renderer's objects. May be called from any thread.
PrRenderInfo prRenderInfo(const PrRender* render);

// The pipelines that a renderer has made for its draws, and the seconds that making them took. A renderer makes a
// pipeline when something first needs it, so the numbers grow as content of new kinds arrives: the forward technique
// has a pipeline for each material type that it draws, in each state that it draws the type in.
typedef struct PrRenderPipelineInfo {
    uint32_t count;
    double seconds;
} PrRenderPipelineInfo;

// Returns the pipelines that the renderer has made. May be called from any thread.
PrRenderPipelineInfo prRenderPipelineInfo(const PrRender* render);

// Begins a frame whose passes go to `graph`, which has begun its own frame (prGpuGraphBegin). Everything that was set
// and committed before the call takes effect: the frame's views draw it. The renderer serves one graph at a time, and
// the views of a frame are added before the graph is recorded.
void prRenderFrameBegin(PrRender* render, PrGpuGraph* graph);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
