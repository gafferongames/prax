// Textures whose texels have not arrived: what a material of the library's glTF type reads in each of its textures'
// places until they do, compared with textures of one texel that hold those values, and with the materials that those
// values describe.

#include "pr/array.h"
#include "pr/float.h"
#include "pr/gpu.h"
#include "pr/render.h"
#include "pr/render_resource.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"

#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

// The techniques that a device draws with.
Array<PrRenderTechnique> techniques(const Fixture& f) {
    Array<PrRenderTechnique> out;
    out.push(PrRenderTechnique_Forward);
    if (prRenderSupportsTechnique(f.render, PrRenderTechnique_Visibility))
        out.push(PrRenderTechnique_Visibility);
    return out;
}

// The textures of a glTF material, in the order of the type's texture indices.
enum {
    Slot_BaseColor,
    Slot_MetallicRoughness,
    Slot_Normal,
    Slot_Occlusion,
    Slot_Emissive,
    Slot_Count,
};
typedef uint8_t Slot;
constexpr PrStringView kSlotNames[Slot_Count] = {"base color", "metallic-roughness", "normal", "occlusion", "emissive"};

// What the glTF type reads in each texture's place before the texture's texels arrive: a white base color, a rough
// dielectric, the flat normal, no occlusion and no emission.
constexpr float kStandIns[Slot_Count][4] = {{1, 1, 1, 1}, {1, 1, 0, 1}, {0.5f, 0.5f, 1, 1}, {1, 1, 1, 1}, {0, 0, 0, 1}};

// Creates a texture of values that a shader reads unchanged, without texels.
PrRenderTexture createPending(Fixture& f) {
    return prRenderTextureCreate(f.render);
}

// Gives a texture texels of floats, `side` to a side.
void setTexels(Fixture& f, PrRenderTexture texture, uint32_t side, const float* texels) {
    PrRenderTextureData data{.format = PrGpuFormat_Rgba32F, .extent = {side, side}, .levelCount = 1};
    data.levels[0] = {texels, 0};
    REQUIRE(prRenderTextureSetData(f.render, texture, &data) == PrStatus_Ok);
}

// Creates a texture of one texel that holds what the glTF type reads for a texture of `slot` without texels.
PrRenderTexture createStandIn(Fixture& f, uint32_t slot) {
    PrRenderTexture texture = createPending(f);
    setTexels(f, texture, 1, kStandIns[slot]);
    return texture;
}

// Two texels by two for each texture of a glTF material, none of which is what the type reads in its place before it
// arrives: a dark base color, a smooth metal, normals that lean, some occlusion, and an emission.
constexpr float kTexels[Slot_Count][16] = {
    {0.3f, 0.2f, 0.1f, 1, 0.25f, 0.3f, 0.2f, 1, 0.2f, 0.2f, 0.35f, 1, 0.3f, 0.3f, 0.3f, 1},
    {1, 0.3f, 1, 1, 1, 0.4f, 0.9f, 1, 1, 0.35f, 1, 1, 1, 0.5f, 0.8f, 1},
    {0.7f, 0.5f, 0.86f, 1, 0.5f, 0.7f, 0.86f, 1, 0.3f, 0.5f, 0.86f, 1, 0.5f, 0.3f, 0.86f, 1},
    {0.5f, 1, 1, 1, 0.7f, 1, 1, 1, 0.6f, 1, 1, 1, 0.4f, 1, 1, 1},
    {0.9f, 0.5f, 0.2f, 1, 0.2f, 0.8f, 0.4f, 1, 0.5f, 0.5f, 0.9f, 1, 0.7f, 0.2f, 0.6f, 1},
};

// A material whose every factor leaves its texture's value to show.
PrRenderKhronosPbrMaterial texturedMaterial() {
    PrRenderKhronosPbrMaterial material = prRenderKhronosPbrMaterialDefault();
    material.baseColor = {0.9f, 0.8f, 0.7f, 1};
    material.metallic = 0.9f;
    material.roughness = 0.6f;
    material.emissive = {0.6f, 0.5f, 0.4f};
    return material;
}

// Returns the use of the texture of `slot` in a glTF material.
PrRenderTextureUse* useOf(PrRenderKhronosPbrMaterial* material, uint32_t slot) {
    PrRenderTextureUse* uses[Slot_Count] = {
        &material->baseColorTexture,
        &material->metallicRoughnessTexture,
        &material->normalTexture,
        &material->occlusionTexture,
        &material->emissiveTexture,
    };
    return uses[slot];
}

// A quad that fills the front view, turned a little so that its normal map and its gloss show, under a sun and an even
// environment, with a material that the test sets.
struct Wall {
    PrRenderScene scene;
    PrRenderMaterial material;
    PrRenderViewDesc view;
};

Wall createWall(Fixture& f) {
    Wall wall{};
    wall.scene = prRenderSceneCreate(f.render);
    wall.material = prRenderMaterialCreate(f.render);
    PrRenderUpdate* update = prRenderUpdateBegin(f.render, wall.scene);
    // Turned about y by 0.3 radians, and wide enough to fill the view still.
    float c = prF32Cos(0.3f);
    float s = prF32Sin(0.3f);
    addInstance(update, createQuad(f), wall.material, PrAffine3{{{c * 2, 0, s, 0}, {0, 2, 0, 0}, {-s * 2, 0, c, 0}}});
    PrRenderLightDesc sun = prRenderLightDescDefault();
    sun.direction = {-0.3f, -0.4f, -0.85f};
    sun.intensity = 2;
    prRenderLightAdd(update, &sun);
    prRenderUpdateCommit(update);
    setEnvironment(f, wall.scene, createEvenEnvironment(f, {0.3f, 0.35f, 0.4f}));
    wall.view = frontView(wall.scene);
    return wall;
}

// Returns the wall's image with `material`.
Image draw(Fixture& f, Wall& wall, const PrRenderKhronosPbrMaterial& material, PrRenderTechnique technique) {
    setGltf(f.render, wall.material, &material);
    wall.view.technique = technique;
    return f.view(wall.view);
}

// Returns the mean of an image's red, green and blue.
double meanOf(const Image& image) {
    double sum = 0;
    uint32_t pixels = image.extent.x * image.extent.y;
    for (uint32_t i = 0; i < pixels; ++i)
        sum += double(image.texels[i * 4]) + double(image.texels[i * 4 + 1]) + double(image.texels[i * 4 + 2]);
    return sum / (double(pixels) * 3);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("render arrival" * doctest::skip(!gpuTests()));

TEST_CASE("a glTF material draws a texture without texels as a texture of one texel of the type's stand-in for it") {
    onEachDevice([](Fixture& f) {
        Wall wall = createWall(f);
        for (uint32_t slot = 0; slot < Slot_Count; ++slot) {
            CAPTURE(kSlotNames[slot]);
            // Every texture but the one of `slot` has its texels. That one has none in one material, and the one texel
            // of its stand-in in the other.
            PrRenderKhronosPbrMaterial pending = texturedMaterial();
            PrRenderKhronosPbrMaterial stoodIn = texturedMaterial();
            for (uint32_t other = 0; other < Slot_Count; ++other) {
                PrRenderTexture texture = createPending(f);
                if (other != slot)
                    setTexels(f, texture, 2, kTexels[other]);
                *useOf(&pending, other) = useOf(f, texture);
                *useOf(&stoodIn, other) = useOf(f, other == slot ? createStandIn(f, slot) : texture);
            }
            PrRenderKhronosPbrMaterial arrived = pending;
            PrRenderTexture whole = createPending(f);
            setTexels(f, whole, 2, kTexels[slot]);
            *useOf(&arrived, slot) = useOf(f, whole);
            for (PrRenderTechnique technique : techniques(f)) {
                CAPTURE(technique);
                Image waiting = draw(f, wall, pending, technique);
                Image expected = draw(f, wall, stoodIn, technique);
                Difference difference = compare(waiting, expected, 0);
                CAPTURE(difference.largest);
                CHECK(difference.over == 0);
                // The texture's own texels draw another image, so the comparison is of the stand-in.
                CHECK(compare(waiting, draw(f, wall, arrived, technique), 0.01f).over > kExtent.x * kExtent.y / 2);
            }
        }
    });
}

TEST_CASE("a glTF material whose textures have not arrived is a rough dielectric, flat, unoccluded and dark") {
    onEachDevice([](Fixture& f) {
        Wall wall = createWall(f);
        for (PrRenderTechnique technique : techniques(f)) {
            CAPTURE(technique);
            PrRenderKhronosPbrMaterial plain = texturedMaterial();

            // A metallic-roughness texture without texels makes no metal: the surface is the dielectric of the
            // material's roughness factor, whatever its metallic factor.
            PrRenderKhronosPbrMaterial waiting = plain;
            waiting.metallicRoughnessTexture = useOf(f, createPending(f));
            PrRenderKhronosPbrMaterial dielectric = plain;
            dielectric.metallic = 0;
            Image dielectricImage = draw(f, wall, dielectric, technique);
            CHECK(compare(draw(f, wall, waiting, technique), dielectricImage, 0).over == 0);
            // The metal that the factor alone describes is another image, which a white stand-in would draw.
            CHECK(compare(draw(f, wall, plain, technique), dielectricImage, 0.01f).over > kExtent.x * kExtent.y / 2);

            // A normal map without texels lights the surface as no normal map does.
            waiting = plain;
            waiting.normalTexture = useOf(f, createPending(f));
            Difference flat = compare(draw(f, wall, waiting, technique), draw(f, wall, plain, technique), 1e-4f);
            CAPTURE(flat.largest);
            CHECK(flat.over == 0);

            // An occlusion texture without texels dims nothing.
            waiting = plain;
            waiting.occlusionTexture = useOf(f, createPending(f));
            CHECK(compare(draw(f, wall, waiting, technique), draw(f, wall, plain, technique), 0).over == 0);

            // An emissive texture without texels adds no light, whatever the emissive factor.
            waiting = plain;
            waiting.emissiveTexture = useOf(f, createPending(f));
            PrRenderKhronosPbrMaterial dark = plain;
            dark.emissive = {0, 0, 0};
            Image darkImage = draw(f, wall, dark, technique);
            CHECK(compare(draw(f, wall, waiting, technique), darkImage, 0).over == 0);
            CHECK(meanOf(draw(f, wall, plain, technique)) > meanOf(darkImage) + 0.3);

            // A base color texture without texels leaves the base color factor.
            waiting = plain;
            waiting.baseColorTexture = useOf(f, createPending(f));
            CHECK(compare(draw(f, wall, waiting, technique), draw(f, wall, plain, technique), 0).over == 0);
        }
    });
}

TEST_CASE("a glTF material's textures arrive in any order, and each step draws the stand-ins of those still to come") {
    onEachDevice([](Fixture& f) {
        Wall wall = createWall(f);
        // The textures that arrive: every one whose stand-in is not the white of a texture that a material lacks, and
        // the base color.
        constexpr uint32_t kArriving = 4;
        const uint32_t slots[kArriving] = {Slot_BaseColor, Slot_MetallicRoughness, Slot_Normal, Slot_Emissive};
        // By the set of the textures that have arrived, as a bit for each: the image of a material with those textures'
        // texels, and a texture of one texel of the stand-in for each of the rest.
        Array<Image> references;
        for (uint32_t arrived = 0; arrived < (1u << kArriving); ++arrived) {
            PrRenderKhronosPbrMaterial material = texturedMaterial();
            for (uint32_t i = 0; i < kArriving; ++i) {
                PrRenderTexture texture = createStandIn(f, slots[i]);
                if ((arrived & (1u << i)) != 0)
                    setTexels(f, texture, 2, kTexels[slots[i]]);
                *useOf(&material, slots[i]) = useOf(f, texture);
            }
            references.push(draw(f, wall, material, PrRenderTechnique_Forward));
        }
        // The material with no metal and no emission, which the wall is while those two textures are still to come.
        PrRenderKhronosPbrMaterial calm = texturedMaterial();
        calm.metallic = 0;
        calm.emissive = {0, 0, 0};
        CHECK(compare(references[0], draw(f, wall, calm, PrRenderTechnique_Forward), 1e-4f).over == 0);

        uint32_t orders = 0;
        uint32_t order[kArriving];
        for (order[0] = 0; order[0] < kArriving; ++order[0]) {
            for (order[1] = 0; order[1] < kArriving; ++order[1]) {
                for (order[2] = 0; order[2] < kArriving; ++order[2]) {
                    for (order[3] = 0; order[3] < kArriving; ++order[3]) {
                        uint32_t seen = 0;
                        for (uint32_t i : order)
                            seen |= 1u << i;
                        if (seen != (1u << kArriving) - 1)
                            continue;
                        ++orders;
                        CAPTURE(order[0]);
                        CAPTURE(order[1]);
                        CAPTURE(order[2]);
                        CAPTURE(order[3]);
                        PrRenderKhronosPbrMaterial material = texturedMaterial();
                        PrRenderTexture textures[kArriving];
                        for (uint32_t i = 0; i < kArriving; ++i) {
                            textures[i] = createPending(f);
                            *useOf(&material, slots[i]) = useOf(f, textures[i]);
                        }
                        uint32_t arrived = 0;
                        for (uint32_t step = 0; step <= kArriving; ++step) {
                            CAPTURE(step);
                            Image image = draw(f, wall, material, PrRenderTechnique_Forward);
                            Difference difference = compare(image, references[arrived], 0);
                            CAPTURE(difference.largest);
                            CHECK(difference.over == 0);
                            if (step == kArriving)
                                break;
                            setTexels(f, textures[order[step]], 2, kTexels[slots[order[step]]]);
                            arrived |= 1u << order[step];
                        }
                        for (PrRenderTexture texture : textures)
                            prRenderTextureDestroy(f.render, texture);
                    }
                }
            }
        }
        CHECK(orders == 24);

        // A texture that is destroyed reads as it did before it arrived.
        PrRenderKhronosPbrMaterial material = texturedMaterial();
        PrRenderTexture gloss = createPending(f);
        setTexels(f, gloss, 2, kTexels[Slot_MetallicRoughness]);
        material.metallicRoughnessTexture = useOf(f, gloss);
        Image whole = draw(f, wall, material, PrRenderTechnique_Forward);
        prRenderTextureDestroy(f.render, gloss);
        Image destroyed = draw(f, wall, material, PrRenderTechnique_Forward);
        PrRenderKhronosPbrMaterial dielectric = texturedMaterial();
        dielectric.metallic = 0;
        CHECK(compare(destroyed, draw(f, wall, dielectric, PrRenderTechnique_Forward), 0).over == 0);
        CHECK(compare(destroyed, whole, 0.01f).over > kExtent.x * kExtent.y / 2);
    });
}

TEST_SUITE_END();
