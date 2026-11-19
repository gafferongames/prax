// Studio Kit's text: labels in the theme's roles and tones.

#pragma once

#include "pr/kit.h"
#include "pr/raw_string.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Where lines sit in their box.
enum {
    PrKitTextAlign_Start,
    PrKitTextAlign_Center,
    PrKitTextAlign_End,
};
typedef uint8_t PrKitTextAlign;

// How text is set. Zero is body text in its role's tone and wrap (PrKitType), aligned at the start.
PR_NO_PADDING_BEGIN
typedef struct PrKitTextStyle {
    PrKitTextRole role;
    PrKitTone tone;
    PrKitWrap wrap;
    PrKitTextAlign align;
    bool preserve;                  // keeps line feeds and runs of spaces
} PrKitTextStyle;
PR_NO_PADDING_END

// A label: one string in one style.
typedef struct PrKitLabel {
    PrUiKey key;
    PrStringView text;
} PrKitLabel;

// Creates a label under `parent`. A label that clips its text (PrKitWrap_Clip) shows the whole text in a tooltip
// (prKitTooltip) while the pointer rests on it and the text does not fit.
PrLayoutNode prKitLabel(PrKit* kit, PrLayoutNode parent, PrKitLabel label, PrKitTextStyle style);

// Returns true if the pointer is over `node`, created in the current round, and the node's text ran past the end of
// `clip`, the node or an ancestor that clips it, at the last layout. A widget that clips text in a box of its own, as a
// table's cell does, shows the whole text in a tooltip while this holds.
bool prKitIsClipped(PrKit* kit, PrLayoutNode node, PrLayoutNode clip);

// A run of text in a role and tone of its own. A run takes its paragraph's wrap, alignment and white space.
typedef struct PrKitTextRun {
    PrStringView text;
    PrKitTextRole role;
    PrKitTone tone;
} PrKitTextRun;

// A paragraph of `count` runs, on lines that its style wraps and aligns. A run may break across lines.
typedef struct PrKitText {
    PrUiKey key;
    const PrKitTextRun* PR_NULLABLE runs;
    uint32_t count;
} PrKitText;

// Creates a paragraph under `parent`.
PrLayoutNode prKitText(PrKit* kit, PrLayoutNode parent, PrKitText text, PrKitTextStyle style);

// Returns the color of a tone in the kit's theme.
PrColor prKitToneColor(const PrKit* kit, PrKitTone tone);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
