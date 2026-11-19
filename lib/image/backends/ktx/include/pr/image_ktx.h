// The KTX 2.0 codec: lib/ktx's textures (pr/ktx.h) as a member of an image codec set.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/image.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a codec that decodes KTX 2.0 textures (.ktx2), of the type image/ktx2, and encodes nothing. A texture has mip
// levels, and its layers and faces are its frames. It decodes to the pixels that lib/ktx decodes it to: one channel is
// gray, and two are red and green with no blue. Its descriptor gives the color of its pixels and whether alpha is
// premultiplied, and its KTXorientation the orientation. Levels that are Zstandard or zlib streams decompress through
// `compress`, which must outlive the codec. A texture of blocks that do not decode to pixels, a volume, an ETC1S video
// and any other format are NotSupported. A texture's blocks, and its transcoding to the blocks that a GPU samples, are
// lib/ktx's to read.
PrImageCodec* prImageKtxCreate(const PrCompressCodecSet* compress);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
