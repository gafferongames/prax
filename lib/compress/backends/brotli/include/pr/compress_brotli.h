// The Brotli codec: lib/brotli's decoder (pr/brotli.h) as a member of a codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes Brotli, and encodes nothing. The codec has no state of its own, so one codec serves
// every thread.
PrCompressCodec* prCompressBrotliCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
