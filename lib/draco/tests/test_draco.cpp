#include "pr/array.h"
#include "pr/bit_cast.h"
#include "pr/draco.h"
#include "pr/mem.h"
#include "pr/status.h"

#include "tools/harness/test.h"

extern "C" int prTestDracoCApi(void);

namespace pr_impl {

namespace {

// A stream that a test writes byte by byte, as the bitstream lays it out.
struct Stream {
    Array<uint8_t> bytes;

    Stream& byte(uint32_t value) {
        bytes.push(uint8_t(value));
        return *this;
    }
    Stream& half(uint32_t value) { return byte(value).byte(value >> 8); }
    Stream& word(uint32_t value) { return half(value).half(value >> 16); }
    Stream& real(float value) { return word(bitCast<uint32_t>(value)); }
    Stream& varint(uint64_t value) {
        while (value >= 0x80) {
            byte(uint32_t(value) | 0x80);
            value >>= 7;
        }
        return byte(uint32_t(value));
    }
    Stream& text(const char* value) {
        for (; *value != 0; ++value)
            byte(uint8_t(*value));
        return *this;
    }

    // The header of a stream of version 2.2: the geometry, the method, and the flags.
    Stream& header(uint32_t geometry, uint32_t method, uint32_t flags = 0, uint32_t minor = 2, uint32_t major = 2) {
        return text("DRACO").byte(major).byte(minor).byte(geometry).byte(method).half(flags);
    }
    // A mesh of one face of three points whose indices are bytes, and the count of its groups of attributes.
    Stream& triangle(uint32_t groups = 1) {
        return header(1, 0).varint(1).varint(3).byte(1).byte(0).byte(1).byte(2).byte(groups);
    }
    // One attribute in a group of its own: its kind, the bitstream's number of its type, and how it is coded.
    Stream& attribute(uint32_t kind, uint32_t type, uint32_t components, uint32_t id, uint32_t coding) {
        return varint(1).byte(kind).byte(type).byte(components).byte(0).varint(id).byte(coding);
    }

    PrByteView view() const { return PrByteView{bytes.data(), bytes.count()}; }
};

// The bitstream's numbers of the types of components.
constexpr uint32_t kInt8 = 1;
constexpr uint32_t kUint8 = 2;
constexpr uint32_t kInt16 = 3;
constexpr uint32_t kFloat32 = 9;

// The bitstream's byte for no prediction.
constexpr uint32_t kNoPrediction = 0xfe;

struct Decoded {
    PrStatus status = PrStatus_Ok;
    PrDraco* draco = nullptr;

    explicit Decoded(PrByteView stream, const PrDracoLimits* limits = nullptr) {
        status = prDracoCreate(stream, limits, &draco);
    }
    ~Decoded() { prDracoDestroy(draco); }
};

PrStatus statusOf(const Stream& stream, const PrDracoLimits* limits = nullptr) {
    Decoded decoded(stream.view(), limits);
    CHECK((decoded.draco == nullptr) == (decoded.status != PrStatus_Ok));
    return decoded.status;
}

uint32_t fnv(const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

uint32_t valuesHash(const PrDraco* draco, uint32_t attribute) {
    PrByteView values = prDracoAttributeValues(draco, attribute);
    return fnv(values.data, values.size);
}

float floatAt(const PrDraco* draco, uint32_t attribute, uint32_t index) {
    return bitCast<float>(prMemReadLeU32(prDracoAttributeValues(draco, attribute).data + index * 4));
}

// Streams that the reference encoder wrote, with what the reference decoder gives for each: the number of points and of
// faces, and a hash of the faces and of each attribute's values (tools/suite/draco compares the decoders at large).

// A cube whose faces each have a normal of their own, with texture coordinates, coded by Edgebreaker with the standard
// traversal: 24 points, 12 faces.
const uint8_t kCube[] = {
    0x44, 0x52, 0x41, 0x43, 0x4f, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x08, 0x0c, 0x02, 0x0b, 0x00, 0x00, 0x03, 0x5f,
    0x4b, 0x15, 0x01, 0x01, 0x10, 0x55, 0x04, 0xd7, 0x53, 0xf6, 0x46, 0x55, 0x04, 0xd7, 0x53, 0xf6, 0x46, 0x03, 0xff, 0x00,
    0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x01, 0x00, 0x09, 0x03, 0x00, 0x00, 0x02, 0x01, 0x01, 0x09, 0x03, 0x00, 0x01,
    0x03, 0x01, 0x03, 0x09, 0x02, 0x00, 0x02, 0x02, 0x01, 0x01, 0x01, 0x00, 0x03, 0x03, 0x01, 0x20, 0x01, 0x20, 0x03, 0x00,
    0x70, 0x41, 0x9c, 0x20, 0x22, 0x43, 0x0b, 0x00, 0x00, 0x00, 0x00, 0xff, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x0b, 0x06, 0x03, 0x01, 0x01, 0x03, 0x01, 0x01, 0x40,
    0x01, 0x00, 0xff, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00, 0xff, 0x02, 0xa1, 0x41, 0x08, 0x05, 0x01, 0x01, 0x01, 0x04,
    0x03, 0x55, 0x29, 0xad, 0x0a, 0x01, 0x0c, 0x0a, 0x86, 0xe3, 0xe9, 0xdd, 0x6e, 0xe5, 0xbd, 0xe7, 0xe7, 0x8d, 0x0c, 0x00,
    0x00, 0x00, 0x15, 0x03, 0xec, 0xdc, 0x81, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x0a,
};

// A torus of 20 vertices with normals, texture coordinates that have seams, colors in normalized bytes and four 16-bit
// integers, coded by Edgebreaker with the valence traversal: 30 points, 40 faces.
const uint8_t kTorus[] = {
    0x44, 0x52, 0x41, 0x43, 0x4f, 0x02, 0x02, 0x01, 0x01, 0x00, 0x00, 0x00, 0x14, 0x28, 0x04, 0x27, 0x05, 0x02, 0x22, 0x08,
    0x03, 0x10, 0x03, 0x0b, 0x7f, 0xfa, 0x27, 0xd5, 0xa5, 0xd6, 0x94, 0x52, 0x4a, 0x51, 0x00, 0x01, 0x01, 0x10, 0xff, 0x02,
    0x5f, 0x44, 0xda, 0x07, 0xb4, 0xad, 0x72, 0x3c, 0x7c, 0x71, 0x81, 0xff, 0x02, 0x5f, 0x44, 0xff, 0x02, 0x5f, 0x44, 0x05,
    0xff, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x02, 0x00, 0x00, 0x03, 0x00, 0x00, 0x01, 0x00, 0x09, 0x03, 0x00,
    0x00, 0x02, 0x01, 0x01, 0x09, 0x03, 0x00, 0x01, 0x03, 0x01, 0x03, 0x09, 0x02, 0x00, 0x02, 0x02, 0x01, 0x02, 0x02, 0x04,
    0x01, 0x03, 0x01, 0x01, 0x04, 0x04, 0x04, 0x00, 0x04, 0x01, 0x01, 0x01, 0x01, 0x00, 0x0c, 0x23, 0x35, 0x03, 0xcd, 0x1c,
    0x01, 0x20, 0x05, 0xef, 0x05, 0x94, 0xdc, 0x7f, 0xb4, 0x51, 0x22, 0x0d, 0x65, 0xe3, 0xe2, 0x0c, 0x01, 0x20, 0x9e, 0x00,
    0x18, 0x1b, 0x00, 0x00, 0x18, 0xfe, 0x1f, 0x00, 0x5c, 0x39, 0x0c, 0x00, 0x0b, 0xf1, 0x0c, 0x40, 0x00, 0xee, 0x34, 0xc3,
    0x4c, 0x03, 0x00, 0x00, 0x59, 0x12, 0x03, 0xc0, 0x95, 0xc4, 0x00, 0xb0, 0xd0, 0xce, 0x00, 0x04, 0x80, 0xa7, 0x86, 0x0c,
    0xff, 0x0f, 0x00, 0xb4, 0x24, 0x06, 0x80, 0x63, 0xf4, 0x1f, 0x00, 0x73, 0x01, 0x08, 0xc0, 0xdc, 0xff, 0x00, 0xe6, 0x02,
    0xa0, 0x19, 0xf6, 0x3c, 0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x07, 0x00, 0x00, 0xbe, 0x1b, 0x4f, 0xc0, 0x70,
    0x78, 0x73, 0xc0, 0x00, 0x00, 0x80, 0xbf, 0x70, 0x78, 0xf3, 0x40, 0x0b, 0x06, 0x03, 0x01, 0x01, 0x04, 0xff, 0x01, 0x69,
    0x16, 0xcd, 0x1c, 0xff, 0xff, 0xff, 0xef, 0xcd, 0x0c, 0x0a, 0x1a, 0x3e, 0xd9, 0x46, 0x58, 0x0a, 0x34, 0x88, 0xd5, 0x85,
    0xff, 0x00, 0x00, 0x00, 0x7f, 0x00, 0x00, 0x00, 0xff, 0x02, 0x59, 0x41, 0x08, 0x05, 0x01, 0x01, 0x00, 0x0b, 0x1f, 0x89,
    0x08, 0x45, 0x24, 0x35, 0x13, 0x07, 0x60, 0x41, 0xf9, 0x51, 0xdf, 0x87, 0x7f, 0x00, 0xce, 0x0c, 0x60, 0x66, 0xe0, 0x0d,
    0x20, 0xd1, 0x40, 0xa8, 0x59, 0x37, 0xc4, 0xc7, 0x31, 0x69, 0x87, 0x0c, 0x9b, 0x19, 0xcb, 0x10, 0xe0, 0x0f, 0x20, 0xcd,
    0xb0, 0x35, 0x62, 0xac, 0x66, 0xc4, 0x02, 0x90, 0x12, 0xc0, 0x36, 0x40, 0xc7, 0x32, 0x69, 0x87, 0x3c, 0xc6, 0x15, 0x21,
    0x00, 0x8d, 0x01, 0x40, 0xae, 0x45, 0x61, 0x66, 0x92, 0x6b, 0x51, 0x08, 0x00, 0x64, 0x00, 0x67, 0x8d, 0x74, 0x04, 0x90,
    0x16, 0x00, 0x1c, 0x00, 0x00, 0x00, 0x09, 0x03, 0xf8, 0x89, 0x84, 0x00, 0x00, 0x00, 0x00, 0xff, 0x03, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x3f, 0x0a, 0x01, 0x01, 0x01, 0x00, 0x09, 0x03, 0x01, 0x10,
    0x35, 0x03, 0x03, 0x35, 0x03, 0x07, 0x69, 0x06, 0x31, 0x23, 0x07, 0x30, 0x4b, 0x69, 0x05, 0xed, 0x8d, 0x81, 0xd1, 0xe8,
    0xa2, 0x5c, 0xce, 0x68, 0x02, 0x63, 0x67, 0x20, 0xa8, 0xcf, 0x64, 0x00, 0x00, 0xc6, 0x60, 0x06, 0x30, 0xa0, 0x0c, 0x00,
    0x00, 0xe0, 0x0d, 0x00, 0x00, 0xb5, 0x73, 0x00, 0x00, 0x40, 0x0b, 0x00, 0x8b, 0x26, 0x74, 0x0a, 0x7d, 0x06, 0x00, 0xf0,
    0x2c, 0xd8, 0x0c, 0x10, 0x00, 0x00, 0x00, 0x90, 0x7a, 0x16, 0x00, 0x1a, 0xa0, 0xd9, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff,
    0x00, 0x00, 0x00, 0x01, 0x01, 0x01, 0x00, 0x06, 0x03, 0x01, 0x30, 0xcd, 0x0c, 0x07, 0x35, 0x03, 0x05, 0x4e, 0x35, 0x95,
    0x4f, 0x83, 0xc8, 0xce, 0xf2, 0xaa, 0x00, 0x00, 0x00, 0xa0, 0x0a, 0xff, 0xaa, 0x00, 0x0f, 0x07, 0x00, 0x00, 0x00, 0x00,
    0x1b, 0x00, 0x00, 0x00,
};

// A sheet of 9 vertices with normals and a signed integer, coded sequentially with its indices compressed: 9 points, 8
// faces.
const uint8_t kSheet[] = {
    0x44, 0x52, 0x41, 0x43, 0x4f, 0x02, 0x02, 0x01, 0x00, 0x00, 0x00, 0x08, 0x09, 0x00, 0x00, 0x05, 0x03, 0xad, 0x02, 0xfd,
    0x1f, 0xad, 0x1a, 0xad, 0x02, 0x07, 0x91, 0x91, 0x79, 0x1a, 0x17, 0x90, 0x80, 0x74, 0xbe, 0xd7, 0xae, 0x8f, 0x9f, 0xac,
    0x07, 0x01, 0x03, 0x00, 0x09, 0x03, 0x00, 0x00, 0x01, 0x09, 0x03, 0x00, 0x01, 0x04, 0x05, 0x01, 0x00, 0x02, 0x02, 0x03,
    0x01, 0x00, 0x01, 0x01, 0x00, 0x0c, 0x03, 0x1d, 0x07, 0x23, 0xe5, 0x38, 0x03, 0x04, 0xe0, 0x85, 0xf8, 0x3f, 0x00, 0x62,
    0xfb, 0xff, 0xff, 0xcb, 0xf6, 0xff, 0x00, 0x08, 0xb4, 0xff, 0xff, 0x4f, 0x64, 0x00, 0xfc, 0xbf, 0x27, 0x02, 0xf0, 0x3f,
    0x5f, 0xfe, 0x0f, 0x00, 0x97, 0xfb, 0x1f, 0x00, 0xcc, 0x03, 0x00, 0x00, 0x00, 0x00, 0xff, 0x07, 0x00, 0x00, 0x00, 0x03,
    0x01, 0x00, 0x09, 0x0b, 0x1d, 0x07, 0x1d, 0x07, 0x03, 0x1d, 0x07, 0x03, 0xad, 0x2a, 0x04, 0x5f, 0xbe, 0x74, 0x87, 0xc0,
    0xad, 0x4f, 0x20, 0xde, 0xe3, 0x07, 0xdc, 0xd3, 0x0f, 0xb0, 0xc6, 0xeb, 0xe3, 0xef, 0x03, 0xff, 0x00, 0x00, 0x00, 0x7f,
    0x00, 0x00, 0x00, 0x00, 0x01, 0x01, 0x00, 0x0e, 0x07, 0x1d, 0x07, 0x1f, 0x75, 0x1c, 0x39, 0x0e, 0x39, 0x0e, 0x04, 0x8a,
    0x79, 0x01, 0x5e, 0x6f, 0x17, 0xfa, 0xa0, 0x0f, 0xfd, 0xcf, 0x87, 0xbb, 0xd0, 0x17, 0xfa, 0x48, 0xf4, 0xff, 0xff, 0xb8,
    0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x0b,
    0x08,
};

template <size_t N>
PrByteView viewOf(const uint8_t (&bytes)[N]) {
    return PrByteView{bytes, N};
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestDracoCApi() == 0);
}

TEST_CASE("a sequential mesh has the faces and the values that the stream stores whole") {
    Stream stream;
    stream.triangle().attribute(PrDracoAttributeKind_Position, kFloat32, 3, 7, 0);
    for (uint32_t i = 0; i < 9; ++i)
        stream.real(float(i) * 0.5f);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    const PrDraco* draco = decoded.draco;
    CHECK(prDracoGeometry(draco) == PrDracoGeometry_TriangleMesh);
    CHECK(prDracoPointCount(draco) == 3);
    REQUIRE(prDracoFaceCount(draco) == 1);
    CHECK(prDracoFaces(draco)[0] == 0);
    CHECK(prDracoFaces(draco)[1] == 1);
    CHECK(prDracoFaces(draco)[2] == 2);
    REQUIRE(prDracoAttributeCount(draco) == 1);
    const PrDracoAttribute& attribute = prDracoAttributes(draco)[0];
    CHECK(attribute.id == 7);
    CHECK(attribute.kind == PrDracoAttributeKind_Position);
    CHECK(attribute.componentType == PrDracoComponentType_Float32);
    CHECK(attribute.componentCount == 3);
    CHECK(!attribute.normalized);
    CHECK(prDracoFindAttribute(draco, 7) == 0);
    CHECK(prDracoFindAttribute(draco, 0) == kPrDracoNone);
    REQUIRE(prDracoAttributeValues(draco, 0).size == 36);
    CHECK(floatAt(draco, 0, 0) == 0.0f);
    CHECK(floatAt(draco, 0, 8) == 4.0f);
}

TEST_CASE("the indices of a sequential mesh take the least size that holds a point") {
    // 300 points take 16-bit indices.
    Stream stream;
    stream.header(1, 0).varint(1).varint(300).byte(1).half(299).half(0).half(256).byte(1);
    stream.attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 0);
    for (uint32_t i = 0; i < 300; ++i)
        stream.byte(i);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(prDracoFaces(decoded.draco)[0] == 299);
    CHECK(prDracoFaces(decoded.draco)[2] == 256);
    CHECK(prDracoAttributeValues(decoded.draco, 0).data[299] == uint8_t(299));
}

TEST_CASE("integers that are stored whole are signed by their low bit") {
    // Three values of two components in a byte each: 0, -1, 1, -2, 2, -3.
    Stream stream;
    stream.triangle().attribute(PrDracoAttributeKind_Generic, kInt16, 2, 0, 1);
    stream.byte(kNoPrediction).byte(0).byte(1);
    for (uint32_t i = 0; i < 6; ++i)
        stream.byte(i);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    PrByteView values = prDracoAttributeValues(decoded.draco, 0);
    REQUIRE(values.size == 12);
    const int16_t expected[] = {0, -1, 1, -2, 2, -3};
    for (uint32_t i = 0; i < 6; ++i)
        CHECK(int16_t(prMemReadLeU16(values.data + i * 2)) == expected[i]);
}

TEST_CASE("quantized floats are a step of the range for each unit, from the least value") {
    // One component of 4 bits over a range of 30 from 10: the integers 0, 15 and 5 are 10, 40 and 20.
    Stream stream;
    stream.triangle().attribute(PrDracoAttributeKind_Generic, kFloat32, 1, 0, 2);
    stream.byte(kNoPrediction).byte(0).byte(1).byte(0).byte(30).byte(10);
    stream.real(10.0f).real(30.0f).byte(4);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(floatAt(decoded.draco, 0, 0) == 10.0f);
    CHECK(floatAt(decoded.draco, 0, 1) == 40.0f);
    CHECK(floatAt(decoded.draco, 0, 2) == 20.0f);
}

TEST_CASE("normals are unit vectors from octahedral coordinates") {
    // Three bits: coordinates from 0 to 6 about the center 3. The center is +x, and the corners are -x.
    Stream stream;
    stream.triangle().attribute(PrDracoAttributeKind_Normal, kFloat32, 3, 0, 3);
    stream.byte(kNoPrediction).byte(0).byte(1).byte(6).byte(6).byte(12).byte(6).byte(0).byte(0);
    stream.byte(3);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    const float expected[] = {1, 0, 0, 0, 1, 0, -1, 0, 0};
    for (uint32_t i = 0; i < 9; ++i)
        CHECK(floatAt(decoded.draco, 0, i) == expected[i]);
}

TEST_CASE("a point cloud that is coded in order has points and no faces") {
    Stream stream;
    stream.header(0, 0, 0, 3).word(2).byte(1).attribute(PrDracoAttributeKind_Position, kInt8, 3, 4, 0);
    for (uint32_t i = 0; i < 6; ++i)
        stream.byte(i + 1);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(prDracoGeometry(decoded.draco) == PrDracoGeometry_PointCloud);
    CHECK(prDracoPointCount(decoded.draco) == 2);
    CHECK(prDracoFaceCount(decoded.draco) == 0);
    CHECK(prDracoAttributeValues(decoded.draco, 0).data[5] == 6);
}

TEST_CASE("metadata is read and not kept") {
    // The metadata of attribute 7, with one entry, and of the geometry, with an entry and an empty child.
    Stream stream;
    stream.header(1, 0, 0x8000).varint(1).varint(7).varint(1).byte(1).text("k").varint(2).text("vv").varint(0);
    stream.varint(1).byte(4).text("name").varint(1).text("x").varint(1).byte(5).text("child").varint(0).varint(0);
    stream.varint(1).varint(3).byte(1).byte(0).byte(1).byte(2).byte(1);
    stream.attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 0).byte(9).byte(8).byte(7);
    Decoded decoded(stream.view());
    REQUIRE(decoded.status == PrStatus_Ok);
    CHECK(prDracoAttributeValues(decoded.draco, 0).data[2] == 7);

    // A value of no bytes is not one that the reference decoder reads.
    Stream empty;
    empty.header(1, 0, 0x8000).varint(0).varint(1).byte(1).text("k").varint(0).varint(0);
    empty.varint(1).varint(3).byte(1).byte(0).byte(1).byte(2).byte(0);
    CHECK(statusOf(empty) == PrStatus_InvalidData);
}

TEST_CASE("an Edgebreaker mesh decodes to the reference decoder's points and faces") {
    SUBCASE("the standard traversal, with seams at every edge") {
        Decoded decoded(viewOf(kCube));
        REQUIRE(decoded.status == PrStatus_Ok);
        const PrDraco* draco = decoded.draco;
        CHECK(prDracoPointCount(draco) == 24);
        REQUIRE(prDracoFaceCount(draco) == 12);
        CHECK(fnv(prDracoFaces(draco), 12 * 3 * 4) == 0xa52df1c2u);
        REQUIRE(prDracoAttributeCount(draco) == 3);
        CHECK(prDracoAttributes(draco)[1].kind == PrDracoAttributeKind_Normal);
        CHECK(prDracoAttributes(draco)[2].componentCount == 2);
        CHECK(valuesHash(draco, 0) == 0x72839595u);
        CHECK(valuesHash(draco, 1) == 0xf15815c5u);
        CHECK(valuesHash(draco, 2) == 0x6a7bdac5u);
    }
    SUBCASE("the valence traversal, with a handle and integer attributes") {
        Decoded decoded(viewOf(kTorus));
        REQUIRE(decoded.status == PrStatus_Ok);
        const PrDraco* draco = decoded.draco;
        CHECK(prDracoPointCount(draco) == 30);
        REQUIRE(prDracoFaceCount(draco) == 40);
        CHECK(fnv(prDracoFaces(draco), 40 * 3 * 4) == 0xaf230482u);
        REQUIRE(prDracoAttributeCount(draco) == 5);
        const PrDracoAttribute& color = prDracoAttributes(draco)[3];
        CHECK(color.kind == PrDracoAttributeKind_Color);
        CHECK(color.componentType == PrDracoComponentType_Uint8);
        CHECK(color.normalized);
        CHECK(prDracoAttributes(draco)[4].componentType == PrDracoComponentType_Uint16);
        const uint32_t hashes[] = {0x75c6992au, 0xc2a02481u, 0x85cf20c8u, 0x93dfb495u, 0xa5799e25u};
        for (uint32_t a = 0; a < 5; ++a)
            CHECK(valuesHash(draco, a) == hashes[a]);
    }
    SUBCASE("a sequential mesh with compressed indices") {
        Decoded decoded(viewOf(kSheet));
        REQUIRE(decoded.status == PrStatus_Ok);
        const PrDraco* draco = decoded.draco;
        CHECK(prDracoPointCount(draco) == 9);
        REQUIRE(prDracoFaceCount(draco) == 8);
        CHECK(fnv(prDracoFaces(draco), 8 * 3 * 4) == 0x8aaa408cu);
        CHECK(valuesHash(draco, 0) == 0x8a9cfe35u);
        CHECK(valuesHash(draco, 1) == 0xccac479fu);
        CHECK(valuesHash(draco, 2) == 0x625e7572u);
    }
}

TEST_CASE("a stream of another version or coding is not supported") {
    Stream old;
    old.header(1, 0, 0, 1).varint(0).varint(0).byte(1).byte(0);
    CHECK(statusOf(old) == PrStatus_NotSupported);
    Stream first;
    first.header(1, 1, 0, 3, 1);
    CHECK(statusOf(first) == PrStatus_NotSupported);
    Stream later;
    later.header(1, 0, 0, 3).varint(0).varint(0).byte(1).byte(0);
    CHECK(statusOf(later) == PrStatus_NotSupported);
    Stream tree;
    tree.header(0, 1, 0, 3);
    CHECK(statusOf(tree) == PrStatus_NotSupported);
}

TEST_CASE("a stream that describes more than the limits is refused before it is decoded") {
    PrDracoLimits limits = {.maxPoints = 23, .maxFaces = 12};
    CHECK(Decoded(viewOf(kCube), &limits).status == PrStatus_Limit);
    limits = {.maxPoints = 24, .maxFaces = 11};
    CHECK(Decoded(viewOf(kCube), &limits).status == PrStatus_Limit);
    limits = {.maxPoints = 24, .maxFaces = 12};
    CHECK(Decoded(viewOf(kCube), &limits).status == PrStatus_Ok);
    limits = {.maxPoints = 8, .maxFaces = 8};
    CHECK(Decoded(viewOf(kSheet), &limits).status == PrStatus_Limit);
    CHECK(prDracoLimitsDefault().maxPoints == 1u << 24);
}

TEST_CASE("a stream that the bitstream does not define is invalid") {
    CHECK(Decoded(PrByteView{nullptr, 0}).status == PrStatus_InvalidData);
    Stream magic;
    magic.text("DRACP").byte(2).byte(2).byte(1).byte(0).half(0);
    CHECK(statusOf(magic) == PrStatus_InvalidData);
    Stream geometry;
    geometry.header(2, 0);
    CHECK(statusOf(geometry) == PrStatus_InvalidData);
    Stream method;
    method.header(1, 2);
    CHECK(statusOf(method) == PrStatus_InvalidData);

    SUBCASE("a face that names no point") {
        Stream stream;
        stream.header(1, 0).varint(1).varint(3).byte(1).byte(0).byte(1).byte(3).byte(0);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
    }
    SUBCASE("more faces than the stream has bytes for") {
        Stream stream;
        stream.header(1, 0).varint(1000).varint(3).byte(1).byte(0).byte(1).byte(2).byte(0);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
    }
    SUBCASE("a group of no attributes") {
        Stream stream;
        stream.triangle().varint(0);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
    }
    SUBCASE("an attribute of no kind, no type or no components") {
        Stream kind;
        kind.triangle().attribute(5, kUint8, 1, 0, 0).byte(0).byte(0).byte(0);
        CHECK(statusOf(kind) == PrStatus_InvalidData);
        Stream type;
        type.triangle().attribute(PrDracoAttributeKind_Generic, 12, 1, 0, 0).byte(0).byte(0).byte(0);
        CHECK(statusOf(type) == PrStatus_InvalidData);
        Stream none;
        none.triangle().attribute(PrDracoAttributeKind_Generic, 0, 1, 0, 0).byte(0).byte(0).byte(0);
        CHECK(statusOf(none) == PrStatus_InvalidData);
        Stream components;
        components.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 0, 0, 0);
        CHECK(statusOf(components) == PrStatus_InvalidData);
    }
    SUBCASE("a coding that the attribute's type cannot have") {
        // Quantization and normals are of floats, normals have three components, and integers are of integers.
        Stream quantized;
        quantized.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 2);
        CHECK(statusOf(quantized) == PrStatus_InvalidData);
        Stream normals;
        normals.triangle().attribute(PrDracoAttributeKind_Normal, kFloat32, 2, 0, 3);
        CHECK(statusOf(normals) == PrStatus_InvalidData);
        Stream integers;
        integers.triangle().attribute(PrDracoAttributeKind_Generic, kFloat32, 1, 0, 1);
        CHECK(statusOf(integers) == PrStatus_InvalidData);
        Stream coding;
        coding.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 4);
        CHECK(statusOf(coding) == PrStatus_InvalidData);
    }
    SUBCASE("values that the stream ends before") {
        Stream stream;
        stream.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 2, 0, 0);
        stream.byte(1).byte(2).byte(3).byte(4).byte(5);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
    }
    SUBCASE("a prediction or a transform that the bitstream does not number") {
        Stream scheme;
        scheme.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 1).byte(7).byte(1).byte(0).byte(0);
        CHECK(statusOf(scheme) == PrStatus_InvalidData);
        Stream undefined;
        undefined.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 1);
        undefined.byte(0xff).byte(1).byte(0).byte(0);
        CHECK(statusOf(undefined) == PrStatus_InvalidData);
        // The integers of an attribute that is not a normal take the wrap transform alone.
        Stream transform;
        transform.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 1).byte(0).byte(3).byte(0).byte(0);
        transform.word(0).word(0);
        CHECK(statusOf(transform) == PrStatus_InvalidData);
    }
    SUBCASE("a wrap transform whose bounds are out of order") {
        Stream stream;
        stream.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 1).byte(0).byte(1).byte(0).byte(0);
        stream.word(5).word(4);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
        Stream ordered;
        ordered.triangle().attribute(PrDracoAttributeKind_Generic, kUint8, 1, 0, 1).byte(0).byte(1).byte(0).byte(0);
        ordered.word(0).word(4);
        CHECK(statusOf(ordered) == PrStatus_Ok);
    }
    SUBCASE("a number of quantization bits that no transform takes") {
        Stream stream;
        stream.triangle().attribute(PrDracoAttributeKind_Generic, kFloat32, 1, 0, 2);
        stream.byte(kNoPrediction).byte(0).byte(1).byte(0).byte(30).byte(10).real(10.0f).real(30.0f).byte(31);
        CHECK(statusOf(stream) == PrStatus_InvalidData);
        Stream normals;
        normals.triangle().attribute(PrDracoAttributeKind_Normal, kFloat32, 3, 0, 3);
        normals.byte(kNoPrediction).byte(0).byte(1).byte(6).byte(6).byte(12).byte(6).byte(0).byte(0).byte(1);
        CHECK(statusOf(normals) == PrStatus_InvalidData);
    }
    SUBCASE("the prediction scheme that the bitstream withdrew for texture coordinates is not supported") {
        Stream stream;
        stream.triangle().attribute(PrDracoAttributeKind_TexCoord, kUint8, 2, 0, 1).byte(3).byte(1).byte(0).byte(0);
        CHECK(statusOf(stream) == PrStatus_NotSupported);
    }
}

TEST_CASE("a stream of the reference encoder that is cut short at any length is invalid") {
    for (size_t length = 0; length < sizeof(kCube); ++length) {
        Array<uint8_t> exact;
        exact.append(kCube, uint32_t(length));
        PrDraco* draco = nullptr;
        PrStatus status = prDracoCreate(PrByteView{exact.data(), length}, nullptr, &draco);
        CHECK(status == PrStatus_InvalidData);
        CHECK(draco == nullptr);
    }
}
