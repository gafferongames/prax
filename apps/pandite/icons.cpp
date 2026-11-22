// Pandite's icons.

#include "apps/pandite/icons.h"

#include "pr/count_of.h"
#include "pr/span.h"

namespace pandite {

namespace {

// The stroke widths, for a grid of 24 shown at 20 px. They are thin enough to stay crisp at small sizes.
constexpr float kLine = 1.6f;
constexpr float kBold = 2.0f;

constexpr UiPathPart kOpen[] = {{"M3 18.5V5.5H9L11 7.5H18.5V10.5", kLine}, {"M3 18.5L6 11H22L19 18.5Z", kLine}};
constexpr UiPathPart kSaveAs[] = {{"M4 4H16.5L20 7.5V20H4Z", kLine}, {"M8 4V8.5H15V4", kLine}, {"M7.5 20V14H16.5V20", kLine}};

// The back sheet stops short of the front sheet, as if the front one covered it.
constexpr UiPathPart kCopyPath[] = {
    {"M8 16H5A2 2 0 0 1 3 14V5A2 2 0 0 1 5 3H12A2 2 0 0 1 14 5V7", kLine},
    {"M11 8H18.5A2 2 0 0 1 20.5 10V19A2 2 0 0 1 18.5 21H11A2 2 0 0 1 9 19V10A2 2 0 0 1 11 8Z", kLine},
    {"M12.5 12.5H17", 1.3f},
    {"M12.5 16.5H15.5", 1.3f},
};
constexpr UiPathPart kUndo[] = {{"M13 8A5.5 5.5 0 0 1 13 19H8", kLine}, {"M13 8H6", kLine}, {"M9.5 4.5L6 8L9.5 11.5", kLine}};
constexpr UiPathPart kRotateRight[] = {{"M19 13A7 7 0 1 1 11.03 6.07", kLine}, {"M10.5 2L16.5 6L10.5 10Z", 0}};
constexpr UiPathPart kRotateLeft[] = {{"M5 13A7 7 0 1 0 12.97 6.07", kLine}, {"M13.5 2L7.5 6L13.5 10Z", 0}};
constexpr UiPathPart kFlipHorizontal[] = {
    {"M12 2.5V5M12 8V10.5M12 13.5V16M12 19V21.5", 1.2f},
    {"M9.5 5V19L2 12Z", 0},
    {"M14.6 5.5V18.5L21.6 12Z", 1.45f},
};
constexpr UiPathPart kFlipVertical[] = {
    {"M2.5 12H5M8 12H10.5M13.5 12H16M19 12H21.5", 1.2f},
    {"M5 9.5H19L12 2Z", 0},
    {"M5.5 14.6H18.5L12 21.6Z", 1.45f},
};
constexpr UiPathPart kCrop[] = {{"M7 2.5V17H21.5", kLine}, {"M2.5 7H17V21.5", kLine}};
constexpr UiPathPart kResize[] = {
    {
        "M4.5 12H10.5A1.5 1.5 0 0 1 12 13.5V19.5A1.5 1.5 0 0 1 10.5 21H4.5A1.5 1.5 0 0 1 3 19.5V13.5A1.5 1.5 0 0 1 4.5 12Z",
        kLine,
    },
    {"M11.5 12.5L20.5 3.5", kLine},
    {"M14.5 3.5H20.5V9.5", kLine},
};
constexpr UiPathPart kFit[] = {
    {"M4.5 5H19.5A2 2 0 0 1 21.5 7V17A2 2 0 0 1 19.5 19H4.5A2 2 0 0 1 2.5 17V7A2 2 0 0 1 4.5 5Z", kLine},
    {"M7 12H17", kLine},
    {"M9.5 9.5L7 12L9.5 14.5", kLine},
    {"M14.5 9.5L17 12L14.5 14.5", kLine},
};
constexpr UiPathPart kActualSize[] = {
    {"M4 8.5L7 6V18.5", 1.8f},
    {"M10.5 9.5A1.5 1.5 0 1 0 13.5 9.5A1.5 1.5 0 1 0 10.5 9.5Z", 0},
    {"M10.5 15.5A1.5 1.5 0 1 0 13.5 15.5A1.5 1.5 0 1 0 10.5 15.5Z", 0},
    {"M16 8.5L19 6V18.5", 1.8f},
};
constexpr UiPathPart kZoomIn[] = {
    {"M2.5 10A7.5 7.5 0 1 0 17.5 10A7.5 7.5 0 1 0 2.5 10Z", kLine},
    {"M16 16L21.5 21.5", 2.2f},
    {"M6.5 10H13.5", kBold},
    {"M10 6.5V13.5", kBold},
};
constexpr UiPathPart kZoomOut[] = {
    {"M2.5 10A7.5 7.5 0 1 0 17.5 10A7.5 7.5 0 1 0 2.5 10Z", kLine},
    {"M16 16L21.5 21.5", 2.2f},
    {"M6.5 10H13.5", kBold},
};
constexpr UiPathPart kPrevious[] = {{"M15 5L8 12L15 19", kBold}};
constexpr UiPathPart kNext[] = {{"M9 5L16 12L9 19", kBold}};
constexpr UiPathPart kGallery[] = {
    {"M5 3H19A2 2 0 0 1 21 5V12A2 2 0 0 1 19 14H5A2 2 0 0 1 3 12V5A2 2 0 0 1 5 3Z", kLine},
    {"M4 16.5H6.8A1 1 0 0 1 7.8 17.5V20A1 1 0 0 1 6.8 21H4A1 1 0 0 1 3 20V17.5A1 1 0 0 1 4 16.5Z", 0},
    {"M10.6 16.5H13.4A1 1 0 0 1 14.4 17.5V20A1 1 0 0 1 13.4 21H10.6A1 1 0 0 1 9.6 20V17.5A1 1 0 0 1 10.6 16.5Z", 0},
    {"M17.2 16.5H20A1 1 0 0 1 21 17.5V20A1 1 0 0 1 20 21H17.2A1 1 0 0 1 16.2 20V17.5A1 1 0 0 1 17.2 16.5Z", 0},
};
constexpr UiPathPart kFullscreen[] = {{"M3 9V3H9M15 3H21V9M21 15V21H15M9 21H3V15", kLine}};
constexpr UiPathPart kExitFullscreen[] = {{"M3 9H9V3M15 3V9H21M21 15H15V21M9 21V15H3", kLine}};
constexpr UiPathPart kMore[] = {
    {"M3.1 12A1.9 1.9 0 1 0 6.9 12A1.9 1.9 0 1 0 3.1 12Z", 0},
    {"M10.1 12A1.9 1.9 0 1 0 13.9 12A1.9 1.9 0 1 0 10.1 12Z", 0},
    {"M17.1 12A1.9 1.9 0 1 0 20.9 12A1.9 1.9 0 1 0 17.1 12Z", 0},
};
constexpr UiPathPart kCheck[] = {{"M4.5 12.5L9.5 17.5L19.5 6.5", kBold}};
constexpr UiPathPart kClose[] = {{"M6 6L18 18M18 6L6 18", kBold}};
constexpr UiPathPart kPicture[] = {
    {"M5 4H19A2 2 0 0 1 21 6V18A2 2 0 0 1 19 20H5A2 2 0 0 1 3 18V6A2 2 0 0 1 5 4Z", kLine},
    {"M5.5 18L10 11.5L13.5 16L15.5 13.5L18.5 18Z", 0},
    {"M14.2 8.5A1.8 1.8 0 1 0 17.8 8.5A1.8 1.8 0 1 0 14.2 8.5Z", 0},
};
constexpr UiPathPart kWarning[] = {
    {"M12 3.5L21.5 20H2.5Z", kLine},
    {"M12 9.5V14", kBold},
    {"M10.7 17A1.3 1.3 0 1 0 13.3 17A1.3 1.3 0 1 0 10.7 17Z", 0},
};

// The parts of each icon, indexed by Icon.
constexpr Span<const UiPathPart> kIcons[] = {
    Span<const UiPathPart>(kOpen),
    Span<const UiPathPart>(kSaveAs),
    Span<const UiPathPart>(kCopyPath),
    Span<const UiPathPart>(kUndo),
    Span<const UiPathPart>(kRotateLeft),
    Span<const UiPathPart>(kRotateRight),
    Span<const UiPathPart>(kFlipHorizontal),
    Span<const UiPathPart>(kFlipVertical),
    Span<const UiPathPart>(kCrop),
    Span<const UiPathPart>(kResize),
    Span<const UiPathPart>(kFit),
    Span<const UiPathPart>(kActualSize),
    Span<const UiPathPart>(kZoomIn),
    Span<const UiPathPart>(kZoomOut),
    Span<const UiPathPart>(kPrevious),
    Span<const UiPathPart>(kNext),
    Span<const UiPathPart>(kGallery),
    Span<const UiPathPart>(kFullscreen),
    Span<const UiPathPart>(kExitFullscreen),
    Span<const UiPathPart>(kMore),
    Span<const UiPathPart>(kCheck),
    Span<const UiPathPart>(kClose),
    Span<const UiPathPart>(kPicture),
    Span<const UiPathPart>(kWarning),
};
static_assert(countOf(kIcons) == Icon_Count, "kIcons has one entry for each Icon");

} // namespace

UiElement icon(UiBuilder& ui, PrLayoutNode parent, Icon which, float size, PrColor color) {
    UiPath path{.parts = kIcons[which], .viewBox = PrVec2{24, 24}, .color = color};
    return ui.path(parent, path).size(size).shrink0().pointerEventsNone();
}

} // namespace pandite
