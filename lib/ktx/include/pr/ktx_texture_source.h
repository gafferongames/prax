// KTX 2.0 as texture sources (pr/texture.h): a file's blocks as they are stored, the blocks that Basis Universal's
// formats transcode to, and the pixels that its levels decode to.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/compress.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a source of the KTX 2.0 texture that `bytes` provides, which it retains, and reads again for each read of the
// source. Supercompressed levels inflate through `codec`, which outlives the source, and are NotSupported without one.
// A file that ends before its levels' data is a texture whose levels fail to read, so the bytes before its levels
// (prKtxInfoBytes) describe it whole.
//
// A file of blocks is a source of those blocks. A file of pixels, or of one of Basis Universal's formats, is a source
// of pixels: Rgba8 of samples of 8 and 16 bits, and Rgba32F of floats. The source produces the other forms too
// (prTextureSourceReformat): the blocks that Basis Universal's formats transcode to (PrKtxInfo::transcodes), and the
// pixels of Rgba8, Rgba16F and Rgba32F of a texture whose levels decode, and their gray or red alone in R8.
//
// Returns the status of reading the bytes, Limit for a texture past `limits` (null means the defaults), and the
// failures of prKtxTextureCreate other than Truncated. On failure, `*out` is null.
PrStatus prKtxTextureSourceOpen(const PrBlobSource* bytes, const PrCompressCodecSet* PR_NULLABLE codec,
    const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Returns the format of an opener (prTextureOpenerCreate) that reads the type image/ktx2, and creates its sources with
// `codec`, which outlives the opener's sources.
PrTextureFormat prKtxTextureFormat(const PrCompressCodecSet* PR_NULLABLE codec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
