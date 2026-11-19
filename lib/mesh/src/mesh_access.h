// Mesh access: reading and writing the caller's attributes and indices, and the arithmetic that the mesh functions
// share.
//
// The module's results must be the same on every platform. Its sources are compiled without floating-point contraction
// (lib/mesh/CMakeLists.txt), and they use only the operations that IEEE 754 defines exactly: arithmetic and the square
// root. The functions of pr/vec.h are defined in that header, so they are compiled here under the same rule. Angles
// come from meshAcos instead of the C library, whose results differ between platforms.

#pragma once

#include "pr/float.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/raw_array.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns true if `view` can be read as values of `floatCount` floats: its format is at least that many 32-bit floats.
inline bool meshAttributeIsReadable(PrMeshAttributeView view, uint32_t floatCount) {
    if (view.count == 0)
        return true;
    PrMeshFormat format = view.format;
    return view.data != nullptr && format.component == PrMeshComponent_Float32 && format.count >= floatCount &&
        format.count <= 4 && view.stride >= format.count * sizeof(float);
}

// Returns `attribute` as a view of what it holds.
inline PrMeshAttributeView meshAttributeView(PrMeshAttribute attribute) {
    return PrMeshAttributeView{attribute.data, attribute.count, attribute.stride, attribute.format};
}

// Return the value of vertex `index`.
inline PrVec3 meshReadVec3(PrMeshAttributeView view, uint32_t index) {
    PrVec3 value;
    prMemCopy(&value, static_cast<const uint8_t*>(view.data) + size_t(index) * view.stride, sizeof(value));
    return value;
}
inline PrVec2 meshReadVec2(PrMeshAttributeView view, uint32_t index) {
    PrVec2 value;
    prMemCopy(&value, static_cast<const uint8_t*>(view.data) + size_t(index) * view.stride, sizeof(value));
    return value;
}

// Writes `floatCount` floats as the value of vertex `index`.
inline void meshWriteFloats(PrMeshAttribute attribute, uint32_t index, const float* values, uint32_t floatCount) {
    prMemCopy(static_cast<uint8_t*>(attribute.data) + size_t(index) * attribute.stride, values,
        floatCount * sizeof(float));
}

// Returns the size of an index of `type` in bytes, or zero if `type` is not an index type.
inline uint32_t meshIndexSize(PrMeshIndexType type) {
    switch (type) {
        case PrMeshIndexType_U16:
            return 2;
        case PrMeshIndexType_U32:
            return 4;
    }
    return 0;
}

// Returns index `at` of `view`, which must be readable.
inline uint32_t meshIndex(PrMeshIndexView view, uint32_t at) {
    if (view.data == nullptr)
        return at;
    if (view.type == PrMeshIndexType_U16) {
        uint16_t value;
        prMemCopy(&value, static_cast<const uint8_t*>(view.data) + size_t(at) * 2, sizeof(value));
        return value;
    }
    uint32_t value;
    prMemCopy(&value, static_cast<const uint8_t*>(view.data) + size_t(at) * 4, sizeof(value));
    return value;
}

// Writes `value` as index `at` of `out`. Returns false if the value does not fit in the type of `out`.
inline bool meshWriteIndex(PrMeshIndices out, uint32_t at, uint32_t value) {
    if (out.type == PrMeshIndexType_U16) {
        if (value > 0xffffu)
            return false;
        uint16_t narrow = uint16_t(value);
        prMemCopy(static_cast<uint8_t*>(out.data) + size_t(at) * 2, &narrow, sizeof(narrow));
        return true;
    }
    prMemCopy(static_cast<uint8_t*>(out.data) + size_t(at) * 4, &value, sizeof(value));
    return true;
}

// Appends the indices of `view` to `out` as 32-bit integers. Returns InvalidData for an index type that is not one, and
// OutOfRange for an index that is not less than `vertexCount`. `out` is the caller's to reset in every case.
PrStatus meshIndicesLoad(PrMeshIndexView view, uint32_t vertexCount, RawArray<uint32_t>* out);

// Loads the indices of a triangle list as meshIndicesLoad does. Returns InvalidData if their count is not a multiple of
// three.
PrStatus meshTrianglesLoad(PrMeshIndexView view, uint32_t vertexCount, RawArray<uint32_t>* out);

// Returns true if every coordinate of `v` is finite.
inline bool meshIsFinite(PrVec3 v) {
    return prF32IsFinite(v.x) && prF32IsFinite(v.y) && prF32IsFinite(v.z);
}

// Returns the arccosine of `x` in radians, for `x` in [-1, 1], to within a few units in the last place of a double. The
// result is the same on every platform.
double meshAcos(double x);

// Writes the sine and the cosine of the angle `turns` of a whole turn, to within a few units in the last place of a
// double. The result is the same on every platform, and exact at each quarter of a turn.
void meshTurnSinCos(double turns, double* sine, double* cosine);

// Returns a unit vector perpendicular to `normal`, the same one for the same normal. Returns (1, 0, 0) if the normal
// has no direction.
PrVec3 meshPerpendicular(PrVec3 normal);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
