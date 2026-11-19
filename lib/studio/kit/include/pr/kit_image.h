// Studio Kit's images: pictures fitted into the boxes that they are placed in, from decoded images or from textures on
// the GPU device that renders the window, such as a 3D view's color.

#pragma once

#include "pr/draw.h"
#include "pr/kit.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// How an image fills its box. The fit never changes the box's size.
enum {
    PrKitImageFit_Fill,             // stretched over the box; the image's ratio is not kept
    PrKitImageFit_Contain,          // the whole image, as large as fits, with the ratio kept
    PrKitImageFit_Cover,            // the whole box, cropping the image, with the ratio kept
    PrKitImageFit_None,             // the image's own size, cropped by the box
    PrKitImageFit_ScaleDown,        // as Contain, but never larger than the image's own size
};
typedef uint8_t PrKitImageFit;

// The content of an image widget: the image that it shows, upright as its orientation displays it (draw_image.h).
typedef struct PrKitImage {
    PrUiKey key;
    const PrTextureSource* image;
} PrKitImage;

// The style of an image widget: the image fitted into its box as `fit` says, at `alignX` and `alignY` in the room that
// the fit leaves or crops (Start, Center or End; Auto centers), in a box that `place` places in its parent. A box with
// no size of its own takes the image's size, a px for each pixel.
typedef struct PrKitImageStyle {
    PrKitImageFit fit;
    PrKitAlign alignX;
    PrKitAlign alignY;
    PrKitPlace place;
} PrKitImageStyle;

// Creates an image widget under `parent`.
PrLayoutNode prKitImage(PrKit* kit, PrLayoutNode parent, PrKitImage image, PrKitImageStyle style);

// The content of a texture widget: a texture of `extent` texels on the device that renders the window's list. The
// widget draws the texture's texels as they are when the list renders, so a texture that the application renders into
// each frame shows each frame's texels. `revision` is a number that the application changes when the texels change and
// nothing else does, so that the window draws a frame again.
typedef struct PrKitTexture {
    PrUiKey key;
    PrTexture texture;
    PrUVec2 extent;
    uint64_t revision;
} PrKitTexture;

// Creates a texture widget under `parent`, styled as an image widget is: the texture is fitted into its box, clipped to
// it, and layered, scrolled and clipped with the rest of the tree, as any image is.
PrLayoutNode prKitTexture(PrKit* kit, PrLayoutNode parent, PrKitTexture texture, PrKitImageStyle style);

// The content of an embed: the slot that the scene's player fills (PrLayoutContentKind_Embed), a nonzero id that the
// host gives, as a Studio window gives a GPU view's (prStudioWindowGpuViewEmbed).
typedef struct PrKitEmbed {
    PrUiKey key;
    uint64_t embed;
} PrKitEmbed;

// Creates an embed under `parent`, in a box that `place` places, which has no size of its own. The player fills the box
// with what it draws there at the box's laid-out size, which is clipped, layered and scrolled with the rest of the
// tree.
PrLayoutNode prKitEmbed(PrKit* kit, PrLayoutNode parent, PrKitEmbed embed, PrKitPlace place);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
