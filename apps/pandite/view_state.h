// The view of one image: how it is sized in the viewport, and where it is panned to. This is pure math over sizes in
// device pixels. The view feeds it input and reads the place of the image from it.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

// How the image is sized in the viewport: fitted, at one image pixel per device pixel, or at a free zoom.
enum : uint8_t {
    ViewMode_Fit,
    ViewMode_OneToOne,
    ViewMode_Zoom,
};
typedef uint8_t ViewMode;

// The range of zoom that a view allows, in device pixels per image pixel.
constexpr float kMinZoom = 0.01f;
constexpr float kMaxZoom = 64.0f;

// The lowest zoom at which each image pixel shows as a crisp square. Below it, the image is smoothed.
constexpr float kCrispZoom = 3.0f;

// A rectangle given by its corner and its size.
struct ViewRect {
    PrVec2 origin;
    PrVec2 size;
};

// The view of an image: its mode, its zoom in the Zoom mode, and the offset of the image's center from the viewport's
// center. On an axis where the image overflows the viewport, the offset never lets the image leave the viewport. On an
// axis where the image fits, the image is centered.
struct ViewState {
    ViewMode mode = ViewMode_Fit;
    float zoom = 1;
    PrVec2 pan = {};
};

// Returns the scale of an image of `image` device pixels in a viewport of `viewport`. The scale is the fit in Fit mode,
// 1 in OneToOne mode, and the zoom in Zoom mode. The fit never enlarges the image.
float viewScale(const ViewState& view, PrVec2 image, PrVec2 viewport);

// Returns where the image lands in the viewport.
ViewRect viewImageRect(const ViewState& view, PrVec2 image, PrVec2 viewport);

// Fits the image to the viewport, centered.
void viewFit(ViewState& view);

// Zooms to `zoom`, clamped to the range that a view allows. The image point under `anchor`, a viewport position, stays
// where it is. viewOneToOne zooms to 1, and the view keeps that zoom as the viewport resizes.
void viewZoomTo(ViewState& view, float zoom, PrVec2 image, PrVec2 viewport, PrVec2 anchor);
void viewOneToOne(ViewState& view, PrVec2 image, PrVec2 viewport, PrVec2 anchor);

// Multiplies the zoom by `factor`. The image point under `anchor` stays where it is.
void viewZoomBy(ViewState& view, float factor, PrVec2 image, PrVec2 viewport, PrVec2 anchor);

// Moves the image by `delta` device pixels, as far as the viewport allows.
void viewPanBy(ViewState& view, PrVec2 delta, PrVec2 image, PrVec2 viewport);

// Returns the pan, clamped to what the viewport allows.
PrVec2 viewClampedPan(const ViewState& view, PrVec2 image, PrVec2 viewport);

// Returns the image pixel under `point`, a viewport position.
PrVec2 viewToImage(const ViewState& view, PrVec2 point, PrVec2 image, PrVec2 viewport);

// Returns the zoom as the user reads it, a percentage of the image's pixels.
float viewPercent(const ViewState& view, PrVec2 image, PrVec2 viewport);

// The largest number of images that tiles show at once.
constexpr uint32_t kMaxTiles = 8;

// Writes the tiles for `count` images in `area` to `out`, in reading order, on whole units. `count` is at most
// kMaxTiles. The grid is the one that shows a 3:2 picture largest. If the last row has fewer images, its tiles share
// the whole width. Tiles are `gap` apart, and the outer edges of the grid are the edges of the area.
void viewTiles(PrVec2 area, uint32_t count, float gap, ViewRect* out);

} // namespace pandite

PR_ASSUME_NONNULL_END
