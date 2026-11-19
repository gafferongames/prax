// Textures: descriptions, sources, reads, the sources that wrap a source, openers and uploads.

#include "pr/texture.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/atomic.h"
#include "pr/color_space.h"
#include "pr/float16.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mime_sniff.h"
#include "pr/raw_array.h"
#include "pr/thread.h"

namespace pr_impl {

namespace {

// The next source's id.
Atomic<uint64_t> gNextSourceId;

// A source: its description, with its formats in place, and its references.
struct TextureSource {
    mutable Atomic<uint32_t> references;    // changes behind const, as holders retain and release the immutable source
    uint64_t id;
    PrTextureSourceDesc desc;
    PrGpuFormat formats[kPrTextureSourceMaxFormats];
};

// A read: what it asks for, its storage, and its completion.
struct TextureRead {
    const PrTextureSource* source;
    PrTextureReadDesc desc;
    bool ownsStorage;
    Atomic<bool> done;
    PrStatus status;
    PrMutex lock;
    PrCondition completed;
};

// The opener's formats, in their order.
struct TextureOpener {
    RawArray<PrTextureFormat> formats;
};

// A texture being opened: the opener, the bytes, which it retains, their name, the limits, and the caller's callback.
struct TextureOpen {
    const TextureOpener* opener;
    const PrBlobSource* bytes;
    char* name;
    size_t nameSize;
    PrTextureLimits limits;
    PrTextureOpenFn done;
    void* PR_NULLABLE context;
};

// Returns a source of `blob`, the bytes of `open`, made by the first of the opener's formats that reads its media type
// and does not refuse it.
PrStatus textureOpen(const TextureOpen& open, PrBlob* blob, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    PrByteView all = prBlobBytes(blob);
    PrByteView prefix{all.data, all.size < kPrMimeSniffBytes ? all.size : size_t(kPrMimeSniffBytes)};
    PrStringView type = prMimeFormatInfo(prMimeSniff(prefix, PrStringView{open.name, open.nameSize})).type;
    PrStatus status = PrStatus_NotSupported;
    for (uint32_t i = 0; i < open.opener->formats.count; ++i) {
        const PrTextureFormat& format = open.opener->formats.data[i];
        if (!format.supportsTypeFn(format.context, type))
            continue;
        status = format.createFn(format.context, open.bytes, blob, &open.limits, out);
        if (status != PrStatus_NotSupported)
            break;
    }
    return status;
}

// A wait of the calling thread for a call that completes on any thread.
struct TextureWait {
    PrMutex lock;
    PrCondition completed;
    bool done;
    PrStatus status;
    const void* PR_NULLABLE result;

    void init() {
        prMutexInit(&lock);
        prConditionInit(&completed);
    }

    void complete(PrStatus value, const void* PR_NULLABLE made) {
        prMutexLock(&lock);
        status = value;
        result = made;
        done = true;
        prConditionBroadcast(&completed);
        prMutexUnlock(&lock);
    }

    // Waits for the completion, and frees the wait's state.
    void wait() {
        prMutexLock(&lock);
        while (!done)
            prConditionWait(&completed, &lock);
        prMutexUnlock(&lock);
        prMutexReset(&lock);
        prConditionReset(&completed);
    }
};

// Returns true if the device makes the levels of a texture of `format`: it filters the format and renders to it.
bool makesLevels(const PrGpuDevice* device, PrGpuFormat format) {
    if (prGpuFormatIsCompressed(format) || prGpuFormatSwizzle(format) != PrGpuSwizzle_None)
        return false;
    PrGpuTextureUsageFlags usage = PrGpuTextureUsageFlag_Sampled | PrGpuTextureUsageFlag_RenderTarget;
    return prGpuDeviceSupportsFormat(device, format, usage) && prGpuFormatFilters(format);
}

// Returns the GPU texture's description for an upload of `desc` with `flags` on `device`.
PrGpuTextureDesc uploadDesc(const PrGpuDevice* device, const PrTextureDesc& desc, PrTextureUploadFlags flags) {
    PrGpuTextureDesc gpu = prGpuTextureDesc2d(desc.format, desc.extent, PrGpuTextureUsageFlag_Sampled);
    gpu.kind = desc.kind;
    gpu.levels = uint8_t(desc.levelCount);
    gpu.extent.depth = desc.kind == PrGpuTextureKind_3d ? desc.depth : desc.layerCount;
    bool chain = (flags & PrTextureUploadFlag_MakesLevels) != 0 && desc.levelCount == 1 &&
        desc.kind == PrGpuTextureKind_2d && prGpuMipLevels(desc.extent) > 1 && makesLevels(device, desc.format);
    if (chain) {
        gpu.levels = uint8_t(prGpuMipLevels(desc.extent));
        gpu.usage |= PrGpuTextureUsageFlag_RenderTarget;
    }
    return gpu;
}

// An upload: the device, the source's description, what the caller asked, and the read of the bits.
struct TextureUpload {
    PrGpuDevice* device;
    PrTextureDesc desc;
    PrTextureUploadFlags flags;
    char name[64];
    PrTextureUploadFn done;
    void* PR_NULLABLE context;
    PrTextureReadDesc read;
};

// Creates the GPU texture of an upload whose bits `read` holds, uploads them, and makes the levels that it asks for.
PrStatus uploadTexels(const TextureUpload& upload, const PrTextureRead* read, PrGpuTexture* out) {
    *out = PrGpuTexture{};
    if (prGpuDeviceIsLost(upload.device))
        return PrStatus_Lost;
    const PrTextureDesc& desc = upload.desc;
    if (!prGpuDeviceSupportsFormat(upload.device, desc.format, PrGpuTextureUsageFlag_Sampled))
        return PrStatus_NotSupported;
    PrGpuTextureDesc gpu = uploadDesc(upload.device, desc, upload.flags);
    gpu.name = upload.name;
    PrGpuTexture made{};
    PrStatus status = prGpuTextureCreate(upload.device, &gpu, &made);
    if (status != PrStatus_Ok)
        return status;
    const uint8_t* texels = prTextureReadStorage(read);
    for (uint32_t level = 0; level < desc.levelCount; ++level) {
        PrUVec2 extent = prTextureLevelExtent(desc.extent, level);
        uint32_t depth = desc.layerCount;
        if (desc.kind == PrGpuTextureKind_3d)
            depth = desc.depth >> level > 1 ? desc.depth >> level : 1;
        PrGpuTextureRegion region{.level = uint8_t(level), .extent = {extent.x, extent.y, depth}};
        size_t offset = prTextureReadOffset(&desc, &upload.read, level, 0);
        prGpuTextureUpload(upload.device, made, &region, texels + offset, prGpuFormatRowBytes(desc.format, extent.x), 0);
    }
    PrGpuCommands* commands = nullptr;
    if (gpu.levels > desc.levelCount && prGpuCommandsBegin(upload.device, nullptr, &commands)) {
        prGpuCommandsGenerateMipmaps(commands, made, PrGpuAccessFlag_ShaderRead);
        prGpuCommandsSubmit(commands);
    }
    *out = made;
    return PrStatus_Ok;
}

// ---------------------------------------------------------------------------------------------------------------------
// Pixels of sources

// Returns the bytes of one sample of `sample`.
uint32_t sampleSize(PrTextureSample sample) {
    switch (sample) {
        case PrTextureSample_U8:
            return 1;
        case PrTextureSample_U16:
        case PrTextureSample_I16:
        case PrTextureSample_F16:
            return 2;
        default:
            return 4;
    }
}

// Returns the value of the sample of `sample` at `bytes`.
float sampleValue(PrTextureSample sample, const uint8_t* bytes) {
    switch (sample) {
        case PrTextureSample_U8:
            return float(bytes[0]) / 255;
        case PrTextureSample_U16: {
            uint16_t value = 0;
            prMemCopy(&value, bytes, 2);
            return float(value) / 65535;
        }
        case PrTextureSample_I16: {
            int16_t value = 0;
            prMemCopy(&value, bytes, 2);
            return value <= -32767 ? -1.0f : float(value) / 32767;
        }
        case PrTextureSample_F16: {
            PrFloat16 value{};
            prMemCopy(&value, bytes, 2);
            return prFloat16ToFloat(value);
        }
        case PrTextureSample_F32: {
            float value = 0;
            prMemCopy(&value, bytes, 4);
            return value;
        }
        default: {
            uint32_t value = 0;
            prMemCopy(&value, bytes, 4);
            return float(double(value) / 4294967295.0);
        }
    }
}

// Returns the 8-bit value of the sample of `sample` at `bytes`, as the nearest of 255 steps. A sample of 8 or 16 bits
// rounds exactly, in integers.
uint8_t sampleU8(PrTextureSample sample, const uint8_t* bytes) {
    if (sample == PrTextureSample_U8)
        return bytes[0];
    if (sample == PrTextureSample_U16) {
        uint16_t wide = 0;
        prMemCopy(&wide, bytes, 2);
        return uint8_t((uint32_t(wide) * 255 + 32767) / 65535);
    }
    float value = sampleValue(sample, bytes);
    float clamped = value > 0 ? (value < 1 ? value : 1) : 0;
    return uint8_t(clamped * 255 + 0.5f);
}

// Expands a row of `width` RGB bytes at `from` to opaque RGBA at `to`. The loop has a function of its own, which the
// compiler vectorizes as it does not the same loop inlined among the other forms of an unpack.
void expandRgbRow(const uint8_t* from, uint8_t* to, uint32_t width) {
    for (uint32_t x = 0; x < width; ++x) {
        to[x * 4] = from[x * 3];
        to[x * 4 + 1] = from[x * 3 + 1];
        to[x * 4 + 2] = from[x * 3 + 2];
        to[x * 4 + 3] = 255;
    }
}

// The channels of a stored pixel of `channels`: how many there are, and whether the last is alpha.
struct ChannelLayout {
    uint32_t count;
    bool alpha;
};

ChannelLayout layoutOf(PrTextureChannels channels) {
    switch (channels) {
        case PrTextureChannels_Gray:
            return ChannelLayout{1, false};
        case PrTextureChannels_GrayAlpha:
            return ChannelLayout{2, true};
        case PrTextureChannels_Rg:
            return ChannelLayout{2, false};
        case PrTextureChannels_Rgb:
            return ChannelLayout{3, false};
        default:
            return ChannelLayout{4, true};
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Plain texels

// How a stored plain format lays out a texel: its channels, the bytes of each, and whether red and blue trade places.
struct PlainLayout {
    uint8_t channels;           // 0 for a format that is not plain
    uint8_t bytes;              // 1 for unsigned 8 bits, 2 for half floats, 4 for floats
    bool bgra;
};

// Returns how `format`, a format's stored form, lays out its texels.
PlainLayout plainLayout(PrGpuFormat format) {
    switch (prGpuFormatPlain(prGpuFormatStored(format))) {
        case PrGpuFormat_R8:
            return PlainLayout{1, 1, false};
        case PrGpuFormat_Rg8:
            return PlainLayout{2, 1, false};
        case PrGpuFormat_Rgba8:
            return PlainLayout{4, 1, false};
        case PrGpuFormat_Bgra8:
            return PlainLayout{4, 1, true};
        case PrGpuFormat_R16F:
            return PlainLayout{1, 2, false};
        case PrGpuFormat_Rg16F:
            return PlainLayout{2, 2, false};
        case PrGpuFormat_Rgba16F:
            return PlainLayout{4, 2, false};
        case PrGpuFormat_R32F:
            return PlainLayout{1, 4, false};
        case PrGpuFormat_Rg32F:
            return PlainLayout{2, 4, false};
        case PrGpuFormat_Rgba32F:
            return PlainLayout{4, 4, false};
        default:
            return PlainLayout{};
    }
}

// Returns true if `format` is plain, or a swizzled form of a plain format, which reads and writes texels of.
bool isPlain(PrGpuFormat format) {
    return plainLayout(format).channels != 0;
}

// Reads the stored channels of the texel at `bytes` into `values`, as numbers, and leaves the rest.
void loadStored(PlainLayout layout, const uint8_t* bytes, float* values) {
    for (uint32_t c = 0; c < layout.channels; ++c) {
        uint32_t at = layout.bgra && c < 3 ? 2 - c : c;
        if (layout.bytes == 1) {
            values[c] = float(bytes[at]) / 255;
        } else if (layout.bytes == 2) {
            PrFloat16 half{};
            prMemCopy(&half, bytes + at * 2, 2);
            values[c] = prFloat16ToFloat(half);
        } else {
            prMemCopy(&values[c], bytes + at * 4, 4);
        }
    }
}

// Returns `value` in [0, 1] as the nearest of 255 steps.
uint8_t unorm8(float value) {
    float clamped = value > 0 ? (value < 1 ? value : 1) : 0;
    return uint8_t(clamped * 255 + 0.5f);
}

// Writes the first stored channels of `values` as the texel at `bytes`.
void storeStored(PlainLayout layout, const float* values, uint8_t* bytes) {
    for (uint32_t c = 0; c < layout.channels; ++c) {
        uint32_t at = layout.bgra && c < 3 ? 2 - c : c;
        if (layout.bytes == 1) {
            bytes[at] = unorm8(values[c]);
        } else if (layout.bytes == 2) {
            PrFloat16 half = prFloat16FromFloat(values[c]);
            prMemCopy(bytes + at * 2, &half, 2);
        } else {
            prMemCopy(bytes + at * 4, &values[c], 4);
        }
    }
}

// Reads the texel of `format` at `bytes` as a sampler reads it: red, green, blue and alpha, after the format's swizzle,
// with a channel that the format does not store zero and alpha one.
void loadTexel(PrGpuFormat format, PlainLayout layout, const uint8_t* bytes, float* rgba) {
    float stored[4] = {0, 0, 0, 1};
    loadStored(layout, bytes, stored);
    switch (prGpuFormatSwizzle(format)) {
        case PrGpuSwizzle_Alpha:
            rgba[0] = rgba[1] = rgba[2] = 0;
            rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_Luminance:
            rgba[0] = rgba[1] = rgba[2] = stored[0];
            rgba[3] = 1;
            return;
        case PrGpuSwizzle_Intensity:
            rgba[0] = rgba[1] = rgba[2] = rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_Mask:
            rgba[0] = rgba[1] = rgba[2] = 1;
            rgba[3] = stored[0];
            return;
        case PrGpuSwizzle_LuminanceAlpha:
            rgba[0] = rgba[1] = rgba[2] = stored[0];
            rgba[3] = stored[1];
            return;
        default:
            for (uint32_t c = 0; c < 4; ++c)
                rgba[c] = stored[c];
            return;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Color

// Converts the color channels of a texel between sRGB's encoding and linear values: decoded if `decodes`, and encoded
// otherwise.
void convertTransfer(bool decodes, float* rgba) {
    for (uint32_t c = 0; c < 3; ++c) {
        if (decodes)
            rgba[c] = prColorTransferDecode(PrColorTransfer_Srgb, rgba[c]);
        else
            rgba[c] = prColorTransferEncode(PrColorTransfer_Srgb, rgba[c]);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Conversions

// How a conversion makes its texels.
enum {
    ConvertMode_Relabel,        // the source's bits as they are, under another format
    ConvertMode_Bytes,          // bytes of 8 bits moved between channels, with straight alpha made of premultiplied
    ConvertMode_Numbers,        // each texel read as numbers, converted, and written
};
typedef uint8_t ConvertMode;

// A source that converts another's texels.
struct ConvertSource {
    const PrTextureSource* inner;
    PrTextureDesc innerDesc;
    PrTextureDesc desc;
    PrTextureConvertDesc convert;
    ConvertMode mode;
    bool converts;              // the values change between sRGB's encoding and linear floats
    bool decodes;               // of values that change, from sRGB's encoding to linear floats
    bool straightens;           // the source's alpha is premultiplied and the target's is not
    bool premultiplies;         // the target's alpha is premultiplied and the source's is not
};

// A read of a conversion: the source, and the read that it answers.
struct ConvertRead {
    const ConvertSource* source;
    PrTextureRead* outer;
};

// Returns true if `channels` gives each channel its own.
bool identityChannels(const uint8_t* channels) {
    return channels[0] == 0 && channels[1] == 1 && channels[2] == 2 && channels[3] == 3;
}

// Converts one level of one layer: `count` texels at `from` into `to`. Returns true if a conversion keeps each byte of
// a texel of four in its place, as pixels of 8 bits are premultiplied or made opaque, which it does in the storage that
// its source is read into.
bool convertsInPlace(const ConvertSource& source) {
    if (source.mode != ConvertMode_Bytes || source.straightens)
        return false;
    PlainLayout in = plainLayout(source.innerDesc.format);
    PlainLayout out = plainLayout(source.desc.format);
    const uint8_t* channels = source.convert.channels;
    bool alphaOne = channels[3] == kPrTextureChannelOne || source.desc.alpha == PrTextureAlpha_None;
    return in.channels == 4 && out.channels == 4 && in.bytes == 1 && out.bytes == 1 && in.bgra == out.bgra &&
        prGpuFormatSwizzle(source.innerDesc.format) == PrGpuSwizzle_None && channels[0] == 0 && channels[1] == 1 &&
        channels[2] == 2 && (channels[3] == 3 || alphaOne);
}

// Makes `count` texels of four bytes at `texels` opaque.
void makeOpaque(uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i)
        texels[i * 4 + 3] = 255;
}

// Premultiplies `count` texels of four bytes at `texels`.
void premultiplyTexels(uint8_t* texels, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        uint8_t* texel = texels + i * 4;
        uint32_t a = texel[3];
        if (a == 255)
            continue;
        for (uint32_t c = 0; c < 3; ++c)
            texel[c] = uint8_t((uint32_t(texel[c]) * a + 127) / 255);
    }
}

void convertTexels(const ConvertSource& source, const uint8_t* from, uint8_t* to, size_t count) {
    if (convertsInPlace(source)) {
        PR_ASSERT(from == to);
        if (source.convert.channels[3] == kPrTextureChannelOne || source.desc.alpha == PrTextureAlpha_None)
            makeOpaque(to, count);
        else if (source.premultiplies)
            premultiplyTexels(to, count);
        return;
    }
    PlainLayout in = plainLayout(source.innerDesc.format);
    PlainLayout out = plainLayout(source.desc.format);
    uint32_t inBytes = uint32_t(in.channels) * in.bytes;
    uint32_t outBytes = uint32_t(out.channels) * out.bytes;
    const uint8_t* channels = source.convert.channels;
    bool opaque = source.desc.alpha == PrTextureAlpha_None;
    if (source.mode == ConvertMode_Bytes) {
        // By channel of the source's texel as a sampler reads it, its stored byte, or none for a constant.
        int32_t places[4] = {0, in.channels > 1 ? 1 : -1, in.channels > 2 ? 2 : -1, in.channels > 3 ? 3 : -1};
        if (in.bgra) {
            places[0] = 2;
            places[2] = 0;
        }
        PrGpuSwizzle swizzle = prGpuFormatSwizzle(source.innerDesc.format);
        if (swizzle == PrGpuSwizzle_Luminance || swizzle == PrGpuSwizzle_Intensity)
            places[1] = places[2] = 0;
        if (swizzle == PrGpuSwizzle_Intensity)
            places[3] = 0;
        // A channel moved alone is a copy of bytes.
        if (out.channels == 1 && !source.straightens && channels[0] < 4 && places[channels[0]] >= 0) {
            const uint8_t* moved = from + places[channels[0]];
            for (size_t i = 0; i < count; ++i)
                to[i] = moved[i * inBytes];
            return;
        }
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* texel = from + i * inBytes;
            uint8_t rgba[6] = {0, 0, 0, 255, 0, 255};
            for (uint32_t c = 0; c < 4; ++c) {
                if (places[c] >= 0)
                    rgba[c] = texel[places[c]];
            }
            if (source.straightens && rgba[3] != 0) {
                for (uint32_t c = 0; c < 3; ++c) {
                    uint32_t straight = (uint32_t(rgba[c]) * 255 + rgba[3] / 2) / rgba[3];
                    rgba[c] = uint8_t(straight > 255 ? 255 : straight);
                }
            }
            if (source.premultiplies) {
                for (uint32_t c = 0; c < 3; ++c)
                    rgba[c] = uint8_t((uint32_t(rgba[c]) * rgba[3] + 127) / 255);
            }
            uint8_t* written = to + i * outBytes;
            for (uint32_t c = 0; c < out.channels; ++c) {
                uint8_t value = rgba[channels[c]];
                if (c == 3 && opaque)
                    value = 255;
                written[out.bgra && c < 3 ? 2 - c : c] = value;
            }
        }
        return;
    }
    const float constants[2] = {0, 1};
    for (size_t i = 0; i < count; ++i) {
        float rgba[4];
        loadTexel(source.innerDesc.format, in, from + i * inBytes, rgba);
        float mapped[4];
        for (uint32_t c = 0; c < 4; ++c)
            mapped[c] = channels[c] < 4 ? rgba[channels[c]] : constants[channels[c] - kPrTextureChannelZero];
        if (source.straightens && mapped[3] > 0) {
            for (uint32_t c = 0; c < 3; ++c)
                mapped[c] /= mapped[3];
        }
        if (source.converts)
            convertTransfer(source.decodes, mapped);
        if (opaque)
            mapped[3] = 1;
        if (source.premultiplies) {
            for (uint32_t c = 0; c < 3; ++c)
                mapped[c] *= mapped[3];
        }
        storeStored(out, mapped, to + i * outBytes);
    }
}

// Completes a conversion's read, once the read of its source is done.
void convertDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    ConvertRead* held = static_cast<ConvertRead*>(context);
    const ConvertSource& source = *held->source;
    PrTextureRead* outer = held->outer;
    prMemoryFree(held);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(outer);
    if (status == PrStatus_Ok && source.mode != ConvertMode_Relabel) {
        const uint8_t* from = prTextureReadStorage(inner);
        uint8_t* to = prTextureReadStorage(outer);
        for (uint32_t level = read.firstLevel; level < read.firstLevel + read.levelCount; ++level) {
            PrUVec2 extent = prTextureLevelExtent(source.desc.extent, level);
            size_t count = size_t(extent.x) * extent.y * (prTextureLevelSize(&source.desc, level) /
                prGpuFormatLevelBytes(source.desc.format, extent));
            for (uint32_t layer = read.firstLayer; layer < read.firstLayer + read.layerCount; ++layer) {
                convertTexels(source, from + prTextureReadOffset(&source.innerDesc, &read, level, layer),
                    to + prTextureReadOffset(&source.desc, &read, level, layer), count);
            }
        }
    }
    prTextureReadDestroy(inner);
    prTextureReadComplete(outer, status);
}

void convertRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const ConvertSource* source = static_cast<const ConvertSource*>(context);
    ConvertRead* held = static_cast<ConvertRead*>(prMemoryAlloc(sizeof(ConvertRead)));
    *held = ConvertRead{source, read};
    PrTextureReadDesc inner = *prTextureReadInfo(read);
    // A relabel, and a conversion that keeps its bytes in place, read their source into the read's own storage.
    bool inPlace = source->mode == ConvertMode_Relabel || convertsInPlace(*source);
    inner.storage = inPlace ? prTextureReadStorage(read) : nullptr;
    inner.done = convertDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

void convertRelease(void* PR_NULLABLE context) {
    ConvertSource* source = static_cast<ConvertSource*>(context);
    prTextureSourceRelease(source->inner);
    prMemoryFree(source);
}

// ---------------------------------------------------------------------------------------------------------------------
// Mip levels

// A source that makes the levels that another lacks.
struct MipsSource {
    const PrTextureSource* inner;
    PrTextureDesc innerDesc;
    PrTextureDesc desc;
};

// A read of a source's levels: the source, the read that it answers, and the first of the source's own levels that it
// reads.
struct MipsRead {
    const MipsSource* source;
    PrTextureRead* outer;
    uint32_t firstInner;
};

// The linear light of each 8-bit value of sRGB's encoding, and the sRGB value of each 4096th of linear light.
struct SrgbTables {
    float linear[256];
    uint8_t encoded[4097];
};

// Returns the tables, which the first call fills.
const SrgbTables& srgbTables() {
    static SrgbTables* tables = [] {
        SrgbTables* made = allocZeroed<SrgbTables>();
        for (uint32_t i = 0; i < 256; ++i)
            made->linear[i] = prColorTransferDecode(PrColorTransfer_Srgb, float(i) / 255);
        for (uint32_t i = 0; i <= 4096; ++i)
            made->encoded[i] = unorm8(prColorTransferEncode(PrColorTransfer_Srgb, float(i) / 4096));
        return made;
    }();
    return *tables;
}

// Writes the level after the one at `from`, of `extent`, at `to`: each texel the average of two by two texels of the
// level before, the last row and column repeated where its side is odd.
void halve(PrGpuFormat format, const uint8_t* from, PrUVec2 extent, uint8_t* to) {
    PlainLayout layout = plainLayout(format);
    uint32_t texelBytes = uint32_t(layout.channels) * layout.bytes;
    PrUVec2 half = prTextureLevelExtent(extent, 1);
    bool srgb = prGpuFormatIsSrgb(format);
    const SrgbTables* tables = srgb ? &srgbTables() : nullptr;
    for (uint32_t y = 0; y < half.y; ++y) {
        uint32_t rows[2] = {y * 2 < extent.y ? y * 2 : extent.y - 1, y * 2 + 1 < extent.y ? y * 2 + 1 : extent.y - 1};
        for (uint32_t x = 0; x < half.x; ++x) {
            uint32_t columns[2] = {
                x * 2 < extent.x ? x * 2 : extent.x - 1,
                x * 2 + 1 < extent.x ? x * 2 + 1 : extent.x - 1,
            };
            uint8_t* written = to + (size_t(y) * half.x + x) * texelBytes;
            if (layout.bytes == 1) {
                for (uint32_t c = 0; c < layout.channels; ++c) {
                    bool encoded = tables != nullptr && c < 3;
                    float sum = 0;
                    uint32_t integers = 0;
                    for (uint32_t row : rows) {
                        for (uint32_t column : columns) {
                            uint8_t value = from[(size_t(row) * extent.x + column) * texelBytes + c];
                            if (encoded)
                                sum += tables->linear[value];
                            else
                                integers += value;
                        }
                    }
                    if (encoded)
                        written[c] = tables->encoded[uint32_t(sum / 4 * 4096 + 0.5f)];
                    else
                        written[c] = uint8_t((integers + 2) / 4);
                }
                continue;
            }
            float sum[4] = {0, 0, 0, 0};
            for (uint32_t row : rows) {
                for (uint32_t column : columns) {
                    float values[4];
                    loadStored(layout, from + (size_t(row) * extent.x + column) * texelBytes, values);
                    for (uint32_t c = 0; c < layout.channels; ++c)
                        sum[c] += values[c];
                }
            }
            for (float& value : sum)
                value /= 4;
            storeStored(layout, sum, written);
        }
    }
}

// Completes a read of levels, once the read of the source's own levels is done: copies those that it asks for and makes
// the rest.
void mipsDone(void* PR_NULLABLE context, PrTextureRead* inner) {
    MipsRead* held = static_cast<MipsRead*>(context);
    const MipsSource& source = *held->source;
    PrTextureRead* outer = held->outer;
    uint32_t firstInner = held->firstInner;
    prMemoryFree(held);
    PrStatus status = prTextureReadStatus(inner);
    const PrTextureReadDesc& read = *prTextureReadInfo(outer);
    const PrTextureReadDesc& innerRead = *prTextureReadInfo(inner);
    uint32_t last = read.firstLevel + read.levelCount - 1;
    uint32_t innerLast = source.innerDesc.levelCount - 1;
    const PrTextureDesc& desc = source.desc;
    uint8_t* scratch[2] = {nullptr, nullptr};
    for (uint32_t layer = read.firstLayer; status == PrStatus_Ok && layer < read.firstLayer + read.layerCount; ++layer) {
        for (uint32_t level = firstInner; level <= innerLast && level <= last; ++level) {
            if (level < read.firstLevel)
                continue;
            prMemCopy(prTextureReadStorage(outer) + prTextureReadOffset(&desc, &read, level, layer),
                prTextureReadStorage(inner) + prTextureReadOffset(&source.innerDesc, &innerRead, level, layer),
                prTextureLevelSize(&desc, level));
        }
        const uint8_t* before =
            prTextureReadStorage(inner) + prTextureReadOffset(&source.innerDesc, &innerRead, innerLast, layer);
        for (uint32_t level = innerLast + 1; level <= last; ++level) {
            uint8_t* made = nullptr;
            if (level >= read.firstLevel)
                made = prTextureReadStorage(outer) + prTextureReadOffset(&desc, &read, level, layer);
            if (made == nullptr) {
                uint32_t which = level % 2;
                if (scratch[which] == nullptr)
                    scratch[which] = static_cast<uint8_t*>(prMemoryAlloc(prTextureLevelSize(&desc, innerLast + 1)));
                made = scratch[which];
            }
            halve(desc.format, before, prTextureLevelExtent(desc.extent, level - 1), made);
            before = made;
        }
    }
    prMemoryFree(scratch[0]);
    prMemoryFree(scratch[1]);
    prTextureReadDestroy(inner);
    prTextureReadComplete(outer, status);
}

void mipsRead(void* PR_NULLABLE context, PrTextureRead* read) {
    const MipsSource* source = static_cast<const MipsSource*>(context);
    const PrTextureReadDesc& asked = *prTextureReadInfo(read);
    uint32_t innerLast = source->innerDesc.levelCount - 1;
    PrTextureReadDesc inner = asked;
    if (asked.firstLevel + asked.levelCount - 1 <= innerLast) {
        // The source has every level that the read asks for.
        inner.storage = prTextureReadStorage(read);
        inner.done = [](void* PR_NULLABLE outer, PrTextureRead* done) {
            PrStatus status = prTextureReadStatus(done);
            prTextureReadDestroy(done);
            prTextureReadComplete(static_cast<PrTextureRead*>(outer), status);
        };
        inner.context = read;
        prTextureReadCreate(source->inner, &inner);
        return;
    }
    MipsRead* held = static_cast<MipsRead*>(prMemoryAlloc(sizeof(MipsRead)));
    uint32_t firstInner = asked.firstLevel < innerLast ? asked.firstLevel : innerLast;
    *held = MipsRead{source, read, firstInner};
    inner.firstLevel = firstInner;
    inner.levelCount = innerLast - firstInner + 1;
    inner.storage = nullptr;
    inner.done = mipsDone;
    inner.context = held;
    prTextureReadCreate(source->inner, &inner);
}

void mipsRelease(void* PR_NULLABLE context) {
    MipsSource* source = static_cast<MipsSource*>(context);
    prTextureSourceRelease(source->inner);
    prMemoryFree(source);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrTextureSource : TextureSource {};
struct PrTextureRead : TextureRead {};
struct PrTextureOpener : TextureOpener {};

// ---------------------------------------------------------------------------------------------------------------------
// Descriptions

PrTextureDesc prTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrTextureAlpha alpha) {
    return PrTextureDesc{
        .kind = PrGpuTextureKind_2d,
        .extent = extent,
        .depth = 1,
        .layerCount = 1,
        .levelCount = 1,
        .format = format,
        .alpha = alpha,
    };
}

PrUVec2 prTextureDescDisplayedExtent(const PrTextureDesc* desc) {
    if ((desc->orientation & PrTextureOrientationFlag_Transpose) != 0)
        return PrUVec2{desc->extent.y, desc->extent.x};
    return desc->extent;
}

PrUVec2 prTextureLevelExtent(PrUVec2 extent, uint32_t level) {
    uint32_t width = extent.x >> level;
    uint32_t height = extent.y >> level;
    return PrUVec2{width > 0 ? width : 1, height > 0 ? height : 1};
}

size_t prTextureLevelSize(const PrTextureDesc* desc, uint32_t level) {
    uint32_t depth = 1;
    if (desc->kind == PrGpuTextureKind_3d && desc->depth >> level > 1)
        depth = desc->depth >> level;
    return size_t(prGpuFormatLevelBytes(desc->format, prTextureLevelExtent(desc->extent, level))) * depth;
}

PrTextureLimits prTextureLimitsDefault(void) {
    uint64_t pixels = uint64_t(8192) * 8192;
    return PrTextureLimits{.pixels = pixels, .workingBytes = pixels * 64};
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources

const PrTextureSource* prTextureSourceCreate(const PrTextureSourceDesc* desc) {
    PR_ASSERT(desc->texture.levelCount >= 1 && desc->texture.levelCount <= kPrTextureMaxLevels);
    PR_ASSERT(desc->texture.layerCount >= 1 && desc->formatCount <= kPrTextureSourceMaxFormats);
    PR_ASSERT(desc->formatCount == 0 || desc->reformat != nullptr);
    PrTextureSource* source = allocZeroed<PrTextureSource>();
    source->references.storeRelaxed(1);
    source->id = gNextSourceId.fetchAdd(1) + 1;
    source->desc = *desc;
    if (desc->formatCount != 0)
        prMemCopy(source->formats, desc->formats, desc->formatCount * sizeof(PrGpuFormat));
    source->desc.formats = source->formats;
    return source;
}

const PrTextureSource* prTextureSourceCreatePixels(const PrTextureDesc* desc, PrPixelView pixels) {
    PR_ASSERT(desc->kind == PrGpuTextureKind_2d && desc->levelCount == 1 && desc->layerCount == 1 &&
        isPlain(desc->format));
    PR_ASSERT(pixels.extent.x == desc->extent.x && pixels.extent.y == desc->extent.y);
    size_t row = prGpuFormatRowBytes(desc->format, desc->extent.x);
    PR_ASSERT(row <= pixels.stride || desc->extent.y <= 1);
    size_t size = row * desc->extent.y;
    // The pixels follow their size in one allocation, which is the source's context.
    uint8_t* kept = static_cast<uint8_t*>(prMemoryAlloc(sizeof(size_t) + size + 1));
    prMemCopy(kept, &size, sizeof(size_t));
    for (uint32_t y = 0; y < desc->extent.y; ++y)
        prMemCopy(kept + sizeof(size_t) + y * row, pixels.data + y * pixels.stride, row);
    // An opaque texture's alpha reads as one, whatever the pixels' alpha holds, so the copy holds one.
    PlainLayout layout = plainLayout(desc->format);
    if (desc->alpha == PrTextureAlpha_None && layout.channels == 4) {
        uint8_t one[4] = {255};
        if (layout.bytes == 2) {
            PrFloat16 half = prFloat16FromFloat(1);
            prMemCopy(one, &half, 2);
        } else if (layout.bytes == 4) {
            float full = 1;
            prMemCopy(one, &full, 4);
        }
        size_t texel = size_t(layout.bytes) * 4;
        uint8_t* texels = kept + sizeof(size_t);
        for (size_t at = texel - layout.bytes; at < size; at += texel)
            prMemCopy(texels + at, one, layout.bytes);
    }
    PrTextureSourceDesc source{
        .texture = *desc,
        .read = [](void* PR_NULLABLE context, PrTextureRead* read) {
            const uint8_t* held = static_cast<const uint8_t*>(context);
            size_t bytes = 0;
            prMemCopy(&bytes, held, sizeof(size_t));
            prMemCopy(prTextureReadStorage(read), held + sizeof(size_t), bytes);
            prTextureReadComplete(read, PrStatus_Ok);
        },
        .release = [](void* PR_NULLABLE context) { prMemoryFree(context); },
        .context = kept,
    };
    return prTextureSourceCreate(&source);
}

const PrTextureSource* prTextureSourceCreateCopy(PrPixelView pixels, PrPixelFormat format, PrTextureAlpha alpha) {
    PrTextureDesc desc = prTextureDesc2d(prGpuFormatFromPixelFormat(format), pixels.extent, alpha);
    return prTextureSourceCreatePixels(&desc, pixels);
}

void prTextureSourceRetain(const PrTextureSource* source) {
    source->references.fetchAdd(1);
}

void prTextureSourceRelease(const PrTextureSource* PR_NULLABLE source) {
    if (source == nullptr || source->references.fetchSub(1) != 1)
        return;
    PrTextureSource* held = const_cast<PrTextureSource*>(source);
    if (held->desc.release != nullptr)
        held->desc.release(held->desc.context);
    prMemoryFree(held);
}

const PrTextureDesc* prTextureSourceDesc(const PrTextureSource* source) {
    return &source->desc.texture;
}

uint32_t prTextureSourceReferences(const PrTextureSource* source) {
    return source->references.load();
}

uint64_t prTextureSourceId(const PrTextureSource* source) {
    return source->id;
}

uint32_t prTextureSourceFormats(const PrTextureSource* source, PrGpuFormat* PR_NULLABLE out, uint32_t capacity) {
    for (uint32_t i = 0; i < source->desc.formatCount && i < capacity; ++i)
        out[i] = source->formats[i];
    return source->desc.formatCount;
}

bool prTextureSourceProduces(const PrTextureSource* source, PrGpuFormat format) {
    if (format == source->desc.texture.format)
        return true;
    for (uint32_t i = 0; i < source->desc.formatCount; ++i) {
        if (source->formats[i] == format)
            return true;
    }
    return false;
}

PrStatus prTextureSourceReformat(const PrTextureSource* source, PrGpuFormat format,
        const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    if (format == source->desc.texture.format) {
        prTextureSourceRetain(source);
        *out = source;
        return PrStatus_Ok;
    }
    if (!prTextureSourceProduces(source, format))
        return PrStatus_NotSupported;
    PrStatus status = source->desc.reformat(source->desc.context, format, out);
    if (status != PrStatus_Ok) {
        prTextureSourceRelease(*out);
        *out = nullptr;
    }
    return status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reads

PrTextureReadDesc prTextureReadDescAll(const PrTextureDesc* desc) {
    return PrTextureReadDesc{.levelCount = desc->levelCount, .layerCount = desc->layerCount};
}

size_t prTextureReadSize(const PrTextureDesc* desc, const PrTextureReadDesc* read) {
    size_t size = 0;
    for (uint32_t level = read->firstLevel; level < read->firstLevel + read->levelCount; ++level)
        size += prTextureLevelSize(desc, level) * read->layerCount;
    return size;
}

size_t prTextureReadOffset(const PrTextureDesc* desc, const PrTextureReadDesc* read, uint32_t level, uint32_t layer) {
    PR_ASSERT(level >= read->firstLevel && level < read->firstLevel + read->levelCount);
    PR_ASSERT(layer >= read->firstLayer && layer < read->firstLayer + read->layerCount);
    size_t offset = 0;
    for (uint32_t before = read->firstLevel; before < level; ++before)
        offset += prTextureLevelSize(desc, before) * read->layerCount;
    return offset + prTextureLevelSize(desc, level) * (layer - read->firstLayer);
}

PrTextureRead* prTextureReadCreate(const PrTextureSource* source, const PrTextureReadDesc* desc) {
    const PrTextureDesc& texture = source->desc.texture;
    PR_ASSERT(desc->levelCount >= 1 && desc->firstLevel + desc->levelCount <= texture.levelCount);
    PR_ASSERT(desc->layerCount >= 1 && desc->firstLayer + desc->layerCount <= texture.layerCount);
    PrTextureRead* read = allocZeroed<PrTextureRead>();
    prTextureSourceRetain(source);
    read->source = source;
    read->desc = *desc;
    if (read->desc.storage == nullptr) {
        // One byte more, so that a read of no bytes has storage too.
        read->desc.storage = static_cast<uint8_t*>(prMemoryAlloc(prTextureReadSize(&texture, desc) + 1));
        read->ownsStorage = true;
    }
    read->done.store(false);
    prMutexInit(&read->lock);
    prConditionInit(&read->completed);
    source->desc.read(source->desc.context, read);
    return read;
}

bool prTextureReadIsDone(const PrTextureRead* read) {
    return read->done.load();
}

void prTextureReadWait(PrTextureRead* read) {
    if (read->done.load())
        return;
    prMutexLock(&read->lock);
    while (!read->done.load())
        prConditionWait(&read->completed, &read->lock);
    prMutexUnlock(&read->lock);
}

PrStatus prTextureReadStatus(const PrTextureRead* read) {
    PR_ASSERT(read->done.load());
    return read->status;
}

uint8_t* prTextureReadStorage(const PrTextureRead* read) {
    return read->desc.storage;
}

void prTextureReadDestroy(PrTextureRead* PR_NULLABLE read) {
    if (read == nullptr)
        return;
    // A provider may still hold the lock while it wakes the waiters.
    prMutexLock(&read->lock);
    PR_ASSERT(read->done.load());
    prMutexUnlock(&read->lock);
    if (read->ownsStorage)
        prMemoryFree(read->desc.storage);
    prTextureSourceRelease(read->source);
    prMutexReset(&read->lock);
    prConditionReset(&read->completed);
    prMemoryFree(read);
}

const PrTextureSource* prTextureReadSource(const PrTextureRead* read) {
    return read->source;
}

const PrTextureReadDesc* prTextureReadInfo(const PrTextureRead* read) {
    return &read->desc;
}

void prTextureReadComplete(PrTextureRead* read, PrStatus status) {
    PR_ASSERT(!read->done.load());
    read->status = status;
    // The callback may destroy the read, so the waiters are woken first, under the lock that destroying takes.
    PrTextureReadDoneFn done = read->desc.done;
    void* context = read->desc.context;
    prMutexLock(&read->lock);
    read->done.store(true);
    prConditionBroadcast(&read->completed);
    prMutexUnlock(&read->lock);
    if (done != nullptr)
        done(context, read);
}

// ---------------------------------------------------------------------------------------------------------------------
// Sources that wrap a source

PrTextureConvertDesc prTextureConvertDescDefault(PrGpuFormat format) {
    return PrTextureConvertDesc{.format = format, .alpha = PrTextureAlpha_Straight, .channels = {0, 1, 2, 3}};
}

const PrTextureSource* prTextureSourceCreateConvert(const PrTextureSource* source, const PrTextureConvertDesc* desc) {
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(source);
    for (uint8_t channel : desc->channels)
        PR_ASSERT(channel <= kPrTextureChannelOne);
    ConvertSource* convert = allocZeroed<ConvertSource>();
    prTextureSourceRetain(source);
    convert->inner = source;
    convert->innerDesc = innerDesc;
    convert->convert = *desc;
    PlainLayout in = plainLayout(innerDesc.format);
    PlainLayout out = plainLayout(desc->format);
    // Values change their encoding only between 8 bits and floats, as a sampler reads them.
    convert->decodes = prGpuFormatIsSrgb(innerDesc.format) && out.bytes > 1;
    convert->converts = convert->decodes || (in.bytes > 1 && prGpuFormatIsSrgb(desc->format));
    // A transfer converts in straight alpha, so a conversion of it straightens premultiplied values and multiplies them
    // again.
    PrTextureAlpha innerAlpha = innerDesc.alpha;
    bool premultipliedIn = innerAlpha == PrTextureAlpha_Premultiplied;
    bool premultipliedOut = desc->alpha == PrTextureAlpha_Premultiplied;
    convert->straightens = premultipliedIn && (!premultipliedOut || convert->converts);
    convert->premultiplies = premultipliedOut && innerAlpha != PrTextureAlpha_None &&
        (!premultipliedIn || convert->converts);
    PrTextureDesc made = innerDesc;
    made.format = desc->format;
    made.alpha = desc->alpha;
    // A target of straight alpha over an opaque source stays opaque.
    if (innerAlpha == PrTextureAlpha_None && desc->alpha == PrTextureAlpha_Straight)
        made.alpha = PrTextureAlpha_None;
    convert->desc = made;
    bool sameBits = prGpuFormatPlain(prGpuFormatStored(innerDesc.format)) ==
        prGpuFormatPlain(prGpuFormatStored(desc->format));
    bool alphaKept = innerAlpha == made.alpha || innerAlpha == PrTextureAlpha_None;
    PrGpuSwizzle swizzle = prGpuFormatSwizzle(innerDesc.format);
    bool bytesSwizzle = swizzle == PrGpuSwizzle_None || swizzle == PrGpuSwizzle_Luminance ||
        swizzle == PrGpuSwizzle_Intensity;
    if (sameBits && swizzle == PrGpuSwizzle_None && identityChannels(desc->channels) && alphaKept)
        convert->mode = ConvertMode_Relabel;
    else if (in.bytes == 1 && out.bytes == 1 && bytesSwizzle)
        convert->mode = ConvertMode_Bytes;
    else
        convert->mode = ConvertMode_Numbers;
    PR_ASSERT(convert->mode == ConvertMode_Relabel || (isPlain(innerDesc.format) && isPlain(desc->format)));
    PrTextureSourceDesc wrapped{.texture = made, .read = convertRead, .release = convertRelease, .context = convert};
    return prTextureSourceCreate(&wrapped);
}

const PrTextureSource* prTextureSourceCreateMips(const PrTextureSource* source, uint32_t levelCount) {
    const PrTextureDesc& innerDesc = *prTextureSourceDesc(source);
    PR_ASSERT(isPlain(innerDesc.format) && innerDesc.kind != PrGpuTextureKind_3d);
    uint32_t chain = prGpuMipLevels(innerDesc.extent);
    if (chain > kPrTextureMaxLevels)
        chain = kPrTextureMaxLevels;
    if (levelCount == 0 || levelCount > chain)
        levelCount = chain;
    if (levelCount < innerDesc.levelCount)
        levelCount = innerDesc.levelCount;
    MipsSource* mips = allocZeroed<MipsSource>();
    prTextureSourceRetain(source);
    mips->inner = source;
    mips->innerDesc = innerDesc;
    mips->desc = innerDesc;
    mips->desc.levelCount = levelCount;
    PrTextureSourceDesc wrapped{.texture = mips->desc, .read = mipsRead, .release = mipsRelease, .context = mips};
    return prTextureSourceCreate(&wrapped);
}

// ---------------------------------------------------------------------------------------------------------------------
// Openers

PrTextureOpener* prTextureOpenerCreate(const PrTextureFormat* PR_NULLABLE formats, uint32_t count) {
    PrTextureOpener* opener = allocZeroed<PrTextureOpener>();
    for (uint32_t i = 0; i < count; ++i)
        opener->formats.push(formats[i]);
    return opener;
}

void prTextureOpenerDestroy(PrTextureOpener* PR_NULLABLE opener) {
    if (opener == nullptr)
        return;
    opener->formats.reset();
    prMemoryFree(opener);
}

bool prTextureOpenerSupportsType(const PrTextureOpener* opener, PrStringView type) {
    for (uint32_t i = 0; i < opener->formats.count; ++i) {
        const PrTextureFormat& format = opener->formats.data[i];
        if (format.supportsTypeFn(format.context, type))
            return true;
    }
    return false;
}

void prTextureOpenerOpen(const PrTextureOpener* opener, const PrBlobSource* bytes, PrStringView name,
        const PrTextureLimits* PR_NULLABLE limits, PrTextureOpenFn done, void* PR_NULLABLE context) {
    TextureOpen* open = static_cast<TextureOpen*>(prMemoryAlloc(sizeof(TextureOpen) + name.size + 1));
    prBlobSourceRetain(bytes);
    *open = TextureOpen{
        .opener = opener,
        .bytes = bytes,
        .name = reinterpret_cast<char*>(open + 1),
        .nameSize = name.size,
        .limits = limits != nullptr ? *limits : prTextureLimitsDefault(),
        .done = done,
        .context = context,
    };
    if (name.size != 0)
        prMemCopy(open->name, name.data, name.size);
    PrBlobReadDesc read = prBlobReadDescDefault();
    read.done = [](void* PR_NULLABLE held, PrBlobRead* read) {
        TextureOpen* open = static_cast<TextureOpen*>(held);
        PrStatus status = prBlobReadStatus(read);
        const PrTextureSource* source = nullptr;
        if (status == PrStatus_Ok)
            status = textureOpen(*open, prBlobReadBlob(read), &source);
        prBlobReadDestroy(read);
        prBlobSourceRelease(open->bytes);
        PrTextureOpenFn opened = open->done;
        void* context = open->context;
        prMemoryFree(open);
        opened(context, status, status == PrStatus_Ok ? source : nullptr);
    };
    read.context = open;
    prBlobReadCreate(bytes, &read);
}

PrStatus prTextureOpenerRead(const PrTextureOpener* opener, const PrBlobSource* bytes, PrStringView name,
        const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out) {
    TextureWait wait{};
    wait.init();
    prTextureOpenerOpen(opener, bytes, name, limits,
        [](void* PR_NULLABLE held, PrStatus status, const PrTextureSource* PR_NULLABLE source) {
            static_cast<TextureWait*>(held)->complete(status, source);
        }, &wait);
    wait.wait();
    *out = static_cast<const PrTextureSource*>(wait.result);
    return wait.status;
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures

void prTextureUpload(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags, PrStringView name,
        PrTextureUploadFn done, void* PR_NULLABLE context) {
    // Nothing is read for a device that cannot take it.
    if (prGpuDeviceIsLost(device)) {
        done(context, PrStatus_Lost, PrTexture{});
        return;
    }
    TextureUpload* upload = allocZeroed<TextureUpload>();
    upload->device = device;
    upload->desc = source->desc.texture;
    upload->flags = flags;
    size_t length = name.size < sizeof(upload->name) - 1 ? name.size : sizeof(upload->name) - 1;
    if (length != 0)
        prMemCopy(upload->name, name.data, length);
    upload->done = done;
    upload->context = context;
    upload->read = prTextureReadDescAll(&upload->desc);
    PrTextureReadDesc read = upload->read;
    read.done = [](void* PR_NULLABLE held, PrTextureRead* read) {
        TextureUpload* upload = static_cast<TextureUpload*>(held);
        PrStatus status = prTextureReadStatus(read);
        PrGpuTexture texture{};
        if (status == PrStatus_Ok)
            status = uploadTexels(*upload, read, &texture);
        prTextureReadDestroy(read);
        PrTextureUploadFn finished = upload->done;
        void* context = upload->context;
        PrTexture made{.texture = texture, .alpha = upload->desc.alpha};
        prMemoryFree(upload);
        finished(context, status, made);
    };
    read.context = upload;
    prTextureReadCreate(source, &read);
}

PrStatus prTextureUploadRead(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags,
        PrStringView name, PrTexture* out) {
    TextureWait wait{};
    wait.init();
    PrTexture made{};
    struct Held {
        TextureWait* wait;
        PrTexture* made;
    } held{&wait, &made};
    prTextureUpload(device, source, flags, name, [](void* PR_NULLABLE context, PrStatus status, PrTexture texture) {
        Held* h = static_cast<Held*>(context);
        *h->made = texture;
        h->wait->complete(status, nullptr);
    }, &held);
    wait.wait();
    *out = made;
    return wait.status;
}

uint64_t prTextureUploadSize(const PrGpuDevice* device, const PrTextureDesc* desc, PrTextureUploadFlags flags) {
    PrGpuTextureDesc gpu = uploadDesc(device, *desc, flags);
    PrTextureDesc all = *desc;
    all.levelCount = gpu.levels;
    uint64_t size = 0;
    for (uint32_t level = 0; level < all.levelCount; ++level)
        size += prTextureLevelSize(&all, level) * (desc->kind == PrGpuTextureKind_3d ? 1 : desc->layerCount);
    return size;
}

// ---------------------------------------------------------------------------------------------------------------------
// Helpers for sources

uint32_t prTexturePixelSize(PrTextureChannels channels, PrTextureSample sample) {
    return layoutOf(channels).count * sampleSize(sample);
}

void prTexturePixelsUnpack(PrPixelView stored, PrTextureChannels channels, PrTextureSample sample, PrGpuFormat format,
        PrPixels out) {
    PR_ASSERT(format == PrGpuFormat_R8 || format == PrGpuFormat_Rgba8 || format == PrGpuFormat_Rgba16F ||
        format == PrGpuFormat_Rgba32F);
    ChannelLayout layout = layoutOf(channels);
    uint32_t bytes = sampleSize(sample);
    uint32_t pixelBytes = layout.count * bytes;
    bool gray = channels == PrTextureChannels_Gray || channels == PrTextureChannels_GrayAlpha;
    if (format == PrGpuFormat_R8) {
        // The first channel, gray or red, alone.
        for (uint32_t y = 0; y < stored.extent.y; ++y) {
            const uint8_t* row = stored.data + y * stored.stride;
            uint8_t* to = out.data + y * out.stride;
            for (uint32_t x = 0; x < stored.extent.x; ++x)
                to[x] = sampleU8(sample, row + size_t(x) * pixelBytes);
        }
        return;
    }
    if (format == PrGpuFormat_Rgba8 && sample == PrTextureSample_U8 && (channels == PrTextureChannels_Rgb ||
            channels == PrTextureChannels_Rgba)) {
        // Bytes of color, with opaque alpha where none is stored.
        for (uint32_t y = 0; y < stored.extent.y; ++y) {
            const uint8_t* row = stored.data + y * stored.stride;
            uint8_t* to = out.data + y * out.stride;
            if (channels == PrTextureChannels_Rgba)
                prMemCopy(to, row, size_t(stored.extent.x) * 4);
            else
                expandRgbRow(row, to, stored.extent.x);
        }
        return;
    }
    // By channel of the texel, the stored channel that fills it, or none for a channel that is not stored.
    int32_t sources[4] = {0, gray ? 0 : 1, gray ? 0 : 2, -1};
    if (channels == PrTextureChannels_Rg)
        sources[2] = -1;
    if (layout.alpha)
        sources[3] = int32_t(layout.count) - 1;
    for (uint32_t y = 0; y < stored.extent.y; ++y) {
        const uint8_t* row = stored.data + y * stored.stride;
        uint8_t* to = out.data + y * out.stride;
        for (uint32_t x = 0; x < stored.extent.x; ++x) {
            const uint8_t* from = row + size_t(x) * pixelBytes;
            for (uint32_t c = 0; c < 4; ++c) {
                const uint8_t* sampleBytes = sources[c] >= 0 ? from + uint32_t(sources[c]) * bytes : nullptr;
                if (format == PrGpuFormat_Rgba8) {
                    uint8_t value = c == 3 ? 255 : 0;
                    if (sampleBytes != nullptr)
                        value = sampleU8(sample, sampleBytes);
                    to[size_t(x) * 4 + c] = value;
                    continue;
                }
                float value = c == 3 ? 1.0f : 0.0f;
                if (sampleBytes != nullptr)
                    value = sampleValue(sample, sampleBytes);
                if (format == PrGpuFormat_Rgba16F) {
                    PrFloat16 half = prFloat16FromFloat(value);
                    prMemCopy(to + size_t(x) * 8 + c * 2, &half, 2);
                } else {
                    prMemCopy(to + size_t(x) * 16 + c * 4, &value, 4);
                }
            }
        }
    }
}
