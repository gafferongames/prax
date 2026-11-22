// Pandite's icons. They are original line drawings on a grid of 24 by 24. Each icon is a few strokes and fills of path
// data. The path elements of lib/ui draw the icons, in the color of the text around them or in a color of their own.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/ui_element.h"

PR_ASSUME_NONNULL_BEGIN

namespace pandite {

using namespace pr;

// The icons of the toolbar, the menus and the dialogs, each named for the command or the state that it stands for.
enum : uint8_t {
    Icon_Open,
    Icon_SaveAs,
    Icon_CopyPath,
    Icon_Undo,
    Icon_RotateLeft,
    Icon_RotateRight,
    Icon_FlipHorizontal,
    Icon_FlipVertical,
    Icon_Crop,
    Icon_Resize,
    Icon_Fit,
    Icon_ActualSize,
    Icon_ZoomIn,
    Icon_ZoomOut,
    Icon_Previous,
    Icon_Next,
    Icon_Gallery,
    Icon_Fullscreen,
    Icon_ExitFullscreen,
    Icon_More,
    Icon_Check,
    Icon_Close,
    Icon_Picture,
    Icon_Warning,
    Icon_Count,
};
typedef uint8_t Icon;

// Creates the icon `which` under `parent`. The icon is `size` px square and is drawn in `color`.
UiElement icon(UiBuilder& ui, PrLayoutNode parent, Icon which, float size, PrColor color);

} // namespace pandite

PR_ASSUME_NONNULL_END
