// The model viewer's stages: a model's way from a file's path to a scene, in three steps. readModel reads the file and
// makes a model and a placement of it, stageEnter populates a renderer with the model and puts the placement in a
// scene, and the window draws the scene each frame.

#include "examples/model_viewer/viewer_stage.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/construct.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/geom.h"
#include "pr/memory.h"
#include "pr/text_buffer.h"
#include "pr/thread.h"

namespace model_viewer {

namespace {

// Finds a sphere that holds the model and the height of the ground under it, from the model's bounds, which the
// document states before anything loads. The bounds are those of the placement's pose at steps through its animation,
// so that the camera holds what the animation moves.
void frameModel(Stage& stage) {
    constexpr uint32_t kSteps = 24;
    float duration = 0;
    if (prModelPlacementAnimation(stage.placement) != kPrModelAnimationNone)
        duration = prModelAnimationDuration(stage.model, prModelPlacementAnimation(stage.placement));
    PrBox3 all{};
    bool any = false;
    for (uint32_t step = 0; step < kSteps; ++step) {
        PrBox3 box{};
        prModelPlacementSetTime(stage.placement, duration * float(step) / kSteps);
        if (!prModelPlacementBounds(stage.placement, &box))
            break;
        if (!any)
            all = box;
        any = true;
        all = prBox3Union(all, box);
    }
    prModelPlacementSetTime(stage.placement, 0);
    if (!any)
        return;
    PrVec3 half = (all.max - all.min) * 0.5f;
    stage.center = (all.min + all.max) * 0.5f;
    stage.radius = prF32Max(prVec3Length(half), 1e-6f);
    stage.floorY = all.min.y;
}

// Opens a model of the stage's file, and waits for its description, with one placement, which plays the stage's
// animation if the model has it. The placement is in no scene until the stage enters a renderer. A file that is not an
// asset that the host's formats read leaves the stage without a model, with the reason.
void readModel(Stage& stage) {
    PrModel* model = prModelHostOpenFile(stage.loader->host, stage.path);
    prModelWait(model);
    PrModelProgress progress = prModelProgress(model);
    if (progress.source != PrModelLoad_Ready) {
        if (progress.sourceStatus == PrStatus_Io)
            stage.reason = "the file cannot be read";
        else
            stage.reason = format("it is not an asset that the viewer reads ({})", prStatusName(progress.sourceStatus));
        prModelDestroy(model);
        return;
    }
    stage.model = model;
    PrModelPlacementDesc placed = prModelPlacementDescDefault(PrRenderScene{});
    stage.placement = prModelPlacementCreate(stage.model, &placed);
    if (stage.animation < prModelAnimationCount(stage.model))
        prModelPlacementSetAnimation(stage.placement, stage.animation, PrModelLoop_Repeat);
    frameModel(stage);
}

void readJob(void* PR_NULLABLE context) {
    Stage& stage = *static_cast<Stage*>(context);
    if (!stage.abandoned.load())
        readModel(stage);
    stage.isRead.store(true);
    stage.loader->jobs.fetchSub(1);
}

// Destroys what the stage made in its renderer beside the model's resources, which the model destroys.
void destroyScene(Stage& stage) {
    if (stage.render == nullptr)
        return;
    prRenderSceneDestroy(stage.render, stage.scene);
    prRenderMeshDestroy(stage.render, stage.ground);
    prRenderMaterialDestroy(stage.render, stage.groundMaterial);
    stage.render = nullptr;
}

void freeJob(void* PR_NULLABLE context) {
    Stage* stage = static_cast<Stage*>(context);
    Loader* loader = stage->loader;
    // The model leaves its renderer as it is destroyed, which waits for its running jobs.
    prModelDestroy(stage->model);
    destroyScene(*stage);
    destroy(stage);
    loader->jobs.fetchSub(1);
}

// How far the ground reaches to each side of the model, as a multiple of the model's radius: far enough that its edge
// lies within a few pixels of the horizon, where the sky's own ground of the same color takes over.
constexpr float kGroundReach = 300;

// Gives the ground its data: a square under the model, with a matte material of the ground's albedo.
void setGround(Stage& stage) {
    float reach = stage.radius * kGroundReach;
    PrRenderGeometryVertex vertices[4];
    for (uint32_t i = 0; i < 4; ++i) {
        PrVec3 corner{
            stage.center.x + (i % 2 == 0 ? -reach : reach), stage.floorY,
            stage.center.z + (i < 2 ? -reach : reach),
        };
        vertices[i] = prRenderGeometryVertexPack(corner, {0, 1, 0}, {0, 0, 0}, 1);
    }
    const uint32_t indices[6] = {0, 2, 1, 1, 2, 3};
    PrRenderMeshPart part{.indexCount = 6};
    // The matte material samples no texture, so the ground has no attributes, and its draw reads their fallbacks.
    PrRenderMeshData data{
        .geometry = vertices,
        .vertexCount = 4,
        .indices = indices,
        .indexCount = 6,
        .parts = &part,
        .partCount = 1,
    };
    prRenderMeshSetData(stage.render, stage.ground, &data);
    PrRenderKhronosPbrMaterial matte = prRenderKhronosPbrMaterialDefault();
    matte.baseColor = {kGroundAlbedo, kGroundAlbedo, kGroundAlbedo, 1};
    matte.metallic = 0;
    prRenderMaterialSetKhronosPbr(stage.render, stage.groundMaterial, &matte);
}

} // namespace

PrRenderSkyDesc stageSky(PrVec3 toSun) {
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = toSun;
    sky.groundAlbedo = {kGroundAlbedo, kGroundAlbedo, kGroundAlbedo};
    return sky;
}

Stage* stageCreate(Loader& loader, PrStringView path, uint32_t animation) {
    Stage* stage = create<Stage>();
    stage->loader = &loader;
    stage->path = path;
    stage->animation = animation;
    stage->isRead.store(false);
    stage->abandoned.store(false);
    loader.jobs.fetchAdd(1);
    prTaskPoolPost(loader.serial, readJob, stage);
    return stage;
}

void stageDestroy(Stage* PR_NULLABLE stage) {
    if (stage == nullptr)
        return;
    stage->abandoned.store(true);
    stage->loader->jobs.fetchAdd(1);
    prTaskPoolPost(stage->loader->serial, freeJob, stage);
}

void stageEnter(Stage& stage, PrRender* render, const PrRenderSceneEnvironment& environment,
        const PrRenderLightDesc* PR_NULLABLE sun) {
    PR_ASSERT(stage.isRead.load() && stage.render == nullptr);
    if (stage.model == nullptr)
        return;
    stage.render = render;
    stage.scene = prRenderSceneCreate(render);
    stage.ground = prRenderMeshCreate(render);
    stage.groundMaterial = prRenderMaterialCreate(render);
    setGround(stage);
    PrRenderUpdate* update = prRenderUpdateBegin(render, stage.scene);
    prRenderSceneEnvironmentSet(update, &environment);
    if (sun != nullptr)
        prRenderLightAdd(update, sun);
    // The ground receives the model's shadow and casts none, so the shadow map covers the model alone.
    PrRenderInstanceDesc ground = prRenderInstanceDescDefault(stage.ground);
    ground.materials = &stage.groundMaterial;
    ground.materialCount = 1;
    ground.visibility = ~kPrRenderVisibilityShadow;
    prRenderInstanceAdd(update, &ground);
    prRenderUpdateCommit(update);

    prModelPopulate(stage.model, render);
    prModelPlacementSetScene(stage.placement, stage.scene);
}

void stageLeave(Stage& stage) {
    if (stage.render == nullptr)
        return;
    prModelPopulate(stage.model, nullptr);
    destroyScene(stage);
}

bool stageIsDrawn(const Stage& stage) {
    return stage.render != nullptr;
}

bool stageHasShape(const Stage& stage) {
    if (!stage.isRead.load())
        return false;
    if (stage.model == nullptr)
        return true;
    PrModelProgress progress = prModelProgress(stage.model);
    return stage.render != nullptr && progress.meshesReady + progress.meshesFailed == progress.meshCount;
}

bool stageIsLoaded(const Stage& stage) {
    return stage.isRead.load() && (stage.model == nullptr || prModelIsLoaded(stage.model));
}

void loaderWait(Loader& loader) {
    while (loader.jobs.load() != 0)
        prThreadSleep(1);
}

} // namespace model_viewer
