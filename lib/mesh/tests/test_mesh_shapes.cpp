#include "pr/array.h"
#include "pr/float.h"
#include "pr/mesh_shapes.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// A shape's mesh as prMeshShapeWrite writes it.
struct Shape {
    PrMeshShapeCounts counts{};
    Array<PrVec3> points;
    Array<uint32_t> faceCounts;
    Array<uint32_t> corners;
    Array<PrVec3> normals;

    explicit Shape(const PrMeshShapeDesc& desc) {
        counts = prMeshShapeCounts(&desc);
        points.resize(counts.pointCount);
        faceCounts.resize(counts.faceCount);
        corners.resize(counts.cornerCount);
        normals.resize(counts.cornerCount);
        prMeshShapeWrite(&desc, points.data(), faceCounts.data(), corners.data(), normals.data());
    }

    // The volume that the faces enclose, by the divergence theorem over the triangles of each face's fan. It is
    // positive if the faces wind counterclockwise seen from outside.
    double volume() const {
        double sum = 0;
        uint32_t first = 0;
        for (uint32_t count : faceCounts) {
            PrVec3 a = points[corners[first]];
            for (uint32_t k = 1; k + 1 < count; ++k) {
                PrVec3 b = points[corners[first + k]];
                PrVec3 c = points[corners[first + k + 1]];
                sum += double(prVec3Dot(a, prVec3Cross(b, c)));
            }
            first += count;
        }
        return sum / 6;
    }
};

PrMeshShapeDesc shapeDesc(PrMeshShapeKind kind, uint8_t axis) {
    return PrMeshShapeDesc{
        .kind = kind,
        .axis = axis,
        .radius = 0.5f,
        .radiusBottom = 1,
        .radiusTop = 0.5f,
        .height = 2,
        .size = {1, 2, 3},
        .segments = 64,
    };
}

// Checks what every shape's mesh holds: counts that agree, corners that name points, unit normals, and faces that turn
// toward the normals of their corners.
void checkMesh(const Shape& shape) {
    uint32_t total = 0;
    for (uint32_t count : shape.faceCounts) {
        CHECK(count >= 3);
        total += count;
    }
    CHECK(total == shape.counts.cornerCount);
    uint32_t first = 0;
    for (uint32_t count : shape.faceCounts) {
        // Newell's normal of the face, and the sum of its corners' normals.
        PrVec3 newell{};
        PrVec3 sum{};
        for (uint32_t k = 0; k < count; ++k) {
            REQUIRE(shape.corners[first + k] < shape.counts.pointCount);
            PrVec3 a = shape.points[shape.corners[first + k]];
            PrVec3 b = shape.points[shape.corners[first + (k + 1) % count]];
            newell = newell + prVec3Cross(a, b);
            PrVec3 n = shape.normals[first + k];
            CHECK(prVec3Length(n) == doctest::Approx(1).epsilon(1e-5));
            sum = sum + n;
        }
        CHECK(prVec3Dot(newell, sum) > 0);
        first += count;
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("each shape's faces wind outward around the volume of the surface that they approximate") {
    constexpr double kPi = 3.14159265358979323846;
    for (uint8_t axis = 0; axis < 3; ++axis) {
        CAPTURE(axis);
        Shape sphere(shapeDesc(PrMeshShapeKind_Sphere, axis));
        checkMesh(sphere);
        CHECK(sphere.volume() == doctest::Approx(4.0 / 3 * kPi * 0.125).epsilon(0.01));
        for (PrVec3 p : sphere.points)
            CHECK(prVec3Length(p) == doctest::Approx(0.5).epsilon(1e-5));

        Shape box(shapeDesc(PrMeshShapeKind_Box, axis));
        checkMesh(box);
        CHECK(box.volume() == doctest::Approx(6));

        // A frustum of radii 1 and 0.5 and height 2 encloses pi h (R^2 + R r + r^2) / 3.
        Shape cylinder(shapeDesc(PrMeshShapeKind_Cylinder, axis));
        checkMesh(cylinder);
        CHECK(cylinder.volume() == doctest::Approx(kPi * 2 * 1.75 / 3).epsilon(0.01));

        PrMeshShapeDesc coneDesc = shapeDesc(PrMeshShapeKind_Cylinder, axis);
        coneDesc.radiusTop = 0;
        Shape cone(coneDesc);
        checkMesh(cone);
        CHECK(cone.volume() == doctest::Approx(kPi * 2 / 3).epsilon(0.01));

        Shape capsule(shapeDesc(PrMeshShapeKind_Capsule, axis));
        checkMesh(capsule);
        CHECK(capsule.volume() == doctest::Approx(kPi * 0.25 * 2 + 4.0 / 3 * kPi * 0.125).epsilon(0.01));

        Shape plane(shapeDesc(PrMeshShapeKind_Plane, axis));
        checkMesh(plane);
        REQUIRE(plane.counts.faceCount == 1);
        // The plane faces along its axis.
        const float normal[3] = {plane.normals[0].x, plane.normals[0].y, plane.normals[0].z};
        CHECK(normal[axis] == 1);
    }
}

TEST_CASE("a shape's axis turns the axes in their cycle") {
    PrMeshShapeDesc desc = shapeDesc(PrMeshShapeKind_Box, 0);
    Shape box(desc);
    // The size's z is along x, its x along y, and its y along z.
    PrVec3 max{};
    for (PrVec3 p : box.points) {
        max.x = prF32Max(max.x, p.x);
        max.y = prF32Max(max.y, p.y);
        max.z = prF32Max(max.z, p.z);
    }
    CHECK(max.x == 1.5f);
    CHECK(max.y == 0.5f);
    CHECK(max.z == 1);
}
