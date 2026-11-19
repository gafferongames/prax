// The fixture of the glTF tests: assets written in a test's own text over one small buffer, and the documents that they
// read as.

#pragma once

#include "pr/gltf.h"
#include "pr/mem.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace pr;

// The size of the buffer that the tests' assets read.
constexpr uint32_t kGltfBinSize = 256;

// The bytes of the buffer that an asset names by the URI "bin". A test writes the values that its accessors read.
struct GltfBin {
    uint8_t bytes[kGltfBinSize] = {};

    // Writes each of `values` as a little-endian `T`, from byte `offset`.
    template <typename T, typename... Values>
    void put(uint32_t offset, Values... values) {
        const T items[] = {T(values)...};
        REQUIRE(offset + sizeof(items) <= sizeof(bytes));
        prMemCopy(bytes + offset, items, sizeof(items));
    }
};

// The members of an asset that give it the buffer "bin", buffer view 0 over all of it, and buffer view 1 over all of it
// with a stride of 16 bytes.
constexpr PrStringView kGltfBinViews =
    R"("buffers":[{"byteLength":256,"uri":"bin"}],)"
    R"("bufferViews":[{"buffer":0,"byteLength":256},{"buffer":0,"byteLength":256,"byteStride":16}])";

// The accessors of gltfQuad, which lay the buffer out as the data of a small mesh and its animation:
//
//   0  VEC3 floats, 4 at byte 0: positions, with bounds
//   1  SCALAR unsigned shorts, 6 at byte 64: indices
//   2  VEC3 floats, 4 at byte 96: normals, or the values of four keyframes
//   3  VEC2 floats, 4 at byte 144: texture coordinates
//   4  VEC4 unsigned bytes, 4 at byte 176: joints
//   5  VEC4 floats, 4 at byte 192: weights, or four rotations
//   6  SCALAR floats, 4 at byte 48: the times of four keyframes, with bounds
//   7  SCALAR unsigned shorts, 4 at byte 64: indices of a strip or a fan
//   8  VEC3 floats, 3 at byte 0: the positions of one triangle, with bounds
//   9  SCALAR floats, 2 at byte 48: the times of two keyframes, with bounds
//  10  SCALAR floats, 4 at byte 48: times with no bounds
//  11  SCALAR floats, 1 at byte 48: the time of one keyframe, with bounds
constexpr PrStringView kGltfQuadAccessors =
    R"("accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":64,"componentType":5123,"count":6,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5126,"count":4,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":144,"componentType":5126,"count":4,"type":"VEC2"},)"
    R"({"bufferView":0,"byteOffset":176,"componentType":5121,"count":4,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":192,"componentType":5126,"count":4,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":4,"type":"SCALAR","min":[0],"max":[3]},)"
    R"({"bufferView":0,"byteOffset":64,"componentType":5123,"count":4,"type":"SCALAR"},)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":4,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":1,"type":"SCALAR","min":[0],"max":[0]}])";

// Returns the JSON of an asset of glTF 2.0 with `members` as the other members of its root.
inline String gltfAsset(PrStringView members) {
    String text = R"({"asset":{"version":"2.0"})";
    if (members.size != 0)
        text.append(",").append(members);
    return text.append("}");
}

// Returns the JSON of an asset with the buffer "bin", its two buffer views, and `members`.
inline String gltfBinAsset(PrStringView members) {
    String all = kGltfBinViews;
    return gltfAsset(all.append(",").append(members));
}

// Returns the JSON of an asset with the buffer "bin", its two buffer views, the accessors of kGltfQuadAccessors, and
// `members`.
inline String gltfQuad(PrStringView members) {
    String all = kGltfQuadAccessors;
    return gltfBinAsset(all.append(",").append(members));
}

// Loads the buffer "bin" from the GltfBin that `context` points to, and nothing else.
inline PrBlob* PR_NULLABLE gltfLoadBin(void* PR_NULLABLE context, PrGltfExternal kind, PrStringView uri) {
    const GltfBin* bin = static_cast<const GltfBin*>(context);
    if (bin == nullptr || kind != PrGltfExternal_Buffer || uri != "bin")
        return nullptr;
    return prBlobCopy(PrByteView{bin->bytes, sizeof(bin->bytes)});
}

// What reading an asset gave: a document, or the status and the sentence that say why not.
struct GltfRead {
    PrGltf* PR_NULLABLE gltf = nullptr;
    PrStatus status = PrStatus_Ok;
    String diagnostics;

    // Reads the asset in `file`, JSON or binary glTF, with "bin" loaded from `bin` if there is one.
    explicit GltfRead(PrByteView file, const GltfBin* PR_NULLABLE bin = nullptr) {
        PrTextBuffer why = {};
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = gltfLoadBin;
        options.loadContext = const_cast<GltfBin*>(bin);
        options.diagnostics = &why;
        PrBlob* blob = prBlobCopy(file);
        status = prGltfCreate(blob, &options, &gltf);
        prBlobRelease(blob);
        diagnostics = prTextBufferView(&why);
        prTextBufferReset(&why);
    }
    explicit GltfRead(PrStringView json, const GltfBin* PR_NULLABLE bin = nullptr) :
            GltfRead(PrByteView{reinterpret_cast<const uint8_t*>(json.data), json.size}, bin) {}
    GltfRead(const GltfRead&) = delete;
    GltfRead& operator=(const GltfRead&) = delete;
    ~GltfRead() { prGltfDestroy(gltf); }
};

// Returns the sentence with which reading `json` fails as InvalidData. A test compares it with the sentence that names
// the property and the rule that it broke.
inline String gltfRefusal(PrStringView json, const GltfBin* PR_NULLABLE bin = nullptr) {
    GltfRead read(json, bin);
    CHECK(read.status == PrStatus_InvalidData);
    CHECK(read.gltf == nullptr);
    return read.diagnostics;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
