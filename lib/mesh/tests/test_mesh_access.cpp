#include "pr/array.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mesh.h"
#include "pr/mesh_bvh.h"
#include "pr/mesh_cache.h"
#include "pr/mesh_normals.h"
#include "pr/mesh_tangents.h"
#include "pr/random.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/src/mesh_access.h"
#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// Returns a grid whose heights and texture coordinates are scattered by a seeded generator. Its coordinates come from
// exact arithmetic, so the shape has the same bytes on every platform.
MeshShape bumpyGrid(uint32_t columns, uint32_t rows) {
    MeshShape shape = meshGrid(columns, rows);
    PrRandom random{.state = 1234};
    for (uint32_t i = 0; i < shape.positions.count(); ++i) {
        shape.positions[i].z = float(prRandomU32Below(&random, 1024)) / 1024.0f;
        shape.texcoords[i].x += float(prRandomU32Below(&random, 64)) / 1024.0f;
        shape.texcoords[i].y += shape.texcoords[i].x * 0.25f;
    }
    return shape;
}

template <typename T>
uint64_t hashOf(const Array<T>& values) {
    return prHashBytes(values.data(), size_t(values.count()) * sizeof(T), 0);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the arccosine agrees with the C library's to a double's precision") {
    for (int32_t i = -1000; i <= 1000; ++i) {
        double x = double(i) / 1000.0;
        CHECK(meshAcos(x) == doctest::Approx(prF64Acos(x)).epsilon(1e-14));
    }
    // Close to the ends, where the angle changes fastest.
    for (uint32_t i = 1; i < 60; ++i) {
        double x = 1.0 - 1.0 / double(1ull << i);
        CHECK(meshAcos(x) == doctest::Approx(prF64Acos(x)).epsilon(1e-13));
        CHECK(meshAcos(-x) == doctest::Approx(prF64Acos(-x)).epsilon(1e-13));
    }
    CHECK(meshAcos(1.0) == 0);
    CHECK(meshAcos(-1.0) == kPrF64Pi);
    CHECK(meshAcos(0.0) == doctest::Approx(kPrF64Pi / 2).epsilon(1e-15));
}

TEST_CASE("a perpendicular is a unit vector at a right angle, and (1, 0, 0) for no direction") {
    PrVec3 normals[5] = {{0, 0, 1}, {1, 0, 0}, {0.6f, 0.8f, 0}, {-0.5f, 0.5f, 0.70710678f}, {0, -3, 0}};
    for (PrVec3 normal : normals) {
        PrVec3 perpendicular = meshPerpendicular(normal);
        CHECK(prVec3Length(perpendicular) == doctest::Approx(1.0f).epsilon(1e-6));
        CHECK(prF32Abs(prVec3Dot(perpendicular, normal)) < 1e-6f);
    }
    CHECK(meshPerpendicular(PrVec3{0, 0, 0}) == PrVec3{1, 0, 0});
    CHECK(meshPerpendicular(PrVec3{kPrF32Nan, 0, 1}) == PrVec3{1, 0, 0});
    CHECK(meshPerpendicular(PrVec3{kPrF32Infinity, 0, 1}) == PrVec3{1, 0, 0});
}

TEST_CASE("results have the same bytes on every platform") {
    // The hashes were recorded on one platform, and every platform's tests compare with them.
    MeshShape shape = bumpyGrid(24, 20);
    uint32_t vertexCount = shape.positions.count();
    uint32_t indexCount = shape.indices.count();

    Array<PrVec3> normals(vertexCount);
    CHECK(prMeshNormals(meshView(shape.positions), meshView(shape.indices), meshAttribute(normals)) == PrStatus_Ok);
    CHECK(hashOf(normals) == 3298089428600459862ull);

    Array<float> tangents(indexCount * 4);
    PrMeshAttribute tangentsOut{tangents.data(), indexCount, 4 * sizeof(float), prMeshFormatFloat32(4)};
    CHECK(prMeshTangents(meshView(shape.positions), meshView(normals), meshView(shape.texcoords),
        meshView(shape.indices), tangentsOut) == PrStatus_Ok);
    CHECK(hashOf(tangents) == 14074954891159934270ull);

    PrSphere sphere;
    CHECK(prMeshSphere(meshView(shape.positions), &sphere) == PrStatus_Ok);
    CHECK(prHashBytes(&sphere, sizeof(sphere), 0) == 13422032865040722830ull);

    Array<uint32_t> ordered(indexCount);
    CHECK(prMeshIndicesOptimizeCache(meshView(shape.indices), vertexCount, meshIndices(ordered)) == PrStatus_Ok);
    CHECK(hashOf(ordered) == 12073800421966482146ull);

    Array<PrMeshBvhNode> nodes(prMeshBvhMaxNodes(indexCount / 3));
    Array<uint32_t> order(indexCount / 3);
    PrMeshBvhView bvh;
    CHECK(prMeshBvhBuild(meshView(shape.positions), meshView(shape.indices), nodes.data(), order.data(), &bvh) ==
        PrStatus_Ok);
    CHECK(prHashBytes(nodes.data(), size_t(bvh.nodeCount) * sizeof(PrMeshBvhNode), 0) == 10582345418666749652ull);
    CHECK(hashOf(order) == 4681578827284219805ull);
}
