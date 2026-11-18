// The image codec of Apple platforms: Apple's ImageIO, for one format that it decodes.

#pragma once

#include "pr/base.h"
#include "pr/image.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes images of the media type `type`, such as "image/png" or "image/heic", with ImageIO.
// Returns null if ImageIO does not decode the type. The codec decodes the frames as ImageIO composes them, to 8-bit
// samples: RGB in the image's own color space if it has an RGB one, and sRGB otherwise. It encodes nothing.
PrImageCodec* PR_NULLABLE prImageImageIOCreate(PrStringView type);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
