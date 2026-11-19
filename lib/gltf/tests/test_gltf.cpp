// Containers, the asset and its extensions, buffers, buffer views and accessors.

#include "pr/align.h"
#include "pr/float.h"

#include "gltf/tests/gltf_fixture.h"

extern "C" int prTestGltfCApi(void);

namespace pr_impl {

namespace {

constexpr uint32_t kGlbMagic = 0x46546c67;
constexpr uint32_t kGlbJson = 0x4e4f534a;
constexpr uint32_t kGlbBinary = 0x004e4942;

void appendU32(Array<uint8_t>* out, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8)
        out->push(uint8_t(value >> shift));
}

// Appends a chunk of binary glTF, padded with `pad` to a multiple of four bytes.
void appendChunk(Array<uint8_t>* out, uint32_t type, PrByteView data, uint8_t pad) {
    uint32_t padded = alignUp(uint32_t(data.size), 4);
    appendU32(out, padded);
    appendU32(out, type);
    out->append(data);
    for (uint32_t i = uint32_t(data.size); i < padded; ++i)
        out->push(pad);
}

// Returns binary glTF with `json` as its JSON chunk and, unless it is null, `binary` as a second chunk of `type`.
Array<uint8_t> glb(PrStringView json, PrByteView binary, uint32_t type = kGlbBinary) {
    Array<uint8_t> chunks;
    appendChunk(&chunks, kGlbJson, PrByteView{reinterpret_cast<const uint8_t*>(json.data), json.size}, ' ');
    if (binary.data != nullptr)
        appendChunk(&chunks, type, binary, 0);
    Array<uint8_t> file;
    appendU32(&file, kGlbMagic);
    appendU32(&file, 2);
    appendU32(&file, 12 + chunks.count());
    file.append(chunks.span());
    return file;
}

// Returns the status of reading the binary glTF `file`.
PrStatus glbStatus(const Array<uint8_t>& file) {
    GltfRead read{PrByteView(file)};
    return read.status;
}

// Reads all of accessor `index` as floats.
Array<float> floatsOf(const PrGltf* gltf, uint32_t index) {
    const PrGltfAccessor& accessor = prGltfAccessors(gltf)[index];
    Array<float> out(accessor.count * prGltfElementTypeComponentCount(accessor.type));
    CHECK(prGltfAccessorReadFloats(gltf, index, 0, accessor.count, out.data()));
    return out;
}

// Reads all of accessor `index` as integers.
Array<uint32_t> integersOf(const PrGltf* gltf, uint32_t index) {
    const PrGltfAccessor& accessor = prGltfAccessors(gltf)[index];
    Array<uint32_t> out(accessor.count * prGltfElementTypeComponentCount(accessor.type));
    CHECK(prGltfAccessorReadIntegers(gltf, index, 0, accessor.count, out.data()));
    return out;
}

// Returns the JSON of an asset over "bin" with one accessor, whose members are `accessor`.
String oneAccessor(PrStringView accessor) {
    String members = R"("accessors":[{)";
    return gltfBinAsset(members.append(accessor).append("}]"));
}

// The buffer of the meshopt tests: at byte 0, a stream of attributes of version 0 that holds one element of four bytes,
// the float 1.5, as its baseline, every delta zero; at byte 40, a stream of three indices, 0, 1 and 2.
GltfBin meshoptBin() {
    GltfBin bin;
    bin.put<uint8_t>(0, 0xa0, 0, 0, 0, 0);
    bin.put<float>(33, 1.5f);
    bin.put<uint8_t>(40, 0xd1, 0, 4, 4, 0, 0, 0, 0);
    return bin;
}

// The extension objects of the meshopt tests' two views, which decode the two streams of meshoptBin.
constexpr PrStringView kMeshoptAttributes = R"("buffer":0,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES")";
constexpr PrStringView kMeshoptIndices =
    R"("buffer":0,"byteOffset":40,"byteLength":8,"byteStride":2,"count":3,"mode":"INDICES")";

// Returns an asset whose two views are compressed by `extension` into "bin", over a fallback buffer of no URI, with a
// float accessor of view 0 and an index accessor of view 1. `attributes` and `indices` are the members of the views'
// extension objects, and `fallback` those of the fallback buffer's.
String meshoptAsset(PrStringView extension, PrStringView attributes = kMeshoptAttributes,
        PrStringView indices = kMeshoptIndices, PrStringView fallback = R"("fallback":true)") {
    String members = R"("extensionsUsed":[")";
    members.append(extension).append(R"("],"extensionsRequired":[")").append(extension).append(R"("],)");
    members.append(R"("buffers":[{"byteLength":256,"uri":"bin"},{"byteLength":64,"extensions":{")");
    members.append(extension).append(R"(":{)").append(fallback).append(R"(}}}],"bufferViews":[)");
    members.append(R"({"buffer":1,"byteLength":4,"extensions":{")").append(extension).append(R"(":{)");
    members.append(attributes).append(R"(}}},{"buffer":1,"byteOffset":8,"byteLength":6,"extensions":{")");
    members.append(extension).append(R"(":{)").append(indices).append(R"(}}}],"accessors":[)");
    members.append(R"({"bufferView":0,"componentType":5126,"count":1,"type":"SCALAR"},)");
    members.append(R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}])");
    return gltfAsset(members);
}

constexpr PrStringView kMeshoptExtensions[] = {"EXT_meshopt_compression", "KHR_meshopt_compression"};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestGltfCApi() == 0);
}

// ---------------------------------------------------------------------------------------------------------------------
// Containers

TEST_CASE("an asset of JSON reads with what it says about itself") {
    GltfRead read(R"({"asset":{"version":"2.0","generator":"by hand","copyright":"none"}})");
    REQUIRE(read.status == PrStatus_Ok);
    PrGltfAsset asset = prGltfAsset(read.gltf);
    CHECK(asset.version == "2.0");
    CHECK(asset.generator == "by hand");
    CHECK(asset.copyright == "none");
    CHECK(asset.minVersion.data == nullptr);
    CHECK(prGltfNodeCount(read.gltf) == 0);
    CHECK(prGltfDefaultScene(read.gltf) == kPrGltfNone);
}

TEST_CASE("text that is not JSON is a syntax error, and JSON that is not an object is invalid") {
    GltfRead text(R"({"asset":)");
    CHECK(text.status == PrStatus_Syntax);
    CHECK(text.gltf == nullptr);
    CHECK(text.diagnostics == "the asset is not JSON");
    GltfRead empty("");
    CHECK(empty.status == PrStatus_Syntax);
    GltfRead chunk{PrByteView(glb("{", PrByteView{nullptr, 0}))};
    CHECK(chunk.status == PrStatus_Syntax);
    GltfRead array("[]");
    CHECK(array.status == PrStatus_InvalidData);
    CHECK(array.diagnostics == "the asset must be a JSON object");
}

TEST_CASE("binary glTF reads its JSON chunk, and its first buffer is the binary chunk") {
    const uint8_t binary[] = {1, 2, 3, 4, 5, 6};
    String json = gltfAsset(R"("buffers":[{"byteLength":6}],"bufferViews":[{"buffer":0,"byteOffset":2,"byteLength":3}])");
    GltfRead read{PrByteView(glb(json, binary))};
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfBuffer& buffer = prGltfBuffers(read.gltf)[0];
    CHECK(buffer.uri.data == nullptr);
    CHECK(buffer.byteLength == 6);
    // The chunk is padded to a multiple of four bytes, and the buffer is its first byteLength bytes.
    REQUIRE(buffer.bytes.size == 8);
    CHECK(buffer.bytes.data[5] == 6);
    PrByteView view = prGltfBufferViewBytes(read.gltf, 0);
    const uint8_t expected[] = {3, 4, 5};
    CHECK(view == PrByteView(expected));
}

TEST_CASE("binary glTF with no binary chunk leaves a buffer without a URI with no bytes") {
    String json = gltfAsset(R"("buffers":[{"byteLength":6}])");
    GltfRead read{PrByteView(glb(json, PrByteView{nullptr, 0}))};
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfBuffers(read.gltf)[0].bytes.data == nullptr);
}

TEST_CASE("a chunk of a type that the specification does not define is ignored") {
    const uint8_t other[] = {1, 2, 3, 4};
    String json = gltfAsset(R"("buffers":[{"byteLength":4}])");
    GltfRead read{PrByteView(glb(json, other, 0x12345678))};
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfBuffers(read.gltf)[0].bytes.data == nullptr);
}

TEST_CASE("a binary chunk shorter than the first buffer is invalid") {
    const uint8_t binary[] = {1, 2, 3, 4};
    String json = gltfAsset(R"("buffers":[{"byteLength":9}])");
    GltfRead read{PrByteView(glb(json, binary))};
    CHECK(read.status == PrStatus_InvalidData);
    CHECK(read.diagnostics == "buffers[0].byteLength is more than the bytes that the buffer has");
}

TEST_CASE("malformed binary glTF is refused") {
    Array<uint8_t> whole = glb(gltfAsset(""), PrByteView{nullptr, 0});
    REQUIRE(glbStatus(whole) == PrStatus_Ok);

    SUBCASE("a header that is cut short") {
        whole.resize(10);
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a container of another version") {
        whole[4] = 1;
        CHECK(glbStatus(whole) == PrStatus_NotSupported);
    }
    SUBCASE("a length past the end of the file") {
        whole[8] = uint8_t(whole[8] + 4);
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a length that holds no chunk") {
        whole[8] = 12;
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a first chunk that is not JSON") {
        whole[16] = 'X';
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a chunk that runs past the end of the file") {
        whole[12] = uint8_t(whole[12] + 4);
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a chunk whose length is not a multiple of four") {
        whole[12] = uint8_t(whole[12] - 1);
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
    SUBCASE("a second chunk that is cut short") {
        appendU32(&whole, 4);
        whole[8] = uint8_t(whole[8] + 4);
        CHECK(glbStatus(whole) == PrStatus_InvalidData);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// The asset and its extensions

TEST_CASE("an asset states a version that the module reads") {
    CHECK(gltfRefusal("{}") == "asset is required");
    CHECK(gltfRefusal(R"({"asset":[]})") == "asset must be an object");
    CHECK(gltfRefusal(R"({"asset":{}})") == "asset.version is required");
    CHECK(gltfRefusal(R"({"asset":{"version":2}})") == "asset.version must be a string");
    CHECK(gltfRefusal(R"({"asset":{"version":"2"}})") == "asset.version is not a version");
    CHECK(gltfRefusal(R"({"asset":{"version":"2.x"}})") == "asset.version is not a version");
    CHECK(gltfRefusal(R"({"asset":{"version":"2.0","minVersion":"two"}})") == "asset.minVersion is not a version");

    // A later minor version reads as glTF 2.0, unless the asset says that it must not.
    GltfRead later(R"({"asset":{"version":"2.7"}})");
    CHECK(later.status == PrStatus_Ok);
    GltfRead minimum(R"({"asset":{"version":"2.7","minVersion":"2.0"}})");
    CHECK(minimum.status == PrStatus_Ok);
    GltfRead needsLater(R"({"asset":{"version":"2.7","minVersion":"2.1"}})");
    CHECK(needsLater.status == PrStatus_NotSupported);
    CHECK(needsLater.diagnostics == "asset.minVersion is later than glTF 2.0");
    GltfRead first(R"({"asset":{"version":"1.0"}})");
    CHECK(first.status == PrStatus_NotSupported);
    CHECK(first.diagnostics == "asset.version is not glTF 2");
}

TEST_CASE("the extensions that an asset uses are listed, and one that it requires must be read") {
    CHECK(prGltfExtensionIsSupported("KHR_lights_punctual"));
    CHECK(prGltfExtensionIsSupported("KHR_materials_emissive_strength"));
    CHECK(prGltfExtensionIsSupported("KHR_materials_unlit"));
    CHECK(prGltfExtensionIsSupported("KHR_texture_transform"));
    CHECK(!prGltfExtensionIsSupported("KHR_gaussian_splatting"));
    CHECK(!prGltfExtensionIsSupported(""));

    GltfRead read(gltfAsset(R"("extensionsUsed":["KHR_materials_unlit","VENDOR_other"],)"
        R"("extensionsRequired":["KHR_materials_unlit"])"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfUsedExtensionCount(read.gltf) == 2);
    CHECK(prGltfUsedExtension(read.gltf, 0) == "KHR_materials_unlit");
    CHECK(prGltfUsedExtension(read.gltf, 1) == "VENDOR_other");

    GltfRead required(gltfAsset(R"("extensionsUsed":["VENDOR_other"],"extensionsRequired":["VENDOR_other"])"));
    CHECK(required.status == PrStatus_NotSupported);
    CHECK(required.gltf == nullptr);
    CHECK(required.diagnostics == "VENDOR_other is a required extension that the module does not read");

    CHECK(gltfRefusal(gltfAsset(R"("extensionsUsed":[1])")) == "extensionsUsed must hold strings");
    CHECK(gltfRefusal(gltfAsset(R"("extensionsRequired":[1])")) == "extensionsRequired must hold strings");
    CHECK(gltfRefusal(gltfAsset(R"("extensionsUsed":{})")) == "extensionsUsed must be an array");
}

// ---------------------------------------------------------------------------------------------------------------------
// Buffers and buffer views

TEST_CASE("a data: URI gives a buffer its bytes, in base64 or percent-encoded") {
    GltfRead read(gltfAsset(R"("buffers":[)"
        R"({"byteLength":8,"uri":"data:application/gltf-buffer;base64,AAECAwQFBgc=","name":"embedded"},)"
        R"({"byteLength":3,"uri":"data:application/octet-stream,a%00%ff"}])"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfBuffer* buffers = prGltfBuffers(read.gltf);
    const uint8_t counted[] = {0, 1, 2, 3, 4, 5, 6, 7};
    CHECK(buffers[0].bytes == PrByteView(counted));
    CHECK(buffers[0].name == "embedded");
    // A data: URI is not kept, since its bytes are.
    CHECK(buffers[0].uri == "");
    CHECK(buffers[0].uri.data != nullptr);
    const uint8_t escaped[] = {'a', 0, 0xff};
    CHECK(buffers[1].bytes == PrByteView(escaped));
}

TEST_CASE("the host loads a buffer that a URI names, and without it the buffer has no bytes") {
    struct Asked {
        uint32_t calls = 0;
        PrGltfExternal kind = PrGltfExternal_Image;
        String uri;
    };
    PrGltfLoadFn load = [](void* context, PrGltfExternal kind, PrStringView uri) -> PrBlob* {
        Asked* asked = static_cast<Asked*>(context);
        ++asked->calls;
        asked->kind = kind;
        asked->uri = uri;
        const uint8_t bytes[] = {9, 8, 7, 6, 5};
        return prBlobCopy(bytes);
    };
    String json = gltfAsset(R"("buffers":[{"byteLength":4,"uri":"dir/a%20b.bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":4}],)"
        R"("accessors":[{"bufferView":0,"componentType":5121,"count":4,"type":"SCALAR"}])");
    PrBlob* file = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(json.view().data), json.count()});

    Asked asked;
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = load;
    options.loadContext = &asked;
    PrGltf* loaded = nullptr;
    REQUIRE(prGltfCreate(file, &options, &loaded) == PrStatus_Ok);
    CHECK(asked.calls == 1);
    CHECK(asked.kind == PrGltfExternal_Buffer);
    // The URI is the asset's own text, which the host resolves.
    CHECK(asked.uri == "dir/a%20b.bin");
    CHECK(prGltfBuffers(loaded)[0].uri == "dir/a%20b.bin");
    CHECK(prGltfBuffers(loaded)[0].bytes.size == 5);
    uint32_t values[4] = {};
    CHECK(prGltfAccessorReadIntegers(loaded, 0, 0, 4, values));
    CHECK(values[3] == 6);
    prGltfDestroy(loaded);

    // With no loader the document still describes the asset, and reading the accessor fails with zeros.
    PrGltf* described = nullptr;
    REQUIRE(prGltfCreate(file, nullptr, &described) == PrStatus_Ok);
    CHECK(prGltfBuffers(described)[0].bytes.data == nullptr);
    CHECK(prGltfBuffers(described)[0].byteLength == 4);
    CHECK(prGltfBufferViewBytes(described, 0).data == nullptr);
    values[3] = 77;
    float floats[4] = {1, 1, 1, 1};
    CHECK(!prGltfAccessorReadIntegers(described, 0, 0, 4, values));
    CHECK(!prGltfAccessorReadFloats(described, 0, 0, 4, floats));
    CHECK(values[3] == 0);
    CHECK(floats[0] == 0);
    prGltfDestroy(described);
    prBlobRelease(file);
}

TEST_CASE("a buffer is checked against its bytes") {
    GltfBin bin;
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":257,"uri":"bin"}])"), &bin) ==
        "buffers[0].byteLength is more than the bytes that the buffer has");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":9,"uri":"data:;base64,AAECAwQFBgc="}])")) ==
        "buffers[0].byteLength is more than the bytes that the buffer has");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":1,"uri":"data:;base64,A"}])")) ==
        "buffers[0].uri is a malformed data: URI");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":1,"uri":"data:nocomma"}])")) ==
        "buffers[0].uri is a malformed data: URI");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"uri":"bin"}])"), &bin) == "buffers[0].byteLength must be at least 1");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":0,"uri":"bin"}])"), &bin) ==
        "buffers[0].byteLength must be at least 1");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":1.5,"uri":"bin"}])"), &bin) ==
        "buffers[0].byteLength must be a non-negative integer");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":4,"uri":7}])")) == "buffers[0].uri must be a string");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[7])")) == "buffers[0] must be an object");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":{})")) == "buffers must be an array");
}

TEST_CASE("a buffer view is a range of its buffer") {
    GltfBin bin;
    GltfRead read(gltfAsset(R"("buffers":[{"byteLength":256,"uri":"bin"}],"bufferViews":[)"
        R"({"buffer":0,"byteOffset":8,"byteLength":248,"byteStride":12,"target":34962,"name":"vertices"},)"
        R"({"buffer":0,"byteLength":6,"target":34963}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfBufferView* views = prGltfBufferViews(read.gltf);
    REQUIRE(prGltfBufferViewCount(read.gltf) == 2);
    CHECK(views[0].name == "vertices");
    CHECK(views[0].byteOffset == 8);
    CHECK(views[0].byteLength == 248);
    CHECK(views[0].byteStride == 12);
    CHECK(views[0].target == PrGltfBufferTarget_Vertices);
    CHECK(views[1].byteOffset == 0);
    CHECK(views[1].byteStride == 0);
    CHECK(views[1].target == PrGltfBufferTarget_Indices);
    CHECK(prGltfBufferViewBytes(read.gltf, 0).size == 248);

    const char* buffers = R"("buffers":[{"byteLength":256,"uri":"bin"}],"bufferViews":[{)";
    auto refusal = [&](PrStringView view) {
        String members = buffers;
        return gltfRefusal(gltfAsset(members.append(view).append("}]")), &bin);
    };
    CHECK(refusal(R"("byteLength":4)") == "bufferViews[0].buffer is required");
    CHECK(refusal(R"("buffer":1,"byteLength":4)") == "bufferViews[0].buffer is not the index of an object");
    CHECK(refusal(R"("buffer":-1,"byteLength":4)") == "bufferViews[0].buffer must be a non-negative integer");
    CHECK(refusal(R"("buffer":0)") == "bufferViews[0].byteLength must be at least 1");
    CHECK(refusal(R"("buffer":0,"byteLength":257)") == "bufferViews[0].byteLength runs past the end of the buffer");
    CHECK(refusal(R"("buffer":0,"byteOffset":255,"byteLength":2)") ==
        "bufferViews[0].byteLength runs past the end of the buffer");
    CHECK(refusal(R"("buffer":0,"byteOffset":300,"byteLength":1)") ==
        "bufferViews[0].byteLength runs past the end of the buffer");
    CHECK(refusal(R"("buffer":0,"byteLength":8,"byteStride":2)") ==
        "bufferViews[0].byteStride must be a multiple of 4 from 4 to 252");
    CHECK(refusal(R"("buffer":0,"byteLength":8,"byteStride":6)") ==
        "bufferViews[0].byteStride must be a multiple of 4 from 4 to 252");
    CHECK(refusal(R"("buffer":0,"byteLength":8,"byteStride":256)") ==
        "bufferViews[0].byteStride must be a multiple of 4 from 4 to 252");
    CHECK(refusal(R"("buffer":0,"byteLength":8,"byteStride":12)") == "bufferViews[0].byteStride is more than the byteLength");
    CHECK(refusal(R"("buffer":0,"byteLength":8,"target":1)") == "bufferViews[0].target is not a value of its enumeration");
}

// ---------------------------------------------------------------------------------------------------------------------
// Accessors

TEST_CASE("an element's size counts the padding of a matrix's columns") {
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_Byte) == 1);
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_UnsignedByte) == 1);
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_Short) == 2);
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_UnsignedShort) == 2);
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_UnsignedInt) == 4);
    CHECK(prGltfComponentTypeSize(PrGltfComponentType_Float) == 4);
    CHECK(prGltfElementTypeComponentCount(PrGltfElementType_Scalar) == 1);
    CHECK(prGltfElementTypeComponentCount(PrGltfElementType_Vec3) == 3);
    CHECK(prGltfElementTypeComponentCount(PrGltfElementType_Mat2) == 4);
    CHECK(prGltfElementTypeComponentCount(PrGltfElementType_Mat3) == 9);
    CHECK(prGltfElementTypeComponentCount(PrGltfElementType_Mat4) == 16);

    CHECK(prGltfElementSize(PrGltfElementType_Vec3, PrGltfComponentType_Byte) == 3);
    CHECK(prGltfElementSize(PrGltfElementType_Vec3, PrGltfComponentType_Short) == 6);
    CHECK(prGltfElementSize(PrGltfElementType_Vec4, PrGltfComponentType_Float) == 16);
    CHECK(prGltfElementSize(PrGltfElementType_Mat2, PrGltfComponentType_Byte) == 8);
    CHECK(prGltfElementSize(PrGltfElementType_Mat2, PrGltfComponentType_Short) == 8);
    CHECK(prGltfElementSize(PrGltfElementType_Mat2, PrGltfComponentType_Float) == 16);
    CHECK(prGltfElementSize(PrGltfElementType_Mat3, PrGltfComponentType_Byte) == 12);
    CHECK(prGltfElementSize(PrGltfElementType_Mat3, PrGltfComponentType_Short) == 24);
    CHECK(prGltfElementSize(PrGltfElementType_Mat3, PrGltfComponentType_Float) == 36);
    CHECK(prGltfElementSize(PrGltfElementType_Mat4, PrGltfComponentType_Byte) == 16);
    CHECK(prGltfElementSize(PrGltfElementType_Mat4, PrGltfComponentType_Short) == 32);
    CHECK(prGltfElementSize(PrGltfElementType_Mat4, PrGltfComponentType_Float) == 64);
}

TEST_CASE("components of every type unpack as floats and as integers") {
    GltfBin bin;
    bin.put<int8_t>(0, -128, -1, 127, 5);
    bin.put<uint8_t>(4, 0, 255, 128, 1);
    bin.put<int16_t>(8, -32768, 32767, -1, 100);
    bin.put<uint16_t>(16, 0, 65535, 3, 4);
    bin.put<uint32_t>(24, 0, 4000000000u, 7);
    bin.put<float>(36, 1.5f, -2.25f, 3e9f, 5e9f, kPrF32Nan);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"byteOffset":0,"componentType":5120,"count":4,"type":"SCALAR","name":"bytes"},)"
        R"({"bufferView":0,"byteOffset":4,"componentType":5121,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":8,"componentType":5122,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":16,"componentType":5123,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":24,"componentType":5125,"count":3,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":36,"componentType":5126,"count":5,"type":"SCALAR"}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfAccessorCount(read.gltf) == 6);
    const PrGltfAccessor* accessors = prGltfAccessors(read.gltf);
    CHECK(accessors[0].name == "bytes");
    CHECK(accessors[0].componentType == PrGltfComponentType_Byte);
    CHECK(accessors[3].componentType == PrGltfComponentType_UnsignedShort);
    CHECK(accessors[5].componentType == PrGltfComponentType_Float);
    CHECK(accessors[5].type == PrGltfElementType_Scalar);
    CHECK(accessors[5].count == 5);
    CHECK(accessors[5].byteOffset == 36);
    CHECK(!accessors[5].normalized);
    CHECK(accessors[5].min.count == 0);
    CHECK(accessors[5].sparse.count == 0);
    CHECK(prGltfAccessorStride(read.gltf, 2) == 2);

    const float bytes[] = {-128, -1, 127, 5};
    CHECK(floatsOf(read.gltf, 0) == Array<float>(bytes));
    const float unsignedBytes[] = {0, 255, 128, 1};
    CHECK(floatsOf(read.gltf, 1) == Array<float>(unsignedBytes));
    const float shorts[] = {-32768, 32767, -1, 100};
    CHECK(floatsOf(read.gltf, 2) == Array<float>(shorts));
    const float unsignedShorts[] = {0, 65535, 3, 4};
    CHECK(floatsOf(read.gltf, 3) == Array<float>(unsignedShorts));
    const float unsignedInts[] = {0, 4000000000.0f, 7};
    CHECK(floatsOf(read.gltf, 4) == Array<float>(unsignedInts));
    Array<float> floats = floatsOf(read.gltf, 5);
    CHECK(floats[0] == 1.5f);
    CHECK(floats[1] == -2.25f);
    CHECK(prF32IsNan(floats[4]));

    // A signed integer is in two's complement, and a float is truncated and clamped.
    const uint32_t byteIntegers[] = {0xffffff80u, 0xffffffffu, 127, 5};
    CHECK(integersOf(read.gltf, 0) == Array<uint32_t>(byteIntegers));
    const uint32_t shortIntegers[] = {0xffff8000u, 32767, 0xffffffffu, 100};
    CHECK(integersOf(read.gltf, 2) == Array<uint32_t>(shortIntegers));
    const uint32_t unsignedIntIntegers[] = {0, 4000000000u, 7};
    CHECK(integersOf(read.gltf, 4) == Array<uint32_t>(unsignedIntIntegers));
    const uint32_t floatIntegers[] = {1, 0, 3000000000u, 0xffffffffu, 0};
    CHECK(integersOf(read.gltf, 5) == Array<uint32_t>(floatIntegers));
}

TEST_CASE("normalized integers unpack to [0, 1] or [-1, 1]") {
    GltfBin bin;
    bin.put<int8_t>(0, -128, -127, 127, 0);
    bin.put<uint8_t>(4, 0, 255, 51, 1);
    bin.put<int16_t>(8, -32768, -32767, 32767, 0);
    bin.put<uint16_t>(16, 0, 65535, 13107, 1);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"byteOffset":0,"componentType":5120,"normalized":true,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":4,"componentType":5121,"normalized":true,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":8,"componentType":5122,"normalized":true,"count":4,"type":"SCALAR"},)"
        R"({"bufferView":0,"byteOffset":16,"componentType":5123,"normalized":true,"count":4,"type":"SCALAR"}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfAccessors(read.gltf)[0].normalized);
    // The least value of a signed type stands for -1, as the value above it does.
    const float bytes[] = {-1, -1, 1, 0};
    CHECK(floatsOf(read.gltf, 0) == Array<float>(bytes));
    const float unsignedBytes[] = {0, 1, 51.0f / 255.0f, 1.0f / 255.0f};
    CHECK(floatsOf(read.gltf, 1) == Array<float>(unsignedBytes));
    const float shorts[] = {-1, -1, 1, 0};
    CHECK(floatsOf(read.gltf, 2) == Array<float>(shorts));
    const float unsignedShorts[] = {0, 1, 13107.0f / 65535.0f, 1.0f / 65535.0f};
    CHECK(floatsOf(read.gltf, 3) == Array<float>(unsignedShorts));
    // Normalization does not change the integers.
    const uint32_t integers[] = {0, 255, 51, 1};
    CHECK(integersOf(read.gltf, 1) == Array<uint32_t>(integers));
}

TEST_CASE("elements are a buffer view's stride apart") {
    GltfBin bin;
    // Two vertices of 16 bytes: a position of three floats, then a texture coordinate of two unsigned shorts.
    bin.put<float>(0, 1, 2, 3);
    bin.put<uint16_t>(12, 65535, 0);
    bin.put<float>(16, 4, 5, 6);
    bin.put<uint16_t>(28, 0, 65535);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},)"
        R"({"bufferView":1,"byteOffset":12,"componentType":5123,"normalized":true,"count":2,"type":"VEC2"}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfAccessorStride(read.gltf, 0) == 16);
    const float positions[] = {1, 2, 3, 4, 5, 6};
    CHECK(floatsOf(read.gltf, 0) == Array<float>(positions));
    const float coordinates[] = {1, 0, 0, 1};
    CHECK(floatsOf(read.gltf, 1) == Array<float>(coordinates));

    // A range of an accessor reads from its first element.
    float second[3] = {};
    CHECK(prGltfAccessorReadFloats(read.gltf, 0, 1, 1, second));
    CHECK(second[0] == 4);
    CHECK(second[2] == 6);
    CHECK(prGltfAccessorReadFloats(read.gltf, 0, 2, 0, second));
}

TEST_CASE("the columns of a matrix of small components are padded to four bytes") {
    GltfBin bin;
    bin.put<int8_t>(0, 1, 2, 99, 99, 3, 4, 99, 99);
    bin.put<uint8_t>(8, 1, 2, 3, 99, 4, 5, 6, 99, 7, 8, 9, 99);
    bin.put<int16_t>(20, 1, 2, 3, 99, 4, 5, 6, 99, 7, 8, 9, 99);
    bin.put<int16_t>(44, 1, 2, 3, 4);
    for (uint32_t i = 0; i < 16; ++i)
        bin.put<float>(52 + i * 4, float(i));
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"byteOffset":0,"componentType":5120,"count":1,"type":"MAT2"},)"
        R"({"bufferView":0,"byteOffset":8,"componentType":5121,"count":1,"type":"MAT3"},)"
        R"({"bufferView":0,"byteOffset":20,"componentType":5122,"count":1,"type":"MAT3"},)"
        R"({"bufferView":0,"byteOffset":44,"componentType":5122,"count":1,"type":"MAT2"},)"
        R"({"bufferView":0,"byteOffset":52,"componentType":5126,"count":1,"type":"MAT4"}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const float two[] = {1, 2, 3, 4};
    const float three[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
    CHECK(floatsOf(read.gltf, 0) == Array<float>(two));
    CHECK(floatsOf(read.gltf, 1) == Array<float>(three));
    CHECK(floatsOf(read.gltf, 2) == Array<float>(three));
    CHECK(floatsOf(read.gltf, 3) == Array<float>(two));
    Array<float> four = floatsOf(read.gltf, 4);
    CHECK(four[0] == 0);
    CHECK(four[15] == 15);
}

TEST_CASE("sparse elements replace elements of a buffer view, or of zeros") {
    GltfBin bin;
    bin.put<float>(0, 0, 1, 2, 3, 4);
    bin.put<uint8_t>(32, 1, 3);
    bin.put<float>(40, 10, 30);
    bin.put<uint16_t>(48, 0, 4);
    GltfRead read(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":5,"type":"SCALAR","sparse":{"count":2,)"
        R"("indices":{"bufferView":0,"byteOffset":32,"componentType":5121},)"
        R"("values":{"bufferView":0,"byteOffset":40}}},)"
        R"({"componentType":5126,"count":5,"type":"SCALAR","sparse":{"count":2,)"
        R"("indices":{"bufferView":0,"byteOffset":48,"componentType":5123},)"
        R"("values":{"bufferView":0,"byteOffset":40}}},)"
        R"({"componentType":5123,"count":3,"type":"VEC2"}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfSparse& sparse = prGltfAccessors(read.gltf)[0].sparse;
    CHECK(sparse.count == 2);
    CHECK(sparse.indicesComponentType == PrGltfComponentType_UnsignedByte);
    CHECK(sparse.indicesBufferView == 0);
    CHECK(sparse.indicesByteOffset == 32);
    CHECK(sparse.valuesBufferView == 0);
    CHECK(sparse.valuesByteOffset == 40);

    const float replaced[] = {0, 10, 2, 30, 4};
    CHECK(floatsOf(read.gltf, 0) == Array<float>(replaced));
    // A range finds the sparse elements within it.
    float range[3] = {};
    CHECK(prGltfAccessorReadFloats(read.gltf, 0, 2, 3, range));
    CHECK(range[0] == 2);
    CHECK(range[1] == 30);
    CHECK(range[2] == 4);
    CHECK(prGltfAccessorReadFloats(read.gltf, 0, 3, 1, range));
    CHECK(range[0] == 30);
    CHECK(prGltfAccessorReadFloats(read.gltf, 0, 4, 1, range));
    CHECK(range[0] == 4);

    // An accessor with no buffer view is zeros, sparse or not.
    CHECK(prGltfAccessors(read.gltf)[1].bufferView == kPrGltfNone);
    const float overZeros[] = {10, 0, 0, 0, 30};
    CHECK(floatsOf(read.gltf, 1) == Array<float>(overZeros));
    const uint32_t zeros[] = {0, 0, 0, 0, 0, 0};
    CHECK(integersOf(read.gltf, 2) == Array<uint32_t>(zeros));
}

TEST_CASE("an accessor states its bounds as floats") {
    GltfBin bin;
    GltfRead read(oneAccessor(
        R"("bufferView":0,"componentType":5126,"count":2,"type":"VEC3","min":[-1,0.5,2],"max":[1,2.5,3])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfAccessor& accessor = prGltfAccessors(read.gltf)[0];
    REQUIRE(accessor.min.count == 3);
    REQUIRE(accessor.max.count == 3);
    CHECK(prGltfListFloats(read.gltf, accessor.min)[1] == 0.5f);
    CHECK(prGltfListFloats(read.gltf, accessor.max)[2] == 3);
}

TEST_CASE("an accessor is checked against its buffer view") {
    GltfBin bin;
    auto refusal = [&](PrStringView accessor) { return gltfRefusal(oneAccessor(accessor), &bin); };
    CHECK(refusal(R"("bufferView":0,"count":1,"type":"SCALAR")") == "accessors[0] must have a componentType and a type");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1)") == "accessors[0] must have a componentType and a type");
    CHECK(refusal(R"("bufferView":0,"componentType":5124,"count":1,"type":"SCALAR")") ==
        "accessors[0].componentType is not a value of its enumeration");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1,"type":"VEC5")") ==
        "accessors[0].type is not a value of its enumeration");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1,"type":3)") ==
        "accessors[0].type is not a value of its enumeration");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"type":"SCALAR")") == "accessors[0].count must be at least 1");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":0,"type":"SCALAR")") == "accessors[0].count must be at least 1");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":"1","type":"SCALAR")") == "accessors[0].count must be a number");
    CHECK(refusal(R"("componentType":5126,"count":4294967295,"type":"SCALAR")") == "accessors[0].count is out of range");
    CHECK(refusal(R"("bufferView":2,"componentType":5126,"count":1,"type":"SCALAR")") ==
        "accessors[0].bufferView is not the index of an object");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"normalized":true,"count":1,"type":"SCALAR")") ==
        "accessors[0].normalized must not be true for this componentType");
    CHECK(refusal(R"("bufferView":0,"componentType":5125,"normalized":true,"count":1,"type":"SCALAR")") ==
        "accessors[0].normalized must not be true for this componentType");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"normalized":1,"count":1,"type":"SCALAR")") ==
        "accessors[0].normalized must be true or false");
    CHECK(refusal(R"("byteOffset":0,"componentType":5126,"count":1,"type":"SCALAR")") ==
        "accessors[0].byteOffset must not be defined without a bufferView");
    CHECK(refusal(R"("bufferView":0,"byteOffset":2,"componentType":5126,"count":1,"type":"SCALAR")") ==
        "accessors[0].bufferView is not aligned to the size of a component");

    // 256 bytes hold 64 floats, and with a stride of 16 bytes, 16 elements.
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":65,"type":"SCALAR")") ==
        "accessors[0].bufferView has elements past the end of the buffer view");
    CHECK(refusal(R"("bufferView":0,"byteOffset":4,"componentType":5126,"count":64,"type":"SCALAR")") ==
        "accessors[0].bufferView has elements past the end of the buffer view");
    CHECK(refusal(R"("bufferView":1,"componentType":5126,"count":17,"type":"VEC3")") ==
        "accessors[0].bufferView has elements past the end of the buffer view");
    CHECK(refusal(R"("bufferView":1,"byteOffset":8,"componentType":5126,"count":16,"type":"VEC3")") ==
        "accessors[0].bufferView has elements past the end of the buffer view");
    CHECK(refusal(R"("bufferView":1,"componentType":5126,"count":1,"type":"MAT4")") ==
        "accessors[0].bufferView has a byteStride less than the size of an element");
    GltfRead last(oneAccessor(R"("bufferView":1,"byteOffset":4,"componentType":5126,"count":16,"type":"VEC3")"), &bin);
    CHECK(last.status == PrStatus_Ok);

    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1,"type":"VEC3","min":[0,0])") ==
        "accessors[0].min and max must have a number for each component");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1,"type":"VEC3","max":[0,0,0,0])") ==
        "accessors[0].min and max must have a number for each component");
    CHECK(refusal(R"("bufferView":0,"componentType":5126,"count":1,"type":"VEC3","min":[0,"a",0])") ==
        "accessors[0].min must be a number");
}

TEST_CASE("an accessor with a misaligned buffer view is refused") {
    GltfBin bin;
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":256,"uri":"bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":1,"byteLength":16}],)"
        R"("accessors":[{"bufferView":0,"componentType":5123,"count":1,"type":"SCALAR"}])"), &bin) ==
        "accessors[0].bufferView is not aligned to the size of a component");
}

TEST_CASE("a sparse accessor is checked against its buffer views and its indices") {
    GltfBin bin;
    bin.put<uint8_t>(32, 1, 3, 3, 9);
    auto refusal = [&](PrStringView sparse) {
        String accessor = R"("componentType":5126,"count":5,"type":"SCALAR","sparse":{)";
        return gltfRefusal(oneAccessor(accessor.append(sparse).append("}")), &bin);
    };
    const char* indices = R"("indices":{"bufferView":0,"byteOffset":32,"componentType":5121})";
    const char* values = R"("values":{"bufferView":0,"byteOffset":40})";
    auto with = [&](PrStringView count, PrStringView first, PrStringView second) {
        String sparse = count;
        return refusal(sparse.append(",").append(first).append(",").append(second));
    };
    CHECK(with(R"("count":0)", indices, values) == "accessors[0].sparse.count must be from 1 to the accessor's count");
    CHECK(with(R"("count":6)", indices, values) == "accessors[0].sparse.count must be from 1 to the accessor's count");
    CHECK(refusal(R"("count":2)") == "accessors[0].sparse must have indices and values");
    CHECK(with(R"("count":2)", R"("indices":{"byteOffset":32,"componentType":5121})", values) ==
        "accessors[0].sparse.indices.bufferView is required");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":0,"byteOffset":32})", values) ==
        "accessors[0].sparse.indices.componentType is required");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":0,"byteOffset":32,"componentType":5126})", values) ==
        "accessors[0].sparse.indices.componentType is not a value of its enumeration");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":1,"byteOffset":32,"componentType":5121})", values) ==
        "accessors[0].sparse reads a buffer view that has a byteStride");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":0,"byteOffset":255,"componentType":5121})", values) ==
        "accessors[0].sparse.indices.bufferView has elements past the end of the buffer view");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":0,"byteOffset":33,"componentType":5123})", values) ==
        "accessors[0].sparse.indices.bufferView is not aligned to the size of a component");
    CHECK(with(R"("count":2)", indices, R"("values":{"bufferView":0,"byteOffset":252})") ==
        "accessors[0].sparse.values.bufferView has elements past the end of the buffer view");
    CHECK(with(R"("count":2)", indices, R"("values":{"bufferView":0,"byteOffset":42})") ==
        "accessors[0].sparse.values.bufferView is not aligned to the size of a component");

    // The indices at byte 32 are 1, 3, 3 and 9: the third repeats, and the fourth names no element of five.
    CHECK(with(R"("count":3)", indices, values) ==
        "accessors[0].sparse.indices must increase strictly and be less than the accessor's count");
    CHECK(with(R"("count":2)", R"("indices":{"bufferView":0,"byteOffset":34,"componentType":5121})", values) ==
        "accessors[0].sparse.indices must increase strictly and be less than the accessor's count");
    bin.put<uint8_t>(32, 3, 1);
    CHECK(with(R"("count":2)", indices, values) ==
        "accessors[0].sparse.indices must increase strictly and be less than the accessor's count");
}

// ---------------------------------------------------------------------------------------------------------------------
// Meshopt compression

TEST_CASE("a compressed buffer view has the bytes that its stream decodes to") {
    GltfBin bin = meshoptBin();
    for (PrStringView extension : kMeshoptExtensions) {
        CAPTURE(extension);
        GltfRead read(meshoptAsset(extension), &bin);
        REQUIRE(read.status == PrStatus_Ok);
        const PrGltfBuffer& fallback = prGltfBuffers(read.gltf)[1];
        CHECK(fallback.fallback);
        CHECK(fallback.bytes.data == nullptr);
        const PrGltfMeshopt& meshopt = prGltfBufferViews(read.gltf)[1].meshopt;
        CHECK(meshopt.buffer == 0);
        CHECK(meshopt.mode == PrGltfMeshoptMode_Indices);
        CHECK(meshopt.filter == PrGltfMeshoptFilter_None);
        CHECK(meshopt.v1 == (extension == "KHR_meshopt_compression"));
        CHECK(meshopt.byteStride == 2);
        CHECK(meshopt.count == 3);
        CHECK(meshopt.byteOffset == 40);
        CHECK(meshopt.byteLength == 8);
        CHECK(prGltfBufferViews(read.gltf)[0].meshopt.mode == PrGltfMeshoptMode_Attributes);
        CHECK(prGltfBufferViews(read.gltf)[0].meshopt.byteOffset == 0);
        float value = 0;
        REQUIRE(prGltfAccessorReadFloats(read.gltf, 0, 0, 1, &value));
        CHECK(value == 1.5f);
        uint32_t indices[3] = {};
        REQUIRE(prGltfAccessorReadIntegers(read.gltf, 1, 0, 3, indices));
        CHECK(indices[0] == 0);
        CHECK(indices[1] == 1);
        CHECK(indices[2] == 2);
        CHECK(prGltfBufferViewBytes(read.gltf, 1).size == 6);
    }

    // A view without the extension has no meshopt form, and without the stream's bytes a compressed view has none.
    GltfRead plain(gltfBinAsset(R"("accessors":[])"));
    REQUIRE(plain.status == PrStatus_Ok);
    CHECK(prGltfBufferViews(plain.gltf)[0].meshopt.buffer == kPrGltfNone);
    CHECK(!prGltfBuffers(plain.gltf)[0].fallback);
    GltfRead absent(meshoptAsset("KHR_meshopt_compression"));
    REQUIRE(absent.status == PrStatus_Ok);
    CHECK(prGltfBufferViewBytes(absent.gltf, 0).data == nullptr);
    float value = 1;
    CHECK(!prGltfAccessorReadFloats(absent.gltf, 0, 0, 1, &value));
}

TEST_CASE("a compressed buffer view is checked against the rules of its extension") {
    GltfBin bin = meshoptBin();
    auto attributes = [&](PrStringView members) {
        return gltfRefusal(meshoptAsset("KHR_meshopt_compression", members), &bin);
    };
    auto indices = [&](PrStringView members) {
        return gltfRefusal(meshoptAsset("KHR_meshopt_compression", kMeshoptAttributes, members), &bin);
    };
    const char* view0 = "bufferViews[0].KHR_meshopt_compression.";
    const char* view1 = "bufferViews[1].KHR_meshopt_compression.";
    auto said = [](const char* scope, PrStringView sentence) {
        String text = scope;
        return text.append(sentence);
    };
    CHECK(attributes(R"("byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "buffer is required"));
    CHECK(attributes(R"("buffer":2,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "buffer is not the index of an object"));
    CHECK(attributes(R"("buffer":0,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "byteLength must be at least 1"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "byteStride must be from 1 to 256"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"mode":"ATTRIBUTES")") ==
        said(view0, "count must be at least 1"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"count":1)") == said(view0, "mode is required"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"count":1,"mode":"VERTICES")") ==
        said(view0, "mode is not a value of its enumeration"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES","filter":"BLUR")") ==
        said(view0, "filter is not a value of its enumeration"));
    CHECK(attributes(R"("buffer":0,"byteOffset":250,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "byteLength runs past the end of the buffer"));
    CHECK(attributes(R"("buffer":1,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        said(view0, "buffer is a fallback buffer, which holds no compressed data"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"count":2,"mode":"ATTRIBUTES")") ==
        said(view0, "count times byteStride is not the buffer view's byteLength"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":2,"count":2,"mode":"ATTRIBUTES")") ==
        said(view0, "byteStride of attributes must be a multiple of 4"));
    CHECK(attributes(R"("buffer":0,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES","filter":"QUATERNION")") ==
        said(view0, "byteStride of the QUATERNION filter must be 8"));
    // A stream that the extension does not define: here, too short for its tail.
    CHECK(attributes(R"("buffer":0,"byteLength":30,"byteStride":4,"count":1,"mode":"ATTRIBUTES")") ==
        "bufferViews[0].KHR_meshopt_compression is not a stream that the extension defines");

    CHECK(indices(R"("buffer":0,"byteOffset":40,"byteLength":8,"byteStride":1,"count":6,"mode":"INDICES")") ==
        said(view1, "byteStride of indices must be 2 or 4"));
    CHECK(indices(R"("buffer":0,"byteOffset":40,"byteLength":8,"byteStride":2,"count":3,"mode":"INDICES",)"
        R"("filter":"EXPONENTIAL")") == said(view1, "filter must be NONE for indices"));
    CHECK(indices(R"("buffer":0,"byteOffset":40,"byteLength":8,"byteStride":3,"count":2,"mode":"TRIANGLES")") ==
        said(view1, "byteStride of indices must be 2 or 4"));
    CHECK(indices(R"("buffer":0,"byteOffset":40,"byteLength":8,"byteStride":6,"count":1,"mode":"ATTRIBUTES")") ==
        said(view1, "byteStride of attributes must be a multiple of 4"));

    // The filter of colors is KHR_meshopt_compression's alone, as is version 1 of attributes.
    CHECK(gltfRefusal(meshoptAsset("EXT_meshopt_compression",
        R"("buffer":0,"byteLength":37,"byteStride":4,"count":1,"mode":"ATTRIBUTES","filter":"COLOR")"), &bin) ==
        "bufferViews[0].EXT_meshopt_compression.filter is not a value of its enumeration");
    GltfBin v1 = bin;
    v1.put<uint8_t>(0, 0xa1);
    CHECK(gltfRefusal(meshoptAsset("EXT_meshopt_compression"), &v1) ==
        "bufferViews[0].EXT_meshopt_compression is not a stream that the extension defines");

    // Only a compressed view names a fallback buffer, and a view is compressed by one extension.
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":8,"extensions":{"KHR_meshopt_compression":)"
        R"({"fallback":true}}}],"bufferViews":[{"buffer":0,"byteLength":8}])")) ==
        "bufferViews[0].buffer is a fallback buffer, which only a compressed buffer view names");
    CHECK(gltfRefusal(gltfAsset(R"("buffers":[{"byteLength":8}],"bufferViews":[{"buffer":0,"byteLength":8,)"
        R"("extensions":{"EXT_meshopt_compression":{},"KHR_meshopt_compression":{}}}])")) ==
        "bufferViews[0].extensions must not have both EXT_meshopt_compression and KHR_meshopt_compression");
    CHECK(gltfRefusal(meshoptAsset("KHR_meshopt_compression", kMeshoptAttributes, kMeshoptIndices,
        R"("fallback":"yes")"), &bin) == "buffers[1].KHR_meshopt_compression.fallback must be true or false");
}
