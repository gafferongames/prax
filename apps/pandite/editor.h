// The editor of the image on screen. It covers the edits, crop mode, the resize dialog, and saving the edited image as
// a PNG file. Edits run on the pool against the image decoded at its full extent.
//
// Edits belong to the image on screen, and they never touch the file until the user saves. The editor drops the edits
// when another image comes on screen. The editor runs one job at a time on the pool: a decode, an edit or a save. It
// takes the result of the job on the main thread (editorUpdate).

#pragma once

#include "pr/base.h"
#include "pr/string.h"
#include "pr/texture.h"
#include "pr/ui_element.h"

#include "apps/pandite/edit.h"
#include "apps/pandite/view_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

struct Pandite;
struct EditorJob;

// The state of the resize dialog while it is open. `from` is the extent of the image that the dialog resizes. `mode` is
// the mode of the resize rule. The strings hold the text of the dialog's fields, and `enlarge` and `keepAspect` are its
// checkboxes. `focus` is set if the first field takes focus in the next round. The fields of the exact extent keep the
// aspect while `keepAspect` is set.
struct ResizeDialog {
    bool open;
    PrUVec2 from;
    ResizeMode mode;
    String percent;
    String fitWidth;
    String fitHeight;
    String exactWidth;
    String exactHeight;
    bool enlarge;
    bool keepAspect;
    bool focus;
};

// The edits of the image on screen. `name` is the name of the image's file. `base` is the image decoded at its full
// extent, once the decode finishes, and `failure` is why the decode failed. `stack` is the stack of edits, and `job` is
// the job under way on the pool. `shown` is the image after the last applied edit, in the form that the view shows.
// Crop mode's selection and the resize dialog refer to the image as edited. A save waits in `saveTo` until the edits
// before it apply.
struct Editor {
    String name;
    Bitmap base;
    PrStatus failure;
    EditStack stack;
    EditorJob* PR_NULLABLE job;
    const PrTextureSource* PR_NULLABLE shown;

    bool cropping;
    CropSelection crop;
    ResizeDialog resize;
    String saveTo;
};

// Returns true if the image on screen differs from its file, or will once its edits apply.
bool editorIsEdited(const Pandite& pandite);

// Returns true if a decode, an edit or a save is running or waiting.
bool editorIsBusy(const Pandite& pandite);

// Returns the image after the last applied edit, or null if none applied, and writes its extent to `*extent`.
const PrTextureSource* PR_NULLABLE editorShown(const Pandite& pandite, PrUVec2* extent);

// Returns the image on screen after its last applied edit, or null if none applied.
const Bitmap* PR_NULLABLE editorEdited(const Pandite& pandite);

// Returns the text that the view shows while the editor works, which says what the editor works on. Returns empty text
// while the editor is idle.
String editorBusyText(const Pandite& pandite);

// Adds an edit of the image on screen.
void editorPush(Pandite& pandite, const Edit& edit);

// Takes back the newest edit.
void editorUndo(Pandite& pandite);

// Takes the result of a finished job and starts the next job. Drops the edits of an image that left the screen. Called
// on the main thread before each build.
void editorUpdate(Pandite& pandite);

// Enter and leave crop mode. editorStartCrop enters crop mode on the image on screen, which has extent `extent`.
// editorApplyCrop leaves crop mode and applies the selection. editorCancelCrop leaves crop mode without applying it.
void editorStartCrop(Pandite& pandite, PrUVec2 extent);
void editorApplyCrop(Pandite& pandite);
void editorCancelCrop(Pandite& pandite);

// Opens the resize dialog on the image on screen, which has extent `extent`.
void editorOpenResize(Pandite& pandite, PrUVec2 extent);

// Asks the user where to save the edited image on screen, and saves it there once its edits apply. Asks for
// confirmation before it replaces the image's own file.
void editorSaveAs(Pandite& pandite);

// Saves the edited image on screen at `path` once its edits apply.
void editorSave(Pandite& pandite, PrStringView path);

// Frees what the editor holds. Call it after the pool that ran the editor's jobs is gone.
void editorRelease(Pandite& pandite);

// Handles crop mode's input on the image area `area`. A drag in the area draws, moves and resizes the selection.
// Declares the crop overlay over the image, which is at `image` in px of the area. The overlay has the dimmed surround,
// the selection's lines and handles, and the selection's size. Declares the bar of crop mode on `page`.
void cropMode(Pandite& pandite, UiBuilder& ui, UiElement& area, PrLayoutNode page, ViewRect image);

// Handles the arrow keys while there is a selection. An arrow key moves crop mode's selection by one pixel, or by ten
// with Shift. These keys are taken ahead of the commands of `root`.
void cropKeys(Pandite& pandite, UiBuilder& ui, PrLayoutNode root);

// Declares the resize dialog over `page` while it is open.
void resizeDialog(Pandite& pandite, UiBuilder& ui, PrLayoutNode page);

} // namespace pandite

PR_ASSUME_NONNULL_END
