// The view of an image: fitting, zooming about a point, and panning within the viewport.

#include "apps/pandite/view_state.h"

#include "tools/harness/test.h"

using namespace pandite;

namespace {

constexpr PrVec2 kViewport{800, 600};

} // namespace

TEST_CASE("a fit shrinks an image to the viewport and never enlarges it") {
    ViewState view;
    CHECK(viewScale(view, PrVec2{100, 100}, kViewport) == 1);
    ViewRect small = viewImageRect(view, PrVec2{100, 100}, kViewport);
    CHECK(small.origin.x == 350);
    CHECK(small.origin.y == 250);
    CHECK(viewScale(view, PrVec2{1600, 600}, kViewport) == 0.5f);
    CHECK(viewPercent(view, PrVec2{1600, 600}, kViewport) == 50);
}

TEST_CASE("a zoom keeps the image point under the anchor in place") {
    ViewState view;
    PrVec2 image{4000, 3000};
    PrVec2 anchor{600, 200};
    PrVec2 before = viewToImage(view, anchor, image, kViewport);
    viewZoomBy(view, 4, image, kViewport, anchor);
    CHECK(view.mode == ViewMode_Zoom);
    CHECK(viewScale(view, image, kViewport) == doctest::Approx(0.8));
    PrVec2 after = viewToImage(view, anchor, image, kViewport);
    CHECK(after.x == doctest::Approx(before.x));
    CHECK(after.y == doctest::Approx(before.y));
    // 1:1 holds its scale as the viewport changes, and a fit returns to the center.
    viewOneToOne(view, image, kViewport, anchor);
    CHECK(viewScale(view, image, PrVec2{300, 300}) == 1);
    viewFit(view);
    CHECK(view.pan.x == 0);
    CHECK(viewScale(view, image, kViewport) == doctest::Approx(0.2));
}

TEST_CASE("a pan stops when the image's edge reaches the viewport's edge, and an axis that fits stays centered") {
    ViewState view;
    PrVec2 image{1600, 400};
    viewOneToOne(view, image, kViewport, PrVec2{400, 300});
    viewPanBy(view, PrVec2{10000, 10000}, image, kViewport);
    CHECK(view.pan.x == 400);
    CHECK(view.pan.y == 0);
    ViewRect rect = viewImageRect(view, image, kViewport);
    CHECK(rect.origin.x == 0);
    CHECK(rect.origin.y == 100);
    // Zooms past the limits clamp to them.
    viewZoomTo(view, 1000, image, kViewport, PrVec2{400, 300});
    CHECK(viewScale(view, image, kViewport) == kMaxZoom);
}

TEST_CASE("tiles fill the area without overlap, side by side in a wide area and stacked in a tall one") {
    ViewRect tiles[kMaxTiles];
    for (uint32_t count = 1; count <= kMaxTiles; ++count) {
        CAPTURE(count);
        viewTiles(PrVec2{1200, 800}, count, 4, tiles);
        for (uint32_t i = 0; i < count; ++i) {
            const ViewRect& a = tiles[i];
            CHECK(a.size.x > 100);
            CHECK(a.size.y > 100);
            CHECK(a.origin.x >= 0);
            CHECK(a.origin.y >= 0);
            CHECK(a.origin.x + a.size.x <= 1200);
            CHECK(a.origin.y + a.size.y <= 800);
            for (uint32_t k = i + 1; k < count; ++k) {
                const ViewRect& b = tiles[k];
                bool apart = a.origin.x + a.size.x <= b.origin.x || b.origin.x + b.size.x <= a.origin.x ||
                    a.origin.y + a.size.y <= b.origin.y || b.origin.y + b.size.y <= a.origin.y;
                CHECK(apart);
            }
        }
    }
    viewTiles(PrVec2{1200, 800}, 2, 4, tiles);
    CHECK(tiles[0].origin.y == tiles[1].origin.y);
    viewTiles(PrVec2{500, 1200}, 2, 4, tiles);
    CHECK(tiles[0].origin.x == tiles[1].origin.x);
    // Three in a wide area: a row of two over the third, which takes the whole width.
    viewTiles(PrVec2{1000, 1000}, 3, 4, tiles);
    CHECK(tiles[2].size.x == 1000);
}
