// The tangent generators that lib/mesh's tangents are compared with: the reference implementation of MikkTSpace, and
// meshoptimizer's generator in its two modes. Each takes the same unindexed triangle list and writes four floats for
// each corner, as prMeshTangents does: the tangent, then the handedness.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

namespace tangents {

// An unindexed triangle list: corner k is vertex k, and corner 3t + i is vertex i of triangle t.
struct Corners {
    const float* positions; // three floats each
    const float* normals;   // three floats each
    const float* texcoords; // two floats each
    uint32_t count;
};

// Writes MikkTSpace's tangent for each corner (genTangSpaceDefault, through m_setTSpaceBasic). Returns false if the
// reference fails, which it does for a list without triangles or if it runs out of memory.
bool mikktspace(const Corners& corners, float* out);

// Writes meshoptimizer's tangent for each corner (meshopt_generateTangents): with meshopt_TangentCompatible if
// `compatible`, which weights as MikkTSpace does, and otherwise with its own weighting.
void meshoptimizer(const Corners& corners, bool compatible, float* out);

} // namespace tangents

PR_ASSUME_NONNULL_END
