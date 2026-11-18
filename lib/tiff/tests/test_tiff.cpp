#include "pr/array.h"
#include "pr/mem.h"
#include "pr/tiff.h"

#include "tools/harness/test.h"

extern "C" int prTestTiffCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

// Appends a little-endian 16- or 32-bit value.
void put16(Array<uint8_t>& out, uint32_t value) {
    out.push(uint8_t(value));
    out.push(uint8_t(value >> 8));
}

void put32(Array<uint8_t>& out, uint32_t value) {
    put16(out, value & 0xffff);
    put16(out, value >> 16);
}

// Appends a directory entry of one SHORT or LONG value.
void entry(Array<uint8_t>& out, uint32_t tag, uint32_t type, uint32_t value) {
    put16(out, tag);
    put16(out, type);
    put32(out, 1);
    put32(out, value);
}

// A little-endian TIFF of `width` by `height` RGB pixels, uncompressed in one strip, whose bytes count up from one.
Array<uint8_t> rgbFile(uint32_t width, uint32_t height) {
    Array<uint8_t> out;
    out.push('I');
    out.push('I');
    put16(out, 42);
    put32(out, 8);
    constexpr uint32_t kEntries = 8;
    const uint32_t pixels = 8 + 2 + kEntries * 12 + 4;
    put16(out, kEntries);
    entry(out, 256, 4, width);              // ImageWidth
    entry(out, 257, 4, height);             // ImageLength
    entry(out, 259, 3, 1);                  // Compression: none
    entry(out, 262, 3, 2);                  // PhotometricInterpretation: RGB
    entry(out, 273, 4, pixels);             // StripOffsets
    entry(out, 277, 3, 3);                  // SamplesPerPixel
    entry(out, 278, 4, height);             // RowsPerStrip
    entry(out, 279, 4, width * height * 3); // StripByteCounts
    put32(out, 0);
    for (uint32_t i = 0; i < width * height * 3; ++i)
        out.push(uint8_t(i + 1));
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an uncompressed RGB image decodes to its strip's samples") {
    Array<uint8_t> file = rgbFile(3, 2);
    CHECK(prTiffHasSignature(file));
    PrTiffDecoder* decoder = nullptr;
    REQUIRE(prTiffDecoderCreate(file, nullptr, file.count(), &decoder) == PrStatus_Ok);
    const PrTiffInfo* info = prTiffDecoderInfo(decoder);
    CHECK(info->extent.x == 3);
    CHECK(info->extent.y == 2);
    CHECK(info->channels == 3);
    CHECK_FALSE(info->gray);
    CHECK(info->alpha == PrTiffAlpha_None);
    uint8_t out[12 * 2] = {};
    REQUIRE(prTiffDecoderRead(decoder, PrPixels{.data = out, .stride = 12, .extent = PrUVec2{3, 2}}) == PrStatus_Ok);
    for (uint32_t row = 0; row < 2; ++row) {
        for (uint32_t i = 0; i < 9; ++i)
            CHECK(out[row * 12 + i] == uint8_t(row * 9 + i + 1));
    }
    prTiffDecoderDestroy(decoder);
}

TEST_CASE("bytes without a header are not a TIFF, and an image past the limits is refused") {
    PrTiffDecoder* decoder = nullptr;
    const uint8_t text[] = {'I', 'I', 0, 0};
    CHECK(prTiffDecoderCreate(text, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    Array<uint8_t> file = rgbFile(3, 2);
    PrTiffLimits limits = prTiffLimitsDefault();
    limits.pixels = 5;
    CHECK(prTiffDecoderCreate(file, &limits, file.count(), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestTiffCApi() == 0);
}
