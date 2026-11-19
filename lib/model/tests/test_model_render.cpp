// Models in a renderer: what a frame draws while a model loads and once it has, as its placements move, and after its
// device is lost.

#include "pr/alloc.h"
#include "pr/blob.h"
#include "pr/geom.h"
#include "pr/gltf_model.h"
#include "pr/gpu.h"
#include "pr/gpu_select.h"
#include "pr/memory.h"
#include "pr/model.h"
#include "pr/model_host.h"
#include "pr/model_source_backend.h"
#include "pr/model_texture.h"
#include "pr/render.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/task.h"
#include "pr/thread.h"

#include "model/tests/model_fixture.h"
#include "tools/harness/blob_fixture.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

constexpr PrVec3 kRed{1, 0, 0};
constexpr PrVec3 kWhite{1, 1, 1};
constexpr PrVec3 kSky{0.5f, 0.5f, 0.5f};

// The members of an asset whose quad shows the image "color.png" unlit, through a sampler that does not filter.
String texturedQuad() {
    return modelQuad(
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"extensions":{"KHR_materials_unlit":{}}})",
        R"("nodes":[{"mesh":0}],"images":[{"uri":"color.png"}],"samplers":[{"magFilter":9728,"minFilter":9728}],)"
        R"("textures":[{"source":0,"sampler":0}],)");
}

// The members of an asset whose red quad an animation moves from the origin to (4, 0, 0), out of the front view.
String animatedQuad() {
    return modelQuad(kModelUnlitRed,
        R"("nodes":[{"mesh":0}],"animations":[{"samplers":[{"input":3,"output":4}],)"
        R"("channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]}],)");
}

// The members of an asset whose red quad has a skin of one joint, which moves every vertex. The joint's inverse bind
// matrix moves the quad one aside along x, so that at rest it covers the right half of the front view, and an animation
// moves the joint from the origin to (4, 0, 0). The quad's own node is moved out of the view, which a skinned mesh
// ignores.
String skinnedQuad() {
    String members = R"("extensionsUsed":["KHR_materials_unlit"],"scene":0,"scenes":[{"nodes":[0,1]}],)";
    members.append(R"("nodes":[{"mesh":0,"skin":0,"translation":[0,7,0]},{}],"meshes":[{"primitives":[{)");
    members.append(R"("attributes":{"POSITION":0,"NORMAL":5,"JOINTS_0":13,"WEIGHTS_0":11},"indices":1,"material":0}]}],)");
    members.append(R"("skins":[{"joints":[1],"inverseBindMatrices":16}],)");
    members.append(R"("animations":[{"samplers":[{"input":3,"output":4}],)");
    members.append(R"("channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}],"materials":[)");
    return members.append(kModelUnlitRed).append("]");
}

// The members of an asset whose red quad, at half its size, has a morph target that adds each vertex's own position to
// it. An animation takes the target's weight from 0 to 1, which doubles the quad until it fills the front view.
String morphedQuad() {
    String members = R"("extensionsUsed":["KHR_materials_unlit"],"scene":0,"scenes":[{"nodes":[0]}],)";
    members.append(R"("nodes":[{"mesh":0,"scale":[0.5,0.5,1]}],"meshes":[{"primitives":[{)");
    members.append(R"("attributes":{"POSITION":0,"NORMAL":5},"indices":1,"material":0,"targets":[{"POSITION":10}]}]}],)");
    members.append(R"("animations":[{"samplers":[{"input":3,"output":3}],)");
    members.append(R"("channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}],"materials":[)");
    return members.append(kModelUnlitRed).append("]");
}

// The members of an asset whose quad shows the texture of the image `uri` unlit, as `texturedQuad` does, with the
// texture's source in KHR_texture_basisu where the image is KTX 2.0.
String texturedQuadOf(PrStringView uri, bool basisu) {
    String more = R"("nodes":[{"mesh":0}],"samplers":[{"magFilter":9728,"minFilter":9728}],"images":[{"uri":")";
    more.append(uri).append(R"("}],)");
    more.append(basisu ? R"("textures":[{"sampler":0,"extensions":{"KHR_texture_basisu":{"source":0}}}],)" :
        R"("textures":[{"source":0,"sampler":0}],)");
    return modelQuad(
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"extensions":{"KHR_materials_unlit":{}}})", more);
}

// The members of an asset whose quad has the normal texture of the image `uri`, lit by a directional light that shines
// at its face, so that a normal tilted away from the light darkens it.
String normalQuadOf(PrStringView uri) {
    String members = R"("extensionsUsed":["KHR_lights_punctual"],"scene":0,"scenes":[{"nodes":[0,1]}],)";
    members.append(R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"directional","intensity":3}]}},)");
    members.append(R"("nodes":[{"mesh":0},{"extensions":{"KHR_lights_punctual":{"light":0}},"translation":[0,0,2]}],)");
    members.append(R"("samplers":[{"magFilter":9728,"minFilter":9728}],"images":[{"uri":")").append(uri);
    members.append(R"("}],"textures":[{"source":0,"sampler":0}],)");
    members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5,"TEXCOORD_0":2,"TANGENT":6},)");
    members.append(R"("indices":1,"material":0}]}],"materials":[{"normalTexture":{"index":0}}])");
    return members;
}

// Draws the asset of `members`, whose files are `files`, until its images have loaded, and returns the front view.
Image drawAsset(Fixture& f, const ModelCodecs& codecs, const Array<ModelFile>& files, PrStringView members,
        PrModelResourceInfo* PR_NULLABLE imageInfo = nullptr) {
    ModelAsset asset;
    for (const ModelFile& file : files)
        asset.files.push(ModelFile{file.uri, Array<uint8_t>(file.bytes)});
    PrModelOptions options = prModelOptionsDefault();
    options.textures = codecs.textures;
    PrModel* model = modelOf(asset.read(members), &options);
    prModelPopulate(model, f.render);
    PrRenderScene scene = prRenderSceneCreate(f.render);
    setEnvironment(f, scene, createEvenEnvironment(f, PrVec3{0.5f, 0.5f, 0.5f}));
    PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
    PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
    for (uint32_t i = 0; i < 8 && !prModelIsLoaded(model); ++i)
        prModelUpdate(model);
    CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Ready);
    if (imageInfo != nullptr)
        *imageInfo = prModelImageInfo(model, 0);
    Image image = f.view(frontView(scene));
    prModelPlacementDestroy(placement);
    prModelDestroy(model);
    prRenderSceneDestroy(f.render, scene);
    return image;
}

// Returns true if the colors of two texels are within `tolerance` of each other in each channel.
bool nearTexel(const float* texel, const float* other, float tolerance) {
    return near(texel, PrVec3{other[0], other[1], other[2]}, tolerance);
}

// Creates a scene with an even gray environment in a fixture's renderer.
PrRenderScene createScene(Fixture& f) {
    PrRenderScene scene = prRenderSceneCreate(f.render);
    setEnvironment(f, scene, createEvenEnvironment(f, kSky));
    return scene;
}

// Returns the color at the center of the front view of a scene.
const float* center(Fixture& f, PrRenderScene scene, Image* image) {
    *image = f.view(frontView(scene));
    return image->at(kCenter, kCenter);
}

// A byte provider of bytes in memory that completes its reads later, when the test polls it, as a provider over a
// network or a page's event loop does, and that counts its reads.
struct LateBytes {
    Array<uint8_t> bytes;
    Array<PrBlobRead*> pending;
    uint32_t reads = 0;
    PrBlobSource* source = nullptr;

    explicit LateBytes(PrByteView given) {
        bytes.append(given);
        PrBlobSourceDesc desc{.start = start, .context = this, .size = given.size};
        source = prBlobSourceCreate(&desc);
    }
    LateBytes(const LateBytes&) = delete;
    LateBytes& operator=(const LateBytes&) = delete;
    ~LateBytes() { prBlobSourceRelease(source); }

    static void start(void* PR_NULLABLE context, PrBlobRead* read) {
        LateBytes* late = static_cast<LateBytes*>(context);
        ++late->reads;
        late->pending.push(read);
    }

    // Completes the reads that were started before the call, with the ranges of the bytes that they ask for. Returns
    // the number that it completed.
    uint32_t poll() {
        Array<PrBlobRead*> started = move(pending);
        for (PrBlobRead* read : started) {
            PrBlob* whole = prBlobCopy(PrByteView{bytes.begin(), bytes.count()});
            PrBlobRange range = prBlobReadRange(read);
            PrBlob* part = prBlobSlice(whole, range.offset, range.size);
            prBlobRelease(whole);
            prBlobReadComplete(read, PrStatus_Ok, part, 0);
        }
        return started.count();
    }
};

// A model source that is a glTF document's, but whose images' bytes arrive late: each opens as a provider of
// `LateBytes`, so a texture's format is known only once its bytes arrive.
struct LateImageSource : PrModelSource {
    PrModelSource* inner;
    LateBytes* image;

    static void destroy(PrModelSource* source) {
        LateImageSource* late = static_cast<LateImageSource*>(source);
        prModelSourceDestroy(late->inner);
        prMemoryFree(late);
    }
    static const PrModelSourceInfo* info(const PrModelSource* source) {
        return prModelSourceInfo(static_cast<const LateImageSource*>(source)->inner);
    }
    static PrStatus meshRead(const PrModelSource* source, PrModelMeshIndex mesh, const PrModelMeshRequest* request,
            PrModelMeshData* out) {
        return prModelSourceMeshRead(static_cast<const LateImageSource*>(source)->inner, mesh, request, out);
    }
    static PrStatus imageOpen(const PrModelSource* source, PrModelImageIndex, PrModelImageData* out) {
        const LateImageSource* late = static_cast<const LateImageSource*>(source);
        prBlobSourceRetain(late->image->source);
        out->bytes = late->image->source;
        return PrStatus_Ok;
    }

    static PrModelSource* create(const PrGltf* gltf, LateBytes* image) {
        LateImageSource* late = allocZeroed<LateImageSource>();
        late->destroyFn = destroy;
        late->infoFn = info;
        late->meshReadFn = meshRead;
        late->imageOpenFn = imageOpen;
        late->inner = prGltfModelSourceCreate(gltf, nullptr);
        late->image = image;
        return late;
    }
};

// The text of an asset of the tests with `members`, as ModelAsset::read reads it.
String assetText(PrStringView members) {
    String text = R"({"asset":{"version":"2.0"},)";
    text.append(kModelAccessors);
    return text.append(",").append(members).append("}");
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("model" * doctest::skip(!gpuTests()));

TEST_CASE("a model is placed at once, and draws each part when its data arrives") {
    ModelCodecs codecs;
    onEachDevice([&codecs](Fixture& f) {
        ModelAsset asset;
        const uint8_t red[4] = {255, 0, 0, 255};
        asset.files.push(ModelFile{"color.png", codecs.png({1, 1}, red)});
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        PrModel* model = modelOf(asset.read(texturedQuad()), &options);
        prModelPopulate(model, f.render);
        CHECK(prModelRender(model) == f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        Image image;

        // Nothing has loaded. The instance exists, and the frame draws without it.
        CHECK(near(center(f, scene, &image), kSky, 0));
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 1);
        PrModelProgress progress = prModelProgress(model);
        CHECK(progress.meshCount == 1);
        CHECK(progress.meshesReady == 0);
        CHECK(progress.imageCount == 1);
        CHECK(progress.imagesReady == 0);
        CHECK_FALSE(prModelIsLoaded(model));
        CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Pending);

        // Without a pool, each update runs one job, and the mesh is first: the quad draws with its material's factor,
        // since an unlit material reads a color texture without texels as white.
        prModelUpdate(model);
        CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Ready);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Pending);
        CHECK(near(center(f, scene, &image), kWhite, 1e-3f));
        CHECK(prRenderMeshIsReady(f.render, prModelMesh(model, 0)));
        CHECK(prModelMaterial(model, 0).id != 0);

        prModelUpdate(model);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Ready);
        CHECK(prModelIsLoaded(model));
        CHECK(near(center(f, scene, &image), kRed, 1e-3f));

        // The model's resources go with it.
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        CHECK(near(center(f, scene, &image), kSky, 0));
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 0);
    });
}

TEST_CASE("a wall with color, metallic-roughness and normal images never draws as metal while they arrive") {
    ModelCodecs codecs;
    onEachDevice([&codecs](Fixture& f) {
        // A light wall that is a rough dielectric, as its images say: its material's factors alone, which are glTF's
        // defaults, say a rough metal. Under one light, away from its mirror direction, the metal is much the darker.
        const uint8_t color[4] = {230, 230, 230, 255};
        const uint8_t gloss[4] = {0, 255, 0, 255};
        const uint8_t flat[4] = {128, 128, 255, 255};
        const PrStringView names[3] = {"color.png", "gloss.png", "flat.png"};
        const uint8_t* texels[3] = {color, gloss, flat};
        // The model loads its images in their order in the document, so each order of the three is a document.
        const uint32_t orders[6][3] = {{0, 1, 2}, {0, 2, 1}, {1, 0, 2}, {1, 2, 0}, {2, 0, 1}, {2, 1, 0}};
        for (const uint32_t* order : orders) {
            CAPTURE(order[0]);
            CAPTURE(order[1]);
            CAPTURE(order[2]);
            ModelAsset asset;
            String images = R"("images":[)";
            const PrStringView kDigits[3] = {"0", "1", "2"};
            // Where each of the three images is among the document's.
            uint32_t at[3] = {};
            for (uint32_t i = 0; i < 3; ++i) {
                asset.files.push(ModelFile{names[order[i]], codecs.png({1, 1}, texels[order[i]])});
                images.append(i == 0 ? "" : ",").append(R"({"uri":")").append(names[order[i]]).append(R"("})");
                at[order[i]] = i;
            }
            images.append(R"(],"textures":[{"source":0},{"source":1},{"source":2}],"nodes":[{"mesh":0}],)");
            String material = R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":)";
            material.append(kDigits[at[0]]).append(R"(},"metallicRoughnessTexture":{"index":)");
            material.append(kDigits[at[1]]).append(R"(}},"normalTexture":{"index":)");
            material.append(kDigits[at[2]]).append("}}");
            PrModelOptions options = prModelOptionsDefault();
            options.textures = codecs.textures;
            PrModel* model = modelOf(asset.read(modelQuad(material, images)), &options);
            prModelPopulate(model, f.render);
            PrRenderScene scene = prRenderSceneCreate(f.render);
            PrRenderLightDesc sun = prRenderLightDescDefault();
            sun.direction = {-0.6f, -0.3f, -0.74f};
            sun.intensity = 3;
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            prRenderLightAdd(update, &sun);
            prRenderUpdateCommit(update);
            PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
            prModelPlacementCreate(model, &desc);

            // The wall's brightness after the mesh has arrived, and after each image.
            Array<float> steps;
            Image image;
            prModelUpdate(model);
            REQUIRE(prModelMeshInfo(model, 0).load == PrModelLoad_Ready);
            steps.push(center(f, scene, &image)[1]);
            while (!prModelIsLoaded(model)) {
                prModelUpdate(model);
                steps.push(center(f, scene, &image)[1]);
            }
            REQUIRE(steps.count() == 4);
            CHECK(prModelProgress(model).imagesReady == 3);
            // The whole wall is the dielectric, which is bright. No step before it is darker, as a metal would be: an
            // image that has not arrived leaves a rough dielectric with a flat normal, and a white base color. The flat
            // normal of an image of 8 bits leans by a 255th, which the stand-in's does not.
            float whole = steps[3];
            CHECK(whole > 0.4f);
            for (uint32_t step = 0; step < 3; ++step) {
                CAPTURE(step);
                CHECK(steps[step] >= whole - 0.005f);
            }
            // The metal that the factors alone describe, which the wall never was, is less than half as bright.
            PrRenderKhronosPbrMaterial metal = prRenderKhronosPbrMaterialDefault();
            prRenderMaterialSetKhronosPbr(f.render, prModelMaterial(model, 0), &metal);
            CHECK(center(f, scene, &image)[1] < whole * 0.5f);
            prModelDestroy(model);
        }
    });
}

TEST_CASE("an image that fails to load fails alone, and the model draws without it") {
    ModelCodecs codecs;
    onEachDevice([&codecs](Fixture& f) {
        // The quad's material reads an image that is no image. Two more materials read an image whose file is missing,
        // and a good one.
        ModelAsset asset;
        const uint8_t green[4] = {0, 255, 0, 255};
        Array<uint8_t> garbage(64);
        for (uint32_t i = 0; i < garbage.count(); ++i)
            garbage[i] = uint8_t(i * 7 + 1);
        asset.files.push(ModelFile{"bad.png", garbage});
        asset.files.push(ModelFile{"good.png", codecs.png({1, 1}, green)});
        String members = modelQuad(
            R"({"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1],"baseColorTexture":{"index":0}},)"
            R"("extensions":{"KHR_materials_unlit":{}}},)"
            R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":1}}},)"
            R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":2}}})",
            R"("nodes":[{"mesh":0}],"images":[{"uri":"bad.png"},{"uri":"missing.png"},{"uri":"good.png"},)"
            R"({"uri":"good.png"}],"textures":[{"source":0},{"source":1},{"source":2}],)");
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        PrModel* model = modelOf(asset.read(members), &options);
        prModelPopulate(model, f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        prModelPlacementCreate(model, &desc);
        prModelWait(model);

        CHECK(prModelIsLoaded(model));
        PrModelProgress progress = prModelProgress(model);
        CHECK(progress.imageCount == 3);
        CHECK(progress.imagesReady == 1);
        CHECK(progress.imagesFailed == 2);
        CHECK(progress.meshesReady == 1);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Failed);
        CHECK(prModelImageInfo(model, 0).status == PrStatus_NotSupported);
        CHECK(prModelImageInfo(model, 1).load == PrModelLoad_Failed);
        CHECK(prModelImageInfo(model, 1).status == PrStatus_Io);
        CHECK(prModelImageInfo(model, 2).load == PrModelLoad_Ready);
        // No material reads the last image, so the model does not load it.
        CHECK(prModelImageInfo(model, 3).load == PrModelLoad_None);
        Image image;
        CHECK(near(center(f, scene, &image), kRed, 1e-3f));

        // Without a codec, every image fails, and the model still draws.
        PrModel* undecoded = modelOf(asset.gltf, nullptr);
        prModelPopulate(undecoded, f.render);
        prModelWait(undecoded);
        CHECK(prModelImageInfo(undecoded, 2).load == PrModelLoad_Failed);
        CHECK(prModelImageInfo(undecoded, 2).status == PrStatus_NotSupported);
        CHECK(prModelMeshInfo(undecoded, 0).load == PrModelLoad_Ready);
        prModelDestroy(undecoded);
        prModelDestroy(model);
    });
}

TEST_CASE("a mesh whose buffer has no bytes fails as Io, and the frame draws without it") {
    onEachDevice([](Fixture& f) {
        ModelAsset asset;
        asset.hasBin = false;
        PrModel* model = modelOf(asset.read(modelQuad(kModelUnlitRed, R"("nodes":[{"mesh":0}],)")), nullptr);
        prModelPopulate(model, f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        prModelPlacementCreate(model, &desc);
        prModelWait(model);
        CHECK(prModelIsLoaded(model));
        CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Failed);
        CHECK(prModelMeshInfo(model, 0).status == PrStatus_Io);
        CHECK(prModelProgress(model).meshesFailed == 1);
        Image image;
        CHECK(near(center(f, scene, &image), kSky, 0));
        prModelDestroy(model);
    });
}

TEST_CASE("a pool loads a model on its threads while frames render") {
    if (!prThreadSupported())
        return;
    ModelCodecs codecs;
    PrTaskPool* pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 2);
    REQUIRE(pool != nullptr);
    onEachDevice([&codecs, pool](Fixture& f) {
        ModelAsset asset;
        const uint8_t red[4] = {255, 0, 0, 255};
        asset.files.push(ModelFile{"color.png", codecs.png({1, 1}, red)});
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        options.pool = pool;
        PrModel* model = modelOf(asset.read(texturedQuad()), &options);
        prModelPopulate(model, f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        // Frames render while the jobs run, and each shows the sky, the bare quad or the textured quad.
        Image image;
        while (!prModelIsLoaded(model)) {
            prModelUpdate(model);
            const float* shown = center(f, scene, &image);
            CHECK((near(shown, kSky, 0) || near(shown, kWhite, 1e-3f) || near(shown, kRed, 1e-3f)));
        }
        prModelWait(model);
        CHECK(near(center(f, scene, &image), kRed, 1e-3f));

        // Populating again while jobs may be running waits for them, and loads the model again.
        prModelPopulate(model, f.render);
        prModelPopulate(model, f.render);
        prModelPlacementSetScene(placement, scene);
        prModelWait(model);
        CHECK(prModelIsLoaded(model));
        CHECK(near(center(f, scene, &image), kRed, 1e-3f));
        prModelDestroy(model);
    });
    prTaskPoolDestroy(pool);
}

TEST_CASE("an update writes a placement's pose to its instances, and each placement has its own") {
    onEachDevice([](Fixture& f) {
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(animatedQuad()), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        Image image;
        CHECK(near(center(f, scene, &image), kRed, 0));

        // The animation moves the quad out of the view, once an update writes the pose.
        prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
        prModelPlacementSetTime(placement, 1);
        CHECK(near(center(f, scene, &image), kRed, 0));
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), kSky, 0));

        // The root moves it back.
        PrMatrix4 root = prMatrix4Translate({-4, 0, 0});
        prModelPlacementSetRoot(placement, &root);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), kRed, 0));

        // A second placement is at rest in the view, whatever the first one plays.
        prModelPlacementSetRoot(placement, &desc.root);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), kSky, 0));
        PrModelPlacement* second = prModelPlacementCreate(model, &desc);
        CHECK(near(center(f, scene, &image), kRed, 0));
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 2);

        // A placement leaves its scene with its instances, and enters another in the pose that it has.
        PrRenderScene other = createScene(f);
        prModelPlacementSetScene(second, other);
        CHECK(near(center(f, scene, &image), kSky, 0));
        CHECK(prRenderSceneInstanceCount(f.render, scene) == 1);
        CHECK(near(center(f, other, &image), kRed, 0));
        prModelPlacementDestroy(second);
        CHECK(near(center(f, other, &image), kSky, 0));
        CHECK(prRenderSceneInstanceCount(f.render, other) == 0);
        prModelDestroy(model);
    });
}

TEST_CASE("a skinned model is drawn where its joints put it, as an animation moves them") {
    onEachDevice([](Fixture& f) {
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(skinnedQuad()), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        constexpr uint32_t kLeft = 4;
        constexpr uint32_t kRight = kExtent.x - 5;

        // At rest the joint is at the origin, and its inverse bind matrix puts the quad over the right half of the
        // view.
        Image image = f.view(frontView(scene));
        CHECK(near(image.at(kLeft, kCenter), kSky, 0));
        CHECK(near(image.at(kRight, kCenter), kRed, 0));
        CHECK(near(image.at(kCenter + 2, 2), kRed, 0));

        // The animation moves the joint, and the quad with it: half a unit to the right, and then out of the view.
        prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
        prModelPlacementSetTime(placement, 0.125f);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kCenter + 2, kCenter), kSky, 0));
        CHECK(near(image.at(kRight, kCenter), kRed, 0));
        prModelPlacementSetTime(placement, 1);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kRight, kCenter), kSky, 0));

        // The placement's root moves a skinned mesh as it moves any other.
        PrMatrix4 root = prMatrix4Translate({-5, 0, 0});
        prModelPlacementSetRoot(placement, &root);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kLeft, kCenter), kRed, 0));
        CHECK(near(image.at(kRight, kCenter), kRed, 0));

        // A second placement has a pose of its own.
        PrModelPlacement* second = prModelPlacementCreate(model, &desc);
        prModelPlacementSetRoot(placement, &desc.root);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kLeft, kCenter), kSky, 0));
        CHECK(near(image.at(kRight, kCenter), kRed, 0));
        prModelPlacementDestroy(second);
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
    });
}

TEST_CASE("a model's morph targets are drawn at the weights that an animation gives them") {
    onEachDevice([](Fixture& f) {
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(morphedQuad()), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        // The pixels whose centers are 0.85 and 0.73 to the left of the view's center.
        constexpr uint32_t kOuter = 2;
        constexpr uint32_t kInner = 4;

        // Without a weight the quad reaches one half from the center.
        Image image = f.view(frontView(scene));
        CHECK(near(image.at(kCenter, kCenter), kRed, 0));
        CHECK(near(image.at(kInner, kCenter), kSky, 0));

        // At a weight of one half it reaches three quarters, and at a weight of one it fills the view.
        prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
        prModelPlacementSetTime(placement, 0.5f);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kInner, kCenter), kRed, 0));
        CHECK(near(image.at(kOuter, kCenter), kSky, 0));
        prModelPlacementSetTime(placement, 1);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kOuter, kCenter), kRed, 0));
        CHECK(near(image.at(0, 0), kRed, 0));

        // Back at rest, the quad is as it was.
        prModelPlacementSetAnimation(placement, kPrModelAnimationNone, PrModelLoop_Once);
        prModelUpdate(model);
        image = f.view(frontView(scene));
        CHECK(near(image.at(kInner, kCenter), kSky, 0));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
    });
}

TEST_CASE("a placement adds the document's lights to its scene, and moves them with their nodes") {
    onEachDevice([](Fixture& f) {
        // A white, rough quad that only the document's light can light: a directional light that shines along its
        // node's -z, which is at the quad.
        ModelAsset asset;
        String members = modelQuad(R"({"pbrMetallicRoughness":{"metallicFactor":0}})",
            R"("nodes":[{"mesh":0,"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
            R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"directional","intensity":2}]}},)");
        PrModel* model = modelOf(asset.read(members), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        Image image;
        float lit = center(f, scene, &image)[0];
        CHECK(lit > 0.3f);
        CHECK(prRenderSceneLightCount(f.render, scene) == 1);

        // A placement that does not add the lights leaves the quad dark.
        desc.addsLights = false;
        PrRenderScene dark = prRenderSceneCreate(f.render);
        prModelPlacementSetScene(placement, PrRenderScene{});
        desc.scene = dark;
        PrModelPlacement* unlit = prModelPlacementCreate(model, &desc);
        CHECK(center(f, dark, &image)[0] == 0);
        CHECK(prRenderSceneLightCount(f.render, dark) == 0);
        prModelPlacementDestroy(unlit);
        prModelDestroy(model);
    });
}

TEST_CASE("a model's emission is in the unit that its options give, which it changes in its renderer") {
    onEachDevice([](Fixture& f) {
        // A black quad that emits 0.5, 0.25 and 0.125, in a view exposed by a hundredth.
        ModelAsset asset;
        String members = modelQuad(
            R"({"pbrMetallicRoughness":{"baseColorFactor":[0,0,0,1],"metallicFactor":0},"emissiveFactor":[0.5,0.25,0.125]})",
            R"("nodes":[{"mesh":0}],)");
        PrModel* model = modelOf(asset.read(members), nullptr);
        CHECK(prModelEmission(model) == PrRenderEmission_Luminance);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        PrRenderViewDesc view = frontView(scene);
        view.exposure = 0.01f;
        CHECK(near(f.view(view).at(kCenter, kCenter), {0.005f, 0.0025f, 0.00125f}, 1e-4f));
        prModelSetEmission(model, PrRenderEmission_Display);
        CHECK(prModelEmission(model) == PrRenderEmission_Display);
        CHECK(near(f.view(view).at(kCenter, kCenter), {0.5f, 0.25f, 0.125f}, 2e-3f));
        // A model created in the view's unit draws so from the first.
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        PrModelOptions options = prModelOptionsDefault();
        options.emission = PrRenderEmission_Display;
        model = modelOf(asset.read(members), &options);
        prModelPopulate(model, f.render);
        prModelWait(model);
        placement = prModelPlacementCreate(model, &desc);
        CHECK(near(f.view(view).at(kCenter, kCenter), {0.5f, 0.25f, 0.125f}, 2e-3f));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a placement scales each of the document's lights by its own factor") {
    onEachDevice([](Fixture& f) {
        // The white quad that a directional light of 2 lux lights, as above.
        ModelAsset asset;
        String members = modelQuad(R"({"pbrMetallicRoughness":{"metallicFactor":0}})",
            R"("nodes":[{"mesh":0,"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
            R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"directional","intensity":2}]}},)");
        PrModel* model = modelOf(asset.read(members), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        CHECK(prModelPlacementLightScale(placement, 0) == 1);
        Image image;
        float lit = center(f, scene, &image)[0];
        REQUIRE(lit > 0.3f);
        prModelPlacementSetLightScale(placement, 0, 8);
        CHECK(prModelPlacementLightScale(placement, 0) == 8);
        CHECK(prModelPlacementLight(placement, 0).intensity == 16);
        prModelUpdate(model);
        CHECK(center(f, scene, &image)[0] == doctest::Approx(lit * 8).epsilon(2e-3));
        prModelPlacementSetLightScale(placement, 0, 0);
        prModelUpdate(model);
        CHECK(center(f, scene, &image)[0] == 0);
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a model populates a renderer on a new device after a loss, and its placements keep their poses") {
    for (const TestDevice& shared : testDevices()) {
        PrStringView backend = prBackendName(prGpuSelectInterface(), shared.backend);
        CAPTURE(backend);
        CAPTURE(shared.slotsOnly);
        PrGpuDeviceOptions options{.validation = true, .slotsOnly = shared.slotsOnly};
        PrGpuDevice* lost = nullptr;
        REQUIRE(gpuDeviceCreate(shared.backend, &options, &lost) == PrStatus_Ok);
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(animatedQuad()), nullptr);
        {
            Fixture f(TestDevice{shared.backend, shared.slotsOnly, lost});
            prModelPopulate(model, f.render);
            prModelWait(model);
            PrRenderScene scene = createScene(f);
            PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
            PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
            // The placement plays the animation to its end, and its root brings the quad back into the view.
            PrMatrix4 root = prMatrix4Translate({-4, 0, 0});
            prModelPlacementSetRoot(placement, &root);
            prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
            prModelPlacementSetTime(placement, 1);
            prModelUpdate(model);
            Image image;
            CHECK(near(center(f, scene, &image), kRed, 0));

            // On the lost device, populating again is safe, and what loads fails as Lost.
            prGpuDeviceSimulateLoss(lost);
            prModelPopulate(model, f.render);
            prModelWait(model);
            CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Failed);
            CHECK(prModelMeshInfo(model, 0).status == PrStatus_Lost);

            // One call moves the model to the renderer of a new device, while the old renderer still exists.
            Fixture recovered(shared);
            uint32_t errors = validationErrors(shared);
            prModelPopulate(model, recovered.render);
            CHECK(prModelRender(model) == recovered.render);
            CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Pending);
            PrRenderScene again = createScene(recovered);
            prModelPlacementSetScene(placement, again);
            prModelWait(model);
            CHECK(prModelIsLoaded(model));
            CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Ready);
            CHECK(prModelPlacementAnimation(placement) == 0);
            CHECK(prModelPlacementTime(placement) == 1);
            CHECK(near(center(recovered, again, &image), kRed, 0));
            prModelDestroy(model);
            CHECK(validationErrors(shared) == errors);
        }
        prGpuDeviceDestroy(lost);
    }
}

TEST_CASE("a model draws the same pixels on a device with the slots alone as on one with handles") {
    ModelCodecs codecs;
    Array<Image> images;
    Array<TestDevice> devices;
    for (const TestDevice& device : testDevices()) {
        Fixture f(device);
        // A lit quad with a checker of four texels, under a light that shines at it from one side.
        ModelAsset asset;
        const uint8_t checker[16] = {255, 255, 255, 255, 40, 40, 200, 255, 200, 40, 40, 255, 255, 255, 255, 255};
        asset.files.push(ModelFile{"color.png", codecs.png({2, 2}, checker)});
        String members = modelQuad(
            R"({"pbrMetallicRoughness":{"metallicFactor":0,"roughnessFactor":0.5,"baseColorTexture":{"index":0}}})",
            R"("nodes":[{"mesh":0}],"images":[{"uri":"color.png"}],"textures":[{"source":0}],)");
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        PrModel* model = modelOf(asset.read(members), &options);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        PrRenderLightDesc light = prRenderLightDescDefault();
        light.direction = {-0.5f, -0.3f, -1};
        light.intensity = 3;
        prRenderLightAdd(update, &light);
        prRenderUpdateCommit(update);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        prModelPlacementCreate(model, &desc);
        images.push(f.view(frontView(scene)));
        devices.push(device);
        // The quad is lit, and its texels show.
        const float* lit = images.last().at(kCenter / 2, kCenter / 2);
        CHECK(lit[0] > 0.1f);
        const float* blue = images.last().at(kCenter + kCenter / 2, kCenter / 2);
        CHECK(blue[2] > blue[0] * 2);
        prModelDestroy(model);
    }
    for (uint32_t i = 1; i < images.count(); ++i) {
        if (devices[i].backend != devices[i - 1].backend)
            continue;
        PrStringView backend = prBackendName(prGpuSelectInterface(), devices[i].backend);
        CAPTURE(backend);
        Difference difference = compare(images[i - 1], images[i], 0);
        CHECK(difference.over == 0);
    }
}

// The last case of the binary's last file, in the order in which the cases run.
TEST_CASE("a placement draws the materials of its variant, and hides what an animation hides") {
    onEachDevice([](Fixture& f) {
        // The red quad, which the variant "green" draws green, and which an animation hides.
        ModelAsset asset;
        String members = R"("extensionsUsed":["KHR_materials_unlit","KHR_materials_variants"],"scene":0,)";
        members.append(R"("scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)");
        members.append(R"("extensions":{"KHR_materials_variants":{"variants":[{"name":"green"}]}},)");
        members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5},"indices":1,"material":0,)");
        members.append(R"("extensions":{"KHR_materials_variants":{"mappings":[{"material":1,"variants":[0]}]}}}]}],)");
        members.append(R"("materials":[)").append(kModelUnlitRed);
        members.append(R"(,{"pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1]},)");
        members.append(R"("extensions":{"KHR_materials_unlit":{}}}])");
        PrModel* model = modelOf(asset.read(members), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        Image image;
        CHECK(near(center(f, scene, &image), kRed, 0));
        prModelPlacementSetVariant(placement, 0);
        CHECK(near(center(f, scene, &image), kRed, 0));
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), PrVec3{0, 1, 0}, 0));
        // A second placement draws its own materials.
        PrModelPlacement* second = prModelPlacementCreate(model, &desc);
        prModelPlacementSetVariant(placement, kPrModelVariantNone);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), kRed, 0));
        prModelPlacementDestroy(second);
        prModelDestroy(model);

        // A hidden node draws nothing.
        ModelAsset hidden;
        PrModel* hiddenModel = modelOf(hidden.read(modelQuad(kModelUnlitRed,
            R"("nodes":[{"mesh":0,"extensions":{"KHR_node_visibility":{"visible":false}}}],)")), nullptr);
        prModelPopulate(hiddenModel, f.render);
        prModelWait(hiddenModel);
        PrModelPlacement* unseen = prModelPlacementCreate(hiddenModel, &desc);
        CHECK(near(center(f, scene, &image), kSky, 0));
        prModelPlacementDestroy(unseen);
        prModelDestroy(hiddenModel);
    });
}

TEST_CASE("a placement draws the meshes of its highlighted node, and of the nodes under it, with the highlight") {
    onEachDevice([](Fixture& f) {
        // The red quad, on a node under the root node 0.
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(modelQuad(kModelUnlitRed,
            R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"children":[1]},{"mesh":0}],)")), nullptr);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        PrRenderMaterial blue = prRenderMaterialCreate(f.render);
        PrRenderUnlitMaterial unlit = prRenderUnlitMaterialDefault();
        unlit.color = PrColor{0, 0, 1, 1};
        prRenderMaterialSetUnlit(f.render, blue, &unlit);
        Image image;
        CHECK(near(center(f, scene, &image), kRed, 0));
        // The root's highlight reaches the mesh of the node under it.
        prModelPlacementSetHighlight(placement, 0, blue);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), PrVec3{0, 0, 1}, 0));
        // A placement that enters its scene again, as after the model populates a renderer, keeps its highlight.
        prModelPlacementSetScene(placement, PrRenderScene{});
        prModelPlacementSetScene(placement, scene);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), PrVec3{0, 0, 1}, 0));
        prModelPlacementSetHighlight(placement, kPrModelNodeNone, PrRenderMaterial{});
        prModelUpdate(model);
        CHECK(near(center(f, scene, &image), kRed, 0));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderMaterialDestroy(f.render, blue);
    });
}

TEST_CASE("an animation of a material's factor changes it for the placement that plays it alone") {
    onEachDevice([](Fixture& f) {
        // The red quad, whose base color an animation takes through a pointer from black, at its start, to a blue of
        // 0.7071 at its end.
        ModelAsset asset;
        PrModel* model = modelOf(asset.read(modelQuad(kModelUnlitRed,
            R"("nodes":[{"mesh":0}],"animations":[{"samplers":[{"input":3,"output":12}],)"
            R"("channels":[{"sampler":0,"target":{"path":"pointer","extensions":{"KHR_animation_pointer":)"
            R"({"pointer":"/materials/0/pbrMetallicRoughness/baseColorFactor"}}}}]}],)")), nullptr);
        CHECK(prModelDiagnosticCount(model) == 0);
        prModelPopulate(model, f.render);
        prModelWait(model);
        PrRenderScene played = createScene(f);
        PrRenderScene resting = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(played);
        PrModelPlacement* player = prModelPlacementCreate(model, &desc);
        desc.scene = resting;
        PrModelPlacement* rester = prModelPlacementCreate(model, &desc);
        prModelPlacementSetAnimation(player, 0, PrModelLoop_Once);
        prModelPlacementSetTime(player, 1);
        prModelUpdate(model);
        const PrVec3 blue{0, 0, 0.70710677f};
        Image image;
        CHECK(near(center(f, played, &image), blue, 1e-3f));
        CHECK(near(center(f, resting, &image), kRed, 0));
        // Halfway, the factor is halfway.
        prModelPlacementSetTime(player, 0.5f);
        prModelUpdate(model);
        CHECK(near(center(f, played, &image), PrVec3{0, 0, 0.35355339f}, 1e-3f));
        // The model's material is the document's, at rest.
        CHECK(prModelMaterial(model, 0).id != 0);
        // At rest again, the placement draws the document's material.
        prModelPlacementSetAnimation(player, kPrModelAnimationNone, PrModelLoop_Once);
        prModelUpdate(model);
        CHECK(near(center(f, played, &image), kRed, 0));
        // A placement that leaves its scene frees its material, and makes another when it plays again in a scene.
        prModelPlacementSetScene(player, resting);
        prModelPlacementSetAnimation(player, 0, PrModelLoop_Once);
        prModelPlacementSetTime(player, 1);
        prModelPlacementDestroy(rester);
        prModelUpdate(model);
        CHECK(near(center(f, resting, &image), blue, 1e-3f));
        prModelPlacementDestroy(player);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, played);
        prRenderSceneDestroy(f.render, resting);
    });
}

TEST_CASE("a texture of UASTC is transcoded to blocks that draw as its pixels do, whatever transfer its file states") {
    ModelCodecs codecs;
    // Texels of 8 to a side in four blocks of one color each, red, green, blue and white, and a second level of gray.
    const uint8_t kColors[4][4] = {{220, 30, 30, 255}, {30, 200, 40, 255}, {40, 50, 210, 255}, {240, 240, 240, 255}};
    uint8_t blocks[5 * 16];
    for (uint32_t i = 0; i < 4; ++i)
        modelUastcSolid(kColors[i], blocks + i * 16);
    const uint8_t kGray[4] = {128, 128, 128, 255};
    modelUastcSolid(kGray, blocks + 4 * 16);
    uint8_t pixels[8 * 8 * 4];
    for (uint32_t y = 0; y < 8; ++y) {
        for (uint32_t x = 0; x < 8; ++x)
            prMemCopy(pixels + (y * 8 + x) * 4, kColors[y / 4 * 2 + x / 4], 4);
    }
    const uint32_t sizes[2] = {64, 16};
    Array<ModelFile> ktx;
    ktx.push(ModelFile{"color.ktx2", modelKtx(0, PrUVec2{8, 8}, 2, blocks, sizes, true)});
    Array<ModelFile> png;
    png.push(ModelFile{"color.png", codecs.png({8, 8}, pixels)});
    Array<ModelFile> linear;
    linear.push(ModelFile{"color.ktx2", modelKtx(0, PrUVec2{8, 8}, 2, blocks, sizes, false)});
    onEachDevice([&](Fixture& f) {
        PrModelResourceInfo info{};
        Image transcoded = drawAsset(f, codecs, ktx, texturedQuadOf("color.ktx2", true), &info);
        Image decoded = drawAsset(f, codecs, png, texturedQuadOf("color.png", false));
        // The file states sRGB values, its source is of the Srgb form, and the material samples it as sRGB.
        CHECK_FALSE(info.colorMismatch);
        const uint32_t kQuarters[2] = {kCenter / 2, kCenter + kCenter / 2};
        for (uint32_t y : kQuarters) {
            for (uint32_t x : kQuarters)
                CHECK(nearTexel(transcoded.at(x, y), decoded.at(x, y), 2e-3f));
        }
        // A file that states linear values is of the plain form, which a material samples as sRGB through the Srgb
        // form.
        Image relabeled = drawAsset(f, codecs, linear, texturedQuadOf("color.ktx2", true), &info);
        CHECK_FALSE(info.colorMismatch);
        CHECK(nearTexel(relabeled.at(kQuarters[0], kQuarters[0]), transcoded.at(kQuarters[0], kQuarters[0]), 2e-3f));
    });
}

TEST_CASE("a texture of one value in BC4 or EAC R11 draws as the gray of its pixels, on a device of either kind") {
    ModelCodecs codecs;
    // Texels of 8 to a side of one value, a normal tilted from the light where it reads as gray and further where it
    // reads in red alone, and a second level of the same.
    const uint8_t kValue = 200;
    const uint8_t bc4[8] = {kValue, kValue, 0, 0, 0, 0, 0, 0};
    const uint8_t eac[8] = {kValue, 0, 0, 0, 0, 0, 0, 0};
    uint8_t pixels[8 * 8 * 4];
    uint8_t red[8 * 8 * 4];
    for (uint32_t i = 0; i < 64; ++i) {
        const uint8_t gray[4] = {kValue, kValue, kValue, 255};
        const uint8_t alone[4] = {kValue, 0, 0, 255};
        prMemCopy(pixels + i * 4, gray, 4);
        prMemCopy(red + i * 4, alone, 4);
    }
    Array<ModelFile> png;
    png.push(ModelFile{"normal.png", codecs.png({8, 8}, pixels)});
    Array<ModelFile> redPng;
    redPng.push(ModelFile{"normal.png", codecs.png({8, 8}, red)});
    struct Case {
        uint32_t vkFormat;
        const uint8_t* block;
    };
    const Case kCases[] = {{139, bc4}, {153, eac}};
    onEachDevice([&](Fixture& f) {
        Image gray = drawAsset(f, codecs, png, normalQuadOf("normal.png"));
        Image alone = drawAsset(f, codecs, redPng, normalQuadOf("normal.png"));
        // The two normals light the quad differently.
        CHECK_FALSE(nearTexel(gray.at(kCenter, kCenter), alone.at(kCenter, kCenter), 1e-2f));
        for (const Case& c : kCases) {
            CAPTURE(c.vkFormat);
            uint8_t blocks[5 * 8];
            for (uint32_t i = 0; i < 5; ++i)
                prMemCopy(blocks + i * 8, c.block, 8);
            const uint32_t sizes[2] = {32, 8};
            Array<ModelFile> ktx;
            ktx.push(ModelFile{"normal.ktx2", modelKtx(c.vkFormat, PrUVec2{8, 8}, 2, blocks, sizes)});
            Image drawn = drawAsset(f, codecs, ktx, normalQuadOf("normal.ktx2"));
            CHECK(nearTexel(drawn.at(kCenter, kCenter), gray.at(kCenter, kCenter), 1e-2f));
        }
    });
}

TEST_CASE("a model opened from bytes that arrive late is empty, then fills in, and draws once it has") {
    onEachDevice([](Fixture& f) {
        ModelAsset asset;
        String text = assetText(modelQuad(kModelUnlitRed, R"("nodes":[{"mesh":0}],)"));
        PrStringView json = text;
        LateBytes bytes(PrByteView{reinterpret_cast<const uint8_t*>(json.data), json.size});
        // The buffer that the asset names is read beside it, through the resolver.
        PrModelResolver resolver{
            .resolve = [](void* PR_NULLABLE context, PrStringView location) -> const PrBlobSource* PR_NULLABLE {
                return harnessBlobSource(ModelAsset::load(context, PrGltfExternal_Buffer, location));
            },
            .context = &asset,
        };
        const PrModelFormat formats[] = {prGltfModelFormat()};
        PrModel* model = prModelOpen(bytes.source, "quad.gltf", &resolver, formats, 1, nullptr);
        prModelPopulate(model, f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        uint64_t revision = prModelRevision(model);
        // Until the source is known, the model is empty and pending, and its placement draws nothing.
        prModelUpdate(model);
        CHECK(prModelProgress(model).source == PrModelLoad_Pending);
        CHECK(prModelNodeCount(model) == 0);
        CHECK(prModelSource(model) == nullptr);
        CHECK(!prModelIsLoaded(model));
        PrBox3 box{};
        CHECK(!prModelPlacementBounds(placement, &box));
        Image image;
        CHECK(near(center(f, scene, &image), kSky, 1e-3f));
        // The classification reads the first bytes, and the format reads them whole: each read completes late.
        CHECK(bytes.poll() == 1);
        CHECK(bytes.poll() == 1);
        prModelUpdate(model);
        CHECK(prModelProgress(model).source == PrModelLoad_Ready);
        CHECK(prModelRevision(model) != revision);
        CHECK(prModelNodeCount(model) == 1);
        prModelWait(model);
        CHECK(prModelIsLoaded(model));
        CHECK(near(center(f, scene, &image), kRed, 1e-3f));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a model whose bytes no format reads fails as NotSupported, and stays empty") {
    PrStringView text = "a note, which is no model";
    PrBlob* blob = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    PrBlobSource* bytes = prBlobSourceCreateBlob(blob);
    prBlobRelease(blob);
    const PrModelFormat formats[] = {prGltfModelFormat()};
    PrModel* model = prModelOpen(bytes, "note.txt", prModelHostFileResolver(), formats, 1, nullptr);
    prBlobSourceRelease(bytes);
    prModelWait(model);
    PrModelProgress progress = prModelProgress(model);
    CHECK(progress.source == PrModelLoad_Failed);
    CHECK(progress.sourceStatus == PrStatus_NotSupported);
    CHECK(prModelNodeCount(model) == 0);
    CHECK(prModelIsLoaded(model));
    prModelDestroy(model);
}

TEST_CASE("a texture whose bytes arrive late opens by the format that they show, and loads again after a loss") {
    ModelCodecs codecs;
    onEachDevice([&codecs](Fixture& f) {
        ModelAsset asset;
        const uint8_t green[4] = {0, 255, 0, 255};
        Array<uint8_t> png = codecs.png({1, 1}, green);
        LateBytes image(PrByteView{png.begin(), png.count()});
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        PrModel* model = prModelCreate(LateImageSource::create(asset.read(texturedQuad()), &image), &options);
        prModelPopulate(model, f.render);
        PrRenderScene scene = createScene(f);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        // The mesh loads, and the image waits for its bytes, while the frame draws the texture's stand-in.
        prModelUpdate(model);
        prModelUpdate(model);
        CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_Ready);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Pending);
        Image shown;
        CHECK(near(center(f, scene, &shown), kWhite, 1e-3f));
        // The opener reads the bytes and recognizes them, and the texture's reader reads them again.
        CHECK(image.poll() == 1);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Pending);
        CHECK(image.poll() == 1);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Ready);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &shown), PrVec3{0, 1, 0}, 1e-3f));
        uint32_t reads = image.reads;
        // A renderer that the model populates again, as after a lost device, reads the image again.
        prModelPopulate(model, f.render);
        prModelPlacementSetScene(placement, scene);
        prModelUpdate(model);
        prModelUpdate(model);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Pending);
        image.poll();
        image.poll();
        CHECK(image.reads == reads + 2);
        CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Ready);
        prModelUpdate(model);
        CHECK(near(center(f, scene, &shown), PrVec3{0, 1, 0}, 1e-3f));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a host's policy makes the chain of each use of an image, and makes them again for a new renderer") {
    ModelCodecs codecs;
    // A policy that records each use that it is asked for and the format of the default policy's chain.
    struct Recorder {
        Array<PrModelTextureUse> uses;
        Array<PrGpuFormat> formats;

        static PrStatus chain(void* PR_NULLABLE context, const PrTextureSource* source, const PrModelTextureUse* use,
                const PrGpuDevice* PR_NULLABLE device, PrModelTextureChain* out) {
            Recorder* recorder = static_cast<Recorder*>(context);
            PrStatus status = prModelTextureChainDefault(nullptr, source, use, device, out);
            recorder->uses.push(*use);
            recorder->formats.push(status == PrStatus_Ok ? prTextureSourceDesc(out->source)->format : PrGpuFormat_None);
            return status;
        }
    };
    onEachDevice([&codecs](Fixture& f) {
        // One image that the material reads as its base color and as its occlusion, in red alone.
        uint8_t texels[4 * 4 * 4];
        for (uint32_t i = 0; i < sizeof(texels); ++i)
            texels[i] = uint8_t(i * 5);
        ModelAsset asset;
        asset.files.push(ModelFile{"color.png", codecs.png({4, 4}, texels)});
        String members = modelQuad(
            R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"occlusionTexture":{"index":0}})",
            R"("nodes":[{"mesh":0}],"images":[{"uri":"color.png"}],"textures":[{"source":0}],)");
        Recorder recorder;
        PrModelOptions options = prModelOptionsDefault();
        options.textures = codecs.textures;
        options.chain = Recorder::chain;
        options.chainContext = &recorder;
        PrModel* model = modelOf(asset.read(members), &options);
        for (uint32_t round = 0; round < 2; ++round) {
            // A renderer that the model populates again, as after a lost device, makes the chains again.
            prModelPopulate(model, f.render);
            prModelWait(model);
            prModelUpdate(model);
            CHECK(prModelImageInfo(model, 0).load == PrModelLoad_Ready);
            REQUIRE(recorder.uses.count() == 2 * (round + 1));
            for (uint32_t i = round * 2; i < round * 2 + 2; ++i) {
                const PrModelTextureUse& use = recorder.uses[i];
                if (use.kind == PrRenderTextureKind_Color) {
                    CHECK(use.color == PrModelTextureColor_Srgb);
                    CHECK(use.channels == PrRenderChannelFlag_Rgba);
                    CHECK(recorder.formats[i] == PrGpuFormat_Rgba8Srgb);
                } else {
                    // The occlusion reads red alone, which is all that its texture holds.
                    CHECK(use.color == PrModelTextureColor_Data);
                    CHECK(use.channels == PrRenderChannelFlag_R);
                    CHECK(recorder.formats[i] == PrGpuFormat_R8);
                }
            }
            // The color's levels of four bytes a texel and the occlusion's of one, each made down to one texel.
            CHECK(prModelProgress(model).textureBytes == (16 + 4 + 1) * 4 + (16 + 4 + 1));
        }
        prModelDestroy(model);
    });
}

TEST_CASE("a material's values set the parameters of its type by their names, an extension's among them") {
    onEachDevice([](Fixture& f) {
        // A black dielectric that emits its factor times KHR_materials_emissive_strength's strength, in the view's
        // unit, which the type's parameters emissive and emissiveStrength hold.
        ModelAsset asset;
        String members = modelQuad(
            R"({"pbrMetallicRoughness":{"baseColorFactor":[0,0,0,1],"metallicFactor":0},)"
            R"("emissiveFactor":[0.25,0.125,0.5],"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":2}}})",
            R"("nodes":[{"mesh":0}],)");
        PrModelOptions options = prModelOptionsDefault();
        options.emission = PrRenderEmission_Display;
        PrModel* model = modelOf(asset.read(members), &options);
        prModelPopulate(model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(model, &desc);
        prModelWait(model);
        prModelUpdate(model);
        // The strength alone needs no layer, so the material draws as glTF's core type.
        CHECK(!prModelHasDiagnostic(model, "layers"));
        Image shown;
        CHECK(near(center(f, scene, &shown), PrVec3{0.5f, 0.25f, 1}, 2e-2f));
        prModelPlacementDestroy(placement);
        prModelDestroy(model);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("the devices that the tests share are destroyed") {
    destroyTestDevices();
    CHECK(testDevices().count() == 0);
}

TEST_SUITE_END();
