// Studio's model view: a model's asset drawn by lib/render through lib/model, in a GPU view.

#include "pr/model_view.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/gpu_graph.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_image.h"
#include "pr/kit_input.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"
#include "pr/memory.h"
#include "pr/model.h"
#include "pr/raw_array.h"
#include "pr/raw_string.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/studio_window.h"
#include "pr/text_buffer.h"
#include "pr/time.h"

#include "render_shaders/StudioModelShaders.h"

namespace pr_impl {

namespace {

// The height of the bar of the controls, in px.
constexpr float kBarHeight = 32;

// How far a press may move, in px, and still be a click that picks rather than a drag that turns the camera.
constexpr float kClickSlop = 4;

// The camera's vertical field of view, in radians, and where the orbit camera starts: its turn about the y axis and its
// tilt above the asset's center, in radians.
constexpr float kFov = 0.7f;
constexpr float kTurn = 0.25f;
constexpr float kTilt = 0.2f;

// The direction toward the sun of the sky, which casts the asset's shadow on the ground.
constexpr PrVec3 kToSun{0.6f, 0.9f, 0.7f};

// The albedo of the ground, and how far it reaches to each side of the asset as a multiple of its radius: far enough
// that its edge lies within a few pixels of the horizon, where the sky's own ground of the same albedo takes over.
constexpr float kGroundAlbedo = 0.5f;
constexpr float kGroundReach = 300;

// The exposure at which the ACES curve shows middle gray as middle gray.
constexpr float kAcesExposure = 1.45f;

// The EV100 of a clear day, which a manual exposure starts at.
constexpr float kClearDayEv100 = 15;

// How fast a meter of the image moves the exposure toward its reading: the seconds in which it closes half of the
// difference. The meter's exposure is settled once it is within kSettledEv of its reading, where it stops.
constexpr float kAdaptHalfLife = 0.25f;
constexpr float kSettledEv = 1.0f / 32;

// The names of the choices of the lighting, by their values.
constexpr PrStringView kEnvironmentNames[PrModelViewEnvironment_Count] = {"asset", "sky", "none"};
constexpr PrStringView kExposureNames[PrModelViewExposure_Count] = {"scene", "image", "manual"};

// The width of the lighting's popover, in px.
constexpr float kLightingWidth = 320;

// The color of the highlight of the selection, linear.
constexpr PrColor kHighlight{1.0f, 0.42f, 0.08f, 1};

struct ModelView {
    PrKit* kit;
    PrStudioWindowGpuView* PR_NULLABLE gpuView;     // of the window that shows the view, if the host gave one

    // The asset: the model and its one placement, whether the view has framed the model since its description arrived,
    // and the sphere that holds the placement's meshes through its animation, which frames the orbit camera, and the
    // height of the ground.
    PrModel* PR_NULLABLE model;
    PrModelPlacement* PR_NULLABLE placement;
    bool framed;
    PrVec3 center;
    float radius;
    float floorY;

    // The renderer on the device that renders the window, made at the first render, and what the view puts in it.
    PrGpuDevice* PR_NULLABLE device;
    PrRender* PR_NULLABLE render;
    PrRenderScene scene;
    PrRenderEnvironment environment;
    PrRenderMesh ground;
    PrRenderMaterial groundMaterial;
    PrRenderInstance groundInstance;
    PrRenderMaterial highlight;
    PrRenderLight sun;
    PrRenderMeter meter;
    bool populated;                 // the model is in the renderer and the placement in the scene

    // The camera and the controls.
    uint32_t camera;
    float turn;
    float tilt;
    float zoom;                     // the distance, as a multiple of the distance at which the sphere fills the view
    PrModelAnimationIndex animation;
    bool playing;
    PrRenderToneMap toneMap;
    PrModelViewLighting lighting;
    bool showsGround;
    PrModelNodeIndex selection;

    // What a click picked in the round of the last prModelViewFrame, if `clicked`.
    PrModelPick picked;
    bool clicked;

    // The texture that the view renders into, at the extent of the last render, which the view holds a reference to,
    // whether what it shows changed since, the progress of loading that it showed, and when the animation last moved.
    PrGpuTexture texture;
    PrUVec2 textureExtent;
    bool dirty;
    PrModelProgress progress;
    double animated;
    uint64_t renders;

    // The exposure: the EV100 of the last render, and of a meter of the image, the renders that the meter read and the
    // serial of the last reading that the view took, the EV100 that the reading gave, whether the view has taken one
    // since its asset was set, and when the view last moved its exposure.
    float ev100;
    uint64_t metered;
    uint64_t readSerial;
    float imageTarget;
    bool imageRead;
    double exposedAt;
};

double nowSeconds() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// Finds a sphere that holds the placement's meshes at steps through its animation, and the height of the ground under
// it, from the bounds that the document states, before anything loads.
void frameModel(ModelView* v) {
    constexpr uint32_t kSteps = 24;

    float duration = 0;
    if (v->animation != kPrModelAnimationNone)
        duration = prModelAnimationDuration(v->model, v->animation);
    PrBox3 all{};
    bool any = false;
    for (uint32_t step = 0; step < kSteps; ++step) {
        PrBox3 box{};
        prModelPlacementSetTime(v->placement, duration * float(step) / kSteps);
        if (!prModelPlacementBounds(v->placement, &box))
            break;
        all = any ? prBox3Union(all, box) : box;
        any = true;
    }
    prModelPlacementSetTime(v->placement, 0);
    v->center = any ? (all.min + all.max) * 0.5f : PrVec3{};
    v->radius = any ? prF32Max(prVec3Length((all.max - all.min) * 0.5f), 1e-6f) : 1;
    v->floorY = any ? all.min.y : 0;
}

// Frees the model and its placement, which leave the renderer.
void clearModel(ModelView* v) {
    prModelPlacementDestroy(v->placement);
    prModelDestroy(v->model);
    v->placement = nullptr;
    v->model = nullptr;
    v->populated = false;
    v->framed = false;
}

void setAssetLight(ModelView* v);

// Gives the view's model the view's defaults once its description has arrived: its lighting, its first animation, and
// the orbit camera framing it. A model that is opened arrives later than the view takes it.
void describeModel(ModelView* v) {
    if (v->framed || v->model == nullptr || prModelSource(v->model) == nullptr)
        return;
    v->framed = true;
    v->lighting = prModelViewLightingDefault(v->model);
    v->imageRead = false;
    setAssetLight(v);
    if (prModelAnimationCount(v->model) != 0) {
        v->animation = 0;
        v->playing = true;
        prModelPlacementSetAnimation(v->placement, 0, PrModelLoop_Repeat);
    }
    frameModel(v);
    v->animated = nowSeconds();
    v->dirty = true;
}

// Frees what the view made in its renderer, and the renderer. The model leaves it first.
void closeRenderer(ModelView* v) {
    if (v->model != nullptr)
        prModelPopulate(v->model, nullptr);
    v->populated = false;
    v->metered = 0;
    v->readSerial = 0;
    v->imageRead = false;
    if (v->render != nullptr) {
        prRenderMeterDestroy(v->render, v->meter);
        prRenderSceneDestroy(v->render, v->scene);
        prRenderEnvironmentDestroy(v->render, v->environment);
        prRenderMeshDestroy(v->render, v->ground);
        prRenderMaterialDestroy(v->render, v->groundMaterial);
        prRenderMaterialDestroy(v->render, v->highlight);
        prRenderDestroy(v->render);
    }
    if (v->device != nullptr)
        prGpuTextureRelease(v->device, v->texture);
    v->render = nullptr;
    v->device = nullptr;
    v->texture = PrGpuTexture{};
    v->textureExtent = PrUVec2{};
}

// Returns the sky that lights the asset: a clear day's daylight over the ground.
PrRenderSkyDesc skyDesc() {
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = kToSun;
    sky.groundAlbedo = {kGroundAlbedo, kGroundAlbedo, kGroundAlbedo};
    return sky;
}

bool showsSky(const ModelView* v) {
    return v->lighting.environment == PrModelViewEnvironment_Sky;
}

// Returns the sky's sun as a light of the scene, at the environment's scale, which casts the shadows, or a light that
// gives nothing while the sky is not the environment.
PrRenderLightDesc sunDesc(const ModelView* v) {
    PrRenderSkyDesc sky = skyDesc();
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = -kToSun;
    sun.color = prRenderSkySunIrradiance(&sky);
    sun.intensity = showsSky(v) ? prF32Exp2(v->lighting.environmentStops) : 0;
    sun.castsShadows = showsSky(v);
    return sun;
}

// Sets the scene's environment to the one that the lighting chooses, at its scale, and the sun's light to match. A
// scene has one environment, so the sky replaces the asset's.
void setEnvironment(ModelView* v, PrRenderUpdate* update) {
    PrRenderSceneEnvironment environment = prRenderSceneEnvironmentDefault(PrRenderEnvironment{});
    if (showsSky(v))
        environment = prRenderSceneEnvironmentDefault(v->environment);
    PrRenderSceneEnvironment own{};
    bool asset = v->lighting.environment == PrModelViewEnvironment_Asset;
    if (asset && v->populated && prModelPlacementEnvironment(v->placement, &own))
        environment = own;
    environment.intensity *= prF32Exp2(v->lighting.environmentStops);
    prRenderSceneEnvironmentSet(update, &environment);
    PrRenderLightDesc sun = sunDesc(v);
    prRenderLightSet(update, v->sun, &sun);
}

// Gives the asset's lights and its emission the lighting's scale and unit.
void setAssetLight(ModelView* v) {
    if (v->model == nullptr)
        return;
    prModelSetEmission(v->model, v->lighting.emission);
    float scale = prF32Exp2(v->lighting.lightStops);
    for (uint32_t i = 0; v->placement != nullptr && i < prModelLightCount(v->model); ++i)
        prModelPlacementSetLightScale(v->placement, i, scale);
}

// Returns true while a meter of the image moves the exposure: a render's reading has yet to arrive, or the exposure has
// yet to reach the last reading.
bool imageAdapting(const ModelView* v) {
    if (v->lighting.exposure != PrModelViewExposure_Image || v->render == nullptr)
        return false;
    PrRenderMeterReading reading = prRenderMeterRead(v->render, v->meter);
    if (reading.serial < v->metered)
        return true;
    return reading.serial > v->readSerial || (v->imageRead && prF32Abs(v->ev100 - v->imageTarget) >= kSettledEv);
}

// Returns the EV100 that the next render is exposed at, before the compensation, by the lighting's mode, and moves a
// meter of the image's exposure toward its reading. `sceneEv100` is the meter of the scene's light, which a meter of
// the image starts from until its first reading.
float chooseEv100(ModelView* v, float sceneEv100, double now) {
    double seconds = now - v->exposedAt;
    v->exposedAt = now;
    if (v->lighting.exposure == PrModelViewExposure_Manual)
        return v->lighting.ev100;
    if (v->lighting.exposure == PrModelViewExposure_Scene)
        return sceneEv100;
    PrRenderMeterReading reading = prRenderMeterRead(v->render, v->meter);
    if (reading.serial > v->readSerial) {
        v->readSerial = reading.serial;
        // A view of nothing that has light keeps the exposure that it had.
        if (reading.coverage > 0) {
            // The first reading of an asset is where the exposure starts: there is nothing to adapt from.
            if (!v->imageRead)
                v->ev100 = reading.ev100;
            v->imageTarget = reading.ev100;
            v->imageRead = true;
        }
    }
    if (!v->imageRead)
        return sceneEv100;
    float ev100 = prRenderEv100Adapt(v->ev100, v->imageTarget, float(seconds), kAdaptHalfLife);
    return prF32Abs(ev100 - v->imageTarget) < kSettledEv ? v->imageTarget : ev100;
}

// Makes the renderer on `device` and the scene that every asset of the view is placed in: the sky, its sun, which casts
// the shadows, and the ground's mesh and material. Returns false if the device draws nothing of lib/render.
bool openRenderer(ModelView* v, PrGpuDevice* device) {
    if (v->render != nullptr && v->device == device)
        return true;
    closeRenderer(v);
    PrRenderOptions options = prRenderOptionsDefault();
    options.shaders = studioModelShaderSet();
    if (prRenderCreate(device, &options, &v->render) != PrStatus_Ok) {
        v->render = nullptr;
        return false;
    }
    v->device = device;
    v->scene = prRenderSceneCreate(v->render);
    v->environment = prRenderEnvironmentCreate(v->render);
    PrRenderSkyDesc sky = skyDesc();
    prRenderEnvironmentSetSky(v->render, v->environment, &sky);
    v->ground = prRenderMeshCreate(v->render);
    v->groundMaterial = prRenderMaterialCreate(v->render);
    PrRenderKhronosPbrMaterial matte = prRenderKhronosPbrMaterialDefault();
    matte.baseColor = {kGroundAlbedo, kGroundAlbedo, kGroundAlbedo, 1};
    matte.metallic = 0;
    prRenderMaterialSetKhronosPbr(v->render, v->groundMaterial, &matte);
    v->highlight = prRenderMaterialCreate(v->render);
    PrRenderKhronosPbrMaterial lit = prRenderKhronosPbrMaterialDefault();
    lit.baseColor = kHighlight;
    lit.metallic = 0;
    lit.emissive = PrVec3{kHighlight.r * 0.25f, kHighlight.g * 0.25f, kHighlight.b * 0.25f};
    prRenderMaterialSetKhronosPbr(v->render, v->highlight, &lit);

    v->meter = prRenderMeterCreate(v->render);
    PrRenderUpdate* update = prRenderUpdateBegin(v->render, v->scene);
    PrRenderLightDesc sun = sunDesc(v);
    v->sun = prRenderLightAdd(update, &sun);
    setEnvironment(v, update);
    // The ground receives the asset's shadow and casts none, so the shadow map covers the asset alone.
    PrRenderInstanceDesc ground = prRenderInstanceDescDefault(v->ground);
    ground.materials = &v->groundMaterial;
    ground.materialCount = 1;
    ground.visibility = v->showsGround ? ~kPrRenderVisibilityShadow : 0;
    v->groundInstance = prRenderInstanceAdd(update, &ground);
    prRenderUpdateCommit(update);
    return true;
}

// Gives the ground its data: a square under the asset.
void setGround(ModelView* v) {
    float reach = v->radius * kGroundReach;
    PrRenderGeometryVertex vertices[4];
    for (uint32_t i = 0; i < 4; ++i) {
        PrVec3 corner{v->center.x + (i % 2 == 0 ? -reach : reach), v->floorY, v->center.z + (i < 2 ? -reach : reach)};
        vertices[i] = prRenderGeometryVertexPack(corner, {0, 1, 0}, {0, 0, 0}, 1);
    }
    const uint32_t indices[6] = {0, 2, 1, 1, 2, 3};
    PrRenderMeshPart part{.indexCount = 6};
    // The ground's material samples no texture, so the ground has no attributes, and its draw reads their fallbacks.
    PrRenderMeshData data{
        .geometry = vertices,
        .vertexCount = 4,
        .indices = indices,
        .indexCount = 6,
        .parts = &part,
        .partCount = 1,
    };
    prRenderMeshSetData(v->render, v->ground, &data);
}

// Puts the model in the renderer and its placement in the scene, once both exist.
void populate(ModelView* v) {
    if (v->populated || v->model == nullptr || v->render == nullptr)
        return;
    prModelPopulate(v->model, v->render);
    prModelPlacementSetScene(v->placement, v->scene);
    setGround(v);
    v->populated = true;
}

// Returns the view's camera for a target whose width over its height is `aspect`.
PrRenderCamera cameraOf(const ModelView* v, float aspect) {
    if (v->camera != 0)
        return prModelPlacementCamera(v->placement, v->camera - 1, aspect);
    PrVec3 direction{prF32Cos(v->tilt) * prF32Sin(v->turn), prF32Sin(v->tilt), prF32Cos(v->tilt) * prF32Cos(v->turn)};
    float distance = v->radius / prF32Sin(kFov * 0.5f) * v->zoom;
    return prRenderCameraLookAt(v->center + direction * distance, v->center, kFov, aspect, distance * 0.01f);
}

// Returns the point of the world that a point of clip space maps from, through the inverse of the camera's matrix.
PrVec3 unproject(const PrMatrix4& inverse, float x, float y, float depth) {
    float in[4] = {x, y, depth, 1};
    float out[4] = {};
    for (uint32_t row = 0; row < 4; ++row) {
        for (uint32_t k = 0; k < 4; ++k)
            out[row] += inverse.m[row][k] * in[k];
    }
    return PrVec3{out[0] / out[3], out[1] / out[3], out[2] / out[3]};
}

// Returns what shows at `point`, in px from the top left of the view's asset of `extent` px, with a node of
// kPrModelNodeNone if the point shows nothing of the asset.
PrModelPick pickAt(ModelView* v, PrVec2 point, PrVec2 extent) {
    PrModelPick pick{.node = kPrModelNodeNone, .mesh = kPrModelMeshNone, .part = kPrModelPartNone};
    if (v->placement == nullptr || extent.x <= 0 || extent.y <= 0)
        return pick;
    PrRenderCamera camera = cameraOf(v, extent.x / extent.y);
    PrMatrix4 clip = prMatrix4Concat(&camera.view, &camera.projection);
    PrMatrix4 inverse{};
    if (!prMatrix4Invert(&clip, &inverse))
        return pick;
    // The ray runs from the nearest depth that the camera sees, 1, through a depth halfway to the farthest, which is
    // finite for every projection.
    float x = point.x / extent.x * 2 - 1;
    float y = 1 - point.y / extent.y * 2;
    PrVec3 nearest = unproject(inverse, x, y, 1);
    PrVec3 farther = unproject(inverse, x, y, 0.5f);
    PrMeshRay ray{.origin = nearest, .tMin = 0, .direction = farther - nearest, .tMax = kPrF32Max};
    if (!prModelPlacementPick(v->placement, ray, &pick))
        pick = PrModelPick{.node = kPrModelNodeNone, .mesh = kPrModelMeshNone, .part = kPrModelPartNone};
    return pick;
}

bool sameProgress(const PrModelProgress& a, const PrModelProgress& b) {
    return a.imagesReady == b.imagesReady && a.imagesFailed == b.imagesFailed && a.meshesReady == b.meshesReady &&
        a.meshesFailed == b.meshesFailed;
}

// What a frame shows: the view's color, tone mapped into a texture that the frame's 2D draws.
struct Shown {
    PrRender* render;
    PrGpuGraph* graph;
    PrGpuGraphTexture color;
    PrRenderDisplayOptions display;
};

void drawShown(void* PR_NULLABLE context, PrGpuPass* pass) {
    const Shown& s = *static_cast<const Shown*>(context);
    prRenderDisplayDraw(s.render, pass, prGpuGraphTexture(s.graph, s.color), &s.display);
}

void setCamera(ModelView* v, uint32_t camera) {
    if (v->model == nullptr || camera > prModelCameraCount(v->model))
        camera = 0;
    v->dirty = v->dirty || v->camera != camera;
    v->camera = camera;
}

void setAnimation(ModelView* v, PrModelAnimationIndex animation) {
    if (v->placement == nullptr)
        return;
    if (animation != kPrModelAnimationNone && animation >= prModelAnimationCount(v->model))
        animation = kPrModelAnimationNone;
    v->animation = animation;
    v->playing = animation != kPrModelAnimationNone;
    prModelPlacementSetAnimation(v->placement, animation, PrModelLoop_Repeat);
    v->animated = nowSeconds();
    v->dirty = true;
}

void setPlaying(ModelView* v, bool playing) {
    v->playing = playing && v->animation != kPrModelAnimationNone;
    v->animated = nowSeconds();
    v->dirty = true;
}

PrStringView toneMapLabel(PrRenderToneMap toneMap) {
    switch (toneMap) {
        case PrRenderToneMap_Neutral:
            return "Neutral";
        case PrRenderToneMap_AcesChannels:
            return "ACES, channels";
        default:
            return "ACES, luminance";
    }
}

// Appends a number with one decimal and a sign, then a unit.
void appendStops(PrTextBuffer* text, float value, PrStringView unit) {
    float tenths = prF32Round(value * 10);
    prTextBufferAppend(text, tenths < 0 ? "-" : tenths > 0 ? "+" : "");
    uint64_t whole = uint64_t(prF32Abs(tenths));
    prTextBufferAppendU64(text, whole / 10, 10);
    prTextBufferAppend(text, ".");
    prTextBufferAppendU64(text, whole % 10, 10);
    prTextBufferAppend(text, unit);
}

// Declares a row of the lighting's popover: a label that shows `value` in stops after `name`, and a slider of it
// between `min` and `max`. Returns true if the slider changed the value.
bool stopsRow(ModelView* v, PrLayoutNode parent, PrStringView name, float* value, float min, float max, bool disabled) {
    PrKit* kit = v->kit;
    PrTextBuffer text = {};
    prTextBufferAppend(&text, name);
    prTextBufferAppend(&text, " ");
    appendStops(&text, *value, " EV");
    PrLayoutNode row = prKitInspectorRow(kit, parent, {.label = prTextBufferView(&text)}, {});
    prTextBufferReset(&text);
    float before = *value;
    PrKitSlider slider{.value = prKitScalarF32(value), .min = double(min), .max = double(max), .disabled = disabled};
    prKitSlider(kit, row, slider, {.size = PrKitControlSize_Sm});
    return *value != before;
}

// Declares the controls of the lighting under `parent`, the popover of the bar's Light button.
void buildLighting(ModelView* v, PrLayoutNode parent) {
    PrKit* kit = v->kit;
    PrModelViewLighting& l = v->lighting;
    PrKitChoiceStyle small{.size = PrKitControlSize_Sm};
    PrModelLightingFlags brings = prModelLighting(v->model);
    bool changed = false;

    PrLayoutNode environment = prKitInspectorRow(kit, parent, {.label = "Environment"}, {});
    PrLayoutNode sources = prKitSegmented(kit, environment, {}, small);
    constexpr PrStringView kEnvironmentLabels[PrModelViewEnvironment_Count] = {"Asset", "Sky", "None"};
    for (PrModelViewEnvironment i = 0; i < PrModelViewEnvironment_Count; ++i) {
        bool absent = i == PrModelViewEnvironment_Asset && (brings & PrModelLightingFlag_Environment) == 0;
        PrKitChoice choice{.label = kEnvironmentLabels[i], .selected = l.environment == i, .disabled = absent};
        if (prKitActivated(kit, prKitSegment(kit, sources, choice))) {
            changed = changed || l.environment != i;
            l.environment = i;
        }
    }
    changed |= stopsRow(v, parent, "Environment", &l.environmentStops, -8, 16,
        l.environment == PrModelViewEnvironment_None);
    changed |= stopsRow(v, parent, "Asset lights", &l.lightStops, -16, 8, (brings & PrModelLightingFlag_Lights) == 0);

    PrLayoutNode emission = prKitInspectorRow(kit, parent, {.label = "Emission"}, {});
    PrLayoutNode units = prKitSegmented(kit, emission, {}, small);
    constexpr PrStringView kEmissionLabels[PrRenderEmission_Count] = {"Nits", "Display"};
    for (PrRenderEmission i = 0; i < PrRenderEmission_Count; ++i) {
        PrKitChoice choice{.label = kEmissionLabels[i], .selected = l.emission == i};
        if (prKitActivated(kit, prKitSegment(kit, units, choice))) {
            changed = changed || l.emission != i;
            l.emission = i;
        }
    }

    PrLayoutNode exposure = prKitInspectorRow(kit, parent, {.label = "Exposure"}, {});
    PrLayoutNode modes = prKitSegmented(kit, exposure, {}, small);
    constexpr PrStringView kExposureLabels[PrModelViewExposure_Count] = {"Lights", "Image", "Manual"};
    for (PrModelViewExposure i = 0; i < PrModelViewExposure_Count; ++i) {
        PrKitChoice choice{.label = kExposureLabels[i], .selected = l.exposure == i};
        if (prKitActivated(kit, prKitSegment(kit, modes, choice))) {
            // A manual exposure starts where the meter was, so that choosing it changes nothing at once.
            if (i == PrModelViewExposure_Manual && l.exposure != i)
                l.ev100 = prF32Round(v->ev100 * 10) / 10;
            changed = changed || l.exposure != i;
            l.exposure = i;
        }
    }
    // The EV100 shows the meter's while a meter chooses it.
    bool manual = l.exposure == PrModelViewExposure_Manual;
    float shown = manual ? l.ev100 : v->ev100;
    PrTextBuffer text = {};
    prTextBufferAppend(&text, "EV100 ");
    appendStops(&text, shown, "");
    PrLayoutNode ev = prKitInspectorRow(kit, parent, {.label = prTextBufferView(&text)}, {});
    prTextBufferReset(&text);
    float before = l.ev100;
    PrKitSlider slider{.value = prKitScalarF32(&l.ev100), .min = -4, .max = 20, .disabled = !manual};
    prKitSlider(kit, ev, slider, {.size = PrKitControlSize_Sm});
    changed |= l.ev100 != before;
    changed |= stopsRow(v, parent, "Compensation", &l.compensation, -4, 4, false);
    v->dirty = v->dirty || changed;
}

// Declares the bar of the controls under `parent`.
void buildBar(ModelView* v, PrLayoutNode parent) {
    PrKit* kit = v->kit;
    PrKitBoxStyle row{
        .direction = PrKitDirection_Row,
        .align = PrKitAlign_Center,
        .gap = 6,
        .padding = prKitEdgesAll(4),
        .place = {.height = prKitPx(kBarHeight), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode bar = prKitBox(kit, parent, {}, row);
    PrKitChoiceStyle small{.size = PrKitControlSize_Sm};

    PrTextBuffer text = {};

    // The camera: the orbit camera, or one of the model's.
    uint32_t cameras = prModelCameraCount(v->model);
    if (v->camera == 0) {
        prTextBufferAppend(&text, "Orbit");
    } else {
        PrModelNodeIndex node = prModelCameraNode(v->model, v->camera - 1);
        PrStringView name = prModelNodeName(v->model, node);
        if (name.size != 0)
            prTextBufferAppend(&text, name);
        else
            prTextBufferAppend(&text, "Camera");
    }
    PrLayoutNode camera = prKitSelect(kit, bar, {.label = prTextBufferView(&text), .disabled = cameras == 0}, small);
    prTextBufferClear(&text);
    if (PrLayoutNode menu = prKitSelectMenu(kit, camera); menu != kPrLayoutNodeNone) {
        for (uint32_t i = 0; i <= cameras; ++i) {
            if (i == 0) {
                prTextBufferAppend(&text, "Orbit");
            } else {
                PrStringView name = prModelNodeName(v->model, prModelCameraNode(v->model, i - 1));
                prTextBufferAppend(&text, name.size != 0 ? name : PrStringView("Camera"));
            }
            PrKitMenuItem item{.text = prTextBufferView(&text), .checked = v->camera == i};
            if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {})))
                setCamera(v, i);
            prTextBufferClear(&text);
        }
    }

    // The animation, and whether it plays.
    uint32_t animations = prModelAnimationCount(v->model);
    if (v->animation == kPrModelAnimationNone) {
        prTextBufferAppend(&text, animations == 0 ? "No animation" : "Rest pose");
    } else {
        PrStringView name = prModelAnimationName(v->model, v->animation);
        prTextBufferAppend(&text, name.size != 0 ? name : PrStringView("Animation"));
    }
    PrLayoutNode animation = prKitSelect(kit, bar, {.label = prTextBufferView(&text), .disabled = animations == 0}, small);
    prTextBufferClear(&text);
    if (PrLayoutNode menu = prKitSelectMenu(kit, animation); menu != kPrLayoutNodeNone) {
        PrKitMenuItem rest{.text = "Rest pose", .checked = v->animation == kPrModelAnimationNone};
        if (prKitActivated(kit, prKitMenuItem(kit, menu, rest, {})))
            setAnimation(v, kPrModelAnimationNone);
        for (uint32_t i = 0; i < animations; ++i) {
            PrStringView name = prModelAnimationName(v->model, i);
            if (name.size != 0) {
                prTextBufferAppend(&text, name);
            } else {
                prTextBufferAppend(&text, "Animation ");
                prTextBufferAppendU64(&text, i, 10);
            }
            PrKitMenuItem item{.text = prTextBufferView(&text), .checked = v->animation == i};
            if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {})))
                setAnimation(v, i);
            prTextBufferClear(&text);
        }
    }
    PrKitButtonStyle ghost{.variant = PrKitButtonVariant_Ghost, .size = PrKitControlSize_Sm};
    PrKitButton play{
        .icon = PrStudioIconKind(v->playing ? PrStudioIconKind_Pause : PrStudioIconKind_Play),
        .disabled = v->animation == kPrModelAnimationNone,
    };
    if (prKitActivated(kit, prKitButton(kit, bar, play, ghost)))
        setPlaying(v, !v->playing);

    // The display: the tone map, the exposure, and the ground.
    PrLayoutNode toneMap = prKitSelect(kit, bar, {.label = toneMapLabel(v->toneMap)}, small);
    if (PrLayoutNode menu = prKitSelectMenu(kit, toneMap); menu != kPrLayoutNodeNone) {
        for (uint32_t i = 0; i < PrRenderToneMap_Count; ++i) {
            PrKitMenuItem item{.text = toneMapLabel(PrRenderToneMap(i)), .checked = v->toneMap == i};
            if (prKitActivated(kit, prKitMenuItem(kit, menu, item, {}))) {
                v->toneMap = PrRenderToneMap(i);
                v->dirty = true;
            }
        }
    }
    PrKitButton light{.text = "Light", .icon = PrStudioIconKind_Light};
    PrLayoutNode trigger = prKitButton(kit, bar, light, ghost);
    if (prKitActivated(kit, trigger))
        prKitToggleOpen(kit, trigger);
    PrLayoutNode popover = prKitPopover(kit, trigger, {}, {.offset = {0, 2}, .minWidth = kLightingWidth});
    if (popover != kPrLayoutNodeNone)
        buildLighting(v, popover);
    PrKitButton ground{.text = "Ground", .selected = v->showsGround};
    if (prKitActivated(kit, prKitButton(kit, bar, ground, ghost))) {
        v->showsGround = !v->showsGround;
        v->dirty = true;
    }
    prTextBufferReset(&text);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrModelView : ModelView {};

PrModelView* prModelViewCreate(const PrModelViewDesc* desc) {
    PrModelView* view = allocZeroed<PrModelView>();
    view->kit = desc->kit;
    if (desc->window != nullptr) {
        PrStudioWindowGpuViewDesc gpu{
            .render = [](void* PR_NULLABLE context, const PrStudioWindowRender* render) {
                return prModelViewRender(static_cast<PrModelView*>(context), render);
            },
            .lost = [](void* PR_NULLABLE context, PrGpuDevice*) {
                prModelViewDeviceLost(static_cast<PrModelView*>(context));
            },
            .context = view,
        };
        view->gpuView = prStudioWindowGpuViewCreate(desc->window, &gpu);
    }
    view->animation = kPrModelAnimationNone;
    view->selection = kPrModelNodeNone;
    view->toneMap = PrRenderToneMap_AcesLuminance;
    view->lighting = prModelViewLightingDefault(nullptr);
    view->showsGround = true;
    view->zoom = 1;
    return view;
}

void prModelViewDestroy(PrModelView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    closeRenderer(view);
    clearModel(view);
    prStudioWindowGpuViewDestroy(view->gpuView);
    prMemoryFree(view);
}

void prModelViewSetModel(PrModelView* view, PrModel* PR_NULLABLE model) {
    clearModel(view);
    view->model = model;
    view->camera = 0;
    view->turn = kTurn;
    view->tilt = kTilt;
    view->zoom = 1;
    view->animation = kPrModelAnimationNone;
    view->playing = false;
    view->selection = kPrModelNodeNone;
    view->dirty = true;
    if (model == nullptr)
        return;
    PrModelPlacementDesc placed = prModelPlacementDescDefault(PrRenderScene{});
    view->placement = prModelPlacementCreate(view->model, &placed);
    describeModel(view);
}

const PrModel* PR_NULLABLE prModelViewModel(const PrModelView* view) {
    return view->model;
}

void prModelViewSetSelection(PrModelView* view, PrModelNodeIndex node) {
    if (view->model == nullptr || node >= prModelNodeCount(view->model))
        node = kPrModelNodeNone;
    view->dirty = view->dirty || view->selection != node;
    view->selection = node;
}

void prModelViewSetCamera(PrModelView* view, uint32_t camera) {
    setCamera(view, camera);
}

uint32_t prModelViewCamera(const PrModelView* view) {
    return view->camera;
}

void prModelViewSetAnimation(PrModelView* view, PrModelAnimationIndex animation) {
    setAnimation(view, animation);
}

PrModelAnimationIndex prModelViewAnimation(const PrModelView* view) {
    return view->animation;
}

void prModelViewSetPlaying(PrModelView* view, bool playing) {
    setPlaying(view, playing);
}

bool prModelViewPlaying(const PrModelView* view) {
    return view->playing;
}

PrModelViewLighting prModelViewLightingDefault(const PrModel* PR_NULLABLE model) {
    PrModelLightingFlags brings = model != nullptr ? prModelLighting(model) : 0;
    bool environment = (brings & PrModelLightingFlag_Environment) != 0;
    bool lights = (brings & PrModelLightingFlag_Lights) != 0;
    PrModelViewLighting lighting{
        .environment = PrModelViewEnvironment_None,
        .emission = PrRenderEmission(environment || lights ? PrRenderEmission_Luminance : PrRenderEmission_Display),
        .exposure = PrModelViewExposure_Scene,
        .ev100 = kClearDayEv100,
    };
    if (environment)
        lighting.environment = PrModelViewEnvironment_Asset;
    else if (!lights)
        lighting.environment = PrModelViewEnvironment_Sky;
    return lighting;
}

PrModelViewLighting prModelViewLighting(const PrModelView* view) {
    return view->lighting;
}

void prModelViewSetLighting(PrModelView* view, const PrModelViewLighting* lighting) {
    PR_ASSERT(lighting->environment < PrModelViewEnvironment_Count);
    PR_ASSERT(lighting->exposure < PrModelViewExposure_Count && lighting->emission < PrRenderEmission_Count);
    view->lighting = *lighting;
    setAssetLight(view);
    view->dirty = true;
}

float prModelViewEv100(const PrModelView* view) {
    return view->ev100;
}

PrModelViewEnvironment prModelViewEnvironmentFromName(PrStringView name) {
    for (PrModelViewEnvironment i = 0; i < PrModelViewEnvironment_Count; ++i) {
        if (prAsciiEqualsFold(kEnvironmentNames[i], name))
            return i;
    }
    return PrModelViewEnvironment_Count;
}

PrStringView prModelViewEnvironmentName(PrModelViewEnvironment environment) {
    return environment < PrModelViewEnvironment_Count ? kEnvironmentNames[environment] : PrStringView{};
}

PrModelViewExposure prModelViewExposureFromName(PrStringView name) {
    for (PrModelViewExposure i = 0; i < PrModelViewExposure_Count; ++i) {
        if (prAsciiEqualsFold(kExposureNames[i], name))
            return i;
    }
    return PrModelViewExposure_Count;
}

PrStringView prModelViewExposureName(PrModelViewExposure exposure) {
    return exposure < PrModelViewExposure_Count ? kExposureNames[exposure] : PrStringView{};
}

PrModelViewOrbit prModelViewOrbit(const PrModelView* view) {
    return PrModelViewOrbit{.turn = view->turn, .tilt = view->tilt, .zoom = view->zoom};
}

void prModelViewSetOrbit(PrModelView* view, PrModelViewOrbit orbit) {
    view->turn = orbit.turn;
    view->tilt = prF32Clamp(orbit.tilt, -1.5f, 1.5f);
    view->zoom = prF32Clamp(orbit.zoom, 0.02f, 8);
    view->dirty = true;
}

bool prModelViewPicked(const PrModelView* view, PrModelPick* out) {
    if (!view->clicked)
        return false;
    *out = view->picked;
    return true;
}

uint64_t prModelViewRenders(const PrModelView* view) {
    return view->renders;
}

void prModelViewDeviceLost(PrModelView* view) {
    closeRenderer(view);
    view->dirty = true;
}

PrStudioWindowGpuOutput prModelViewRender(PrModelView* view, const PrStudioWindowRender* render) {
    ModelView* v = view;
    if (v->model == nullptr || render->extent.x == 0 || render->extent.y == 0 || !openRenderer(v, render->device))
        return PrStudioWindowGpuOutput{};
    populate(v);
    double now = nowSeconds();
    if (v->playing && v->animation != kPrModelAnimationNone)
        prModelPlacementAdvance(v->placement, float(now - v->animated));
    v->animated = now;
    PrRenderMaterial highlight = v->selection != kPrModelNodeNone ? v->highlight : PrRenderMaterial{};
    prModelPlacementSetHighlight(v->placement, v->selection, highlight);
    setAssetLight(v);
    prModelUpdate(v->model);
    PrRenderUpdate* update = prRenderUpdateBegin(v->render, v->scene);
    prRenderInstanceSetMasks(update, v->groundInstance, PrRenderMobilityFlags(0),
        v->showsGround ? ~kPrRenderVisibilityShadow : 0);
    setEnvironment(v, update);
    prRenderUpdateCommit(update);
    // The frame draws what loaded before it begins. A part that the model's jobs finish loading after this differs from
    // the progress that the render records, so the next build renders the view again (prModelViewFrame).
    PrModelProgress drawn = prModelProgress(v->model);
    prRenderFrameBegin(v->render, render->graph);
    PrRenderViewDesc desc = prRenderViewDescDefault(v->scene);
    desc.camera = cameraOf(v, float(render->extent.x) / float(render->extent.y));
    desc.extent = render->extent;
    float illuminance = prRenderSceneIlluminance(v->render, v->scene, v->center);
    v->ev100 = chooseEv100(v, prRenderEv100FromIlluminance(illuminance), now);
    desc.exposure = prRenderExposureFromEv100(v->ev100);
    if (v->lighting.exposure == PrModelViewExposure_Image) {
        desc.meter = v->meter;
        ++v->metered;
    }
    PrGpuGraphTexture color = prRenderViewAdd(v->render, &desc).color;

    // The view's texture holds half floats, so that what lib/draw composes keeps the values that the display draw
    // writes. It is made again at another size.
    if (v->texture.id == 0 || v->textureExtent.x != render->extent.x || v->textureExtent.y != render->extent.y) {
        prGpuTextureRelease(v->device, v->texture);
        PrGpuTextureDesc texture = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, render->extent,
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        texture.name = "model view";
        if (prGpuTextureCreate(v->device, &texture, &v->texture) != PrStatus_Ok)
            v->texture = PrGpuTexture{};
        v->textureExtent = render->extent;
    }
    PrGpuGraphTexture shown = prGpuGraphImportTexture(render->graph, v->texture, PrGpuAccessFlag_ShaderRead);
    Shown* draw = static_cast<Shown*>(prGpuGraphAlloc(render->graph, sizeof(Shown)));
    *draw = Shown{v->render, render->graph, color, prRenderDisplayOptionsDefault()};
    draw->display.toneMap = v->toneMap;
    float exposure = v->toneMap == PrRenderToneMap_Neutral ? 1 : kAcesExposure;
    draw->display.exposure = exposure * prF32Exp2(v->lighting.compensation);
    PrGpuGraphPassDesc pass{
        .name = "model view",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = shown, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphPass display = prGpuGraphAddPass(render->graph, &pass, drawShown, draw);
    prGpuGraphUseTexture(render->graph, display, color, PrGpuAccessFlag_FragmentRead);
    prGpuGraphLeaveTexture(render->graph, shown, PrGpuAccessFlag_ShaderRead);
    v->progress = drawn;
    v->dirty = false;
    ++v->renders;
    return PrStudioWindowGpuOutput{.texture = shown, .alpha = PrTextureAlpha_None};
}

void prModelViewFrame(PrModelView* view, PrLayoutNode parent) {
    PrKit* kit = view->kit;
    PrKitBoxStyle column{.place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed}};
    PrLayoutNode box = prKitBox(kit, parent, {}, column);
    // A model that is opened brings its description when its source arrives, which an update takes.
    if (view->model != nullptr)
        prModelUpdate(view->model);
    describeModel(view);
    PrModelProgress progress = view->model != nullptr ? prModelProgress(view->model) : PrModelProgress{};
    if (view->model == nullptr || progress.source != PrModelLoad_Ready) {
        PrKitTextStyle muted{.role = PrKitTextRole_BodySmall, .tone = PrKitTone_Muted};
        PrStringView why{};
        if (view->model != nullptr && progress.source == PrModelLoad_Failed)
            why = progress.sourceStatus == PrStatus_NotSupported ? "No format of the program reads the asset." :
                "The asset does not open as a model.";
        else if (view->model != nullptr)
            why = "Opening the asset.";
        prKitLabel(kit, box, {.text = why}, muted);
        if (view->model != nullptr && progress.source == PrModelLoad_Pending && view->gpuView != nullptr)
            prStudioWindowGpuViewInvalidate(view->gpuView);
        return;
    }
    buildBar(view, box);
    // The asset takes the room under the bar, which layout gives it, and renders at that size.
    PrKitPlace place{.width = prKitPercent(100), .flex = PrKitFlex_Fill};
    if (view->gpuView == nullptr)
        return;
    PrLayoutNode shown = prKitEmbed(kit, box, {.embed = prStudioWindowGpuViewEmbed(view->gpuView)}, place);
    PrUi* ui = kit->ui;
    prUiSetFlags(ui, shown, PrUiFlag_Pressable | PrUiFlag_Wheel);
    // A drag turns the orbit camera, and the wheel moves it nearer or farther. A press that is released before it moves
    // past kClickSlop is a click, which picks what shows under it and selects its node.
    view->clicked = false;
    PrUiDrag drag{};
    bool dragging = prUiDrag(ui, shown, 0, kClickSlop, &drag);
    if (dragging && drag.moved && (drag.delta.x != 0 || drag.delta.y != 0)) {
        view->camera = 0;
        view->turn -= drag.delta.x * 0.008f;
        view->tilt = prF32Clamp(view->tilt + drag.delta.y * 0.006f, -1.5f, 1.5f);
        view->dirty = true;
    }
    PrVec2 local{};
    if (dragging && drag.released && !drag.moved && !drag.cancelled && prUiLocalPoint(ui, shown, drag.origin, &local)) {
        // The click is on the asset as the last layout showed it, whose size the committed tree holds.
        PrVec2 extent = prLayoutVecToPx(prLayoutNodeContentExtent(kit->tree, shown));
        view->picked = pickAt(view, local, extent);
        view->clicked = true;
        prModelViewSetSelection(view, view->picked.node);
    }
    PrVec2 wheel{};
    if (prUiWheel(ui, shown, &wheel) && wheel.y != 0) {
        view->camera = 0;
        view->zoom = prF32Clamp(view->zoom * prF32Exp(-wheel.y * 0.002f), 0.02f, 8);
        view->dirty = true;
    }
    // A change, a part of the asset that loads, or the next moment of an animation renders the view again.
    if (view->dirty || (view->playing && view->animation != kPrModelAnimationNone) || !prModelIsLoaded(view->model) ||
            !sameProgress(prModelProgress(view->model), view->progress) || imageAdapting(view))
        prStudioWindowGpuViewInvalidate(view->gpuView);
}
