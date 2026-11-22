// The comparison of selected images. A slider comparison shows one selected image against the image on screen, split by
// a slider. A tile comparison shows two to eight selected images in tiles.
//
// The comparison follows the selection. An image that leaves the selection loses its tile, and an image that joins the
// selection gets a tile. The comparison ends if it cannot show the selection. The view's zoom and pan apply to every
// image in the comparison.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/string.h"
#include "pr/ui_element.h"

#include "apps/pandite/loader.h"
#include "apps/pandite/view_state.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

struct Pandite;
struct ViewFrame;

// The state of the comparison. `on` is set while the comparison shows, and `tiles` is set for a tile comparison.
// `names` holds the names of the compared images in the order of the folder. `split` is the slider's position as a
// fraction of the width of the image area. `splitting` is set while a drag moves the slider.
struct Compare {
    bool on;
    bool tiles;
    Array<String> names;
    float split = 0.5f;
    bool splitting;
};

// The placement of one picture in the image area. `picture` is null if the pane has no picture. `image` is the extent
// of the picture in image pixels. `rect` is where the view puts the picture. `clip` is the part of the image area that
// the pane draws in, in px of the image area.
struct ComparePane {
    const Picture* PR_NULLABLE picture;
    PrVec2 image;
    ViewRect rect;
    ViewRect clip;
};

// Writes the panes of the image area to `out` and returns their number, which is at most kMaxTiles. Without a
// comparison, the only pane is `shown`, the image on screen. A slider comparison gives the selected image and then the
// image on screen. A tile comparison gives the tiles.
uint32_t comparePanes(Pandite& pandite, const Picture* PR_NULLABLE shown, const ViewFrame& frame, ComparePane* out);

// Returns true if a comparison can start with the selection. Also returns true if a comparison shows, because the
// command then ends it.
bool compareEnabled(const Pandite& pandite);

// Starts a comparison of the selected images, or ends the one that shows.
void compareToggle(Pandite& pandite);

// Ends the comparison. After a tile comparison, the view fits the image on screen again.
void compareEnd(Pandite& pandite);

// Updates the comparison to follow the selection and the folder. Wants the pictures of the compared images from the
// loader. Called before each build.
void compareFollow(Pandite& pandite);

// In a tile comparison, writes to `frame` the tile that zoom and pan act on. If `over` is set, that is the tile under
// `pointer`, which is in px of the image area. Otherwise it is the first tile that has its picture. Writes the extent
// of the tile's image, the tile's viewport, and the tile's origin in the image area.
void compareFrame(Pandite& pandite, ViewFrame& frame, PrVec2 pointer, bool over);

// Handles a drag on the slider's line, which moves the slider. Returns true while the drag moves the slider and not the
// view.
bool compareSplit(Pandite& pandite, UiElement& area, const ViewFrame& frame);

// Declares the comparison in the image area `area`, in place of `shown`, the image on screen. A slider comparison
// declares the two images split by the slider, with their names and the slider's line. A tile comparison declares the
// tiles, each with its outline and its name.
void compareView(Pandite& pandite, UiBuilder& ui, UiElement& area, const Picture* PR_NULLABLE shown, const ViewFrame& frame);

// Returns the line about comparing that the selection box shows. The line names the key that starts or ends a
// comparison, or says why a comparison cannot start.
PrStringView compareHint(const Pandite& pandite);

} // namespace pandite

PR_ASSUME_NONNULL_END
