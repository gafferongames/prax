// The JPEG codec: lib/jpeg's decoder (pr/jpeg.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes JPEG, of the type image/jpeg, and encodes nothing.
PrImageCodec* prImageJpegCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
