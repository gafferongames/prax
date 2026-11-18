#include "pr/array.h"
#include "pr/compress_deflate.h"
#include "pr/mem.h"
#include "pr/png.h"

#include "tools/harness/test.h"

extern "C" int prTestPngCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// A set of the Deflate codec alone, which is all that PNG needs.
struct Compress {
    PrCompressCodecSet* set;

    Compress() {
        PrCompressCodec* deflate = prCompressDeflateCreate();
        set = prCompressCodecSetCreate(&deflate, 1);
    }
    ~Compress() { prCompressCodecSetDestroy(set); }
};

// Encodes `width` by `height` pixels of RGBA whose bytes count up from `seed`.
Array<uint8_t> encode(const Compress& compress, uint32_t width, uint32_t height, uint8_t seed, Array<uint8_t>* pixels) {
    pixels->resize(width * height * 4);
    for (uint32_t i = 0; i < pixels->count(); ++i)
        (*pixels)[i] = uint8_t(seed + i);
    PrPixelView view{.data = pixels->data(), .stride = size_t(width) * 4, .extent = PrUVec2{width, height}};
    Array<uint8_t> out(uint32_t(prPngEncode(view, compress.set, nullptr, 0)));
    CHECK(prPngEncode(view, compress.set, out.data(), out.count()) == out.count());
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an encoded image decodes to its pixels, as 8-bit RGBA with alpha") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 5, 3, 7, &pixels);
    CHECK(prPngHasSignature(file));
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreate(file, compress.set, nullptr, file.count(), &decoder) == PrStatus_Ok);
    const PrPngInfo* info = prPngDecoderInfo(decoder);
    CHECK(info->extent.x == 5);
    CHECK(info->extent.y == 3);
    CHECK(info->channels == 4);
    CHECK(info->sampleBytes == 1);
    CHECK(info->alpha);
    CHECK(info->frameCount == 1);
    CHECK(info->playCount == 1);
    CHECK_FALSE(info->srgb);
    CHECK_FALSE(info->hasCicp);
    CHECK(info->profile.size == 0);
    CHECK(info->exif.size == 0);
    CHECK(info->workingBytes >= file.count());
    CHECK(prPngDecoderFrameDuration(decoder, 0) == 0);
    // Rows of a wider stride keep the bytes between them.
    Array<uint8_t> out(size_t(24) * 3);
    prMemFill(out.data(), 0xee, out.count());
    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{.data = out.data(), .stride = 24, .extent = PrUVec2{5, 3}}) ==
        PrStatus_Ok);
    for (uint32_t row = 0; row < 3; ++row) {
        CHECK(prMemEquals(out.data() + row * 24, pixels.data() + row * 20, 20));
        CHECK(out[row * 24 + 20] == 0xee);
    }
    prPngDecoderDestroy(decoder);
}

TEST_CASE("bytes without the signature are not a PNG, and a file cut short is truncated") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 4, 4, 1, &pixels);
    PrPngDecoder* decoder = nullptr;
    const uint8_t text[] = {'n', 'o', 't', ' ', 'a', ' ', 'p', 'n', 'g'};
    CHECK(prPngDecoderCreate(text, compress.set, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    CHECK(decoder == nullptr);
    // The file without its IEND chunk ends before its image data is known to end.
    PrByteView cut{file.data(), file.count() - 12};
    REQUIRE(prPngDecoderCreate(cut, compress.set, nullptr, cut.size, &decoder) == PrStatus_Truncated);
    CHECK(decoder != nullptr);
    prPngDecoderDestroy(decoder);
}

TEST_CASE("an image past the limits is refused") {
    Compress compress;
    Array<uint8_t> pixels;
    Array<uint8_t> file = encode(compress, 8, 8, 3, &pixels);
    PrPngLimits limits = prPngLimitsDefault();
    limits.pixels = 63;
    PrPngDecoder* decoder = nullptr;
    CHECK(prPngDecoderCreate(file, compress.set, &limits, file.count(), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
    limits = prPngLimitsDefault();
    limits.workingBytes = file.count();
    CHECK(prPngDecoderCreate(file, compress.set, &limits, file.count(), &decoder) == PrStatus_Limit);
}

TEST_CASE("a set without zlib stores the image data, which still decodes") {
    Compress compress;
    Array<uint8_t> pixels(size_t(2) * 2 * 4);
    prMemFill(pixels.data(), 9, pixels.count());
    PrCompressCodecSet* none = prCompressCodecSetCreate(nullptr, 0);
    PrPixelView view{.data = pixels.data(), .stride = 8, .extent = PrUVec2{2, 2}};
    Array<uint8_t> stored(uint32_t(prPngEncode(view, none, nullptr, 0)));
    prPngEncode(view, none, stored.data(), stored.count());
    prCompressCodecSetDestroy(none);
    PrPngDecoder* decoder = nullptr;
    REQUIRE(prPngDecoderCreate(stored, compress.set, nullptr, stored.count(), &decoder) == PrStatus_Ok);
    Array<uint8_t> out(pixels.count());
    REQUIRE(prPngDecoderRead(decoder, 0, PrPixels{.data = out.data(), .stride = 8, .extent = PrUVec2{2, 2}}) ==
        PrStatus_Ok);
    CHECK(out == pixels);
    prPngDecoderDestroy(decoder);
}

TEST_CASE("the public headers compile as C") {
    CHECK(prTestPngCApi() == 0);
}
