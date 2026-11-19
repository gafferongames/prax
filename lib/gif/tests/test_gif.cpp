#include "pr/gif.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

extern "C" int prTestGifCApi(void);

namespace pr_impl {

namespace {

// A 6x4 GIF87a of one frame with a global color table, and its screen as RGBA (gen_gif_vectors.py made it).
constexpr uint8_t kGlobalTable87aBytes[] = {
    0x47, 0x49, 0x46, 0x38, 0x37, 0x61, 0x06, 0x00, 0x04, 0x00, 0xF1, 0x00, 0x00, 0x00, 0x00,
    0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x2C, 0x00, 0x00, 0x00, 0x00,
    0x06, 0x00, 0x04, 0x00, 0x00, 0x02, 0x06, 0x44, 0x6C, 0xA7, 0x80, 0xBA, 0x57, 0x00, 0x3B,
};
constexpr uint8_t kGlobalTable87aScreen0[] = {
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF,
    0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0xFF,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a still image decodes to its screen as RGBA") {
    CHECK(prGifHasSignature(kGlobalTable87aBytes));
    PrGifDecoder* decoder = nullptr;
    REQUIRE(prGifDecoderCreate(kGlobalTable87aBytes, nullptr, sizeof(kGlobalTable87aBytes), &decoder) == PrStatus_Ok);
    const PrGifInfo* info = prGifDecoderInfo(decoder);
    CHECK(info->extent.x == 6);
    CHECK(info->extent.y == 4);
    CHECK(info->frameCount == 1);
    CHECK(info->playCount == 1);
    CHECK(prGifDecoderFrameDuration(decoder, 0) == 0);
    uint8_t out[28 * 4] = {};
    REQUIRE(prGifDecoderRead(decoder, 0, PrPixels{.data = out, .stride = 28, .extent = PrUVec2{6, 4}}) == PrStatus_Ok);
    for (uint32_t row = 0; row < 4; ++row)
        CHECK(prMemEquals(out + row * 28, kGlobalTable87aScreen0 + row * 24, 24));
    prGifDecoderDestroy(decoder);
}

TEST_CASE("bytes without the signature are not a GIF, a file without its trailer is truncated, and limits hold") {
    PrGifDecoder* decoder = nullptr;
    const uint8_t text[] = {'G', 'I', 'F', '9', '9', 'z'};
    CHECK(prGifDecoderCreate(text, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    PrByteView cut{kGlobalTable87aBytes, sizeof(kGlobalTable87aBytes) - 1};
    CHECK(prGifDecoderCreate(cut, nullptr, cut.size, &decoder) == PrStatus_Truncated);
    prGifDecoderDestroy(decoder);
    PrGifLimits limits = prGifLimitsDefault();
    limits.pixels = 23;
    decoder = nullptr;
    CHECK(prGifDecoderCreate(kGlobalTable87aBytes, &limits, sizeof(kGlobalTable87aBytes), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestGifCApi() == 0);
}
