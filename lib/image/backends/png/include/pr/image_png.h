// The PNG codec: lib/png's decoder and encoder (pr/png.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes PNG, animated PNG included, of the types image/png and image/apng, and encodes
// image/png. Its zlib streams decompress and compress through `compress`, which must outlive the codec. If `compress`
// encodes no zlib, the codec stores the image data uncompressed.
PrImageCodec* prImagePngCreate(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
