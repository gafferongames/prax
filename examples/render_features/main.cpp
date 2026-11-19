// The render features example: the 3D analog of examples/draw_features. Each page is a small procedural scene that
// shows one feature of lib/render and sweeps its parameters (features.h): every lobe of the Khronos PBR model and of
// OpenPBR Surface, alpha modes, texture transforms, lights, shadows, environments from each source, tone maps,
// instancing, skinning and morphing, and the example's own material types and shading model. The example owns the
// window, the camera and the frame's graph, and lib/render adds the passes of the camera's view. The pass that shows
// the view draws lib/draw's 2D over it: a label that names the page and says what it shows.
//
// Dragging turns the camera about the page's world, scrolling moves it nearer or farther, the left and right arrows
// step through the pages, Space pauses the pages' clock, and T steps through the tone maps.
//
// Usage: render_features [--page NAME] [--list] [--slots] [--visibility] [--tone-map NAME] [--memory] [--stats]
// [--still] [--capture FILE]. --page shows the page of that name first, and --list prints the pages with what each
// shows. --slots creates a device that uses the slots alone. --visibility draws with the visibility technique.
// --tone-map shows the view by the tone map that NAME names (prRenderToneMapFromName), and not by the ACES curve on
// luminance. --memory makes the frame's graph favor memory over barriers. --stats prints what recording each of the
// first frames' graphs did, and then every couple of seconds the frames' average times on the CPU and the GPU and the
// pipelines that the renderer made. --capture writes the fourth frame to FILE as a PNG, at a fixed time, and exits.
// --still holds the pages at that time, so that a screenshot of the page on the web shows what a capture writes.

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
#include "pr/font.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/gpu_select.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/mem.h"
#include "pr/print.h"
#include "pr/render.h"
#include "pr/render_display.h"
#include "pr/render_view.h"
#include "pr/resource.h"
#include "pr/status.h"
#include "pr/string.h"
#include "pr/task.h"
#include "pr/text.h"
#include "pr/type_traits.h"

#include "examples/frame_stats.h"
#include "examples/render_features/features.h"
#include "render_shaders/RenderFeaturesShaders.h"

namespace {

using namespace frame_stats;
using namespace pr;
using namespace render_features;

// The exposure at which the ACES curve shows middle gray as middle gray. At an exposure of 1 the curve shows it three
// quarters of a stop darker, as ACES's reference rendering does.
constexpr float kAcesExposure = 1.45f;

// The frame that --capture writes, which follows the frames that the swap chain and the renderer warm up in.
constexpr uint32_t kCaptureFrame = 3;

struct Window {
    bool slotsOnly = false;
    PrRenderTechnique technique = PrRenderTechnique_Forward;
    PrRenderToneMap toneMap = PrRenderToneMap_AcesLuminance;
    bool printsStats = false;
    PrGpuGraphPriority priority = PrGpuGraphPriority_Barriers;
    PrStringView capturePath{};
    bool still = false;         // the pages stay at the time of a capture

    double clock = 0;           // the pages' time, in seconds, which stands while paused
    bool paused = false;

    // The camera's place on a sphere about the world's origin.
    float turn = 0;             // about the y axis, in radians
    float tilt = 0.4f;          // above the ground, in radians
    float distance = 12;
    uint32_t page = 0;          // the page that is shown
    uint32_t shown = 0;         // the page whose world the renderer holds
    bool dragging = false;

    PrApp* app = nullptr;
    PrAppWindow* window = nullptr;
    PrTaskPool* pool = nullptr;
    PrGpuDevice* device = nullptr;
    PrGpuSwapChain* swapChain = nullptr;
    PrRender* render = nullptr;
    World world;
    PrGpuGraph* graph = nullptr;
    PrDrawRenderer* drawRenderer = nullptr;
    PrDrawFrame* drawFrame = nullptr;

    // The label over the view, and what it is drawn with.
    PrDrawList* list = nullptr;
    const PrFont* PR_NULLABLE font = nullptr;   // null if the font is missing, and the label then has no text
    PrTextShaper* shaper = nullptr;
    PrTextGlyphBuffer* glyphs = nullptr;

    uint32_t frames = 0;
    FrameStats frameStats;
    Atomic<bool> captured;
};

void closeDevice(Window& w) {
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
}

void requestDevice(Window& w) {
    if (w.device != nullptr)
        return;
    PrGpuDeviceOptions options = prGpuDeviceOptionsDefault();
    options.pool = w.pool;
    options.slotsOnly = w.slotsOnly;
    w.device = prGpuDeviceCreate(PrGpuBackend_Default, &options);
}

// Returns true if the window can render. Frees a lost device with everything made on it, and creates another. Makes the
// swap chain and the renderer once the device is ready.
bool openDevice(Window& w) {
    if (w.device != nullptr && prGpuDeviceIsLost(w.device)) {
        if (w.swapChain == nullptr)
            printError("render_features: no GPU device to render with ({})\n",
                prStatusName(prGpuDeviceLostStatus(w.device)));
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
    if (opened) {
        // The renderer draws with the example's shader set: the library's types and model, and OpenPBR.
        PrRenderOptions options = prRenderOptionsDefault();
        options.shaders = renderFeaturesShaderSet();
        opened = prRenderCreate(w.device, &options, &w.render) == PrStatus_Ok;
    }
    if (!opened) {
        printError("render_features: the device cannot render to the window\n");
        closeDevice(w);
        prAppStop(w.app);
        return false;
    }
    worldCreate(w.render, pages()[w.page], &w.world);
    w.shown = w.page;
    w.distance = w.world.distance;
    PrGpuGraphOptions graphOptions{.priority = w.priority};
    w.graph = prGpuGraphCreate(w.device, &graphOptions);
    w.drawFrame = prDrawFrameCreate(w.drawRenderer);
    PrGpuDeviceInfo info = prGpuDeviceInfo(w.device);
    PrStringView binding = (info.features & PrGpuFeatureFlag_Bindless) != 0 ? "handles" : "slots";
    print("render_features: rendering on {} ({}), with {}\n", info.backend, info.adapter, binding);
    return true;
}

PrRenderCamera cameraOf(const Window& w, PrUVec2 extent) {
    PrVec3 direction{prF32Cos(w.tilt) * prF32Sin(w.turn), prF32Sin(w.tilt), prF32Cos(w.tilt) * prF32Cos(w.turn)};
    PrVec3 target = w.world.target;
    return prRenderCameraLookAt(direction * w.distance + target, target, 0.9f, float(extent.x) / float(extent.y), 0.1f);
}

// Shapes `text` into the window's glyph buffer at `size` pixels to the em, and returns its width, or zero if it cannot
// be shaped.
float shape(Window& w, PrStringView text, float size) {
    PrTextRun run{};
    run.font = w.font;
    run.text = text;
    run.pixelsPerEm = size;
    prTextGlyphBufferClear(w.glyphs);
    if (w.font == nullptr || prTextShaperShape(w.shaper, &run, w.glyphs) != PrStatus_Ok)
        return 0;
    return prTextGlyphBufferAdvance(w.glyphs).x * size;
}

// Records `text` at `size`, with its baseline's start at `origin`, and returns its width.
float drawText(Window& w, PrStringView text, PrVec2 origin, float size, PrColor color) {
    float width = shape(w, text, size);
    Array<PrFontGlyph> placed(prTextGlyphBufferGlyphCount(w.glyphs));
    Array<PrAffine2> transforms(placed.count());
    if (placed.count() == 0)
        return width;
    prTextGlyphBufferPlace(w.glyphs, size, origin, placed.begin(), transforms.begin());
    PrFontGlyphRun glyphRun{};
    glyphRun.font = w.font;
    glyphRun.size = size;
    glyphRun.glyphs = placed.begin();
    glyphRun.glyphCount = placed.count();
    prDrawListGlyphs(w.list, &glyphRun, PrVec2{}, {.color = color});
    return width;
}

// Records the 2D that lies over the view, in logical pixels: a panel that names the page, says where it is among the
// pages, and says what it shows.
void recordLabel(Window& w, float pixelRatio) {
    constexpr float kMargin = 12;
    constexpr float kPadding = 12;
    constexpr float kTitle = 17;
    constexpr float kText = 13;
    constexpr PrColor kDim{0.74f, 0.77f, 0.82f, 1};
    const Page& page = pages()[w.page];
    String place = format("{} of {}", w.page + 1, pages().count());
    float width = prF32Max(shape(w, page.name, kTitle) + kText + shape(w, place, kText), shape(w, page.text, kText));
    PrRect panel{kMargin, kMargin, kMargin + width + 2 * kPadding, kMargin + 2 * kPadding + kTitle + kText * 1.6f};
    prDrawListClear(w.list);
    prDrawListPushTransformAffine2(w.list, prAffine2Scale(PrVec2{pixelRatio, pixelRatio}));
    prDrawListCornerRect(w.list, prCornerRectRound(panel, 8), {.color = PrColor{0.04f, 0.05f, 0.07f, 0.82f}});
    PrVec2 origin{panel.left + kPadding, panel.top + kPadding + kTitle * 0.9f};
    float title = drawText(w, page.name, origin, kTitle, PrColor{1, 1, 1, 1});
    drawText(w, place, {origin.x + title + kText, origin.y}, kText, kDim);
    drawText(w, page.text, {origin.x, origin.y + kText * 1.6f}, kText, kDim);
    prDrawListPopTransform(w.list);
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
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    size_t size = 0;
    prImageCodecSetEncode(images, "image/png", pixels, nullptr, 0, &size);
    Array<uint8_t> png(size);
    bool written = prImageCodecSetEncode(images, "image/png", pixels, png.begin(), size, &size) == PrStatus_Ok &&
        prFileWrite(w.capturePath, PrByteView{png.begin(), size});
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    if (!written)
        printError("render_features: could not write {}\n", w.capturePath);
    w.captured.store(true);
}

// What the display pass and the capture read while the graph records.
struct Display {
    Window* window;
    PrGpuGraphTexture color;
};
struct Prepare {
    Window* window;
    PrDrawTarget target;
};

void render(Window& w, PrUVec2 extent) {
    PrTimePerf began = prTimePerfNow();
    PrGpuSwapChainFrame frame{};
    PrGpuCommands* commands = nullptr;
    // The frame that the capture takes draws into the swap chain's own texture, and is not presented.
    if (w.capturePath.size != 0 && w.frames == kCaptureFrame)
        prGpuSwapChainRequestCapture(w.swapChain, writeCapture, &w);
    if (prGpuSwapChainAcquire(w.swapChain, extent, &frame) != PrStatus_Ok ||
            !prGpuCommandsBegin(w.device, nullptr, &commands))
        return;
    PrGpuTexture drawable = frame.color;
    PrPixelFormat format{};
    prGpuFormatToPixelFormat(prGpuSwapChainInfo(w.swapChain).format, &format);

    prGpuGraphBegin(w.graph);
    prRenderFrameBegin(w.render, w.graph);
    PrRenderViewDesc view = prRenderViewDescDefault(w.world.scene);
    view.camera = cameraOf(w, extent);
    view.extent = extent;
    view.technique = w.technique;
    PrGpuGraphTexture sceneColor = prRenderViewAdd(w.render, &view).color;
    // The 2D prepares its own textures before the pass that it draws in begins.
    Prepare* prepare = static_cast<Prepare*>(prGpuGraphAlloc(w.graph, sizeof(Prepare)));
    *prepare = Prepare{&w, PrDrawTarget{.extent = extent, .format = format}};
    prGpuGraphAddWork(w.graph, "prepare 2D", [](void* PR_NULLABLE context, PrGpuCommands* recording) {
        Prepare& p = *static_cast<Prepare*>(context);
        p.target.commands = recording;
        prDrawFramePrepare(p.window->drawFrame, p.window->list, &p.target);
    }, prepare);
    PrGpuGraphTexture target = prGpuGraphImportTexture(w.graph, drawable, PrGpuAccessFlag_Discard);
    prGpuGraphLeaveTexture(w.graph, target, PrGpuAccessFlag_Present);
    PrGpuGraphPassDesc desc{
        .name = "display",
        .viewCount = 1,
        .colorCount = 1,
        .color = {{.texture = target, .load = PrGpuLoad_Discard, .store = PrGpuStore_Keep}},
    };
    Display* display = static_cast<Display*>(prGpuGraphAlloc(w.graph, sizeof(Display)));
    *display = Display{&w, sceneColor};
    PrGpuGraphPass pass = prGpuGraphAddPass(w.graph, &desc, [](void* PR_NULLABLE context, PrGpuPass* drawn) {
        const Display& d = *static_cast<const Display*>(context);
        Window& held = *d.window;
        PrRenderDisplayOptions shownAs{.exposure = 1, .headroom = 1, .toneMap = held.toneMap};
        if (held.toneMap != PrRenderToneMap_Neutral)
            shownAs.exposure = kAcesExposure;
        prRenderDisplayDraw(held.render, drawn, prGpuGraphTexture(held.graph, d.color), &shownAs);
        prDrawFrameRender(held.drawFrame, drawn, PrIVec2{0, 0});
    }, display);
    prGpuGraphUseTexture(w.graph, pass, sceneColor, PrGpuAccessFlag_FragmentRead);
    PrGpuGraphStats stats = prGpuGraphRecord(w.graph, commands);
    if (w.printsStats && w.frames <= kCaptureFrame) {
        print("render_features: frame {}: {} passes, {} barriers, {} textures of {} KiB\n", w.frames, stats.passes,
            stats.barriers, stats.textures, stats.textureBytes / 1024);
    }
    for (uint32_t i = 0; i < prGpuGraphErrorCount(w.graph); ++i)
        printError("render_features: graph: pass '{}' is misused\n", prGpuGraphError(w.graph, i).name);
    prGpuSwapChainPresent(w.swapChain, commands);
    if (w.printsStats)
        frameStatsSubmit(w.frameStats, commands, prTimePerfMilliseconds(began, prTimePerfNow()));
    prGpuCommandsSubmit(commands);
    ++w.frames;
}

bool open(void*, PrAppWindow*, const PrAppFrame*) {
    return true;
}

PrAppOutput frame(void* context, PrAppWindow*, const PrAppFrame* f) {
    Window& w = *static_cast<Window*>(context);
    if (w.captured.load()) {
        prAppStop(w.app);
        return PrAppOutput{};
    }
    if (!openDevice(w))
        return PrAppOutput{.wantsFrame = w.device != nullptr && !prGpuDeviceIsReady(w.device)};
    prGpuSwapChainWait(w.swapChain);
    // A step to another page replaces the world, and tells what the page shows.
    if (w.shown != w.page) {
        worldDestroy(w.render, &w.world);
        worldCreate(w.render, pages()[w.page], &w.world);
        w.shown = w.page;
        w.distance = w.world.distance;
        print("render_features: {}: {}\n", pages()[w.page].name, pages()[w.page].text);
    }
    if (w.capturePath.size != 0 || w.still)
        w.clock = kStillTime;
    else if (!w.paused)
        w.clock += f->delta;
    worldSetTime(w.render, w.world, w.clock);
    float ratio = f->display.pixelRatio;
    recordLabel(w, ratio);
    PrUVec2 extent{uint32_t(f->extent.x * ratio + 0.5f), uint32_t(f->extent.y * ratio + 0.5f)};
    if (extent.x != 0 && extent.y != 0)
        render(w, extent);
    if (w.printsStats)
        frameStatsReport(w.frameStats, "render_features", w.render);
    return PrAppOutput{.wantsFrame = true};
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
                w.tilt = prF32Clamp(w.tilt + e->pointer.movement.y * 0.006f, 0.05f, 1.45f);
            }
            break;
        case PrAppEventKind_Scroll:
            w.distance = prF32Clamp(w.distance - prAppEventScrollPixels(e, 16).y * 0.02f, 3, 24);
            break;
        case PrAppEventKind_KeyDown:
            if (e->keyboard.key == ' ')
                w.paused = !w.paused;
            else if (e->keyboard.key == 't' || e->keyboard.key == 'T')
                w.toneMap = PrRenderToneMap((w.toneMap + 1) % PrRenderToneMap_Count);
            else if (e->keyboard.key == PrAppKey_Right)
                w.page = (w.page + 1) % uint32_t(pages().count());
            else if (e->keyboard.key == PrAppKey_Left)
                w.page = (w.page + uint32_t(pages().count()) - 1) % uint32_t(pages().count());
            else if (e->keyboard.key == PrAppKey_Escape)
                prAppStop(w.app);
            break;
        default:
            break;
    }
}

bool start(void* context, PrApp* app) {
    Window& w = *static_cast<Window*>(context);
    w.app = app;
    PrAppWindowDesc desc{
        .title = "Prax Render Features",
        .extent = {960, 720},
        .handler = {.context = &w, .open = open, .frame = frame, .event = event},
    };
    return prAppWindowCreate(app, &desc, &w.window) == PrStatus_Ok;
}

void appEvent(void*, PrApp*, const PrAppEvent*) {}

void shutdown(void* context, PrApp*) {
    closeDevice(*static_cast<Window*>(context));
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Window w;
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument == "--slots") {
            w.slotsOnly = true;
        } else if (argument == "--visibility") {
            w.technique = PrRenderTechnique_Visibility;
        } else if (argument == "--tone-map" && i + 1 < argc) {
            w.toneMap = prRenderToneMapFromName(argv[++i]);
            if (w.toneMap == PrRenderToneMap_Count) {
                printError("render_features: no tone map named {}\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument == "--page" && i + 1 < argc) {
            w.page = pageNamed(argv[++i]);
            if (w.page == pages().count()) {
                printError("render_features: no page named {}; --list prints the pages\n", PrStringView{argv[i]});
                return 1;
            }
        } else if (argument == "--list") {
            for (const Page& page : pages())
                print("{}: {}\n", page.name, page.text);
            return 0;
        } else if (argument == "--memory") {
            w.priority = PrGpuGraphPriority_Memory;
        } else if (argument == "--stats") {
            w.printsStats = true;
        } else if (argument == "--still") {
            w.still = true;
        } else if (argument == "--capture" && i + 1 < argc) {
            w.capturePath = argv[++i];
        } else {
            printError("usage: render_features [--page NAME] [--list] [--slots] [--visibility] "
                "[--tone-map NAME] [--memory] [--stats] [--still] [--capture FILE]\n");
            return 1;
        }
    }
    w.list = prDrawListCreate();
    w.shaper = prTextShaperCreate(PrTextShaperBackend_Default);
    w.glyphs = prTextGlyphBufferCreate();
    PrBlob* fontFile = prResourceOpen("font_bundle/NotoSans[wdth,wght].ttf");
    if (fontFile == nullptr || prFontCreate(fontFile, 0, nullptr, &w.font) != PrStatus_Ok)
        printError("render_features: the label's font is missing, so it shows no text\n");
    prBlobRelease(fontFile);
    w.pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 1);
    requestDevice(w);
    if (w.device == nullptr) {
        printError("render_features: the backend makes no device with these options\n");
        return 1;
    }
    PrApp* app = prAppCreate(PrAppBackend_Default, nullptr);
    prAppRun(app, {.context = &w, .start = start, .event = appEvent, .shutdown = shutdown});
    prAppDestroy(app);
    prTaskPoolDestroy(w.pool);
    prFontRelease(w.font);
    prTextGlyphBufferDestroy(w.glyphs);
    prTextShaperDestroy(w.shaper);
    prDrawListRelease(w.list);
    return 0;
}
