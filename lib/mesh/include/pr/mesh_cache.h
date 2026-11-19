// Mesh cache order: triangles and vertices ordered for the caches of a GPU.

#pragma once

#include "pr/base.h"
#include "pr/mesh.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Writes to `out` the triangles of `indices` in an order that reuses recently shaded vertices, so that a GPU shades
// fewer vertices. `out` holds `indices.count` indices. The triangles are the same and each keeps the order of its
// vertices. Only the order of the triangles changes. `out` may be the storage of `indices`.
//
// The order is Forsyth's (Tom Forsyth, "Linear-Speed Vertex Cache Optimisation", 2006). A vertex scores higher the more
// recently it was used and the fewer triangles still need it, and the next triangle is the one whose vertices score
// highest among the triangles of recently used vertices. The search looks at a bounded number of triangles for each
// step, so the time is proportional to the indices for any mesh. The order suits caches of any size and is not the best
// order for any one size.
PrStatus prMeshIndicesOptimizeCache(PrMeshIndexView indices, uint32_t vertexCount, PrMeshIndices out);

// Writes to `out` the average cache miss ratio of a triangle list: the number of vertices that a GPU shades for each
// triangle if it keeps the last `cacheSize` shaded vertices in a first-in, first-out cache. The ratio is 3 if no
// triangle reuses a vertex, and approaches 0.5 for a large regular mesh in a good order. It is zero for no triangles.
// `cacheSize` is at least 3. Hardware differs, and 16 is a common size to measure with.
PrStatus prMeshCacheMissRatio(PrMeshIndexView indices, uint32_t vertexCount, uint32_t cacheSize, float* out);

// Writes to `out` the fetch miss ratio of a triangle list over vertices of `vertexSize` bytes. The vertices are read in
// the order of the indices through a first-in, first-out cache of 256 lines of 64 bytes. The ratio is the number of
// lines loaded, divided by the number of lines that hold a vertex that the indices use. It is 1 if every line is loaded
// once, and greater the more often the triangles return to vertices that have left the cache. Storing the vertices in
// the order of prMeshFetchRemap lowers it. It is zero for no indices.
PrStatus prMeshFetchMissRatio(PrMeshIndexView indices, uint32_t vertexCount, uint32_t vertexSize, float* out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
