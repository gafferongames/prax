// KTX 2.0 textures: the reference encoder's of Basis Universal's formats, and textures written here of pixels and of
// blocks, read as pixels, as blocks and transcoded to blocks.

#include "pr/array.h"
#include "pr/basis.h"
#include "pr/compress.h"
#include "pr/compress_select.h"
#include "pr/ktx.h"
#include "pr/mem.h"

#include "ktx/tests/ktx_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestKtxCApi(void);

namespace pr_impl {

namespace {

// Returns the bytes of one pixel that a texture of `info` decodes to.
uint32_t pixelBytes(const PrKtxInfo& info) {
    const uint32_t kSampleBytes[] = {1, 2, 2, 4};
    return info.channels * kSampleBytes[info.sample];
}

// Creates the texture in `bytes` and prepares it. Returns the status of the creation, or of the preparation if that
// fails. `*out` is the texture if the status is Ok or Truncated, and null otherwise.
PrStatus textureOpen(PrByteView bytes, PrKtxTexture* PR_NULLABLE* PR_NONNULL out) {
    PrStatus status = prKtxTextureCreate(bytes, prCompressCodecSetCreateDefault(), out);
    if (*out == nullptr)
        return status;
    PrStatus prepared = prKtxTexturePrepare(*out);
    if (prepared != PrStatus_Ok) {
        prKtxTextureDestroy(*out);
        *out = nullptr;
        return prepared;
    }
    return status;
}

// Returns the texture in `size` bytes at `file`, opened and prepared, or null if it does not open.
PrKtxTexture* PR_NULLABLE textureOpen(const uint8_t* file, size_t size) {
    PrKtxTexture* texture = nullptr;
    textureOpen(PrByteView{file, size}, &texture);
    return texture;
}

// A decoded level, in the texture's own format, and how reading it ended.
struct Level {
    PrStatus status;
    Array<uint8_t> pixels;
};

Level readLevel(PrKtxTexture* texture, uint32_t frame, uint32_t level) {
    const PrKtxInfo& info = *prKtxTextureInfo(texture);
    PrUVec2 extent = prKtxLevelExtent(info.extent, level);
    uint32_t bytes = pixelBytes(info);
    Level out;
    out.pixels.resize(extent.x * extent.y * bytes);
    out.status = prKtxTextureRead(texture, frame, level, PrPixels{out.pixels.data(), size_t(extent.x) * bytes, extent});
    return out;
}

// Opens `file`, which must open, and reads the first level of its first frame.
Level readFirst(const Array<uint8_t>& file, PrKtxInfo* info) {
    PrKtxTexture* texture = textureOpen(file.data(), file.count());
    REQUIRE(texture != nullptr);
    *info = *prKtxTextureInfo(texture);
    Level level = readLevel(texture, 0, 0);
    prKtxTextureDestroy(texture);
    return level;
}

PrStatus openStatus(const Array<uint8_t>& file) {
    PrKtxTexture* texture = nullptr;
    PrStatus status = textureOpen(PrByteView{file.data(), file.count()}, &texture);
    prKtxTextureDestroy(texture);
    return status;
}

// Checks that every level of the texture in `file` decodes to its part of `expected`.
void checkLevels(const uint8_t* file, size_t size, const uint8_t* expected) {
    PrKtxTexture* texture = textureOpen(file, size);
    REQUIRE(texture != nullptr);
    PrKtxInfo info = *prKtxTextureInfo(texture);
    CHECK(info.extent.x == 12);
    CHECK(info.extent.y == 8);
    CHECK(info.levelCount == 4);
    CHECK(info.layerCount == 1);
    CHECK(info.faceCount == 1);
    CHECK(info.blocks == PrBasisBlocks_None);
    CHECK(info.channels == 4);
    CHECK(info.sample == PrKtxSample_U8);
    CHECK(info.alpha == PrKtxAlpha_Straight);
    CHECK(info.color == PrKtxColor_SrgbLinear);
    for (uint32_t level = 0; level < info.levelCount; ++level) {
        CAPTURE(level);
        Level decoded = readLevel(texture, 0, level);
        CHECK(decoded.status == PrStatus_Ok);
        CHECK(prMemEquals(decoded.pixels.data(), expected, decoded.pixels.count()));
        expected += decoded.pixels.count();
    }
    prKtxTextureDestroy(texture);
}

// Returns true if a test reads the texture: one of at most a million texels, which a changed byte may make far larger.
bool readable(const PrKtxTexture* PR_NULLABLE texture) {
    return texture != nullptr &&
        uint64_t(prKtxTextureInfo(texture)->extent.x) * prKtxTextureInfo(texture)->extent.y <= uint64_t(1) << 20;
}

// Opens every prefix of `file` and `file` with each of its bytes changed, and reads what opens. Nothing is checked but
// that each ends, within the bounds of its storage.
void checkHostile(const uint8_t* file, size_t size) {
    Array<uint8_t> bytes = bytesOf(file, size);
    for (size_t cut = 0; cut < size; ++cut) {
        PrKtxTexture* texture = textureOpen(file, cut);
        for (uint32_t level = 0; readable(texture) && level < prKtxTextureInfo(texture)->levelCount; ++level)
            readLevel(texture, 0, level);
        prKtxTextureDestroy(texture);
    }
    const uint8_t kFlips[] = {0x01, 0x80, 0xff};
    for (uint32_t at = 0; at < bytes.count(); ++at) {
        for (uint8_t flip : kFlips) {
            bytes[at] ^= flip;
            PrKtxTexture* texture = textureOpen(bytes.data(), bytes.count());
            for (uint32_t level = 0; readable(texture) && level < prKtxTextureInfo(texture)->levelCount; ++level)
                readLevel(texture, 0, level);
            prKtxTextureDestroy(texture);
            bytes[at] ^= flip;
        }
    }
}

constexpr KtxSpec kRgba8{.vkFormat = 37, .extent = {2, 2}, .levelCount = 1, .faceCount = 1, .model = 1, .transfer = 1};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("ktx decodes the levels of an ETC1S texture as the reference transcoder does") {
    checkLevels(kKtxEtc1s, sizeof(kKtxEtc1s), kKtxEtc1sPixels);
}

TEST_CASE("ktx decodes the levels of a UASTC texture as the reference transcoder does") {
    checkLevels(kKtxUastc, sizeof(kKtxUastc), kKtxUastcPixels);
}

TEST_CASE("ktx is recognized by its identifier") {
    CHECK(prKtxHasSignature(PrByteView{kKtxEtc1s, sizeof(kKtxEtc1s)}));
    CHECK(!prKtxHasSignature(PrByteView{kKtxEtc1s, 11}));
}

TEST_CASE("ktx survives truncated and corrupt textures") {
    checkHostile(kKtxEtc1s, sizeof(kKtxEtc1s));
    checkHostile(kKtxUastc, sizeof(kKtxUastc));
}

TEST_CASE("ktx passes pixels through, in the channels that the image's format has") {
    const uint8_t rgba[16] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16};
    PrKtxInfo info;
    Level level = readFirst(buildKtx(kRgba8, bytesOf(rgba, 16)), &info);
    CHECK(level.status == PrStatus_Ok);
    CHECK(info.alpha == PrKtxAlpha_Straight);
    CHECK(info.color == PrKtxColor_SrgbLinear);
    CHECK(!info.fromRight);
    CHECK(!info.fromBottom);
    CHECK(prMemEquals(level.pixels.data(), rgba, 16));

    // Blue first, sRGB, premultiplied, and stored from the bottom right.
    KtxSpec bgra = kRgba8;
    bgra.vkFormat = 50;
    bgra.transfer = 2;
    bgra.flags = 1;
    bgra.orientation = "lu";
    level = readFirst(buildKtx(bgra, bytesOf(rgba, 16)), &info);
    const uint8_t swapped[16] = {3, 2, 1, 4, 7, 6, 5, 8, 11, 10, 9, 12, 15, 14, 13, 16};
    CHECK(info.alpha == PrKtxAlpha_Premultiplied);
    CHECK(info.color == PrKtxColor_Srgb);
    CHECK(info.fromRight);
    CHECK(info.fromBottom);
    CHECK(prMemEquals(level.pixels.data(), swapped, 16));

    // Two channels are red and green, and one is gray.
    KtxSpec rg = kRgba8;
    rg.vkFormat = 16;
    level = readFirst(buildKtx(rg, bytesOf(rgba, 8)), &info);
    const uint8_t pairs[12] = {1, 2, 0, 3, 4, 0, 5, 6, 0, 7, 8, 0};
    CHECK(info.channels == 3);
    CHECK(info.alpha == PrKtxAlpha_None);
    CHECK(prMemEquals(level.pixels.data(), pairs, 12));
    KtxSpec gray = kRgba8;
    gray.vkFormat = 70;
    level = readFirst(buildKtx(gray, bytesOf(rgba, 8)), &info);
    CHECK(info.channels == 1);
    CHECK(info.sample == PrKtxSample_U16);
    CHECK(prMemEquals(level.pixels.data(), rgba, 8));
}

TEST_CASE("ktx inflates levels, and reads the faces of a cube and the layers of an array as frames") {
    // Two levels of six faces of two layers, each a texel, or four, of one byte that names it. The first level is a
    // zlib stream.
    Array<uint8_t> levels[2];
    Array<uint8_t> first;
    for (uint8_t frame = 0; frame < 12; ++frame) {
        for (int texel = 0; texel < 4; ++texel)
            first.push(uint8_t(frame * 4 + texel));
        levels[1].push(uint8_t(100 + frame));
    }
    PrCompressCodecSet* compress = prCompressCodecSetCreateDefault();
    levels[0].resize(uint32_t(prCompressCodecSetEncodeBound(compress, PrCompressFormat_Zlib, first.count())));
    size_t written = 0;
    REQUIRE(prCompressCodecSetEncode(compress, PrCompressFormat_Zlib, PrCompressLevel_Default,
        PrByteView{first.data(), first.count()}, levels[0].data(), &written) == PrStatus_Ok);
    levels[0].resize(uint32_t(written));
    Array<uint8_t> second;
    second.resize(uint32_t(prCompressCodecSetEncodeBound(compress, PrCompressFormat_Zlib, 12)));
    REQUIRE(prCompressCodecSetEncode(compress, PrCompressFormat_Zlib, PrCompressLevel_Default,
        PrByteView{levels[1].data(), 12}, second.data(), &written) == PrStatus_Ok);
    second.resize(uint32_t(written));
    levels[1] = second;
    KtxSpec spec = kRgba8;
    spec.vkFormat = 9;
    spec.levelCount = 2;
    spec.layerCount = 2;
    spec.faceCount = 6;
    spec.scheme = 3;
    const uint64_t inflated[2] = {48, 12};
    Array<uint8_t> file = buildKtx(spec, levels, inflated);
    PrKtxTexture* texture = textureOpen(file.data(), file.count());
    REQUIRE(texture != nullptr);
    const PrKtxInfo& info = *prKtxTextureInfo(texture);
    CHECK(info.layerCount == 2);
    CHECK(info.faceCount == 6);
    CHECK(info.levelCount == 2);
    CHECK(info.inflatedBytes == 48);
    for (uint32_t frame = 0; frame < 12; ++frame) {
        CAPTURE(frame);
        Level level = readLevel(texture, frame, 0);
        CHECK(level.status == PrStatus_Ok);
        CHECK(level.pixels[0] == frame * 4);
        CHECK(level.pixels[3] == frame * 4 + 3);
        CHECK(readLevel(texture, frame, 1).pixels[0] == 100 + frame);
    }
    prKtxTextureDestroy(texture);
    // A level that inflates to another size than its texture gives it.
    file[80 + 16] = 47;
    CHECK(openStatus(file) == PrStatus_InvalidData);
}

TEST_CASE("ktx hands over the blocks of a format that a GPU samples, and has no pixels of blocks that it does not decode") {
    Array<uint8_t> blocks;
    for (uint8_t i = 0; i < 32; ++i)
        blocks.push(i);
    KtxSpec spec = kRgba8;
    spec.vkFormat = 158;
    spec.extent = {8, 4};
    spec.transfer = 2;
    Array<uint8_t> file = buildKtx(spec, blocks);
    PrKtxTexture* texture = textureOpen(file.data(), file.count());
    REQUIRE(texture != nullptr);
    const PrKtxInfo& info = *prKtxTextureInfo(texture);
    CHECK(info.payload == PrKtxPayload_Blocks);
    CHECK(info.blocks == PrBasisBlocks_Astc4x4);
    CHECK_FALSE(info.blocksDecode);
    CHECK(info.color == PrKtxColor_Srgb);
    CHECK(prKtxTextureBlocksSize(texture, 0) == 32);
    uint8_t out[32] = {};
    CHECK(prKtxTextureReadBlocks(texture, 0, 0, out, 32) == PrStatus_Ok);
    CHECK(prMemEquals(out, blocks.data(), 32));
    uint8_t pixels[8 * 4 * 4];
    CHECK(prKtxTextureRead(texture, 0, 0, PrPixels{pixels, 32, {8, 4}}) == PrStatus_NotSupported);
    prKtxTextureDestroy(texture);
    texture = textureOpen(kKtxUastc, sizeof(kKtxUastc));
    REQUIRE(texture != nullptr);
    CHECK(prKtxTextureBlocksSize(texture, 0) == 0);
    CHECK(prKtxTextureReadBlocks(texture, 0, 0, out, 0) == PrStatus_NotSupported);
    prKtxTextureDestroy(texture);
}

TEST_CASE("ktx passes through the blocks of every format of BC, ETC2, EAC and ASTC, and refuses any other format") {
    struct Format {
        uint32_t vkFormat;
        PrBasisBlocks blocks;
        PrUVec2 block;
        uint32_t bytes;
    };
    const Format kFormats[] = {
        {131, PrBasisBlocks_Bc1, {4, 4}, 8}, {134, PrBasisBlocks_Bc1, {4, 4}, 8}, {135, PrBasisBlocks_Bc2, {4, 4}, 16},
        {136, PrBasisBlocks_Bc2, {4, 4}, 16}, {137, PrBasisBlocks_Bc3, {4, 4}, 16}, {139, PrBasisBlocks_Bc4, {4, 4}, 8},
        {140, PrBasisBlocks_Bc4Snorm, {4, 4}, 8}, {141, PrBasisBlocks_Bc5, {4, 4}, 16},
        {142, PrBasisBlocks_Bc5Snorm, {4, 4}, 16}, {143, PrBasisBlocks_Bc6HUfloat, {4, 4}, 16},
        {144, PrBasisBlocks_Bc6HSfloat, {4, 4}, 16}, {145, PrBasisBlocks_Bc7, {4, 4}, 16},
        {147, PrBasisBlocks_Etc2Rgb8, {4, 4}, 8}, {148, PrBasisBlocks_Etc2Rgb8, {4, 4}, 8},
        {149, PrBasisBlocks_Etc2Rgb8A1, {4, 4}, 8}, {152, PrBasisBlocks_Etc2Rgba8, {4, 4}, 16},
        {153, PrBasisBlocks_EacR11, {4, 4}, 8}, {154, PrBasisBlocks_EacR11Snorm, {4, 4}, 8},
        {155, PrBasisBlocks_EacRg11, {4, 4}, 16}, {156, PrBasisBlocks_EacRg11Snorm, {4, 4}, 16},
        {157, PrBasisBlocks_Astc4x4, {4, 4}, 16}, {160, PrBasisBlocks_Astc5x4, {5, 4}, 16},
        {167, PrBasisBlocks_Astc8x5, {8, 5}, 16}, {173, PrBasisBlocks_Astc10x5, {10, 5}, 16},
        {183, PrBasisBlocks_Astc12x12, {12, 12}, 16}, {184, PrBasisBlocks_Astc12x12, {12, 12}, 16},
        {1000066000, PrBasisBlocks_Astc4x4Hdr, {4, 4}, 16}, {1000066008, PrBasisBlocks_Astc10x5Hdr, {10, 5}, 16},
        {1000066013, PrBasisBlocks_Astc12x12Hdr, {12, 12}, 16},
    };
    for (const Format& format : kFormats) {
        CAPTURE(format.vkFormat);
        CHECK(prBasisBlocksExtent(format.blocks).x == format.block.x);
        CHECK(prBasisBlocksExtent(format.blocks).y == format.block.y);
        CHECK(prBasisBlocksBytes(format.blocks) == format.bytes);
        // Three blocks by two, the last of each row and the last row cut short.
        KtxSpec spec = kRgba8;
        spec.vkFormat = format.vkFormat;
        spec.extent = {format.block.x * 2 + 1, format.block.y + 1};
        Array<uint8_t> blocks;
        for (uint32_t i = 0; i < format.bytes * 6; ++i)
            blocks.push(uint8_t(i * 7 + format.vkFormat));
        CHECK(prBasisBlocksSize(spec.extent, format.blocks) == blocks.count());
        Array<uint8_t> file = buildKtx(spec, blocks);
        PrKtxTexture* texture = textureOpen(file.data(), file.count());
        REQUIRE(texture != nullptr);
        CHECK(prKtxTextureInfo(texture)->blocks == format.blocks);
        CHECK(prKtxTextureBlocksSize(texture, 0) == blocks.count());
        Array<uint8_t> out(blocks.count());
        CHECK(prKtxTextureReadBlocks(texture, 0, 0, out.data(), out.count()) == PrStatus_Ok);
        CHECK(prMemEquals(out.data(), blocks.data(), blocks.count()));
        prKtxTextureDestroy(texture);
        // A level that is not whole blocks is not the texture's.
        blocks.push(0);
        CHECK(openStatus(buildKtx(spec, blocks)) == PrStatus_InvalidData);
    }
    // A format that is neither pixels nor blocks that the reader knows: PVRTC, and packed pixels.
    KtxSpec other = kRgba8;
    uint8_t texels[16] = {};
    other.vkFormat = 1000054000;
    CHECK(openStatus(buildKtx(other, bytesOf(texels, 16))) == PrStatus_NotSupported);
    other.vkFormat = 2;
    CHECK(openStatus(buildKtx(other, bytesOf(texels, 16))) == PrStatus_NotSupported);
    other.vkFormat = 185;
    CHECK(openStatus(buildKtx(other, bytesOf(texels, 16))) == PrStatus_NotSupported);
}

TEST_CASE("ktx decodes the blocks of BC and of ETC2 to pixels, in the channels that each format has") {
    struct Case {
        uint32_t vkFormat;
        uint8_t block[16];
        uint32_t size;
        uint32_t pixelBytes;
        uint8_t pixel[4];
    };
    const Case kCases[] = {
        // BC1, both colors red. BC4 and BC5, both values of each the same.
        {131, {0x00, 0xF8, 0x00, 0xF8}, 8, 3, {255, 0, 0}},
        {133, {0x00, 0xF8, 0x00, 0xF8}, 8, 4, {255, 0, 0, 255}},
        {139, {100, 100}, 8, 1, {100}},
        {141, {100, 100, 0, 0, 0, 0, 0, 0, 30, 30}, 16, 3, {100, 30, 0}},
        // BC2, alpha of four bits a texel. BC3, alpha as BC4.
        {135, {0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x00, 0xF8, 0x00, 0xF8}, 16, 4, {255, 0, 0, 119}},
        {137, {77, 77, 0, 0, 0, 0, 0, 0, 0x00, 0xF8, 0x00, 0xF8}, 16, 4, {255, 0, 0, 77}},
        // BC7, mode 6, both ends white and opaque.
        {145, {0xC0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01}, 16, 4, {255, 255, 255, 255}},
        // ETC2, two colors of four bits a channel, each red, and the first modifier of the first table, which adds 2.
        {147, {0xFF, 0x00, 0x00, 0x00}, 8, 3, {255, 2, 2}},
        {149, {0xF8, 0x00, 0x00, 0x02}, 8, 4, {255, 2, 2, 255}},
        // EAC, a base and the first modifier of the first table, -3, times one: of alpha, and of 11 bits.
        {151, {200, 0x10, 0, 0, 0, 0, 0, 0, 0xFF, 0x00, 0x00, 0x00}, 16, 4, {255, 2, 2, 197}},
        {153, {128, 0x00}, 8, 1, {128}},
        {155, {128, 0x00, 0, 0, 0, 0, 0, 0, 64, 0x00}, 16, 3, {128, 64, 0}},
    };
    for (const Case& test : kCases) {
        CAPTURE(test.vkFormat);
        KtxSpec spec = kRgba8;
        spec.vkFormat = test.vkFormat;
        spec.extent = {4, 4};
        PrKtxInfo info{};
        Level level = readFirst(buildKtx(spec, bytesOf(test.block, test.size)), &info);
        CHECK(info.blocksDecode);
        CHECK(pixelBytes(info) == test.pixelBytes);
        REQUIRE(level.status == PrStatus_Ok);
        for (uint32_t i = 0; i < 16; ++i)
            CHECK(prMemEquals(&level.pixels[i * test.pixelBytes], test.pixel, test.pixelBytes));
    }
}

TEST_CASE("ktx transcodes UASTC to blocks of BC7 and ASTC 4x4, and nothing that it has no blocks for") {
    PrKtxTexture* texture = textureOpen(kKtxUastc, sizeof(kKtxUastc));
    REQUIRE(texture != nullptr);
    const PrKtxInfo& info = *prKtxTextureInfo(texture);
    CHECK(info.transcodes == (uint64_t(1) << PrBasisBlocks_Bc7 | uint64_t(1) << PrBasisBlocks_Astc4x4));
    CHECK(prBasisBlocksSize({12, 8}, PrBasisBlocks_Bc7) == 96);
    CHECK(prBasisBlocksSize({1, 1}, PrBasisBlocks_Bc1) == 8);
    CHECK(prBasisBlocksSize({12, 8}, PrBasisBlocks_None) == 0);
    uint8_t blocks[96] = {};
    for (uint32_t level = 0; level < info.levelCount; ++level) {
        CAPTURE(level);
        size_t size = prBasisBlocksSize(prKtxLevelExtent(info.extent, level), PrBasisBlocks_Bc7);
        CHECK(prKtxTextureTranscode(texture, 0, level, PrBasisBlocks_Bc7, blocks, size) == PrStatus_Ok);
        // A block of BC7 begins with the bit of its mode, of which there are eight.
        CHECK(blocks[0] != 0);
    }
    // A block of ASTC 4x4 of one subset or more, of one plane or two, has a block mode of which bits 0 and 1 are not
    // both zero, or is a block of one color.
    REQUIRE(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_Astc4x4, blocks, 96) == PrStatus_Ok);
    for (uint32_t block = 0; block < 6; ++block)
        CHECK(((blocks[block * 16] & 3) != 0 || prMemReadLeU16(blocks + block * 16) == 0xFDFC));
    CHECK(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_Bc1, blocks, 48) == PrStatus_NotSupported);
    prKtxTextureDestroy(texture);
}

TEST_CASE("ktx transcodes ETC1S with alpha to blocks of ETC2 RGBA8: its own colors, and alpha that is near") {
    PrKtxTexture* texture = textureOpen(kKtxEtc1s, sizeof(kKtxEtc1s));
    REQUIRE(texture != nullptr);
    uint8_t blocks[96] = {};
    REQUIRE(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_Etc2Rgba8, blocks, 96) == PrStatus_Ok);
    uint32_t worst = 0;
    for (uint32_t block = 0; block < 6; ++block) {
        uint8_t texels[64];
        prBasisBlockDecode(PrBasisBlocks_Etc2Rgba8, blocks + block * 16, texels);
        for (uint32_t i = 0; i < 16; ++i) {
            uint32_t x = block % 3 * 4 + i % 4;
            uint32_t y = block / 3 * 4 + i / 4;
            const uint8_t* pixel = &kKtxEtc1sPixels[(y * 12 + x) * 4];
            // ETC1S is blocks of ETC1, which ETC2 holds as they are.
            CHECK(prMemEquals(texels + i * 4, pixel, 3));
            uint32_t alpha = texels[i * 4 + 3];
            uint32_t error = alpha > pixel[3] ? alpha - pixel[3] : pixel[3] - alpha;
            worst = error > worst ? error : worst;
        }
    }
    // EAC's values lie within a few steps of the four that a block of ETC1S has.
    CHECK(worst <= 4);
    // A texture of color and alpha has neither one value nor two.
    CHECK(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_EacR11, blocks, 48) == PrStatus_NotSupported);
    CHECK(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_EacRg11, blocks, 96) == PrStatus_NotSupported);
    prKtxTextureDestroy(texture);
}

TEST_CASE("ktx transcodes ETC1S with alpha to blocks of BC3, whose colors and alpha are near the pixels") {
    PrKtxTexture* texture = textureOpen(kKtxEtc1s, sizeof(kKtxEtc1s));
    REQUIRE(texture != nullptr);
    const PrKtxInfo& info = *prKtxTextureInfo(texture);
    CHECK(info.transcodes == (1u << PrBasisBlocks_Bc3 | 1u << PrBasisBlocks_Etc2Rgba8));
    uint8_t blocks[96] = {};
    REQUIRE(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_Bc3, blocks, 96) == PrStatus_Ok);
    uint32_t worst[2] = {0, 0};
    for (uint32_t block = 0; block < 6; ++block) {
        uint8_t texels[64];
        prBasisBlockDecode(PrBasisBlocks_Bc3, blocks + block * 16, texels);
        // BC3's color has four colors, its first end the greater.
        CHECK(prMemReadLeU16(blocks + block * 16 + 8) > prMemReadLeU16(blocks + block * 16 + 10));
        for (uint32_t i = 0; i < 16; ++i) {
            uint32_t x = block % 3 * 4 + i % 4;
            uint32_t y = block / 3 * 4 + i / 4;
            const uint8_t* pixel = &kKtxEtc1sPixels[(y * 12 + x) * 4];
            for (uint32_t c = 0; c < 4; ++c) {
                uint32_t value = texels[i * 4 + c];
                uint32_t error = value > pixel[c] ? value - pixel[c] : pixel[c] - value;
                uint32_t& kept = worst[c == 3 ? 1 : 0];
                kept = error > kept ? error : kept;
            }
        }
    }
    // BC1's four colors on a line, of 5 and 6 bits, place the four of ETC1S less finely than BC4's eight place its
    // values, whose ends have 8 bits.
    CHECK(worst[0] <= 24);
    CHECK(worst[1] <= 2);
    CHECK(prKtxTextureTranscode(texture, 0, 0, PrBasisBlocks_Bc1, blocks, 48) == PrStatus_NotSupported);
    prKtxTextureDestroy(texture);
}

TEST_CASE("ktx refuses what it does not decode, and what is not a texture") {
    Array<uint8_t> texels = bytesOf(kKtxEtc1sPixels, 16);
    CHECK(openStatus(buildKtx(kRgba8, texels)) == PrStatus_Ok);
    // A format of Vulkan's that is neither pixels of whole channels nor blocks that pr/gpu.h has.
    KtxSpec packed = kRgba8;
    packed.vkFormat = 2;
    CHECK(openStatus(buildKtx(packed, texels)) == PrStatus_NotSupported);
    KtxSpec volume = kRgba8;
    volume.depth = 2;
    CHECK(openStatus(buildKtx(volume, texels)) == PrStatus_NotSupported);
    KtxSpec scheme = kRgba8;
    scheme.scheme = 0x10000;
    CHECK(openStatus(buildKtx(scheme, texels)) == PrStatus_NotSupported);
    // A color model that names no format, where the format is to be Basis Universal's.
    KtxSpec model = kRgba8;
    model.vkFormat = 0;
    CHECK(openStatus(buildKtx(model, texels)) == PrStatus_NotSupported);
    // More levels than the texture's size has, no width, five faces, and a level of the wrong size.
    KtxSpec levels = kRgba8;
    levels.levelCount = 3;
    const Array<uint8_t> three[3] = {bytesOf(kKtxEtc1sPixels, 16), bytesOf(kKtxEtc1sPixels, 4), bytesOf(kKtxEtc1sPixels, 4)};
    const uint64_t sizes[3] = {16, 4, 4};
    CHECK(openStatus(buildKtx(levels, three, sizes)) == PrStatus_InvalidData);
    KtxSpec empty = kRgba8;
    empty.extent.x = 0;
    CHECK(openStatus(buildKtx(empty, texels)) == PrStatus_InvalidData);
    KtxSpec faces = kRgba8;
    faces.faceCount = 5;
    CHECK(openStatus(buildKtx(faces, texels)) == PrStatus_InvalidData);
    CHECK(openStatus(buildKtx(kRgba8, bytesOf(kKtxEtc1sPixels, 15))) == PrStatus_InvalidData);
}

TEST_CASE("ktx reports its texture from the bytes before its levels' data, which its first 80 bytes place") {
    const uint8_t* const kFiles[] = {kKtxUastc, kKtxEtc1s};
    const size_t kSizes[] = {sizeof(kKtxUastc), sizeof(kKtxEtc1s)};
    for (size_t f = 0; f < 2; ++f) {
        CAPTURE(f);
        PrByteView file{kFiles[f], kSizes[f]};
        // Too few bytes to tell ask for the header, which places the rest.
        CHECK(prKtxInfoBytes(PrByteView{file.data, 0}) == 80);
        CHECK(prKtxInfoBytes(PrByteView{file.data, 40}) == 80);
        uint64_t needed = prKtxInfoBytes(PrByteView{file.data, 80});
        REQUIRE(needed > 80);
        REQUIRE(needed < file.size);
        CHECK(prKtxInfoBytes(file) == needed);
        // Created from those bytes alone, the texture is what the whole file reports, and its levels are not there.
        PrKtxTexture* whole = textureOpen(file.data, file.size);
        REQUIRE(whole != nullptr);
        PrKtxTexture* header = nullptr;
        REQUIRE(textureOpen(PrByteView{file.data, size_t(needed)}, &header) == PrStatus_Truncated);
        const PrKtxInfo& full = *prKtxTextureInfo(whole);
        const PrKtxInfo& read = *prKtxTextureInfo(header);
        CHECK(read.extent == full.extent);
        CHECK(read.layerCount == full.layerCount);
        CHECK(read.faceCount == full.faceCount);
        CHECK(read.levelCount == full.levelCount);
        CHECK(read.blocks == full.blocks);
        CHECK(read.transcodes == full.transcodes);
        CHECK(pixelBytes(read) == pixelBytes(full));
        uint8_t pixels[12 * 8 * 4];
        PrPixels output{pixels, size_t(full.extent.x) * pixelBytes(full), full.extent};
        CHECK(prKtxTextureRead(header, 0, 0, output) != PrStatus_Ok);
        CHECK(prKtxTextureRead(whole, 0, 0, output) == PrStatus_Ok);
        prKtxTextureDestroy(header);
        // Fewer bytes than those do not hold the texture.
        CHECK(textureOpen(PrByteView{file.data, size_t(needed - 1)}, &header) != PrStatus_Truncated);
        prKtxTextureDestroy(header);
        prKtxTextureDestroy(whole);
    }
    // Bytes of another format are not a texture's.
    const uint8_t kPng[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    CHECK(prKtxInfoBytes(PrByteView{kPng, sizeof(kPng)}) == 0);
}

TEST_CASE("KTX's public header compiles as C") {
    CHECK(prTestKtxCApi() == 0);
}
