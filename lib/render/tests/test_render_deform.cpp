// Deforming instances: skinned and morphed meshes, whose vertices a pass of the frame writes. The cases read those
// vertices back and compare them with the same arithmetic on the CPU, and compare what a view draws of them with what
// it draws of a rigid mesh in the same place.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/render.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/span.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Arithmetic

PrVec3 row(const PrAffine3& t, uint32_t index) {
    return {t.m[index][0], t.m[index][1], t.m[index][2]};
}

// Returns a normal under a transform: the normal times the cofactors of the transform's linear part.
PrVec3 mapNormal(const PrAffine3& t, PrVec3 n) {
    return {
        prVec3Dot(prVec3Cross(row(t, 1), row(t, 2)), n), prVec3Dot(prVec3Cross(row(t, 2), row(t, 0)), n),
        prVec3Dot(prVec3Cross(row(t, 0), row(t, 1)), n),
    };
}

// Returns the transform that turns space by `angle` radians about the line through `pivot` along x.
PrAffine3 turnAboutX(float angle, PrVec3 pivot) {
    float c = prF32Cos(angle);
    float s = prF32Sin(angle);
    PrAffine3 t{{{1, 0, 0, 0}, {0, c, -s, 0}, {0, s, c, 0}}};
    PrVec3 moved = prAffine3MapPoint(&t, pivot);
    t.m[0][3] = pivot.x - moved.x;
    t.m[1][3] = pivot.y - moved.y;
    t.m[2][3] = pivot.z - moved.z;
    return t;
}

// ---------------------------------------------------------------------------------------------------------------------
// A bar

constexpr float kBarHalfWidth = 0.15f;
constexpr float kBarLength = 2;
constexpr uint32_t kBarRings = 9;
constexpr uint32_t kBarVertices = 4 * kBarRings * 2;
constexpr PrVec3 kBarElbow{0, 1, 0};

// A mesh with what deforms it, as a test gives it to a renderer and deforms it itself.
struct Shape {
    Array<PrRenderGeometryVertex> vertices;
    Array<PrVec2> uv;
    Array<uint32_t> indices;
    Array<PrRenderSkinVertex> skin;
    Array<PrRenderMorphDelta> morphs;
    uint32_t jointCount = 0;
    uint32_t morphTargetCount = 0;
};

// Returns a bar of square section that stands on the origin and reaches kBarLength up y, in rings a quarter apart. Each
// of its four sides has vertices of its own, with the side's normal. With `skinned`, the bar has two joints: the first
// moves it below three quarters of the way to its elbow, the second above as far past the elbow, and both between. With
// `morphed`, it has two morph targets: one leans it along x by its height, and one widens it along z, and both change
// its normals.
Shape createBar(bool skinned, bool morphed) {
    Shape shape;
    for (uint32_t side = 0; side < 4; ++side) {
        const PrVec3 normals[4] = {{1, 0, 0}, {0, 0, 1}, {-1, 0, 0}, {0, 0, -1}};
        PrVec3 n = normals[side];
        PrVec3 along{-n.z, 0, n.x};
        uint32_t first = shape.vertices.count();
        for (uint32_t ring = 0; ring < kBarRings; ++ring) {
            for (uint32_t corner = 0; corner < 2; ++corner) {
                float y = kBarLength * float(ring) / float(kBarRings - 1);
                PrVec3 position = (n + along * (corner == 0 ? -1.0f : 1.0f)) * kBarHalfWidth + PrVec3{0, y, 0};
                shape.vertices.push(prRenderGeometryVertexPack(position, n, along, 1));
                shape.uv.push(PrVec2{float(corner), y});
                if (skinned) {
                    const uint16_t joints[4] = {0, 1, 0, 0};
                    float upper = prF32Clamp((y - 0.75f) / 0.5f, 0, 1);
                    const float weights[4] = {1 - upper, upper, 0, 0};
                    shape.skin.push(prRenderSkinVertexPack(joints, weights));
                }
                if (morphed) {
                    shape.morphs.push(PrRenderMorphDelta{{0.5f * y, 0, 0}, {0, -0.4f * n.x, 0}, {0, 0, 0}});
                    shape.morphs.push(PrRenderMorphDelta{{0, 0, position.z}, {0.3f * n.z, 0, 0}, {0, 0.2f, 0}});
                }
            }
        }
        for (uint32_t ring = 0; ring + 1 < kBarRings; ++ring) {
            uint32_t a = first + ring * 2;
            const uint32_t corners[6] = {a, a + 3, a + 1, a, a + 2, a + 3};
            for (uint32_t corner : corners)
                shape.indices.push(corner);
        }
    }
    shape.jointCount = skinned ? 2 : 0;
    shape.morphTargetCount = morphed ? 2 : 0;
    return shape;
}

PrRenderMesh createMesh(Fixture& f, const Shape& shape) {
    PrRenderMeshPart part{.firstIndex = 0, .indexCount = shape.indices.count()};
    const PrRenderAttributeSource sources[] = {
        {PrRenderSemantic_Uv, 0, {}, {shape.uv.begin(), shape.uv.count(), sizeof(PrVec2), prMeshFormatFloat32(2)}},
    };
    Array<Array<uint8_t>> bytes;
    Array<PrRenderMeshStream> streams;
    fillStreams(f, sources, 1, shape.vertices.count(), &bytes, &streams);
    PrRenderMeshData data{
        .geometry = shape.vertices.begin(),
        .vertexCount = shape.vertices.count(),
        .streams = streams.begin(),
        .streamCount = streams.count(),
        .indices = shape.indices.begin(),
        .indexCount = shape.indices.count(),
        .parts = &part,
        .partCount = 1,
        .skin = shape.skin.isEmpty() ? nullptr : shape.skin.begin(),
        .jointCount = shape.jointCount,
        .morphs = shape.morphs.isEmpty() ? nullptr : shape.morphs.begin(),
        .morphTargetCount = shape.morphTargetCount,
    };
    PrRenderMesh mesh = prRenderMeshCreate(f.render);
    REQUIRE(prRenderMeshSetData(f.render, mesh, &data) == PrStatus_Ok);
    return mesh;
}

PrVec3 positionOf(const PrRenderGeometryVertex& vertex) {
    return {vertex.position[0], vertex.position[1], vertex.position[2]};
}

PrVec3 normalOf(const PrRenderGeometryVertex& vertex) {
    return PrVec3{float(vertex.normal[0]), float(vertex.normal[1]), float(vertex.normal[2])} / 32767.0f;
}

// Where a pose puts a shape's vertices, and their unit normals.
struct Posed {
    Array<PrVec3> positions;
    Array<PrVec3> normals;
};

// Deforms a shape on the CPU as the renderer's pass deforms it: the morph targets times their weights are added to each
// vertex, and the vertex is then moved by the sum of its joints' matrices, each times its weight.
Posed pose(const Shape& shape, Span<const PrAffine3> joints, Span<const float> weights) {
    Posed posed;
    for (uint32_t i = 0; i < shape.vertices.count(); ++i) {
        PrVec3 p = positionOf(shape.vertices[i]);
        PrVec3 n = normalOf(shape.vertices[i]);
        for (uint32_t t = 0; t < shape.morphTargetCount && t < weights.count(); ++t) {
            const PrRenderMorphDelta& delta = shape.morphs[i * shape.morphTargetCount + t];
            p = p + PrVec3{delta.position[0], delta.position[1], delta.position[2]} * weights[t];
            n = n + PrVec3{delta.normal[0], delta.normal[1], delta.normal[2]} * weights[t];
        }
        if (shape.jointCount != 0) {
            PrAffine3 sum{};
            for (uint32_t k = 0; k < kPrRenderSkinInfluences; ++k) {
                float weight = float(shape.skin[i].weights[k]) / 65535.0f;
                uint32_t index = shape.skin[i].joints[k];
                PrAffine3 joint = index < joints.count() ? joints[index] : prAffine3Identity();
                for (uint32_t r = 0; r < 3; ++r) {
                    for (uint32_t c = 0; c < 4; ++c)
                        sum.m[r][c] += weight * joint.m[r][c];
                }
            }
            p = prAffine3MapPoint(&sum, p);
            n = mapNormal(sum, n);
        }
        posed.positions.push(p);
        posed.normals.push(n / prVec3Length(n));
    }
    return posed;
}

// ---------------------------------------------------------------------------------------------------------------------
// Reading deformed vertices back

// What a frame's deformation left for one instance.
struct Deformed {
    PrRenderInstancePart part{};
    Array<PrRenderGeometryVertex> vertices;
    Array<PrVec3> previous;
    Image image;
};

// A copy of a range of a buffer of the frame's graph into memory that the test reads.
struct Copy {
    PrGpuGraph* graph;
    PrGpuGraphBuffer source;
    uint64_t offset;
    uint64_t size;
    PrGpuBuffer target;
};

PrGpuBuffer addCopy(Fixture& f, PrGpuGraphBuffer source, uint64_t offset, uint64_t size) {
    PrGpuBufferDesc desc{.size = size, .memory = PrGpuMemory_Readback};
    PrGpuBuffer target{};
    REQUIRE(prGpuBufferCreate(f.device, &desc, &target) == PrStatus_Ok);
    Copy* copy = static_cast<Copy*>(prGpuGraphAlloc(f.graph, sizeof(Copy)));
    *copy = Copy{f.graph, source, offset, size, target};
    PrGpuGraphPass pass = prGpuGraphAddWork(f.graph, "copy", [](void* PR_NULLABLE context, PrGpuCommands* commands) {
        const Copy& c = *static_cast<const Copy*>(context);
        prGpuCommandsCopyBuffer(commands, prGpuGraphBuffer(c.graph, c.source), c.offset, c.target, 0, c.size);
    }, copy);
    prGpuGraphUseBuffer(f.graph, pass, source, PrGpuAccessFlag_CopyRead);
    return target;
}

// Renders a view of a scene, and returns the view's color with the vertices that the frame holds for one of the scene's
// deforming instances, and where they were a frame before.
Deformed render(Fixture& f, const PrRenderViewDesc& view, PrRenderInstance instance, uint32_t vertexCount) {
    Deformed out;
    prGpuGraphBegin(f.graph);
    prRenderFrameBegin(f.render, f.graph);
    REQUIRE(prRenderSceneInstancePart(f.render, view.scene, instance, 0, &out.part));
    REQUIRE(out.part.deforms);
    f.addRead(prRenderViewAdd(f.render, &view).color, &out.image);
    PrRenderDeformedBuffers buffers = prRenderSceneDeformedBuffers(f.render, view.scene);
    REQUIRE(buffers.vertices.id != 0);
    REQUIRE(buffers.previousPositions.id != 0);
    uint64_t first = out.part.firstVertex;
    uint64_t stride = sizeof(PrRenderGeometryVertex);
    PrGpuBuffer vertices = addCopy(f, buffers.vertices, first * stride, uint64_t(vertexCount) * stride);
    PrGpuBuffer previous =
        addCopy(f, buffers.previousPositions, first * sizeof(PrVec3), uint64_t(vertexCount) * sizeof(PrVec3));
    f.record();
    out.vertices.resize(vertexCount);
    out.previous.resize(vertexCount);
    prMemCopy(out.vertices.begin(), prGpuBufferMap(f.device, vertices), size_t(vertexCount * stride));
    prMemCopy(out.previous.begin(), prGpuBufferMap(f.device, previous), size_t(vertexCount) * sizeof(PrVec3));
    prGpuBufferRelease(f.device, vertices);
    prGpuBufferRelease(f.device, previous);
    return out;
}

// Returns the farthest that a deformed vertex's position is from where the CPU puts it.
float positionError(const Deformed& deformed, const Posed& expected) {
    float largest = 0;
    for (uint32_t i = 0; i < expected.positions.count(); ++i)
        largest = prF32Max(largest, prVec3Length(positionOf(deformed.vertices[i]) - expected.positions[i]));
    return largest;
}

// Returns the farthest that a deformed vertex's normal is from the CPU's.
float normalError(const Deformed& deformed, const Posed& expected) {
    float largest = 0;
    for (uint32_t i = 0; i < expected.normals.count(); ++i)
        largest = prF32Max(largest, prVec3Length(normalOf(deformed.vertices[i]) - expected.normals[i]));
    return largest;
}

// Returns the farthest that a previous position is from `positions`.
float previousError(const Deformed& deformed, const Array<PrVec3>& positions) {
    float largest = 0;
    for (uint32_t i = 0; i < positions.count(); ++i)
        largest = prF32Max(largest, prVec3Length(deformed.previous[i] - positions[i]));
    return largest;
}

// Returns the farthest that a posed vertex, under `world`, lies outside the sphere that bounds its instance's part, or
// a negative number if the sphere holds them all.
float outsideBounds(const PrRenderInstancePart& part, const Posed& posed, const PrAffine3& world) {
    float farthest = -part.bounds.radius;
    for (PrVec3 p : posed.positions)
        farthest =
            prF32Max(farthest, prVec3Length(prAffine3MapPoint(&world, p) - part.bounds.center) - part.bounds.radius);
    return farthest;
}

// The errors that the cases allow. A position is a few operations on floats of magnitude 2. A normal is stored in 16
// bits, which is 3e-5 a component, after the same operations.
constexpr float kPositionTolerance = 2e-5f;
constexpr float kNormalTolerance = 2e-4f;

// A scene with one instance of a bar that stands on the bottom of the front view, under a light from the front and
// above.
struct BarScene {
    PrRenderScene scene;
    PrRenderMesh mesh;
    PrRenderMaterial material;
    PrRenderInstance instance;
    PrAffine3 world;
    PrRenderViewDesc view;
};

BarScene createBarScene(Fixture& f, const Shape& shape) {
    BarScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.mesh = createMesh(f, shape);
    PrRenderKhronosPbrMaterial white = prRenderKhronosPbrMaterialDefault();
    white.metallic = 0;
    s.material = createGltf(f, white);
    s.world = placed({0, -1, 0});
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    s.instance = addInstance(update, s.mesh, s.material, s.world);
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = {0, -1, -1};
    sun.intensity = 3;
    prRenderLightAdd(update, &sun);
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    return s;
}

void setJoints(Fixture& f, PrRenderScene scene, PrRenderInstance instance, Span<const PrAffine3> joints) {
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderInstanceSetJoints(update, instance, joints.data(), joints.count());
    prRenderUpdateCommit(update);
}

void setWeights(Fixture& f, PrRenderScene scene, PrRenderInstance instance, Span<const float> weights) {
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderInstanceSetMorphWeights(update, instance, weights.data(), weights.count());
    prRenderUpdateCommit(update);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render deformation" * doctest::skip(!gpuTests()));

TEST_CASE("a bent bar's vertices are where its joints put them, inside its bounds, with where they were before") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(true, false);
        BarScene s = createBarScene(f, bar);
        // The upper joint turns about the elbow and moves aside.
        PrAffine3 bent[2] = {prAffine3Identity(), turnAboutX(0.9f, kBarElbow)};
        bent[1].m[0][3] = 0.6f;
        setJoints(f, s.scene, s.instance, bent);
        Posed expected = pose(bar, bent, Span<const float>());
        // The bend takes the bar's end well away from where it was bound.
        CHECK(prVec3Length(expected.positions[kBarVertices - 1] - positionOf(bar.vertices[kBarVertices - 1])) > 0.5f);

        Deformed first = render(f, s.view, s.instance, kBarVertices);
        CHECK(positionError(first, expected) < kPositionTolerance);
        CHECK(normalError(first, expected) < kNormalTolerance);
        // Vertices that were never deformed before were nowhere else.
        CHECK(previousError(first, expected.positions) < kPositionTolerance);
        // Every vertex lies in the sphere that the instance is culled by, which is not the sphere of the bar as it was
        // bound: that one does not hold the bent end.
        CHECK(outsideBounds(first.part, expected, s.world) < 0);
        PrRenderInstancePart bound{.bounds = {{0, 0, 0}, prVec3Length(PrVec3{kBarHalfWidth, 1, kBarHalfWidth})}};
        CHECK(outsideBounds(bound, pose(bar, Span<const PrAffine3>(), Span<const float>()), s.world) < 1e-4f);
        CHECK(outsideBounds(bound, expected, s.world) > 0.05f);
        CHECK(first.part.bounds.radius < 2 * bound.bounds.radius);

        // A new pose: the vertices move, and their previous positions are where the pose before put them.
        const PrAffine3 more[2] = {turnAboutX(-0.2f, {0, 0, 0}), turnAboutX(-1.3f, kBarElbow)};
        setJoints(f, s.scene, s.instance, more);
        Posed moved = pose(bar, more, Span<const float>());
        Deformed second = render(f, s.view, s.instance, kBarVertices);
        CHECK(positionError(second, moved) < kPositionTolerance);
        CHECK(normalError(second, moved) < kNormalTolerance);
        CHECK(previousError(second, expected.positions) < kPositionTolerance);
        CHECK(outsideBounds(second.part, moved, s.world) < 0);

        // A frame in which the pose does not change: the vertices stay, and were where they are.
        Deformed third = render(f, s.view, s.instance, kBarVertices);
        CHECK(positionError(third, moved) < kPositionTolerance);
        CHECK(previousError(third, moved.positions) < kPositionTolerance);
        // After that, nothing is deformed until the pose changes: the frame is the view's cull, its depth and its
        // surfaces, and the three reads.
        Deformed fourth = render(f, s.view, s.instance, kBarVertices);
        CHECK(f.stats.passes == 6);
        CHECK(positionError(fourth, moved) < kPositionTolerance);
    });
}

TEST_CASE("joints at the identity leave a skinned mesh as it was bound") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(true, false);
        BarScene s = createBarScene(f, bar);
        // An instance that no update gave a pose draws the mesh's own vertices.
        prGpuGraphBegin(f.graph);
        prRenderFrameBegin(f.render, f.graph);
        PrRenderInstancePart part{};
        REQUIRE(prRenderSceneInstancePart(f.render, s.scene, s.instance, 0, &part));
        CHECK_FALSE(part.deforms);
        CHECK(prRenderSceneDeformedBuffers(f.render, s.scene).vertices.id == 0);
        Image rigid;
        f.addRead(prRenderViewAdd(f.render, &s.view).color, &rigid);
        f.record();

        const PrAffine3 rest[2] = {prAffine3Identity(), prAffine3Identity()};
        setJoints(f, s.scene, s.instance, rest);
        Deformed deformed = render(f, s.view, s.instance, kBarVertices);
        float farthest = 0;
        int steps = 0;
        for (uint32_t i = 0; i < kBarVertices; ++i) {
            farthest = prF32Max(farthest, prVec3Length(positionOf(deformed.vertices[i]) - positionOf(bar.vertices[i])));
            for (uint32_t k = 0; k < 4; ++k) {
                int normal = int(deformed.vertices[i].normal[k]) - int(bar.vertices[i].normal[k]);
                int tangent = int(deformed.vertices[i].tangent[k]) - int(bar.vertices[i].tangent[k]);
                steps = normal > steps ? normal : (-normal > steps ? -normal : steps);
                steps = tangent > steps ? tangent : (-tangent > steps ? -tangent : steps);
            }
        }
        // The weights of a vertex sum to one only as closely as floats add, so a position may move by the last bit of a
        // float, and a normal by the last bit of what stores it.
        CHECK(farthest < 1e-6f);
        CHECK(steps <= 1);
        // The view shows the same bar.
        Difference difference = compare(rigid, deformed.image, 1e-3f);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
        // A pose that names no joint leaves the others at the identity, too.
        setJoints(f, s.scene, s.instance, Span<const PrAffine3>());
        Deformed empty = render(f, s.view, s.instance, kBarVertices);
        CHECK(compare(rigid, empty.image, 1e-3f).over == 0);
    });
}

TEST_CASE("a bent bar is lit by its turned normals, as a rigid bar turned the same way is") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(true, false);
        BarScene s = createBarScene(f, bar);
        // The upper joint turns the bar's upper half toward the light.
        PrAffine3 turn = turnAboutX(-0.5f, kBarElbow);
        const PrAffine3 bent[2] = {prAffine3Identity(), turn};
        setJoints(f, s.scene, s.instance, bent);

        // A second scene holds a bar that does not deform, turned whole as the upper joint turns.
        BarScene rigid = createBarScene(f, createBar(false, false));
        PrAffine3 whole{};
        for (uint32_t r = 0; r < 3; ++r) {
            for (uint32_t c = 0; c < 4; ++c)
                whole.m[r][c] = turn.m[r][c];
        }
        whole.m[1][3] += s.world.m[1][3];
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, rigid.scene);
        prRenderInstanceSetTransform(update, rigid.instance, &whole);
        prRenderUpdateCommit(update);
        BarScene straight = createBarScene(f, createBar(false, false));

        // A point on the front face of the upper half, and one on the lower half, which the upper joint does not move.
        PrVec3 upper = prAffine3MapPoint(&s.world, prAffine3MapPoint(&turn, PrVec3{0, 1.7f, kBarHalfWidth}));
        PrVec3 lower = prAffine3MapPoint(&s.world, PrVec3{0, 0.4f, kBarHalfWidth});
        PrUVec2 upperPixel = pixelOf(s.view, upper);
        PrUVec2 lowerPixel = pixelOf(s.view, lower);
        Array<PrRenderTechnique> techniques;
        techniques.push(PrRenderTechnique_Forward);
        if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
            techniques.push(PrRenderTechnique_Visibility);
        for (PrRenderTechnique technique : techniques) {
            CAPTURE(technique);
            s.view.technique = technique;
            rigid.view.technique = technique;
            straight.view.technique = technique;
            Image skinned = render(f, s.view, s.instance, kBarVertices).image;
            Image turned = f.view(rigid.view);
            Image unbent = f.view(straight.view);
            float upperLight = skinned.at(upperPixel.x, upperPixel.y)[0];
            float lowerLight = skinned.at(lowerPixel.x, lowerPixel.y)[0];
            CHECK(upperLight == doctest::Approx(double(turned.at(upperPixel.x, upperPixel.y)[0])).epsilon(0.005));
            CHECK(lowerLight == doctest::Approx(double(unbent.at(lowerPixel.x, lowerPixel.y)[0])).epsilon(0.005));
            // The turn brings the upper half's face nearer to facing the light, so it is lit more.
            CHECK(lowerLight > 0.1f);
            CHECK(upperLight > lowerLight * 1.2f);
        }
    });
}

TEST_CASE("the instances of one skinned mesh each hold their own pose") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(true, false);
        BarScene s = createBarScene(f, bar);
        const float angles[3] = {0.4f, -0.7f, 1.1f};
        PrRenderInstance instances[3] = {s.instance, {}, {}};
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        instances[1] = addInstance(update, s.mesh, s.material, placed({-0.6f, -1, 0}));
        instances[2] = addInstance(update, s.mesh, s.material, placed({0.6f, -1, 0}));
        for (uint32_t i = 0; i < 3; ++i) {
            const PrAffine3 joints[2] = {prAffine3Identity(), turnAboutX(angles[i], kBarElbow)};
            prRenderInstanceSetJoints(update, instances[i], joints, 2);
        }
        prRenderUpdateCommit(update);
        uint32_t firsts[3];
        for (uint32_t i = 0; i < 3; ++i) {
            CAPTURE(i);
            const PrAffine3 joints[2] = {prAffine3Identity(), turnAboutX(angles[i], kBarElbow)};
            Deformed deformed = render(f, s.view, instances[i], kBarVertices);
            CHECK(positionError(deformed, pose(bar, joints, Span<const float>())) < kPositionTolerance);
            firsts[i] = deformed.part.firstVertex;
        }
        // Each instance has vertices of its own.
        CHECK(firsts[0] != firsts[1]);
        CHECK(firsts[1] != firsts[2]);
        CHECK(firsts[0] != firsts[2]);

        // An instance that is removed gives its vertices to the next that deforms, and the others keep theirs.
        update = prRenderUpdateBegin(f.render, s.scene);
        prRenderInstanceRemove(update, instances[1]);
        PrRenderInstance added = addInstance(update, s.mesh, s.material, placed({0, -1, 0.5f}));
        const PrAffine3 joints[2] = {prAffine3Identity(), turnAboutX(0.2f, kBarElbow)};
        prRenderInstanceSetJoints(update, added, joints, 2);
        prRenderUpdateCommit(update);
        Deformed again = render(f, s.view, added, kBarVertices);
        CHECK(again.part.firstVertex == firsts[1]);
        CHECK(positionError(again, pose(bar, joints, Span<const float>())) < kPositionTolerance);
        const PrAffine3 last[2] = {prAffine3Identity(), turnAboutX(angles[2], kBarElbow)};
        Deformed kept = render(f, s.view, instances[2], kBarVertices);
        CHECK(kept.part.firstVertex == firsts[2]);
        CHECK(positionError(kept, pose(bar, last, Span<const float>())) < kPositionTolerance);

        // A mesh whose data is replaced is deformed again from the new data, and one that is destroyed is not drawn.
        Shape shifted = createBar(true, false);
        for (PrRenderGeometryVertex& vertex : shifted.vertices)
            vertex.position[0] += 0.25f;
        PrRenderMeshPart part{.firstIndex = 0, .indexCount = shifted.indices.count()};
        PrRenderMeshData data{
            .geometry = shifted.vertices.begin(),
            .vertexCount = shifted.vertices.count(),
            .indices = shifted.indices.begin(),
            .indexCount = shifted.indices.count(),
            .parts = &part,
            .partCount = 1,
            .skin = shifted.skin.begin(),
            .jointCount = 2,
        };
        REQUIRE(prRenderMeshSetData(f.render, s.mesh, &data) == PrStatus_Ok);
        Deformed replaced = render(f, s.view, instances[2], kBarVertices);
        CHECK(positionError(replaced, pose(shifted, last, Span<const float>())) < kPositionTolerance);
        prRenderMeshDestroy(f.render, s.mesh);
        prGpuGraphBegin(f.graph);
        prRenderFrameBegin(f.render, f.graph);
        PrRenderInstancePart none{};
        CHECK_FALSE(prRenderSceneInstancePart(f.render, s.scene, instances[2], 0, &none));
        Image empty;
        f.addRead(prRenderViewAdd(f.render, &s.view).color, &empty);
        f.record();
        CHECK(near(empty.at(kCenter, kCenter), {0, 0, 0}, 0));
    });
}

TEST_CASE("morph targets move a mesh by their weights: not at all at zero, wholly at one, and in part between") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(false, true);
        BarScene s = createBarScene(f, bar);
        const float cases[][2] = {{0, 0}, {1, 0}, {0, 1}, {0.35f, 0.6f}, {-0.5f, 1.5f}};
        for (const float* weights : cases) {
            CAPTURE(weights[0]);
            CAPTURE(weights[1]);
            setWeights(f, s.scene, s.instance, Span<const float>(weights, 2));
            Posed expected = pose(bar, Span<const PrAffine3>(), Span<const float>(weights, 2));
            Deformed deformed = render(f, s.view, s.instance, kBarVertices);
            CHECK(positionError(deformed, expected) < kPositionTolerance);
            CHECK(normalError(deformed, expected) < kNormalTolerance);
            CHECK(outsideBounds(deformed.part, expected, s.world) < 0);
        }
        // At weights of zero the vertices are the mesh's, and at a weight of one a vertex has moved by its whole delta.
        const float none[2] = {0, 0};
        const float whole[2] = {1, 0};
        Posed rest = pose(bar, Span<const PrAffine3>(), Span<const float>(none, 2));
        Posed leaned = pose(bar, Span<const PrAffine3>(), Span<const float>(whole, 2));
        CHECK(prVec3Length(rest.positions[kBarVertices - 1] - positionOf(bar.vertices[kBarVertices - 1])) == 0);
        CHECK(leaned.positions[kBarVertices - 1].x - rest.positions[kBarVertices - 1].x == doctest::Approx(1.0));
        // A weight for the first target alone leaves the second at zero.
        setWeights(f, s.scene, s.instance, Span<const float>(whole, 1));
        CHECK(positionError(render(f, s.view, s.instance, kBarVertices), leaned) < kPositionTolerance);
    });
}

TEST_CASE("a mesh with morph targets and a skin is morphed and then moved by its joints") {
    onEachDevice([](Fixture& f) {
        Shape bar = createBar(true, true);
        BarScene s = createBarScene(f, bar);
        const PrAffine3 joints[2] = {turnAboutX(0.3f, {0, 0, 0}), turnAboutX(-0.8f, kBarElbow)};
        const float weights[2] = {0.7f, 0.4f};
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        prRenderInstanceSetJoints(update, s.instance, joints, 2);
        prRenderInstanceSetMorphWeights(update, s.instance, weights, 2);
        prRenderUpdateCommit(update);
        Posed expected = pose(bar, joints, weights);
        Deformed deformed = render(f, s.view, s.instance, kBarVertices);
        CHECK(positionError(deformed, expected) < kPositionTolerance);
        CHECK(normalError(deformed, expected) < kNormalTolerance);
        CHECK(outsideBounds(deformed.part, expected, s.world) < 0);
        // The order matters: the joints applied first, and the targets after, put the bar's end elsewhere.
        Posed skinnedOnly = pose(bar, joints, Span<const float>());
        PrVec3 delta{
            bar.morphs[(kBarVertices - 1) * 2].position[0] * weights[0], 0,
            bar.morphs[(kBarVertices - 1) * 2 + 1].position[2] * weights[1],
        };
        CHECK(prVec3Length(skinnedOnly.positions[kBarVertices - 1] + delta - expected.positions[kBarVertices - 1]) > 0.02f);

        // Changing the weights alone deforms the mesh again, in the pose that it has.
        const float other[2] = {-0.2f, 1};
        setWeights(f, s.scene, s.instance, other);
        Deformed changed = render(f, s.view, s.instance, kBarVertices);
        CHECK(positionError(changed, pose(bar, joints, other)) < kPositionTolerance);
        CHECK(previousError(changed, expected.positions) < kPositionTolerance);
    });
}

TEST_SUITE_END();
