// Pandite's view.

#include "apps/pandite/view.h"

#include "pr/blob.h"
#include "pr/command_ui.h"
#include "pr/construct.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/format.h"
#include "pr/string_ops.h"

#include "apps/pandite/icons.h"
#include "apps/pandite/theme.h"

namespace pandite {

namespace {

// How long a message shows at the bottom of the window, and how long it then takes to fade, in seconds.
constexpr double kToastHold = 2.2;
constexpr double kToastFade = 0.35;

// The toolbar's sizes, in px.
constexpr float kToolbarHeight = 40;
constexpr float kButton = 32;
constexpr float kIconSize = 20;

// How long the pointer rests on a toolbar button before its tooltip shows, in seconds.
constexpr double kTooltipDelay = 0.55;

// How long the pointer rests before the toolbar hides, and how long the toolbar takes to fade in and out, in seconds.
constexpr double kIdleHide = 2.2;
constexpr float kFadeIn = 0.09f;
constexpr float kFadeOut = 0.25f;

// How long an image loads before the view shows the loading notice. The delay keeps a fast load from flashing the
// notice.
constexpr double kLoadingDelay = 0.15;

// The zoom factor of a key or of one notch of the wheel, the px of the wheel per notch, and the step of a pan key in
// px.
constexpr float kZoomStep = 1.2f;
constexpr float kWheelNotch = 100;
constexpr float kPanStep = 80;

// The number of images on each side of the image on screen that decode ahead.
constexpr uint32_t kAhead = 2;

// The index that stands for no row of a list.
constexpr uint32_t kNoRow = 0xffffffffu;

// The side of a thumbnail, the gap, the padding and the height of the strip for a gallery size, in px.
float cellOf(GallerySize size) {
    return size == GallerySize_Small ? 64 : size == GallerySize_Medium ? 96 : 128;
}
float gapOf(GallerySize size) {
    return cellOf(size) * (6.0f / 64);
}
float padOf(GallerySize size) {
    return cellOf(size) * (10.0f / 64);
}
float stripOf(GallerySize size) {
    return cellOf(size) + 2 * padOf(size);
}

// ---------------------------------------------------------------------------------------------------------------------
// Commands

// The buttons of the toolbar and their icons. Separators part the buttons into groups.
struct Button {
    Command command;
    Icon icon;
};
constexpr Button kFileGroup[] = {
    {Command_Open, Icon_Open},
    {Command_SaveAs, Icon_SaveAs},
    {Command_CopyPath, Icon_CopyPath},
};
constexpr Button kStepGroup[] = {{Command_Previous, Icon_Previous}, {Command_Next, Icon_Next}};
constexpr Button kZoomGroup[] = {
    {Command_ZoomOut, Icon_ZoomOut},
    {Command_ZoomIn, Icon_ZoomIn},
    {Command_Fit, Icon_Fit},
    {Command_ActualSize, Icon_ActualSize},
};
constexpr Button kEditGroup[] = {
    {Command_Undo, Icon_Undo},
    {Command_RotateLeft, Icon_RotateLeft},
    {Command_RotateRight, Icon_RotateRight},
    {Command_FlipHorizontal, Icon_FlipHorizontal},
    {Command_FlipVertical, Icon_FlipVertical},
    {Command_Crop, Icon_Crop},
    {Command_Resize, Icon_Resize},
};
constexpr Button kWindowGroup[] = {{Command_ToggleGallery, Icon_Gallery}, {Command_Fullscreen, Icon_Fullscreen}};

struct ButtonGroup {
    const Button* buttons;
    uint32_t count;
};
constexpr ButtonGroup kGroups[] = {
    {kFileGroup, countOf(kFileGroup)},
    {kStepGroup, countOf(kStepGroup)},
    {kEditGroup, countOf(kEditGroup)},
    {kZoomGroup, countOf(kZoomGroup)},
    {kWindowGroup, countOf(kWindowGroup)},
};

uint32_t imageCount(const Pandite& pandite) {
    return pandite.folder.names.count();
}

// Shows the image at `index`, fitted. `index` is clamped to the folder.
void show(Pandite& pandite, uint32_t index, double time) {
    uint32_t count = imageCount(pandite);
    if (count == 0)
        return;
    index = index >= count ? count - 1 : index;
    if (index == pandite.index)
        return;
    pandite.index = index;
    viewFit(pandite.view);
    pandite.shownAt = time;
    pandite.revealCurrent = true;
}

// Returns the number of thumbnails that fit whole in the gallery strip of a window that is `width` px wide.
uint32_t pageOf(const Pandite& pandite, float width) {
    float cell = cellOf(pandite.gallerySize);
    float gap = gapOf(pandite.gallerySize);
    float room = width - 2 * padOf(pandite.gallerySize) + gap;
    uint32_t page = room > 0 ? uint32_t(room / (cell + gap)) : 0;
    return page > 0 ? page : 1;
}

bool isEnabled(const Pandite& pandite, Command command, const ViewFrame& frame) {
    uint32_t count = imageCount(pandite);
    // Crop mode keeps the commands that neither change the image nor leave it. The commands of crop mode work only in
    // crop mode.
    CommandWhere where = commandWhere(command);
    if ((where & (pandite.editor.cropping ? CommandWhere_Crop : CommandWhere_Viewer)) == 0)
        return false;
    switch (command) {
        case Command_Previous:
        case Command_First:
        case Command_PageBack: return count > 0 && pandite.index > 0;
        case Command_Next:
        case Command_Last:
        case Command_PageForward: return count > 0 && pandite.index + 1 < count;
        case Command_Fit:
        case Command_ActualSize:
        case Command_ZoomIn:
        case Command_ZoomOut:
        case Command_PanUp:
        case Command_PanDown:
        case Command_PanLeft:
        case Command_PanRight: return frame.image.x > 0;
        case Command_ToggleGallery: return count > 0;
        case Command_SaveAs:
        case Command_RotateLeft:
        case Command_RotateRight:
        case Command_FlipHorizontal:
        case Command_FlipVertical: return count > 0 && pandite.index != kNoImage;
        // Crop mode and the resize dialog measure the image as the edits so far leave it, so they wait for those edits.
        case Command_Crop:
        case Command_Resize: return frame.image.x > 0 && !editIsBusy(pandite.editor.stack);
        case Command_Undo: return editorIsEdited(pandite);
        case Command_Compare: return compareEnabled(pandite);
        case Command_Peek: return frame.image.x > 0 || pandite.compare.on;
        case Command_ToggleInfo: return count > 0 && pandite.index != kNoImage;
        case Command_CropApply: {
            PrIRect pixels{};
            return cropPixels(pandite.editor.crop, &pixels);
        }
        case Command_CopyImage:
        case Command_CopyPath:
        case Command_SelectImage:
        case Command_SelectPrevious:
        case Command_SelectNext:
        case Command_MoveTo:
        case Command_CopyTo:
        case Command_Trash:
        case Command_Delete: return count > 0 && pandite.index != kNoImage;
        case Command_ClearSelection: return pandite.selected.count() != 0;
        case Command_LargerThumbnails: return pandite.galleryVisible && pandite.gallerySize != GallerySize_Large;
        case Command_SmallerThumbnails: return pandite.galleryVisible && pandite.gallerySize != GallerySize_Small;
        default: return true;
    }
}

void openDialog(Pandite& pandite) {
    if (pandite.app == nullptr)
        return;
    PrAppFileType types[] = {{.name = "Images", .extensions = "png,jpg,jpeg,gif,webp,tif,tiff,ico,exr,hdr,ktx2,qoi,tga,bmp,dib,dds,psd,psb"}};
    PrAppFileDialogDesc desc{
        .kind = PrAppFileDialogKind_Open,
        .parent = pandite.window != nullptr ? prStudioWindowAppWindow(pandite.window) : nullptr,
        .title = "Open",
        .directory = pandite.folder.directory,
        .types = types,
        .typeCount = 1,
    };
    prAppFileDialog(pandite.app, &desc, [](void* PR_NULLABLE context, PrStatus status, PrAppDataSource source) {
        Pandite& p = *static_cast<Pandite*>(context);
        if (status != PrStatus_Ok || p.app == nullptr)
            return;
        prAppDataReadFiles(p.app, source, [](void* PR_NULLABLE inner, PrStatus read, const PrAppDataFile* PR_NULLABLE files,
            uint32_t count) {
                if (read == PrStatus_Ok && files != nullptr && count != 0)
                    panditeOpen(*static_cast<Pandite*>(inner), files[0].path);
            }, &p);
    }, &pandite);
}

// Shows `text` at the bottom of the window for a while.
void say(Pandite& pandite, String text) {
    panditeSay(pandite, move(text));
}

// Returns the names of the files that an action takes: the selected images in the order of the folder, or the image on
// screen if none is selected.
Array<String> targets(const Pandite& pandite) {
    Array<String> names;
    for (const String& name : pandite.folder.names) {
        if (pandite.selected.contains(name))
            names.push(name);
    }
    if (names.count() == 0 && pandite.index != kNoImage)
        names.push(pandite.folder.names[pandite.index]);
    return names;
}

// Removes the files `names` from the folder, the selection and the loader. Shows the image that takes the place of the
// image on screen.
void removeFiles(Pandite& pandite, Span<const String> names) {
    for (const String& name : names) {
        uint32_t at = folderFind(pandite.folder, name);
        if (at != kNoImage)
            loaderForget(pandite.loader, folderPath(pandite.folder, at));
        pandite.selected.erase(name);
    }
    String shown = pandite.index != kNoImage ? pandite.folder.names[pandite.index] : String();
    uint32_t next = folderRemove(pandite.folder, names, pandite.index);
    pandite.index = next;
    if (next != kNoImage && pandite.folder.names[next] != shown) {
        viewFit(pandite.view);
        pandite.shownAt = pandite.time;
    }
    pandite.revealCurrent = true;
}

// Returns a count of images as a phrase: "1 image", "3 images".
String imagesText(uint32_t count) {
    return count == 1 ? String("1 image") : format("{} images", count);
}

// Returns why an action on a file failed, in the words of a message.
PrStringView reasonOf(PrStatus status) {
    switch (status) {
        case PrStatus_NotAllowed: return "a file with that name already exists in the folder";
        case PrStatus_NotFound: return "the file no longer exists";
        default: return "the system refused";
    }
}

// Shows a message that says what an action did. The message gives `verb` and the number of images that the action took.
// If the action failed on an image, the message names the first such image, followed by `failed` and why. It then gives
// the number of other images that the action failed on.
void sayOutcome(Pandite& pandite, const FileOutcome& outcome, PrStringView verb, PrStringView failed) {
    String text;
    if (outcome.done.count() != 0)
        text = format("{} {}", verb, imagesText(outcome.done.count()));
    if (!outcome.failed.isEmpty()) {
        if (!text.isEmpty())
            text.append(". ");
        text.append(format("{} {}: {}", outcome.failed, failed, reasonOf(outcome.failure)));
        if (outcome.otherFailures != 0)
            text.append(format(", and {} more", imagesText(outcome.otherFailures)));
    }
    say(pandite, move(text));
}

// Moves the files `names` to the trash, each on its own. Removes from the folder the files that went.
void trashFiles(Pandite& pandite, Span<const String> names) {
    FileOutcome outcome{};
    for (const String& name : names) {
        uint32_t at = folderFind(pandite.folder, name);
        PrStatus status = PrStatus_NotFound;
        if (pandite.app != nullptr && at != kNoImage)
            status = prAppTrashPath(pandite.app, folderPath(pandite.folder, at));
        if (status == PrStatus_Ok) {
            outcome.done.push(name);
        } else if (outcome.failed.isEmpty()) {
            outcome.failed = name;
            outcome.failure = status;
        } else {
            outcome.otherFailures += 1;
        }
    }
    removeFiles(pandite, outcome.done.span());
    // A file that went without a question needs no message. A file that did not go gets a message that says why.
    if (!outcome.failed.isEmpty())
        sayOutcome(pandite, outcome, "Moved", "did not move");
}

// Asks before an action on `names`: a move of the selected images to the trash, or a permanent delete.
void ask(Pandite& pandite, Array<String> names, bool permanent) {
    uint32_t count = names.count();
    Confirm confirm{.open = true, .focus = true};
    if (permanent) {
        confirm.message = format("Delete {} permanently?", count == 1 ? names[0] : imagesText(count));
        confirm.act = "Delete";
        confirm.action = FileAction_Delete;
    } else {
#if defined(_WIN32)
        confirm.message = format("Move {} to the Recycle Bin?", imagesText(count));
#else
        confirm.message = format("Move {} to the Trash?", imagesText(count));
#endif
        confirm.act = "Move";
        confirm.trash = true;
        // A move to the trash can be taken back, so the button that acts is the button that Enter presses.
        confirm.acting = true;
    }
    confirm.names = move(names);
    pandite.confirm = move(confirm);
}

// Runs the action that the confirm dialog asked about.
void confirmAction(Pandite& pandite) {
    Confirm confirm = move(pandite.confirm);
    pandite.confirm = Confirm{};
    if (confirm.trash) {
        trashFiles(pandite, confirm.names.span());
        return;
    }
    if (confirm.replace) {
        editorSave(pandite, confirm.path);
        return;
    }
    FileOutcome outcome = fileAct(pandite.folder, confirm.names.span(), FileAction_Delete, "");
    removeFiles(pandite, outcome.done.span());
    sayOutcome(pandite, outcome, "Deleted", "was not deleted");
}

// The context that the folder picker of a move or a copy carries to its answer.
struct Destination {
    Pandite* pandite;
    FileAction action;
};

// Moves or copies the selected images, or the image on screen, to the folder that the user picks.
void moveOrCopy(Pandite& pandite, FileAction action) {
    if (pandite.app == nullptr)
        return;
    static Destination destination;
    destination = Destination{.pandite = &pandite, .action = action};
    PrAppFileDialogDesc desc{
        .kind = PrAppFileDialogKind_Open,
        .flags = PrAppFileDialogFlag_Directories,
        .parent = pandite.window != nullptr ? prStudioWindowAppWindow(pandite.window) : nullptr,
        .title = action == FileAction_Move ? "Move to" : "Copy to",
        .directory = pandite.folder.directory,
    };
    prAppFileDialog(pandite.app, &desc, [](void* PR_NULLABLE context, PrStatus status, PrAppDataSource source) {
        Destination& d = *static_cast<Destination*>(context);
        if (status != PrStatus_Ok || d.pandite->app == nullptr)
            return;
        prAppDataReadFiles(d.pandite->app, source, [](void* PR_NULLABLE inner, PrStatus read,
            const PrAppDataFile* PR_NULLABLE files, uint32_t count) {
                Destination& dest = *static_cast<Destination*>(inner);
                Pandite& p = *dest.pandite;
                if (read != PrStatus_Ok || files == nullptr || count == 0)
                    return;
                PrStringView to = files[0].path;
                if (prFileSame(p.folder.directory, to)) {
                    say(p, "The images are already in that folder");
                    return;
                }
                Array<String> names = targets(p);
                FileOutcome outcome = fileAct(p.folder, names.span(), dest.action, to);
                if (dest.action == FileAction_Move) {
                    removeFiles(p, outcome.done.span());
                    sayOutcome(p, outcome, "Moved", "did not move");
                } else {
                    sayOutcome(p, outcome, "Copied", "was not copied");
                }
                if (p.app != nullptr)
                    prAppWake(p.app);
            }, &d);
    }, &destination);
}

// Decodes the image of a copy to the clipboard, upright, and wakes the application. Runs on the pool.
void runCopy(void* PR_NULLABLE context) {
    ImageCopy& copy = *static_cast<ImageCopy*>(context);
    copy.status = bitmapDecode(copy.codecs, copy.path, &copy.bitmap);
    copy.done.store(true);
    if (copy.app != nullptr)
        prAppWake(copy.app);
}

// Puts `bitmap` on the clipboard, and says how it went.
void putOnClipboard(Pandite& pandite, const Bitmap& bitmap, PrStatus status) {
    if ((status == PrStatus_Ok || status == PrStatus_Truncated) && pandite.app != nullptr) {
        PrAppDataContent content{.forms = PrAppDataFormFlag_Image, .image = bitmapView(bitmap)};
        status = prAppDataWrite(pandite.app, kPrAppDataSourceClipboard, &content);
    }
    if (status == PrStatus_Ok || status == PrStatus_Truncated)
        say(pandite, "Image copied");
    else
        say(pandite, format("Cannot copy the image: {}", prStatusName(status)));
}

// Copies the image on screen to the clipboard as pixels, at its full resolution. Copies the edited image if there is
// one. Otherwise decodes the file on the pool.
void copyImage(Pandite& pandite) {
    if (pandite.copying != nullptr)
        return;
    if (const Bitmap* edited = editorEdited(pandite)) {
        putOnClipboard(pandite, *edited, PrStatus_Ok);
        return;
    }
    ImageCopy* copy = create<ImageCopy>();
    copy->path = folderPath(pandite.folder, pandite.index);
    copy->codecs = pandite.codecs;
    copy->app = pandite.app;
    pandite.copying = copy;
    say(pandite, "Copying image...");
    prTaskPoolPost(pandite.pool, runCopy, copy);
}

// Reads the facts of an image file for the information panel and wakes the application. Runs on the pool.
void runInfo(void* PR_NULLABLE context) {
    InfoRead& read = *static_cast<InfoRead*>(context);
    factsRead(read.codecs, read.path, &read.facts);
    read.done.store(true);
    if (read.app != nullptr)
        prAppWake(read.app);
}

// Takes a finished read of facts. Starts the read of the image on screen if the panel shows and holds the facts of
// another image. One read runs at a time.
void updateInfo(Pandite& pandite) {
    InfoPanel& info = pandite.info;
    InfoRead* read = info.reading;
    if (read != nullptr) {
        if (!read->done.load())
            return;
        info.reading = nullptr;
        info.name = move(read->name);
        info.facts = move(read->facts);
        destroy(read);
    }
    if (!info.visible || pandite.index == kNoImage || info.name == pandite.folder.names[pandite.index])
        return;
    read = create<InfoRead>();
    read->name = pandite.folder.names[pandite.index];
    read->path = folderPath(pandite.folder, pandite.index);
    read->codecs = pandite.codecs;
    read->app = pandite.app;
    info.reading = read;
    prTaskPoolPost(pandite.pool, runInfo, read);
}

// Puts a decoded copy on the clipboard once its decode is done.
void finishCopy(Pandite& pandite) {
    ImageCopy* copy = pandite.copying;
    if (copy == nullptr || !copy->done.load())
        return;
    pandite.copying = nullptr;
    putOnClipboard(pandite, copy->bitmap, copy->status);
    destroy(copy);
}

// Returns the extent of the image on screen, as edited, or zero before it opens.
PrUVec2 extentOf(const ViewFrame& frame) {
    return PrUVec2{uint32_t(frame.image.x), uint32_t(frame.image.y)};
}

// Selects the image on screen and steps by `step`. In a run that began on a selected image, clears the selection of the
// image on screen instead. The view stays at the first and the last image.
void selectAndStep(Pandite& pandite, int32_t step) {
    const String& name = pandite.folder.names[pandite.index];
    if (!pandite.selectRun) {
        pandite.selectRun = true;
        pandite.selectClears = pandite.selected.contains(name);
    }
    if (pandite.selectClears)
        pandite.selected.erase(name);
    else
        pandite.selected.add(name);
    uint32_t count = imageCount(pandite);
    if (step < 0 && pandite.index > 0)
        show(pandite, pandite.index - 1, pandite.time);
    else if (step > 0 && pandite.index + 1 < count)
        show(pandite, pandite.index + 1, pandite.time);
}

void toggleFullscreen(Pandite& pandite) {
    if (pandite.window == nullptr)
        return;
    PrAppWindow* window = prStudioWindowAppWindow(pandite.window);
    bool full = prAppWindowState(window) == PrAppWindowState_Fullscreen;
    prAppWindowSetState(window, full ? PrAppWindowState_Normal : PrAppWindowState_Fullscreen);
    pandite.fullscreen = !full;
}

void run(Pandite& pandite, Command command, const ViewFrame& frame, double time) {
    if (!isEnabled(pandite, command, frame))
        return;
    PrVec2 center{frame.viewport.x * 0.5f, frame.viewport.y * 0.5f};
    float step = kPanStep * frame.ratio;
    uint32_t page = pageOf(pandite, frame.stage.x);
    switch (command) {
        case Command_Open: openDialog(pandite); break;
        case Command_SaveAs: editorSaveAs(pandite); break;
        case Command_Undo: editorUndo(pandite); break;
        case Command_Compare: compareToggle(pandite); break;
        case Command_Peek: pandite.peek = !pandite.peek; break;
        case Command_ToggleInfo: pandite.info.visible = !pandite.info.visible; break;
        case Command_RotateLeft: editorPush(pandite, Edit{.kind = EditKind_RotateLeft}); break;
        case Command_RotateRight: editorPush(pandite, Edit{.kind = EditKind_RotateRight}); break;
        case Command_FlipHorizontal: editorPush(pandite, Edit{.kind = EditKind_FlipHorizontal}); break;
        case Command_FlipVertical: editorPush(pandite, Edit{.kind = EditKind_FlipVertical}); break;
        case Command_Crop: editorStartCrop(pandite, extentOf(frame)); break;
        case Command_Resize: editorOpenResize(pandite, extentOf(frame)); break;
        case Command_CropApply: editorApplyCrop(pandite); break;
        case Command_CropSelectAll: cropSelectAll(pandite.editor.crop); break;
        case Command_CropCancel: editorCancelCrop(pandite); break;
        case Command_CopyImage: copyImage(pandite); break;
        case Command_CopyPath:
            if (pandite.app != nullptr && prAppDataWriteText(pandite.app, kPrAppDataSourceClipboard,
                    folderPath(pandite.folder, pandite.index)) == PrStatus_Ok)
                say(pandite, "Path copied");
            break;
        case Command_SelectImage: {
            const String& name = pandite.folder.names[pandite.index];
            if (!pandite.selected.erase(name))
                pandite.selected.add(name);
            break;
        }
        case Command_ClearSelection: pandite.selected.clear(); break;
        case Command_SelectPrevious: selectAndStep(pandite, -1); break;
        case Command_SelectNext: selectAndStep(pandite, 1); break;
        case Command_MoveTo: moveOrCopy(pandite, FileAction_Move); break;
        case Command_CopyTo: moveOrCopy(pandite, FileAction_Copy); break;
        case Command_Trash:
            // The image on screen goes to the trash without a question, because it can come back. A selection asks
            // first.
            if (pandite.selected.count() == 0)
                trashFiles(pandite, targets(pandite).span());
            else
                ask(pandite, targets(pandite), false);
            break;
        case Command_Delete: ask(pandite, targets(pandite), true); break;
        case Command_Previous: show(pandite, pandite.index - 1, time); break;
        case Command_Next: show(pandite, pandite.index + 1, time); break;
        case Command_First: show(pandite, 0, time); break;
        case Command_Last: show(pandite, imageCount(pandite) - 1, time); break;
        case Command_PageBack: show(pandite, pandite.index > page ? pandite.index - page : 0, time); break;
        case Command_PageForward: show(pandite, pandite.index + page, time); break;
        case Command_Fit: viewFit(pandite.view); break;
        case Command_ActualSize: viewOneToOne(pandite.view, frame.image, frame.viewport, center); break;
        case Command_ZoomIn: viewZoomBy(pandite.view, kZoomStep, frame.image, frame.viewport, center); break;
        case Command_ZoomOut: viewZoomBy(pandite.view, 1 / kZoomStep, frame.image, frame.viewport, center); break;
        case Command_PanUp: viewPanBy(pandite.view, PrVec2{0, step}, frame.image, frame.viewport); break;
        case Command_PanDown: viewPanBy(pandite.view, PrVec2{0, -step}, frame.image, frame.viewport); break;
        case Command_PanLeft: viewPanBy(pandite.view, PrVec2{step, 0}, frame.image, frame.viewport); break;
        case Command_PanRight: viewPanBy(pandite.view, PrVec2{-step, 0}, frame.image, frame.viewport); break;
        case Command_ToggleGallery:
            pandite.galleryVisible = !pandite.galleryVisible;
            pandite.revealCurrent = true;
            break;
        case Command_LargerThumbnails:
            pandite.gallerySize = GallerySize(pandite.gallerySize + 1);
            pandite.revealCurrent = true;
            break;
        case Command_SmallerThumbnails:
            pandite.gallerySize = GallerySize(pandite.gallerySize - 1);
            pandite.revealCurrent = true;
            break;
        case Command_Fullscreen: toggleFullscreen(pandite); break;
        case Command_ContextMenu:
            pandite.menu = ContextMenu{.open = true, .centered = true, .highlight = kNoRow};
            break;
        case Command_Shortcuts:
            pandite.shortcuts = Shortcuts{.open = true, .focus = true};
            break;
        case Command_Quit: pandite.quit = true; break;
        default: break;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The context menu

// The value that marks a separator between the groups of items of a menu.
constexpr Command kNoCommand = Command_Count;

// Crop mode's context menu.
constexpr Command kCropMenuItems[] = {
    Command_CropApply,
    Command_CropSelectAll,
    Command_CropCancel,
    kNoCommand,
    Command_Fit,
    Command_ActualSize,
    Command_ZoomIn,
    Command_ZoomOut,
};

// The viewer's context menu.
constexpr Command kMenuItems[] = {
    Command_Open,
    Command_SaveAs,
    Command_CopyImage,
    Command_CopyPath,
    kNoCommand,
    Command_Undo,
    kNoCommand,
    Command_SelectImage,
    Command_ClearSelection,
    Command_MoveTo,
    Command_CopyTo,
    Command_Trash,
    Command_Delete,
    kNoCommand,
    Command_RotateLeft,
    Command_RotateRight,
    Command_FlipHorizontal,
    Command_FlipVertical,
    Command_Crop,
    Command_Resize,
    kNoCommand,
    Command_Fit,
    Command_ActualSize,
    Command_ZoomIn,
    Command_ZoomOut,
    kNoCommand,
    Command_Previous,
    Command_Next,
    kNoCommand,
    Command_ToggleGallery,
    Command_LargerThumbnails,
    Command_SmallerThumbnails,
    Command_Fullscreen,
    Command_Shortcuts,
    kNoCommand,
    Command_Quit,
};

// Returns the text of the first chord that binds `command` in the viewer's context, as the platform shows it, or the
// null string if none does.
String firstChordOf(const Pandite& pandite, Command command) {
    PrStringView id = commandId(command);
    const PrCommandKeymap* keymap = pandite.commands->keymap;
    for (uint32_t i = 0; i < prCommandKeymapCount(keymap); ++i) {
        PrCommandBinding binding = prCommandKeymapBinding(keymap, i);
        if (binding.commandId == id && binding.context == commandContext(command))
            return String::adopt(prCommandChordText(binding.chord));
    }
    return String();
}

// Returns the next enabled item of the menu after `from` in the direction of `step`, wrapping around. Returns kNoRow if
// no item is enabled.
uint32_t stepItem(const bool* enabled, uint32_t count, uint32_t from, int32_t step) {
    uint32_t at = from == kNoRow ? (step > 0 ? count - 1 : 0) : from;
    for (uint32_t tried = 0; tried < count; ++tried) {
        at = (at + count + uint32_t(step)) % count;
        if (enabled[at])
            return at;
    }
    return kNoRow;
}

// Returns the items of the context menu: the items of crop mode in crop mode, and the items of the viewer otherwise.
Span<const Command> menuItems(const Pandite& pandite) {
    return pandite.editor.cropping ? Span<const Command>(kCropMenuItems) : Span<const Command>(kMenuItems);
}

// Declares the context menu, which floats at the pointer over the page. Each item shows its check and its first chord.
// The pointer or Up and Down move the highlight. A click or Enter runs an item. Escape or a press outside the menu
// closes it.
void contextMenu(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const ViewFrame& frame) {
    ContextMenu& menu = pandite.menu;
    PrVec2 at = menu.at;
    if (menu.centered)
        at = PrVec2{frame.stage.x * 0.5f, frame.stage.y * 0.5f};
    PrAppPopupPlacement placement{
        .anchor = PrRect{at.x, at.y, at.x, at.y},
        .side = PrAppPopupSide_Below,
        .align = PrAppPopupAlign_Start,
        .adjust = PrAppPopupAdjustFlag_Flip | PrAppPopupAdjustFlag_Slide,
    };
    UiElement popup = ui.popup(page, 0x6d656e75, placement);
    popup.w(280).flexCol().py(4).bg(kPanelSolid).border(1).borderColor(kBorder).rounded(6).shadowLg().textSm();
    bool close = popup.dismissed(kPrLayoutNodeNone);
    bool up = popup.keyPressed(PrAppKey_Up, 0, true);
    bool down = popup.keyPressed(PrAppKey_Down, 0, true);
    bool enter = popup.keyPressed(PrAppKey_Enter);

    const PrCommandRegistry* registry = pandite.commands->registry;
    Span<const Command> items = menuItems(pandite);
    uint32_t count = items.count();
    bool enabled[countOf(kMenuItems)];
    for (uint32_t i = 0; i < count; ++i) {
        Command command = items[i];
        enabled[i] = command != kNoCommand && (prCommandRegistryState(registry, command) & PrCommandStateFlag_Disabled) == 0;
    }
    if (up)
        menu.highlight = stepItem(enabled, count, menu.highlight, -1);
    if (down)
        menu.highlight = stepItem(enabled, count, menu.highlight, 1);

    Command chosen = kNoCommand;
    for (uint32_t i = 0; i < count; ++i) {
        Command command = items[i];
        if (command == kNoCommand) {
            ui.div(popup).h(1).mx(10).my(4).bg(kSeparator);
            continue;
        }
        PrCommandStateFlags state = prCommandRegistryState(registry, command);
        UiElement row = ui.div(popup, PrUiKey(i) + 1).flexRow().itemsCenter().h(28).mx(4).pr(10).rounded(4);
        if (enabled[i] && row.hovered())
            menu.highlight = i;
        if (enabled[i] && menu.highlight == i)
            row.bg(kHover);
        PrColor color = enabled[i] ? kText : kTextDisabled;
        UiElement gutter = ui.div(row).w(30).shrink0().flexRow().justifyCenter();
        if ((state & PrCommandStateFlag_Checked) != 0)
            icon(ui, gutter, Icon_Check, 16, color);
        ui.div(row).flex1().minW(0).whitespaceNowrap().overflowHidden().textColor(color)
            .text(prCommandRegistryCommand(registry, command).title);
        String chord = firstChordOf(pandite, command);
        if (!chord.isEmpty())
            ui.div(row).shrink0().ml(18).textXs().textColor(enabled[i] ? kTextDim : kTextDisabled).text(chord);
        if (enabled[i] && row.clicked())
            chosen = command;
    }
    if (enter && menu.highlight < count)
        chosen = items[menu.highlight];
    if (close || chosen != kNoCommand)
        menu = ContextMenu{};
    if (chosen != kNoCommand)
        prCommandRegistryRun(registry, commandId(chosen), nullptr, nullptr);
}

// ---------------------------------------------------------------------------------------------------------------------
// The image

// Wants the picture of the image on screen from the loader, and the pictures of the images beside it ahead of need.
// Returns the picture on screen.
const Picture* PR_NULLABLE want(Pandite& pandite) {
    uint32_t count = imageCount(pandite);
    if (count == 0 || pandite.index == kNoImage)
        return nullptr;
    const Picture* shown = &loaderWant(pandite.loader, folderPath(pandite.folder, pandite.index));
    for (uint32_t step = 1; step <= kAhead; ++step) {
        // The next image is asked for before the previous one, because the user steps forward more often than back.
        if (pandite.index + step < count)
            loaderWantAhead(pandite.loader, folderPath(pandite.folder, pandite.index + step));
        if (pandite.index >= step)
            loaderWantAhead(pandite.loader, folderPath(pandite.folder, pandite.index - step));
    }
    return shown;
}

// Declares the image area: the picture on screen at its view. The wheel zooms about the pointer, a drag pans, and a
// double click fits the image or shows it at 1:1.
void stage(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const Picture* PR_NULLABLE picture, ViewFrame& frame,
        double time) {
    UiElement area = ui.div(page).absolute().left(0).top(0).w(frame.stage.x).h(frame.stage.y).overflowHidden();
    PrVec2 local{};
    bool over = area.pointer(&local);
    PrVec2 anchor{frame.viewport.x * 0.5f, frame.viewport.y * 0.5f};
    if (over)
        anchor = PrVec2{(local.x - frame.origin.x) * frame.ratio, (local.y - frame.origin.y) * frame.ratio};
    PrVec2 wheel{};
    if (area.wheel(&wheel) && frame.image.x > 0 && wheel.y != 0) {
        float factor = prF32Pow(kZoomStep, -wheel.y / kWheelNotch);
        viewZoomBy(pandite.view, factor, frame.image, frame.viewport, anchor);
    }
    // In crop mode, a drag selects instead of panning.
    bool cropping = pandite.editor.cropping && frame.image.x > 0;
    // In a comparison, a drag on the slider's line moves the slider instead of the view.
    bool splitting = compareSplit(pandite, area, frame);
    PrUiDrag drag{};
    if (!cropping && area.dragged(&drag) && !splitting && drag.moved && frame.image.x > 0)
        viewPanBy(pandite.view, PrVec2{drag.delta.x * frame.ratio, drag.delta.y * frame.ratio}, frame.image,
            frame.viewport);
    panditeMenuOn(pandite, ui, area);
    uint32_t clicks = 0;
    if (!cropping && area.clickedWith(0, &clicks) && clicks == 2 && !drag.moved && frame.image.x > 0) {
        if (pandite.view.mode == ViewMode_Fit)
            viewOneToOne(pandite.view, frame.image, frame.viewport, anchor);
        else
            viewFit(pandite.view);
    }
    // The magnifier shows over the pictures while the pointer is over the image area, outside crop mode.
    bool peeking = pandite.peek && !cropping && over && area.hovered();
    if (pandite.compare.on) {
        compareView(pandite, ui, area, picture, frame);
        if (peeking) {
            ComparePane panes[kMaxTiles];
            uint32_t count = comparePanes(pandite, picture, frame, panes);
            peekView(ui, area, Span<const ComparePane>(panes, count), pandite.compare.tiles, local, frame.ratio);
        }
        return;
    }
    if (picture == nullptr) {
        UiElement empty = ui.div(area).absolute().inset(0).flexCol().itemsCenter().justifyCenter().gap(10)
            .textColor(kTextDim).cursorPointer();
        icon(ui, empty, Icon_Picture, 48, kTextDim);
        ui.div(empty).textBase().text("Open an image");
        // The hint names the chord that opens a file as the platform shows it.
        String chord = firstChordOf(pandite, Command_Open);
        String hint("Drop an image on the pandite icon");
        if (!chord.isEmpty())
            hint = format("Press {}, or drop an image on the pandite icon", chord);
        ui.div(empty).textXs().textColor(kTextDisabled).text(hint);
        if (empty.clicked())
            openDialog(pandite);
        return;
    }
    if (picture->source != nullptr && frame.image.x > 0) {
        ViewRect rect = viewImageRect(pandite.view, frame.image, frame.viewport);
        bool crisp = viewScale(pandite.view, frame.image, frame.viewport) >= kCrispZoom;
        // The user waits for the image on screen, so its texture starts ahead of the textures of the thumbnails.
        UiImage image{
            .source = picture->source,
            .rendering = PrLayoutImageRendering(crisp ? PrLayoutImageRendering_Pixelated : PrLayoutImageRendering_Auto),
            .priority = PrTextureCachePriority_High,
        };
        ui.image(area, image).absolute().left(rect.origin.x / frame.ratio).top(rect.origin.y / frame.ratio)
            .w(rect.size.x / frame.ratio).h(rect.size.y / frame.ratio).pointerEventsNone();
        if (cropping) {
            ViewRect shown{
                .origin = PrVec2{rect.origin.x / frame.ratio, rect.origin.y / frame.ratio},
                .size = PrVec2{rect.size.x / frame.ratio, rect.size.y / frame.ratio},
            };
            cropMode(pandite, ui, area, page, shown);
        }
        if (peeking) {
            ComparePane pane{};
            comparePanes(pandite, picture, frame, &pane);
            peekView(ui, area, Span<const ComparePane>(&pane, 1), false, local, frame.ratio);
        }
    }
    if (picture->failed) {
        UiElement notice = ui.div(area).absolute().inset(0).flexCol().itemsCenter().justifyCenter().gap(8)
            .textColor(kTextDim).pointerEventsNone();
        icon(ui, notice, Icon_Warning, 40, kTextDim);
        ui.div(notice).textBase().text(format("Cannot open {}", pandite.folder.names[pandite.index]));
        PrStringView reason = prStatusName(picture->status);
        if (!reason.isEmpty())
            ui.div(notice).textXs().textColor(kTextDisabled).text(reason);
    } else if (picture->source == nullptr && time - pandite.shownAt >= kLoadingDelay) {
        UiElement pill = ui.div(area).absolute().bottom(16).left(0).right(0).flexRow().justifyCenter()
            .pointerEventsNone();
        ui.div(pill).px(12).py(6).roundedFull().bg(kPanel).border(1).borderColor(kBorder).textSm().textColor(kTextDim)
            .text("Loading...");
    } else if (picture->source == nullptr) {
        prUiRequestFrameAt(ui.ui(), pandite.shownAt + kLoadingDelay);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The toolbar

// Declares the status text of the toolbar: the name of the file, its pixels, the zoom and the position in the folder.
void status(Pandite& pandite, UiBuilder& ui, PrLayoutNode bar, const Picture* PR_NULLABLE picture, const ViewFrame& frame) {
    UiElement line = ui.div(bar).flex1().minW(0).flexRow().itemsCenter().justifyEnd().gap(14).ml(12).textSm()
        .whitespaceNowrap().overflowHidden();
    uint32_t count = imageCount(pandite);
    if (count == 0)
        return;
    ui.div(line).minW(0).shrink().overflowHidden().textColor(kText).text(pandite.folder.names[pandite.index]);
    if (picture != nullptr && picture->extent.x != 0) {
        ui.div(line).shrink0().textColor(kTextDim).text(format("{} x {}", picture->extent.x, picture->extent.y));
        float percent = viewPercent(pandite.view, frame.image, frame.viewport);
        String zoom = percent < 10 ? format("{:.1f}%", percent) : format("{:.0f}%", percent);
        ui.div(line).shrink0().textColor(kTextDim).text(zoom);
    }
    if (pandite.selected.count() != 0)
        ui.div(line).shrink0().textColor(kText).text(format("{} selected", pandite.selected.count()));
    ui.div(line).shrink0().textColor(kTextDim).text(format("{} / {}", pandite.index + 1, count));
}

// Declares the tooltip of a toolbar button: the title and the first chord of its command. While no image is open, the
// tooltip also says why the button is disabled.
void buttonTooltip(Pandite& pandite, UiBuilder& ui, PrLayoutNode button, Command command, bool enabled) {
    PrStringView title = commandTitle(command);
    if (command == Command_Fullscreen && pandite.fullscreen)
        title = "Leave full screen";
    String text(title);
    String chord = firstChordOf(pandite, command);
    if (!chord.isEmpty())
        text.append(format("  ({})", chord));
    if (!enabled && imageCount(pandite) == 0)
        text.append(" - open an image first");
    ui.tooltip(button, 1).px(8).py(4).rounded(4).bg(kTooltip).textXs().textColor(kTooltipText).whitespaceNowrap()
        .text(text);
}

void toolbar(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const Picture* PR_NULLABLE picture,
        const ViewFrame& frame, double time) {
    bool shown = pandite.toolbarPinned || time - pandite.activeAt < kIdleHide;
    UiElement bar = ui.div(page).absolute().left(0).top(0).wFull().h(kToolbarHeight).flexRow().itemsCenter().px(8)
        .bg(kPanel).borderB(1).borderColor(kBorder);
    panditeMenuOn(pandite, ui, bar);
    // The toolbar stays shown while the pointer is over it.
    if (bar.hovered())
        shown = true;
    bar.opacity(shown ? 1 : 0).transition(shown ? kFadeIn : kFadeOut);
    if (!shown)
        bar.pointerEventsNone();
    else if (!pandite.toolbarPinned && !bar.hovered())
        prUiRequestFrameAt(ui.ui(), pandite.activeAt + kIdleHide);
    for (uint32_t g = 0; g < countOf(kGroups); ++g) {
        if (g > 0)
            ui.div(bar).w(1).h(20).mx(6).shrink0().bg(kSeparator);
        const ButtonGroup& group = kGroups[g];
        PrLayoutNode row = ui.div(bar).flexRow().gap(2).shrink0();
        for (uint32_t i = 0; i < group.count; ++i) {
            const Button& button = group.buttons[i];
            bool enabled = isEnabled(pandite, button.command, frame);
            bool toggled = button.command == Command_ToggleGallery && pandite.galleryVisible && imageCount(pandite) > 0;
            UiElement b =
                ui.div(row).size(kButton).rounded(4).flexRow().itemsCenter().justifyCenter().disabled(!enabled);
            if (toggled)
                b.bg(kHover);
            if (enabled)
                b.hoverBg(kHover).activeBg(kPressed);
            Icon glyph = button.icon;
            if (button.command == Command_Fullscreen && pandite.fullscreen)
                glyph = Icon_ExitFullscreen;
            icon(ui, b, glyph, kIconSize, enabled ? kIcon : kTextDisabled);
            if (enabled && b.clicked())
                run(pandite, button.command, frame, time);
            if (shown && !b.pressed() && b.hoveredFor(kTooltipDelay))
                buttonTooltip(pandite, ui, b, button.command, enabled);
        }
    }
    status(pandite, ui, bar, picture, frame);
}

// ---------------------------------------------------------------------------------------------------------------------
// The gallery

// The range of thumbnails that a strip of `width` px shows at the gallery's scroll.
struct Visible {
    uint32_t first;
    uint32_t last;
};

Visible visibleOf(const Pandite& pandite, float width) {
    uint32_t count = imageCount(pandite);
    float stride = cellOf(pandite.gallerySize) + gapOf(pandite.gallerySize);
    float start = pandite.galleryScroll - padOf(pandite.gallerySize);
    uint32_t first = start > 0 ? uint32_t(start / stride) : 0;
    uint32_t last = uint32_t((pandite.galleryScroll + width) / stride) + 1;
    return Visible{first < count ? first : count, last < count ? last : count - 1};
}

// Returns `scroll` clamped so that the content of the gallery, `content` px wide, stays within a strip of `width` px.
float clampScroll(float scroll, float content, float width) {
    float most = prF32Max(content - width, 0);
    return scroll < 0 ? 0 : scroll > most ? most : scroll;
}

// Declares the badge of a selected thumbnail: a small white disc with a blue check mark, in the top left corner of the
// thumbnail.
void badge(UiBuilder& ui, PrLayoutNode box) {
    UiElement disc = ui.div(box).absolute().left(4).top(4).size(14).roundedFull().bg(kCheckFill).border(1)
        .borderColor(kCheckLine).flexRow().itemsCenter().justifyCenter().pointerEventsNone();
    icon(ui, disc, Icon_Check, 10, kCheckMark);
}

void gallery(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const ViewFrame& frame, double time) {
    uint32_t count = imageCount(pandite);
    float cell = cellOf(pandite.gallerySize);
    float gap = gapOf(pandite.gallerySize);
    float pad = padOf(pandite.gallerySize);
    float height = stripOf(pandite.gallerySize);
    float width = frame.stage.x;
    float content = 2 * pad + float(count) * cell + float(count > 0 ? count - 1 : 0) * gap;
    float stride = cell + gap;
    if (pandite.revealCurrent) {
        // Scrolls the image on screen into view. It is centered if it was out of view.
        float at = pad + float(pandite.index) * stride;
        if (at < pandite.galleryScroll || at + cell > pandite.galleryScroll + width)
            pandite.galleryScroll = at + cell * 0.5f - width * 0.5f;
        pandite.revealCurrent = false;
    }
    UiElement strip = ui.div(page).absolute().left(0).bottom(0).wFull().h(height).bg(kPanelSolid).borderT(1)
        .borderColor(kBorder).overflowHidden();
    panditeMenuOn(pandite, ui, strip);
    PrVec2 wheel{};
    if (strip.wheel(&wheel))
        pandite.galleryScroll += prF32Abs(wheel.x) > prF32Abs(wheel.y) ? wheel.x : wheel.y;
    PrUiDrag drag{};
    if (strip.dragged(&drag) && drag.moved)
        pandite.galleryScroll -= drag.delta.x;
    pandite.galleryScroll = clampScroll(pandite.galleryScroll, content, width);
    Visible visible = visibleOf(pandite, width);
    for (uint32_t i = visible.first; i <= visible.last && i < count; ++i) {
        const Picture& thumbnail = loaderWant(pandite.loader, folderPath(pandite.folder, i));
        bool current = i == pandite.index;
        UiElement box = ui.div(strip, PrUiKey(i) + 1).absolute().left(pad + float(i) * stride - pandite.galleryScroll)
            .top(pad).size(cell).rounded(4).bg(kPlaceholder).overflowHidden().flexRow().itemsCenter().justifyCenter()
            .cursorPointer();
        if (current)
            box.outline(2, kAccent, 0);
        if (thumbnail.source != nullptr)
            // A thumbnail shows the first picture of an animation, and does not play it.
            ui.image(box, {.source = thumbnail.source, .still = true}).absolute().inset(0).sizeFull().objectCover()
                .pointerEventsNone();
        else
            icon(ui, box, thumbnail.failed ? Icon_Warning : Icon_Picture, kIconSize, kTextDim);
        if (pandite.selected.contains(pandite.folder.names[i]))
            badge(ui, box);
        if (!current && box.hovered())
            ui.div(box).absolute().inset(0).bg(kHover).pointerEventsNone();
        if (box.clicked() && !drag.moved)
            show(pandite, i, time);
        panditeMenuOn(pandite, ui, box);
    }
    // Where the image on screen is in the folder.
    if (count > 1) {
        float trackWidth = prF32Min(width * 0.4f, 320);
        UiElement track = ui.div(strip).absolute().bottom(pad * 0.3f).left((width - trackWidth) * 0.5f).w(trackWidth)
            .h(3).roundedFull().bg(kTrack).pointerEventsNone();
        float at = float(pandite.index) / float(count - 1) * (trackWidth - 24);
        ui.div(track).absolute().left(at).top(-1).w(24).h(5).roundedFull().bg(kMarker).transition(0.15f);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Input

// Returns the text of the chords that bind `command` in the viewer's context, as the platform shows them, joined.
String chordsOf(const Pandite& pandite, Command command) {
    String text;
    PrStringView id = commandId(command);
    const PrCommandKeymap* keymap = pandite.commands->keymap;
    for (uint32_t i = 0; i < prCommandKeymapCount(keymap); ++i) {
        PrCommandBinding binding = prCommandKeymapBinding(keymap, i);
        if (binding.commandId != id || binding.context != commandContext(command))
            continue;
        String chord = String::adopt(prCommandChordText(binding.chord));
        if (!text.isEmpty())
            text.append(", ");
        text.append(chord);
    }
    return text;
}

// Declares the selection box in the bottom right corner of the image area. The box shows the number of selected images
// and the key that clears them. The "Marked" pill shows above the box while the image on screen is selected.
void selectionBox(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, float bottom) {
    uint32_t count = pandite.selected.count();
    if (count == 0)
        return;
    UiElement corner = ui.div(page).absolute().right(16).bottom(bottom + 16).flexCol().itemsEnd().gap(6)
        .pointerEventsNone();
    if (pandite.index != kNoImage && pandite.selected.contains(pandite.folder.names[pandite.index])) {
        ui.div(corner).h(20).px(8).flexRow().itemsCenter().roundedFull().bg(kCheckFill).border(1)
            .borderColor(kCheckLine).textXs().semibold().textColor(kCheckMark).text("Marked");
    }
    UiElement box = ui.div(corner).flexCol().itemsEnd().px(10).py(6).rounded(6).bg(kPanel).border(1)
        .borderColor(kBorder);
    ui.div(box).textSm().textColor(kAccent).text(format("{} selected", count));
    ui.div(box).textXs().textColor(kTextDim).text("Backspace to clear selection");
    ui.div(box).textXs().textColor(kTextDim).text(compareHint(pandite));
}

// Declares the information panel in the bottom left corner of the image area. The panel has a row for each fact of the
// image on screen, as many as fit between the toolbar and the bottom of the area. Crop mode hides the panel, so that
// the whole image stays editable. A tile comparison hides it too, because the facts do not describe the images of the
// tiles.
void infoPanel(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const ViewFrame& frame, float bottom) {
    const InfoPanel& info = pandite.info;
    bool tiles = pandite.compare.on && pandite.compare.tiles;
    if (!info.visible || pandite.editor.cropping || tiles || pandite.index == kNoImage)
        return;
    if (info.name != pandite.folder.names[pandite.index])
        return;
    constexpr float margin = 16;
    constexpr float pad = 10;
    constexpr float rowHeight = 20;
    float room = frame.stage.y - kToolbarHeight - 2 * margin - 2 * pad;
    uint32_t fit = room > rowHeight ? uint32_t(room / rowHeight) : 0;
    if (fit == 0 || frame.stage.x < 2 * margin + 160)
        return;
    Array<InfoRow> rows;
    factsRows(info.facts, &rows);
    UiElement panel = ui.div(page).absolute().left(margin).bottom(bottom + margin).w(prF32Min(460, frame.stage.x - 2 * margin))
        .flexCol().p(pad).bg(kPanel).border(1).borderColor(kBorder).rounded(6).shadowMd().pointerEventsNone();
    for (uint32_t i = 0; i < rows.count() && i < fit; ++i) {
        UiElement row = ui.div(panel, PrUiKey(i) + 1).h(rowHeight).flexRow().itemsCenter().gap(12).whitespaceNowrap();
        ui.div(row).w(132).shrink0().overflowHidden().textXs().textColor(kTextDim).text(rows[i].label);
        ui.div(row).flex1().minW(0).overflowHidden().textSm().textColor(kText).text(rows[i].value);
    }
}

// Declares the message at the bottom of the image area. The message fades once it has shown for a while.
void toastLine(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const ViewFrame& frame, float bottom, double time) {
    if (pandite.toast.isEmpty())
        return;
    double age = time - pandite.toastAt;
    if (age >= kToastHold + kToastFade) {
        pandite.toast = String();
        return;
    }
    float opacity = age <= kToastHold ? 1 : float(1 - (age - kToastHold) / kToastFade);
    prUiRequestFrame(ui.ui());
    UiElement row = ui.div(page).absolute().left(0).w(frame.stage.x).bottom(bottom + 16).flexRow().justifyCenter()
        .pointerEventsNone();
    ui.div(row).px(14).py(8).rounded(6).bg(kPanel).border(1).borderColor(kBorder).shadowMd().opacity(opacity)
        .text(pandite.toast);
}

// Declares the question before an action that cannot be taken back: its message, the button that keeps the files and
// the button that acts. Left and Right move between the buttons. Enter presses the button that `acting` selects. Esc
// keeps the files.
void confirmDialog(Pandite& pandite, UiBuilder& ui, PrLayoutNode page) {
    Confirm& confirm = pandite.confirm;
    UiElement backdrop = ui.div(page).absolute().inset(0).bg(kBackdrop).flexRow().itemsCenter().justifyCenter();
    UiElement panel = ui.div(backdrop).w(440).maxW(uiPct(92)).flexCol().gap(16).p(20).bg(kPanelSolid).rounded(8)
        .border(1).borderColor(kBorder).shadowXl().pressFocus();
    if (confirm.focus) {
        panel.takeFocus();
        confirm.focus = false;
    }
    bool keep = panel.keyCaptured(PrAppKey_Escape) || backdrop.clicked();
    if (panel.keyCaptured(PrAppKey_Left) || panel.keyCaptured(PrAppKey_Right) || panel.keyCaptured(PrAppKey_Tab) ||
            panel.keyCaptured(PrAppKey_Tab, PrAppModifierFlag_Shift))
        confirm.acting = !confirm.acting;
    bool enter = panel.keyCaptured(PrAppKey_Enter);
    ui.div(panel).textBase().text(confirm.message);
    UiElement buttons = ui.div(panel).flexRow().justifyEnd().gap(8);
    bool acts[2] = {false, true};
    bool pressed[2] = {false, false};
    for (uint32_t i = 0; i < 2; ++i) {
        bool highlighted = confirm.acting == acts[i];
        UiElement button = ui.div(buttons, i + 1).h(30).px(14).flexRow().itemsCenter().rounded(4).textSm()
            .cursorPointer();
        if (highlighted)
            button.bg(kAccent).textColor(kAccentText).semibold();
        else
            button.bg(kHover).hoverBg(kPressed);
        if (acts[i] && !confirm.trash && !confirm.replace)
            button.textColor(highlighted ? kAccentText : kError);
        bool cancels = confirm.trash || confirm.replace;
        button.text(acts[i] ? PrStringView(confirm.act) : PrStringView(cancels ? "Cancel" : "Keep"));
        pressed[i] = button.clicked();
    }
    if (pressed[1] || (enter && confirm.acting)) {
        confirmAction(pandite);
        return;
    }
    if (keep || pressed[0] || enter)
        pandite.confirm = Confirm{};
}

// Declares the keyboard shortcuts window. The window lists every command with its keys. Its search keeps the commands
// whose title or keys hold each word typed. Enter runs the highlighted match, and Up and Down move the highlight. Esc
// or a click beside the window closes it. A click on a row runs the command of that row.
void shortcutsWindow(Pandite& pandite, UiBuilder& ui, PrLayoutNode page) {
    Shortcuts& state = pandite.shortcuts;
    UiElement backdrop = ui.div(page).absolute().inset(0).bg(kBackdrop).flexRow().itemsCenter().justifyCenter();
    bool close = backdrop.clicked();
    UiElement panel = ui.div(backdrop).w(560).maxW(uiPct(92)).h(uiPct(78)).flexCol().bg(kPanelSolid).rounded(8)
        .border(1).borderColor(kBorder).shadowXl().pressable();
    // The window takes these keys ahead of its search.
    close = panel.keyCaptured(PrAppKey_Escape) || close;
    bool up = panel.keyCaptured(PrAppKey_Up, 0, true);
    bool down = panel.keyCaptured(PrAppKey_Down, 0, true);
    bool enter = panel.keyCaptured(PrAppKey_Enter);

    ui.div(panel).px(16).pt(14).pb(4).textBase().semibold().text("Keyboard shortcuts");
    UiElement search = ui.div(panel).relative().mx(16).my(8).px(10).py(6).rounded(4).bg(kField).border(1)
        .borderColor(kFieldBorder).textSm().whitespaceNowrap().overflowHidden();
    if (state.focus) {
        search.takeFocus();
        state.focus = false;
    }
    PrStringView typed{};
    if (search.edit({.value = state.query, .selection = kSelection, .caret = kText}, &typed)) {
        state.query = typed;
        state.highlight = 0;
    }
    if (state.query.isEmpty())
        ui.span(search).absolute().left(10).top(6).textColor(kTextDisabled).pointerEventsNone().text("Type to search");

    // The commands that match, in the registry's order.
    Command matched[Command_Count];
    String keys[Command_Count];
    uint32_t count = 0;
    const PrCommandRegistry* registry = pandite.commands->registry;
    for (uint32_t i = 0; i < Command_Count; ++i) {
        String chords = chordsOf(pandite, Command(i));
        PrCommandDesc desc = prCommandRegistryCommand(registry, i);
        if (prCommandMatches(state.query, desc.title, chords)) {
            keys[count] = move(chords);
            matched[count++] = Command(i);
        }
    }
    if (up && state.highlight > 0)
        state.highlight -= 1;
    if (down && state.highlight + 1 < count)
        state.highlight += 1;
    if (state.highlight >= count)
        state.highlight = count > 0 ? count - 1 : 0;

    UiElement list = ui.div(panel).flex1().minH(0).flexCol().px(8).pb(8).overflowYScroll();
    uint32_t chosen = Command_Count;
    for (uint32_t n = 0; n < count; ++n) {
        Command command = matched[n];
        bool enabled = (prCommandRegistryState(registry, command) & PrCommandStateFlag_Disabled) == 0;
        bool highlighted = n == state.highlight;
        UiElement row = ui.div(list, PrUiKey(command) + 1).shrink0().flexRow().itemsCenter().gap(12).h(28).px(8)
            .rounded(4).cursorPointer();
        if (highlighted)
            row.bg(kHover);
        else
            row.hoverBg(kHover);
        ui.div(row).w(190).shrink0().whitespaceNowrap().overflowHidden().textColor(kTextDim).text(keys[n]);
        ui.div(row).flex1().minW(0).whitespaceNowrap().overflowHidden()
            .textColor(enabled ? kText : kTextDisabled).text(prCommandRegistryCommand(registry, command).title);
        if (highlighted && (up || down))
            prUiReveal(ui.ui(), row);
        if (row.clicked())
            chosen = command;
    }
    if (count == 0)
        ui.div(list).px(8).py(6).textColor(kTextDim).text("No matching commands");
    if (enter && count != 0)
        chosen = matched[state.highlight];
    ui.div(panel).px(16).py(8).borderT(1).borderColor(kBorder).textXs().textColor(kTextDim)
        .text("Up and Down choose, Enter runs, Esc closes");

    if (close || chosen != Command_Count)
        state = Shortcuts{};
    if (chosen != Command_Count)
        prCommandRegistryRun(registry, commandId(Command(chosen)), nullptr, nullptr);
}

// The callback of the folder's watch. Marks the folder changed and wakes the application.
void watched(void* PR_NULLABLE context) {
    Pandite& pandite = *static_cast<Pandite*>(context);
    pandite.folderChanged.store(1);
    if (pandite.app != nullptr)
        prAppWake(pandite.app);
}

// Returns the picture that the view shows for the image on screen. That is the loader's picture, or the editor's
// picture once an edit applied.
const Picture* PR_NULLABLE shownPicture(Pandite& pandite, Picture& edited) {
    const Picture* picture = want(pandite);
    PrUVec2 extent{};
    const PrTextureSource* source = editorShown(pandite, &extent);
    if (picture == nullptr || source == nullptr)
        return picture;
    edited = *picture;
    edited.source = source;
    edited.extent = extent;
    edited.failed = false;
    return &edited;
}

// Declares the pill below the toolbar that says what the editor works on.
void busyLine(Pandite& pandite, UiBuilder& ui, PrLayoutNode page, const ViewFrame& frame) {
    String text = editorBusyText(pandite);
    if (text.isEmpty())
        return;
    UiElement row = ui.div(page).absolute().left(0).w(frame.stage.x).top(kToolbarHeight + 12).flexRow().justifyCenter()
        .pointerEventsNone();
    ui.div(row).px(12).py(6).roundedFull().bg(kPanel).border(1).borderColor(kBorder).textSm().textColor(kTextDim)
        .text(format("{}...", text));
}

} // namespace

void panditeSay(Pandite& pandite, String text) {
    pandite.toast = move(text);
    pandite.toastAt = pandite.time;
}

void panditeMenuOn(Pandite& pandite, UiBuilder& ui, UiElement& region) {
    PrVec2 at{};
    if (region.clickedWith(1) && prUiPointer(ui.ui(), nullptr, &at))
        pandite.menu = ContextMenu{.open = true, .at = at, .highlight = kNoRow};
}

CommandHandler panditeCommandHandler(Pandite& pandite) {
    return CommandHandler{
        .run = [](void* PR_NULLABLE context, Command command) {
            Pandite& p = *static_cast<Pandite*>(context);
            p.activeAt = p.time;
            run(p, command, p.frame, p.time);
        },
        .state = [](void* PR_NULLABLE context, Command command) {
            const Pandite& p = *static_cast<const Pandite*>(context);
            uint32_t flags = isEnabled(p, command, p.frame) ? 0 : PrCommandStateFlag_Disabled;
            bool checked = (command == Command_ToggleGallery && p.galleryVisible) ||
                (command == Command_Fullscreen && p.fullscreen) || (command == Command_Peek && p.peek) ||
                (command == Command_ToggleInfo && p.info.visible);
            return PrCommandStateFlags(flags | (checked ? PrCommandStateFlag_Checked : 0));
        },
        .context = &pandite,
    };
}

bool panditeOpen(Pandite& pandite, PrStringView path) {
    String absolute = String::adopt(prFilePathAbsolute(path, ""));
    PrFileInfo info{};
    if (absolute.isNull() || !prFileInfo(absolute, &info))
        return false;
    bool directory = info.kind == PrFileKind_Directory;
    String folder = directory ? absolute : String(prFilePathParent(absolute));
    PrStringView name = directory ? PrStringView{} : prFilePathName(absolute);
    Folder listed;
    if (!folderList(folder, pandite.codecs, &listed))
        return false;
    uint32_t index = directory ? (listed.names.count() != 0 ? 0 : kNoImage) : folderFind(listed, name);
    if (!directory && index == kNoImage) {
        // The folder leaves out a file of a format that it does not list, such as a file whose extension names no
        // image. Such a file still opens alone.
        listed.names.insert(0, String(name));
        index = 0;
    }
    pandite.folder = move(listed);
    pandite.index = kNoImage;
    show(pandite, index, pandite.shownAt);
    prFileWatchDestroy(pandite.watch);
    pandite.watch = prFileWatchCreate(folder, watched, &pandite);
    pandite.folderChanged.store(0);
    return true;
}

void panditeRefresh(Pandite& pandite) {
    finishCopy(pandite);
    updateInfo(pandite);
    editorUpdate(pandite);
    if (pandite.folderChanged.exchange(0) == 0 || pandite.folder.directory.isEmpty())
        return;
    String current = pandite.index != kNoImage ? pandite.folder.names[pandite.index] : String();
    Folder listed;
    if (!folderList(pandite.folder.directory, pandite.codecs, &listed))
        return;
    pandite.folder = move(listed);
    // A selected file that left the folder leaves the selection.
    Array<String> gone;
    for (const String& name : pandite.selected) {
        if (folderFind(pandite.folder, name) == kNoImage)
            gone.push(name);
    }
    for (const String& name : gone)
        pandite.selected.erase(name);
    uint32_t index = folderFind(pandite.folder, current);
    if (index == kNoImage)
        index = folderNearest(pandite.folder, current);
    // The image on screen stays as it was if it is still there.
    if (index != pandite.index) {
        pandite.index = index;
        pandite.revealCurrent = true;
    }
}

void panditeClose(Pandite& pandite) {
    prFileWatchDestroy(pandite.watch);
    pandite.watch = nullptr;
}

void panditeRelease(Pandite& pandite) {
    destroy(pandite.copying);
    pandite.copying = nullptr;
    destroy(pandite.info.reading);
    pandite.info.reading = nullptr;
    editorRelease(pandite);
}

void buildPandite(Pandite& pandite, UiBuilder& ui, PrLayoutNode root, PrVec2 extent) {
    double time = ui.time();
    if (pandite.quit && pandite.app != nullptr)
        prAppStop(pandite.app);
    float ratio = prUiDisplay(ui.ui()).pixelRatio;
    ratio = ratio > 0 ? ratio : 1;
    bool showGallery = pandite.galleryVisible && imageCount(pandite) > 0;
    float galleryHeight = showGallery ? stripOf(pandite.gallerySize) : 0;

    // A move of the pointer shows the toolbar.
    PrVec2 pointer{};
    if (prUiPointer(ui.ui(), nullptr, &pointer) && (pointer.x != pandite.pointer.x || pointer.y != pandite.pointer.y)) {
        pandite.pointer = pointer;
        pandite.activeAt = time;
    }

    UiElement page = ui.div(root).relative().sizeFull().bg(kCanvas).overflowHidden().textColor(kText).textSm();
    ViewFrame frame{.stage = PrVec2{extent.x, prF32Max(extent.y - galleryHeight, 0)}, .ratio = ratio};
    frame.viewport = PrVec2{frame.stage.x * ratio, frame.stage.y * ratio};
    Picture edited{};
    compareFollow(pandite);
    const Picture* picture = shownPicture(pandite, edited);
    if (picture != nullptr && picture->extent.x != 0)
        frame.image = PrVec2{float(picture->extent.x), float(picture->extent.y)};
    // In tiles, the view acts on the tile under the pointer. The image area is at the page's top left.
    PrVec2 over{};
    bool pointed = prUiPointer(ui.ui(), nullptr, &over) && over.y < frame.stage.y;
    compareFrame(pandite, frame, over, pointed);
    // The commands of the round act on the frame of the round. Their scope is the window's root. A key starts at the
    // root while nothing is focused, so the commands apply wherever the user works. While a dialog is open, the dialog
    // takes the keys. Only the commands of the dialog's context, full screen and quit, stay bound. Crop mode binds its
    // own context, and its selection takes the arrow keys ahead of the commands.
    pandite.frame = frame;
    pandite.time = time;
    bool dialog = pandite.shortcuts.open || pandite.confirm.open || pandite.editor.resize.open;
    PrStringView context = dialog ? kDialogContext : pandite.editor.cropping ? kCropContext : PrStringView("");
    if (!dialog)
        cropKeys(pandite, ui, root);
    // Escape ends a comparison ahead of the commands.
    if (!dialog && pandite.compare.on && prUiKeyCaptured(ui.ui(), root, PrAppKey_Escape, 0))
        compareEnd(pandite);
    // A run of the keys that select and step lasts while Shift is held.
    if ((prUiModifiers(ui.ui()) & PrAppModifierFlag_Shift) == 0)
        pandite.selectRun = false;
    prCommandScope(ui.ui(), root, pandite.commands->registry, pandite.commands->keymap, context);
    // A command may have moved to another image or edited this one. This round shows the picture of that image.
    picture = shownPicture(pandite, edited);
    frame.image = PrVec2{};
    if (picture != nullptr && picture->extent.x != 0)
        frame.image = PrVec2{float(picture->extent.x), float(picture->extent.y)};
    compareFollow(pandite);
    compareFrame(pandite, frame, over, pointed);
    stage(pandite, ui, page, picture, frame, time);
    if (showGallery)
        gallery(pandite, ui, page, frame, time);
    // The overlays of the image area stand on its bottom edge, above the gallery.
    selectionBox(pandite, ui, page, galleryHeight);
    // The commands of the round may have shown another image or the panel. The read of the facts starts now.
    updateInfo(pandite);
    infoPanel(pandite, ui, page, frame, galleryHeight);
    toastLine(pandite, ui, page, frame, galleryHeight, time);
    // Crop mode's bar takes the toolbar's place.
    if (!pandite.editor.cropping)
        toolbar(pandite, ui, page, picture, frame, time);
    busyLine(pandite, ui, page, frame);
    if (pandite.menu.open)
        contextMenu(pandite, ui, page, frame);
    if (pandite.shortcuts.open)
        shortcutsWindow(pandite, ui, page);
    if (pandite.editor.resize.open)
        resizeDialog(pandite, ui, page);
    if (pandite.confirm.open)
        confirmDialog(pandite, ui, page);
}

} // namespace pandite
