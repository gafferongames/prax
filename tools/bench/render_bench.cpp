// What lib/render's frames cost, which ./dev.sh bench render runs.
//
// `render_bench [--gpu NAME] <path>...` loads the asset at each path, of any format of the model host, as a model on a
// device with handles and on one with the slots alone, each with a renderer of its own, lights it with a clear sky and
// its sun, and draws it by each technique that the device has. Each case writes a line of JSON:
//
//   {"name": the asset's name and the case,
//    "pipelines": the pipelines that the renderer made for the case's first frame,
//    "pipelineMs": the milliseconds that making them took,
//    "loadMs": the milliseconds from opening the asset to its model having every resource in the renderer, once for
//        each asset and tier, by the clock,
//    "textureBytes": the bytes of the texels of the renderer's textures that show the model's images, every level of
//        each (PrModelProgress::textureBytes),
//    "counts": {"time": the cost of one frame: building and recording its graph, and waiting for its work, with the
//        device's time}}
//
// A case's first frame makes the pipelines that it needs and no other frame does, so the frames that are measured make
// none. The asset is drawn by each case in a view of 1280 by 720 that frames its box. NAME is a backend of lib/gpu, the
// default one's by default.

#include "pr/compress_select.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/image_select.h"
#include "pr/model.h"
#include "pr/model_host.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_environment.h"
#include "pr/render_exposure.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/string.h"
#include "pr/time.h"

#include "tools/harness/harness.h"
#include "tools/harness/harness_gpu.h"

using namespace harness;
using namespace pr;

namespace {

constexpr PrUVec2 kExtent{1280, 720};
constexpr float kFov = 0.7f;

// The direction from the model toward the camera, and toward the light.
constexpr PrVec3 kToCamera{0.25f, 0.2f, 1};
constexpr PrVec3 kToLight{0.6f, 0.9f, 0.7f};

// A device with a renderer and a graph on it.
struct Target {
    PrGpuDevice* PR_NULLABLE device = nullptr;
    PrRender* PR_NULLABLE render = nullptr;
    PrGpuGraph* PR_NULLABLE graph = nullptr;
    PrGpuTexture shown{};       // what each frame shows its view in, which makes the view a result of the frame
};

bool openTarget(Target* target, PrGpuBackend backend, bool slotsOnly) {
    PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
    options.slotsOnly = slotsOnly;
    if (gpuDeviceCreate(backend, &options, &target->device) != PrStatus_Ok)
        return false;
    if (prRenderCreate(target->device, nullptr, &target->render) != PrStatus_Ok)
        return false;
    target->graph = prGpuGraphCreate(target->device, nullptr);
    PrGpuTextureDesc desc = prGpuTextureDesc2d(PrGpuFormat_Rgba8, kExtent,
        PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
    desc.name = "render_bench shown";
    return prGpuTextureCreate(target->device, &desc, &target->shown) == PrStatus_Ok;
}

void closeTarget(Target* target) {
    if (target->device == nullptr)
        return;
    prGpuDeviceWaitIdle(target->device);
    prGpuGraphDestroy(target->graph);
    prGpuTextureRelease(target->device, target->shown);
    prRenderDestroy(target->render);
    prGpuDeviceDestroy(target->device);
}

// What the display pass of a frame reads while the graph records.
struct Shown {
    Target* target;
    PrGpuGraphTexture color;
};

// Builds, records and submits one frame of a view, shown in a texture of the frame, and waits for its work. Returns the
// milliseconds that the device spent on it.
double frame(Target* target, const PrRenderViewDesc& view) {
    PrGpuGraph* graph = target->graph;
    prGpuGraphBegin(graph);
    prRenderFrameBegin(target->render, graph);
    Shown* s = static_cast<Shown*>(prGpuGraphAlloc(graph, sizeof(Shown)));
    *s = Shown{target, prRenderViewAdd(target->render, &view).color};
    PrGpuGraphTexture shown = prGpuGraphImportTexture(graph, target->shown, PrGpuAccessFlag_ShaderRead);
    PrGpuGraphPassDesc passDesc{
        .name = "display",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = shown, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    PrGpuGraphPass display = prGpuGraphAddPass(graph, &passDesc, [](void* PR_NULLABLE context, PrGpuPass* pass) {
        const Shown& s = *static_cast<const Shown*>(context);
        prRenderDisplayDraw(s.target->render, pass, prGpuGraphTexture(s.target->graph, s.color), nullptr);
    }, s);
    prGpuGraphUseTexture(graph, display, s->color, PrGpuAccessFlag_FragmentRead);
    // The shown texture is a result of the frame, so the graph records the passes that draw it.
    prGpuGraphLeaveTexture(graph, shown, PrGpuAccessFlag_ShaderRead);
    PrGpuCommands* commands = nullptr;
    if (!prGpuCommandsBegin(target->device, nullptr, &commands))
        return 0;
    prGpuGraphRecord(graph, commands);
    return gpuSubmitAndWait(commands) * 1000;
}

// Returns a camera that looks at the center of a box from the direction `toCamera`, from the least distance at which
// every corner of the box is in a view of kExtent, and a little further.
PrRenderCamera framing(const PrBox3& box, PrVec3 toCamera) {
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

// Puts a populated model in a scene of a target lit by a clear sky and its sun, and returns the view that frames it.
PrRenderViewDesc stage(Target* target, PrModel* model, PrModelPlacement* placement) {
    PrRenderScene scene = prRenderSceneCreate(target->render);
    PrRenderUpdate* update = prRenderUpdateBegin(target->render, scene);
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = kToLight;
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
    prModelUpdate(model);
    PrBox3 box{};
    prModelPlacementBounds(placement, &box);
    PrVec3 light = prRenderSkySunIrradiance(&sky);
    float illuminance = 0.2126f * light.x + 0.7152f * light.y + 0.0722f * light.z;
    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = framing(box, kToCamera);
    view.extent = kExtent;
    view.exposure = prRenderExposureFromEv100(prRenderEv100FromIlluminance(illuminance));
    return view;
}

// What a case reports of its model's load: its time, and the bytes of its textures.
struct Load {
    double ms;
    uint64_t textureBytes;
};

// Draws the case's first frame, which makes its pipelines, and then measures its frames, and writes its row.
void measureCase(Target* target, PrRenderViewDesc view, PrStringView name, Load load) {
    PrRenderPipelineInfo before = prRenderPipelineInfo(target->render);
    frame(target, view);
    PrRenderPipelineInfo after = prRenderPipelineInfo(target->render);
    double gpu = 0;
    HarnessCounters cost = harnessMeasure([&] {
        gpu = frame(target, view);
    });
    // harnessMeasure keeps the last batch, whose frames each took about the same time on the device.
    cost.gpu = gpu;
    HarnessCounts counts{};
    harnessCountsAdd(&counts, "time", cost);
    PrTextBuffer row{};
    harnessAppendFormat(&row, "{\"name\": \"%.*s\", \"pipelines\": %u, \"pipelineMs\": %.3f, \"loadMs\": %.3f, "
        "\"textureBytes\": %llu", int(name.size), name.data, after.count - before.count,
        (after.seconds - before.seconds) * 1000, load.ms, static_cast<unsigned long long>(load.textureBytes));
    harnessCountsWrite(&counts, &row);
    prTextBufferAppend(&row, "}");
    harnessWriteLine(prTextBufferView(&row));
    prTextBufferReset(&row);
}

// Measures each case of one asset on one target. Returns false if the asset does not read.
bool run(PrStringView path, Target* target, PrStringView tier, const PrModelHost* host) {
    PrTimePerf start = prTimePerfNow();
    PrModel* model = prModelHostOpenFile(host, path);
    prModelWait(model);
    if (prModelProgress(model).source != PrModelLoad_Ready) {
        prModelDestroy(model);
        return false;
    }
    PrModelPlacementDesc placed = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(model, &placed);
    prModelPopulate(model, target->render);
    prModelWait(model);
    Load load{prTimePerfMilliseconds(start, prTimePerfNow()), prModelProgress(model).textureBytes};
    PrRenderViewDesc view = stage(target, model, placement);
    PrStringView asset = prFilePathName(path);
    view.technique = PrRenderTechnique_Forward;
    measureCase(target, view, format("{} forward {}", asset, tier), load);
    if (prRenderSupportsTechnique(target->render, PrRenderTechnique_Visibility)) {
        view.technique = PrRenderTechnique_Visibility;
        measureCase(target, view, format("{} visibility {}", asset, tier), load);
    }
    prModelPlacementDestroy(placement);
    prRenderSceneDestroy(target->render, view.scene);
    prModelDestroy(model);
    return true;
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    PrModelHostDesc hostDesc{.images = images, .compress = compress};
    PrModelHost* host = prModelHostCreate(&hostDesc);
    int status = 0;
    PrGpuBackend backend = PrGpuBackend_Default;
    int first = 1;
    if (argc > 2 && PrStringView(argv[1]) == "--gpu") {
        backend = PrGpuBackend(prBackendFromName(prGpuSelectInterface(), argv[2]));
        first = 3;
    }
    // Each asset is drawn by a renderer of its own, whose first frames make every pipeline that the asset needs.
    const bool tiers[] = {false, true};
    for (bool slotsOnly : tiers) {
        for (int i = first; i < argc; ++i) {
            Target target;
            PrStringView binding = slotsOnly ? "slots" : "handles";
            bool drawn = openTarget(&target, backend, slotsOnly) && run(argv[i], &target, binding, host);
            closeTarget(&target);
            if (!drawn)
                status = 1;
        }
    }
    prModelHostDestroy(host);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    return status;
}
