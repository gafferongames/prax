// The fixture of the model tests: small glTF assets written in a test's own text over one buffer of a quad and its
// animation, with images that the fixture encodes.

#pragma once

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/compress_select.h"
#include "pr/gltf.h"
#include "pr/gltf_model.h"
#include "pr/image.h"
#include "pr/image_select.h"
#include "pr/image_texture_source.h"
#include "pr/ktx_texture_source.h"
#include "pr/mem.h"
#include "pr/model.h"
#include "pr/string.h"
#include "pr/text_buffer.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace pr;

// The size of the buffer that the tests' assets read.
constexpr uint32_t kModelBinSize = 400;

// The accessors of every asset of the tests, over the buffer "bin". The quad is the square of side 2 about the origin
// in the plane z = 0, which faces along z. Its vertices are at (-1, -1), (1, -1), (-1, 1) and (1, 1).
//
//   0  VEC3 floats, 4 at byte 0: the quad's positions, with bounds
//   1  SCALAR unsigned bytes, 6 at byte 48: the quad as a triangle list
//   2  VEC2 floats, 4 at byte 56: texture coordinates that show an image upright on the quad
//   3  SCALAR floats, 2 at byte 88: the times 0 and 1, with bounds
//   4  VEC3 floats, 2 at byte 96: the origin and (4, 0, 0), as the values of two keyframes
//   5  VEC3 floats, 4 at byte 120: normals along z
//   6  VEC4 floats, 4 at byte 168: tangents along y with a handedness of -1
//   7  SCALAR unsigned shorts, 4 at byte 232: the quad as a triangle strip
//   8  SCALAR unsigned shorts, 4 at byte 240: the quad as a triangle fan
//   9  VEC2 floats, 4 at byte 248: texture coordinates that show an image mirrored from left to right
//  10  VEC3 floats, 4 at byte 0: the quad's positions, with bounds that reach 1 along z
//  11  VEC4 floats, 4 at byte 168: the bytes of the tangents as colors, or as weights
//  12  VEC4 floats, 2 at byte 280: no rotation and a quarter turn about z, as the values of two keyframes
//  13  VEC4 unsigned bytes, 4 at byte 312: joints, all zero
//  14  VEC3 floats, 3 at byte 0: the positions of the quad's first triangle, with bounds
//  15  VEC3 floats, 3 at byte 120: the normals of that triangle
//  16  MAT4 floats, 1 at byte 336: the matrix that moves by (1, 0, 0), by columns
constexpr PrStringView kModelAccessors =
    R"("buffers":[{"byteLength":400,"uri":"bin"}],"bufferViews":[{"buffer":0,"byteLength":400}],"accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5121,"count":6,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":56,"componentType":5126,"count":4,"type":"VEC2"},)"
    R"({"bufferView":0,"byteOffset":88,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5126,"count":2,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":120,"componentType":5126,"count":4,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":168,"componentType":5126,"count":4,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":232,"componentType":5123,"count":4,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":240,"componentType":5123,"count":4,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":248,"componentType":5126,"count":4,"type":"VEC2"},)"
    R"({"bufferView":0,"componentType":5126,"count":4,"type":"VEC3","min":[-1,-1,0],"max":[1,1,1]},)"
    R"({"bufferView":0,"byteOffset":168,"componentType":5126,"count":4,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":280,"componentType":5126,"count":2,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":312,"componentType":5121,"count":4,"type":"VEC4"},)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":120,"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":336,"componentType":5126,"count":1,"type":"MAT4"}])";

// The bytes of the buffer "bin".
struct ModelBin {
    uint8_t bytes[kModelBinSize] = {};

    template <typename T, typename... Values>
    void put(uint32_t offset, Values... values) {
        const T items[] = {T(values)...};
        REQUIRE(offset + sizeof(items) <= sizeof(bytes));
        prMemCopy(bytes + offset, items, sizeof(items));
    }

    ModelBin() {
        put<float>(0, -1, -1, 0, 1, -1, 0, -1, 1, 0, 1, 1, 0);
        put<uint8_t>(48, 0, 1, 2, 2, 1, 3);
        put<float>(56, 0, 1, 1, 1, 0, 0, 1, 0);
        put<float>(88, 0, 1);
        put<float>(96, 0, 0, 0, 4, 0, 0);
        put<float>(120, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1);
        put<float>(168, 0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1, 0, 1, 0, -1);
        put<uint16_t>(232, 0, 1, 2, 3);
        put<uint16_t>(240, 0, 1, 3, 2);
        put<float>(248, 1, 1, 0, 1, 1, 0, 0, 0);
        put<float>(280, 0, 0, 0, 1, 0, 0, 0.70710678f, 0.70710678f);
        put<float>(336, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 1, 0, 0, 1);
    }
};

// An image that an asset names by a URI.
struct ModelFile {
    String uri;
    Array<uint8_t> bytes;
};

// The codecs that the tests decode and encode images with, and the opener of the textures of a model's images, of KTX
// 2.0 and of what the image codec decodes.
struct ModelCodecs {
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    PrImageCodecSet* images = prImageCodecSetCreateDefault(compress);
    PrTextureOpener* textures = nullptr;

    ModelCodecs() {
        const PrTextureFormat formats[2] = {prKtxTextureFormat(compress), prImageTextureFormat(images)};
        textures = prTextureOpenerCreate(formats, 2);
    }
    ModelCodecs(const ModelCodecs&) = delete;
    ModelCodecs& operator=(const ModelCodecs&) = delete;
    ~ModelCodecs() {
        prTextureOpenerDestroy(textures);
        prImageCodecSetDestroy(images);
        prCompressCodecSetDestroy(compress);
    }

    // Returns a PNG of `extent` texels of four 8-bit channels each.
    Array<uint8_t> png(PrUVec2 extent, const uint8_t* texels) const {
        PrPixelView pixels{texels, size_t(extent.x) * 4, extent};
        size_t size = 0;
        prImageCodecSetEncode(images, "image/png", pixels, nullptr, 0, &size);
        Array<uint8_t> encoded;
        encoded.resize(uint32_t(size));
        REQUIRE(prImageCodecSetEncode(images, "image/png", pixels, encoded.begin(), size, &size) == PrStatus_Ok);
        return encoded;
    }
};

// Returns a KTX 2.0 texture of `extent` texels whose `levelCount` levels hold the bytes at `data` one after another,
// `sizes` of them each: of `vkFormat`, or of Basis Universal's UASTC with red, green, blue and alpha where it is zero.
// Its descriptor states sRGB values if `srgb`, and linear values otherwise.
inline Array<uint8_t> modelKtx(uint32_t vkFormat, PrUVec2 extent, uint32_t levelCount, const uint8_t* data,
        const uint32_t* sizes, bool srgb = false) {
    Array<uint8_t> file;
    auto word = [&](uint32_t value) {
        for (int k = 0; k < 4; ++k)
            file.push(uint8_t(value >> (8 * k)));
    };
    const uint8_t identifier[12] = {0xAB, 'K', 'T', 'X', ' ', '2', '0', 0xBB, '\r', '\n', 0x1A, '\n'};
    for (uint8_t byte : identifier)
        file.push(byte);
    uint32_t dfdOffset = 80 + 24 * levelCount;
    // The format, its type size, the extent, no depth, layers or metadata, one face, the levels, no supercompression,
    // and where the descriptor is.
    const uint32_t header[] = {
        vkFormat, 1, extent.x, extent.y, 0, 0, 1, levelCount, 0, dfdOffset, 44, 0, 0, 0, 0, 0, 0,
    };
    for (uint32_t value : header)
        word(value);
    uint32_t at = dfdOffset + 44;
    for (uint32_t level = 0; level < levelCount; ++level) {
        const uint32_t entry[] = {at, 0, sizes[level], 0, sizes[level], 0};
        for (uint32_t value : entry)
            word(value);
        at += sizes[level];
    }
    // A descriptor of one sample: of the color model RGBSDA, or UASTC's of blocks of 4 by 4 texels in 16 bytes, of
    // BT.709's primaries, and of sRGB's transfer function or the linear one.
    uint32_t transfer = srgb ? 2 : 1;
    bool uastc = vkFormat == 0;
    const uint32_t descriptor[] = {
        44, 0, 2 | 40u << 16, (uastc ? 166u : 1u) | 1u << 8 | transfer << 16, uastc ? 3u | 3u << 8 : 0u,
        uastc ? 16u : 0u, 0, uastc ? 127u << 16 | 3u << 24 : 0u, 0, 0, 0,
    };
    for (uint32_t value : descriptor)
        word(value);
    for (uint32_t i = 0; i < at - dfdOffset - 44; ++i)
        file.push(data[i]);
    return file;
}

// Returns the block of UASTC of one color, `rgba`.
inline void modelUastcSolid(const uint8_t* rgba, uint8_t* block) {
    uint64_t bits = 0x17 | uint64_t(rgba[0]) << 5 | uint64_t(rgba[1]) << 13 | uint64_t(rgba[2]) << 21 |
        uint64_t(rgba[3]) << 29;
    for (int k = 0; k < 16; ++k)
        block[k] = k < 8 ? uint8_t(bits >> (8 * k)) : 0;
}

// An asset of the tests, read into a document: glTF 2.0 with kModelAccessors and the members that a test gives.
struct ModelAsset {
    ModelBin bin;
    Array<ModelFile> files;
    bool hasBin = true;         // "bin" has bytes
    PrGltf* PR_NULLABLE gltf = nullptr;
    String diagnostics;

    ModelAsset() = default;
    ModelAsset(const ModelAsset&) = delete;
    ModelAsset& operator=(const ModelAsset&) = delete;
    ~ModelAsset() { prGltfDestroy(gltf); }

    static PrBlob* PR_NULLABLE load(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
        const ModelAsset* asset = static_cast<const ModelAsset*>(context);
        if (uri == "bin" && asset->hasBin)
            return prBlobCopy(PrByteView{asset->bin.bytes, sizeof(asset->bin.bytes)});
        for (const ModelFile& file : asset->files) {
            if (file.uri == uri)
                return prBlobCopy(PrByteView{file.bytes.begin(), file.bytes.count()});
        }
        return nullptr;
    }

    // Reads the asset with `members` as the other members of its root, and returns the document.
    const PrGltf* read(PrStringView members) {
        String text = R"({"asset":{"version":"2.0"},)";
        text.append(kModelAccessors);
        if (members.size != 0)
            text.append(",").append(members);
        text.append("}");
        PrTextBuffer why = {};
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = load;
        options.loadContext = this;
        options.diagnostics = &why;
        PrStringView json = text;
        PrBlob* blob = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(json.data), json.size});
        prGltfDestroy(gltf);
        PrStatus status = prGltfCreate(blob, &options, &gltf);
        prBlobRelease(blob);
        diagnostics = prTextBufferView(&why);
        prTextBufferReset(&why);
        CAPTURE(diagnostics);
        REQUIRE(status == PrStatus_Ok);
        return gltf;
    }
};

// Returns a model of a document, through glTF's model source of the scene `scene`, which the caller destroys.
inline PrModel* modelOf(const PrGltf* gltf, const PrModelOptions* PR_NULLABLE options = nullptr,
        PrGltfSceneIndex scene = kPrGltfNone) {
    PrGltfModelOptions source = prGltfModelOptionsDefault();
    source.scene = scene;
    return prModelCreate(prGltfModelSourceCreate(gltf, &source), options);
}

// Returns true if the model reports each of `features` (prModelDiagnostic), and no other.
inline bool hasDiagnostics(const PrModel* model, Span<const PrStringView> features) {
    if (prModelDiagnosticCount(model) != features.count())
        return false;
    for (PrStringView feature : features) {
        if (!prModelHasDiagnostic(model, feature))
            return false;
    }
    return true;
}

// The members of an asset whose mesh 0 is the quad as a triangle list with normals and texture coordinates, drawn with
// material 0, and whose scene is node 0, which draws the mesh. `materials` are the asset's materials, as the items of a
// JSON array, and `more` are further members, each followed by a comma.
inline String modelQuad(PrStringView materials, PrStringView more = "") {
    String members(more);
    members.append(R"("extensionsUsed":["KHR_materials_unlit","KHR_lights_punctual"],"scene":0,)");
    members.append(R"("scenes":[{"nodes":[0]}],)");
    members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5,"TEXCOORD_0":2},)");
    members.append(R"("indices":1,"material":0}]}],"materials":[)").append(materials).append("]");
    return members;
}

// A material that no light changes, of a color, as an item of a JSON array.
constexpr PrStringView kModelUnlitRed =
    R"({"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]},"extensions":{"KHR_materials_unlit":{}}})";

} // namespace pr_impl

PR_ASSUME_NONNULL_END
