// The ICO codec: lib/ico's decoder (pr/ico.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes ICO, of the types image/x-icon and image/vnd.microsoft.icon, and encodes nothing. It
// decodes an icon's bitmap images itself, and opens its PNG images through the set that opens the icon, so a set
// without a PNG codec reports NotSupported for them.
PrImageCodec* prImageIcoCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
