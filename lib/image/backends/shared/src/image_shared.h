// What the image codecs share as they adapt the format libraries: the whole input of an image in memory, and the
// meaning of the color code points that several formats carry.

#pragma once

#include "pr/base.h"
#include "pr/color.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads the whole of `source` into storage that the caller frees with prMemoryFree, and writes it and its size to
// `*out` and `*size`. `readerBytes` is what the codec's reader holds besides. Returns Limit if the input and the reader
// exceed the limits, or the source's read failure.
PrStatus imageReadSource(const PrImageSource* source, const PrImageLimits& limits, uint64_t readerBytes,
    uint8_t* PR_NULLABLE* PR_NONNULL out, size_t* size);

// Returns the space that the code points of Rec. ITU-T H.273 name: colour primaries, transfer characteristics, matrix
// coefficients and the full-range flag. Only RGB at full range names a space. Returns PrColorSpace_Count for code
// points that name none of PrColorSpace, as for narrow range or YCbCr.
PrColorSpace imageCicpSpace(uint8_t primaries, uint8_t transfer, uint8_t matrix, uint8_t fullRange);

// Returns the space that the cicp tag of the ICC profile names (ICC.1 v4.4), or PrColorSpace_Count.
PrColorSpace imageProfileCicpSpace(PrByteView profile);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
