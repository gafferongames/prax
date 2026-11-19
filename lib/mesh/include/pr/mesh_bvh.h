// Mesh bounding volume hierarchies: a tree of boxes over a mesh's triangles, for ray queries.

#pragma once

#include "pr/base.h"
#include "pr/mesh.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN
PR_NO_PADDING_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The greatest number of nodes on a path from the root to a leaf. A query's stack is sized for it.
enum { kPrMeshBvhMaxDepth = 64 };

// A node of a hierarchy, 32 bytes. Node 0 is the root. A node refers to its children and its triangles by index, so a
// hierarchy can be copied, stored in a file or read by a shader as it is.
typedef struct PrMeshBvhNode {
    PrVec3 min;                 // the least corner of the box around the node's triangles
    uint32_t first;             // a leaf's first entry in the triangle order, or an interior node's first child. The
                                // second child is the next node
    PrVec3 max;                 // the greatest corner of the box
    uint32_t count;             // the number of a leaf's triangles, or zero for an interior node
} PrMeshBvhNode;

// A hierarchy in the storage that holds it. `triangles` is the triangle order: the triangles of the mesh, as indices of
// triangles, arranged so that each leaf's triangles are consecutive. A hierarchy without triangles has no nodes.
typedef struct PrMeshBvhView {
    const PrMeshBvhNode* PR_NULLABLE nodes;
    uint32_t nodeCount;
    uint32_t triangleCount;
    const uint32_t* PR_NULLABLE triangles;
} PrMeshBvhView;

// A ray, and the part of it that a query searches: the points `origin` + t * `direction` for t from `tMin` to `tMax`.
// The direction need not have unit length.
typedef struct PrMeshRay {
    PrVec3 origin;
    float tMin;
    PrVec3 direction;
    float tMax;
} PrMeshRay;

// Where a ray meets a triangle.
typedef struct PrMeshRayHit {
    uint32_t triangle;          // the triangle's index: its indices are 3 * triangle and the two after it
    float t;                    // the hit is at origin + t * direction

    // The weights of the triangle's second and third vertices at the hit. The weight of the first is one minus both.
    PrVec2 barycentrics;
} PrMeshRayHit;

// Returns the number of nodes that a hierarchy over `triangleCount` triangles can need.
uint32_t prMeshBvhMaxNodes(uint32_t triangleCount);

// Builds a hierarchy over the triangles of a triangle list and writes it to `out`. `nodes` holds prMeshBvhMaxNodes of
// the number of triangles, and `triangles` holds one entry for each triangle. `out` views that storage, and the caller
// may shrink the storage to the counts in `out`. `positions` holds three floats for each vertex.
//
// A triangle with a coordinate that is not finite is left out of the hierarchy, so no ray hits it.
//
// The build splits a node where a binned surface area heuristic costs least: it sorts the node's triangles into 16 bins
// along each axis by the centers of their boxes and tries each boundary between bins. Triangles that the bins cannot
// separate, and nodes deep enough to threaten kPrMeshBvhMaxDepth, are split into halves by count instead. A leaf holds
// at most four triangles. The build takes time proportional to the triangles times the depth of the tree, which is the
// logarithm of their number for ordinary meshes and never more than kPrMeshBvhMaxDepth. The tree is close to the
// quality of a full sweep of split positions, and it is built for queries, not for rebuilding every frame.
PrStatus prMeshBvhBuild(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshBvhNode* PR_NULLABLE nodes,
    uint32_t* PR_NULLABLE triangles, PrMeshBvhView* out);

// Writes to `out` the nodes of `bvh` with the boxes of their triangles at `positions`, for a mesh whose vertices moved
// since the hierarchy was built. `out` holds `bvh.nodeCount` nodes, and may be the storage that `bvh` views, so that
// the hierarchy refits in place. The tree keeps its shape and its triangle order: a triangle that the build left out
// stays out, and a triangle with a coordinate that is not finite adds nothing to its leaf's box. `positions` holds
// three floats for each vertex, and `bvh` must be valid for the mesh of `indices` (prMeshBvhIsValid).
//
// Takes time proportional to the nodes, and allocates nothing. Queries of a refitted tree find what they find in a tree
// built over the same positions, but its boxes grow and overlap as the vertices move away from where they were built,
// which slows each query. prMeshBvhCost measures that, and a build over the new positions restores it.
//
// Returns InvalidData, and writes nothing, if `positions` cannot be read as three floats a vertex. `out` may be null
// for a hierarchy without nodes.
PrStatus prMeshBvhRefit(PrMeshBvhView bvh, PrMeshAttributeView positions, PrMeshIndexView indices,
    PrMeshBvhNode* PR_NULLABLE out);

// Returns the cost that the surface area heuristic gives the hierarchy: the expected number of nodes that a ray through
// its root visits and of triangles that it tests, each node weighted by the area of its box over that of the root. A
// hierarchy over the same triangles costs more as its boxes overlap, so the ratio of the costs of a refitted tree and
// of the tree as it was built tells when a build pays. A root box without area weights every node by one. A hierarchy
// without nodes costs zero. Takes time proportional to the nodes.
float prMeshBvhCost(PrMeshBvhView bvh);

// Returns true if `bvh` is a well-formed hierarchy over a mesh of `triangleCount` triangles: every node is reached once
// from the root, no path is longer than kPrMeshBvhMaxDepth, and every leaf names entries of the triangle order that
// name triangles of the mesh. A hierarchy that prMeshBvhBuild wrote is valid. Check a hierarchy that was read from a
// file before querying it. The check does not compare the boxes with the mesh. Takes time proportional to the
// hierarchy, and allocates a byte for each node.
bool prMeshBvhIsValid(PrMeshBvhView bvh, uint32_t triangleCount);

// Finds the nearest point at which `ray` meets a triangle of the mesh, on either side of the triangle. Returns false if
// the ray meets none. `bvh` must be valid for the mesh (prMeshBvhIsValid), and `positions` and `indices` are those that
// it was built from. A triangle whose indices name no vertex is never hit.
//
// If the ray meets several triangles at the same nearest distance, the hit is one of them, and the same one every time.
// A ray that passes exactly through an edge or a vertex may miss both triangles: the test is not watertight. A ray with
// a coordinate that is not finite meets nothing.
//
// Allocates nothing. Takes time proportional to the depth of the tree for a ray that passes few boxes.
bool prMeshBvhIntersect(PrMeshBvhView bvh, PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshRay ray,
    PrMeshRayHit* out);

#if defined(__cplusplus)
}
#endif

PR_NO_PADDING_END
PR_ASSUME_NONNULL_END
