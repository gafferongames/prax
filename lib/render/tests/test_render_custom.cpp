// A shader set of the tests' own (test_render_materials.slang), built as an application builds one: material types and
// shading models that the library does not have, drawn by both techniques on both binding tiers, and compared with the
// formulas that their functions state.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/render.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_shaders.h"
#include "pr/render_view.h"

#include "render/tests/test_render_materials.h"
#include "render_shaders/TestRenderShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

// The tables of the tests' shader set. The ramp holds u in red, v in green, and a quarter plus half of their product in
// blue: a function that interpolation between four texels reproduces exactly. The flat table holds three quarters in
// every channel, or the value that its context points at, so a texel of another table that reached a sample of it would
// show.
void fillRamp(void* PR_NULLABLE context, PrUVec2 extent, float* texels) {
    (void)context;
    for (uint32_t y = 0; y < extent.y; ++y) {
        for (uint32_t x = 0; x < extent.x; ++x) {
            float u = float(x) / float(extent.x - 1);
            float v = float(y) / float(extent.y - 1);
            float* texel = texels + (y * extent.x + x) * 4;
            texel[0] = u;
            texel[1] = v;
            texel[2] = 0.25f + 0.5f * u * v;
            texel[3] = 1;
        }
    }
}

void fillFlat(void* PR_NULLABLE context, PrUVec2 extent, float* texels) {
    float value = context != nullptr ? *static_cast<const float*>(context) : 0.75f;
    for (uint32_t i = 0; i < extent.x * extent.y * 4; ++i)
        texels[i] = value;
}

// What fills the tables of the tests' shader set.
constexpr PrRenderTableSource kTestTables[] = {
    {.table = kTestRenderTableRamp, .fill = fillRamp},
    {.table = kTestRenderTableFlat, .fill = fillFlat},
};

PrVec3 rgb(PrRenderVec4 v) {
    return {v.x, v.y, v.z};
}

// The techniques that a device draws with.
Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

// Return the description of a material of each of the tests' types.
PrRenderMaterialDesc ringsDesc(const TestRingsParameters& parameters) {
    return prRenderMaterialDescDefault(kTestRenderMaterialRings, &parameters, sizeof(parameters));
}
PrRenderMaterialDesc toonDesc(const TestToonParameters& parameters) {
    return prRenderMaterialDescDefault(kTestRenderMaterialToonPaint, &parameters, sizeof(parameters));
}
PrRenderMaterialDesc feltDesc(const TestFeltParameters& parameters) {
    return prRenderMaterialDescDefault(kTestRenderMaterialFelt, &parameters, sizeof(parameters));
}

// Returns the part of a ring pattern's `high` color at texture coordinates and a time, as the material type computes
// it.
float ringsPart(const TestRingsParameters& p, float u, float v, float time) {
    return 0.5f + 0.5f * prF32Sin(p.frequencyU * u + p.rate * time) * prF32Cos(p.frequencyV * v);
}

PrRenderVec4 ringsColor(const TestRingsParameters& p, float u, float v, float time) {
    float part = ringsPart(p, u, v, time);
    return {
        p.low.x + (p.high.x - p.low.x) * part,
        p.low.y + (p.high.y - p.low.y) * part,
        p.low.z + (p.high.z - p.low.z) * part,
        p.low.w + (p.high.w - p.low.w) * part,
    };
}

// A pattern that emits its color and reflects nothing, so that a pixel holds the pattern's value at its center.
TestRingsParameters glowingRings() {
    return TestRingsParameters{
        .low = {0.1f, 0.2f, 0.9f, 1},
        .high = {1.5f, 0.6f, 0.05f, 1},
        .frequencyU = 7,
        .frequencyV = 4,
        .rate = 1.5f,
        .lit = 0,
    };
}

// A scene of one quad that fills the front view, whose first texture coordinates run from (0, 0) at the view's top left
// to (1, 1) at its bottom right.
struct QuadScene {
    PrRenderScene scene;
    PrRenderMesh quad;
    PrRenderMaterial material;
    PrRenderViewDesc view;
};

QuadScene createQuadScene(Fixture& f, PrRenderMaterial material) {
    QuadScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.quad = createQuad(f);
    s.material = material;
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    addInstance(update, s.quad, s.material, placed({0, 0, 0}));
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    return s;
}

void setTime(Fixture& f, PrRenderScene scene, float seconds) {
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderSceneTimeSet(update, seconds);
    prRenderUpdateCommit(update);
}

void addSun(Fixture& f, PrRenderScene scene, PrVec3 direction, float lux) {
    PrRenderLightDesc light = prRenderLightDescDefault();
    light.direction = direction;
    light.intensity = lux;
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderLightAdd(update, &light);
    prRenderUpdateCommit(update);
}

// The texture coordinates at the center of a pixel of the front view of a quad scene.
PrVec2 pixelUv(uint32_t x, uint32_t y) {
    return {(float(x) + 0.5f) / float(kExtent.x), (float(y) + 0.5f) / float(kExtent.y)};
}

// Returns the number of pixels of a front view of a quad scene that are not the ring pattern at time zero times `left`
// on the view's left half, and times `right` on its right half.
uint32_t wrongRings(const Image& image, const TestRingsParameters& rings, PrVec3 left, PrVec3 right) {
    uint32_t wrong = 0;
    for (uint32_t y = 0; y < kExtent.y; ++y) {
        for (uint32_t x = 0; x < kExtent.x; ++x) {
            if (x == kCenter)
                continue;
            PrVec2 uv = pixelUv(x, y);
            PrVec3 factor = x < kCenter ? left : right;
            PrVec3 color = rgb(ringsColor(rings, uv.x, uv.y, 0));
            PrVec3 expected{color.x * factor.x, color.y * factor.y, color.z * factor.z};
            if (!near(image.at(x, y), expected, 3e-3f))
                ++wrong;
        }
    }
    return wrong;
}

// ---------------------------------------------------------------------------------------------------------------------
// A scene that mixes the library's types with the tests'

constexpr PrUVec2 kMixedExtent{64, 64};

// Creates a scene that draws the library's two types beside the tests' three, under two shading models of the tests'
// and the library's own: a lit pattern with a texture on the ground, a masked pattern, toon and felt spheres, a blended
// toon quad, a metal and an unlit picture, under a sun with shadows, a point light and an environment.
PrRenderViewDesc createMixed(Fixture& f) {
    PrRenderScene scene = prRenderSceneCreate(f.render);
    PrRenderMesh quad = createQuad(f);
    PrRenderMesh sphere = createSphere(f, 24);

    Array<uint8_t> checker;
    for (uint32_t i = 0; i < 64; ++i) {
        bool light = ((i % 8) + (i / 8)) % 2 == 0;
        const uint8_t texel[4] = {uint8_t(light ? 240 : 90), uint8_t(light ? 220 : 120), uint8_t(light ? 200 : 60), 255};
        checker.append(texel, 4);
    }
    PrRenderTexture texture = createTexture(f, {8, 8}, checker.begin(), TextureEncoding_Srgb, PrGpuFilter_Linear);

    TestRingsParameters groundRings{
        .low = {0.15f, 0.3f, 0.2f, 1},
        .high = {0.9f, 0.85f, 0.6f, 1},
        .frequencyU = 11,
        .frequencyV = 9,
        .rate = 0.5f,
        .lit = 1,
    };
    PrRenderMaterialDesc ground = ringsDesc(groundRings);
    PrRenderTextureUse groundTexture = useOf(f, texture);
    groundTexture.transform = prRenderTextureTransform({0, 0}, 0.4f, {3, 3});
    ground.textures = &groundTexture;
    ground.textureCount = 1;
    TestRingsParameters cutRings = glowingRings();
    cutRings.low.w = 0;
    cutRings.lit = 0.5f;
    PrRenderMaterialDesc cut = ringsDesc(cutRings);
    cut.alphaMode = PrRenderAlphaMode_Mask;
    cut.flags |= PrRenderMaterialFlag_DoubleSided;
    TestToonParameters paint{.color = {0.9f, 0.3f, 0.2f, 1}, .highlight = {1, 1, 0.8f, 0.97f}, .bands = 3, .ambient = 0.15f};
    TestToonParameters tint{.color = {0.2f, 0.5f, 0.9f, 0.5f}, .highlight = {0, 0, 0, 2}, .bands = 2, .ambient = 0.3f};
    PrRenderMaterialDesc glass = toonDesc(tint);
    glass.alphaMode = PrRenderAlphaMode_Blend;
    TestFeltParameters felt{.color = {0.3f, 0.7f, 0.35f, 1}};
    PrRenderKhronosPbrMaterial metal = prRenderKhronosPbrMaterialDefault();
    metal.baseColor = {0.9f, 0.7f, 0.4f, 1};
    metal.roughness = 0.35f;
    PrRenderUnlitMaterial picture = prRenderUnlitMaterialDefault();
    picture.colorTexture = useOf(f, texture);

    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    addInstance(update, quad, createMaterial(f, ground), facingUp({0, -1, 0}, 4));
    addInstance(update, quad, createMaterial(f, cut), placed({-1.9f, 0, 0.2f}, {0.7f, 0.9f, 1}));
    addInstance(update, sphere, createMaterial(f, toonDesc(paint)), placed({-0.4f, -0.2f, 0}, {0.8f, 0.8f, 0.8f}));
    addInstance(update, sphere, createMaterial(f, feltDesc(felt)), placed({1.2f, -0.3f, -0.6f}, {0.7f, 0.7f, 0.7f}));
    addInstance(update, quad, createMaterial(f, glass), placed({0.5f, 0.1f, 1.4f}, {0.6f, 0.6f, 1}));
    addInstance(update, quad, createGltf(f, metal), placed({2.2f, 0.2f, 0.6f}, {0.6f, 0.7f, 1}));
    addInstance(update, quad, createMaterial(f, picture), placed({-1.2f, 1.3f, -1}, {0.6f, 0.5f, 1}));
    prRenderSceneTimeSet(update, 0.75f);

    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = {-0.4f, -1, -0.3f};
    sun.intensity = 3;
    sun.color = {1, 0.95f, 0.85f};
    sun.castsShadows = true;
    prRenderLightAdd(update, &sun);
    PrRenderLightDesc lamp = prRenderLightDescDefault();
    lamp.kind = PrRenderLightKind_Point;
    lamp.position = {1, 0.5f, 2.5f};
    lamp.color = {1, 0.6f, 0.3f};
    lamp.intensity = 6;
    lamp.range = 8;
    prRenderLightAdd(update, &lamp);
    prRenderUpdateCommit(update);

    PrRenderEnvironment environment = createEnvironment(f, cubeOf(16, [](PrVec3 d) {
        float height = d.y * 0.5f + 0.5f;
        return PrVec3{0.1f + 0.3f * height, 0.12f + 0.4f * height, 0.15f + 0.65f * height};
    }));
    setEnvironment(f, scene, environment);

    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = prRenderCameraLookAt({2.5f, 2.2f, 5.5f}, {0, 0, 0}, 0.9f, 1, 0.1f);
    view.extent = kMixedExtent;
    return view;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render custom" * doctest::skip(!gpuTests()));

TEST_CASE("a shader set lists the library's material types and then its own, with each one's record and textures") {
    const PrRenderShaderSet* set = testRenderShaderSet();
    CHECK(set->name == "TestRender");
    REQUIRE(set->materialTypeCount == kTestRenderMaterialCount);
    CHECK(kTestRenderMaterialKhronosPbr == prRenderMaterialTypeFromName(set, "KhronosPbr"));
    CHECK(kTestRenderMaterialUnlit == prRenderMaterialTypeFromName(set, "Unlit"));
    CHECK(kTestRenderMaterialHeat == prRenderMaterialTypeFromName(set, "Heat"));
    CHECK(kTestRenderMaterialCount == 10);

    const PrRenderMaterialType& rings = set->materialTypes[kTestRenderMaterialRings];
    CHECK(rings.name == "Rings");
    CHECK(rings.shadingModel == "RenderPbrSurface");
    CHECK(rings.parameterBytes == sizeof(TestRingsParameters));
    CHECK(rings.textureCount == 1);
    CHECK(rings.textures[kTestRenderRingsTexturePattern].standIn.x == 0.25f);
    CHECK(rings.textures[kTestRenderRingsTexturePattern].standIn.z == 0.75f);
    CHECK(rings.textures[kTestRenderRingsTexturePattern].name == "pattern");
    const PrRenderMaterialType& toon = set->materialTypes[kTestRenderMaterialToonPaint];
    CHECK(toon.shadingModel == "TestToonSurface");
    CHECK(toon.parameterBytes == sizeof(TestToonParameters));
    CHECK(toon.textureCount == 0);
    REQUIRE(toon.variantCount == 1);
    CHECK(set->variants[toon.firstVariant].forwardFragment == "forwardPixelToonPaint");
    CHECK(set->variants[set->materialTypes[kTestRenderMaterialKhronosPbr].firstVariant].forwardFragment ==
        "forwardPixelKhronosPbr");
    // The parameters of a type are its record's fields, by name, at the offsets that a shader loads them from.
    REQUIRE(rings.parameterCount == kTestRenderRingsParameterCount);
    const PrRenderMaterialParameter& lit = rings.parameters[kTestRenderRingsParameterLit];
    CHECK(lit.name == "lit");
    CHECK(lit.offset == offsetof(TestRingsParameters, lit));
    CHECK(lit.componentCount == 1);
    CHECK(rings.parameters[kTestRenderRingsParameterHigh].componentCount == 4);
    const PrRenderMaterialType& quilt = set->materialTypes[kTestRenderMaterialQuilt];
    const PrRenderMaterialParameter& weights = quilt.parameters[prRenderMaterialParameterFromName(&quilt, "weights")];
    CHECK(weights.elementCount == kTestQuiltTextures);
    CHECK(weights.offset == offsetof(TestQuiltParameters, weights));
    const PrRenderMaterialType& probe = set->materialTypes[kTestRenderMaterialProbe];
    CHECK(probe.parameters[prRenderMaterialParameterFromName(&probe, "shows")].kind == PrRenderParameterKind_Uint);
    CHECK(set->materialTypes[kTestRenderMaterialFelt].shadingModel == "TestWrapSurface");
    CHECK(set->materialTypes[kTestRenderMaterialFelt].parameterBytes == sizeof(TestFeltParameters));

    // Types whose declarations of attributes are equal share a layout: the library's Khronos types', its preview
    // type's, the types that read one set of coordinates, the types that read none, and the type of its own attributes.
    auto layoutOf = [set](uint32_t type) { return prRenderMaterialTypeLayout(set, type, 0); };
    CHECK(set->layoutCount == 5);
    CHECK(layoutOf(kTestRenderMaterialRings) == layoutOf(kTestRenderMaterialQuilt));
    CHECK(layoutOf(kTestRenderMaterialRings) == layoutOf(kTestRenderMaterialProbe));
    CHECK(layoutOf(kTestRenderMaterialToonPaint) == layoutOf(kTestRenderMaterialFelt));
    CHECK(layoutOf(kTestRenderMaterialRings) != layoutOf(kTestRenderMaterialKhronosPbr));
    CHECK(layoutOf(kTestRenderMaterialToonPaint)->attributeCount == 0);
    const PrRenderVertexLayout& heat = *layoutOf(kTestRenderMaterialHeat);
    REQUIRE(heat.attributeCount == 2);
    CHECK(heat.attributes[kTestRenderHeatAttributeTemperature].name == "temperature");
    CHECK(heat.attributes[kTestRenderHeatAttributeBand].semantic == PrRenderSemantic_Custom);
    CHECK(heat.attributes[kTestRenderHeatAttributeBand].format.component == PrMeshComponent_Uint32);
    CHECK(heat.attributes[kTestRenderHeatAttributeBand].fallback.x == 3);
    CHECK(heat.stride == 8);

    // Every pass has a library for a device with bindless, and all but the one that needs bindless for the slots.
    for (uint32_t pass = 0; pass < kPrRenderShaderPassCount; ++pass) {
        CHECK(set->handles[pass].size != 0);
        CHECK((set->slots[pass].size != 0) == (pass != PrRenderShaderPass_Visibility));
    }
    onEachDevice([](Fixture& f) { CHECK(prRenderShaderSet(f.render) == testRenderShaderSet()); }, testRenderShaderSet(),
        kTestTables);
    onEachDevice([](Fixture& f) { CHECK(prRenderShaderSet(f.render) == prRenderShaderSetLibrary()); });
}

TEST_CASE("a custom type reads attributes that only it knows, converted from the producer's own formats") {
    onEachDevice([](Fixture& f) {
        // The quad's corners, as createQuad lays them out: three columns of two rows, with u along x.
        Array<PrRenderGeometryVertex> geometry;
        double temperatures[6];
        int64_t bands[6];
        for (uint32_t row = 0; row < 2; ++row) {
            for (uint32_t column = 0; column < 3; ++column) {
                geometry.push(prRenderGeometryVertexPack({float(column) - 1, 1 - float(row) * 2, 0}, {0, 0, 1}, {1, 0, 0}, 1));
                temperatures[row * 3 + column] = double(column) * 0.5;
                bands[row * 3 + column] = 2;
            }
        }
        const uint32_t indices[12] = {0, 3, 4, 0, 4, 1, 1, 4, 5, 1, 5, 2};
        const PrRenderMeshPart part{.firstIndex = 0, .indexCount = 12};

        // The producer knows the attributes by name and holds them in formats of its own: doubles and 64-bit integers,
        // which the layout's fill converts to the float and the 32-bit integer that the type declares.
        const PrRenderShaderSet* set = prRenderShaderSet(f.render);
        const PrRenderVertexLayout* layout = prRenderMaterialTypeLayout(set, kTestRenderMaterialHeat, 0);
        const PrRenderAttributeSource sources[] = {
            {
                PrRenderSemantic_Custom, 0, "temperature",
                {temperatures, 6, sizeof(double), {.component = PrMeshComponent_Float64, .count = 1}},
            },
            {PrRenderSemantic_Custom, 0, "band", {bands, 6, sizeof(int64_t), {.component = PrMeshComponent_Int64, .count = 1}}},
        };
        Array<uint8_t> bytes;
        bytes.resize(layout->stride * 6);
        uint32_t missing = 1;
        REQUIRE(prRenderVertexLayoutFill(layout, sources, 2, 6, bytes.begin(), &missing) == PrStatus_Ok);
        CHECK(missing == 0);
        const PrRenderMeshStream stream{layout, bytes.begin()};
        PrRenderMesh heated = prRenderMeshCreate(f.render);
        PrRenderMeshData data{
            .geometry = geometry.begin(),
            .vertexCount = 6,
            .streams = &stream,
            .streamCount = 1,
            .indices = indices,
            .indexCount = 12,
            .parts = &part,
            .partCount = 1,
        };
        REQUIRE(prRenderMeshSetData(f.render, heated, &data) == PrStatus_Ok);
        // A mesh without the layout's stream, whose draws read each attribute's fallback.
        PrRenderMesh plain = prRenderMeshCreate(f.render);
        data.streams = nullptr;
        data.streamCount = 0;
        REQUIRE(prRenderMeshSetData(f.render, plain, &data) == PrStatus_Ok);

        const TestHeatParameters parameters{.scale = {1, 4, 0, 0}};
        PrRenderMaterial material =
            createMaterial(f, prRenderMaterialDescDefault(kTestRenderMaterialHeat, &parameters, sizeof(parameters)));
        const PrRenderMesh meshes[2] = {heated, plain};
        for (PrRenderMesh mesh : meshes) {
            bool converted = mesh.id == heated.id;
            CAPTURE(converted);
            PrRenderScene scene = prRenderSceneCreate(f.render);
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            addInstance(update, mesh, material, placed({0, 0, 0}));
            prRenderUpdateCommit(update);
            PrRenderViewDesc view = frontView(scene);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                view.technique = technique;
                Image image = f.view(view);
                uint32_t wrong = 0;
                for (uint32_t y = 0; y < kExtent.y; ++y) {
                    for (uint32_t x = 0; x < kExtent.x; ++x) {
                        PrVec3 expected = converted ? PrVec3{pixelUv(x, y).x, 0.5f, 0} : PrVec3{0.5f, 0.75f, 0};
                        if (!near(image.at(x, y), expected, 2e-3f))
                            ++wrong;
                    }
                }
                CHECK(wrong == 0);
            }
            CHECK(prRenderSceneFallbackDrawCount(f.render, scene) == (converted ? 0u : 1u));
            prRenderSceneDestroy(f.render, scene);
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a custom material type draws the pattern that its formula gives, from its parameters and the scene's time") {
    onEachDevice([](Fixture& f) {
        TestRingsParameters rings = glowingRings();
        QuadScene s = createQuadScene(f, createMaterial(f, ringsDesc(rings)));
        // A light changes nothing of a surface that only emits.
        addSun(f, s.scene, {0, 0, -1}, 5);
        const float times[] = {0, 0.9f, 2.6f};
        for (float time : times) {
            CAPTURE(time);
            setTime(f, s.scene, time);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                uint32_t wrong = 0;
                float lowest = 1;
                float highest = 0;
                for (uint32_t y = 0; y < kExtent.y; ++y) {
                    for (uint32_t x = 0; x < kExtent.x; ++x) {
                        PrVec2 uv = pixelUv(x, y);
                        float part = ringsPart(rings, uv.x, uv.y, time);
                        lowest = prF32Min(lowest, part);
                        highest = prF32Max(highest, part);
                        if (!near(image.at(x, y), rgb(ringsColor(rings, uv.x, uv.y, time)), 2e-3f))
                            ++wrong;
                    }
                }
                CHECK(wrong == 0);
                // The view shows the pattern from one of its colors to the other.
                CHECK(lowest < 0.05f);
                CHECK(highest > 0.95f);
            }
        }

        // The parameters are the material's own: another material of the type draws another pattern.
        rings.frequencyU = 3;
        rings.high = {0.2f, 2, 0.4f, 1};
        PrRenderMaterialDesc desc = ringsDesc(rings);
        prRenderMaterialSet(f.render, s.material, &desc);
        s.view.technique = PrRenderTechnique_Forward;
        Image image = f.view(s.view);
        PrVec2 uv = pixelUv(5, 20);
        CHECK(near(image.at(5, 20), rgb(ringsColor(rings, uv.x, uv.y, 2.6f)), 2e-3f));
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a custom material type's alpha cuts its surface out in every pass, by its one function") {
    onEachDevice([](Fixture& f) {
        // The pattern's alpha is the part of its high color, so the mask keeps the surface where that part is at least
        // the cutoff.
        TestRingsParameters rings = glowingRings();
        rings.low.w = 0;
        PrRenderMaterialDesc desc = ringsDesc(rings);
        desc.alphaMode = PrRenderAlphaMode_Mask;
        desc.alphaCutoff = 0.4f;
        QuadScene s = createQuadScene(f, createMaterial(f, desc));
        const PrVec3 back{0, 0.5f, 0.25f};
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        addInstance(update, s.quad, createUnlit(f, {back.x, back.y, back.z, 1}), placed({0, 0, -1}, {2, 2, 1}));
        prRenderUpdateCommit(update);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image image = f.view(s.view);
            uint32_t wrong = 0;
            uint32_t kept = 0;
            uint32_t cut = 0;
            for (uint32_t y = 0; y < kExtent.y; ++y) {
                for (uint32_t x = 0; x < kExtent.x; ++x) {
                    PrVec2 uv = pixelUv(x, y);
                    float part = ringsPart(rings, uv.x, uv.y, 0);
                    // A pixel whose alpha is at the cutoff may fall to either side.
                    if (prF32Abs(part - desc.alphaCutoff) < 0.01f)
                        continue;
                    bool keeps = part >= desc.alphaCutoff;
                    ++(keeps ? kept : cut);
                    // A kept fragment is opaque, and shows the pattern's color.
                    PrVec3 expected = keeps ? rgb(ringsColor(rings, uv.x, uv.y, 0)) : back;
                    if (!near(image.at(x, y), expected, 2e-3f))
                        ++wrong;
                }
            }
            CHECK(wrong == 0);
            CHECK(kept > 200);
            CHECK(cut > 200);
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a custom material type samples its texture on both tiers, and reads its own stand-in until texels arrive") {
    onEachDevice([](Fixture& f) {
        TestRingsParameters rings = glowingRings();
        // Two texels across: the left is (1, 0.5, 0.25), and the right (0.25, 1, 0.5).
        const float texels[8] = {1, 0.5f, 0.25f, 1, 0.25f, 1, 0.5f, 1};
        PrRenderTexture texture = prRenderTextureCreate(f.render);
        f.samplers.set(texture.id, prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Repeat));
        PrRenderMaterialDesc desc = ringsDesc(rings);
        PrRenderTextureUse use = useOf(f, texture);
        desc.textures = &use;
        desc.textureCount = 1;
        QuadScene s = createQuadScene(f, createMaterial(f, desc));

        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            // The texture has no texels yet, so the type's function reads the value that the type declares.
            Image pending = f.view(s.view);
            CHECK(wrongRings(pending, rings, {0.25f, 0.5f, 0.75f}, {0.25f, 0.5f, 0.75f}) == 0);
        }
        PrRenderTextureData data{.format = PrGpuFormat_Rgba32F, .extent = {2, 1}, .levelCount = 1, .levels = {{texels, 0}}};
        REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image arrived = f.view(s.view);
            CHECK(wrongRings(arrived, rings, {1, 0.5f, 0.25f}, {0.25f, 1, 0.5f}) == 0);
        }
        // A material that names no texture multiplies by white.
        desc.textureCount = 0;
        prRenderMaterialSet(f.render, s.material, &desc);
        s.view.technique = PrRenderTechnique_Forward;
        CHECK(wrongRings(f.view(s.view), rings, {1, 1, 1}, {1, 1, 1}) == 0);
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a toon shading model lights a sphere in bands that are each flat, under a highlight with a hard edge") {
    onEachDevice([](Fixture& f) {
        constexpr PrUVec2 kSphereExtent{97, 97};
        constexpr float kRadius = 0.8f;
        constexpr float kLux = 2;
        TestToonParameters paint{
            .color = {0.8f, 0.4f, 0.2f, 1},
            .highlight = {0.5f, 0.75f, 1, 0.985f},
            .bands = 4,
            .ambient = 0.125f,
        };
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh sphere = createSphere(f, 96);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, sphere, createMaterial(f, toonDesc(paint)), placed({0, 0, 0}, {kRadius, kRadius, kRadius}));
        prRenderUpdateCommit(update);
        // The light comes from the side, so that the view shows the sphere's dark side too.
        PrVec3 toLight = prVec3Normalize({0.8f, 0.3f, 0.25f});
        addSun(f, scene, toLight * -1, kLux);
        PrRenderViewDesc view = frontView(scene);
        view.extent = kSphereExtent;

        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            Image image = f.view(view);
            // By band, the least and the greatest red of its pixels outside the highlight, and the number of them.
            constexpr uint32_t kBands = 4;
            float least[kBands + 1];
            float greatest[kBands + 1];
            uint32_t counts[kBands + 1] = {};
            uint32_t wrong = 0;
            uint32_t highlit = 0;
            PrVec3 eye{0, 0, kCameraDistance};
            for (uint32_t y = 0; y < kSphereExtent.y; ++y) {
                for (uint32_t x = 0; x < kSphereExtent.x; ++x) {
                    // The pixel's ray, and where it meets the sphere.
                    PrVec3 through{(float(x) + 0.5f) / kSphereExtent.x * 2 - 1, 1 - (float(y) + 0.5f) / kSphereExtent.y * 2, 0};
                    PrVec3 d = prVec3Normalize(through - eye);
                    float b = prVec3Dot(eye, d);
                    float discriminant = b * b - (prVec3Dot(eye, eye) - kRadius * kRadius);
                    if (discriminant <= 0)
                        continue;
                    PrVec3 n = (eye + d * (-b - prF32Sqrt(discriminant))) / kRadius;
                    PrVec3 toEye = d * -1;
                    // Near the sphere's edge the mesh's facets are not the sphere.
                    if (prVec3Dot(n, toEye) < 0.3f)
                        continue;
                    float nl = prVec3Dot(n, toLight);
                    float scaled = prF32Max(nl, 0) * paint.bands;
                    // A pixel at the edge of a band or of the highlight may fall to either side.
                    if (nl > -0.02f && prF32Abs(scaled - prF32Round(scaled)) < 0.08f)
                        continue;
                    float nh = prVec3Dot(n, prVec3Normalize(toEye + toLight));
                    if (prF32Abs(nh - paint.highlight.w) < 0.004f)
                        continue;
                    uint32_t band = uint32_t(prF32Ceil(scaled));
                    bool lit = nl > 0 && nh > paint.highlight.w;
                    float diffuse = float(band) / paint.bands * kLux + paint.ambient;
                    PrVec3 expected = rgb(paint.color) * diffuse;
                    if (lit)
                        expected = expected + rgb(paint.highlight) * kLux;
                    const float* texel = image.at(x, y);
                    if (!near(texel, expected, 2e-3f))
                        ++wrong;
                    if (lit) {
                        ++highlit;
                        continue;
                    }
                    if (counts[band]++ == 0) {
                        least[band] = texel[0];
                        greatest[band] = texel[0];
                    }
                    least[band] = prF32Min(least[band], texel[0]);
                    greatest[band] = prF32Max(greatest[band], texel[0]);
                }
            }
            CHECK(wrong == 0);
            CHECK(highlit > 20);
            // Every band is on the sphere, the dark side among them, and no pixel of a band differs from another.
            for (uint32_t band = 0; band <= kBands; ++band) {
                CAPTURE(band);
                CHECK(counts[band] > 30);
                CHECK(greatest[band] == least[band]);
            }
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a second custom shading model lights its material type alone") {
    onEachDevice([](Fixture& f) {
        const PrVec3 color{0.6f, 0.3f, 0.9f};
        TestFeltParameters felt{.color = {color.x, color.y, color.z, 1}};
        QuadScene s = createQuadScene(f, createMaterial(f, feltDesc(felt)));
        constexpr float kLux = 1.5f;
        const PrVec3 radiance{0.2f, 0.4f, 0.1f};
        setEnvironment(f, s.scene, createEvenEnvironment(f, radiance));

        // The light's direction, and the part of the color that the wrap model returns for it: the cosine of the
        // light's angle, moved from [-1, 1] to [0, 1], squared.
        struct Case {
            PrVec3 direction;
            float wrapped;
        };
        const Case cases[] = {{{0, 0, -1}, 1}, {{1, 0, 0}, 0.25f}, {{0, 0, 1}, 0}, {{0, -0.6f, -0.8f}, 0.81f}};
        PrRenderLightDesc light = prRenderLightDescDefault();
        light.intensity = kLux;
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLight sun = prRenderLightAdd(update, &light);
        prRenderUpdateCommit(update);
        for (const Case& c : cases) {
            CAPTURE(c.wrapped);
            light.direction = c.direction;
            update = prRenderUpdateBegin(f.render, s.scene);
            prRenderLightSet(update, sun, &light);
            prRenderUpdateCommit(update);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                // The even environment gives a white surface its own radiance.
                PrVec3 expected{
                    color.x * (c.wrapped * kLux + radiance.x),
                    color.y * (c.wrapped * kLux + radiance.y),
                    color.z * (c.wrapped * kLux + radiance.z),
                };
                CHECK(near(image.at(kCenter, kCenter), expected, 3e-3f));
                CHECK(near(image.at(3, 29), expected, 3e-3f));
            }
        }

        // The library's model lights the same color otherwise: it returns nothing for a light behind the surface, and
        // less than the wrap model's all for one in front.
        PrRenderKhronosPbrMaterial gltf = prRenderKhronosPbrMaterialDefault();
        gltf.baseColor = {color.x, color.y, color.z, 1};
        gltf.metallic = 0;
        setGltf(f.render, s.material, &gltf);
        light.direction = {0, 0, -1};
        update = prRenderUpdateBegin(f.render, s.scene);
        prRenderLightSet(update, sun, &light);
        prRenderUpdateCommit(update);
        s.view.technique = PrRenderTechnique_Forward;
        Image standard = f.view(s.view);
        CHECK(standard.at(kCenter, kCenter)[2] < color.z * (kLux * 0.5f + radiance.z));
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a shading model prepares a context that its emission reads with the eye, and sees each light's size") {
    onEachDevice([](Fixture& f) {
        TestProbeParameters probe{.color = {1, 0.5f, 0.25f, 1}, .shows = kTestProbeFacing};
        PrRenderMaterialDesc desc = prRenderMaterialDescDefault(kTestRenderMaterialProbe, &probe, sizeof(probe));
        QuadScene s = createQuadScene(f, createMaterial(f, desc));

        // The quad faces the camera, so the cosine of its normal with the eye at a point is the camera's distance over
        // the distance from the camera to the point. The emission is the color times the cosine from the context, times
        // the cosine from the direction that the emission is given.
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image image = f.view(s.view);
            const PrUVec2 pixels[] = {{kCenter, kCenter}, {2, kCenter}, {29, 3}};
            for (PrUVec2 pixel : pixels) {
                CAPTURE(pixel.x);
                PrVec2 uv = pixelUv(pixel.x, pixel.y);
                float x = uv.x * 2 - 1;
                float y = uv.y * 2 - 1;
                float cosine = kCameraDistance / prF32Sqrt(kCameraDistance * kCameraDistance + x * x + y * y);
                PrVec3 expected{
                    probe.color.x * cosine * cosine, probe.color.y * cosine * cosine,
                    probe.color.z * cosine * cosine,
                };
                CHECK(near(image.at(pixel.x, pixel.y), expected, 2e-3f));
            }
        }

        // A directional light's angular radius is the light's own. A point light's is the angle that the radius of its
        // source spans at the point, and zero for a source without size.
        probe.shows = kTestProbeLightSize;
        prRenderMaterialSet(f.render, s.material, &desc);
        PrRenderLightDesc sun = prRenderLightDescDefault();
        sun.direction = {0, 0, -1};
        sun.angularRadius = 0.25f;
        PrRenderLightDesc lamp = prRenderLightDescDefault();
        lamp.kind = PrRenderLightKind_Point;
        lamp.position = {0, 0, 2};
        lamp.radius = 1;
        PrRenderLightDesc bare = lamp;
        bare.radius = 0;
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        prRenderLightAdd(update, &sun);
        prRenderLightAdd(update, &lamp);
        prRenderLightAdd(update, &bare);
        prRenderUpdateCommit(update);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            // The source of radius 1 is 2 away, and spans the angle whose sine is one half.
            float expected = 0.25f + 0.5235988f;
            CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(expected).epsilon(0.002));
        }

        // The triangle's own normal: the quad's is its vertices' normal, and a sphere's is near the sphere's normal at
        // the pixel, of unit length, and the same in both techniques to within the difference of two neighboring
        // triangles.
        probe.shows = kTestProbeGeometric;
        prRenderMaterialSet(f.render, s.material, &desc);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            CHECK(near(f.view(s.view).at(3, 20), {0.5f, 0.5f, 1}, 2e-3f));
        }
        PrRenderScene scene = prRenderSceneCreate(f.render);
        update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, createSphere(f, 48), s.material, placed({0, 0, 0}));
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            Image image = f.view(view);
            const PrVec2 points[] = {{0, 0}, {0.5f, 0.3f}, {-0.6f, 0.5f}, {0.2f, -0.7f}};
            for (PrVec2 point : points) {
                // The point of the unit sphere that the camera sees through the point of the plane z = 0.
                PrVec3 eye{0, 0, kCameraDistance};
                PrVec3 d = prVec3Normalize(PrVec3{point.x, point.y, 0} - eye);
                float b = prVec3Dot(eye, d);
                PrVec3 n = eye + d * (-b - prF32Sqrt(b * b - (prVec3Dot(eye, eye) - 1)));
                PrUVec2 pixel = pixelOf(view, n);
                const float* texel = image.at(pixel.x, pixel.y);
                PrVec3 shown{texel[0] * 2 - 1, texel[1] * 2 - 1, texel[2] * 2 - 1};
                CAPTURE(point.x);
                CHECK(prVec3Length(shown) == doctest::Approx(1).epsilon(0.01));
                CHECK(prVec3Dot(shown, n) > 0.99f);
            }
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a shader set's tables read back at their texels and between them, each apart from the others") {
    const PrRenderShaderSet* set = testRenderShaderSet();
    // The library's table is the set's first, and the set's own lie under it in one texture.
    REQUIRE(set->tableCount == kTestRenderTableCount);
    CHECK(kTestRenderTableSplitSum == 0);
    CHECK(set->tables[0].name == "SplitSum");
    CHECK(set->tables[0].fill == prRenderSplitSumTable);
    CHECK(set->tables[kTestRenderTableRamp].name == "Ramp");
    CHECK(set->tables[kTestRenderTableRamp].fill == nullptr);
    CHECK(set->tables[1].origin == PrUVec2{0, 32});
    CHECK(set->tables[1].extent == PrUVec2{4, 2});
    CHECK(set->tables[2].origin == PrUVec2{0, 34});
    CHECK(set->tablesExtent == PrUVec2{32, 36});
    CHECK(prRenderShaderSetLibrary()->tableCount == 1);
    CHECK(prRenderShaderSetLibrary()->tablesExtent == PrUVec2{32, 32});

    // A source's context reaches its function: a renderer whose flat table is filled with another value.
    for (const TestDevice& device : testDevices()) {
        static float value = 0.375f;
        const PrRenderTableSource sources[] = {
            {.table = kTestRenderTableRamp, .fill = fillRamp},
            {.table = kTestRenderTableFlat, .fill = fillFlat, .context = &value},
        };
        Fixture f(device, testRenderShaderSet(), sources);
        TestProbeParameters probe{.color = {1, 1, 1, 1}, .shows = kTestProbeFlat};
        PrRenderMaterialDesc desc = prRenderMaterialDescDefault(kTestRenderMaterialProbe, &probe, sizeof(probe));
        QuadScene s = createQuadScene(f, createMaterial(f, desc));
        CHECK(near(f.view(s.view).at(kCenter, kCenter), {0.375f, 0.375f, 0.375f}, 1e-4f));
    }

    onEachDevice([](Fixture& f) {
        TestProbeParameters probe{.color = {1, 1, 1, 1}, .shows = kTestProbeRamp};
        PrRenderMaterialDesc desc = prRenderMaterialDescDefault(kTestRenderMaterialProbe, &probe, sizeof(probe));
        QuadScene s = createQuadScene(f, createMaterial(f, desc));
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;

            // The ramp, wherever the view samples it.
            probe.shows = kTestProbeRamp;
            prRenderMaterialSet(f.render, s.material, &desc);
            Image ramp = f.view(s.view);
            uint32_t wrong = 0;
            for (uint32_t y = 0; y < kExtent.y; ++y) {
                for (uint32_t x = 0; x < kExtent.x; ++x) {
                    PrVec2 uv = pixelUv(x, y);
                    if (!near(ramp.at(x, y), {uv.x, uv.y, 0.25f + 0.5f * uv.x * uv.y}, 1e-3f))
                        ++wrong;
                }
            }
            CHECK(wrong == 0);

            // The flat table holds one value, and no texel of the tables about it reaches a sample of it.
            probe.shows = kTestProbeFlat;
            prRenderMaterialSet(f.render, s.material, &desc);
            Image flat = f.view(s.view);
            wrong = 0;
            for (uint32_t y = 0; y < kExtent.y; ++y) {
                for (uint32_t x = 0; x < kExtent.x; ++x) {
                    if (!near(flat.at(x, y), {0.75f, 0.75f, 0.75f}, 1e-4f))
                        ++wrong;
                }
            }
            CHECK(wrong == 0);

            // The library's split sum, as a shading model reads it, against the tests' statement of the same lookup,
            // which their expectations of the library's model are made with.
            probe.shows = kTestProbeSplitSum;
            prRenderMaterialSet(f.render, s.material, &desc);
            Image split = f.view(s.view);
            float largest = 0;
            for (uint32_t y = 0; y < kExtent.y; ++y) {
                for (uint32_t x = 0; x < kExtent.x; ++x) {
                    PrVec2 uv = pixelUv(x, y);
                    SplitSum expected = referenceSplitSum(double(uv.x), double(uv.y));
                    largest = prF32Max(largest, prF32Abs(split.at(x, y)[0] - float(expected.scale)));
                    largest = prF32Max(largest, prF32Abs(split.at(x, y)[1] - float(expected.bias)));
                }
            }
            CAPTURE(largest);
            CHECK(largest < 1e-3f);
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a material type has the textures and the record that it declares, past what a draw binds through slots") {
    const PrRenderShaderSet* set = testRenderShaderSet();
    const PrRenderMaterialType& type = set->materialTypes[kTestRenderMaterialQuilt];
    CHECK(type.textureCount == kTestQuiltTextures);
    CHECK(type.parameterBytes == sizeof(TestQuiltParameters));
    CHECK(sizeof(TestQuiltParameters) > 128);

    onEachDevice([&](Fixture& f) {
        // A device with bindless samples every texture of a material, and any other the textures that a draw binds,
        // which the passes declare.
        TestQuiltParameters quiltRecord{};
        PrRenderMaterialDesc limited =
            prRenderMaterialDescDefault(kTestRenderMaterialQuilt, &quiltRecord, sizeof(quiltRecord));
        uint32_t limit = prRenderMaterialTextureLimit(f.render, limited.flags);
        CHECK(limit == (f.bindless ? UINT32_MAX : 5));
        CHECK((f.bindless || kTestQuiltTextures > limit));

        // Each texture is one texel of a red of its own, and each weight puts its texture in a channel of its own
        // scale, so that the sum says which textures were sampled.
        TestQuiltParameters quilt{.tint = {0.01f, 0.02f, 0.03f, 1}};
        PrRenderTextureUse uses[kTestQuiltTextures];
        float texels[kTestQuiltTextures];
        for (uint32_t i = 0; i < kTestQuiltTextures; ++i) {
            quilt.weights[i] = {i % 2 == 0 ? 0.125f : 0.0f, i % 2 == 1 ? 0.125f : 0.0f, float(i + 1) / 64, 0};
            uint8_t texel[4] = {uint8_t(20 + 25 * i), 0, 0, 255};
            texels[i] = float(texel[0]) / 255;
            uses[i] = useOf(f, createTexture(f, {1, 1}, texel));
        }
        PrRenderMaterialDesc desc = prRenderMaterialDescDefault(kTestRenderMaterialQuilt, &quilt, sizeof(quilt));
        desc.textures = uses;
        desc.textureCount = kTestQuiltTextures;
        QuadScene s = createQuadScene(f, createMaterial(f, desc));

        // Returns the color of a material that names the textures of `named`: the tint, and each texture's weight times
        // what the texture reads as. The first textures that the material names, up to the limit, are sampled. Those
        // that it names past them read as their stand-ins, and those that it does not name as white.
        auto expected = [&](uint32_t named) {
            PrVec3 sum{quilt.tint.x, quilt.tint.y, quilt.tint.z};
            uint32_t sampled = 0;
            for (uint32_t i = 0; i < kTestQuiltTextures; ++i) {
                float value = 1;
                if ((named >> i & 1) != 0)
                    value = sampled++ < limit ? texels[i] : type.textures[i].standIn.x;
                sum = sum + PrVec3{quilt.weights[i].x, quilt.weights[i].y, quilt.weights[i].z} * value;
            }
            return sum;
        };
        // Every texture, the five that a draw's slots hold in any places, and one alone.
        const uint32_t cases[] = {0xff, 0xea, 0x1f, 0x80, 0};
        for (uint32_t named : cases) {
            CAPTURE(named);
            PrRenderTextureUse some[kTestQuiltTextures];
            for (uint32_t i = 0; i < kTestQuiltTextures; ++i)
                some[i] = (named >> i & 1) != 0 ? uses[i] : useOf(f, PrRenderTexture{});
            desc.textures = some;
            prRenderMaterialSet(f.render, s.material, &desc);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                CHECK(near(f.view(s.view).at(kCenter, kCenter), expected(named), 2e-3f));
            }
        }
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("the forward technique makes a pipeline for a material type when a material of the type is first set") {
    onEachDevice([](Fixture& f) {
        TestRingsParameters rings = glowingRings();
        TestFeltParameters felt{.color = {0.6f, 0.3f, 0.9f, 1}};
        PrRenderMaterialDesc ringsMaterial = ringsDesc(rings);
        PrRenderMaterialDesc feltMaterial = feltDesc(felt);
        QuadScene s = createQuadScene(f, createMaterial(f, ringsMaterial));

        // The visibility technique shades every type in one shader, so a type that it has not drawn costs no pipeline.
        if (f.bindless) {
            s.view.technique = PrRenderTechnique_Visibility;
            f.view(s.view);
            uint32_t made = prRenderPipelineInfo(f.render).count;
            prRenderMaterialSet(f.render, s.material, &feltMaterial);
            f.view(s.view);
            CHECK(prRenderPipelineInfo(f.render).count == made);
            prRenderMaterialSet(f.render, s.material, &ringsMaterial);
        }

        s.view.technique = PrRenderTechnique_Forward;
        f.view(s.view);
        uint32_t made = prRenderPipelineInfo(f.render).count;
        // The call that sets a material of a new type makes the type's pipeline for the view's states, so the frame
        // that first draws the material makes none.
        prRenderMaterialSet(f.render, s.material, &feltMaterial);
        CHECK(prRenderPipelineInfo(f.render).count == made + 1);
        f.view(s.view);
        CHECK(prRenderPipelineInfo(f.render).count == made + 1);
        // A type that the view has drawn has its pipeline.
        prRenderMaterialSet(f.render, s.material, &ringsMaterial);
        f.view(s.view);
        CHECK(prRenderPipelineInfo(f.render).count == made + 1);
        // A blended material of the type is drawn in other states, which are a pipeline of their own.
        feltMaterial.alphaMode = PrRenderAlphaMode_Blend;
        prRenderMaterialSet(f.render, s.material, &feltMaterial);
        CHECK(prRenderPipelineInfo(f.render).count == made + 2);
        f.view(s.view);
        CHECK(prRenderPipelineInfo(f.render).count == made + 2);
    }, testRenderShaderSet(), kTestTables);
}

TEST_CASE("a scene that mixes custom and library types draws alike with slots and with bindless") {
    const Array<TestDevice>& devices = testDevices();
    for (uint32_t i = 0; i + 1 < devices.count(); ++i) {
        if (devices[i].backend != devices[i + 1].backend || devices[i].slotsOnly || !devices[i + 1].slotsOnly)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        Image handles;
        Image slots;
        {
            Fixture f(devices[i], testRenderShaderSet(), kTestTables);
            handles = f.view(createMixed(f));
        }
        {
            Fixture f(devices[i + 1], testRenderShaderSet(), kTestTables);
            REQUIRE_FALSE(f.bindless);
            slots = f.view(createMixed(f));
        }
        Difference difference = compare(handles, slots, 0);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
    }
}

TEST_CASE("the backends draw a scene of custom and library types alike, to within their compilers' rounding") {
    Array<Image> images;
    Array<PrStringView> names;
    for (const TestDevice& device : testDevices()) {
        Fixture f(device, testRenderShaderSet(), kTestTables);
        if (!f.bindless)
            continue;
        images.push(f.view(createMixed(f)));
        names.push(prBackendName(prGpuSelectInterface(), device.backend));
    }
    for (uint32_t i = 1; i < images.count(); ++i) {
        CAPTURE(names[0]);
        CAPTURE(names[i]);
        // A sample on the edge of a triangle, of a shadow, of a mask or of a toon band can fall to the other side under
        // another compiler's rounding, so a few pixels in a thousand may differ by more than the last bits.
        Difference difference = compare(images[0], images[i], 0.01f);
        CAPTURE(difference.largest);
        CAPTURE(difference.over);
        CHECK(difference.over <= kMixedExtent.x * kMixedExtent.y / 250);
        CHECK(difference.mean < 5e-4);
    }
}

TEST_CASE("the two techniques shade a scene of custom and library types alike, away from edges") {
    onEachDevice([](Fixture& f) {
        if (!prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
            return;
        PrRenderViewDesc view = createMixed(f);
        view.technique = PrRenderTechnique_Visibility;
        Image visibility = f.view(view);
        view.technique = PrRenderTechnique_Forward;
        view.samples = 1;
        Image forward = f.view(view);
        view.samples = 4;
        Image smoothed = f.view(view);
        uint32_t pixels = kMixedExtent.x * kMixedExtent.y;

        // With one sample each, the techniques cover the same pixels and call the same functions with the same
        // arguments. They differ by the rounding of the visibility technique's own interpolation, and by more at the
        // few pixels where that rounding moves the edge of a texel, a shadow, a mask or a toon band.
        Difference single = compare(forward, visibility, 0.02f);
        CAPTURE(single.largest);
        CAPTURE(single.over);
        CHECK(single.over <= pixels / 250);
        CHECK(single.mean < 1e-3);

        // A pixel of the multisampled image that differs from the single-sampled one lies on a geometric edge. Away
        // from those, the multisampled forward image and the visibility image agree as closely.
        uint32_t edges = 0;
        uint32_t differing = 0;
        for (uint32_t i = 0; i < pixels; ++i) {
            float edge = 0;
            float technique = 0;
            for (uint32_t channel = 0; channel < 3; ++channel) {
                edge = prF32Max(edge, prF32Abs(smoothed.texels[i * 4 + channel] - forward.texels[i * 4 + channel]));
                technique = prF32Max(technique,
                    prF32Abs(smoothed.texels[i * 4 + channel] - visibility.texels[i * 4 + channel]));
            }
            if (edge > 0.02f)
                ++edges;
            else if (technique > 0.04f)
                ++differing;
        }
        CAPTURE(edges);
        CAPTURE(differing);
        CHECK(edges > pixels / 100);
        CHECK(edges < pixels / 3);
        CHECK(differing <= pixels / 250);
    }, testRenderShaderSet(), kTestTables);
}

TEST_SUITE_END();
