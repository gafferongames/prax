// The magnifier of pixel peek: a square at the pointer that shows the picture under the pointer enlarged. A comparison
// has one square for each of its pictures.
//
// A single image has one square, centered on the pointer. A slider comparison has two squares side by side at the
// pointer, one for each picture. Both show the spot under the pointer. A tile comparison has one square in every tile,
// at the spot that matches the place of the pointer in its own tile. A picture that shows below 50% shows at 100% in
// its square. Any other picture shows at twice its size.

#pragma once

#include "pr/base.h"
#include "pr/span.h"
#include "pr/ui_element.h"

#include "apps/pandite/compare.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The side of one square of the magnifier, in px.
constexpr float kPeekSide = 280;

// Declares the squares of the magnifier in the image area `area` over `panes` (comparePanes). `pointer` is the position
// of the pointer in px of the area. `ratio` is the number of device pixels per px. If `tiles` is set, every pane gets a
// square.
void peekView(UiBuilder& ui, PrLayoutNode area, Span<const ComparePane> panes, bool tiles, PrVec2 pointer, float ratio);

} // namespace pandite

PR_ASSUME_NONNULL_END
