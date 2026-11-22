// Pandite's dark theme: the canvas behind the image, translucent panels over it, the solid panel of the gallery and of
// dialogs, and the colors of text, fields, selection and crop mode.

#pragma once

#include "pr/color.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

// The canvas, the panels over it and their borders, and text.
constexpr PrColor kCanvas = prColorFromHex(0x121213);
constexpr PrColor kPanel = prColorFromHexAlpha(0x1e1e20e8);
constexpr PrColor kPanelSolid = prColorFromHex(0x1e1e20);
constexpr PrColor kBorder = prColorFromHexAlpha(0xffffff1a);
constexpr PrColor kSeparator = prColorFromHexAlpha(0xffffff1e);
constexpr PrColor kText = prColorFromHex(0xececee);
constexpr PrColor kTextDim = prColorFromHex(0xa0a0a6);
constexpr PrColor kTextDisabled = prColorFromHex(0x5c5c62);

// Icons, a control that is hovered and one that is pressed, the accent, the placeholder of an image, a scroll track and
// the backdrop of a dialog.
constexpr PrColor kIcon = prColorFromHex(0xe2e2e6);
constexpr PrColor kHover = prColorFromHexAlpha(0xffffff1a);
constexpr PrColor kPressed = prColorFromHexAlpha(0xffffff2e);
constexpr PrColor kAccent = prColorFromHex(0x4894ff);
constexpr PrColor kPlaceholder = prColorFromHex(0x2c2c30);
constexpr PrColor kTrack = prColorFromHexAlpha(0xffffff24);
constexpr PrColor kBackdrop = prColorFromHexAlpha(0x0000008c);

// Text fields and their selection, text on the accent, an error, and the badge of a selected thumbnail.
constexpr PrColor kField = prColorFromHex(0x141416);
constexpr PrColor kFieldBorder = prColorFromHexAlpha(0xffffff30);
constexpr PrColor kSelection = prColorFromHexAlpha(0x4894ff6e);
constexpr PrColor kAccentText = prColorFromHex(0x081020);
constexpr PrColor kError = prColorFromHex(0xff7870);
constexpr PrColor kCheckFill = prColorFromHexAlpha(0xffffffeb);
constexpr PrColor kCheckLine = prColorFromHexAlpha(0x00000040);
constexpr PrColor kCheckMark = prColorFromHex(0x0a60d6);

// The marker of the position in the gallery.
constexpr PrColor kMarker = prColorFromHex(0xe2e2e6);

// Crop mode: the dimmed surround of the selection, the lines of the selection, the shadow of the lines, and the lines
// at the thirds of the selection.
constexpr PrColor kCropDim = prColorFromHexAlpha(0x00000096);
constexpr PrColor kCropLine = prColorFromHex(0xffffff);
constexpr PrColor kCropShadow = prColorFromHexAlpha(0x00000096);
constexpr PrColor kCropThirds = prColorFromHexAlpha(0xffffff59);

// A tooltip, and the pill that shows the size of the crop selection.
constexpr PrColor kTooltip = prColorFromHex(0x343438);
constexpr PrColor kTooltipText = prColorFromHex(0xf4f4f6);

} // namespace pandite

PR_ASSUME_NONNULL_END
