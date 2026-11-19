// The codec sets that the tests, the harnesses and the tools decode and encode with: every codec that Prax has, in the
// selectors' order, created once for the process and never freed.

#pragma once

#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

namespace harness {

// Returns the set of every compression codec. Any thread may call it.
const PrCompressCodecSet* harnessCompress();

// Returns the set of every image codec, which decompresses through harnessCompress. Any thread may call it.
const PrImageCodecSet* harnessImages();

} // namespace harness

PR_ASSUME_NONNULL_END
