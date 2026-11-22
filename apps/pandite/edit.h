// The viewer's edits of the image on screen: quarter turns, mirrors, a crop and a resize. They apply to the decoded
// pixels of the image.
//
// Edits never touch the file until the user saves. A stack holds the edits and the image after each edit, so undo shows
// the image before the last edit at once. An edit runs on a worker while the next edits wait. The stack plans the work
// (editNextJob) and takes the results (editFinish). Otherwise the stack is plain data. Crop mode's selection is plain
// data too, in pixels of the image that the crop applies to.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/blob_ref.h"
#include "pr/geom.h"
#include "pr/image.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The largest side of an image that an edit makes, in pixels.
constexpr uint32_t kMaxSide = 65535;

// An image in memory: packed Rgba8 pixels of sRGB color and straight alpha. The holders of a bitmap share its bytes. An
// empty bitmap has no bytes and a zero extent.
struct Bitmap {
    BlobRef bytes;
    PrUVec2 extent;
};

// Returns a bitmap of extent `extent` that takes ownership of `pixels`.
Bitmap bitmapOf(Array<uint8_t>&& pixels, PrUVec2 extent);

// Returns the pixels of `bitmap`.
PrPixelView bitmapView(const Bitmap& bitmap);

// Decodes the first frame of the image file at `path` with `codecs` and writes it to `*out`. The bitmap has the full
// extent of the image and is upright, as the image displays. A truncated file decodes the pixels that it holds and
// returns Truncated. Runs on any thread.
PrStatus bitmapDecode(const PrImageCodecSet* codecs, PrStringView path, Bitmap* out);

// Writes `bitmap` as a PNG file at `path` and replaces any file there. A reader never sees a partly written file. Runs
// on any thread.
PrStatus bitmapWritePng(const PrImageCodecSet* codecs, const Bitmap& bitmap, PrStringView path);

// What an edit does.
enum : uint8_t {
    EditKind_RotateLeft,        // a quarter turn counterclockwise
    EditKind_RotateRight,       // a quarter turn clockwise
    EditKind_FlipHorizontal,    // left to right
    EditKind_FlipVertical,      // top to bottom
    EditKind_Crop,              // to the pixels of Edit::crop
    EditKind_Resize,            // to the extent that Edit::resize gives
};
typedef uint8_t EditKind;

// How a resize finds its extent.
enum : uint8_t {
    ResizeMode_Percent,         // a percent of each side
    ResizeMode_Fit,             // the largest that fits a box and keeps the aspect, and no larger unless `enlarge`
    ResizeMode_Exact,           // the given extent
};
typedef uint8_t ResizeMode;

// A rule for the extent of a resize.
struct ResizeSpec {
    ResizeMode mode;
    float percent;
    PrUVec2 size;
    bool enlarge;
};

// One edit. A crop keeps the rectangle `crop`, in pixels of the image before the edit. A resize resamples the image to
// the extent that `resize` gives.
struct Edit {
    EditKind kind;
    PrIRect crop;
    ResizeSpec resize;
};

// Returns the extent that `spec` gives for an image of extent `from`. Each side is at least one pixel. Returns zero if
// a side of the result is larger than kMaxSide or if the spec is not valid.
PrUVec2 resizeExtent(const ResizeSpec& spec, PrUVec2 from);

// Returns the extent of the image that `edit` makes from an image of extent `from`, or zero if the edit cannot apply.
PrUVec2 editExtent(const Edit& edit, PrUVec2 from);

// Applies `edit` to `image` and returns the result. Returns the empty bitmap if the edit cannot apply. Runs on any
// thread.
Bitmap editApply(const Edit& edit, const Bitmap& image);

// Returns the name of an edit kind for use in a message: "Rotate", "Flip", "Crop" or "Resize".
const char* editLabel(EditKind kind);

// What editUndo took back.
enum : uint8_t {
    EditUndo_Nothing,           // there was no edit
    EditUndo_Dequeued,          // an edit that had not started. The image is unchanged.
    EditUndo_DroppedRunning,    // the running edit. Its result is dropped when it finishes. The image is unchanged.
    EditUndo_Reverted,          // the last applied edit. The image is the one before that edit.
};
typedef uint8_t EditUndo;

// An edit that applied, and the image that it made.
struct AppliedEdit {
    Edit edit;
    Bitmap result;
};

// The edits of the image on screen: the applied edits in order, the running edit, and the queued edits.
struct EditStack {
    Array<AppliedEdit> applied;
    Array<Edit> queued;
    bool running;
    bool discardRunning;
    Edit runningEdit;
};

// Returns the image to show: the result of the last applied edit, or `base`.
const Bitmap& editCurrent(const EditStack& stack, const Bitmap& base);

// Returns true if an edit is applied, running or queued. The image then differs from its file, or will.
bool editIsEdited(const EditStack& stack);

// Returns true if an edit is running or queued.
bool editIsBusy(const EditStack& stack);

// Adds an edit. It runs after the edits before it.
void editPush(EditStack& stack, const Edit& edit);

// Starts the next queued edit if no edit is running. Marks the edit as running, writes it to `*out` and returns true.
// The caller applies the edit to the image that editCurrent returns at the time of the call, and gives the result to
// editFinish.
bool editNextJob(EditStack& stack, Edit* out);

// Takes the result of the running edit. An empty `result` means that the edit failed. Returns true if the image on
// screen changed. Returns false if the edit failed or if the stack drops the result, as it does for an edit that was
// undone while it ran.
bool editFinish(EditStack& stack, Bitmap result);

// Takes back the newest edit. That is a queued edit if there is one, then the running edit, then the last applied edit.
EditUndo editUndo(EditStack& stack);

// Forgets every edit, so the image is its base again. Drops the result of a running edit.
void editClear(EditStack& stack);

// One of the eight handles of a crop selection, on its corners and sides.
enum : uint8_t {
    CropHandle_TopLeft,
    CropHandle_Top,
    CropHandle_TopRight,
    CropHandle_Right,
    CropHandle_BottomRight,
    CropHandle_Bottom,
    CropHandle_BottomLeft,
    CropHandle_Left,
    CropHandle_Count,
};
typedef uint8_t CropHandle;

// What a press in crop mode grabs.
enum : uint8_t {
    CropHit_Outside,            // a new selection begins
    CropHit_Inside,             // the selection moves
    CropHit_Handle,             // the selection's handle resizes it
};
typedef uint8_t CropHit;

// Crop mode's selection and the drag under way. The selection is in pixels of the image that the crop applies to, so a
// zoom or a pan of the view never disturbs it. It is held as floats while a drag moves it. Every change keeps the
// selection within the image.
struct CropSelection {
    PrUVec2 image;
    bool selected;
    PrRect rect;

    bool dragging;
    CropHit grabbed;
    CropHandle handle;
    PrVec2 anchor;              // where a new selection began, or where a move grabbed the selection
    PrRect start;               // the selection when a move or a resize began
};

// Returns the center of `handle` on `rect`.
PrVec2 cropHandleCenter(CropHandle handle, PrRect rect);

// Returns what a press at `at` grabs, where `at` is in pixels of the image. Writes a grabbed handle to `*handle`. A
// press grabs a handle that is within `reach` image pixels of `at`. Corners are tested before sides, and the handle of
// a side spans the whole side.
CropHit cropHit(const CropSelection& crop, PrVec2 at, float reach, CropHandle* handle);

// Begins a drag at `at`, in pixels of the image. The drag grabs what is within `reach`.
void cropBegin(CropSelection& crop, PrVec2 at, float reach);

// Moves the drag to `at`. If `square` is set, a new selection and a resize by a corner stay square.
void cropDragTo(CropSelection& crop, PrVec2 at, bool square);

// Ends the drag. A selection less than a pixel on a side is dropped.
void cropEnd(CropSelection& crop);

// Selects the whole image.
void cropSelectAll(CropSelection& crop);

// Moves the selection by `delta` pixels and keeps it within the image.
void cropNudge(CropSelection& crop, PrVec2 delta);

// Writes the selection, rounded to whole pixels, to `*out`, and returns true if it is at least a pixel on a side.
bool cropPixels(const CropSelection& crop, PrIRect* out);

} // namespace pandite

PR_ASSUME_NONNULL_END
