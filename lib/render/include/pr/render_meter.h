// The meters of a renderer (pr/render.h): what views show of the scene's luminance, read back for an exposure that
// follows the image.

#pragma once

#include "pr/base.h"
#include "pr/render.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Meters
//
// A meter reads what a view shows, emission and background included, for an exposure that follows the image rather than
// the scene's lights (prRenderSceneIlluminance). A view that names a meter adds a pass that takes the mean of the
// base-2 logarithm of the luminance over each cell of a grid of kPrRenderMeterGrid squared cells across its color
// (pr/render_records.h), divided by the view's exposure, and reads the grid back. A pixel that the view stores as black
// has no light to read, and is left out. Once the frame's work is done, the meter sorts the cells by their mean, each
// weighted by the share of its pixels that have light, and averages the logarithms between a tenth and nine tenths of
// that weight: the darkest and the brightest tenths, such as a deep shadow and a highlight of the sun, do not move the
// reading. The sort is exact, where a histogram's bins would round each cell, and 4,096 cells cost microseconds.
//
// A reading is of a frame that has completed, so it arrives a few frames after the frame that it reads, and an exposure
// that follows readings follows the image with that delay. A reading is a function of the view's color and its exposure
// alone. Content whose values are the view's, an unlit color or an emission in the view's unit, has a luminance that
// the exposure divides, so it pulls the reading toward the exposure that shows it, and a view of such content alone
// reads whatever the exposure was.

// What a meter read of one view in one frame.
typedef struct PrRenderMeterReading {
    // The readings that the meter has taken, which a new reading increments, and zero before the first.
    uint64_t serial;

    // The exposure that the view had in the frame (PrRenderViewDesc::exposure).
    float exposure;

    // The share of the view that has light. Zero means that no pixel had any, and the luminance is then zero.
    float coverage;

    // The mean luminance, in nits, as the mean of its logarithm, and the EV100 that a reflected-light meter reads for
    // it (prRenderEv100FromLuminance).
    float luminance;
    float ev100;
} PrRenderMeterReading;

// A meter: a view names it to have what it shows read (PrRenderViewDesc::meter). Zero is none.
typedef struct PrRenderMeter {
    uint32_t id;
} PrRenderMeter;

// Creates a meter without a reading, or destroys one. Any thread calls them. A reading that is in flight when the meter
// is destroyed is dropped.
PrRenderMeter prRenderMeterCreate(PrRender* render);
void prRenderMeterDestroy(PrRender* render, PrRenderMeter meter);

// Returns the meter's latest reading, with a serial of zero before the first. Any thread calls it.
PrRenderMeterReading prRenderMeterRead(const PrRender* render, PrRenderMeter meter);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
