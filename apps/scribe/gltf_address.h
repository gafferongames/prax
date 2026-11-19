// The address handler of glTF assets, JSON and binary: a step is a JSON Pointer into the asset's JSON (RFC 6901), the
// notation that KHR_animation_pointer addresses properties with.
//
// `/images/N`, `/bufferViews/N` and `/buffers/N` reach the bytes of the image, the buffer view or the buffer, and any
// other pointer that names a value of the JSON is a location, as `/nodes/3` or `/meshes/0/primitives/1`. The images,
// buffers and buffer views that an asset names by URI are read beside it, relative to its address.

#pragma once

#include "pr/gltf.h"
#include "pr/json.h"

#include "apps/scribe/address.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

// Creates the handler of glTF assets. The caller frees it with addressHandlerDestroy.
AddressHandler* gltfAddressHandlerCreate();

// Returns the document of a node that a glTF handler opens, opening it if it is not open, or null if the node is not a
// glTF asset or does not read. The document is valid while the node is held.
const PrGltf* PR_NULLABLE gltfAddressDocument(AddressNode* node);

// Returns the JSON of a node that a glTF handler opens, which its steps name values of, opening the node if it is not
// open, or null as gltfAddressDocument does. The JSON is valid while the node is held.
const PrJson* PR_NULLABLE gltfAddressJson(AddressNode* node);

} // namespace scribe

PR_ASSUME_NONNULL_END
