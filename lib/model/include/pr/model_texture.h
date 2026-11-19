// The textures of a model's material system: how an image's source becomes the chain of sources that a renderer's
// texture is uploaded from, for what the materials read of it and the device that samples it (pr/texture.h).
//
// The binder makes one texture of an image for each use that differs in what it needs: the kind that the material types
// declare, and whether it is sampled as color, through the Srgb form of its format, or as data, through the plain form,
// which the asset's declaration of the texture decides. It gives the policy the image's source, the use, with the
// channels that every material of the use reads, and the device. The policy returns the chain and how it is uploaded. A
// host replaces the policy through PrModelOptions::chain.
//
// A texture's format is all that it says of its color. An asset's working color space is not handled per texture.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/model_source.h"
#include "pr/render_shaders.h"
#include "pr/status.h"
#include "pr/texture.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// What a model's materials need of one texture of an image: what the material types declare of it, color or data, the
// channels of its value that they read, some of red to alpha, and how its texels are sampled.
typedef struct PrModelTextureUse {
    PrRenderTextureKind kind;
    PrRenderChannelFlags channels;
    PrModelTextureColor color;
} PrModelTextureUse;

// The chain that a texture is uploaded from, with a reference that the caller releases, and how it is uploaded.
typedef struct PrModelTextureChain {
    const PrTextureSource* PR_NULLABLE source;
    PrTextureUploadFlags flags;
} PrModelTextureChain;

// Called to make the chain of a texture of `use` over `source`, the source of the image, for `device`, or for no device
// if it is null, at `*out`. Returns NotSupported if the device samples no chain that the policy makes. Called on any
// thread, and on several at once.
typedef PrStatus (*PrModelTextureChainFn)(void* PR_NULLABLE context, const PrTextureSource* source,
    const PrModelTextureUse* use, const PrGpuDevice* PR_NULLABLE device, PrModelTextureChain* out);

// The default policy, which reads no context.
//
// - Blocks that the device samples are kept, unless the texture has one level larger than a texel and decodes, when
//   its pixels are kept so that the device makes its mip chain, which does more for a minified texture than its blocks
//   save.
// - A texture of several levels that transcodes becomes the first of ASTC 4x4, BC7, EAC R11, BC4, EAC RG11, BC5, ETC2
//   RGBA8, BC3, ETC2 RGB8 and BC1 that it transcodes to and the device samples, with those of one channel first for a
//   use that reads one. A format of one channel is sampled as gray, through its swizzled form
//   (PrGpuSwizzle_Luminance).
// - Floats become 16-bit floats of the same values.
// - Data of which the materials read one channel becomes R8: the channel in red, or on a device of swizzled formats any
//   channel, read through R8's Luminance form, or for alpha its Intensity form, with the mip chain made on the CPU,
//   since the device renders to no swizzled format.
// - Anything else becomes Rgba8 of straight alpha.
// - A use of sRGB color takes the Srgb form of its format, and a use of data the plain form, through a conversion that
//   relabels the bits. A format without a Srgb form is not drawn as sRGB color, so the format alone says how a sampler
//   reads the values.
// - A chain of one level whose format the device renders to has its mip chain made on the device.
PrStatus prModelTextureChainDefault(void* PR_NULLABLE context, const PrTextureSource* source,
    const PrModelTextureUse* use, const PrGpuDevice* PR_NULLABLE device, PrModelTextureChain* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
