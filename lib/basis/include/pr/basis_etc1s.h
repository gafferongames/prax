// Basis Universal's ETC1S: one of the two formats that a KTX 2.0 texture holds in place of one that a GPU samples. A
// texture is blocks of the ETC1 subset, each two indices into codebooks that the whole texture shares, entropy coded
// (BasisLZ).
//
// KTX File Format Specification, version 2, appendices C and D -
// https://registry.khronos.org/KTX/specs/2.0/ktxspec.v2.html

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What the slices of an ETC1S texture share: the codebooks of endpoints and selectors, the Huffman codes of a slice,
// and where each image's slices are in its level.
typedef struct PrBasisEtc1s PrBasisEtc1s;

// Where the slices of one image are in the bytes of its mip level. An image without a second slice has none of its
// size.
typedef struct PrBasisEtc1sImage {
    uint32_t colorOffset;
    uint32_t colorSize;
    uint32_t alphaOffset;
    uint32_t alphaSize;
} PrBasisEtc1sImage;

// Reads the global data of a texture of `imageCount` images: the levels of its layers' faces, by level, then layer,
// then face. Returns InvalidData for data that is not what the specification describes, and NotSupported for a texture
// with an image that depends on the one before it, as the frames of a video do. On failure `*out` is null.
PrStatus prBasisEtc1sCreate(PrByteView global, uint32_t imageCount, PrBasisEtc1s* PR_NULLABLE* PR_NONNULL out);

// Frees what prBasisEtc1sCreate read.
void prBasisEtc1sDestroy(PrBasisEtc1s* PR_NULLABLE etc1s);

// Returns the number of bytes that the texture's shared data takes once read.
size_t prBasisEtc1sWorkingBytes(const PrBasisEtc1s* etc1s);

// Returns where the slices of image `index` are.
PrBasisEtc1sImage prBasisEtc1sImage(const PrBasisEtc1s* etc1s, uint32_t index);

// What a slice is decoded to.
enum {
    PrBasisEtc1sTarget_Pixels,
    PrBasisEtc1sTarget_Bc1,       // blocks of BC1, of the slice's color
    PrBasisEtc1sTarget_Bc3Color,  // blocks of BC1 of four colors, as BC3's color is, of the slice's color
    PrBasisEtc1sTarget_Bc4,       // blocks of BC4, of one channel of the slice's color
    PrBasisEtc1sTarget_Etc1,      // blocks of ETC1, which are blocks of ETC2 RGB8, of the slice's color
    PrBasisEtc1sTarget_EacAlpha,  // blocks of 8-bit EAC, as ETC2 RGBA8's alpha, of one channel of the slice's color
    PrBasisEtc1sTarget_EacR11,    // blocks of EAC R11, of one channel of the slice's color
};
typedef uint8_t PrBasisEtc1sTarget;

// Decodes the slice in `slice`, an image of `extent` texels, into pixels of `pixelBytes` bytes each in rows `stride`
// bytes apart. It writes `count` bytes of each pixel from byte `offset`: red, green and blue for a count of three, and
// for a count of one the channel `component` of the slice's color, zero for red or one for green. Returns false for a
// slice that is corrupt, after which the pixels are unspecified.
//
// For a target of blocks, it writes the 8 bytes of each block instead, from byte `offset` of `pixelBytes` bytes that a
// block has, in rows of blocks `stride` bytes apart: BC1 of the color, or BC4 or EAC of the channel `component`.
//
// A block of ETC1 is the slice's own block, and loses nothing. A block of BC1 or of EAC is the one whose colors or
// values come nearest those of the block, which the library's tables hold for its base color, its modifiers and the
// selectors that it uses. These are the blocks that Basis Universal's transcoder writes. A block of BC4 is likewise
// from tables, of the ends whose eight values come nearest the values of the set of selectors that the block uses.
bool prBasisEtc1sDecodeSlice(const PrBasisEtc1s* etc1s, PrByteView slice, PrUVec2 extent, PrBasisEtc1sTarget target,
    uint8_t* out, size_t stride, uint32_t pixelBytes, uint32_t offset, uint32_t count, uint32_t component);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
