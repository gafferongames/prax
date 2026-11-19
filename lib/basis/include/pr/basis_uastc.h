// Basis Universal's UASTC: one of the two formats that a KTX 2.0 texture holds in place of one that a GPU samples. A
// texture is blocks of a subset of ASTC 4x4, 16 bytes each, laid out otherwise.
//
// UASTC LDR 4x4 Texture Specification - https://github.com/BinomialLLC/basis_universal/wiki

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Decodes the UASTC block at `block`, of 16 bytes, to its 16 texels, in rows from the first, each red, green, blue and
// alpha. Returns false for a block of the mode that the format reserves or of a pattern that it does not define.
bool prBasisUastcDecodeBlock(const uint8_t* block, uint8_t* texels);

// Transcodes the UASTC block at `block` to the block of BC7 at `out`, of 16 bytes, which decodes to nearly the same
// texels: the mode of BC7 that has the block's subsets and planes, with its endpoints rounded to the bits that the mode
// has, and its weights where BC7 has weights of as many bits. Returns false as prBasisUastcDecodeBlock does.
bool prBasisUastcTranscodeBc7(const uint8_t* block, uint8_t* out);

// Transcodes the UASTC block at `block` to the block of ASTC 4x4 at `out`, of 16 bytes, which decodes to the same
// texels: the block's own endpoints and weights, laid out as ASTC lays them out, with the endpoints of a subset that
// ASTC would read with blue contracted exchanged, and its weights inverted. Returns false as prBasisUastcDecodeBlock
// does.
bool prBasisUastcTranscodeAstc(const uint8_t* block, uint8_t* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
