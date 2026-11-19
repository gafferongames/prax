// The Deflate codec: lib/deflate (pr/deflate.h) as a member of a codec set, which frames zlib and gzip around it.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes and encodes Deflate, zlib and gzip. The codec has no state of its own, so one codec
// serves every thread.
PrCompressCodec* prCompressDeflateCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
