#include "pr/array.h"
#include "pr/float.h"
#include "pr/mesh.h"
#include "pr/mesh_bvh.h"
#include "pr/random.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

namespace pr_impl {

namespace {

// A hierarchy and the storage that holds it.
struct Built {
    Array<PrMeshBvhNode> nodes;
    Array<uint32_t> triangles;
    PrMeshBvhView view;
};

Built build(const MeshShape& shape) {
    Built built;
    uint32_t triangleCount = shape.indices.count() / 3;
    built.nodes.resize(prMeshBvhMaxNodes(triangleCount));
    built.triangles.resize(triangleCount);
    CHECK(prMeshBvhBuild(meshView(shape.positions), meshView(shape.indices), built.nodes.data(),
        built.triangles.data(), &built.view) == PrStatus_Ok);
    return built;
}

// Finds the nearest hit of `ray` by testing every triangle, with the arithmetic of a double.
bool bruteForce(const MeshShape& shape, PrMeshRay ray, double* outT) {
    bool found = false;
    double best = double(ray.tMax);
    for (uint32_t i = 0; i + 2 < shape.indices.count(); i += 3) {
        PrVec3 p0 = shape.positions[shape.indices[i]];
        PrVec3 p1 = shape.positions[shape.indices[i + 1]];
        PrVec3 p2 = shape.positions[shape.indices[i + 2]];
        PrVec3 e1 = p1 - p0;
        PrVec3 e2 = p2 - p0;
        PrVec3 pvec = prVec3Cross(ray.direction, e2);
        double determinant = double(prVec3Dot(e1, pvec));
        if (determinant == 0)
            continue;
        PrVec3 tvec = ray.origin - p0;
        double u = double(prVec3Dot(tvec, pvec)) / determinant;
        PrVec3 qvec = prVec3Cross(tvec, e1);
        double v = double(prVec3Dot(ray.direction, qvec)) / determinant;
        double t = double(prVec3Dot(e2, qvec)) / determinant;
        // Hits within a rounding error of an edge are left to the function under test.
        if (u < 1e-4 || v < 1e-4 || u + v > 1 - 1e-4 || t < double(ray.tMin) || t >= best)
            continue;
        best = t;
        found = true;
    }
    *outT = best;
    return found;
}

PrVec3 randomUnit(PrRandom* random) {
    for (;;) {
        PrVec3 v{
            prRandomF32Between(random, -1, 1), prRandomF32Between(random, -1, 1),
            prRandomF32Between(random, -1, 1),
        };
        float length = prVec3Length(v);
        if (length > 0.1f && length <= 1)
            return v / length;
    }
}

bool isFinite(PrVec3 v) {
    return prF32IsFinite(v.x) && prF32IsFinite(v.y) && prF32IsFinite(v.z);
}

// Returns true if the box of `node` contains `point`.
bool contains(const PrMeshBvhNode& node, PrVec3 point) {
    return point.x >= node.min.x && point.x <= node.max.x && point.y >= node.min.y && point.y <= node.max.y &&
        point.z >= node.min.z && point.z <= node.max.z;
}

// Returns true if the box of every node contains the triangles of the leaves below it, each leaf holds from one to four
// triangles, and the leaves name every entry of the triangle order once.
bool isTight(const MeshShape& shape, PrMeshBvhView bvh) {
    Array<uint32_t> uses(bvh.triangleCount, 0u);
    for (uint32_t i = 0; i < bvh.nodeCount; ++i) {
        const PrMeshBvhNode& node = bvh.nodes[i];
        if (node.count == 0) {
            const PrMeshBvhNode& first = bvh.nodes[node.first];
            const PrMeshBvhNode& second = bvh.nodes[node.first + 1];
            if (!contains(node, first.min) || !contains(node, first.max) || !contains(node, second.min) ||
                    !contains(node, second.max))
                return false;
            continue;
        }
        if (node.count > 4)
            return false;
        for (uint32_t entry = node.first; entry < node.first + node.count; ++entry) {
            ++uses[entry];
            for (uint32_t corner = 0; corner < 3; ++corner) {
                if (!contains(node, shape.positions[shape.indices[bvh.triangles[entry] * 3 + corner]]))
                    return false;
            }
        }
    }
    for (uint32_t use : uses) {
        if (use != 1)
            return false;
    }
    return true;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a hierarchy holds every triangle once, in boxes that contain them") {
    MeshShape sphere = meshSphere(48, 24);
    Built built = build(sphere);
    uint32_t triangleCount = sphere.indices.count() / 3;
    CHECK(built.view.triangleCount == triangleCount);
    CHECK(built.view.nodeCount <= prMeshBvhMaxNodes(triangleCount));
    CHECK(built.view.nodeCount >= triangleCount / 4);
    CHECK(prMeshBvhIsValid(built.view, triangleCount));
    CHECK(isTight(sphere, built.view));

    Array<bool> seen(triangleCount, false);
    for (uint32_t i = 0; i < triangleCount; ++i) {
        CHECK(!seen[built.triangles[i]]);
        seen[built.triangles[i]] = true;
    }
}

TEST_CASE("a ray hits the nearest triangle, as testing every triangle finds it") {
    MeshShape sphere = meshSphere(48, 24);
    // A second, smaller sphere inside the first gives every ray farther triangles to reject.
    uint32_t outerVertices = sphere.positions.count();
    uint32_t outerIndices = sphere.indices.count();
    for (uint32_t i = 0; i < outerVertices; ++i)
        sphere.positions.push(sphere.positions[i] * 0.5f);
    for (uint32_t i = 0; i < outerIndices; ++i)
        sphere.indices.push(sphere.indices[i] + outerVertices);
    Built built = build(sphere);

    PrRandom random{.state = 21};
    uint32_t hits = 0;
    uint32_t misses = 0;
    for (uint32_t i = 0; i < 2000; ++i) {
        PrMeshRay ray{};
        ray.origin = randomUnit(&random) * prRandomF32Between(&random, 0.0f, 3.0f);
        ray.direction = randomUnit(&random) * prRandomF32Between(&random, 0.5f, 2.0f);
        ray.tMax = kPrF32Infinity;
        double expected = 0;
        bool expectHit = bruteForce(sphere, ray, &expected);
        PrMeshRayHit hit;
        bool isHit = prMeshBvhIntersect(built.view, meshView(sphere.positions), meshView(sphere.indices), ray, &hit);
        if (!expectHit) {
            // The reference leaves out hits on edges, which the query may report.
            misses += isHit ? 0 : 1;
            continue;
        }
        CHECK(isHit);
        CHECK(double(hit.t) <= expected * 1.0001 + 1e-5);
        if (double(hit.t) < expected * 0.9999 - 1e-5)
            continue;
        ++hits;

        // The barycentric weights give the same point as the distance along the ray.
        PrVec3 p0 = sphere.positions[sphere.indices[hit.triangle * 3]];
        PrVec3 p1 = sphere.positions[sphere.indices[hit.triangle * 3 + 1]];
        PrVec3 p2 = sphere.positions[sphere.indices[hit.triangle * 3 + 2]];
        float u = hit.barycentrics.x;
        float v = hit.barycentrics.y;
        PrVec3 onTriangle = p0 * (1 - u - v) + p1 * u + p2 * v;
        CHECK(meshTestNear(onTriangle, ray.origin + ray.direction * hit.t, 1e-4f));
    }
    CHECK(hits > 800);
    CHECK(misses > 100);
}

TEST_CASE("a ray is searched only between its limits") {
    MeshShape grid = meshGrid(4, 4);
    Built built = build(grid);
    PrMeshAttributeView positions = meshView(grid.positions);
    PrMeshIndexView indices = meshView(grid.indices);
    PrMeshRay ray{.origin = {1.25f, 2.5f, 3}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 10};
    PrMeshRayHit hit;
    CHECK(prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    CHECK(hit.t == 3);
    // The square at column 1, row 2. The point is above its diagonal, in the second of its two triangles.
    CHECK(hit.triangle == (2 * 4 + 1) * 2 + 1);
    CHECK(hit.barycentrics.x == doctest::Approx(0.25f));
    CHECK(hit.barycentrics.y == doctest::Approx(0.25f));

    ray.tMax = 2.5f;
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    CHECK(hit.triangle == kPrMeshIndexNone);
    ray.tMax = 10;
    ray.tMin = 3.5f;
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));

    // From behind, the ray still hits: triangles have two sides.
    ray = PrMeshRay{.origin = {1.25f, 2.5f, -2}, .tMin = 0, .direction = {0, 0, 4}, .tMax = 10};
    CHECK(prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    CHECK(hit.t == 0.5f);

    // Parallel to the grid, and aimed past it.
    ray = PrMeshRay{.origin = {-1, 2, 1}, .tMin = 0, .direction = {1, 0, 0}, .tMax = 100};
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    ray = PrMeshRay{.origin = {9, 9, 3}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 100};
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
}

TEST_CASE("a ray that is not finite or has no direction hits nothing") {
    MeshShape grid = meshGrid(2, 2);
    Built built = build(grid);
    PrMeshAttributeView positions = meshView(grid.positions);
    PrMeshIndexView indices = meshView(grid.indices);
    PrMeshRay good{.origin = {0.5f, 0.5f, 1}, .tMin = 0, .direction = {0, 0, -1}, .tMax = kPrF32Infinity};
    PrMeshRayHit hit;
    CHECK(prMeshBvhIntersect(built.view, positions, indices, good, &hit));

    PrMeshRay ray = good;
    ray.origin.x = kPrF32Nan;
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    ray = good;
    ray.direction.z = -kPrF32Infinity;
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    ray = good;
    ray.direction = PrVec3{0, 0, 0};
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
    ray = good;
    ray.tMax = kPrF32Nan;
    CHECK(!prMeshBvhIntersect(built.view, positions, indices, ray, &hit));
}

TEST_CASE("triangles that are not finite are left out, and the rest are still hit") {
    MeshShape grid = meshGrid(3, 1);
    grid.positions[0].z = kPrF32Nan;
    grid.positions[7].x = kPrF32Infinity;
    Built built = build(grid);
    // Vertex 0 is in both triangles of the first square, and vertex 7 in both of the last.
    CHECK(built.view.triangleCount == 2);
    CHECK(prMeshBvhIsValid(built.view, 6));
    PrMeshRayHit hit;
    PrMeshRay ray{.origin = {1.75f, 0.25f, 1}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 10};
    CHECK(prMeshBvhIntersect(built.view, meshView(grid.positions), meshView(grid.indices), ray, &hit));
    CHECK(hit.triangle == 2);
    ray.origin = PrVec3{0.5f, 0.25f, 1};
    CHECK(!prMeshBvhIntersect(built.view, meshView(grid.positions), meshView(grid.indices), ray, &hit));
}

TEST_CASE("a mesh without triangles builds a hierarchy without nodes") {
    MeshShape empty;
    PrMeshBvhView view{};
    CHECK(prMeshBvhMaxNodes(0) == 0);
    CHECK(prMeshBvhBuild(meshView(empty.positions), meshView(empty.indices), nullptr, nullptr, &view) == PrStatus_Ok);
    CHECK(view.nodeCount == 0);
    CHECK(prMeshBvhIsValid(view, 0));
    PrMeshRayHit hit;
    PrMeshRay ray{.origin = {0, 0, 1}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 10};
    CHECK(!prMeshBvhIntersect(view, meshView(empty.positions), meshView(empty.indices), ray, &hit));
}

TEST_CASE("triangles that no split separates still build a shallow, valid hierarchy") {
    // A thousand copies of one triangle, and a thousand more at huge coordinates whose areas overflow.
    MeshShape shape;
    for (uint32_t i = 0; i < 1000; ++i) {
        uint32_t base = shape.positions.count();
        shape.positions.push(PrVec3{0, 0, 0});
        shape.positions.push(PrVec3{1, 0, 0});
        shape.positions.push(PrVec3{0, 1, 0});
        shape.positions.push(PrVec3{-3e38f, -3e38f, float(i)});
        shape.positions.push(PrVec3{3e38f, -3e38f, float(i)});
        shape.positions.push(PrVec3{0, 3e38f, float(i)});
        for (uint32_t corner = 0; corner < 6; ++corner)
            shape.indices.push(base + corner);
    }
    Built built = build(shape);
    CHECK(built.view.triangleCount == 2000);
    CHECK(prMeshBvhIsValid(built.view, 2000));
    CHECK(isTight(shape, built.view));
    PrMeshRayHit hit;
    PrMeshRay ray{.origin = {0.25f, 0.25f, -1}, .tMin = 0, .direction = {0, 0, 1}, .tMax = 10};
    CHECK(prMeshBvhIntersect(built.view, meshView(shape.positions), meshView(shape.indices), ray, &hit));
    CHECK(hit.t == 1);
}

TEST_CASE("a hierarchy works from a copy of its storage") {
    MeshShape sphere = meshSphere(16, 8);
    Built built = build(sphere);
    Array<PrMeshBvhNode> nodes;
    Array<uint32_t> triangles;
    nodes.append(built.nodes.data(), built.view.nodeCount);
    triangles.append(built.triangles.data(), built.view.triangleCount);
    PrMeshBvhView copy{nodes.data(), nodes.count(), triangles.count(), triangles.data()};
    built.nodes.reset();
    built.triangles.reset();
    CHECK(prMeshBvhIsValid(copy, sphere.indices.count() / 3));
    PrMeshRayHit hit;
    PrMeshRay ray{.origin = {0.1f, 3, 0.2f}, .tMin = 0, .direction = {0, -1, 0}, .tMax = 10};
    CHECK(prMeshBvhIntersect(copy, meshView(sphere.positions), meshView(sphere.indices), ray, &hit));
    CHECK(hit.t == doctest::Approx(2.0f).epsilon(0.02));
}

TEST_CASE("a hierarchy that is not well formed is not valid") {
    MeshShape grid = meshGrid(4, 4);
    Built built = build(grid);
    uint32_t triangleCount = grid.indices.count() / 3;
    REQUIRE(prMeshBvhIsValid(built.view, triangleCount));
    REQUIRE(built.view.nodeCount >= 5);
    REQUIRE(built.nodes[0].count == 0);

    // The hierarchy of a larger mesh names triangles that a smaller mesh does not have.
    CHECK(!prMeshBvhIsValid(built.view, triangleCount - 1));

    PrMeshBvhNode root = built.nodes[0];
    built.nodes[0].first = 0;                       // a node that is its own child
    CHECK(!prMeshBvhIsValid(built.view, triangleCount));
    built.nodes[0].first = built.view.nodeCount - 1; // a second child past the end
    CHECK(!prMeshBvhIsValid(built.view, triangleCount));
    built.nodes[0] = root;

    uint32_t leaf = 0;
    while (built.nodes[leaf].count == 0)
        ++leaf;
    PrMeshBvhNode saved = built.nodes[leaf];
    built.nodes[leaf].count = triangleCount + 1;    // a leaf with more triangles than the order holds
    CHECK(!prMeshBvhIsValid(built.view, triangleCount));
    built.nodes[leaf].first = root.first;           // two parents for one pair of children
    built.nodes[leaf].count = 0;
    CHECK(!prMeshBvhIsValid(built.view, triangleCount));
    built.nodes[leaf] = saved;

    PrMeshBvhView shorter = built.view;
    shorter.nodeCount -= 1;                         // a child that is not there
    CHECK(!prMeshBvhIsValid(shorter, triangleCount));
    CHECK(prMeshBvhIsValid(built.view, triangleCount));
}

TEST_CASE("a build rejects indices that name no vertex and counts that are not triangles") {
    MeshShape grid = meshGrid(1, 1);
    PrMeshBvhNode nodes[3];
    uint32_t triangles[2];
    PrMeshBvhView view;
    grid.indices[5] = 4;
    CHECK(prMeshBvhBuild(meshView(grid.positions), meshView(grid.indices), nodes, triangles, &view) ==
        PrStatus_OutOfRange);
    CHECK(view.nodeCount == 0);
    grid.indices.pop();
    CHECK(prMeshBvhBuild(meshView(grid.positions), meshView(grid.indices), nodes, triangles, &view) ==
        PrStatus_InvalidData);
    PrMeshAttributeView positions = meshView(grid.positions);
    positions.stride = 4;
    CHECK(prMeshBvhBuild(positions, meshSequence(3), nodes, triangles, &view) == PrStatus_InvalidData);
}

TEST_CASE("a refitted hierarchy holds its moved triangles and finds the hits that testing every triangle finds") {
    MeshShape sphere = meshSphere(32, 16);
    Built built = build(sphere);
    uint32_t triangleCount = sphere.indices.count() / 3;

    // Positions that did not move refit to the boxes of the build.
    Array<PrMeshBvhNode> refitted(built.view.nodeCount, PrMeshBvhNode{});
    REQUIRE(prMeshBvhRefit(built.view, meshView(sphere.positions), meshView(sphere.indices), refitted.data()) ==
        PrStatus_Ok);
    CHECK(prMemEquals(refitted.data(), built.nodes.data(), built.view.nodeCount * sizeof(PrMeshBvhNode)));
    CHECK(prMeshBvhCost(built.view) > 0);

    // A twist about z, by an angle that grows with height, and a stretch along it.
    MeshShape twisted = sphere;
    for (PrVec3& p : twisted.positions) {
        float angle = p.z * 1.5f;
        float c = 1 - angle * angle / 2 + angle * angle * angle * angle / 24;
        float s = angle - angle * angle * angle / 6;
        p = PrVec3{p.x * c - p.y * s, p.x * s + p.y * c, p.z * 2};
    }
    REQUIRE(prMeshBvhRefit(built.view, meshView(twisted.positions), meshView(twisted.indices), refitted.data()) ==
        PrStatus_Ok);
    PrMeshBvhView view = built.view;
    view.nodes = refitted.data();
    CHECK(prMeshBvhIsValid(view, triangleCount));
    CHECK(isTight(twisted, view));

    PrRandom random{.state = 7};
    uint32_t hits = 0;
    for (uint32_t i = 0; i < 1000; ++i) {
        PrMeshRay ray{};
        ray.origin = randomUnit(&random) * prRandomF32Between(&random, 0.0f, 4.0f);
        ray.direction = randomUnit(&random);
        ray.tMax = kPrF32Infinity;
        double expected = 0;
        if (!bruteForce(twisted, ray, &expected))
            continue;
        PrMeshRayHit hit;
        REQUIRE(prMeshBvhIntersect(view, meshView(twisted.positions), meshView(twisted.indices), ray, &hit));
        CHECK(double(hit.t) <= expected * 1.0001 + 1e-5);
        ++hits;
    }
    CHECK(hits > 300);

    // In place, the tree refits to the same boxes.
    REQUIRE(prMeshBvhRefit(built.view, meshView(twisted.positions), meshView(twisted.indices), built.nodes.data()) ==
        PrStatus_Ok);
    CHECK(prMemEquals(refitted.data(), built.nodes.data(), built.view.nodeCount * sizeof(PrMeshBvhNode)));

    PrMeshAttributeView unreadable = meshView(twisted.positions);
    unreadable.stride = 4;
    CHECK(prMeshBvhRefit(built.view, unreadable, meshView(twisted.indices), refitted.data()) == PrStatus_InvalidData);
}

TEST_CASE("a refitted tree costs more as its triangles scatter, and a build over them costs less again") {
    // A small triangle of its own in each cell of a grid of 16 by 16.
    MeshShape grid;
    for (uint32_t cell = 0; cell < 256; ++cell) {
        PrVec3 corner{float(cell % 16), float(cell / 16), 0};
        grid.positions.push(corner);
        grid.positions.push(corner + PrVec3{0.5f, 0, 0});
        grid.positions.push(corner + PrVec3{0, 0.5f, 0});
        for (uint32_t k = 0; k < 3; ++k)
            grid.indices.push(cell * 3 + k);
    }
    Built built = build(grid);
    float cost = prMeshBvhCost(built.view);
    CHECK(cost > 0);

    // A move of the whole mesh keeps each node's share of the root's area, and so the cost.
    MeshShape moved = grid;
    for (PrVec3& p : moved.positions)
        p = p * 4.0f + PrVec3{100, -50, 2};
    Array<PrMeshBvhNode> refitted(built.view.nodeCount, PrMeshBvhNode{});
    REQUIRE(prMeshBvhRefit(built.view, meshView(moved.positions), meshView(moved.indices), refitted.data()) ==
        PrStatus_Ok);
    PrMeshBvhView view = built.view;
    view.nodes = refitted.data();
    CHECK(prMeshBvhCost(view) == doctest::Approx(cost).epsilon(1e-4));

    // Triangles that trade cells give each node a box across the grid, though each triangle keeps its size.
    MeshShape scattered = grid;
    PrRandom random{.state = 3};
    for (uint32_t i = 256; i > 1; --i) {
        uint32_t j = prRandomU32Below(&random, i);
        for (uint32_t k = 0; k < 3; ++k) {
            PrVec3 swapped = scattered.positions[(i - 1) * 3 + k];
            scattered.positions[(i - 1) * 3 + k] = scattered.positions[j * 3 + k];
            scattered.positions[j * 3 + k] = swapped;
        }
    }
    REQUIRE(prMeshBvhRefit(built.view, meshView(scattered.positions), meshView(scattered.indices), refitted.data()) ==
        PrStatus_Ok);
    float scatteredCost = prMeshBvhCost(view);
    CHECK(scatteredCost > cost * 4);
    Built rebuilt = build(scattered);
    CHECK(prMeshBvhCost(rebuilt.view) < scatteredCost / 2);

    PrMeshBvhView empty{};
    CHECK(prMeshBvhCost(empty) == 0);
}

TEST_CASE("a refit leaves a triangle whose coordinates are no longer finite out of its leaf's box") {
    MeshShape grid = meshGrid(3, 1);
    Built built = build(grid);
    // Vertex 0 is in both triangles of the first square.
    grid.positions[0].z = kPrF32Nan;
    REQUIRE(prMeshBvhRefit(built.view, meshView(grid.positions), meshView(grid.indices), built.nodes.data()) ==
        PrStatus_Ok);
    // Each box is empty, for a leaf of those two triangles alone, or finite.
    for (uint32_t i = 0; i < built.view.nodeCount; ++i) {
        const PrMeshBvhNode& node = built.nodes[i];
        bool isEmpty = node.min.x > node.max.x;
        CHECK((isEmpty || (isFinite(node.min) && isFinite(node.max))));
    }
    PrMeshRayHit hit;
    PrMeshRay ray{.origin = {1.75f, 0.25f, 1}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 10};
    CHECK(prMeshBvhIntersect(built.view, meshView(grid.positions), meshView(grid.indices), ray, &hit));
    CHECK(hit.triangle == 2);
}
