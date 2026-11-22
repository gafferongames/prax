// The editor of the image on screen.

#include "apps/pandite/editor.h"

#include "pr/ascii.h"
#include "pr/atomic.h"
#include "pr/construct.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/format.h"
#include "pr/string_ops.h"
#include "pr/task.h"

#include "apps/pandite/icons.h"
#include "apps/pandite/theme.h"
#include "apps/pandite/view.h"

namespace pandite {

// What a job of the editor does on the pool.
enum : uint8_t {
    EditorJobKind_Decode,       // decodes the file at `path` to `output`
    EditorJobKind_Apply,        // applies `edit` to `input` and writes `output`
    EditorJobKind_Save,         // writes `input` as a PNG file at `path`
};
typedef uint8_t EditorJobKind;

// A job of the editor on the pool. The job serves the edits of the image `name`. Its result is valid once `done` is
// set.
struct EditorJob {
    EditorJobKind kind;
    String name;
    String path;
    const PrImageCodecSet* codecs;
    Edit edit;
    Bitmap input;
    Bitmap output;
    PrStatus status;
    Atomic<bool> done;
    PrApp* PR_NULLABLE app;
};

namespace {

// The distance around a handle within which a press grabs the handle, and the side of a handle, in px.
constexpr float kHandleReach = 8;
constexpr float kHandleSize = 8;

// The width and the height of the crop bar, and the width of its buttons, in px.
constexpr float kCropBarWidth = 400;
constexpr float kCropBarHeight = 40;
constexpr float kCropButton = 92;

// The largest number of characters in a field of the resize dialog.
constexpr uint32_t kFieldLength = 7;

// The largest percent of a resize.
constexpr float kMaxPercent = 1000;

void runJob(void* PR_NULLABLE context) {
    EditorJob& job = *static_cast<EditorJob*>(context);
    switch (job.kind) {
        case EditorJobKind_Decode: job.status = bitmapDecode(job.codecs, job.path, &job.output); break;
        case EditorJobKind_Apply:
            job.output = editApply(job.edit, job.input);
            job.status = job.output.bytes.get() != nullptr ? PrStatus_Ok : PrStatus_OutOfRange;
            break;
        default: job.status = bitmapWritePng(job.codecs, job.input, job.path); break;
    }
    job.done.store(true);
    if (job.app != nullptr)
        prAppWake(job.app);
}

// Returns the name of the image on screen, or the null string.
String shownName(const Pandite& pandite) {
    return pandite.index != kNoImage ? pandite.folder.names[pandite.index] : String();
}

// Posts a job of `kind` for the edits of the image on screen.
EditorJob* post(Pandite& pandite, EditorJobKind kind) {
    EditorJob* job = create<EditorJob>();
    job->kind = kind;
    job->name = pandite.editor.name;
    job->codecs = pandite.codecs;
    job->app = pandite.app;
    pandite.editor.job = job;
    return job;
}

void start(Pandite& pandite, EditorJob* job) {
    prTaskPoolPost(pandite.pool, runJob, job);
}

// Shows the image after the last applied edit, or the image of the file if none applied.
void updateShown(Editor& editor) {
    prTextureSourceRelease(editor.shown);
    editor.shown = nullptr;
    if (editor.stack.applied.count() == 0)
        return;
    const Bitmap& current = editor.stack.applied.last().result;
    editor.shown = prTextureSourceCreateCopy(bitmapView(current), PrPixelFormat_Rgba8, PrTextureAlpha_Straight);
}

// Forgets the edits, crop mode and the dialog. The result of a job under way is dropped when the job finishes.
void reset(Editor& editor) {
    editor.name = String();
    editor.base = Bitmap{};
    editor.failure = PrStatus_Ok;
    editClear(editor.stack);
    editor.stack.applied.clear();
    updateShown(editor);
    editor.cropping = false;
    editor.crop = CropSelection{};
    editor.resize = ResizeDialog{};
    editor.saveTo = String();
}

// Makes the edits that follow belong to the image on screen. Drops the edits of another image.
void own(Pandite& pandite) {
    Editor& editor = pandite.editor;
    String name = shownName(pandite);
    if (editor.name != name) {
        reset(editor);
        editor.name = name;
    }
}

// Starts the next job if no job runs. The decode of the image comes first if work waits for it, then the next edit,
// then a save. The job wakes the application when it finishes. The editor takes the result in the frame that the wake
// asks for.
void pump(Pandite& pandite) {
    Editor& editor = pandite.editor;
    if (editor.job != nullptr || editor.name.isEmpty() || pandite.index == kNoImage)
        return;
    bool wanted = editIsBusy(editor.stack) || !editor.saveTo.isEmpty();
    if (editor.base.bytes.get() == nullptr) {
        if (!wanted || editor.failure != PrStatus_Ok)
            return;
        EditorJob* decode = post(pandite, EditorJobKind_Decode);
        decode->path = folderPath(pandite.folder, pandite.index);
        start(pandite, decode);
        return;
    }
    Edit edit{};
    if (editNextJob(editor.stack, &edit)) {
        EditorJob* apply = post(pandite, EditorJobKind_Apply);
        apply->edit = edit;
        apply->input = editCurrent(editor.stack, editor.base);
        start(pandite, apply);
        return;
    }
    if (!editor.saveTo.isEmpty()) {
        EditorJob* save = post(pandite, EditorJobKind_Save);
        save->path = static_cast<String&&>(editor.saveTo);
        editor.saveTo = String();
        save->input = editCurrent(editor.stack, editor.base);
        start(pandite, save);
    }
}

// Takes the result of a finished job.
void finish(Pandite& pandite, EditorJob& job) {
    Editor& editor = pandite.editor;
    bool mine = job.name == editor.name && !editor.name.isEmpty();
    if (job.kind == EditorJobKind_Save) {
        PrStringView name = prFilePathName(job.path);
        if (job.status != PrStatus_Ok) {
            panditeSay(pandite, format("Cannot save {}: {}", name, prStatusName(job.status)));
            return;
        }
        panditeSay(pandite, format("Saved {}", name));
        // A save that replaces the image's own file makes the file match what the view shows. The loader decodes the
        // file again.
        if (pandite.index != kNoImage && prFileSame(job.path, folderPath(pandite.folder, pandite.index))) {
            loaderForget(pandite.loader, job.path);
            if (mine)
                reset(editor);
        }
        return;
    }
    if (!mine)
        return;
    if (job.kind == EditorJobKind_Decode) {
        if (job.status == PrStatus_Ok || job.status == PrStatus_Truncated) {
            editor.base = static_cast<Bitmap&&>(job.output);
            return;
        }
        editor.failure = job.status;
        panditeSay(pandite, format("Cannot edit {}: {}", editor.name, prStatusName(job.status)));
        editClear(editor.stack);
        editor.saveTo = String();
        return;
    }
    bool discarded = editor.stack.discardRunning;
    EditKind kind = editor.stack.runningEdit.kind;
    if (editFinish(editor.stack, static_cast<Bitmap&&>(job.output)))
        updateShown(editor);
    else if (!discarded)
        panditeSay(pandite, format("Cannot apply {}", PrStringView(editLabel(kind))));
}

// Returns the extent of the image that crop mode and the dialog act on. That is the image after the last applied edit,
// or `fallback` if no edit applied.
PrUVec2 currentExtent(const Editor& editor, PrUVec2 fallback) {
    return editor.stack.applied.count() != 0 ? editor.stack.applied.last().result.extent : fallback;
}

// Returns `text` reduced to its digits, and to a decimal point if `decimal` is set. The result has at most kFieldLength
// characters.
String digitsOf(PrStringView text, bool decimal) {
    String kept;
    bool point = false;
    for (size_t i = 0; i < text.size && kept.count() < kFieldLength; ++i) {
        char c = text.data[i];
        if (c >= '0' && c <= '9') {
            kept.append(PrStringView(&c, 1));
        } else if (decimal && c == '.' && !point) {
            point = true;
            kept.append(PrStringView(&c, 1));
        }
    }
    return kept;
}

// Returns the side that a field holds, or zero if the text is not a whole number from 1 to kMaxSide.
uint32_t sideOf(const String& text) {
    if (text.isEmpty())
        return 0;
    uint64_t value = 0;
    for (char c : text.view()) {
        if (c < '0' || c > '9')
            return 0;
        value = value * 10 + uint64_t(c - '0');
        if (value > kMaxSide)
            return 0;
    }
    return uint32_t(value);
}

// Returns the percent that a field holds, or zero if the value is not above 0 and at most kMaxPercent.
float percentOf(const String& text) {
    double value = 0;
    if (text.isEmpty() || prF64ParsePrefix(text, &value) != text.count() || !(value > 0) || value > double(kMaxPercent))
        return 0;
    return float(value);
}

// Validates the fields of the dialog. If they give a rule, writes the rule to `*spec` and the extent that it gives to
// `*extent`, and returns the empty view. Otherwise returns why the fields give no rule.
PrStringView validate(const ResizeDialog& dialog, ResizeSpec* spec, PrUVec2* extent) {
    *spec = ResizeSpec{.mode = dialog.mode, .enlarge = dialog.enlarge};
    if (dialog.mode == ResizeMode_Percent) {
        spec->percent = percentOf(dialog.percent);
        if (spec->percent == 0)
            return "Enter a percentage above 0 and at most 1000.";
    } else {
        const String& width = dialog.mode == ResizeMode_Fit ? dialog.fitWidth : dialog.exactWidth;
        const String& height = dialog.mode == ResizeMode_Fit ? dialog.fitHeight : dialog.exactHeight;
        spec->size = PrUVec2{sideOf(width), sideOf(height)};
        if (spec->size.x == 0 || spec->size.y == 0)
            return "Enter a width and height from 1 to 65535.";
    }
    *extent = resizeExtent(*spec, dialog.from);
    if (extent->x == 0)
        return "The result would exceed 65535 pixels.";
    return "";
}

// Returns the cursor for the part of the crop selection that `hit` and `handle` name.
PrAppCursor cursorOf(CropHit hit, CropHandle handle) {
    if (hit == CropHit_Inside)
        return PrAppCursor_Move;
    if (hit == CropHit_Outside)
        return PrAppCursor_Crosshair;
    switch (handle) {
        case CropHandle_TopLeft:
        case CropHandle_BottomRight: return PrAppCursor_ResizeNWSE;
        case CropHandle_TopRight:
        case CropHandle_BottomLeft: return PrAppCursor_ResizeNESW;
        case CropHandle_Top:
        case CropHandle_Bottom: return PrAppCursor_ResizeY;
        default: return PrAppCursor_ResizeX;
    }
}

// Declares a button of a dialog or a bar. The button that Enter presses is in the accent color. Any other button is
// quiet.
UiElement button(UiBuilder& ui, PrLayoutNode parent, PrUiKey key, PrStringView label, bool primary, bool enabled) {
    UiElement b = ui.div(parent, key).h(30).px(14).flexRow().itemsCenter().justifyCenter().rounded(4).textSm();
    if (!enabled)
        b.bg(kHover).textColor(kTextDisabled);
    else if (primary)
        b.bg(kAccent).textColor(kAccentText).semibold().cursorPointer();
    else
        b.bg(kHover).hoverBg(kPressed).cursorPointer();
    b.text(label);
    return b;
}

// Declares a field of the resize dialog: `label`, the field, and its unit. Keeps the text of the field to digits.
// Returns true if the user changed the text.
bool field(UiBuilder& ui, PrLayoutNode parent, PrUiKey key, PrStringView label, String& value, PrStringView unit,
        bool decimal, bool focus) {
    UiElement row = ui.div(parent, key).flexRow().itemsCenter().gap(8);
    ui.div(row).w(96).textSm().textColor(kTextDim).text(label);
    UiElement box = ui.div(row).w(90).px(8).py(5).rounded(4).bg(kField).border(1).borderColor(kFieldBorder).textSm()
        .overflowHidden();
    if (focus)
        box.takeFocus();
    if (box.focused())
        box.borderColor(kAccent);
    PrStringView typed{};
    bool changed = box.edit({.value = value, .selection = kSelection, .caret = kText}, &typed);
    if (changed)
        value = digitsOf(typed, decimal);
    if (!unit.isEmpty())
        ui.div(row).textSm().textColor(kTextDim).text(unit);
    return changed;
}

// Declares a checkbox of the resize dialog. A click toggles it.
void check(UiBuilder& ui, PrLayoutNode parent, PrUiKey key, PrStringView label, bool& value) {
    UiElement row = ui.div(parent, key).flexRow().itemsCenter().gap(8).cursorPointer();
    UiElement box = ui.div(row).size(16).rounded(3).border(1).flexRow().itemsCenter().justifyCenter();
    if (value)
        box.bg(kAccent).borderColor(kAccent);
    else
        box.borderColor(kFieldBorder).bg(kField);
    if (value)
        icon(ui, box, Icon_Check, 12, kAccentText);
    ui.div(row).textSm().text(label);
    if (row.clicked())
        value = !value;
}

} // namespace

bool editorIsEdited(const Pandite& pandite) {
    return !pandite.editor.name.isEmpty() && editIsEdited(pandite.editor.stack);
}

bool editorIsBusy(const Pandite& pandite) {
    return pandite.editor.job != nullptr || editIsBusy(pandite.editor.stack) || !pandite.editor.saveTo.isEmpty();
}

const PrTextureSource* PR_NULLABLE editorShown(const Pandite& pandite, PrUVec2* extent) {
    const Editor& editor = pandite.editor;
    if (editor.shown == nullptr || editor.name != shownName(pandite))
        return nullptr;
    *extent = editor.stack.applied.last().result.extent;
    return editor.shown;
}

const Bitmap* PR_NULLABLE editorEdited(const Pandite& pandite) {
    const Editor& editor = pandite.editor;
    if (editor.stack.applied.count() == 0 || editor.name != shownName(pandite))
        return nullptr;
    return &editor.stack.applied.last().result;
}

String editorBusyText(const Pandite& pandite) {
    const Editor& editor = pandite.editor;
    EditorJob* job = editor.job;
    if (job != nullptr && job->kind == EditorJobKind_Save)
        return format("Saving {}", prFilePathName(job->path));
    if (job != nullptr && job->kind == EditorJobKind_Decode)
        return editIsBusy(editor.stack) ? String("Loading full resolution") : String("Preparing to save");
    if (editIsBusy(editor.stack))
        return String("Applying edits");
    if (!editor.saveTo.isEmpty())
        return String("Preparing to save");
    return String();
}

void editorPush(Pandite& pandite, const Edit& edit) {
    if (pandite.index == kNoImage)
        return;
    own(pandite);
    if (pandite.editor.failure != PrStatus_Ok) {
        panditeSay(pandite, format("Cannot edit {}: {}", pandite.editor.name, prStatusName(pandite.editor.failure)));
        return;
    }
    editPush(pandite.editor.stack, edit);
    pump(pandite);
}

void editorUndo(Pandite& pandite) {
    Editor& editor = pandite.editor;
    if (editor.name != shownName(pandite)) {
        panditeSay(pandite, "Nothing to undo");
        return;
    }
    EditUndo undone = editUndo(editor.stack);
    if (undone == EditUndo_Reverted)
        updateShown(editor);
    else if (undone == EditUndo_Nothing)
        panditeSay(pandite, "Nothing to undo");
}

void editorUpdate(Pandite& pandite) {
    Editor& editor = pandite.editor;
    if (!editor.name.isEmpty() && editor.name != shownName(pandite))
        reset(editor);
    EditorJob* job = editor.job;
    if (job != nullptr) {
        if (!job->done.load())
            return;
        editor.job = nullptr;
        finish(pandite, *job);
        destroy(job);
    }
    pump(pandite);
}

void editorStartCrop(Pandite& pandite, PrUVec2 extent) {
    if (pandite.index == kNoImage || extent.x == 0)
        return;
    own(pandite);
    Editor& editor = pandite.editor;
    editor.cropping = true;
    editor.crop = CropSelection{.image = currentExtent(editor, extent)};
}

void editorApplyCrop(Pandite& pandite) {
    Editor& editor = pandite.editor;
    PrIRect pixels{};
    if (!editor.cropping || !cropPixels(editor.crop, &pixels))
        return;
    editor.cropping = false;
    editorPush(pandite, Edit{.kind = EditKind_Crop, .crop = pixels});
}

void editorCancelCrop(Pandite& pandite) {
    pandite.editor.cropping = false;
}

void editorOpenResize(Pandite& pandite, PrUVec2 extent) {
    if (pandite.index == kNoImage || extent.x == 0)
        return;
    own(pandite);
    PrUVec2 from = currentExtent(pandite.editor, extent);
    pandite.editor.resize = ResizeDialog{
        .open = true,
        .from = from,
        .mode = ResizeMode_Percent,
        .percent = "50",
        .fitWidth = "1920",
        .fitHeight = "1080",
        .exactWidth = format("{}", from.x),
        .exactHeight = format("{}", from.y),
        .keepAspect = true,
        .focus = true,
    };
}

void editorSaveAs(Pandite& pandite) {
    if (pandite.app == nullptr || pandite.index == kNoImage)
        return;
    if (!pandite.editor.saveTo.isEmpty()) {
        panditeSay(pandite, "Still saving");
        return;
    }
    PrStringView name = pandite.folder.names[pandite.index].view();
    PrStringView stem = prStringHead(name, name.size - prFilePathExtension(name).size);
    String suggested = format("{}.png", stem);
    const PrAppFileType types[] = {{.name = "PNG image", .extensions = "png"}};
    PrAppFileDialogDesc desc{
        .kind = PrAppFileDialogKind_Save,
        .parent = pandite.window != nullptr ? prStudioWindowAppWindow(pandite.window) : nullptr,
        .title = "Save as",
        .directory = pandite.folder.directory,
        .name = suggested,
        .types = types,
        .typeCount = countOf(types),
    };
    prAppFileDialog(pandite.app, &desc, [](void* PR_NULLABLE context, PrStatus status, PrAppDataSource source) {
        Pandite& p = *static_cast<Pandite*>(context);
        if (status != PrStatus_Ok || p.app == nullptr)
            return;
        prAppDataReadFiles(p.app, source, [](void* PR_NULLABLE inner, PrStatus read, const PrAppDataFile* PR_NULLABLE files,
            uint32_t count) {
                Pandite& q = *static_cast<Pandite*>(inner);
                if (read != PrStatus_Ok || files == nullptr || count == 0 || q.index == kNoImage)
                    return;
                // Only PNG encodes, so a name of another type gets the extension of PNG.
                String path(files[0].path);
                if (!prAsciiEqualsLowered(".png", prFilePathExtension(path)))
                    path.append(".png");
                if (prFileSame(path, folderPath(q.folder, q.index))) {
                    Confirm confirm{.open = true, .focus = true};
                    confirm.message = format("Replace {} with the edited image?", q.folder.names[q.index]);
                    confirm.act = "Replace";
                    confirm.replace = true;
                    confirm.path = static_cast<String&&>(path);
                    confirm.acting = true;
                    q.confirm = static_cast<Confirm&&>(confirm);
                } else {
                    editorSave(q, path);
                }
                if (q.app != nullptr)
                    prAppWake(q.app);
            }, &p);
    }, &pandite);
}

void editorSave(Pandite& pandite, PrStringView path) {
    if (pandite.index == kNoImage)
        return;
    own(pandite);
    if (pandite.editor.failure != PrStatus_Ok) {
        panditeSay(pandite, format("Cannot save {}: {}", pandite.editor.name, prStatusName(pandite.editor.failure)));
        return;
    }
    pandite.editor.saveTo = String(path);
    pump(pandite);
}

void editorRelease(Pandite& pandite) {
    Editor& editor = pandite.editor;
    destroy(editor.job);
    editor.job = nullptr;
    reset(editor);
}

void cropMode(Pandite& pandite, UiBuilder& ui, UiElement& area, PrLayoutNode page, ViewRect image) {
    Editor& editor = pandite.editor;
    CropSelection& crop = editor.crop;
    if (crop.image.x == 0 || image.size.x <= 0)
        return;
    float scale = image.size.x / float(crop.image.x);
    auto toImage = [&](PrVec2 p) {
        return PrVec2{(p.x - image.origin.x) / scale, (p.y - image.origin.y) / scale};
    };
    float reach = kHandleReach / scale;

    // A drag draws a new selection, or moves or resizes the one there. Shift keeps a square.
    PrUiDrag drag{};
    if (area.dragged(&drag, 0, 0)) {
        if (!crop.dragging)
            cropBegin(crop, toImage(drag.origin), reach);
        bool square = (prUiModifiers(ui.ui()) & PrAppModifierFlag_Shift) != 0;
        cropDragTo(crop, toImage(drag.position), square);
        if (drag.released || drag.cancelled)
            cropEnd(crop);
    }
    PrVec2 over{};
    if (!crop.dragging && area.pointer(&over)) {
        CropHandle handle = CropHandle_TopLeft;
        area.cursor(cursorOf(cropHit(crop, toImage(over), reach, &handle), handle));
    } else if (crop.dragging) {
        area.cursor(crop.grabbed == CropHit_Inside ? PrAppCursor_Move : cursorOf(crop.grabbed, crop.handle));
    }

    PrVec2 stage{};
    area.extent(&stage);
    if (crop.selected) {
        // The selection in px, on whole px.
        float left = prF32Round(image.origin.x + crop.rect.left * scale);
        float top = prF32Round(image.origin.y + crop.rect.top * scale);
        float right = prF32Round(image.origin.x + crop.rect.right * scale);
        float bottom = prF32Round(image.origin.y + crop.rect.bottom * scale);
        float w = right - left;
        float h = bottom - top;
        // The surround dims in four bands.
        struct Band {
            float x, y, w, h;
        };
        const Band bands[] = {
            {0, 0, stage.x, top},
            {0, bottom, stage.x, stage.y - bottom},
            {0, top, left, h},
            {right, top, stage.x - right, h},
        };
        for (uint32_t i = 0; i < countOf(bands); ++i) {
            const Band& b = bands[i];
            if (b.w > 0 && b.h > 0)
                ui.div(area, i + 1).absolute().left(b.x).top(b.y).w(b.w).h(b.h).bg(kCropDim).pointerEventsNone();
        }
        // The thirds, then the outline and its shadow.
        for (uint32_t k = 1; k < 3; ++k) {
            float x = prF32Round(left + w * float(k) / 3);
            float y = prF32Round(top + h * float(k) / 3);
            ui.div(area, 10 + k).absolute().left(x).top(top).w(1).h(h).bg(kCropThirds).pointerEventsNone();
            ui.div(area, 20 + k).absolute().left(left).top(y).w(w).h(1).bg(kCropThirds).pointerEventsNone();
        }
        ui.div(area, 30).absolute().left(left - 1).top(top - 1).w(w + 2).h(h + 2).border(3).borderColor(kCropShadow)
            .pointerEventsNone();
        ui.div(area, 31).absolute().left(left).top(top).w(w).h(h).border(1).borderColor(kCropLine).pointerEventsNone();
        PrRect shown{left, top, right, bottom};
        for (CropHandle handle = 0; handle < CropHandle_Count; ++handle) {
            PrVec2 c = cropHandleCenter(handle, shown);
            float half = kHandleSize / 2;
            ui.div(area, 40 + handle).absolute().left(prF32Round(c.x - half)).top(prF32Round(c.y - half))
                .size(kHandleSize).bg(kCropLine).border(1).borderColor(kCropShadow).pointerEventsNone();
        }
        // The size of the selection shows below the selection, or above it if the area ends there.
        PrIRect pixels{};
        if (cropPixels(crop, &pixels)) {
            constexpr float pillHeight = 22;
            float y = bottom + 8;
            if (y + pillHeight > stage.y)
                y = prF32Max(top - pillHeight - 8, 0);
            UiElement row = ui.div(area, 50).absolute().left(left).top(y).w(w).flexRow().justifyCenter()
                .pointerEventsNone();
            ui.div(row).h(pillHeight).px(10).flexRow().itemsCenter().roundedFull().bg(kTooltip).textXs()
                .textColor(kTooltipText).whitespaceNowrap()
                .text(format("{} x {}", pixels.right - pixels.left, pixels.bottom - pixels.top));
        }
    }

    // The bar of crop mode: what the selection crops to, and the buttons that apply it and leave.
    UiElement row = ui.div(page).absolute().left(0).top(8).wFull().flexRow().justifyCenter().pointerEventsNone();
    UiElement bar = ui.div(row).w(kCropBarWidth).maxW(uiPct(96)).h(kCropBarHeight).flexRow().itemsCenter().gap(6)
        .pl(12).pr(6).bg(kPanel).rounded(6).border(1).borderColor(kBorder).shadowMd().pointerEventsAuto();
    panditeMenuOn(pandite, ui, bar);
    PrIRect pixels{};
    bool selected = cropPixels(crop, &pixels);
    String label("Drag on the image to select an area");
    if (selected)
        label = format("Crop to {} x {}", pixels.right - pixels.left, pixels.bottom - pixels.top);
    ui.div(bar).flex1().minW(0).textSm().whitespaceNowrap().overflowHidden().text(label);
    bool apply = button(ui, bar, 1, "Apply", true, selected).w(kCropButton).clicked();
    bool cancel = button(ui, bar, 2, "Cancel", false, true).w(kCropButton).clicked();
    if (apply && selected)
        editorApplyCrop(pandite);
    else if (cancel)
        editorCancelCrop(pandite);
}

void cropKeys(Pandite& pandite, UiBuilder& ui, PrLayoutNode root) {
    CropSelection& crop = pandite.editor.crop;
    if (!pandite.editor.cropping || !crop.selected)
        return;
    struct Nudge {
        PrAppKey key;
        PrVec2 step;
    };
    const Nudge nudges[] = {
        {PrAppKey_Left, {-1, 0}},
        {PrAppKey_Right, {1, 0}},
        {PrAppKey_Up, {0, -1}},
        {PrAppKey_Down, {0, 1}},
    };
    for (const Nudge& nudge : nudges) {
        if (prUiKeyCaptured(ui.ui(), root, nudge.key, 0))
            cropNudge(crop, nudge.step);
        if (prUiKeyCaptured(ui.ui(), root, nudge.key, PrAppModifierFlag_Shift))
            cropNudge(crop, PrVec2{nudge.step.x * 10, nudge.step.y * 10});
    }
}

void resizeDialog(Pandite& pandite, UiBuilder& ui, PrLayoutNode page) {
    ResizeDialog& dialog = pandite.editor.resize;
    UiElement backdrop = ui.div(page).absolute().inset(0).bg(kBackdrop).flexRow().itemsCenter().justifyCenter();
    UiElement panel = ui.div(backdrop).w(400).maxW(uiPct(92)).flexCol().gap(12).p(20).bg(kPanelSolid).rounded(8)
        .border(1).borderColor(kBorder).shadowXl().pressFocus();
    bool focus = dialog.focus;
    dialog.focus = false;
    bool close = panel.keyCaptured(PrAppKey_Escape) || backdrop.clicked();
    bool enter = panel.keyCaptured(PrAppKey_Enter);

    ui.div(panel).textBase().semibold().text("Resize image");
    ui.div(panel).textSm().textColor(kTextDim).text(format("Current size: {} x {} px", dialog.from.x, dialog.from.y));

    // The rule's mode, as a segmented control.
    UiElement modes = ui.div(panel).flexRow().itemsCenter().gap(8);
    ui.div(modes).w(96).textSm().textColor(kTextDim).text("Resize by");
    UiElement segments = ui.div(modes).flexRow().rounded(4).border(1).borderColor(kFieldBorder).overflowHidden();
    const char* labels[] = {"Percent", "Fit in box", "Exact size"};
    for (uint32_t i = 0; i < countOf(labels); ++i) {
        UiElement segment = ui.div(segments, i + 1).px(10).py(5).textSm().cursorPointer();
        if (dialog.mode == ResizeMode(i))
            segment.bg(kAccent).textColor(kAccentText).semibold();
        else
            segment.hoverBg(kHover);
        segment.text(labels[i]);
        if (segment.clicked() && dialog.mode != ResizeMode(i)) {
            dialog.mode = ResizeMode(i);
            focus = true;
        }
    }

    UiElement fields = ui.div(panel, 10 + dialog.mode).flexCol().gap(8);
    if (dialog.mode == ResizeMode_Percent) {
        field(ui, fields, 1, "Scale", dialog.percent, "%", true, focus);
    } else if (dialog.mode == ResizeMode_Fit) {
        field(ui, fields, 1, "Max width", dialog.fitWidth, "px", false, focus);
        field(ui, fields, 2, "Max height", dialog.fitHeight, "px", false, false);
        check(ui, fields, 3, "Enlarge smaller images", dialog.enlarge);
    } else {
        // If the aspect is kept, a change to one side updates the other side.
        PrUVec2 from = dialog.from;
        if (field(ui, fields, 1, "Width", dialog.exactWidth, "px", false, focus) && dialog.keepAspect) {
            uint32_t w = sideOf(dialog.exactWidth);
            if (w != 0)
                dialog.exactHeight = format("{}", uint32_t(prF64Max(prF64Round(double(w) * from.y / from.x), 1)));
        }
        if (field(ui, fields, 2, "Height", dialog.exactHeight, "px", false, false) && dialog.keepAspect) {
            uint32_t h = sideOf(dialog.exactHeight);
            if (h != 0)
                dialog.exactWidth = format("{}", uint32_t(prF64Max(prF64Round(double(h) * from.x / from.y), 1)));
        }
        check(ui, fields, 3, "Keep aspect ratio", dialog.keepAspect);
    }

    ResizeSpec spec{};
    PrUVec2 extent{};
    PrStringView problem = validate(dialog, &spec, &extent);
    if (problem.isEmpty())
        ui.div(panel).textSm().text(format("New size: {} x {} px", extent.x, extent.y));
    else
        ui.div(panel).textSm().textColor(kError).text(problem);

    UiElement buttons = ui.div(panel).flexRow().justifyEnd().gap(8).mt(4);
    bool cancel = button(ui, buttons, 1, "Cancel", false, true).clicked();
    bool apply = button(ui, buttons, 2, "Resize", true, problem.isEmpty()).clicked();
    if ((apply || enter) && problem.isEmpty()) {
        dialog.open = false;
        editorPush(pandite, Edit{.kind = EditKind_Resize, .resize = spec});
        return;
    }
    if (close || cancel)
        dialog.open = false;
}

} // namespace pandite
