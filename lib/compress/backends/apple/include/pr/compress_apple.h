// The compression codec of Apple platforms: Apple's Compression library, for the bare formats that it codes.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes and encodes the bare `format`, Deflate, Brotli or LZFSE, with Apple's Compression
// library. Its decoders may read input past the end of their streams (pr/compress.h, prCompressCodecSetCreate). It
// encodes at the library's one level, whatever level a call asks for.
PrCompressCodec* prCompressAppleCreate(PrCompressFormat format);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
