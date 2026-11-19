// Views rendered offscreen and read back: what the equations predict for simple scenes, and the same scene compared
// between binding tiers, backends and techniques.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/render.h"
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

constexpr float kPi = 3.14159265358979323846f;

// ---------------------------------------------------------------------------------------------------------------------
// The shading model on the CPU

// The reference that the images are checked against is in doubles.
using Vec = PrDVec3;

Vec normalized(Vec v) {
    double length = double(prF32Sqrt(float(prDVec3Dot(v, v))));
    return v / length;
}

double saturate(double value) {
    if (value < 0)
        return 0;
    return value > 1 ? 1 : value;
}

// A gray surface, as a material gives it to the shading model.
struct GraySurface {
    double baseColor;
    double metallic;
    double roughness;
    Vec normal;
};

// Returns the radiance that leaves a gray surface toward `toEye` for light from `toLight` with unit irradiance on a
// plane that faces it: the direct lobes of glTF's metallic-roughness model, as pr/render_brdf.slang states them.
double brdfDirect(const GraySurface& s, Vec toEye, Vec toLight) {
    double nl = saturate(prDVec3Dot(s.normal, toLight));
    double nv = prDVec3Dot(s.normal, toEye) > 1e-4 ? prDVec3Dot(s.normal, toEye) : 1e-4;
    if (nl <= 0)
        return 0;
    Vec h = normalized({toEye.x + toLight.x, toEye.y + toLight.y, toEye.z + toLight.z});
    double nh = saturate(prDVec3Dot(s.normal, h));
    double vh = saturate(prDVec3Dot(toEye, h));
    double alpha = s.roughness * s.roughness > 0.002 ? s.roughness * s.roughness : 0.002;
    double alpha2 = alpha * alpha;
    double d = nh * nh * (alpha2 - 1) + 1;
    double distribution = alpha2 / (double(kPi) * d * d);
    double visibility = 0.5 / (nl * double(prF32Sqrt(float(nv * nv * (1 - alpha2) + alpha2))) +
        nv * double(prF32Sqrt(float(nl * nl * (1 - alpha2) + alpha2))));
    double schlick = double(prF32Pow(float(1 - vh), 5));
    double f0 = 0.04 + (s.baseColor - 0.04) * s.metallic;
    double fresnel = f0 + (1 - f0) * schlick;
    // The diffuse lobe is the dielectric's, under the dielectric's own reflection.
    double diffuse = (1 - (0.04 + 0.96 * schlick)) * (1 - s.metallic) * s.baseColor / double(kPi);
    return (diffuse + fresnel * distribution * visibility) * nl;
}

// Returns the part of a white furnace's light that a white surface sends toward an eye at the cosine `nv` from its
// normal: brdfDirect summed over the hemisphere of light directions.
double furnace(double metallic, double roughness, double nv) {
    constexpr uint32_t kPolar = 256;
    constexpr uint32_t kAround = 512;
    GraySurface surface{1, metallic, roughness, {0, 0, 1}};
    Vec toEye{double(prF32Sqrt(float(1 - nv * nv))), 0, nv};
    double sum = 0;
    for (uint32_t i = 0; i < kPolar; ++i) {
        double nl = (i + 0.5) / kPolar;
        double sine = double(prF32Sqrt(float(1 - nl * nl)));
        for (uint32_t j = 0; j < kAround; ++j) {
            float around = (float(j) + 0.5f) / kAround * 2 * kPi;
            sum += brdfDirect(surface, toEye, {sine * double(prF32Cos(around)), sine * double(prF32Sin(around)), nl});
        }
    }
    return sum * 2 * double(kPi) / (kPolar * kAround);
}

// ---------------------------------------------------------------------------------------------------------------------
// Scenes

// The world's point at the center of a pixel of the front view, on the plane z = 0.
Vec frontPoint(uint32_t x, uint32_t y) {
    return {(x + 0.5) / kExtent.x * 2 - 1, 1 - (y + 0.5) / kExtent.y * 2, 0};
}

// The unit direction from a point toward the front view's camera.
Vec towardCamera(Vec point) {
    return normalized({-point.x, -point.y, double(kCameraDistance) - point.z});
}

// A scene with one quad that fills the front view, with a material that the test changes.
struct QuadScene {
    PrRenderScene scene;
    PrRenderMesh quad;
    PrRenderMaterial material;
    PrRenderInstance instance;
    PrRenderViewDesc view;
};

QuadScene createQuadScene(Fixture& f, PrRenderMaterial material) {
    QuadScene s{};
    s.scene = prRenderSceneCreate(f.render);
    s.quad = createQuad(f);
    s.material = material;
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
    s.instance = addInstance(update, s.quad, s.material, placed({0, 0, 0}));
    prRenderUpdateCommit(update);
    s.view = frontView(s.scene);
    return s;
}

// A white, rough dielectric.
PrRenderKhronosPbrMaterial whiteRough() {
    PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
    material.metallic = 0;
    return material;
}

void addLight(Fixture& f, PrRenderScene scene, const PrRenderLightDesc& light) {
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    prRenderLightAdd(update, &light);
    prRenderUpdateCommit(update);
}

// A directional light of `lux` that shines along `direction`.
PrRenderLightDesc sunLight(PrVec3 direction, float lux) {
    PrRenderLightDesc light = prRenderLightDescDefault();
    light.direction = direction;
    light.intensity = lux;
    return light;
}

// The techniques that a device draws with.
Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

// Returns the value in linear light that sRGB's encoding holds as `encoded`, also above 1.
float srgbDecode(float encoded) {
    return encoded <= 0.04045f ? encoded / 12.92f : prF32Pow((encoded + 0.055f) / 1.055f, 2.4f);
}

// The tone maps of the display on the CPU, as lib/render's display shader states them: what a display of `headroom`
// shows for a scene-referred color, in linear light with SDR white at 1.
float neutralCompress(float peak) {
    return peak < 0.76f ? peak : 1 - 0.24f * 0.24f / (peak + 0.24f - 0.76f);
}

PrVec3 toneMapNeutral(PrVec3 color, float headroom) {
    float low = prF32Min(color.x, prF32Min(color.y, color.z));
    float offset = low < 0.08f ? low - 6.25f * low * low : 0.04f;
    color = color - PrVec3{offset, offset, offset};
    float peak = prF32Max(color.x, prF32Max(color.y, color.z));
    if (peak < 0.76f)
        return color;
    float white = neutralCompress(0.96f);
    float scale = (headroom - white) / (1 - white);
    float compressed = neutralCompress(prF32Min(peak, 0.96f) + prF32Max(peak - 0.96f, 0) / scale);
    float newPeak = compressed + (scale - 1) * prF32Max(compressed - white, 0);
    color = color * (newPeak / peak);
    float g = 1 - 1 / (0.15f * (peak - newPeak) + 1);
    return color + (PrVec3{newPeak, newPeak, newPeak} - color) * g;
}

float acesCurve(float value) {
    float x = value * 0.5f;
    return prF32Clamp(x * (2.51f * x + 0.03f) / (x * (2.43f * x + 0.59f) + 0.14f), 0, 1);
}

float acesMapped(float value, float headroom) {
    float white = acesCurve(1);
    float scale = (headroom - white) / (1 - white);
    float mapped = acesCurve(prF32Min(value, 1) + prF32Max(value - 1, 0) / scale);
    return mapped + (scale - 1) * prF32Max(mapped - white, 0);
}

PrVec3 toneMapAcesChannels(PrVec3 color, float headroom) {
    return {acesMapped(color.x, headroom), acesMapped(color.y, headroom), acesMapped(color.z, headroom)};
}

float luminanceOf(PrVec3 color) {
    return 0.2126f * color.x + 0.7152f * color.y + 0.0722f * color.z;
}

PrVec3 toneMapAcesLuminance(PrVec3 color, float headroom) {
    float luminance = luminanceOf(color);
    if (luminance <= 0)
        return {};
    float mapped = acesMapped(luminance, headroom);
    color = color * (mapped / luminance);
    float white = acesCurve(1);
    float ceiling = 1 + (prF32Max(mapped, white) - white) * (headroom - 1) / (headroom - white);
    float room = prF32Max(ceiling - mapped, 0);
    float excess = prF32Max(color.x, prF32Max(color.y, color.z)) - mapped;
    if (excess <= 0.75f * room)
        return color;
    float d = 0.25f * room;
    float compressed = room - d * d / (excess + room - 1.5f * room);
    PrVec3 gray{mapped, mapped, mapped};
    return gray + (color - gray) * (compressed / excess);
}

PrVec3 toneMapped(PrRenderToneMap toneMap, PrVec3 color, float headroom) {
    if (toneMap == PrRenderToneMap_AcesChannels)
        return toneMapAcesChannels(color, headroom);
    if (toneMap == PrRenderToneMap_AcesLuminance)
        return toneMapAcesLuminance(color, headroom);
    return toneMapNeutral(color, headroom);
}

// A scene-referred value on the neutral axis, and what ACES's reference rendering shows for it.
struct AcesReference {
    float scene;
    float shown;
};

// The tone scale of ACES 1.0.3's reference rendering for an sRGB display of 100 nits in a dim surround (RRT.ctl,
// ODT.Academy.sRGB_100nits_dim.ctl), on the neutral axis, in linear light with the display's white at 1. A neutral
// value passes through the RRT's tone scale, the tone scale of the 48-nit ODT, the scaling of 0.02 to 48 nits into [0,
// 1], and the dim surround's gamma of 0.9811. The values are computed from the splines of ACESlib.Tonescales.ctl, which
// put middle gray at 4.8 nits.
constexpr AcesReference kAcesReference[] = {
    {0.01f, 0.00099f}, {0.02f, 0.00319f}, {0.045f, 0.01201f}, {0.09f, 0.03605f}, {0.18f, 0.10406f}, {0.36f, 0.26356f},
    {0.72f, 0.51510f}, {1, 0.62432f},     {2, 0.79481f},      {4, 0.90127f},     {8, 0.96495f},     {16, 0.99941f},
};

// How far Narkowicz's fit lies from the reference's tone scale at most, at the exposure that the display gives it.
constexpr float kAcesFitError = 0.014f;

// Returns a color's saturation, as HSV defines it.
float saturationOf(PrVec3 color) {
    float high = prF32Max(color.x, prF32Max(color.y, color.z));
    float low = prF32Min(color.x, prF32Min(color.y, color.z));
    return (high - low) / high;
}

// Returns where a color's green lies between its blue and its red, which is the hue of a color whose red is its largest
// channel and whose blue is its smallest. Mixing the color with a gray does not change it.
float hueOf(PrVec3 color) {
    return (color.y - color.z) / (color.x - color.z);
}

// ---------------------------------------------------------------------------------------------------------------------
// A scene with every feature

constexpr PrUVec2 kGalleryExtent{64, 64};

// Creates a scene that draws with every feature at once, for comparing one way of rendering with another: textured,
// masked, blended, double-sided and mirrored surfaces under a sun with shadows, a point light and a spot light.
PrRenderViewDesc createGallery(Fixture& f) {
    PrRenderScene scene = prRenderSceneCreate(f.render);
    PrRenderMesh quad = createQuad(f);
    PrRenderMesh halves = createQuad(f, true);

    Array<uint8_t> checker;
    for (uint32_t i = 0; i < 64; ++i) {
        bool light = ((i % 8) + (i / 8)) % 2 == 0;
        const uint8_t texel[4] = {uint8_t(light ? 230 : 60), uint8_t(light ? 200 : 90), uint8_t(light ? 120 : 160), 255};
        checker.append(texel, 4);
    }
    const uint8_t normals[16] = {180, 128, 220, 255, 128, 180, 220, 255, 80, 128, 220, 255, 128, 80, 220, 255};
    const uint8_t surface[16] = {255, 80, 0, 255, 128, 200, 255, 255, 200, 140, 60, 255, 90, 255, 128, 255};
    const uint8_t alpha[16] = {255, 255, 255, 60, 255, 255, 255, 200, 255, 255, 255, 220, 255, 255, 255, 30};
    PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Repeat);
    PrRenderTexture textures[4];
    const uint8_t* texels[4] = {checker.begin(), normals, surface, alpha};
    const PrUVec2 extents[4] = {{8, 8}, {2, 2}, {2, 2}, {2, 2}};
    for (uint32_t i = 0; i < 4; ++i) {
        sampler.address[0] = i == 3 ? PrGpuAddress_Clamp : PrGpuAddress_Repeat;
        textures[i] = prRenderTextureCreate(f.render);
        f.samplers.set(textures[i].id, sampler);
        PrGpuFormat format = i == 0 ? PrGpuFormat_Rgba8Srgb : PrGpuFormat_Rgba8;
        PrRenderTextureData data{.format = format, .extent = extents[i], .levelCount = 1};
        data.levels[0] = {texels[i], 0};
        REQUIRE(prRenderTextureSetData(f.render, textures[i], &data) == PrStatus_Ok);
    }

    PrRenderKhronosPbrMaterial ground = whiteRough();
    ground.roughness = 0.6f;
    ground.metallic = 0.5f;
    ground.baseColorTexture = useOf(f, textures[0]);
    ground.baseColorTexture.transform = prRenderTextureTransform({0.1f, 0.2f}, 0.3f, {2, 2});
    ground.normalTexture = useOf(f, textures[1]);
    ground.normalTexture.transform = prAffine2Scale({3, 3});
    ground.normalScale = 0.8f;
    ground.metallicRoughnessTexture = useOf(f, textures[2]);
    ground.occlusionTexture = useOf(f, textures[2]);
    ground.occlusionTexture.texCoord = 1;
    ground.occlusionStrength = 0.7f;
    PrRenderUnlitMaterial picture = prRenderUnlitMaterialDefault();
    picture.colorTexture = useOf(f, textures[0]);
    PrRenderKhronosPbrMaterial masked = whiteRough();
    masked.baseColor = {0.9f, 0.8f, 0.2f, 1};
    masked.alphaMode = PrRenderAlphaMode_Mask;
    masked.baseColorTexture = useOf(f, textures[3]);
    masked.flags |= PrRenderMaterialFlag_DoubleSided;
    PrRenderKhronosPbrMaterial glass = whiteRough();
    glass.baseColor = {0.2f, 0.4f, 0.9f, 0.5f};
    glass.roughness = 0.3f;
    glass.alphaMode = PrRenderAlphaMode_Blend;
    PrRenderKhronosPbrMaterial backed = whiteRough();
    backed.baseColor = {0.3f, 0.8f, 0.4f, 1};
    backed.flags |= PrRenderMaterialFlag_DoubleSided;
    PrRenderKhronosPbrMaterial glowing = whiteRough();
    glowing.baseColor = {0.1f, 0.1f, 0.1f, 1};
    glowing.emissive = {1, 0.4f, 0.1f};
    glowing.emissiveStrength = 3;
    PrRenderKhronosPbrMaterial metal = prRenderKhronosPbrMaterialDefault();
    metal.baseColor = {0.9f, 0.7f, 0.4f, 1};
    metal.roughness = 0.35f;
    PrRenderMaterial materials[8] = {
        createGltf(f, ground),
        createMaterial(f, picture),
        createGltf(f, masked),
        createGltf(f, glass),
        createUnlit(f, {0.9f, 0.1f, 0.2f, 0.4f}, PrRenderAlphaMode_Blend),
        createGltf(f, backed),
        createGltf(f, glowing),
        createGltf(f, metal),
    };

    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    addInstance(update, quad, materials[0], facingUp({0, -1, 0}, 4));
    addInstance(update, quad, materials[1], placed({-2, 0, -1}, {0.8f, 0.8f, 1}));
    addInstance(update, quad, materials[2], placed({-0.5f, 0, 0}, {0.7f, 0.9f, 1}));
    addInstance(update, quad, materials[3], placed({0.6f, 0, 1}, {0.8f, 0.8f, 1}));
    addInstance(update, quad, materials[4], placed({0.2f, 0.2f, 1.6f}, {0.6f, 0.5f, 1}));
    // A quad that faces away from the camera, and one that a reflection mirrors.
    addInstance(update, quad, materials[5], placed({1.8f, 0, -0.5f}, {-0.7f, 0.9f, -1}));
    addInstance(update, quad, materials[7], placed({2.2f, 0.3f, 0.8f}, {-0.6f, 0.6f, 1}));
    PrRenderInstanceDesc two = prRenderInstanceDescDefault(halves);
    const PrRenderMaterial pair[2] = {materials[6], materials[7]};
    two.materials = pair;
    two.materialCount = 2;
    two.world = placed({-1.5f, 1.4f, 0.4f}, {0.5f, 0.4f, 1});
    prRenderInstanceAdd(update, &two);

    PrRenderLightDesc sun = sunLight({-0.4f, -1, -0.3f}, 3);
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
    PrRenderLightDesc spot = prRenderLightDescDefault();
    spot.kind = PrRenderLightKind_Spot;
    spot.position = {-2, 2, 2};
    spot.direction = {2, -3, -2};
    spot.color = {0.4f, 0.6f, 1};
    spot.intensity = 30;
    spot.innerConeAngle = 0.25f;
    spot.outerConeAngle = 0.5f;
    prRenderLightAdd(update, &spot);
    prRenderUpdateCommit(update);

    // An environment that differs in every direction, with a bright patch, turned and dimmed for the scene. Its cube
    // has a level for each of three roughnesses.
    PrRenderEnvironment environment = createEnvironment(f, cubeOf(32, [](PrVec3 d) {
        float height = d.y * 0.5f + 0.5f;
        float patch = prF32Max(d.x * 0.6f + d.y * 0.64f + d.z * 0.48f, 0);
        float glow = 6 * prF32Pow(patch, 24);
        return PrVec3{0.1f + 0.3f * height + glow, 0.12f + 0.4f * height + glow * 0.9f, 0.15f + 0.65f * height + glow * 0.7f};
    }));
    setEnvironment(f, scene, environment, 0.8f, PrAffine3{{{0.8f, 0, 0.6f, 0}, {0, 1, 0, 0}, {-0.6f, 0, 0.8f, 0}}});

    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = prRenderCameraLookAt({2.5f, 2.2f, 5.5f}, {0, 0, 0}, 0.9f, 1, 0.1f);
    view.extent = kGalleryExtent;
    return view;
}

// ---------------------------------------------------------------------------------------------------------------------
// A minified ground

constexpr PrUVec2 kGroundExtent{96, 96};
constexpr uint32_t kLevelSide = 64;
constexpr uint32_t kLevelCount = 7;

// The step of 8 bits between the values of two levels of the level texture.
constexpr uint32_t kLevelStep = 32;

// Creates a texture whose every level holds the level's own number, as kLevelStep times it in 8 bits, filtered linearly
// between levels. A surface that shows it shows the level that its derivatives chose.
PrRenderTexture createLevelTexture(Fixture& f, uint8_t maxAnisotropy) {
    Array<uint8_t> texels[kLevelCount];
    PrRenderTextureData data{
        .format = PrGpuFormat_Rgba8,
        .extent = {kLevelSide, kLevelSide},
        .levelCount = kLevelCount,
    };
    for (uint32_t level = 0; level < kLevelCount; ++level) {
        uint32_t side = kLevelSide >> level;
        texels[level].resize(side * side * 4);
        for (uint8_t& value : texels[level])
            value = uint8_t(level * kLevelStep);
        data.levels[level] = {texels[level].begin(), 0};
    }
    PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Repeat);
    sampler.maxAnisotropy = maxAnisotropy;
    PrRenderTexture texture = prRenderTextureCreate(f.render);
    f.samplers.set(texture.id, sampler);
    REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);
    return texture;
}

// Returns the level that a texel of an image of the level texture shows.
float levelOf(float value) {
    return value * 255 / float(kLevelStep);
}

// Creates a scene of one ground plane that shows `texture` many times over, turned and moved by a texture transform,
// and returns a view with one sample from a camera that looks along it, so that the texture is minified everywhere and
// by more along the view than across it. The plane is unlit, or with `lit` a glTF material under a sun.
PrRenderViewDesc createGround(Fixture& f, PrRenderTexture texture, bool lit) {
    PrRenderScene scene = prRenderSceneCreate(f.render);
    PrRenderTextureUse use = useOf(f, texture);
    use.transform = prRenderTextureTransform({0.3f, 0.1f}, 0.4f, {64, 64});
    PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
    unlit.colorTexture = use;
    PrRenderKhronosPbrMaterial rough = whiteRough();
    rough.baseColorTexture = use;
    PrRenderMaterial material = lit ? createMaterial(f, rough) : createMaterial(f, unlit);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
    addInstance(update, createQuad(f), material, facingUp({0, -1, -10}, 16));
    if (lit) {
        PrRenderLightDesc sun = sunLight({-0.3f, -1, -0.2f}, 3);
        prRenderLightAdd(update, &sun);
    }
    prRenderUpdateCommit(update);
    PrRenderViewDesc view = prRenderViewDescDefault(scene);
    view.camera = prRenderCameraLookAt({0.4f, 0.5f, 5}, {0, -1.6f, 0}, 0.9f, 1, 0.1f);
    view.extent = kGroundExtent;
    view.samples = 1;
    return view;
}

// A position on the ground of createGround, in the level texture's texels. The texture's transform turns and moves
// these coordinates, which changes no length.
struct GroundTexel {
    double u;
    double v;
};

// Returns the point of the ground's plane that the position (`x`, `y`) of the view shows, in the target's pixels.
GroundTexel groundTexel(const PrRenderViewDesc& view, double x, double y) {
    PrMatrix4 m = prMatrix4Concat(&view.camera.view, &view.camera.projection);
    // A point (px, -1, pz) of the plane is at the clip position m * (px, -1, pz, 1). It shows at (x, y) where its clip
    // x and y are the position's normalized coordinates times its clip w, which is two linear equations in px and pz.
    double nx = x / view.extent.x * 2 - 1;
    double ny = 1 - y / view.extent.y * 2;
    double rows[2][3];
    const double normalized[2] = {nx, ny};
    for (uint32_t row = 0; row < 2; ++row) {
        double n = normalized[row];
        rows[row][0] = double(m.m[row][0]) - n * double(m.m[3][0]);
        rows[row][1] = double(m.m[row][2]) - n * double(m.m[3][2]);
        rows[row][2] = (double(m.m[row][1]) - double(m.m[row][3])) - n * (double(m.m[3][1]) - double(m.m[3][3]));
    }
    double determinant = rows[0][0] * rows[1][1] - rows[0][1] * rows[1][0];
    double px = (rows[0][2] * rows[1][1] - rows[0][1] * rows[1][2]) / determinant;
    double pz = (rows[0][0] * rows[1][2] - rows[0][2] * rows[1][0]) / determinant;
    // The plane is 32 units wide, and shows the texture 64 times over.
    double texelsPerUnit = double(kLevelSide) * 64 / 32;
    return {px * texelsPerUnit, pz * texelsPerUnit};
}

// Returns the level that a sampler reads for a pixel whose texture position changes by (`ux`, `vx`) texels across one
// pixel to the right and by (`uy`, `vy`) one pixel down. Without anisotropy, the level is the logarithm of the longer
// change. With it, the two changes span an ellipse of texels. The sampler takes up to `anisotropy` samples along the
// ellipse's longer axis, and the level is the logarithm of that axis over the number of samples. That is the shorter
// axis until the ratio of the two passes `anisotropy`.
double levelFor(double ux, double vx, double uy, double vy, uint8_t anisotropy) {
    double x = ux * ux + vx * vx;
    double y = uy * uy + vy * vy;
    double footprint = prF64Sqrt(prF64Max(x, y));
    if (anisotropy > 1) {
        // The squares of the axes are the eigenvalues of the changes' matrix times its transpose.
        double determinant = ux * vy - uy * vx;
        double spread = prF64Sqrt(prF64Max((x + y) * (x + y) - 4 * determinant * determinant, 0));
        double longer = prF64Sqrt((x + y + spread) / 2);
        double shorter = prF64Sqrt((x + y - spread) / 2);
        footprint = longer / prF64Min(longer / shorter, double(anisotropy));
    }
    return prF64Min(prF64Max(prF64Log2(footprint), 0), double(kLevelCount - 1));
}

// The levels that the two kinds of derivative choose at a pixel of the ground.
struct GroundLevels {
    double block;       // the difference between the pixel and its neighbor in its block of two by two
    double exact;       // the derivative at the pixel's center
};

// Returns the levels that a sampler of `anisotropy` reads at the pixel (`x`, `y`) of a view of the ground.
GroundLevels groundLevels(const PrRenderViewDesc& view, uint32_t x, uint32_t y, uint8_t anisotropy) {
    double cx = x + 0.5;
    double cy = y + 0.5;
    GroundTexel here = groundTexel(view, cx, cy);
    GroundTexel beside = groundTexel(view, cx + ((x & 1) == 0 ? 1 : -1), cy);
    GroundTexel below = groundTexel(view, cx, cy + ((y & 1) == 0 ? 1 : -1));
    // The plane's texels are a ratio of two linear functions of the pixel, so a central difference over a small step is
    // the derivative to many more digits than the levels are compared to.
    constexpr double kStep = 1.0 / 1024;
    GroundTexel left = groundTexel(view, cx - kStep, cy);
    GroundTexel right = groundTexel(view, cx + kStep, cy);
    GroundTexel up = groundTexel(view, cx, cy - kStep);
    GroundTexel down = groundTexel(view, cx, cy + kStep);
    return {
        .block = levelFor(beside.u - here.u, beside.v - here.v, below.u - here.u, below.v - here.v, anisotropy),
        .exact = levelFor((right.u - left.u) / (2 * kStep), (right.v - left.v) / (2 * kStep),
            (down.u - up.u) / (2 * kStep), (down.v - up.v) / (2 * kStep), anisotropy),
    };
}

// Returns what the display shows for a scene-referred color, in linear light: the center of the view of a quad scene,
// whose material becomes an unlit one of that color.
PrVec3 shownColor(Fixture& f, const QuadScene& s, PrVec3 color, const PrRenderDisplayOptions& display) {
    PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
    unlit.color = {color.x, color.y, color.z, 1};
    prRenderMaterialSetUnlit(f.render, s.material, &unlit);
    Image shown = f.shown(s.view, display);
    const float* texel = shown.at(kCenter, kCenter);
    return PrVec3{srgbDecode(texel[0]), srgbDecode(texel[1]), srgbDecode(texel[2])};
}

// Creates a square of side 2 about the origin in the plane z = 0, which faces along z, whose corners are red, green,
// blue and white.
PrRenderMesh createColoredQuad(Fixture& f) {
    const PrColor colors[4] = {{1, 0, 0, 1}, {0, 1, 0, 1}, {0, 0, 1, 1}, {1, 1, 1, 1}};
    Array<FixtureVertex> vertices;
    for (uint32_t i = 0; i < 4; ++i) {
        FixtureVertex v;
        v.position = {float(i % 2) * 2 - 1, 1 - float(i / 2) * 2, 0};
        v.normal = {0, 0, 1};
        v.uv0 = {float(i % 2), float(i / 2)};
        v.color = colors[i];
        vertices.push(v);
    }
    const uint32_t corners[6] = {0, 2, 3, 0, 3, 1};
    Array<uint32_t> indices;
    for (uint32_t corner : corners)
        indices.push(corner);
    PrRenderMeshPart part{.firstIndex = 0, .indexCount = 6};
    PrRenderMesh mesh = prRenderMeshCreate(f.render);
    setMesh(f, mesh, vertices, indices, &part, 1);
    return mesh;
}

// Returns the number of pixels of a gallery image that the scene's surfaces cover: those that differ from the view of
// the environment alone, which a view that sees no instance draws.
uint32_t coveredPixels(Fixture& f, PrRenderViewDesc view, const Image& image) {
    view.mask = 0;
    Image background = f.view(view);
    uint32_t covered = 0;
    for (uint32_t y = 0; y < image.extent.y; ++y) {
        for (uint32_t x = 0; x < image.extent.x; ++x) {
            const float* texel = background.at(x, y);
            if (!near(image.at(x, y), {texel[0], texel[1], texel[2]}, 1e-3f))
                ++covered;
        }
    }
    return covered;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render images" * doctest::skip(!gpuTests()));

// ---------------------------------------------------------------------------------------------------------------------
// What the equations predict

TEST_CASE("an unlit material's color comes back exactly once the display's transform is undone") {
    onEachDevice([](Fixture& f) {
        const PrVec3 color{0.25f, 0.5f, 0.75f};
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.color = {color.x, color.y, color.z, 1};
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        // Lights and an environment change nothing of an unlit surface.
        addLight(f, s.scene, sunLight({0, 0, -1}, 5));
        setEnvironment(f, s.scene, createEvenEnvironment(f, {1, 1, 1}));

        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            Image linear = f.view(s.view);
            // The view's color holds the material's color to the precision of a half float.
            CHECK(near(linear.at(kCenter, kCenter), color, 2e-4f));
            CHECK(near(linear.at(1, 1), color, 2e-4f));
            CHECK(near(linear.at(kExtent.x - 2, kExtent.y - 2), color, 2e-4f));

            // The neutral tone map changes no value this low, but for the 4% that it takes away.
            Image shown = f.shown(s.view, prRenderDisplayOptionsDefault());
            const float* texel = shown.at(kCenter, kCenter);
            CHECK(texel[3] == 1);
            for (uint32_t channel = 0; channel < 3; ++channel) {
                CHECK(texel[channel] <= 1);
                CHECK(srgbDecode(texel[channel]) + 0.04f ==
                    doctest::Approx(double((&color.x)[channel])).epsilon(0.004));
            }
        }
    });
}

TEST_CASE("the display shows every color up to diffuse white alike on any display, and highlights by its headroom") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        auto shown = [&](PrVec3 color, PrRenderDisplayOptions display) {
            return shownColor(f, s, color, display);
        };
        for (PrRenderToneMap toneMap = 0; toneMap < PrRenderToneMap_Count; ++toneMap) {
            CAPTURE(prRenderToneMapName(toneMap));
            PrRenderDisplayOptions sdr{.exposure = 1, .headroom = 1, .toneMap = toneMap};
            PrRenderDisplayOptions hdr{.exposure = 1, .headroom = 4, .toneMap = toneMap};

            // Shadows, middle gray, a saturated color and diffuse white are shown exactly alike with and without
            // headroom, as the curve of the tone map alone puts them.
            const PrVec3 tones[] = {{0.02f, 0.02f, 0.02f}, {0.18f, 0.18f, 0.18f}, {0.9f, 0.3f, 0.05f}, {1, 1, 1}};
            for (PrVec3 tone : tones) {
                CAPTURE(tone.x);
                PrVec3 low = shown(tone, sdr);
                PrVec3 high = shown(tone, hdr);
                CHECK(near(&high.x, low, 1e-4f));
                CHECK(near(&low.x, toneMapped(toneMap, tone, 1), 2e-3f));
                CHECK(low.x <= 1);
            }

            // A highlight stays below 1 without headroom. With headroom it is brighter, the more so the brighter the
            // scene, and never above the headroom.
            float last = shown({1, 1, 1}, hdr).x;
            const float highlights[] = {2, 8, 32, 4000};
            for (float highlight : highlights) {
                CAPTURE(highlight);
                PrVec3 gray{highlight, highlight, highlight};
                float low = shown(gray, sdr).x;
                float high = shown(gray, hdr).x;
                CHECK(low <= 1.0001f);
                CHECK(high > low);
                CHECK(high > last);
                CHECK(high <= 4.001f);
                CHECK(high == doctest::Approx(double(toneMapped(toneMap, gray, 4).x)).epsilon(0.005));
                last = high;
            }
            // The brightest highlight uses nearly all of the headroom.
            CHECK(last > 3.9f);

            // An exposure of one quarter shows a value of 2 as a value of one half is shown.
            PrRenderDisplayOptions quarter{.exposure = 0.25f, .headroom = 1, .toneMap = toneMap};
            CHECK(shown({2, 2, 2}, quarter).x ==
                doctest::Approx(double(shown({0.5f, 0.5f, 0.5f}, sdr).x)).epsilon(0.002));
        }

        // The neutral tone map keeps a base color: below the start of its compression it only takes away the 4% that a
        // dielectric reflects. The ACES curve darkens middle gray, and darkens diffuse white further.
        PrRenderDisplayOptions neutral = prRenderDisplayOptionsDefault();
        PrVec3 kept = shown({0.6f, 0.3f, 0.2f}, neutral);
        CHECK(near(&kept.x, {0.56f, 0.26f, 0.16f}, 2e-3f));
        PrRenderDisplayOptions aces{.exposure = 1, .headroom = 1, .toneMap = PrRenderToneMap_AcesChannels};
        CHECK(shown({0.18f, 0.18f, 0.18f}, aces).x < 0.12f);
        CHECK(shown({1, 1, 1}, aces).x < shown({1, 1, 1}, neutral).x);
    });
}

TEST_CASE("the ACES tone maps show a neutral as the tone scale of ACES's reference rendering does") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        const PrRenderToneMap toneMaps[] = {PrRenderToneMap_AcesChannels, PrRenderToneMap_AcesLuminance};
        for (PrRenderToneMap toneMap : toneMaps) {
            CAPTURE(prRenderToneMapName(toneMap));
            PrRenderDisplayOptions display{.exposure = 1, .headroom = 1, .toneMap = toneMap};
            float last = 0;
            for (AcesReference reference : kAcesReference) {
                CAPTURE(reference.scene);
                // The fit itself is within its error of the reference, and the display within the rounding of a half
                // float of the fit.
                CHECK(prF32Abs(acesCurve(reference.scene) - reference.shown) <= kAcesFitError);
                PrVec3 shown = shownColor(f, s, {reference.scene, reference.scene, reference.scene}, display);
                CHECK(prF32Abs(shown.x - reference.shown) <= kAcesFitError + 1e-3f);
                // A neutral stays neutral, and a brighter one is shown brighter.
                CHECK(prF32Abs(shown.y - shown.x) <= 1e-4f);
                CHECK(prF32Abs(shown.z - shown.x) <= 1e-4f);
                CHECK(shown.x > last);
                CHECK(shown.x <= 1);
                last = shown.x;
            }
        }
    });
}

TEST_CASE("the ACES curve on luminance keeps a color's hue, and its saturation until the color nears the gamut") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        // A saturated orange with a luminance of 0.41, whose red is 2.2 times its luminance.
        const PrVec3 color{0.9f, 0.3f, 0.05f};
        const float hue = hueOf(color);
        const float saturation = saturationOf(color);

        // The exposures run from 4 stops under to 12 stops over. Up to an exposure of 1 the scaled color lies inside
        // the display's gamut, away from its boundary, and its chromaticity is the scene's. Above that the color loses
        // saturation, more with each stop, and ends as white. Its hue stays, and its luminance is the curve's at every
        // exposure.
        const float exposures[] = {0.0625f, 0.25f, 1, 4, 16, 64, 4096};
        float lastLuminance = 0;
        float lastSaturation = saturation;
        for (float exposure : exposures) {
            CAPTURE(exposure);
            PrRenderDisplayOptions display{
                .exposure = exposure,
                .headroom = 1,
                .toneMap = PrRenderToneMap_AcesLuminance,
            };
            PrVec3 shown = shownColor(f, s, color, display);
            CHECK(near(&shown.x, toneMapAcesLuminance(color * exposure, 1), 2e-3f));
            CHECK(prF32Max(shown.x, prF32Max(shown.y, shown.z)) <= 1);
            CHECK(prF32Min(shown.x, prF32Min(shown.y, shown.z)) >= 0);

            float luminance = luminanceOf(shown);
            CHECK(luminance == doctest::Approx(double(acesCurve(luminanceOf(color) * exposure))).epsilon(0.005));
            CHECK(luminance >= lastLuminance);
            if (lastLuminance < 0.99f)
                CHECK(luminance > lastLuminance);
            lastLuminance = luminance;

            if (exposure <= 1) {
                CHECK(shown.y / shown.x == doctest::Approx(double(color.y / color.x)).epsilon(0.01));
                CHECK(shown.z / shown.x == doctest::Approx(double(color.z / color.x)).epsilon(0.01));
                CHECK(saturationOf(shown) == doctest::Approx(double(saturation)).epsilon(0.002));
            } else if (lastSaturation > 0.001f) {
                CHECK(saturationOf(shown) < lastSaturation - 0.05f);
            }
            // A color that is nearly white has no hue to measure.
            if (saturationOf(shown) > 0.05f)
                CHECK(hueOf(shown) == doctest::Approx(double(hue)).epsilon(0.01));
            lastSaturation = saturationOf(shown);
        }
        CHECK(lastSaturation < 0.01f);
        // A half float holds the encoded white to one part in a thousand.
        CHECK(lastLuminance > 0.998f);

        // The curve on each channel does not keep the hue: two stops over, the orange is shown nearer to yellow.
        PrRenderDisplayOptions channels{.exposure = 4, .headroom = 1, .toneMap = PrRenderToneMap_AcesChannels};
        CHECK(hueOf(shownColor(f, s, color, channels)) > hue + 0.2f);

        // The primaries, which lie farthest from the gray of their luminance, stay inside the display at any exposure.
        const PrVec3 primaries[] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
        for (PrVec3 primary : primaries) {
            for (float exposure : exposures) {
                PrRenderDisplayOptions display{
                    .exposure = exposure,
                    .headroom = 1,
                    .toneMap = PrRenderToneMap_AcesLuminance,
                };
                PrVec3 shown = shownColor(f, s, primary, display);
                CHECK(prF32Max(shown.x, prF32Max(shown.y, shown.z)) <= 1);
                CHECK(prF32Min(shown.x, prF32Min(shown.y, shown.z)) >= 0);
            }
        }
    });
}

TEST_CASE("the ACES curve on luminance gives headroom to colors whose luminance is above diffuse white's") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        PrRenderDisplayOptions sdr{.exposure = 1, .headroom = 1, .toneMap = PrRenderToneMap_AcesLuminance};
        PrRenderDisplayOptions hdr{.exposure = 1, .headroom = 4, .toneMap = PrRenderToneMap_AcesLuminance};

        // A color whose luminance is at most 1 is shown alike with and without headroom, whatever its channels hold.
        const PrVec3 below[] = {{3, 0.2f, 0.2f}, {0, 0, 10}, {0.2f, 1.2f, 0.1f}, {1.6f, 0.9f, 0.2f}};
        for (PrVec3 color : below) {
            CAPTURE(color.x);
            REQUIRE(luminanceOf(color) <= 1);
            PrVec3 low = shownColor(f, s, color, sdr);
            PrVec3 high = shownColor(f, s, color, hdr);
            CHECK(near(&high.x, low, 1e-4f));
            CHECK(prF32Max(low.x, prF32Max(low.y, low.z)) <= 1);
        }

        // A brighter color keeps more of its saturation with headroom than without, has the luminance that the curve
        // gives it, and stays under the headroom.
        const PrVec3 above[] = {{8, 2, 0.5f}, {2, 6, 1}, {30, 20, 4}};
        for (PrVec3 color : above) {
            CAPTURE(color.x);
            PrVec3 low = shownColor(f, s, color, sdr);
            PrVec3 high = shownColor(f, s, color, hdr);
            CHECK(near(&high.x, toneMapAcesLuminance(color, 4), 0.01f));
            CHECK(luminanceOf(high) == doctest::Approx(double(acesMapped(luminanceOf(color), 4))).epsilon(0.005));
            CHECK(luminanceOf(high) > luminanceOf(low));
            CHECK(saturationOf(high) > saturationOf(low));
            CHECK(prF32Max(high.x, prF32Max(high.y, high.z)) <= 4);
            CHECK(prF32Max(low.x, prF32Max(low.y, low.z)) <= 1);
        }
    });
}

TEST_CASE("a color in shadow keeps its saturation by the ACES curve on luminance, and gains it by the other maps") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        // A white surface in a shadow that a blue sky fills.
        const PrVec3 shadow{0.041f, 0.050f, 0.074f};
        PrVec3 shown[PrRenderToneMap_Count];
        for (PrRenderToneMap toneMap = 0; toneMap < PrRenderToneMap_Count; ++toneMap) {
            CAPTURE(prRenderToneMapName(toneMap));
            PrRenderDisplayOptions display{.exposure = 1, .headroom = 1, .toneMap = toneMap};
            shown[toneMap] = shownColor(f, s, shadow, display);
            CHECK(near(&shown[toneMap].x, toneMapped(toneMap, shadow, 1), 5e-4f));
        }
        CHECK(near(&shown[PrRenderToneMap_Neutral].x, {0.0105f, 0.0195f, 0.0435f}, 5e-4f));
        CHECK(near(&shown[PrRenderToneMap_AcesChannels].x, {0.0109f, 0.0148f, 0.0275f}, 5e-4f));
        CHECK(near(&shown[PrRenderToneMap_AcesLuminance].x, {0.0121f, 0.0148f, 0.0219f}, 5e-4f));

        // The shadow's saturation is 0.45. The curve on luminance keeps it, the curve on each channel raises it, and
        // the neutral map raises it most.
        float saturation = saturationOf(shadow);
        CHECK(saturationOf(shown[PrRenderToneMap_AcesLuminance]) == doctest::Approx(double(saturation)).epsilon(0.01));
        CHECK(saturationOf(shown[PrRenderToneMap_AcesChannels]) > saturation + 0.1f);
        CHECK(saturationOf(shown[PrRenderToneMap_Neutral]) > saturationOf(shown[PrRenderToneMap_AcesChannels]) + 0.1f);
    });
}

TEST_CASE("a white surface in a white furnace returns no more light than it receives") {
    onEachDevice([](Fixture& f) {
        constexpr uint32_t kSide = 8;
        constexpr float kLowRoughness = 0.35f;
        PrGpuShaderLibrary* library = nullptr;
        REQUIRE(prGpuShaderLibraryCreate(f.device, kTestRenderShaders, sizeof(kTestRenderShaders), &library) ==
            PrStatus_Ok);
        PrGpuGraphicsPipelineDesc desc = prGpuGraphicsPipelineDescDefault(library);
        desc.vertex = "furnaceVertex";
        desc.fragment = "furnacePixel";
        desc.formats.colorCount = 1;
        desc.formats.color[0] = PrGpuFormat_Rgba16F;
        PrGpuPipeline pipeline{};
        REQUIRE(prGpuPipelineCreateGraphics(f.device, &desc, &pipeline) == PrStatus_Ok);
        PrGpuTextureDesc targetDesc = prGpuTextureDesc2d(PrGpuFormat_Rgba16F, PrUVec2{kSide, kSide},
            PrGpuTextureUsageFlag_RenderTarget | PrGpuTextureUsageFlag_Sampled);
        PrGpuTexture target{};
        REQUIRE(prGpuTextureCreate(f.device, &targetDesc, &target) == PrStatus_Ok);

        const float metallics[] = {0, 1};
        for (float metallic : metallics) {
            CAPTURE(metallic);
            PrGpuCommands* commands = nullptr;
            REQUIRE(prGpuCommandsBegin(f.device, nullptr, &commands));
            PrGpuSubresources all{};
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ShaderRead,
                PrGpuAccessFlag_ColorAttachment);
            PrGpuPassDesc passDesc = prGpuPassDescColor(target, PrGpuLoad_Clear, PrColor{});
            PrGpuPass* pass = prGpuPassBegin(commands, &passDesc);
            const float push[4] = {metallic, float(kSide), kLowRoughness, 0};
            prGpuCommandsSetPipeline(commands, pipeline);
            prGpuCommandsSetPush(commands, push, sizeof(push));
            prGpuPassDraw(pass, 3, 1, 0, 0);
            prGpuPassEnd(pass);
            prGpuCommandsTextureBarrier(commands, target, all, PrGpuAccessFlag_ColorAttachment,
                PrGpuAccessFlag_ShaderRead);
            Image image;
            prGpuCommandsReadTexture(commands, target, PrGpuAccessFlag_ShaderRead, readHalves, &image);
            gpuSubmitAndWait(commands);

            for (uint32_t y = 0; y < kSide; ++y) {
                for (uint32_t x = 0; x < kSide; ++x) {
                    CAPTURE(x);
                    CAPTURE(y);
                    float returned = image.at(x, y)[0];
                    // No energy is made, at any roughness and any angle of the eye. A metal has the specular lobe
                    // alone, which makes no energy at any roughness or angle of the eye. A dielectric mixes its two
                    // lobes by the Fresnel term of each light's half vector, as glTF's model does. That mix returns no
                    // more than it receives while the eye is within about 45 degrees of the normal. At a grazing eye it
                    // returns more: up to a quarter more for the smoothest surface here.
                    bool conserves = metallic == 1 || y >= kSide * 5 / 8;
                    CHECK(returned <= (conserves ? 1.005f : 1.27f));
                    CHECK(returned > 0.25f);
                }
            }
            // The shader's sum agrees with the same equations summed on the CPU.
            const PrUVec2 samples[] = {{0, kSide - 1}, {kSide / 2, kSide / 2}, {kSide - 1, 1}, {kSide - 1, kSide - 1}};
            for (PrUVec2 at : samples) {
                double roughness = double(kLowRoughness) + (1 - double(kLowRoughness)) * (at.x + 0.5) / kSide;
                double expected = furnace(double(metallic), roughness, (at.y + 0.5) / kSide);
                CHECK(image.at(at.x, at.y)[0] == doctest::Approx(expected).epsilon(0.01));
            }
        }
        // A smooth white metal returns nearly all of the light, and the single lobe loses energy as it roughens.
        CHECK(furnace(1, 0.35, 0.9) > 0.97);
        CHECK(furnace(1, 1, 0.9) < furnace(1, 0.35, 0.9));
        prGpuTextureRelease(f.device, target);
        prGpuPipelineRelease(f.device, pipeline);
        prGpuShaderLibraryDestroy(library);
    });
}

TEST_CASE("a blended quad over an opaque one composites as premultiplied alpha predicts, the farthest first") {
    onEachDevice([](Fixture& f) {
        const PrVec3 back{0.1f, 0.6f, 0.9f};
        const PrVec3 farColor{0.9f, 0.2f, 0.1f};
        const PrVec3 nearColor{0.2f, 0.9f, 0.3f};
        constexpr float kFarAlpha = 0.25f;
        constexpr float kNearAlpha = 0.6f;
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial backMaterial = createUnlit(f, {back.x, back.y, back.z, 1});
        PrRenderMaterial farMaterial =
            createUnlit(f, {farColor.x, farColor.y, farColor.z, kFarAlpha}, PrRenderAlphaMode_Blend);
        PrRenderMaterial nearMaterial =
            createUnlit(f, {nearColor.x, nearColor.y, nearColor.z, kNearAlpha}, PrRenderAlphaMode_Blend);
        // The nearest quad is added first, so the order of the draws must come from the view.
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, nearMaterial, placed({-1, 0, 1}));
        addInstance(update, quad, farMaterial, placed({0, 0, 0}));
        addInstance(update, quad, backMaterial, placed({0, 0, -1}, {2, 2, 1}));
        prRenderUpdateCommit(update);

        PrVec3 one = farColor * kFarAlpha + back * (1 - kFarAlpha);
        PrVec3 two = nearColor * kNearAlpha + one * (1 - kNearAlpha);
        PrRenderViewDesc view = frontView(scene);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            Image image = f.view(view);
            // The near quad covers the left of the view, over the far quad, which covers all of it.
            CHECK(near(image.at(4, kCenter), two, 1e-3f));
            CHECK(near(image.at(kExtent.x - 4, kCenter), one, 1e-3f));
        }
    });
}

TEST_CASE("an alpha-masked quad cuts out where its alpha is below its cutoff") {
    onEachDevice([](Fixture& f) {
        const PrVec3 back{0, 0, 1};
        const PrVec3 front{1, 1, 0};
        // The left texel's alpha is 0.4, and the right's is 0.6.
        const uint8_t texels[8] = {255, 255, 255, 102, 255, 255, 255, 153};
        PrRenderTexture texture = createTexture(f, {2, 1}, texels);
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.color = {front.x, front.y, front.z, 1};
        unlit.colorTexture = useOf(f, texture);
        unlit.alphaMode = PrRenderAlphaMode_Mask;
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        addInstance(update, s.quad, createUnlit(f, {back.x, back.y, back.z, 1}), placed({0, 0, -1}, {2, 2, 1}));
        prRenderUpdateCommit(update);

        // Each cutoff, and whether the left and the right of the quad are kept at it.
        struct Cut {
            float cutoff;
            bool left;
            bool right;
        };
        const Cut cuts[] = {{0.5f, false, true}, {0.3f, true, true}, {0.7f, false, false}};
        for (const Cut& cut : cuts) {
            CAPTURE(cut.cutoff);
            unlit.alphaCutoff = cut.cutoff;
            prRenderMaterialSetUnlit(f.render, s.material, &unlit);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                // A kept fragment is opaque: its alpha does not blend it.
                CHECK(near(image.at(4, kCenter), cut.left ? front : back, 1e-3f));
                CHECK(near(image.at(kExtent.x - 4, kCenter), cut.right ? front : back, 1e-3f));            }
        }
    });
}

TEST_CASE("a shadowed point is dark, and an unshadowed one is lit as the equations predict") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial white = createGltf(f, whiteRough());
        PrRenderMaterial cut = createUnlit(f, {1, 1, 1, 0}, PrRenderAlphaMode_Mask);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        addInstance(update, quad, white, facingUp({0, 0, 0}, 3));
        PrRenderInstance occluder = addInstance(update, quad, white, facingUp({-1, 1, 0}, 0.5f));
        PrRenderLightDesc sun = sunLight({0, -1, 0}, 2);
        sun.castsShadows = true;
        prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);

        PrRenderViewDesc view = prRenderViewDescDefault(scene);
        view.camera = prRenderCameraLookAt({0, 5, 5}, {0, 0, 0}, 0.9f, 1, 0.1f);
        view.extent = kExtent;
        PrUVec2 shadowed = pixelOf(view, {-1, 0, 0.2f});
        PrUVec2 lit = pixelOf(view, {1, 0, 0.2f});
        Vec toEye = normalized({-1, 5, 4.8});
        double expected = brdfDirect({1, 0, 1, {0, 1, 0}}, toEye, {0, 1, 0}) * 2;

        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            view.shadows = true;
            Image image = f.view(view);
            CHECK(image.at(lit.x, lit.y)[0] == doctest::Approx(expected).epsilon(0.02));
            CHECK(image.at(shadowed.x, shadowed.y)[0] < 0.01f * image.at(lit.x, lit.y)[0]);

            // A view without shadows lights both points.
            view.shadows = false;
            Image unshadowed = f.view(view);
            CHECK(unshadowed.at(shadowed.x, shadowed.y)[0] > 0.8f * unshadowed.at(lit.x, lit.y)[0]);
        }

        // An instance casts no shadow without the shadow bit of its mask, and none where a mask cuts it out.
        view.technique = PrRenderTechnique_Forward;
        view.shadows = true;
        PrRenderUpdate* masks = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetMasks(masks, occluder, PrRenderMobilityFlag_Static, 1);
        prRenderUpdateCommit(masks);
        Image castless = f.view(view);
        CHECK(castless.at(shadowed.x, shadowed.y)[0] > 0.8f * castless.at(lit.x, lit.y)[0]);
        PrRenderUpdate* masked = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetMasks(masked, occluder, PrRenderMobilityFlag_Static, 0xffffffffu);
        prRenderInstanceSetMaterial(masked, occluder, 0, cut);
        prRenderUpdateCommit(masked);
        Image cutOut = f.view(view);
        CHECK(cutOut.at(shadowed.x, shadowed.y)[0] > 0.8f * cutOut.at(lit.x, lit.y)[0]);
    });
}

TEST_CASE("a surface beyond the shadow map's far plane is shadowed only by what the map holds") {
    onEachDevice([](Fixture& f) {
        // The ground casts no shadow, so the map covers the sphere of the occluder alone, which ends above the ground.
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial white = createGltf(f, whiteRough());
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance ground = addInstance(update, quad, white, facingUp({0, 0, 0}, 3));
        prRenderInstanceSetMasks(update, ground, PrRenderMobilityFlag_Static, 1);
        addInstance(update, quad, white, facingUp({-1, 2, 0}, 0.5f));
        PrRenderLightDesc sun = sunLight({0, -1, 0}, 2);
        sun.castsShadows = true;
        prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);

        PrRenderViewDesc view = prRenderViewDescDefault(scene);
        view.camera = prRenderCameraLookAt({0, 5, 5}, {0, 0, 0}, 0.9f, 1, 0.1f);
        view.extent = kExtent;
        // A point under the occluder, a point beside it that the map's square still covers, and a point outside the
        // map.
        PrUVec2 shadowed = pixelOf(view, {-1, 0, 0.2f});
        PrUVec2 beside = pixelOf(view, {-0.38f, 0, 0.2f});
        PrUVec2 lit = pixelOf(view, {1, 0, 0.2f});
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            Image image = f.view(view);
            CHECK(image.at(lit.x, lit.y)[0] > 0.3f);
            CHECK(image.at(shadowed.x, shadowed.y)[0] < 0.01f * image.at(lit.x, lit.y)[0]);
            CHECK(image.at(beside.x, beside.y)[0] > 0.8f * image.at(lit.x, lit.y)[0]);
        }
    });
}

TEST_CASE("a shadow's edge is as wide as the light's source makes it at the distance from what casts it") {
    onEachDevice([](Fixture& f) {
        // A square hangs level over a wide ground that casts no shadow, with its edge over the line x = 0, under a
        // light that shines straight down. From a point of the ground at x, the square hides the part of the light's
        // disc that lies past the edge: the disc's radius on the ground is the square's height times the tangent of the
        // light's angular radius.
        constexpr float kAngularRadius = 0.05f;
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh quad = createQuad(f);
        PrRenderMaterial white = createGltf(f, whiteRough());
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance ground = addInstance(update, quad, white, facingUp({0, 0, 0}, 50));
        prRenderInstanceSetMasks(update, ground, PrRenderMobilityFlag_Static, 1);
        PrRenderInstance occluder = addInstance(update, quad, white, facingUp({-1, 1, 0}, 1));
        PrRenderLightDesc sun = sunLight({0, -1, 0}, 2);
        sun.castsShadows = true;
        sun.angularRadius = kAngularRadius;
        PrRenderLight light = prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);

        // The camera looks along the ground under the square, at the line below its edge.
        PrRenderViewDesc view = prRenderViewDescDefault(scene);
        view.extent = {192, 48};
        view.camera = prRenderCameraLookAt({0, 0.6f, 3}, {0, 0, 0}, 0.12f, 4, 0.1f);
        view.samples = 1;
        const float heights[] = {1, 2};
        for (float height : heights) {
            CAPTURE(height);
            PrRenderUpdate* moved = prRenderUpdateBegin(f.render, scene);
            PrAffine3 at = facingUp({-1, height, 0}, 1);
            prRenderInstanceSetTransform(moved, occluder, &at);
            prRenderUpdateCommit(moved);
            float radius = height * prF32Tan(kAngularRadius);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                view.technique = technique;
                view.shadows = true;
                Image image = f.view(view);
                view.shadows = false;
                Image unshadowed = f.view(view);
                const float places[] = {-1.3f, -0.7f, -0.35f, 0, 0.35f, 0.7f, 1.3f};
                for (float place : places) {
                    CAPTURE(place);
                    PrUVec2 pixel = pixelOf(view, {place * radius, 0, 0});
                    float lit = image.at(pixel.x, pixel.y)[0] / unshadowed.at(pixel.x, pixel.y)[0];
                    // The part of a disc that lies on one side of a line at `place` radii from its middle.
                    float t = prF32Clamp(place, -1, 1);
                    float expected = 1 - (prF32Acos(t) - t * prF32Sqrt(1 - t * t)) / kPi;
                    // The filter's 32 taps count the part in steps of 3%, about a place that the map's texels and the
                    // view's pixels each move by a tenth of the radius or less. The part is within 0.032 here.
                    CHECK(prF32Abs(lit - expected) < 0.06f);
                }
            }
        }

        // A light without size has a sharp shadow: its edge is as wide as the pixels and the map's texels make it,
        // which is under a hundredth here, although the map's view could have held the ground's 100.
        sun.angularRadius = 0;
        PrRenderUpdate* sharpened = prRenderUpdateBegin(f.render, scene);
        prRenderLightSet(sharpened, light, &sun);
        prRenderUpdateCommit(sharpened);
        view.technique = PrRenderTechnique_Forward;
        view.shadows = true;
        Image sharp = f.view(view);
        view.shadows = false;
        Image unshadowed = f.view(view);
        PrUVec2 inside = pixelOf(view, {-0.02f, 0, 0});
        PrUVec2 outside = pixelOf(view, {0.02f, 0, 0});
        CHECK(sharp.at(inside.x, inside.y)[0] < 0.01f * unshadowed.at(inside.x, inside.y)[0]);
        CHECK(sharp.at(outside.x, outside.y)[0] > 0.99f * unshadowed.at(outside.x, outside.y)[0]);
    });
}

TEST_CASE("a normal-mapped plane is lit as its normal map says") {
    onEachDevice([](Fixture& f) {
        // One texel, whose normal leans toward the tangent: (0.6, 0, 0.8) as 8 bits hold it.
        const uint8_t texel[4] = {204, 128, 230, 255};
        Vec mapped = normalized({204 / 255.0 * 2 - 1, 128 / 255.0 * 2 - 1, 230 / 255.0 * 2 - 1});
        PrRenderTexture texture = createTexture(f, {1, 1}, texel);
        PrRenderKhronosPbrMaterial material = whiteRough();
        material.normalTexture = useOf(f, texture);
        QuadScene s = createQuadScene(f, createMaterial(f, material));
        // The light arrives along the mapped normal.
        const Vec toLight = normalized({0.6, 0, 0.8});
        addLight(f, s.scene, sunLight({-0.6f, 0, -0.8f}, 3));
        Vec toEye{0, 0, 1};
        double bent = brdfDirect({1, 0, 1, mapped}, toEye, toLight) * 3;
        double flat = brdfDirect({1, 0, 1, {0, 0, 1}}, toEye, toLight) * 3;
        CHECK(bent > flat * 1.15);

        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            s.view.technique = technique;
            material.normalScale = 1;
            setGltf(f.render, s.material, &material);
            CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(bent).epsilon(0.01));

            // A scale of zero flattens the map, and the plane is lit by its own normal.
            material.normalScale = 0;
            setGltf(f.render, s.material, &material);
            CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(flat).epsilon(0.01));
        }
    });
}

TEST_CASE("point and spot lights fall off with distance, range and cone as glTF defines") {
    onEachDevice([](Fixture& f) {
        PrRenderKhronosPbrMaterial white = whiteRough();
        QuadScene s = createQuadScene(f, createMaterial(f, white));
        PrRenderLightDesc lamp = prRenderLightDescDefault();
        lamp.kind = PrRenderLightKind_Point;
        lamp.position = {0, 0, 2};
        lamp.intensity = 8;
        lamp.color = {1, 0.5f, 0.25f};
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLight light = prRenderLightAdd(update, &lamp);
        prRenderUpdateCommit(update);

        // Returns the red that the lamp gives a pixel of the plane, before any range or cone.
        auto lampRed = [](uint32_t x, uint32_t y, double candela) {
            Vec point = frontPoint(x, y);
            Vec offset{-point.x, -point.y, 2};
            double irradiance = candela / prDVec3Dot(offset, offset);
            return brdfDirect({1, 0, 1, {0, 0, 1}}, towardCamera(point), normalized(offset)) * irradiance;
        };
        Image point = f.view(s.view);
        const float* center = point.at(kCenter, kCenter);
        CHECK(center[0] == doctest::Approx(lampRed(kCenter, kCenter, 8)).epsilon(0.01));
        CHECK(center[1] == doctest::Approx(double(center[0]) * 0.5).epsilon(0.01));
        CHECK(center[2] == doctest::Approx(double(center[0]) * 0.25).epsilon(0.01));
        CHECK(point.at(3, 7)[0] == doctest::Approx(lampRed(3, 7, 8)).epsilon(0.01));

        // A range dims the light by 1 - (distance / range)^4, and to nothing past the range.
        auto setLight = [&](const PrRenderLightDesc& desc) {
            PrRenderUpdate* change = prRenderUpdateBegin(f.render, s.scene);
            prRenderLightSet(change, light, &desc);
            prRenderUpdateCommit(change);
        };
        lamp.range = 2.5f;
        setLight(lamp);
        double window = 1 - double(prF32Pow(2 / 2.5f, 4));
        CHECK(f.view(s.view).at(kCenter, kCenter)[0] ==
            doctest::Approx(lampRed(kCenter, kCenter, 8) * window).epsilon(0.01));
        lamp.range = 1.5f;
        setLight(lamp);
        CHECK(f.view(s.view).at(kCenter, kCenter)[0] == 0);

        // A spot is at full strength inside its inner cone, dark outside its outer cone, and between them falls off by
        // the square of a ramp in the cosine of the angle.
        lamp.range = 0;
        lamp.kind = PrRenderLightKind_Spot;
        lamp.direction = {0, 0, -3};
        lamp.innerConeAngle = 0.15f;
        lamp.outerConeAngle = 0.35f;
        setLight(lamp);
        Image spot = f.view(s.view);
        CHECK(spot.at(kCenter, kCenter)[0] == doctest::Approx(lampRed(kCenter, kCenter, 8)).epsilon(0.01));
        CHECK(spot.at(1, kCenter)[0] == 0);
        constexpr uint32_t kBetween = kCenter + 8;
        Vec between = frontPoint(kBetween, kCenter);
        double cosine = 2 / double(prF32Sqrt(float(between.x * between.x + 4)));
        double ramp = saturate((cosine - double(prF32Cos(0.35f))) / double(prF32Cos(0.15f) - prF32Cos(0.35f)));
        CHECK(ramp > 0.1);
        CHECK(ramp < 0.9);
        CHECK(spot.at(kBetween, kCenter)[0] ==
            doctest::Approx(lampRed(kBetween, kCenter, 8) * ramp * ramp).epsilon(0.02));
    });
}

TEST_CASE("a material's emission, occlusion and environment light add up as glTF's model says") {
    onEachDevice([](Fixture& f) {
        // Emission alone: a black surface under a black environment radiates its emissive factor times its strength.
        PrRenderKhronosPbrMaterial material = whiteRough();
        material.baseColor = {0, 0, 0, 1};
        material.emissive = {0.2f, 0.4f, 0.6f};
        material.emissiveStrength = 2;
        QuadScene s = createQuadScene(f, createMaterial(f, material));
        for (PrRenderTechnique technique : techniques(f)) {
            s.view.technique = technique;
            Image image = f.view(s.view);
            // A dielectric still reflects 4% of a black environment, which is nothing.
            CHECK(near(image.at(kCenter, kCenter), {0.4f, 0.8f, 1.2f}, 1e-3f));
        }

        // Occlusion dims the environment's light alone: with a strength of one half, a texel of 0 leaves half of it.
        const uint8_t occluded[4] = {0, 255, 255, 255};
        PrRenderTexture texture = createTexture(f, {1, 1}, occluded);
        material = whiteRough();
        material.occlusionTexture = useOf(f, texture);
        material.occlusionStrength = 0;
        setGltf(f.render, s.material, &material);
        setEnvironment(f, s.scene, createEvenEnvironment(f, {0.8f, 0.8f, 0.8f}));
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderLightDesc sun = sunLight({0, 0, -1}, 2);
        PrRenderLight light = prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);
        s.view.technique = PrRenderTechnique_Forward;
        float open = f.view(s.view).at(kCenter, kCenter)[0];
        material.occlusionStrength = 0.5f;
        setGltf(f.render, s.material, &material);
        float half = f.view(s.view).at(kCenter, kCenter)[0];
        PrRenderUpdate* dark = prRenderUpdateBegin(f.render, s.scene);
        prRenderLightRemove(dark, light);
        prRenderUpdateCommit(dark);
        float ambient = f.view(s.view).at(kCenter, kCenter)[0];
        // Under an even environment of 0.8, a white dielectric returns the environment's radiance, whatever its two
        // lobes each return of it. The occlusion leaves half of that, and nothing less of the sun's light.
        CHECK(ambient == doctest::Approx(0.4).epsilon(0.005));
        CHECK(open - half == doctest::Approx(0.4).epsilon(0.01));
        CHECK(half - ambient == doctest::Approx(double(open) - 0.8).epsilon(0.01));
    });
}

TEST_CASE("a preview surface draws as the glTF material of its inputs, from whichever textures and channels they read") {
    onEachDevice([](Fixture& f) {
        // Four texels of roughness, metallic and occlusion, packed for glTF and spread over three textures for the
        // preview surface: roughness in red, metallic in alpha, and occlusion in the green of a third.
        const uint8_t packed[16] = {200, 60, 0, 255, 255, 150, 255, 255, 120, 230, 90, 255, 40, 100, 200, 255};
        const uint8_t rough[16] = {60, 0, 0, 255, 150, 0, 0, 255, 230, 0, 0, 255, 100, 0, 0, 255};
        const uint8_t metal[16] = {0, 0, 0, 0, 0, 0, 0, 255, 0, 0, 0, 90, 0, 0, 0, 200};
        const uint8_t occlusion[16] = {0, 200, 0, 255, 0, 255, 0, 255, 0, 120, 0, 255, 0, 40, 0, 255};
        const uint8_t normals[16] = {180, 128, 220, 255, 128, 180, 220, 255, 80, 128, 220, 255, 128, 80, 220, 255};
        PrRenderTexture packedTexture = createTexture(f, {2, 2}, packed);
        PrRenderTexture roughTexture = createTexture(f, {2, 2}, rough);
        PrRenderTexture metalTexture = createTexture(f, {2, 2}, metal);
        PrRenderTexture occlusionTexture = createTexture(f, {2, 2}, occlusion);
        PrRenderTexture normalTexture = createTexture(f, {2, 2}, normals);

        PrRenderKhronosPbrMaterial gltf = whiteRough();
        gltf.baseColor = {0.6f, 0.3f, 0.2f, 1};
        gltf.metallic = 1;
        gltf.roughness = 1;
        gltf.metallicRoughnessTexture = useOf(f, packedTexture);
        gltf.normalTexture = useOf(f, normalTexture);
        gltf.emissive = {0.05f, 0.1f, 0};
        // glTF reads occlusion from red, so its texture is the packed one, whose red is the preview's green.
        const uint8_t occlusionRed[16] = {200, 0, 0, 255, 255, 0, 0, 255, 120, 0, 0, 255, 40, 0, 0, 255};
        PrRenderTexture redOcclusion = createTexture(f, {2, 2}, occlusionRed);
        gltf.occlusionTexture = useOf(f, redOcclusion);

        PrRenderUsdPreviewMaterial preview = prRenderUsdPreviewMaterialDefault();
        preview.diffuseColor = {0.6f, 0.3f, 0.2f};
        preview.emissiveColor = {0.05f, 0.1f, 0};
        preview.roughnessTexture = useOf(f, roughTexture);
        preview.roughnessTexture.channel = 0;
        preview.metallicTexture = useOf(f, metalTexture);
        preview.metallicTexture.channel = 3;
        preview.occlusionTexture = useOf(f, occlusionTexture);
        preview.occlusionTexture.channel = 1;
        // A network reads a normal map with a scale of 2 and a bias of -1 where glTF's mapping is implied.
        PrRenderTextureUse& normal = preview.normalTexture;
        normal = useOf(f, normalTexture);
        for (uint32_t c = 0; c < 3; ++c) {
            normal.scale[c] = 2;
            normal.bias[c] = -1;
        }

        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh sphere = createSphere(f);
        PrRenderMaterial gltfMaterial = createGltf(f, gltf);
        PrRenderMaterial previewMaterial = prRenderMaterialCreate(f.render);
        prRenderMaterialSetUsdPreview(f.render, previewMaterial, &preview);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance instance = addInstance(update, sphere, gltfMaterial, placed({0, 0, 0}, {0.9f, 0.9f, 0.9f}));
        PrRenderLightDesc sun = sunLight(PrVec3{-0.4f, -0.5f, -0.77f}, 3);
        prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);
        setEnvironment(f, scene, createEvenEnvironment(f, {0.3f, 0.3f, 0.3f}));
        PrRenderViewDesc view = frontView(scene);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            update = prRenderUpdateBegin(f.render, scene);
            prRenderInstanceSetMaterial(update, instance, 0, gltfMaterial);
            prRenderUpdateCommit(update);
            Image expected = f.view(view);
            update = prRenderUpdateBegin(f.render, scene);
            prRenderInstanceSetMaterial(update, instance, 0, previewMaterial);
            prRenderUpdateCommit(update);
            Image drawn = f.view(view);
            Difference difference = compare(drawn, expected, 2e-3f);
            CHECK(difference.over == 0);
            CHECK(difference.largest < 2e-3f);
            // The sphere is lit and shows its texels.
            CHECK(drawn.at(kCenter, kCenter)[0] > 0.05f);
        }
    });
}

TEST_CASE("a preview surface's opacity cuts it away below a threshold, and blends it without one") {
    onEachDevice([](Fixture& f) {
        const uint8_t alpha[16] = {255, 255, 255, 60, 255, 255, 255, 200, 255, 255, 255, 220, 255, 255, 255, 30};
        PrRenderTexture texture = createTexture(f, {2, 2}, alpha);
        PrRenderUsdPreviewMaterial preview = prRenderUsdPreviewMaterialDefault();
        preview.diffuseColor = {0.9f, 0.8f, 0.2f};
        preview.roughness = 1;
        preview.opacityTexture = useOf(f, texture);
        preview.opacityTexture.channel = 3;
        PrRenderKhronosPbrMaterial gltf = whiteRough();
        gltf.baseColor = {0.9f, 0.8f, 0.2f, 1};
        gltf.baseColorTexture = useOf(f, texture);
        // Each threshold and the glTF alpha mode that it is.
        struct Case {
            float threshold;
            PrRenderAlphaMode mode;
            float cutoff;
        };
        const Case cases[] = {{0.5f, PrRenderAlphaMode_Mask, 0.5f}, {0, PrRenderAlphaMode_Blend, 0.5f}};
        for (const Case& c : cases) {
            CAPTURE(c.threshold);
            preview.opacityThreshold = c.threshold;
            gltf.alphaMode = c.mode;
            gltf.alphaCutoff = c.cutoff;
            PrRenderMaterial previewMaterial = prRenderMaterialCreate(f.render);
            prRenderMaterialSetUsdPreview(f.render, previewMaterial, &preview);
            QuadScene usd = createQuadScene(f, previewMaterial);
            QuadScene reference = createQuadScene(f, createMaterial(f, gltf));
            addLight(f, usd.scene, sunLight({0, 0, -1}, 2));
            addLight(f, reference.scene, sunLight({0, 0, -1}, 2));
            Difference difference = compare(f.view(usd.view), f.view(reference.view), 2e-3f);
            CHECK(difference.over == 0);
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// Textures and geometry

TEST_CASE("a texture's mip chain is made from its first level, in the texture's color space") {
    onEachDevice([](Fixture& f) {
        // A checker of black and white texels, which averages to one half in linear light.
        Array<uint8_t> checker;
        for (uint32_t i = 0; i < 256; ++i) {
            uint8_t value = ((i % 16) + (i / 16)) % 2 == 0 ? 255 : 0;
            const uint8_t texel[4] = {value, value, value, 255};
            checker.append(texel, 4);
        }
        const TextureEncoding spaces[] = {TextureEncoding_Srgb, TextureEncoding_Linear};
        for (TextureEncoding space : spaces) {
            CAPTURE(space);
            PrRenderTexture texture = createTexture(f, {16, 16}, checker.begin(), space, PrGpuFilter_Linear);
            PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
            unlit.colorTexture = useOf(f, texture);
            QuadScene s = createQuadScene(f, createMaterial(f, unlit));
            // The whole texture covers two pixels of the view, so a pixel reads a level whose texels are all averages.
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
            PrAffine3 small = placed({0, 0, 0}, {2.0f / kExtent.x, 2.0f / kExtent.y, 1});
            prRenderInstanceSetTransform(update, s.instance, &small);
            prRenderUpdateCommit(update);
            CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(0.5).epsilon(0.02));
        }

        // A texel of one half in sRGB's encoding is 0.2158 in linear light, and one half as a linear value.
        const uint8_t gray[4] = {128, 128, 128, 255};
        for (TextureEncoding space : spaces) {
            PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
            unlit.colorTexture = useOf(f, createTexture(f, {1, 1}, gray, space));
            QuadScene s = createQuadScene(f, createMaterial(f, unlit));
            double expected = space == TextureEncoding_Srgb ? 0.2158 : 128 / 255.0;
            CHECK(f.view(s.view).at(kCenter, kCenter)[0] == doctest::Approx(expected).epsilon(0.005));
        }
    });
}

TEST_CASE("a texture's levels, formats and failures follow its data") {
    onEachDevice([](Fixture& f) {
        // Two levels given: the second is the caller's, not an average of the first.
        const uint8_t first[16] = {255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255};
        const uint8_t second[4] = {0, 255, 0, 255};
        PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Nearest, PrGpuAddress_Repeat);
        sampler.minLod = 1;
        PrRenderTexture texture = prRenderTextureCreate(f.render);
        f.samplers.set(texture.id, sampler);
        PrRenderTextureData data{.format = PrGpuFormat_Rgba8, .extent = {2, 2}, .levelCount = 2};
        data.levels[0] = {first, 8};
        data.levels[1] = {second, 4};
        REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.colorTexture = useOf(f, texture);
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        CHECK(near(f.view(s.view).at(kCenter, kCenter), {0, 1, 0}, 1e-3f));

        // Float texels pass through unchanged, with rows farther apart than their texels.
        float floats[2][8] = {{0.25f, 3, -1, 1, 9, 9, 9, 9}, {0.25f, 3, -1, 1, 9, 9, 9, 9}};
        PrRenderTextureData floatData{.format = PrGpuFormat_Rgba32F, .extent = {1, 2}, .levelCount = 1};
        floatData.levels[0] = {floats, sizeof(floats[0])};
        sampler.minLod = 0;
        PrRenderTexture floatTexture = prRenderTextureCreate(f.render);
        f.samplers.set(floatTexture.id, sampler);
        REQUIRE(prRenderTextureSetData(f.render, floatTexture, &floatData) == PrStatus_Ok);
        unlit.colorTexture = useOf(f, floatTexture);
        prRenderMaterialSetUnlit(f.render, s.material, &unlit);
        CHECK(near(f.view(s.view).at(kCenter, kCenter), {0.25f, 3, -1}, 1e-3f));

        // A format that the device does not sample is refused, and the texture keeps what it had.
        bool compressed = (prGpuDeviceInfo(f.device).features & PrGpuFeatureFlag_TextureCompressionAstc) != 0;
        if (!compressed) {
            const uint8_t block[16] = {};
            PrRenderTextureData astc{.format = PrGpuFormat_Astc4x4, .extent = {4, 4}, .levelCount = 1};
            astc.levels[0] = {block, 16};
            CHECK(prRenderTextureSetData(f.render, floatTexture, &astc) == PrStatus_NotSupported);
            CHECK(near(f.view(s.view).at(kCenter, kCenter), {0.25f, 3, -1}, 1e-3f));
        }
    });
}

TEST_CASE("a texture is read through its transform, from the set of coordinates that the material names") {
    onEachDevice([](Fixture& f) {
        const PrVec3 red{1, 0, 0};
        const PrVec3 green{0, 1, 0};
        const uint8_t texels[8] = {255, 0, 0, 255, 0, 255, 0, 255};
        PrRenderTexture texture = createTexture(f, {2, 1}, texels);
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.colorTexture = useOf(f, texture);
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        constexpr uint32_t kLeft = 4;
        constexpr uint32_t kRight = kExtent.x - 5;
        constexpr uint32_t kTop = 4;
        constexpr uint32_t kBottom = kExtent.y - 5;

        // Each use of the texture, and the colors that it shows at the view's four corners: top left, top right, bottom
        // left and bottom right.
        struct Case {
            PrRenderTextureUse use;
            PrVec3 corners[4];
        };
        PrRenderTextureUse plain = useOf(f, texture);
        PrRenderTextureUse moved = plain;
        moved.transform = prAffine2Translate({0.5f, 0});
        PrRenderTextureUse second = plain;
        second.texCoord = 1;
        PrRenderTextureUse scaled = plain;
        scaled.transform = prAffine2Scale({0.5f, 1});
        // A quarter turn maps (u, v) to (v, -u), so the texture's columns run down the view.
        PrRenderTextureUse turned = plain;
        turned.transform = prRenderTextureTransform({0, 0}, kPi / 2, {1, 1});
        const Case cases[] = {
            {plain, {red, green, red, green}},
            {moved, {green, red, green, red}},
            {second, {green, red, green, red}},
            {scaled, {red, red, red, red}},
            {turned, {red, red, green, green}},
        };
        for (const Case& c : cases) {
            unlit.colorTexture = c.use;
            prRenderMaterialSetUnlit(f.render, s.material, &unlit);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                s.view.technique = technique;
                Image image = f.view(s.view);
                CHECK(near(image.at(kLeft, kTop), c.corners[0], 1e-3f));
                CHECK(near(image.at(kRight, kTop), c.corners[1], 1e-3f));
                CHECK(near(image.at(kLeft, kBottom), c.corners[2], 1e-3f));
                CHECK(near(image.at(kRight, kBottom), c.corners[3], 1e-3f));
            }
        }
    });
}

TEST_CASE("each part of a mesh draws with its own material, and a part without one with the neutral material") {
    onEachDevice([](Fixture& f) {
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh halves = createQuad(f, true);
        const PrRenderMaterial materials[2] = {createUnlit(f, {1, 0, 0, 1}), createUnlit(f, {0, 1, 0, 1})};
        PrRenderInstanceDesc desc = prRenderInstanceDescDefault(halves);
        desc.materials = materials;
        desc.materialCount = 1;
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderInstance instance = prRenderInstanceAdd(update, &desc);
        prRenderUpdateCommit(update);
        setEnvironment(f, scene, createEvenEnvironment(f, {0.5f, 0.5f, 0.5f}));
        PrRenderViewDesc view = frontView(scene);
        // The neutral material is a rough gray dielectric, which the even environment lights.
        float nv = float(towardCamera(frontPoint(kExtent.x - 4, kCenter)).z);
        float gray = float(referenceIndirect(0.5, 0, 1, double(nv), 0.5, 0.5));
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            view.technique = technique;
            Image image = f.view(view);
            CHECK(near(image.at(4, kCenter), {1, 0, 0}, 0));
            CHECK(near(image.at(kExtent.x - 4, kCenter), {gray, gray, gray}, 2e-3f));
            CHECK(gray == doctest::Approx(0.25).epsilon(0.03));
        }
        // A material for the second part, which the instance was added without.
        PrRenderUpdate* change = prRenderUpdateBegin(f.render, scene);
        prRenderInstanceSetMaterial(change, instance, 1, materials[1]);
        prRenderUpdateCommit(change);
        Image image = f.view(view);
        CHECK(near(image.at(4, kCenter), {1, 0, 0}, 0));
        CHECK(near(image.at(kExtent.x - 4, kCenter), {0, 1, 0}, 0));
    });
}

TEST_CASE("back faces are culled unless a material is double sided, and a mirrored instance keeps its front") {
    onEachDevice([](Fixture& f) {
        PrRenderKhronosPbrMaterial material = whiteRough();
        QuadScene s = createQuadScene(f, createMaterial(f, material));
        addLight(f, s.scene, sunLight({0, 0, -1}, 2));
        double lit = brdfDirect({1, 0, 1, {0, 0, 1}}, {0, 0, 1}, {0, 0, 1}) * 2;

        // Each placement of the quad: its scale, and whether a one-sided material shows it.
        struct Placement {
            PrVec3 scale;
            bool front;
        };
        const Placement placements[] = {
            {{1, 1, 1}, true},      // as it is
            {{-1, 1, -1}, false},   // turned away, by half a turn about y
            {{-1, 1, 1}, true},     // mirrored: its winding is reversed, and it still faces the camera
            {{1, 1, -1}, false},    // mirrored and turned away
        };
        for (const Placement& placement : placements) {
            CAPTURE(placement.scale.x);
            CAPTURE(placement.scale.z);
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
            PrAffine3 world = placed({0, 0, 0}, placement.scale);
            prRenderInstanceSetTransform(update, s.instance, &world);
            prRenderUpdateCommit(update);
            const bool sides[] = {false, true};
            for (bool doubleSided : sides) {
                CAPTURE(doubleSided);
                material.flags = doubleSided ? PrRenderMaterialFlag_DoubleSided : 0;
                setGltf(f.render, s.material, &material);
                for (PrRenderTechnique technique : techniques(f)) {
                    CAPTURE(technique);
                    s.view.technique = technique;
                    float red = f.view(s.view).at(kCenter, kCenter)[0];
                    // A face that is drawn is lit by the normal on the camera's side, whichever face it is.
                    if (placement.front || doubleSided)
                        CHECK(red == doctest::Approx(lit).epsilon(0.01));
                    else
                        CHECK(red == 0);
                }
            }
        }
    });
}

TEST_CASE("more samples smooth the edges of the forward technique") {
    onEachDevice([](Fixture& f) {
        PrRenderUnlitMaterial white = prRenderUnlitMaterialDefault();
        QuadScene s = createQuadScene(f, createMaterial(f, white));
        // A quad turned a little about z, so that its edges cross pixels at a slant.
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrAffine3 turned{{{0.5f, -0.2f, 0, 0}, {0.2f, 0.5f, 0, 0}, {0, 0, 1, 0}}};
        prRenderInstanceSetTransform(update, s.instance, &turned);
        prRenderUpdateCommit(update);
        const uint8_t counts[] = {1, 2, 4, 8};
        uint32_t supported = prGpuDeviceInfo(f.device).sampleCounts;
        CHECK((supported & 4) != 0);
        uint8_t samples = 1;
        for (uint8_t asked : counts) {
            // A view draws with the number of samples that it asks for, or with the most below it that the device has.
            CAPTURE(asked);
            s.view.samples = asked;
            if ((supported & asked) != 0)
                samples = asked;
            Image image = f.view(s.view);
            uint32_t partial = 0;
            for (uint32_t i = 0; i < kExtent.x * kExtent.y; ++i) {
                float value = image.texels[i * 4];
                // A pixel's value is the part of its samples that the quad covers.
                float scaled = value * float(samples);
                CHECK(prF32Abs(scaled - prF32Round(scaled)) < 2e-3f);
                if (value > 0.01f && value < 0.99f)
                    ++partial;
            }
            CHECK((partial != 0) == (samples > 1));
            CHECK(near(image.at(kCenter, kCenter), {1, 1, 1}, 0));
        }
    });
}

TEST_CASE("the views of a frame share the scene's shadow pass, and each is drawn as it is alone") {
    onEachDevice([](Fixture& f) {
        PrRenderViewDesc gallery = createGallery(f);
        PrRenderViewDesc other = gallery;
        other.camera = prRenderCameraLookAt({-3, 3, 5}, {0, 0, 0}, 0.9f, 1, 0.1f);
        other.samples = 1;
        Image alone = f.view(gallery);
        // One view: the light's cull and the shadow map, then the view's cull, depth and surfaces, and the read.
        CHECK(f.stats.passes == 6);
        Image otherAlone = f.view(other);
        const PrRenderViewDesc both[2] = {gallery, other};
        Array<Image> images;
        f.views(both, &images);
        CHECK(f.stats.passes == 10);
        CHECK(compare(images[0], alone, 0).over == 0);
        CHECK(compare(images[1], otherAlone, 0).over == 0);
    });
}

TEST_CASE("a view keeps its depth for later passes only if it is asked to") {
    onEachDevice([](Fixture& f) {
        const PrVec3 back{0.1f, 0.6f, 0.9f};
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.color = {back.x, back.y, back.z, 1};
        QuadScene s = createQuadScene(f, createMaterial(f, unlit));
        // A blended quad over the left half, so that the visibility technique draws over its own depth too.
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, s.scene);
        PrRenderMaterial glass = createUnlit(f, {1, 1, 1, 0.5f}, PrRenderAlphaMode_Blend);
        addInstance(update, s.quad, glass, placed({-1, 0, 1}));
        prRenderUpdateCommit(update);
        const uint8_t counts[] = {1, 4};
        const bool keeps[] = {false, true};
        for (PrRenderTechnique technique : techniques(f)) {
            for (uint8_t samples : counts) {
                for (bool keepsDepth : keeps) {
                    CAPTURE(technique);
                    CAPTURE(samples);
                    CAPTURE(keepsDepth);
                    s.view.technique = technique;
                    s.view.samples = samples;
                    s.view.keepsDepth = keepsDepth;
                    Image image;
                    prGpuGraphBegin(f.graph);
                    prRenderFrameBegin(f.render, f.graph);
                    PrRenderViewTextures textures = prRenderViewAdd(f.render, &s.view);
                    CHECK((textures.depth.id != 0) == keepsDepth);
                    f.addRead(textures.color, &image);
                    f.record();
                    CHECK(near(image.at(kExtent.x - 4, kCenter), back, 1e-3f));
                    CHECK(near(image.at(4, kCenter), back * 0.5f + PrVec3{0.5f, 0.5f, 0.5f}, 1e-3f));
                }
            }
        }
    });
}

// ---------------------------------------------------------------------------------------------------------------------
// One way of rendering against another

TEST_CASE("a device with slots only draws the pixels that a device with bindless draws") {
    const Array<TestDevice>& devices = testDevices();
    for (uint32_t i = 0; i + 1 < devices.count(); ++i) {
        if (devices[i].backend != devices[i + 1].backend || devices[i].slotsOnly || !devices[i + 1].slotsOnly)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        Image handles;
        Image slots;
        {
            Fixture f(devices[i]);
            REQUIRE(f.bindless);
            PrRenderViewDesc view = createGallery(f);
            handles = f.view(view);
            // The scene fills most of the view, so the comparison is of surfaces and not of the background.
            CHECK(coveredPixels(f, view, handles) > kGalleryExtent.x * kGalleryExtent.y / 2);
        }
        {
            Fixture f(devices[i + 1]);
            REQUIRE_FALSE(f.bindless);
            slots = f.view(createGallery(f));
        }
        // The two tiers run the same shader code on the same data, and differ only in how a resource is reached, so
        // every pixel is equal.
        Difference difference = compare(handles, slots, kTierTolerance);
        CAPTURE(difference.largest);
        CHECK(difference.over == 0);
    }
}

TEST_CASE("the backends draw the same image, to within the rounding of their compilers") {
    Array<Image> images;
    Array<PrStringView> names;
    for (const TestDevice& device : testDevices()) {
        Fixture f(device);
        if (!f.bindless)
            continue;
        images.push(f.view(createGallery(f)));
        names.push(prBackendName(prGpuSelectInterface(), device.backend));
    }
    for (uint32_t i = 1; i < images.count(); ++i) {
        CAPTURE(names[0]);
        CAPTURE(names[i]);
        // Each backend compiles the shaders with its own compiler, which orders and fuses the arithmetic its own way.
        // The images then differ by the last bits of a half float. A sample that lies on an edge of a triangle or of a
        // shadow can fall to the other side, so a few pixels in a thousand may differ by more.
        Difference difference = compare(images[0], images[i], 0.01f);
        CAPTURE(difference.largest);
        CAPTURE(difference.over);
        CHECK(difference.over <= kGalleryExtent.x * kGalleryExtent.y / 500);
        CHECK(difference.mean < 1e-4);
    }
}

TEST_CASE("the display shows the same pixels with slots as with handles, and on each backend to within rounding") {
    // Returns the gallery as each tone map shows it, without headroom and then with it.
    auto shownGallery = [](Fixture& f) {
        Array<Image> images;
        PrRenderViewDesc view = createGallery(f);
        for (PrRenderToneMap toneMap = 0; toneMap < PrRenderToneMap_Count; ++toneMap) {
            images.push(f.shown(view, PrRenderDisplayOptions{.exposure = 2, .headroom = 1, .toneMap = toneMap}));
            images.push(f.shown(view, PrRenderDisplayOptions{.exposure = 2, .headroom = 4, .toneMap = toneMap}));
        }
        return images;
    };
    Array<Array<Image>> handles;
    Array<PrStringView> names;
    const Array<TestDevice>& devices = testDevices();
    for (uint32_t i = 0; i < devices.count(); ++i) {
        Fixture f(devices[i]);
        if (!f.bindless)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        Array<Image> images = shownGallery(f);
        if (i + 1 < devices.count() && devices[i + 1].backend == devices[i].backend && devices[i + 1].slotsOnly) {
            Fixture slotsOnly(devices[i + 1]);
            REQUIRE_FALSE(slotsOnly.bindless);
            Array<Image> slots = shownGallery(slotsOnly);
            for (uint32_t image = 0; image < images.count(); ++image) {
                CAPTURE(image);
                CHECK(compare(images[image], slots[image], kTierTolerance).over == 0);
            }
        }
        handles.push(move(images));
        names.push(backend);
    }
    for (uint32_t i = 1; i < handles.count(); ++i) {
        CAPTURE(names[0]);
        CAPTURE(names[i]);
        for (uint32_t image = 0; image < handles[i].count(); ++image) {
            CAPTURE(image);
            // The images hold encoded values, so the rounding of a backend's compiler shows most in the dark pixels.
            Difference difference = compare(handles[0][image], handles[i][image], 0.01f);
            CAPTURE(difference.largest);
            CAPTURE(difference.over);
            CHECK(difference.over <= kGalleryExtent.x * kGalleryExtent.y / 500);
            CHECK(difference.mean < 1e-3);
        }
    }
}

TEST_CASE("the forward and visibility techniques shade the same surfaces alike, away from geometric edges") {
    onEachDevice([](Fixture& f) {
        if (!prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
            return;
        PrRenderViewDesc view = createGallery(f);
        view.technique = PrRenderTechnique_Visibility;
        Image visibility = f.view(view);
        view.technique = PrRenderTechnique_Forward;
        view.samples = 1;
        Image forward = f.view(view);
        view.samples = 4;
        Image smoothed = f.view(view);
        uint32_t pixels = kGalleryExtent.x * kGalleryExtent.y;

        // With one sample each, the two techniques cover the same pixels, and shade each alike. The visibility
        // technique interpolates a pixel's point from its triangle's corners itself, so the two differ by rounding, and
        // by more at the few pixels where that rounding moves a texel's or a shadow's edge.
        Difference single = compare(forward, visibility, 0.02f);
        CAPTURE(single.largest);
        CAPTURE(single.over);
        CHECK(single.over <= pixels / 500);
        CHECK(single.mean < 5e-4);

        // A pixel of the multisampled image that differs from the single-sampled one lies on a geometric edge. Away
        // from those pixels, the multisampled forward image and the visibility image agree as closely.
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
        CHECK(differing <= pixels / 500);
    });
}

TEST_CASE("each technique's derivatives choose the mip level that the technique promises") {
    onEachDevice([](Fixture& f) {
        // Without anisotropy, the level follows from the longer of the two derivatives. With it, the level follows from
        // the shorter one, until the ratio of the two passes the sampler's. So the two samplers together measure both
        // derivatives.
        const uint8_t anisotropies[] = {1, 16};
        for (uint8_t anisotropy : anisotropies) {
            CAPTURE(anisotropy);
            PrRenderViewDesc view = createGround(f, createLevelTexture(f, anisotropy), false);
            Array<PrRenderTechnique> drawn = techniques(f);
            Array<Image> images;
            for (PrRenderTechnique technique : drawn) {
                view.technique = technique;
                images.push(f.view(view));
            }

            // The forward technique promises the derivative that a draw's fragment shader takes, and the visibility
            // technique the exact one.
            for (uint32_t t = 0; t < drawn.count(); ++t) {
                CAPTURE(drawn[t]);
                double lowest = double(kLevelCount);
                double highest = 0;
                double largest = 0;
                double sum = 0;
                uint32_t pixels = 0;
                for (uint32_t y = 0; y < kGroundExtent.y; ++y) {
                    for (uint32_t x = 0; x < kGroundExtent.x; ++x) {
                        double shown = double(levelOf(images[t].at(x, y)[0]));
                        // The sky above the plane is black, and the plane shows no level below the first.
                        if (shown == 0)
                            continue;
                        GroundLevels levels = groundLevels(view, x, y, anisotropy);
                        double expected = drawn[t] == PrRenderTechnique_Forward ? levels.block : levels.exact;
                        ++pixels;
                        lowest = prF64Min(lowest, shown);
                        highest = prF64Max(highest, shown);
                        largest = prF64Max(largest, prF64Abs(shown - expected));
                        sum += prF64Abs(shown - expected);
                    }
                }
                CAPTURE(largest);
                CAPTURE(sum / pixels);
                // The plane covers most of the view, minified everywhere, and by more toward the horizon, so the levels
                // span a range.
                CHECK(pixels > kGroundExtent.x * kGroundExtent.y / 2);
                CHECK(lowest > 0.5);
                CHECK(highest - lowest > 1.5);
                // A device computes a level from the derivatives by an approximation of its own, which Vulkan and
                // Direct3D allow. Without anisotropy, it is within a tenth of a level of the logarithm. An anisotropic
                // sampler also approximates the footprint's ellipse, so its level is held less closely. A derivative
                // that is wrong by a factor of two moves the level by one.
                CHECK(largest < (anisotropy == 1 ? 0.15 : 0.75));
                CHECK(sum / pixels < (anisotropy == 1 ? 0.06 : 0.3));
            }

            // The two kinds of derivative differ by up to 0.07 of a level on this plane, less than a device's error in
            // the level. That error is nearly the same for both techniques, so it cancels in the difference between
            // their levels at each pixel, and the difference shows which derivative each technique took.
            if (drawn.count() < 2 || anisotropy != 1)
                continue;
            PR_ASSERT(drawn[0] == PrRenderTechnique_Forward);
            double sum = 0;
            double swapped = 0;
            uint32_t pixels = 0;
            for (uint32_t y = 0; y < kGroundExtent.y; ++y) {
                for (uint32_t x = 0; x < kGroundExtent.x; ++x) {
                    double forward = double(levelOf(images[0].at(x, y)[0]));
                    double visibility = double(levelOf(images[1].at(x, y)[0]));
                    if (forward == 0 || visibility == 0)
                        continue;
                    GroundLevels levels = groundLevels(view, x, y, anisotropy);
                    double promised = levels.block - levels.exact;
                    ++pixels;
                    sum += prF64Abs(forward - visibility - promised);
                    swapped += prF64Abs(forward - visibility + promised);
                }
            }
            CAPTURE(sum / pixels);
            CAPTURE(swapped / pixels);
            CHECK(swapped > 4 * sum);
        }
    });
}

TEST_CASE("the techniques draw a minified, anisotropic, transformed texture alike, but for the levels they choose") {
    onEachDevice([](Fixture& f) {
        if (!prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
            return;
        // A checker of 64 texels, whose mip chain the renderer makes, read through 16 samples of anisotropy.
        Array<uint8_t> checker;
        for (uint32_t i = 0; i < kLevelSide * kLevelSide; ++i) {
            bool light = ((i % kLevelSide) / 4 + (i / kLevelSide) / 4) % 2 == 0;
            const uint8_t texel[4] = {uint8_t(light ? 240 : 30), uint8_t(light ? 220 : 60), uint8_t(light ? 90 : 150), 255};
            checker.append(texel, 4);
        }
        PrGpuSamplerDesc sampler = prGpuSamplerDescDefault(PrGpuFilter_Linear, PrGpuAddress_Repeat);
        sampler.maxAnisotropy = 16;
        PrRenderTexture texture = prRenderTextureCreate(f.render);
        f.samplers.set(texture.id, sampler);
        PrRenderTextureData data{.format = PrGpuFormat_Rgba8Srgb, .extent = {kLevelSide, kLevelSide}, .levelCount = 1};
        data.levels[0] = {checker.begin(), 0};
        REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);

        PrRenderViewDesc view = createGround(f, texture, true);
        view.technique = PrRenderTechnique_Forward;
        Image forward = f.view(view);
        view.technique = PrRenderTechnique_Visibility;
        Image visibility = f.view(view);
        // The image has contrast, so that a wrong level would show.
        float darkest = 1;
        float lightest = 0;
        for (uint32_t i = 0; i < kGroundExtent.x * kGroundExtent.y; ++i) {
            darkest = prF32Min(darkest, forward.texels[i * 4]);
            lightest = prF32Max(lightest, forward.texels[i * 4]);
        }
        CHECK(lightest - darkest > 0.2f);
        // The two techniques filter the texture by different derivatives, which the case above measures, so they read
        // slightly different levels of the checker. The tolerances are what that difference in level changes on this
        // plane: a few hundredths at the pixels where the levels still hold the checker's contrast.
        Difference difference = compare(forward, visibility, 0.04f);
        CAPTURE(difference.largest);
        CAPTURE(difference.mean);
        CAPTURE(difference.over);
        CHECK(difference.over <= kGroundExtent.x * kGroundExtent.y / 200);
        CHECK(difference.largest < 0.1f);
        CHECK(difference.mean < 3e-3);
    });
}

TEST_CASE("the techniques draw masked, double-sided and vertex-colored surfaces alike") {
    onEachDevice([](Fixture& f) {
        if (!prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
            return;
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderMesh colored = createColoredQuad(f);

        // A mask whose alpha is a checker of 4 texels, filtered, so that the cutoff's edge depends on the level read.
        Array<uint8_t> mask;
        for (uint32_t i = 0; i < 64; ++i) {
            const uint8_t texel[4] = {255, 255, 255, uint8_t(((i % 8) / 4 + (i / 8) / 4) % 2 == 0 ? 255 : 0)};
            mask.append(texel, 4);
        }
        PrRenderTexture alpha = createTexture(f, {8, 8}, mask.begin(), TextureEncoding_Linear, PrGpuFilter_Linear);
        PrRenderUnlitMaterial plain = prRenderUnlitMaterialDefault();
        PrRenderUnlitMaterial cut = prRenderUnlitMaterialDefault();
        cut.alphaMode = PrRenderAlphaMode_Mask;
        cut.colorTexture = useOf(f, alpha);
        cut.colorTexture.transform = prAffine2Scale({3, 3});
        PrRenderKhronosPbrMaterial backed = whiteRough();
        backed.flags |= PrRenderMaterialFlag_DoubleSided;
        PrRenderMaterial materials[3] = {createMaterial(f, plain), createMaterial(f, cut), createGltf(f, backed)};
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        // The colors alone at the top left, the colors cut by the mask at the top right, and below them the back face
        // of a lit quad, which is turned away from the camera and leans back.
        addInstance(update, colored, materials[0], placed({-0.5f, 0.5f, 0}, {0.45f, 0.45f, 1}));
        addInstance(update, colored, materials[1], placed({0.5f, 0.5f, 0}, {0.45f, 0.45f, 1}));
        PrAffine3 leaning{{{-0.9f, 0, 0, 0}, {0, 0.4f, 0.2f, -0.5f}, {0, 0.2f, -0.4f, 0}}};
        addInstance(update, colored, materials[2], leaning);
        PrRenderLightDesc sun = sunLight({0.2f, -0.3f, -1}, 2);
        prRenderLightAdd(update, &sun);
        prRenderUpdateCommit(update);

        PrRenderViewDesc view = frontView(scene);
        view.extent = kGroundExtent;
        view.technique = PrRenderTechnique_Forward;
        Image forward = f.view(view);
        view.technique = PrRenderTechnique_Visibility;
        Image visibility = f.view(view);

        // The center of the plain quad lies on the edge that its triangles share, halfway from its red corner to its
        // white one, and the back face is lit.
        PrUVec2 plainCenter = pixelOf(view, {-0.5f, 0.5f, 0});
        PrUVec2 backCenter = pixelOf(view, {0, -0.5f, 0});
        const Image* images[2] = {&forward, &visibility};
        for (const Image* image : images) {
            CHECK(near(image->at(plainCenter.x, plainCenter.y), {1, 0.5f, 0.5f}, 0.03f));
            CHECK(image->at(backCenter.x, backCenter.y)[1] > 0.1f);
        }
        // The mask keeps some of its quad and cuts some of it out.
        uint32_t kept = 0;
        uint32_t cutOut = 0;
        for (uint32_t y = 0; y < kGroundExtent.y / 2; ++y) {
            for (uint32_t x = kGroundExtent.x * 5 / 8; x < kGroundExtent.x * 7 / 8; ++x) {
                const float* texel = forward.at(x, y);
                bool drawn = texel[0] + texel[1] + texel[2] > 0;
                kept += drawn;
                cutOut += !drawn;
            }
        }
        CHECK(kept > 50);
        CHECK(cutOut > 50);

        // The visibility pass tests the mask with the draw's own derivatives, as the forward technique's passes do, so
        // the two cover the same pixels. The colors differ by the rounding of the interpolation.
        Difference difference = compare(forward, visibility, 0.01f);
        CAPTURE(difference.largest);
        CAPTURE(difference.over);
        CHECK(difference.over <= kGroundExtent.x * kGroundExtent.y / 1000);
        CHECK(difference.mean < 2e-4);
    });
}

// The last case of the binary's last file, in the order in which the cases run.
TEST_CASE("the devices that the tests share are destroyed") {
    destroyTestDevices();
    CHECK(testDevices().count() == 0);
}

TEST_SUITE_END();
