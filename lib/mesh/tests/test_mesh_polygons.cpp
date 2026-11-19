#include "pr/array.h"
#include "pr/mesh_polygons.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// Polygons in the caller's arrays, and the triangles that cutting them makes.
struct Polygons {
    Array<PrVec3> positions;
    Array<uint32_t> corners;
    Array<uint32_t> counts;
    Array<uint32_t> triangles;
    Array<uint32_t> polygons;

    // Adds a polygon of the points `xy` in the plane z = 0, in their order.
    template <uint32_t N>
    void add(const PrVec2 (&xy)[N]) {
        for (PrVec2 p : xy) {
            corners.push(positions.count());
            positions.push(PrVec3{p.x, p.y, 0});
        }
        counts.push(N);
    }

    PrStatus cut() {
        uint32_t count = prMeshPolygonTriangleCount(counts.data(), counts.count());
        triangles.resize(count * 3);
        polygons.resize(count);
        return prMeshPolygonsTriangulate(meshView(positions), meshView(corners), counts.data(), counts.count(),
            triangles.data(), polygons.data());
    }

    // Returns the signed area of triangle `t` in the plane z = 0, positive if it winds counterclockwise.
    float area(uint32_t t) const {
        PrVec3 a = positions[corners[triangles[t * 3]]];
        PrVec3 b = positions[corners[triangles[t * 3 + 1]]];
        PrVec3 c = positions[corners[triangles[t * 3 + 2]]];
        return ((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x)) / 2;
    }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a convex polygon is a fan from its first corner") {
    Polygons p;
    p.add({{0, 0}, {1, 0}, {1, 1}, {0, 1}});
    p.add({{2, 0}, {3, 0}, {3, 1}});
    REQUIRE(p.cut() == PrStatus_Ok);
    REQUIRE(p.triangles.count() == 9);
    uint32_t expected[] = {0, 1, 2, 0, 2, 3, 4, 5, 6};
    for (uint32_t i = 0; i < 9; ++i)
        CHECK(p.triangles[i] == expected[i]);
    CHECK(p.polygons[0] == 0);
    CHECK(p.polygons[1] == 0);
    CHECK(p.polygons[2] == 1);
}

TEST_CASE("a concave polygon is cut into triangles inside it that wind as it does") {
    // An L, an arrow whose point is reflex, and a comb of three teeth: each cut covers the polygon's area exactly, with
    // every triangle counterclockwise, where a fan would cross outside it.
    Polygons p;
    p.add({{0, 0}, {2, 0}, {2, 1}, {1, 1}, {1, 2}, {0, 2}});
    p.add({{0, 0}, {2, 1}, {4, 0}, {2, 3}});
    p.add({{0, 0}, {5, 0}, {5, 3}, {4, 3}, {4, 1}, {3, 1}, {3, 3}, {2, 3}, {2, 1}, {1, 1}, {1, 3}, {0, 3}});
    REQUIRE(p.cut() == PrStatus_Ok);
    float expectedArea[] = {3, 4, 11};
    float area[3] = {};
    for (uint32_t t = 0; t < p.polygons.count(); ++t) {
        CHECK(p.area(t) > 0);
        area[p.polygons[t]] += p.area(t);
    }
    for (uint32_t i = 0; i < 3; ++i)
        CHECK(area[i] == doctest::Approx(expectedArea[i]));
    CHECK(p.polygons.count() == 4 + 2 + 10);
}

TEST_CASE("a polygon is cut in the plane that fits it, whichever way it faces") {
    // The L of the test above in the plane x = 1, facing -x, which mirrors it in the plane that it is cut in.
    Polygons p;
    float yz[6][2] = {{0, 0}, {0, 2}, {1, 2}, {1, 1}, {2, 1}, {2, 0}};
    for (uint32_t i = 0; i < 6; ++i) {
        p.corners.push(i);
        p.positions.push(PrVec3{1, yz[i][0], yz[i][1]});
    }
    p.counts.push(6);
    REQUIRE(p.cut() == PrStatus_Ok);
    REQUIRE(p.polygons.count() == 4);
    float area = 0;
    for (uint32_t t = 0; t < 4; ++t) {
        PrVec3 a = p.positions[p.corners[p.triangles[t * 3]]];
        PrVec3 b = p.positions[p.corners[p.triangles[t * 3 + 1]]];
        PrVec3 c = p.positions[p.corners[p.triangles[t * 3 + 2]]];
        // The x of the cross product of the edges, which faces -x for each triangle of the polygon.
        float x = (b.y - a.y) * (c.z - a.z) - (b.z - a.z) * (c.y - a.y);
        CHECK(x < 0);
        area -= x / 2;
    }
    CHECK(area == doctest::Approx(3));
}

TEST_CASE("a polygon that cannot be cut by its ears is a fan") {
    // A bow tie crosses itself, and a polygon on a line has no area.
    Polygons p;
    p.add({{0, 0}, {1, 1}, {1, 0}, {0, 1}});
    p.add({{0, 0}, {1, 0}, {2, 0}, {3, 0}});
    REQUIRE(p.cut() == PrStatus_Ok);
    uint32_t expected[] = {0, 1, 2, 0, 2, 3, 4, 5, 6, 4, 6, 7};
    REQUIRE(p.triangles.count() == 12);
    for (uint32_t i = 0; i < 12; ++i)
        CHECK(p.triangles[i] == expected[i]);
}

TEST_CASE("a polygon of fewer than three corners makes no triangle") {
    Polygons p;
    p.add({{0, 0}, {1, 0}});
    p.add({{0, 0}, {1, 0}, {0, 1}});
    CHECK(prMeshPolygonTriangleCount(p.counts.data(), p.counts.count()) == 1);
    REQUIRE(p.cut() == PrStatus_Ok);
    CHECK(p.triangles[0] == 2);
    CHECK(p.polygons[0] == 1);
}

TEST_CASE("counts that do not match the corners, and corners past the positions, are refused") {
    Polygons p;
    p.add({{0, 0}, {1, 0}, {0, 1}});
    p.counts[0] = 4;
    CHECK(p.cut() == PrStatus_InvalidData);
    p.counts[0] = 3;
    p.corners[2] = 7;
    CHECK(p.cut() == PrStatus_OutOfRange);
}
