// The default policy of a model's textures: the chain of sources that a texture of an image is uploaded from.

#include "pr/model_texture.h"

#include "pr/count_of.h"

namespace pr_impl {

namespace {

// The formats that a texture of several levels transcodes to, in the order of preference.
constexpr PrGpuFormat kTargets[] = {
    PrGpuFormat_Astc4x4, PrGpuFormat_Bc7, PrGpuFormat_EacR11, PrGpuFormat_Bc4, PrGpuFormat_EacRg11, PrGpuFormat_Bc5,
    PrGpuFormat_Etc2Rgba8, PrGpuFormat_Bc3, PrGpuFormat_Etc2Rgb8, PrGpuFormat_Bc1,
};

// Returns true if `format` holds one channel.
bool isSingle(PrGpuFormat format) {
    return format == PrGpuFormat_Bc4 || format == PrGpuFormat_EacR11;
}

// Returns true if `device` samples textures of `format` whose first level is of `extent`: a device takes blocks only in
// a first level that is whole blocks.
bool samples(const PrGpuDevice* PR_NULLABLE device, PrGpuFormat format, PrUVec2 extent) {
    if (device == nullptr || format == PrGpuFormat_None ||
            !prGpuDeviceSupportsFormat(device, format, PrGpuTextureUsageFlag_Sampled))
        return false;
    PrGpuFormatBlock block = prGpuFormatBlock(format);
    return extent.x % block.width == 0 && extent.y % block.height == 0;
}

// Returns the format that blocks of `blocks` are sampled in for a use, sRGB's form for sRGB color: that format, but for
// one of one channel, whose value reads as gray, as its pixels decode, in the format that places it in red, green and
// blue. Returns None for sRGB color in a format without a Srgb form.
PrGpuFormat sampledFormat(PrGpuFormat blocks, bool srgb) {
    PrGpuFormat plain = prGpuFormatPlain(blocks);
    PrGpuFormat format = isSingle(plain) ? prGpuFormatSwizzled(plain, PrGpuSwizzle_Luminance) : plain;
    if (!srgb)
        return format;
    PrGpuFormat encoded = prGpuFormatSrgb(format);
    return encoded != format ? encoded : PrGpuFormat_None;
}

// Returns the form of `plain` that `source` produces, the plain form itself or its Srgb form, or None if it produces
// neither.
PrGpuFormat producedForm(const PrTextureSource* source, PrGpuFormat plain) {
    if (prTextureSourceProduces(source, plain))
        return plain;
    PrGpuFormat srgb = prGpuFormatSrgb(plain);
    return srgb != plain && prTextureSourceProduces(source, srgb) ? srgb : PrGpuFormat_None;
}

// Sets `*out` to a chain of `source` relabeled as `format`, which holds the same bits, and returns Ok.
PrStatus relabel(const PrTextureSource* source, PrGpuFormat format, PrModelTextureChain* out) {
    if (prTextureSourceDesc(source)->format == format) {
        prTextureSourceRetain(source);
        out->source = source;
        return PrStatus_Ok;
    }
    PrTextureConvertDesc convert = prTextureConvertDescDefault(format);
    convert.alpha = prTextureSourceDesc(source)->alpha;
    out->source = prTextureSourceCreateConvert(source, &convert);
    return PrStatus_Ok;
}

// Returns the one channel of `channels`, 0 to 3, or 4 if it holds none or several.
uint8_t singleChannel(PrRenderChannelFlags channels) {
    for (uint8_t c = 0; c < 4; ++c) {
        if (channels == PrRenderChannelFlags(1u << c))
            return c;
    }
    return 4;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prModelTextureChainDefault(void* PR_NULLABLE, const PrTextureSource* source, const PrModelTextureUse* use,
        const PrGpuDevice* PR_NULLABLE device, PrModelTextureChain* out) {
    *out = PrModelTextureChain{};
    const PrTextureDesc& desc = *prTextureSourceDesc(source);
    bool srgb = use->color == PrModelTextureColor_Srgb;
    uint8_t single = use->kind == PrRenderTextureKind_Data && !srgb ? singleChannel(use->channels) : 4;
    bool chainable = prGpuMipLevels(desc.extent) > 1;
    PrGpuFormat pixelForm = producedForm(source, PrGpuFormat_Rgba8);
    bool decodes = pixelForm != PrGpuFormat_None;
    // A texture of one level has its mip chain made from pixels: a texture of blocks then becomes pixels where it
    // decodes, since a chain does more for it than blocks do.
    bool chained = desc.levelCount == 1 && chainable;

    if (prGpuFormatIsCompressed(desc.format) && !(decodes && chained)) {
        PrGpuFormat sampled = sampledFormat(desc.format, srgb);
        if (samples(device, sampled, desc.extent))
            return relabel(source, sampled, out);
    }
    if (!chained) {
        PrGpuFormat order[countOf(kTargets)];
        uint32_t count = 0;
        // A use of one channel takes a format of one channel first.
        for (PrGpuFormat target : kTargets) {
            if (single < 4 && isSingle(target))
                order[count++] = target;
        }
        for (PrGpuFormat target : kTargets) {
            if (!(single < 4 && isSingle(target)))
                order[count++] = target;
        }
        for (uint32_t i = 0; i < count; ++i) {
            PrGpuFormat target = producedForm(source, order[i]);
            if (target == PrGpuFormat_None || target == desc.format)
                continue;
            PrGpuFormat sampled = sampledFormat(target, srgb);
            if (!samples(device, sampled, desc.extent))
                continue;
            const PrTextureSource* transcoded = nullptr;
            PrStatus status = prTextureSourceReformat(source, target, &transcoded);
            if (status != PrStatus_Ok)
                return status;
            status = relabel(transcoded, sampled, out);
            prTextureSourceRelease(transcoded);
            return status;
        }
    }
    if (!decodes)
        return PrStatus_NotSupported;

    if (desc.format == PrGpuFormat_Rgba16F || desc.format == PrGpuFormat_Rgba32F) {
        const PrTextureSource* decoded = nullptr;
        PrStatus status = prTextureSourceReformat(source, desc.format, &decoded);
        if (status != PrStatus_Ok)
            return status;
        PrTextureConvertDesc convert = prTextureConvertDescDefault(PrGpuFormat_Rgba16F);
        out->source = prTextureSourceCreateConvert(decoded, &convert);
        out->flags = PrTextureUploadFlag_MakesLevels;
        prTextureSourceRelease(decoded);
        return PrStatus_Ok;
    }

    // Red alone is decoded alone where the source decodes it so, and the device makes its chain.
    if (single == 0 && prTextureSourceProduces(source, PrGpuFormat_R8)) {
        PrStatus status = prTextureSourceReformat(source, PrGpuFormat_R8, &out->source);
        out->flags = PrTextureUploadFlag_MakesLevels;
        return status;
    }
    const PrTextureSource* pixels = nullptr;
    PrStatus status = prTextureSourceReformat(source, pixelForm, &pixels);
    if (status != PrStatus_Ok)
        return status;
    bool swizzles = device != nullptr && (prGpuDeviceInfo(device).features & PrGpuFeatureFlag_TextureSwizzle) != 0;
    if (single == 0 || (single < 4 && swizzles)) {
        PrGpuFormat format = PrGpuFormat_R8;
        if (single == 3)
            format = PrGpuFormat_R8Intensity;
        else if (single != 0)
            format = PrGpuFormat_R8Luminance;
        PrTextureConvertDesc convert = prTextureConvertDescDefault(format);
        convert.alpha = PrTextureAlpha_None;
        convert.channels[0] = single;
        const PrTextureSource* reduced = prTextureSourceCreateConvert(pixels, &convert);
        prTextureSourceRelease(pixels);
        if (format == PrGpuFormat_R8 || !chained) {
            out->source = reduced;
            out->flags = PrTextureUploadFlag_MakesLevels;
            return PrStatus_Ok;
        }
        // The device renders to no swizzled format, so the chain is made here.
        out->source = prTextureSourceCreateMips(reduced, 0);
        prTextureSourceRelease(reduced);
        return PrStatus_Ok;
    }
    PrTextureConvertDesc convert = prTextureConvertDescDefault(srgb ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8);
    convert.alpha = desc.alpha == PrTextureAlpha_None ? PrTextureAlpha_None : PrTextureAlpha_Straight;
    out->source = prTextureSourceCreateConvert(pixels, &convert);
    out->flags = PrTextureUploadFlag_MakesLevels;
    prTextureSourceRelease(pixels);
    return PrStatus_Ok;
}
