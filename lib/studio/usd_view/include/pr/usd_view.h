// Studio's USD structure view: what a composed stage holds and where it came from.
//
// The view shows the selected prim: its path, type, specifier and flags, its variant sets and their selections, its
// resolved properties, and its opinions strongest first, each with its layer and its arc, which is the prim's
// provenance. A selected property shows its own opinions. The pseudo-root shows the stage: its metadata, its layers and
// its composition errors. The host chooses the prim, as from an outline of the stage's prims (prUsdViewPrimLabel), and
// the view writes the property that the user chooses. One thread uses a view.

#pragma once

#include "pr/kit.h"
#include "pr/text_buffer.h"
#include "pr/usd_stage.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The cursors of the view's tables.
typedef struct PrUsdView PrUsdView;

// Creates a view that draws with `kit`, which outlives it, or frees one.
PrUsdView* prUsdViewCreate(PrKit* kit);
void prUsdViewDestroy(PrUsdView* PR_NULLABLE view);

// What the view shows: a prim of a stage, and the property of the prim that is chosen, which the view writes when the
// user chooses another. `*property` is an index of the stage's array of properties, or kPrUsdNone for none.
typedef struct PrUsdViewContent {
    const PrUsdStage* stage;
    PrUsdPrimIndex prim;
    uint32_t* property;
} PrUsdViewContent;

// Declares the view in a round under `parent`, which it fills, scrolling what does not fit.
void prUsdViewFrame(PrUsdView* view, PrLayoutNode parent, PrUsdViewContent content);

// Appends what an outline shows for a prim: its name, or `/` for the pseudo-root, and its type if it has one.
void prUsdViewPrimLabel(const PrUsdStage* stage, PrUsdPrimIndex prim, PrTextBuffer* out);

// Appends a value as the view shows it: a scalar, a vector or a string as text, and an array as its count and its first
// elements.
void prUsdViewValueText(const PrUsdValue* value, PrTextBuffer* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
