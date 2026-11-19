// The model viewer: a browser of glTF files, each drawn by lib/render, through lib/model, with lib/draw's text over it
// in one frame.
//
// From a file's path to a lit model in a window takes three steps. A stage (viewer_stage.h) reads the file and makes a
// model and a placement of it, then populates a renderer with the model and puts the placement in a scene, and addView
// here draws the scene each frame. The list (viewer_list.h) finds the files, and the overlay (viewer_overlay.h) says
// where the viewer is among them. The rest of this file is the window: its device, its camera and its keys.
//
// No step makes a frame wait. A thread reads the file of the model that the user chose while the window draws the model
// before it, and the new model takes its place once its meshes have loaded. Its textures appear as they arrive, and the
// overlay shows how far it has come. A model that the user steps past before it arrives is abandoned, so holding a key
// down runs through the list. A lost device costs the window its renderer and nothing else: the stages enter a renderer
// on the new device, and the placement keeps its animation.
//
// Left and Right step to the model before and after, Page Up and Page Down step by ten, and Home and End go to the ends
// of the list. Tab lists the models around the current one. A model file or a directory dropped on the window joins the
// list and is shown. Dragging turns the camera about the model, and scrolling moves it nearer or farther. C steps
// through the orbit camera and the model's own cameras, A through the model's animations, T between the tone maps,
// Space pauses, L loses the device, and Escape quits.
//
// Usage: model_viewer PATH... [--model NAME] [--list] [--slots] [--visibility] [--environment FILE] [--tone-map NAME]
// [--exposure FACTOR] [--animation N] [--time SECONDS] [--camera N] [--turn RADIANS] [--tilt RADIANS] [--zoom FACTOR]
// [--lose-device] [--capture FILE] [--cycle PASSES] [--skim] [--stats].
//
// Each PATH is a .gltf file, whose other files are read from beside it, a .glb file, or a directory, which gives every
// model file under it. A directory that holds the variants of one model, as each of Khronos's sample assets does, gives
// one of them (viewer_list.h). The list is sorted by name. --model starts at the model called NAME, or at the model
// whose number in the list NAME is, and not at the first. --list shows the list of models, as Tab does.
//
// --slots creates a device that uses the slots alone. --visibility draws with the visibility technique. --environment
// lights the models with an equirectangular Radiance picture (.hdr), in place of the sky and its sun. --tone-map shows
// the view by the tone map that NAME names (prRenderToneMapFromName), and not by the ACES curve on luminance.
// --exposure scales the view's color by FACTOR before the tone map, and not by the factor that suits the tone map.
// --animation plays each model's animation N, from zero, and not its first. --time shows that animation at SECONDS,
// paused. --camera looks through each model's camera N, from zero. --turn, --tilt and --zoom place the orbit camera
// where each model starts: its turn about the y axis, its tilt above the model's center, and its distance as a multiple
// of the distance at which the model fills the view, which is 1. --lose-device loses the device once, when the first
// model has loaded, as L does. --capture writes a frame to FILE as a PNG once the first model has loaded, and exits.
//
// --cycle steps through the list PASSES times without a user: it waits for each model to load, draws a few frames of
// it, and goes on. After each pass it frees the last model and prints how many objects the renderer still holds, and
// how many allocations the process does. It exits with a failure if the renderer holds more after a later pass than
// after the first. With --cycle, --lose-device loses the device halfway through the first pass, as the next model
// loads, and --skim steps on every frame without waiting, as a user who holds Right down does, so that all but the last
// model of a pass are abandoned on their way.
//
// --stats prints, every couple of seconds, the frames' average times on the CPU and the GPU, and the pipelines that the
// renderer has made.

#include "pr/app.h"
#include "pr/app_select.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/compress_select.h"
#include "pr/draw_list.h"
#include "pr/draw_renderer.h"
#include "pr/draw_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/format.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/integer.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/min_max.h"
#include "pr/model.h"
#include "pr/model_host.h"
#include "pr/print.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/task.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/type_traits.h"

#include "examples/frame_stats.h"
#include "examples/model_viewer/viewer_list.h"
#include "examples/model_viewer/viewer_overlay.h"
#include "examples/model_viewer/viewer_stage.h"

namespace {

using namespace frame_stats;
using namespace model_viewer;
using namespace pr;

// The camera's vertical field of view, in radians.
constexpr float kFov = 0.7f;

// Where the orbit camera starts for each model, unless the options say: its turn about the y axis and its tilt above
// the model's center, in radians.
constexpr float kTurn = 0.25f;
constexpr float kTilt = 0.2f;

// The direction toward the light that casts the model's shadow on the ground, which is the sun of the scene's sky.
constexpr PrVec3 kToLight{0.6f, 0.9f, 0.7f};

// What the window shows where it draws no model.
constexpr PrColor kEmptyColor{0.07f, 0.08f, 0.1f, 1};

// The exposure at which the ACES curve shows middle gray as middle gray. At an exposure of 1 the curve shows it a stop
// darker, as ACES's reference rendering does. The neutral tone map keeps middle gray at an exposure of 1.
constexpr float kAcesExposure = 1.45f;

// The number of frames that the window draws with the model loaded before --capture writes one, so that the swap chain
// and the renderer have warmed up.
constexpr uint32_t kCaptureFrames = 3;

// The number of frames that --cycle draws of each loaded model, and the number that it draws after it has freed the
// last model of a pass, so that the renderer applies every destruction and the device frees what the frames in flight
// held.
constexpr uint32_t kCycleFrames = 3;
constexpr uint32_t kCycleSettleFrames = 30;

// The seconds that a notice stays in the overlay.
constexpr double kNoticeSeconds = 4;

// The unattended run through the list that --cycle asks for.
struct Cycle {
    uint32_t passes = 0;        // the passes to make, or zero without --cycle
    bool skims = false;         // the cycle steps on every frame, and waits only for the last model of a pass
    uint32_t pass = 0;          // the passes made
    uint32_t frames = 0;        // the frames drawn of the current model since it loaded, or since the pass's end
    uint32_t unshown = 0;       // the models of this pass that could not be shown
    bool settling = false;      // the pass's last model has been freed, and frames are letting the renderer settle
    PrRenderInfo first{};       // what the renderer held after the first pass
    bool grew = false;          // the renderer held more after a later pass
};

// The browser's window, and the device that it renders on.
struct Window {
    bool slotsOnly = false;
    PrRenderTechnique technique = PrRenderTechnique_Forward;
    PrRenderToneMap toneMap = PrRenderToneMap_AcesLuminance;
    double exposure = 0;        // the factor that --exposure gives, or 0 for the factor that suits the tone map
    PrStringView capturePath{};
    PrStringView environmentPath{};     // the picture that --environment names, or empty for the sky
    bool losesDevice = false;           // the window has yet to lose its device, as --lose-device asks
    uint32_t animation = 0;             // the animation that each model plays, if it has it
    uint32_t modelCamera = UINT32_MAX;  // the camera of its own that each model is seen through, if it has it
    double seconds = -1;                // the time that each model's animation is paused at, or negative to play it
    Cycle cycle;

    // The list, and the entry that the user chose. `shown` is the stage that the frame draws, or null before the first
    // has loaded. `next` is the stage of the current entry while it is on its way, and null once it is shown. The
    // stages last through a lost device, and their scenes are made again in the new renderer.
    Array<ModelEntry> models;
    uint32_t current = 0;
    Loader loader;
    Stage* shown = nullptr;
    Stage* next = nullptr;
    double chosenAt = 0;                // when the user chose the current entry, in seconds
    bool paused = false;
    bool listsModels = false;
    String notice;                      // a sentence that the overlay shows until `noticeUntil`
    double noticeUntil = 0;

    // The camera: the orbit camera, which is at a place on a sphere about the model, or one of the model's own.
    uint32_t camera = 0;        // 0 for the orbit camera, or 1 more than the index of one of the model's cameras
    float turn = kTurn;
    float tilt = kTilt;
    float zoom = 1;             // the distance, as a multiple of the distance at which the sphere fills the view
    float firstTurn = kTurn;    // where the orbit camera starts for each model
    float firstTilt = kTilt;
    float firstZoom = 1;
    bool dragging = false;

    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrCompressCodecSet* compress = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrRender* render = nullptr;
    PrGpuGraph* graph = nullptr;        // the frame's passes, built again each frame

    // What lights every stage's scene in the renderer: an environment, and the sun if the environment is the sky.
    PrRenderSceneEnvironment lit{};
    PrRenderLightDesc sun{};
    bool hasSun = false;

    Overlay overlay;
    PrDrawRenderer* drawRenderer = nullptr;
    PrDrawFrame* drawFrame = nullptr;

    uint32_t settledFrames = 0; // the frames drawn with the current model shown and loaded
    bool printsStats = false;
    FrameStats frameStats;      // what --stats reports
    Atomic<bool> captured;      // set on the device's thread once the capture is written
};

double now() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// ---------------------------------------------------------------------------------------------------------------------
// The models

// Makes the entry at `index` the current one, and starts loading it, unless it is the model shown. The model that was
// on its way is abandoned.
void choose(Window& w, uint32_t index) {
    w.current = index;
    w.settledFrames = 0;
    stageDestroy(w.next);
    w.next = nullptr;
    PrStringView path = w.models[index].path;
    if (w.shown != nullptr && w.shown->path == path)
        return;
    w.next = stageCreate(w.loader, path, w.animation);
    w.chosenAt = now();
}

// Chooses the entry `delta` after the current one, or the end of the list that lies before it.
void step(Window& w, int64_t delta) {
    int64_t last = int64_t(w.models.count()) - 1;
    int64_t index = clamp(int64_t(w.current) + delta, int64_t(0), last);
    if (uint32_t(index) != w.current)
        choose(w, uint32_t(index));
}

// Shows the model that is on its way, once it is worth showing in place of the one before it. The stage enters the
// renderer as soon as its file is read, and its model loads while the frames draw the stage that is shown. The camera
// and the animation start again for the new model.
void showNext(Window& w) {
    if (w.next == nullptr || !w.next->isRead.load())
        return;
    if (!stageIsDrawn(*w.next))
        stageEnter(*w.next, w.render, w.lit, w.hasSun ? &w.sun : nullptr);
    if (!stageHasShape(*w.next))
        return;
    stageDestroy(w.shown);
    w.shown = w.next;
    w.next = nullptr;
    w.settledFrames = 0;
    w.camera = 0;
    w.turn = w.firstTurn;
    w.tilt = w.firstTilt;
    w.zoom = w.firstZoom;
    w.paused = false;
    if (w.shown->model == nullptr)
        return;
    if (w.modelCamera < prModelCameraCount(w.shown->model))
        w.camera = w.modelCamera + 1;
    if (w.seconds >= 0) {
        prModelPlacementSetTime(w.shown->placement, float(w.seconds));
        w.paused = true;
    }
}

// Returns true if the frame draws the current model and nothing of it is loading.
bool isSettled(const Window& w) {
    return w.next == nullptr && w.shown != nullptr && stageIsLoaded(*w.shown);
}

// Returns the window's camera for a target of `extent`.
PrRenderCamera cameraOf(Window& w, PrUVec2 extent) {
    const Stage& stage = *w.shown;
    float aspect = float(extent.x) / float(extent.y);
    if (w.camera != 0)
        return prModelPlacementCamera(stage.placement, w.camera - 1, aspect);
    PrVec3 direction{prF32Cos(w.tilt) * prF32Sin(w.turn), prF32Sin(w.tilt), prF32Cos(w.tilt) * prF32Cos(w.turn)};
    float distance = stage.radius / prF32Sin(kFov * 0.5f) * w.zoom;
    return prRenderCameraLookAt(stage.center + direction * distance, stage.center, kFov, aspect, distance * 0.01f);
}

// Moves the animation of the model shown by `delta` seconds, and writes its pose to the scene. Called before the
// renderer's frame begins, which is when the pose takes effect.
void updateModel(Window& w, float delta) {
    if (!w.paused)
        prModelPlacementAdvance(w.shown->placement, delta);
    prModelUpdate(w.shown->model);
}

// Adds the passes that draw the scene of the model shown to the frame's graph. Returns the texture that will hold the
// view's color. The view is exposed as an incident-light meter at the model's center reads what lights it, so a white
// surface without gloss that faces the brightest of that light shows as white.
PrGpuGraphTexture addView(Window& w, PrUVec2 extent) {
    PrRenderViewDesc view = prRenderViewDescDefault(w.shown->scene);
    view.camera = cameraOf(w, extent);
    view.extent = extent;
    float illuminance = prRenderSceneIlluminance(w.render, w.shown->scene, w.shown->center);
    view.exposure = prRenderExposureFromEv100(prRenderEv100FromIlluminance(illuminance));
    view.technique = w.technique;
    return prRenderViewAdd(w.render, &view).color;
}

// Shows `text` in the overlay for a few seconds.
void notify(Window& w, PrStringView text) {
    w.notice = text;
    w.noticeUntil = now() + kNoticeSeconds;
}

// Adds the files and the directories that were dropped on the window to the list, and chooses the first model among
// them.
void addDropped(void* PR_NULLABLE context, PrStatus status, const PrAppDataFile* PR_NULLABLE files, uint32_t count) {
    Window& w = *static_cast<Window*>(context);
    if (status != PrStatus_Ok)
        count = 0;
    // An entry's index changes as the list takes others before it, so the entries are followed by their paths.
    String current = w.models[w.current].path;
    String first;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t index = modelListAdd(w.models, files[i].path);
        if (index != kModelNone && first.isEmpty())
            first = w.models[index].path;
    }
    w.current = modelListFindPath(w.models, current);
    if (first.isEmpty()) {
        notify(w, "Nothing that was dropped is a glTF or USD file, or a directory that holds one");
        return;
    }
    uint32_t chosen = modelListFindPath(w.models, first);
    if (chosen != w.current)
        choose(w, chosen);
}

// ---------------------------------------------------------------------------------------------------------------------
// The device and the frame

// Add a pass or work to a graph, whose callback is `draw` or `record`: a function object that the graph's memory holds
// for the frame. What it captures is copied as bytes, and must outlive the graph's recording.
template <typename Draw>
PrGpuGraphPass graphAddPass(PrGpuGraph* graph, const PrGpuGraphPassDesc& desc, const Draw& draw) {
    static_assert(isTriviallyCopyable<Draw> && alignof(Draw) <= 16);
    void* context = prGpuGraphAlloc(graph, sizeof(Draw));
    prMemCopy(context, &draw, sizeof(Draw));
    return prGpuGraphAddPass(graph, &desc, [](void* PR_NULLABLE held, PrGpuPass* pass) {
        (*static_cast<const Draw*>(held))(pass);
    }, context);
}
template <typename Record>
PrGpuGraphPass graphAddWork(PrGpuGraph* graph, PrStringView name, const Record& record) {
    static_assert(isTriviallyCopyable<Record> && alignof(Record) <= 16);
    void* context = prGpuGraphAlloc(graph, sizeof(Record));
    prMemCopy(context, &record, sizeof(Record));
    return prGpuGraphAddWork(graph, name, [](void* PR_NULLABLE held, PrGpuCommands* commands) {
        (*static_cast<const Record*>(held))(commands);
    }, context);
}

// Sets an environment's light from the equirectangular Radiance picture at `path`. Returns false, with a message, if
// the file is not such a picture.
bool setEnvironmentFromFile(Window& w, PrRenderEnvironment environment, PrStringView path) {
    PrBlob* file = prFileOpen(path);
    if (file == nullptr) {
        printError("model_viewer: could not read {}\n", path);
        return false;
    }
    PrImageDecoder* decoder = prImageDecoderCreate(w.loader.images);
    PrImageSource source{};
    source.memory = prBlobBytes(file);
    bool set = false;
    if (prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok) {
        const PrImageInfo& info = *prImageDecoderInfo(decoder);
        // A picture of radiance: red, green and blue floats in linear light.
        if (info.format.model == PrColorModel_Rgb && info.format.sample == PrImageSample_F32 &&
                info.format.alpha == PrImageAlpha_None) {
            Array<float> pixels(info.extent.x * info.extent.y * 3);
            size_t stride = size_t(info.extent.x) * 3 * sizeof(float);
            PrPixels output{reinterpret_cast<uint8_t*>(pixels.begin()), stride, info.extent};
            if (prImageDecoderRead(decoder, 0, output) == PrStatus_Ok) {
                PrRenderRadianceImage image{.pixels = {output.data, stride, info.extent}, .channels = 3};
                set = prRenderEnvironmentSetEquirect(w.render, environment, &image) == PrStatus_Ok;
            }
        }
    }
    prImageDecoderDestroy(decoder);
    prBlobRelease(file);
    if (!set)
        printError("model_viewer: {} is not a Radiance picture that the viewer can light with\n", path);
    return set;
}

// Makes what lights every stage's scene in the window's renderer, which is new. The environment is the picture that
// --environment names, which holds its own lights, or else a sky, whose sun is each scene's light and casts its
// shadows.
void lightScenes(Window& w) {
    PrRenderEnvironment environment = prRenderEnvironmentCreate(w.render);
    w.lit = prRenderSceneEnvironmentDefault(environment);
    w.hasSun = false;
    if (w.environmentPath.size != 0 && setEnvironmentFromFile(w, environment, w.environmentPath))
        return;
    PrRenderSkyDesc sky = stageSky(kToLight);
    prRenderEnvironmentSetSky(w.render, environment, &sky);
    w.sun = prRenderLightDescDefault();
    w.sun.direction = -kToLight;
    w.sun.color = prRenderSkySunIrradiance(&sky);
    w.sun.castsShadows = true;
    w.hasSun = true;
}

// Frees what the window made on its device, and the device. The stages leave the renderer first, once the loader has
// freed the stages that were in it, and keep everything that is not on the device.
void closeDevice(Window& w) {
    loaderWait(w.loader);
    if (w.shown != nullptr)
        stageLeave(*w.shown);
    if (w.next != nullptr)
        stageLeave(*w.next);
    prDrawFrameDestroy(w.drawFrame);
    prDrawRendererDestroy(w.drawRenderer);
    prRenderDestroy(w.render);
    prGpuGraphDestroy(w.graph);
    prGpuSwapChainDestroy(w.swapChain);
    prGpuDeviceDestroy(w.device);
    w.drawFrame = nullptr;
    w.drawRenderer = nullptr;
    w.render = nullptr;
    w.graph = nullptr;
    w.swapChain = nullptr;
    w.device = nullptr;
    w.settledFrames = 0;
}

// Creates the device on the pool, if the window has none.
void requestDevice(Window& w) {
    if (w.device != nullptr)
        return;
    PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
    options.pool = w.loader.pool;
    options.slotsOnly = w.slotsOnly;
    w.device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
}

// Returns true if the window can render. Frees a lost device, which includes one whose creation failed, with everything
// made on it, and creates another. Makes the swap chain and the renderers once the device is ready, and puts the stage
// that is shown in the new renderer.
bool openDevice(Window& w) {
    if (w.device != nullptr && prGpuDeviceIsLost(w.device)) {
        if (w.swapChain == nullptr)
            printError("model_viewer: no GPU device ({})\n", prStatusName(prGpuDeviceLostStatus(w.device)));
        closeDevice(w);
    }
    requestDevice(w);
    if (w.device == nullptr || !prGpuDeviceIsReady(w.device) || prGpuDeviceIsLost(w.device))
        return false;
    if (w.swapChain != nullptr)
        return true;
    PrSurface surface{};
    PrGpuSwapChainDesc desc{.intent = PrGpuSwapChainIntent_Sdr};
    bool opened = prAppWindowSurface(w.window, &surface) == PrStatus_Ok &&
        prGpuSwapChainCreate(w.device, &surface, &desc, &w.swapChain) == PrStatus_Ok &&
        prDrawRendererCreate(PrDrawRendererBackend_Default, w.device, nullptr, &w.drawRenderer) == PrStatus_Ok;
    if (opened)
        opened = prRenderCreate(w.device, nullptr, &w.render) == PrStatus_Ok;
    if (!opened) {
        printError("model_viewer: the device cannot render to the window\n");
        closeDevice(w);
        // A device that cannot render is not created again, so that the failure is reported once.
        prAppStop(w.app);
        return false;
    }
    w.drawFrame = prDrawFrameCreate(w.drawRenderer);
    w.graph = prGpuGraphCreate(w.device, nullptr);
    lightScenes(w);
    if (w.shown != nullptr)
        stageEnter(*w.shown, w.render, w.lit, w.hasSun ? &w.sun : nullptr);
    PrGpuDeviceInfo info = prGpuDeviceInfo(w.device);
    PrStringView binding = (info.features & PrGpuFeatureFlag_Bindless) != 0 ? "handles" : "slots";
    print("model_viewer: rendering on {} ({}), with {}\n", info.backend, info.adapter, binding);
    return true;
}

// Writes the texels of the frame that the swap chain captured, which are BGRA, to the capture's file as a PNG. Called
// on the device's thread once the frame's work is done.
void writeCapture(void* PR_NULLABLE context, PrPixelView texels, PrGpuFormat) {
    Window& w = *static_cast<Window*>(context);
    Array<uint8_t> rgba(size_t(texels.extent.x) * texels.extent.y * 4);
    for (uint32_t y = 0; y < texels.extent.y; ++y) {
        const uint8_t* from = texels.data + y * texels.stride;
        uint8_t* to = rgba.begin() + size_t(y) * texels.extent.x * 4;
        for (uint32_t x = 0; x < texels.extent.x; ++x, from += 4, to += 4) {
            to[0] = from[2];
            to[1] = from[1];
            to[2] = from[0];
            to[3] = 255;
        }
    }
    PrPixelView pixels{rgba.begin(), size_t(texels.extent.x) * 4, texels.extent};
    size_t size = 0;
    prImageCodecSetEncode(w.loader.images, "image/png", pixels, nullptr, 0, &size);
    Array<uint8_t> png(size);
    bool written = prImageCodecSetEncode(w.loader.images, "image/png", pixels, png.begin(), size, &size) == PrStatus_Ok &&
        prFileWrite(w.capturePath, PrByteView{png.begin(), size});
    if (!written)
        printError("model_viewer: could not write {}\n", w.capturePath);
    w.captured.store(true);
}

// Renders a frame into the swap chain's texture at `extent` target pixels and presents it.
void render(Window& w, PrUVec2 extent, float delta) {
    PrTimePerf began = prTimePerfNow();
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    // The frame that the capture takes draws into the swap chain's own texture, and is not presented.
    if (w.capturePath.size != 0 && w.settledFrames == kCaptureFrames)
        prGpuSwapChainRequestCapture(w.swapChain, writeCapture, &w);
    if (prGpuSwapChainAcquire(w.swapChain, extent, &frame) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuTexture drawable = frame.color;
    PrPixelFormat format{};
    prGpuFormatToPixelFormat(prGpuSwapChainInfo(w.swapChain).format, &format);

    // The window draws a scene unless no model has loaded yet, or the one that it shows has none.
    bool drawsScene = w.shown != nullptr && stageIsDrawn(*w.shown);
    prGpuGraphBegin(w.graph);
    if (drawsScene)
        updateModel(w, delta);
    prRenderFrameBegin(w.render, w.graph);
    PrGpuGraphTexture sceneColor{};
    if (drawsScene)
        sceneColor = addView(w, extent);
    // The 2D prepares its own textures before the pass that it draws in begins.
    graphAddWork(w.graph, "prepare 2D", [&w, extent, format](PrGpuCommands* recording) {
        PrDrawTarget target{.extent = extent, .format = format, .commands = recording};
        prDrawFramePrepare(w.drawFrame, w.overlay.list, &target);
    });
    PrGpuGraphTexture target = prGpuGraphImportTexture(w.graph, drawable, PrGpuAccessFlag_Discard);
    prGpuGraphLeaveTexture(w.graph, target, PrGpuAccessFlag_Present);
    PrGpuGraphPassDesc desc{
        .name = "display",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = target, .load = PrGpuLoad_Clear, .store = PrGpuStore_Keep}},
        .clearColor = {kEmptyColor},
    };
    // The view's color is drawn tone mapped, and the overlay over it, in one pass.
    PrGpuGraphPass display = graphAddPass(w.graph, desc, [&w, sceneColor, drawsScene](PrGpuPass* pass) {
        PrRenderDisplayOptions shownAs = prRenderDisplayOptionsDefault();
        shownAs.toneMap = w.toneMap;
        shownAs.exposure = w.toneMap == PrRenderToneMap_Neutral ? 1 : kAcesExposure;
        if (w.exposure > 0)
            shownAs.exposure = float(w.exposure);
        if (drawsScene)
            prRenderDisplayDraw(w.render, pass, prGpuGraphTexture(w.graph, sceneColor), &shownAs);
        prDrawFrameRender(w.drawFrame, pass, PrIVec2{0, 0});
    });
    if (drawsScene)
        prGpuGraphUseTexture(w.graph, display, sceneColor, PrGpuAccessFlag_FragmentRead);
    prGpuGraphRecord(w.graph, commands);
    prGpuSwapChainPresent(w.swapChain, commands);
    if (w.printsStats)
        frameStatsSubmit(w.frameStats, commands, prTimePerfMilliseconds(began, prTimePerfNow()));
    prGpuCommandsSubmit(commands);
    if (isSettled(w))
        ++w.settledFrames;
    if (w.losesDevice && w.cycle.passes == 0 && w.settledFrames != 0) {
        // The next frame finds the device lost, and the model loads again on a new one before a capture.
        w.losesDevice = false;
        w.settledFrames = 0;
        prGpuDeviceSimulateLoss(w.device);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The unattended run

// Ends a pass of the cycle once the frames after it have let the renderer settle: prints what the renderer and the
// process still hold, and begins the next pass, or stops the application after the last.
void endPass(Window& w) {
    Cycle& c = w.cycle;
    if (w.loader.jobs.load() != 0) {
        c.frames = 0;
        return;
    }
    if (++c.frames == kCycleSettleFrames) {
        // The device frees what the frames before this one held, and the allocator gives back what it has free, so that
        // the process's memory is what the viewer holds at rest.
        prGpuDeviceWaitIdle(w.device);
        prMemoryTrim();
        PrRenderInfo info = prRenderInfo(w.render);
        print("model_viewer: pass {} of {}: {} models, {} not shown; the renderer holds {} meshes, {} textures, {} "
            "materials, {} environments and {} scenes, and the process {} allocations\n", c.pass + 1, c.passes,
            w.models.count(), c.unshown, info.meshCount, info.textureCount, info.materialCount, info.environmentCount,
            info.sceneCount, prMemoryLiveCount());
        if (c.pass == 0)
            c.first = info;
        c.grew = c.grew || info.meshCount > c.first.meshCount || info.textureCount > c.first.textureCount ||
            info.materialCount > c.first.materialCount || info.environmentCount > c.first.environmentCount ||
            info.sceneCount > c.first.sceneCount;
    }
    // The frames after the line give a tool that watches the process time to read its memory at rest.
    if (c.frames < 2 * kCycleSettleFrames)
        return;
    c.settling = false;
    c.frames = 0;
    c.unshown = 0;
    if (++c.pass == c.passes) {
        if (c.grew)
            printError("model_viewer: the renderer holds more than it did after the first pass\n");
        prAppStop(w.app);
        return;
    }
    choose(w, 0);
}

// Moves the cycle on by one frame: goes to the next model once the current one has loaded and been drawn, and frees the
// last model of a pass.
void cycle(Window& w) {
    Cycle& c = w.cycle;
    if (c.settling) {
        endPass(w);
        return;
    }
    if (c.skims && w.current + 1 < w.models.count()) {
        choose(w, w.current + 1);
        return;
    }
    if (!isSettled(w) || ++c.frames < kCycleFrames)
        return;
    c.frames = 0;
    const Stage& stage = *w.shown;
    PrStringView name = w.models[w.current].name;
    if (stage.model == nullptr) {
        ++c.unshown;
        if (c.pass == 0)
            print("model_viewer: {} cannot be shown: {}\n", name, stage.reason);
    } else if (c.pass == 0) {
        PrModelProgress progress = prModelProgress(stage.model);
        if (progress.meshesFailed + progress.imagesFailed != 0)
            print("model_viewer: {}: {} meshes and {} images failed to load\n", name, progress.meshesFailed,
                progress.imagesFailed);
    }
    if (w.current + 1 < w.models.count()) {
        choose(w, w.current + 1);
        if (w.losesDevice && w.current == w.models.count() / 2) {
            // The device is lost while the next model is read, and the frames find it lost with a model shown.
            w.losesDevice = false;
            print("model_viewer: losing the device at {}\n", w.models[w.current].name);
            prGpuDeviceSimulateLoss(w.device);
        }
        return;
    }
    stageDestroy(w.shown);
    w.shown = nullptr;
    c.settling = true;
}

// ---------------------------------------------------------------------------------------------------------------------
// The window

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    if (w.captured.load()) {
        prAppStop(w.app);
        return PrAppOutput{};
    }
    // Frames continue while the device is being created. The window takes a copy of what is dropped on it.
    if (!openDevice(w)) {
        return PrAppOutput{
            .wantsFrame = w.device != nullptr && !prGpuDeviceIsReady(w.device),
            .dropOperation = PrAppDragOperation_Copy,
        };
    }
    prGpuSwapChainWait(w.swapChain);
    showNext(w);
    if (w.cycle.passes != 0)
        cycle(w);
    double time = now();
    OverlayContent content{
        .models = &w.models,
        .current = w.current,
        .shown = w.shown,
        .next = w.next,
        .waited = time - w.chosenAt,
        .paused = w.paused,
        .listsModels = w.listsModels,
        .extent = f->extent,
        .pixelRatio = f->display.pixelRatio,
    };
    if (time < w.noticeUntil)
        content.notice = w.notice;
    overlayRecord(w.overlay, content);
    float ratio = f->display.pixelRatio;
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent, float(f->delta));
    if (w.printsStats)
        frameStatsReport(w.frameStats, "model_viewer", w.render);
    return PrAppOutput{.wantsFrame = true, .dropOperation = PrAppDragOperation_Copy};
}

void keyDown(Window& w, uint32_t key) {
    // The keys of the list, and then the keys of the model shown, if the stage that is shown has one.
    if (key == PrAppKey_Left) {
        step(w, -1);
    } else if (key == PrAppKey_Right) {
        step(w, 1);
    } else if (key == PrAppKey_PageUp) {
        step(w, -10);
    } else if (key == PrAppKey_PageDown) {
        step(w, 10);
    } else if (key == PrAppKey_Home) {
        step(w, -int64_t(w.models.count()));
    } else if (key == PrAppKey_End) {
        step(w, int64_t(w.models.count()));
    } else if (key == PrAppKey_Tab) {
        w.listsModels = !w.listsModels;
    } else if (key == 't' || key == 'T') {
        w.toneMap = PrRenderToneMap((w.toneMap + 1) % PrRenderToneMap_Count);
    } else if ((key == 'l' || key == 'L') && w.device != nullptr) {
        prGpuDeviceSimulateLoss(w.device);
    } else if (key == PrAppKey_Escape) {
        prAppStop(w.app);
    }
    if (w.shown == nullptr || w.shown->model == nullptr)
        return;
    const Stage& stage = *w.shown;
    uint32_t animations = prModelAnimationCount(stage.model);
    if (key == ' ') {
        w.paused = !w.paused;
    } else if (key == 'c' || key == 'C') {
        w.camera = (w.camera + 1) % (prModelCameraCount(stage.model) + 1);
    } else if ((key == 'a' || key == 'A') && animations != 0) {
        PrModelAnimationIndex following = (prModelPlacementAnimation(stage.placement) + 1) % animations;
        prModelPlacementSetAnimation(stage.placement, following, PrModelLoop_Repeat);
    }
}

void event(void* context, PrAppWindow*, const PrAppEvent* e) {
    Window& w = *static_cast<Window*>(context);
    switch (e->kind) {
        case PrAppEventKind_CloseRequested:
            prAppStop(w.app);
            break;
        case PrAppEventKind_PointerDown:
        case PrAppEventKind_PointerUp:
            if (e->pointer.button == 0)
                w.dragging = e->kind == PrAppEventKind_PointerDown;
            break;
        case PrAppEventKind_PointerMove:
            if (w.dragging) {
                w.turn -= e->pointer.movement.x * 0.008f;
                w.tilt = prF32Clamp(w.tilt + e->pointer.movement.y * 0.006f, -1.5f, 1.5f);
            }
            break;
        case PrAppEventKind_Scroll:
            w.zoom = prF32Clamp(w.zoom * prF32Exp(-prAppEventScrollPixels(e, 16).y * 0.002f), 0.02f, 8);
            break;
        case PrAppEventKind_KeyDown:
            keyDown(w, e->keyboard.key);
            break;
        case PrAppEventKind_Drop:
            // The files are read after the event, and the read keeps what was dropped until then.
            if (w.cycle.passes == 0)
                prAppDataReadFiles(w.app, e->drag.drop, addDropped, &w);
            break;
        default:
            break;
    }
}

// Opens the window. The window renders once the device is ready, and its close stops the application.
bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Model Viewer",
        .extent = {960, 640},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    return prAppWindowCreate(app, &desc, &w.window) == PrStatus_Ok;
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

// Frees the stages, and then the device, which waits until the loader has freed them.
void shutdown(void* context, PrApp*) {
    Window& w = *static_cast<Window*>(context);
    stageDestroy(w.shown);
    stageDestroy(w.next);
    w.shown = nullptr;
    w.next = nullptr;
    closeDevice(w);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Window w;
    uint64_t animation = 0;
    uint64_t camera = UINT64_MAX;
    uint64_t passes = 0;
    double turn = double(kTurn);
    double tilt = double(kTilt);
    double zoom = 1;
    PrStringView model{};
    Array<PrStringView> paths;
    bool usage = false;
    for (int i = 1; i < argc && !usage; ++i) {
        PrStringView argument = argv[i];
        bool hasValue = i + 1 < argc;
        if (argument == "--slots") {
            w.slotsOnly = true;
        } else if (argument == "--visibility") {
            w.technique = PrRenderTechnique_Visibility;
        } else if (argument == "--environment" && hasValue) {
            w.environmentPath = argv[++i];
        } else if (argument == "--animation" && hasValue) {
            PrStringView number = argv[++i];
            usage = !prU64Parse(number.data, number.size, &animation, 10);
        } else if (argument == "--time" && hasValue) {
            PrStringView number = argv[++i];
            usage = prF64ParsePrefix(number, &w.seconds) != number.size;
        } else if (argument == "--camera" && hasValue) {
            PrStringView number = argv[++i];
            usage = !prU64Parse(number.data, number.size, &camera, 10);
        } else if (argument == "--turn" && hasValue) {
            PrStringView number = argv[++i];
            usage = prF64ParsePrefix(number, &turn) != number.size;
        } else if (argument == "--tilt" && hasValue) {
            PrStringView number = argv[++i];
            usage = prF64ParsePrefix(number, &tilt) != number.size;
        } else if (argument == "--zoom" && hasValue) {
            PrStringView number = argv[++i];
            usage = prF64ParsePrefix(number, &zoom) != number.size;
        } else if (argument == "--tone-map" && hasValue) {
            w.toneMap = prRenderToneMapFromName(argv[++i]);
            if (w.toneMap == PrRenderToneMap_Count) {
                printError("model_viewer: no tone map named {}\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument == "--exposure" && hasValue) {
            PrStringView number = argv[++i];
            usage = prF64ParsePrefix(number, &w.exposure) != number.size || !(w.exposure > 0);
        } else if (argument == "--model" && hasValue) {
            model = argv[++i];
        } else if (argument == "--list") {
            w.listsModels = true;
        } else if (argument == "--cycle" && hasValue) {
            PrStringView number = argv[++i];
            usage = !prU64Parse(number.data, number.size, &passes, 10) || passes == 0 || passes > 1000;
        } else if (argument == "--skim") {
            w.cycle.skims = true;
        } else if (argument == "--lose-device") {
            w.losesDevice = true;
        } else if (argument == "--stats") {
            w.printsStats = true;
        } else if (argument == "--capture" && hasValue) {
            w.capturePath = argv[++i];
        } else if (argument.size != 0 && argument.data[0] != '-') {
            paths.push(argument);
        } else {
            usage = true;
        }
    }
    if (usage || paths.count() == 0) {
        printError("usage: model_viewer PATH... [--model NAME] [--list] [--slots] [--visibility] "
            "[--environment FILE] [--tone-map NAME] [--exposure FACTOR] [--animation N] [--time SECONDS] [--camera N] "
            "[--turn RADIANS] [--tilt RADIANS] [--zoom FACTOR] [--lose-device] [--capture FILE] [--cycle PASSES] "
            "[--skim] [--stats]\n"
            "    PATH is a glTF or USD file, or a directory that is searched for them\n");
        return 1;
    }
    w.animation = uint32_t(min(animation, uint64_t(UINT32_MAX)));
    w.modelCamera = uint32_t(min(camera, uint64_t(UINT32_MAX)));
    w.cycle.passes = uint32_t(passes);
    w.firstTurn = float(turn);
    w.firstTilt = prF32Clamp(float(tilt), -1.5f, 1.5f);
    w.firstZoom = prF32Clamp(float(zoom), 0.02f, 8);
    for (PrStringView path : paths) {
        if (modelListAdd(w.models, path) == kModelNone)
            printError("model_viewer: {} is not a .gltf or .glb file, or a directory that holds one\n", path);
    }
    if (w.models.count() == 0)
        return 1;
    uint32_t first = 0;
    if (model.size != 0)
        first = modelListFind(w.models, model);
    if (first == kModelNone) {
        printError("model_viewer: the list of {} models has none called or numbered {}\n", w.models.count(), model);
        return 1;
    }

    // The pool creates the device and loads each model's meshes and images, and the loader's own thread reads the
    // files, while the window opens and draws.
    w.loader.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, clamp(prThreadHardwareCount(), 3u, 9u) - 1);
    w.loader.serial = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    w.compress = prCompressCodecSetCreateDefault();
    w.loader.images = prImageCodecSetCreateDefault(w.compress);
    // The host's formats read glTF and USD, KTX 2.0 through lib/ktx, which transcodes Basis Universal's blocks for the
    // device, and every other image through the codec.
    PrModelHostDesc host{.images = w.loader.images, .compress = w.compress, .pool = w.loader.pool};
    w.loader.host = prModelHostCreate(&host);
    requestDevice(w);
    if (w.device == nullptr) {
        printError("model_viewer: the backend makes no device with these options\n");
        return 1;
    }
    overlayCreate(w.overlay);
    choose(w, first);

    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);

    overlayDestroy(w.overlay);
    prModelHostDestroy(w.loader.host);
    prImageCodecSetDestroy(w.loader.images);
    prCompressCodecSetDestroy(w.compress);
    prTaskPoolDestroy(w.loader.serial);
    prTaskPoolDestroy(w.loader.pool);
    return w.cycle.grew ? 1 : 0;
}
