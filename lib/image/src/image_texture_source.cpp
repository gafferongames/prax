// Images as texture sources: an image's encoded bytes decoded to pixels.

#include "pr/image_texture_source.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"

namespace pr_impl {

namespace {

// What decodes an image: its bytes, its codec and the limits within which it opens, and the image as the codec reported
// it when the source was made.
struct ImageTexture {
    const PrBlobSource* bytes;
    const PrImageCodecSet* codec;
    PrImageLimits limits;
    PrImageInfo image;
    bool converted;             // read as the codec converts them to sRGB, in 8 bits
    bool wide;                  // read as linear extended sRGB in half floats, from `space`
    PrColorSpace space;
    PrTextureChannels channels;
    PrTextureSample sample;
    PrTextureDesc desc;
};

// A read of a source: the source, and the read that it answers.
struct ImageRead {
    const ImageTexture* image;
    PrTextureRead* read;
};

// The orientation flags of each EXIF orientation, by PrImageOrientation.
constexpr PrTextureOrientationFlags kOrientations[] = {
    0,
    PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipX | PrTextureOrientationFlag_FlipY,
    PrTextureOrientationFlag_Transpose | PrTextureOrientationFlag_FlipY,
};

// The samples of a texture, by PrImageSample.
constexpr PrTextureSample kSamples[] = {
    PrTextureSample_U8, PrTextureSample_U16, PrTextureSample_I16, PrTextureSample_F16, PrTextureSample_F32,
    PrTextureSample_U32,
};

// The formats that an image decodes to.
constexpr PrGpuFormat kDecoded[] = {PrGpuFormat_R8, PrGpuFormat_Rgba8, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};

// Returns the limits within which an image of a texture of `limits` opens, as a decoder checks them against the header
// before it decodes. The texture's budget sets them, not the image library's defaults, which bound a page's images: the
// input, the working bytes and the pixels of one frame each take up to its working bytes, and the work scales with its
// pixels, so that an image as large as the texture admits opens. The other limits are the library's.
PrImageLimits imageLimits(const PrTextureLimits& limits) {
    PrImageLimits image = prImageLimitsDefault();
    image.inputBytes = limits.workingBytes;
    image.pixels = limits.pixels;
    image.workingBytes = limits.workingBytes;
    image.outputBytes = limits.workingBytes;
    // The steps of a decode: a few for each sample of each pixel.
    const uint64_t work = limits.pixels * 64;
    image.work = work > image.work ? work : image.work;
    return image;
}

// Opens the image of `bytes` with a decoder of `codec` at `*out`.
PrStatus imageOpen(const PrImageCodecSet* codec, PrByteView bytes, const PrImageLimits& limits,
        PrImageDecoder* PR_NULLABLE* PR_NONNULL out) {
    PrImageDecoder* decoder = prImageDecoderCreate(codec);
    PrImageSource source{};
    source.memory = bytes;
    PrStatus status = prImageDecoderOpen(decoder, source, &limits);
    if (status == PrStatus_Truncated) {
        // Not every frame and level reads, so the image is not a texture.
        status = PrStatus_InvalidData;
    }
    if (status != PrStatus_Ok) {
        prImageDecoderDestroy(decoder);
        decoder = nullptr;
    }
    *out = decoder;
    return status;
}

// Converts `count` texels of straight half floats in `space` to linear extended sRGB in place.
void toLinearSrgb(PrColorSpace space, uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        PrFloat16 texel[4];
        prMemCopy(texel, texels + i * 8, sizeof(texel));
        double channels[3];
        for (uint32_t k = 0; k < 3; ++k)
            channels[k] = double(prFloat16ToFloat(texel[k]));
        prColorConvert(space, PrColorSpace_SrgbLinear, channels, channels);
        for (uint32_t k = 0; k < 3; ++k)
            texel[k] = prFloat16FromFloat(float(channels[k]));
        prMemCopy(texels + i * 8, texel, sizeof(texel));
    }
}

// Decodes frame `layer` of a wide image as linear extended sRGB in straight half floats at `pixels`.
PrStatus decodeWide(const ImageTexture& image, PrImageDecoder* decoder, uint32_t layer, PrPixels pixels) {
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    PrUVec2 extent = pixels.extent;
    size_t stride = size_t(extent.x) * prImageFormatSize(&info.format);
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y + 1));
    PrStatus status = prImageDecoderRead(decoder, layer, PrPixels{stored, stride, extent});
    // The texture holds four channels, so an opaque image's alpha converts to one.
    PrImageFormat wide{
        .model = PrColorModel_Rgb,
        .sample = PrImageSample_F16,
        .alpha = PrImageAlpha_Straight,
        .color = info.format.color,
    };
    if (status == PrStatus_Ok)
        status = prImageConvert(PrPixelView{stored, stride, extent}, &info.format, pixels, &wide, 0);
    if (status == PrStatus_Ok)
        toLinearSrgb(image.space, pixels.data, size_t(extent.x) * extent.y);
    prMemoryFree(stored);
    return status;
}

// Decodes level `level` of frame `layer` in the source's format at `out`.
PrStatus decodeLevel(const ImageTexture& image, PrImageDecoder* decoder, uint32_t level, uint32_t layer, uint8_t* out) {
    PrUVec2 extent = prTextureLevelExtent(image.desc.extent, level);
    uint32_t texelBytes = prGpuFormatBlock(image.desc.format).bytes;
    PrPixels pixels{out, size_t(extent.x) * texelBytes, extent};
    if (image.wide)
        return decodeWide(image, decoder, layer, pixels);
    if (image.converted) {
        // The codec converts the first level to sRGB, which is what the texture's Srgb form holds.
        return prImageDecoderReadRgba8(decoder, layer, PrImageAlpha_Straight, PrImageOrientation_Identity, pixels);
    }
    // Pixels stored as the format holds them decode in place. A Srgb form holds the same bytes as its plain form.
    PrGpuFormat plain = prGpuFormatPlain(image.desc.format);
    if (image.channels == PrTextureChannels_Rgba && image.sample == PrTextureSample_U8 && plain == PrGpuFormat_Rgba8)
        return prImageDecoderReadLevel(decoder, layer, level, pixels);
    uint32_t pixelBytes = prTexturePixelSize(image.channels, image.sample);
    size_t stride = size_t(extent.x) * pixelBytes;
    uint8_t* stored = static_cast<uint8_t*>(prMemoryAlloc(stride * extent.y));
    PrStatus status = prImageDecoderReadLevel(decoder, layer, level, PrPixels{stored, stride, extent});
    if (status == PrStatus_Ok)
        prTexturePixelsUnpack(PrPixelView{stored, stride, extent}, image.channels, image.sample, plain, pixels);
    prMemoryFree(stored);
    return status;
}

// Decodes what a read asks for from `bytes`, the image's bytes as they were read again.
PrStatus decodeRead(const ImageTexture& image, PrByteView bytes, PrTextureRead* read) {
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(image.codec, bytes, image.limits, &decoder);
    // The bytes that are read again are those of the image that the source describes.
    if (status == PrStatus_Ok) {
        const PrImageInfo& again = *prImageDecoderInfo(decoder);
        bool same = again.extent == image.image.extent && again.levelCount == image.image.levelCount &&
            again.frameCount == image.image.frameCount && again.format.sample == image.image.format.sample &&
            again.format.model == image.image.format.model && again.format.alpha == image.image.format.alpha;
        status = same ? PrStatus_Ok : PrStatus_InvalidData;
    }
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    for (uint32_t level = asked.firstLevel; status == PrStatus_Ok && level < asked.firstLevel + asked.levelCount; ++level) {
        for (uint32_t layer = asked.firstLayer; status == PrStatus_Ok && layer < asked.firstLayer + asked.layerCount; ++layer) {
            uint8_t* out = prTextureReadStorage(read) + prTextureReadOffset(&image.desc, &asked, level, layer);
            status = decodeLevel(image, decoder, level, layer, out);
        }
    }
    prImageDecoderDestroy(decoder);
    return status == PrStatus_Truncated ? PrStatus_InvalidData : status;
}

void sourceRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const ImageTexture* image = static_cast<const ImageTexture*>(context);
    ImageRead* held = static_cast<ImageRead*>(prMemoryAlloc(sizeof(ImageRead)));
    *held = ImageRead{image, read};
    PrBlobReadDesc bytes = prBlobReadDescDefault();
    bytes.done = [](void* PR_NULLABLE context, PrBlobRead* blob) {
        ImageRead held = *static_cast<ImageRead*>(context);
        prMemoryFree(context);
        PrStatus status = prBlobReadStatus(blob);
        if (status == PrStatus_Ok)
            status = decodeRead(*held.image, prBlobBytes(prBlobReadBlob(blob)), held.read);
        prBlobReadDestroy(blob);
        prTextureReadComplete(held.read, status);
    };
    bytes.context = held;
    prBlobReadCreate(image->bytes, &bytes);
}

void sourceRelease(void* PR_NULLABLE context) {
    ImageTexture* image = static_cast<ImageTexture*>(context);
    prBlobSourceRelease(image->bytes);
    prMemoryFree(image);
}

const PrTextureSource* sourceOf(const ImageTexture& image);

PrStatus sourceReformat(void* PR_NULLABLE context, PrGpuFormat format, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    ImageTexture image = *static_cast<const ImageTexture*>(context);
    image.desc.format = format;
    *out = sourceOf(image);
    return PrStatus_Ok;
}

// Returns a source of a copy of `image`, which retains its bytes.
const PrTextureSource* sourceOf(const ImageTexture& image) {
    ImageTexture* held = allocZeroed<ImageTexture>();
    *held = image;
    prBlobSourceRetain(image.bytes);
    PrGpuFormat formats[4];
    uint32_t count = 0;
    for (PrGpuFormat format : kDecoded) {
        if (format != image.desc.format && !image.converted && !image.wide)
            formats[count++] = format;
    }
    PrTextureSourceDesc desc{
        .texture = image.desc,
        .read = sourceRead,
        .reformat = sourceReformat,
        .formats = formats,
        .formatCount = count,
        .release = sourceRelease,
        .context = held,
    };
    return prTextureSourceCreate(&desc);
}

// Returns the number of the image's mip levels that halve as a texture's do, rounding down, from the first.
uint32_t halvingLevels(const PrImageDecoder* decoder, const PrImageInfo& info) {
    if (info.levels != PrImageLevels_Mip)
        return 1;
    uint32_t count = 1;
    while (count < info.levelCount && count < uint32_t(kPrTextureMaxLevels) &&
            prImageDecoderLevelExtent(decoder, count) == prTextureLevelExtent(info.extent, count))
        ++count;
    return count;
}

// Returns true if `color` is sRGB's, or says nothing, which is taken as sRGB's.
bool isSrgb(const PrImageColor& color) {
    return color.encoding == PrImageColorEncoding_Unknown ||
        (color.encoding == PrImageColorEncoding_Named && color.space == PrColorSpace_Srgb);
}

// Creates a source over `bytes`, described by `blob`, whose colors are as `color` says.
PrStatus sourceCreate(const PrBlobSource* bytes, const PrBlob* blob, const PrImageCodecSet* codec,
        const PrTextureLimits& limits, PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrImageLimits imageBounds = imageLimits(limits);
    PrImageDecoder* decoder = nullptr;
    PrStatus status = imageOpen(codec, prBlobBytes(blob), imageBounds, &decoder);
    if (status != PrStatus_Ok)
        return status;
    const PrImageInfo& info = *prImageDecoderInfo(decoder);
    const PrImageFormat& format = info.format;
    bool gray = format.model == PrColorModel_Gray;
    bool hasAlpha = format.alpha != PrImageAlpha_None;
    ImageTexture image{
        .bytes = bytes,
        .codec = codec,
        .limits = imageBounds,
        .image = info,
        .sample = kSamples[format.sample],
    };
    image.image.format.color.profile = PrByteView{};
    if (gray)
        image.channels = hasAlpha ? PrTextureChannels_GrayAlpha : PrTextureChannels_Gray;
    else
        image.channels = hasAlpha ? PrTextureChannels_Rgba : PrTextureChannels_Rgb;
    image.converted = (!gray && format.model != PrColorModel_Rgb) ||
        prImageFormatSize(&format) != prTexturePixelSize(image.channels, image.sample);
    bool wide = format.sample == PrImageSample_F16 || format.sample == PrImageSample_F32 ||
        format.sample == PrImageSample_U32;
    // In sRGB, an image of another named RGB space is converted to linear floats, and one of any other color, an ICC
    // profile's included, as the codec converts it.
    bool sized = info.extent.x != 0 && info.extent.y != 0;
    if (color == PrImageTextureColor_Srgb && sized && !wide && !isSrgb(format.color) && !image.converted) {
        if (format.color.encoding == PrImageColorEncoding_Named &&
                prColorSpaceModel(format.color.space) == PrColorModel_Rgb) {
            image.wide = true;
            image.space = format.color.space;
        } else {
            image.converted = true;
        }
    }
    image.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = info.extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = halvingLevels(decoder, info),
        .format = PrGpuFormat(wide && !image.converted ? PrGpuFormat_Rgba32F : PrGpuFormat_Rgba8),
        .alpha = PrTextureAlpha(hasAlpha ? PrTextureAlpha_Straight : PrTextureAlpha_None),
        .orientation = kOrientations[info.orientation],
    };
    if (format.alpha == PrImageAlpha_Premultiplied)
        image.desc.alpha = PrTextureAlpha_Premultiplied;
    // The frames of a cube are its faces, layer after layer.
    if (info.faceCount == 6 && info.frameCount >= 6) {
        image.desc.kind = info.frameCount > 6 ? PrGpuTextureKind_CubeArray : PrGpuTextureKind_Cube;
        image.desc.layerCount = info.frameCount / 6 * 6;
    }
    // Channels that the codec converts become values of sRGB's transfer function, which are color.
    if (image.converted) {
        image.desc.levelCount = 1;
        image.desc.alpha = PrTextureAlpha_Straight;
        image.desc.format = PrGpuFormat_Rgba8Srgb;
    }
    if (image.wide) {
        image.desc.levelCount = 1;
        image.desc.format = PrGpuFormat_Rgba16F;
        image.desc.layerCount = 1;
        image.desc.kind = PrGpuTextureKind_2d;
    }
    // In sRGB, values of 8 bits are sRGB's.
    if (color == PrImageTextureColor_Srgb && image.desc.format == PrGpuFormat_Rgba8)
        image.desc.format = PrGpuFormat_Rgba8Srgb;
    prImageDecoderDestroy(decoder);
    *out = sourceOf(image);
    return PrStatus_Ok;
}

bool formatSupportsType(const void* PR_NULLABLE context, PrStringView type) {
    return prImageCodecSetSupportsType(static_cast<const PrImageCodecSet*>(context), type);
}

PrStatus formatCreate(const void* PR_NULLABLE context, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    return sourceCreate(bytes, blob, static_cast<const PrImageCodecSet*>(context), *limits, PrImageTextureColor_Stored, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prImageTextureSourceOpen(const PrBlobSource* bytes, const PrImageCodecSet* codec,
        const PrTextureLimits* PR_NULLABLE limits, PrImageTextureColor color,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrBlob* blob = nullptr;
    PrStatus status = prBlobSourceRead(bytes, prBlobRangeWhole(), &blob);
    if (status != PrStatus_Ok)
        return status;
    PrTextureLimits bounds = limits != nullptr ? *limits : prTextureLimitsDefault();
    status = sourceCreate(bytes, blob, codec, bounds, color, out);
    prBlobRelease(blob);
    return status;
}

PrStatus prImageTextureSourceOpenBytes(PrByteView bytes, const PrImageCodecSet* codec,
        const PrTextureLimits* PR_NULLABLE limits,
        PrImageTextureColor color, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrBlobSource* source = prBlobSourceCreateBlob(prBlobCopy(bytes));
    PrStatus status = prImageTextureSourceOpen(source, codec, limits, color, out);
    prBlobSourceRelease(source);
    return status;
}

PrTextureFormat prImageTextureFormat(const PrImageCodecSet* codec) {
    return PrTextureFormat{.supportsTypeFn = formatSupportsType, .createFn = formatCreate, .context = codec};
}
