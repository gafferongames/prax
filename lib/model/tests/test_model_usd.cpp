// Models of USD stages: what a model reads of a stage's schemas, and what it draws of them.

#include "pr/geom.h"
#include "pr/model.h"
#include "pr/render.h"
#include "pr/render_scene.h"
#include "pr/render_view.h"
#include "pr/string_ops.h"
#include "pr/usd_model.h"
#include "pr/usd_stage.h"

#include "model/src/model.h"
#include "model/tests/model_fixture.h"
#include "render_shaders/TestOpenPbrShaders.h"
#include "tools/harness/harness_gpu.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

using namespace harness;

namespace {

// A stage of one text layer at `/assets/scene.usda`, written as the test's text after the header, whose resources a
// test adds by location, and a model of it.
struct UsdModel {
    String text;
    Array<ModelFile> files;
    PrUsdStage* PR_NULLABLE stage = nullptr;
    PrModel* PR_NULLABLE model = nullptr;
    PrModelTextureChainFn PR_NULLABLE chain = nullptr;     // the model's policy of textures, or the default
    void* PR_NULLABLE chainContext = nullptr;

    static const PrBlobSource* PR_NULLABLE open(void* PR_NULLABLE context, PrStringView location) {
        UsdModel* u = static_cast<UsdModel*>(context);
        PrBlob* blob = nullptr;
        if (location == "/assets/scene.usda")
            blob = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(u->text.data()), u->text.count()});
        for (const ModelFile& file : u->files) {
            if (blob == nullptr && file.uri == location)
                blob = prBlobCopy(PrByteView{file.bytes.begin(), file.bytes.count()});
        }
        if (blob == nullptr)
            return nullptr;
        const PrBlobSource* source = prBlobSourceCreateBlob(blob);
        prBlobRelease(blob);
        return source;
    }

    // Composes the stage with the context's variant selections, and makes the model of its source in the context.
    void open(PrStringView body, const PrTextureOpener* PR_NULLABLE textures = nullptr, const PrUsdModelContext& context = {}) {
        text = String("#usda 1.0\n");
        text.append(body);
        PrUsdStageOptions options = prUsdStageOptionsDefault();
        options.open = open;
        options.openContext = this;
        options.variantSelectionCount = context.variantSelectionCount;
        options.variantSelections = context.variantSelections;
        REQUIRE(prUsdStageCreate("/assets/scene.usda", &options, &stage) == PrStatus_Ok);
        PrModelOptions modelOptions = prModelOptionsDefault();
        modelOptions.textures = textures;
        modelOptions.chain = chain;
        modelOptions.chainContext = chainContext;
        model = prModelCreate(source(context), &modelOptions);
    }

    // Returns a new source of the stage in `context`.
    PrModelSource* source(const PrUsdModelContext& context) {
        PrUsdModelOptions options = prUsdModelOptionsDefault();
        options.context = context;
        options.open = open;
        options.openContext = this;
        return prUsdModelSourceCreate(stage, &options);
    }

    // Returns the canonical text of a new source's description in `context`, with the data of each of its meshes.
    String described(const PrUsdModelContext& context) {
        PrModelSource* made = source(context);
        const PrModelSourceInfo* info = prModelSourceInfo(made);
        PrTextBuffer buffer = {};
        prModelSourceInfoWrite(info, &buffer);
        for (uint32_t i = 0; i < info->meshCount; ++i) {
            PrModelMeshData data{};
            PrModelMeshRequest request{};
            REQUIRE(prModelSourceMeshRead(made, i, &request, &data) == PrStatus_Ok);
            for (uint32_t k = 0; k < data.vertexCount * 3; ++k) {
                uint32_t bits = 0;
                prMemCopy(&bits, &data.positions[k], sizeof(bits));
                prTextBufferAppendChar(&buffer, ' ');
                prTextBufferAppendU64(&buffer, bits, 16);
            }
            for (uint32_t k = 0; k < data.triangleCount * 3; ++k) {
                prTextBufferAppendChar(&buffer, ' ');
                prTextBufferAppendU64(&buffer, data.triangles[k], 10);
            }
            prModelMeshDataReset(&data);
        }
        String result(prTextBufferView(&buffer));
        prTextBufferReset(&buffer);
        prModelSourceDestroy(made);
        return result;
    }

    ~UsdModel() {
        prModelDestroy(model);
        prUsdStageDestroy(stage);
    }

    uint32_t node(PrStringView path) const {
        uint32_t found = prModelNodeFromAddress(model, path);
        REQUIRE(found != kPrModelNodeNone);
        return found;
    }

    String path(uint32_t node) const {
        PrTextBuffer buffer = {};
        prModelNodeAddress(model, node, &buffer);
        String result(prTextBufferView(&buffer));
        prTextBufferReset(&buffer);
        return result;
    }
};

// A quad of side 2 about the origin in the plane z = 0, facing +z, with texture coordinates that show an image upright.
constexpr PrStringView kQuad = " point3f[] points = [(-1, -1, 0), (1, -1, 0), (1, 1, 0), (-1, 1, 0)]\n"
    " int[] faceVertexCounts = [4]\n int[] faceVertexIndices = [0, 1, 2, 3]\n"
    " uniform token subdivisionScheme = \"none\"\n"
    " texCoord2f[] primvars:st = [(0, 0), (1, 0), (1, 1), (0, 1)] (\n"
    " interpolation = \"vertex\"\n )\n";

// Returns true if a layout of the vertices that a model's meshes carry has the preview type's emissive color. A model
// is its state, which the handle derives from.
bool carriesEmissiveColor(const PrModel* model) {
    const Model* m = reinterpret_cast<const Model*>(model);
    for (uint32_t i = 0; i < m->layouts.count; ++i) {
        if (prRenderVertexAttributeFromName(m->layouts.data[i], "emissiveColor") != kPrRenderVertexAttributeMissing)
            return true;
    }
    return false;
}

void checkBox(const PrBox3& box, PrVec3 min, PrVec3 max) {
    CHECK(box.min.x == doctest::Approx(min.x));
    CHECK(box.min.y == doctest::Approx(min.y));
    CHECK(box.min.z == doctest::Approx(min.z));
    CHECK(box.max.x == doctest::Approx(max.x));
    CHECK(box.max.y == doctest::Approx(max.y));
    CHECK(box.max.z == doctest::Approx(max.z));
}

// Returns the background that a view of `scene` from the origin toward `direction` shows at its center.
PrVec3 backgroundToward(Fixture& f, PrRenderScene scene, PrVec3 direction) {
    PrRenderViewDesc view = frontView(scene);
    view.camera = prRenderCameraLookAt({0, 0, 0}, direction, 0.2f, 1, 0.1f);
    Image image = f.view(view);
    const float* texel = image.at(kCenter, kCenter);
    return PrVec3{texel[0], texel[1], texel[2]};
}

void checkColor(PrVec3 color, PrVec3 expected) {
    CHECK(color.x == doctest::Approx(expected.x).epsilon(0.02));
    CHECK(color.y == doctest::Approx(expected.y).epsilon(0.02));
    CHECK(color.z == doctest::Approx(expected.z).epsilon(0.02));
}

// Returns the color at the center of a front view of a model's quad, in an even environment of radiance 1.
PrVec3 quadCenter(Fixture& f, UsdModel& u) {
    prModelPopulate(u.model, f.render);
    PrRenderScene scene = prRenderSceneCreate(f.render);
    PrRenderEnvironment environment = createEvenEnvironment(f, {1, 1, 1});
    setEnvironment(f, scene, environment);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    prModelWait(u.model);
    prModelUpdate(u.model);
    Image image = f.view(frontView(scene));
    const float* texel = image.at(kCenter, kCenter);
    PrVec3 color{texel[0], texel[1], texel[2]};
    prModelPlacementDestroy(placement);
    prModelPopulate(u.model, nullptr);
    prRenderSceneDestroy(f.render, scene);
    prRenderEnvironmentDestroy(f.render, environment);
    return color;
}

// A stage of a second at 24 time codes a second.
constexpr PrStringView kSecond = "(\n metersPerUnit = 1\n startTimeCode = 0\n endTimeCode = 24\n"
    " timeCodesPerSecond = 24\n)\n";

// Returns the tracks of a model's description that set `target`.
uint32_t tracksOf(const PrModel* model, PrModelTrackTarget target) {
    const PrModelSourceInfo* info = prModelSourceInfo(prModelSource(model));
    uint32_t count = 0;
    for (uint32_t i = 0; i < info->trackCount; ++i)
        count += info->tracks[i].target == target ? 1 : 0;
    return count;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a stage's prims are a model's nodes, found by their paths, in meters with +y up") {
    UsdModel u;
    String body("(\n upAxis = \"Z\"\n metersPerUnit = 0.01\n)\ndef Xform \"World\" {\n"
        " double3 xformOp:translate = (0, 0, 100)\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
        " def Mesh \"Quad\" {\n");
    body.append(kQuad).append(" }\n def Xform \"Guides\" {\n uniform token purpose = \"guide\"\n"
        " def Mesh \"Hidden\" {\n");
    body.append(kQuad).append(" }\n }\n def Points \"Dots\" {\n }\n}\n");
    u.open(body);
    // The guide and what is under it are no nodes.
    CHECK(prModelNodeCount(u.model) == 3);
    uint32_t quad = u.node("/World/Quad");
    CHECK(prModelNodeName(u.model, quad) == "Quad");
    CHECK(prModelNodeParent(u.model, quad) == u.node("/World"));
    CHECK(u.path(quad) == "/World/Quad");
    CHECK(prModelNodeFromAddress(u.model, "/World/Guides") == kPrModelNodeNone);
    CHECK(prModelNodeFromAddress(u.model, "/World/Missing") == kPrModelNodeNone);
    CHECK(prModelHasDiagnostic(u.model, "primitives"));

    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    // A quad of 2 cm, 1 m up along the stage's z, is 2 cm wide at y = 1 in the scene, facing +y, which was +z.
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placement, &box));
    checkBox(box, {-0.01f, 1, -0.01f}, {0.01f, 1, 0.01f});
    // A ray down onto it meets the quad, and the pick names its node.
    PrModelPick pick{};
    PrMeshRay ray{.origin = {0, 2, 0}, .tMin = 0, .direction = {0, -1, 0}, .tMax = 10};
    REQUIRE(prModelPlacementPick(placement, ray, &pick));
    CHECK(pick.node == quad);
    CHECK(pick.t == doctest::Approx(1));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a mesh's subsets are its parts, each with the material that it binds") {
    UsdModel u;
    String body("(\n metersPerUnit = 1\n)\ndef Scope \"Looks\" {\n def Material \"Red\" {\n"
        " token outputs:surface.connect = </Looks/Red/S.outputs:surface>\n def Shader \"S\" {\n"
        " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:diffuseColor = (1, 0, 0)\n"
        " token outputs:surface\n }\n }\n def Material \"Glass\" {\n"
        " token outputs:surface.connect = </Looks/Glass/S.outputs:surface>\n def Shader \"S\" {\n"
        " uniform token info:id = \"UsdPreviewSurface\"\n float inputs:opacity = 0.3\n"
        " token outputs:surface\n }\n }\n}\n"
        "def Mesh \"M\" {\n rel material:binding = </Looks/Red>\n"
        " point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0), (2, 1, 0)]\n"
        " int[] faceVertexCounts = [4, 4, 3]\n int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 5, 2, 0, 2, 3]\n"
        " int[] holeIndices = [2]\n"
        " def GeomSubset \"Pane\" {\n uniform token elementType = \"face\"\n"
        " uniform token familyName = \"materialBind\"\n int[] indices = [1]\n"
        " rel material:binding = </Looks/Glass>\n }\n}\n"
        "def Mesh \"Plain\" {\n");
    body.append(kQuad).append("}\n");
    u.open(body);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    // The subset is the first part, and the faces that it does not hold the second; the hole is in neither.
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{1.5f, 0.5f, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/M"));
    uint32_t pane = pick.part;
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{0.75f, 0.25f, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.part == pane + 1);
    // The hole is the triangle over the first quad's upper left half, so a ray there meets the quad itself.
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{0.25f, 0.75f, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.part == pane + 1);
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{-0.5f, -0.5f, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/Plain"));
    prModelPlacementDestroy(placement);
}

TEST_CASE("every instance of a prototype has nodes of its own, and shares its meshes") {
    UsdModel u;
    String body("(\n metersPerUnit = 1\n)\nclass Xform \"Chair\" {\n def Mesh \"Seat\" {\n");
    body.append(kQuad).append(" }\n}\n");
    const PrStringView names[] = {"A", "B"};
    for (PrStringView name : names) {
        body.append("def Xform \"").append(name).append("\" (\n instanceable = true\n inherits = </Chair>\n) {\n");
        body.append(name == "A" ? " double3 xformOp:translate = (-5, 0, 0)\n" : " double3 xformOp:translate = (5, 0, 0)\n");
        body.append(" uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
    }
    u.open(body);
    uint32_t a = u.node("/A/Seat");
    uint32_t b = u.node("/B/Seat");
    CHECK(a != b);
    CHECK(prModelNodeName(u.model, a) == prModelNodeName(u.model, b));
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placement, &box));
    checkBox(box, {-6, -1, 0}, {6, 1, 0});
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{5, 0, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == b);
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{-5, 0, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == a);
    CHECK(pick.mesh == 0);
    prModelPlacementDestroy(placement);
}

TEST_CASE("an implicit surface is a mesh of its shape, along its axis") {
    UsdModel u;
    u.open("(\n metersPerUnit = 1\n)\ndef Sphere \"Ball\" {\n double radius = 0.5\n}\n"
        "def Cylinder \"Post\" {\n double3 xformOp:translate = (5, 0, 0)\n uniform token[] xformOpOrder = "
        "[\"xformOp:translate\"]\n token axis = \"X\"\n double height = 4\n}\n");
    CHECK(prModelDiagnosticCount(u.model) == 0);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    // The cylinder of radius 1 lies along x from 3 to 7.
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placement, &box));
    checkBox(box, {-0.5f, -1, -1}, {7, 1, 1});
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{0, 0, 2}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/Ball"));
    CHECK(pick.t == doctest::Approx(1.5));
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{6.5f, 0, 2}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/Post"));
    CHECK(pick.t == doctest::Approx(1));
    // The cylinder's cap faces along x, past its end.
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{9, 0, 0}, 0, {-1, 0, 0}, 10}, &pick));
    CHECK(pick.t == doctest::Approx(2));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a point instancer draws its prototype at each of its visible instances") {
    UsdModel u;
    String body("(\n metersPerUnit = 1\n)\ndef PointInstancer \"Forest\" {\n rel prototypes = [</Forest/Protos/Tree>]\n"
        " int[] protoIndices = [0, 0, 0]\n point3f[] positions = [(0, 0, 0), (10, 0, 0), (20, 0, 0)]\n"
        " int64[] invisibleIds = [2]\n def Scope \"Protos\" {\n def Mesh \"Tree\" {\n");
    body.append(kQuad).append(" }\n }\n}\n");
    u.open(body);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placement, &box));
    checkBox(box, {-1, -1, 0}, {11, 1, 0});
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{10, 0, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/Forest"));
    CHECK(pick.instance == 1);
    CHECK(!prModelPlacementPick(placement, PrMeshRay{{20, 0, 1}, 0, {0, 0, -1}, 10}, &pick));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a stage's time codes are the model's animation, which moves its transforms") {
    UsdModel u;
    String body("(\n metersPerUnit = 1\n startTimeCode = 0\n endTimeCode = 48\n timeCodesPerSecond = 24\n)\n"
        "def Xform \"Mover\" {\n double3 xformOp:translate.timeSamples = {\n 0: (0, 0, 0),\n 48: (8, 0, 0),\n }\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\"]\n def Mesh \"Quad\" {\n");
    body.append(kQuad).append(" }\n}\n");
    u.open(body);
    REQUIRE(prModelAnimationCount(u.model) == 1);
    CHECK(prModelAnimationDuration(u.model, 0) == doctest::Approx(2));
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    uint32_t mover = u.node("/Mover");
    CHECK(prModelPlacementNodeWorld(placement, mover).m[0][3] == doctest::Approx(0));
    prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
    prModelPlacementSetTime(placement, 1);
    CHECK(prModelPlacementNodeWorld(placement, mover).m[0][3] == doctest::Approx(4));
    prModelPlacementSetTime(placement, 2);
    CHECK(prModelPlacementNodeWorld(placement, mover).m[0][3] == doctest::Approx(8));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a camera and a light of the stage are the model's") {
    UsdModel u;
    u.open("(\n metersPerUnit = 0.01\n)\ndef Camera \"Shot\" {\n float focalLength = 25\n float verticalAperture = 50\n"
        " float horizontalAperture = 100\n float2 clippingRange = (10, 10000)\n}\n"
        "def SphereLight \"Bulb\" {\n float inputs:intensity = 4\n bool inputs:normalize = 1\n"
        " color3f inputs:color = (1, 0.5, 0)\n}\ndef DistantLight \"Sun\" {\n float inputs:intensity = 2\n}\n");
    REQUIRE(prModelCameraCount(u.model) == 1);
    CHECK(prModelCameraNode(u.model, 0) == u.node("/Shot"));
    REQUIRE(prModelLightCount(u.model) == 2);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    // A focal length of half the vertical aperture sees 90 degrees from the bottom to the top.
    PrRenderCamera camera = prModelPlacementCamera(placement, 0, 2);
    PrMatrix4 expected = prMatrix4PerspectiveProjection(3.14159265f / 2, 2, 0.1f);
    CHECK(camera.projection.m[0][0] == doctest::Approx(expected.m[0][0]));
    CHECK(camera.projection.m[1][1] == doctest::Approx(expected.m[1][1]));
    PrRenderLightDesc bulb = prModelPlacementLight(placement, 0);
    CHECK(bulb.kind == PrRenderLightKind_Point);
    CHECK(bulb.color.y == 0.5f);
    // A light that normalizes its size sends a quarter of its radiance as its intensity, as a sphere seen from afar.
    CHECK(bulb.intensity == doctest::Approx(1));
    PrRenderLightDesc sun = prModelPlacementLight(placement, 1);
    CHECK(sun.kind == PrRenderLightKind_Directional);
    CHECK(sun.intensity == 2);
    prModelPlacementDestroy(placement);
}

TEST_CASE("a stage reports its dome, its lights and a preview surface that emits") {
    UsdModel u;
    u.open("(\n metersPerUnit = 1\n)\ndef DomeLight \"Dome\" {\n}\ndef DistantLight \"Sun\" {\n}\n"
        "def Material \"Glow\" {\n token outputs:surface.connect = </Glow/S.outputs:surface>\n def Shader \"S\" {\n"
        " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:emissiveColor = (0, 2, 0)\n"
        " token outputs:surface\n }\n}\n"
        "def Mesh \"M\" {\n rel material:binding = </Glow>\n point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n}\n");
    CHECK(prModelLighting(u.model) ==
        (PrModelLightingFlag_Lights | PrModelLightingFlag_Environment | PrModelLightingFlag_Emission));
    // A mesh without a material emits nothing.
    UsdModel plain;
    plain.open("(\n metersPerUnit = 1\n)\ndef Mesh \"M\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n}\n");
    CHECK(prModelLighting(plain.model) == 0);
}

TEST_CASE("a skeleton and a light of a kind that the source lacks are named by diagnostics") {
    UsdModel u;
    String body("(\n metersPerUnit = 1\n)\ndef SkelRoot \"Rig\" {\n def Skeleton \"Skel\" {\n }\n"
        " def Mesh \"Skin\" {\n");
    body.append(kQuad).append(" }\n}\ndef PortalLight \"Window\" {\n}\n");
    u.open(body);
    CHECK(prModelHasDiagnostic(u.model, "skeletons"));
    CHECK(prModelHasDiagnostic(u.model, "lights"));
    CHECK(prModelLightCount(u.model) == 0);
    // The skinned mesh draws as it is authored.
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    PrModelPick pick{};
    REQUIRE(prModelPlacementPick(placement, PrMeshRay{{0, 0, 1}, 0, {0, 0, -1}, 10}, &pick));
    CHECK(pick.node == u.node("/Rig/Skin"));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a light's intensity keeps its photometric unit: a distant light's fallback is 50,000 lux") {
    UsdModel u;
    u.open("(\n metersPerUnit = 1\n)\ndef DistantLight \"Sun\" {\n}\ndef SphereLight \"Bulb\" (\n prepend apiSchemas = [\"ShapingAPI\"]\n)\n{\n"
        " float inputs:intensity = 1000\n float inputs:radius = 0.1\n float inputs:shaping:cone:angle = 45\n}\n");
    REQUIRE(prModelLightCount(u.model) == 2);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    PrRenderLightDesc sun = prModelPlacementLight(placement, 0);
    CHECK(sun.kind == PrRenderLightKind_Directional);
    CHECK(sun.intensity == 50000);
    // A sphere of 1000 nits and a radius of 0.1 m, shaped to a spot, is pi r^2 L candela along its axis.
    PrRenderLightDesc bulb = prModelPlacementLight(placement, 1);
    CHECK(bulb.kind == PrRenderLightKind_Spot);
    CHECK(bulb.intensity == doctest::Approx(kPrF64Pi * 0.01 * 1000).epsilon(1e-5));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a stage's visibility, lights, cameras and material values that vary are tracks of its animation") {
    UsdModel u;
    String body(kSecond);
    body.append("def Xform \"Flicker\" {\n token visibility.timeSamples = {\n 0: \"inherited\",\n 12: \"invisible\",\n"
        " }\n def Mesh \"Quad\" {\n");
    body.append(kQuad).append(" }\n}\n"
        "def SphereLight \"Bulb\" {\n float inputs:intensity.timeSamples = {\n 0: 1,\n 24: 3,\n }\n}\n"
        "def Camera \"Shot\" {\n float focalLength.timeSamples = {\n 0: 25,\n 24: 50,\n }\n"
        " float verticalAperture = 50\n float horizontalAperture = 100\n}\n"
        "def Material \"Glow\" {\n token outputs:surface.connect = </Glow/S.outputs:surface>\n def Shader \"S\" {\n"
        " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:emissiveColor.timeSamples = {\n"
        " 0: (0, 0, 0),\n 24: (0, 2, 0),\n }\n token outputs:surface\n }\n}\n"
        "def Mesh \"Lit\" {\n rel material:binding = </Glow>\n point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n"
        " uniform token subdivisionScheme = \"none\"\n}\n");
    u.open(body);
    CHECK(prModelDiagnosticCount(u.model) == 0);
    REQUIRE(prModelAnimationCount(u.model) == 1);
    CHECK(tracksOf(u.model, PrModelTrackTarget_Visible) == 1);
    CHECK(tracksOf(u.model, PrModelTrackTarget_LightIntensity) == 1);
    CHECK(tracksOf(u.model, PrModelTrackTarget_CameraYfov) == 1);
    CHECK(tracksOf(u.model, PrModelTrackTarget_Input) == 1);
    // Nothing that holds still has a track.
    CHECK(tracksOf(u.model, PrModelTrackTarget_Matrix) == 0);
    CHECK(tracksOf(u.model, PrModelTrackTarget_LightColor) == 0);
    CHECK(tracksOf(u.model, PrModelTrackTarget_CameraZfar) == 0);

    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
    PrModelPick pick{};
    PrMeshRay ray{{0, 0, 1}, 0, {0, 0, -1}, 10};
    REQUIRE(prModelPlacementPick(placement, ray, &pick));
    CHECK(pick.node == u.node("/Flicker/Quad"));
    float atStart = prModelPlacementLight(placement, 0).intensity;
    // A focal length of half the vertical aperture sees 90 degrees, and one of the aperture about 53.
    CHECK(prModelPlacementCamera(placement, 0, 2).projection.m[1][1] == doctest::Approx(1));
    prModelPlacementSetTime(placement, 1);
    CHECK(prModelPlacementCamera(placement, 0, 2).projection.m[1][1] == doctest::Approx(2));
    CHECK(prModelPlacementLight(placement, 0).intensity == doctest::Approx(atStart * 3));
    // The quad is invisible from the half second, so the ray meets the triangle behind it, or nothing.
    CHECK((!prModelPlacementPick(placement, ray, &pick) || pick.node != u.node("/Flicker/Quad")));
    prModelPlacementDestroy(placement);
}

TEST_CASE("a value that varies and that no track holds is reified at the start, and a diagnostic names it") {
    UsdModel u;
    String body(kSecond);
    body.append("def Mesh \"Wave\" {\n point3f[] points.timeSamples = {\n 0: [(0, 0, 0), (1, 0, 0), (1, 1, 0)],\n"
        " 24: [(0, 0, 0), (2, 0, 0), (2, 2, 0)],\n }\n int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n"
        " uniform token subdivisionScheme = \"none\"\n}\n");
    u.open(body);
    CHECK(prModelHasDiagnostic(u.model, "mesh-animation"));
    CHECK(prModelAnimationCount(u.model) == 0);
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    PrBox3 box{};
    REQUIRE(prModelPlacementBounds(placement, &box));
    checkBox(box, {0, 0, 0}, {1, 1, 0});
    prModelPlacementDestroy(placement);

    // A stage that states no time codes is reified at its start, which a context of no time range keeps.
    UsdModel still;
    still.open("(\n metersPerUnit = 1\n)\ndef Xform \"Mover\" {\n double3 xformOp:translate.timeSamples = {\n"
        " 0: (0, 0, 0),\n 48: (8, 0, 0),\n }\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
    CHECK(prModelHasDiagnostic(still.model, "time-range"));
    CHECK(prModelAnimationCount(still.model) == 0);
}

TEST_CASE("a source's context chooses the time codes, the purposes and the variants that it reifies") {
    String body("(\n metersPerUnit = 1\n startTimeCode = 0\n endTimeCode = 48\n timeCodesPerSecond = 24\n)\n"
        "def Xform \"Mover\" (\n variants = { string size = \"small\" }\n prepend variantSets = \"size\"\n) {\n"
        " double3 xformOp:translate.timeSamples = {\n 0: (0, 0, 0),\n 48: (8, 0, 0),\n }\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\"]\n"
        " variantSet \"size\" = {\n \"small\" { def Xform \"Small\" {} }\n \"big\" { def Xform \"Big\" {} }\n }\n"
        " def Xform \"Full\" {\n uniform token purpose = \"render\"\n }\n"
        " def Xform \"Preview\" {\n uniform token purpose = \"proxy\"\n }\n"
        " def Xform \"Guide\" {\n uniform token purpose = \"guide\"\n }\n}\n");
    {
        UsdModel u;
        u.open(body);
        CHECK(prModelAnimationDuration(u.model, 0) == doctest::Approx(2));
        CHECK(prModelNodeFromAddress(u.model, "/Mover/Small") != kPrModelNodeNone);
        CHECK(prModelNodeFromAddress(u.model, "/Mover/Full") != kPrModelNodeNone);
        CHECK(prModelNodeFromAddress(u.model, "/Mover/Preview") == kPrModelNodeNone);
        CHECK(prModelNodeFromAddress(u.model, "/Mover/Guide") == kPrModelNodeNone);
    }
    // The second half second of the stage, its preview and its guides, in its big variant.
    const PrUsdPrimVariantSelection big{"/Mover", "size", "big"};
    PrUsdModelContext context{
        .variantSelectionCount = 1,
        .variantSelections = &big,
        .proxy = true,
        .guides = true,
        .hasTimeRange = true,
        .startTimeCode = 24,
        .endTimeCode = 36,
    };
    UsdModel u;
    u.open(body, nullptr, context);
    CHECK(prModelDiagnosticCount(u.model) == 0);
    CHECK(prModelNodeFromAddress(u.model, "/Mover/Big") != kPrModelNodeNone);
    CHECK(prModelNodeFromAddress(u.model, "/Mover/Small") == kPrModelNodeNone);
    CHECK(prModelNodeFromAddress(u.model, "/Mover/Full") == kPrModelNodeNone);
    CHECK(prModelNodeFromAddress(u.model, "/Mover/Preview") != kPrModelNodeNone);
    CHECK(prModelNodeFromAddress(u.model, "/Mover/Guide") != kPrModelNodeNone);
    REQUIRE(prModelAnimationCount(u.model) == 1);
    CHECK(prModelAnimationDuration(u.model, 0) == doctest::Approx(0.5));
    PrModelPlacementDesc desc = prModelPlacementDescDefault(PrRenderScene{});
    PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
    uint32_t mover = u.node("/Mover");
    CHECK(prModelPlacementNodeWorld(placement, mover).m[0][3] == doctest::Approx(4));
    prModelPlacementSetAnimation(placement, 0, PrModelLoop_Once);
    prModelPlacementSetTime(placement, 0.5f);
    CHECK(prModelPlacementNodeWorld(placement, mover).m[0][3] == doctest::Approx(6));
    prModelPlacementDestroy(placement);
    // A source of the stage in a context whose selections the stage was not composed with says so.
    const PrUsdPrimVariantSelection small{"/Mover", "size", "small"};
    PrUsdModelContext other{.variantSelectionCount = 1, .variantSelections = &small};
    PrModel* mismatched = prModelCreate(u.source(other), nullptr);
    CHECK(prModelHasDiagnostic(mismatched, "variant-selections"));
    prModelDestroy(mismatched);
}

TEST_CASE("a source of the same stage in the same context has the same description and the same meshes") {
    UsdModel u;
    String body(kSecond);
    body.append("def Xform \"Mover\" {\n double3 xformOp:translate.timeSamples = {\n 0: (0, 0, 0),\n 24: (8, 0, 0),\n"
        " }\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n def Sphere \"Ball\" {\n }\n"
        " def Mesh \"Quad\" {\n");
    body.append(kQuad).append(" }\n}\ndef SphereLight \"Bulb\" {\n float inputs:intensity.timeSamples = {\n 0: 1,\n"
        " 24: 3,\n }\n}\n");
    u.open(body);
    String first = u.described({});
    CHECK(first.count() > 0);
    CHECK(u.described({}) == first);
    PrUsdModelContext later{.hasTimeRange = true, .startTimeCode = 12, .endTimeCode = 24};
    String moved = u.described(later);
    CHECK(moved != first);
    CHECK(u.described(later) == moved);
}

TEST_SUITE_BEGIN("model usd" * doctest::skip(!gpuTests()));

TEST_CASE("a primitive variable that a preview surface reads into its emissive color draws at each vertex") {
    onEachDevice([](Fixture& f) {
        UsdModel u;
        // The quad glows red at its bottom and green at its top, and reflects nothing.
        String body("(\n metersPerUnit = 1\n)\ndef Material \"Glow\" {\n"
            " token outputs:surface.connect = </Glow/S.outputs:surface>\n def Shader \"S\" {\n"
            " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:emissiveColor.connect = </Glow/R.outputs:result>\n"
            " color3f inputs:diffuseColor = (0, 0, 0)\n token outputs:surface\n }\n def Shader \"R\" {\n"
            " uniform token info:id = \"UsdPrimvarReader_float3\"\n string inputs:varname = \"glow\"\n"
            " float3 outputs:result\n }\n}\n"
            "def Mesh \"Quad\" {\n rel material:binding = </Glow>\n"
            " color3f[] primvars:glow = [(1, 0, 0), (1, 0, 0), (0, 1, 0), (0, 1, 0)] (\n interpolation = \"vertex\"\n )\n");
        body.append(kQuad).append("}\n");
        u.open(body);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        CHECK(prModelIsLoaded(u.model));
        // The renderer's preview type declares an attribute for the emissive color, so nothing of the wiring is lost.
        CHECK(!prModelHasDiagnostic(u.model, "attribute-inputs"));
        // The attribute is optional, and the material binds it, so the mesh carries it.
        CHECK(carriesEmissiveColor(u.model));
        Image image = f.view(frontView(scene));
        const float* top = image.at(kCenter, 4);
        const float* bottom = image.at(kCenter, kExtent.y - 4);
        CAPTURE(top[0]);
        CAPTURE(top[1]);
        CAPTURE(bottom[0]);
        CAPTURE(bottom[1]);
        CHECK(top[1] > 0.8f);
        CHECK(top[0] < 0.2f);
        CHECK(bottom[0] > 0.8f);
        CHECK(bottom[1] < 0.2f);
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a preview surface whose emissive color no primitive variable gives draws its mesh without the attribute") {
    onEachDevice([](Fixture& f) {
        UsdModel u;
        // The quad glows an even blue, which a value gives, and reflects nothing.
        String body("(\n metersPerUnit = 1\n)\ndef Material \"Glow\" {\n"
            " token outputs:surface.connect = </Glow/S.outputs:surface>\n def Shader \"S\" {\n"
            " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:emissiveColor = (0, 0, 1)\n"
            " color3f inputs:diffuseColor = (0, 0, 0)\n token outputs:surface\n }\n}\n"
            "def Mesh \"Quad\" {\n rel material:binding = </Glow>\n");
        body.append(kQuad).append("}\n");
        u.open(body);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        CHECK(prModelIsLoaded(u.model));
        // The mesh carries the layout of the Khronos types alone, which the preview type's variant without the
        // attribute shares, and that variant reads the attribute as its value where a vertex lacks it, which is white.
        CHECK(!carriesEmissiveColor(u.model));
        const PrRenderShaderSet* set = prRenderShaderSet(f.render);
        const Model* m = reinterpret_cast<const Model*>(u.model);
        REQUIRE(m->layouts.count == 1);
        CHECK(m->layouts.data[0] == prRenderMaterialTypeLayout(set, prRenderMaterialTypeFromName(set, "KhronosPbr"), 0));
        Image image = f.view(frontView(scene));
        const float* top = image.at(kCenter, 4);
        const float* bottom = image.at(kCenter, kExtent.y - 4);
        CHECK(top[2] > 0.8f);
        CHECK(bottom[2] > 0.8f);
        CHECK(top[0] < 0.2f);
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a preview surface's texture, in a file beside its layer, shows upright on its quad") {
    onEachDevice([](Fixture& f) {
        ModelCodecs codecs;
        UsdModel u;
        // The image's top half is red and its bottom half green, so an image upright on the quad is red above.
        const uint8_t texels[32] = {
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0,
            255, 0, 255, 0, 255, 0, 255, 0, 255,
        };
        u.files.push(ModelFile{String("/assets/textures/halves.png"), codecs.png({2, 4}, texels)});
        String body("(\n metersPerUnit = 1\n)\ndef Material \"Lit\" {\n"
            " token outputs:surface.connect = </Lit/S.outputs:surface>\n def Shader \"S\" {\n"
            " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:emissiveColor.connect = </Lit/T.outputs:rgb>\n"
            " color3f inputs:diffuseColor = (0, 0, 0)\n token outputs:surface\n }\n def Shader \"T\" {\n"
            " uniform token info:id = \"UsdUVTexture\"\n asset inputs:file = @textures/halves.png@\n"
            " token inputs:sourceColorSpace = \"raw\"\n float2 inputs:st.connect = </Lit/R.outputs:result>\n"
            " float3 outputs:rgb\n }\n def Shader \"R\" {\n uniform token info:id = \"UsdPrimvarReader_float2\"\n"
            " string inputs:varname = \"st\"\n float2 outputs:result\n }\n}\n"
            "def Mesh \"Quad\" {\n rel material:binding = </Lit>\n");
        body.append(kQuad).append("}\n");
        u.open(body, codecs.textures);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        CHECK(prModelIsLoaded(u.model));
        CHECK(prModelImageInfo(u.model, 0).load == PrModelLoad_Ready);
        CHECK(prModelMeshInfo(u.model, 0).load == PrModelLoad_Ready);
        Image image = f.view(frontView(scene));
        // The emission alone, which no light changes: red above the middle, and green below it.
        CHECK(near(image.at(kCenter, 8), {1, 0, 0}, 0.02f));
        const float* low = image.at(kCenter, kExtent.y - 8);
        CAPTURE(low[0]);
        CAPTURE(low[1]);
        CAPTURE(low[2]);
        CHECK(near(low, {0, 1, 0}, 0.02f));
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a color texture that declares ACEScg makes the chain of one that declares linear values") {
    // The use of each chain that the policy makes, and what it makes.
    struct Recorder {
        Array<PrModelTextureUse> uses;
        Array<PrTextureDesc> descs;
        Array<PrTextureUploadFlags> flags;

        static PrStatus chain(void* PR_NULLABLE context, const PrTextureSource* source, const PrModelTextureUse* use,
                const PrGpuDevice* PR_NULLABLE device, PrModelTextureChain* out) {
            Recorder* recorder = static_cast<Recorder*>(context);
            PrStatus status = prModelTextureChainDefault(nullptr, source, use, device, out);
            REQUIRE(status == PrStatus_Ok);
            recorder->uses.push(*use);
            recorder->descs.push(*prTextureSourceDesc(out->source));
            recorder->flags.push(out->flags);
            return status;
        }
    };
    onEachDevice([](Fixture& f) {
        ModelCodecs codecs;
        uint8_t texels[4 * 4 * 4];
        for (uint32_t i = 0; i < sizeof(texels); ++i)
            texels[i] = uint8_t(i * 5);
        // The texture's file names its color space, or its shader reads it raw.
        const PrStringView kSpaces[2] = {
            "asset inputs:file = @textures/color.png@ (\n colorSpace = \"acescg\"\n)\n",
            "asset inputs:file = @textures/color.png@\n token inputs:sourceColorSpace = \"raw\"\n",
        };
        Recorder recorders[2];
        for (uint32_t i = 0; i < 2; ++i) {
            UsdModel u;
            u.chain = Recorder::chain;
            u.chainContext = &recorders[i];
            u.files.push(ModelFile{String("/assets/textures/color.png"), codecs.png({4, 4}, texels)});
            String body("(\n metersPerUnit = 1\n)\ndef Material \"Lit\" {\n"
                " token outputs:surface.connect = </Lit/S.outputs:surface>\n def Shader \"S\" {\n"
                " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:diffuseColor.connect = </Lit/T.outputs:rgb>\n"
                " token outputs:surface\n }\n def Shader \"T\" {\n uniform token info:id = \"UsdUVTexture\"\n ");
            body.append(kSpaces[i]).append(" float3 outputs:rgb\n }\n}\ndef Mesh \"Quad\" {\n rel material:binding = </Lit>\n");
            body.append(kQuad).append("}\n");
            u.open(body, codecs.textures);
            CHECK(prModelDiagnosticCount(u.model) == 0);
            prModelPopulate(u.model, f.render);
            prModelWait(u.model);
            prModelUpdate(u.model);
            CHECK(prModelImageInfo(u.model, 0).load == PrModelLoad_Ready);
            prModelPopulate(u.model, nullptr);
        }
        // Both are read as the values that they store, which the color input samples through the plain form.
        REQUIRE(recorders[0].uses.count() == 1);
        REQUIRE(recorders[1].uses.count() == 1);
        const PrModelTextureUse& acescg = recorders[0].uses[0];
        const PrModelTextureUse& linear = recorders[1].uses[0];
        CHECK(acescg.kind == PrRenderTextureKind_Color);
        CHECK(acescg.color == PrModelTextureColor_Data);
        CHECK(acescg.kind == linear.kind);
        CHECK(acescg.channels == linear.channels);
        CHECK(acescg.color == linear.color);
        const PrTextureDesc& made = recorders[0].descs[0];
        const PrTextureDesc& other = recorders[1].descs[0];
        CHECK(made.format == PrGpuFormat_Rgba8);
        CHECK(made.format == other.format);
        CHECK(made.extent == other.extent);
        CHECK(made.levelCount == other.levelCount);
        CHECK(made.alpha == other.alpha);
        CHECK(recorders[0].flags[0] == recorders[1].flags[0]);
    });
}

// A MaterialX document whose surface emits an image beside it, halved by a multiply node, and reflects nothing.
constexpr PrStringView kGlowLook = R"(<?xml version="1.0"?>
<materialx version="1.39" colorspace="lin_rec709">
  <nodegraph name="NG_glow">
    <image name="halves" type="color3">
      <input name="file" type="filename" value="textures/halves.png" />
    </image>
    <multiply name="dimmed" type="color3">
      <input name="in1" type="color3" nodename="halves" />
      <input name="in2" type="float" value="0.5" />
    </multiply>
    <output name="out" type="color3" nodename="dimmed" />
  </nodegraph>
  <open_pbr_surface name="SR_glow" type="surfaceshader">
    <input name="base_weight" type="float" value="0" />
    <input name="specular_weight" type="float" value="0" />
    <input name="emission_luminance" type="float" value="1" />
    <input name="emission_color" type="color3" nodegraph="NG_glow" output="out" />
  </open_pbr_surface>
  <surfacematerial name="Glow" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_glow" />
  </surfacematerial>
</materialx>
)";

TEST_CASE("a MaterialX surface draws as OpenPBR, or as the preview surface where the renderer has no OpenPBR") {
    auto check = [](Fixture& f) {
        ModelCodecs codecs;
        UsdModel u;
        // The image's top half is red and its bottom half green.
        const uint8_t texels[32] = {
            255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 255, 0, 0, 255, 0, 255, 0, 255, 0, 255, 0,
            255, 0, 255, 0, 255, 0, 255, 0, 255,
        };
        u.files.push(ModelFile{String("/assets/textures/halves.png"), codecs.png({2, 4}, texels)});
        Array<uint8_t> look;
        look.append(Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kGlowLook.data), kGlowLook.size));
        u.files.push(ModelFile{String("/assets/look.mtlx"), static_cast<Array<uint8_t>&&>(look)});
        String body("(\n metersPerUnit = 1\n)\ndef Scope \"Looks\" (\n references = @./look.mtlx@</MaterialX/Materials>\n"
            ")\n{\n}\ndef Mesh \"Quad\" {\n rel material:binding = </Looks/Glow>\n");
        body.append(kQuad).append("}\n");
        u.open(body, codecs.textures);
        CHECK(prModelDiagnosticCount(u.model) == 0);
        const PrModelSourceInfo& info = *prModelSourceInfo(prModelSource(u.model));
        REQUIRE(info.imageCount == 1);
        CHECK(info.images[0].location == "/assets/textures/halves.png");
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        CHECK(prModelIsLoaded(u.model));
        Image image = f.view(frontView(scene));
        // Half of the image's emission, upright: red above the middle, and green below it.
        CHECK(near(image.at(kCenter, 8), {0.5f, 0, 0}, 0.02f));
        CHECK(near(image.at(kCenter, kExtent.y - 8), {0, 0.5f, 0}, 0.02f));
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
    };
    onEachDevice(check, testOpenPbrShaderSet());
    onEachDevice(check);
}

TEST_CASE("a dome light without a texture is an even environment of its color, intensity and exposure") {
    onEachDevice([](Fixture& f) {
        UsdModel u;
        u.open("(\n metersPerUnit = 1\n)\ndef DomeLight \"Dome\" {\n color3f inputs:color = (1, 0.5, 0.25)\n"
            " float inputs:intensity = 0.5\n float inputs:exposure = 1\n}\n");
        REQUIRE(prModelEnvironmentNode(u.model) == u.node("/Dome"));
        CHECK(prModelLightCount(u.model) == 0);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        PrRenderSceneEnvironment environment{};
        REQUIRE(prModelPlacementEnvironment(placement, &environment));
        CHECK(environment.intensity == 1);
        PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
        prRenderSceneEnvironmentSet(update, &environment);
        prRenderUpdateCommit(update);
        checkColor(backgroundToward(f, scene, {0, 0, -1}), {1, 0.5f, 0.25f});
        checkColor(backgroundToward(f, scene, {0, 1, 0}), {1, 0.5f, 0.25f});
        // The scene's meter reads the dome: pi times its luminance on any plane.
        double luminance = 0.2126 + 0.7152 * 0.5 + 0.0722 * 0.25;
        CHECK(prRenderSceneIlluminance(f.render, scene, {0, 0, 0}) ==
            doctest::Approx(kPrF64Pi * luminance).epsilon(0.02));
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
    });
}

TEST_CASE("a dome light's latlong texture is OpenEXR's map in the dome's frame, which its transform turns") {
    onEachDevice([](Fixture& f) {
        ModelCodecs codecs;
        // Four bands of longitude, from the left: red toward -z and +x, green toward +x and +z, blue toward +z and -x,
        // and white toward -x and -z.
        constexpr PrUVec2 kSkyExtent{64, 32};
        const uint8_t bands[4][4] = {{255, 0, 0, 255}, {0, 255, 0, 255}, {0, 0, 255, 255}, {255, 255, 255, 255}};
        Array<uint8_t> texels;
        for (uint32_t y = 0; y < kSkyExtent.y; ++y) {
            for (uint32_t x = 0; x < kSkyExtent.x; ++x)
                texels.append(Span<const uint8_t>(bands[x / 16], 4));
        }
        auto background = [&](PrStringView turn, PrVec3 direction) {
            UsdModel u;
            u.files.push(ModelFile{String("/assets/sky.png"), codecs.png(kSkyExtent, texels.begin())});
            String body("(\n metersPerUnit = 1\n)\ndef DomeLight \"Dome\" {\n asset inputs:texture:file = @sky.png@\n"
                " token inputs:texture:format = \"latlong\"\n");
            body.append(turn).append("}\n");
            u.open(body, codecs.textures);
            prModelPopulate(u.model, f.render);
            PrRenderScene scene = prRenderSceneCreate(f.render);
            PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
            PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
            prModelWait(u.model);
            CHECK(prModelImageInfo(u.model, 0).load == PrModelLoad_Ready);
            PrRenderSceneEnvironment environment{};
            REQUIRE(prModelPlacementEnvironment(placement, &environment));
            PrRenderUpdate* update = prRenderUpdateBegin(f.render, scene);
            prRenderSceneEnvironmentSet(update, &environment);
            prRenderUpdateCommit(update);
            PrVec3 color = backgroundToward(f, scene, direction);
            prModelPlacementDestroy(placement);
            prModelPopulate(u.model, nullptr);
            prRenderSceneDestroy(f.render, scene);
            return color;
        };
        checkColor(background("", {1, 0, -1}), {1, 0, 0});
        checkColor(background("", {1, 0, 1}), {0, 1, 0});
        checkColor(background("", {-1, 0, 1}), {0, 0, 1});
        checkColor(background("", {-1, 0, -1}), {1, 1, 1});
        // A quarter turn about y takes the green band's middle, toward +x and +z, to +x and -z.
        PrStringView turned = " double3 xformOp:rotateXYZ = (0, 90, 0)\n uniform token[] xformOpOrder = "
            "[\"xformOp:rotateXYZ\"]\n";
        checkColor(background(turned, {1, 0, -1}), {0, 1, 0});
    });
}

// A MaterialX document of a white metal of Standard Surface whose dielectric reflects nothing.
constexpr PrStringView kMetalLook = R"(<?xml version="1.0"?>
<materialx version="1.38">
  <standard_surface name="SR_metal" type="surfaceshader">
    <input name="base" type="float" value="1" />
    <input name="base_color" type="color3" value="1, 1, 1" />
    <input name="specular" type="float" value="0" />
    <input name="specular_roughness" type="float" value="0.5" />
    <input name="metalness" type="float" value="1" />
  </standard_surface>
  <surfacematerial name="Metal" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_metal" />
  </surfacematerial>
</materialx>
)";

TEST_CASE("Standard Surface's specular weight leaves its metal's reflection whole") {
    onEachDevice([](Fixture& f) {
        UsdModel u;
        Array<uint8_t> look;
        look.append(Span<const uint8_t>(reinterpret_cast<const uint8_t*>(kMetalLook.data), kMetalLook.size));
        u.files.push(ModelFile{String("/assets/look.mtlx"), static_cast<Array<uint8_t>&&>(look)});
        String body("(\n metersPerUnit = 1\n)\ndef Scope \"Looks\" (\n references = @./look.mtlx@</MaterialX/Materials>\n"
            ")\n{\n}\ndef Mesh \"Quad\" {\n rel material:binding = </Looks/Metal>\n");
        body.append(kQuad).append("}\n");
        u.open(body);
        CHECK(prModelDiagnosticCount(u.model) == 0);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrRenderEnvironment environment = createEvenEnvironment(f, {1, 1, 1});
        setEnvironment(f, scene, environment);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        Image image = f.view(frontView(scene));
        // A white metal in an even environment returns what it receives.
        const float* center = image.at(kCenter, kCenter);
        CAPTURE(center[0]);
        CHECK(near(center, {1, 1, 1}, 0.05f));
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
        prRenderEnvironmentDestroy(f.render, environment);
    }, testOpenPbrShaderSet());
}

// The primitive variables of a quad that the readers of the tests read, constant over it: a glow and its power, and a
// display color.
constexpr PrStringView kQuadGlow = " color3f[] primvars:glow = [(0.5, 0.25, 1)] (\n interpolation = \"constant\"\n )\n"
    " float[] primvars:power = [0.5] (\n interpolation = \"constant\"\n )\n";
constexpr PrStringView kQuadDisplay =
    " color3f[] primvars:displayColor = [(0.25, 0.5, 1)] (\n interpolation = \"constant\"\n )\n";

TEST_CASE("a preview surface's input reads a primitive variable of its mesh through a reader") {
    onEachDevice([](Fixture& f) {
        UsdModel u;
        String body("(\n metersPerUnit = 1\n)\ndef Material \"Lit\" {\n"
            " token outputs:surface.connect = </Lit/S.outputs:surface>\n def Shader \"S\" {\n"
            " uniform token info:id = \"UsdPreviewSurface\"\n color3f inputs:diffuseColor = (0, 0, 0)\n"
            " color3f inputs:specularColor = (0, 0, 0)\n int inputs:useSpecularWorkflow = 1\n"
            " color3f inputs:emissiveColor.connect = </Lit/R.outputs:result>\n token outputs:surface\n }\n"
            " def Shader \"R\" {\n uniform token info:id = \"UsdPrimvarReader_float3\"\n string inputs:varname = \"glow\"\n"
            " float3 outputs:result\n }\n}\ndef Mesh \"Quad\" {\n rel material:binding = </Lit>\n");
        body.append(kQuad).append(kQuadGlow).append("}\n");
        u.open(body);
        CHECK(prModelDiagnosticCount(u.model) == 0);
        PrVec3 color = quadCenter(f, u);
        CHECK(color.x == doctest::Approx(0.5).epsilon(0.03));
        CHECK(color.y == doctest::Approx(0.25).epsilon(0.03));
        CHECK(color.z == doctest::Approx(1).epsilon(0.03));
    });
}

// A MaterialX document whose emission is a primitive variable's color times another's scalar.
constexpr PrStringView kGlowVariableLook = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG">
    <geompropvalue name="glow" type="color3">
      <input name="geomprop" type="string" value="glow" />
    </geompropvalue>
    <geompropvalue name="power" type="float">
      <input name="geomprop" type="string" value="power" />
    </geompropvalue>
    <output name="glow_out" type="color3" nodename="glow" />
    <output name="power_out" type="float" nodename="power" />
  </nodegraph>
  <open_pbr_surface name="SR" type="surfaceshader">
    <input name="specular_weight" type="float" value="0" />
    <input name="base_color" type="color3" value="1, 1, 1" />
    <input name="emission_color" type="color3" nodegraph="NG" output="glow_out" />
    <input name="emission_luminance" type="float" nodegraph="NG" output="power_out" />
  </open_pbr_surface>
  <surfacematerial name="Look" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR" />
  </surfacematerial>
</materialx>
)";

// A MaterialX document whose base color is an image times the display color.
constexpr PrStringView kTintLook = R"(<?xml version="1.0"?>
<materialx version="1.39">
  <nodegraph name="NG">
    <image name="white" type="color3">
      <input name="file" type="filename" value="white.png" />
    </image>
    <geompropvalue name="tint" type="color3">
      <input name="geomprop" type="string" value="displayColor" />
    </geompropvalue>
    <multiply name="tinted" type="color3">
      <input name="in1" type="color3" nodename="white" />
      <input name="in2" type="color3" nodename="tint" />
    </multiply>
    <output name="base_out" type="color3" nodename="tinted" />
  </nodegraph>
  <open_pbr_surface name="SR" type="surfaceshader">
    <input name="specular_weight" type="float" value="0" />
    <input name="base_color" type="color3" nodegraph="NG" output="base_out" />
  </open_pbr_surface>
  <surfacematerial name="Look" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR" />
  </surfacematerial>
</materialx>
)";

TEST_CASE("a MaterialX input reads a primitive variable of its mesh, or an image tinted by one") {
    auto check = [](Fixture& f, PrStringView document) {
        ModelCodecs codecs;
        UsdModel u;
        const uint8_t white[16] = {255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
        u.files.push(ModelFile{String("/assets/white.png"), codecs.png({2, 2}, white)});
        Array<uint8_t> look;
        look.append(Span<const uint8_t>(reinterpret_cast<const uint8_t*>(document.data), uint32_t(document.size)));
        u.files.push(ModelFile{String("/assets/look.mtlx"), static_cast<Array<uint8_t>&&>(look)});
        String body("(\n metersPerUnit = 1\n)\ndef Scope \"Looks\" (\n references = @./look.mtlx@</MaterialX/Materials>\n"
            ")\n{\n}\ndef Mesh \"Quad\" {\n rel material:binding = </Looks/Look>\n");
        body.append(kQuad).append(kQuadDisplay).append(kQuadGlow).append("}\n");
        u.open(body, codecs.textures);
        CHECK(prModelDiagnosticCount(u.model) == 0);
        return quadCenter(f, u);
    };
    onEachDevice([&](Fixture& f) {
        // A white surface without gloss in an even light of 1 returns its base color, and emits the glow times the
        // power on top.
        PrVec3 glowing = check(f, kGlowVariableLook);
        CHECK(glowing.x == doctest::Approx(1.25).epsilon(0.05));
        CHECK(glowing.y == doctest::Approx(1.125).epsilon(0.05));
        CHECK(glowing.z == doctest::Approx(1.5).epsilon(0.05));
        // The display color tints the image.
        PrVec3 tinted = check(f, kTintLook);
        CHECK(tinted.x == doctest::Approx(0.25).epsilon(0.05));
        CHECK(tinted.y == doctest::Approx(0.5).epsilon(0.05));
        CHECK(tinted.z == doctest::Approx(1).epsilon(0.05));
    }, testOpenPbrShaderSet());
}

TEST_CASE("Standard Surface's specular rotation turns its anisotropy's tangent by a fraction of a turn") {
    // MaterialX's brushed metal, under a sun that slants across a quad, with its rotation of the tangent set.
    Array<uint8_t> pinned = readFile(format("{}/standard_surface_metal_brushed.mtlx", PR_MATERIALX_DIR));
    String document(PrStringView{reinterpret_cast<const char*>(pinned.begin()), pinned.count()});
    auto render = [&](Fixture& f, PrStringView rotation) {
        UsdModel u;
        String turned;
        PrStringView authored = "<input name=\"specular_rotation\" type=\"float\" value=\"0.0\" />";
        size_t at = prStringFind(document, authored);
        REQUIRE(at != document.count());
        turned.append(PrStringView{document.data(), at});
        turned.append("<input name=\"specular_rotation\" type=\"float\" value=\"").append(rotation).append("\" />");
        turned.append(PrStringView{document.data() + at + authored.size, document.count() - at - authored.size});
        Array<uint8_t> look;
        look.append(Span<const uint8_t>(reinterpret_cast<const uint8_t*>(turned.data()), turned.count()));
        u.files.push(ModelFile{String("/assets/look.mtlx"), static_cast<Array<uint8_t>&&>(look)});
        String body("(\n metersPerUnit = 1\n)\ndef Scope \"Looks\" (\n references = @./look.mtlx@</MaterialX/Materials>\n"
            ")\n{\n}\ndef DistantLight \"Sun\" {\n float inputs:intensity = 1\n"
            " double3 xformOp:rotateXYZ = (-20, 40, 0)\n uniform token[] xformOpOrder = [\"xformOp:rotateXYZ\"]\n}\n"
            "def Mesh \"Quad\" {\n rel material:binding = </Looks/Metal_Brushed>\n");
        body.append(kQuad).append("}\n");
        u.open(body);
        CHECK(prModelDiagnosticCount(u.model) == 0);
        prModelPopulate(u.model, f.render);
        PrRenderScene scene = prRenderSceneCreate(f.render);
        PrModelPlacementDesc desc = prModelPlacementDescDefault(scene);
        PrModelPlacement* placement = prModelPlacementCreate(u.model, &desc);
        prModelWait(u.model);
        prModelUpdate(u.model);
        Image image = f.view(frontView(scene));
        prModelPlacementDestroy(placement);
        prModelPopulate(u.model, nullptr);
        prRenderSceneDestroy(f.render, scene);
        return image;
    };
    onEachDevice([&](Fixture& f) {
        // The lobe is symmetric under a half turn of its tangent, and not under a quarter turn.
        Image still = render(f, "0.0");
        Image half = render(f, "0.5");
        Image quarter = render(f, "0.25");
        Difference same = compare(still, half, 0.01f);
        CHECK(same.over == 0);
        Difference turned = compare(still, quarter, 0.01f);
        CAPTURE(turned.largest);
        CHECK(turned.over > kExtent.x * kExtent.y / 8);
        CHECK(turned.largest > 0.05f);
    }, testOpenPbrShaderSet());
}

TEST_SUITE_END();
