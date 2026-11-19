// The layers that Khronos's material extensions add to the PBR model, against their equations: each layer in a white
// furnace, the sheen's albedo against the lobe's integral, the anisotropic lobe against its distribution, a coat over
// black against its Fresnel term, a thin film against the Airy sum, a slab against Beer's law, the specular-glossiness
// workflow against the metallic-roughness one where the two coincide, and a scene of layered materials between binding
// tiers, backends and techniques.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_shaders.h"
#include "pr/render_view.h"

#include "render/tests/kTestRenderLayerShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

constexpr double kPi = 3.14159265358979323846;

// The layers of test_render_layers.slang.
enum {
    Layer_None,
    Layer_Clearcoat,
    Layer_Sheen,
    Layer_Iridescence,
    Layer_Anisotropy,
    Layer_DiffuseTransmission,
    Layer_Transmission,
    Layer_Count,
};
typedef uint32_t Layer;

constexpr uint32_t kSide = 8;
constexpr float kLowRoughness = 0.35f;

// Draws test_render_layers.slang's probe of a surface with one layer, and returns its target.
struct LayerProbe {
    Fixture& f;
    PrGpuShaderLibrary* library = nullptr;
    PrGpuPipeline pipeline{};
    PrGpuTexture target{};

    explicit LayerProbe(Fixture& fixture) : f(fixture) {
        REQUIRE(prGpuShaderLibraryCreate(f.device, kTestRenderLayerShaders, sizeof(kTestRenderLayerShaders),
            &library) == PrStatus_Ok);
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = "layerVertex";
        desc.fragment = "layerPixel";
        desc.formats.colorCount = 1;
        desc.formats.color[0] = PrGpuFormat_Rgba16F;
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &pipeline) == PrStatus_Ok);
        PrGpuTextureDesc targetDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, PrUVec2{kSide, kSide},
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        REQUIRE(prGpuTextureCreate(f.device, &targetDesc, &target) == PrStatus_Ok);
    }

    ~LayerProbe() {
        prGpuTextureRelease(f.device, target);
        prGpuPipelineRelease(f.device, pipeline);
        prGpuShaderLibraryDestroy(library);
    }

    Image draw(Layer layer, float metallic, float value) {
        PrGpuCommands* commands = nullptr;
        REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
        PrGpuSubresources all{};
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead, PrGpuAccessFlag_ColorAttachment);
        PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
        PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
        const float push[8] = {float(layer), float(kSide), kLowRoughness, metallic, value, 0, 0, 0};
        prGpuCommandsSetPipeline(commands, pipeline);
        prGpuCommandsSetPush(commands, push, sizeof(push));
        prGpuPassDraw(pass, 3, 1, 0, 0);
        prGpuPassEnd(pass);
        prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment, PrGpuAccessFlag_ShaderRead);
        Image image;
        prGpuCommandsReadTexture(commands, target, PrGpuAccessFlag_ShaderRead, readHalves, &image);
        gpuSubmitAndWait(commands);
        return image;
    }
};

// The roughness and the cosine of the eye's angle at a pixel of the probe's target.
double probeRoughness(uint32_t x) {
    return double(kLowRoughness) + (1 - double(kLowRoughness)) * (x + 0.5) / kSide;
}

double probeCosine(uint32_t y) {
    return (y + 0.5) / kSide;
}

// Returns the sheen's albedo that the library's table holds for a roughness and a cosine, read between its entries as
// the shaders read it (renderSheenAlbedo).
double tableSheenAlbedo(double roughness, double nv) {
    constexpr uint32_t kTable = 32;
    static Array<float> texels;
    if (texels.count() == 0) {
        texels.resize(kTable * kTable * 4);
        prRenderSplitSumTable(nullptr, PrUVec2{kTable, kTable}, texels.begin());
    }
    double u = roughness * (kTable - 1);
    double v = prF64Sqrt(nv) * (kTable - 1);
    uint32_t x = uint32_t(prF64Min(prF64Floor(u), kTable - 2));
    uint32_t y = uint32_t(prF64Min(prF64Floor(v), kTable - 2));
    double fx = u - x;
    double fy = v - y;
    auto at = [&](uint32_t column, uint32_t row) { return double(texels[(row * kTable + column) * 4 + 2]); };
    double low = at(x, y) + (at(x + 1, y) - at(x, y)) * fx;
    double high = at(x, y + 1) + (at(x + 1, y + 1) - at(x, y + 1)) * fx;
    return low + (high - low) * fy;
}

// Returns the anisotropic GGX lobe without its Fresnel term, as KHR_materials_anisotropy defines it ("Implementation":
// D of Burley's addenda and V of Heitz), for directions in the frame of the tangent, the bitangent and the normal.
double anisotropicLobe(double at, double ab, const double* v, const double* l) {
    double h[3] = {v[0] + l[0], v[1] + l[1], v[2] + l[2]};
    double length = prF64Sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]);
    for (double& c : h)
        c /= length;
    double a2 = at * ab;
    double f[3] = {ab * h[0], at * h[1], a2 * h[2]};
    double w2 = a2 / (f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    double d = a2 * w2 * w2 / kPi;
    double ggxV = l[2] * prF64Sqrt(at * at * v[0] * v[0] + ab * ab * v[1] * v[1] + v[2] * v[2]);
    double ggxL = v[2] * prF64Sqrt(at * at * l[0] * l[0] + ab * ab * l[1] * l[1] + l[2] * l[2]);
    return d * 0.5 / (ggxV + ggxL);
}

// Returns one channel of what a thin film reflects, as KHR_materials_iridescence defines it ("Implementation",
// "Iridescence Fresnel"), in double precision: the Airy sum of the film's reflections over a base that reflects
// `baseF0` at normal incidence, seen at the cosine `cosine`, for a film of the index `filmIor` and `thickness`
// nanometers, in linear sRGB.
void referenceIridescence(double filmIor, double cosine, double thickness, double baseF0, double* rgb) {
    auto smoothstep = [](double a, double b, double x) {
        double t = prF64Min(prF64Max((x - a) / (b - a), 0), 1);
        return t * t * (3 - 2 * t);
    };
    auto schlick = [](double f0, double c) { return f0 + (1 - f0) * prF64Pow(1 - c, 5); };
    double ior = 1 + (filmIor - 1) * smoothstep(0, 0.03, thickness);
    double sin2 = (1 / ior) * (1 / ior) * (1 - cosine * cosine);
    double cos2 = 1 - sin2;
    if (cos2 < 0) {
        rgb[0] = rgb[1] = rgb[2] = 1;
        return;
    }
    double cosFilm = prF64Sqrt(cos2);
    double r0 = ((ior - 1) / (ior + 1)) * ((ior - 1) / (ior + 1));
    double r12 = schlick(r0, cosine);
    double t121 = 1 - r12;
    double phi12 = ior < 1 ? kPi : 0;
    double phi21 = kPi - phi12;
    double root = prF64Sqrt(prF64Min(baseF0, 0.9999));
    double baseIor = (1 + root) / (1 - root);
    double r1 = ((baseIor - ior) / (baseIor + ior)) * ((baseIor - ior) / (baseIor + ior));
    double r23 = schlick(r1, cosFilm);
    double phi23 = baseIor < ior ? kPi : 0;
    double opd = 2 * ior * thickness * cosFilm;
    double phi = phi21 + phi23;
    double r123Squared = prF64Min(prF64Max(r12 * r23, 1e-5), 0.9999);
    double r123 = prF64Sqrt(r123Squared);
    double rs = t121 * t121 * r23 / (1 - r123Squared);
    double sum[3] = {r12 + rs, r12 + rs, r12 + rs};
    double cm = rs - t121;
    for (int m = 1; m <= 2; ++m) {
        cm *= r123;
        // The sensitivity of the eye in Fourier space (Belcour and Barla's fits).
        double phase = 2 * kPi * (m * opd) * 1.0e-9;
        double shift = m * phi;
        const double value[3] = {5.4856e-13, 4.4201e-13, 5.2481e-13};
        const double position[3] = {1.6810e+06, 1.7953e+06, 2.2084e+06};
        const double variance[3] = {4.3278e+09, 9.3046e+09, 6.6121e+09};
        double xyz[3];
        for (int i = 0; i < 3; ++i) {
            xyz[i] = value[i] * prF64Sqrt(2 * kPi * variance[i]) * prF64Cos(position[i] * phase + shift) *
                prF64Exp(-phase * phase * variance[i]);
        }
        xyz[0] += 9.7470e-14 * prF64Sqrt(2 * kPi * 4.5282e+09) * prF64Cos(2.2399e+06 * phase + shift) *
            prF64Exp(-4.5282e+09 * phase * phase);
        for (double& c : xyz)
            c /= 1.0685e-7;
        const double toRgb[3][3] = {
            {3.2404542, -1.5371385, -0.4985314},
            {-0.9692660, 1.8760108, 0.0415560},
            {0.0556434, -0.2040259, 1.0572252},
        };
        for (int i = 0; i < 3; ++i)
            sum[i] += cm * 2 * (toRgb[i][0] * xyz[0] + toRgb[i][1] * xyz[1] + toRgb[i][2] * xyz[2]);
    }
    for (int i = 0; i < 3; ++i)
        rgb[i] = prF64Max(sum[i], 0);
}

// A scene of one unit sphere at the origin, seen from the front.
struct SphereScene {
    PrRenderScene scene;
    PrRenderMaterial material;
    PrRenderViewDesc view;
};

SphereScene createSphereScene(Fixture& f) {
    SphereScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.material = prRenderMaterialCreate(f.render);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    addInstance(update, createSphere(f), s.material, placed({0, 0, 0}));
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    return s;
}

// Returns the cosine of the angle between the normal and the eye at the point of the unit sphere that a pixel of the
// front view shows, or a negative number for a pixel that shows no sphere.
double sphereCosine(uint32_t x, uint32_t y) {
    double px = (x + 0.5) / kExtent.x * 2 - 1;
    double py = 1 - (y + 0.5) / kExtent.y * 2;
    double length = prF64Sqrt(px * px + py * py + double(kCameraDistance) * double(kCameraDistance));
    double ray[3] = {px / length, py / length, -double(kCameraDistance) / length};
    double distance = double(kCameraDistance);
    double b = distance * ray[2];
    double discriminant = b * b - (distance * distance - 1);
    if (discriminant <= 0)
        return -1;
    double t = -b - prF64Sqrt(discriminant);
    double normal[3] = {ray[0] * t, ray[1] * t, distance + ray[2] * t};
    return -(normal[0] * ray[0] + normal[1] * ray[1] + normal[2] * ray[2]);
}

Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

// Returns a white glTF material with one layer, as test_render_layers.slang's layerSurface makes a surface.
PrRenderKhronosPbrMaterial layeredMaterial(Layer layer, float metallic, float roughness) {
    PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
    material.metallic = metallic;
    material.roughness = roughness;
    switch (layer) {
        case Layer_Clearcoat:
            material.clearcoat = 1;
            material.clearcoatRoughness = roughness;
            break;
        case Layer_Sheen:
            material.sheenColor = {1, 1, 1};
            material.sheenRoughness = roughness;
            break;
        case Layer_Iridescence:
            material.iridescence = 1;
            break;
        case Layer_Anisotropy:
            material.anisotropy = 0.8f;
            break;
        case Layer_DiffuseTransmission:
            material.diffuseTransmission = 0.5f;
            break;
        case Layer_Transmission:
            material.transmission = 1;
            material.thickness = 0.5f;
            break;
        default:
            break;
    }
    return material;
}

// A row of spheres, one for each layer, on a ground, under a sun that casts shadows and an environment that differs in
// every direction.
PrRenderViewDesc createLayeredScene(Fixture& f) {
    PrRenderScene scene = prRenderSceneCreate(f.render);
    PrRenderMesh sphere = createSphere(f, 24);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    for (uint32_t layer = 0; layer < Layer_Count; ++layer) {
        for (uint32_t row = 0; row < 2; ++row) {
            PrRenderKhronosPbrMaterial material = layeredMaterial(Layer(layer), row == 0 ? 0.0f : 1.0f, 0.4f);
            material.baseColor = {0.8f, 0.5f + 0.05f * float(layer), 0.3f, 1};
            PrVec3 at{(float(layer) - 3) * 0.28f, row == 0 ? 0.3f : -0.3f, 0};
            addInstance(update, sphere, createMaterial(f, material), placed(at, {0.13f, 0.13f, 0.13f}));
        }
    }
    PrRenderKhronosPbrMaterial ground = prRenderKhronosPbrMaterialDefault();
    ground.metallic = 0;
    addInstance(update, createQuad(f), createMaterial(f, ground), placed({0, 0, -0.2f}));
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = {-0.4f, -0.5f, -1};
    sun.intensity = 3;
    sun.castsShadows = true;
    prRenderLightAdd(update, &sun);
    prRenderUpdateCommit(update);
    setEnvironment(f, scene, createEnvironment(f, cubeOf(16, [](PrVec3 d) {
        float height = d.y * 0.5f + 0.5f;
        return PrVec3{0.2f + 0.6f * height, 0.25f + 0.5f * height, 0.3f + 0.9f * height * (d.x * 0.5f + 0.5f)};
    })));
    PrRenderViewDesc view = frontView(scene);
    view.extent = {128, 128};
    return view;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render layers" * doctest::skip(!gpuTests()));

TEST_CASE("a white surface with any one layer returns no more of a white furnace than the core material does") {
    onEachDevice([](Fixture& f) {
        LayerProbe probe(f);
        Image none = probe.draw(Layer_None, 0, 0);
        const float metallics[] = {0, 1};
        for (uint32_t layer = Layer_Clearcoat; layer < Layer_Count; ++layer) {
            for (float metallic : metallics) {
                CAPTURE(layer);
                CAPTURE(metallic);
                Image image = probe.draw(Layer(layer), metallic, layer == Layer_Iridescence ? 400.0f : 0.8f);
                Image core = metallic == 0 ? none : probe.draw(Layer_None, 1, 0);
                for (uint32_t y = 0; y < kSide; ++y) {
                    for (uint32_t x = 0; x < kSide; ++x) {
                        CAPTURE(x);
                        CAPTURE(y);
                        float returned = image.at(x, y)[0];
                        // A layer makes no energy. Each takes its share of the light before it passes the rest on: the
                        // coat and the sheen by what they reflect toward the eye, the film by replacing the Fresnel
                        // term, and the two kinds of transmission by the part of the diffuse light that they move to
                        // the far side. So the surface returns no more than the core material at the same roughness and
                        // angle, which glTF's Fresnel mix lets exceed 1 for a dielectric at a grazing eye, plus the
                        // error of the sums' steps. Two lobes do return more. The sheen's lobe is not normalized: at a
                        // grazing eye its visibility is clamped, not exact. And the anisotropic lobe is wider along its
                        // direction than the core's, where the single-scattering lobe loses less.
                        float bound = prF32Max(core.at(x, y)[0], 1) + 0.01f;
                        if (layer == Layer_Sheen && y < kSide / 4)
                            bound += 0.25f;
                        if (layer == Layer_Anisotropy)
                            bound += 0.03f;
                        CHECK(returned <= bound);
                        CHECK(returned > 0.2f);
                    }
                }
            }
        }
    });
}

TEST_CASE("the table of the sheen's albedo is the sheen lobe's integral over the light's directions") {
    onEachDevice([](Fixture& f) {
        LayerProbe probe(f);
        Image image = probe.draw(Layer_Sheen, 0, 0);
        double largest = 0;
        for (uint32_t y = 0; y < kSide; ++y) {
            for (uint32_t x = 0; x < kSide; ++x) {
                CAPTURE(x);
                CAPTURE(y);
                // The shader sums the lobe of pr/render_lobes.slang at each pixel's roughness and angle, and the table
                // was summed by the generator from its own copy of the specification's equations.
                double summed = double(image.at(x, y)[1]);
                double tabulated = tableSheenAlbedo(probeRoughness(x), probeCosine(y));
                largest = prF64Max(largest, prF64Abs(summed - tabulated));
                CHECK(summed == doctest::Approx(tabulated).epsilon(0.03));
            }
        }
        CAPTURE(largest);
        CHECK(largest < 0.012);
        // The albedo is what KHR_materials_sheen's layering takes from the base, so it is a part of the light.
        CHECK(tableSheenAlbedo(1, 1) > 0.05);
        CHECK(tableSheenAlbedo(1, 1) < 0.5);
    });
}

TEST_CASE("the anisotropic lobe is a normalized distribution, and its highlight has the shape that it defines") {
    onEachDevice([](Fixture& f) {
        LayerProbe probe(f);
        const float strengths[] = {0.0f, 0.5f, 1.0f};
        for (float strength : strengths) {
            CAPTURE(strength);
            Image image = probe.draw(Layer_Anisotropy, 1, strength);
            for (uint32_t x = 0; x < kSide; ++x) {
                CAPTURE(x);
                // The distribution times the cosine of the half vector sums to 1 over the half vectors, whatever its
                // two widths are (Burley 2012, addenda).
                if (strength > 0)
                    CHECK(double(image.at(x, kSide / 2)[2]) == doctest::Approx(1).epsilon(0.02));

                // A light 45 degrees from the normal, at the angle around it that the pixel's column gives, and an eye
                // along the normal: the lobe along the wider direction against the lobe across it.
                double turn = (x + 0.5) / kSide * 2 * kPi;
                double alpha = double(kLowRoughness) * double(kLowRoughness);
                double at = alpha + (1 - alpha) * double(strength) * double(strength);
                const double eye[3] = {0, 0, 1};
                const double light[3] =
                    {prF64Cos(turn) * prF64Sqrt(0.5), prF64Sin(turn) * prF64Sqrt(0.5), prF64Sqrt(0.5)};
                double expected = anisotropicLobe(at, alpha, eye, light);
                CHECK(double(image.at(x, 0)[3]) == doctest::Approx(expected).epsilon(0.01));
            }
            // With a strength, the highlight reaches farther along the direction than across it.
            const double eye[3] = {0, 0, 1};
            const double along[3] = {prF64Sqrt(0.5), 0, prF64Sqrt(0.5)};
            const double across[3] = {0, prF64Sqrt(0.5), prF64Sqrt(0.5)};
            double alpha = double(kLowRoughness) * double(kLowRoughness);
            double at = alpha + (1 - alpha) * double(strength) * double(strength);
            double ratio = anisotropicLobe(at, alpha, eye, along) / anisotropicLobe(at, alpha, eye, across);
            if (strength == 0)
                CHECK(ratio == doctest::Approx(1));
            else
                CHECK(ratio > 4);
        }
    });
}

TEST_CASE("a white sphere with any one layer returns all of a white furnace, or less under a thin film") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        const float metallics[] = {0, 1};
        const float roughnesses[] = {0, 0.5f, 1};
        for (uint32_t layer = Layer_Clearcoat; layer < Layer_Count; ++layer) {
            for (float metallic : metallics) {
                for (float roughness : roughnesses) {
                    CAPTURE(layer);
                    CAPTURE(metallic);
                    CAPTURE(roughness);
                    PrRenderKhronosPbrMaterial material = layeredMaterial(Layer(layer), metallic, roughness);
                    setGltf(f.render, s.material, &material);
                    for (PrRenderTechnique technique : techniques(f)) {
                        CAPTURE(technique);
                        s.view.technique = technique;
                        Image image = f.view(s.view);
                        float lowest = 1e9f;
                        float highest = 0;
                        for (uint32_t i = 0; i < kExtent.x * kExtent.y; ++i) {
                            lowest = prF32Min(lowest, image.texels[i * 4 + 1]);
                            highest = prF32Max(highest, image.texels[i * 4 + 1]);
                        }
                        CAPTURE(lowest);
                        CAPTURE(highest);
                        // Under an even environment, every layer passes on exactly what it does not return: the coat
                        // mixes by its Fresnel term, the sheen scales the base by one minus its albedo, and the light
                        // from behind the surface is as bright as the light before it. A film over a metal is the one
                        // layer that absorbs: its Fresnel term replaces the metal's, and the light of the further
                        // bounces, which the core metal adds back, is not added for it.
                        CHECK(highest <= 1.002f);
                        if (layer == Layer_Iridescence && metallic == 1)
                            CHECK(lowest >= 0.3f);
                        else
                            CHECK(lowest >= 0.998f);
                    }
                }
            }
        }
    });
}

TEST_CASE("a smooth coat over a black base reflects the Fresnel term of its interface") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
        material.baseColor = {0, 0, 0, 1};
        material.metallic = 0;
        // Without the base's own reflection, the coat's is all that the sphere returns.
        material.specular = 0;
        material.clearcoat = 1;
        setGltf(f.render, s.material, &material);
        Image image = f.view(s.view);
        uint32_t checked = 0;
        for (uint32_t y = 0; y < kExtent.y; ++y) {
            for (uint32_t x = 0; x < kExtent.x; ++x) {
                double nv = sphereCosine(x, y);
                // Away from the silhouette, where a pixel covers a range of angles.
                if (nv < 0.3)
                    continue;
                CAPTURE(x);
                CAPTURE(y);
                // KHR_materials_clearcoat, "Clearcoat BRDF": a coat of the index 1.5 reflects 4% at normal incidence,
                // by Schlick's term, and the coat's strength times that term mixes it over the base.
                double expected = 0.04 + 0.96 * prF64Pow(1 - nv, 5);
                CHECK(double(image.at(x, y)[0]) == doctest::Approx(expected).epsilon(0.02));
                ++checked;
            }
        }
        CHECK(checked > 300);
        CHECK(double(image.at(kCenter, kCenter)[0]) == doctest::Approx(0.04).epsilon(0.01));

        // Half a coat reflects half of it.
        material.clearcoat = 0.5f;
        setGltf(f.render, s.material, &material);
        CHECK(double(f.view(s.view).at(kCenter, kCenter)[0]) == doctest::Approx(0.02).epsilon(0.01));
    });
}

TEST_CASE("a thin film over a smooth metal reflects the Airy sum of its thickness, and its base alone without one") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        const float thicknesses[] = {150, 300, 450, 800};
        const float baseColor[3] = {0.9f, 0.6f, 0.3f};
        for (float thickness : thicknesses) {
            CAPTURE(thickness);
            PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
            material.baseColor = {baseColor[0], baseColor[1], baseColor[2], 1};
            material.metallic = 1;
            material.roughness = 0;
            material.iridescence = 1;
            material.iridescenceIor = 1.3f;
            material.iridescenceThicknessMaximum = thickness;
            setGltf(f.render, s.material, &material);
            Image image = f.view(s.view);
            const PrUVec2 pixels[] = {{kCenter, kCenter}, {kCenter + 8, kCenter}, {kCenter, kCenter - 11}};
            for (PrUVec2 at : pixels) {
                double nv = sphereCosine(at.x, at.y);
                REQUIRE(nv > 0);
                CAPTURE(nv);
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    CAPTURE(channel);
                    // A smooth metal mirrors the furnace, so the pixel is the film's Fresnel term at the eye's angle,
                    // times the lobe's albedo, which is 1 for a smooth surface to the table's accuracy.
                    double rgb[3];
                    referenceIridescence(1.3, nv, double(thickness), double(baseColor[channel]), rgb);
                    CHECK(double(image.at(at.x, at.y)[channel]) == doctest::Approx(rgb[channel]).epsilon(0.02));
                }
            }
        }
        // The films differ: the color of the reflection moves with the thickness.
        double thin[3];
        double thick[3];
        referenceIridescence(1.3, 1, 150, 0.9, thin);
        referenceIridescence(1.3, 1, 450, 0.9, thick);
        CHECK(prF64Abs(thin[0] - thick[0]) + prF64Abs(thin[1] - thick[1]) + prF64Abs(thin[2] - thick[2]) > 0.05);
        // A film that is thin against every wavelength interferes with none: it reflects what its base does.
        double none[3];
        referenceIridescence(1.3, 1, 1e-3, 0.5, none);
        CHECK(none[0] == doctest::Approx(0.5).epsilon(0.02));
        CHECK(none[2] == doctest::Approx(0.5).epsilon(0.02));
    });
}

TEST_CASE("a slab that transmits dims the light behind it by Beer's law over its thickness") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMaterial slab = prRenderMaterialCreate(f.render);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        // The slab is drawn at half its size, so its thickness in the world is half of the material's.
        addInstance(update, createQuad(f), slab, placed({0, 0, 0}, {0.5f, 0.5f, 0.5f}));
        prRenderUpdateCommit(update);
        setEnvironment(f, scene, createEvenEnvironment(f, {1, 1, 1}));
        PrRenderViewDesc view = frontView(scene);
        const float thicknesses[] = {0, 0.5f, 1, 3};
        const float color[3] = {0.8f, 0.4f, 0.1f};
        for (float thickness : thicknesses) {
            CAPTURE(thickness);
            PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
            material.metallic = 0;
            material.roughness = 0;
            material.transmission = 1;
            material.thickness = thickness;
            material.attenuationDistance = 0.75f;
            material.attenuationColor = {color[0], color[1], color[2]};
            setGltf(f.render, slab, &material);
            Image image = f.view(view);
            for (uint32_t channel = 0; channel < 3; ++channel) {
                CAPTURE(channel);
                // KHR_materials_volume, "Attenuation": T(x) = c^(x / d). Seen along its normal, the slab's interface
                // reflects 4% of the furnace and passes the rest, which crosses the thickness once.
                double transmittance = prF64Pow(double(color[channel]), double(thickness) * 0.5 / 0.75);
                double expected = 0.04 + 0.96 * transmittance;
                CHECK(double(image.at(kCenter, kCenter)[channel]) == doctest::Approx(expected).epsilon(0.01));
            }
        }
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a sheet that transmits diffusely passes a light from behind it, which its own shadow does not hide") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
        material.baseColor = {0, 0, 0, 1};
        material.metallic = 0;
        material.flags |= PrRenderMaterialFlag_DoubleSided;
        material.diffuseTransmission = 1;
        material.diffuseTransmissionColor = {0.8f, 0.5f, 0.2f};
        PrRenderMaterial sheet = createMaterial(f, material);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, sheet, placed({0, 0, 0}, {0.5f, 0.5f, 0.5f}));
        // A light that shines through the sheet toward the eye, and casts the scene's shadows.
        PrRenderLightDesc light = prRenderLightDescDefault();
        light.intensity = 2;
        light.castsShadows = true;
        PrRenderLight lit = prRenderLightAdd(update, &light);
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        // Straight through the sheet, and slanting through it.
        const PrVec3 directions[] = {{0, 0, 1}, {0.5f, 0.3f, 1}, {-1.2f, 0.4f, 1}};
        for (PrVec3 direction : directions) {
            CAPTURE(direction.x);
            light.direction = direction;
            update = prRenderUpdateBegin(f.render, scene);
            prRenderLightSet(update, lit, &light);
            prRenderUpdateCommit(update);
            double cosine = double(direction.z) / double(prVec3Length(direction));
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                view.technique = technique;
                Image image = f.view(view);
                const float color[3] = {0.8f, 0.5f, 0.2f};
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    CAPTURE(channel);
                    // KHR_materials_diffuse_transmission, "Implementation": a Lambertian lobe about the reversed
                    // normal, of the transmission's color, under what the interface passes toward the eye along the
                    // normal, 96%.
                    double expected = double(color[channel]) / kPi * 2 * cosine * 0.96;
                    CHECK(double(image.at(kCenter, kCenter)[channel]) == doctest::Approx(expected).epsilon(0.01));
                }
            }
        }

        // What stands between the sheet and the light still shadows it.
        update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, createGltf(f, prRenderKhronosPbrMaterialDefault()), placed({0, 0, -0.5f}));
        prRenderUpdateCommit(update);
        CHECK(f.view(view).at(kCenter, kCenter)[0] < 0.01f);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a surface that transmits shows the scene behind it, by either technique and with any number of samples") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMaterial glass = prRenderMaterialCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        // A backdrop that no light changes, with a darker half on the left, and a pane before its middle.
        addInstance(update, quad, createUnlit(f, {0.1f, 0.2f, 0.05f, 1}), placed({-1, 0, -1}, {1, 2, 1}));
        addInstance(update, quad, createUnlit(f, {0.9f, 0.5f, 0.25f, 1}), placed({1, 0, -1}, {1, 2, 1}));
        PrRenderInstance pane = addInstance(update, quad, glass, placed({0, 0, 0}, {0.5f, 0.5f, 0.5f}));
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
        material.baseColor = {1, 0.5f, 1, 1};
        material.metallic = 0;
        material.roughness = 0;
        material.transmission = 1;
        setGltf(f.render, glass, &material);

        // The pixel right of the middle shows the bright half through the pane, and the one left of it the dark half.
        const uint32_t samples[] = {1, 4};
        for (PrRenderTechnique technique : techniques(f)) {
            for (uint32_t count : samples) {
                CAPTURE(technique);
                CAPTURE(count);
                view.technique = technique;
                view.samples = uint8_t(count);
                Image image = f.view(view);
                // KHR_materials_transmission, "Transmission BTDF": what the interface does not reflect passes, tinted
                // by the base color. Seen along the normal, the interface reflects 4% of a black environment.
                const float* bright = image.at(kCenter + 3, kCenter);
                CHECK(bright[0] == doctest::Approx(0.96f * 0.9f).epsilon(0.01));
                CHECK(bright[1] == doctest::Approx(0.96f * 0.5f * 0.5f).epsilon(0.01));
                CHECK(bright[2] == doctest::Approx(0.96f * 0.25f).epsilon(0.01));
                CHECK(image.at(kCenter - 3, kCenter)[0] == doctest::Approx(0.96f * 0.1f).epsilon(0.01));
                // Beside the pane, the backdrop is as it was.
                CHECK(image.at(kExtent.x - 2, kCenter)[0] == doctest::Approx(0.9f).epsilon(0.001));
            }
        }
        view.technique = PrRenderTechnique_Forward;
        view.samples = 1;
        Image sharp = f.view(view);

        // A rough pane blurs what it shows: near the backdrop's edge, the bright half is dimmed by the dark one.
        material.roughness = 0.5f;
        setGltf(f.render, glass, &material);
        Image blurred = f.view(view);
        CHECK(blurred.at(kCenter + 2, kCenter)[0] < sharp.at(kCenter + 2, kCenter)[0] - 0.05f);
        CHECK(blurred.at(kCenter - 2, kCenter)[0] > sharp.at(kCenter - 2, kCenter)[0] + 0.05f);

        // KHR_materials_volume: a pane with thickness bends the eye's ray, so a pane that is turned shows the backdrop
        // displaced, and KHR_materials_dispersion bends each channel by an index of its own, so the edge of the
        // backdrop shows at another place in red than in blue.
        material.roughness = 0;
        material.thickness = 1.5f;
        material.ior = 1.5f;
        setGltf(f.render, glass, &material);
        PrAffine3 turned = placed({0, 0, 0}, {0.5f, 0.5f, 0.5f});
        float c = prF32Cos(0.7f);
        float s = prF32Sin(0.7f);
        turned.m[0][0] = 0.5f * c;
        turned.m[0][2] = 0.5f * s;
        turned.m[2][0] = -0.5f * s;
        turned.m[2][2] = 0.5f * c;
        update = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetTransform(update, pane, &turned);
        prRenderUpdateCommit(update);
        Image thick = f.view(view);
        material.thickness = 0;
        setGltf(f.render, glass, &material);
        Image thin = f.view(view);
        // The thin pane shows the edge at the view's middle, and the thick one moves it: one of the pixels about the
        // middle shows the other half.
        float moved = 0;
        for (uint32_t x = kCenter - 4; x <= kCenter + 4; ++x)
            moved = prF32Max(moved, prF32Abs(thick.at(x, kCenter)[0] - thin.at(x, kCenter)[0]));
        CHECK(moved > 0.3f);
        material.thickness = 1.5f;
        material.dispersion = 20;
        setGltf(f.render, glass, &material);
        Image dispersed = f.view(view);
        float split = 0;
        for (uint32_t x = kCenter - 6; x <= kCenter + 6; ++x) {
            // Without dispersion a pixel's red and blue are the backdrop's, in the ratio of one of its halves.
            const float* texel = dispersed.at(x, kCenter);
            const float* even = thick.at(x, kCenter);
            split = prF32Max(split, prF32Abs(texel[0] / prF32Max(texel[2], 1e-3f) - even[0] / prF32Max(even[2], 1e-3f)));
        }
        CHECK(split > 0.2f);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("the scene behind is made for a view of a scene with a surface that reads it, and for no other") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMaterial pane = prRenderMaterialCreate(f.render);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, createQuad(f), createUnlit(f, {0.5f, 0.5f, 0.5f, 1}), placed({0, 0, -1}));
        addInstance(update, createQuad(f), pane, placed({0, 0, 0}, {0.5f, 0.5f, 0.5f}));
        prRenderUpdateCommit(update);
        PrRenderViewDesc view = frontView(scene);
        PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
        material.clearcoat = 1;
        setGltf(f.render, pane, &material);
        f.view(view);
        PrGpuGraphStats opaque = f.stats;
        // A blended material reads nothing behind it: the device blends it.
        material.alphaMode = PrRenderAlphaMode_Blend;
        setGltf(f.render, pane, &material);
        f.view(view);
        PrGpuGraphStats blended = f.stats;
        CHECK(blended.passes == opaque.passes);
        CHECK(blended.textures == opaque.textures);
        // A material that transmits adds the texture, the work that fills it, and the pass of the blended draws.
        material.alphaMode = PrRenderAlphaMode_Opaque;
        material.transmission = 0.5f;
        setGltf(f.render, pane, &material);
        f.view(view);
        CHECK(f.stats.passes == opaque.passes + 3);
        CHECK(f.stats.textures == opaque.textures + 1);
        view.samples = 4;
        f.view(view);
        PrGpuGraphStats multisampled = f.stats;
        material.transmission = 0;
        setGltf(f.render, pane, &material);
        f.view(view);
        // With more than one sample the first pass resolves into the texture, so no copy fills it.
        CHECK(multisampled.passes == f.stats.passes + 2);
        CHECK(multisampled.textures == f.stats.textures + 1);

        // On a device without bindless, the texture takes a slot of the material's textures.
        uint32_t plain = prRenderMaterialTextureLimit(f.render, 0);
        CHECK(prRenderMaterialTextureLimit(f.render, PrRenderMaterialFlag_ReadsBehind) ==
            (f.bindless ? UINT32_MAX : plain - 1));
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("the specular-glossiness workflow draws what metallic-roughness draws where the two define one surface") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEnvironment(f, cubeOf(16, [](PrVec3 d) {
            return PrVec3{0.3f + 0.3f * d.x, 0.5f + 0.4f * d.y, 0.4f};
        })));
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLightDesc light = prRenderLightDescDefault();
        light.direction = {-0.5f, -0.3f, -1};
        prRenderLightAdd(update, &light);
        prRenderUpdateCommit(update);

        // A dielectric: KHR_materials_pbrSpecularGlossiness, "Specular-Glossiness" gives c_diff = diffuse * (1 -
        // max(specular)) and F0 = specular, and the core material gives c_diff = base color and F0 = 0.04.
        PrRenderKhronosPbrMaterial rough = prRenderKhronosPbrMaterialDefault();
        rough.baseColor = {0.6f * 0.96f, 0.3f * 0.96f, 0.2f * 0.96f, 1};
        rough.metallic = 0;
        rough.roughness = 0.4f;
        PrRenderKhronosPbrMaterial glossy = prRenderKhronosPbrMaterialDefault();
        glossy.specularGlossiness = true;
        glossy.baseColor = {0.6f, 0.3f, 0.2f, 1};
        glossy.specularColor = {0.04f, 0.04f, 0.04f};
        glossy.glossiness = 0.6f;
        setGltf(f.render, s.material, &rough);
        Image expected = f.view(s.view);
        setGltf(f.render, s.material, &glossy);
        Image image = f.view(s.view);
        Difference difference = compare(expected, image, 1e-3f);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
        CHECK(expected.at(kCenter, kCenter)[0] > 0.05f);

        // A metal: no diffuse color, and the specular color is the metal's base color.
        rough.baseColor = {0.9f, 0.7f, 0.3f, 1};
        rough.metallic = 1;
        glossy.baseColor = {0, 0, 0, 1};
        glossy.specularColor = {0.9f, 0.7f, 0.3f};
        setGltf(f.render, s.material, &rough);
        expected = f.view(s.view);
        setGltf(f.render, s.material, &glossy);
        image = f.view(s.view);
        difference = compare(expected, image, 1e-3f);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
    });
}

TEST_CASE("an index of refraction and a specular color set what a dielectric reflects at normal incidence") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        struct Case {
            float ior;
            PrVec3 color;
            float strength;
        };
        const Case cases[] = {
            {1.5f, {1, 1, 1}, 1}, {1.33f, {1, 1, 1}, 1}, {2.4f, {1, 1, 1}, 1}, {1.5f, {1, 0.5f, 0.25f}, 1},
            {1.5f, {1, 1, 1}, 0.5f}, {0, {1, 1, 1}, 1}, {1, {1, 1, 1}, 1},
        };
        for (const Case& c : cases) {
            CAPTURE(c.ior);
            CAPTURE(c.strength);
            PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
            material.baseColor = {0, 0, 0, 1};
            material.metallic = 0;
            material.roughness = 0;
            material.ior = c.ior;
            material.specular = c.strength;
            material.specularColor = c.color;
            setGltf(f.render, s.material, &material);
            Image image = f.view(s.view);
            const float tint[3] = {c.color.x, c.color.y, c.color.z};
            for (uint32_t channel = 0; channel < 3; ++channel) {
                CAPTURE(channel);
                // KHR_materials_ior: f0 = ((ior - 1) / (ior + 1))^2, and an index of 0 reflects all.
                // KHR_materials_specular: f0 = min(f0 * specular_color, 1) * specular.
                double f0 = c.ior > 0 ? prF64Pow((double(c.ior) - 1) / (double(c.ior) + 1), 2) : 1;
                double expected = prF64Min(f0 * double(tint[channel]), 1) * double(c.strength);
                CHECK(double(image.at(kCenter, kCenter)[channel]) ==
                    doctest::Approx(expected).epsilon(0.02).scale(0.05));
            }
        }
    });
}

TEST_CASE("a scene of layered materials draws alike with slots and handles, on each backend, and by each technique") {
    Array<Image> handles;
    Array<PrStringView> names;
    const Array<TestDevice>& devices = testDevices();
    for (uint32_t i = 0; i < devices.count(); ++i) {
        Fixture f(devices[i]);
        if (!f.bindless)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        PrRenderViewDesc view = createLayeredScene(f);
        Image image = f.view(view);
        if (i + 1 < devices.count() && devices[i + 1].backend == devices[i].backend && devices[i + 1].slotsOnly) {
            // The materials name no texture, so a device with the slots alone draws every layer.
            Fixture slotsOnly(devices[i + 1]);
            REQUIRE_FALSE(slotsOnly.bindless);
            Image slots = slotsOnly.view(createLayeredScene(slotsOnly));
            Difference difference = compare(image, slots, 0);
            CAPTURE(difference.largest);
            CHECK(difference.over == 0);
        }
        if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility)) {
            view.technique = PrRenderTechnique_Visibility;
            Image visibility = f.view(view);
            // The two techniques shade one surface with one function. A pixel on a geometric edge shows another surface
            // by each, so the spheres' silhouettes differ, and nothing else does.
            Difference difference = compare(image, visibility, 0.02f);
            CAPTURE(difference.over);
            CHECK(difference.over <= view.extent.x * view.extent.y / 20);
            CHECK(difference.mean < 5e-3);
        }
        handles.push(move(image));
        names.push(backend);
    }
    for (uint32_t i = 1; i < handles.count(); ++i) {
        CAPTURE(names[0]);
        CAPTURE(names[i]);
        Difference difference = compare(handles[0], handles[i], 0.01f);
        CAPTURE(difference.largest);
        CAPTURE(difference.over);
        CHECK(difference.over <= 128 * 128 / 200);
        CHECK(difference.mean < 2e-4);
    }
}

TEST_SUITE_END();
