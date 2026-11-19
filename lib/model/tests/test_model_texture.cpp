// The default policy of a model's textures: the chain that it makes of a source for each use and device.

#include "pr/array.h"
#include "pr/gpu.h"
#include "pr/gpu_memory.h"
#include "pr/gpu_select.h"
#include "pr/mem.h"
#include "pr/model_texture.h"
#include "pr/texture.h"

#include "tools/harness/harness_gpu.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// A source of bits in memory, which produces the formats that a test lists, each of its own texels, and counts its
// reads.
struct FakeSource {
    PrTextureDesc desc{};
    Array<PrGpuFormat> formats;
    uint32_t reads = 0;

    static void read(void* PR_NULLABLE context, PrTextureRead* read) {
        FakeSource* fake = static_cast<FakeSource*>(context);
        ++fake->reads;
        const PrTextureDesc& desc = *prTextureSourceDesc(prTextureReadSource(read));
        size_t size = prTextureReadSize(&desc, prTextureReadInfo(read));
        // Each byte of the texels is its offset, so that a test tells channels apart.
        for (size_t i = 0; i < size; ++i)
            prTextureReadStorage(read)[i] = uint8_t(i * 3 + 1);
        prTextureReadComplete(read, PrStatus_Ok);
    }

    static PrStatus reformat(void* PR_NULLABLE context, PrGpuFormat format,
            const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
        FakeSource* fake = static_cast<FakeSource*>(context);
        PrTextureDesc desc = fake->desc;
        desc.format = format;
        PrTextureSourceDesc made{.texture = desc, .read = read, .context = fake};
        *out = prTextureSourceCreate(&made);
        return PrStatus_Ok;
    }

    const PrTextureSource* source() {
        PrTextureSourceDesc made{
            .texture = desc,
            .read = read,
            .reformat = reformat,
            .formats = formats.begin(),
            .formatCount = formats.count(),
            .context = this,
        };
        return prTextureSourceCreate(&made);
    }
};

// Returns a fake of pixels of `extent` and `levels` levels, which decodes to Rgba8, R8 and floats.
FakeSource pixels(PrUVec2 extent, uint32_t levels = 1, PrGpuFormat format = PrGpuFormat_Rgba8) {
    FakeSource fake;
    fake.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = levels,
        .format = format,
        .alpha = PrTextureAlpha_Straight,
    };
    const PrGpuFormat decoded[4] = {PrGpuFormat_R8, PrGpuFormat_Rgba8, PrGpuFormat_Rgba16F, PrGpuFormat_Rgba32F};
    for (PrGpuFormat other : decoded) {
        if (other != format)
            fake.formats.push(other);
    }
    return fake;
}

// Returns a use of `kind` that reads `channels`, of `color`.
PrModelTextureUse useOf(PrRenderTextureKind kind, PrRenderChannelFlags channels, PrModelTextureColor color) {
    return PrModelTextureUse{.kind = kind, .channels = channels, .color = color};
}

// Returns the chain that the default policy makes, which the caller releases.
PrModelTextureChain chainOf(const PrTextureSource* source, const PrModelTextureUse& use, const PrGpuDevice* PR_NULLABLE device) {
    PrModelTextureChain chain{};
    REQUIRE(prModelTextureChainDefault(nullptr, source, &use, device, &chain) == PrStatus_Ok);
    REQUIRE(chain.source != nullptr);
    return chain;
}

// The device that a test samples blocks on, or null where the build or the system has none.
PrGpuDevice* PR_NULLABLE blockDevice() {
    static PrGpuDevice* device = [] {
        PrGpuDevice* made = nullptr;
        if (gpuTests())
            gpuDeviceCreate(PrGpuBackend_Default, nullptr, &made);
        return made;
    }();
    return device;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("model texture");

TEST_CASE("color takes the Srgb form of its pixels, and data keeps their values, each in a chain of its own") {
    FakeSource fake = pixels({4, 4});
    const PrTextureSource* source = fake.source();
    PrGpuDevice* device = prGpuMemoryCreate(nullptr);
    // One image of two uses: color, and data of every channel.
    PrModelTextureChain color =
        chainOf(source, useOf(PrRenderTextureKind_Color, PrRenderChannelFlag_Rgba, PrModelTextureColor_Srgb), device);
    PrModelTextureChain data =
        chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_Rgb, PrModelTextureColor_Data), device);
    CHECK(prTextureSourceDesc(color.source)->format == PrGpuFormat_Rgba8Srgb);
    CHECK(prTextureSourceDesc(data.source)->format == PrGpuFormat_Rgba8);
    CHECK(color.source != data.source);
    // The device makes the chains of their levels.
    CHECK(color.flags == PrTextureUploadFlag_MakesLevels);
    CHECK(data.flags == PrTextureUploadFlag_MakesLevels);
    // Each chain reads the image's bits for itself, and they are the same bits.
    const PrTextureSource* chains[2] = {color.source, data.source};
    uint8_t texels[2][64] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        PrTextureReadDesc read = prTextureReadDescAll(prTextureSourceDesc(chains[i]));
        read.storage = texels[i];
        PrTextureRead* made = prTextureReadCreate(chains[i], &read);
        REQUIRE(prTextureReadStatus(made) == PrStatus_Ok);
        prTextureReadDestroy(made);
    }
    CHECK(fake.reads == 2);
    CHECK(prMemEquals(texels[0], texels[1], 64));
    prTextureSourceRelease(color.source);
    prTextureSourceRelease(data.source);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(device);
}

TEST_CASE("data of which the materials read one channel becomes that channel alone, in R8") {
    // A device of swizzled formats where the system has one, which reads one channel as gray.
    PrGpuDevice* memory = prGpuMemoryCreate(nullptr);
    PrGpuDevice* device = blockDevice() != nullptr ? blockDevice() : memory;
    FakeSource fake = pixels({4, 4});
    const PrTextureSource* source = fake.source();
    // Red alone decodes to R8 itself, and the device makes its chain.
    PrModelTextureChain red =
        chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_R, PrModelTextureColor_Data), device);
    CHECK(prTextureSourceDesc(red.source)->format == PrGpuFormat_R8);
    CHECK(red.flags == PrTextureUploadFlag_MakesLevels);
    prTextureSourceRelease(red.source);
    // Green alone, on a device of swizzled formats, is R8 that a sampler reads as gray, whose chain is made here, since
    // no device renders to a swizzled format.
    bool swizzles = (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_TextureSwizzle) != 0;
    PrModelTextureChain green =
        chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_G, PrModelTextureColor_Data), device);
    const PrTextureDesc& greenDesc = *prTextureSourceDesc(green.source);
    if (swizzles) {
        CHECK(greenDesc.format == PrGpuFormat_R8Luminance);
        CHECK(greenDesc.levelCount == 3);
        CHECK(green.flags == 0);
        // The chain's first level is the source's green.
        PrTextureReadDesc read = prTextureReadDescAll(&greenDesc);
        read.levelCount = 1;
        PrTextureRead* made = prTextureReadCreate(green.source, &read);
        REQUIRE(prTextureReadStatus(made) == PrStatus_Ok);
        CHECK(prTextureReadStorage(made)[0] == uint8_t(1 * 3 + 1));
        CHECK(prTextureReadStorage(made)[1] == uint8_t(5 * 3 + 1));
        prTextureReadDestroy(made);
    } else {
        CHECK(greenDesc.format == PrGpuFormat_Rgba8);
    }
    prTextureSourceRelease(green.source);
    // Color of one channel, and data of sRGB, keep their four channels.
    PrModelTextureChain color =
        chainOf(source, useOf(PrRenderTextureKind_Color, PrRenderChannelFlag_R, PrModelTextureColor_Srgb), device);
    CHECK(prTextureSourceDesc(color.source)->format == PrGpuFormat_Rgba8Srgb);
    prTextureSourceRelease(color.source);
    prTextureSourceRelease(source);
    prGpuDeviceDestroy(memory);
}

TEST_CASE("floats become 16-bit floats of the same values, for color as for data") {
    FakeSource fake = pixels({2, 2}, 1, PrGpuFormat_Rgba32F);
    const PrTextureSource* source = fake.source();
    const PrModelTextureUse uses[2] = {
        useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_Rgb, PrModelTextureColor_Data),
        useOf(PrRenderTextureKind_Color, PrRenderChannelFlag_Rgb, PrModelTextureColor_Data),
    };
    for (const PrModelTextureUse& use : uses) {
        PrModelTextureChain chain = chainOf(source, use, nullptr);
        CHECK(prTextureSourceDesc(chain.source)->format == PrGpuFormat_Rgba16F);
        CHECK(chain.flags == PrTextureUploadFlag_MakesLevels);
        prTextureSourceRelease(chain.source);
    }
    prTextureSourceRelease(source);
}

TEST_CASE("pixels of a Srgb form are relabeled to the plain form for data, and keep their bits") {
    FakeSource fake = pixels({4, 4});
    fake.desc.format = PrGpuFormat_Rgba8Srgb;
    fake.formats.clear();
    const PrTextureSource* source = fake.source();
    PrModelTextureChain data =
        chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_Rgb, PrModelTextureColor_Data), nullptr);
    CHECK(prTextureSourceDesc(data.source)->format == PrGpuFormat_Rgba8);
    PrModelTextureChain color =
        chainOf(source, useOf(PrRenderTextureKind_Color, PrRenderChannelFlag_Rgb, PrModelTextureColor_Srgb), nullptr);
    CHECK(prTextureSourceDesc(color.source)->format == PrGpuFormat_Rgba8Srgb);
    uint8_t texels[2][64] = {};
    const PrTextureSource* chains[2] = {data.source, color.source};
    for (uint32_t i = 0; i < 2; ++i) {
        PrTextureReadDesc read = prTextureReadDescAll(prTextureSourceDesc(chains[i]));
        read.storage = texels[i];
        PrTextureRead* made = prTextureReadCreate(chains[i], &read);
        REQUIRE(prTextureReadStatus(made) == PrStatus_Ok);
        prTextureReadDestroy(made);
    }
    CHECK(prMemEquals(texels[0], texels[1], 64));
    prTextureSourceRelease(data.source);
    prTextureSourceRelease(color.source);
    prTextureSourceRelease(source);
}

TEST_CASE("blocks that the device samples are kept, and pixels where they have one level or the device lacks them") {
    FakeSource blocks;
    blocks.desc = PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = {8, 8},
        .depth = 1,
        .layerCount = 1,
        .levelCount = 4,
        .format = PrGpuFormat_Bc7,
        .alpha = PrTextureAlpha_Straight,
    };
    blocks.formats.push(PrGpuFormat_Rgba8);
    const PrTextureSource* source = blocks.source();
    PrModelTextureUse color = useOf(PrRenderTextureKind_Color, PrRenderChannelFlag_Rgba, PrModelTextureColor_Srgb);
    // Without a device that samples them, the blocks decode to pixels.
    PrModelTextureChain chain = chainOf(source, color, nullptr);
    CHECK(prTextureSourceDesc(chain.source)->format == PrGpuFormat_Rgba8Srgb);
    prTextureSourceRelease(chain.source);
    PrGpuDevice* device = blockDevice();
    if (device != nullptr && prGpuDeviceSupportsFormat(device, PrGpuFormat_Bc7Srgb, PrGpuTextureUsageFlag_Sampled)) {
        chain = chainOf(source, color, device);
        CHECK(prTextureSourceDesc(chain.source)->format == PrGpuFormat_Bc7Srgb);
        CHECK(prTextureSourceDesc(chain.source)->levelCount == 4);
        prTextureSourceRelease(chain.source);
    }
    prTextureSourceRelease(source);

    // A texture that transcodes to blocks of one channel takes them first for a use of one channel, read as gray.
    FakeSource basis = pixels({8, 8}, 4);
    basis.formats.push(PrGpuFormat_Bc7);
    basis.formats.push(PrGpuFormat_Bc4);
    source = basis.source();
    if (device != nullptr && prGpuDeviceSupportsFormat(device, PrGpuFormat_Bc4Luminance, PrGpuTextureUsageFlag_Sampled)) {
        chain = chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_G, PrModelTextureColor_Data), device);
        CHECK(prTextureSourceDesc(chain.source)->format == PrGpuFormat_Bc4Luminance);
        prTextureSourceRelease(chain.source);
        chain = chainOf(source, useOf(PrRenderTextureKind_Data, PrRenderChannelFlag_Rgb, PrModelTextureColor_Data), device);
        CHECK(prTextureSourceDesc(chain.source)->format == PrGpuFormat_Bc7);
        prTextureSourceRelease(chain.source);
    }
    prTextureSourceRelease(source);
}

TEST_SUITE_END();
