// Reading the elements of glTF accessors.

#pragma once

#include "gltf/src/gltf_document.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The numbers that an accessor's components are unpacked into.
enum {
    GltfNumber_Float,       // float
    GltfNumber_Integer,     // uint32_t
};
typedef uint8_t GltfNumber;

// Returns the size of a component in bytes, the number of components in an element, and the size of an element in bytes
// with the padding of a matrix's columns (glTF-2.0/#accessor-element-size).
uint32_t gltfComponentSize(PrGltfComponentType type);
uint32_t gltfComponentCount(PrGltfElementType type);
uint32_t gltfElementSize(PrGltfElementType type, PrGltfComponentType componentType);

// Returns the offset in bytes of component `component` of an element, in the order of prGltfAccessorReadFloats.
uint32_t gltfComponentOffset(PrGltfElementType type, PrGltfComponentType componentType, uint32_t component);

// Returns the bytes of a buffer view, or the null view if its buffer has no bytes.
PrByteView gltfBufferViewBytes(const Gltf* gltf, PrGltfBufferViewIndex bufferView);

// Returns true if every buffer that the accessor reads has bytes.
bool gltfAccessorHasBytes(const Gltf* gltf, const PrGltfAccessor& accessor);

// Returns sparse index `at` of an accessor, whose sparse indices must have bytes.
uint32_t gltfSparseIndex(const Gltf* gltf, const PrGltfAccessor& accessor, uint32_t at);

// Unpacks `count` elements of an accessor from element `first` into `out`, an array of the type that `number` names, as
// prGltfAccessorReadFloats and prGltfAccessorReadIntegers describe. The accessor's sparse indices must increase
// strictly, which reading an asset checks before anything else reads the accessor.
bool gltfAccessorRead(const Gltf* gltf, const PrGltfAccessor& accessor, uint32_t first, uint32_t count,
    GltfNumber number, void* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
