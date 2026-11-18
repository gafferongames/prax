// emjc: the images that Apple Color Emoji stores in its sbix strikes on iOS, and their pixels. Apple does not document
// the format. The decoder follows emjc-decoder (github.com/cc4966/emjc-decoder) and EmojiFonts' notes, with the
// rounding that reproduces the PNG strikes of the same glyphs exactly. The planes of an image decompress as LZFSE
// through a set of compression codecs.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The size of the signature that begins an emjc image.
enum { kPrEmjcSignatureBytes = 4 };

// Returns true if `bytes` begin with the signature of an emjc image.
bool prEmjcHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrEmjcLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the image's pixels
    uint64_t work;          // the pixels of the image
} PrEmjcLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrEmjcLimits prEmjcLimitsDefault(void);

// An emjc image, as its header describes it. It decodes to 8-bit RGBA, straight alpha.
typedef struct PrEmjcInfo {
    PrUVec2 extent;
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrEmjcInfo;

// An emjc image open for reading. One thread uses a decoder at a time.
typedef struct PrEmjcDecoder PrEmjcDecoder;

// Opens the emjc image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. Its planes decompress through `compress`, which must outlive the decoder. `held` is the
// bytes that the caller holds for it already, its input among them, which the info's workingBytes counts with the
// decoder's own. Returns NotSupported without the signature or if `compress` decodes no LZFSE, InvalidData for a header
// of no pixels, and Limit past the limits. `*out` is the decoder if the status is Ok, and null otherwise.
PrStatus prEmjcDecoderCreate(PrByteView bytes, const PrCompressCodecSet* compress,
    const PrEmjcLimits* PR_NULLABLE limits, uint64_t held, PrEmjcDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prEmjcDecoderDestroy(PrEmjcDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrEmjcInfo* prEmjcDecoderInfo(const PrEmjcDecoder* decoder);

// Decodes the image into `output`, of its extent. Returns InvalidData if the planes do not decompress to their size or
// a row names no filter.
PrStatus prEmjcDecoderRead(PrEmjcDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
