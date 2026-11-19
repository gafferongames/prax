// The ImageIO codec against Prax's own: images that ImageIO and Prax encode decode to the same pixels through both.

#include "pr/array.h"
#include "pr/compress_select.h"
#include "pr/environment.h"
#include "pr/image.h"
#include "pr/image_imageio.h"
#include "pr/image_select.h"

#include "tools/harness/test.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>
#include <ImageIO/ImageIO.h>

namespace pr_impl {

namespace {

using namespace pr;

constexpr uint32_t kWidth = 37;
constexpr uint32_t kHeight = 23;

// Straight-alpha RGBA8 of smooth gradients, opaque or with alpha that varies across the image.
Array<uint8_t> pattern(bool alpha) {
    Array<uint8_t> pixels(kWidth * kHeight * 4);
    for (uint32_t y = 0; y < kHeight; ++y) {
        for (uint32_t x = 0; x < kWidth; ++x) {
            uint8_t* p = pixels.data() + (y * kWidth + x) * 4;
            p[0] = uint8_t(x * 255 / (kWidth - 1));
            p[1] = uint8_t(y * 255 / (kHeight - 1));
            p[2] = uint8_t((x + y) * 255 / (kWidth + kHeight - 2));
            p[3] = alpha ? uint8_t(128 + x * 127 / (kWidth - 1)) : 255;
        }
    }
    return pixels;
}

// Encodes straight-alpha RGBA8 `pixels` with ImageIO as the format `identifier`, in sRGB.
Array<uint8_t> imageioEncode(const char* identifier, const Array<uint8_t>& pixels, bool alpha) {
    CFDataRef data = CFDataCreate(kCFAllocatorDefault, pixels.data(), CFIndex(pixels.count()));
    CGDataProviderRef provider = CGDataProviderCreateWithCFData(data);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    const uint32_t info = alpha ? uint32_t(kCGImageAlphaLast) : uint32_t(kCGImageAlphaNoneSkipLast);
    CGImageRef image = CGImageCreate(kWidth, kHeight, 8, 32, kWidth * 4, space, info, provider, nullptr, false,
        kCGRenderingIntentDefault);
    CFMutableDataRef encoded = CFDataCreateMutable(kCFAllocatorDefault, 0);
    CFStringRef type = CFStringCreateWithCString(kCFAllocatorDefault, identifier, kCFStringEncodingUTF8);
    CGImageDestinationRef destination = CGImageDestinationCreateWithData(encoded, type, 1, nullptr);
    Array<uint8_t> out;
    if (destination != nullptr) {
        // JPEG and HEIC at their best quality, so that the decoders' differences stay small.
        const double quality = 1.0;
        CFNumberRef number = CFNumberCreate(kCFAllocatorDefault, kCFNumberDoubleType, &quality);
        const void* keys[] = {kCGImageDestinationLossyCompressionQuality};
        const void* values[] = {number};
        CFDictionaryRef options = CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CGImageDestinationAddImage(destination, image, options);
        if (CGImageDestinationFinalize(destination))
            out.append(CFDataGetBytePtr(encoded), uint32_t(CFDataGetLength(encoded)));
        CFRelease(options);
        CFRelease(number);
        CFRelease(destination);
    }
    CFRelease(type);
    CFRelease(encoded);
    CGImageRelease(image);
    CGColorSpaceRelease(space);
    CGDataProviderRelease(provider);
    CFRelease(data);
    return out;
}

// A set of one codec.
struct OneCodec {
    PrImageCodecSet* PR_NULLABLE set = nullptr;

    explicit OneCodec(PrImageCodec* PR_NULLABLE codec) {
        if (codec != nullptr)
            set = prImageCodecSetCreate(&codec, 1);
    }
    ~OneCodec() { prImageCodecSetDestroy(set); }
    OneCodec(const OneCodec&) = delete;
    OneCodec& operator=(const OneCodec&) = delete;
};

const PrCompressCodecSet* compress() {
    static PrCompressCodecSet* set = prCompressCodecSetCreateDefault();
    return set;
}

// Decodes the first frame of `bytes` through `set` to straight-alpha RGBA8, and writes the image's info to `*info`.
// Returns an empty array if the image does not decode.
Array<uint8_t> decode(const PrImageCodecSet* set, const Array<uint8_t>& bytes, PrImageInfo* info = nullptr) {
    PrImageDecoder* decoder = prImageDecoderCreate(set);
    PrImageSource source{};
    source.memory = PrByteView{bytes.data(), bytes.count()};
    Array<uint8_t> out;
    if (prImageDecoderOpen(decoder, source, nullptr) == PrStatus_Ok) {
        const PrImageInfo& opened = *prImageDecoderInfo(decoder);
        if (info != nullptr)
            *info = opened;
        out.resize(opened.extent.x * opened.extent.y * 4);
        const PrPixels pixels{.data = out.data(), .stride = opened.extent.x * 4, .extent = opened.extent};
        if (prImageDecoderReadRgba8(decoder, 0, PrImageAlpha_Straight, PrImageOrientation_Identity, pixels) !=
                PrStatus_Ok)
            out.clear();
    }
    prImageDecoderDestroy(decoder);
    return out;
}

// Returns the largest difference of one sample of `a` and `b`, which are the same size.
uint32_t largestDifference(const Array<uint8_t>& a, const Array<uint8_t>& b) {
    uint32_t largest = 0;
    for (uint32_t i = 0; i < a.count(); ++i) {
        const uint32_t difference = a[i] > b[i] ? a[i] - b[i] : b[i] - a[i];
        largest = difference > largest ? difference : largest;
    }
    return largest;
}

// Decodes `bytes` of the media type `type` through Prax's codec and ImageIO's, and returns the largest difference of
// their samples.
uint32_t compareDecoders(PrStringView type, const Array<uint8_t>& bytes) {
    OneCodec builtin(prImageCodecCreate(PrImageCodecBackend_Builtin, type, compress()));
    OneCodec imageio(prImageImageIOCreate(type));
    REQUIRE(builtin.set != nullptr);
    REQUIRE(imageio.set != nullptr);
    PrImageInfo builtinInfo{};
    PrImageInfo imageioInfo{};
    const Array<uint8_t> a = decode(builtin.set, bytes, &builtinInfo);
    const Array<uint8_t> b = decode(imageio.set, bytes, &imageioInfo);
    REQUIRE(a.count() == kWidth * kHeight * 4);
    REQUIRE(b.count() == a.count());
    CHECK(imageioInfo.extent.x == builtinInfo.extent.x);
    CHECK(imageioInfo.extent.y == builtinInfo.extent.y);
    CHECK(imageioInfo.frameCount == builtinInfo.frameCount);
    return largestDifference(a, b);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ImageIO and Prax decode the PNGs that Prax encodes to the same pixels") {
    OneCodec png(prImageCodecCreate(PrImageCodecBackend_Builtin, "image/png", compress()));
    const bool alphas[] = {false, true};
    for (bool alpha : alphas) {
        CAPTURE(alpha);
        const Array<uint8_t> pixels = pattern(alpha);
        Array<uint8_t> encoded(1 << 16);
        size_t size = 0;
        const PrPixelView view{.data = pixels.data(), .stride = kWidth * 4, .extent = PrUVec2{kWidth, kHeight}};
        REQUIRE(prImageCodecSetEncode(png.set, "image/png", view, encoded.data(), encoded.count(), &size) ==
            PrStatus_Ok);
        encoded.resize(uint32_t(size));
        // Alpha of at least 128 loses at most one step through ImageIO's premultiplied pixels.
        CHECK(compareDecoders("image/png", encoded) <= (alpha ? 1u : 0u));
    }
}

TEST_CASE("ImageIO and Prax decode the GIF and TIFF that ImageIO encodes to the same pixels") {
    const Array<uint8_t> opaque = pattern(false);
    const Array<uint8_t> gif = imageioEncode("com.compuserve.gif", opaque, false);
    REQUIRE(!gif.isEmpty());
    CHECK(compareDecoders("image/gif", gif) == 0);
    const Array<uint8_t> tiff = imageioEncode("public.tiff", opaque, false);
    REQUIRE(!tiff.isEmpty());
    CHECK(compareDecoders("image/tiff", tiff) == 0);
}

TEST_CASE("ImageIO and Prax decode the JPEG that ImageIO encodes to nearly the same pixels") {
    const Array<uint8_t> jpeg = imageioEncode("public.jpeg", pattern(false), false);
    REQUIRE(!jpeg.isEmpty());
    // The decoders' inverse transforms and chroma upsampling may differ by a few steps.
    CHECK(compareDecoders("image/jpeg", jpeg) <= 8);
}

TEST_CASE("ImageIO decodes a format that Prax does not have") {
    OneCodec heic(prImageImageIOCreate("image/heic"));
    if (heic.set == nullptr)
        return;
    const Array<uint8_t> encoded = imageioEncode("public.heic", pattern(false), false);
    if (encoded.isEmpty())
        return;
    PrImageInfo info{};
    const Array<uint8_t> decoded = decode(heic.set, encoded, &info);
    REQUIRE(decoded.count() == kWidth * kHeight * 4);
    CHECK(info.extent.x == kWidth);
    CHECK(info.extent.y == kHeight);
    // HEIC is lossy, so the pattern comes back only close.
    CHECK(largestDifference(decoded, pattern(false)) <= 24);
}

TEST_CASE("an ImageIO codec recognizes only its own format, and refuses types that ImageIO does not decode") {
    CHECK(prImageImageIOCreate("image/x-emjc") == nullptr);
    CHECK(prImageImageIOCreate("text/plain") == nullptr);
    OneCodec jpeg(prImageImageIOCreate("image/jpeg"));
    REQUIRE(jpeg.set != nullptr);
    CHECK(prImageCodecSetSupportsType(jpeg.set, "image/jpeg"));
    CHECK_FALSE(prImageCodecSetSupportsType(jpeg.set, "image/png"));
    const Array<uint8_t> tiff = imageioEncode("public.tiff", pattern(false), false);
    CHECK(decode(jpeg.set, tiff).isEmpty());
}

TEST_CASE("the environment chooses ImageIO for a format") {
    const PrBackendInterface* png = prImageCodecSelectInterface("image/png");
    REQUIRE(png != nullptr);
    CHECK(png->name == "image_png");
    CHECK(prBackendIsBuilt(png, PrImageCodecBackend_ImageIO));
    CHECK_FALSE(prBackendIsBuilt(prImageCodecSelectInterface("image/x-emjc"), PrImageCodecBackend_ImageIO));
    CHECK(prImageCodecSelectInterface("image/heic") == nullptr);
    REQUIRE(prEnvironmentSetVariable("PR_BACKEND_IMAGE_PNG", "imageio"));
    PrImageCodecSet* set = prImageCodecSetCreateDefault(compress());
    REQUIRE(set != nullptr);
    // ImageIO's PNG codec encodes nothing, so a set that chose it for PNG encodes no PNG.
    uint8_t pixel[4] = {};
    size_t size = 0;
    const PrPixelView view{.data = pixel, .stride = 4, .extent = PrUVec2{1, 1}};
    CHECK(prImageCodecSetEncode(set, "image/png", view, nullptr, 0, &size) == PrStatus_NotSupported);
    prImageCodecSetDestroy(set);
    REQUIRE(prEnvironmentSetVariable("PR_BACKEND_IMAGE_PNG", "nonesuch"));
    CHECK(prImageCodecSetCreateDefault(compress()) == nullptr);
    REQUIRE(prEnvironmentRemoveVariable("PR_BACKEND_IMAGE_PNG"));
}
