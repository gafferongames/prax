// Environments and the light that they give a scene: the cube that the device filters, the spherical harmonics of the
// diffuse light, the table of the split sum, the sky's model against its integrals and against published skies, the
// sun's disc, and spheres lit by an environment.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/render.h"
#include "pr/render_environment.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"

#include "render/tests/kTestRenderShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

constexpr double kPi = 3.14159265358979323846;

double squareRoot(double value) {
    return double(prF32Sqrt(float(value)));
}

// ---------------------------------------------------------------------------------------------------------------------
// Sums over a cube

// Returns the weights of a cube's texels in a sum over the sphere: their solid angles, scaled to add up to the sphere.
Array<double> texelWeights(uint32_t size) {
    Array<double> weights(size * size);
    double sum = 0;
    for (uint32_t y = 0; y < size; ++y) {
        for (uint32_t x = 0; x < size; ++x) {
            weights[y * size + x] = cubeTexelSolidAngle(x, y, size);
            sum += weights[y * size + x] * 6;
        }
    }
    for (double& weight : weights)
        weight *= 4 * kPi / sum;
    return weights;
}

// Returns the red light of a cube summed over the sphere: its mean radiance times the sphere's solid angle.
double cubeEnergy(const CubeTexels& cube) {
    Array<double> weights = texelWeights(cube.size);
    double sum = 0;
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t i = 0; i < cube.size * cube.size; ++i)
            sum += double(cube.at(face, i % cube.size, i / cube.size)[0]) * weights[i];
    }
    return sum;
}

// Returns the mean angle, in radians, between a direction and the directions of a cube's red light.
double cubeSpread(const CubeTexels& cube, PrVec3 from) {
    Array<double> weights = texelWeights(cube.size);
    double sum = 0;
    double energy = 0;
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < cube.size; ++y) {
            for (uint32_t x = 0; x < cube.size; ++x) {
                double light = double(cube.at(face, x, y)[0]) * weights[y * cube.size + x];
                float cosine = prF32Clamp(prVec3Dot(cubeTexelDirection(face, x, y, cube.size), from), -1, 1);
                sum += light * double(prF32Acos(cosine));
                energy += light;
            }
        }
    }
    return sum / energy;
}

// Returns the radiance that a white surface without gloss leaves under a cube's light, for a surface with the unit
// normal `normal`: the cube's radiance summed over the hemisphere about the normal by the cosine of each direction's
// angle to it, over pi.
PrVec3 cubeIrradiance(const CubeTexels& cube, PrVec3 normal) {
    Array<double> weights = texelWeights(cube.size);
    double sum[3] = {0, 0, 0};
    for (uint32_t face = 0; face < 6; ++face) {
        for (uint32_t y = 0; y < cube.size; ++y) {
            for (uint32_t x = 0; x < cube.size; ++x) {
                double cosine = double(prVec3Dot(cubeTexelDirection(face, x, y, cube.size), normal));
                if (cosine <= 0)
                    continue;
                for (uint32_t channel = 0; channel < 3; ++channel)
                    sum[channel] += double(cube.at(face, x, y)[channel]) * cosine * weights[y * cube.size + x];
            }
        }
    }
    return {float(sum[0] / kPi), float(sum[1] / kPi), float(sum[2] / kPi)};
}

// ---------------------------------------------------------------------------------------------------------------------
// The split sum, integrated

constexpr uint32_t kTableSide = 32;

// The scale and the bias of the split sum of the library's specular lobe at each roughness and each cosine of the eye's
// angle to the normal, at the middles of kTableSide steps of each, integrated over the directions of light.
struct SplitSumTable {
    SplitSum entries[kTableSide][kTableSide];   // by roughness, then by the cosine
};

// Integrates the split sum at a roughness and a cosine of the eye's angle: the lobe of pr/render_brdf.slang, GGX with
// the height-correlated Smith visibility, with a Fresnel term of one for the scale and bias together, and Schlick's
// weight between them. The half vectors are sampled by the lobe's distribution.
SplitSum integrateSplitSum(double roughness, double nv) {
    constexpr uint32_t kSamples = 2048;
    double alpha = roughness * roughness > 0.002 ? roughness * roughness : 0.002;
    double alpha2 = alpha * alpha;
    double eye[3] = {squareRoot(1 - nv * nv), 0, nv};
    SplitSum sum{};
    for (uint32_t i = 0; i < kSamples; ++i) {
        double u1 = (i + 0.5) / kSamples;
        uint32_t bits = i;
        double u2 = 0;
        for (double digit = 0.5; bits != 0; bits >>= 1, digit *= 0.5)
            u2 += digit * (bits & 1);
        float around = float(2 * kPi * u1);
        double cosine = squareRoot((1 - u2) / (1 + (alpha2 - 1) * u2));
        double sine = squareRoot(1 - cosine * cosine);
        double h[3] = {sine * double(prF32Cos(around)), sine * double(prF32Sin(around)), cosine};
        double vh = eye[0] * h[0] + eye[1] * h[1] + eye[2] * h[2];
        double nl = 2 * vh * h[2] - eye[2];
        if (nl <= 0 || vh <= 0)
            continue;
        double visibility =
            0.5 / (nl * squareRoot(nv * nv * (1 - alpha2) + alpha2) + nv * squareRoot(nl * nl * (1 - alpha2) + alpha2));
        // The lobe times the light's cosine, over the density of the sampled direction.
        double weight = visibility * nl * 4 * vh / h[2];
        double schlick = double(prF32Pow(float(1 - vh), 5));
        sum.scale += weight * (1 - schlick);
        sum.bias += weight * schlick;
    }
    return {sum.scale / kSamples, sum.bias / kSamples};
}

const SplitSumTable& splitSumTable() {
    static SplitSumTable table;
    static bool filled = false;
    if (!filled) {
        for (uint32_t r = 0; r < kTableSide; ++r) {
            for (uint32_t v = 0; v < kTableSide; ++v)
                table.entries[r][v] = integrateSplitSum((r + 0.5) / kTableSide, (v + 0.5) / kTableSide);
        }
        filled = true;
    }
    return table;
}

// ---------------------------------------------------------------------------------------------------------------------
// Spheres

// A scene of one sphere of radius 1 about the origin, seen from the front.
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

void setSphereMaterial(Fixture& f, const SphereScene& s, float metallic, float roughness, float baseColor = 1) {
    PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
    material.baseColor = {baseColor, baseColor, baseColor, 1};
    material.metallic = metallic;
    material.roughness = roughness;
    setGltf(f.render, s.material, &material);
}

// Returns the unit direction from the front view's camera through the center of a pixel.
PrVec3 pixelRay(uint32_t x, uint32_t y) {
    float px = (float(x) + 0.5f) / float(kExtent.x) * 2 - 1;
    float py = 1 - (float(y) + 0.5f) / float(kExtent.y) * 2;
    return prVec3Normalize({px, py, -kCameraDistance});
}

// Where the front view's ray through a pixel meets the unit sphere about the origin.
struct SphereHit {
    bool hit;
    PrVec3 normal;
    PrVec3 toEye;
};

SphereHit sphereHit(uint32_t x, uint32_t y) {
    PrVec3 origin{0, 0, kCameraDistance};
    PrVec3 ray = pixelRay(x, y);
    float b = prVec3Dot(origin, ray);
    float discriminant = b * b - (prVec3Dot(origin, origin) - 1);
    if (discriminant <= 0)
        return SphereHit{};
    PrVec3 point = origin + ray * (-b - prF32Sqrt(discriminant));
    return SphereHit{.hit = true, .normal = point, .toEye = ray * -1.0f};
}

// The techniques that a device draws with.
Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

// An environment that differs smoothly in every direction, which a cube's filtering changes little.
PrVec3 smoothRadiance(PrVec3 d) {
    return {0.6f + 0.35f * d.x, 0.5f + 0.3f * d.y + 0.15f * d.x * d.z, 0.55f - 0.4f * d.z + 0.1f * d.y * d.y};
}

// Returns the turn about the y axis by `angle` radians.
PrAffine3 turnAboutY(float angle) {
    float c = prF32Cos(angle);
    float s = prF32Sin(angle);
    return PrAffine3{{{c, 0, s, 0}, {0, 1, 0, 0}, {-s, 0, c, 0}}};
}

// Returns the direction of an environment that a direction in the world shows, for an environment that is turned by
// `turn` in its scene.
PrVec3 unturned(const PrAffine3& turn, PrVec3 d) {
    const float (*m)[4] = turn.m;
    return {
        m[0][0] * d.x + m[1][0] * d.y + m[2][0] * d.z,
        m[0][1] * d.x + m[1][1] * d.y + m[2][1] * d.z,
        m[0][2] * d.x + m[1][2] * d.y + m[2][2] * d.z,
    };
}

// ---------------------------------------------------------------------------------------------------------------------
// The sky's model, integrated without tables

// Returns the luminance of linear sRGB.
double luminance(const float* color) {
    return 0.2126 * double(color[0]) + 0.7152 * double(color[1]) + 0.0722 * double(color[2]);
}

// The atmosphere of the sky's model (render_environment.cpp): the radii of the planet and of the atmosphere's top, the
// air's scattering and the ozone's absorption of red, green and blue, and a clear day's haze, of which nine tenths is
// scattering.
constexpr double kSkyGround = 6360e3;
constexpr double kSkyTop = 6460e3;
constexpr double kSkyAir[3] = {5.802e-6, 13.558e-6, 33.1e-6};
constexpr double kSkyOzone[3] = {0.650e-6, 1.881e-6, 0.085e-6};
constexpr double kSkyHaze = 6.7e-5;

// What a meter of the atmosphere at a distance from the planet's center takes from a channel's light, and what it
// scatters of it.
struct SkyMedium {
    double loss[3];
    double scatter[3];
};

SkyMedium referenceMedium(double radius, double haze) {
    double height = prF64Max(radius - kSkyGround, 0);
    double air = prF64Exp(-height / 8000);
    double hazeDensity = prF64Exp(-height / 1200) * haze;
    double ozone = prF64Max(1 - prF64Abs(height - 25e3) / 15e3, 0);
    SkyMedium medium{};
    for (uint32_t channel = 0; channel < 3; ++channel) {
        medium.scatter[channel] = kSkyAir[channel] * air + kSkyHaze * 0.9 * hazeDensity;
        medium.loss[channel] = kSkyAir[channel] * air + kSkyHaze * hazeDensity + kSkyOzone[channel] * ozone;
    }
    return medium;
}

// A ray from a point at `radius` from the planet's center, along a direction at the cosine `cosine` from the point's
// zenith: whether it meets the planet, and how far it runs to the ground or to the atmosphere's top.
bool referenceGrounded(double radius, double cosine) {
    return cosine < 0 && radius * radius * (cosine * cosine - 1) + kSkyGround * kSkyGround > 0;
}

double referenceLength(double radius, double cosine) {
    double b = radius * cosine;
    if (referenceGrounded(radius, cosine))
        return -b - double(prF64Sqrt(prF64Max(b * b - radius * radius + kSkyGround * kSkyGround, 0)));
    return -b + double(prF64Sqrt(prF64Max(b * b - radius * radius + kSkyTop * kSkyTop, 0)));
}

// Writes the part of each channel's sunlight that reaches a point, for a sun at `cosine` from the point's zenith: the
// loss summed over steps that lengthen with the square of the distance, since the air is densest at the start.
void referenceSunlight(double radius, double cosine, double haze, double* out) {
    constexpr uint32_t kSteps = 48;
    out[0] = out[1] = out[2] = 0;
    if (referenceGrounded(radius, cosine))
        return;
    double length = referenceLength(radius, cosine);
    double depth[3] = {0, 0, 0};
    for (uint32_t i = 0; i < kSteps; ++i) {
        double from = double(i) / kSteps;
        double to = double(i + 1) / kSteps;
        double at = length * (from * from + to * to) / 2;
        double atRadius = prF64Sqrt(radius * radius + at * at + 2 * radius * cosine * at);
        SkyMedium medium = referenceMedium(atRadius, haze);
        for (uint32_t channel = 0; channel < 3; ++channel)
            depth[channel] += medium.loss[channel] * length * (to * to - from * from);
    }
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = prF64Exp(-depth[channel]);
}

// Writes the light that reaches a point after two scatterings or more, for a unit of the sun's irradiance and of the
// point's scattering, as Hillaire defines it: the light scattered twice, with the ground's, integrated over every
// direction about the point as if scattering were even, over one less the part of the point's own light that the air
// scatters again.
void referenceMulti(double radius, double sunCosine, double haze, double albedo, double* out) {
    constexpr uint32_t kCosines = 8;
    constexpr uint32_t kTurns = 6;
    constexpr uint32_t kSteps = 96;
    PrDVec3 sun{prF64Sqrt(prF64Max(1 - sunCosine * sunCosine, 0)), sunCosine, 0};
    double second[3] = {0, 0, 0};
    double again[3] = {0, 0, 0};
    for (uint32_t i = 0; i < kCosines; ++i) {
        double cosine = (i + 0.5) / kCosines * 2 - 1;
        double sine = prF64Sqrt(1 - cosine * cosine);
        double length = referenceLength(radius, cosine);
        for (uint32_t k = 0; k < kTurns; ++k) {
            // The half circle on one side of the sun stands for both.
            double turn = kPi * (k + 0.5) / kTurns;
            PrDVec3 d{sine * prF64Cos(turn), cosine, sine * prF64Sin(turn)};
            double through[3] = {1, 1, 1};
            double step = length / kSteps;
            for (uint32_t j = 0; j <= kSteps; ++j) {
                // The last point is the ray's end, which is the ground if the ray meets it.
                double at = j < kSteps ? (j + 0.5) * step : length;
                PrDVec3 p{d.x * at, radius + d.y * at, d.z * at};
                double atRadius = prF64Sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
                double atCosine = (p.x * sun.x + p.y * sun.y) / atRadius;
                double sunlight[3];
                referenceSunlight(atRadius, atCosine, haze, sunlight);
                if (j == kSteps) {
                    if (referenceGrounded(radius, cosine) && atCosine > 0) {
                        for (uint32_t channel = 0; channel < 3; ++channel)
                            second[channel] += through[channel] * sunlight[channel] * atCosine * albedo / kPi;
                    }
                    break;
                }
                SkyMedium medium = referenceMedium(atRadius, haze);
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    double middle = through[channel] * prF64Exp(-medium.loss[channel] * step / 2);
                    second[channel] += middle * medium.scatter[channel] * sunlight[channel] * step / (4 * kPi);
                    again[channel] += middle * medium.scatter[channel] * step;
                    through[channel] *= prF64Exp(-medium.loss[channel] * step);
                }
            }
        }
    }
    for (uint32_t channel = 0; channel < 3; ++channel)
        out[channel] = second[channel] / (kCosines * kTurns) / (1 - again[channel] / (kCosines * kTurns));
}

// Writes the sky's radiance along a unit direction above the horizon, for a viewer a meter above the ground and a unit
// of the sun's irradiance above the atmosphere: the sunlight scattered once toward the viewer by the air, by Rayleigh's
// phase function, and by the haze, by Cornette and Shanks's, and with `multiple` the light scattered before.
void referenceSky(PrDVec3 view, PrDVec3 sun, double haze, double albedo, bool multiple, double* out) {
    constexpr uint32_t kSteps = 64;
    double radius = kSkyGround + 1;
    double length = referenceLength(radius, view.y);
    double cosine = view.x * sun.x + view.y * sun.y + view.z * sun.z;
    double g = 0.76;
    double base = 1 + g * g - 2 * g * cosine;
    double airPhase = 3 / (16 * kPi) * (1 + cosine * cosine);
    double hazePhase = 3 / (8 * kPi) * (1 - g * g) * (1 + cosine * cosine) / ((2 + g * g) * base * prF64Sqrt(base));
    double depth[3] = {0, 0, 0};
    out[0] = out[1] = out[2] = 0;
    for (uint32_t i = 0; i < kSteps; ++i) {
        double from = double(i) / kSteps;
        double to = double(i + 1) / kSteps;
        double at = length * (from * from + to * to) / 2;
        double step = length * (to * to - from * from);
        PrDVec3 p{view.x * at, radius + view.y * at, view.z * at};
        double atRadius = prF64Sqrt(p.x * p.x + p.y * p.y + p.z * p.z);
        double atCosine = (p.x * sun.x + p.y * sun.y + p.z * sun.z) / atRadius;
        double height = prF64Max(atRadius - kSkyGround, 0);
        SkyMedium medium = referenceMedium(atRadius, haze);
        double sunlight[3];
        double before[3] = {0, 0, 0};
        referenceSunlight(atRadius, atCosine, haze, sunlight);
        if (multiple)
            referenceMulti(atRadius, atCosine, haze, albedo, before);
        for (uint32_t channel = 0; channel < 3; ++channel) {
            double through = prF64Exp(-(depth[channel] + medium.loss[channel] * step / 2));
            double once = kSkyAir[channel] * prF64Exp(-height / 8000) * airPhase +
                kSkyHaze * 0.9 * haze * prF64Exp(-height / 1200) * hazePhase;
            out[channel] += through * (once * sunlight[channel] + medium.scatter[channel] * before[channel]) * step;
            depth[channel] += medium.loss[channel] * step;
        }
    }
}

// Returns the luminance of the CIE's standard clear sky of low turbidity (type 12 of CIE S 011/E:2003), in a unit of
// its own, at the angle `fromZenith` from the zenith and `fromSun` from the sun, in radians.
double cieClearSky(double fromZenith, double fromSun) {
    double cosine = prF64Max(prF64Cos(fromZenith), 1e-6);
    double gradation = 1 - prF64Exp(-0.32 / cosine);
    double toward = prF64Cos(fromSun);
    double indicatrix = 1 + 10 * (prF64Exp(-3 * fromSun) - prF64Exp(-3 * kPi / 2)) + 0.45 * toward * toward;
    return gradation * indicatrix;
}

// Returns the radiance that leaves a white, rough dielectric toward `toEye` for light from `toLight` with unit
// irradiance on a plane that faces it: the direct lobes of pr/render_brdf.slang at a roughness of 1.
double whiteRoughDirect(PrVec3 normal, PrVec3 toEye, PrVec3 toLight) {
    double nl = double(prVec3Dot(normal, toLight));
    if (nl <= 0)
        return 0;
    double nv = prF64Max(double(prVec3Dot(normal, toEye)), 1e-4);
    double vh = double(prVec3Dot(toEye, prVec3Normalize(toEye + toLight)));
    double schlick = double(prF32Pow(float(1 - vh), 5));
    double fresnel = 0.04 + 0.96 * schlick;
    // At a roughness of 1 the lobe's distribution is even, and its visibility term is 1 / (2 (nl + nv)).
    return ((1 - fresnel) / kPi + fresnel / kPi * 0.5 / (nl + nv)) * nl;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render_environment");

// ---------------------------------------------------------------------------------------------------------------------
// The cube

TEST_CASE("the first level of an environment's cube is the cube that it was made from") {
    onEachDevice([](Fixture& f) {
        // Values across the range of a half float, which differ from texel to texel.
        CubeTexels source = cubeOf(16, [](PrVec3 d) {
            return PrVec3{prF32Abs(d.x) * 900, 0.001f + prF32Abs(d.y), (d.z + 1.5f) * (d.x + 1.5f) * 20};
        });
        *source.at(2, 3, 5) = 60000;
        PrRenderEnvironment environment = createEnvironment(f, source);
        f.frame();
        f.record();
        PrRenderEnvironmentInfo info = prRenderEnvironmentInfo(f.render, environment);
        CHECK(info.faceSize == 16);
        // Levels of 16 and of 8 texels, for a roughness of 0 and of 1.
        CHECK(info.levels == 2);
        CHECK(prGpuTextureDesc(f.device, info.texture).kind == PrGpuTextureKind_Cube);
        CHECK(prGpuTextureDesc(f.device, info.texture).format == PrGpuFormat_Rgba16F);
        CubeTexels first = readEnvironment(f, environment, 0);
        REQUIRE(first.texels.count() == source.texels.count());
        uint32_t different = 0;
        for (uint32_t i = 0; i < source.texels.count(); ++i) {
            if (first.texels[i] != prFloat16ToFloat(prFloat16FromFloat(source.texels[i])))
                ++different;
        }
        // Each texel is the source's, rounded to a half float.
        CHECK(different == 0);
    });
}

TEST_CASE("a cube takes the values that a half float holds: no negative value, and none that is not a number") {
    onEachDevice([](Fixture& f) {
        CubeTexels source = cubeOf(1, [](PrVec3) { return PrVec3{-3, 1e9f, 0.25f}; });
        *source.at(1, 0, 0) = prF32Sqrt(-1.0f);
        PrRenderEnvironment environment = createEnvironment(f, source);
        f.frame();
        f.record();
        CubeTexels first = readEnvironment(f, environment, 0);
        CHECK(near(first.at(0, 0, 0), {0, 65504, 0.25f}, 0));
        CHECK(near(first.at(1, 0, 0), {0, 65504, 0.25f}, 0));
        CHECK(prRenderEnvironmentInfo(f.render, environment).levels == 1);
    });
}

TEST_CASE("an even environment stays even at every level of its cube") {
    onEachDevice([](Fixture& f) {
        PrRenderEnvironment environment = createEnvironment(f, cubeOf(64, [](PrVec3) { return PrVec3{0.7f, 2, 40}; }));
        f.frame();
        f.record();
        PrRenderEnvironmentInfo info = prRenderEnvironmentInfo(f.render, environment);
        // Levels of 64, 32, 16 and 8 texels.
        REQUIRE(info.levels == 4);
        for (uint32_t level = 0; level < info.levels; ++level) {
            CAPTURE(level);
            CubeTexels texels = readEnvironment(f, environment, level);
            CHECK(texels.size == 64u >> level);
            float lowest = 1e9f;
            float highest = 0;
            for (uint32_t i = 0; i < texels.texels.count(); i += 3) {
                lowest = prF32Min(lowest, texels.texels[i] / 0.7f);
                lowest = prF32Min(lowest, prF32Min(texels.texels[i + 1] / 2, texels.texels[i + 2] / 40));
                highest = prF32Max(highest, texels.texels[i] / 0.7f);
                highest = prF32Max(highest, prF32Max(texels.texels[i + 1] / 2, texels.texels[i + 2] / 40));
            }
            // To within the precision of the device's texture filter, which here loses up to three steps of a half
            // float.
            CHECK(lowest > 0.996f);
            CHECK(highest < 1.004f);
        }
    });
}

TEST_CASE("one bright texel spreads over more of each rougher level, and keeps its light") {
    onEachDevice([](Fixture& f) {
        CubeTexels source = cubeOf(64, [](PrVec3) { return PrVec3{0, 0, 0}; });
        *source.at(4, 40, 23) = 1000;
        PrVec3 from = cubeTexelDirection(4, 40, 23, 64);
        PrRenderEnvironment environment = createEnvironment(f, source);
        f.frame();
        f.record();
        PrRenderEnvironmentInfo info = prRenderEnvironmentInfo(f.render, environment);
        REQUIRE(info.levels == 4);
        double energy = cubeEnergy(source);
        double lastSpread = -1;
        for (uint32_t level = 0; level < info.levels; ++level) {
            CAPTURE(level);
            CubeTexels texels = readEnvironment(f, environment, level);
            double spread = cubeSpread(texels, from);
            double kept = cubeEnergy(texels) / energy;
            CAPTURE(spread);
            CAPTURE(kept);
            // The light lies farther from the texel's direction, on average, at each level: within a texel of it at the
            // first, and tens of degrees from it at the last, which is filtered for a roughness of 1.
            CHECK(spread > lastSpread);
            lastSpread = spread;
            // The filter's kernel is the same about every texel, so the light that it spreads is the light that it
            // took. The sums that stand for it, of 128 samples over a level's few texels, keep that to within 5%: the
            // levels here hold 99.7%, 101.0% and 97.2% of the texel's light.
            CHECK(kept > 0.95);
            CHECK(kept < 1.05);
            // No texel is negative, and none is brighter than the source's one.
            for (float value : texels.texels) {
                CHECK(value >= 0);
                CHECK(value <= 1001);
            }
        }
        // The light lies 0.3, 0.65 and 0.84 radians from the texel's direction, on average, at the three filtered
        // levels.
        CHECK(cubeSpread(readEnvironment(f, environment, 0), from) < 0.01);
        CHECK(lastSpread > 0.8);
    });
}

TEST_CASE("an equirectangular image maps longitude to its columns and latitude to its rows") {
    onEachDevice([](Fixture& f) {
        // An image of 64 by 32 whose top half is red and bottom half green, with blue in the eighth of its columns
        // right of its middle, and rows that have 8 bytes between them.
        constexpr uint32_t kWidth = 64;
        constexpr uint32_t kHeight = 32;
        constexpr size_t kStride = kWidth * 4 * sizeof(float) + 8;
        Array<uint8_t> bytes(kStride * kHeight);
        for (uint32_t y = 0; y < kHeight; ++y) {
            for (uint32_t x = 0; x < kWidth; ++x) {
                bool middle = x >= kWidth / 2 && x < kWidth * 5 / 8;
                const float pixel[4] = {y < kHeight / 2 ? 1.0f : 0.0f, y < kHeight / 2 ? 0.0f : 1.0f, middle ? 1.0f : 0.0f, 9};
                prMemCopy(&bytes[y * kStride + x * sizeof(pixel)], pixel, sizeof(pixel));
            }
        }
        PrRenderRadianceImage image{.pixels = {bytes.begin(), kStride, {kWidth, kHeight}}, .channels = 4};
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetEquirect(f.render, environment, &image) == PrStatus_Ok);
        f.frame();
        f.record();
        PrRenderEnvironmentInfo info = prRenderEnvironmentInfo(f.render, environment);
        CHECK(info.faceSize == kEnvironmentSize);
        CubeTexels cube = readEnvironment(f, environment, 0);
        uint32_t middle = kEnvironmentSize / 2;
        uint32_t quarter = kEnvironmentSize / 4;
        // Up is the top row, and down the bottom one.
        CHECK(cube.at(2, middle, middle)[0] == doctest::Approx(1).epsilon(1e-3));
        CHECK(cube.at(2, middle, middle)[1] == doctest::Approx(0).epsilon(1e-3));
        CHECK(cube.at(3, middle, middle)[0] == doctest::Approx(0).epsilon(1e-3));
        CHECK(cube.at(3, middle, middle)[1] == doctest::Approx(1).epsilon(1e-3));
        // The middle column is toward +x, and the blue columns to its right lie on the side of +x that is toward +z. A
        // face's columns run toward -z on the face toward +x.
        CHECK(near(cube.at(0, quarter, middle - 4), {1, 0, 1}, 1e-3f));
        CHECK(near(cube.at(0, quarter, middle + 4), {0, 1, 1}, 1e-3f));
        CHECK(near(cube.at(0, 3 * quarter, middle - 4), {1, 0, 0}, 1e-3f));
        // The blue ends an eighth of the way around, short of +z. The far side, toward -x, is where the image's left
        // and right edges meet.
        CHECK(near(cube.at(4, middle, middle - 4), {1, 0, 0}, 1e-3f));
        CHECK(near(cube.at(5, middle, middle - 4), {1, 0, 0}, 1e-3f));
        CHECK(near(cube.at(1, middle, middle + 4), {0, 1, 0}, 1e-3f));
    });
}

TEST_CASE("an equirectangular image far larger than the cube is averaged, so that a small light keeps its energy") {
    onEachDevice([](Fixture& f) {
        // An image 16 times as wide as a face, black but for one block of 8 by 8 pixels on the horizon toward +x.
        constexpr uint32_t kWidth = kEnvironmentSize * 16;
        constexpr uint32_t kHeight = kWidth / 2;
        Array<float> pixels(kWidth * kHeight * 3);
        for (float& value : pixels)
            value = 0;
        for (uint32_t y = kHeight / 2 - 4; y < kHeight / 2 + 4; ++y) {
            for (uint32_t x = kWidth / 2 - 4; x < kWidth / 2 + 4; ++x)
                pixels[(y * kWidth + x) * 3] = 500;
        }
        const uint8_t* data = reinterpret_cast<const uint8_t*>(pixels.begin());
        PrRenderRadianceImage image{.pixels = {data, kWidth * 3 * sizeof(float), {kWidth, kHeight}}, .channels = 3};
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetEquirect(f.render, environment, &image) == PrStatus_Ok);
        f.frame();
        f.record();
        // The block's light: its radiance times its solid angle, which on the horizon is its share of the image's
        // columns and rows times 2 pi by pi.
        double expected = 500 * (8.0 / kWidth * 2 * kPi) * (8.0 / kHeight * kPi);
        double energy = cubeEnergy(readEnvironment(f, environment, 0));
        CAPTURE(energy);
        CHECK(energy == doctest::Approx(expected).epsilon(0.05));
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Diffuse light

TEST_CASE("the diffuse light of an environment's harmonics is the light summed over the hemisphere") {
    onEachDevice([](Fixture& f) {
        const PrVec3 normals[] = {
            {1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1},
            prVec3Normalize({1, 1, 1}), prVec3Normalize({-1, 2, 0.5f}), prVec3Normalize({0.3f, -0.4f, -1}),
            prVec3Normalize({-2, -1, 3}),
        };
        // Returns the largest difference between the harmonics' light and the summed light, over the normals, as a part
        // of the environment's mean light.
        auto largestError = [&](const CubeTexels& cube) {
            PrRenderEnvironment environment = createEnvironment(f, cube);
            f.frame();
            float mean = float(cubeEnergy(cube) / (4 * kPi));
            float largest = 0;
            for (PrVec3 normal : normals) {
                PrVec3 fromHarmonics = prRenderEnvironmentIrradiance(f.render, environment, normal);
                PrVec3 summed = cubeIrradiance(cube, normal);
                largest = prF32Max(largest, prF32Abs(fromHarmonics.x - summed.x) / mean);
                largest = prF32Max(largest, prF32Abs(fromHarmonics.y - summed.y) / mean);
                largest = prF32Max(largest, prF32Abs(fromHarmonics.z - summed.z) / mean);
            }
            f.record();
            return largest;
        };

        // An even environment's diffuse light is its radiance, for every normal.
        PrRenderEnvironment even = createEnvironment(f, cubeOf(8, [](PrVec3) { return PrVec3{0.25f, 1, 3}; }));
        f.frame();
        f.record();
        for (PrVec3 normal : normals) {
            PrVec3 diffuse = prRenderEnvironmentIrradiance(f.render, even, normal);
            CHECK(near(&diffuse.x, {0.25f, 1, 3}, 1e-4f));
        }

        // An environment that is a polynomial of degree 2 in the direction lies within the harmonics' three bands, so
        // its diffuse light is exact but for the texels' sums: within 0.03% of its mean light here.
        float smooth = largestError(cubeOf(32, smoothRadiance));
        CAPTURE(smooth);
        CHECK(smooth < 0.002f);

        // A sky over a dark ground with a broad sun does not: the three bands hold its diffuse light to within 5% of
        // its mean light, and to within 2.8% here.
        float sunny = largestError(cubeOf(32, [](PrVec3 d) {
            float sun = prF32Pow(prF32Max(prVec3Dot(d, prVec3Normalize({0.5f, 0.7f, 0.3f})), 0), 8) * 6;
            float sky = d.y > 0 ? 0.8f : 0.15f;
            return PrVec3{sky + sun, sky + sun, sky + sun};
        }));
        CAPTURE(sunny);
        CHECK(sunny < 0.05f);
    });
}

TEST_CASE("a rough white surface shows the diffuse light of the environment, turned and scaled as the scene says") {
    onEachDevice([](Fixture& f) {
        CubeTexels cube = cubeOf(32, smoothRadiance);
        PrRenderEnvironment environment = createEnvironment(f, cube);
        SphereScene s = createSphereScene(f);
        setSphereMaterial(f, s, 0, 1);
        PrAffine3 turn = turnAboutY(0.9f);
        setEnvironment(f, s.scene, environment, 1.5f, turn);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image image = f.view(s.view);
            float largest = 0;
            for (uint32_t y = 2; y < kExtent.y; y += 4) {
                for (uint32_t x = 2; x < kExtent.x; x += 4) {
                    SphereHit hit = sphereHit(x, y);
                    if (!hit.hit || prVec3Dot(hit.normal, hit.toEye) < 0.3f)
                        continue;
                    PrVec3 diffuse = prRenderEnvironmentIrradiance(f.render, environment, unturned(turn, hit.normal));
                    for (uint32_t channel = 0; channel < 3; ++channel)
                        largest = prF32Max(largest, prF32Abs(image.at(x, y)[channel] - (&diffuse.x)[channel] * 1.5f));
                }
            }
            // A rough white dielectric sends on the diffuse light, but for the 1% or so that its specular lobe takes
            // from the filtered radiance about the mirror direction instead, which differs from it by less than 1.
            CAPTURE(largest);
            CHECK(largest < 0.015f);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The split sum

TEST_CASE("the table of the split sum is within 0.02 of the lobe's integral between its entries, and 0.001 on average") {
    // The table as the shaders read it, filtered between its entries, at the middles of 32 by 32 cells of the roughness
    // and the cosine, which lie between the table's entries, against the integral there.
    const SplitSumTable& table = splitSumTable();
    double largestScale = 0;
    double largestBias = 0;
    double meanScale = 0;
    double meanBias = 0;
    for (uint32_t r = 0; r < kTableSide; ++r) {
        for (uint32_t v = 0; v < kTableSide; ++v) {
            const SplitSum& integrated = table.entries[r][v];
            SplitSum read = referenceSplitSum((r + 0.5) / kTableSide, (v + 0.5) / kTableSide);
            double scale = prF64Abs(read.scale - integrated.scale);
            double bias = prF64Abs(read.bias - integrated.bias);
            largestScale = scale > largestScale ? scale : largestScale;
            largestBias = bias > largestBias ? bias : largestBias;
            meanScale += scale / (kTableSide * kTableSide);
            meanBias += bias / (kTableSide * kTableSide);
            // The scale and the bias are parts of the light: neither is negative, and they add up to no more than all
            // of it.
            CHECK(read.scale >= 0);
            CHECK(read.bias >= 0);
            CHECK(read.scale + read.bias <= 1.0005);
        }
    }
    CAPTURE(largestScale);
    CAPTURE(largestBias);
    CAPTURE(meanScale);
    CAPTURE(meanBias);
    // The scale is off by 0.0064 at most and 0.0007 on average, and the bias by 0.017 at most and 0.0002 on average.
    // The largest errors lie where a smooth surface is seen at a grazing angle, between the table's first rows.
    CHECK(largestScale < 0.008);
    CHECK(largestBias < 0.02);
    CHECK(meanScale < 0.001);
    CHECK(meanBias < 0.0004);
    // The table itself: a mirror seen head on reflects everything, with almost no bias, and the roughest lobe seen head
    // on reflects under a third.
    CHECK(table.entries[0][kTableSide - 1].scale > 0.99);
    CHECK(table.entries[0][kTableSide - 1].bias < 0.01);
    CHECK(table.entries[kTableSide - 1][kTableSide - 1].scale < 0.36);
}

// ---------------------------------------------------------------------------------------------------------------------
// Spheres under an environment

TEST_CASE("a white sphere in a white furnace returns all that it receives, rough or smooth, metal or not") {
    onEachDevice([](Fixture& f) {
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));
        const float metallics[] = {0, 0.5f, 1};
        const float roughnesses[] = {0, 0.25f, 0.5f, 0.75f, 1};
        for (float metallic : metallics) {
            for (float roughness : roughnesses) {
                CAPTURE(metallic);
                CAPTURE(roughness);
                setSphereMaterial(f, s, metallic, roughness);
                for (PrRenderTechnique technique : techniques(f)) {
                    CAPTURE(technique);
                    s.view.technique = technique;
                    Image image = f.view(s.view);
                    float lowest = 1e9f;
                    float highest = 0;
                    for (uint32_t i = 0; i < kExtent.x * kExtent.y; ++i) {
                        lowest = prF32Min(lowest, image.texels[i * 4]);
                        highest = prF32Max(highest, image.texels[i * 4]);
                    }
                    // The sphere is as bright as the furnace behind it at every pixel, to the last step of a half
                    // float: it makes no light, and with the light that bounces between its microfacets added back, it
                    // loses none.
                    CAPTURE(lowest);
                    CAPTURE(highest);
                    CHECK(highest <= 1.001f);
                    CHECK(lowest >= 0.999f);
                }
            }
        }

        // A gray sphere absorbs light, the more of it the rougher it is as a metal, since the light that bounces again
        // is absorbed again. No pixel of it is brighter than the furnace.
        setSphereMaterial(f, s, 1, 1, 0.5f);
        Image gray = f.view(s.view);
        float center = gray.at(kCenter, kCenter)[0];
        CHECK(center < 0.3f);
        CHECK(center > 0.15f);
        CHECK(center == doctest::Approx(referenceIndirect(0.5, 1, 1, 1, 1, 1)).epsilon(0.01));
        setSphereMaterial(f, s, 1, 0, 0.5f);
        CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(0.5).epsilon(0.01));
        // The single lobe alone would return under a third of the furnace at the middle of a rough white metal sphere.
        CHECK(integrateSplitSum(1, 1).scale + integrateSplitSum(1, 1).bias < 0.32);
    });
}

TEST_CASE("a mirror sphere shows the environment along the directions that it reflects") {
    onEachDevice([](Fixture& f) {
        PrRenderEnvironment environment = createEnvironment(f, cubeOf(64, smoothRadiance));
        SphereScene s = createSphereScene(f);
        setSphereMaterial(f, s, 1, 0);
        const float angles[] = {0, 2.1f};
        for (float angle : angles) {
            CAPTURE(angle);
            PrAffine3 turn = turnAboutY(angle);
            setEnvironment(f, s.scene, environment, 1, turn);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                float largest = 0;
                uint32_t compared = 0;
                for (uint32_t y = 0; y < kExtent.y; ++y) {
                    for (uint32_t x = 0; x < kExtent.x; ++x) {
                        SphereHit hit = sphereHit(x, y);
                        // Near the sphere's edge, the reflected direction changes fast across a pixel.
                        float nv = hit.hit ? prVec3Dot(hit.normal, hit.toEye) : 0;
                        if (nv < 0.35f)
                            continue;
                        PrVec3 mirrored = hit.normal * (2 * nv) - hit.toEye;
                        PrVec3 expected = smoothRadiance(unturned(turn, mirrored));
                        for (uint32_t channel = 0; channel < 3; ++channel)
                            largest = prF32Max(largest, prF32Abs(image.at(x, y)[channel] - (&expected.x)[channel]));
                        ++compared;
                    }
                }
                // The sphere's facets and the cube's texels both stand between the pixel and the environment's own
                // value, which varies by 0.7 across its directions. The largest difference here is 0.002.
                CAPTURE(largest);
                CHECK(compared > 400);
                CHECK(largest < 0.005f);
            }
        }
    });
}

TEST_CASE("a rougher metal sphere shows a bright light of the environment larger and dimmer") {
    onEachDevice([](Fixture& f) {
        // A dark environment with one bright patch behind the camera, which the middle of the sphere mirrors.
        PrRenderEnvironment environment = createEnvironment(f, cubeOf(64, [](PrVec3 d) {
            float light = d.z > 0.98f ? 30.0f : 0.02f;
            return PrVec3{light, light, light};
        }));
        SphereScene s = createSphereScene(f);
        setEnvironment(f, s.scene, environment);
        float lastCenter = 1e9f;
        uint32_t lastLit = 0;
        const float roughnesses[] = {0, 0.34f, 0.67f, 1};
        for (float roughness : roughnesses) {
            CAPTURE(roughness);
            setSphereMaterial(f, s, 1, roughness);
            Image image = f.view(s.view);
            float center = image.at(kCenter, kCenter)[0];
            uint32_t lit = 0;
            for (uint32_t i = 0; i < kExtent.x * kExtent.y; ++i) {
                if (image.texels[i * 4] > 0.1f)
                    ++lit;
            }
            CHECK(center < lastCenter);
            CHECK(lit > lastLit);
            lastCenter = center;
            lastLit = lit;
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Backgrounds

TEST_CASE("a view's background is the environment along each pixel's direction, by either technique") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderEnvironment environment = createEnvironment(f, cubeOf(64, smoothRadiance));
        PrAffine3 turn = turnAboutY(-1.2f);
        setEnvironment(f, scene, environment, 2, turn);
        // A camera off every axis, with a target that is not square.
        PrRenderViewDesc view = prRenderViewDescDefault(scene);
        PrVec3 eye{1, 2, 3};
        PrVec3 target{-2, 0.5f, -1};
        float fov = 1.1f;
        view.extent = {40, 24};
        float aspect = float(view.extent.x) / float(view.extent.y);
        view.camera = prRenderCameraLookAt(eye, target, fov, aspect, 0.1f);
        PrVec3 forward = prVec3Normalize(target - eye);
        PrVec3 right = prVec3Normalize({-forward.z, 0, forward.x});
        PrVec3 up = prVec3Cross(right, forward);
        float tangent = prF32Tan(fov * 0.5f);
        const uint8_t sampleCounts[] = {1, 4};
        for (PrRenderTechnique technique : techniques(f)) {
            for (uint8_t samples : sampleCounts) {
                CAPTURE(technique);
                CAPTURE(samples);
                view.technique = technique;
                view.samples = samples;
                Image image = f.view(view);
                float largest = 0;
                for (uint32_t y = 0; y < view.extent.y; ++y) {
                    for (uint32_t x = 0; x < view.extent.x; ++x) {
                        float px = ((float(x) + 0.5f) / float(view.extent.x) * 2 - 1) * tangent * aspect;
                        float py = (1 - (float(y) + 0.5f) / float(view.extent.y) * 2) * tangent;
                        PrVec3 direction = prVec3Normalize(forward + right * px + up * py);
                        PrVec3 expected = smoothRadiance(unturned(turn, direction)) * 2.0f;
                        for (uint32_t channel = 0; channel < 3; ++channel)
                            largest = prF32Max(largest, prF32Abs(image.at(x, y)[channel] - (&expected.x)[channel]));
                    }
                }
                CAPTURE(largest);
                CHECK(largest < 0.004f);
            }
        }
    });
}

TEST_CASE("a scene shows the environment that it was last given, and black once that is destroyed") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderViewDesc view = frontView(scene);
        // A scene without an environment is black.
        CHECK(near(f.view(view).at(kCenter, kCenter), {0, 0, 0}, 0));
        PrRenderEnvironment red = createEvenEnvironment(f, {1, 0, 0});
        PrRenderEnvironment green = createEvenEnvironment(f, {0, 1, 0});
        setEnvironment(f, scene, red);
        CHECK(near(f.view(view).at(kCenter, kCenter), {1, 0, 0}, 1e-4f));
        setEnvironment(f, scene, green, 0.5f);
        CHECK(near(f.view(view).at(kCenter, kCenter), {0, 0.5f, 0}, 1e-4f));

        // Setting an environment again replaces its light, and the frame before the next one keeps the old.
        CubeTexels blue = cubeOf(1, [](PrVec3) { return PrVec3{0, 0, 1}; });
        PrRenderRadianceImage faces[6];
        for (uint32_t face = 0; face < 6; ++face) {
            const uint8_t* data = reinterpret_cast<const uint8_t*>(blue.at(face, 0, 0));
            faces[face] = {.pixels = {data, 3 * sizeof(float), {1, 1}}, .channels = 3};
        }
        PrGpuTexture before = prRenderEnvironmentInfo(f.render, green).texture;
        REQUIRE(prRenderEnvironmentSetCube(f.render, green, faces) == PrStatus_Ok);
        CHECK(prRenderEnvironmentInfo(f.render, green).texture.id == before.id);
        CHECK(near(f.view(view).at(kCenter, kCenter), {0, 0, 0.5f}, 1e-4f));
        PrVec3 diffuse = prRenderEnvironmentIrradiance(f.render, green, {0, 1, 0});
        CHECK(near(&diffuse.x, {0, 0, 1}, 1e-4f));

        prRenderEnvironmentDestroy(f.render, green);
        CHECK(prRenderEnvironmentIsReady(f.render, green));
        CHECK(near(f.view(view).at(kCenter, kCenter), {0, 0, 0}, 0));
        CHECK_FALSE(prRenderEnvironmentIsReady(f.render, green));
        CHECK(prRenderEnvironmentIsReady(f.render, red));
        diffuse = prRenderEnvironmentIrradiance(f.render, green, {0, 1, 0});
        CHECK(near(&diffuse.x, {0, 0, 0}, 0));
        // The destroyed environment's id names the next one.
        PrRenderEnvironment next = prRenderEnvironmentCreate(f.render);
        CHECK(next.id == green.id);
        CHECK_FALSE(prRenderEnvironmentIsReady(f.render, next));
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The sky

TEST_CASE("the sky is blue overhead, paler and brighter at the horizon and around the sun, over a lit ground") {
    onEachDevice([](Fixture& f) {
        PrRenderSkyDesc sky = prRenderSkyDescDefault();
        sky.sunDirection = {0.6f, 0.8f, 0};
        sky.sunIrradiance = 10;
        sky.groundAlbedo = {0.3f, 0.2f, 0.1f};
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        f.frame();
        f.record();
        CubeTexels cube = readEnvironment(f, environment, 0);
        uint32_t size = kEnvironmentSize;
        uint32_t middle = size / 2;
        // Overhead: more blue than green, and more green than red.
        const float* zenith = cube.at(2, middle, middle);
        CHECK(zenith[2] > zenith[1] * 1.3f);
        CHECK(zenith[1] > zenith[0] * 1.2f);
        CHECK(zenith[0] > 0);
        // Just above the horizon away from the sun, the light crosses far more air: brighter than overhead, and less
        // blue.
        const float* horizon = cube.at(5, middle, middle - 2);
        CHECK(horizon[1] > zenith[1] * 1.5f);
        CHECK(horizon[2] / horizon[0] < zenith[2] / zenith[0]);
        CHECK(horizon[2] > horizon[0]);
        // The haze scatters forward, so the sky near the sun is brighter than the sky as far from the zenith on the
        // other side.
        PrVec3 sun = prVec3Normalize(sky.sunDirection);
        float nearSun = 0;
        float opposite = 0;
        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                PrVec3 d = cubeTexelDirection(2, x, y, size);
                if (prVec3Dot(d, sun) > 0.995f)
                    nearSun = cube.at(2, x, y)[1];
                if (prVec3Dot(d, PrVec3{-sun.x, sun.y, sun.z}) > 0.995f)
                    opposite = cube.at(2, x, y)[1];
            }
        }
        CHECK(nearSun > opposite * 3);
        // The ground is one color: its albedo times what the sun and the sky give it, over pi. The sky gives a level
        // surface a tenth to a quarter of its red light.
        PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
        const float* ground = cube.at(3, middle, middle);
        CHECK(near(cube.at(3, 3, size - 5), {ground[0], ground[1], ground[2]}, 1e-4f));
        CHECK(near(cube.at(0, middle, middle + 3), {ground[0], ground[1], ground[2]}, 1e-4f));
        float fromSun = 0.3f * sunlight.x * sun.y / float(kPi);
        CHECK(ground[0] > fromSun * 1.1f);
        CHECK(ground[0] < fromSun * 1.35f);
        // The sun's light at the ground: most of what arrives above the air, and red more than blue.
        CHECK(sunlight.x < 10);
        CHECK(sunlight.x > sunlight.y);
        CHECK(sunlight.y > sunlight.z);
        CHECK(sunlight.z > 6);

        // The diffuse light of the sky on a plane that faces up is its light summed over the hemisphere.
        PrVec3 up = prRenderEnvironmentIrradiance(f.render, environment, {0, 1, 0});
        PrVec3 summed = cubeIrradiance(cube, {0, 1, 0});
        CHECK(up.z == doctest::Approx(double(summed.z)).epsilon(0.08));
    });
}

TEST_CASE("the sky's tables hold the model's integrals: a cube's texels are the integrals taken without tables") {
    // The sun 30 degrees up, toward +x, over a ground that reflects a fifth.
    PrRenderSkyDesc sky = prRenderSkyDescDefault();
    sky.sunDirection = {prF32Cos(float(kPi) / 6), prF32Sin(float(kPi) / 6), 0};
    sky.sunIrradiance = 1;
    PrDVec3 sun{double(sky.sunDirection.x), double(sky.sunDirection.y), 0};
    // Texels of the faces toward +y, +z, +x and -x: near the zenith, low to the side of the sun, between the sun and
    // the zenith, and low on the side away from the sun.
    struct Texel {
        uint32_t face;
        uint32_t x;
        uint32_t y;
        double expected[3];
    };
    Texel texels[] = {
        {2, kEnvironmentSize / 2, kEnvironmentSize / 2, {}},
        {4, kEnvironmentSize / 2 + 3, kEnvironmentSize / 2 - 5, {}},
        {2, kEnvironmentSize - 6, kEnvironmentSize / 2 + 2, {}},
        {1, kEnvironmentSize / 2 - 4, kEnvironmentSize / 2 - 10, {}},
    };
    for (Texel& texel : texels) {
        PrVec3 d = prVec3Normalize(cubeTexelDirection(texel.face, texel.x, texel.y, kEnvironmentSize));
        referenceSky({double(d.x), double(d.y), double(d.z)}, sun, 1, 0.2, true, texel.expected);
    }

    // The integrals themselves, where they have a closed form. With the sun overhead, the light that reaches the viewer
    // from straight up after one scattering was scattered forward at some height, and crossed all of the atmosphere on
    // the one line whatever that height. So its radiance is the sun's irradiance, times the part that crosses the
    // atmosphere, times each scatterer's coefficient, phase function straight ahead, and amount: its density at the
    // ground times the height that its density falls by e over.
    double single[3];
    double through[3];
    referenceSky({0, 1, 0}, {0, 1, 0}, 1, 0.2, false, single);
    referenceSunlight(kSkyGround + 1, 1, 1, through);
    for (uint32_t channel = 0; channel < 3; ++channel) {
        CAPTURE(channel);
        double depth = kSkyAir[channel] * 8000 + kSkyHaze * 1200 + kSkyOzone[channel] * 15000;
        CHECK(through[channel] == doctest::Approx(prF64Exp(-depth)).epsilon(0.002));
        double g = 0.76;
        double airPhase = 3 / (16 * kPi) * 2;
        double hazePhase = 3 / (8 * kPi) * (1 - g * g) * 2 / ((2 + g * g) * (1 - g) * (1 - g) * (1 - g));
        double expected = prF64Exp(-depth) * (kSkyAir[channel] * airPhase * 8000 + kSkyHaze * 0.9 * hazePhase * 1200);
        CHECK(single[channel] == doctest::Approx(expected).epsilon(0.005));
    }
    // The light scattered more than once adds six tenths to the green light of the zenith of a sun 30 degrees up.
    double once[3];
    PrDVec3 zenith{0, 1, 0};
    referenceSky(zenith, sun, 1, 0.2, false, once);
    double all[3];
    referenceSky(zenith, sun, 1, 0.2, true, all);
    CHECK(all[1] / once[1] > 1.3);
    CHECK(all[1] / once[1] < 2);

    onEachDevice([&](Fixture& f) {
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        f.frame();
        f.record();
        CubeTexels cube = readEnvironment(f, environment, 0);
        for (const Texel& texel : texels) {
            CAPTURE(texel.face);
            const float* held = cube.at(texel.face, texel.x, texel.y);
            for (uint32_t channel = 0; channel < 3; ++channel) {
                CAPTURE(channel);
                // The tables' steps, and the entries between which a texel reads them, cost 1.1% at most here.
                CHECK(double(held[channel]) == doctest::Approx(texel.expected[channel]).epsilon(0.02));
            }
        }
        // The sunlight at the ground is integrated for itself, without a table.
        PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
        double expected[3];
        referenceSunlight(kSkyGround + 1, sun.y, 1, expected);
        for (uint32_t channel = 0; channel < 3; ++channel)
            CHECK(double((&sunlight.x)[channel]) == doctest::Approx(expected[channel]).epsilon(0.005));
    });
}

TEST_CASE("a clear day's sun and sky light a level surface as the daylight tables say") {
    onEachDevice([](Fixture& f) {
        // The Illuminating Engineering Society's daylight availability for a clear sky (IES RP-21-84, and the IESNA
        // Lighting Handbook): the sun's illuminance on a plane that faces it is the illuminance above the atmosphere,
        // 127.5 klx, times exp(-0.21 m), where m is the relative air mass, 1 / sin of the sun's elevation, and the
        // sky's illuminance on a level surface is 0.8 + 15.5 sqrt(sin of the elevation) klx. RP-21 itself was not to
        // hand. The forms, 127.5 klx and the sky's constants are Gillette, Treado and Pierpoint's (NBS, 1982), as
        // Robbins and Hunter quote them ("A Model for Illuminance on Horizontal and Vertical Surfaces",
        // SERI/TR-254-1703, 1983, equations 2-16 and 2-17). The clear sky's extinction of 0.21 and the sky's constants
        // are the IESNA Lighting Handbook's, as Golmohammadi, Shekari and Mahjub tabulate them ("Estimation of
        // Horizontal Illuminance for Clear Skies in Iran", Journal of Applied Sciences 9, 2009).
        struct Day {
            float elevation;        // of the sun, in degrees
            double sunTolerance;
            double skyTolerance;
            double shareTolerance;
        };
        // With the sun 30 degrees up and higher, the model's sun is within 1.1% of the tables' and its sky within 1.4%.
        // The haze's amount was chosen for the sun to be so, and the sky follows. At 10 degrees the path through the
        // air is 5.8 times as long: the model's sun is 9% above the tables' and its sky 13% below, where the tables'
        // one exponent for every wavelength and the model's three part ways.
        const Day days[] = {
            {60, 0.02, 0.03, 0.03},
            {45, 0.02, 0.03, 0.03},
            {30, 0.02, 0.03, 0.03},
            {10, 0.12, 0.16, 0.13},
        };
        double lastShare = 0;
        for (const Day& day : days) {
            CAPTURE(day.elevation);
            float elevation = day.elevation * float(kPi) / 180;
            PrRenderSkyDesc sky = prRenderSkyDescDefault();
            sky.sunDirection = {prF32Cos(elevation), prF32Sin(elevation), 0};
            sky.sunIrradiance = 1;
            PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
            REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
            f.frame();
            f.record();
            CubeTexels cube = readEnvironment(f, environment, 0);
            PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
            PrVec3 fromSky = cubeIrradiance(cube, {0, 1, 0}) * float(kPi);
            double sine = double(prF32Sin(elevation));
            double sun = luminance(&sunlight.x);
            double skyLight = luminance(&fromSky.x);
            double tableSun = prF64Exp(-0.21 / sine);
            double tableSky = (0.8 + 15.5 * squareRoot(sine)) / 127.5;
            CHECK(sun == doctest::Approx(tableSun).epsilon(day.sunTolerance));
            CHECK(skyLight == doctest::Approx(tableSky).epsilon(day.skyTolerance));
            // The sky's share of a level surface's light: 15% with the sun 60 degrees up, 17% at 45 and 22% at 30, in
            // the model and in the tables alike, and 47% at 10 degrees, where the tables have 52%.
            double share = skyLight / (skyLight + sun * sine);
            double tableShare = tableSky / (tableSky + tableSun * sine);
            CHECK(share == doctest::Approx(tableShare).epsilon(day.shareTolerance));
            CHECK(share > lastShare);
            lastShare = share;
            prRenderEnvironmentDestroy(f.render, environment);
        }
    });
}

TEST_CASE("a clear day's zenith and horizon are as bright as the published skies'") {
    onEachDevice([](Fixture& f) {
        const float elevations[] = {60, 45, 30};
        for (float degrees : elevations) {
            CAPTURE(degrees);
            float elevation = degrees * float(kPi) / 180;
            PrRenderSkyDesc sky = prRenderSkyDescDefault();
            sky.sunDirection = {prF32Cos(elevation), prF32Sin(elevation), 0};
            sky.sunIrradiance = 1;
            PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
            REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
            f.frame();
            f.record();
            CubeTexels cube = readEnvironment(f, environment, 0);
            // The zenith's luminance of a clear sky in kcd/m2, for a turbidity factor T, which is 1 for an ideal clear
            // atmosphere: (1.376 T - 1.81) tan(elevation) + 0.38, as Radiance's gensky computes it (src/gen/gensky.c,
            // whose default T is 2.45). The formula is attributed to Karayel, Navvab, Ne'eman and Selkowitz ("Zenith
            // luminance and sky luminance distributions for daylighting calculations", Energy and Buildings 6, 1984),
            // which was not to hand. The test takes 2 to 3.5 as the turbidities of clear skies. The model's zenith is
            // that of a turbidity of 2.8 to 2.9: 1.5, 2.3 and 4.0 kcd/m2 under a sun of 127.5 klx at 30, 45 and 60
            // degrees. The four texels about the zenith are a degree from it.
            double zenith = luminance(cube.at(2, kEnvironmentSize / 2, kEnvironmentSize / 2));
            double tangent = double(prF32Tan(elevation));
            CHECK(zenith > ((1.376 * 2 - 1.81) * tangent + 0.38) / 127.5);
            CHECK(zenith < ((1.376 * 3.5 - 1.81) * tangent + 0.38) / 127.5);

            // The luminance of the sky a quarter turn from the sun against the zenith's, by the CIE's standard clear
            // sky of low turbidity (CIE S 011/E:2003, ISO 15469:2004, type 12): its gradation 1 - exp(-0.32 / cos Z) at
            // the angle Z from the zenith, times its indicatrix 1 + 10 (exp(-3 x) - exp(-3 pi / 2)) + 0.45 cos2 x at
            // the angle x from the sun (type 12's a, b, c, d and e of -1, -0.32, 10, -3 and 0.45, as the standard was
            // not to hand: as the colour library's colour.phenomena.sky.cie2003 reproduces its table and formulae).
            // That gradation has the form of the light that an atmosphere of optical depth 0.32 scatters once, and the
            // model's clear day, which is the IES's, has 0.21: so the model's horizon is brighter against its zenith.
            // The model's ratio is 1.07 to 1.14 times the CIE's at 40 degrees above the horizon, 1.19 to 1.31 times at
            // 20 degrees and 1.29 to 1.46 times at 10 degrees.
            const float heights[] = {10, 20, 40};
            for (float height : heights) {
                CAPTURE(height);
                float angle = height * float(kPi) / 180;
                PrVec3 toward{0, prF32Sin(angle), prF32Cos(angle)};
                // The texel of the face toward +z that is nearest to the direction.
                float nearest = -1;
                const float* texel = nullptr;
                for (uint32_t y = 0; y < cube.size; ++y) {
                    for (uint32_t x = 0; x < cube.size; ++x) {
                        float cosine = prVec3Dot(prVec3Normalize(cubeTexelDirection(4, x, y, cube.size)), toward);
                        if (cosine > nearest) {
                            nearest = cosine;
                            texel = cube.at(4, x, y);
                        }
                    }
                }
                REQUIRE(texel != nullptr);
                double fromSun = double(prF32Acos(prVec3Dot(toward, sky.sunDirection)));
                double model = luminance(texel) / zenith;
                double standard = cieClearSky(kPi / 2 - double(angle), fromSun) /
                    cieClearSky(0, kPi / 2 - double(elevation));
                CHECK(model > standard);
                CHECK(model < standard * 1.5);
            }
            prRenderEnvironmentDestroy(f.render, environment);
        }
    });
}

TEST_CASE("a sun below the horizon lights nothing directly") {
    onEachDevice([](Fixture& f) {
        PrRenderSkyDesc sky = prRenderSkyDescDefault();
        sky.sunDirection = {1, -0.3f, 0};
        PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
        CHECK(near(&sunlight.x, {0, 0, 0}, 0));
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        f.frame();
        f.record();
        CubeTexels cube = readEnvironment(f, environment, 0);
        // The sky is far darker than by day, and nothing in it is negative or not a number.
        float brightest = 0;
        for (float value : cube.texels) {
            CHECK(value >= 0);
            brightest = prF32Max(brightest, value);
        }
        CHECK(brightest < 0.2f);
        // A sun without a direction is overhead.
        sky.sunDirection = {0, 0, 0};
        PrRenderSkyDesc overhead = sky;
        overhead.sunDirection = {0, 1, 0};
        CHECK(prRenderSkySunIrradiance(&sky).y == prRenderSkySunIrradiance(&overhead).y);
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// The sun

TEST_CASE("the sun's disc is in a view's background, with the light that the sky's sun gives the ground") {
    onEachDevice([](Fixture& f) {
        PrRenderSkyDesc sky = prRenderSkyDescDefault();
        sky.sunDirection = {0.3f, 0.5f, -0.8f};
        sky.sunIrradiance = 1;
        PrVec3 sun = prVec3Normalize(sky.sunDirection);
        PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        // The scene turns the environment and doubles it, and the camera looks at where the sun then is, through a view
        // 0.04 radians wide, which a disc 0.0093 radians wide fills a quarter of.
        PrAffine3 turn = turnAboutY(0.7f);
        setEnvironment(f, scene, environment, 2, turn);
        const float (*m)[4] = turn.m;
        PrVec3 turned{
            m[0][0] * sun.x + m[0][1] * sun.y + m[0][2] * sun.z,
            m[1][0] * sun.x + m[1][1] * sun.y + m[1][2] * sun.z,
            m[2][0] * sun.x + m[2][1] * sun.y + m[2][2] * sun.z,
        };
        PrRenderViewDesc view = prRenderViewDescDefault(scene);
        constexpr uint32_t kSide = 64;
        constexpr float kFov = 0.04f;
        view.extent = {kSide, kSide};
        view.camera = prRenderCameraLookAt({0, 0, 0}, turned, kFov, 1, 0.1f);
        const uint8_t sampleCounts[] = {1, 4};
        for (PrRenderTechnique technique : techniques(f)) {
            for (uint8_t samples : sampleCounts) {
                CAPTURE(technique);
                CAPTURE(samples);
                view.technique = technique;
                view.samples = samples;
                Image image = f.view(view);
                // The sky behind the disc is what the view's corner shows: the sky changes little across 0.04 radians.
                const float* corner = image.at(1, 1);
                double pixelAngle = 2 * double(prF32Tan(kFov / 2)) / kSide;
                double light[3] = {0, 0, 0};
                for (uint32_t y = 0; y < kSide; ++y) {
                    for (uint32_t x = 0; x < kSide; ++x) {
                        for (uint32_t channel = 0; channel < 3; ++channel) {
                            light[channel] +=
                                double(image.at(x, y)[channel] - corner[channel]) * pixelAngle * pixelAngle;
                        }
                    }
                }
                // The disc's light, summed over the pixels' solid angles, is the irradiance of the sky's sun, times the
                // scene's factor.
                for (uint32_t channel = 0; channel < 3; ++channel) {
                    CAPTURE(channel);
                    CHECK(light[channel] == doctest::Approx(double((&sunlight.x)[channel]) * 2).epsilon(0.02));
                }
                // The disc is at the middle of the view, 7.4 pixels in radius, and darkens toward its edge by the
                // linear law: at 6.5 pixels from the middle, to 0.4 + 0.6 sqrt(1 - (6.5 / 7.4)^2) of the middle.
                const float* middle = image.at(kSide / 2, kSide / 2);
                double mean = double(sunlight.y) * 2 / (kPi * 0.004654 * 0.004654);
                CHECK(double(middle[1] - corner[1]) == doctest::Approx(mean / 0.8).epsilon(0.02));
                const float* outer = image.at(kSide / 2 + 6, kSide / 2);
                double dimmed = double(outer[1] - corner[1]) / double(middle[1] - corner[1]);
                CHECK(dimmed == doctest::Approx(0.69).epsilon(0.05));
                CHECK(image.at(kSide / 2 + 10, kSide / 2)[1] < corner[1] * 1.5f);
            }
        }

        // No disc is in the cube that lights surfaces: its brightest texel near the sun is the sky's glow, thousands of
        // times dimmer than the disc.
        CubeTexels cube = readEnvironment(f, environment, 0);
        float brightest = 0;
        for (uint32_t face = 0; face < 6; ++face) {
            for (uint32_t y = 0; y < cube.size; ++y) {
                for (uint32_t x = 0; x < cube.size; ++x)
                    brightest = prF32Max(brightest, cube.at(face, x, y)[1]);
            }
        }
        CHECK(brightest < sunlight.y / (float(kPi) * 0.004654f * 0.004654f) * 1e-3f);

        // A sky without a disc has none in its background.
        sky.sunAngularRadius = 0;
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        view.technique = PrRenderTechnique_Forward;
        Image plain = f.view(view);
        CHECK(plain.at(kSide / 2, kSide / 2)[1] < plain.at(1, 1)[1] * 1.5f);
    });
}

TEST_CASE("a white surface without gloss under the sky and its sun leaves what the two give it, once") {
    onEachDevice([](Fixture& f) {
        PrRenderSkyDesc sky = prRenderSkyDescDefault();
        sky.sunDirection = {0.5f, 0.7f, 0.4f};
        sky.sunIrradiance = 4;
        PrVec3 toSun = prVec3Normalize(sky.sunDirection);
        PrVec3 sunlight = prRenderSkySunIrradiance(&sky);
        PrRenderEnvironment environment = prRenderEnvironmentCreate(f.render);
        REQUIRE(prRenderEnvironmentSetSky(f.render, environment, &sky) == PrStatus_Ok);
        SphereScene s = createSphereScene(f);
        setSphereMaterial(f, s, 0, 1);
        setEnvironment(f, s.scene, environment);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLightDesc sun = prRenderLightDescDefault();
        sun.direction = toSun * -1.0f;
        sun.color = sunlight;
        prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);
        f.frame();
        f.record();
        CubeTexels cube = readEnvironment(f, environment, 0);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image image = f.view(s.view);
            float largest = 0;
            uint32_t compared = 0;
            for (uint32_t y = 2; y < kExtent.y; y += 4) {
                for (uint32_t x = 2; x < kExtent.x; x += 4) {
                    SphereHit hit = sphereHit(x, y);
                    if (!hit.hit || prVec3Dot(hit.normal, hit.toEye) < 0.3f)
                        continue;
                    // The sky's light is the cube's, summed over the hemisphere about the normal: the sky and the
                    // ground, without the sun. The harmonics hold it to within 5% of the sky's mean light.
                    PrVec3 fromSky = cubeIrradiance(cube, hit.normal);
                    double nv = double(prVec3Dot(hit.normal, hit.toEye));
                    for (uint32_t channel = 0; channel < 3; ++channel) {
                        double direct = whiteRoughDirect(hit.normal, hit.toEye, toSun) * double((&sunlight.x)[channel]);
                        double skyLight = double((&fromSky.x)[channel]);
                        double expected = direct + referenceIndirect(1, 0, 1, nv, skyLight, skyLight);
                        largest = prF32Max(largest, prF32Abs(image.at(x, y)[channel] - float(expected)));
                        ++compared;
                    }
                }
            }
            // The sun gives the surface that faces it an irradiance of 3, so a radiance of 0.95, and the sky adds 0.1
            // to 0.2. A disc of the sun in the environment's light would add the sun's 0.95 again. The largest
            // difference here is 0.003.
            CAPTURE(largest);
            CHECK(compared > 60);
            CHECK(largest < 0.01f);
        }
    });
}

TEST_SUITE_END();
