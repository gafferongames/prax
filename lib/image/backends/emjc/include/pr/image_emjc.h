// The emjc codec: lib/emjc's decoder (pr/emjc.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes emjc, the images of Apple Color Emoji on iOS, of the type image/x-emjc, and encodes
// nothing. Their planes decompress as LZFSE through `compress`, which must outlive the codec.
PrImageCodec* prImageEmjcCreate(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
