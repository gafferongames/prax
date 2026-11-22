// Pandite's view. It shows the image on screen, the toolbar that hides while the pointer rests, the gallery strip of
// the folder's thumbnails, the editor's modes and dialogs, and the keyboard shortcuts window. It is built with the
// elements of lib/ui (pr/ui_element.h) in pandite's own theme.

#pragma once

#include "pr/app.h"
#include "pr/array.h"
#include "pr/atomic.h"
#include "pr/file.h"
#include "pr/set.h"
#include "pr/string.h"
#include "pr/studio_window.h"
#include "pr/ui_element.h"

#include "apps/pandite/commands.h"
#include "apps/pandite/compare.h"
#include "apps/pandite/edit.h"
#include "apps/pandite/editor.h"
#include "apps/pandite/files.h"
#include "apps/pandite/folder.h"
#include "apps/pandite/info.h"
#include "apps/pandite/loader.h"
#include "apps/pandite/peek.h"
#include "apps/pandite/view_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The sizes of the gallery's thumbnails.
enum : uint8_t {
    GallerySize_Small,
    GallerySize_Medium,
    GallerySize_Large,
};
typedef uint8_t GallerySize;

// What the commands of a round act on. `stage` is the extent of the image area in px. `viewport` is the extent of the
// viewport that the view places the image in, in device pixels. `origin` is the viewport's origin in the image area, in
// px. The viewport is the whole image area, except in a tile comparison. `image` is the extent of the image in device
// pixels, or zero if there is no image. `ratio` is the number of device pixels per px.
struct ViewFrame {
    PrVec2 stage;
    PrVec2 viewport;
    PrVec2 origin;
    PrVec2 image;
    float ratio;
};

// The state of the keyboard shortcuts window while it is open. `query` is the text that the user typed. `highlight` is
// the highlighted row among the commands that match `query`. `focus` is set if the search field takes focus in the next
// round.
struct Shortcuts {
    bool open;
    String query;
    uint32_t highlight;
    bool focus;
};

// The state of the context menu while it is open. `at` is where the menu opens, in px of the window. `centered` is set
// for a menu that a key opened, which opens at the center of the image area instead. `highlight` is the highlighted row
// of the menu, if any.
struct ContextMenu {
    bool open;
    bool centered;
    PrVec2 at;
    uint32_t highlight;
};

// The state of a confirmation dialog while it is open. The dialog asks before an action that the user cannot take back.
// `message` is the question, and `act` is the label of the button that acts. `action` and `names` are the action and
// the files that it acts on. `path` is the file that a save replaces. `acting` selects the button that Enter presses:
// the button that acts if it is set, and the button that keeps the files otherwise. `focus` is set if the dialog takes
// focus in the next round.
struct Confirm {
    bool open;
    String message;
    String act;
    FileAction action;
    bool trash;                 // the action moves the files to the trash, rather than deleting them
    bool replace;               // the action saves the edited image at `path`, in place of its file
    Array<String> names;
    String path;
    bool acting;
    bool focus;
};

// A copy of the image on screen to the clipboard. The image decodes on the pool. `path` is the file of the image. Once
// `done` is set, `bitmap` holds the pixels as the image displays, or `status` says why the image did not decode.
struct ImageCopy {
    String path;
    const PrImageCodecSet* codecs;
    Bitmap bitmap;
    PrStatus status;
    Atomic<bool> done;
    PrApp* PR_NULLABLE app;
};

// A read of the facts of an image file for the information panel. The read runs on the pool. `name` and `path` name the
// file. `facts` holds the facts once `done` is set.
struct InfoRead {
    String name;
    String path;
    const PrImageCodecSet* codecs;
    ImageFacts facts;
    Atomic<bool> done;
    PrApp* PR_NULLABLE app;
};

// The information panel. `visible` is set while the panel shows. `name` is the name of the image that `facts`
// describes. `reading` is the read under way.
struct InfoPanel {
    bool visible;
    String name;
    ImageFacts facts;
    InfoRead* PR_NULLABLE reading;
};

// The viewer: what it shows, how it shows it, and what it decodes with. `app` and `window` are set while the window is
// open.
struct Pandite {
    const PrImageCodecSet* codecs;
    Loader* loader;
    Commands* commands;
    PrTaskPool* pool;
    PrApp* PR_NULLABLE app;
    PrStudioWindow* PR_NULLABLE window;

    // The folder of the image on screen, the index of that image, and the watch on the folder. `folderChanged` is set
    // if the watch saw the folder's entries change since the folder was last listed. The watch's thread sets it.
    Folder folder;
    uint32_t index = kNoImage;
    PrFileWatch* PR_NULLABLE watch;
    Atomic<uint32_t> folderChanged;

    ViewState view;
    double shownAt;             // the time when the image on screen came on screen
    bool galleryVisible = true;
    GallerySize gallerySize = GallerySize_Medium;
    float galleryScroll;        // px from the first thumbnail's place to the strip's left edge
    bool revealCurrent = true;  // the gallery scrolls to the image on screen in the next build
    bool fullscreen;
    bool peek;                  // the magnifier of pixel peek follows the pointer over the image area
    bool quit;
    Shortcuts shortcuts;
    ContextMenu menu;
    Confirm confirm;
    Editor editor;
    Compare compare;

    // The names of the selected images of the folder. `selectRun` is set during a run of the keys that select and step,
    // which lasts until a round finds Shift up. `selectClears` is set if the run clears the selection of the images
    // that it leaves. A run that began on a selected image does so.
    Set<String> selected;
    bool selectRun;
    bool selectClears;

    // The message at the bottom of the window, and the time when it appeared. `copying` is the copy of the image to the
    // clipboard while it decodes. `info` is the information panel.
    String toast;
    double toastAt;
    ImageCopy* PR_NULLABLE copying;
    InfoPanel info;

    // What the commands of the round act on, and the time of the round.
    ViewFrame frame;
    double time;

    // The time when the pointer last moved or a key was last pressed, and where the pointer was. That activity shows
    // the toolbar. A pinned toolbar shows whatever the pointer does, so that a capture of the window shows it.
    double activeAt;
    PrVec2 pointer;
    bool toolbarPinned;
};

// Shows `text` at the bottom of the window for a while.
void panditeSay(Pandite& pandite, String text);

// Opens the context menu at the pointer if `region` is right-clicked. `region` is a part of the window where a
// right-click opens the menu.
void panditeMenuOn(Pandite& pandite, UiBuilder& ui, UiElement& region);

// Returns the handler that runs the viewer's commands (commandsCreate) on `pandite`.
CommandHandler panditeCommandHandler(Pandite& pandite);

// Opens the image at `path` with the images of its folder. If `path` is a directory, opens its first image. Returns
// false and leaves the viewer unchanged if `path` names neither.
bool panditeOpen(Pandite& pandite, PrStringView path);

// Applies what changed since the last frame. Takes the results of the work on the pool. Lists the folder again if its
// entries changed, and keeps the image on screen, or the image that took its place.
void panditeRefresh(Pandite& pandite);

// Stops the watch on the folder.
void panditeClose(Pandite& pandite);

// Frees what the viewer holds. Call it after the pool that ran the viewer's work is gone.
void panditeRelease(Pandite& pandite);

// Declares a round of the view under `root` and handles the input of the round. `root` fills the window, whose extent
// is `extent` px.
void buildPandite(Pandite& pandite, UiBuilder& ui, PrLayoutNode root, PrVec2 extent);

} // namespace pandite

PR_ASSUME_NONNULL_END
