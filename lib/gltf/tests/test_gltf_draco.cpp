#include "pr/bit_cast.h"

#include "gltf/tests/gltf_fixture.h"

namespace pr_impl {

namespace {

// Writes a Draco stream into a buffer byte by byte, as the bitstream lays it out.
struct DracoWriter {
    GltfBin* bin;
    uint32_t at;

    DracoWriter& byte(uint32_t value) {
        REQUIRE(at < kGltfBinSize);
        bin->bytes[at++] = uint8_t(value);
        return *this;
    }
    DracoWriter& real(float value) {
        uint32_t bits = bitCast<uint32_t>(value);
        return byte(bits).byte(bits >> 8).byte(bits >> 16).byte(bits >> 24);
    }
    DracoWriter& text(const char* value) {
        for (; *value != 0; ++value)
            byte(uint8_t(*value));
        return *this;
    }
    // One attribute's kind, the bitstream's number of its type, its components, whether it is normalized, and its id.
    DracoWriter& attribute(uint32_t kind, uint32_t type, uint32_t components, uint32_t normalized, uint32_t id) {
        return byte(kind).byte(type).byte(components).byte(normalized).byte(id);
    }
};

// The bitstream's numbers of the kinds of attributes and of the types of components.
constexpr uint32_t kPosition = 0;
constexpr uint32_t kTexCoord = 3;
constexpr uint32_t kGeneric = 4;
constexpr uint32_t kUint8 = 2;
constexpr uint32_t kInt16 = 3;
constexpr uint32_t kFloat32 = 9;

// The positions of the quad of the Draco tests, whose texture coordinates are their x and y.
constexpr float kQuad[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {1, 1, 0}};

// What a stream of the Draco tests holds besides a quad of four points and the faces (0, 1, 2) and (2, 1, 3).
struct DracoQuad {
    uint32_t minor = 2;                 // the minor version of the bitstream
    uint32_t geometry = 1;              // a triangle mesh
    bool quantized = false;             // positions as normalized 16-bit integers, 32767 for 1
    bool positionsOnly = false;         // no other attribute than the positions
    float scale = 1;                    // of the positions
    uint32_t offset = 0;                // of the stream in the buffer
};

// Writes into a buffer a stream that is coded sequentially, with every value stored whole: positions of three floats
// with id 0, texture coordinates of two floats with id 1, and four unsigned bytes with id 2, which are 4p to 4p + 3 for
// point p.
void putDracoQuad(GltfBin* bin, DracoQuad quad = {}) {
    DracoWriter writer = {bin, quad.offset};
    writer.text("DRACO").byte(2).byte(quad.minor).byte(quad.geometry).byte(0).byte(0).byte(0);
    if (quad.geometry == 1)
        writer.byte(2).byte(4).byte(1).byte(0).byte(1).byte(2).byte(2).byte(1).byte(3);
    else
        writer.byte(4).byte(0).byte(0).byte(0);
    writer.byte(1).byte(quad.positionsOnly ? 1 : 3);
    if (quad.quantized)
        writer.attribute(kPosition, kInt16, 3, 1, 0);
    else
        writer.attribute(kPosition, kFloat32, 3, 0, 0);
    if (!quad.positionsOnly)
        writer.attribute(kTexCoord, kFloat32, 2, 0, 1).attribute(kGeneric, kUint8, 4, 0, 2).byte(0).byte(0);
    writer.byte(0);
    for (const float* point : kQuad) {
        for (uint32_t c = 0; c < 3; ++c) {
            if (quad.quantized)
                writer.byte(point[c] != 0 ? 0xff : 0).byte(point[c] != 0 ? 0x7f : 0);
            else
                writer.real(point[c] * quad.scale);
        }
    }
    if (quad.positionsOnly)
        return;
    for (const float* point : kQuad)
        writer.real(point[0]).real(point[1]);
    for (uint32_t i = 0; i < 16; ++i)
        writer.byte(i);
}

// The accessors of the Draco tests:
//
//   0  VEC3 floats, 4: positions, with bounds, which the stream fills
//   1  SCALAR unsigned shorts, 6: indices, which the stream fills
//   2  VEC2 floats, 4: texture coordinates, which the stream fills
//   3  VEC4 unsigned bytes, 4: joints, which the stream fills
//   4  VEC4 floats, 4 at byte 144 of the buffer: weights, which the stream does not hold
//   5  VEC3 floats, 4 at byte 208 of the buffer: the positions of a morph target
constexpr PrStringView kDracoAccessors =
    R"({"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"componentType":5123,"count":6,"type":"SCALAR"},)"
    R"({"componentType":5126,"count":4,"type":"VEC2"},)"
    R"({"componentType":5121,"count":4,"type":"VEC4"},)"
    R"({"bufferView":1,"componentType":5126,"count":4,"type":"VEC4"},)"
    R"({"bufferView":1,"byteOffset":64,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[9,9,9]})";

constexpr PrStringView kDracoPrimitive =
    R"("attributes":{"POSITION":0,"TEXCOORD_0":2,"JOINTS_0":3,"WEIGHTS_0":4},"indices":1,"targets":[{"POSITION":5}])";

constexpr PrStringView kDracoExtension = R"("bufferView":0,"attributes":{"POSITION":0,"TEXCOORD_0":1,"JOINTS_0":2})";

// Returns an asset that requires KHR_draco_mesh_compression, with the buffer "bin", buffer view 0 over the stream at
// its start, buffer view 1 over its bytes from 144, the accessors `accessors`, and one primitive with the members
// `primitive` and the extension's object `extension`.
String dracoAsset(PrStringView extension = kDracoExtension, PrStringView primitive = kDracoPrimitive,
        PrStringView accessors = kDracoAccessors) {
    String members = R"("extensionsUsed":["KHR_draco_mesh_compression","KHR_mesh_quantization"],)"
        R"("extensionsRequired":["KHR_draco_mesh_compression","KHR_mesh_quantization"],)"
        R"("buffers":[{"byteLength":256,"uri":"bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":140},{"buffer":0,"byteOffset":144,"byteLength":112},)"
        R"({"buffer":0,"byteLength":140,"byteStride":4}],)"
        R"("accessors":[)";
    members.append(accessors).append(R"(],"meshes":[{"primitives":[{)").append(primitive);
    members.append(R"(,"extensions":{"KHR_draco_mesh_compression":{)").append(extension).append("}}}]}]");
    return gltfAsset(members);
}

// Returns the sentence with which an asset of the Draco tests is refused, after the path of the extension.
String dracoRefusal(const GltfBin& bin, PrStringView extension = kDracoExtension,
        PrStringView primitive = kDracoPrimitive, PrStringView accessors = kDracoAccessors) {
    return gltfRefusal(dracoAsset(extension, primitive, accessors), &bin);
}

String said(PrStringView sentence) {
    String text = "meshes[0].primitives[0].KHR_draco_mesh_compression";
    return text.append(sentence);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a primitive's Draco stream fills the accessors of its indices and of the attributes that it holds") {
    GltfBin bin;
    putDracoQuad(&bin);
    bin.put<float>(144, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 0.25f, 0.75f, 0, 0);
    bin.put<float>(208, 9, 9, 9, 0, 0, 0, 0, 0, 0, 0, 0, 0);
    CHECK(prGltfExtensionIsSupported("KHR_draco_mesh_compression"));
    GltfRead read(dracoAsset(), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfPrimitive& primitive = prGltfPrimitives(read.gltf)[0];
    CHECK(primitive.draco == 0);
    CHECK(primitive.vertexCount == 4);
    const PrGltfAccessor* accessors = prGltfAccessors(read.gltf);
    for (uint32_t a = 0; a < 6; ++a)
        CHECK(accessors[a].draco == (a < 4));
    CHECK(accessors[0].bufferView == kPrGltfNone);
    CHECK(prGltfAccessorStride(read.gltf, 0) == 12);

    float positions[12] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 0, 0, 4, positions));
    float texCoords[8] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 2, 0, 4, texCoords));
    for (uint32_t p = 0; p < 4; ++p) {
        for (uint32_t c = 0; c < 3; ++c)
            CHECK(positions[p * 3 + c] == kQuad[p][c]);
        CHECK(texCoords[p * 2] == kQuad[p][0]);
        CHECK(texCoords[p * 2 + 1] == kQuad[p][1]);
    }
    uint32_t joints[16] = {};
    REQUIRE(prGltfAccessorReadIntegers(read.gltf, 3, 0, 4, joints));
    for (uint32_t i = 0; i < 16; ++i)
        CHECK(joints[i] == i);
    uint32_t indices[6] = {};
    REQUIRE(prGltfAccessorReadIntegers(read.gltf, 1, 0, 6, indices));
    const uint32_t expected[6] = {0, 1, 2, 2, 1, 3};
    for (uint32_t i = 0; i < 6; ++i)
        CHECK(indices[i] == expected[i]);
    uint32_t triangles[6] = {};
    CHECK(prGltfPrimitiveTriangles(read.gltf, 0, triangles, 6) == 6);
    CHECK(triangles[5] == 3);

    // An attribute that the extension does not name, and a morph target, are read from their own accessors.
    float weights[4] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 4, 3, 1, weights));
    CHECK(weights[1] == 0.75f);
    float displaced[3] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 5, 0, 1, displaced));
    CHECK(displaced[0] == 9.0f);
}

TEST_CASE("an accessor that a Draco stream fills reads the stream and not its own buffer view") {
    GltfBin bin;
    putDracoQuad(&bin);
    bin.put<float>(208, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9);
    // The positions name the bytes at 208 as well, for a reader that does not read the extension.
    PrStringView accessors =
        R"({"bufferView":1,"byteOffset":64,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"componentType":5125,"count":6,"type":"SCALAR"})";
    PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
    GltfRead read(dracoAsset(R"("bufferView":0,"attributes":{"POSITION":0})", primitive, accessors), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfAccessors(read.gltf)[0].bufferView == 1);
    CHECK(prGltfAccessors(read.gltf)[0].draco);
    float positions[12] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 0, 0, 4, positions));
    CHECK(positions[3] == 1.0f);
    CHECK(positions[11] == 0.0f);
    // Indices are written in the component type of their accessor.
    uint32_t indices[6] = {};
    REQUIRE(prGltfAccessorReadIntegers(read.gltf, 1, 0, 6, indices));
    CHECK(indices[5] == 3);

    // Without the bytes of the stream's buffer, the accessors that it fills have none.
    GltfRead described(dracoAsset(R"("bufferView":0,"attributes":{"POSITION":0})", primitive, accessors));
    REQUIRE(described.status == PrStatus_Ok);
    CHECK(prGltfPrimitives(described.gltf)[0].draco == 0);
    CHECK(prGltfAccessors(described.gltf)[0].draco);
    CHECK(!prGltfAccessorReadFloats(described.gltf, 0, 0, 4, positions));
    CHECK(!prGltfAccessorReadIntegers(described.gltf, 1, 0, 6, indices));
}

TEST_CASE("sparse elements replace elements that a Draco stream gives") {
    GltfBin bin;
    putDracoQuad(&bin);
    bin.put<uint8_t>(144, 2);
    bin.put<float>(148, 5, 6, 7);
    PrStringView accessors =
        R"({"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[5,6,7],"sparse":{"count":1,)"
        R"("indices":{"bufferView":1,"componentType":5121},"values":{"bufferView":1,"byteOffset":4}}},)"
        R"({"componentType":5121,"count":6,"type":"SCALAR"})";
    PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
    GltfRead read(dracoAsset(R"("bufferView":0,"attributes":{"POSITION":0})", primitive, accessors), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    float positions[12] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 0, 0, 4, positions));
    CHECK(positions[3] == 1.0f);
    CHECK(positions[6] == 5.0f);
    CHECK(positions[8] == 7.0f);
    CHECK(positions[9] == 1.0f);
}

TEST_CASE("a Draco stream fills a quantized attribute in the integers of its accessor") {
    GltfBin bin;
    putDracoQuad(&bin, {.quantized = true});
    PrStringView accessors =
        R"({"componentType":5122,"normalized":true,"count":4,"type":"VEC3","min":[0,0,0],"max":[32767,32767,0]},)"
        R"({"componentType":5123,"count":6,"type":"SCALAR"})";
    PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
    GltfRead read(dracoAsset(R"("bufferView":0,"attributes":{"POSITION":0})", primitive, accessors), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    float positions[12] = {};
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 0, 0, 4, positions));
    for (uint32_t p = 0; p < 4; ++p) {
        for (uint32_t c = 0; c < 3; ++c)
            CHECK(positions[p * 3 + c] == kQuad[p][c]);
    }
    uint32_t integers[3] = {};
    REQUIRE(prGltfAccessorReadIntegers(read.gltf, 0, 3, 1, integers));
    CHECK(integers[0] == 32767);
}

TEST_CASE("two primitives may share a Draco stream, and may not give an accessor different elements") {
    GltfBin bin;
    putDracoQuad(&bin);
    PrStringView accessors =
        R"({"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
        R"({"componentType":5123,"count":6,"type":"SCALAR"})";
    // The second primitive of each asset follows the first's extension object.
    PrStringView same = R"("bufferView":0,"attributes":{"POSITION":0}}}},{"attributes":{"POSITION":0},"indices":1,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":0})";
    PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
    GltfRead read(dracoAsset(same, primitive, accessors), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    CHECK(prGltfPrimitives(read.gltf)[1].draco == 0);

    // A second stream, in the second buffer view, with the same faces and positions twice as far apart.
    putDracoQuad(&bin, {.positionsOnly = true, .scale = 2, .offset = 144});
    PrStringView other = R"("bufferView":0,"attributes":{"POSITION":0}}}},{"attributes":{"POSITION":0},"indices":1,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":1,"attributes":{"POSITION":0})";
    GltfRead refused(dracoAsset(other, primitive, accessors), &bin);
    CHECK(refused.status == PrStatus_InvalidData);
    CHECK(refused.diagnostics == "meshes[0].primitives[1].KHR_draco_mesh_compression.bufferView gives an accessor "
        "other elements than another primitive's stream gave it");
}

TEST_CASE("a primitive's Draco stream is checked against the rules of its extension") {
    GltfBin bin;
    putDracoQuad(&bin);
    bin.put<float>(144, 1, 0, 0, 0, 0.5f, 0.5f, 0, 0, 1, 0, 0, 0, 0.25f, 0.75f, 0, 0);
    CHECK(dracoRefusal(bin, R"("attributes":{"POSITION":0})") == said(".bufferView is required"));
    CHECK(dracoRefusal(bin, R"("bufferView":3,"attributes":{"POSITION":0})") ==
        said(".bufferView is not the index of an object"));
    CHECK(dracoRefusal(bin, R"("bufferView":0)") == said(".attributes must hold at least one attribute"));
    CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{})") ==
        said(".attributes must hold at least one attribute"));
    CHECK(dracoRefusal(bin, R"("bufferView":2,"attributes":{"POSITION":0})") ==
        said(".bufferView must not have a byteStride"));
    CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{"POSITION":0.5})") ==
        said(".attributes must be a non-negative integer"));
    // The attributes of the stream are a subset of the primitive's.
    CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{"POSITION":0,"NORMAL":1})") ==
        said(".attributes names an attribute that the primitive does not have"));
    CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{"POSITION":7})") ==
        said(".bufferView has no attribute with an id that attributes names"));

    SUBCASE("the primitive is indexed triangles") {
        PrStringView points = R"("attributes":{"POSITION":0},"indices":1,"mode":0)";
        CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{"POSITION":0})", points) ==
            said(" is of a primitive whose mode is not triangles"));
        PrStringView unindexed = R"("attributes":{"POSITION":0})";
        PrStringView three = R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})";
        CHECK(dracoRefusal(bin, R"("bufferView":0,"attributes":{"POSITION":0})", unindexed, three) ==
            said(" is of a primitive that has no indices"));
        // How decoded triangles become a strip is not defined, so a strip is not read.
        PrStringView strip = R"("attributes":{"POSITION":0},"indices":1,"mode":5)";
        GltfRead read(dracoAsset(R"("bufferView":0,"attributes":{"POSITION":0})", strip), &bin);
        CHECK(read.status == PrStatus_NotSupported);
        CHECK(read.diagnostics == said(" is of a triangle strip, which the module does not read"));
    }
    SUBCASE("the accessors match the decoded mesh") {
        PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
        PrStringView extension = R"("bufferView":0,"attributes":{"POSITION":0})";
        auto with = [&](PrStringView position, PrStringView indices) {
            String accessors = position;
            accessors.append(",").append(indices);
            return dracoRefusal(bin, extension, primitive, accessors);
        };
        PrStringView position = R"({"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})";
        PrStringView indices = R"({"componentType":5123,"count":6,"type":"SCALAR"})";
        CHECK(with(R"({"componentType":5126,"count":5,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})", indices) ==
            said(".bufferView does not have as many points as the primitive's attributes have elements"));
        CHECK(with(R"({"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]})", indices) ==
            said(".bufferView describes more points or faces than the primitive's accessors count"));
        CHECK(with(position, R"({"componentType":5123,"count":9,"type":"SCALAR"})") ==
            said(".bufferView does not have as many indices as the primitive's indices"));
        CHECK(with(position, R"({"componentType":5123,"count":3,"type":"SCALAR"})") ==
            said(".bufferView describes more points or faces than the primitive's accessors count"));
        // The stream's positions are three floats.
        PrStringView custom = R"("attributes":{"_A":0},"indices":1)";
        PrStringView named = R"("bufferView":0,"attributes":{"_A":0})";
        auto typed = [&](PrStringView accessor) {
            String accessors = accessor;
            accessors.append(",").append(indices);
            return dracoRefusal(bin, named, custom, accessors);
        };
        CHECK(typed(R"({"componentType":5126,"count":4,"type":"VEC2"})") ==
            said(".bufferView has an attribute whose type is not its accessor's"));
        CHECK(typed(R"({"componentType":5123,"count":4,"type":"VEC3"})") ==
            said(".bufferView has an attribute whose type is not its accessor's"));
        CHECK(GltfRead(dracoAsset(named, custom, R"({"componentType":5126,"count":4,"type":"VEC3"},)"
            R"({"componentType":5123,"count":6,"type":"SCALAR"})"), &bin).status == PrStatus_Ok);
    }
    SUBCASE("the stream is a Draco mesh that the module reads") {
        PrStringView primitive = R"("attributes":{"POSITION":0},"indices":1)";
        PrStringView extension = R"("bufferView":0,"attributes":{"POSITION":0})";
        PrStringView accessors =
            R"({"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
            R"({"componentType":5123,"count":6,"type":"SCALAR"})";
        GltfBin garbage;
        garbage.put<uint8_t>(0, 'D', 'R', 'A', 'C', 'X', 2, 2, 1, 0, 0, 0, 2, 4, 1);
        CHECK(dracoRefusal(garbage, extension, primitive, accessors) ==
            said(".bufferView is not a stream that the Draco bitstream defines"));
        GltfBin cloud;
        putDracoQuad(&cloud, {.minor = 3, .geometry = 0});
        CHECK(dracoRefusal(cloud, extension, primitive, accessors) == said(".bufferView is not a stream of a triangle mesh"));
        GltfBin old;
        putDracoQuad(&old, {.minor = 1});
        GltfRead read(dracoAsset(extension, primitive, accessors), &old);
        CHECK(read.status == PrStatus_NotSupported);
        CHECK(read.diagnostics == said(".bufferView is a Draco stream of a version or a coding that the module does not read"));
    }
}
