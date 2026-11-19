// Mesh normals: normals computed from the positions of a triangle list.

#pragma once

#include "pr/base.h"
#include "pr/mesh.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Writes to `normals` a unit normal for each vertex of `positions`, three floats each. `normals` holds
// `positions.count` values. A vertex's normal is the sum of the normals of the triangles that use it, each weighted by
// the triangle's angle at the vertex, so the result does not depend on how a surface is cut into triangles. A normal
// points to the side from which its triangles wind counterclockwise.
//
// A triangle without area, or with a coordinate that is not finite, contributes nothing. A vertex that is left without
// a normal, because no triangle contributes or because the contributions cancel, gets (0, 0, 1).
//
// Vertices share a normal only if triangles share the vertex, so a mesh whose faces do not share vertices gets the
// normals of its faces. No coordinate is written as negative zero, so equal normals have equal bytes and weld. Takes
// time proportional to the indices.
PrStatus prMeshNormals(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshAttribute normals);

// Writes to `normals` the unit normal of each triangle, once for each of the triangle's three indices. `normals` holds
// `indices.count` values, three floats each, and value k belongs to index k. These are the normals of the vertices that
// prMeshVerticesUnindex makes from the same indices, because flat shading needs vertices that triangles do not share.
// prMeshWeldRemap then shares the vertices that are still equal.
//
// A triangle without area, or with a coordinate that is not finite, gets (0, 0, 1). No coordinate is written as
// negative zero.
PrStatus prMeshFlatNormals(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshAttribute normals);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
