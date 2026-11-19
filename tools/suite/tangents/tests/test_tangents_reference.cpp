// lib/mesh's tangents beside the reference implementation of MikkTSpace, over the shapes that lib/mesh's own tangent
// tests build (lib/mesh/tests/test_mesh_tangents.cpp). Both get the same unindexed corners.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/mesh.h"
#include "pr/mesh_tangents.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "tools/harness/mesh_shapes.h"
#include "tools/suite/tangents/tangent_references.h"

#include "tools/harness/test.h"

using namespace harness;

namespace {

// A shape's corners, unindexed, with the tangents of lib/mesh and of the reference.
struct Compared {
    Array<float> positions;
    Array<float> normals;
    Array<float> texcoords;
    Array<float> prax;
    Array<float> reference;
    uint32_t count;
};

Compared compare(const MeshShape& shape) {
    Compared c;
    c.count = shape.indices.count();
    for (uint32_t index : shape.indices) {
        c.positions.append(&shape.positions[index].x, 3);
        c.normals.append(&shape.normals[index].x, 3);
        c.texcoords.append(&shape.texcoords[index].x, 2);
    }
    c.prax.resize(c.count * 4);
    c.reference.resize(c.count * 4);
    CHECK(prMeshTangents(PrMeshAttributeView{c.positions.data(), c.count, 3 * sizeof(float), prMeshFormatFloat32(3)},
        PrMeshAttributeView{c.normals.data(), c.count, 3 * sizeof(float), prMeshFormatFloat32(3)},
        PrMeshAttributeView{c.texcoords.data(), c.count, 2 * sizeof(float), prMeshFormatFloat32(2)},
        PrMeshIndexView{nullptr, c.count, PrMeshIndexType_U32},
        PrMeshAttribute{c.prax.data(), c.count, 4 * sizeof(float), prMeshFormatFloat32(4)}) == PrStatus_Ok);
    CHECK(tangents::mikktspace(tangents::Corners{c.positions.data(), c.normals.data(), c.texcoords.data(), c.count},
        c.reference.data()));
    return c;
}

// Returns true if corner k has the same four floats from both.
bool equalAt(const Compared& c, uint32_t k) {
    for (uint32_t i = 0; i < 4; ++i) {
        if (c.prax[k * 4 + i] != c.reference[k * 4 + i])
            return false;
    }
    return true;
}

// Checks that every corner's tangent is the reference's, float for float.
void checkEqual(const MeshShape& shape) {
    Compared c = compare(shape);
    uint32_t differ = 0;
    for (uint32_t k = 0; k < c.count; ++k)
        differ += equalAt(c, k) ? 0 : 1;
    CHECK(differ == 0);
}

// Checks that each of the `replaced` corners that the reference leaves without a tangent of its own, with a zero
// tangent or its default of (1, 0, 0) and a handedness of -1, gets lib/mesh's replacement: a unit tangent perpendicular
// to the normal with a handedness of 1. Every other corner must be the reference's.
void checkEqualOrReplaced(const MeshShape& shape, uint32_t replaced) {
    Compared c = compare(shape);
    uint32_t differ = 0;
    uint32_t replacements = 0;
    for (uint32_t k = 0; k < c.count; ++k) {
        if (equalAt(c, k))
            continue;
        const float* r = &c.reference[k * 4];
        const float* e = &c.prax[k * 4];
        PrVec3 tangent{e[0], e[1], e[2]};
        PrVec3 normal{c.normals[k * 3], c.normals[k * 3 + 1], c.normals[k * 3 + 2]};
        bool isZero = r[0] == 0 && r[1] == 0 && r[2] == 0;
        bool isDefault = isZero || (r[0] == 1 && r[1] == 0 && r[2] == 0 && r[3] == -1);
        bool isReplacement = e[3] == 1 && prF32Abs(prVec3Length(tangent) - 1) < 1e-6f &&
            prF32Abs(prVec3Dot(tangent, normal)) < 1e-6f;
        if (isDefault && isReplacement)
            ++replacements;
        else
            ++differ;
    }
    CHECK(differ == 0);
    CHECK(replacements == replaced);
}

} // namespace

TEST_CASE("one triangle's tangents are the reference's") {
    checkEqual(meshTriangle({0, 0}, {1, 0}, {0, 1}));
    checkEqual(meshTriangle({0, 0}, {0, 1}, {-1, 0}));
    checkEqual(meshTriangle({0, 0}, {-1, 0}, {0, 1}));
}

TEST_CASE("a grid's and a sphere's tangents are the reference's") {
    checkEqual(meshGrid(5, 4));
    checkEqual(meshSphere(24, 12));
    MeshShape shuffled = meshSphere(16, 8);
    meshShuffleTriangles(shuffled.indices, 7);
    checkEqual(shuffled);
}

TEST_CASE("tangents on both sides of a mirror and across a continuing edge are the reference's") {
    checkEqual(meshTrianglePair({{0, 0}, {0, 1}, {1, 0}, {1, 0}}));
    checkEqual(meshTrianglePair({{0.5f, 0}, {0.5f, 1}, {0, 0}, {1, 0}}));
}

TEST_CASE("triangles without texture area take the tangents that the reference gives them") {
    // A grid whose texture is turned a quarter, so that its tangents point along +y, unlike lib/mesh's replacement.
    auto turned = [](MeshShape shape) {
        for (PrVec2& texcoord : shape.texcoords)
            texcoord = PrVec2{texcoord.y, -texcoord.x};
        return shape;
    };
    // A square whose texture collapses after the square that gives it a tangent: its corners at the shared edge take
    // that tangent, and the three corners away from it have none in the reference.
    MeshShape last = turned(meshGrid(2, 1));
    last.texcoords[2] = last.texcoords[1];
    last.texcoords[5] = last.texcoords[4];
    checkEqualOrReplaced(last, 3);
    // The collapsed square first in the list, before the square that gives it a tangent.
    MeshShape first = turned(meshGrid(2, 1));
    first.texcoords[1] = first.texcoords[0];
    first.texcoords[4] = first.texcoords[3];
    checkEqualOrReplaced(first, 3);

    // Where no triangle has texture area, the reference leaves every corner with its default.
    MeshShape sphere = meshSphere(12, 6);
    for (PrVec2& texcoord : sphere.texcoords)
        texcoord = PrVec2{0.25f, 0.75f};
    checkEqualOrReplaced(sphere, sphere.indices.count());
}

TEST_CASE("triangles with equal positions copy the tangents that the reference copies") {
    MeshShape shape = meshTriangle({0, 0}, {0, 1}, {-1, 0});
    uint32_t degenerate[3] = {0, 1, 1};
    shape.indices.append(degenerate, 3);
    for (uint32_t i = 0; i < 3; ++i) {
        shape.positions.push(PrVec3{5, 5, float(i / 2)});
        shape.normals.push(PrVec3{0, 1, 0});
        shape.texcoords.push(PrVec2{float(i), 0});
        shape.indices.push(3 + i);
    }
    // The triangle that shares nothing has no tangent in the reference.
    checkEqualOrReplaced(shape, 3);
}

TEST_CASE("a texture wound around a vertex and a fan of slivers have the reference's tangents") {
    checkEqual(meshDisc(16));
    // The reference compares every pair of triangles around a vertex, so the fan is smaller than lib/mesh's own test's.
    checkEqual(meshFan(2000));
}

TEST_CASE("triangles that share an edge with more than one other pair as the reference pairs them") {
    // Fins around the edge from (0, 0, 0) to (0, 1, 0), wound alternately one way and the other, so that each fin can
    // pair with several across the edge. The texture of each fin runs a different way.
    for (uint32_t fins = 3; fins <= 6; ++fins) {
        MeshShape shape;
        shape.positions.push(PrVec3{0, 0, 0});
        shape.positions.push(PrVec3{0, 1, 0});
        shape.texcoords.push(PrVec2{0, 0});
        shape.texcoords.push(PrVec2{0, 1});
        for (uint32_t i = 0; i < fins; ++i) {
            float angle = 2 * kPrF32Pi * float(i) / float(fins);
            shape.positions.push(PrVec3{prF32Cos(angle), 0.5f, prF32Sin(angle)});
            shape.texcoords.push(PrVec2{1 + 0.25f * float(i), 0.2f * float(i)});
            uint32_t triangle[3] = {i % 2 == 0 ? 0u : 1u, i % 2 == 0 ? 1u : 0u, 2 + i};
            shape.indices.append(triangle, 3);
        }
        // The normal runs along the edge, so that every fin's tangent has a direction in the plane of the normal. At a
        // fin's tip, both edges point the same way in that plane, so the tip has no angle and no tangent.
        for (uint32_t i = 0; i < shape.positions.count(); ++i)
            shape.normals.push(PrVec3{0, 1, 0});
        checkEqualOrReplaced(shape, fins);
        meshShuffleTriangles(shape.indices, fins);
        checkEqualOrReplaced(shape, fins);
    }
}
