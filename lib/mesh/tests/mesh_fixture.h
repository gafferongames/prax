// The fixture of the mesh tests: views of the arrays of the shapes that tools/harness/mesh_shapes.h builds.

#pragma once

#include "pr/array.h"
#include "pr/mesh.h"
#include "pr/vec.h"

#include "tools/harness/mesh_shapes.h"

#include "tools/harness/test.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

using namespace harness;
using namespace pr;

// Views of a shape's arrays, to read and to write.
inline PrMeshAttributeView meshView(const Array<PrVec3>& values) {
    return PrMeshAttributeView{values.data(), values.count(), sizeof(PrVec3), prMeshFormatFloat32(3)};
}
inline PrMeshAttributeView meshView(const Array<PrVec2>& values) {
    return PrMeshAttributeView{values.data(), values.count(), sizeof(PrVec2), prMeshFormatFloat32(2)};
}
inline PrMeshIndexView meshView(const Array<uint32_t>& indices) {
    return PrMeshIndexView{indices.data(), indices.count(), PrMeshIndexType_U32};
}
inline PrMeshIndices meshIndices(Array<uint32_t>& indices) {
    return PrMeshIndices{indices.data(), indices.count(), PrMeshIndexType_U32};
}
inline PrMeshAttribute meshAttribute(Array<PrVec3>& values) {
    return PrMeshAttribute{values.data(), values.count(), sizeof(PrVec3), prMeshFormatFloat32(3)};
}

// A view of `count` vertices that are not indexed.
inline PrMeshIndexView meshSequence(uint32_t count) {
    return PrMeshIndexView{nullptr, count, PrMeshIndexType_U32};
}

// Returns true if `a` and `b` are no farther apart than `tolerance`.
inline bool meshTestNear(PrVec3 a, PrVec3 b, float tolerance) {
    return prVec3Length(a - b) <= tolerance;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
