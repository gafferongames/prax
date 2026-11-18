// Every bitmap depth, and PNG images, are tested through the image codec (test_image_ico.cpp). These tests cover the
// directory and the decoder's checks.

#include "pr/array.h"
#include "pr/ico.h"

#include "tools/harness/test.h"

extern "C" int prTestIcoCApi(void);

namespace pr_impl {

namespace {

using namespace pr;

void put16(Array<uint8_t>& bytes, uint32_t at, uint16_t value) {
    bytes[at] = uint8_t(value);
    bytes[at + 1] = uint8_t(value >> 8);
}

void put32(Array<uint8_t>& bytes, uint32_t at, uint32_t value) {
    put16(bytes, at, uint16_t(value));
    put16(bytes, at + 2, uint16_t(value >> 16));
}

// An icon of one 2x2, 32-bit bitmap, whose pixels are opaque blue, green, red and white, bottom-up as BMP stores them.
Array<uint8_t> icon() {
    const uint32_t rows = 2 * 8 + 2 * 4;
    Array<uint8_t> bytes(22 + 40 + rows);
    put16(bytes, 2, 1);
    put16(bytes, 4, 1);
    bytes[6] = 2;
    bytes[7] = 2;
    put16(bytes, 10, 1);
    put16(bytes, 12, 32);
    put32(bytes, 14, 40 + rows);
    put32(bytes, 18, 22);
    put32(bytes, 22, 40);
    put32(bytes, 26, 2);
    put32(bytes, 30, 4);        // the bitmap and its mask, each two rows tall
    put16(bytes, 34, 1);
    put16(bytes, 36, 32);
    const uint8_t pixels[] = {
        0, 0, 255, 255, 255, 255, 255, 255,     // the bottom row, in BGRA: red, white
        255, 0, 0, 255, 0, 255, 0, 255,         // the top row: blue, green
    };
    for (uint32_t i = 0; i < sizeof(pixels); ++i)
        bytes[62 + i] = pixels[i];
    return bytes;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("an icon's bitmap decodes to RGBA, top row first") {
    Array<uint8_t> file = icon();
    CHECK(prIcoHasSignature(file));
    PrIcoDecoder* decoder = nullptr;
    REQUIRE(prIcoDecoderCreate(file, nullptr, file.count(), &decoder) == PrStatus_Ok);
    const PrIcoInfo* info = prIcoDecoderInfo(decoder);
    CHECK(info->extent.x == 2);
    CHECK(info->extent.y == 2);
    CHECK(info->png.size == 0);
    uint8_t out[2 * 8] = {};
    REQUIRE(prIcoDecoderRead(decoder, PrPixels{.data = out, .stride = 8, .extent = PrUVec2{2, 2}}) == PrStatus_Ok);
    const uint8_t expected[] = {0, 0, 255, 255, 0, 255, 0, 255, 255, 0, 0, 255, 255, 255, 255, 255};
    for (uint32_t i = 0; i < sizeof(expected); ++i)
        CHECK(out[i] == expected[i]);
    prIcoDecoderDestroy(decoder);
}

TEST_CASE("bytes without a directory are not an icon, and one past the limits is refused") {
    PrIcoDecoder* decoder = nullptr;
    const uint8_t text[] = {0, 0, 2, 0, 1, 0};
    CHECK(prIcoDecoderCreate(text, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    Array<uint8_t> file = icon();
    PrIcoLimits limits = prIcoLimitsDefault();
    limits.pixels = 3;
    CHECK(prIcoDecoderCreate(file, &limits, file.count(), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestIcoCApi() == 0);
}
