// Mesh polygons: faces of any number of vertices cut into triangles.
//
// A polygon mesh lists, for each face, the number of its corners, and for each corner, the vertex that it is at, face
// after face, as OpenUSD's meshes and Wavefront OBJ write them. A corner is a position in that list, so what a file
// gives each corner of each face, such as OpenUSD's face-varying values, follows a triangle's corners.

#pragma once

#include "pr/base.h"
#include "pr/mesh.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Returns the number of triangles that prMeshPolygonsTriangulate cuts `polygonCount` polygons into, whose numbers of
// corners are `counts`: n - 2 of a polygon of n corners, and none of a polygon of fewer than three. Returns UINT32_MAX
// if the number does not fit in 32 bits.
uint32_t prMeshPolygonTriangleCount(const uint32_t* PR_NULLABLE counts, uint32_t polygonCount);

// Cuts polygons into triangles. `corners` holds the vertex of each corner of each polygon, polygon after polygon, and
// `counts` the number of each polygon's corners. Writes three corners for each triangle to `triangles`, as positions in
// `corners`, which holds 3 * prMeshPolygonTriangleCount values, and, if `polygons` is not null, the polygon of each
// triangle to `polygons`, which holds one value for each triangle. A polygon of fewer than three corners is passed
// over.
//
// The triangles of a polygon cover it and wind as it does, with its corners in its order about the normal of the plane
// that fits it best (Newell's). A polygon that is convex in that plane is a fan from its first corner, so a triangle or
// a quad keeps the triangles that a fan gives it. A polygon that is not convex is cut by ear clipping in that plane,
// which keeps every triangle inside it. A polygon that ear clipping cannot cut, because it crosses itself or has no
// area, is a fan. Takes time proportional to the corners of the convex polygons, and up to the cube of the corners of
// each one that is not convex.
//
// Returns InvalidData if the counts do not sum to `corners.count`, and OutOfRange for a corner whose vertex is not less
// than `positions.count`. A position that is not finite makes its polygon a fan.
PrStatus prMeshPolygonsTriangulate(PrMeshAttributeView positions, PrMeshIndexView corners,
    const uint32_t* PR_NULLABLE counts, uint32_t polygonCount, uint32_t* PR_NULLABLE triangles, uint32_t* PR_NULLABLE polygons);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
