// PNG (png-3): its header and the ancillary chunks that describe the image, its pixels, the frames of an animated PNG,
// and the encoding of 8-bit RGBA. The zlib streams of the image data and of compressed chunks decompress through a set
// of compression codecs. pr/png_text.h reads and writes the file's texts.

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

// The size of the signature that begins a PNG file (png-3/#5PNG-file-signature).
enum { kPrPngSignatureBytes = 8 };

// The frame index that reads the default image of an animated PNG that is not one of its frames.
enum { kPrPngFrameCover = 0xffffffffu };

// Returns true if `bytes` begin with the PNG signature.
bool prPngHasSignature(PrByteView bytes);

// The largest amounts of resources that a decode may use. Each is a hard limit.
typedef struct PrPngLimits {
    uint64_t pixels;
    uint64_t workingBytes;  // the total bytes that a decoder holds, including those its caller holds for it
    uint64_t outputBytes;   // of one frame's pixels
    uint64_t work;          // the chunks walked, the samples decoded, and the bytes of the file and of each frame
    uint32_t frames;
    uint32_t metadataBytes; // of the ancillary chunks, and of the profile and the Exif data expanded
} PrPngLimits;

// Returns the default limits, which bound the resources that the images of a page can reasonably use.
PrPngLimits prPngLimitsDefault(void);

// The code points of Rec. ITU-T H.273 that a cICP chunk gives (png-3/#cICP-chunk).
typedef struct PrPngCicp {
    uint8_t primaries;
    uint8_t transfer;
    uint8_t matrix;
    uint8_t fullRange;
} PrPngCicp;

// A PNG image, as its header and its chunks describe it. The decoded pixels are interleaved samples of `channels`:
// gray, gray and alpha, RGB, or RGBA, with straight alpha. An indexed image decodes to RGB, or RGBA if its palette has
// transparency, and transparency of a gray or RGB image adds an alpha channel. The views are of the file's bytes or of
// storage that the decoder holds.
typedef struct PrPngInfo {
    PrUVec2 extent;
    uint32_t channels;
    uint32_t sampleBytes;   // 1, or 2 for 16-bit samples in the machine's byte order
    bool alpha;             // the last channel is alpha

    // What the color is: the sRGB chunk's sRGB if the file has no profile (png-3/#11sRGB), the code points of a cICP
    // chunk, and the ICC profile of an iCCP chunk, expanded within the metadata limit.
    bool srgb;
    bool hasCicp;
    PrPngCicp cicp;
    PrByteView profile;

    // The Exif data, a TIFF structure, from the eXIf chunk or else from a raw profile of the text chunks, or empty.
    PrByteView exif;
    PrVec2 pixelsPerMeter;  // the pHYs chunk's resolution, or zero if it gives none in meters

    uint32_t frameCount;    // one for a still image
    uint32_t playCount;     // the plays of an animation, or zero for forever, and one for a still image
    bool hasCover;          // true if kPrPngFrameCover reads a default image that is not a frame
    uint64_t workingBytes;  // the most bytes that the decoder holds, including those its caller holds for it
} PrPngInfo;

// A PNG image open for reading: its chunks, its expanded metadata, and the composition of an animation. One thread uses
// a decoder at a time.
typedef struct PrPngDecoder PrPngDecoder;

// Opens the PNG image in `bytes`, which must stay valid until the decoder is destroyed, within `limits`, or the
// defaults if they are null. Its zlib streams decompress through `compress`, which must outlive the decoder. `held` is
// the bytes that the caller holds for it already, its input among them, which the info's workingBytes counts with the
// decoder's own. Returns NotSupported without the signature or for a header that the format does not define,
// InvalidData for chunks that break the format, Limit past the limits, and Truncated for a file that ends before its
// image data does. `*out` is the decoder if the status is Ok or Truncated, and null otherwise.
PrStatus prPngDecoderCreate(PrByteView bytes, const PrCompressCodecSet* compress, const PrPngLimits* PR_NULLABLE limits,
    uint64_t held, PrPngDecoder* PR_NULLABLE* PR_NONNULL out);

// Frees the decoder and what it holds.
void prPngDecoderDestroy(PrPngDecoder* PR_NULLABLE decoder);

// Returns what the decoder's image is.
const PrPngInfo* prPngDecoderInfo(const PrPngDecoder* decoder);

// Returns how long `frame` shows, in milliseconds as encoded, or zero for a still image. The image has the frame.
uint32_t prPngDecoderFrameDuration(const PrPngDecoder* decoder, uint32_t frame);

// Decodes `frame`, which the image has, into `output`, of the image's extent. An animated PNG's frames are composed
// over the frames before them, from a transparent canvas: composing them in order continues the composition, and any
// other order restarts it. Returns InvalidData for a frame that does not decode.
PrStatus prPngDecoderRead(PrPngDecoder* decoder, uint32_t frame, PrPixels output);

// Encodes `rgba8`, top-down straight-alpha 8-bit RGBA of nonzero size, as a PNG. Each row is filtered, and the rows are
// compressed as zlib by `compress`, or stored if it encodes no zlib. Writes up to `capacity` bytes to `out`, and
// returns the size of the whole encoding. `out` may be null if `capacity` is zero. Each call compresses the image
// again.
size_t prPngEncode(PrPixelView rgba8, const PrCompressCodecSet* compress, uint8_t* PR_NULLABLE out, size_t capacity);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
