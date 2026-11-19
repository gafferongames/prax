// Studio's image view.

#include "pr/array.h"
#include "pr/image_view.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

extern "C" int prTestImageViewCApi(void);

namespace pr_impl {

namespace {

// A GIF of two frames of one pixel each, a tenth of a second apart.
constexpr uint8_t kAnimation[] = {
    'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff,
    0x21, 0xf9, 4, 0, 10, 0, 0, 0, 0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0,
    0x21, 0xf9, 4, 0, 10, 0, 0, 0, 0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0,
    0x3b,
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public header compiles as C") {
    CHECK(prTestImageViewCApi() == 0);
}

TEST_CASE("an image view shows an image, and plays an animation") {
    KitFixture f;
    PrImageViewDesc desc{.kit = f.kit, .codec = f.images};
    PrImageView* view = prImageViewCreate(&desc);
    f.build = [&](PrLayoutNode root) { prImageViewFrame(view, root); };
    f.frame();
    CHECK(prImageViewInfo(view) == nullptr);

    uint8_t pixels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    PrPixelView still{pixels, 8, {2, 2}};
    size_t size = 0;
    REQUIRE(prImageCodecSetEncode(f.images, "image/png", still, nullptr, 0, &size) == PrStatus_Ok);
    Array<uint8_t> png{uint32_t(size)};
    REQUIRE(prImageCodecSetEncode(f.images, "image/png", still, png.data(), png.count(), &size) == PrStatus_Ok);
    PrBlob* bytes = prBlobCopy(PrByteView{png.data(), size});
    REQUIRE(prImageViewSetBytes(view, bytes) == PrStatus_Ok);
    prBlobRelease(bytes);
    f.frame();
    REQUIRE(prImageViewInfo(view) != nullptr);
    CHECK(prImageViewInfo(view)->extent.x == 2);
    CHECK(f.paintedCommands() >= 1);

    bytes = prBlobCopy(PrByteView{kAnimation, sizeof(kAnimation)});
    REQUIRE(prImageViewSetBytes(view, bytes) == PrStatus_Ok);
    prBlobRelease(bytes);
    f.frame();
    CHECK(prImageViewInfo(view)->frameCount == 2);
    prImageViewShow(view, 1, 0);
    CHECK(prImageViewShownFrame(view) == 1);
    prImageViewShow(view, 7, 3);
    CHECK(prImageViewShownFrame(view) == 1);
    CHECK(prImageViewShownLevel(view) == 0);
    f.frame();

    // Bytes that are no image say so in the view.
    bytes = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>("nothing"), 7});
    CHECK(prImageViewSetBytes(view, bytes) != PrStatus_Ok);
    prBlobRelease(bytes);
    f.frame();
    CHECK(prImageViewInfo(view) == nullptr);
    prImageViewDestroy(view);
}
