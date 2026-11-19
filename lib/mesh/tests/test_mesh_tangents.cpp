#include "pr/array.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/mesh_tangents.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// A tangent as prMeshTangents writes it.
struct Tangent {
    PrVec3 direction;
    float handedness;
};

bool sameBits(Tangent a, Tangent b) {
    return prMemEquals(&a, &b, sizeof(Tangent));
}

// Returns the tangents of `shape`, one for each index.
Array<Tangent> tangentsOf(const MeshShape& shape) {
    Array<Tangent> tangents(shape.indices.count());
    PrMeshAttribute out{tangents.data(), tangents.count(), sizeof(Tangent), prMeshFormatFloat32(4)};
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), meshView(shape.texcoords),
        meshView(shape.indices), out) == PrStatus_Ok);
    return tangents;
}

// Returns true if `tangent` is a unit vector perpendicular to `normal` with a handedness of 1 or -1.
bool isFrame(Tangent tangent, PrVec3 normal) {
    float length = prVec3Length(tangent.direction);
    return length > 0.9999f && length < 1.0001f && prF32Abs(prVec3Dot(tangent.direction, normal)) < 1e-4f &&
        (tangent.handedness == 1 || tangent.handedness == -1);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a tangent points where the first texture coordinate increases") {
    Array<Tangent> tangents = tangentsOf(meshTriangle({0, 0}, {1, 0}, {0, 1}));
    for (Tangent tangent : tangents) {
        CHECK(meshTestNear(tangent.direction, PrVec3{1, 0, 0}, 1e-6f));
        CHECK(tangent.handedness == 1);
    }

    // The texture turned a quarter: the first coordinate increases toward -y, and the second toward +x.
    tangents = tangentsOf(meshTriangle({0, 0}, {0, 1}, {-1, 0}));
    for (Tangent tangent : tangents) {
        CHECK(meshTestNear(tangent.direction, PrVec3{0, -1, 0}, 1e-6f));
        CHECK(tangent.handedness == 1);
    }
}

TEST_CASE("handedness is negative where the texture is mirrored") {
    Array<Tangent> tangents = tangentsOf(meshTriangle({0, 0}, {-1, 0}, {0, 1}));
    for (Tangent tangent : tangents) {
        CHECK(meshTestNear(tangent.direction, PrVec3{-1, 0, 0}, 1e-6f));
        CHECK(tangent.handedness == -1);
        // The bitangent still points where the second texture coordinate increases.
        PrVec3 bitangent = prVec3Cross(PrVec3{0, 0, 1}, tangent.direction) * tangent.handedness;
        CHECK(meshTestNear(bitangent, PrVec3{0, 1, 0}, 1e-6f));
    }
}

TEST_CASE("a grid's tangents follow its texture") {
    MeshShape grid = meshGrid(5, 4);
    Array<Tangent> tangents = tangentsOf(grid);
    for (Tangent tangent : tangents) {
        CHECK(meshTestNear(tangent.direction, PrVec3{1, 0, 0}, 1e-6f));
        CHECK(tangent.handedness == 1);
    }
}

TEST_CASE("a sphere's tangents are unit vectors perpendicular to its normals") {
    MeshShape sphere = meshSphere(24, 12);
    Array<Tangent> tangents = tangentsOf(sphere);
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        CHECK(isFrame(tangents[i], sphere.normals[sphere.indices[i]]));
        CHECK(tangents[i].handedness == 1);
    }
    // Away from the poles the tangent runs along the circle of latitude, toward greater longitude.
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        PrVec3 position = sphere.positions[sphere.indices[i]];
        if (prF32Abs(position.y) > 0.9f)
            continue;
        PrVec3 east = PrVec3{-position.z, 0, position.x} / prVec3Length(PrVec3{-position.z, 0, position.x});
        CHECK(prVec3Dot(tangents[i].direction, east) > 0.98f);
    }
}

TEST_CASE("corners that share a vertex of a smooth surface share a tangent") {
    MeshShape sphere = meshSphere(24, 12);
    Array<Tangent> tangents = tangentsOf(sphere);
    Array<uint32_t> firstCorner(sphere.positions.count(), kPrMeshIndexNone);
    uint32_t shared = 0;
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        uint32_t vertex = sphere.indices[i];
        // A pole's vertices are each used by one triangle of a fan whose texture is pinched, so they are left out.
        if (prF32Abs(sphere.positions[vertex].y) > 0.999f)
            continue;
        if (firstCorner[vertex] == kPrMeshIndexNone) {
            firstCorner[vertex] = i;
            continue;
        }
        CHECK(sameBits(tangents[i], tangents[firstCorner[vertex]]));
        ++shared;
    }
    CHECK(shared > 1000);
}

TEST_CASE("tangents do not depend on which vertices the indices share") {
    MeshShape sphere = meshSphere(16, 8);
    Array<Tangent> indexed = tangentsOf(sphere);

    MeshShape unshared;
    for (uint32_t index : sphere.indices) {
        unshared.positions.push(sphere.positions[index]);
        unshared.normals.push(sphere.normals[index]);
        unshared.texcoords.push(sphere.texcoords[index]);
    }
    Array<Tangent> flat(indexed.count());
    PrMeshAttribute out{flat.data(), flat.count(), sizeof(Tangent), prMeshFormatFloat32(4)};
    CHECK(prMeshTangents(meshView(unshared.positions), meshView(unshared.normals), meshView(unshared.texcoords),
        meshSequence(flat.count()), out) == PrStatus_Ok);
    for (uint32_t i = 0; i < flat.count(); ++i)
        CHECK(sameBits(flat[i], indexed[i]));
}

TEST_CASE("a mirrored texture splits the vertices along the mirror") {
    // Two triangles share the edge from (0, 0) to (0, 1). The texture of the second is the mirror of the first's, so
    // the shared vertices have equal positions, normals and texture coordinates.
    MeshShape shape = meshTrianglePair({{0, 0}, {0, 1}, {1, 0}, {1, 0}});
    Array<Tangent> tangents = tangentsOf(shape);
    for (uint32_t i = 0; i < 3; ++i) {
        CHECK(meshTestNear(tangents[i].direction, PrVec3{-1, 0, 0}, 1e-6f));
        CHECK(tangents[i].handedness == -1);
        CHECK(meshTestNear(tangents[i + 3].direction, PrVec3{1, 0, 0}, 1e-6f));
        CHECK(tangents[i + 3].handedness == 1);
    }

    // Welding the unshared vertices with their tangents keeps the two sides apart: three vertices for each triangle.
    struct Vertex {
        PrVec3 position;
        PrVec2 texcoord;
        Tangent tangent;
    };
    Array<Vertex> vertices(6u);
    for (uint32_t i = 0; i < 6; ++i)
        vertices[i] = Vertex{shape.positions[shape.indices[i]], shape.texcoords[shape.indices[i]], tangents[i]};
    uint32_t remap[6];
    uint32_t count = 0;
    CHECK(prMeshWeldRemap(vertices.data(), 6, sizeof(Vertex), remap, &count) == PrStatus_Ok);
    CHECK(count == 6);
}

TEST_CASE("triangles with one texture orientation share tangents across an edge") {
    // The same two triangles with a texture that continues across the edge.
    MeshShape shape = meshTrianglePair({{0.5f, 0}, {0.5f, 1}, {0, 0}, {1, 0}});
    Array<Tangent> tangents = tangentsOf(shape);
    CHECK(sameBits(tangents[0], tangents[3]));
    CHECK(sameBits(tangents[1], tangents[5]));
    CHECK(meshTestNear(tangents[0].direction, PrVec3{1, 0, 0}, 1e-6f));
    CHECK(tangents[0].handedness == 1);
}

TEST_CASE("texture coordinates without area leave unit tangents") {
    // Every vertex has the same texture coordinate, so no triangle says where the texture increases.
    MeshShape sphere = meshSphere(12, 6);
    for (PrVec2& texcoord : sphere.texcoords)
        texcoord = PrVec2{0.25f, 0.75f};
    Array<Tangent> tangents = tangentsOf(sphere);
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        CHECK(isFrame(tangents[i], sphere.normals[sphere.indices[i]]));
        CHECK(tangents[i].handedness == 1);
    }
}

TEST_CASE("a triangle without texture area takes its neighbor's tangent") {
    MeshShape grid = meshGrid(2, 1);
    // The second square's vertices on the right get the texture coordinates of those on its left.
    grid.texcoords[2] = grid.texcoords[1];
    grid.texcoords[5] = grid.texcoords[4];
    Array<Tangent> tangents = tangentsOf(grid);
    for (Tangent tangent : tangents) {
        CHECK(meshTestNear(tangent.direction, PrVec3{1, 0, 0}, 1e-6f));
        CHECK(tangent.handedness == 1);
    }
}

TEST_CASE("a triangle without texture area before its neighbor still takes the neighbor's tangent") {
    // The texture is turned a quarter, so that the tangent runs along +y, and the first square's vertices on the right
    // get the texture coordinates of those on its left. A group starts only at a triangle with texture area, so the
    // collapsed square, first in the list, joins the second square's groups along the edge that they share.
    MeshShape grid = meshGrid(2, 1);
    for (PrVec2& texcoord : grid.texcoords)
        texcoord = PrVec2{texcoord.y, -texcoord.x};
    grid.texcoords[1] = grid.texcoords[0];
    grid.texcoords[4] = grid.texcoords[3];
    Array<Tangent> tangents = tangentsOf(grid);
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        // The corners on the left edge, which no group reaches, get the replacement tangent.
        if (grid.positions[grid.indices[i]].x == 0) {
            CHECK(isFrame(tangents[i], PrVec3{0, 0, 1}));
            continue;
        }
        CHECK(meshTestNear(tangents[i].direction, PrVec3{0, 1, 0}, 1e-6f));
        CHECK(tangents[i].handedness == 1);
    }
}

TEST_CASE("a triangle with equal positions copies a neighbor's tangents") {
    MeshShape shape = meshTriangle({0, 0}, {0, 1}, {-1, 0});
    // A second triangle that uses the first's vertices 0 and 1, and repeats vertex 1.
    uint32_t degenerate[3] = {0, 1, 1};
    shape.indices.append(degenerate, 3);
    // A third triangle of two equal positions that shares nothing.
    for (uint32_t i = 0; i < 3; ++i) {
        shape.positions.push(PrVec3{5, 5, float(i / 2)});
        shape.normals.push(PrVec3{0, 1, 0});
        shape.texcoords.push(PrVec2{float(i), 0});
        shape.indices.push(3 + i);
    }
    Array<Tangent> tangents = tangentsOf(shape);
    CHECK(sameBits(tangents[3], tangents[0]));
    CHECK(sameBits(tangents[4], tangents[1]));
    CHECK(sameBits(tangents[5], tangents[1]));
    for (uint32_t i = 6; i < 9; ++i) {
        CHECK(isFrame(tangents[i], PrVec3{0, 1, 0}));
        CHECK(tangents[i].handedness == 1);
    }
}

TEST_CASE("a texture wound around a vertex gives each corner there a tangent of its own direction") {
    // A flat disc whose texture wraps around its center: the first coordinate follows the angle. The triangles at the
    // center are one group whose tangent directions turn all the way around.
    constexpr uint32_t kSegments = 16;
    MeshShape disc = meshDisc(kSegments);
    Array<Tangent> tangents = tangentsOf(disc);
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        CHECK(isFrame(tangents[i], PrVec3{0, 0, 1}));
        // The second coordinate increases outward, to the right of the direction around the disc.
        CHECK(tangents[i].handedness == -1);
    }
    // At the rim, the tangent runs around the disc, counterclockwise.
    for (uint32_t i = 0; i < kSegments; ++i) {
        PrVec3 position = disc.positions[i + 1];
        PrVec3 around{-position.y, position.x, 0};
        CHECK(prVec3Dot(tangents[i * 3 + 1].direction, around) > 0.9f);
    }
}

TEST_CASE("many triangles around one vertex with one texture direction take linear time") {
    // A fan in a plane with a texture laid flat over it. Comparing every pair of the triangles at the center would not
    // finish within the tests' time limit.
    MeshShape fan = meshFan(200000);
    Array<Tangent> tangents = tangentsOf(fan);
    uint32_t wrong = 0;
    for (Tangent tangent : tangents)
        wrong += meshTestNear(tangent.direction, PrVec3{1, 0, 0}, 1e-4f) && tangent.handedness == 1 ? 0 : 1;
    CHECK(wrong == 0);
}

TEST_CASE("coordinates that are not finite leave unit tangents and spare the other triangles") {
    MeshShape grid = meshGrid(6, 6);
    Array<Tangent> clean = tangentsOf(grid);
    grid.positions[0].x = kPrF32Nan;
    grid.texcoords[48].y = kPrF32Infinity;
    grid.normals[6].z = kPrF32Nan;
    Array<Tangent> tangents = tangentsOf(grid);
    for (uint32_t i = 0; i < tangents.count(); ++i) {
        float length = prVec3Length(tangents[i].direction);
        CHECK(length > 0.9999f);
        CHECK(length < 1.0001f);
        CHECK(prF32Abs(tangents[i].handedness) == 1);
    }
    // The triangles of the middle squares touch none of the spoiled vertices.
    uint32_t middle = (3 * 6 + 3) * 6;
    for (uint32_t i = middle; i < middle + 6; ++i)
        CHECK(sameBits(tangents[i], clean[i]));
}

TEST_CASE("tangents are written through a stride and leave the other bytes alone") {
    struct Vertex {
        uint32_t before;
        Tangent tangent;
        uint32_t after;
    };
    MeshShape shape = meshTriangle({0, 0}, {1, 0}, {0, 1});
    Vertex vertices[3] = {{7, {}, 9}, {7, {}, 9}, {7, {}, 9}};
    PrMeshAttribute out{&vertices[0].tangent, 3, sizeof(Vertex), prMeshFormatFloat32(4)};
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), meshView(shape.texcoords),
        meshView(shape.indices), out) == PrStatus_Ok);
    for (const Vertex& vertex : vertices) {
        CHECK(vertex.before == 7);
        CHECK(vertex.after == 9);
        CHECK(meshTestNear(vertex.tangent.direction, PrVec3{1, 0, 0}, 1e-6f));
    }
}

TEST_CASE("tangents reject attributes of different counts and indices that name no vertex") {
    MeshShape shape = meshTriangle({0, 0}, {1, 0}, {0, 1});
    Tangent tangents[3];
    PrMeshAttribute out{tangents, 3, sizeof(Tangent), prMeshFormatFloat32(4)};
    shape.indices[2] = 3;
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), meshView(shape.texcoords),
        meshView(shape.indices), out) == PrStatus_OutOfRange);
    shape.indices[2] = 2;
    shape.normals.pop();
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), meshView(shape.texcoords),
        meshView(shape.indices), out) == PrStatus_InvalidData);
    shape.normals.push(PrVec3{0, 0, 1});
    PrMeshAttributeView texcoords = meshView(shape.texcoords);
    texcoords.stride = 4;
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), texcoords, meshView(shape.indices), out) ==
        PrStatus_InvalidData);
    out.stride = 12;
    CHECK(prMeshTangents(meshView(shape.positions), meshView(shape.normals), meshView(shape.texcoords),
        meshView(shape.indices), out) == PrStatus_InvalidData);
}
