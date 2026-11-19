// The model viewer's overlay: the 2D that lib/draw draws over the scene. It says where the viewer is in its list, how
// far the model has loaded, and what it plays, and it can list the models around the current one.

#pragma once

#include "pr/array.h"
#include "pr/base.h"
#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/string_view.h"
#include "pr/text.h"
#include "pr/vec.h"

#include "examples/model_viewer/viewer_list.h"
#include "examples/model_viewer/viewer_stage.h"

PR_ASSUME_NONNULL_BEGIN

namespace model_viewer {

using namespace pr;

// What the overlay draws with. The list holds the overlay's drawing after overlayRecord.
struct Overlay {
    PrDrawList* list = nullptr;
    const PrFont* PR_NULLABLE font = nullptr;   // null if the font is missing, and the overlay then has no text
    PrTextShaper* shaper = nullptr;
    PrTextGlyphBuffer* glyphs = nullptr;
};

// Creates what the overlay draws with, and frees it.
void overlayCreate(Overlay& overlay);
void overlayDestroy(Overlay& overlay);

// What the overlay shows in one frame.
struct OverlayContent {
    const Array<ModelEntry>* models = nullptr;
    uint32_t current = 0;                       // the entry that the user chose, which may still be on its way
    const Stage* PR_NULLABLE shown = nullptr;   // the stage that the frame draws
    const Stage* PR_NULLABLE next = nullptr;    // the stage of the current entry while another is shown
    double waited = 0;                          // the seconds since the user chose the current entry
    bool paused = false;
    bool listsModels = false;                   // the list of the models around the current one is shown
    PrStringView notice{};                      // a sentence for the user, or empty
    PrVec2 extent{};                            // the window's extent, in logical pixels
    float pixelRatio = 1;
};

// Records the overlay for a frame into the overlay's list, in target pixels.
void overlayRecord(Overlay& overlay, const OverlayContent& content);

} // namespace model_viewer

PR_ASSUME_NONNULL_END
