#include "pr/array.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/mesh.h"
#include "pr/random.h"
#include "pr/status.h"
#include "pr/vec.h"

#include "mesh/tests/mesh_fixture.h"

extern "C" int prTestMeshCApi(void);

namespace pr_impl {

namespace {

// A vertex with its position in the middle, to read through a stride.
struct Interleaved {
    uint32_t id;
    PrVec3 position;
    float weight;
};

// Returns true if every finite point of `points` is inside `sphere`, allowing the rounding of a float distance.
bool contains(PrSphere sphere, const Array<PrVec3>& points) {
    for (PrVec3 point : points) {
        if (!prF32IsFinite(point.x) || !prF32IsFinite(point.y) || !prF32IsFinite(point.z))
            continue;
        if (prVec3Length(point - sphere.center) > sphere.radius * 1.00001f + 1e-6f)
            return false;
    }
    return true;
}

// Returns the normal of triangle `triangle` of a triangle list, not normalized.
PrVec3 windingOf(const Array<PrVec3>& positions, const Array<uint32_t>& list, uint32_t triangle) {
    PrVec3 a = positions[list[triangle * 3]];
    PrVec3 b = positions[list[triangle * 3 + 1]];
    PrVec3 c = positions[list[triangle * 3 + 2]];
    return prVec3Cross(b - a, c - a);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("the public headers compile as C") {
    CHECK(prTestMeshCApi() == 0);
}

TEST_CASE("a box holds the finite points and ignores the rest") {
    Array<PrVec3> points;
    points.push(PrVec3{kPrF32Nan, 0, 0});
    points.push(PrVec3{1, -2, 3});
    points.push(PrVec3{-4, 5, kPrF32Infinity});
    points.push(PrVec3{-1, 7, -6});
    PrBox3 box;
    CHECK(prMeshBox(meshView(points), &box) == PrStatus_Ok);
    CHECK(box.min == PrVec3{-1, -2, -6});
    CHECK(box.max == PrVec3{1, 7, 3});
}

TEST_CASE("bounds of no finite point are zeros") {
    Array<PrVec3> points;
    PrBox3 box{{1, 1, 1}, {2, 2, 2}};
    PrSphere sphere{{1, 1, 1}, 5};
    CHECK(prMeshBox(meshView(points), &box) == PrStatus_Ok);
    CHECK(box.min == PrVec3{0, 0, 0});
    CHECK(box.max == PrVec3{0, 0, 0});
    points.push(PrVec3{kPrF32Nan, kPrF32Nan, kPrF32Nan});
    CHECK(prMeshSphere(meshView(points), &sphere) == PrStatus_Ok);
    CHECK(sphere.center == PrVec3{0, 0, 0});
    CHECK(sphere.radius == 0);
}

TEST_CASE("bounds read positions through a stride and reject a stride that is too small") {
    Interleaved vertices[3] = {{1, {1, 2, 3}, 100}, {2, {-1, 0, 5}, 200}, {3, {0, 9, 4}, 300}};
    PrMeshAttributeView view{&vertices[0].position, 3, sizeof(Interleaved), prMeshFormatFloat32(3)};
    PrBox3 box;
    CHECK(prMeshBox(view, &box) == PrStatus_Ok);
    CHECK(box.min == PrVec3{-1, 0, 3});
    CHECK(box.max == PrVec3{1, 9, 5});

    view.stride = 8;
    PrSphere sphere;
    CHECK(prMeshBox(view, &box) == PrStatus_InvalidData);
    CHECK(prMeshSphere(view, &sphere) == PrStatus_InvalidData);
    view.stride = sizeof(Interleaved);
    view.format = PrMeshFormat{.component = PrMeshComponent_Float64, .count = 3};
    CHECK(prMeshBox(view, &box) == PrStatus_InvalidData);
    view.format = prMeshFormatFloat32(2);
    CHECK(prMeshBox(view, &box) == PrStatus_InvalidData);
    view.format = prMeshFormatFloat32(3);
    view.data = nullptr;
    CHECK(prMeshBox(view, &box) == PrStatus_InvalidData);
}

TEST_CASE("a format's size is its components', and a float is never normalized") {
    CHECK(prMeshFormatSize(prMeshFormatFloat32(3)) == 12);
    CHECK(prMeshFormatSize(PrMeshFormat{.component = PrMeshComponent_Uint8, .count = 4, .normalized = true}) == 4);
    CHECK(prMeshFormatSize(PrMeshFormat{.component = PrMeshComponent_Int64, .count = 1}) == 8);
    CHECK(prMeshFormatSize(PrMeshFormat{.component = PrMeshComponent_Float16, .count = 2}) == 4);
    CHECK_FALSE(prMeshFormatIsValid(PrMeshFormat{.component = PrMeshComponent_Float32, .count = 2, .normalized = true}));
    CHECK_FALSE(prMeshFormatIsValid(PrMeshFormat{.component = PrMeshComponent_Float32, .count = 0}));
    CHECK_FALSE(prMeshFormatIsValid(PrMeshFormat{.component = PrMeshComponent_Float32, .count = 5}));
    CHECK_FALSE(prMeshFormatIsValid(PrMeshFormat{.component = PrMeshComponent_Count, .count = 1}));
}

TEST_CASE("a conversion reads doubles and 64-bit integers as numbers and writes them as floats") {
    const double heat[3] = {0.25, -1e10, 1.0 / 3.0};
    float written[3] = {};
    PrMeshAttributeView doubles{heat, 3, sizeof(double), PrMeshFormat{.component = PrMeshComponent_Float64, .count = 1}};
    CHECK(prMeshAttributeConvert(doubles, nullptr, PrMeshAttribute{written, 3, 4, prMeshFormatFloat32(1)}) ==
        PrStatus_Ok);
    CHECK(written[0] == 0.25f);
    CHECK(written[1] == -1e10f);
    CHECK(written[2] == float(1.0 / 3.0));

    const int64_t bands[3] = {-7, int64_t(1) << 40, 9007199254740993};
    PrMeshAttributeView integers{bands, 3, sizeof(int64_t), PrMeshFormat{.component = PrMeshComponent_Int64, .count = 1}};
    CHECK(prMeshAttributeConvert(integers, nullptr, PrMeshAttribute{written, 3, 4, prMeshFormatFloat32(1)}) ==
        PrStatus_Ok);
    CHECK(written[0] == -7);
    CHECK(written[1] == 1099511627776.0f);
    CHECK(written[2] == float(9007199254740992.0));

    // Into 32-bit integers, a 64-bit value is clamped to the type's range.
    int32_t narrowed[3] = {};
    CHECK(prMeshAttributeConvert(integers, nullptr,
        PrMeshAttribute{narrowed, 3, 4, PrMeshFormat{.component = PrMeshComponent_Int32, .count = 1}}) == PrStatus_Ok);
    CHECK(narrowed[0] == -7);
    CHECK(narrowed[1] == 2147483647);
}

TEST_CASE("a conversion normalizes, rounds and clamps integers, and fills what the source lacks from the fallback") {
    // Three channels of 16-bit color, normalized, become four of 8 bits, with the alpha of the fallback.
    const uint16_t colors[6] = {65535, 0, 32768, 0, 65535, 13107};
    uint8_t packed[8] = {};
    PrMeshAttributeView source{
        colors, 2, 6, PrMeshFormat{.component = PrMeshComponent_Uint16, .count = 3, .normalized = true},
    };
    const float fallback[4] = {1, 1, 1, 0.5f};
    CHECK(prMeshAttributeConvert(source, fallback, PrMeshAttribute{
        packed, 2, 4, PrMeshFormat{.component = PrMeshComponent_Uint8, .count = 4, .normalized = true},
    }) == PrStatus_Ok);
    const uint8_t expected[8] = {255, 0, 128, 128, 0, 255, 51, 128};
    for (uint32_t i = 0; i < 8; ++i)
        CHECK(packed[i] == expected[i]);

    // Signed normalized values reach -1 at the type's least value, and a float beyond [-1, 1] is clamped.
    const float directions[4] = {-1, 0.5f, 2, kPrF32Nan};
    int16_t snorm[4] = {};
    CHECK(prMeshAttributeConvert(PrMeshAttributeView{directions, 1, 16, prMeshFormatFloat32(4)}, nullptr,
        PrMeshAttribute{
            snorm, 1, 8, PrMeshFormat{.component = PrMeshComponent_Int16, .count = 4, .normalized = true},
        }) == PrStatus_Ok);
    CHECK(snorm[0] == -32767);
    CHECK(snorm[1] == 16384);
    CHECK(snorm[2] == 32767);
    // A component that is not finite takes the fallback's, which is (0, 0, 0, 1) by default.
    CHECK(snorm[3] == 32767);

    // Vertices past the source's, and every vertex of a source without data, take the fallback.
    float uv[6] = {};
    const float one[2] = {0.25f, 0.75f};
    CHECK(prMeshAttributeConvert(PrMeshAttributeView{one, 1, 8, prMeshFormatFloat32(2)}, fallback,
        PrMeshAttribute{uv, 3, 8, prMeshFormatFloat32(2)}) == PrStatus_Ok);
    CHECK(uv[0] == 0.25f);
    CHECK(uv[1] == 0.75f);
    CHECK(uv[2] == 1);
    CHECK(uv[5] == 1);
    CHECK(prMeshAttributeConvert(PrMeshAttributeView{}, nullptr, PrMeshAttribute{uv, 3, 8, prMeshFormatFloat32(2)}) ==
        PrStatus_Ok);
    CHECK(uv[0] == 0);
    CHECK(uv[4] == 0);
}

TEST_CASE("a conversion writes one attribute of interleaved vertices and leaves the rest of each vertex alone") {
    struct Vertex {
        float uv[2];
        uint8_t color[4];
        uint32_t marker;
    };
    Vertex vertices[2] = {{{9, 9}, {9, 9, 9, 9}, 7}, {{9, 9}, {9, 9, 9, 9}, 7}};
    const float colors[8] = {1, 0, 0, 1, 0, 0.5f, 1, 0};
    CHECK(prMeshAttributeConvert(PrMeshAttributeView{colors, 2, 16, prMeshFormatFloat32(4)}, nullptr, PrMeshAttribute{
        &vertices[0].color, 2, sizeof(Vertex),
        PrMeshFormat{.component = PrMeshComponent_Uint8, .count = 4, .normalized = true},
    }) == PrStatus_Ok);
    CHECK(vertices[1].color[1] == 128);
    CHECK(vertices[0].uv[0] == 9);
    CHECK(vertices[1].marker == 7);
    // A stride smaller than a value is refused.
    CHECK(prMeshAttributeConvert(PrMeshAttributeView{colors, 2, 8, prMeshFormatFloat32(4)}, nullptr, PrMeshAttribute{
        &vertices[0].color, 2, sizeof(Vertex),
        PrMeshFormat{.component = PrMeshComponent_Uint8, .count = 4, .normalized = true},
    }) == PrStatus_InvalidData);
}

TEST_CASE("the sphere of one point has no radius, and of two points spans them") {
    Array<PrVec3> points;
    points.push(PrVec3{3, 4, 5});
    PrSphere sphere;
    CHECK(prMeshSphere(meshView(points), &sphere) == PrStatus_Ok);
    CHECK(sphere.center == PrVec3{3, 4, 5});
    CHECK(sphere.radius == 0);
    points.push(PrVec3{7, 4, 5});
    CHECK(prMeshSphere(meshView(points), &sphere) == PrStatus_Ok);
    CHECK(sphere.center == PrVec3{5, 4, 5});
    CHECK(sphere.radius == 2);
}

TEST_CASE("the sphere of a cube's corners is the smallest one") {
    Array<PrVec3> points;
    for (uint32_t i = 0; i < 8; ++i)
        points.push(PrVec3{float(i & 1) * 2, float((i >> 1) & 1) * 2, float(i >> 2) * 2});
    PrSphere sphere;
    CHECK(prMeshSphere(meshView(points), &sphere) == PrStatus_Ok);
    CHECK(contains(sphere, points));
    CHECK(sphere.radius == doctest::Approx(prF32Sqrt(3.0f)).epsilon(1e-5));
}

TEST_CASE("the sphere contains scattered points and is close to the smallest") {
    // The points lie on the unit sphere and cover it, so the smallest sphere has a radius of 1.
    MeshShape shape = meshSphere(24, 12);
    PrRandom random{.state = 7};
    for (uint32_t i = 0; i < 500; ++i) {
        float scale = prRandomF32(&random);
        shape.positions.push(shape.positions[i % 300] * scale);
    }
    shape.positions.push(PrVec3{kPrF32Infinity, 0, 0});
    PrSphere sphere;
    CHECK(prMeshSphere(meshView(shape.positions), &sphere) == PrStatus_Ok);
    CHECK(contains(sphere, shape.positions));
    CHECK(sphere.radius < 1.02f);

    // Points along a diagonal are far from filling their box, which a sphere around the box's center would not see.
    Array<PrVec3> diagonal;
    for (uint32_t i = 0; i <= 100; ++i)
        diagonal.push(PrVec3{float(i), float(i) * 0.5f, float(i % 7) * 0.01f});
    CHECK(prMeshSphere(meshView(diagonal), &sphere) == PrStatus_Ok);
    CHECK(contains(sphere, diagonal));
    PrBox3 box;
    CHECK(prMeshBox(meshView(diagonal), &box) == PrStatus_Ok);
    CHECK(sphere.radius <= prVec3Length(box.max - box.min) * 0.5f * 1.00001f);
    CHECK(sphere.radius < 57.0f);
}

TEST_CASE("the sphere of huge coordinates does not overflow") {
    Array<PrVec3> points;
    points.push(PrVec3{-3e38f, 0, 0});
    points.push(PrVec3{3e38f, 1e38f, 0});
    PrSphere sphere;
    CHECK(prMeshSphere(meshView(points), &sphere) == PrStatus_Ok);
    CHECK(!prF32IsNan(sphere.radius));
    CHECK(sphere.radius >= 3e38f);
    CHECK(prF32IsFinite(sphere.center.x));
}

TEST_CASE("a triangle list counts three indices for each triangle of a strip or a fan") {
    CHECK(prMeshTriangleListCount(PrMeshTopology_TriangleList, 7) == 6);
    CHECK(prMeshTriangleListCount(PrMeshTopology_TriangleStrip, 2) == 0);
    CHECK(prMeshTriangleListCount(PrMeshTopology_TriangleStrip, 5) == 9);
    CHECK(prMeshTriangleListCount(PrMeshTopology_TriangleFan, 6) == 12);
    CHECK(prMeshTriangleListCount(PrMeshTopology_TriangleFan, 0xffffffffu) == 0);
    CHECK(prMeshTriangleListCount(PrMeshTopology(9), 6) == 0);
}

TEST_CASE("a strip keeps each triangle's first vertex and exchanges the others of every second triangle") {
    uint16_t strip[5] = {10, 11, 12, 13, 14};
    Array<uint32_t> list(9u);
    PrMeshIndexView view{strip, 5, PrMeshIndexType_U16};
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, view, 15, meshIndices(list)) == PrStatus_Ok);
    uint32_t expected[9] = {10, 11, 12, 11, 13, 12, 12, 13, 14};
    CHECK(list == Array<uint32_t>(expected));
}

TEST_CASE("a fan ends each triangle with the fan's first vertex") {
    uint32_t fan[5] = {4, 0, 1, 2, 3};
    Array<uint32_t> list(9u);
    PrMeshIndexView view{fan, 5, PrMeshIndexType_U32};
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleFan, view, 5, meshIndices(list)) == PrStatus_Ok);
    uint32_t expected[9] = {0, 1, 4, 1, 2, 4, 2, 3, 4};
    CHECK(list == Array<uint32_t>(expected));
}

TEST_CASE("strips and fans without indices keep one winding") {
    // A zigzag strip and a fan, both counterclockwise seen from +z.
    Array<PrVec3> strip;
    for (uint32_t i = 0; i < 8; ++i)
        strip.push(PrVec3{float(i / 2), float(1 - i % 2), 0});
    Array<uint32_t> list(prMeshTriangleListCount(PrMeshTopology_TriangleStrip, 8));
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, meshSequence(8), 8, meshIndices(list)) == PrStatus_Ok);
    PrVec3 first = windingOf(strip, list, 0);
    CHECK(first.z != 0);
    for (uint32_t triangle = 0; triangle < 6; ++triangle)
        CHECK(windingOf(strip, list, triangle).z * first.z > 0);

    Array<PrVec3> fan;
    fan.push(PrVec3{0, 0, 0});
    for (uint32_t i = 0; i < 6; ++i)
        fan.push(PrVec3{prF32Cos(float(i)), prF32Sin(float(i)), 0});
    Array<uint32_t> fanList(prMeshTriangleListCount(PrMeshTopology_TriangleFan, 7));
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleFan, meshSequence(7), 7, meshIndices(fanList)) == PrStatus_Ok);
    for (uint32_t triangle = 0; triangle < 5; ++triangle)
        CHECK(windingOf(fan, fanList, triangle).z > 0);
}

TEST_CASE("a triangle list converts to itself, drops a partial triangle and changes index type") {
    uint32_t wide[7] = {0, 1, 2, 2, 1, 3, 0};
    uint16_t narrow[6] = {};
    PrMeshIndexView view{wide, 7, PrMeshIndexType_U32};
    PrMeshIndices out{narrow, 6, PrMeshIndexType_U16};
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleList, view, 4, out) == PrStatus_Ok);
    CHECK(narrow[3] == 2);
    CHECK(narrow[5] == 3);
}

TEST_CASE("a triangle list rejects what it cannot read or write") {
    uint32_t indices[4] = {0, 1, 70000, 2};
    uint16_t narrow[6] = {};
    uint32_t wide[6] = {};
    PrMeshIndexView view{indices, 4, PrMeshIndexType_U32};
    PrMeshIndices narrowOut{narrow, 6, PrMeshIndexType_U16};
    PrMeshIndices wideOut{wide, 6, PrMeshIndexType_U32};
    // An index past the vertices, and an index that the output's type cannot hold.
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, view, 100, wideOut) == PrStatus_OutOfRange);
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, view, 80000, narrowOut) == PrStatus_OutOfRange);
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, view, 80000, wideOut) == PrStatus_Ok);
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, meshSequence(4), 3, wideOut) == PrStatus_OutOfRange);
    CHECK(prMeshTriangleList(PrMeshTopology(7), view, 80000, wideOut) == PrStatus_InvalidData);
    view.type = PrMeshIndexType(5);
    CHECK(prMeshTriangleList(PrMeshTopology_TriangleStrip, view, 80000, wideOut) == PrStatus_InvalidData);
}

TEST_CASE("unindexing and welding return a mesh to its shared vertices") {
    MeshShape grid = meshGrid(3, 2);
    Array<PrVec3> corners(grid.indices.count());
    CHECK(prMeshVerticesUnindex(grid.positions.data(), grid.positions.count(), sizeof(PrVec3), meshView(grid.indices),
        corners.data()) == PrStatus_Ok);
    for (uint32_t i = 0; i < corners.count(); ++i)
        CHECK(corners[i] == grid.positions[grid.indices[i]]);

    Array<uint32_t> remap(corners.count());
    uint32_t count = 0;
    CHECK(prMeshWeldRemap(corners.data(), corners.count(), sizeof(PrVec3), remap.data(), &count) == PrStatus_Ok);
    CHECK(count == grid.positions.count());
    // Welded vertices are numbered in the order of their first corners.
    CHECK(remap[0] == 0);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 2);
    CHECK(remap[3] == 0);
    CHECK(remap[4] == 2);
    CHECK(remap[5] == 3);

    Array<PrVec3> welded(count);
    Array<uint32_t> indices(corners.count());
    CHECK(prMeshVerticesRemap(corners.data(), corners.count(), sizeof(PrVec3), remap.data(), welded.data(), count) ==
        PrStatus_Ok);
    CHECK(prMeshIndicesRemap(meshSequence(corners.count()), remap.data(), corners.count(), meshIndices(indices)) ==
        PrStatus_Ok);
    for (uint32_t i = 0; i < indices.count(); ++i)
        CHECK(welded[indices[i]] == corners[i]);
}

TEST_CASE("welding compares bytes, so zero and negative zero stay apart") {
    float values[4] = {0.0f, -0.0f, 0.0f, 1.0f};
    uint32_t remap[4];
    uint32_t count = 0;
    CHECK(prMeshWeldRemap(values, 4, sizeof(float), remap, &count) == PrStatus_Ok);
    CHECK(count == 3);
    CHECK(remap[0] == 0);
    CHECK(remap[1] == 1);
    CHECK(remap[2] == 0);
    CHECK(remap[3] == 2);
    CHECK(prMeshWeldRemap(values, 4, 0, remap, &count) == PrStatus_InvalidData);
}

TEST_CASE("a fetch remap orders vertices by first use and leaves out the unused") {
    uint16_t indices[6] = {5, 2, 4, 4, 2, 0};
    PrMeshIndexView view{indices, 6, PrMeshIndexType_U16};
    uint32_t remap[6];
    uint32_t count = 0;
    CHECK(prMeshFetchRemap(view, 6, remap, &count) == PrStatus_Ok);
    CHECK(count == 4);
    uint32_t expected[6] = {3, kPrMeshIndexNone, 1, kPrMeshIndexNone, 2, 0};
    for (uint32_t i = 0; i < 6; ++i)
        CHECK(remap[i] == expected[i]);

    uint8_t vertices[6] = {10, 11, 12, 13, 14, 15};
    uint8_t moved[4] = {};
    CHECK(prMeshVerticesRemap(vertices, 6, 1, remap, moved, 4) == PrStatus_Ok);
    CHECK(moved[0] == 15);
    CHECK(moved[1] == 12);
    CHECK(moved[2] == 14);
    CHECK(moved[3] == 10);

    // The indices are remapped in place.
    PrMeshIndices out{indices, 6, PrMeshIndexType_U16};
    CHECK(prMeshIndicesRemap(view, remap, 6, out) == PrStatus_Ok);
    uint16_t remapped[6] = {0, 1, 2, 2, 1, 3};
    CHECK(prMemEquals(indices, remapped, sizeof(indices)));

    CHECK(prMeshFetchRemap(view, 4, remap, &count) == PrStatus_Ok);
    indices[1] = 4;
    CHECK(prMeshFetchRemap(view, 4, remap, &count) == PrStatus_OutOfRange);
}

TEST_CASE("applying a remap rejects indices it cannot place") {
    uint32_t indices[3] = {0, 1, 2};
    uint32_t remap[3] = {0, kPrMeshIndexNone, 5};
    uint32_t out[3];
    uint8_t vertices[3] = {1, 2, 3};
    uint8_t moved[2];
    PrMeshIndexView view{indices, 3, PrMeshIndexType_U32};
    // An index that the remap leaves out, an index past the vertices, and a new index past the output.
    CHECK(prMeshIndicesRemap(view, remap, 3, PrMeshIndices{out, 3, PrMeshIndexType_U32}) == PrStatus_OutOfRange);
    CHECK(prMeshIndicesRemap(view, remap, 2, PrMeshIndices{out, 3, PrMeshIndexType_U32}) == PrStatus_OutOfRange);
    CHECK(prMeshVerticesRemap(vertices, 3, 1, remap, moved, 2) == PrStatus_OutOfRange);
    CHECK(prMeshVerticesUnindex(vertices, 2, 1, view, moved) == PrStatus_OutOfRange);
}
