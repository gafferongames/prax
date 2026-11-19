// Mesh shapes: polygon meshes that approximate the quadric surfaces and the solids of planes.

#pragma once

#include "pr/base.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The kinds of shape. Each is centered on the origin and turned about its axis.
enum {
    PrMeshShapeKind_Sphere,     // of `radius`
    PrMeshShapeKind_Box,        // of `size` along each axis, the shape's axis taking z's
    PrMeshShapeKind_Cylinder,   // of `height` along its axis, from `radiusBottom` to `radiusTop`, with both ends capped
    PrMeshShapeKind_Capsule,    // a cylinder of `radius` and `height`, with a hemisphere of `radius` on each end
    PrMeshShapeKind_Plane,      // of `size.x` by `size.y`, facing along its axis
};
typedef uint8_t PrMeshShapeKind;

// A shape. Its axis is 0, 1 or 2, for x, y or z. The shape is made about z, and then the axes turn in their cycle, so
// that z becomes the shape's axis: an axis of x takes x to y and y to z, and an axis of y takes x to z and y to x.
typedef struct PrMeshShapeDesc {
    PrMeshShapeKind kind;
    uint8_t axis;
    uint8_t _pad[2];
    float radius;
    float radiusBottom;         // a cylinder's radius at its bottom, the negative end of its axis
    float radiusTop;            // a cylinder's radius at its top, which is a cone's apex at zero
    float height;
    PrVec3 size;
    uint32_t segments;          // the number of edges around a round shape's axis, at least 3
} PrMeshShapeDesc;

// The sizes of a shape's mesh: its points, its faces, and the corners of all its faces.
typedef struct PrMeshShapeCounts {
    uint32_t pointCount;
    uint32_t faceCount;
    uint32_t cornerCount;
} PrMeshShapeCounts;

// Returns the sizes of the mesh that prMeshShapeWrite writes for `desc`.
PrMeshShapeCounts prMeshShapeCounts(const PrMeshShapeDesc* desc);

// Writes a shape's mesh: its points, the number of corners of each face, the point of each corner, and the unit normal
// of the surface at each corner. Each array holds what prMeshShapeCounts gives. A face winds counterclockwise seen from
// outside the shape, and a plane faces along its axis. A round shape's points lie on its surface, and its normals are
// the surface's at those points, so an edge between two faces that the surface joins smoothly shades smoothly. An edge
// where the surface folds, as a cylinder's rim, has a normal on each side. A cylinder's end of radius zero is a point,
// as a cone's apex is, and has no cap. The same desc writes the same bytes on every platform.
void prMeshShapeWrite(const PrMeshShapeDesc* desc, PrVec3* points, uint32_t* faceCounts, uint32_t* corners, PrVec3* normals);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
