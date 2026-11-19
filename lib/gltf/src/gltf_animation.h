// Evaluating glTF animation channels.

#pragma once

#include "gltf/src/gltf_document.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Writes the value of `channel` at `seconds` to `out`, as prGltfAnimationChannelEvaluate describes.
bool gltfAnimationChannelEvaluate(const Gltf* gltf, const PrGltfAnimationChannel& channel, float seconds, float* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
