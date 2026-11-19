// The TIFF codec: lib/tiff's decoder (pr/tiff.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes the first image of an uncompressed TIFF of 8-bit chunky samples, of the type image/tiff,
// and encodes nothing.
PrImageCodec* prImageTiffCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
