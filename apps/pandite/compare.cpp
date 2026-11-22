// The comparison of selected images.

#include "apps/pandite/compare.h"

#include "pr/float.h"
#include "pr/format.h"

#include "apps/pandite/theme.h"
#include "apps/pandite/view.h"

namespace pandite {

namespace {

// Sizes in px: the gap between tiles, the distance from the slider's line within which a press grabs the slider, and
// the top of the slider's names, which is below the toolbar.
constexpr float kTileGap = 4;
constexpr float kSplitReach = 10;
constexpr float kLabelTop = 48;

// Returns the names of the selected images, in the order of the folder.
Array<String> selectedNames(const Pandite& pandite) {
    Array<String> names;
    for (const String& name : pandite.folder.names) {
        if (pandite.selected.contains(name))
            names.push(name);
    }
    return names;
}

// Returns the name of the image on screen, or the null string.
String shownName(const Pandite& pandite) {
    return pandite.index != kNoImage ? pandite.folder.names[pandite.index] : String();
}

// Returns the picture of the compared image `name` that the loader holds.
const Picture& pictureOf(Pandite& pandite, const String& name) {
    uint32_t at = folderFind(pandite.folder, name);
    return loaderWant(pandite.loader, folderPath(pandite.folder, at));
}

// Returns the extent of a picture in device pixels, or zero before it opens.
PrVec2 extentOf(const Picture& picture) {
    return PrVec2{float(picture.extent.x), float(picture.extent.y)};
}

// Writes the tiles of the comparison in an image area of `stage` px to `out`, and returns their number.
uint32_t tilesOf(const Pandite& pandite, PrVec2 stage, ViewRect* out) {
    uint32_t count = pandite.compare.names.count();
    count = count < kMaxTiles ? count : kMaxTiles;
    viewTiles(stage, count, kTileGap, out);
    return count;
}

// Returns the x of the slider's line in an image area of `width` px.
float splitOf(const Pandite& pandite, float width) {
    return prF32Round(width * prF32Clamp(pandite.compare.split, 0, 1));
}

// Declares `picture` in `parent` at `rect`, in px of the parent. The picture is crisp if `crisp` is set, which a caller
// does once the view enlarges the picture past kCrispZoom.
void picture(UiBuilder& ui, PrLayoutNode parent, const Picture& picture, ViewRect rect, bool crisp) {
    if (picture.source == nullptr)
        return;
    UiImage image{
        .source = picture.source,
        .rendering = PrLayoutImageRendering(crisp ? PrLayoutImageRendering_Pixelated : PrLayoutImageRendering_Auto),
    };
    ui.image(parent, image).absolute().left(rect.origin.x).top(rect.origin.y).w(rect.size.x).h(rect.size.y)
        .pointerEventsNone();
}

// Converts `rect` from device pixels to px.
ViewRect pxOf(ViewRect rect, float ratio) {
    return ViewRect{
        .origin = PrVec2{rect.origin.x / ratio, rect.origin.y / ratio},
        .size = PrVec2{rect.size.x / ratio, rect.size.y / ratio},
    };
}

// A name in a small box in `area`, at `top`. The row of the box is inset by `left` and `right`. The box is at the left
// of the row if only `left` is set, at the right if only `right` is set, and at the center if both are set.
void label(UiBuilder& ui, PrLayoutNode area, PrUiKey key, PrStringView text, float top, float left, float right) {
    UiElement row = ui.div(area, key).absolute().top(top).left(left).right(right).flexRow().pointerEventsNone();
    if (left > 0 && right > 0)
        row.justifyCenter();
    else if (right > 0)
        row.justifyEnd();
    ui.div(row).minW(0).px(8).py(3).bg(kPanel).border(1).borderColor(kBorder).textXs().whitespaceNowrap()
        .overflowHidden().text(text);
}

} // namespace

uint32_t comparePanes(Pandite& pandite, const Picture* PR_NULLABLE shown, const ViewFrame& frame, ComparePane* out) {
    const Compare& compare = pandite.compare;
    PrVec2 stage = frame.stage;
    float ratio = frame.ratio;
    PrVec2 viewport{stage.x * ratio, stage.y * ratio};
    ViewRect whole{.size = stage};
    // The extent of the image on screen. The frame holds it, except in a tile comparison.
    PrVec2 image{};
    if (shown != nullptr)
        image = extentOf(*shown);
    if (!compare.on) {
        out[0] = ComparePane{
            .picture = shown,
            .image = image,
            .rect = pxOf(viewImageRect(pandite.view, image, viewport), ratio),
            .clip = whole,
        };
        return 1;
    }
    if (!compare.tiles) {
        // The selected image is left of the line and the image on screen is right of it. Each is where the view puts
        // it. If the selected image has the shape of the image on screen, it is placed at the size of the image on
        // screen and covers that image exactly. A smaller copy of the image on screen is such an image.
        const Picture& other = pictureOf(pandite, compare.names[0]);
        PrVec2 otherImage = extentOf(other);
        PrVec2 placed = otherImage;
        if (image.x > 0 && otherImage.x > 0) {
            float a = image.x / image.y;
            float b = otherImage.x / otherImage.y;
            if (prF32Abs(a - b) <= 0.01f * prF32Max(a, b))
                placed = image;
        }
        float x = splitOf(pandite, stage.x);
        out[0] = ComparePane{
            .picture = &other,
            .image = otherImage,
            .rect = pxOf(viewImageRect(pandite.view, placed, viewport), ratio),
            .clip = ViewRect{.size = PrVec2{x, stage.y}},
        };
        out[1] = ComparePane{
            .picture = shown,
            .image = image,
            .rect = pxOf(viewImageRect(pandite.view, image, viewport), ratio),
            .clip = ViewRect{.origin = PrVec2{x, 0}, .size = PrVec2{prF32Max(stage.x - x, 0), stage.y}},
        };
        return 2;
    }
    ViewRect tiles[kMaxTiles];
    uint32_t count = tilesOf(pandite, stage, tiles);
    for (uint32_t i = 0; i < count; ++i) {
        const ViewRect& tile = tiles[i];
        const Picture& item = pictureOf(pandite, compare.names[i]);
        PrVec2 port{tile.size.x * ratio, tile.size.y * ratio};
        ViewRect rect = pxOf(viewImageRect(pandite.view, extentOf(item), port), ratio);
        rect.origin = PrVec2{rect.origin.x + tile.origin.x, rect.origin.y + tile.origin.y};
        out[i] = ComparePane{.picture = &item, .image = extentOf(item), .rect = rect, .clip = tile};
    }
    return count;
}

bool compareEnabled(const Pandite& pandite) {
    if (pandite.compare.on)
        return true;
    uint32_t count = pandite.selected.count();
    if (count == 1)
        return pandite.index != kNoImage && !pandite.selected.contains(shownName(pandite));
    return count >= 2 && count <= kMaxTiles;
}

void compareToggle(Pandite& pandite) {
    if (pandite.compare.on) {
        compareEnd(pandite);
        return;
    }
    if (!compareEnabled(pandite))
        return;
    pandite.compare = Compare{.on = true, .tiles = pandite.selected.count() > 1, .names = selectedNames(pandite)};
    // Tiles have their own fit.
    if (pandite.compare.tiles)
        viewFit(pandite.view);
}

void compareEnd(Pandite& pandite) {
    bool tiles = pandite.compare.tiles;
    pandite.compare = Compare{};
    if (tiles)
        viewFit(pandite.view);
}

void compareFollow(Pandite& pandite) {
    Compare& compare = pandite.compare;
    if (!compare.on)
        return;
    Array<String> names = selectedNames(pandite);
    // A slider comparison sets one image against the image on screen. A tile comparison shows two to eight images.
    bool fits = names.count() >= 2 && names.count() <= kMaxTiles;
    if (!compare.tiles)
        fits = names.count() == 1 && pandite.index != kNoImage && names[0] != shownName(pandite);
    if (!fits) {
        compareEnd(pandite);
        return;
    }
    compare.names = move(names);
    for (const String& name : compare.names)
        pictureOf(pandite, name);
}

void compareFrame(Pandite& pandite, ViewFrame& frame, PrVec2 pointer, bool over) {
    if (!pandite.compare.on || !pandite.compare.tiles)
        return;
    ViewRect tiles[kMaxTiles];
    uint32_t count = tilesOf(pandite, frame.stage, tiles);
    uint32_t chosen = count;
    for (uint32_t i = 0; i < count && over; ++i) {
        const ViewRect& t = tiles[i];
        bool within = pointer.x >= t.origin.x && pointer.x < t.origin.x + t.size.x && pointer.y >= t.origin.y &&
            pointer.y < t.origin.y + t.size.y;
        if (within && pictureOf(pandite, pandite.compare.names[i]).extent.x != 0)
            chosen = i;
    }
    for (uint32_t i = 0; i < count && chosen == count; ++i) {
        if (pictureOf(pandite, pandite.compare.names[i]).extent.x != 0)
            chosen = i;
    }
    if (chosen == count) {
        frame.image = PrVec2{};
        return;
    }
    const ViewRect& tile = tiles[chosen];
    frame.image = extentOf(pictureOf(pandite, pandite.compare.names[chosen]));
    frame.viewport = PrVec2{tile.size.x * frame.ratio, tile.size.y * frame.ratio};
    frame.origin = tile.origin;
}

bool compareSplit(Pandite& pandite, UiElement& area, const ViewFrame& frame) {
    Compare& compare = pandite.compare;
    if (!compare.on || compare.tiles)
        return false;
    float x = splitOf(pandite, frame.stage.x);
    PrVec2 over{};
    if (area.pointer(&over) && prF32Abs(over.x - x) <= kSplitReach)
        area.cursor(PrAppCursor_ResizeX);
    PrUiDrag drag{};
    if (!area.dragged(&drag, 0, 0)) {
        compare.splitting = false;
        return false;
    }
    if (!compare.splitting && !drag.moved && drag.delta.x == 0 && drag.delta.y == 0)
        compare.splitting = prF32Abs(drag.origin.x - x) <= kSplitReach;
    if (compare.splitting) {
        area.cursor(PrAppCursor_ResizeX);
        if (frame.stage.x > 0)
            compare.split = prF32Clamp(drag.position.x / frame.stage.x, 0, 1);
    }
    bool splitting = compare.splitting;
    if (drag.released || drag.cancelled)
        compare.splitting = false;
    return splitting;
}

void compareView(Pandite& pandite, UiBuilder& ui, UiElement& area, const Picture* PR_NULLABLE shown, const ViewFrame& frame) {
    const Compare& compare = pandite.compare;
    PrVec2 stage = frame.stage;
    ComparePane panes[kMaxTiles];
    uint32_t count = comparePanes(pandite, shown, frame, panes);
    // Each picture in its pane, clipped to it.
    for (uint32_t i = 0; i < count; ++i) {
        const ComparePane& pane = panes[i];
        const ViewRect& clip = pane.clip;
        UiElement cell = ui.div(area, PrUiKey(i) + 10).absolute().left(clip.origin.x).top(clip.origin.y)
            .w(clip.size.x).h(clip.size.y).overflowHidden().pointerEventsNone();
        if (pane.picture != nullptr && pane.image.x > 0) {
            ViewRect rect = pane.rect;
            rect.origin = PrVec2{rect.origin.x - clip.origin.x, rect.origin.y - clip.origin.y};
            bool crisp = rect.size.x * frame.ratio / pane.image.x >= kCrispZoom;
            picture(ui, cell, *pane.picture, rect, crisp);
        }
        if (!compare.tiles)
            continue;
        // Each tile has an outline and a name. The outline of the image on screen is in the accent color.
        const String& name = compare.names[i];
        bool hot = name == shownName(pandite);
        ui.div(cell).absolute().inset(0).border(hot ? 2 : 1).borderColor(hot ? kAccent : kBorder);
        String text(name);
        if (pane.picture->failed)
            text.append(" - cannot open");
        else if (pane.picture->source == nullptr)
            text.append(" - loading");
        label(ui, cell, 1, text, clip.size.y - 32, 8, 8);
    }
    if (compare.tiles)
        return;
    const Picture& other = *panes[0].picture;
    String otherName(compare.names[0]);
    if (other.failed)
        otherName.append(" - cannot open");
    else if (other.source == nullptr)
        otherName.append(" - loading");
    label(ui, area, 1, otherName, kLabelTop, 8, 0);
    if (pandite.index != kNoImage)
        label(ui, area, 2, pandite.folder.names[pandite.index], kLabelTop, 0, 8);
    // A dark line with a light core shows on every picture. The grip of the slider is in the middle of the line.
    float x = splitOf(pandite, stage.x);
    ui.div(area, 3).absolute().left(x - 2).top(0).w(4).h(stage.y).bg(kCheckLine).pointerEventsNone();
    ui.div(area, 4).absolute().left(x - 1).top(0).w(2).h(stage.y).bg(kCheckFill).pointerEventsNone();
    ui.div(area, 5).absolute().left(prF32Round(x - 7)).top(prF32Round((stage.y - 36) / 2)).w(14).h(36).roundedFull()
        .bg(kCheckFill).border(1.5f).borderColor(kCheckLine).pointerEventsNone();
}

PrStringView compareHint(const Pandite& pandite) {
    if (pandite.compare.on)
        return "Esc to end comparison";
    if (pandite.selected.count() > kMaxTiles)
        return "Compare up to 8 images";
    return "D to compare";
}

} // namespace pandite
