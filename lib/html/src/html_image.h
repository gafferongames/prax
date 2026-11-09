// Images
//
// mimesniff - https://mimesniff.spec.whatwg.org/#matching-an-image-type-pattern

#pragma once

#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// What an image is, which decides what draws it: the host's decoder draws a raster image, and the engine draws an SVG
// image and a Mermaid diagram itself.
enum {
    HtmlImageKind_None,                 // not an image that the engine accepts
    HtmlImageKind_Raster,
    HtmlImageKind_Svg,
    HtmlImageKind_Diagram,              // a Mermaid diagram's source
};
typedef uint8_t HtmlImageKind;

// Returns the kind of the image `bytes`, which a response of type `type` holds, and writes the natural size that a
// raster or SVG image's header records. A diagram is known by its type, text/vnd.mermaid, and has the size of its
// layout, so this writes zero for it. Any other image is known by its bytes, whatever its type. Writes zero for a
// dimension that the image leaves to its containing document, which an SVG image can do.
HtmlImageKind htmlImageKind(PrStringView type, PrStringView bytes, uint32_t* width, uint32_t* height);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
