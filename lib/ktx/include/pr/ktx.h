// KTX 2.0: the container of GPU textures, which holds the mip levels of a texture's layers and faces in a format that a
// GPU samples (pr/basis.h), as pixels, or in one of Basis Universal's two formats, which transcode to one. A texture of
// blocks decodes to pixels where Basis Universal's library decodes its blocks, and one of Basis Universal's formats
// decodes to pixels too. A level that is supercompressed with Zstandard or zlib inflates through the compression codec
// that the texture is given.
//
// KTX File Format Specification, version 2 - https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html

#pragma once

#include "pr/base.h"
#include "pr/basis.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/pixels.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The bytes of a file's header, which places the rest of what comes before its levels' data, and the most mip levels
// that a texture has: those of a side of 2^31 texels.
enum {
    kPrKtxHeaderBytes = 80,
    kPrKtxMaxLevels = 32,
};

// Returns true if `bytes` begin with the identifier of a KTX 2.0 file.
bool prKtxHasSignature(PrByteView bytes);

// Returns how many bytes from the start of a file hold all that comes before its levels' data, given its first bytes
// `start`: the header, which places the rest, then the index of levels, the descriptor, the key and value data and the
// supercompression's global data. Returns kPrKtxHeaderBytes for fewer bytes than that that may begin a file, and zero
// for bytes that are not a KTX 2.0 file's. A texture created from that many bytes reports all of its info, and is
// Truncated.
uint64_t prKtxInfoBytes(PrByteView start);

// How the bytes of a level become texels.
enum {
    PrKtxPayload_Pixels,    // rows of texels of one to four channels of one sample type
    PrKtxPayload_Blocks,    // blocks of a format that a GPU samples
    PrKtxPayload_Etc1s,     // Basis Universal ETC1S slices, which the texture's codebooks decode (BasisLZ)
    PrKtxPayload_Uastc,     // Basis Universal UASTC blocks
};
typedef uint8_t PrKtxPayload;

// How a level is compressed after its payload was formed.
enum {
    PrKtxScheme_None,
    PrKtxScheme_BasisLz,    // the payload is ETC1S
    PrKtxScheme_Zstd,
    PrKtxScheme_Zlib,
};
typedef uint8_t PrKtxScheme;

// How a channel of the pixels that a texture decodes to is stored, in the machine's byte order. Unsigned integers map
// to [0, 1].
enum {
    PrKtxSample_U8,
    PrKtxSample_U16,
    PrKtxSample_F16,
    PrKtxSample_F32,
};
typedef uint8_t PrKtxSample;

// How the alpha of the pixels that a texture decodes to relates to their color.
enum {
    PrKtxAlpha_None,
    PrKtxAlpha_Straight,
    PrKtxAlpha_Premultiplied,
};
typedef uint8_t PrKtxAlpha;

// What the values of a texture's color channels mean, as its descriptor's transfer function and primaries say.
enum {
    PrKtxColor_Data,        // neither: linear values of unspecified primaries are data, not color
    PrKtxColor_Srgb,        // sRGB encoded
    PrKtxColor_SrgbLinear,  // linear light in the primaries of BT.709
};
typedef uint8_t PrKtxColor;

// A texture, as its header, its index, its descriptor and its metadata describe it.
//
// A texture's layers and faces are its frames: the faces of each layer in turn, in the order +X, -X, +Y, -Y, +Z, -Z.
// Each frame has `levelCount` mip levels, each half the one before, rounded down (prKtxLevelExtent).
//
// The pixels that it decodes to are rows of `channels` channels of `sample`: one channel is gray, three are red, green
// and blue, and four are those with alpha. A texture of two values decodes to red and green, with blue zero.
typedef struct PrKtxInfo {
    PrUVec2 extent;             // of the first level
    uint32_t layerCount;        // at least one
    uint32_t faceCount;         // one, or six for a cube
    uint32_t levelCount;        // at least one
    PrKtxPayload payload;
    PrKtxScheme scheme;
    PrBasisBlocks blocks;       // the format of a texture of blocks, and None for any other
    bool blocksDecode;          // true if the texture's blocks decode to pixels
    uint64_t transcodes;        // a bit for each PrBasisBlocks that prKtxTextureTranscode makes of the texture
    uint8_t channels;
    PrKtxSample sample;
    PrKtxAlpha alpha;
    PrKtxColor color;

    // The KTXorientation of the texture: whether its columns run from the right, and its rows from the bottom.
    bool fromRight;
    bool fromBottom;

    // The bytes of the first level, the largest, once it is inflated, which a read of it holds while it reads, or zero
    // for a level that is read where it is stored.
    uint64_t inflatedBytes;
} PrKtxInfo;

// Returns the extent of mip level `level` of a texture of `extent`: each level is half the one before, rounded down,
// and at least one texel.
PrUVec2 prKtxLevelExtent(PrUVec2 extent, uint32_t level);

// A texture open for reading. Reads of a texture that is prepared may run on any number of threads at once.
typedef struct PrKtxTexture PrKtxTexture;

// Reads the texture in `bytes`, which must stay valid until the texture is destroyed, without its levels' data.
// Supercompressed levels inflate through `codec`, which outlives the texture, and are NotSupported without one. Returns
// InvalidData for a file whose structure is not that of the specification, NotSupported for a texture that is not read:
// a volume, a format that is neither Basis Universal's, nor pixels, nor blocks of PrBasisBlocks, a supercompression
// scheme that is not the specification's own, or an animation with frames that depend on others, and Truncated for a
// file that ends before its levels do, of which the rest is read. `*out` is the texture if the status is Ok or
// Truncated, and null otherwise.
PrStatus prKtxTextureCreate(PrByteView bytes, const PrCompressCodecSet* PR_NULLABLE codec,
    PrKtxTexture* PR_NULLABLE* PR_NONNULL out);

// Frees the texture.
void prKtxTextureDestroy(PrKtxTexture* PR_NULLABLE texture);

// Returns what the texture is.
const PrKtxInfo* prKtxTextureInfo(const PrKtxTexture* texture);

// Reads what the levels of the texture share, which every read needs: the codebooks of ETC1S. Returns InvalidData for
// data that is not what the specification describes. It is called once, before the first read.
PrStatus prKtxTexturePrepare(PrKtxTexture* texture);

// Returns the number of bytes that prKtxTexturePrepare allocated.
uint64_t prKtxTextureSharedBytes(const PrKtxTexture* texture);

// Returns the number of bytes of the blocks of `level`, which the texture has, of a texture of blocks, or zero for any
// other.
size_t prKtxTextureBlocksSize(const PrKtxTexture* texture, uint32_t level);

// Decodes `level` of `frame`, which the texture has, into `output`, of the level's extent, in the format of the
// texture's info: the pixels that it holds, or those that its blocks or Basis Universal's formats decode to. Returns
// InvalidData for a level that is corrupt or does not inflate to the size that the texture gives it, NotSupported for
// blocks that do not decode or a scheme that the codec does not decode, and Truncated for a level that the bytes end
// before.
PrStatus prKtxTextureRead(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrPixels output);

// Copies the blocks of `level` of `frame` of a texture of blocks to `out`, which has room for prKtxTextureBlocksSize
// bytes: rows of blocks without anything between them. Returns NotSupported for a texture that is not of blocks, and
// the failures of prKtxTextureRead otherwise.
PrStatus prKtxTextureReadBlocks(const PrKtxTexture* texture, uint32_t frame, uint32_t level, uint8_t* out, size_t size);

// Transcodes `level` of `frame` of a texture of one of Basis Universal's formats to blocks of `target`, one of its
// `transcodes`, without decoding it to pixels, at `out`, which has room for prBasisBlocksSize bytes of the level's
// extent: rows of blocks as prKtxTextureReadBlocks writes them. The color becomes BC1 or ETC2 RGB8, and with alpha BC3
// or ETC2 RGBA8; one value becomes BC4 or EAC R11, and two values BC5 or EAC RG11; UASTC becomes BC7 or ASTC 4x4.
// Returns NotSupported for a target that is not among the texture's `transcodes`, and the failures of prKtxTextureRead
// otherwise.
PrStatus prKtxTextureTranscode(const PrKtxTexture* texture, uint32_t frame, uint32_t level, PrBasisBlocks target,
    uint8_t* out, size_t size);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
