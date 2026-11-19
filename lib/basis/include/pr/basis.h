// Basis Universal's library of GPU blocks: the block-compressed formats that a GPU samples, which Basis Universal's
// formats transcode to (pr/basis_etc1s.h, pr/basis_uastc.h), and their decoding to pixels in software, for blocks that
// are wanted as pixels. The decoders cover the formats of BC and of ETC2 whose texels are of 8 bits a channel, or
// become them.
//
// Texture block compression in Direct3D 11, for BC1 to BC5 and BC7 -
// https://learn.microsoft.com/en-us/windows/win32/direct3d11/texture-block-compression-in-direct3d-11
//
// Khronos Data Format Specification 1.3, chapter 21, for ETC2 and EAC -
// https://registry.khronos.org/DataFormat/specs/1.3/dataformat.1.3.html

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The block-compressed formats that a GPU samples: rows of blocks from the first row of texels, each block of
// prBasisBlocksExtent texels in prBasisBlocksBytes bytes. Whether the texels are sRGB encoded is the texture's color.
enum {
    PrBasisBlocks_None,
    PrBasisBlocks_Bc1,
    PrBasisBlocks_Bc2,
    PrBasisBlocks_Bc3,
    PrBasisBlocks_Bc4,          // one channel, unsigned
    PrBasisBlocks_Bc4Snorm,
    PrBasisBlocks_Bc5,          // two channels, unsigned
    PrBasisBlocks_Bc5Snorm,
    PrBasisBlocks_Bc6HUfloat,
    PrBasisBlocks_Bc6HSfloat,
    PrBasisBlocks_Bc7,
    PrBasisBlocks_Etc2Rgb8,
    PrBasisBlocks_Etc2Rgb8A1,
    PrBasisBlocks_Etc2Rgba8,
    PrBasisBlocks_EacR11,       // one channel, unsigned
    PrBasisBlocks_EacR11Snorm,
    PrBasisBlocks_EacRg11,      // two channels, unsigned
    PrBasisBlocks_EacRg11Snorm,
    // ASTC, by the texels of a block: its LDR profile, then its HDR profile, whose texels are floats.
    PrBasisBlocks_Astc4x4,
    PrBasisBlocks_Astc4x4Hdr,
    PrBasisBlocks_Astc5x4,
    PrBasisBlocks_Astc5x4Hdr,
    PrBasisBlocks_Astc5x5,
    PrBasisBlocks_Astc5x5Hdr,
    PrBasisBlocks_Astc6x5,
    PrBasisBlocks_Astc6x5Hdr,
    PrBasisBlocks_Astc6x6,
    PrBasisBlocks_Astc6x6Hdr,
    PrBasisBlocks_Astc8x5,
    PrBasisBlocks_Astc8x5Hdr,
    PrBasisBlocks_Astc8x6,
    PrBasisBlocks_Astc8x6Hdr,
    PrBasisBlocks_Astc8x8,
    PrBasisBlocks_Astc8x8Hdr,
    PrBasisBlocks_Astc10x5,
    PrBasisBlocks_Astc10x5Hdr,
    PrBasisBlocks_Astc10x6,
    PrBasisBlocks_Astc10x6Hdr,
    PrBasisBlocks_Astc10x8,
    PrBasisBlocks_Astc10x8Hdr,
    PrBasisBlocks_Astc10x10,
    PrBasisBlocks_Astc10x10Hdr,
    PrBasisBlocks_Astc12x10,
    PrBasisBlocks_Astc12x10Hdr,
    PrBasisBlocks_Astc12x12,
    PrBasisBlocks_Astc12x12Hdr,
    PrBasisBlocks_Count,
};
typedef uint8_t PrBasisBlocks;

// Return the texels that a block of the format `blocks` covers on each axis, and its bytes, or zero for None.
PrUVec2 prBasisBlocksExtent(PrBasisBlocks blocks);
uint32_t prBasisBlocksBytes(PrBasisBlocks blocks);

// Returns the number of bytes of the blocks of an image of `extent` in the format `blocks`, or zero for None.
size_t prBasisBlocksSize(PrUVec2 extent, PrBasisBlocks blocks);

// Returns true if prBasisBlockDecode decodes blocks of `blocks`: BC1, BC2, BC3, BC4, BC5 and BC7, ETC2 RGB8, RGB8A1 and
// RGBA8, and EAC R11 and RG11. It decodes neither the signed formats nor those of floats, nor ASTC.
bool prBasisBlocksDecodes(PrBasisBlocks blocks);

// Decodes the block of `blocks` at `block` to its 16 texels, in rows from the first, each red, green, blue and alpha of
// 8 bits, at `texels`, which has room for 64 bytes. A format without a channel leaves green and blue zero and alpha
// 255. The 11 bits of EAC are rounded to 8. The format is one that prBasisBlocksDecodes decodes.
void prBasisBlockDecode(PrBasisBlocks blocks, const uint8_t* block, uint8_t* texels);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
