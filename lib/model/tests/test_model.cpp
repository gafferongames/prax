// What a model computes from its document alone, without a renderer: the hierarchy, animations, bounds, cameras and
// lights of a placement, and what a model reports that it does not draw.

#include "pr/float.h"
#include "pr/geom.h"
#include "pr/model.h"

#include "model/tests/model_fixture.h"

#include "tools/harness/test.h"

extern "C" int prTestModelCApi(void);

using namespace harness;

namespace pr_impl {

namespace {

constexpr float kHalfRoot = 0.70710678f;

// Returns where a matrix maps the origin.
PrVec3 originOf(const PrMatrix4& matrix) {
    return PrVec3{matrix.m[0][3], matrix.m[1][3], matrix.m[2][3]};
}

bool isNear(PrVec3 a, PrVec3 b, float tolerance = 1e-5f) {
    return prF32Abs(a.x - b.x) <= tolerance && prF32Abs(a.y - b.y) <= tolerance && prF32Abs(a.z - b.z) <= tolerance;
}

// Returns the ray from (x, y, 10) straight down the z axis, toward the quads, which lie in planes of constant z.
PrMeshRay down(float x, float y) {
    return PrMeshRay{.origin = {x, y, 10}, .tMin = 0, .direction = {0, 0, -1}, .tMax = 100};
}

// A model of a document and one placement of it in no scene.
struct Placed {
    PrModel* model;
    PrModelPlacement* placement;

    explicit Placed(const PrGltf* gltf) {
        model = modelOf(gltf, nullptr);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
        placement = prModelPlacementCreate(model, &desc);
    }
    Placed(const Placed&) = delete;
    Placed& operator=(const Placed&) = delete;
    ~Placed() { prModelDestroy(model); }
};

// The members of an asset with node 0 above node 1, which draws the quad, and an animation that moves node 1 from the
// origin to (4, 0, 0) in one second.
constexpr PrStringView kAnimatedNodes =
    R"("nodes":[{"children":[1],"translation":[1,2,3],"scale":[2,2,2]},{"mesh":0,"translation":[1,0,0]}],)"
    R"("animations":[{"samplers":[{"input":3,"output":4}],)"
    R"("channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}],)";

// The members of an asset whose node 0 draws the quad with a skin of one joint, node 1, which moves every vertex and
// which an animation moves from the origin to (4, 0, 0) in one second. Node 0 is moved aside, which a skinned mesh
// ignores. `skin` is further members of the skin, each after a comma.
String skinnedQuad(PrStringView skin) {
    String members = R"("scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0,"skin":0,"translation":[0,7,0]},{}],)";
    members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5,"JOINTS_0":13,"WEIGHTS_0":11},)");
    members.append(R"("indices":1}]}],"skins":[{"joints":[1])").append(skin).append("}],");
    members.append(R"("animations":[{"samplers":[{"input":3,"output":4}],)");
    members.append(R"("channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}])");
    return members;
}

// The members of an asset whose node draws the quad with one morph target, which adds each vertex's own position to it,
// so that a weight of 1 doubles the quad. The document states that the target reaches 1 along z as well. The mesh has a
// weight of one half, and an animation takes the weight from 0 to 1 in one second. `node` is further members of the
// node, each after a comma.
String morphedQuad(PrStringView node) {
    String members = R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0)";
    members.append(node).append(R"(}],"meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5},"indices":1,)");
    members.append(R"("targets":[{"POSITION":10}]}],"weights":[0.5]}],)");
    members.append(R"("animations":[{"samplers":[{"input":3,"output":3}],)");
    members.append(R"("channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}])");
    return members;
}

// The members of an asset whose node 0 draws the quad with the morph target of morphedQuad, at the mesh's weight of 1,
// and the skin of skinnedQuad, whose joint, node 1, an animation moves from the origin to (4, 0, 0) in one second.
String morphedSkinnedQuad() {
    String members = R"("scene":0,"scenes":[{"nodes":[0,1]}],"nodes":[{"mesh":0,"skin":0},{}],)";
    members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5,"JOINTS_0":13,"WEIGHTS_0":11},)");
    members.append(R"("indices":1,"targets":[{"POSITION":10}]}],"weights":[1]}],"skins":[{"joints":[1]}],)");
    members.append(R"("animations":[{"samplers":[{"input":3,"output":4}],)");
    members.append(R"("channels":[{"sampler":0,"target":{"node":1,"path":"translation"}}]}])");
    return members;
}

// A document of Khronos's sample assets (cmake/test_data.cmake), with what it names beside it.
struct SampleAsset {
    PrGltf* PR_NULLABLE gltf = nullptr;

    static PrBlob* PR_NULLABLE load(void* PR_NULLABLE, PrGltfExternal, PrStringView uri) {
        Array<uint8_t> bytes = readFile(format("{}/{}", PR_GLTF_DIR, uri));
        return prBlobCopy(PrByteView{bytes.begin(), bytes.count()});
    }

    explicit SampleAsset(PrStringView name) {
        Array<uint8_t> bytes = readFile(format("{}/{}", PR_GLTF_DIR, name));
        PrBlob* blob = prBlobCopy(PrByteView{bytes.begin(), bytes.count()});
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = load;
        REQUIRE(prGltfCreate(blob, &options, &gltf) == PrStatus_Ok);
        prBlobRelease(blob);
    }
    SampleAsset(const SampleAsset&) = delete;
    SampleAsset& operator=(const SampleAsset&) = delete;
    ~SampleAsset() { prGltfDestroy(gltf); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_SUITE_BEGIN("model");

TEST_CASE("the public header compiles as C") {
    CHECK(prTestModelCApi() == 0);
}

TEST_CASE("a placement maps each node to the world through its ancestors and its root") {
    ModelAsset asset;
    // Node 2 has a matrix, which turns x to y and moves by (0, 0, 5). Node 3 is outside the scene.
    Placed placed(asset.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"children":[1,2],"translation":[1,2,3],"scale":[2,2,2]},{"mesh":0,"translation":[1,0,0]},)"
        R"({"matrix":[0,1,0,0,-1,0,0,0,0,0,1,0,0,0,5,1]},{"translation":[9,9,9]}],)")));
    CHECK(isNear(originOf(prModelPlacementNodeWorld(placed.placement, 0)), {1, 2, 3}));
    // The child's translation is scaled by its parent.
    CHECK(isNear(originOf(prModelPlacementNodeWorld(placed.placement, 1)), {3, 2, 3}));
    PrMatrix4 turned = prModelPlacementNodeWorld(placed.placement, 2);
    CHECK(isNear(originOf(turned), {1, 2, 13}));
    CHECK(isNear({turned.m[0][0], turned.m[1][0], turned.m[2][0]}, {0, 2, 0}));

    PrMatrix4 root = prMatrix4Translate({10, 0, 0});
    prModelPlacementSetRoot(placed.placement, &root);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(placed.placement, 1)), {13, 2, 3}));
    // A node outside the scene is at the root.
    CHECK(isNear(originOf(prModelPlacementNodeWorld(placed.placement, 3)), {10, 0, 0}));
}

TEST_CASE("a model draws the scene that its options name, or the document's default") {
    ModelAsset asset;
    const PrGltf* gltf = asset.read(
        R"("scene":1,"scenes":[{"nodes":[0]},{"nodes":[1]}],"nodes":[{"camera":0},{"camera":0,"children":[2]},)"
        R"({"camera":0}],"cameras":[{"type":"perspective","perspective":{"yfov":1,"znear":0.1}}])");
    PrModel* byDefault = modelOf(gltf, nullptr);
    REQUIRE(prModelCameraCount(byDefault) == 2);
    CHECK(prModelCameraNode(byDefault, 0) == 1);
    CHECK(prModelCameraNode(byDefault, 1) == 2);
    prModelDestroy(byDefault);
    PrModel* named = modelOf(gltf, nullptr, 0);
    REQUIRE(prModelCameraCount(named) == 1);
    CHECK(prModelCameraNode(named, 0) == 0);
    prModelDestroy(named);
}

TEST_CASE("an animation moves the nodes that it has channels for, at the placement's time") {
    ModelAsset asset;
    Placed placed(asset.read(modelQuad(kModelUnlitRed, kAnimatedNodes)));
    PrModelPlacement* p = placed.placement;
    // At rest the node is where the document puts it.
    CHECK(prModelPlacementAnimation(p) == kPrModelAnimationNone);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {3, 2, 3}));

    // The animation's translation replaces the node's own, under the parent's scale.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    CHECK(prModelPlacementAnimation(p) == 0);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {1, 2, 3}));
    prModelPlacementSetTime(p, 0.5f);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {5, 2, 3}));
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 0)), {1, 2, 3}));

    // Played once, the animation holds its last pose.
    prModelPlacementAdvance(p, 0.75f);
    CHECK(prModelPlacementTime(p) == 1);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {9, 2, 3}));

    // Repeated, it starts again.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Repeat);
    CHECK(prModelPlacementTime(p) == 0);
    prModelPlacementAdvance(p, 0.75f);
    prModelPlacementAdvance(p, 0.75f);
    CHECK(prModelPlacementTime(p) == doctest::Approx(0.5f));
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {5, 2, 3}));
    prModelPlacementAdvance(p, -0.75f);
    CHECK(prModelPlacementTime(p) == doctest::Approx(0.75f));

    // No animation is the rest pose again.
    prModelPlacementSetAnimation(p, kPrModelAnimationNone, PrModelLoop_Once);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {3, 2, 3}));
}

TEST_CASE("a blend mixes two animations by its weight") {
    ModelAsset asset;
    // Animation 0 moves node 0 to (4, 0, 0), and animation 1 turns it a quarter turn about z. Node 1 is 1 along its x.
    Placed placed(asset.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"mesh":0,"children":[1]},{"translation":[1,0,0]}],)"
        R"("animations":[{"samplers":[{"input":3,"output":4}],)"
        R"("channels":[{"sampler":0,"target":{"node":0,"path":"translation"}}]},)"
        R"({"samplers":[{"input":3,"output":12}],"channels":[{"sampler":0,"target":{"node":0,"path":"rotation"}}]}],)")));
    PrModelPlacement* p = placed.placement;
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    prModelPlacementSetTime(p, 1);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {5, 0, 0}));
    // Half of each: half of the first's translation, since the second leaves the node at the origin, and half of the
    // second's quarter turn, since the first leaves the node unturned.
    prModelPlacementSetBlend(p, 1, 1, 0.5f);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 0)), {2, 0, 0}));
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {2 + kHalfRoot, kHalfRoot, 0}));
    // The blend's time is its own.
    prModelPlacementSetBlend(p, 1, 0, 0.5f);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {3, 0, 0}));
    prModelPlacementSetBlend(p, 1, 1, 1);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {0, 1, 0}));
    prModelPlacementSetBlend(p, kPrModelAnimationNone, 0, 0);
    CHECK(isNear(originOf(prModelPlacementNodeWorld(p, 1)), {5, 0, 0}));
}

TEST_CASE("a placement's bounds are known from the document, before anything loads") {
    ModelAsset asset;
    Placed placed(asset.read(modelQuad(kModelUnlitRed, kAnimatedNodes)));
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placed.placement, &box));
    // The quad reaches 1 from its node, which its parent scales by 2 and puts at (3, 2, 3).
    CHECK(isNear(box.min, {1, 0, 3}));
    CHECK(isNear(box.max, {5, 4, 3}));
    prModelPlacementSetAnimation(placed.placement, 0, PrModelLoop_Once);
    prModelPlacementSetTime(placed.placement, 1);
    REQUIRE(prModelPlacementBounds(placed.placement, &box));
    CHECK(isNear(box.min, {7, 0, 3}));

    // A model that draws no mesh has no bounds.
    ModelAsset empty;
    Placed unbounded(empty.read(R"("nodes":[{}])"));
    CHECK_FALSE(prModelPlacementBounds(unbounded.placement, &box));
    CHECK(isNear(box.min, {0, 0, 0}));
    CHECK(isNear(box.max, {0, 0, 0}));
}

TEST_CASE("a skinned mesh is where its joints put it, whatever its own node says") {
    ModelAsset asset;
    Placed placed(asset.read(skinnedQuad("")));
    PrModelPlacement* p = placed.placement;
    CHECK(prModelDiagnosticCount(placed.model) == 0);
    PrBox3 box{};
    // At rest the joint is at the origin, and the quad is as it was bound: its node's translation moves nothing.
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.min, {-1, -1, 0}));
    CHECK(isNear(box.max, {1, 1, 0}));
    // The joint's animation moves the quad, and the placement's root moves it with everything else.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    prModelPlacementSetTime(p, 0.5f);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.min, {1, -1, 0}));
    CHECK(isNear(box.max, {3, 1, 0}));
    PrMatrix4 root = prMatrix4Translate({0, 0, 5});
    prModelPlacementSetRoot(p, &root);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.min, {1, -1, 5}));

    // An inverse bind matrix moves the mesh into its joint's space first: here, one aside along x.
    ModelAsset bound;
    Placed aside(bound.read(skinnedQuad(R"(,"inverseBindMatrices":16)")));
    REQUIRE(prModelPlacementBounds(aside.placement, &box));
    CHECK(isNear(box.min, {0, -1, 0}));
    CHECK(isNear(box.max, {2, 1, 0}));
}

TEST_CASE("a mesh's morph weights are its node's, or else its mesh's, until an animation gives them") {
    ModelAsset asset;
    Placed placed(asset.read(morphedQuad("")));
    PrModelPlacement* p = placed.placement;
    CHECK(prModelDiagnosticCount(placed.model) == 0);
    PrBox3 box{};
    // The mesh's weight of one half makes the quad half as large again. The bounds are the document's, which says that
    // the target reaches along z as well.
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.min, {-1.5f, -1.5f, 0}));
    CHECK(isNear(box.max, {1.5f, 1.5f, 0.5f}));
    // The animation's weight replaces it.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.max, {1, 1, 0}));
    prModelPlacementSetTime(p, 0.25f);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.min, {-1.25f, -1.25f, 0}));
    CHECK(isNear(box.max, {1.25f, 1.25f, 0.25f}));
    // A blend mixes the weights of its two animations.
    prModelPlacementSetBlend(p, 0, 1, 0.5f);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.max, {1.625f, 1.625f, 0.625f}));

    // A node's own weights come before its mesh's.
    ModelAsset own;
    Placed weighted(own.read(morphedQuad(R"(,"weights":[1])")));
    REQUIRE(prModelPlacementBounds(weighted.placement, &box));
    CHECK(isNear(box.min, {-2, -2, 0}));
    CHECK(isNear(box.max, {2, 2, 1}));
}

TEST_CASE("a placement's cameras and lights follow their nodes") {
    ModelAsset asset;
    // Node 0 carries a camera and a spot light, 5 along z and turned a quarter turn about y, so that it looks along -x.
    // Its parent scales by 3, which the camera's view leaves out.
    Placed placed(asset.read(
        R"("extensionsUsed":["KHR_lights_punctual"],"scene":0,"scenes":[{"nodes":[1]}],)"
        R"("nodes":[{"camera":0,"translation":[0,0,5],"rotation":[0,0.70710678,0,0.70710678],)"
        R"("extensions":{"KHR_lights_punctual":{"light":0}}},{"children":[0,2],"scale":[3,3,3]},{"camera":1}],)"
        R"("cameras":[{"type":"perspective","perspective":{"yfov":1,"znear":0.25,"aspectRatio":2}},)"
        R"({"type":"orthographic","orthographic":{"xmag":4,"ymag":2,"znear":1,"zfar":11}}],)"
        R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"spot","color":[1,0.5,0.25],"intensity":7,)"
        R"("range":20,"spot":{"innerConeAngle":0.1,"outerConeAngle":0.6}}]}})"));
    REQUIRE(prModelCameraCount(placed.model) == 2);
    REQUIRE(prModelLightCount(placed.model) == 1);
    CHECK(prModelLightNode(placed.model, 0) == 0);

    PrRenderCamera camera = prModelPlacementCamera(placed.placement, 0, 1.5f);
    // The camera stands at (0, 0, 15), and a point 2 further along -x is 2 ahead of it.
    PrVec3 ahead{-2, 0, 15};
    const float (*v)[4] = camera.view.m;
    PrVec3 seen{
        v[0][0] * ahead.x + v[0][1] * ahead.y + v[0][2] * ahead.z + v[0][3],
        v[1][0] * ahead.x + v[1][1] * ahead.y + v[1][2] * ahead.z + v[1][3],
        v[2][0] * ahead.x + v[2][1] * ahead.y + v[2][2] * ahead.z + v[2][3],
    };
    CHECK(isNear(seen, {0, 0, -2}, 1e-4f));
    float f = 1 / prF32Tan(0.5f);
    CHECK(camera.projection.m[1][1] == doctest::Approx(f));
    CHECK(camera.projection.m[0][0] == doctest::Approx(f / 1.5f));
    // Depth is 1 at the near distance.
    CHECK(camera.projection.m[2][3] / 0.25f == doctest::Approx(1));
    // Without an aspect ratio, the camera has the document's.
    CHECK(prModelPlacementCamera(placed.placement, 0, 0).projection.m[0][0] == doctest::Approx(f / 2));

    // The orthographic camera keeps its height, and its depth runs from 1 at the near plane to 0 at the far plane.
    PrRenderCamera flat = prModelPlacementCamera(placed.placement, 1, 3);
    CHECK(flat.projection.m[1][1] == doctest::Approx(0.5f));
    CHECK(flat.projection.m[0][0] == doctest::Approx(1 / 6.0f));
    CHECK(flat.projection.m[2][2] * -1 + flat.projection.m[2][3] == doctest::Approx(1));
    CHECK(flat.projection.m[2][2] * -11 + flat.projection.m[2][3] == doctest::Approx(0).epsilon(1e-6));
    CHECK(flat.projection.m[3][3] == 1);

    PrRenderLightDesc light = prModelPlacementLight(placed.placement, 0);
    CHECK(light.kind == PrRenderLightKind_Spot);
    CHECK(isNear(light.position, {0, 0, 15}));
    CHECK(isNear(light.direction, {-1, 0, 0}));
    CHECK(isNear(light.color, {1, 0.5f, 0.25f}));
    CHECK(light.intensity == 7);
    CHECK(light.range == 20);
    CHECK(light.innerConeAngle == doctest::Approx(0.1f));
    CHECK(light.outerConeAngle == doctest::Approx(0.6f));
    CHECK_FALSE(light.castsShadows);
}

TEST_CASE("a model reports what its document uses that it does not draw") {
    ModelAsset asset;
    // Mesh 0 is points and lines alone, mesh 1 has a morph target, and node 2 draws mesh 2 with a skin.
    const PrGltf* gltf = asset.read(
        R"("nodes":[{"mesh":0},{"mesh":1},{"mesh":2,"skin":0},{}],"skins":[{"joints":[3]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"mode":0},{"attributes":{"POSITION":0},"mode":1}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":10}]}]},)"
        R"({"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":13,"WEIGHTS_0":11},"indices":1}]}])");
    PrModel* model = modelOf(gltf, nullptr);
    const PrStringView both[] = {"points", "lines"};
    CHECK(hasDiagnostics(model, both));
    // A mesh without triangles is not loaded, and the others wait for a renderer.
    CHECK(prModelMeshInfo(model, 0).load == PrModelLoad_None);
    CHECK(prModelMeshInfo(model, 1).load == PrModelLoad_Pending);
    CHECK(prModelMeshInfo(model, 2).load == PrModelLoad_Pending);
    PrModelProgress progress = prModelProgress(model);
    CHECK(progress.meshCount == 2);
    CHECK(progress.imageCount == 0);
    CHECK_FALSE(prModelIsLoaded(model));
    CHECK(prModelMesh(model, 1).id == 0);
    CHECK(prModelRender(model) == nullptr);
    prModelDestroy(model);

    ModelAsset plain;
    PrModel* drawn = modelOf(plain.read(modelQuad(kModelUnlitRed, R"("nodes":[{"mesh":0}],)")), nullptr);
    CHECK(prModelDiagnosticCount(drawn) == 0);
    prModelDestroy(drawn);

    // A texture that reads a third set of texture coordinates reads the first.
    ModelAsset sets;
    PrModel* third = modelOf(sets.read(modelQuad(
        R"({"pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":2}}})",
        R"("nodes":[{"mesh":0}],"textures":[{}],)")), nullptr);
    const PrStringView sets3[] = {"texture-coordinate-sets"};
    CHECK(hasDiagnostics(third, sets3));
    // The texture has no image, so the model loads none.
    CHECK(prModelProgress(third).imageCount == 0);
    prModelDestroy(third);
}

TEST_CASE("a node with instances draws its mesh at each instance's transform within the node") {
    // Two instances of the quad, at the origin and at (4, 0, 0), under a node at (0, 0, 2).
    ModelAsset asset;
    Placed placed(asset.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"mesh":0,"translation":[0,0,2],"extensions":{"EXT_mesh_gpu_instancing":)"
        R"({"attributes":{"TRANSLATION":4}}}}],)")));
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placed.placement, &box));
    CHECK(isNear(box.min, {-1, -1, 2}));
    CHECK(isNear(box.max, {5, 1, 2}));
    CHECK(prModelDiagnosticCount(placed.model) == 0);
}

TEST_CASE("a pick finds the nearest triangle that a ray meets, at each instance and in the skinned pose") {
    ModelAsset instanced;
    Placed placed(instanced.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"mesh":0,"translation":[0,0,2],"extensions":{"EXT_mesh_gpu_instancing":)"
        R"({"attributes":{"TRANSLATION":4}}}}],)")));
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placed.placement, down(4.5f, 0.5f), &pick));
    CHECK(pick.node == 0);
    CHECK(pick.mesh == 0);
    CHECK(pick.part == 0);
    CHECK(pick.instance == 1);
    CHECK(pick.t == doctest::Approx(8.0));
    CHECK(isNear(pick.position, {4.5f, 0.5f, 2}));
    CHECK_FALSE(prModelPlacementPick(placed.placement, down(2, 0), &pick));
    // The placement's root moves what a pick meets.
    PrMatrix4 root = prMatrix4Translate({0, 0, 3});
    prModelPlacementSetRoot(placed.placement, &root);
    REQUIRE(prModelPlacementPick(placed.placement, down(0, 0), &pick));
    CHECK(pick.instance == 0);
    CHECK(pick.t == doctest::Approx(5.0));

    // A skinned quad is met where its joint puts it in the pose, not where its node or its bind pose is.
    ModelAsset skinned;
    Placed posed(skinned.read(skinnedQuad("")));
    REQUIRE(prModelPlacementPick(posed.placement, down(0, 0), &pick));
    prModelPlacementSetAnimation(posed.placement, 0, PrModelLoop_Once);
    prModelPlacementSetTime(posed.placement, 0.5f);
    CHECK_FALSE(prModelPlacementPick(posed.placement, down(0, 0), &pick));
    REQUIRE(prModelPlacementPick(posed.placement, down(2.5f, 0), &pick));
    CHECK(isNear(pick.position, {2.5f, 0, 0}));

    // A hidden node is not met, and the visible sibling behind it is.
    ModelAsset sibling;
    Placed shown(sibling.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"children":[1,2]},{"mesh":0,"translation":[0,0,1],)"
        R"("extensions":{"KHR_node_visibility":{"visible":false}}},{"mesh":0}],)")));
    REQUIRE(prModelPlacementPick(shown.placement, down(0, 0), &pick));
    CHECK(pick.node == 2);
}

TEST_CASE("a pick meets a morphed mesh in the shape that its weights give it, and its skin where both apply") {
    // The mesh's weight of one half makes the quad half as large again.
    ModelAsset asset;
    Placed placed(asset.read(morphedQuad("")));
    PrModelPlacement* p = placed.placement;
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(p, down(1.4f, -1.4f), &pick));
    CHECK(isNear(pick.position, {1.4f, -1.4f, 0}));
    CHECK_FALSE(prModelPlacementPick(p, down(1.6f, 0), &pick));

    // The animation's weight replaces the mesh's, and each pick meets the shape at its own time, as a view that picks
    // while the animation plays does.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    CHECK_FALSE(prModelPlacementPick(p, down(1.1f, 0), &pick));
    for (uint32_t step = 0; step <= 10; ++step) {
        float weight = float(step) / 10;
        prModelPlacementSetTime(p, weight);
        CAPTURE(weight);
        float edge = 1 + weight;
        REQUIRE(prModelPlacementPick(p, down(edge - 0.05f, edge - 0.05f), &pick));
        CHECK(pick.t == doctest::Approx(10.0));
        CHECK_FALSE(prModelPlacementPick(p, down(edge + 0.05f, 0), &pick));
    }

    // A node's own weight comes before its mesh's.
    ModelAsset own;
    Placed weighted(own.read(morphedQuad(R"(,"weights":[1])")));
    CHECK(prModelPlacementPick(weighted.placement, down(1.9f, 1.9f), &pick));

    // The target doubles the quad, and the joint then moves it: at half a second, by (2, 0, 0).
    ModelAsset both;
    Placed posed(both.read(morphedSkinnedQuad()));
    REQUIRE(prModelPlacementPick(posed.placement, down(-1.9f, 0), &pick));
    prModelPlacementSetAnimation(posed.placement, 0, PrModelLoop_Once);
    prModelPlacementSetTime(posed.placement, 0.5f);
    CHECK_FALSE(prModelPlacementPick(posed.placement, down(-0.1f, 0), &pick));
    REQUIRE(prModelPlacementPick(posed.placement, down(3.9f, 1.9f), &pick));
    CHECK(isNear(pick.position, {3.9f, 1.9f, 0}));
    CHECK(pick.node == 0);
}

TEST_CASE("a pick meets Khronos's AnimatedMorphCube where its animation's targets move its faces") {
    // The cube is 2 on a side about the origin. Its node turns the mesh's y, along which the targets move the cube's
    // bottom face toward its top, to -z, so the bottom face is the one that a ray down z meets first, at z = 1.
    SampleAsset asset("AnimatedMorphCube.gltf");
    Placed placed(asset.gltf);
    PrModelPlacement* p = placed.placement;
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(p, down(0.3f, 0.2f), &pick));
    CHECK(pick.mesh == 0);
    CHECK(pick.t == doctest::Approx(9.0).epsilon(1e-4));

    // At 1.6 seconds the first target ("thin") has its whole weight, which raises the face by 1.893 of the cube's 2,
    // and at 0.8 seconds half of it.
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    prModelPlacementSetTime(p, 1.6f);
    REQUIRE(prModelPlacementPick(p, down(0.3f, 0.2f), &pick));
    CHECK(pick.t == doctest::Approx(10.893).epsilon(1e-3));
    prModelPlacementSetTime(p, 0.8f);
    REQUIRE(prModelPlacementPick(p, down(0.3f, 0.2f), &pick));
    CHECK(pick.t == doctest::Approx(9.947).epsilon(1e-3));

    // The second target ("angle") raises only the face's edge at y = 1, so the face slopes, and meets a ray nearer y =
    // 1 later than one nearer y = -1.
    prModelPlacementSetTime(p, 3.4f);
    REQUIRE(prModelPlacementPick(p, down(0, 0.9f), &pick));
    float raised = pick.t;
    REQUIRE(prModelPlacementPick(p, down(0, -0.9f), &pick));
    CHECK(raised > pick.t + 1.5f);
}

TEST_CASE("a hidden node hides itself and its descendants") {
    ModelAsset asset;
    Placed hidden(asset.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"children":[1],"extensions":{"KHR_node_visibility":{"visible":false}}},)"
        R"({"mesh":0,"extensions":{"KHR_node_visibility":{"visible":true}}}],)")));
    PrBox3 box{};
    CHECK_FALSE(prModelPlacementBounds(hidden.placement, &box));

    // A visible sibling still draws.
    ModelAsset sibling;
    Placed shown(sibling.read(modelQuad(kModelUnlitRed,
        R"("nodes":[{"children":[1,2]},{"mesh":0,"extensions":{"KHR_node_visibility":{"visible":false}}},)"
        R"({"mesh":0,"translation":[4,0,0]}],)")));
    REQUIRE(prModelPlacementBounds(shown.placement, &box));
    CHECK(isNear(box.min, {3, -1, 0}));
}

TEST_CASE("pointers animate one weight of a node and the weights of a mesh") {
    // The morphed quad, whose weights the animation sets through a pointer to the first weight of the node.
    ModelAsset asset;
    String members = R"("scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],)";
    members.append(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5},"indices":1,)");
    members.append(R"("targets":[{"POSITION":10}]}],"weights":[0.5]}],"animations":[)");
    members.append(R"({"samplers":[{"input":3,"output":3}],"channels":[{"sampler":0,"target":{"path":"pointer",)");
    members.append(R"("extensions":{"KHR_animation_pointer":{"pointer":"/nodes/0/weights/0"}}}}]},)");
    members.append(R"({"samplers":[{"input":3,"output":3}],"channels":[{"sampler":0,"target":{"path":"pointer",)");
    members.append(R"("extensions":{"KHR_animation_pointer":{"pointer":"/meshes/0/weights"}}}}]}])");
    Placed placed(asset.read(members));
    PrModelPlacement* p = placed.placement;
    PrBox3 box{};
    prModelPlacementSetAnimation(p, 0, PrModelLoop_Once);
    prModelPlacementSetTime(p, 0.25f);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.max, {1.25f, 1.25f, 0.25f}));
    prModelPlacementSetAnimation(p, 1, PrModelLoop_Once);
    prModelPlacementSetTime(p, 1);
    REQUIRE(prModelPlacementBounds(p, &box));
    CHECK(isNear(box.max, {2, 2, 1}));
}

TEST_CASE("a placement draws the materials of the variant that it is set to") {
    ModelAsset asset;
    Placed placed(asset.read(modelQuad(kModelUnlitRed,
        R"("extensions":{"KHR_materials_variants":{"variants":[{"name":"plain"}]}},"nodes":[{"mesh":0}],)")));
    CHECK(prModelPlacementVariant(placed.placement) == kPrModelVariantNone);
    prModelPlacementSetVariant(placed.placement, 0);
    CHECK(prModelPlacementVariant(placed.placement) == 0);
    prModelPlacementSetVariant(placed.placement, kPrModelVariantNone);
    CHECK(prModelPlacementVariant(placed.placement) == kPrModelVariantNone);
}

TEST_CASE("a model names the extensions of the material model that it draws its materials without") {
    // The renderer's glTF material has the layers of every extension that lib/gltf reads, so a model names none.
    ModelAsset asset;
    Placed coated(asset.read(modelQuad(R"({"extensions":{"KHR_materials_clearcoat":{},"KHR_materials_ior":{},)"
        R"("KHR_materials_emissive_strength":{},"KHR_materials_transmission":{},"KHR_materials_sheen":{}}})",
        R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelDiagnosticCount(coated.model) == 0);
    // A material without textures names none that a device does not sample.
    CHECK(prModelUnsampledTextureCount(coated.model) == 0);
    // An unlit material ignores the other extensions.
    ModelAsset unlit;
    Placed flat(unlit.read(modelQuad(R"({"extensions":{"KHR_materials_unlit":{},"KHR_materials_sheen":{}}})",
        R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelDiagnosticCount(flat.model) == 0);
}

TEST_CASE("a model reports its lights, its environment, its emission and whether every surface is unlit") {
    ModelAsset plain;
    Placed lit(plain.read(modelQuad(R"({})", R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelLighting(lit.model) == 0);
    ModelAsset unlit;
    Placed flat(unlit.read(modelQuad(kModelUnlitRed, R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelLighting(flat.model) == PrModelLightingFlag_Unlit);
    // An emissive factor emits, and a strength of zero takes it away.
    ModelAsset emissive;
    Placed glowing(emissive.read(modelQuad(R"({"emissiveFactor":[0,0.5,0]})", R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelLighting(glowing.model) == PrModelLightingFlag_Emission);
    ModelAsset dark;
    Placed none(dark.read(modelQuad(R"({"emissiveFactor":[1,1,1],)"
        R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":0}}})", R"("nodes":[{"mesh":0}],)")));
    CHECK(prModelLighting(none.model) == 0);
    ModelAsset lamp;
    Placed lighted(lamp.read(modelQuad(R"({"emissiveFactor":[1,1,1]})",
        R"("nodes":[{"mesh":0,"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
        R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","intensity":5}]}},)")));
    CHECK(prModelLighting(lighted.model) == (PrModelLightingFlag_Lights | PrModelLightingFlag_Emission));
}

TEST_CASE("a placement's light is the document's times the placement's scale of it") {
    ModelAsset asset;
    Placed placed(asset.read(modelQuad(R"({})",
        R"("nodes":[{"mesh":0,"extensions":{"KHR_lights_punctual":{"light":0}}}],)"
        R"("extensions":{"KHR_lights_punctual":{"lights":[{"type":"point","intensity":5}]}},)")));
    CHECK(prModelPlacementLightScale(placed.placement, 0) == 1);
    CHECK(prModelPlacementLight(placed.placement, 0).intensity == 5);
    prModelPlacementSetLightScale(placed.placement, 0, 0.25f);
    CHECK(prModelPlacementLight(placed.placement, 0).intensity == 1.25f);
}

TEST_SUITE_END();
