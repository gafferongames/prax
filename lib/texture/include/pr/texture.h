// Textures: what a shader samples, which is a GPU texture and how its alpha relates to its color, made from sources of
// bits that are read again whenever a texture is made.
//
// A source describes its bits (PrTextureDesc): their kind, extent, layers and levels, one GPU format, and their alpha.
// It produces the bits of the levels and layers that a read asks for, later or during the call. The simplest source is
// an application's or a format's own bits (prTextureSourceCreate). Every other source wraps a source and changes its
// format alone: a format's library decodes or transcodes its encoded bytes (prTextureSourceReformat), and this library
// converts pixels (prTextureSourceCreateConvert) and makes mip levels (prTextureSourceCreateMips). A consumer chooses
// the chain for its use and its device. A texture is uploaded from a chain (prTextureUpload), and holds no data: after
// a device is lost, the consumer uploads the chain again, which reads its bits again.
//
// Sources, reads and openers follow the library's default thread contract: a source is immutable, and its reads may run
// on any number of threads at once. A read completes on the thread that its innermost provider completes it on.

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/byte_view.h"
#include "pr/color.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Descriptions

// The most mip levels that a texture has: those of a side of 32768 texels.
enum { kPrTextureMaxLevels = 16 };

// How a texture's alpha relates to its color channels.
enum {
    PrTextureAlpha_None,            // no alpha: the texture is opaque, and its alpha reads as 1
    PrTextureAlpha_Straight,        // opacity, not applied to the color channels
    PrTextureAlpha_Premultiplied,   // opacity, already multiplied into the color channels
};
typedef uint8_t PrTextureAlpha;

// How a texture's stored texels map to its displayed ones: the displayed texel at (x, y) is the stored one at (y, x) if
// the texture is transposed, and then mirrored across as FlipX says and down as FlipY says. A consumer that shows the
// texture upright applies it, and one that samples it as data, as a material does, ignores it.
enum {
    PrTextureOrientationFlag_FlipX = 1u << 0,
    PrTextureOrientationFlag_FlipY = 1u << 1,
    PrTextureOrientationFlag_Transpose = 1u << 2,
};
typedef uint8_t PrTextureOrientationFlags;

// The bits of a texture.
//
// Its layers are those of lib/gpu: a cube has six, one for each face in the order +X, -X, +Y, -Y, +Z, -Z, and an array
// of cubes six for each element. Each layer has `levelCount` mip levels, each half the one before, rounded down, and at
// least one texel (prTextureLevelExtent). `format` is a GPU format: plain texels, blocks, or a swizzled form of either
// (PrGpuSwizzle), whose Srgb forms decode as sRGB where a sampler reads them.
//
// The format is all that a texture says of the color of its values: a Srgb form holds values of sRGB's transfer
// function, and a plain form values that a sampler reads as they are. A texture carries no primaries. A source is of
// the format that its bits are in, and one that does not know whether its values are color, such as a PNG's, which may
// be a normal map, is of the plain form. A consumer that samples values as color relabels them to the Srgb form
// (prTextureSourceCreateConvert).
typedef struct PrTextureDesc {
    PrGpuTextureKind kind;
    PrUVec2 extent;                     // of the first level
    uint32_t depth;                     // the depth of a 3D texture, and one for any other
    uint32_t layerCount;                // at least one
    uint32_t levelCount;                // at least one, at most kPrTextureMaxLevels
    PrGpuFormat format;
    PrTextureAlpha alpha;
    PrTextureOrientationFlags orientation;
} PrTextureDesc;

// Returns the description of a 2D texture of one layer and one level, in its stored orientation.
PrTextureDesc prTextureDesc2d(PrGpuFormat format, PrUVec2 extent, PrTextureAlpha alpha);

// Returns the extent of a texture of `desc` as it is displayed: its first level's, transposed if its orientation
// transposes.
PrUVec2 prTextureDescDisplayedExtent(const PrTextureDesc* desc);

// Returns the extent of mip level `level` of a texture whose first level is of `extent`.
PrUVec2 prTextureLevelExtent(PrUVec2 extent, uint32_t level);

// Returns the bytes of level `level` of one layer of `desc`: its rows of texels or blocks with nothing between them,
// and for a 3D texture its slices one after another.
size_t prTextureLevelSize(const PrTextureDesc* desc, uint32_t level);

// The bounds within which a format opens a texture: its texels, and the bytes that a read holds at once, such as a
// level that it inflates.
typedef struct PrTextureLimits {
    uint64_t pixels;
    uint64_t workingBytes;
} PrTextureLimits;

// Returns limits that admit a texture of 8192 texels to a side.
PrTextureLimits prTextureLimitsDefault(void);

// ---------------------------------------------------------------------------------------------------------------------
// Sources

// What produces a texture's bits. A source is immutable and reference counted.
typedef struct PrTextureSource PrTextureSource;

// A read of a source's bits, which completes once.
typedef struct PrTextureRead PrTextureRead;

// Called with a source's `context` to begin `read`: the provider writes the levels and layers that the read names
// (prTextureReadInfo) to its storage (prTextureReadStorage), and completes it then or later, on any thread
// (prTextureReadComplete). A provider that reads bytes reads them from their byte provider (pr/blob.h) for each read.
typedef void (*PrTextureSourceReadFn)(void* PR_NULLABLE context, PrTextureRead* read);

// Called with a source's `context` to create a source of the same texture whose bits are in `format`, one of the
// source's formats (PrTextureSourceDesc::formats). Returns the status that prTextureSourceReformat reports.
typedef PrStatus (*PrTextureSourceReformatFn)(void* PR_NULLABLE context, PrGpuFormat format,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// Called with a source's `context` once the source's last reference is released, on the thread that releases it.
typedef void (*PrTextureSourceReleaseFn)(void* PR_NULLABLE context);

// The most formats that a source produces of its bits.
enum { kPrTextureSourceMaxFormats = 32 };

// What a source is: its bits' description, the function that reads them, and the other formats that it produces of them
// itself, which a format's library decodes or transcodes to, through `reformat`. `context` is valid until `release` is
// called with it.
typedef struct PrTextureSourceDesc {
    PrTextureDesc texture;
    PrTextureSourceReadFn read;
    PrTextureSourceReformatFn PR_NULLABLE reformat;
    const PrGpuFormat* PR_NULLABLE formats;     // other than `texture.format`, at most kPrTextureSourceMaxFormats
    uint32_t formatCount;
    PrTextureSourceReleaseFn PR_NULLABLE release;
    void* PR_NULLABLE context;
} PrTextureSourceDesc;

// Returns a source whose reads `desc->read` begins, which copies `desc->formats`. The caller holds the source's one
// reference.
const PrTextureSource* prTextureSourceCreate(const PrTextureSourceDesc* desc);

// Returns a source of a 2D texture of one level and one layer of `desc`, of a plain format or a swizzled form of one,
// whose bits are a copy of `pixels` that it keeps, of the texture's extent. It is for bits that nothing can produce
// again, as an application's pixels that change after the call. Without alpha, the copy's alpha is one, whatever the
// pixels' alpha holds. The caller holds the source's one reference.
const PrTextureSource* prTextureSourceCreatePixels(const PrTextureDesc* desc, PrPixelView pixels);

// Returns a source of a copy of `pixels`, a 2D texture of `format` and `alpha`, as prTextureSourceCreatePixels does.
const PrTextureSource* prTextureSourceCreateCopy(PrPixelView pixels, PrPixelFormat format, PrTextureAlpha alpha);

// Takes a reference to the source, or releases one. The last release releases the source's context, which for a source
// that wraps another releases that one.
void prTextureSourceRetain(const PrTextureSource* source);
void prTextureSourceRelease(const PrTextureSource* PR_NULLABLE source);

// Returns the description of the source's bits, which does not change.
const PrTextureDesc* prTextureSourceDesc(const PrTextureSource* source);

// Returns the number of references to the source, for a cache that keeps what others hold. Another thread may change it
// as it returns.
uint32_t prTextureSourceReferences(const PrTextureSource* source);

// Returns a number that is unique to the source for the life of the process.
uint64_t prTextureSourceId(const PrTextureSource* source);

// Writes up to `capacity` of the formats that the source produces of its bits other than its own, without converting
// them, and returns their number: the blocks that it transcodes to, or pixels of another precision that it decodes to.
uint32_t prTextureSourceFormats(const PrTextureSource* source, PrGpuFormat* PR_NULLABLE out, uint32_t capacity);

// Returns true if the source produces its bits in `format`: its own, or one of prTextureSourceFormats.
bool prTextureSourceProduces(const PrTextureSource* source, PrGpuFormat format);

// Creates a source of the same texture whose bits are in `format`, which the source produces, at `*out`: the source
// itself, retained, for its own format, and otherwise the format library's source that decodes or transcodes to it.
// Returns NotSupported for a format that the source does not produce. On failure, `*out` is null.
PrStatus prTextureSourceReformat(const PrTextureSource* source, PrGpuFormat format,
    const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Reads

// Called once when a read completes, with the read, on the thread that completes it.
typedef void (*PrTextureReadDoneFn)(void* PR_NULLABLE context, PrTextureRead* read);

// What a read asks for: the levels from `firstLevel` and the layers from `firstLayer`, and the storage that receives
// them, or null for storage that the read allocates and frees when it is destroyed. The storage holds prTextureReadSize
// bytes: the levels in order, each its layers in order, each prTextureLevelSize bytes. `done`, if it is given, is
// called once the read completes.
typedef struct PrTextureReadDesc {
    uint32_t firstLevel;
    uint32_t levelCount;
    uint32_t firstLayer;
    uint32_t layerCount;
    uint8_t* PR_NULLABLE storage;
    PrTextureReadDoneFn PR_NULLABLE done;
    void* PR_NULLABLE context;              // passed to `done`
} PrTextureReadDesc;

// Returns a description of a read of every level and layer of `desc` into storage that the read allocates, without a
// callback.
PrTextureReadDesc prTextureReadDescAll(const PrTextureDesc* desc);

// Return the bytes of the storage that a read of `desc` for `read` fills, and the offset in it of a level's layer.
size_t prTextureReadSize(const PrTextureDesc* desc, const PrTextureReadDesc* read);
size_t prTextureReadOffset(const PrTextureDesc* desc, const PrTextureReadDesc* read, uint32_t level, uint32_t layer);

// Starts a read of the source, which the caller destroys once it has completed (prTextureReadDestroy): in `done`, or
// after it. The read retains the source.
PrTextureRead* prTextureReadCreate(const PrTextureSource* source, const PrTextureReadDesc* desc);

// Returns true if the read has completed. Any thread may poll it.
bool prTextureReadIsDone(const PrTextureRead* read);

// Returns once the read has completed, blocking the calling thread until its provider completes it.
void prTextureReadWait(PrTextureRead* read);

// Return how a completed read went, Ok or the status of its provider; and its storage, which holds the bits of a read
// that succeeded, valid while the read lives.
PrStatus prTextureReadStatus(const PrTextureRead* read);
uint8_t* prTextureReadStorage(const PrTextureRead* read);

// Frees a completed read, and its storage if it allocated it.
void prTextureReadDestroy(PrTextureRead* PR_NULLABLE read);

// For a provider: return the source that a read reads and what it asks for, whose storage is that of
// prTextureReadStorage.
const PrTextureSource* prTextureReadSource(const PrTextureRead* read);
const PrTextureReadDesc* prTextureReadInfo(const PrTextureRead* read);

// For a provider: completes a read with `status`, Ok once its storage holds the bits. The read's callback runs during
// the call. The read must not be completed twice.
void prTextureReadComplete(PrTextureRead* read, PrStatus status);

// ---------------------------------------------------------------------------------------------------------------------
// Sources that wrap a source

// A channel of a conversion's target that takes no channel of its source: zero, or one.
enum {
    kPrTextureChannelZero = 4,
    kPrTextureChannelOne = 5,
};

// A conversion of a source's pixels. The target is `format`, a plain format or a swizzled form of one, of `alpha`, None
// for opaque. Each channel c of the target's stored texels, red to alpha, takes the source's channel `channels[c]` as a
// sampler reads it, 0 to 3, or a constant. Values keep their encoding between formats of 8 bits, so a target of a
// format's Srgb form over values of the plain form says only that they are sRGB's. Between 8 bits and floats, values
// convert as a sampler reads them: a Srgb form's are decoded to linear floats, and floats are encoded for a target of a
// Srgb form. The transfer converts in straight alpha: premultiplied values are made straight before it, and multiplied
// again after it for a Premultiplied target.
typedef struct PrTextureConvertDesc {
    PrGpuFormat format;
    PrTextureAlpha alpha;
    uint8_t channels[4];
} PrTextureConvertDesc;

// Returns a conversion to `format` that keeps the source's alpha, its channels and its values.
PrTextureConvertDesc prTextureConvertDescDefault(PrGpuFormat format);

// Creates a source of `source`'s texture converted as `desc` says, which retains `source`. A conversion that changes
// only the format's sRGB form or swizzle reads the source's bits into the read's storage as they are, and applies to a
// format of any kind, blocks included. Any other applies to plain formats and their swizzled forms.
const PrTextureSource* prTextureSourceCreateConvert(const PrTextureSource* source, const PrTextureConvertDesc* desc);

// Creates a source of `source`'s texture with `levelCount` mip levels, or with every level down to one texel for zero,
// which retains `source`. A level past the source's own is the average of each two by two texels of the level before,
// in linear light for a format's Srgb form. `source` is of a plain format or a swizzled form of one.
const PrTextureSource* prTextureSourceCreateMips(const PrTextureSource* source, uint32_t levelCount);

// ---------------------------------------------------------------------------------------------------------------------
// Openers
//
// An opener makes sources of encoded bytes in the formats that it was built with. A program builds it from the formats
// of the libraries that it links, and hands it to the libraries that load textures, which then depend on no format.

// One format that an opener opens: its functions and their context, which outlives the opener's sources.
// `supportsTypeFn` says if the format reads a MIME type, in lowercase and without parameters. `createFn` creates a
// source over `bytes`, which it retains and reads again for each read of the source. It describes the texture from
// `blob`, the bytes as the opener just read them, which it does not keep, and returns NotSupported for bytes that it
// does not read after all.
typedef struct PrTextureFormat {
    bool (*supportsTypeFn)(const void* PR_NULLABLE context, PrStringView type);
    PrStatus (*createFn)(const void* PR_NULLABLE context, const PrBlobSource* bytes, const PrBlob* blob,
        const PrTextureLimits* limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);
    const void* PR_NULLABLE context;
} PrTextureFormat;

// An ordered list of formats. The order is the priority: a resource opens with the first format that reads its media
// type and does not refuse it as NotSupported.
typedef struct PrTextureOpener PrTextureOpener;

// Creates an opener of `formats`, which it copies.
PrTextureOpener* prTextureOpenerCreate(const PrTextureFormat* PR_NULLABLE formats, uint32_t count);

// Frees the opener. The sources that it made stay valid while their formats' contexts are.
void prTextureOpenerDestroy(PrTextureOpener* PR_NULLABLE opener);

// Returns true if a format of the opener reads the MIME type `type`.
bool prTextureOpenerSupportsType(const PrTextureOpener* opener, PrStringView type);

// Called once with a source that the callee owns a reference to, or with the failure that left it null, on the thread
// that completed the read of the bytes, which may be the thread that opened it, during the call.
typedef void (*PrTextureOpenFn)(void* PR_NULLABLE context, PrStatus status, const PrTextureSource* PR_NULLABLE source);

// Reads the resource that `bytes` provides, which may complete later (pr/blob.h), classifies its first bytes and `name`
// (pr/mime_sniff.h), and calls `done` with a source that the first format that reads the media type and does not refuse
// the bytes makes. The source retains `bytes`. A null `limits` means the defaults, which the call copies. The status is
// that of reading the bytes, NotSupported if no format opens them, Limit for a texture past the limits, and InvalidData
// for one that does not parse.
void prTextureOpenerOpen(const PrTextureOpener* opener, const PrBlobSource* bytes, PrStringView name,
    const PrTextureLimits* PR_NULLABLE limits, PrTextureOpenFn done, void* PR_NULLABLE context);

// Opens a source as prTextureOpenerOpen does, and waits for it, blocking the calling thread until the bytes are read.
// Returns the status that prTextureOpenerOpen reports. On failure, `*out` is null.
PrStatus prTextureOpenerRead(const PrTextureOpener* opener, const PrBlobSource* bytes, PrStringView name,
    const PrTextureLimits* PR_NULLABLE limits, const PrTextureSource* PR_NULLABLE* PR_NONNULL out);

// ---------------------------------------------------------------------------------------------------------------------
// Textures

// A texture, as a shader samples it: a GPU texture, and how its alpha relates to its color. The format of the GPU
// texture says how a sampler reads its values, as sRGB for a format's Srgb form.
typedef struct PrTexture {
    PrGpuTexture texture;
    PrTextureAlpha alpha;
} PrTexture;

// A part of a texture that holds one source's texels: the texture, whose first level is `extent`, and the rect of
// texels within it, on layer `layer`. A consumer samples the rect alone, which may be the whole texture or a part of
// one that others share.
typedef struct PrTextureRegion {
    PrTexture texture;
    PrUVec2 extent;
    uint32_t layer;
    PrRect rect;
} PrTextureRegion;

// How a texture is uploaded.
enum {
    // A texture of one level, in a format that the device filters and renders to, gets every level down to one texel,
    // which the device makes from the first.
    PrTextureUploadFlag_MakesLevels = 1u << 0,
};
typedef uint8_t PrTextureUploadFlags;

// Called once with the uploaded texture, whose reference the callee takes, or with the failure that left it zero, on
// the thread that completed the read of the source's bits.
typedef void (*PrTextureUploadFn)(void* PR_NULLABLE context, PrStatus status, PrTexture texture);

// Reads every level and layer of `source`, which may complete later, creates a sampled GPU texture of its description
// on `device`, uploads the bits, frees them, and calls `done` with the texture. Nothing of the bits is kept: uploading
// the source again, as after a lost device, reads them again. `name` names the GPU texture for tools. The status is
// that of the read, NotSupported for a format that the device does not sample, the failures of prGpuTextureCreate, and
// Lost for a device that is lost before the texture is created.
void prTextureUpload(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags, PrStringView name,
    PrTextureUploadFn done, void* PR_NULLABLE context);

// Uploads `source` as prTextureUpload does, and waits for it, blocking the calling thread until the bits are read and
// the texture is made. Returns the status that prTextureUpload reports. On failure, `*out` is zero.
PrStatus prTextureUploadRead(PrGpuDevice* device, const PrTextureSource* source, PrTextureUploadFlags flags,
    PrStringView name, PrTexture* out);

// Returns the bytes of the GPU texture that prTextureUpload makes of a source of `desc` with `flags` on `device`: every
// level of every layer, those that the device makes included.
uint64_t prTextureUploadSize(const PrGpuDevice* device, const PrTextureDesc* desc, PrTextureUploadFlags flags);

// ---------------------------------------------------------------------------------------------------------------------
// Helpers for sources

// How a channel of stored pixels is laid out, in the machine's byte order. Unsigned integers map to [0, 1], and I16's
// -32767 through 32767 map to [-1, 1].
enum {
    PrTextureSample_U8,
    PrTextureSample_U16,
    PrTextureSample_I16,
    PrTextureSample_F16,
    PrTextureSample_F32,
    PrTextureSample_U32,
};
typedef uint8_t PrTextureSample;

// The channels of a stored pixel, in order.
enum {
    PrTextureChannels_Gray,
    PrTextureChannels_GrayAlpha,
    PrTextureChannels_Rg,
    PrTextureChannels_Rgb,
    PrTextureChannels_Rgba,
};
typedef uint8_t PrTextureChannels;

// Returns the bytes of one stored pixel.
uint32_t prTexturePixelSize(PrTextureChannels channels, PrTextureSample sample);

// Unpacks stored pixels of `channels` and `sample` at `stored` into `out`, of the same extent, in `format`, Rgba8,
// Rgba16F or Rgba32F: without converting their values, with gray in red, green and blue, and opaque alpha where none is
// stored. R8 holds the first channel alone, gray or red. A format of 8 bits rounds each value to the nearest of 255
// steps, and clamps it to [0, 1].
void prTexturePixelsUnpack(PrPixelView stored, PrTextureChannels channels, PrTextureSample sample, PrGpuFormat format,
    PrPixels out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
