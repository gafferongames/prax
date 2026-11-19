// GIF (GIF89a and GIF87a): an image's logical screen, its frames, their disposal and their delays, and their pixels.

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

// The size of the signature and version that begin a GIF file.
enum { kPrGifSignatureBytes = 6 };

// Returns true if `bytes` begin with the GIF signature and a version that the decoder reads.
bool prGifHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrGifLimits {
    uint64_t pixels;        // of the screen, and of each frame
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of one frame's pixels
    uint64_t work;          // the blocks walked, and the bytes of the file and the pixels of its frames
    uint32_t frames;
    uint32_t metadataBytes; // of the extension blocks
} PrGifLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrGifLimits prGifLimitsDefault(void);

// A GIF image, as its blocks describe it. Its frames decode to 8-bit RGBA, straight alpha, with sRGB colors.
typedef struct PrGifInfo {
    PrUVec2 extent;         // the logical screen
    uint32_t frameCount;    // the frames whose blocks the file holds whole, which may be none
    uint32_t playCount;     // the plays of a NETSCAPE2.0 loop, or zero for forever, and one without the extension
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrGifInfo;

// A GIF image open for reading: its frames, and the composition of the screen. One thread uses a decoder at a time.
typedef struct PrGifDecoder PrGifDecoder;

// Opens the GIF image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. `held` is the bytes that the caller holds for it already, its input among them, which the
// info's workingBytes counts with the decoder's own. Returns NotSupported without the signature, InvalidData for blocks
// that break the format, Limit past the limits, and Truncated for a file that ends before its trailer, with the frames
// whose blocks it holds whole. `*out` is the decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prGifDecoderCreate(PrByteView bytes, const PrGifLimits* PR_NULLABLE limits, uint64_t held,
    PrGifDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prGifDecoderDestroy(PrGifDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrGifInfo* prGifDecoderInfo(const PrGifDecoder* decoder);

// Returns how long `frame` shows, in milliseconds as encoded. The image has the frame.
uint32_t prGifDecoderFrameDuration(const PrGifDecoder* decoder, uint32_t frame);

// Decodes `frame`, which the image has, into `output`, of the screen's extent, composed over the frames before it from
// a transparent screen. Composing the frames in order continues the composition, and any other order restarts it.
// Returns InvalidData for a frame that does not decode.
PrStatus prGifDecoderRead(PrGifDecoder* decoder, uint32_t frame, PrPixels output);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
