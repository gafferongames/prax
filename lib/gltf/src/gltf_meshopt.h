// Decoding the streams of meshopt compression (EXT_meshopt_compression, KHR_meshopt_compression).

#pragma once

#include "pr/gltf.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Decodes a stream of the form that `meshopt` describes into its `count` elements of `byteStride` bytes at `out`, and
// applies its filter, as prGltfMeshoptDecode describes.
bool gltfMeshoptDecode(PrByteView source, const PrGltfMeshopt& meshopt, uint8_t* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
