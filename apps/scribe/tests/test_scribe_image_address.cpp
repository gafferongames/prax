// The address handler of images: steps that name a frame or a mip level.

#include "pr/array.h"
#include "pr/compress_select.h"
#include "pr/image_select.h"
#include "pr/string.h"

#include "apps/scribe/image_address.h"
#include "apps/scribe/tests/address_fixture.h"

#include "tools/harness/test.h"

using namespace scribe;

namespace {

// A GIF of two frames of one pixel each.
constexpr uint8_t kAnimation[] = {
    'G', 'I', 'F', '8', '9', 'a', 1, 0, 1, 0, 0x80, 0, 0, 0, 0, 0, 0xff, 0xff, 0xff,
    0x21, 0xf9, 4, 0, 10, 0, 0, 0, 0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0,
    0x21, 0xf9, 4, 0, 10, 0, 0, 0, 0x2c, 0, 0, 0, 0, 1, 0, 1, 0, 0, 2, 2, 0x44, 1, 0,
    0x3b,
};

// The image handler over the built-in codec.
struct ImageAddresses {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* codec = prImageCodecSetCreateDefault(compress);
    AddressHandler* handlers[1] = {imageAddressHandlerCreate(codec)};
    AddressFixture f{handlers};

    ImageAddresses() = default;
    ImageAddresses(const ImageAddresses&) = delete;
    ImageAddresses& operator=(const ImageAddresses&) = delete;
    ~ImageAddresses() {
        addressResolverDestroy(f.resolver);
        f.resolver = nullptr;
        for (AddressHandler* handler : f.handlers)
            addressHandlerDestroy(handler);
        f.handlers.clear();
        prImageCodecSetDestroy(codec);
        prCompressCodecSetDestroy(compress);
    }

    // Returns the status of resolving `text`, which names a location if it resolves.
    PrStatus status(PrStringView text) {
        AddressNode* node = nullptr;
        PrStatus result = f.resolve(text, &node);
        if (result == PrStatus_Ok)
            CHECK(addressNodeKind(node) == AddressNodeKind_Location);
        addressNodeRelease(node);
        return result;
    }
};

} // namespace

TEST_CASE("a step names a frame or a level of an image") {
    ImageAddresses i;
    uint8_t pixels[4 * 4] = {255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255, 255, 255, 255, 255};
    size_t size = 0;
    PrPixelView view{pixels, 8, {2, 2}};
    REQUIRE(prImageCodecSetEncode(i.codec, "image/png", view, nullptr, 0, &size) == PrStatus_Ok);
    Array<uint8_t> png{uint32_t(size)};
    REQUIRE(prImageCodecSetEncode(i.codec, "image/png", view, png.data(), png.count(), &size) == PrStatus_Ok);
    i.f.set("file:///still.png", PrStringView{reinterpret_cast<const char*>(png.data()), size});
    CHECK(i.status("file:///still.png#frame=0") == PrStatus_Ok);
    CHECK(i.status("file:///still.png#level=0") == PrStatus_Ok);
    CHECK(i.status("file:///still.png#frame=0&level=0") == PrStatus_Ok);
    CHECK(i.status("file:///still.png#level=0&frame=0") == PrStatus_Ok);
    CHECK(i.status("file:///still.png#frame=1") == PrStatus_NotFound);
    CHECK(i.status("file:///still.png#level=1") == PrStatus_NotFound);
    CHECK(i.status("file:///still.png#frame=") == PrStatus_Syntax);
    CHECK(i.status("file:///still.png#frame=x") == PrStatus_Syntax);
    CHECK(i.status("file:///still.png#frame=0&frame=0") == PrStatus_Syntax);
    CHECK(i.status("file:///still.png#t=10") == PrStatus_Syntax);

    i.f.set("file:///spinner.gif", PrStringView{reinterpret_cast<const char*>(kAnimation), sizeof(kAnimation)});
    CHECK(i.status("file:///spinner.gif#frame=1") == PrStatus_Ok);
    CHECK(i.status("file:///spinner.gif#frame=2") == PrStatus_NotFound);

    // Bytes that are not an image take no step.
    i.f.set("file:///broken.png", {"\x89PNG\r\n\x1a\nbroken", 14});
    CHECK(i.status("file:///broken.png#frame=0") == PrStatus_InvalidData);
}
