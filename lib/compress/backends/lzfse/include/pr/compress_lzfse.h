// The LZFSE codec: lib/lzfse's decoder (pr/lzfse.h) as a member of a codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes LZFSE, and encodes nothing. The codec has no state of its own, so one codec serves every
// thread.
PrCompressCodec* prCompressLzfseCreate(void);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
