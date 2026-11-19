// The address handler of glTF assets: JSON Pointers to images, buffer views and buffers, and to locations.

#include "pr/align.h"
#include "pr/string.h"

#include "apps/scribe/gltf_address.h"
#include "apps/scribe/tests/address_fixture.h"
#include "apps/scribe/zip_address.h"
#include "tools/harness/zip_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace scribe;

namespace {

// PNG's signature, which is all of the image that the tests embed.
constexpr PrStringView kPng = "\x89PNG\r\n\x1a\n";

// An asset with an image in a data: URI, an image beside it, and a node in a scene.
constexpr PrStringView kAsset = R"({"asset":{"version":"2.0"},)"
    R"("images":[{"uri":"data:image/png;base64,iVBORw0KGgo=","name":"logo"},{"uri":"textures/wood%20grain.png"}],)"
    R"("nodes":[{"name":"root"}],"scenes":[{"nodes":[0]}],"extras":{"a/b":{"~":1}}})";

// Returns binary glTF of `json` and the binary chunk `bin` (glTF-2.0/#binary-gltf-layout).
String glb(PrStringView json, PrStringView bin) {
    auto put32 = [](String& out, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i)
            out.append(1, char((value >> (8 * i)) & 0xff));
    };
    uint32_t jsonSize = alignUp(uint32_t(json.size), 4u);
    uint32_t binSize = alignUp(uint32_t(bin.size), 4u);
    String out("glTF");
    put32(out, 2);
    put32(out, 12 + 8 + jsonSize + 8 + binSize);
    put32(out, jsonSize);
    out.append("JSON");
    out.append(json);
    out.append(jsonSize - uint32_t(json.size), ' ');
    put32(out, binSize);
    out.append({"BIN\0", 4});
    out.append(bin);
    out.append(binSize - uint32_t(bin.size), '\0');
    return out;
}

String text(PrByteView bytes) {
    return PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size};
}

} // namespace

TEST_CASE("a JSON Pointer reaches an image's bytes, or a location of the asset") {
    AddressHandler* handlers[] = {gltfAddressHandlerCreate()};
    AddressFixture f(handlers);
    f.set("file:///assets/box.gltf", kAsset);
    f.set("file:///assets/textures/wood%20grain.png", "wood");
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///assets/box.gltf#/images/0", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Resource);
    CHECK(addressNodeText(node) == kPng);
    CHECK(addressNodeName(node) == "logo.png");
    CHECK(addressNodeFormat(node) == PrMimeFormat_Png);
    AddressNode* asset = addressNodeParent(node);
    CHECK(addressNodeFormat(asset) == PrMimeFormat_Gltf);
    REQUIRE(gltfAddressDocument(asset) != nullptr);
    CHECK(prGltfImageCount(gltfAddressDocument(asset)) == 2);
    CHECK(gltfAddressJson(asset) != nullptr);
    // Only the image that the asset embeds is a nested resource of its own.
    REQUIRE(addressNodeChildCount(asset) == 1);
    CHECK(addressNodeChild(asset, 0).step == "/images/0");
    CHECK(addressNodeChild(asset, 0).path == "images/logo.png");
    addressNodeRelease(node);

    // An image that the asset names by URI is read beside it.
    REQUIRE(f.resolve("file:///assets/box.gltf#/images/1", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == "wood");
    CHECK(addressNodeName(node) == "wood%20grain.png");
    addressNodeRelease(node);

    REQUIRE(f.resolve("file:///assets/box.gltf#/nodes/0", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Location);
    addressNodeRelease(node);
    CHECK(f.status("file:///assets/box.gltf#/extras/a~1b/~0") == PrStatus_Ok);
    REQUIRE(f.resolve("file:///assets/box.gltf#/images/0/name", &node) == PrStatus_Ok);
    CHECK(addressNodeKind(node) == AddressNodeKind_Location);
    addressNodeRelease(node);

    uint32_t failed = 0;
    CHECK(f.status("file:///assets/box.gltf#/nodes/1", &failed) == PrStatus_NotFound);
    CHECK(failed == 0);
    CHECK(f.status("file:///assets/box.gltf#nodes") == PrStatus_Syntax);
    CHECK(f.status("file:///assets/box.gltf#/nodes/01") == PrStatus_Syntax);
    CHECK(f.status("file:///assets/box.gltf#/nodes/0!more", &failed) == PrStatus_NotFound);
    CHECK(failed == 1);
}

TEST_CASE("binary glTF's images and buffer views are its binary chunk's bytes") {
    AddressHandler* handlers[] = {gltfAddressHandlerCreate()};
    AddressFixture f(handlers);
    String bin(kPng);
    bin.append("0123");
    String json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":12}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":4}],)"
        R"("images":[{"bufferView":0,"mimeType":"image/png"}]})";
    String file = glb(json, bin);
    f.set("file:///box.glb", file);
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///box.glb#/images/0", &node) == PrStatus_Ok);
    CHECK(addressNodeFormat(addressNodeParent(node)) == PrMimeFormat_Glb);
    CHECK(addressNodeName(node) == "image0.png");
    CHECK(addressNodeFormat(node) == PrMimeFormat_Png);
    // The image is read in place, in the file's binary chunk.
    PrByteView whole = prBlobBytes(addressNodeBytes(addressNodeParent(node)));
    CHECK(prBlobBytes(addressNodeBytes(node)).data == whole.data + 20 + alignUp(json.count(), 4u) + 8);
    addressNodeRelease(node);
    REQUIRE(f.resolve("file:///box.glb#/bufferViews/1", &node) == PrStatus_Ok);
    CHECK(text(prBlobBytes(addressNodeBytes(node))) == "0123");
    addressNodeRelease(node);
    REQUIRE(f.resolve("file:///box.glb#/buffers/0", &node) == PrStatus_Ok);
    CHECK(prBlobBytes(addressNodeBytes(node)).size == 12);
    AddressNode* asset = addressNodeParent(node);
    REQUIRE(addressNodeChildCount(asset) == 1);
    CHECK(addressNodeChild(asset, 0).path == "images/image0.png");
    addressNodeRelease(node);
}

TEST_CASE("an asset in an archive reads its images from the archive") {
    AddressHandler* handlers[] = {zipAddressHandlerCreate(nullptr), gltfAddressHandlerCreate()};
    AddressFixture f(handlers);
    ZipFixtureFile files[] = {{"Box/glTF/box.gltf", kAsset}, {"Box/glTF/textures/wood grain.png", kPng}};
    f.set("file:///models.zip", zipFixture(files));
    AddressNode* node = nullptr;
    REQUIRE(f.resolve("file:///models.zip#Box/glTF/box.gltf!/images/1", &node) == PrStatus_Ok);
    CHECK(addressNodeText(node) == kPng);
    CHECK(addressNodeFormat(node) == PrMimeFormat_Png);
    addressNodeRelease(node);
    // A binary asset in the archive, whose image is in its binary chunk.
    String json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":8}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"images":[{"bufferView":0,"mimeType":"image/png"}]})";
    String binary = glb(json, kPng);
    ZipFixtureFile packed[] = {{"Box/glTF-Binary/Box.glb", binary}};
    f.set("file:///binary.zip", zipFixture(packed));
    REQUIRE(f.resolve("file:///binary.zip#Box/glTF-Binary/Box.glb!/images/0", &node) == PrStatus_Ok);
    CHECK(addressNodeFormat(node) == PrMimeFormat_Png);
    CHECK(addressText(addressNodeAddress(node)) == "file:///binary.zip#Box/glTF-Binary/Box.glb!/images/0");
    addressNodeRelease(node);
}
