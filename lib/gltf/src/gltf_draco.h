// Filling the accessors of a primitive from its Draco stream (KHR_draco_mesh_compression).

#pragma once

#include "pr/span.h"

#include "gltf/src/gltf_document.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An attribute of a primitive that its Draco stream holds: the attribute's accessor, and the id of the stream's
// attribute.
struct GltfDracoAttribute {
    PrGltfAccessorIndex accessor;
    uint32_t id;
};

// Decodes `stream`, the Draco stream of a primitive of triangles with the accessor `indices` and `vertexCount`
// vertices, and gives the indices and each of `attributes` its elements, in the layout of its accessor, which the
// document keeps. The stream must be a mesh of exactly the accessors' counts, and each of its attributes must have the
// component type and the number of components of its accessor. An accessor that an earlier stream filled must get the
// same elements. Returns Ok, or NotSupported for a stream that lib/draco does not read and InvalidData for any other,
// with what is wrong with the stream in `*message`.
PrStatus gltfDracoDecode(Gltf* gltf, PrByteView stream, PrGltfAccessorIndex indices, uint32_t vertexCount,
    Span<const GltfDracoAttribute> attributes, PrStringView* message);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
