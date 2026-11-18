// The decode of every filter and of the residuals is tested through the image codec (test_image_emjc.cpp), which writes
// whole images. These tests cover the header and the decoder's checks.

#include "pr/compress_lzfse.h"
#include "pr/emjc.h"

#include "tools/harness/test.h"

extern "C" int prTestEmjcCApi(void);

namespace pr_impl {

namespace {

// The header of a 3x2 image without an appendix, followed by no planes.
constexpr uint8_t kHeaderOnly[] = {'e', 'm', 'j', '1', 0, 0, 0, 0, 3, 0, 2, 0, 0, 0, 0, 0};

// A set of the LZFSE codec alone, which is all that emjc needs.
struct Compress {
    PrCompressCodecSet* set;

    Compress() {
        PrCompressCodec* lzfse = prCompressLzfseCreate();
        set = prCompressCodecSetCreate(&lzfse, 1);
    }
    ~Compress() { prCompressCodecSetDestroy(set); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a header gives the extent, and planes that do not decompress do not decode") {
    Compress compress;
    CHECK(prEmjcHasSignature(kHeaderOnly));
    PrEmjcDecoder* decoder = nullptr;
    REQUIRE(prEmjcDecoderCreate(kHeaderOnly, compress.set, nullptr, sizeof(kHeaderOnly), &decoder) == PrStatus_Ok);
    CHECK(prEmjcDecoderInfo(decoder)->extent.x == 3);
    CHECK(prEmjcDecoderInfo(decoder)->extent.y == 2);
    uint8_t out[3 * 2 * 4] = {};
    CHECK(prEmjcDecoderRead(decoder, PrPixels{.data = out, .stride = 12, .extent = PrUVec2{3, 2}}) ==
        PrStatus_InvalidData);
    prEmjcDecoderDestroy(decoder);
}

TEST_CASE("an image is refused without the signature, without LZFSE, and past the limits") {
    Compress compress;
    PrEmjcDecoder* decoder = nullptr;
    const uint8_t text[] = {'e', 'm', 'j', '0'};
    CHECK(prEmjcDecoderCreate(text, compress.set, nullptr, sizeof(text), &decoder) == PrStatus_NotSupported);
    PrCompressCodecSet* none = prCompressCodecSetCreate(nullptr, 0);
    CHECK(prEmjcDecoderCreate(kHeaderOnly, none, nullptr, sizeof(kHeaderOnly), &decoder) == PrStatus_NotSupported);
    prCompressCodecSetDestroy(none);
    PrEmjcLimits limits = prEmjcLimitsDefault();
    limits.pixels = 5;
    CHECK(prEmjcDecoderCreate(kHeaderOnly, compress.set, &limits, sizeof(kHeaderOnly), &decoder) == PrStatus_Limit);
    CHECK(decoder == nullptr);
}

TEST_CASE("the public header compiles as C") {
    CHECK(prTestEmjcCApi() == 0);
}
