// The magnifier of pixel peek.

#include "apps/pandite/peek.h"

#include "pr/float.h"

#include "apps/pandite/theme.h"

namespace pandite {

namespace {

// One square of the magnifier. `square` is its rectangle. `pane` is the pane whose picture it shows. `source` is the
// point of the area that the center of the square shows. `bounds` is the part of the area that the square may cover.
struct Lens {
    ViewRect square;
    uint32_t pane;
    PrVec2 source;
    ViewRect bounds;
};

// Returns the square of the magnifier centered on `center`.
ViewRect squareAt(PrVec2 center) {
    return ViewRect{
        .origin = PrVec2{prF32Round(center.x - kPeekSide / 2), prF32Round(center.y - kPeekSide / 2)},
        .size = PrVec2{kPeekSide, kPeekSide},
    };
}

// Returns the part of `a` within `b`, which may be empty.
ViewRect intersect(ViewRect a, ViewRect b) {
    float left = prF32Max(a.origin.x, b.origin.x);
    float top = prF32Max(a.origin.y, b.origin.y);
    float right = prF32Min(a.origin.x + a.size.x, b.origin.x + b.size.x);
    float bottom = prF32Min(a.origin.y + a.size.y, b.origin.y + b.size.y);
    return ViewRect{.origin = PrVec2{left, top}, .size = PrVec2{prF32Max(right - left, 0), prF32Max(bottom - top, 0)}};
}

// Returns true if `rect` holds `point`.
bool holds(ViewRect rect, PrVec2 point) {
    return point.x >= rect.origin.x && point.x < rect.origin.x + rect.size.x && point.y >= rect.origin.y &&
        point.y < rect.origin.y + rect.size.y;
}

// Returns the center of `rect`.
PrVec2 centerOf(ViewRect rect) {
    return PrVec2{rect.origin.x + rect.size.x / 2, rect.origin.y + rect.size.y / 2};
}

} // namespace

void peekView(UiBuilder& ui, PrLayoutNode area, Span<const ComparePane> panes, bool tiles, PrVec2 pointer, float ratio) {
    uint32_t count = panes.count();
    if (count == 0)
        return;
    ViewRect whole{};
    for (const ComparePane& pane : panes) {
        ViewRect c = pane.clip;
        float right = prF32Max(whole.origin.x + whole.size.x, c.origin.x + c.size.x);
        float bottom = prF32Max(whole.origin.y + whole.size.y, c.origin.y + c.size.y);
        whole.size = PrVec2{right, bottom};
    }
    Lens lenses[kMaxTiles];
    uint32_t lensCount = 0;
    if (tiles) {
        uint32_t under = count;
        for (uint32_t i = 0; i < count; ++i) {
            if (holds(panes[i].clip, pointer))
                under = i;
        }
        if (under == count)
            return;
        PrVec2 from = centerOf(panes[under].clip);
        for (uint32_t i = 0; i < count; ++i) {
            PrVec2 to = centerOf(panes[i].clip);
            PrVec2 spot{pointer.x + to.x - from.x, pointer.y + to.y - from.y};
            lenses[lensCount++] = Lens{.square = squareAt(spot), .pane = i, .source = spot, .bounds = panes[i].clip};
        }
    } else if (count == 2) {
        float half = (kPeekSide + 6) / 2;
        lenses[lensCount++] = Lens{squareAt(PrVec2{pointer.x - half, pointer.y}), 0, pointer, whole};
        lenses[lensCount++] = Lens{squareAt(PrVec2{pointer.x + half, pointer.y}), 1, pointer, whole};
    } else {
        lenses[lensCount++] = Lens{squareAt(pointer), 0, pointer, whole};
    }

    for (uint32_t i = 0; i < lensCount; ++i) {
        const Lens& lens = lenses[i];
        ViewRect clip = intersect(lens.square, lens.bounds);
        if (clip.size.x <= 0 || clip.size.y <= 0)
            continue;
        UiElement square = ui.div(area, PrUiKey(i) + 0x7065656b).absolute().left(clip.origin.x).top(clip.origin.y)
            .w(clip.size.x).h(clip.size.y).overflowHidden().bg(kCanvas).pointerEventsNone();
        const ComparePane& pane = panes[lens.pane];
        if (pane.picture != nullptr && pane.picture->source != nullptr && pane.image.x > 0 && pane.rect.size.x > 0) {
            // Declares the picture again, enlarged about the source point. The source point lands on the center of the
            // square.
            PrVec2 at{prF32Round(pane.rect.origin.x), prF32Round(pane.rect.origin.y)};
            float scale = pane.rect.size.x * ratio / pane.image.x;
            float enlarged = prF32Max(2 * scale, 1);
            float by = enlarged / scale;
            PrVec2 center = centerOf(lens.square);
            UiImage image{
                .source = pane.picture->source,
                .rendering = PrLayoutImageRendering(
                    enlarged >= kCrispZoom ? PrLayoutImageRendering_Pixelated : PrLayoutImageRendering_Auto),
            };
            ui.image(square, image).absolute() .left(center.x + (at.x - lens.source.x) * by - clip.origin.x)
                .top(center.y + (at.y - lens.source.y) * by - clip.origin.y)
                .w(pane.rect.size.x * by).h(pane.rect.size.y * by).pointerEventsNone();
        }
        // A dark frame with a light line inside it shows on every picture.
        ui.div(area, PrUiKey(i) + 0x70656b30).absolute().left(clip.origin.x - 1).top(clip.origin.y - 1)
            .w(clip.size.x + 2).h(clip.size.y + 2).border(3).borderColor(kCheckLine).pointerEventsNone();
        ui.div(area, PrUiKey(i) + 0x70656b40).absolute().left(clip.origin.x).top(clip.origin.y).w(clip.size.x)
            .h(clip.size.y).border(1).borderColor(kCheckFill).pointerEventsNone();
    }
}

} // namespace pandite
