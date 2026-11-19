// The model viewer's stages. A stage is one model of the list with what the viewer draws around it: the document, the
// model, one placement of it, and in a renderer a scene with a ground under the model.
//
// A stage never makes a frame wait. Creating one returns at once, and a thread of the loader reads the file, makes the
// model and frames it. The frame's thread then puts the stage in a renderer, which returns at once too, and lib/model
// loads the meshes and the images on the task pool. Destroying a stage returns at once: the loader's thread frees it,
// since a model waits for its running jobs before it leaves its renderer.
//
// Threads: the frame's thread calls every function here. It reads what the loader writes to a stage only once the stage
// says that it was read.

#pragma once

#include "pr/atomic.h"
#include "pr/base.h"
#include "pr/image.h"
#include "pr/model.h"
#include "pr/model_host.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/string_view.h"
#include "pr/task.h"
#include "pr/texture.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace model_viewer {

using namespace pr;

// What stages load with. The pools and the codec outlive every stage.
struct Loader {
    PrTaskPool* pool = nullptr;         // runs the jobs that load a model's meshes and images
    PrImageCodecSet* images = nullptr;
    PrModelHost* host = nullptr;            // the formats of models and of their textures

    // A pool of one thread, which reads the files and frees the stages. One thread runs them in the order that they
    // were asked for, so a stage is freed after its file was read, and at most one file is being read.
    PrTaskPool* serial = nullptr;

    Atomic<uint32_t> jobs;              // the jobs posted to `serial` that have not finished
};

// One model of the list, and its scene.
struct Stage {
    Loader* loader = nullptr;
    String path;
    uint32_t animation = 0;             // the animation that the placement plays, if the model has it

    // Set when the loader has read the file, or has skipped it because the stage was abandoned first. What follows is
    // the loader's until then.
    Atomic<bool> isRead;

    Atomic<bool> abandoned;             // the stage is to be freed, so its file need not be read

    // The model, or null with the reason if the file is not an asset that the host's formats read.
    PrModel* PR_NULLABLE model = nullptr;
    PrModelPlacement* PR_NULLABLE placement = nullptr;
    String reason;

    // A sphere that holds the model through its animation, and the height of the ground under it.
    PrVec3 center{};
    float radius = 1;
    float floorY = 0;

    // The renderer that the stage is in, or null, and what the stage made in it beside the model's own resources.
    PrRender* PR_NULLABLE render = nullptr;
    PrRenderScene scene{};
    PrRenderMesh ground{};
    PrRenderMaterial groundMaterial{};
};

// The part of the light that a stage's ground reflects, which is neutral: a light floor, as pale concrete is. It is
// light so that the model's shadow on it, which only the sky fills, stays above the darkest tones, which the neutral
// tone map darkens and saturates. The sky that lights a stage has the same ground, so that the stage's ground goes on
// to the horizon, and gives the model's shaded sides the light that such a floor sends up.
constexpr float kGroundAlbedo = 0.5f;

// Returns the sky that lights a stage, with its sun toward `toSun`: a clear day's daylight over the stage's ground.
PrRenderSkyDesc stageSky(PrVec3 toSun);

// Creates a stage for the model file at `path`, and starts reading it. The stage is in no renderer.
Stage* stageCreate(Loader& loader, PrStringView path, uint32_t animation);

// Frees the stage with everything that it made, some time after the call. A stage whose file has not been read yet is
// freed without reading it. The stage's renderer must outlive the freeing (loaderWait).
void stageDestroy(Stage* PR_NULLABLE stage);

// Puts a stage that was read into `render`: makes its scene, lit by `environment` and by `sun` if there is one, with
// the ground, populates the renderer with the model, and puts the placement in the scene. The model's parts appear as
// they load. A stage without a model makes nothing.
void stageEnter(Stage& stage, PrRender* render, const PrRenderSceneEnvironment& environment,
    const PrRenderLightDesc* PR_NULLABLE sun);

// Ends the stage's use of its renderer, as the renderer's destruction needs. The stage keeps its model and its
// placement, with its animation, and may enter another renderer. The call waits for the model's running jobs.
void stageLeave(Stage& stage);

// Returns true if the stage has a model in a renderer.
bool stageIsDrawn(const Stage& stage);

// Returns true if the stage is worth showing in place of another: it was read, and either it has no model to wait for,
// or every mesh of its model has loaded or failed. Its images may still be loading.
bool stageHasShape(const Stage& stage);

// Returns true if nothing of the stage is on its way: it was read, and its model, if it has one, has loaded.
bool stageIsLoaded(const Stage& stage);

// Waits until the loader's thread has nothing left to do: every file asked for is read, and every stage destroyed is
// freed. A renderer is destroyed after this.
void loaderWait(Loader& loader);

} // namespace model_viewer

PR_ASSUME_NONNULL_END
