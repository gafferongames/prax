// Images as texture sources (pr/texture.h): any image that an image codec decodes, as the pixels of its mip levels and
// of a cube's faces and layers, as the codec reports them (PrImageInfo).

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/image.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What the values of an image's source are.
enum {
    // As the image stores them: an image does not say whether its values are color, so they are of the plain forms.
    PrImageTextureColor_Stored,
    // Colors in sRGB, as an image is shown: values of 8 bits are of the Srgb form, an image of another named RGB space
    // reads as linear extended sRGB in Rgba16F, and one of any other color, an ICC profile's included, reads in
    // Rgba8Srgb as the codec converts it. Images of floats keep their linear values.
    PrImageTextureColor_Srgb,
};
typedef uint8_t PrImageTextureColor;

// Creates a source that decodes the image that `bytes` provides with `codec`, which outlives the source. The source
// retains `bytes`, and each of its reads reads them again and decodes what the read asks for with a decoder of its own.
//
// The texture is the image's first frame, or its cube's faces and layers, in the image's stored orientation, which its
// description states, with the image's alpha. Its mip levels are the image's own if they halve as a texture's do, and
// its first alone otherwise, as for a rip map. Its pixels are of the plain forms, since an image does not say whether
// its values are color: Rgba8 for samples of 8 and 16 bits, and Rgba32F for floats and 32-bit integers. It decodes to
// the others of Rgba8, Rgba16F and Rgba32F too (prTextureSourceReformat), without converting the values, and to R8, its
// gray or its red alone. An image whose channels are not gray or RGB, such as CMYK, decodes to Rgba8Srgb alone, as the
// codec converts it to sRGB.
//
// Returns the status of reading the bytes, NotSupported if the codec does not recognize them, Limit for an image past
// `limits` (null means the defaults) or the image library's (prImageLimitsDefault), as its header states it, and
// InvalidData for one that does not open. On failure, `*out` is null.
PrStatus prImageTextureSourceOpen(const PrBlobSource* bytes, const PrImageCodecSet* codec,
    const PrTextureLimits* PR_NULLABLE limits, PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Opens a source as prImageTextureSourceOpen does, over a copy of `bytes`.
PrStatus prImageTextureSourceOpenBytes(PrByteView bytes, const PrImageCodecSet* codec,
    const PrTextureLimits* PR_NULLABLE limits, PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Returns the format of an opener (prTextureOpenerCreate) that reads the media types that `codec` decodes, which
// outlives the opener's sources.
PrTextureFormat prImageTextureFormat(const PrImageCodecSet* codec);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
