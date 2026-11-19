#include "pr/array.h"
#include "pr/float.h"
#include "pr/mesh.h"
#include "pr/mesh_normals.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// Returns a cube from -1 to 1 with eight shared vertices. Each face is two triangles, wound counterclockwise seen from
// outside, and `diagonals` chooses for each face which of its two diagonals divides it.
MeshShape cube(uint32_t diagonals) {
    MeshShape shape;
    for (uint32_t i = 0; i < 8; ++i)
        shape.positions.push(PrVec3{float(i & 1) * 2 - 1, float((i >> 1) & 1) * 2 - 1, float(i >> 2) * 2 - 1});
    uint32_t faces[6][4] = {{0, 4, 6, 2}, {1, 3, 7, 5}, {0, 1, 5, 4}, {2, 6, 7, 3}, {0, 2, 3, 1}, {4, 5, 7, 6}};
    for (uint32_t face = 0; face < 6; ++face) {
        const uint32_t* quad = faces[face];
        if ((diagonals >> face) & 1) {
            uint32_t triangles[6] = {quad[0], quad[1], quad[3], quad[1], quad[2], quad[3]};
            shape.indices.append(triangles, 6);
        } else {
            uint32_t triangles[6] = {quad[0], quad[1], quad[2], quad[0], quad[2], quad[3]};
            shape.indices.append(triangles, 6);
        }
    }
    return shape;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a flat grid's normals all face the side its triangles wind counterclockwise from") {
    MeshShape grid = meshGrid(4, 3);
    Array<PrVec3> normals(grid.positions.count());
    CHECK(prMeshNormals(meshView(grid.positions), meshView(grid.indices), meshAttribute(normals)) == PrStatus_Ok);
    for (PrVec3 normal : normals)
        CHECK(normal == PrVec3{0, 0, 1});
}

TEST_CASE("angle weights make a cube's normals independent of how its faces are cut") {
    uint32_t cuts[4] = {0, 0x3f, 0x15, 0x26};
    for (uint32_t diagonals : cuts) {
        MeshShape shape = cube(diagonals);
        Array<PrVec3> normals(8u);
        CHECK(prMeshNormals(meshView(shape.positions), meshView(shape.indices), meshAttribute(normals)) == PrStatus_Ok);
        for (uint32_t i = 0; i < 8; ++i) {
            PrVec3 expected = shape.positions[i] / prF32Sqrt(3.0f);
            CHECK(meshTestNear(normals[i], expected, 1e-6f));
        }
    }
}

TEST_CASE("a sphere's normals point along its positions") {
    MeshShape sphere = meshSphere(32, 16);
    // The seam and the poles repeat positions, so the triangles are joined there by welding.
    Array<uint32_t> remap(sphere.positions.count());
    uint32_t count = 0;
    CHECK(prMeshWeldRemap(sphere.positions.data(), sphere.positions.count(), sizeof(PrVec3), remap.data(), &count) ==
        PrStatus_Ok);
    Array<PrVec3> welded(count);
    Array<uint32_t> indices(sphere.indices.count());
    CHECK(prMeshVerticesRemap(sphere.positions.data(), sphere.positions.count(), sizeof(PrVec3), remap.data(),
        welded.data(), count) == PrStatus_Ok);
    CHECK(prMeshIndicesRemap(meshView(sphere.indices), remap.data(), remap.count(), meshIndices(indices)) ==
        PrStatus_Ok);

    Array<PrVec3> normals(count);
    CHECK(prMeshNormals(meshView(welded), meshView(indices), meshAttribute(normals)) == PrStatus_Ok);
    for (uint32_t i = 0; i < count; ++i) {
        CHECK(prVec3Length(normals[i]) == doctest::Approx(1.0f).epsilon(1e-5));
        CHECK(prVec3Dot(normals[i], welded[i]) > 0.995f);
    }
}

TEST_CASE("degenerate and non-finite triangles contribute nothing to a normal") {
    Array<PrVec3> positions;
    positions.push(PrVec3{0, 0, 0});
    positions.push(PrVec3{1, 0, 0});
    positions.push(PrVec3{0, 0, -1});
    positions.push(PrVec3{2, 0, 0});            // collinear with the first two
    positions.push(PrVec3{kPrF32Nan, 1, 0});
    positions.push(PrVec3{5, 5, 5});            // used by no triangle
    positions.push(PrVec3{kPrF32Infinity, 0, 0});
    // A triangle facing +y, a collinear one, a repeated vertex, and two with coordinates that are not finite.
    Array<uint32_t> indices;
    uint32_t list[15] = {0, 1, 2, 0, 1, 3, 0, 0, 1, 0, 1, 4, 0, 6, 2};
    indices.append(list, 15);
    Array<PrVec3> normals(positions.count());
    CHECK(prMeshNormals(meshView(positions), meshView(indices), meshAttribute(normals)) == PrStatus_Ok);
    CHECK(normals[0] == PrVec3{0, 1, 0});
    CHECK(normals[1] == PrVec3{0, 1, 0});
    CHECK(normals[2] == PrVec3{0, 1, 0});
    CHECK(normals[3] == PrVec3{0, 0, 1});
    CHECK(normals[4] == PrVec3{0, 0, 1});
    CHECK(normals[5] == PrVec3{0, 0, 1});
    CHECK(normals[6] == PrVec3{0, 0, 1});
}

TEST_CASE("normals that cancel leave the default normal") {
    Array<PrVec3> positions;
    positions.push(PrVec3{0, 0, 0});
    positions.push(PrVec3{1, 0, 0});
    positions.push(PrVec3{0, 1, 0});
    Array<uint32_t> indices;
    uint32_t list[6] = {0, 1, 2, 0, 2, 1};
    indices.append(list, 6);
    Array<PrVec3> normals(3u);
    CHECK(prMeshNormals(meshView(positions), meshView(indices), meshAttribute(normals)) == PrStatus_Ok);
    for (PrVec3 normal : normals)
        CHECK(normal == PrVec3{0, 0, 1});
}

TEST_CASE("flat normals give each index its triangle's normal") {
    MeshShape shape = cube(0);
    Array<PrVec3> normals(shape.indices.count());
    CHECK(prMeshFlatNormals(meshView(shape.positions), meshView(shape.indices), meshAttribute(normals)) == PrStatus_Ok);
    PrVec3 faces[6] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    for (uint32_t i = 0; i < normals.count(); ++i)
        CHECK(normals[i] == faces[i / 6]);

    // Unshared vertices with these normals weld to four vertices for each face.
    struct Vertex {
        PrVec3 position;
        PrVec3 normal;
    };
    Array<Vertex> vertices(shape.indices.count());
    for (uint32_t i = 0; i < vertices.count(); ++i)
        vertices[i] = Vertex{shape.positions[shape.indices[i]], normals[i]};
    Array<uint32_t> remap(vertices.count());
    uint32_t count = 0;
    CHECK(prMeshWeldRemap(vertices.data(), vertices.count(), sizeof(Vertex), remap.data(), &count) == PrStatus_Ok);
    CHECK(count == 24);
}

TEST_CASE("a flat normal of a triangle without area is the default normal") {
    Array<PrVec3> positions;
    positions.push(PrVec3{0, 0, 0});
    positions.push(PrVec3{1, 0, 0});
    positions.push(PrVec3{2, 0, 0});
    Array<PrVec3> normals(3u);
    CHECK(prMeshFlatNormals(meshView(positions), meshSequence(3), meshAttribute(normals)) == PrStatus_Ok);
    for (PrVec3 normal : normals)
        CHECK(normal == PrVec3{0, 0, 1});
}

TEST_CASE("normals reject indices that name no vertex and counts that are not triangles") {
    MeshShape grid = meshGrid(1, 1);
    Array<PrVec3> normals(6u);
    PrMeshAttribute out = meshAttribute(normals);
    grid.indices[4] = 4;
    CHECK(prMeshNormals(meshView(grid.positions), meshView(grid.indices), out) == PrStatus_OutOfRange);
    CHECK(prMeshFlatNormals(meshView(grid.positions), meshView(grid.indices), out) == PrStatus_OutOfRange);
    grid.indices[4] = 3;
    grid.indices.pop();
    CHECK(prMeshNormals(meshView(grid.positions), meshView(grid.indices), out) == PrStatus_InvalidData);
    CHECK(prMeshFlatNormals(meshView(grid.positions), meshView(grid.indices), out) == PrStatus_InvalidData);
    CHECK(prMeshNormals(meshView(grid.positions), meshSequence(6), out) == PrStatus_OutOfRange);
    out.stride = 4;
    CHECK(prMeshNormals(meshView(grid.positions), meshSequence(3), out) == PrStatus_InvalidData);
}
