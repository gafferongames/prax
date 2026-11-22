// Pandite: an image viewer that stays responsive, in Studio's window (pr/studio_window.h).
//
// main() is the composition root. It creates the viewer, the catalog, the loader, the builder and the system's fonts.
// The catalog names the sources of the viewer's pictures and opens them on a pool of threads. The loader holds the
// pictures that a frame wants. The builder builds the view over the window's context.
//
// Usage: pandite [--capture png] [--run command]... [--] [path]
//
// `path` names the image or the folder to open. --run runs a command by its id once the window opens. For example,
// `--run app.shortcuts` shows the keyboard shortcuts. --capture writes one frame of the window to a PNG and quits. It
// is the first frame that shows everything that the window wants decoded. The toolbar is shown in the capture.

#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/compress_select.h"
#include "pr/construct.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/kit.h"
#include "pr/print.h"
#include "pr/studio_app.h"
#include "pr/task.h"
#include "pr/texture_catalog.h"
#include "pr/texture_select.h"
#include "pr/thread.h"
#include "pr/time.h"

#include "apps/pandite/view.h"

using namespace pandite;
using namespace pr;

namespace {

// The state of the application. `pandite` is the viewer. `resources` is what the view draws with, and it lives as long
// as the app. `ui` is the builder of the view. It lives from the opening of the window to its closing, as the context
// of the window does.
struct Viewer {
    Pandite pandite;
    UiResources* resources = nullptr;
    UiBuilder* ui = nullptr;
    String title;

    // The commands of --run. They run in turn, one per frame, each once the viewer is idle, the way that a user's keys
    // would arrive. `runAt` is the next command to run. `refusedAt` is the time, in seconds from the start, since which
    // the viewer has been idle with that command disabled, or -1.
    Array<PrStringView> runs;
    uint32_t runAt = 0;
    double refusedAt = -1;

    // The PNG of --capture. `captureAsked` is set once the capture is asked for. `captured` is set once the capture is
    // written, and the GPU device's thread sets it.
    PrStringView capture;
    const PrImageCodecSet* images = nullptr;
    bool captureAsked = false;
    double settledAt = -1;          // seconds from the start when the loader last became idle, or -1 while it decodes
    PrTimePerf started = prTimePerfNow();
    Atomic<bool> captured;
};

// How long the viewer stays idle before --capture captures the window, in seconds. A command of --run that stays
// disabled for that long is given up.
constexpr double kCaptureSettle = 0.5;

Viewer& viewerOf(void* context) {
    return *static_cast<Viewer*>(context);
}

// Returns the seconds since the viewer started.
double secondsOf(const Viewer& app) {
    return prTimePerfMilliseconds(app.started, prTimePerfNow()) / 1000;
}

// Returns true if no decode or edit runs.
bool idle(const Viewer& app) {
    return !loaderIsBusy(app.pandite.loader) && !editorIsBusy(app.pandite) && app.pandite.info.reading == nullptr;
}

// Runs the next command of --run once the viewer is idle. Gives up on a command that stays disabled while the viewer
// stays idle.
void runNext(Viewer& app) {
    if (app.runAt == app.runs.count() || !idle(app))
        return;
    PrStringView id = app.runs[app.runAt];
    if (prCommandRegistryRun(app.pandite.commands->registry, id, nullptr, nullptr) == PrStatus_Ok) {
        ++app.runAt;
        app.refusedAt = -1;
        return;
    }
    double now = secondsOf(app);
    if (app.refusedAt < 0) {
        app.refusedAt = now;
    } else if (now - app.refusedAt >= kCaptureSettle) {
        printError("pandite: cannot run command {}\n", id);
        ++app.runAt;
        app.refusedAt = -1;
    }
}

// Names the window for the image on screen.
void entitle(Viewer& app, PrStudioWindow* window) {
    const Pandite& p = app.pandite;
    String title("pandite");
    if (p.index != kNoImage)
        title = format("{}{} - pandite", p.folder.names[p.index], editorIsEdited(p) ? " (edited)" : "");
    if (title == app.title)
        return;
    app.title = title;
    prAppWindowSetTitle(prStudioWindowAppWindow(window), title);
}

// Writes the pixels of the frame that the window captured to the PNG of --capture. Wakes the application, which then
// quits. Called on the GPU device's thread.
void writeCapture(void* PR_NULLABLE context, PrPixelView rgba) {
    Viewer& app = viewerOf(context);
    bool written = false;
    if (rgba.data != nullptr) {
        size_t size = 0;
        prImageCodecSetEncode(app.images, "image/png", rgba, nullptr, 0, &size);
        Array<uint8_t> png;
        png.resize(uint32_t(size));
        written = prImageCodecSetEncode(app.images, "image/png", rgba, png.data(), png.count(), &size) == PrStatus_Ok &&
            prFileWrite(app.capture, PrByteView{png.data(), size});
    }
    if (!written)
        printError("pandite: cannot write {}\n", app.capture);
    app.captured.store(true);
    if (app.pandite.app != nullptr)
        prAppWake(app.pandite.app);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Viewer app;
    PrFontSet* fonts = prFontSetCreate(nullptr);
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    if (system != nullptr)
        prFontSetSetSystem(fonts, system);
    app.resources = create<UiResources>(fonts);

    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = compress != nullptr ? prImageCodecSetCreateDefault(compress) : nullptr;
    if (images == nullptr) {
        printError("pandite: cannot create the image codecs\n");
        return 1;
    }
    // The pool opens the files and reads what the window draws of them. It has a thread for each core except the core
    // that runs the window.
    uint32_t cores = prThreadHardwareCount();
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, cores > 3 ? cores - 1 : 2);
    app.pandite.codecs = images;
    app.pandite.pool = pool;
    // The pictures are the sources of texture and image files. A KTX 2.0 file opens as a texture, so its blocks reach
    // the device as blocks and its levels are kept. Every other file opens through the image codecs, and its colors
    // show as sRGB. The catalog names one source for each path, and wakes the window when a file opens.
    PrTextureFormatSet* textureFormats = prTextureFormatSetCreateDefault(compress, images, PrImageTextureColor_Srgb);
    PrTextureCatalogDesc catalogDesc = prTextureCatalogDescDefault(textureFormats);
    catalogDesc.tasks = pool;
    catalogDesc.opened = [](void* PR_NULLABLE context) {
        Pandite& p = *static_cast<Pandite*>(context);
        if (p.app != nullptr)
            prAppWake(p.app);
    };
    catalogDesc.openedContext = &app.pandite;
    PrTextureCatalog* catalog = prTextureCatalogCreate(&catalogDesc);
    app.pandite.loader = loaderCreate(catalog);
    app.pandite.commands = commandsCreate(panditeCommandHandler(app.pandite));
    PrStringView path{};
    for (int i = 1; i < argc; ++i) {
        PrStringView arg(argv[i]);
        if (arg == "--capture" && i + 1 < argc)
            app.capture = PrStringView(argv[++i]);
        else if (arg == "--run" && i + 1 < argc)
            app.runs.push(PrStringView(argv[++i]));
        else if (arg == "--" && i + 1 < argc)
            path = PrStringView(argv[++i]);
        else
            path = arg;
    }
    app.images = images;
    app.pandite.toolbarPinned = !app.capture.isEmpty();
    if (!path.isEmpty() && !panditeOpen(app.pandite, path))
        printError("pandite: cannot open {}\n", path);

    PrStudioWindowDesc desc{
        .title = "pandite",
        .extent = {1280, 860},
        .minExtent = {360, 240},
        .open = [](void* context, PrStudioWindow* window) {
            Viewer& a = viewerOf(context);
            a.pandite.window = window;
            a.pandite.app = prStudioWindowApp(window);
            UiBuilderDesc builder{.ui = prStudioWindowUi(window), .resources = a.resources, .textColor = {1, 1, 1, 1}};
            a.ui = create<UiBuilder>(builder);
            return true;
        },
        .beginFrame = [](void* context, PrStudioWindow* window, const PrAppFrame* frame) {
            Viewer& a = viewerOf(context);
            if (a.captured.load())
                prAppStop(prStudioWindowApp(window));
            panditeRefresh(a.pandite);
            PrUVec2 screen{
                uint32_t(frame->extent.x * frame->display.pixelRatio),
                uint32_t(frame->extent.y * frame->display.pixelRatio),
            };
            loaderBegin(a.pandite.loader, screen, prKitTextures(prStudioWindowKit(window)));
            runNext(a);
            entitle(a, window);
        },
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2 extent) {
            Viewer& a = viewerOf(context);
            buildPandite(a.pandite, *a.ui, root, extent);
        },
        .laidOut = [](void* context, PrStudioWindow* window) {
            Viewer& a = viewerOf(context);
            loaderEnd(a.pandite.loader);
            bool running = a.runAt < a.runs.count();
            if (a.capture.isEmpty() || a.captureAsked)
                return running;
            // A capture waits until the commands of --run have run, and then until no decode or edit has run for
            // kCaptureSettle seconds. The frames in that time ask for the pictures that they show, so the textures of
            // the pictures that decoded are on the device when the capture is taken.
            double now = secondsOf(a);
            if (running || !idle(a))
                a.settledAt = -1;
            else if (a.settledAt < 0)
                a.settledAt = now;
            if (a.settledAt >= 0 && now - a.settledAt >= kCaptureSettle) {
                a.captureAsked = true;
                prStudioWindowCapture(window, writeCapture, &a);
                return false;
            }
            return true;
        },
        .close = [](void* context, PrStudioWindow*, PrStatus) {
            Viewer& a = viewerOf(context);
            destroy(a.ui);
            a.ui = nullptr;
            a.pandite.window = nullptr;
        },
        .context = &app,
    };
    // Pandite asks for Desktop windows, so that its menus, tooltips and dialogs show in windows of their own and may
    // reach past the viewer's window. A capture asks for Single, so that the captured window shows what its commands
    // open.
    PrAppOptions options = prAppOptionsDefault();
    options.windowModel = app.capture.isEmpty() ? PrAppWindowModel_Desktop : PrAppWindowModel_Single;
    PrStudioAppDesc studio{
        .fonts = fonts,
        .options = &options,
        .pool = pool,
        .compress = compress,
        .images = images,
        .window = &desc,
    };
    PrStatus status = prStudioAppRun(&studio);
    app.pandite.app = nullptr;
    panditeClose(app.pandite);
    loaderDestroy(app.pandite.loader);
    prTextureCatalogDestroy(catalog);
    prTextureFormatSetDestroy(textureFormats);
    commandsDestroy(app.pandite.commands);
    prTaskPoolDestroy(pool);
    panditeRelease(app.pandite);
    prImageCodecSetDestroy(images);
    prCompressCodecSetDestroy(compress);
    destroy(app.resources);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("pandite: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
