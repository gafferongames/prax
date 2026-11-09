// CSS colors outside a style sheet: parses the sRGB forms of <color> found in other text, such as diagrams and
// configuration files.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Parses `text` as a CSS color in sRGB (css-color-4/#color-syntax). It accepts a hex color, rgb() or rgba(), hsl() or
// hsla() in their legacy and modern syntaxes, a named color, or transparent. Parsing ignores ASCII case and surrounding
// white space. Returns false for any other text, including colors that need a style sheet to resolve, such as
// currentcolor and the system colors. The channels are in [0, 1], with the precision that the text gives them.
bool prColorFromCss(PrStringView text, PrColor* out);

// Parses `text` as prColorFromCss does, rounding the channels to 8 bits.
bool prColor8FromCss(PrStringView text, PrColor8* out);

// Looks up the sRGB value of the named color `name`, ignoring ASCII case (css-color-4/#named-colors). Returns false for
// any other name.
bool prColor8FromCssName(PrStringView name, PrColor8* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
