#include "pr/mesh.h"
#include "pr/mesh_bvh.h"
#include "pr/mesh_cache.h"
#include "pr/mesh_normals.h"
#include "pr/mesh_polygons.h"
#include "pr/mesh_tangents.h"

int prTestMeshCApi(void);

// The public headers compiled as C. Views are plain structures over the caller's arrays.
int prTestMeshCApi(void) {
    float positions[] = {0, 0, 0, 2, 0, 0, 0, 2, 0};
    uint16_t indices[] = {0, 1, 2};
    PrMeshAttributeView view = {positions, 3, 3 * sizeof(float), prMeshFormatFloat32(3)};
    PrMeshIndexView indexView = {indices, 3, PrMeshIndexType_U16};
    PrBox3 box;
    float normals[9];
    PrMeshAttribute normalsOut = {normals, 3, 3 * sizeof(float), prMeshFormatFloat32(3)};
    PrMeshBvhNode nodes[1];
    uint32_t order[1];
    PrMeshBvhView bvh;
    PrMeshRay ray = {{0.5f, 0.5f, 1}, 0, {0, 0, -1}, 10};
    PrMeshRayHit hit;
    float ratio;
    int result = prMeshBox(view, &box) != PrStatus_Ok || box.max.x != 2 || box.max.z != 0;
    result |= prMeshNormals(view, indexView, normalsOut) != PrStatus_Ok || normals[2] != 1;
    result |= prMeshBvhBuild(view, indexView, nodes, order, &bvh) != PrStatus_Ok || bvh.nodeCount != 1;
    result |= !prMeshBvhIntersect(bvh, view, indexView, ray, &hit) || hit.triangle != 0 || hit.t != 1;
    result |= prMeshCacheMissRatio(indexView, 3, 16, &ratio) != PrStatus_Ok || ratio != 3;
    result |= prMeshTriangleListCount(PrMeshTopology_TriangleStrip, 5) != 9 || kPrMeshIndexNone != 0xffffffffu;
    {
        uint32_t counts[] = {3};
        uint32_t triangle[3];
        result |= prMeshPolygonTriangleCount(counts, 1) != 1;
        result |= prMeshPolygonsTriangulate(view, indexView, counts, 1, triangle, NULL) != PrStatus_Ok || triangle[2] != 2;
    }
    return result;
}
