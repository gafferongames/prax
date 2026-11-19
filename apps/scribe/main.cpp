// Scribe: Studio's text editor, in a window.
//
// main() is the composition root: Scribe's interface in Studio's window (pr/studio_window.h), its text set in the
// monospace face of the window's fonts.
//
// Usage: scribe [--trace] [--capture png] [--debug program] [--run command]... [--move x,y | --click x,y[,button]]...
//        [file | folder | --diff old new] [file...]
//
// A folder opens as the project, and a file opens in the project of its folder, each in the view that its format calls
// for. A path may end in an address's steps after a `#`, which open the address in the file and select its location.
// Without a path, the last project opens again, and --capture leaves the last project as it was. --diff shows the
// comparison of two files, each editable. --debug makes lldb-dap the debugger, which launches `program` (Debug > Start
// or Continue Debugging). --trace prints each frame that takes long, by its phases, each long gap between frames, and
// how long input waited for the frame that took it, all in milliseconds. Each file after the first opens in a tab of
// its own. --run runs a command by its id once the files are open, as the palette does, in the order given, with the
// arguments that follow the id after a space as a JSON object, as `model.camera {"camera": "Camera"}`. --capture writes
// what the window shows once it has shown it for a few seconds, as the GPU renders it, as a PNG, and quits, whether or
// not the display shows the window. --move and --click are input for a capture to show, which the window takes in the
// order given, after the commands of --run: the pointer moves to `x`, `y`, in logical pixels of the window's content,
// and a click there presses and releases `button`, 0 for the primary and 1 for the secondary, as a context menu's.

#include "pr/array.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/command.h"
#include "pr/compress.h"
#include "pr/compress_select.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float_convert.h"
#include "pr/font_bundle.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/json.h"
#include "pr/print.h"
#include "pr/string_ops.h"
#include "pr/string_view.h"
#include "pr/studio_app.h"
#include "pr/text_font.h"
#include "pr/thread.h"
#include "pr/time.h"
#include "pr/url_file.h"

#include "apps/scribe/scribe.h"
#include "apps/scribe/workspace.h"

using namespace scribe;

namespace {

// How long a frame takes, or how long a gap or input's wait lasts, for --trace to report it, in milliseconds.
constexpr double kTraceSlowFrame = 16;
constexpr double kTraceLongGap = 100;

// The pointer's input of --move or --click: where it goes, in logical pixels of the window's content, and for a click,
// the button that it presses there.
struct PointerInput {
    PrVec2 at;
    bool click;
    uint8_t button;
};

// What the command line asks for, and the editor that it opens.
struct Run {
    Scribe scribe;
    PrStringView path;
    PrStringView compared;          // the old file of --diff
    PrStringView debuggee;          // the program of --debug
    Array<PrStringView> more;       // the files to open after the first
    PrStringView capture;           // the PNG of --capture
    PrStudioWindow* window = nullptr;
    PrThread captureWaker{};        // wakes the application when --capture is due
    Atomic<bool> captureDue;
    Atomic<bool> captured;
    Array<PrStringView> commands;   // the ids of --run
    Array<PointerInput> inputs;     // --move and --click, in order
    bool diff = false;

    // The program decodes every format that Prax has, and the capture encodes PNG.
    PrCompressCodecSet* PR_NULLABLE compress = nullptr;
    PrImageCodecSet* PR_NULLABLE images = nullptr;
};

// How long the window shows before --capture takes it, in seconds.
constexpr double kCaptureSeconds = 3;

Run& runOf(void* context) {
    return *static_cast<Run*>(context);
}

// Opens a path of the command line as openPath does. A path that ends in an address's steps after a `#`, as
// `stage.usda#/World/Car.points`, opens the file and then the address, which selects its location.
bool openArgument(Scribe& s, PrStringView argument) {
    size_t hash = prStringFindByte(argument, '#');
    PrFileInfo info{};
    if (hash == argument.size || prFileInfo(argument, &info))
        return openPath(s, argument);
    // The file's folder is the project, and the address opens in the tab.
    PrStringView path = prStringHead(argument, hash);
    String absolute = String::adopt(prFilePathAbsolute(path, ""));
    if (!prFileInfo(absolute, &info) || !openProject(s, prFilePathParent(absolute)))
        return false;
    String address = String::adopt(prUrlFromFilePath(absolute));
    address.append(prStringFrom(argument, hash));
    return openAddress(s, address) == PrStatus_Ok;
}

// Returns true if a path of the command line names something on disk, before any address's steps after a `#`.
bool exists(PrStringView argument) {
    PrFileInfo info{};
    return prFileInfo(argument, &info) || prFileInfo(prStringHead(argument, prStringFindByte(argument, '#')), &info);
}

// Runs a command of --run: its id, and after a space, its arguments as a JSON object.
void runCommand(Scribe& s, PrStringView command) {
    size_t space = prStringFindByte(command, ' ');
    PrStringView id = prStringHead(command, space);
    PrJson* arguments = nullptr;
    PrCommandArgs args{};
    if (space != command.size) {
        arguments = prJsonCreate();
        if (!prJsonParse(arguments, prStringFrom(command, space + 1), 0, nullptr)) {
            printError("scribe: the arguments of {} are not JSON\n", id);
            prJsonDestroy(arguments);
            return;
        }
        args.document = arguments;
    }
    PrRawString error{};
    if (prCommandRegistryRun(s.commands, id, arguments != nullptr ? &args : nullptr, &error) != PrStatus_Ok)
        printError("scribe: cannot run {}: {}\n", id, prRawStringView(&error));
    prRawStringReset(&error);
    prJsonDestroy(arguments);
}

// Reads the input of --move or --click from `text`, which is `x,y`, or for a click `x,y,button`. Returns false if the
// text is not that.
bool parsePointer(PrStringView text, bool click, PointerInput* out) {
    double values[3] = {0, 0, 0};
    uint32_t count = 0;
    size_t at = 0;
    while (count < 3) {
        size_t used = prF64ParsePrefix(prStringFrom(text, at), &values[count]);
        if (used == 0)
            return false;
        at += used;
        ++count;
        if (at == text.size || text.data[at] != ',')
            break;
        ++at;
    }
    if (at != text.size || count < 2 || (count == 3 && (!click || values[2] < 0 || values[2] > 7)))
        return false;
    *out = PointerInput{
        .at = PrVec2{float(values[0]), float(values[1])},
        .click = click,
        .button = uint8_t(count == 3 ? values[2] : 0),
    };
    return true;
}

// Queues the input of --move or --click in the window's context, which routes it in the window's next frames.
void queuePointer(Scribe& s, const PointerInput& input) {
    PrAppEvent move{.kind = PrAppEventKind_PointerMove, .pointer = {.position = input.at}};
    prUiPushEvent(s.ui, kPrUiViewportMain, &move);
    if (!input.click)
        return;
    PrAppEvent down{
        .kind = PrAppEventKind_PointerDown,
        .pointer = {.position = input.at, .button = input.button, .clicks = 1},
    };
    prUiPushEvent(s.ui, kPrUiViewportMain, &down);
    PrAppEvent up{.kind = PrAppEventKind_PointerUp, .pointer = {.position = input.at, .button = input.button}};
    prUiPushEvent(s.ui, kPrUiViewportMain, &up);
}

void wakeForCapture(void* PR_NULLABLE context);

// Makes the editor over the window's kit and its monospace font, and opens the command line's documents.
bool open(void* context, PrStudioWindow* window) {
    Run& r = runOf(context);
    Scribe& s = r.scribe;
    PrFontFamily mono{.generic = PrFontGeneric_Monospace};
    PrFontStyle style = prFontStyleDefault();
    style.size = 13;
    PrTextFontDesc font{.set = prStudioWindowFonts(window), .families = &mono, .familyCount = 1, .style = style};
    s.font = prTextFontCreate(&font);
    s.fonts = prStudioWindowFonts(window);
    s.shaper = prStudioWindowShaper(window);
    s.kit = prStudioWindowKit(window);
    s.ui = prStudioWindowUi(window);
    s.app = prStudioWindowApp(window);
    r.window = window;
    s.window = window;
    // A capture is a look at the window, and leaves the last project as it was.
    s.statePath = r.capture.isEmpty() ? defaultStatePath() : String();
    openEditor(s);
    if (!r.debuggee.isEmpty())
        setDebuggee(s, r.debuggee);
    bool opened = false;
    if (r.diff) {
        opened = openDiff(s, r.compared, r.path);
    } else if (!r.path.isEmpty() && exists(r.path)) {
        opened = openArgument(s, r.path);
    } else if (String last = lastProject(s); r.path.isEmpty() && !last.isEmpty()) {
        // A launch without a path reopens the last project.
        opened = openProject(s, last);
    } else {
        opened = openDocument(s, r.path);
    }
    for (PrStringView more : r.more) {
        if (!openArgument(s, more))
            printError("scribe: cannot read {}\n", more);
    }
    for (PrStringView command : r.commands)
        runCommand(s, command);
    for (const PointerInput& input : r.inputs)
        queuePointer(s, input);
    if (!r.capture.isEmpty())
        prThreadStart(&r.captureWaker, wakeForCapture, &r);
    if (!opened && r.diff)
        printError("scribe: cannot read {} or {}\n", r.compared, r.path);
    else if (!opened)
        printError("scribe: cannot read {}\n", r.path);
    return opened;
}

// Writes the pixels of the frame that the window captured to the PNG of --capture, and wakes the application, which
// then quits. Called on the GPU device's thread.
void writeCapture(void* PR_NULLABLE context, PrPixelView rgba) {
    Run& r = runOf(context);
    bool written = false;
    if (rgba.data != nullptr && r.images != nullptr) {
        size_t size = 0;
        prImageCodecSetEncode(r.images, "image/png", rgba, nullptr, 0, &size);
        Array<uint8_t> png;
        png.resize(uint32_t(size));
        written = prImageCodecSetEncode(r.images, "image/png", rgba, png.data(), png.count(), &size) == PrStatus_Ok &&
            prFileWrite(r.capture, PrByteView{png.data(), size});
    }
    if (!written)
        printError("scribe: cannot write {}\n", r.capture);
    r.captured.store(true);
    prAppWake(r.scribe.app);
}

// Wakes the application once the window has shown for a while, which then captures its next frame for --capture. The
// wake comes whether or not the display shows the window.
void wakeForCapture(void* PR_NULLABLE context) {
    Run& r = runOf(context);
    prThreadSleep(kCaptureSeconds * 1000);
    r.captureDue.store(true);
    prAppWake(r.scribe.app);
}

// Takes up a wake: captures the window's next frame once --capture is due, and quits once it is written.
void appEvent(void* PR_NULLABLE context, PrStudioApp* app, const PrAppEvent* event) {
    Run& r = runOf(context);
    if (r.captured.load()) {
        prStudioAppStop(app);
        return;
    }
    if (r.captureDue.exchange(false) && r.window != nullptr)
        prStudioWindowCapture(r.window, writeCapture, &r);
    handleEvent(r.scribe, *event);
}

void trace(void*, const PrStudioWindowFrameTimes* t) {
    if (t->total <= kTraceSlowFrame && t->gap <= kTraceLongGap && t->inputWait <= kTraceLongGap)
        return;
    printError("frame {:.0f}x{:.0f}: total {:.1f} (wait {:.1f}, rounds {} {:.1f}, scene {:.1f}, play {:.1f}, "
        "render {:.1f}), gap {:.1f}, input waited {:.1f}\n", t->extent.x, t->extent.y, t->total, t->wait, t->rounds,
        t->build, t->scene, t->play, t->render, t->gap, t->inputWait);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    static Run r;
    bool tracing = false;
    for (int i = 1; i < argc; ++i) {
        PrStringView argument = argv[i];
        if (argument == "--trace") {
            tracing = true;
        } else if (argument == "--capture" && i + 1 < argc) {
            r.capture = argv[++i];
        } else if (argument == "--debug" && i + 1 < argc) {
            r.debuggee = argv[++i];
        } else if (argument == "--run" && i + 1 < argc) {
            r.commands.push(PrStringView(argv[++i]));
        } else if ((argument == "--move" || argument == "--click") && i + 1 < argc) {
            PointerInput input{};
            if (!parsePointer(argv[++i], argument == "--click", &input)) {
                printError("scribe: {} takes x,y{}\n", argument, argument == "--click" ? "[,button]" : "");
                return 2;
            }
            r.inputs.push(input);
        } else if (argument == "--diff" && i + 2 < argc) {
            r.diff = true;
            r.compared = argv[++i];
            r.path = argv[++i];
        } else if (r.path.isEmpty()) {
            r.path = argument;
        } else if (!r.diff && !prStringStartsWith(argument, "--")) {
            r.more.push(argument);
        } else {
            printError("usage: scribe [--trace] [--capture png] [--debug program] [--run command]... "
                "[--move x,y | --click x,y[,button]]... [file | folder | --diff old new] [file...]\n");
            return 2;
        }
    }
    // The text is set in Noto Sans Mono, and the terminal adds icons from Nerd Fonts. Everything else uses the system's
    // fonts.
    PrFontSet* fonts = prFontSetCreate(nullptr);
    const PrFontBundle bundles[] = {PrFontBundle_NotoMono, PrFontBundle_NerdFontSymbols};
    if (prFontBundleInstall(fonts, bundles, uint32_t(countOf(bundles))) == 0) {
        printError("scribe: the font bundles among its resources could not be installed\n");
        prFontSetDestroy(fonts);
        return 1;
    }
    PrFontSystem* system = prFontSystemCreate(PrFontSystemBackend_Default);
    prFontSetSetSystem(fonts, system);
    PrStudioWindowDesc desc{
        .title = "Scribe",
        .extent = {1200, 900},
        .open = open,
        .event = [](void* context, PrStudioWindow*, const PrAppEvent* event) {
            return handleWindowEvent(runOf(context).scribe, *event);
        },
        .build = [](void* context, PrStudioWindow*, PrLayoutNode root, PrVec2) {
            buildScribe(runOf(context).scribe, root);
        },
        .laidOut = [](void* context, PrStudioWindow*) { return afterLayout(runOf(context).scribe); },
        .close = [](void* context, PrStudioWindow*) {
            runOf(context).window = nullptr;
            Scribe& s = runOf(context).scribe;
            if (s.editor != nullptr)
                closeEditor(s);
        },
        .trace = tracing ? trace : nullptr,
        .context = &r,
    };
    r.compress = prCompressCodecSetCreateDefault();
    r.images = r.compress != nullptr ? prImageCodecSetCreateDefault(r.compress) : nullptr;
    PrStudioAppDesc app{
        .fonts = fonts,
        .compress = r.compress,
        .images = r.images,
        .window = &desc,
        .event = appEvent,
        .context = &r,
    };
    PrStatus status = prStudioAppRun(&app);
    if (!r.capture.isEmpty())
        prThreadJoin(&r.captureWaker);
    prImageCodecSetDestroy(r.images);
    prCompressCodecSetDestroy(r.compress);
    prFontSetDestroy(fonts);
    prFontSystemDestroy(system);
    if (status == PrStatus_NotSupported)
        printError("scribe: no GPU device to render with\n");
    return status == PrStatus_Ok ? 0 : 1;
}
