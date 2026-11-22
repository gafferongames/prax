// The view of one image.

#include "apps/pandite/view_state.h"

#include "pr/float.h"

namespace pandite {

namespace {

float clampZoom(float zoom) {
    if (!prF32IsFinite(zoom))
        return 1;
    return zoom < kMinZoom ? kMinZoom : zoom > kMaxZoom ? kMaxZoom : zoom;
}

float clampAxis(float value, float limit) {
    return value < -limit ? -limit : value > limit ? limit : value;
}

bool isEmpty(PrVec2 size) {
    return !(size.x > 0) || !(size.y > 0);
}

} // namespace

float viewScale(const ViewState& view, PrVec2 image, PrVec2 viewport) {
    switch (view.mode) {
        case ViewMode_Fit: {
            if (isEmpty(image))
                return 1;
            float fit = prF32Min(viewport.x / image.x, viewport.y / image.y);
            return fit < 1 ? fit : 1;
        }
        case ViewMode_OneToOne: return 1;
        default: return clampZoom(view.zoom);
    }
}

ViewRect viewImageRect(const ViewState& view, PrVec2 image, PrVec2 viewport) {
    float scale = viewScale(view, image, viewport);
    PrVec2 size{image.x * scale, image.y * scale};
    PrVec2 pan = viewClampedPan(view, image, viewport);
    PrVec2 origin{(viewport.x - size.x) * 0.5f + pan.x, (viewport.y - size.y) * 0.5f + pan.y};
    return ViewRect{.origin = origin, .size = size};
}

void viewFit(ViewState& view) {
    view.mode = ViewMode_Fit;
    view.pan = PrVec2{};
}

void viewZoomTo(ViewState& view, float zoom, PrVec2 image, PrVec2 viewport, PrVec2 anchor) {
    if (isEmpty(image) || isEmpty(viewport) || !prF32IsFinite(zoom))
        return;
    ViewRect before = viewImageRect(view, image, viewport);
    float oldScale = viewScale(view, image, viewport);
    float newScale = clampZoom(zoom);
    // The image point under the anchor. The new pan puts this point back under the anchor.
    PrVec2 point{(anchor.x - before.origin.x) / oldScale, (anchor.y - before.origin.y) / oldScale};
    view.mode = ViewMode_Zoom;
    view.zoom = newScale;
    PrVec2 centered{(viewport.x - image.x * newScale) * 0.5f, (viewport.y - image.y * newScale) * 0.5f};
    view.pan = PrVec2{anchor.x - point.x * newScale - centered.x, anchor.y - point.y * newScale - centered.y};
    view.pan = viewClampedPan(view, image, viewport);
}

void viewOneToOne(ViewState& view, PrVec2 image, PrVec2 viewport, PrVec2 anchor) {
    viewZoomTo(view, 1, image, viewport, anchor);
    view.mode = ViewMode_OneToOne;
}

void viewZoomBy(ViewState& view, float factor, PrVec2 image, PrVec2 viewport, PrVec2 anchor) {
    viewZoomTo(view, viewScale(view, image, viewport) * factor, image, viewport, anchor);
}

void viewPanBy(ViewState& view, PrVec2 delta, PrVec2 image, PrVec2 viewport) {
    view.pan = PrVec2{view.pan.x + delta.x, view.pan.y + delta.y};
    view.pan = viewClampedPan(view, image, viewport);
}

PrVec2 viewClampedPan(const ViewState& view, PrVec2 image, PrVec2 viewport) {
    float scale = viewScale(view, image, viewport);
    float limitX = prF32Max((image.x * scale - viewport.x) * 0.5f, 0);
    float limitY = prF32Max((image.y * scale - viewport.y) * 0.5f, 0);
    return PrVec2{clampAxis(view.pan.x, limitX), clampAxis(view.pan.y, limitY)};
}

PrVec2 viewToImage(const ViewState& view, PrVec2 point, PrVec2 image, PrVec2 viewport) {
    ViewRect rect = viewImageRect(view, image, viewport);
    float scale = viewScale(view, image, viewport);
    return PrVec2{(point.x - rect.origin.x) / scale, (point.y - rect.origin.y) / scale};
}

float viewPercent(const ViewState& view, PrVec2 image, PrVec2 viewport) {
    return viewScale(view, image, viewport) * 100;
}

void viewTiles(PrVec2 area, uint32_t count, float gap, ViewRect* out) {
    if (count == 0 || area.x <= 0 || area.y <= 0)
        return;
    uint32_t columns = 1;
    float best = 0;
    for (uint32_t c = 1; c <= count; ++c) {
        uint32_t rows = (count + c - 1) / c;
        float score = prF32Min(area.x / float(c) / 1.5f, area.y / float(rows));
        if (score >= best) {
            best = score;
            columns = c;
        }
    }
    uint32_t rows = (count + columns - 1) / columns;
    float lowGap = prF32Floor(gap * 0.5f);
    float highGap = prF32Ceil(gap * 0.5f);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t row = i / columns;
        uint32_t inRow = row + 1 == rows ? count - row * columns : columns;
        uint32_t column = i - row * columns;
        float x0 = prF32Round(area.x * float(column) / float(inRow));
        float x1 = prF32Round(area.x * float(column + 1) / float(inRow));
        float y0 = prF32Round(area.y * float(row) / float(rows));
        float y1 = prF32Round(area.y * float(row + 1) / float(rows));
        float left = column == 0 ? 0 : lowGap;
        float right = column + 1 == inRow ? 0 : highGap;
        float top = row == 0 ? 0 : lowGap;
        float bottom = row + 1 == rows ? 0 : highGap;
        out[i] = ViewRect{
            .origin = PrVec2{x0 + left, y0 + top},
            .size = PrVec2{prF32Max(x1 - x0 - left - right, 0), prF32Max(y1 - y0 - top - bottom, 0)},
        };
    }
}

} // namespace pandite
