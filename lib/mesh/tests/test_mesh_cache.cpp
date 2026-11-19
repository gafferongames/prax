#include "pr/array.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/mesh_cache.h"
#include "pr/random.h"
#include "pr/sort.h"
#include "pr/status.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

float cacheMissRatio(const Array<uint32_t>& indices, uint32_t vertexCount) {
    float ratio = -1;
    CHECK(prMeshCacheMissRatio(meshView(indices), vertexCount, 16, &ratio) == PrStatus_Ok);
    return ratio;
}

float fetchMissRatio(const Array<uint32_t>& indices, uint32_t vertexCount, uint32_t vertexSize) {
    float ratio = -1;
    CHECK(prMeshFetchMissRatio(meshView(indices), vertexCount, vertexSize, &ratio) == PrStatus_Ok);
    return ratio;
}

// One triangle as three indices, to compare lists of triangles whatever their order.
struct Corners {
    uint32_t indices[3];
};

Array<Corners> sortedTriangles(const Array<uint32_t>& indices) {
    Array<Corners> triangles;
    for (uint32_t i = 0; i + 2 < indices.count(); i += 3)
        triangles.push(Corners{{indices[i], indices[i + 1], indices[i + 2]}});
    sortItems(Span<Corners>(triangles.data(), triangles.count()), [](const Corners& a, const Corners& b) {
        return prMemCompare(&a, &b, sizeof(Corners)) < 0;
    });
    return triangles;
}

bool sameTriangles(const Array<uint32_t>& a, const Array<uint32_t>& b) {
    Array<Corners> left = sortedTriangles(a);
    Array<Corners> right = sortedTriangles(b);
    return left.count() == right.count() &&
        prMemEquals(left.data(), right.data(), size_t(left.count()) * sizeof(Corners));
}

Array<uint32_t> optimized(const Array<uint32_t>& indices, uint32_t vertexCount) {
    Array<uint32_t> out(indices.count());
    CHECK(prMeshIndicesOptimizeCache(meshView(indices), vertexCount, meshIndices(out)) == PrStatus_Ok);
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the cache miss ratio counts the vertices shaded for each triangle") {
    // Two triangles that share an edge shade four vertices. A third that repeats the first shades none.
    uint32_t list[9] = {0, 1, 2, 2, 1, 3, 0, 1, 2};
    Array<uint32_t> indices;
    indices.append(list, 9);
    CHECK(cacheMissRatio(indices, 4) == doctest::Approx(4.0 / 3.0));

    // A cache of three vertices has lost vertex 0 by the time the third triangle asks for it, and loading each of that
    // triangle's vertices pushes out the next.
    float ratio = 0;
    CHECK(prMeshCacheMissRatio(meshView(indices), 4, 3, &ratio) == PrStatus_Ok);
    CHECK(ratio == doctest::Approx(7.0 / 3.0));

    Array<uint32_t> none;
    CHECK(cacheMissRatio(none, 4) == 0);
}

TEST_CASE("optimizing a shuffled grid lowers its cache miss ratio") {
    MeshShape grid = meshGrid(40, 40);
    uint32_t vertexCount = grid.positions.count();
    float rows = cacheMissRatio(grid.indices, vertexCount);
    meshShuffleTriangles(grid.indices, 3);
    float shuffled = cacheMissRatio(grid.indices, vertexCount);
    Array<uint32_t> ordered = optimized(grid.indices, vertexCount);
    float best = cacheMissRatio(ordered, vertexCount);
    CHECK(sameTriangles(ordered, grid.indices));
    CHECK(shuffled > 2.5f);
    CHECK(best < shuffled * 0.35f);
    // A grid drawn row by row misses about one vertex for each triangle, and a good order well under that.
    CHECK(best < rows * 0.8f);
    CHECK(best < 0.8f);
}

TEST_CASE("optimizing a sphere lowers its cache miss ratio at several cache sizes") {
    MeshShape sphere = meshSphere(64, 32);
    uint32_t vertexCount = sphere.positions.count();
    meshShuffleTriangles(sphere.indices, 11);
    Array<uint32_t> ordered = optimized(sphere.indices, vertexCount);
    CHECK(sameTriangles(ordered, sphere.indices));
    uint32_t sizes[3] = {8, 16, 32};
    for (uint32_t size : sizes) {
        float before = 0;
        float after = 0;
        CHECK(prMeshCacheMissRatio(meshView(sphere.indices), vertexCount, size, &before) == PrStatus_Ok);
        CHECK(prMeshCacheMissRatio(meshView(ordered), vertexCount, size, &after) == PrStatus_Ok);
        CHECK(after < before * 0.4f);
        CHECK(after < 0.9f);
    }
}

TEST_CASE("the optimized order is the same every time and keeps each triangle's vertex order") {
    MeshShape grid = meshGrid(12, 9);
    meshShuffleTriangles(grid.indices, 5);
    Array<uint32_t> first = optimized(grid.indices, grid.positions.count());
    Array<uint32_t> second = optimized(grid.indices, grid.positions.count());
    CHECK(first == second);

    // In place, through 16-bit indices.
    Array<uint16_t> narrow(grid.indices.count());
    for (uint32_t i = 0; i < narrow.count(); ++i)
        narrow[i] = uint16_t(grid.indices[i]);
    PrMeshIndexView view{narrow.data(), narrow.count(), PrMeshIndexType_U16};
    PrMeshIndices out{narrow.data(), narrow.count(), PrMeshIndexType_U16};
    CHECK(prMeshIndicesOptimizeCache(view, grid.positions.count(), out) == PrStatus_Ok);
    for (uint32_t i = 0; i < narrow.count(); ++i)
        CHECK(narrow[i] == first[i]);
}

TEST_CASE("a fetch remap lowers the fetch miss ratio of scattered vertices") {
    constexpr uint32_t kVertexSize = 32;
    MeshShape grid = meshGrid(60, 60);
    uint32_t vertexCount = grid.positions.count();

    // The vertices start in an order unrelated to the triangles.
    PrRandom random{.state = 9};
    Array<uint32_t> scatter(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i)
        scatter[i] = i;
    for (uint32_t i = vertexCount; i > 1; --i) {
        uint32_t other = prRandomU32Below(&random, i);
        uint32_t moving = scatter[i - 1];
        scatter[i - 1] = scatter[other];
        scatter[other] = moving;
    }
    for (uint32_t& index : grid.indices)
        index = scatter[index];

    // Triangles in rows use each vertex again within two rows, so in fetch order every line is loaded once.
    Array<uint32_t> remap(vertexCount);
    Array<uint32_t> rows(grid.indices.count());
    uint32_t count = 0;
    CHECK(fetchMissRatio(grid.indices, vertexCount, kVertexSize) > 1.5f);
    CHECK(prMeshFetchRemap(meshView(grid.indices), vertexCount, remap.data(), &count) == PrStatus_Ok);
    CHECK(count == vertexCount);
    CHECK(prMeshIndicesRemap(meshView(grid.indices), remap.data(), vertexCount, meshIndices(rows)) == PrStatus_Ok);
    CHECK(fetchMissRatio(rows, vertexCount, kVertexSize) == 1.0f);

    // The optimized order returns to some vertices later, and the remap still removes most of the misses.
    Array<uint32_t> ordered = optimized(grid.indices, vertexCount);
    float before = fetchMissRatio(ordered, vertexCount, kVertexSize);
    CHECK(prMeshFetchRemap(meshView(ordered), vertexCount, remap.data(), &count) == PrStatus_Ok);
    CHECK(prMeshIndicesRemap(meshView(ordered), remap.data(), vertexCount, meshIndices(ordered)) == PrStatus_Ok);
    float after = fetchMissRatio(ordered, vertexCount, kVertexSize);
    CHECK(before > 2.0f);
    CHECK(after < before * 0.7f);
}

TEST_CASE("the fetch miss ratio counts the lines that a vertex spans") {
    // Each vertex of 96 bytes spans two lines of 64, and three vertices span five lines between them.
    uint32_t list[3] = {0, 1, 2};
    Array<uint32_t> indices;
    indices.append(list, 3);
    CHECK(fetchMissRatio(indices, 3, 96) == doctest::Approx(1.0f));
    Array<uint32_t> none;
    CHECK(fetchMissRatio(none, 3, 96) == 0);
    float ratio;
    CHECK(prMeshFetchMissRatio(meshView(indices), 3, 0, &ratio) == PrStatus_InvalidData);
    CHECK(prMeshFetchMissRatio(meshView(indices), 2, 16, &ratio) == PrStatus_OutOfRange);
}

TEST_CASE("optimizing keeps triangles that repeat a vertex and vertices that nothing uses") {
    uint32_t list[15] = {0, 0, 1, 5, 5, 5, 1, 2, 9, 9, 2, 1, 0, 1, 2};
    Array<uint32_t> indices;
    indices.append(list, 15);
    Array<uint32_t> ordered = optimized(indices, 12);
    CHECK(sameTriangles(ordered, indices));
}

TEST_CASE("optimizing a fan around one vertex takes linear time") {
    // Every triangle uses vertex 0. A search that read all of a vertex's triangles at each step would not finish within
    // the tests' time limit.
    constexpr uint32_t kTriangles = 300000;
    Array<uint32_t> indices;
    indices.reserve(kTriangles * 3);
    for (uint32_t i = 0; i < kTriangles; ++i) {
        uint32_t triangle[3] = {0, i + 1, i + 2};
        indices.append(triangle, 3);
    }
    meshShuffleTriangles(indices, 2);
    Array<uint32_t> ordered = optimized(indices, kTriangles + 2);
    CHECK(sameTriangles(ordered, indices));
    CHECK(cacheMissRatio(ordered, kTriangles + 2) < cacheMissRatio(indices, kTriangles + 2));
}

TEST_CASE("the cache functions reject indices that name no vertex and counts that are not triangles") {
    uint32_t list[6] = {0, 1, 2, 2, 1, 7};
    uint32_t out[6];
    uint16_t narrow[6];
    float ratio;
    PrMeshIndexView view{list, 6, PrMeshIndexType_U32};
    CHECK(prMeshIndicesOptimizeCache(view, 7, PrMeshIndices{out, 6, PrMeshIndexType_U32}) == PrStatus_OutOfRange);
    CHECK(prMeshCacheMissRatio(view, 7, 16, &ratio) == PrStatus_OutOfRange);
    view.count = 5;
    CHECK(prMeshIndicesOptimizeCache(view, 8, PrMeshIndices{out, 6, PrMeshIndexType_U32}) == PrStatus_InvalidData);
    CHECK(prMeshCacheMissRatio(view, 8, 16, &ratio) == PrStatus_InvalidData);
    // An index that the output's type cannot hold.
    view.count = 6;
    list[5] = 70000;
    CHECK(prMeshIndicesOptimizeCache(view, 80000, PrMeshIndices{narrow, 6, PrMeshIndexType_U16}) ==
        PrStatus_OutOfRange);
}
