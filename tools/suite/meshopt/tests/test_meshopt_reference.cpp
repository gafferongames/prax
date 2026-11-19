// lib/gltf's decoders of meshopt compression beside meshoptimizer's, over streams that meshoptimizer's encoders write:
// attributes in both versions at every level, with each stride and count that changes how blocks and groups fall, the
// indices of triangles and of sequences, and the four filters. The codecs are lossless, so each decodes to what was
// encoded. The filters round, and the extension allows a decoder one unit in the last place, except for the exponential
// filter, which is exact.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gltf.h"
#include "pr/mem.h"

#include "meshoptimizer.h"
#include "tools/harness/test.h"

using namespace pr;

namespace {

// A sequence of pseudo-random numbers, the same on every run.
struct Sequence {
    uint32_t state = 0x2545f491u;

    uint32_t next() {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }

    float unit() { return float(next() >> 8) / float(1u << 24); }
};

// The kinds of data that an encoder chooses differently for: bytes that change little from one element to the next,
// bytes at random, and bytes that never change.
enum {
    Kind_Smooth,
    Kind_Random,
    Kind_Constant,
};
typedef uint8_t Kind;

constexpr Kind kKinds[] = {Kind_Smooth, Kind_Random, Kind_Constant};
constexpr uint32_t kTriangleCounts[] = {1, 2, 50, 1000};
constexpr uint32_t kIndexStrides[] = {2, 4};
constexpr uint32_t kFilterStrides[] = {4, 8};
constexpr int kFilterBits[] = {4, 8, 12, 16};
constexpr int kQuaternionBits[] = {4, 9, 12, 16};
constexpr int kExponentBits[] = {8, 15, 23};
constexpr meshopt_EncodeExpMode kExponentModes[] = {
    meshopt_EncodeExpSeparate,
    meshopt_EncodeExpSharedVector,
    meshopt_EncodeExpSharedComponent,
};

Array<uint8_t> elements(uint32_t count, uint32_t stride, Kind kind, Sequence* random) {
    Array<uint8_t> data;
    data.resize(count * stride);
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t k = 0; k < stride; ++k) {
            uint8_t& byte = data[i * stride + k];
            if (kind == Kind_Random)
                byte = uint8_t(random->next());
            else if (kind == Kind_Constant)
                byte = uint8_t(k * 37);
            else
                byte = uint8_t((i == 0 ? k : data[(i - 1) * stride + k]) + (random->next() % 5) - 2);
        }
    }
    return data;
}

// Decodes `stream` with lib/gltf as a view of `meshopt`, and returns its bytes, or an empty array if it fails.
Array<uint8_t> decodeGltf(const Array<uint8_t>& stream, PrGltfMeshopt meshopt) {
    Array<uint8_t> out;
    out.resize(meshopt.count * meshopt.byteStride);
    if (!prGltfMeshoptDecode(PrByteView{stream.data(), stream.count()}, &meshopt, out.data()))
        out.clear();
    return out;
}

PrGltfMeshopt describe(PrGltfMeshoptMode mode, PrGltfMeshoptFilter filter, uint32_t count, uint32_t stride, bool v1) {
    return PrGltfMeshopt{
        .buffer = 0,
        .mode = mode,
        .filter = filter,
        .v1 = v1,
        .byteStride = uint16_t(stride),
        .count = count,
        .byteOffset = 0,
        .byteLength = 0,
    };
}

Array<uint8_t> encodeAttributes(const Array<uint8_t>& data, uint32_t count, uint32_t stride, int level, int version) {
    Array<uint8_t> stream;
    stream.resize(uint32_t(meshopt_encodeVertexBufferBound(count, stride)));
    size_t size = meshopt_encodeVertexBufferLevel(stream.data(), stream.count(), data.data(), count, stride, level, version);
    REQUIRE(size != 0);
    stream.resize(uint32_t(size));
    return stream;
}

// Encodes `values` with a filter of meshoptimizer, then as attributes of version 1, and returns the components that
// lib/gltf decodes and those that meshoptimizer decodes, as signed or unsigned integers of the element's width.
struct Filtered {
    Array<uint8_t> gltf;
    Array<uint8_t> reference;
};

Filtered filtered(PrGltfMeshoptFilter filter, const Array<uint8_t>& encoded, uint32_t count, uint32_t stride) {
    Array<uint8_t> stream = encodeAttributes(encoded, count, stride, 2, 1);
    Filtered result;
    result.gltf = decodeGltf(stream, describe(PrGltfMeshoptMode_Attributes, filter, count, stride, true));
    result.reference.resize(count * stride);
    REQUIRE(meshopt_decodeVertexBuffer(result.reference.data(), count, stride, stream.data(), stream.count()) == 0);
    switch (filter) {
        case PrGltfMeshoptFilter_Octahedral:
            meshopt_decodeFilterOct(result.reference.data(), count, stride);
            break;
        case PrGltfMeshoptFilter_Quaternion:
            meshopt_decodeFilterQuat(result.reference.data(), count, stride);
            break;
        case PrGltfMeshoptFilter_Exponential:
            meshopt_decodeFilterExp(result.reference.data(), count, stride);
            break;
        default:
            meshopt_decodeFilterColor(result.reference.data(), count, stride);
            break;
    }
    REQUIRE(result.gltf.count() == result.reference.count());
    return result;
}

// Returns the greatest difference between the components of `a` and `b`, as integers of `size` bytes, signed or not.
int32_t greatestDifference(const Array<uint8_t>& a, const Array<uint8_t>& b, uint32_t size, bool isSigned) {
    int32_t greatest = 0;
    for (uint32_t i = 0; i + size <= a.count(); i += size) {
        int32_t x = 0;
        int32_t y = 0;
        if (size == 1) {
            x = isSigned ? int32_t(int8_t(a[i])) : int32_t(a[i]);
            y = isSigned ? int32_t(int8_t(b[i])) : int32_t(b[i]);
        } else {
            x = isSigned ? int32_t(int16_t(prMemReadLeU16(&a[i]))) : int32_t(prMemReadLeU16(&a[i]));
            y = isSigned ? int32_t(int16_t(prMemReadLeU16(&b[i]))) : int32_t(prMemReadLeU16(&b[i]));
        }
        int32_t difference = x > y ? x - y : y - x;
        greatest = difference > greatest ? difference : greatest;
    }
    return greatest;
}

} // namespace

TEST_CASE("attributes decode as meshoptimizer decodes them, in both versions and at every level") {
    Sequence random;
    const uint32_t strides[] = {4, 8, 12, 16, 20, 32, 64, 128, 256};
    const uint32_t counts[] = {1, 15, 16, 17, 100, 255, 256, 257, 1000};
    uint32_t checked = 0;
    for (uint32_t stride : strides) {
        for (uint32_t count : counts) {
            for (Kind kind : kKinds) {
                Array<uint8_t> data = elements(count, stride, kind, &random);
                for (int version = 0; version <= 1; ++version) {
                    for (int level = 0; level <= 3; ++level) {
                        CAPTURE(stride);
                        CAPTURE(count);
                        CAPTURE(int(kind));
                        CAPTURE(version);
                        CAPTURE(level);
                        Array<uint8_t> stream = encodeAttributes(data, count, stride, level, version);
                        Array<uint8_t> reference;
                        reference.resize(count * stride);
                        REQUIRE(meshopt_decodeVertexBuffer(reference.data(), count, stride, stream.data(),
                            stream.count()) == 0);
                        Array<uint8_t> decoded = decodeGltf(stream,
                            describe(PrGltfMeshoptMode_Attributes, PrGltfMeshoptFilter_None, count, stride, true));
                        REQUIRE(decoded.count() == data.count());
                        CHECK(prMemEquals(decoded.data(), reference.data(), decoded.count()));
                        CHECK(prMemEquals(decoded.data(), data.data(), decoded.count()));
                        // EXT_meshopt_compression reads version 0 alone.
                        Array<uint8_t> ext = decodeGltf(stream,
                            describe(PrGltfMeshoptMode_Attributes, PrGltfMeshoptFilter_None, count, stride, false));
                        CHECK(ext.count() == (version == 0 ? data.count() : 0));
                        ++checked;
                    }
                }
            }
        }
    }
    CHECK(checked == 9 * 9 * 3 * 2 * 4);
}

TEST_CASE("a stream of attributes that is cut short, or has bytes left over, does not decode") {
    Sequence random;
    Array<uint8_t> data = elements(100, 12, Kind_Random, &random);
    Array<uint8_t> stream = encodeAttributes(data, 100, 12, 2, 1);
    PrGltfMeshopt meshopt = describe(PrGltfMeshoptMode_Attributes, PrGltfMeshoptFilter_None, 100, 12, true);
    for (uint32_t cut = 1; cut < stream.count(); cut += 7) {
        Array<uint8_t> shorter;
        shorter.append(stream.data(), stream.count() - cut);
        CHECK(decodeGltf(shorter, meshopt).count() == 0);
    }
    Array<uint8_t> longer = stream;
    longer.insert(1, 0);
    CHECK(decodeGltf(longer, meshopt).count() == 0);
    // More elements than the stream holds.
    meshopt.count = 101;
    CHECK(decodeGltf(stream, meshopt).count() == 0);
}

TEST_CASE("the indices of triangles and of sequences decode as meshoptimizer decodes them") {
    Sequence random;
    for (uint32_t triangles : kTriangleCounts) {
        // A strip of quads, whose triangles share edges and vertices as a mesh's do, and some at random.
        Array<uint32_t> indices;
        for (uint32_t t = 0; t < triangles; ++t) {
            uint32_t base = t / 2;
            if (t % 7 == 6) {
                indices.push(random.next() % 3000);
                indices.push(random.next() % 3000);
                indices.push(random.next() % 3000);
            } else if (t % 2 == 0) {
                indices.push(base);
                indices.push(base + 1);
                indices.push(base + 2);
            } else {
                indices.push(base + 2);
                indices.push(base + 1);
                indices.push(base + 3);
            }
        }
        uint32_t count = indices.count();
        for (uint32_t stride : kIndexStrides) {
            CAPTURE(triangles);
            CAPTURE(stride);
            Array<uint8_t> stream;
            stream.resize(uint32_t(meshopt_encodeIndexBufferBound(count, 3000)));
            size_t size = meshopt_encodeIndexBuffer(stream.data(), stream.count(), indices.data(), count);
            REQUIRE(size != 0);
            stream.resize(uint32_t(size));
            Array<uint8_t> reference;
            reference.resize(count * stride);
            REQUIRE(meshopt_decodeIndexBuffer(reference.data(), count, stride, stream.data(), stream.count()) == 0);
            Array<uint8_t> decoded =
                decodeGltf(stream, describe(PrGltfMeshoptMode_Triangles, PrGltfMeshoptFilter_None, count, stride, true));
            REQUIRE(decoded.count() == reference.count());
            CHECK(prMemEquals(decoded.data(), reference.data(), decoded.count()));

            stream.resize(uint32_t(meshopt_encodeIndexSequenceBound(count, 3000)));
            size = meshopt_encodeIndexSequence(stream.data(), stream.count(), indices.data(), count);
            REQUIRE(size != 0);
            stream.resize(uint32_t(size));
            REQUIRE(meshopt_decodeIndexSequence(reference.data(), count, stride, stream.data(), stream.count()) == 0);
            decoded = decodeGltf(stream, describe(PrGltfMeshoptMode_Indices, PrGltfMeshoptFilter_None, count, stride, true));
            REQUIRE(decoded.count() == reference.count());
            CHECK(prMemEquals(decoded.data(), reference.data(), decoded.count()));
            for (uint32_t i = 0; stride == 4 && i < count; ++i)
                CHECK(prMemReadLeU32(decoded.data() + i * 4) == indices[i]);

            // A stream cut short does not decode.
            stream.resize(stream.count() - 1);
            CHECK(decodeGltf(stream, describe(PrGltfMeshoptMode_Indices, PrGltfMeshoptFilter_None, count, stride, true))
                .count() == 0);
        }
    }
}

TEST_CASE("the filters decode as meshoptimizer's do, to a unit in the last place, and exponents exactly") {
    Sequence random;
    const uint32_t count = 500;
    // Unit vectors and quaternions, and floats and colors over wide ranges.
    Array<float> vectors;
    Array<float> quaternions;
    Array<float> floats;
    Array<float> colors;
    for (uint32_t i = 0; i < count; ++i) {
        float v[4] = {random.unit() * 2 - 1, random.unit() * 2 - 1, random.unit() * 2 - 1, random.unit() * 2 - 1};
        float length = prF32Sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float qlength = prF32Sqrt(length * length + v[3] * v[3]);
        for (uint32_t k = 0; k < 3; ++k)
            vectors.push(v[k] / length);
        vectors.push(i % 2 == 0 ? 1.0f : -1.0f);
        for (uint32_t k = 0; k < 4; ++k)
            quaternions.push(v[k] / qlength);
        for (uint32_t k = 0; k < 3; ++k)
            floats.push((random.unit() * 2 - 1) * float(1u << (random.next() % 20)));
        for (uint32_t k = 0; k < 4; ++k)
            colors.push(random.unit());
    }
    Array<uint8_t> encoded;
    for (uint32_t stride : kFilterStrides) {
        for (int bits : kFilterBits) {
            if (bits > int(stride) * 2)
                continue;
            CAPTURE(stride);
            CAPTURE(bits);
            encoded.resize(count * stride);
            meshopt_encodeFilterOct(encoded.data(), count, stride, bits, vectors.data());
            Filtered oct = filtered(PrGltfMeshoptFilter_Octahedral, encoded, count, stride);
            CHECK(greatestDifference(oct.gltf, oct.reference, stride / 4, true) <= 1);
            meshopt_encodeFilterColor(encoded.data(), count, stride, bits, colors.data());
            Filtered color = filtered(PrGltfMeshoptFilter_Color, encoded, count, stride);
            CHECK(greatestDifference(color.gltf, color.reference, stride / 4, false) <= 1);
        }
    }
    for (int bits : kQuaternionBits) {
        CAPTURE(bits);
        encoded.resize(count * 8);
        meshopt_encodeFilterQuat(encoded.data(), count, 8, bits, quaternions.data());
        Filtered quat = filtered(PrGltfMeshoptFilter_Quaternion, encoded, count, 8);
        CHECK(greatestDifference(quat.gltf, quat.reference, 2, true) <= 1);
    }
    for (int bits : kExponentBits) {
        for (meshopt_EncodeExpMode mode : kExponentModes) {
            CAPTURE(bits);
            CAPTURE(int(mode));
            encoded.resize(count * 12);
            meshopt_encodeFilterExp(encoded.data(), count, 12, bits, floats.data(), mode);
            Filtered exp = filtered(PrGltfMeshoptFilter_Exponential, encoded, count, 12);
            CHECK(prMemEquals(exp.gltf.data(), exp.reference.data(), exp.gltf.count()));
        }
    }
}
