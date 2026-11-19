#include "pr/atomic.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/thread.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestRenderCApi(void);

namespace pr_impl {

using namespace harness;

namespace {

constexpr PrVec3 kRed{1, 0, 0};
constexpr PrVec3 kGreen{0, 1, 0};
constexpr PrVec3 kBlack{0, 0, 0};

// What a producer thread adds to a scene.
struct Producer {
    PrRender* render;
    PrRenderScene scene;
    uint32_t updates;           // the updates that the thread commits
    uint32_t instances;         // the instances that each update adds
    Atomic<bool> done;
};

// Creates a mesh and a material of its own, and commits updates that each add instances of them.
void produce(void* PR_NULLABLE context) {
    Producer& p = *static_cast<Producer*>(context);
    PrRenderMesh mesh = prRenderMeshCreate(p.render);
    PrRenderMaterial material = prRenderMaterialCreate(p.render);
    PrRenderGeometryVertex vertices[3] = {};
    vertices[1].position[0] = 1;
    vertices[2].position[1] = 1;
    const uint32_t indices[3] = {0, 1, 2};
    PrRenderMeshPart part{.indexCount = 3};
    // The mesh gives no attributes, so its draws read the fallbacks of their layout.
    PrRenderMeshData data{
        .geometry = vertices,
        .vertexCount = 3,
        .indices = indices,
        .indexCount = 3,
        .parts = &part,
        .partCount = 1,
    };
    prRenderMeshSetData(p.render, mesh, &data);
    PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
    prRenderMaterialSetUnlit(p.render, material, &unlit);
    for (uint32_t i = 0; i < p.updates; ++i) {
        PrRenderUpdate* update = prRenderUpdateBegin(p.render, p.scene);
        for (uint32_t j = 0; j < p.instances; ++j)
            addInstance(update, mesh, material, placed({float(i), float(j), 0}));
        prRenderUpdateCommit(update);
    }
    p.done.store(true);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render" * doctest::skip(!gpuTests()));

TEST_CASE("the public headers compile as C, with the records' layout") {
    CHECK(prTestRenderCApi() == 0);
}

TEST_CASE("the defaults are the ones that the headers state") {
    CHECK(prRenderOptionsDefault().shadowMapSize == 2048);
    PrRenderViewDesc view = prRenderViewDescDefault(PrRenderScene{7});
    CHECK(view.scene.id == 7);
    CHECK(view.technique == PrRenderTechnique_Forward);
    CHECK(view.samples == 4);
    CHECK(view.exposure == 1);
    CHECK(view.shadows);
    PrRenderInstanceDesc instance = prRenderInstanceDescDefault(PrRenderMesh{3});
    CHECK(instance.mesh.id == 3);
    CHECK(instance.world.m[0][0] == 1);
    CHECK(instance.world.m[1][1] == 1);
    CHECK(instance.world.m[2][3] == 0);
    CHECK((instance.visibility & kPrRenderVisibilityShadow) != 0);
    CHECK(instance.mobility == PrRenderMobilityFlag_Static);
    PrRenderKhronosPbrMaterial gltf = prRenderKhronosPbrMaterialDefault();
    CHECK(gltf.metallic == 1);
    CHECK(gltf.roughness == 1);
    CHECK(gltf.alphaCutoff == 0.5f);
    CHECK(gltf.baseColorTexture.transform.a == 1);
    CHECK(gltf.baseColorTexture.transform.d == 1);
    CHECK(prRenderDisplayOptionsDefault().exposure == 1);
    CHECK(prRenderDisplayOptionsDefault().headroom == 1);
    prRenderDestroy(nullptr);
    prRenderUpdateDiscard(nullptr);
}

TEST_CASE("exposure is a camera's at ISO 100, and an incident-light meter shows white under what it reads as white") {
    // ISO 12232's saturation luminance, 1.2 * 2^EV100 cd/m2, at a clear day's EV100 of 15 and at 0.
    CHECK(prRenderExposureFromEv100(15) == doctest::Approx(1 / (1.2 * 32768)).epsilon(1e-6));
    CHECK(prRenderExposureFromEv100(0) == doctest::Approx(1 / 1.2).epsilon(1e-6));
    // A meter's calibration constant of 120 pi lx s: log2(E * 100 / C), whose exposure is pi / E.
    const float illuminances[] = {0.01f, 1.0f, 80.0f, 50000.0f, 127500.0f};
    for (float illuminance : illuminances) {
        CAPTURE(illuminance);
        float ev100 = prRenderEv100FromIlluminance(illuminance);
        CHECK(ev100 == doctest::Approx(prF64Log2(double(illuminance) * 100 / (120 * kPrF64Pi))).epsilon(1e-5));
        CHECK(prRenderExposureFromEv100(ev100) == doctest::Approx(kPrF64Pi / double(illuminance)).epsilon(1e-5));
    }
    // 50,000 lux, UsdLux's fallback for a distant light, reads 13.7 and is exposed by 1 / 15,915.
    CHECK(prRenderEv100FromIlluminance(50000) == doctest::Approx(13.695).epsilon(1e-4));
    // No light reads as a millionth of a lux.
    CHECK(prRenderEv100FromIlluminance(0) == prRenderEv100FromIlluminance(1e-6f));
    CHECK(prRenderEv100FromIlluminance(-1) == prRenderEv100FromIlluminance(1e-6f));
}

TEST_CASE("a scene's meter reads the lights and the environment as the shaders compute them") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        // A sun of 1000 lux shining down.
        PrRenderLightDesc sun = prRenderLightDescDefault();
        sun.intensity = 1000;
        PrRenderLight sunLight = prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);
        f.frame();
        CHECK(prRenderSceneIlluminance(f.render, scene, {0, 0, 0}) == doctest::Approx(1000));
        CHECK(prRenderSceneIlluminance(f.render, PrRenderScene{}, {0, 0, 0}) == 0);

        // A green point light of 100 candela 2 m to the side gives 0.7152 * 100 / 4 lux on a plane that faces it, which
        // the sun's plane does not face. The plane that faces the sun is the brightest.
        update = prRenderUpdateBegin(f.render, scene);
        PrRenderLightDesc point = prRenderLightDescDefault();
        point.kind = PrRenderLightKind_Point;
        point.color = {0, 1, 0};
        point.intensity = 100;
        point.position = {2, 0, 0};
        prRenderLightAdd(update, &point);
        prRenderLightRemove(update, sunLight);
        prRenderUpdateCommit(update);
        f.frame();
        CHECK(prRenderSceneIlluminance(f.render, scene, {0, 0, 0}) == doctest::Approx(0.7152 * 25).epsilon(1e-5));
        // Its range windows it as glTF's falloff does: (1 - (d / r)^4) at d of 2 and r of 4.
        update = prRenderUpdateBegin(f.render, scene);
        point.range = 4;
        prRenderLightSet(update, PrRenderLight{2}, &point);
        prRenderUpdateCommit(update);
        f.frame();
        CHECK(prRenderSceneIlluminance(f.render, scene, {0, 0, 0}) ==
            doctest::Approx(0.7152 * 25 * (1 - 1.0 / 16)).epsilon(1e-5));

        // An even environment of 10 nits, at an intensity of 3, gives every plane pi * 30 lux besides.
        PrRenderEnvironment even = createEvenEnvironment(f, {10, 10, 10});
        setEnvironment(f, scene, even, 3);
        f.frame();
        CHECK(prRenderSceneIlluminance(f.render, scene, {0, 0, 0}) ==
            doctest::Approx(0.7152 * 25 * (1 - 1.0 / 16) + kPrF64Pi * 30).epsilon(1e-3));
        prRenderEnvironmentDestroy(f.render, even);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a view's exposure scales its surfaces and its background before they are stored, and no unlit color") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        // A black surface that emits 2000 nits, exposed by one thousandth, and beside it an unlit color, whose values
        // are the view's and which the exposure leaves as they are.
        PrRenderKhronosPbrMaterial glowing = prRenderKhronosPbrMaterialDefault();
        glowing.baseColor = {0, 0, 0, 1};
        glowing.metallic = 0;
        glowing.emissive = {2000, 1000, 500};
        PrRenderMaterial bright = createGltf(f, glowing);
        PrRenderMaterial unlit = createUnlit(f, {0.25f, 0.5f, 1, 1});
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, bright, placed({0, 0, 0}, {0.25f, 0.25f, 1}));
        addInstance(update, quad, unlit, placed({0.5f, 0, 0}, {0.25f, 0.25f, 1}));
        prRenderUpdateCommit(update);
        PrRenderEnvironment even = createEvenEnvironment(f, {4000, 4000, 4000});
        PrRenderViewDesc view = frontView(scene);
        view.exposure = 0.001f;
        const PrRenderTechnique techniques[] = {PrRenderTechnique_Forward, PrRenderTechnique_Visibility};
        // The surfaces in the dark, and then the background of an even environment, which the black surface would
        // reflect.
        const bool lits[] = {false, true};
        for (bool lit : lits) {
            if (lit)
                setEnvironment(f, scene, even);
            for (PrRenderTechnique technique : techniques) {
                if (!prRenderSupportsTechnique(f.render, technique))
                    continue;
                CAPTURE(lit);
                CAPTURE(int(technique));
                view.technique = technique;
                Image image = f.view(view);
                if (lit) {
                    CHECK(near(image.at(1, 1), {4, 4, 4}, 4e-3f));
                } else {
                    CHECK(near(image.at(kCenter, kCenter), {2, 1, 0.5f}, 2e-3f));
                    CHECK(near(image.at(kCenter + 8, kCenter), {0.25f, 0.5f, 1}, 1e-3f));
                }
            }
        }
        prRenderEnvironmentDestroy(f.render, even);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("an emission is luminance under the view's exposure, or the view's own value at any exposure") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        // Black surfaces that emit the same numbers: a glTF material, with a strength, and a preview surface, each in
        // both units. Three quads lie across the top row and three across the bottom.
        PrRenderKhronosPbrMaterial gltf = prRenderKhronosPbrMaterialDefault();
        gltf.baseColor = {0, 0, 0, 1};
        gltf.metallic = 0;
        gltf.emissive = {0.25f, 0.125f, 0.5f};
        gltf.emissiveStrength = 2;
        PrRenderUsdPreviewMaterial preview = prRenderUsdPreviewMaterialDefault();
        preview.diffuseColor = {0, 0, 0};
        preview.emissiveColor = {0.5f, 0.25f, 1};
        PrRenderMaterial materials[2][2];
        for (PrRenderEmission emission = 0; emission < PrRenderEmission_Count; ++emission) {
            gltf.emission = emission;
            preview.emission = emission;
            materials[emission][0] = createGltf(f, gltf);
            materials[emission][1] = prRenderMaterialCreate(f.render);
            prRenderMaterialSetUsdPreview(f.render, materials[emission][1], &preview);
        }
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        for (uint32_t emission = 0; emission < 2; ++emission) {
            for (uint32_t kind = 0; kind < 2; ++kind) {
                PrVec3 at{kind == 0 ? -0.5f : 0.5f, emission == 0 ? 0.5f : -0.5f, 0};
                addInstance(update, quad, materials[emission][kind], placed(at, {0.25f, 0.25f, 1}));
            }
        }
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        const PrRenderTechnique techniques[] = {PrRenderTechnique_Forward, PrRenderTechnique_Visibility};
        const float exposures[] = {1, 0.001f, 50};
        for (PrRenderTechnique technique : techniques) {
            if (!prRenderSupportsTechnique(f.render, technique))
                continue;
            for (float exposure : exposures) {
                CAPTURE(int(technique));
                CAPTURE(exposure);
                view.technique = technique;
                view.exposure = exposure;
                Image image = f.view(view);
                // Nits are scaled by the exposure, and the view's values are kept as they are.
                PrVec3 nits = PrVec3{0.5f, 0.25f, 1} * exposure;
                float tolerance = 2e-3f * prF32Max(1, exposure);
                for (uint32_t kind = 0; kind < 2; ++kind) {
                    PrUVec2 luminance = pixelOf(view, {kind == 0 ? -0.5f : 0.5f, 0.5f, 0});
                    PrUVec2 shown = pixelOf(view, {kind == 0 ? -0.5f : 0.5f, -0.5f, 0});
                    CHECK(near(image.at(luminance.x, luminance.y), nits, tolerance));
                    CHECK(near(image.at(shown.x, shown.y), {0.5f, 0.25f, 1}, 2e-3f));
                }
            }
        }
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a reflected-light meter reads luminance by ISO 2720's constant, and an exposure adapts by its half-life") {
    // log2(L * 100 / 12.5), whose exposure shows L as 12.5 / 120.
    CHECK(prRenderEv100FromLuminance(1) == doctest::Approx(3).epsilon(1e-6));
    CHECK(prRenderEv100FromLuminance(1000) == doctest::Approx(12.9658).epsilon(1e-5));
    float exposure = prRenderExposureFromEv100(prRenderEv100FromLuminance(250));
    CHECK(exposure * 250 == doctest::Approx(12.5 / 120).epsilon(1e-5));
    CHECK(prRenderEv100FromLuminance(0) == prRenderEv100FromLuminance(1e-6f));
    // Half of the difference closes in each half-life, and none in no time.
    CHECK(prRenderEv100Adapt(10, 14, 0.5f, 0.5f) == doctest::Approx(12).epsilon(1e-6));
    CHECK(prRenderEv100Adapt(10, 14, 1, 0.5f) == doctest::Approx(13).epsilon(1e-6));
    CHECK(prRenderEv100Adapt(10, 14, 0, 0.5f) == 10);
    CHECK(prRenderEv100Adapt(10, 14, 0.1f, 0) == 14);
    CHECK(prRenderEv100Adapt(10, 6, 0.5f, 0.5f) == doctest::Approx(8).epsilon(1e-6));
}

TEST_CASE("a meter reads the luminance that a view shows, whatever its exposure, and leaves out what has no light") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        // Black surfaces that emit 100 and 1600 nits, whose logarithms' mean is that of 400 nits.
        PrRenderKhronosPbrMaterial gltf = prRenderKhronosPbrMaterialDefault();
        gltf.baseColor = {0, 0, 0, 1};
        gltf.metallic = 0;
        gltf.emissive = {100, 100, 100};
        PrRenderMaterial dim = createGltf(f, gltf);
        gltf.emissive = {1600, 1600, 1600};
        PrRenderMaterial bright = createGltf(f, gltf);
        PrRenderMeter meter = prRenderMeterCreate(f.render);
        CHECK(prRenderMeterRead(f.render, meter).serial == 0);
        // The view's pixels are the meter's cells, so each half of the view is half of the cells.
        PrRenderViewDesc view = frontView(scene);
        view.extent = {kPrRenderMeterGrid, kPrRenderMeterGrid};
        view.meter = meter;

        SUBCASE("halves") {
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            addInstance(update, quad, dim, placed({-0.5f, 0, 0}, {0.5f, 1, 1}));
            addInstance(update, quad, bright, placed({0.5f, 0, 0}, {0.5f, 1, 1}));
            prRenderUpdateCommit(update);
            const float exposures[] = {1.0f / 400, 1.0f / 40000, 0.1f};
            uint64_t serial = 0;
            for (float exposure : exposures) {
                CAPTURE(exposure);
                view.exposure = exposure;
                f.view(view);
                PrRenderMeterReading reading = prRenderMeterRead(f.render, meter);
                CHECK(reading.serial == serial + 1);
                serial = reading.serial;
                CHECK(reading.exposure == exposure);
                CHECK(reading.coverage == doctest::Approx(1));
                CHECK(reading.luminance == doctest::Approx(400).epsilon(0.01));
                CHECK(reading.ev100 == doctest::Approx(prRenderEv100FromLuminance(400)).epsilon(0.002));
            }
        }
        SUBCASE("the background has no light, and the brightest tenth does not count") {
            // A quad of 100 nits over a quarter of the view, and a sliver of 1600 nits that is a sixteenth of it,
            // against no environment.
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            addInstance(update, quad, dim, placed({0, 0, 0}, {0.5f, 0.5f, 1}));
            addInstance(update, quad, bright, placed({0.5f, 0.75f, 0.1f}, {0.5f, 0.125f, 1}));
            prRenderUpdateCommit(update);
            view.exposure = 1.0f / 100;
            f.view(view);
            PrRenderMeterReading reading = prRenderMeterRead(f.render, meter);
            // The sliver lies outside the quad, so a fifth of what has light is bright, which is more than a tenth:
            // part of it counts. A quarter plus a sixteenth of the view has light.
            CHECK(reading.coverage == doctest::Approx(0.25 + 0.0625).epsilon(0.02));
            double brightShare = 0.0625 / 0.3125;
            double counted = brightShare - 0.1;
            double expected = (0.7 * 6.643856 + counted * 10.643856) / (0.7 + counted);
            CHECK(prF32Log2(reading.luminance) == doctest::Approx(expected).epsilon(0.005));
        }
        SUBCASE("a view of nothing") {
            view.exposure = 1;
            f.view(view);
            PrRenderMeterReading reading = prRenderMeterRead(f.render, meter);
            CHECK(reading.serial == 1);
            CHECK(reading.coverage == 0);
            CHECK(reading.luminance == 0);
        }
        prRenderMeterDestroy(f.render, meter);
        prRenderSceneDestroy(f.render, scene);
        // A destroyed meter has no reading.
        f.frame();
        CHECK(prRenderMeterRead(f.render, meter).serial == 0);
    });
}

TEST_CASE("a vertex's geometry packs to its precision") {
    PrRenderGeometryVertex vertex = prRenderGeometryVertexPack({1.5f, -2, 3}, {0, 1, 0}, {-1, 0, 0.5f}, -1);
    CHECK(sizeof(vertex) == kPrRenderGeometryBytes);
    CHECK(vertex.position[0] == 1.5f);
    CHECK(vertex.position[2] == 3);
    CHECK(vertex.normal[1] == 32767);
    CHECK(vertex.normal[3] == 0);
    CHECK(vertex.tangent[0] == -32767);
    CHECK(vertex.tangent[2] == 16384);
    CHECK(vertex.tangent[3] == -32767);
    // The geometry's layout describes the same bytes, so a producer converts into it as into any other.
    const PrRenderVertexLayout* layout = prRenderGeometryLayout();
    CHECK(layout->stride == sizeof(PrRenderGeometryVertex));
    CHECK(layout->attributes[1].offset == offsetof(PrRenderGeometryVertex, normal));
    CHECK(layout->attributes[2].offset == offsetof(PrRenderGeometryVertex, tangent));
}

TEST_CASE("a layout is filled from a producer's attributes by semantic and name, and from its fallbacks") {
    const PrRenderShaderSet* set = prRenderShaderSetLibrary();
    const PrRenderVertexLayout* layout = prRenderMaterialTypeLayout(set, 0, 0);
    REQUIRE(layout->attributeCount == 3);
    // Two vertices with a set of texture coordinates of doubles and no color.
    const double uv[4] = {0.25, 0.5, 1, 2};
    PrRenderAttributeSource sources[] = {
        {PrRenderSemantic_Uv, 0, {}, {uv, 2, 16, PrMeshFormat{.component = PrMeshComponent_Float64, .count = 2}}},
    };
    uint8_t bytes[40] = {};
    uint32_t missing = 0;
    REQUIRE(layout->stride == 20);
    CHECK(prRenderVertexLayoutFill(layout, sources, 1, 2, bytes, &missing) == PrStatus_Ok);
    CHECK(missing == 2);
    float first[2];
    prMemCopy(first, bytes + layout->attributes[0].offset, sizeof(first));
    CHECK(first[0] == 0.25f);
    CHECK(first[1] == 0.5f);
    float second[2];
    prMemCopy(second, bytes + layout->stride + layout->attributes[1].offset, sizeof(second));
    CHECK(second[0] == 0);
    // The color's fallback is white, in the layout's 8 bits a channel.
    const uint8_t* color = bytes + layout->stride + layout->attributes[2].offset;
    CHECK(color[0] == 255);
    CHECK(color[3] == 255);
}

TEST_CASE("the library's shader set has the library's material types, with their records, stand-ins and layouts") {
    const PrRenderShaderSet* set = prRenderShaderSetLibrary();
    REQUIRE(set->materialTypeCount == 4);
    uint32_t gltf = prRenderMaterialTypeFromName(set, "KhronosPbr");
    REQUIRE(gltf != kPrRenderMaterialTypeMissing);
    CHECK(prRenderMaterialTypeFromName(set, "OpenPbr") == kPrRenderMaterialTypeMissing);
    const PrRenderMaterialType& gltfType = set->materialTypes[gltf];
    CHECK(gltfType.shadingModel == "RenderPbrSurface");
    CHECK(gltfType.parameterBytes == 64);
    // The parameters are glTF's properties by name, with glTF's defaults.
    const PrRenderMaterialParameter& roughness =
        gltfType.parameters[prRenderMaterialParameterFromName(&gltfType, "roughness")];
    CHECK(roughness.componentCount == 1);
    CHECK(roughness.value.x == 1);
    const PrRenderMaterialParameter& emissive =
        gltfType.parameters[prRenderMaterialParameterFromName(&gltfType, "emissive")];
    CHECK(emissive.componentCount == 4);
    CHECK(emissive.offset == 16);
    CHECK(prRenderMaterialParameterFromName(&gltfType, "sheenColor") == kPrRenderMaterialParameterMissing);
    CHECK(gltfType.textures[prRenderMaterialTextureFromName(&gltfType, "metallicRoughness")].channels ==
        (PrRenderChannelFlag_G | PrRenderChannelFlag_B));
    uint32_t baseColor = prRenderMaterialTextureFromName(&gltfType, "baseColor");
    CHECK(gltfType.textures[baseColor].channels == PrRenderChannelFlag_Rgba);
    CHECK(gltfType.textureCount == 5);
    uint32_t metallicRoughness = prRenderMaterialTextureFromName(&gltfType, "metallicRoughness");
    REQUIRE(metallicRoughness != kPrRenderMaterialTextureMissing);
    CHECK(gltfType.textures[metallicRoughness].kind == PrRenderTextureKind_Data);
    CHECK(gltfType.textures[metallicRoughness].standIn.y == 1);
    CHECK(gltfType.textures[metallicRoughness].standIn.z == 0);
    CHECK(gltfType.textures[prRenderMaterialTextureFromName(&gltfType, "baseColor")].kind == PrRenderTextureKind_Color);
    CHECK(gltfType.textures[prRenderMaterialTextureFromName(&gltfType, "normal")].standIn.x == 0.5f);
    CHECK(gltfType.textures[prRenderMaterialTextureFromName(&gltfType, "emissive")].standIn.x == 0);
    CHECK(prRenderMaterialTextureFromName(&gltfType, "sheenColor") == kPrRenderMaterialTextureMissing);
    const PrRenderMaterialType& unlit = set->materialTypes[prRenderMaterialTypeFromName(set, "Unlit")];
    CHECK(unlit.parameterBytes == 16);
    CHECK(prRenderMaterialParameterFromName(&unlit, "baseColor") == 0);
    CHECK(prRenderMaterialTextureFromName(&unlit, "baseColor") == 0);
    const PrRenderMaterialType& layered = set->materialTypes[prRenderMaterialTypeFromName(set, "KhronosPbrLayered")];
    CHECK(layered.shadingModel == "RenderPbrSurface");
    CHECK(layered.parameterBytes == 192);
    CHECK(layered.parameters[prRenderMaterialParameterFromName(&layered, "ior")].value.x == 1.5f);
    CHECK(layered.textureCount == 19);
    // A layer whose texture has not arrived is absent, and a specular texture that has not is the factor alone.
    CHECK(layered.textures[prRenderMaterialTextureFromName(&layered, "clearcoat")].standIn.x == 0);
    CHECK(layered.textures[prRenderMaterialTextureFromName(&layered, "transmission")].standIn.x == 0);
    CHECK(layered.textures[prRenderMaterialTextureFromName(&layered, "diffuseTransmission")].standIn.w == 0);
    CHECK(layered.textures[prRenderMaterialTextureFromName(&layered, "specular")].standIn.w == 1);
    const PrRenderMaterialType& preview = set->materialTypes[prRenderMaterialTypeFromName(set, "UsdPreview")];
    CHECK(preview.parameterBytes == 80);
    CHECK(preview.textures[prRenderMaterialTextureFromName(&preview, "roughness")].channels ==
        PrRenderChannelFlag_Chosen);
    CHECK(preview.textureCount == 10);
    // An input whose texture has not arrived is a flat, rough dielectric's, without emission.
    CHECK(preview.textures[prRenderMaterialTextureFromName(&preview, "normal")].standIn.z == 1);
    CHECK(preview.textures[prRenderMaterialTextureFromName(&preview, "roughness")].standIn.x == 1);
    CHECK(preview.textures[prRenderMaterialTextureFromName(&preview, "metallic")].standIn.x == 0);
    CHECK(preview.textures[prRenderMaterialTextureFromName(&preview, "emissiveColor")].standIn.x == 0);
    // The Khronos PBR types share one layout: two sets of coordinates and a color. The preview type declares an
    // optional emissive color beside them: its variant without it shares that layout, and its variant with it has a
    // layout of its own.
    REQUIRE(set->layoutCount == 2);
    uint32_t previewType = prRenderMaterialTypeFromName(set, "UsdPreview");
    REQUIRE(preview.optionalAttributeCount == 1);
    CHECK(preview.optionalAttributes[0] == "emissiveColor");
    CHECK(preview.variantCount == 2);
    CHECK(set->variantCount == 5);
    for (uint32_t i = 0; i < set->materialTypeCount; ++i)
        CHECK(prRenderMaterialTypeLayout(set, i, 0) == &set->layouts[0]);
    CHECK(prRenderMaterialTypeLayout(set, previewType, 1) == &set->layouts[1]);
    CHECK(set->variants[preview.firstVariant + 1].forwardFragment == "forwardPixelUsdPreview1");
    const PrRenderVertexLayout& previewLayout = set->layouts[1];
    REQUIRE(previewLayout.attributeCount == 4);
    CHECK(previewLayout.attributes[3].semantic == PrRenderSemantic_Custom);
    CHECK(previewLayout.attributes[3].name == "emissiveColor");
    CHECK(previewLayout.attributes[3].fallback.x == 1);
    const PrRenderVertexLayout& layout = set->layouts[0];
    CHECK(prRenderVertexAttributeFromName(&layout, "uv1") == 1);
    CHECK(layout.attributes[1].semantic == PrRenderSemantic_Uv);
    CHECK(layout.attributes[1].semanticIndex == 1);
    CHECK(layout.attributes[2].semantic == PrRenderSemantic_Color);
    CHECK(layout.attributes[2].format.component == PrMeshComponent_Uint8);
    CHECK(layout.attributes[2].fallback.w == 1);
    PrRenderUsdPreviewMaterial defaults = prRenderUsdPreviewMaterialDefault();
    CHECK(defaults.diffuseColor.x == 0.18f);
    CHECK(defaults.roughness == 0.5f);
    CHECK(defaults.ior == 1.5f);
    CHECK(defaults.normalTexture.scale[0] == 1);
}

TEST_CASE("an id names one object at a time, and is taken again only after a frame applies the destruction") {
    onEachDevice([](Fixture& f) {
        PrRenderMesh a = prRenderMeshCreate(f.render);
        PrRenderMesh b = prRenderMeshCreate(f.render);
        PrRenderMesh c = prRenderMeshCreate(f.render);
        CHECK(a.id == 1);
        CHECK(b.id == 2);
        CHECK(c.id == 3);
        // Each kind of object has ids of its own.
        CHECK(prRenderMaterialCreate(f.render).id == 1);
        CHECK(prRenderTextureCreate(f.render).id == 1);
        CHECK(prRenderSceneCreate(f.render).id == 1);

        prRenderMeshDestroy(f.render, b);
        CHECK(prRenderMeshCreate(f.render).id == 4);
        f.frame();
        CHECK(prRenderMeshCreate(f.render).id == 2);
        CHECK(prRenderMeshCreate(f.render).id == 5);
    });
}

TEST_CASE("a renderer counts its objects until a frame applies their destruction") {
    onEachDevice([](Fixture& f) {
        PrRenderInfo before = prRenderInfo(f.render);
        PrRenderMesh mesh = prRenderMeshCreate(f.render);
        PrRenderMaterial material = prRenderMaterialCreate(f.render);
        PrRenderTexture texture = prRenderTextureCreate(f.render);
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderInfo made = prRenderInfo(f.render);
        CHECK(made.meshCount == before.meshCount + 1);
        CHECK(made.materialCount == before.materialCount + 1);
        CHECK(made.textureCount == before.textureCount + 1);
        CHECK(made.environmentCount == before.environmentCount + 1);
        CHECK(made.sceneCount == before.sceneCount + 1);

        prRenderMeshDestroy(f.render, mesh);
        prRenderMaterialDestroy(f.render, material);
        prRenderTextureDestroy(f.render, texture);
        prRenderEnvironmentDestroy(f.render, environment);
        prRenderSceneDestroy(f.render, scene);
        CHECK(prRenderInfo(f.render).meshCount == made.meshCount);
        f.frame();
        PrRenderInfo after = prRenderInfo(f.render);
        CHECK(after.meshCount == before.meshCount);
        CHECK(after.materialCount == before.materialCount);
        CHECK(after.textureCount == before.textureCount);
        CHECK(after.environmentCount == before.environmentCount);
        CHECK(after.sceneCount == before.sceneCount);
    });
}

TEST_CASE("an update's changes take effect together, when a frame begins after its commit") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial red = createUnlit(f, {1, 0, 0, 1});
        PrRenderViewDesc view = frontView(scene);

        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance first = addInstance(update, quad, red, placed({0, 0, 0}));
        PrRenderInstance second = addInstance(update, quad, red, placed({0, 0, -1}));
        PrRenderLightDesc light = prRenderLightDescDefault();
        PrRenderLight sun = prRenderLightAdd(update, &light);
        CHECK(first.id == 1);
        CHECK(second.id == 2);
        CHECK(sun.id == 1);

        // An update that is open changes nothing.
        Image before = f.view(view);
        // The scene's instance records are a buffer of the frame's graph, which a pass of the application's may write.
        CHECK(prRenderSceneInstanceBuffer(f.render, scene).id != 0);
        CHECK(prRenderSceneInstanceBuffer(f.render, PrRenderScene{}).id == 0);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 0);
        CHECK(prRenderSceneLightCount(f.render, scene) == 0);
        CHECK(near(before.at(kCenter, kCenter), kBlack, 0));

        prRenderUpdateCommit(update);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 0);
        Image after = f.view(view);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 2);
        CHECK(prRenderSceneLightCount(f.render, scene) == 1);
        CHECK(near(after.at(kCenter, kCenter), kRed, 0));

        // A removed instance and light give their ids back once a frame has applied the removal.
        PrRenderUpdate* removal = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceRemove(removal, first);
        prRenderLightRemove(removal, sun);
        PrRenderInstance third = addInstance(removal, quad, red, placed({0, 0, -2}));
        CHECK(third.id == 3);
        prRenderUpdateCommit(removal);
        f.frame();
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 2);
        CHECK(prRenderSceneLightCount(f.render, scene) == 0);
        PrRenderUpdate* reuse = prRenderUpdateBegin(f.render, scene);
        CHECK(addInstance(reuse, quad, red, placed({0, 0, -3})).id == 1);
        CHECK(prRenderLightAdd(reuse, &light).id == 1);
        prRenderUpdateCommit(reuse);
        f.frame();
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 3);
    });
}

TEST_CASE("a discarded update changes nothing and gives its ids back") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial red = createUnlit(f, {1, 0, 0, 1});
        PrRenderUpdate* discarded = prRenderUpdateBegin(f.render, scene);
        CHECK(addInstance(discarded, quad, red, placed({0, 0, 0})).id == 1);
        prRenderUpdateDiscard(discarded);
        f.frame();
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 0);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        CHECK(addInstance(update, quad, red, placed({0, 0, 0})).id == 1);
        prRenderUpdateCommit(update);
        f.frame();
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 1);
    });
}

TEST_CASE("updates take effect in the order of their commits, and transforms are set in bulk") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial red = createUnlit(f, {1, 0, 0, 1});
        PrRenderMaterial green = createUnlit(f, {0, 1, 0, 1});
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance instances[2] = {
            addInstance(update, quad, red, placed({0, 0, -1})),
            addInstance(update, quad, green, placed({0, 0, -2})),
        };
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        CHECK(near(f.view(view).at(kCenter, kCenter), kRed, 0));

        // Two updates move both quads, and the one committed last decides where they are: green in front.
        PrAffine3 greenInFront[2] = {placed({0, 0, -2}), placed({0, 0, -1})};
        PrAffine3 redInFront[2] = {placed({0, 0, -1}), placed({0, 0, -3})};
        PrRenderUpdate* early = prRenderUpdateBegin(f.render, scene);
        PrRenderUpdate* late = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetTransforms(late, instances, greenInFront, 2);
        prRenderInstanceSetTransforms(early, instances, redInFront, 2);
        prRenderUpdateCommit(early);
        prRenderUpdateCommit(late);
        CHECK(near(f.view(view).at(kCenter, kCenter), kGreen, 0));

        // An instance takes a new material for a part, and stops being seen by a view whose mask it does not share.
        PrRenderUpdate* change = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetMaterial(change, instances[1], 0, red);
        prRenderUpdateCommit(change);
        CHECK(near(f.view(view).at(kCenter, kCenter), kRed, 0));
        PrRenderUpdate* hide = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetMasks(hide, instances[1], PrRenderMobilityFlag_Dynamic, 2);
        prRenderInstanceSetMasks(hide, instances[0], PrRenderMobilityFlag_Dynamic, 2);
        prRenderUpdateCommit(hide);
        view.mask = 1;
        CHECK(near(f.view(view).at(kCenter, kCenter), kBlack, 0));
        view.mask = 2;
        CHECK(near(f.view(view).at(kCenter, kCenter), kRed, 0));
    });
}

TEST_CASE("threads commit updates and create resources while frames render, and each update arrives whole") {
    if (!prThreadSupported())
        return;
    onEachDevice([](Fixture& f) {
        constexpr uint32_t kThreads = 3;
        constexpr uint32_t kUpdates = 40;
        constexpr uint32_t kInstances = 7;
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderViewDesc view = frontView(scene);
        view.extent = {8, 8};
        Producer producers[kThreads];
        PrThread threads[kThreads];
        for (uint32_t i = 0; i < kThreads; ++i) {
            producers[i].render = f.render;
            producers[i].scene = scene;
            producers[i].updates = kUpdates;
            producers[i].instances = kInstances;
            producers[i].done.store(false);
            prThreadStart(&threads[i], produce, &producers[i]);
        }
        // Frames render while the threads work. Each sees only whole updates.
        bool working = true;
        while (working) {
            working = false;
            for (const Producer& producer : producers)
                working = working || !producer.done.load();
            f.view(view);
            CHECK(prRenderSceneInstanceCount(f.render, scene) % kInstances == 0);
        }
        for (PrThread& thread : threads)
            prThreadJoin(&thread);
        f.view(view);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == kThreads * kUpdates * kInstances);
        // Each thread's mesh has data by now.
        for (uint32_t id = 1; id <= kThreads; ++id)
            CHECK(prRenderMeshIsReady(f.render, PrRenderMesh{id}));
    });
}

TEST_CASE("a frame renders with resources whose data has not arrived") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh mesh = prRenderMeshCreate(f.render);
        PrRenderTexture texture = prRenderTextureCreate(f.render);
        f.samplers.set(texture.id, prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Repeat));
        PrRenderMaterial material = prRenderMaterialCreate(f.render);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, mesh, material, placed({0, 0, 0}));
        prRenderUpdateCommit(update);
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        setEnvironment(f, scene, environment);
        PrRenderViewDesc view = frontView(scene);

        // The instance exists, and is not drawn while its mesh has no data. The environment has no light yet, so the
        // view shows black.
        Image dark = f.view(view);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 1);
        CHECK_FALSE(prRenderMeshIsReady(f.render, mesh));
        CHECK_FALSE(prRenderEnvironmentIsReady(f.render, environment));
        CHECK(prRenderEnvironmentInfo(f.render, environment).texture.id == 0);
        CHECK(near(dark.at(kCenter, kCenter), {0, 0, 0}, 0));

        // With light, the environment is the view's background.
        CubeTexels even = cubeOf(1, [](PrVec3) { return PrVec3{0.5f, 0.5f, 0.5f}; });
        PrRenderRadianceImage faces[6];
        for (uint32_t face = 0; face < 6; ++face) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(even.at(face, 0, 0));
            faces[face] = {.pixels = {data, 3 * sizeof(float), {1, 1}}, .channels = 3};
        }
        REQUIRE(prRenderEnvironmentSetCube(f.render, environment, faces) == PrStatus_Ok);
        CHECK_FALSE(prRenderEnvironmentIsReady(f.render, environment));
        Image empty = f.view(view);
        CHECK(prRenderEnvironmentIsReady(f.render, environment));
        CHECK(near(empty.at(kCenter, kCenter), {0.5f, 0.5f, 0.5f}, 1e-4f));

        // With data, the mesh is drawn with the neutral material, since its own is not set: a rough gray under the
        // environment.
        Array<FixtureVertex> vertices;
        for (uint32_t i = 0; i < 4; ++i) {
            FixtureVertex v;
            v.position = {i % 2 == 0 ? -1.0f : 1.0f, i < 2 ? 1.0f : -1.0f, 0};
            v.normal = {0, 0, 1};
            v.uv0 = {float(i % 2), float(i / 2)};
            vertices.push(v);
        }
        const uint32_t kSquare[6] = {0, 2, 3, 0, 3, 1};
        Array<uint32_t> indices;
        for (uint32_t index : kSquare)
            indices.push(index);
        PrRenderMeshPart part{.indexCount = 6};
        setMesh(f, mesh, vertices, indices, &part, 1);
        Image neutral = f.view(view);
        CHECK(prRenderMeshIsReady(f.render, mesh));
        const float* gray = neutral.at(kCenter, kCenter);
        CHECK(gray[0] > 0.2f);
        CHECK(gray[0] < 0.3f);
        CHECK(gray[0] == gray[1]);

        // A material whose texture has no texels reads the texture as white, and shows the texture's color once it has
        // texels.
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.color = {1, 0.5f, 0.25f, 1};
        unlit.colorTexture = useOf(f, texture);
        prRenderMaterialSetUnlit(f.render, material, &unlit);
        Image untextured = f.view(view);
        CHECK_FALSE(prRenderTextureIsReady(f.render, texture));
        CHECK(near(untextured.at(kCenter, kCenter), {1, 0.5f, 0.25f}, 1e-3f));
        const uint8_t texel[4] = {0, 255, 255, 255};
        PrRenderTextureData texels{.format = PrGpuFormat_Rgba8, .extent = {1, 1}, .levelCount = 1, .levels = {{texel, 0}}};
        REQUIRE(prRenderTextureSetData(f.render, texture, &texels) == PrStatus_Ok);
        Image textured = f.view(view);
        CHECK(prRenderTextureIsReady(f.render, texture));
        CHECK(near(textured.at(kCenter, kCenter), {0, 0.5f, 0.25f}, 1e-3f));

        // Destroyed resources stop being drawn, and the frame still renders.
        prRenderTextureDestroy(f.render, texture);
        CHECK(near(f.view(view).at(kCenter, kCenter), {1, 0.5f, 0.25f}, 1e-3f));
        prRenderMaterialDestroy(f.render, material);
        CHECK(f.view(view).at(kCenter, kCenter)[0] < 0.3f);
        prRenderMeshDestroy(f.render, mesh);
        CHECK(near(f.view(view).at(kCenter, kCenter), {0.5f, 0.5f, 0.5f}, 0));
    });
}

TEST_CASE("a destroyed scene renders nothing, and its id is taken again") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial red = createUnlit(f, {1, 0, 0, 1});
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, red, placed({0, 0, 0}));
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        CHECK(near(f.view(view).at(kCenter, kCenter), kRed, 0));
        prRenderSceneDestroy(f.render, scene);
        prGpuGraphBegin(f.graph);
        prRenderFrameBegin(f.render, f.graph);
        CHECK(prRenderViewAdd(f.render, &view).color.id == 0);
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 0);
        CHECK(prRenderSceneCreate(f.render).id == scene.id);
    });
}

TEST_CASE("a lost device leaves every call safe") {
    for (const TestDevice& shared : testDevices()) {
        PrGpuDeviceOptions options{.validation = true, .slotsOnly = shared.slotsOnly};
        PrGpuDevice* device = nullptr;
        REQUIRE(gpuDeviceCreate(shared.backend, &options, &device) == PrStatus_Ok);
        {
            TestDevice own{shared.backend, shared.slotsOnly, device};
            Fixture f(own);
            PrRenderScene scene = prRenderSceneCreate(f.render);
            PrRenderMesh quad = createQuad(f);
            PrRenderMaterial red = createUnlit(f, {1, 0, 0, 1});
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            addInstance(update, quad, red, placed({0, 0, 0}));
            prRenderUpdateCommit(update);
            PrRenderViewDesc view = frontView(scene);
            CHECK(near(f.view(view).at(kCenter, kCenter), kRed, 0));

            prGpuDeviceSimulateLoss(device);
            PrRenderMesh late = prRenderMeshCreate(f.render);
            PrRenderGeometryVertex vertices[3] = {};
            const uint32_t indices[3] = {0, 1, 2};
            PrRenderMeshPart part{.indexCount = 3};
            PrRenderMeshData data{
                .geometry = vertices,
                .vertexCount = 3,
                .indices = indices,
                .indexCount = 3,
                .parts = &part,
                .partCount = 1,
            };
            CHECK(prRenderMeshSetData(f.render, late, &data) != PrStatus_Ok);
            PrRenderUpdate* more = prRenderUpdateBegin(f.render, scene);
            addInstance(more, late, red, placed({0, 0, 0}));
            prRenderUpdateCommit(more);
            prGpuGraphBegin(f.graph);
            prRenderFrameBegin(f.render, f.graph);
            prRenderViewAdd(f.render, &view);
            PrGpuCommands* commands = nullptr;
            if (prGpuCommandsBegin(device, nullptr, &commands)) {
                prGpuGraphRecord(f.graph, commands);
                prGpuCommandsSubmit(commands);
            }
            CHECK(prRenderSceneInstanceCount(f.render, scene) == 2);

            // A renderer is not made on a lost device.
            PrRender* another = nullptr;
            CHECK(prRenderCreate(device, nullptr, &another) != PrStatus_Ok);
            CHECK(another == nullptr);
        }
        prGpuDeviceDestroy(device);
    }
}

TEST_SUITE_END();
