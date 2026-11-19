// Reading the elements of glTF accessors.

#include "gltf/src/gltf_accessor.h"

#include "pr/align.h"
#include "pr/assert.h"
#include "pr/bit_cast.h"
#include "pr/mem.h"

namespace pr_impl {

namespace {

// How the components of one element are laid out in its bytes.
struct ElementLayout {
    uint32_t componentCount;
    uint32_t elementSize;
    uint8_t offsets[16];        // by component: its byte offset in the element
};

// The number of rows of a matrix type, and zero for a scalar or a vector.
uint32_t matrixRows(PrGltfElementType type) {
    switch (type) {
        case PrGltfElementType_Mat2:
            return 2;
        case PrGltfElementType_Mat3:
            return 3;
        case PrGltfElementType_Mat4:
            return 4;
        default:
            return 0;
    }
}

// The bytes from one column of a matrix to the next: its rows, rounded up to a multiple of four bytes.
uint32_t matrixColumnSize(uint32_t rows, uint32_t componentSize) {
    return alignUp(rows * componentSize, 4);
}

ElementLayout layoutOf(PrGltfElementType type, PrGltfComponentType componentType) {
    ElementLayout layout = {};
    layout.componentCount = gltfComponentCount(type);
    layout.elementSize = gltfElementSize(type, componentType);
    uint32_t componentSize = gltfComponentSize(componentType);
    uint32_t rows = matrixRows(type);
    for (uint32_t i = 0; i < layout.componentCount; ++i) {
        if (rows == 0)
            layout.offsets[i] = uint8_t(i * componentSize);
        else
            layout.offsets[i] = uint8_t(i / rows * matrixColumnSize(rows, componentSize) + i % rows * componentSize);
    }
    return layout;
}

// Returns the float that a component stands for. A normalized integer maps onto [0, 1] or [-1, 1]
// (glTF-2.0/#accessor-data-types).
float componentFloat(const uint8_t* at, PrGltfComponentType type, bool normalized) {
    switch (type) {
        case PrGltfComponentType_Byte: {
            float value = float(int8_t(at[0]));
            if (!normalized)
                return value;
            return value < -127.0f ? -1.0f : value / 127.0f;
        }
        case PrGltfComponentType_UnsignedByte:
            return normalized ? float(at[0]) / 255.0f : float(at[0]);
        case PrGltfComponentType_Short: {
            float value = float(int16_t(prMemReadLeU16(at)));
            if (!normalized)
                return value;
            return value < -32767.0f ? -1.0f : value / 32767.0f;
        }
        case PrGltfComponentType_UnsignedShort:
            return normalized ? float(prMemReadLeU16(at)) / 65535.0f : float(prMemReadLeU16(at));
        case PrGltfComponentType_UnsignedInt:
            return float(prMemReadLeU32(at));
        case PrGltfComponentType_Float:
            return bitCast<float>(prMemReadLeU32(at));
    }
    return 0;
}

uint32_t componentInteger(const uint8_t* at, PrGltfComponentType type) {
    switch (type) {
        case PrGltfComponentType_Byte:
            return uint32_t(int32_t(int8_t(at[0])));
        case PrGltfComponentType_UnsignedByte:
            return at[0];
        case PrGltfComponentType_Short:
            return uint32_t(int32_t(int16_t(prMemReadLeU16(at))));
        case PrGltfComponentType_UnsignedShort:
            return prMemReadLeU16(at);
        case PrGltfComponentType_UnsignedInt:
            return prMemReadLeU32(at);
        case PrGltfComponentType_Float: {
            float value = bitCast<float>(prMemReadLeU32(at));
            // The comparisons are false for a NaN, which reads as zero.
            if (value >= 4294967296.0f)
                return 0xffffffffu;
            return value >= 1.0f ? uint32_t(value) : 0;
        }
    }
    return 0;
}

// Returns the position of the first sparse index that names `element` or a later element, by binary search.
uint32_t sparseLowerBound(const Gltf* gltf, const PrGltfAccessor& accessor, uint32_t element) {
    uint32_t low = 0;
    uint32_t high = accessor.sparse.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (gltfSparseIndex(gltf, accessor, middle) < element)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

} // namespace

uint32_t gltfComponentSize(PrGltfComponentType type) {
    switch (type) {
        case PrGltfComponentType_Byte:
        case PrGltfComponentType_UnsignedByte:
            return 1;
        case PrGltfComponentType_Short:
        case PrGltfComponentType_UnsignedShort:
            return 2;
        default:
            return 4;
    }
}

uint32_t gltfComponentCount(PrGltfElementType type) {
    switch (type) {
        case PrGltfElementType_Scalar:
            return 1;
        case PrGltfElementType_Vec2:
            return 2;
        case PrGltfElementType_Vec3:
            return 3;
        case PrGltfElementType_Vec4:
        case PrGltfElementType_Mat2:
            return 4;
        case PrGltfElementType_Mat3:
            return 9;
        default:
            return 16;
    }
}

uint32_t gltfElementSize(PrGltfElementType type, PrGltfComponentType componentType) {
    uint32_t componentSize = gltfComponentSize(componentType);
    uint32_t rows = matrixRows(type);
    if (rows == 0)
        return gltfComponentCount(type) * componentSize;
    return rows * matrixColumnSize(rows, componentSize);
}

uint32_t gltfComponentOffset(PrGltfElementType type, PrGltfComponentType componentType, uint32_t component) {
    return layoutOf(type, componentType).offsets[component];
}

PrByteView gltfBufferViewBytes(const Gltf* gltf, PrGltfBufferViewIndex bufferView) {
    PR_ASSERT(bufferView < gltf->bufferViews.count);
    const PrGltfBufferView& view = gltf->bufferViews.data[bufferView];
    // A compressed view has the bytes that its stream decodes to, if its stream had bytes to decode.
    if (view.meshopt.buffer != kPrGltfNone) {
        const uint8_t* decoded = gltf->decodedViews.data[bufferView];
        return decoded != nullptr ? PrByteView{decoded, size_t(view.byteLength)} : PrByteView{nullptr, 0};
    }
    const PrGltfBuffer& buffer = gltf->buffers.data[view.buffer];
    if (buffer.bytes.data == nullptr)
        return PrByteView{nullptr, 0};
    return PrByteView{buffer.bytes.data + view.byteOffset, size_t(view.byteLength)};
}

// Returns the elements that a Draco stream gave an accessor of the document, or null.
static const uint8_t* decodedElements(const Gltf* gltf, const PrGltfAccessor& accessor) {
    return gltf->decodedAccessors.data[&accessor - gltf->accessors.data];
}

bool gltfAccessorHasBytes(const Gltf* gltf, const PrGltfAccessor& accessor) {
    // The elements of an accessor that a Draco stream fills are the stream's, and not its buffer view's.
    if (accessor.draco) {
        if (decodedElements(gltf, accessor) == nullptr)
            return false;
    } else if (accessor.bufferView != kPrGltfNone && gltfBufferViewBytes(gltf, accessor.bufferView).data == nullptr) {
        return false;
    }
    if (accessor.sparse.count == 0)
        return true;
    return gltfBufferViewBytes(gltf, accessor.sparse.indicesBufferView).data != nullptr &&
        gltfBufferViewBytes(gltf, accessor.sparse.valuesBufferView).data != nullptr;
}

uint32_t gltfSparseIndex(const Gltf* gltf, const PrGltfAccessor& accessor, uint32_t at) {
    PR_ASSERT(at < accessor.sparse.count);
    const PrGltfSparse& sparse = accessor.sparse;
    const uint8_t* indices = gltfBufferViewBytes(gltf, sparse.indicesBufferView).data + sparse.indicesByteOffset;
    return componentInteger(indices + size_t(at) * gltfComponentSize(sparse.indicesComponentType),
        sparse.indicesComponentType);
}

bool gltfAccessorRead(const Gltf* gltf, const PrGltfAccessor& accessor, uint32_t first, uint32_t count,
        GltfNumber number, void* out) {
    PR_ASSERT(first <= accessor.count && count <= accessor.count - first);
    ElementLayout layout = layoutOf(accessor.type, accessor.componentType);
    if (!gltfAccessorHasBytes(gltf, accessor)) {
        prMemZero(out, size_t(count) * layout.componentCount * sizeof(uint32_t));
        return false;
    }

    // The source of the elements: what a Draco stream gave, a buffer view, or zeros.
    const uint8_t* base = nullptr;
    size_t stride = layout.elementSize;
    if (accessor.draco) {
        base = decodedElements(gltf, accessor);
    } else if (accessor.bufferView != kPrGltfNone) {
        base = gltfBufferViewBytes(gltf, accessor.bufferView).data + accessor.byteOffset;
        if (gltf->bufferViews.data[accessor.bufferView].byteStride != 0)
            stride = gltf->bufferViews.data[accessor.bufferView].byteStride;
    }

    // The sparse elements from `first` on replace elements of the source as the walk reaches them.
    const PrGltfSparse& sparse = accessor.sparse;
    const uint8_t* sparseValues = nullptr;
    uint32_t sparseAt = sparse.count;
    uint32_t sparseElement = kPrGltfNone;
    if (sparse.count != 0) {
        sparseValues = gltfBufferViewBytes(gltf, sparse.valuesBufferView).data + sparse.valuesByteOffset;
        sparseAt = sparseLowerBound(gltf, accessor, first);
        if (sparseAt < sparse.count)
            sparseElement = gltfSparseIndex(gltf, accessor, sparseAt);
    }

    static const uint8_t kZeros[64] = {};
    float* floats = static_cast<float*>(out);
    uint32_t* integers = static_cast<uint32_t*>(out);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t element = first + i;
        const uint8_t* source = base != nullptr ? base + size_t(element) * stride : kZeros;
        if (element == sparseElement) {
            source = sparseValues + size_t(sparseAt) * layout.elementSize;
            ++sparseAt;
            sparseElement = sparseAt < sparse.count ? gltfSparseIndex(gltf, accessor, sparseAt) : kPrGltfNone;
        }
        for (uint32_t c = 0; c < layout.componentCount; ++c) {
            const uint8_t* at = source + layout.offsets[c];
            if (number == GltfNumber_Float)
                *floats++ = componentFloat(at, accessor.componentType, accessor.normalized);
            else
                *integers++ = componentInteger(at, accessor.componentType);
        }
    }
    return true;
}

} // namespace pr_impl
