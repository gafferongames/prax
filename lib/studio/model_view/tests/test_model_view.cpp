// Studio's model view: its wiring in a kit's tree, and what it renders on the system's GPU device.

#include "pr/compress_select.h"
#include "pr/float16.h"
#include "pr/font_set.h"
#include "pr/gltf_model.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/mem.h"
#include "pr/model_view.h"
#include "pr/studio_app.h"
#include "pr/usd_model.h"
#include "pr/usd_stage.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// A glTF asset of one red triangle that no light changes, on node 0, with its buffer in a file beside it, and an
// animation that turns the node.
constexpr PrStringView kAsset = R"({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],)"
    R"("nodes":[{"mesh":0,"name":"triangle"}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0}]}],)"
    R"("extensionsUsed":["KHR_materials_unlit"],"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]},)"
    R"("doubleSided":true,"extensions":{"KHR_materials_unlit":{}}}],)"
    R"("accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
    R"({"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":8},)"
    R"({"buffer":0,"byteOffset":44,"byteLength":32}],"buffers":[{"byteLength":76,"uri":"triangle.bin"}],)"
    R"("animations":[{"name":"turn","channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}],)"
    R"("samplers":[{"input":1,"output":2}]}]})";

// The buffer: the triangle's corners, the animation's two times, and its two rotations, both the identity.
String assetBuffer() {
    const float values[] = {-1, -1, 0, 1, -1, 0, 0, 1, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1};
    return String(PrStringView{reinterpret_cast<const char*>(values), sizeof(values)});
}

// Returns a blob of the bytes of `text`.
PrBlob* blobOf(PrStringView text) {
    return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
}

// Returns a model of a glTF asset, which outlives it, that loads on `pool`.
PrModel* modelOf(const PrGltf* gltf, PrTaskPool* PR_NULLABLE pool = nullptr) {
    PrModelOptions options = prModelOptionsDefault();
    options.pool = pool;
    return prModelCreate(prGltfModelSourceCreate(gltf, nullptr), &options);
}

// A kit with a model view of the asset, whose buffer it reads beside the asset.
struct ViewFixture {
    KitFixture kit;
    PrGltf* PR_NULLABLE gltf = nullptr;
    PrModelView* view = nullptr;
    PrLayoutNode root = kPrLayoutNodeNone;

    ViewFixture() {
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = [](void* PR_NULLABLE, PrGltfExternal, PrStringView uri) -> PrBlob* PR_NULLABLE {
            return uri == "triangle.bin" ? blobOf(assetBuffer()) : nullptr;
        };
        PrBlob* asset = blobOf(kAsset);
        REQUIRE(prGltfCreate(asset, &options, &gltf) == PrStatus_Ok);
        prBlobRelease(asset);
        PrModelViewDesc desc{.kit = kit.kit};
        view = prModelViewCreate(&desc);
        kit.build = [this](PrLayoutNode at) {
            root = at;
            prModelViewFrame(view, at);
        };
    }
    ~ViewFixture() {
        prModelViewDestroy(view);
        prGltfDestroy(gltf);
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a model view shows its asset's controls and a GPU view of it, which the user turns, and follows the "
        "selection") {
    ViewFixture f;
    prModelViewSetModel(f.view, modelOf(f.gltf));
    const PrModel* shown = prModelViewModel(f.view);
    REQUIRE(shown != nullptr);
    // The buffer beside the asset was read through the resolver.
    CHECK(prModelSourceInfo(prModelSource(shown))->meshCount == 1);
    // The first animation plays from the start.
    CHECK(prModelViewAnimation(f.view) == 0);
    CHECK(prModelViewPlaying(f.view));
    f.kit.frame();

    // A drag over the view turns the orbit camera, and keeps it the camera.
    prModelViewSetCamera(f.view, 3);
    CHECK(prModelViewCamera(f.view) == 0);
    PrRect bounds = f.kit.bounds(f.root);
    PrVec2 at{(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2 + 20};
    f.kit.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at}});
    f.kit.frame();
    f.kit.push(PrAppEvent{.kind = PrAppEventKind_PointerMove, .pointer = {.position = {at.x + 40, at.y}}});
    f.kit.frame();
    f.kit.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = {at.x + 40, at.y}}});
    f.kit.frame();

    prModelViewSetPlaying(f.view, false);
    CHECK(!prModelViewPlaying(f.view));
    prModelViewSetAnimation(f.view, kPrModelAnimationNone);
    CHECK(!prModelViewPlaying(f.view));
    prModelViewSetSelection(f.view, 0);
    f.kit.frame();
    CHECK(kitVerified(f.kit.tree));

    // A view without a model shows nothing.
    PrModelViewDesc desc{.kit = f.kit.kit};
    PrModelView* other = prModelViewCreate(&desc);
    prModelViewSetModel(other, nullptr);
    CHECK(prModelViewModel(other) == nullptr);
    prModelViewDestroy(other);
}

TEST_CASE("a model view renders its asset on the GPU into a texture of its own, at the size that its window gives") {
    PrGpuDevice* device = nullptr;
    if (gpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok)
        return;
    ViewFixture f;
    prModelViewSetModel(f.view, modelOf(f.gltf));
    prModelViewSetAnimation(f.view, kPrModelAnimationNone);
    PrGpuGraph* graph = prGpuGraphCreate(device, nullptr);
    constexpr PrUVec2 kExtent{64, 48};

    // Renders the view into a frame's graph, as its window does, and returns the device's texture that it shows.
    auto frame = [&]() {
        prGpuGraphBegin(graph);
        PrStudioWindowRender render{.device = device, .graph = graph, .extent = kExtent};
        PrGpuTexture shown = prGpuGraphTexture(graph, prModelViewRender(f.view, &render).texture);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuGraphRecord(graph, commands);
        CHECK(prGpuGraphErrorCount(graph) == 0);
        gpuSubmitAndWait(commands);
        return shown;
    };
    // Reads the texel at the center of a texture that the view showed, as floats of its half floats.
    auto center = [&](PrGpuTexture shown) {
        REQUIRE(shown.id != 0);
        CHECK(prGpuTextureExtent(device, shown).x == kExtent.x);
        PrFloat16 texel[4] = {};
        gpuTextureRead(device, shown, PrGpuAccessFlag_ShaderRead, PrIVec2{kExtent.x / 2, kExtent.y / 2},
            PrPixels{reinterpret_cast<uint8_t*>(texel), sizeof(texel), {1, 1}});
        PrColor color{};
        float* channels = &color.r;
        for (uint32_t i = 0; i < 4; ++i)
            channels[i] = prFloat16ToFloat(texel[i]);
        return color;
    };

    // The asset loads over the first frames, and a frame that the window asks for renders the view into its texture.
    PrGpuTexture shown{};
    for (uint32_t i = 0; i < 8; ++i)
        shown = frame();
    CHECK(prModelViewRenders(f.view) == 8);
    PrColor red = center(shown);
    // The tone map takes some of the red into the other channels, alike.
    CHECK(red.r > red.g + 0.4f);
    CHECK(red.g == doctest::Approx(red.b).epsilon(0.01));

    // The selection draws the triangle with the highlight, in the view's same texture.
    prModelViewSetSelection(f.view, 0);
    PrGpuTexture highlighted = frame();
    CHECK(highlighted.id == shown.id);
    CHECK(center(highlighted).g > red.g + 0.1f);
    prGpuGraphDestroy(graph);

    // A lost device's resources are the view's to free before the device goes, and the view renders on the next one.
    prGpuDeviceSimulateLoss(device);
    prModelViewDeviceLost(f.view);
    prGpuDeviceDestroy(device);
    REQUIRE(gpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) == PrStatus_Ok);
    graph = prGpuGraphCreate(device, nullptr);
    for (uint32_t i = 0; i < 8; ++i)
        shown = frame();
    CHECK(center(shown).g > red.g + 0.1f);
    prGpuGraphDestroy(graph);
    prModelViewDestroy(f.view);
    f.view = nullptr;
    prGpuDeviceDestroy(device);
}

TEST_CASE("a model view's lighting starts from what its asset brings, and names its choices") {
    // An asset that brings nothing is lit by the sky, with its emission in the view's unit.
    PrModelViewLighting none = prModelViewLightingDefault(nullptr);
    CHECK(none.environment == PrModelViewEnvironment_Sky);
    CHECK(none.emission == PrRenderEmission_Display);
    CHECK(none.exposure == PrModelViewExposure_Scene);
    CHECK(none.environmentStops == 0);
    CHECK(none.lightStops == 0);
    CHECK(none.compensation == 0);
    ViewFixture f;
    prModelViewSetModel(f.view, modelOf(f.gltf));
    PrModelViewLighting lighting = prModelViewLighting(f.view);
    CHECK(lighting.environment == none.environment);
    CHECK(lighting.emission == none.emission);
    lighting.environment = PrModelViewEnvironment_None;
    lighting.exposure = PrModelViewExposure_Manual;
    lighting.ev100 = 7;
    prModelViewSetLighting(f.view, &lighting);
    CHECK(prModelViewLighting(f.view).ev100 == 7);
    CHECK(prModelEmission(prModelViewModel(f.view)) == PrRenderEmission_Display);
    for (PrModelViewEnvironment i = 0; i < PrModelViewEnvironment_Count; ++i)
        CHECK(prModelViewEnvironmentFromName(prModelViewEnvironmentName(i)) == i);
    for (PrModelViewExposure i = 0; i < PrModelViewExposure_Count; ++i)
        CHECK(prModelViewExposureFromName(prModelViewExposureName(i)) == i);
    CHECK(prModelViewExposureFromName("IMAGE") == PrModelViewExposure_Image);
    CHECK(prModelViewEnvironmentFromName("dome") == PrModelViewEnvironment_Count);
    f.kit.frame();
    CHECK(kitVerified(f.kit.tree));
}

TEST_CASE("a model view exposes by the user's EV100, or by a meter of its image, which settles") {
    PrGpuDevice* device = nullptr;
    if (gpuDeviceCreate(PrGpuBackend_Default, nullptr, &device) != PrStatus_Ok)
        return;
    ViewFixture f;
    prModelViewSetModel(f.view, modelOf(f.gltf));
    prModelViewSetAnimation(f.view, kPrModelAnimationNone);
    PrGpuGraph* graph = prGpuGraphCreate(device, nullptr);
    auto frame = [&]() {
        prGpuGraphBegin(graph);
        PrStudioWindowRender render{.device = device, .graph = graph, .extent = {64, 48}};
        prModelViewRender(f.view, &render);
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(device, nullptr, &commands));
        prGpuGraphRecord(graph, commands);
        gpuSubmitAndWait(commands);
    };
    for (uint32_t i = 0; i < 4; ++i)
        frame();
    // The meter of the sky's light reads the sun's daylight: some 100 klx.
    float scene = prModelViewEv100(f.view);
    CHECK(scene > 13);
    CHECK(scene < 16);

    PrModelViewLighting lighting = prModelViewLighting(f.view);
    lighting.exposure = PrModelViewExposure_Manual;
    lighting.ev100 = 9.5f;
    prModelViewSetLighting(f.view, &lighting);
    frame();
    CHECK(prModelViewEv100(f.view) == 9.5f);

    // A meter of the image reads what the frames show, and each frame waits for its work here, so each reading arrives
    // before the next frame. The exposure starts at the first reading, and then holds still.
    lighting.exposure = PrModelViewExposure_Image;
    prModelViewSetLighting(f.view, &lighting);
    for (uint32_t i = 0; i < 6; ++i)
        frame();
    float metered = prModelViewEv100(f.view);
    frame();
    CHECK(prModelViewEv100(f.view) == doctest::Approx(metered).epsilon(1e-3));
    // It reads the image, which the scene's meter of the light does not.
    CHECK(metered != doctest::Approx(scene).epsilon(1e-3));
    prGpuGraphDestroy(graph);
    prModelViewDestroy(f.view);
    f.view = nullptr;
    prGpuDeviceDestroy(device);
}

TEST_CASE("a drag over a model view in its window turns its orbit camera, and the wheel moves it nearer") {
    struct Dragged {
        ViewFixture* fixture = nullptr;
        PrModelView* view = nullptr;
        uint32_t frames = 0;
        uint64_t rendersBefore = 0;
        PrModelViewOrbit before{};
        PrModelViewOrbit after{};
        uint64_t rendersAfter = 0;
    };
    static Dragged run;
    ViewFixture f;
    run.fixture = &f;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrStudioWindowDesc window{
        .title = "Model view",
        .extent = {320, 240},
        .open = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Dragged& r = *static_cast<Dragged*>(context);
            PrModelViewDesc desc{.kit = prStudioWindowKit(w), .window = w};
            r.view = prModelViewCreate(&desc);
            prModelViewSetModel(r.view, modelOf(r.fixture->gltf));
            prModelViewSetAnimation(r.view, kPrModelAnimationNone);
            return true;
        },
        .build = [](void* PR_NULLABLE context, PrStudioWindow*, PrLayoutNode root, PrVec2) {
            prModelViewFrame(static_cast<Dragged*>(context)->view, root);
        },
        .laidOut = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Dragged& r = *static_cast<Dragged*>(context);
            PrUi* ui = prStudioWindowUi(w);
            ++r.frames;
            // A press in the middle of the view, a move to its right, a release, and a turn of the wheel.
            PrVec2 at{160, 160};
            if (r.frames == 10) {
                r.before = prModelViewOrbit(r.view);
                r.rendersBefore = prModelViewRenders(r.view);
                PrAppEvent down{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at}};
                prUiPushEvent(ui, kPrUiViewportMain, &down);
            } else if (r.frames == 11) {
                PrAppEvent move{.kind = PrAppEventKind_PointerMove, .pointer = {.position = {at.x + 50, at.y}}};
                prUiPushEvent(ui, kPrUiViewportMain, &move);
            } else if (r.frames == 12) {
                PrAppEvent up{.kind = PrAppEventKind_PointerUp, .pointer = {.position = {at.x + 50, at.y}}};
                prUiPushEvent(ui, kPrUiViewportMain, &up);
                PrAppEvent wheel{.kind = PrAppEventKind_Scroll, .scroll = {.position = at, .delta = {0, 40}}};
                prUiPushEvent(ui, kPrUiViewportMain, &wheel);
            } else if (r.frames == 16) {
                r.after = prModelViewOrbit(r.view);
                r.rendersAfter = prModelViewRenders(r.view);
                prStudioAppStop(prStudioWindowStudioApp(w));
            }
            return true;
        },
        .close = [](void* PR_NULLABLE context, PrStudioWindow*) {
            Dragged& r = *static_cast<Dragged*>(context);
            prModelViewDestroy(r.view);
            r.view = nullptr;
        },
        .context = &run,
    };
    PrAppOptions options{.headless = true};
    PrStudioAppDesc desc{.fonts = fonts, .options = &options, .window = &window, .context = &run};
    PrStatus status = prStudioAppRun(&desc);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        return;
    REQUIRE(status == PrStatus_Ok);
    REQUIRE(run.frames >= 16);
    CHECK(run.rendersBefore >= 1);
    // The drag turned the camera toward the left of the asset, and the wheel brought it nearer.
    CHECK(run.after.turn < run.before.turn);
    CHECK(run.after.zoom < run.before.zoom);
    CHECK(run.rendersAfter > run.rendersBefore);
}

TEST_CASE("a model view in its window shows its asset once it holds still, though the asset loaded at its first render") {
    // The asset has no image, so its one mesh is all that loads, on the pool's thread while the first render runs, and
    // nothing that the asset shows changes after it.
    struct Shown {
        ViewFixture* fixture = nullptr;
        PrTaskPool* pool = nullptr;
        PrModelView* view = nullptr;
        uint32_t frames = 0;
        bool captured = false;
        uint8_t center[4] = {};
    };
    static Shown run;
    ViewFixture f;
    run.fixture = &f;
    run.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrStudioWindowDesc window{
        .title = "Model view",
        .extent = {320, 240},
        .open = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Shown& r = *static_cast<Shown*>(context);
            PrModelViewDesc desc{.kit = prStudioWindowKit(w), .window = w};
            r.view = prModelViewCreate(&desc);
            prModelViewSetModel(r.view, modelOf(r.fixture->gltf, r.pool));
            prModelViewSetAnimation(r.view, kPrModelAnimationNone);
            return true;
        },
        .build = [](void* PR_NULLABLE context, PrStudioWindow*, PrLayoutNode root, PrVec2) {
            prModelViewFrame(static_cast<Shown*>(context)->view, root);
        },
        .laidOut = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Shown& r = *static_cast<Shown*>(context);
            ++r.frames;
            // The pixel at the middle of the asset, below the bar of the controls, once the view holds still.
            if (r.frames == 20) {
                prStudioWindowCapture(w, [](void* PR_NULLABLE c, PrPixelView rgba) {
                    Shown& s = *static_cast<Shown*>(c);
                    if (rgba.data == nullptr || rgba.extent.x == 0)
                        return;
                    uint32_t x = rgba.extent.x / 2;
                    uint32_t y = uint32_t(float(rgba.extent.y) * (136.0f / 240.0f));
                    for (uint32_t i = 0; i < 4; ++i)
                        s.center[i] = rgba.data[size_t(y) * rgba.stride + size_t(x) * 4 + i];
                    s.captured = true;
                }, &r);
            }
            if (r.captured || r.frames > 200)
                prStudioAppStop(prStudioWindowStudioApp(w));
            else
                prAppWindowRequestFrame(prStudioWindowAppWindow(w));
            return true;
        },
        .close = [](void* PR_NULLABLE context, PrStudioWindow*) {
            Shown& r = *static_cast<Shown*>(context);
            prModelViewDestroy(r.view);
            r.view = nullptr;
        },
        .context = &run,
    };
    PrAppOptions options{.headless = true};
    PrStudioAppDesc desc{.fonts = fonts, .options = &options, .window = &window, .context = &run};
    PrStatus status = prStudioAppRun(&desc);
    prFontSetDestroy(fonts);
    prTaskPoolDestroy(run.pool);
    if (status == PrStatus_NotSupported)
        return;
    REQUIRE(status == PrStatus_Ok);
    REQUIRE(run.captured);
    // The triangle is red, which no part of the sky or the ground is.
    CHECK(int(run.center[0]) > int(run.center[1]) + 80);
}

TEST_CASE("a click on a model view in its window picks the node under it, and a click beside the asset picks none") {
    struct Clicked {
        ViewFixture* fixture = nullptr;
        PrModelView* view = nullptr;
        uint32_t frames = 0;
        uint32_t picks = 0;
        PrModelPick on{};
        PrModelPick beside{};
        PrModelViewOrbit before{};
        PrModelViewOrbit after{};
    };
    static Clicked run;
    ViewFixture f;
    run.fixture = &f;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrStudioWindowDesc window{
        .title = "Model view",
        .extent = {320, 240},
        .open = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Clicked& r = *static_cast<Clicked*>(context);
            PrModelViewDesc desc{.kit = prStudioWindowKit(w), .window = w};
            r.view = prModelViewCreate(&desc);
            prModelViewSetModel(r.view, modelOf(r.fixture->gltf));
            prModelViewSetAnimation(r.view, kPrModelAnimationNone);
            return true;
        },
        .build = [](void* PR_NULLABLE context, PrStudioWindow*, PrLayoutNode root, PrVec2) {
            Clicked& r = *static_cast<Clicked*>(context);
            prModelViewFrame(r.view, root);
            PrModelPick pick{};
            if (prModelViewPicked(r.view, &pick))
                (r.picks++ == 0 ? r.on : r.beside) = pick;
        },
        .laidOut = [](void* PR_NULLABLE context, PrStudioWindow* w) {
            Clicked& r = *static_cast<Clicked*>(context);
            PrUi* ui = prStudioWindowUi(w);
            ++r.frames;
            // A click at the middle of the asset, below the bar of the controls, and then one at its corner.
            PrVec2 at = r.frames < 14 ? PrVec2{160, 136} : PrVec2{4, 236};
            if (r.frames == 10)
                r.before = prModelViewOrbit(r.view);
            if (r.frames == 10 || r.frames == 14) {
                PrAppEvent down{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at}};
                prUiPushEvent(ui, kPrUiViewportMain, &down);
            } else if (r.frames == 11 || r.frames == 15) {
                PrAppEvent up{.kind = PrAppEventKind_PointerUp, .pointer = {.position = at}};
                prUiPushEvent(ui, kPrUiViewportMain, &up);
            } else if (r.frames == 18) {
                r.after = prModelViewOrbit(r.view);
                prStudioAppStop(prStudioWindowStudioApp(w));
            }
            return true;
        },
        .close = [](void* PR_NULLABLE context, PrStudioWindow*) {
            Clicked& r = *static_cast<Clicked*>(context);
            prModelViewDestroy(r.view);
            r.view = nullptr;
        },
        .context = &run,
    };
    PrAppOptions options{.headless = true};
    PrStudioAppDesc desc{.fonts = fonts, .options = &options, .window = &window, .context = &run};
    PrStatus status = prStudioAppRun(&desc);
    prFontSetDestroy(fonts);
    if (status == PrStatus_NotSupported)
        return;
    REQUIRE(status == PrStatus_Ok);
    REQUIRE(run.frames >= 18);
    // The click on the asset picked its triangle's node, and the click beside it picked nothing. Neither turned the
    // camera.
    REQUIRE(run.picks == 2);
    CHECK(run.on.node == 0);
    CHECK(run.on.mesh == 0);
    CHECK(run.beside.node == kPrModelNodeNone);
    CHECK(run.after.turn == run.before.turn);
}

TEST_CASE("a model view shows a USD layer's stage, whose prims it selects by their nodes") {
    KitFixture kit;
    PrCompressCodecSet* codec = prCompressCodecSetCreateDefault();
    // The stage's opener reads the layer from memory.
    PrUsdOpenFn open = [](void* PR_NULLABLE, PrStringView location) -> const PrBlobSource* PR_NULLABLE {
        if (location != "quad.usda")
            return nullptr;
        PrBlob* layer = blobOf("#usda 1.0\n(\n    metersPerUnit = 1\n)\ndef Xform \"World\"\n{\n    def Mesh \"Quad\"\n"
            "    {\n        point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n"
            "        int[] faceVertexCounts = [4]\n        int[] faceVertexIndices = [0, 1, 2, 3]\n    }\n}\n");
        const PrBlobSource* source = prBlobSourceCreateBlob(layer);
        prBlobRelease(layer);
        return source;
    };
    PrUsdStageOptions stageOptions = prUsdStageOptionsDefault();
    stageOptions.layer.codec = codec;
    stageOptions.open = open;
    PrUsdStage* stage = nullptr;
    REQUIRE(prUsdStageCreate("quad.usda", &stageOptions, &stage) == PrStatus_Ok);
    PrModelViewDesc desc{.kit = kit.kit};
    PrModelView* view = prModelViewCreate(&desc);
    kit.build = [view](PrLayoutNode at) { prModelViewFrame(view, at); };
    PrUsdModelOptions source = prUsdModelOptionsDefault();
    source.open = open;
    source.codec = codec;
    PrModelOptions options = prModelOptionsDefault();
    prModelViewSetModel(view, prModelCreate(prUsdModelSourceCreate(stage, &source), &options));
    const PrModel* model = prModelViewModel(view);
    REQUIRE(model != nullptr);
    CHECK(prModelNodeCount(model) == 2);
    // A stage without a time range has no animation to play.
    CHECK(prModelViewAnimation(view) == kPrModelAnimationNone);
    CHECK(!prModelViewPlaying(view));
    prModelViewSetSelection(view, prModelNodeFromAddress(model, "/World/Quad"));
    kit.frame();
    prModelViewDestroy(view);
    prUsdStageDestroy(stage);
    prCompressCodecSetDestroy(codec);
}
