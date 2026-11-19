// The glyph decoder of SVG, which draws the SVG documents of glyphs, and their images in bitmap strikes.

#pragma once

#include "pr/base.h"
#include "pr/compress.h"
#include "pr/draw_glyph.h"
#include "pr/image.h"
#include "pr/texture_cache.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Creates a decoder that draws the SVG documents of glyphs, and the images of glyphs without a document from their
// bitmap strikes (prFontGlyphImage). It decodes images through `imageCodec`, and inflates gzip-compressed documents
// through `compressCodec`. Images, and the rasters of documents, realize in `textures`, on the device of the renderer
// that the decoder serves. Without a codec or the cache, what it would draw draws nothing. Each outlives the decoder.
// The decoder keeps the documents that it parsed most recently, and the images that it opened. A glyph's image draws
// once its texture arrives: until then, each use of the glyph cache asks again.
PrDrawGlyphDecoder* prSvgGlyphDecoderCreate(const PrImageCodecSet* PR_NULLABLE imageCodec,
    const PrCompressCodecSet* PR_NULLABLE compressCodec, PrTextureCache* PR_NULLABLE textures);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
