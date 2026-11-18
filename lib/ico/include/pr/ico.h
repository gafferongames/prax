// ICO: choosing an image from an icon's directory, and the pixels of a bitmap image. An image that the directory holds
// as a PNG stays encoded, for its caller to decode.

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

// The size of the directory's header, which begins an ICO file.
enum { kPrIcoSignatureBytes = 6 };

// Returns true if `bytes` begin with the header of an icon's directory.
bool prIcoHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrIcoLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of the image's pixels
    uint64_t work;          // the directory's entries, and the pixels of a bitmap and its mask
} PrIcoLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrIcoLimits prIcoLimitsDefault(void);

// The image that a decoder chose from the directory: the largest complete image that it reads, and among images of
// equal area, the one of the greatest bit depth. A bitmap decodes to 8-bit RGBA, straight alpha, with sRGB colors. A
// PNG image is a view of the file's bytes, and its extent is the directory's.
typedef struct PrIcoInfo {
    PrUVec2 extent;
    PrByteView png;         // the PNG image, or empty for a bitmap
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrIcoInfo;

// An icon open for reading. One thread uses a decoder at a time.
typedef struct PrIcoDecoder PrIcoDecoder;

// Opens the icon in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the defaults if
// they are null. `held` is the bytes that the caller holds for it already, its input among them, which the info's
// workingBytes counts with the decoder's own. Returns NotSupported without the header, InvalidData for a malformed
// directory or one without an image that the decoder reads, and Limit past the limits. `*out` is the decoder if the
// status is Ok, and null otherwise.
PrStatus prIcoDecoderCreate(PrByteView bytes, const PrIcoLimits* PR_NULLABLE limits, uint64_t held,
    PrIcoDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prIcoDecoderDestroy(PrIcoDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrIcoInfo* prIcoDecoderInfo(const PrIcoDecoder* decoder);

// Decodes a bitmap image into `output`, of its extent. Returns NotSupported for a PNG image, and InvalidData for a
// bitmap that does not decode.
PrStatus prIcoDecoderRead(PrIcoDecoder* decoder, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
