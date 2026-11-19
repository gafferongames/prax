// TIFF (TIFF 6.0): the first image of a file, uncompressed, with 8-bit samples stored chunky, and its pixels.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The size of the header's byte order and version that begin a TIFF file.
enum { kPrTiffSignatureBytes = 4 };

// Returns true if `bytes` begin with a TIFF header in either byte order.
bool prTiffHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrTiffLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the image's pixels
    uint64_t work;          // the pixels of the image
} PrTiffLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrTiffLimits prTiffLimitsDefault(void);

// What the extra sample after the color samples is, as the ExtraSamples tag gives it (TIFF 6.0 section 18).
enum {
    PrTiffAlpha_None,           // no extra sample
    PrTiffAlpha_Associated,     // opacity, already multiplied into the color samples
    PrTiffAlpha_Unassociated,   // opacity, not applied to the color samples
    PrTiffAlpha_Unspecified,    // data of the application's own
};
typedef uint8_t PrTiffAlpha;

// A TIFF image, as the tags of its first image file directory describe it. The decoded pixels are interleaved 8-bit
// samples: gray or RGB, then the extra sample if there is one. Gray is black at zero.
typedef struct PrTiffInfo {
    PrUVec2 extent;
    uint32_t channels;
    bool gray;
    PrTiffAlpha alpha;
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrTiffInfo;

// A TIFF image open for reading. One thread uses a decoder at a time.
typedef struct PrTiffDecoder PrTiffDecoder;

// Opens the TIFF image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own. Returns NotSupported for a compression, a sample size or a layout
// that the decoder does not read, InvalidData for tags that break the format, and Limit past the limits. `*out` is the
// decoder if the status is Ok, and null otherwise.
PrStatus prTiffDecoderCreate(PrByteView bytes, const PrTiffLimits* PR_NULLABLE limits, uint64_t held,
    PrTiffDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prTiffDecoderDestroy(PrTiffDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrTiffInfo* prTiffDecoderInfo(const PrTiffDecoder* decoder);

// Decodes the image into `output`, of its extent. Returns InvalidData if a strip lies outside the file.
PrStatus prTiffDecoderRead(PrTiffDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
