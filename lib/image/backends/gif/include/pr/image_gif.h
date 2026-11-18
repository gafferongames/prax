// The GIF codec: lib/gif's decoder (pr/gif.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes GIF, animated or still, of the type image/gif, and encodes nothing.
PrImageCodec* prImageGifCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
