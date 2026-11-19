// The OpenPBR shading model (pr/render_openpbr.slang) against the equations that it states and against the library's
// Pbr model, drawn by both techniques on each device, with handles and with slots, as lib/render's tests draw their own
// shader set (lib/render/tests/test_render_custom.cpp).
//
// Where the two models are the same model, they draw the same pixels. Where OpenPBR's specification differs from
// glTF's, the difference is computed here from both, and the pixels must differ by it and by nothing more.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/render.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_shaders.h"
#include "pr/render_view.h"

#include "render_shaders/TestOpenPbrShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

constexpr double kPi = 3.14159265358979323846;

Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

PrRenderMaterialDesc openPbrDesc(const PrRenderParameterRecord& parameters) {
    return prRenderMaterialDescDefault(kTestOpenPbrMaterialOpenPbr, parameters.bytes, parameters.type->parameterBytes);
}

// An OpenPBR material of a base color, a metalness and a specular roughness, with every other parameter at the
// specification's default: an IOR of 1.5, and no coat, fuzz, thin film or emission.
PrRenderParameterRecord openPbrBase(float color, float metalness, float roughness) {
    PrRenderParameterRecord p(&testOpenPbrShaderSet()->materialTypes[kTestOpenPbrMaterialOpenPbr]);
    p.set("baseColor", {color, color, color}).set("baseMetalness", {metalness}).set("specularRoughness", {roughness});
    return p;
}

// The glTF material of the same base color, metalness and roughness.
PrRenderKhronosPbrMaterial gltfBase(float color, float metalness, float roughness) {
    PrRenderKhronosPbrMaterial m = prRenderKhronosPbrMaterialDefault();
    m.baseColor = {color, color, color, 1};
    m.metallic = metalness;
    m.roughness = roughness;
    return m;
}

// A scene of one quad that fills the front view, and the material that it shows.
struct QuadScene {
    PrRenderScene scene;
    PrRenderMaterial material;
    PrRenderViewDesc view;
};

QuadScene createQuadScene(Fixture& f) {
    QuadScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.material = prRenderMaterialCreate(f.render);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    addInstance(update, createQuad(f), s.material, placed({0, 0, 0}));
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    return s;
}

// A scene of a sphere of radius kSphereRadius about the origin, which a front view of kSphereExtent shows whole.
constexpr PrUVec2 kSphereExtent{97, 97};
constexpr float kSphereRadius = 0.8f;

QuadScene createSphereScene(Fixture& f) {
    QuadScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.material = prRenderMaterialCreate(f.render);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    addInstance(update, createSphere(f, 96), s.material, placed({0, 0, 0}, {
        kSphereRadius, kSphereRadius,
        kSphereRadius,
    }));
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    s.view.extent = kSphereExtent;
    return s;
}

// Returns the cosine between the eye and the sphere's normal at the point that a pixel of the sphere scene's view
// shows, or 0 where the pixel misses the sphere.
double sphereCosine(uint32_t x, uint32_t y) {
    PrVec3 eye{0, 0, kCameraDistance};
    PrVec3 through{(float(x) + 0.5f) / kSphereExtent.x * 2 - 1, 1 - (float(y) + 0.5f) / kSphereExtent.y * 2, 0};
    PrVec3 d = prVec3Normalize(through - eye);
    float b = prVec3Dot(eye, d);
    float discriminant = b * b - (prVec3Dot(eye, eye) - kSphereRadius * kSphereRadius);
    if (discriminant <= 0)
        return 0;
    PrVec3 n = (eye + d * (-b - prF32Sqrt(discriminant))) / kSphereRadius;
    return double(prVec3Dot(n, d * -1));
}

void setMaterial(Fixture& f, PrRenderMaterial material, const PrRenderMaterialDesc& desc) {
    prRenderMaterialSet(f.render, material, &desc);
}

void addSun(Fixture& f, PrRenderScene scene, PrVec3 direction, float lux) {
    PrRenderLightDesc light = prRenderLightDescDefault();
    light.direction = direction;
    light.intensity = lux;
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderLightAdd(update, &light);
    prRenderUpdateCommit(update);
}

// ---------------------------------------------------------------------------------------------------------------------
// The two models on the CPU, at a point that the eye sees along its normal

// Returns what a dielectric interface of the relative index `eta` reflects at the cosine `c`: Fresnel's equations.
double fresnelDielectric(double c, double eta) {
    double g = double(prF32Sqrt(float(eta * eta + c * c - 1)));
    double a = (g - c) / (g + c);
    double b = ((g + c) * c - 1) / ((g - c) * c + 1);
    return 0.5 * a * a * (1 + b * b);
}

double schlick(double f0, double c) {
    return f0 + (1 - f0) * double(prF32Pow(float(1 - c), 5));
}

// The GGX lobe of pr/render_brdf.slang's renderBrdfGgx, times the cosine of the light.
double ggx(double roughness, double nh, double nl, double nv) {
    double alpha = roughness * roughness > 0.002 ? roughness * roughness : 0.002;
    double alpha2 = alpha * alpha;
    double d = nh * nh * (alpha2 - 1) + 1;
    double distribution = alpha2 / (kPi * d * d);
    double visibility = 0.5 / (nl * double(prF32Sqrt(float(nv * nv * (1 - alpha2) + alpha2))) +
        nv * double(prF32Sqrt(float(nl * nl * (1 - alpha2) + alpha2))));
    return distribution * visibility * nl;
}

// Returns the factor by which OpenPBR scales a lobe's direct light for the light that bounces between microfacets more
// than once: 1 / (1 - mean Fresnel times the energy that one bounce loses), of the split sum at `nv`.
double compensation(double f0, double roughness, double nv) {
    SplitSum split = referenceSplitSum(roughness, nv);
    double lost = 1 - (split.scale + split.bias);
    return 1 / (1 - (f0 + (1 - f0) / 21) * lost);
}

// The radiance of a gray surface under a light of unit irradiance at the angle `theta` from the normal, seen along the
// normal, by each model: glTF's (pr/render_brdf.slang's renderBrdfDirect) and OpenPBR's (renderOpenPbrBrdf, without
// coat, fuzz or thin film, with an IOR of 1.5).
struct Direct {
    double gltf;
    double openPbr;
};

Direct directAlongNormal(double base, double metalness, double roughness, double theta) {
    double nl = double(prF32Cos(float(theta)));
    double vh = double(prF32Cos(float(theta / 2)));
    double lobe = ggx(roughness, vh, nl, 1);
    Direct out{};

    // glTF: Schlick's term of the mixed reflection at the half vector, over the dielectric's diffuse lobe under the
    // dielectric's term at the half vector.
    double f0 = 0.04 + (base - 0.04) * metalness;
    double dielectricTerm = schlick(0.04, vh);
    out.gltf = schlick(f0, vh) * lobe + (1 - dielectricTerm) * (1 - metalness) * base / kPi * nl;

    // OpenPBR: Fresnel's equations at the half vector for the dielectric and F82 with a white tint, which is Schlick's
    // term, for the metal, each scaled for further bounces; the diffuse lobe under the dielectric's albedo toward the
    // eye, which is the albedo scaling of the specification's "Layering".
    double dielectricF0 = 0.04;
    SplitSum split = referenceSplitSum(roughness, 1);
    double dielectricCompensation = compensation(dielectricF0, roughness, 1);
    double albedo = (dielectricF0 * split.scale + split.bias) * dielectricCompensation;
    double dielectric = fresnelDielectric(vh, 1.5) * lobe * dielectricCompensation + (1 - albedo) * base / kPi * nl;
    double metal = schlick(base, vh) * lobe * compensation(base, roughness, 1);
    out.openPbr = dielectric + (metal - dielectric) * metalness;
    return out;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render openpbr" * doctest::skip(!gpuTests()));

TEST_CASE("the OpenPBR shader set adds one material type and one shading model to the library's") {
    const PrRenderShaderSet* set = testOpenPbrShaderSet();
    CHECK(set->name == "TestOpenPbr");
    REQUIRE(set->materialTypeCount == kTestOpenPbrMaterialCount);
    CHECK(kTestOpenPbrMaterialCount == 5);
    const PrRenderMaterialType& surface = set->materialTypes[kTestOpenPbrMaterialOpenPbr];
    CHECK(surface.shadingModel == "RenderOpenPbrSurface");
    CHECK(surface.parameterBytes == 272);
    // Its parameters are the specification's inputs by name, run together, with the specification's defaults.
    CHECK(surface.parameters[prRenderMaterialParameterFromName(&surface, "specularIor")].value.x == 1.5f);
    CHECK(surface.parameters[prRenderMaterialParameterFromName(&surface, "baseColor")].value.x == 0.8f);
    CHECK(surface.textureCount == kTestOpenPbrOpenPbrTextureCount);
    CHECK(surface.textures[kTestOpenPbrOpenPbrTextureBaseColor].kind == PrRenderTextureKind_Color);
    CHECK(surface.textures[kTestOpenPbrOpenPbrTextureGeometryCoatNormal].standIn.z == 1);
    // OpenPBR is a type like any other, which a loader finds by its name, and shares the library's layout.
    CHECK(prRenderMaterialTypeFromName(set, "OpenPbr") == kTestOpenPbrMaterialOpenPbr);
    CHECK(prRenderMaterialTypeLayout(set, kTestOpenPbrMaterialOpenPbr, 0) ==
        prRenderMaterialTypeLayout(set, kTestOpenPbrMaterialKhronosPbr, 0));
}

TEST_CASE("OpenPBR draws what Pbr draws under an environment, where the two specifications agree") {
    // Seen within 20 degrees of the normal, Fresnel's equations for an IOR of 1.5 and Schlick's term of 0.04 differ by
    // less than 1e-5, and so does glTF's rise of Schlick's term with roughness, which grows as the fifth power of one
    // minus the cosine. The two models then share the split sum, the light of further bounces and the diffuse lobe's
    // share, so their pixels differ by the rounding of their arithmetic alone.
    onEachDevice([](Fixture& f) {
        QuadScene s = createQuadScene(f);
        setEnvironment(f, s.scene, createEnvironment(f, cubeOf(16, [](PrVec3 d) {
            float height = d.y * 0.5f + 0.5f;
            return PrVec3{0.2f + 0.6f * height, 0.25f + 0.5f * height, 0.3f + 0.9f * height * (d.x * 0.5f + 0.5f)};
        })));
        const float metalnesses[] = {0, 1};
        const float roughnesses[] = {0.1f, 0.45f, 1};
        for (float metalness : metalnesses) {
            for (float roughness : roughnesses) {
                CAPTURE(metalness);
                CAPTURE(roughness);
                for (PrRenderTechnique technique : techniques(f)) {
                    CAPTURE(technique);
                    s.view.technique = technique;
                    setMaterial(f, s.material, openPbrDesc(openPbrBase(0.6f, metalness, roughness)));
                    Image openPbr = f.view(s.view);
                    PrRenderKhronosPbrMaterial gltf = gltfBase(0.6f, metalness, roughness);
                    setGltf(f.render, s.material, &gltf);
                    Image pbr = f.view(s.view);
                    Difference difference = compare(openPbr, pbr, 1e-3f);
                    CAPTURE(difference.largest);
                    CHECK(difference.over == 0);
                    CHECK(openPbr.at(kCenter, kCenter)[0] > 0.1f);
                }
            }
        }
    }, testOpenPbrShaderSet());
}

TEST_CASE("under a light, OpenPBR differs from Pbr by what the two specifications differ by, and no more") {
    // At the quad's center the eye looks along the normal. There the models differ in three ways, which the CPU
    // computes for each: OpenPBR's dielectric reflects by Fresnel's equations where glTF's reflects by Schlick's term;
    // its diffuse lobe lies under the specular layer's albedo toward the eye, where glTF's lies under the Fresnel term
    // at each light's half vector; and it scales each specular lobe for the light that further bounces return, which
    // glTF leaves out of a light's part. The last is the largest: it brightens a metal's highlight by 4% at a roughness
    // of 0.3 and by 13% at 0.6, and a metal differs by it alone.
    onEachDevice([](Fixture& f) {
        QuadScene s = createQuadScene(f);
        constexpr float kLux = 2;
        const float thetas[] = {0, 0.5f, 1.05f};
        const float metalnesses[] = {0, 1};
        const float roughnesses[] = {0.3f, 0.6f};
        PrRenderLightDesc light = prRenderLightDescDefault();
        light.intensity = kLux;
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLight sun = prRenderLightAdd(update, &light);
        prRenderUpdateCommit(update);
        double largestRatio = 0;
        double smallestRatio = 2;
        for (float theta : thetas) {
            // The light comes from the right, at theta from the normal.
            light.direction = {-prF32Sin(theta), 0, -prF32Cos(theta)};
            update = prRenderUpdateBegin(f.render, s.scene);
            prRenderLightSet(update, sun, &light);
            prRenderUpdateCommit(update);
            for (float metalness : metalnesses) {
                for (float roughness : roughnesses) {
                    CAPTURE(theta);
                    CAPTURE(metalness);
                    CAPTURE(roughness);
                    Direct expected = directAlongNormal(0.6, double(metalness), double(roughness), double(theta));
                    for (PrRenderTechnique technique : techniques(f)) {
                        CAPTURE(technique);
                        s.view.technique = technique;
                        setMaterial(f, s.material, openPbrDesc(openPbrBase(0.6f, metalness, roughness)));
                        float openPbr = f.view(s.view).at(kCenter, kCenter)[0];
                        PrRenderKhronosPbrMaterial gltf = gltfBase(0.6f, metalness, roughness);
                        setGltf(f.render, s.material, &gltf);
                        float pbr = f.view(s.view).at(kCenter, kCenter)[0];
                        CAPTURE(openPbr);
                        CAPTURE(pbr);
                        // Each model draws its own equation, to the rounding of a half float.
                        CHECK(prF32Abs(openPbr - float(expected.openPbr * double(kLux))) <= 2e-3f * openPbr + 1e-4f);
                        CHECK(prF32Abs(pbr - float(expected.gltf * double(kLux))) <= 2e-3f * pbr + 1e-4f);
                        double ratio = double(openPbr / pbr);
                        if (metalness == 1) {
                            // A metal's Fresnel terms are the same, so the models differ by the compensation alone.
                            double scale = compensation(0.6, double(roughness), 1);
                            CHECK(prF64Abs(ratio - scale) <= 4e-3 * scale);
                        } else {
                            largestRatio = ratio > largestRatio ? ratio : largestRatio;
                            smallestRatio = ratio < smallestRatio ? ratio : smallestRatio;
                        }
                    }
                }
            }
        }
        // A dielectric's three differences are each small at these angles, and together within 2%.
        CAPTURE(smallestRatio);
        CAPTURE(largestRatio);
        CHECK(smallestRatio > 0.98);
        CHECK(largestRatio < 1.02);
    }, testOpenPbrShaderSet());
}

TEST_CASE("a white OpenPBR sphere in a white furnace returns all that it receives, with any coat and fuzz") {
    // Every layer reflects what it does not pass on, and passes the rest to a base that returns all it receives
    // (OpenPBR Surface, "White furnace testing"). The furnace behind the sphere is as bright, so every pixel of the
    // view is the furnace's radiance, at the sphere's edge too.
    struct Case {
        float metalness;
        float roughness;
        float coat;
        float coatRoughness;
        float fuzz;
        float fuzzRoughness;
    };
    const Case cases[] = {
        {0, 0.2f, 0, 0, 0, 0.5f},
        {0, 0.8f, 0, 0, 0, 0.5f},
        {1, 0.2f, 0, 0, 0, 0.5f},
        {1, 0.8f, 0, 0, 0, 0.5f},
        {0, 0.4f, 1, 0, 0, 0.5f},
        {1, 0.4f, 1, 0.5f, 0, 0.5f},
        {0, 0.4f, 0.5f, 0.2f, 0, 0.5f},
        {0, 0.4f, 0, 0, 1, 0.2f},
        {1, 0.3f, 0, 0, 1, 0.9f},
        {0, 0.5f, 1, 0.3f, 0.6f, 0.5f},
    };
    onEachDevice([&](Fixture& f) {
        QuadScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        for (const Case& c : cases) {
            CAPTURE(c.metalness);
            CAPTURE(c.roughness);
            CAPTURE(c.coat);
            CAPTURE(c.coatRoughness);
            CAPTURE(c.fuzz);
            CAPTURE(c.fuzzRoughness);
            PrRenderParameterRecord p = openPbrBase(1, c.metalness, c.roughness);
            p.set("coatWeight", {c.coat});
            p.set("coatRoughness", {c.coatRoughness});
            p.set("fuzzWeight", {c.fuzz});
            p.set("fuzzRoughness", {c.fuzzRoughness});
            setMaterial(f, s.material, openPbrDesc(p));
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                float lowest = 1e9f;
                float highest = 0;
                for (uint32_t i = 0; i < kSphereExtent.x * kSphereExtent.y; ++i) {
                    for (uint32_t channel = 0; channel < 3; ++channel) {
                        lowest = prF32Min(lowest, image.texels[i * 4 + channel]);
                        highest = prF32Max(highest, image.texels[i * 4 + channel]);
                    }
                }
                CAPTURE(lowest);
                CAPTURE(highest);
                CHECK(highest <= 1.002f);
                CHECK(lowest >= 0.998f);
            }
        }
    }, testOpenPbrShaderSet());
}

TEST_CASE("a smooth coat over a black base reflects what Fresnel's equations give for the coat's IOR") {
    // A base of no color and no specular weight reflects nothing, so the sphere shows the coat alone, which in an even
    // environment returns its directional albedo. A smooth coat's albedo is the reflectance of a dielectric interface
    // at the eye's angle. The model's albedo is the split sum's, with Schlick's term replaced by Fresnel's equations in
    // the mirror direction, so it departs from them only by the split sum's table at a roughness of 0
    // (pr/render_shading.slang), which the test computes from the table.
    onEachDevice([](Fixture& f) {
        QuadScene s = createSphereScene(f);
        constexpr float kRadiance = 1;
        setEnvironment(f, s.scene, createEvenEnvironment(f, {kRadiance, kRadiance, kRadiance}));
        const float iors[] = {1.6f, 2.2f};
        for (float ior : iors) {
            CAPTURE(ior);
            PrRenderParameterRecord p = openPbrBase(0, 0, 0.5f);
            p.set("specularWeight", {0});
            p.set("coatWeight", {1});
            p.set("coatRoughness", {0});
            p.set("coatIor", {ior});
            setMaterial(f, s.material, openPbrDesc(p));
            double eta = double(ior);
            double f0 = ((eta - 1) / (eta + 1)) * ((eta - 1) / (eta + 1));
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                uint32_t wrong = 0;
                uint32_t counted = 0;
                double largest = 0;
                for (uint32_t y = 0; y < kSphereExtent.y; ++y) {
                    for (uint32_t x = 0; x < kSphereExtent.x; ++x) {
                        double nv = sphereCosine(x, y);
                        // Near the sphere's edge the mesh's facets are not the sphere.
                        if (nv < 0.25)
                            continue;
                        double expected = fresnelDielectric(nv, eta) * double(kRadiance);
                        // The table's departure from Schlick's term at a roughness of 0, as a part of the term, which
                        // carries over to the result, and the light that the table lets bounce again.
                        SplitSum split = referenceSplitSum(0, nv);
                        double schlicks = schlick(f0, nv);
                        double fitted = f0 * split.scale + split.bias;
                        double lost = prF64Abs(1 - (split.scale + split.bias));
                        double fit = expected * (prF64Abs(fitted - schlicks) / schlicks + (f0 + (1 - f0) / 21) * lost);
                        double tolerance = fit + 2e-3 * expected + 1e-3;
                        double got = double(image.at(x, y)[0]);
                        double off = prF64Abs(got - expected);
                        largest = off > largest ? off : largest;
                        ++counted;
                        if (off > tolerance)
                            ++wrong;
                    }
                }
                CAPTURE(largest);
                CHECK(counted > 4000);
                CHECK(wrong == 0);
            }
        }
    }, testOpenPbrShaderSet());
}

TEST_CASE("OpenPBR's emission is its color times its luminance, under a coat that passes what it does not reflect") {
    onEachDevice([](Fixture& f) {
        QuadScene s = createQuadScene(f);
        const PrVec3 color{1, 0.5f, 0.25f};
        constexpr float kLuminance = 1.5f;
        const float coats[] = {0, 0.5f, 1};
        for (float coat : coats) {
            CAPTURE(coat);
            PrRenderParameterRecord p = openPbrBase(0, 0, 0.5f);
            p.set("specularWeight", {0});
            p.set("emissionColor", {color.x, color.y, color.z}).set("emissionLuminance", {kLuminance});
            p.set("coatColor", {1, 0.8f, 0.6f}).set("coatWeight", {coat});
            setMaterial(f, s.material, openPbrDesc(p));
            // The coat reflects (IOR - 1)^2 / (IOR + 1)^2 of the emission back at normal incidence, and absorbs by its
            // color.
            float passed = 1 - (0.6f / 2.6f) * (0.6f / 2.6f);
            PrVec3 coated{p.value("coatColor", 0) * passed, p.value("coatColor", 1) * passed, p.value("coatColor", 2) * passed};
            PrVec3 expected{
                color.x * kLuminance * (1 + (coated.x - 1) * coat),
                color.y * kLuminance * (1 + (coated.y - 1) * coat),
                color.z * kLuminance * (1 + (coated.z - 1) * coat),
            };
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                CHECK(near(image.at(kCenter, kCenter), expected, 3e-3f));
                CHECK(near(image.at(2, 30), expected, 3e-3f));
            }
        }
    }, testOpenPbrShaderSet());
}

TEST_CASE("an OpenPBR scene draws alike with slots and with bindless") {
    const Array<TestDevice>& devices = testDevices();
    for (uint32_t i = 0; i + 1 < devices.count(); ++i) {
        if (devices[i].backend != devices[i + 1].backend || devices[i].slotsOnly || !devices[i + 1].slotsOnly)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        Image images[2];
        for (uint32_t tier = 0; tier < 2; ++tier) {
            Fixture f(devices[i + tier], testOpenPbrShaderSet());
            QuadScene s = createSphereScene(f);
            PrRenderParameterRecord p = openPbrBase(0.5f, 0.3f, 0.4f);
            p.set("coatWeight", {0.7f});
            p.set("fuzzWeight", {0.5f});
            p.set("thinFilmWeight", {1});
            p.set("emissionLuminance", {0.1f});
            setMaterial(f, s.material, openPbrDesc(p));
            setEnvironment(f, s.scene, createEvenEnvironment(f, {0.3f, 0.4f, 0.6f}));
            addSun(f, s.scene, prVec3Normalize({-0.3f, -0.5f, -1}), 3);
            images[tier] = f.view(s.view);
        }
        Difference difference = compare(images[0], images[1], 0);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
    }
}

// The last case of the binary, in the order in which the cases run.
TEST_CASE("the devices that the tests share are destroyed") {
    destroyTestDevices();
    CHECK(testDevices().count() == 0);
}

TEST_CASE("a white OpenPBR sphere returns all of a white furnace with each input that this renderer degrades") {
    // The inputs that the model added after its first form: a rough diffuse lobe, anisotropy of the base and of the
    // coat, a translucent base, a subsurface, and a thin wall. Each keeps the model's layering, so each returns all
    // that it receives of an even light.
    struct Case {
        PrStringView name;
        void (*set)(PrRenderParameterRecord* p);
    };
    const Case cases[] = {
        {"diffuse roughness", [](PrRenderParameterRecord* p) { p->set("baseDiffuseRoughness", {1}); }},
        {
            "anisotropy",
            [](PrRenderParameterRecord* p) {
                p->set("baseMetalness", {1});
                p->set("specularRoughnessAnisotropy", {0.8f});
                p->set("geometryTangentRotation", {0.6f});
            },
        },
        {
            "coat anisotropy",
            [](PrRenderParameterRecord* p) {
                p->set("coatWeight", {1});
                p->set("coatRoughness", {0.4f});
                p->set("coatRoughnessAnisotropy", {0.7f});
            },
        },
        {
            "transmission",
            [](PrRenderParameterRecord* p) {
                p->set("transmissionWeight", {1});
                p->set("thickness", {0.5f});
            },
        },
        {
            "dispersion",
            [](PrRenderParameterRecord* p) {
                p->set("transmissionWeight", {0.7f});
                p->set("thickness", {0.5f});
                p->set("transmissionDispersionScale", {1});
            },
        },
        {"subsurface", [](PrRenderParameterRecord* p) { p->set("subsurfaceColor", {1, 1, 1}).set("subsurfaceWeight", {1}); }},
        {
            "thin-walled subsurface",
            [](PrRenderParameterRecord* p) {
                p->set("subsurfaceColor", {1, 1, 1}).set("subsurfaceWeight", {0.6f});
                p->set("geometryThinWalled", {1});
            },
        },
        {
            "thin-walled transmission",
            [](PrRenderParameterRecord* p) {
                p->set("transmissionWeight", {1});
                p->set("geometryThinWalled", {1});
            },
        },
    };
    onEachDevice([&](Fixture& f) {
        QuadScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        for (const Case& c : cases) {
            CAPTURE(c.name);
            PrRenderParameterRecord p = openPbrBase(1, 0, 0.4f);
            c.set(&p);
            PrRenderMaterialDesc desc = openPbrDesc(p);
            desc.flags = p.value("transmissionWeight") > 0 ? PrRenderMaterialFlag_ReadsBehind : 0;
            setMaterial(f, s.material, desc);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                float lowest = 1e9f;
                float highest = 0;
                for (uint32_t i = 0; i < kSphereExtent.x * kSphereExtent.y; ++i) {
                    lowest = prF32Min(lowest, image.texels[i * 4 + 1]);
                    highest = prF32Max(highest, image.texels[i * 4 + 1]);
                }
                CAPTURE(lowest);
                CAPTURE(highest);
                CHECK(highest <= 1.002f);
                CHECK(lowest >= 0.998f);
            }
        }
    }, testOpenPbrShaderSet());
}

TEST_CASE("OpenPBR's translucent base and glTF's transmission show one slab alike, dimmed by Beer's law") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMaterial slab = prRenderMaterialCreate(f.render);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, createQuad(f), createUnlit(f, {0.9f, 0.6f, 0.3f, 1}), placed({0, 0, -1}, {2, 2, 2}));
        addInstance(update, createQuad(f), slab, placed({0, 0, 0}, {0.5f, 0.5f, 0.5f}));
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        const float color[3] = {0.7f, 0.4f, 0.2f};
        const float backdrop[3] = {0.9f, 0.6f, 0.3f};
        const float thicknesses[] = {0, 1, 2.5f};
        for (float thickness : thicknesses) {
            CAPTURE(thickness);
            PrRenderParameterRecord p = openPbrBase(1, 0, 0);
            p.set("transmissionColor", {color[0], color[1], color[2]}).set("transmissionWeight", {1});
            p.set("transmissionDepth", {0.8f});
            p.set("thickness", {thickness});
            PrRenderMaterialDesc desc = openPbrDesc(p);
            desc.flags |= PrRenderMaterialFlag_ReadsBehind;
            setMaterial(f, slab, desc);
            Image openPbr = f.view(view);

            PrRenderKhronosPbrMaterial gltf = gltfBase(1, 0, 0);
            gltf.transmission = 1;
            gltf.thickness = thickness;
            gltf.attenuationDistance = 0.8f;
            gltf.attenuationColor = {color[0], color[1], color[2]};
            setGltf(f.render, slab, &gltf);
            Image pbr = f.view(view);
            for (uint32_t channel = 0; channel < 3; ++channel) {
                CAPTURE(channel);
                // "Translucent base": the medium's extinction is that for which white light has transmission_color
                // after transmission_depth. The slab is drawn at half its size, so its thickness in the world is half
                // of the material's. Seen along the normal, the interface of an IOR of 1.5 reflects 4% of a black
                // environment and passes the rest.
                double path = double(thickness) * 0.5;
                double transmittance = double(prF32Pow(color[channel], float(path / 0.8)));
                double expected = 0.96 * transmittance * double(backdrop[channel]);
                CHECK(double(openPbr.at(kCenter, kCenter)[channel]) == doctest::Approx(expected).epsilon(0.01));
                // The two specifications agree on the slab: KHR_materials_volume's attenuation is the same law.
                CHECK(openPbr.at(kCenter, kCenter)[channel] ==
                    doctest::Approx(pbr.at(kCenter, kCenter)[channel]).epsilon(0.005));
            }
        }
        // Without a depth the medium is absent, and the color tints what passes, which glTF has no word for.
        PrRenderParameterRecord tinted = openPbrBase(1, 0, 0);
        tinted.set("transmissionColor", {color[0], color[1], color[2]}).set("transmissionWeight", {1});
        tinted.set("thickness", {2});
        PrRenderMaterialDesc tintedDesc = openPbrDesc(tinted);
        tintedDesc.flags |= PrRenderMaterialFlag_ReadsBehind;
        setMaterial(f, slab, tintedDesc);
        CHECK(double(f.view(view).at(kCenter, kCenter)[1]) ==
            doctest::Approx(0.96 * double(color[1]) * double(backdrop[1])).epsilon(0.01));
        prRenderSceneDestroy(f.render, scene);
    }, testOpenPbrShaderSet());
}

TEST_CASE("OpenPBR's rough diffuse lobe and its thin-walled subsurface are the lobes that the specification gives") {
    onEachDevice([](Fixture& f) {
        QuadScene s = createQuadScene(f);
        constexpr float kLight = 3;
        // The light shines along the view's axis, so at the view's middle the light, the eye and the normal are one.
        addSun(f, s.scene, {0, 0, -1}, kLight);
        const float roughnesses[] = {0, 0.5f, 1};
        for (float roughness : roughnesses) {
            CAPTURE(roughness);
            PrRenderParameterRecord p = openPbrBase(0.6f, 0, 0.5f);
            // Without a specular layer, the diffuse lobe is all that the surface reflects.
            p.set("specularWeight", {0});
            p.set("baseDiffuseRoughness", {roughness});
            setMaterial(f, s.material, openPbrDesc(p));
            // "Glossy-diffuse", the energy-preserving Oren-Nayar model: with the light and the eye along the normal,
            // the single-scattering lobe is rho A / pi with A = 1 / (1 + (1/2 - 2 / (3 pi)) sigma), and the lobe of the
            // further bounces is rho_ms (1 - E(1))^2 / (pi (1 - E_avg)), where E(1) = A at the normal.
            double sigma = double(roughness);
            double rho = 0.6;
            double a = 1 / (1 + (0.5 - 2 / (3 * kPi)) * sigma);
            double average = a * (1 + (2.0 / 3 - 28 / (15 * kPi)) * sigma);
            double multiColor = rho * rho * average / (1 - rho * (1 - average));
            double multiple = roughness > 0 ? multiColor * (1 - a) * (1 - a) / (1 - average) : 0;
            double expected = double(kLight) * (rho * a + multiple) / kPi;
            CHECK(double(f.view(s.view).at(kCenter, kCenter)[0]) == doctest::Approx(expected).epsilon(0.01));
        }

        // "Thin-walled mode": the subsurface of a thin wall is a diffuse lobe of its color, half reflected and half
        // transmitted. So the wall is as bright lit from behind as from before, at half of what the color reflects.
        PrRenderParameterRecord p = openPbrBase(0.6f, 0, 0.5f);
        p.set("specularWeight", {0});
        p.set("subsurfaceColor", {0.8f, 0.4f, 0.2f}).set("subsurfaceWeight", {1});
        p.set("geometryThinWalled", {1});
        setMaterial(f, s.material, openPbrDesc(p));
        Image front = f.view(s.view);
        CHECK(double(front.at(kCenter, kCenter)[0]) == doctest::Approx(double(kLight) * 0.8 * 0.5 / kPi).epsilon(0.01));
        CHECK(double(front.at(kCenter, kCenter)[2]) == doctest::Approx(double(kLight) * 0.2 * 0.5 / kPi).epsilon(0.01));
        QuadScene back = createQuadScene(f);
        addSun(f, back.scene, {0, 0, 1}, kLight);
        setMaterial(f, back.material, openPbrDesc(p));
        Image behind = f.view(back.view);
        CHECK(behind.at(kCenter, kCenter)[0] == doctest::Approx(front.at(kCenter, kCenter)[0]).epsilon(0.01));
        // Without the thin wall, the subsurface reflects all of its color and nothing passes.
        p.set("geometryThinWalled", {0});
        setMaterial(f, back.material, openPbrDesc(p));
        CHECK(f.view(back.view).at(kCenter, kCenter)[0] < 1e-4f);
        setMaterial(f, s.material, openPbrDesc(p));
        CHECK(double(f.view(s.view).at(kCenter, kCenter)[0]) ==
            doctest::Approx(double(kLight) * 0.8 / kPi).epsilon(0.01));
    }, testOpenPbrShaderSet());
}

TEST_CASE("a coat's own normal map bends the coat's highlight and leaves the base's") {
    onEachDevice([](Fixture& f) {
        QuadScene s = createQuadScene(f);
        // One texel of a normal that leans toward +x, as a tangent-space normal map holds it.
        const uint8_t texel[4] = {200, 128, 219, 255};
        PrRenderTexture leaning = createTexture(f, PrUVec2{1, 1}, texel, TextureEncoding_Linear);
        setEnvironment(f, s.scene, createEnvironment(f, cubeOf(16, [](PrVec3 d) {
            return PrVec3{d.x > 0.3f ? 1.0f : 0.0f, 0, 0};
        })));
        PrRenderParameterRecord p = openPbrBase(0, 0, 0.6f);
        p.set("specularWeight", {0});
        p.set("coatWeight", {1});
        p.set("coatRoughness", {0});
        PrRenderTextureUse textures[kTestOpenPbrOpenPbrTextureCount];
        for (PrRenderTextureUse& use : textures)
            use = useOf(f, PrRenderTexture{});
        PrRenderMaterialDesc desc = openPbrDesc(p);
        desc.textures = textures;
        desc.textureCount = kTestOpenPbrOpenPbrTextureCount;
        // A normal map's texels map [0, 1] to [-1, 1], by the use's scale and bias.
        PrRenderTextureUse normal = useOf(f, leaning);
        for (uint32_t c = 0; c < 3; ++c) {
            normal.scale[c] = 2;
            normal.bias[c] = -1;
        }
        // The base's normal map alone: the coat mirrors the environment straight back, which is dark there.
        textures[kTestOpenPbrOpenPbrTextureGeometryNormal] = normal;
        setMaterial(f, s.material, desc);
        float flat = f.view(s.view).at(kCenter, kCenter)[0];
        // The coat's own map: the coat mirrors the bright side.
        textures[kTestOpenPbrOpenPbrTextureGeometryNormal] = useOf(f, PrRenderTexture{});
        textures[kTestOpenPbrOpenPbrTextureGeometryCoatNormal] = normal;
        setMaterial(f, s.material, desc);
        float bent = f.view(s.view).at(kCenter, kCenter)[0];
        CHECK(flat < 0.005f);
        CHECK(bent > 0.03f);
    }, testOpenPbrShaderSet());
}

TEST_SUITE_END();
