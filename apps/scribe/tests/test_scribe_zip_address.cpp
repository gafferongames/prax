// The address handler of ZIP archives: entries by their names, stored and deflated, and the budget of decoding them.

#include "pr/compress_select.h"
#include "pr/string.h"
#include "pr/string_ops.h"

#include "apps/scribe/tests/address_fixture.h"
#include "apps/scribe/zip_address.h"
#include "tools/harness/zip_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace scribe;

namespace {

// A resolver with the handler of ZIP archives, over a codec of its own.
struct ZipAddresses {
    PrCompressCodecSet* codec = prCompressCodecSetCreateDefault();
    AddressHandler* handlers[1] = {zipAddressHandlerCreate(codec)};
    AddressFixture f;

    explicit ZipAddresses(AddressLimits limits = {}) : f(handlers, limits) {}
    ZipAddresses(const ZipAddresses&) = delete;
    ZipAddresses& operator=(const ZipAddresses&) = delete;
    ~ZipAddresses() {
        addressResolverDestroy(f.resolver);
        f.resolver = nullptr;
        for (AddressHandler* handler : f.handlers)
            addressHandlerDestroy(handler);
        f.handlers.clear();
        prCompressCodecSetDestroy(codec);
    }
};

} // namespace

TEST_CASE("a step into an archive reaches an entry's bytes, stored or deflated") {
    ZipAddresses z;
    ZipFixtureFile files[] = {
        {"readme.txt", "plain bytes"},
        {"dir/data.json", "{\"a\": [1, 1, 1, 1, 1, 1, 1, 1, 1, 1]}", true},
        {"dir/", ""},
        {"dir/main.ts", "let x: number = 1;\n"},
    };
    z.f.set("file:///a.zip", zipFixture(files, z.codec));
    AddressNode* node = nullptr;
    REQUIRE(z.f.resolve("file:///a.zip#readme.txt", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == "plain bytes");
    CHECK(addressNodeName(node) == "readme.txt");
    CHECK(addressNodeFormat(node) == PrMimeFormat_Text);
    // A stored entry is read in place, in the archive's own bytes.
    AddressNode* outer = addressNodeParent(node);
    PrByteView whole = prBlobBytes(addressNodeBytes(outer));
    PrByteView entry = prBlobBytes(addressNodeBytes(node));
    CHECK(entry.data >= whole.data);
    CHECK(entry.data + entry.size <= whole.data + whole.size);
    addressNodeRelease(node);

    REQUIRE(z.f.resolve("file:///a.zip#dir/data.json", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == "{\"a\": [1, 1, 1, 1, 1, 1, 1, 1, 1, 1]}");
    CHECK(addressNodeName(node) == "data.json");
    CHECK(addressNodeFormat(node) == PrMimeFormat_Json);
    addressNodeRelease(node);

    // An entry's name refines its text, as a file's does.
    REQUIRE(z.f.resolve("file:///a.zip#dir/main.ts", &node) == PrStatus_Ok);
    CHECK(addressNodeFormat(node) == PrMimeFormat_TypeScript);
    addressNodeRelease(node);

    // A folder is no entry of its own.
    uint32_t failed = 0;
    CHECK(z.f.status("file:///a.zip#dir/", &failed) == PrStatus_NotFound);
    CHECK(z.f.status("file:///a.zip#missing.txt", &failed) == PrStatus_NotFound);
    CHECK(failed == 0);
}

TEST_CASE("an archive lists its files, and not its folders") {
    ZipAddresses z;
    ZipFixtureFile files[] = {{"dir/", ""}, {"dir/a.txt", "a"}, {"b.png", "\x89PNG"}};
    z.f.set("file:///a.zip", zipFixture(files));
    AddressNode* node = nullptr;
    REQUIRE(z.f.resolve("file:///a.zip", &node) == PrStatus_Ok);
    CHECK(addressNodeFormat(node) == PrMimeFormat_Zip);
    REQUIRE(addressNodeOpen(node) == PrStatus_Ok);
    REQUIRE(addressNodeChildCount(node) == 2);
    CHECK(addressNodeChild(node, 0).step == "dir/a.txt");
    CHECK(addressNodeChild(node, 0).path == "dir/a.txt");
    CHECK(addressNodeChild(node, 1).size == 4);
    addressNodeRelease(node);
}

TEST_CASE("an archive in an archive takes another step") {
    ZipAddresses z;
    ZipFixtureFile inner[] = {{"deep/leaf.txt", "leaf", true}};
    String innerZip = zipFixture(inner, z.codec);
    ZipFixtureFile middle[] = {{"inner.zip", innerZip}};
    String middleZip = zipFixture(middle, z.codec);
    ZipFixtureFile outer[] = {{"middle.zip", middleZip, true}};
    z.f.set("file:///outer.zip", zipFixture(outer, z.codec));
    AddressNode* node = nullptr;
    REQUIRE(z.f.resolve("file:///outer.zip#middle.zip!inner.zip!deep/leaf.txt", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == "leaf");
    addressNodeRelease(node);
}

TEST_CASE("a deflated entry larger than the budget is refused before it is decoded") {
    ZipAddresses z(AddressLimits{.bytes = 64 * 1024});
    // A megabyte of zeros deflates to about a kilobyte.
    String zeros;
    zeros.append(1024 * 1024, '0');
    ZipFixtureFile files[] = {{"bomb.txt", zeros, true}, {"small.txt", "small", true}};
    z.f.set("file:///bomb.zip", zipFixture(files, z.codec));
    uint32_t failed = 0;
    CHECK(z.f.status("file:///bomb.zip#bomb.txt", &failed) == PrStatus_Limit);
    CHECK(failed == 0);
    CHECK(z.f.status("file:///bomb.zip#small.txt") == PrStatus_Ok);

    // An entry that declares a smaller size than it decodes to fails as corrupt, and decodes no more than it declared.
    String archive = zipFixture(files, z.codec);
    // The uncompressed size of the first entry, in its local header and in the central directory.
    uint32_t central = uint32_t(prStringFind(archive, {"PK\x01\x02", 4}));
    uint32_t sizes[] = {22, central + 24};
    for (uint32_t at : sizes) {
        uint32_t size = 100;
        for (uint32_t i = 0; i < 4; ++i)
            archive.replace(at + i, 1, PrStringView{reinterpret_cast<const char*>(&size) + i, 1});
    }
    z.f.set("file:///lie.zip", archive);
    CHECK(z.f.status("file:///lie.zip#bomb.txt", &failed) == PrStatus_InvalidData);
}

TEST_CASE("an entry reads its siblings by relative paths") {
    ZipAddresses z;
    ZipFixtureFile files[] = {{"a/b/scene.txt", "scene"}, {"a/textures/wood.png", "wood"}};
    z.f.set("file:///a.zip", zipFixture(files));
    AddressNode* node = nullptr;
    REQUIRE(z.f.resolve("file:///a.zip#a/b/scene.txt", &node) == PrStatus_Ok);
    PrBlob* sibling = nullptr;
    REQUIRE(addressNodeReadRelative(node, "../textures/wood.png", &sibling) == PrStatus_Ok);
    CHECK(prBlobBytes(sibling).size == 4);
    prBlobRelease(sibling);
    CHECK(addressNodeReadRelative(node, "../../../x", &sibling) == PrStatus_NotFound);
    CHECK(addressNodeReadRelative(node, "missing.png", &sibling) == PrStatus_NotFound);
    addressNodeRelease(node);
}
