// The model corpus harness, which tools/suite/model.py drives.
//
// `model_corpus [--gpu NAME] [--out DIRECTORY] <path>...` loads the glTF asset at each path as a model, renders it
// offscreen, and writes a line of JSON for each:
//
//   {"status": the name of the status with which the asset failed to read, or "",
//    "diagnostics": the sentence of that failure,
//    "failedImages", "failedMeshes": the numbers of the model's resources that failed to load,
//    "failures": the first of those resources, each as its kind, its index in the asset and its status,
//    "mismatches": the images whose sources are of a Srgb form, as a file that states sRGB values is, and that a
//        material samples as data, each as its index in the asset and "srgb", what its file states,
//    "textureBytes": the bytes of the texels of the renderer's textures that show the model's images,
//    "unsupported": the names of what the asset uses that a model does not draw,
//    "validation": the number of validation errors that the devices reported for the asset,
//    "slots": the number of pixels that a device with the slots alone draws otherwise than one with handles, by more
//        than kTierLevels,
//    "unsampled": the number of the materials' textures that the device with the slots alone does not sample,
//    "techniques": the number of pixels away from geometric edges that the visibility technique draws otherwise than
//        the forward technique, or -1 if the device does not draw with it,
//    "edges": the number of pixels on geometric edges,
//    "covered": the part of the image that the model covers, from 0 to 1,
//    "pixels": the number of pixels of the image,
//    "image": the path of the image that the harness wrote, or ""}
//
// The harness makes two devices of the backend, one with handles and one with the slots alone, both validating. A model
// is populated on the first, rendered, and populated again on the second, which is the path that an application takes
// after a lost device. The camera looks at the box of the model's placement from a little to the right of its front and
// above it, and a directional light with shadows shines from above the camera's right shoulder under a plain sky. A
// model with animations is drawn a fifth of the way through its first. The image is the forward technique's, with four
// samples, tone mapped, written to DIRECTORY as a PNG named for the asset's model and variant.
//
// `model_corpus [--gpu NAME] --arrival DIRECTORY <path>...` shows each asset as it loads instead, and writes no answer:
// it writes the image of the model once its meshes have arrived, and again after each of its images arrives, one at a
// time, to DIRECTORY as PNGs named for the asset and the number of images that had arrived. They show what a viewer
// draws of a model whose textures are still to come.
//
// A device that is lost while an asset renders, as when the system resets the GPU for another process's fault, drew
// nothing that an answer could describe. The harness then exits with a status of 2 and no answer for the asset, and the
// suite renders the asset again on new devices.

#include "pr/array.h"
#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/mem.h"
#include "pr/model.h"
#include "pr/model_host.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/sort.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

#if defined(PR_RENDER_TEST_VULKAN)
#include "pr/gpu_vulkan.h"
#endif

#include "tools/harness/harness.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/harness_png.h"

using namespace harness;
using namespace pr;

namespace {

constexpr PrUVec2 kExtent{640, 480};
constexpr float kFov = 0.7f;

// The direction from the model toward the camera, and toward the light.
constexpr PrVec3 kToCamera{0.25f, 0.2f, 1};
constexpr PrVec3 kToLight{0.6f, 0.9f, 0.7f};

// A difference of a channel, of 255, above which two pixels count as drawn otherwise: on a geometric edge between one
// sample and four, and between the two techniques.
constexpr int kEdgeLevels = 6;
constexpr int kTechniqueLevels = 12;

// A difference of a channel, of 255, that the two binding tiers may draw a pixel apart by. The tiers compile the same
// arithmetic into two libraries, beside different code to reach a texture, and fast math lets a compiler fuse and
// reassociate each where that code allows, so they round apart. On Metal, eight of the corpus's cases differed by one
// level at one to six pixels, and none with fast math off (a set's PRECISE_MATH).
constexpr int kTierLevels = 1;

// A device of the harness, with a renderer and a graph on it.
struct Target {
    PrGpuBackend backend = PrGpuBackend_Default;
    PrGpuDevice* PR_NULLABLE device = nullptr;
    PrRender* PR_NULLABLE render = nullptr;
    PrGpuGraph* PR_NULLABLE graph = nullptr;
};

// An image as it is shown: rows of four 8-bit channels.
struct Picture {
    PrUVec2 extent{};
    Array<uint8_t> rgba;
};

bool openTarget(Target* target, PrGpuBackend backend, bool slotsOnly) {
    PrGpuDeviceOptions options{.validation = true, .slotsOnly = slotsOnly};
    target->backend = backend;
    if (gpuDeviceCreate(backend, &options, &target->device) != PrStatus_Ok)
        return false;
    if (prRenderCreate(target->device, nullptr, &target->render) != PrStatus_Ok)
        return false;
    target->graph = prGpuGraphCreate(target->device, nullptr);
    return true;
}

void closeTarget(Target* target) {
    if (target->device == nullptr)
        return;
    prGpuDeviceWaitIdle(target->device);
    prGpuGraphDestroy(target->graph);
    prRenderDestroy(target->render);
    prGpuDeviceDestroy(target->device);
}

uint32_t validationErrors(const Target& target) {
#if defined(PR_RENDER_TEST_VULKAN)
    if (target.backend == PrGpuBackend_Vulkan)
        return prGpuVulkanValidationErrors(target.device);
#endif
    (void)target;
    return 0;
}

void readPicture(void* PR_NULLABLE context, PrPixelView texels) {
    Picture& picture = *static_cast<Picture*>(context);
    picture.extent = texels.extent;
    picture.rgba.resize(texels.extent.x * texels.extent.y * 4);
    for (uint32_t y = 0; y < texels.extent.y; ++y)
        prMemCopy(&picture.rgba[y * texels.extent.x * 4], texels.data + size_t(y) * texels.stride, texels.extent.x * 4);
}

// What the passes of one render read while the graph records.
struct Shown {
    Target* target;
    PrGpuGraphTexture color;
    PrGpuGraphTexture shown;
    Picture* picture;
};

// Renders a view, tone mapped, and returns it as it is shown.
Picture render(Target* target, const PrRenderViewDesc& view) {
    Picture picture;
    PrGpuGraph* graph = target->graph;
    prGpuGraphBegin(graph);
    prRenderFrameBegin(target->render, graph);
    Shown* s = static_cast<Shown*>(prGpuGraphAlloc(graph, sizeof(Shown)));
    PrGpuTextureDesc shownDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, view.extent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    *s = Shown{target, prRenderViewAdd(target->render, &view).color, prGpuGraphAddTexture(graph, &shownDesc), &picture};
    PrGpuGraphPassDesc passDesc{
        .name = "display",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = s->shown, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphPass display = prGpuGraphAddPass(graph, &passDesc, [](void* PR_NULLABLE context, PrGpuPass* pass) {
        const Shown& shown = *static_cast<const Shown*>(context);
        prRenderDisplayDraw(shown.target->render, pass, prGpuGraphTexture(shown.target->graph, shown.color), nullptr);
    }, s);
    prGpuGraphUseTexture(graph, display, s->color, PrGpuAccessFlag_FragmentRead);
    PrGpuGraphPass read = prGpuGraphAddWork(graph, "read", [](void* PR_NULLABLE context, PrGpuCommands* commands) {
        const Shown& shown = *static_cast<const Shown*>(context);
        PrGpuTexture texture = prGpuGraphTexture(shown.target->graph, shown.shown);
        if (texture.id != 0)
            prGpuCommandsReadTexture(commands, texture, PrGpuAccessFlag_ShaderRead, readPicture, shown.picture);
    }, s);
    prGpuGraphUseTexture(graph, read, s->shown, PrGpuAccessFlag_ShaderRead);
    PrGpuCommands* commands = nullptr;
    if (prGpuCommandsBegin(target->device, nullptr, &commands)) {
        prGpuGraphRecord(graph, commands);
        gpuSubmitAndWait(commands);
    }
    return picture;
}

// The frames that a target renders of a view before the frame that the tiers are compared by. On Metal, the first
// frames that a process draws with blending are not always the bytes of its later frames: at some frame of the first
// three, a few blended pixels change by one unit of a half float, and stay as they then are. A second renderer on the
// same device, with resources and pipelines of its own, draws its first frame as its later ones, so nothing that the
// renderer uploads or filters is late: the device's driver replaces the code of a blended pipeline after it first draws
// with it. One such pixel of one asset lies where that unit rounds to another level of the picture's 8 bits, so the
// first frames of two targets may differ where their later frames do not.
constexpr uint32_t kSettlingFrames = 3;

// Renders a view after the frames in which a target's picture settles, and returns it as it is shown.
Picture renderSettled(Target* target, const PrRenderViewDesc& view) {
    for (uint32_t i = 0; i < kSettlingFrames; ++i)
        render(target, view);
    return render(target, view);
}

// Returns the largest difference of a channel between pixel `i` of two pictures.
int difference(const Picture& a, const Picture& b, uint32_t i) {
    int largest = 0;
    for (uint32_t channel = 0; channel < 3; ++channel) {
        int d = int(a.rgba[i * 4 + channel]) - int(b.rgba[i * 4 + channel]);
        largest = d < 0 ? (largest > -d ? largest : -d) : (largest > d ? largest : d);
    }
    return largest;
}

// Returns the part of a picture whose pixels are not of its most common color, which is the background's unless the
// model hides it.
double covered(const Picture& picture) {
    Array<uint32_t> colors(picture.rgba.count() / 4);
    if (colors.isEmpty())
        return 0;
    prMemCopy(colors.begin(), picture.rgba.begin(), picture.rgba.count());
    sortItems(Span<uint32_t>(colors.begin(), colors.count()), [](uint32_t a, uint32_t b) { return a < b; });
    uint32_t longest = 0;
    uint32_t run = 0;
    for (uint32_t i = 0; i < colors.count(); ++i) {
        run = i != 0 && colors[i] == colors[i - 1] ? run + 1 : 1;
        longest = run > longest ? run : longest;
    }
    return 1 - double(longest) / double(colors.count());
}

// Returns a camera that looks at the center of a box from the direction `toCamera`, from the least distance at which
// every corner of the box is in a view of kExtent, and a little further.
PrRenderCamera frame(const PrBox3& box, PrVec3 toCamera) {
    PrVec3 center = (box.min + box.max) * 0.5f;
    PrVec3 back = prVec3Normalize(toCamera);
    PrVec3 right = prVec3Normalize(prVec3Cross(PrVec3{0, 1, 0}, back));
    PrVec3 up = prVec3Cross(back, right);
    float aspect = float(kExtent.x) / float(kExtent.y);
    float slope = prF32Tan(kFov * 0.5f);
    float distance = 1e-6f;
    float depth = 0;
    for (uint32_t corner = 0; corner < 8; ++corner) {
        PrVec3 offset = PrVec3{
            (corner & 1) != 0 ? box.max.x : box.min.x,
            (corner & 2) != 0 ? box.max.y : box.min.y,
            (corner & 4) != 0 ? box.max.z : box.min.z,
        } - center;
        float toward = prVec3Dot(offset, back);
        distance = prF32Max(distance, toward + prF32Abs(prVec3Dot(offset, up)) / slope);
        distance = prF32Max(distance, toward + prF32Abs(prVec3Dot(offset, right)) / (slope * aspect));
        depth = prF32Max(depth, toward);
    }
    distance *= 1.08f;
    return prRenderCameraLookAt(center + back * distance, center, kFov, aspect, (distance - depth) * 0.5f);
}

// Returns the sky that lights a model: a clear day's daylight, with its sun toward the light.
PrRenderSkyDesc stageSky() {
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = kToLight;
    return sky;
}

// Returns the exposure of a view of the sky's scene: an incident-light meter's under the sun's light alone, at which a
// white surface without gloss that faces the sun shows as white.
float stageExposure() {
    PrRenderSkyDesc sky = stageSky();
    PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
    float illuminance = 0.2126f * sunlight.x + 0.7152f * sunlight.y + 0.0722f * sunlight.z;
    return prRenderExposureFromEv100(prRenderEv100FromIlluminance(illuminance));
}

// Returns true if every mesh of a model has loaded or failed.
bool meshesArrived(const PrModel* model) {
    PrModelProgress progress = prModelProgress(model);
    return progress.meshesReady + progress.meshesFailed == progress.meshCount;
}

// A model in a scene of one target's renderer, lit, with the view that frames it. The model has loaded, or with
// `meshesOnly` only its meshes have, and each update that follows loads one of its images.
PrRenderViewDesc stage(Target* target, PrModel* model, PrModelPlacement* placement, bool meshesOnly = false) {
    prModelPopulate(model, target->render);
    while (meshesOnly && !meshesArrived(model))
        prModelUpdate(model);
    if (!meshesOnly)
        prModelWait(model);
    PrRenderScene scene = prRenderSceneCreate(target->render);
    PrRenderUpdate* update = prRenderUpdateBegin(target->render, scene);
    // A sky is the scene's environment, and its sun is the scene's light.
    PrRenderSkyDesc sky = stageSky();
    PrRenderEnvironment environment = prRenderEnvironmentCreate(target->render);
    prRenderEnvironmentSetSky(target->render, environment, &sky);
    PrRenderSceneEnvironment lit = prRenderSceneEnvironmentDefault(environment);
    prRenderSceneEnvironmentSet(update, &lit);
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = -kToLight;
    sun.color = prRenderSkySunIrradiance(&sky);
    sun.castsShadows = true;
    prRenderLightAdd(update, &sun);
    prRenderUpdateCommit(update);
    prModelPlacementSetScene(placement, scene);
    // An update of a model that is loading loads one resource more, and the pose of a model at rest needs none.
    if (!meshesOnly)
        prModelUpdate(model);

    PrBox3 box{};
    prModelPlacementBounds(placement, &box);
    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = frame(box, kToCamera);
    view.extent = kExtent;
    view.exposure = stageExposure();
    return view;
}

// Writes the names of what a model does not draw as its asset means it (prModelDiagnostic).
void appendNames(PrTextBuffer* out, const PrModel* model) {
    prTextBufferAppendChar(out, '[');
    for (uint32_t i = 0; i < prModelDiagnosticCount(model); ++i) {
        if (i != 0)
            prTextBufferAppendChar(out, ',');
        harnessWriteString(out, prModelDiagnostic(model, i).feature);
    }
    prTextBufferAppendChar(out, ']');
}

// What the harness found of one asset.
struct Answer {
    PrStatus status = PrStatus_Ok;
    String diagnostics;
    PrModelProgress progress{};
    String failures;                   // the items of a JSON array
    String mismatches;                 // the items of a JSON array
    String unsupported = String("[]"); // a JSON array of names
    uint32_t validation = 0;
    uint32_t slots = 0;
    uint32_t unsampled = 0;
    int64_t techniques = -1;
    uint32_t edges = 0;
    double covered = 0;
    String image;
};

void write(const Answer& answer) {
    PrTextBuffer out = {};
    prTextBufferAppend(&out, "{\"status\":");
    harnessWriteString(&out, prStatusName(answer.status));
    prTextBufferAppend(&out, ",\"diagnostics\":");
    harnessWriteString(&out, answer.diagnostics);
    harnessAppendFormat(&out, ",\"failedImages\":%u,\"failedMeshes\":%u,\"textureBytes\":%llu,\"failures\":[",
        answer.progress.imagesFailed, answer.progress.meshesFailed,
        static_cast<unsigned long long>(answer.progress.textureBytes));
    prTextBufferAppend(&out, answer.failures);
    prTextBufferAppend(&out, "],\"mismatches\":[");
    prTextBufferAppend(&out, answer.mismatches);
    prTextBufferAppend(&out, "],\"unsupported\":");
    prTextBufferAppend(&out, answer.unsupported);
    harnessAppendFormat(&out, ",\"validation\":%u,\"slots\":%u,\"unsampled\":%u,\"techniques\":%lld,\"edges\":%u,"
        "\"covered\":%.4f,\"pixels\":%u,\"image\":", answer.validation, answer.slots, answer.unsampled,
        static_cast<long long>(answer.techniques), answer.edges, answer.covered, kExtent.x * kExtent.y);
    harnessWriteString(&out, answer.image);
    prTextBufferAppendChar(&out, '}');
    harnessWriteLine(prTextBufferView(&out));
    prTextBufferReset(&out);
}

// The number of failed resources that an answer names.
constexpr uint32_t kMaxFailures = 4;

// Appends a resource that failed to load to an answer's failures, if it failed and the answer has room.
void appendFailure(Answer* answer, PrStringView kind, uint32_t index, PrModelResourceInfo info, uint32_t* count) {
    if (info.load != PrModelLoad_Failed || *count >= kMaxFailures)
        return;
    if (*count != 0)
        answer->failures.append(",");
    formatTo(answer->failures, "\"{} {}: {}\"", kind, index, prStatusName(info.status));
    ++*count;
}

// Returns the name of an asset's image: its model and its variant, which are the two folders above the asset.
String imageName(PrStringView path) {
    PrStringView variant = prFilePathParent(path);
    PrStringView model = prFilePathParent(variant);
    return format("{}.{}.png", prFilePathName(model), prFilePathName(variant));
}

// Writes a picture to `directory` as a PNG of a name. Returns the file's path, or nothing if it was not written.
String writePicture(PrStringView directory, PrStringView name, const Picture& picture) {
    PrTextBuffer file = {};
    prTextBufferAppend(&file, directory);
    prFilePathAppend(&file, name);
    prTextBufferAppendChar(&file, '\0');
    PrPixelView shown{picture.rgba.begin(), size_t(kExtent.x) * 4, kExtent};
    String path;
    if (picture.rgba.count() == kExtent.x * kExtent.y * 4 && harnessWritePng(file.data, shown))
        path = PrStringView{file.data, file.size - 1};
    prTextBufferReset(&file);
    return path;
}

// Renders one asset as it loads: once its meshes have arrived, and after each image that arrives then. Writes each
// image to `outDirectory`.
void runArrival(PrStringView path, PrStringView outDirectory, const PrModelHost* host, Target* target) {
    PrModel* model = prModelHostOpenFile(host, path);
    prModelWait(model);
    if (prModelProgress(model).source != PrModelLoad_Ready) {
        prModelDestroy(model);
        return;
    }
    PrModelPlacementDesc placed = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(model, &placed);
    PrRenderViewDesc view = stage(target, model, placement, true);
    String name = imageName(path);
    for (uint32_t step = 0;; ++step) {
        PrModelProgress progress = prModelProgress(model);
        PrStringView whole = name;
        String file = format("{}.arrived-{}.png", PrStringView{whole.data, whole.size - 4}, step);
        String written = writePicture(outDirectory, file, render(target, view));
        harnessWriteLine(format("{}: {} of {} images", written, progress.imagesReady, progress.imageCount));
        if (prModelIsLoaded(model))
            break;
        prModelUpdate(model);
    }
    prModelPlacementDestroy(placement);
    prRenderSceneDestroy(target->render, view.scene);
    prModelDestroy(model);
}

// Renders one asset and writes its answer. Returns false, and writes nothing, if a device was lost meanwhile.
bool run(PrStringView path, PrStringView outDirectory, const PrModelHost* host, Target* handles, Target* slots) {
    Answer answer;
    // The model is opened through the host's formats, which read the asset and what it names before it is described.
    PrModel* model = prModelHostOpenFile(host, path);
    prModelWait(model);
    PrModelProgress opened = prModelProgress(model);
    if (opened.source != PrModelLoad_Ready) {
        answer.status = opened.sourceStatus;
        answer.diagnostics = String(opened.sourceStatus == PrStatus_NotSupported ? "no format reads the asset" :
            "the asset does not read");
        prModelDestroy(model);
        write(answer);
        return true;
    }
    uint32_t errors = validationErrors(*handles) + validationErrors(*slots);
    const PrModelSourceInfo& described = *prModelSourceInfo(prModelSource(model));
    PrModelPlacementDesc placed = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(model, &placed);
    if (prModelAnimationCount(model) != 0) {
        prModelPlacementSetAnimation(placement, 0, PrModelLoop_Repeat);
        prModelPlacementSetTime(placement, prModelAnimationDuration(model, 0) * 0.2f);
    }

    PrRenderViewDesc view = stage(handles, model, placement);
    answer.progress = prModelProgress(model);
    uint32_t failures = 0;
    for (uint32_t i = 0; i < described.meshCount; ++i)
        appendFailure(&answer, "mesh", i, prModelMeshInfo(model, i), &failures);
    for (uint32_t i = 0; i < described.imageCount; ++i) {
        PrModelResourceInfo info = prModelImageInfo(model, i);
        appendFailure(&answer, "image", i, info, &failures);
        if (info.load != PrModelLoad_Ready || !info.colorMismatch)
            continue;
        formatTo(answer.mismatches, "{}{{\"image\":{},\"file\":\"srgb\"}}", answer.mismatches.isEmpty() ? "" : ",", i);
    }
    PrTextBuffer names = {};
    appendNames(&names, model);
    answer.unsupported = String(prTextBufferView(&names));
    prTextBufferReset(&names);
    Picture smoothed = renderSettled(handles, view);
    view.samples = 1;
    Picture single = render(handles, view);
    uint32_t pixels = kExtent.x * kExtent.y;
    bool rendered = smoothed.rgba.count() == pixels * 4 && single.rgba.count() == pixels * 4;
    if (rendered && prRenderSupportsTechnique(handles->render, PrRenderTechnique_Visibility)) {
        view.technique = PrRenderTechnique_Visibility;
        Picture visibility = render(handles, view);
        view.technique = PrRenderTechnique_Forward;
        answer.techniques = 0;
        for (uint32_t i = 0; visibility.rgba.count() == pixels * 4 && i < pixels; ++i) {
            if (difference(smoothed, single, i) > kEdgeLevels)
                ++answer.edges;
            else if (difference(single, visibility, i) > kTechniqueLevels)
                ++answer.techniques;
        }
    }
    PrRenderScene firstScene = view.scene;

    // The second device takes the model as a new device takes it after a loss.
    view = stage(slots, model, placement);
    prRenderSceneDestroy(handles->render, firstScene);
    Picture slotted = renderSettled(slots, view);
    answer.unsampled = prModelUnsampledTextureCount(model);
    answer.slots = pixels;
    if (rendered && slotted.rgba.count() == pixels * 4) {
        answer.slots = 0;
        for (uint32_t i = 0; i < pixels; ++i)
            answer.slots += difference(smoothed, slotted, i) > kTierLevels;
        answer.covered = covered(smoothed);
        if (outDirectory.size != 0)
            answer.image = writePicture(outDirectory, imageName(path), smoothed);
    }
    prModelPlacementDestroy(placement);
    prRenderSceneDestroy(slots->render, view.scene);
    prModelDestroy(model);
    // A renderer frees what was destroyed when a frame begins, so that the next asset finds room on the device.
    Target* const targets[] = {handles, slots};
    for (Target* target : targets) {
        prGpuGraphBegin(target->graph);
        prRenderFrameBegin(target->render, target->graph);
    }
    answer.validation = validationErrors(*handles) + validationErrors(*slots) - errors;
    if (prGpuDeviceIsLost(handles->device) || prGpuDeviceIsLost(slots->device))
        return false;
    write(answer);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    PrGpuBackend backend = PrGpuBackend_Default;
    PrStringView outDirectory{};
    bool arrival = false;
    int first = 1;
    while (first + 1 < argc) {
        PrStringView option = argv[first];
        if (option == "--gpu")
            backend = PrGpuBackend(prBackendFromName(prGpuSelectInterface(), argv[first + 1]));
        else if (option == "--out" || option == "--arrival")
            outDirectory = argv[first + 1];
        else
            break;
        arrival = arrival || option == "--arrival";
        first += 2;
    }
    if (outDirectory.size != 0)
        prFileCreateDirectory(outDirectory);
    Target handles;
    Target slots;
    if (!prBackendIsBuilt(prGpuSelectInterface(), backend) || !openTarget(&handles, backend, false) ||
            !openTarget(&slots, backend, true)) {
        harnessWriteLine("{\"status\":\"NotSupported\",\"diagnostics\":\"the backend makes no device\"}");
        return 1;
    }
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    // The host reads glTF and USD, KTX 2.0 through lib/ktx, which transcodes its blocks for the device, and every other
    // image through the codec.
    PrModelHostDesc desc{.images = images, .compress = compress};
    PrModelHost* host = prModelHostCreate(&desc);
    bool lost = false;
    for (int i = first; !lost && i < argc; ++i) {
        if (arrival)
            runArrival(argv[i], outDirectory, host, &handles);
        else
            lost = !run(argv[i], outDirectory, host, &handles, &slots);
    }
    prModelHostDestroy(host);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    closeTarget(&slots);
    closeTarget(&handles);
    return lost ? 2 : 0;
}
