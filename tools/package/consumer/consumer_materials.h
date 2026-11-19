// The parameter record of the consumer's material type, which C++ and Slang both compile.

#pragma once

#include "pr/render_records.h"

#if !defined(__SLANG__)
PR_ASSUME_NONNULL_BEGIN
#endif

// Stripes of two colors along the distance that the mesh's vertices hold.
struct ConsumerStripesParameters {
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 first;         // the color of every other stripe, linear
    PR_RENDER_PARAMETER(0, 0, 0, 0) PrRenderVec4 second;
    PR_RENDER_PARAMETER(0, 0, 0, 0)
    float count;                // the number of pairs of stripes to one unit of the distance
    float _pad[3];
};

#if !defined(__SLANG__)
PR_ASSUME_NONNULL_END
#endif
