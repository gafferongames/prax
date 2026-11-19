// Mesh tangents: the tangent space that a normal map is sampled in.

#pragma once

#include "pr/base.h"
#include "pr/mesh.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Writes to `tangents` a tangent for each index of a triangle list, by the MikkTSpace algorithm (Mikkelsen, "Simulation
// of Wrinkled Surfaces Revisited", 2008), which glTF names for a mesh that has a normal map and no tangents
// (glTF-2.0/#meshes-overview). `positions` and `normals` are three floats each, `texcoords` two, and all three have one
// value for each vertex. `tangents` holds `indices.count` values of four floats, and value k belongs to index k.
//
// The first three floats are the tangent: a unit vector perpendicular to the vertex's normal, in the direction in which
// the first texture coordinate increases. The fourth is the handedness, 1 or -1. The bitangent is cross(normal,
// tangent) times the handedness, and points in the direction in which the second texture coordinate increases. glTF's
// normal maps point their green channel toward the top of the image, where glTF's second texture coordinate decreases,
// so a glTF loader negates the handedness.
//
// The tangents are per index because the algorithm splits vertices. Two triangles that share a vertex get the same
// tangent there only if the position, normal and texture coordinate are equal, the triangles are joined around the
// vertex through shared edges, and they map the texture with the same orientation. A caller that wants an indexed mesh
// unshares its vertices (prMeshVerticesUnindex), adds the tangents, and welds (prMeshWeldRemap). No coordinate is
// written as negative zero, so equal tangents have equal bytes and weld.
//
// The result depends only on the triangles' positions, normals and texture coordinates and on the order of the
// triangles. It does not depend on which vertices the indices share.
//
// How the result relates to the reference implementation of MikkTSpace (mikktspace.c, genTangSpaceDefault):
// - It follows the same steps with the same single-precision arithmetic: vertices welded by exact equality, triangles
//   grouped around each vertex by shared edges and orientation, and each group's tangents summed with angle weights.
//   Every corner at which the reference computes a tangent gets the reference's tangent and handedness to the last
//   bit, with the reference compiled without fused multiply-adds, as this library is, and the angles rounded alike
//   (below). tools/suite/tangents checks this over the shapes of this module's tests and over Khronos's glTF sample
//   assets.
// - An edge that more than two triangles share pairs them as the reference does: in the order of their indices, each
//   with the first that winds the other way across the edge and is not yet paired.
// - The reference fails to pair some edges of the vertex whose welded index is the lower end of the edges last in its
//   sort, because it leaves the last run of each sort unsorted. Such triangles fall into separate groups there, at a
//   few corners near the end of a mesh, and here they are paired as the algorithm describes.
// - A corner that the reference leaves without a tangent of its own, which it writes as zero or as (1, 0, 0) with a
//   handedness of -1, is given a unit tangent perpendicular to its normal, with a handedness of 1. This happens if no
//   triangle of the corner's group maps the texture with area, if no group reaches a triangle without texture area, if
//   the triangles' edges at the corner are parallel in the plane of the normal, or if the triangle has two equal
//   positions and no other triangle shares the corner.
// - Angles come from this library's own arccosine, in double precision and then rounded to single, so that every
//   platform computes the same tangents. The reference rounds the platform's acos instead, and an angle can differ only
//   where the two double results straddle a single-precision rounding boundary.
//
// The normals must have unit length for the tangents to be perpendicular to them. A coordinate that is not finite
// spoils only the tangents of the triangles around it, which are then unit vectors of no particular direction.
//
// Takes time proportional to the indices times the logarithm of their number. A vertex whose triangles map the texture
// in directions more than a quarter turn apart, such as the tip of a cone, adds the square of the number of triangles
// around it.
PrStatus prMeshTangents(PrMeshAttributeView positions, PrMeshAttributeView normals, PrMeshAttributeView texcoords,
    PrMeshIndexView indices, PrMeshAttribute tangents);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
