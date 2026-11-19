// Reading glTF JSON into a document, and checking it.

#pragma once

#include "gltf/src/gltf_document.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Reads the asset whose JSON is `json` into the empty document `gltf`, and checks everything that a document
// guarantees. `binaryChunk` is the binary chunk of binary glTF, or the null view if the asset has none. Returns what
// prGltfCreate returns. If reading fails, the document holds part of the asset, and the caller resets it.
PrStatus gltfParse(Gltf* gltf, PrStringView json, PrByteView binaryChunk, const PrGltfOptions& options);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
