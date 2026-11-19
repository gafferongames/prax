// Filling the accessors of a primitive from its Draco stream (KHR_draco_mesh_compression).

#include "gltf/src/gltf_draco.h"

#include "pr/draco.h"
#include "pr/mem.h"
#include "pr/raw_array.h"

#include "gltf/src/gltf_accessor.h"

namespace pr_impl {

namespace {

constexpr PrStringView kOtherElements = "gives an accessor other elements than another primitive's stream gave it";

// Returns true if a Draco attribute's components are those of an accessor's component type.
bool sameComponentType(PrDracoComponentType type, PrGltfComponentType componentType) {
    switch (componentType) {
        case PrGltfComponentType_Byte:
            return type == PrDracoComponentType_Int8;
        case PrGltfComponentType_UnsignedByte:
            return type == PrDracoComponentType_Uint8;
        case PrGltfComponentType_Short:
            return type == PrDracoComponentType_Int16;
        case PrGltfComponentType_UnsignedShort:
            return type == PrDracoComponentType_Uint16;
        case PrGltfComponentType_UnsignedInt:
            return type == PrDracoComponentType_Uint32;
        case PrGltfComponentType_Float:
            return type == PrDracoComponentType_Float32;
    }
    return false;
}

// Keeps `size` bytes as the elements of an accessor. Returns false if the accessor has other elements already.
bool keep(Gltf* gltf, PrGltfAccessorIndex accessor, const uint8_t* bytes, size_t size) {
    const uint8_t*& decoded = gltf->decodedAccessors.data[accessor];
    if (decoded != nullptr)
        return prMemEquals(decoded, bytes, size);
    uint8_t* copy = gltfAllocDecoded(gltf, size);
    prMemCopy(copy, bytes, size);
    decoded = copy;
    return true;
}

// Fills the accessors from a decoded geometry. Returns what is wrong with the geometry, or the null view.
PrStringView fill(Gltf* gltf, const PrDraco* draco, PrGltfAccessorIndex indices, uint32_t vertexCount,
        Span<const GltfDracoAttribute> attributes, RawArray<uint8_t>* scratch) {
    const PrGltfAccessor& indexAccessor = gltf->accessors.data[indices];
    if (prDracoGeometry(draco) != PrDracoGeometry_TriangleMesh)
        return "is not a stream of a triangle mesh";
    if (prDracoPointCount(draco) != vertexCount)
        return "does not have as many points as the primitive's attributes have elements";
    if (uint64_t(prDracoFaceCount(draco)) * 3 != indexAccessor.count)
        return "does not have as many indices as the primitive's indices";

    uint32_t indexSize = gltfComponentSize(indexAccessor.componentType);
    uint32_t indexLimit = indexSize == 4 ? 0xffffffffu : (1u << (8 * indexSize)) - 1;
    const uint32_t* faces = prDracoFaces(draco);
    scratch->resize(indexAccessor.count * indexSize);
    for (uint32_t i = 0; i < indexAccessor.count; ++i) {
        if (faces[i] > indexLimit)
            return "has an index that the componentType of the primitive's indices does not hold";
        for (uint32_t b = 0; b < indexSize; ++b)
            scratch->data[size_t(i) * indexSize + b] = uint8_t(faces[i] >> (8 * b));
    }
    if (!keep(gltf, indices, scratch->data, scratch->count))
        return kOtherElements;

    for (const GltfDracoAttribute& attribute : attributes) {
        const PrGltfAccessor& accessor = gltf->accessors.data[attribute.accessor];
        uint32_t found = prDracoFindAttribute(draco, attribute.id);
        if (found == kPrDracoNone)
            return "has no attribute with an id that attributes names";
        const PrDracoAttribute& info = prDracoAttributes(draco)[found];
        uint32_t components = gltfComponentCount(accessor.type);
        if (info.componentCount != components || !sameComponentType(info.componentType, accessor.componentType))
            return "has an attribute whose type is not its accessor's";
        // An element's components are tightly packed in the stream, and laid out as the accessor's type lays them out
        // in the document, which pads the columns of a matrix.
        uint32_t componentSize = gltfComponentSize(accessor.componentType);
        uint32_t elementSize = gltfElementSize(accessor.type, accessor.componentType);
        PrByteView values = prDracoAttributeValues(draco, found);
        if (elementSize == components * componentSize) {
            if (!keep(gltf, attribute.accessor, values.data, values.size))
                return kOtherElements;
            continue;
        }
        scratch->clear();
        scratch->resize(vertexCount * elementSize);
        prMemZero(scratch->data, scratch->count);
        for (uint32_t v = 0; v < vertexCount; ++v) {
            uint8_t* element = scratch->data + size_t(v) * elementSize;
            const uint8_t* value = values.data + size_t(v) * components * componentSize;
            for (uint32_t c = 0; c < components; ++c) {
                prMemCopy(element + gltfComponentOffset(accessor.type, accessor.componentType, c),
                    value + size_t(c) * componentSize, componentSize);
            }
        }
        if (!keep(gltf, attribute.accessor, scratch->data, scratch->count))
            return kOtherElements;
    }
    return PrStringView{};
}

} // namespace

PrStatus gltfDracoDecode(Gltf* gltf, PrByteView stream, PrGltfAccessorIndex indices, uint32_t vertexCount,
        Span<const GltfDracoAttribute> attributes, PrStringView* message) {
    // The stream describes exactly what the accessors count, so it is not let describe more.
    PrDracoLimits limits = {.maxPoints = vertexCount, .maxFaces = gltf->accessors.data[indices].count / 3};
    PrDraco* draco = nullptr;
    PrStatus status = prDracoCreate(stream, &limits, &draco);
    if (status == PrStatus_NotSupported) {
        *message = "is a Draco stream of a version or a coding that the module does not read";
        return status;
    }
    if (status == PrStatus_Limit) {
        *message = "describes more points or faces than the primitive's accessors count";
        return PrStatus_InvalidData;
    }
    if (status != PrStatus_Ok) {
        *message = "is not a stream that the Draco bitstream defines";
        return PrStatus_InvalidData;
    }
    RawArray<uint8_t> scratch;
    *message = fill(gltf, draco, indices, vertexCount, attributes, &scratch);
    scratch.reset();
    prDracoDestroy(draco);
    return message->data == nullptr ? PrStatus_Ok : PrStatus_InvalidData;
}

} // namespace pr_impl
