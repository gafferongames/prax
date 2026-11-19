#include "pr/float.h"
#include "pr/geom.h"
#include "pr/raw_string.h"
#include "pr/usd_geom.h"
#include "pr/usd_stage.h"

#include "usd/tests/usd_fixture.h"

using namespace harness;

namespace pr_impl {

using namespace harness;

namespace {

// A stage of one text layer at `/assets/scene.usda`, written as the test's text after the header, and layers that it
// may reference: a text layer at `/assets/parts/part.usda`, and a MaterialX document at `/assets/looks/look.mtlx`.
struct GeomStage {
    String root;
    String part;
    String look;
    PrUsdStage* PR_NULLABLE stage = nullptr;
    PrUsdScratch* scratch = prUsdScratchCreate();

    explicit GeomStage(PrStringView body, PrStringView partBody = "", PrStringView lookDocument = "") {
        root = String("#usda 1.0\n");
        root.append(body);
        part = String("#usda 1.0\n");
        part.append(partBody);
        look = String(lookDocument);
        PrUsdStageOptions options = prUsdStageOptionsDefault();
        options.open = [](void* PR_NULLABLE context, PrStringView location) -> const PrBlobSource* PR_NULLABLE {
            return harnessBlobSource(read(context, location));
        };
        options.openContext = this;
        REQUIRE(prUsdStageCreate("/assets/scene.usda", &options, &stage) == PrStatus_Ok);
        CHECK(prUsdStageErrorCount(stage) == 0);
    }
    GeomStage(const GeomStage&) = delete;
    ~GeomStage() {
        prUsdStageDestroy(stage);
        prUsdScratchDestroy(scratch);
    }

    static PrBlob* PR_NULLABLE read(void* PR_NULLABLE context, PrStringView location) {
        GeomStage* s = static_cast<GeomStage*>(context);
        const String* text = nullptr;
        if (location == "/assets/scene.usda")
            text = &s->root;
        else if (location == "/assets/parts/part.usda")
            text = &s->part;
        else if (location == "/assets/looks/look.mtlx")
            text = &s->look;
        if (text == nullptr)
            return nullptr;
        return prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(text->data()), text->count()});
    }

    PrUsdPrimIndex prim(PrStringView path) const {
        PrUsdPrimIndex index = prUsdStageFindPrim(stage, path);
        if (index == kPrUsdNone)
            index = prUsdStageFindProxy(stage, path);
        REQUIRE(index != kPrUsdNone);
        return index;
    }

    PrDMatrix4 local(PrStringView path, double time = kPrF64Nan, bool* resets = nullptr) const {
        PrDMatrix4 m{};
        bool reset = false;
        CHECK(prUsdGeomLocalTransform(stage, prim(path), time, scratch, &m, &reset));
        if (resets != nullptr)
            *resets = reset;
        return m;
    }
};

// Returns `point` mapped through `m`.
PrDVec3 mapPoint(const PrDMatrix4& m, PrDVec3 p) {
    return PrDVec3{
        m.m[0][0] * p.x + m.m[0][1] * p.y + m.m[0][2] * p.z + m.m[0][3],
        m.m[1][0] * p.x + m.m[1][1] * p.y + m.m[1][2] * p.z + m.m[1][3],
        m.m[2][0] * p.x + m.m[2][1] * p.y + m.m[2][2] * p.z + m.m[2][3],
    };
}

void checkPoint(PrDVec3 actual, PrDVec3 expected) {
    CHECK(actual.x == doctest::Approx(expected.x).epsilon(1e-6).scale(1));
    CHECK(actual.y == doctest::Approx(expected.y).epsilon(1e-6).scale(1));
    CHECK(actual.z == doctest::Approx(expected.z).epsilon(1e-6).scale(1));
}

// Returns MaterialX's own document `name`, which the tests pin (cmake/test_data.cmake).
String materialxDocument(PrStringView name) {
    Array<uint8_t> bytes = readFile(format("{}/{}", PR_MATERIALX_DIR, name));
    return String(PrStringView{reinterpret_cast<const char*>(bytes.begin()), bytes.count()});
}

// Checks that `transform` maps the coordinates (u, v) where the specification's place2d does ("place2d"): less the
// pivot, then for SRT divided by the scale, rotated counterclockwise and less the offset, or for TRS less the offset,
// rotated and divided by the scale, and the pivot added back.
void checkPlacement(const PrAffine2& transform, bool trs, const double pivot[2], const double scale[2], double degrees,
        const double offset[2]) {
    const double points[3][2] = {{0, 0}, {1, 0.25}, {0.3, 0.9}};
    double c = prF64Cos(degrees * kPrF64Pi / 180);
    double s = prF64Sin(degrees * kPrF64Pi / 180);
    for (const double* p : points) {
        double x = p[0] - pivot[0];
        double y = p[1] - pivot[1];
        if (trs) {
            x -= offset[0];
            y -= offset[1];
        } else {
            x /= scale[0];
            y /= scale[1];
        }
        double rx = c * x - s * y;
        double ry = s * x + c * y;
        if (trs) {
            rx /= scale[0];
            ry /= scale[1];
        } else {
            rx -= offset[0];
            ry -= offset[1];
        }
        double u = double(transform.a) * p[0] + double(transform.c) * p[1] + double(transform.e);
        double v = double(transform.b) * p[0] + double(transform.d) * p[1] + double(transform.f);
        CHECK(u == doctest::Approx(rx + pivot[0]).epsilon(1e-5));
        CHECK(v == doctest::Approx(ry + pivot[1]).epsilon(1e-5));
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a stage's metadata has OpenUSD's fallbacks") {
    {
        GeomStage s("");
        PrUsdGeomStageInfo info = prUsdGeomStageInfo(s.stage);
        CHECK(info.upAxis == PrUsdUpAxis_Y);
        CHECK(info.metersPerUnit == 0.01);
        CHECK(!info.hasTimeRange);
        CHECK(info.timeCodesPerSecond == 24);
    }
    GeomStage s("(\n upAxis = \"Z\"\n metersPerUnit = 1\n startTimeCode = 1\n endTimeCode = 48\n"
        " timeCodesPerSecond = 12\n)\n");
    PrUsdGeomStageInfo info = prUsdGeomStageInfo(s.stage);
    CHECK(info.upAxis == PrUsdUpAxis_Z);
    CHECK(info.metersPerUnit == 1);
    CHECK(info.hasTimeRange);
    CHECK(info.startTimeCode == 1);
    CHECK(info.endTimeCode == 48);
    CHECK(info.timeCodesPerSecond == 12);
}

TEST_CASE("a prim's type is a schema that the module reads, or other") {
    GeomStage s("def Xform \"W\" {\n def Mesh \"M\" {\n def GeomSubset \"S\" {\n}\n}\n def Scope \"Looks\" {\n"
        " def Material \"Mat\" {\n def Shader \"Surface\" {\n}\n}\n}\n def SphereLight \"L\" {\n}\n"
        " def Camera \"C\" {\n}\n def Cube \"Box\" {\n}\n def Points \"P\" {\n}\n def \"Untyped\" {\n}\n"
        " def Bogus \"B\" {\n}\n}\n");
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W")) == PrUsdGeomKind_Xform);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/M")) == PrUsdGeomKind_Mesh);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/M/S")) == PrUsdGeomKind_Subset);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/Looks")) == PrUsdGeomKind_Scope);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/Looks/Mat")) == PrUsdGeomKind_Material);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/Looks/Mat/Surface")) == PrUsdGeomKind_Shader);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/L")) == PrUsdGeomKind_Light);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/C")) == PrUsdGeomKind_Camera);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/Box")) == PrUsdGeomKind_Shape);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/P")) == PrUsdGeomKind_Gprim);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/Untyped")) == PrUsdGeomKind_Other);
    CHECK(prUsdGeomPrimKind(s.stage, s.prim("/W/B")) == PrUsdGeomKind_Other);
    CHECK(prUsdGeomKindIsXformable(PrUsdGeomKind_Mesh));
    CHECK(!prUsdGeomKindIsXformable(PrUsdGeomKind_Scope));
}

TEST_CASE("a purpose and an invisible prim are the prim's own") {
    GeomStage s("def Xform \"W\" {\n token purpose = \"proxy\"\n token visibility.timeSamples = {\n"
        " 0: \"inherited\",\n 10: \"invisible\",\n }\n def Mesh \"M\" {\n}\n}\n");
    bool authored = false;
    CHECK(prUsdGeomPurpose(s.stage, s.prim("/W"), s.scratch, &authored) == PrUsdGeomPurpose_Proxy);
    CHECK(authored);
    CHECK(prUsdGeomPurpose(s.stage, s.prim("/W/M"), s.scratch, &authored) == PrUsdGeomPurpose_Default);
    CHECK(!authored);
    CHECK(!prUsdGeomIsInvisible(s.stage, s.prim("/W"), 0, s.scratch));
    CHECK(prUsdGeomIsInvisible(s.stage, s.prim("/W"), 10, s.scratch));
    CHECK(!prUsdGeomIsInvisible(s.stage, s.prim("/W/M"), 10, s.scratch));
}

TEST_CASE("a transform is its operations in order, the last applied first") {
    GeomStage s("def Xform \"T\" {\n double3 xformOp:translate = (1, 2, 3)\n float3 xformOp:rotateXYZ = (90, 0, 0)\n"
        " float3 xformOp:scale = (2, 2, 2)\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateXYZ\", \"xformOp:scale\"]\n}\n"
        "def Xform \"Pivot\" {\n float xformOp:rotateZ = 90\n float3 xformOp:translate:pivot = (1, 0, 0)\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate:pivot\", \"xformOp:rotateZ\","
        " \"!invert!xformOp:translate:pivot\"]\n}\n"
        "def Xform \"Orient\" {\n quatf xformOp:orient = (0.70710677, 0, 0.70710677, 0)\n"
        " uniform token[] xformOpOrder = [\"xformOp:orient\"]\n}\n"
        "def Xform \"Matrix\" {\n matrix4d xformOp:transform = ( (1, 0, 0, 0), (0, 1, 0, 0), (0, 0, 1, 0),"
        " (5, 6, 7, 1) )\n uniform token[] xformOpOrder = [\"xformOp:transform\"]\n}\n"
        "def Xform \"Euler\" {\n float3 xformOp:rotateZYX = (90, 90, 0)\n"
        " uniform token[] xformOpOrder = [\"xformOp:rotateZYX\"]\n}\n"
        "def Xform \"Reset\" {\n double3 xformOp:translate = (1, 0, 0)\n"
        " uniform token[] xformOpOrder = [\"!resetXformStack!\", \"xformOp:translate\"]\n}\n"
        "def Xform \"None\" {\n}\n");
    // Scaled by 2, turned a quarter about x, then moved: (1, 1, 0) becomes (2, 0, 2) + (1, 2, 3).
    checkPoint(mapPoint(s.local("/T"), {1, 1, 0}), {3, 2, 5});
    // A quarter turn about z around the pivot at (1, 0, 0) takes (2, 0, 0) to (1, 1, 0).
    checkPoint(mapPoint(s.local("/Pivot"), {2, 0, 0}), {1, 1, 0});
    // A quarter turn about y takes +x to -z.
    checkPoint(mapPoint(s.local("/Orient"), {1, 0, 0}), {0, 0, -1});
    // A matrix is in USD's rows, with its translation in the last.
    checkPoint(mapPoint(s.local("/Matrix"), {0, 0, 0}), {5, 6, 7});
    // rotateZYX turns about z, then y, then x: y's quarter turn takes +x to -z, and x's takes -z to +y.
    checkPoint(mapPoint(s.local("/Euler"), {1, 0, 0}), {0, 1, 0});
    bool resets = false;
    checkPoint(mapPoint(s.local("/Reset", kPrF64Nan, &resets), {0, 0, 0}), {1, 0, 0});
    CHECK(resets);
    s.local("/None", kPrF64Nan, &resets);
    CHECK(!resets);
}

TEST_CASE("an order that names what the prim lacks, or what the schema does not know, fails") {
    GeomStage s("def Xform \"Missing\" {\n uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n"
        "def Xform \"Unknown\" {\n float3 xformOp:shear = (1, 1, 1)\n"
        " uniform token[] xformOpOrder = [\"xformOp:shear\"]\n}\n"
        "def Xform \"Typed\" {\n string xformOp:translate = \"far\"\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
    for (PrStringView path : usdEach<PrStringView>("/Missing", "/Unknown", "/Typed")) {
        CAPTURE(path);
        PrDMatrix4 m{};
        bool resets = false;
        CHECK(!prUsdGeomLocalTransform(s.stage, s.prim(path), kPrF64Nan, s.scratch, &m, &resets));
        CHECK(m.m[0][3] == 0);
        CHECK(m.m[0][0] == 1);
    }
}

TEST_CASE("an animated transform interpolates between its samples, whose times are listed once") {
    GeomStage s("def Xform \"A\" {\n double3 xformOp:translate.timeSamples = {\n 0: (0, 0, 0),\n 10: (10, 0, 0),\n }\n"
        " float xformOp:rotateY.timeSamples = {\n 5: 0,\n 10: 90,\n 20: 180,\n }\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\", \"xformOp:rotateY\"]\n}\n"
        "def Xform \"Still\" {\n double3 xformOp:translate = (1, 0, 0)\n"
        " uniform token[] xformOpOrder = [\"xformOp:translate\"]\n}\n");
    checkPoint(mapPoint(s.local("/A", 5), {0, 0, 0}), {5, 0, 0});
    checkPoint(mapPoint(s.local("/A", 10), {1, 0, 0}), {10, 0, -1});
    CHECK(prUsdGeomTransformVaries(s.stage, s.prim("/A"), s.scratch));
    CHECK(!prUsdGeomTransformVaries(s.stage, s.prim("/Still"), s.scratch));
    double times[8];
    REQUIRE(prUsdGeomTransformTimes(s.stage, s.prim("/A"), 0, 100, s.scratch, times, 8) == 4);
    CHECK(times[0] == 0);
    CHECK(times[1] == 5);
    CHECK(times[2] == 10);
    CHECK(times[3] == 20);
    CHECK(prUsdGeomTransformTimes(s.stage, s.prim("/A"), 6, 15, s.scratch, times, 8) == 1);
    CHECK(prUsdGeomTransformTimes(s.stage, s.prim("/Still"), 0, 100, s.scratch, times, 8) == 0);
}

TEST_CASE("a mesh reads with its topology checked") {
    GeomStage s("def Mesh \"Quad\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (1, 1, 0), (0, 1, 0), (2, 0, 0)]\n"
        " int[] faceVertexCounts = [4, 3]\n int[] faceVertexIndices = [0, 1, 2, 3, 1, 4, 2]\n"
        " int[] holeIndices = [1]\n uniform token orientation = \"leftHanded\"\n uniform bool doubleSided = 1\n"
        " uniform token subdivisionScheme = \"none\"\n"
        " normal3f[] normals = [(0, 0, 1), (0, 0, 1)] (\n interpolation = \"uniform\"\n )\n}\n"
        "def Mesh \"Primvar\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n"
        " normal3f[] normals = [(1, 0, 0)] (\n interpolation = \"constant\"\n )\n"
        " normal3f[] primvars:normals = [(0, 0, 1)] (\n interpolation = \"faceVarying\"\n )\n"
        " int[] primvars:normals:indices = [0, 0, 0]\n}\n"
        "def Mesh \"Short\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1]\n}\n"
        "def Mesh \"Far\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 3]\n}\n"
        "def Mesh \"Line\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0)]\n"
        " int[] faceVertexCounts = [2]\n int[] faceVertexIndices = [0, 1]\n}\n"
        "def Mesh \"Normals\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n"
        " normal3f[] normals = [(0, 0, 1)]\n}\n"
        "def Mesh \"Empty\" {\n}\n");
    PrUsdGeomMesh mesh{};
    REQUIRE(prUsdGeomMeshRead(s.stage, s.prim("/Quad"), kPrF64Nan, s.scratch, &mesh) == PrStatus_Ok);
    CHECK(mesh.pointCount == 5);
    CHECK(mesh.faceCount == 2);
    CHECK(mesh.cornerCount == 7);
    CHECK(mesh.holeCount == 1);
    CHECK(mesh.leftHanded);
    CHECK(mesh.doubleSided);
    CHECK(mesh.subdivision == PrUsdGeomSubdivision_None);
    CHECK(mesh.normals.interpolation == PrUsdGeomInterpolation_Uniform);
    CHECK(mesh.normals.value.count == 2);
    CHECK(mesh.points[4].x == 2);

    REQUIRE(prUsdGeomMeshRead(s.stage, s.prim("/Primvar"), kPrF64Nan, s.scratch, &mesh) == PrStatus_Ok);
    CHECK(!mesh.leftHanded);
    CHECK(mesh.subdivision == PrUsdGeomSubdivision_CatmullClark);
    CHECK(mesh.normals.interpolation == PrUsdGeomInterpolation_FaceVarying);
    CHECK(mesh.normals.indexCount == 3);
    CHECK(static_cast<const PrVec3*>(mesh.normals.value.data)[0].z == 1);

    CHECK(prUsdGeomMeshRead(s.stage, s.prim("/Short"), kPrF64Nan, s.scratch, &mesh) == PrStatus_InvalidData);
    CHECK(prUsdGeomMeshRead(s.stage, s.prim("/Far"), kPrF64Nan, s.scratch, &mesh) == PrStatus_InvalidData);
    CHECK(prUsdGeomMeshRead(s.stage, s.prim("/Line"), kPrF64Nan, s.scratch, &mesh) == PrStatus_InvalidData);
    // Normals of the vertex interpolation, the attribute's fallback, need one for each point.
    CHECK(prUsdGeomMeshRead(s.stage, s.prim("/Normals"), kPrF64Nan, s.scratch, &mesh) == PrStatus_InvalidData);
    CHECK(prUsdGeomMeshRead(s.stage, s.prim("/Empty"), kPrF64Nan, s.scratch, &mesh) == PrStatus_NotFound);
}

TEST_CASE("a primitive variable reads with its indices, its interpolation and its element size") {
    GeomStage s("def Mesh \"M\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0)]\n"
        " int[] faceVertexCounts = [3]\n int[] faceVertexIndices = [0, 1, 2]\n"
        " texCoord2f[] primvars:st = [(0, 0), (1, 1)] (\n interpolation = \"faceVarying\"\n )\n"
        " int[] primvars:st:indices = [0, 1, 1]\n"
        " color3f[] primvars:displayColor = [(1, 0, 0)]\n"
        " float[] primvars:pairs = [1, 2, 3, 4, 5, 6] (\n interpolation = \"vertex\"\n elementSize = 2\n )\n"
        " float[] primvars:bad = [1] (\n interpolation = \"vertex\"\n )\n int[] primvars:bad:indices = [0, 1, 0]\n"
        "}\n");
    PrUsdGeomMesh mesh{};
    REQUIRE(prUsdGeomMeshRead(s.stage, s.prim("/M"), kPrF64Nan, s.scratch, &mesh) == PrStatus_Ok);
    PrUsdGeomPrimvar st{};
    REQUIRE(prUsdGeomPrimvarRead(s.stage, s.prim("/M"), "st", kPrF64Nan, s.scratch, &st));
    CHECK(st.interpolation == PrUsdGeomInterpolation_FaceVarying);
    CHECK(st.value.type == PrUsdType_Float2);
    CHECK(st.indexCount == 3);
    CHECK(prUsdGeomPrimvarFits(&mesh, &st));
    PrUsdGeomPrimvar color{};
    REQUIRE(prUsdGeomPrimvarRead(s.stage, s.prim("/M"), "displayColor", kPrF64Nan, s.scratch, &color));
    CHECK(color.interpolation == PrUsdGeomInterpolation_Constant);
    CHECK(prUsdGeomPrimvarFits(&mesh, &color));
    PrUsdGeomPrimvar pairs{};
    REQUIRE(prUsdGeomPrimvarRead(s.stage, s.prim("/M"), "pairs", kPrF64Nan, s.scratch, &pairs));
    CHECK(pairs.elementSize == 2);
    CHECK(prUsdGeomPrimvarFits(&mesh, &pairs));
    PrUsdGeomPrimvar bad{};
    CHECK(!prUsdGeomPrimvarRead(s.stage, s.prim("/M"), "bad", kPrF64Nan, s.scratch, &bad));
    CHECK(!prUsdGeomPrimvarRead(s.stage, s.prim("/M"), "missing", kPrF64Nan, s.scratch, &bad));
}

TEST_CASE("a mesh's subsets of a family are its children that name its faces") {
    GeomStage s("def Mesh \"M\" {\n point3f[] points = [(0, 0, 0), (1, 0, 0), (0, 1, 0), (1, 1, 0)]\n"
        " int[] faceVertexCounts = [3, 3]\n int[] faceVertexIndices = [0, 1, 2, 1, 3, 2]\n"
        " def GeomSubset \"A\" {\n uniform token elementType = \"face\"\n"
        " uniform token familyName = \"materialBind\"\n int[] indices = [1]\n }\n"
        " def GeomSubset \"Other\" {\n uniform token familyName = \"parts\"\n int[] indices = [0]\n }\n"
        " def GeomSubset \"Past\" {\n uniform token familyName = \"materialBind\"\n int[] indices = [2]\n }\n"
        " def GeomSubset \"Points\" {\n uniform token elementType = \"point\"\n"
        " uniform token familyName = \"materialBind\"\n int[] indices = [0]\n }\n}\n");
    PrUsdGeomSubset subsets[4];
    REQUIRE(prUsdGeomSubsets(s.stage, s.prim("/M"), 2, "materialBind", kPrF64Nan, s.scratch, subsets, 4) == 1);
    CHECK(subsets[0].prim == s.prim("/M/A"));
    CHECK(subsets[0].indexCount == 1);
    CHECK(subsets[0].indices[0] == 1);
}

TEST_CASE("a shape has its schema's fallbacks, and its axis") {
    GeomStage s("def Sphere \"S\" {\n}\ndef Capsule \"C\" {\n token axis = \"Y\"\n}\ndef Plane \"P\" {\n"
        " double width = 3\n token axis = \"X\"\n}\ndef Cylinder_1 \"F\" {\n double radiusTop = 0.25\n}\n"
        "def Cube \"Bad\" {\n double size = -1\n}\ndef Mesh \"M\" {\n}\n");
    PrUsdGeomShape shape{};
    REQUIRE(prUsdGeomShapeRead(s.stage, s.prim("/S"), 0, s.scratch, &shape));
    CHECK(shape.kind == PrUsdGeomShapeKind_Sphere);
    CHECK(shape.radius == 1);
    CHECK(shape.axis == 2);
    CHECK(!shape.doubleSided);
    REQUIRE(prUsdGeomShapeRead(s.stage, s.prim("/C"), 0, s.scratch, &shape));
    CHECK(shape.kind == PrUsdGeomShapeKind_Capsule);
    CHECK(shape.radius == 0.5);
    CHECK(shape.height == 1);
    CHECK(shape.axis == 1);
    // A plane is double sided unless it says otherwise.
    REQUIRE(prUsdGeomShapeRead(s.stage, s.prim("/P"), 0, s.scratch, &shape));
    CHECK(shape.kind == PrUsdGeomShapeKind_Plane);
    CHECK(shape.width == 3);
    CHECK(shape.length == 2);
    CHECK(shape.axis == 0);
    CHECK(shape.doubleSided);
    REQUIRE(prUsdGeomShapeRead(s.stage, s.prim("/F"), 0, s.scratch, &shape));
    CHECK(shape.kind == PrUsdGeomShapeKind_Cylinder);
    CHECK(shape.radiusBottom == 1);
    CHECK(shape.radiusTop == 0.25);
    CHECK(!prUsdGeomShapeRead(s.stage, s.prim("/Bad"), 0, s.scratch, &shape));
    CHECK(!prUsdGeomShapeRead(s.stage, s.prim("/M"), 0, s.scratch, &shape));
}

TEST_CASE("a camera has the schema's fallbacks") {
    GeomStage s("def Camera \"Default\" {\n}\ndef Camera \"Wide\" {\n float focalLength = 18\n"
        " float2 clippingRange = (0.1, 100)\n token projection = \"orthographic\"\n}\n");
    PrUsdGeomCamera camera{};
    REQUIRE(prUsdGeomCameraRead(s.stage, s.prim("/Default"), kPrF64Nan, s.scratch, &camera));
    CHECK(!camera.orthographic);
    CHECK(camera.focalLength == 50);
    CHECK(camera.horizontalAperture == doctest::Approx(20.955));
    CHECK(camera.verticalAperture == doctest::Approx(15.2908));
    CHECK(camera.nearClip == 1);
    CHECK(camera.farClip == 1000000);
    REQUIRE(prUsdGeomCameraRead(s.stage, s.prim("/Wide"), kPrF64Nan, s.scratch, &camera));
    CHECK(camera.orthographic);
    CHECK(camera.focalLength == 18);
    CHECK(camera.nearClip == doctest::Approx(0.1));
    CHECK(camera.farClip == 100);
}

TEST_CASE("a point instancer places each instance of a prototype by its scale, orientation and position") {
    GeomStage s("def PointInstancer \"P\" {\n rel prototypes = [</P/Protos/A>, </P/Protos/B>]\n"
        " int[] protoIndices = [0, 1, 0]\n point3f[] positions = [(1, 0, 0), (2, 0, 0), (3, 0, 0)]\n"
        " quath[] orientations = [(1, 0, 0, 0), (0, 0, 0, 1), (1, 0, 0, 0)]\n"
        " float3[] scales = [(1, 1, 1), (2, 2, 2), (1, 1, 1)]\n int64[] invisibleIds = [2]\n"
        " def Scope \"Protos\" {\n def Xform \"A\" {\n}\n def Xform \"B\" {\n}\n }\n}\n"
        "def PointInstancer \"Bad\" {\n rel prototypes = [</P/Protos/A>]\n int[] protoIndices = [0, 1]\n}\n"
        "def PointInstancer \"Short\" {\n rel prototypes = [</P/Protos/A>]\n int[] protoIndices = [0, 0]\n"
        " point3f[] positions = [(1, 0, 0)]\n}\n");
    PrUsdGeomPointInstancer instancer{};
    REQUIRE(prUsdGeomPointInstancerRead(s.stage, s.prim("/P"), kPrF64Nan, s.scratch, &instancer) == PrStatus_Ok);
    REQUIRE(instancer.prototypeCount == 2);
    CHECK(instancer.prototypes[1] == s.prim("/P/Protos/B"));
    CHECK(instancer.instanceCount == 3);
    // The second is doubled, turned a half turn about z, and moved to (2, 0, 0): +y becomes (2, -2, 0). Halves hold
    // that quaternion exactly.
    checkPoint(mapPoint(prUsdGeomInstanceTransform(&instancer, 1), {0, 1, 0}), {2, -2, 0});
    checkPoint(mapPoint(prUsdGeomInstanceTransform(&instancer, 0), {0, 0, 0}), {1, 0, 0});
    CHECK(prUsdGeomInstanceIsVisible(&instancer, 0));
    CHECK(!prUsdGeomInstanceIsVisible(&instancer, 2));
    CHECK(prUsdGeomPointInstancerRead(s.stage, s.prim("/Bad"), kPrF64Nan, s.scratch, &instancer) ==
        PrStatus_InvalidData);
    CHECK(prUsdGeomPointInstancerRead(s.stage, s.prim("/Short"), kPrF64Nan, s.scratch, &instancer) ==
        PrStatus_InvalidData);
}

TEST_CASE("a binding is the prim's own, for the preview purpose first") {
    GeomStage s("def Scope \"Looks\" {\n def Material \"Red\" {\n}\n def Material \"Blue\" {\n}\n"
        " def Scope \"NotMaterial\" {\n}\n}\n"
        "def Xform \"W\" (\n prepend apiSchemas = [\"MaterialBindingAPI\"]\n) {\n"
        " rel material:binding = </Looks/Red> (\n bindMaterialAs = \"strongerThanDescendants\"\n )\n"
        " def Mesh \"Both\" {\n rel material:binding = </Looks/Red>\n rel material:binding:preview = </Looks/Blue>\n"
        " }\n def Mesh \"Wrong\" {\n rel material:binding = </Looks/NotMaterial>\n }\n"
        " def Mesh \"None\" {\n }\n}\n");
    PrUsdShadeBinding binding = prUsdShadeDirectBinding(s.stage, s.prim("/W"), s.scratch);
    CHECK(binding.authored);
    CHECK(binding.material == s.prim("/Looks/Red"));
    CHECK(binding.strongerThanDescendants);
    binding = prUsdShadeDirectBinding(s.stage, s.prim("/W/Both"), s.scratch);
    CHECK(binding.material == s.prim("/Looks/Blue"));
    CHECK(!binding.strongerThanDescendants);
    binding = prUsdShadeDirectBinding(s.stage, s.prim("/W/Wrong"), s.scratch);
    CHECK(binding.authored);
    CHECK(binding.material == kPrUsdNone);
    binding = prUsdShadeDirectBinding(s.stage, s.prim("/W/None"), s.scratch);
    CHECK(!binding.authored);
    // The binding of /W is stronger than its descendants', so it decides for each of them.
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/W/None"), s.scratch) == s.prim("/Looks/Red"));
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/W/Both"), s.scratch) == s.prim("/Looks/Red"));
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/Looks"), s.scratch) == kPrUsdNone);
}

TEST_CASE("a prim takes its nearest ancestor's binding where none is stronger") {
    GeomStage s("def Scope \"Looks\" {\n def Material \"Red\" {\n}\n def Material \"Blue\" {\n}\n}\n"
        "def Xform \"W\" {\n rel material:binding = </Looks/Red>\n"
        " def Xform \"G\" {\n def Mesh \"Own\" {\n rel material:binding = </Looks/Blue>\n }\n"
        " def Mesh \"Inherits\" {\n }\n }\n}\n");
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/W/G/Own"), s.scratch) == s.prim("/Looks/Blue"));
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/W/G/Inherits"), s.scratch) == s.prim("/Looks/Red"));
}

TEST_CASE("a preview surface follows its network to values and textures") {
    // The material's own input names the primitive variable, a node graph passes a texture's output on, and the texture
    // that it reads is in the referenced layer, relative to which its file resolves.
    GeomStage s("def Material \"Mat\" (\n prepend references = @parts/part.usda@</Looks>\n) {\n"
        " token outputs:surface.connect = </Mat/Surface.outputs:surface>\n"
        " def Shader \"Surface\" {\n uniform token info:id = \"UsdPreviewSurface\"\n"
        " color3f inputs:diffuseColor.connect = </Mat/Graph.outputs:color>\n"
        " float inputs:roughness = 0.25\n float inputs:metallic.connect = </Mat/Tex.outputs:b>\n"
        " int inputs:useSpecularWorkflow = 1\n float inputs:opacity.connect = </Mat.inputs:opacity>\n"
        " token outputs:surface\n }\n float inputs:opacity = 0.5\n}\n"
        "def Material \"Plain\" {\n token outputs:surface.connect = </Plain/S.outputs:surface>\n"
        " def Shader \"S\" {\n uniform token info:id = \"UsdPreviewSurface\"\n token outputs:surface\n }\n}\n"
        "def Material \"Painted\" {\n token outputs:surface.connect = </Painted/S.outputs:surface>\n"
        " def Shader \"S\" {\n uniform token info:id = \"UsdPreviewSurface\"\n"
        " color3f inputs:diffuseColor.connect = </Painted/R.outputs:result>\n token outputs:surface\n }\n"
        " def Shader \"R\" {\n uniform token info:id = \"UsdPrimvarReader_float3\"\n"
        " string inputs:varname = \"displayColor\"\n float3 inputs:fallback = (0.5, 0.5, 0.5)\n"
        " float3 outputs:result\n }\n}\n"
        "def Material \"Other\" {\n token outputs:surface.connect = </Other/S.outputs:surface>\n"
        " def Shader \"S\" {\n uniform token info:id = \"ND_standard_surface_surfaceshader\"\n"
        " token outputs:surface\n }\n}\n", "def Scope \"Looks\" {\n string inputs:frame = \"uv\"\n"
        " def NodeGraph \"Graph\" {\n color3f outputs:color.connect = </Looks/Tex.outputs:rgb>\n }\n"
        " def Shader \"Tex\" {\n uniform token info:id = \"UsdUVTexture\"\n asset inputs:file = @../textures/wood.png@\n"
        " token inputs:wrapS = \"mirror\"\n token inputs:wrapT = \"clamp\"\n token inputs:sourceColorSpace = \"raw\"\n"
        " float4 inputs:scale = (2, 2, 2, 1)\n float4 inputs:bias = (-1, -1, -1, 0)\n"
        " float2 inputs:st.connect = </Looks/Place.outputs:result>\n float3 outputs:rgb\n float outputs:b\n }\n"
        " def Shader \"Place\" {\n uniform token info:id = \"UsdTransform2d\"\n float inputs:rotation = 45\n"
        " float2 inputs:scale = (2, 3)\n float2 inputs:in.connect = </Looks/Reader.outputs:result>\n"
        " float2 outputs:result\n }\n"
        " def Shader \"Reader\" {\n uniform token info:id = \"UsdPrimvarReader_float2\"\n"
        " string inputs:varname.connect = </Looks.inputs:frame>\n float2 outputs:result\n }\n}\n");
    PrUsdPreviewSurface surface{};
    REQUIRE(prUsdPreviewSurfaceRead(s.stage, s.prim("/Mat"), kPrF64Nan, s.scratch, &surface));
    CHECK(surface.shader == s.prim("/Mat/Surface"));
    CHECK(surface.useSpecularWorkflow);
    CHECK(surface.inputs[PrUsdPreviewInput_Roughness].value[0] == 0.25f);
    CHECK(!surface.inputs[PrUsdPreviewInput_Roughness].textured);
    CHECK(surface.inputs[PrUsdPreviewInput_Opacity].value[0] == 0.5f);
    CHECK(surface.inputs[PrUsdPreviewInput_Ior].value[0] == 1.5f);
    const PrUsdPreviewInputValue& diffuse = surface.inputs[PrUsdPreviewInput_DiffuseColor];
    REQUIRE(diffuse.textured);
    CHECK(diffuse.texture.channel == PrUsdTextureChannel_Rgb);
    CHECK(diffuse.texture.file == "../textures/wood.png");
    CHECK(diffuse.texture.wrapS == PrUsdTextureWrap_Mirror);
    CHECK(diffuse.texture.wrapT == PrUsdTextureWrap_Clamp);
    CHECK(diffuse.texture.colorSpace == PrUsdTextureColorSpace_Raw);
    CHECK(diffuse.texture.scale[0] == 2);
    CHECK(diffuse.texture.bias[0] == -1);
    CHECK(diffuse.texture.primvar == "uv");
    // Scaled by (2, 3), then turned by 45 degrees counterclockwise: (1, 0) goes to (sqrt 2, sqrt 2), and (0, 1) to (-3
    // / sqrt 2, 3 / sqrt 2).
    const PrAffine2& t = diffuse.texture.transform;
    CHECK(t.a == doctest::Approx(1.41421356));
    CHECK(t.b == doctest::Approx(1.41421356));
    CHECK(t.c == doctest::Approx(-2.12132034));
    CHECK(t.d == doctest::Approx(2.12132034));
    CHECK(t.e == 0);
    CHECK(t.f == 0);
    PrRawString location = prUsdTextureLocation(s.stage, &diffuse.texture, kPrF64Nan);
    CHECK(prRawStringView(&location) == "/assets/textures/wood.png");
    prRawStringReset(&location);
    const PrUsdPreviewInputValue& metallic = surface.inputs[PrUsdPreviewInput_Metallic];
    REQUIRE(metallic.textured);
    CHECK(metallic.texture.channel == PrUsdTextureChannel_B);

    REQUIRE(prUsdPreviewSurfaceRead(s.stage, s.prim("/Plain"), kPrF64Nan, s.scratch, &surface));
    CHECK(!surface.useSpecularWorkflow);
    CHECK(surface.inputs[PrUsdPreviewInput_DiffuseColor].value[1] == 0.18f);
    CHECK(surface.inputs[PrUsdPreviewInput_ClearcoatRoughness].value[0] == 0.01f);
    CHECK(surface.inputs[PrUsdPreviewInput_Normal].value[2] == 1);
    // A reader of a primitive variable gives the input that variable, and its own fallback.
    REQUIRE(prUsdPreviewSurfaceRead(s.stage, s.prim("/Painted"), kPrF64Nan, s.scratch, &surface));
    CHECK(surface.inputs[PrUsdPreviewInput_DiffuseColor].primvar == "displayColor");
    CHECK(surface.inputs[PrUsdPreviewInput_DiffuseColor].value[0] == 0.5f);
    CHECK(!surface.inputs[PrUsdPreviewInput_DiffuseColor].textured);
    CHECK(!prUsdPreviewSurfaceRead(s.stage, s.prim("/Other"), kPrF64Nan, s.scratch, &surface));
}

TEST_CASE("a dome light reads its texture, its format and its pole") {
    GeomStage s("(\n upAxis = \"Z\"\n)\ndef DomeLight \"Dome\" {\n asset inputs:texture:file = @sky/day.hdr@\n"
        " token inputs:texture:format = \"latlong\"\n}\ndef DomeLight_1 \"Dome1\" {\n"
        " token inputs:texture:format = \"mirroredBall\"\n}\ndef DomeLight_1 \"Pole\" {\n uniform token poleAxis = \"Y\"\n}\n");
    PrUsdLuxLight light{};
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/Dome"), 0, s.scratch, &light));
    CHECK(light.kind == PrUsdLightKind_Dome);
    CHECK(light.textureFile == "sky/day.hdr");
    CHECK(light.textureFormat == PrUsdLuxTextureFormat_Latlong);
    CHECK(light.poleAxis == PrUsdLuxPoleAxis_Y);
    PrRawString location = prUsdLuxTextureLocation(s.stage, s.prim("/Dome"), &light, 0);
    CHECK(prRawStringView(&location) == "/assets/sky/day.hdr");
    prRawStringReset(&location);
    // A DomeLight_1's pole is the stage's up axis unless it names one.
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/Dome1"), 0, s.scratch, &light));
    CHECK(light.kind == PrUsdLightKind_Dome);
    CHECK(light.textureFormat == PrUsdLuxTextureFormat_MirroredBall);
    CHECK(light.poleAxis == PrUsdLuxPoleAxis_Scene);
    CHECK(light.textureFileProperty == kPrUsdNone);
    location = prUsdLuxTextureLocation(s.stage, s.prim("/Dome1"), &light, 0);
    CHECK(prRawStringView(&location).size == 0);
    prRawStringReset(&location);
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/Pole"), 0, s.scratch, &light));
    CHECK(light.poleAxis == PrUsdLuxPoleAxis_Y);
}

TEST_CASE("a light reads its inputs by their names in the namespace, as UsdLux does") {
    GeomStage s("def SphereLight \"New\" {\n float inputs:intensity = 30\n float inputs:exposure = 2\n"
        " color3f inputs:color = (1, 0.5, 0.25)\n float inputs:radius = 2\n bool inputs:normalize = 1\n"
        " float inputs:shaping:cone:angle = 30\n}\n"
        "def DistantLight \"Old\" {\n float intensity = 3\n float angle = 1\n}\n"
        "def DistantLight \"Sun\" {\n}\ndef Xform \"NotLight\" {\n}\n");
    PrUsdLuxLight light{};
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/New"), kPrF64Nan, s.scratch, &light));
    CHECK(light.kind == PrUsdLightKind_Sphere);
    CHECK(light.intensity == 30);
    CHECK(light.exposure == 2);
    CHECK(light.color.y == 0.5f);
    CHECK(light.radius == 2);
    CHECK(light.normalize);
    CHECK(light.coneAngle == 30);
    // The names that came before the namespace are no inputs of the schema's.
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/Old"), kPrF64Nan, s.scratch, &light));
    CHECK(light.kind == PrUsdLightKind_Distant);
    CHECK(light.intensity == 50000);
    CHECK(light.angle == doctest::Approx(0.53));
    REQUIRE(prUsdLuxLightRead(s.stage, s.prim("/Sun"), kPrF64Nan, s.scratch, &light));
    CHECK(light.intensity == 50000);
    CHECK(light.angle == doctest::Approx(0.53));
    CHECK(!prUsdLuxLightRead(s.stage, s.prim("/NotLight"), kPrF64Nan, s.scratch, &light));
}

// A MaterialX document with a node graph of images and of the arithmetic that folds into an image's map, a standard
// surface that connects to it, and a material.
constexpr PrStringView kLook = R"(<?xml version="1.0"?>
<materialx version="1.38" colorspace="lin_rec709">
  <nodegraph name="NG_wood">
    <input name="tint" type="float" value="0.5" />
    <image name="color" type="color3">
      <input name="file" type="filename" value="tex/wood_color.png" colorspace="srgb_texture" />
    </image>
    <image name="packed" type="color4">
      <input name="file" type="filename" value="tex/wood_packed.png" />
      <input name="uaddressmode" type="string" value="clamp" />
      <input name="texcoord" type="vector2" nodename="uv1" />
    </image>
    <texcoord name="uv1" type="vector2">
      <input name="index" type="integer" value="1" />
    </texcoord>
    <extract name="green" type="float">
      <input name="in" type="color4" nodename="packed" />
      <input name="index" type="integer" value="1" />
    </extract>
    <multiply name="scaled" type="float">
      <input name="in1" type="float" nodename="green" />
      <input name="in2" type="float" interfacename="tint" />
    </multiply>
    <add name="lifted" type="float">
      <input name="in1" type="float" nodename="scaled" />
      <input name="in2" type="float" value="0.25" />
    </add>
    <image name="bumps" type="vector3">
      <input name="file" type="filename" value="tex/wood_normal.png" />
    </image>
    <normalmap name="mapped" type="vector3">
      <input name="in" type="vector3" nodename="bumps" />
      <input name="scale" type="float" value="0.5" />
    </normalmap>
    <output name="color_out" type="color3" nodename="color" />
    <output name="rough_out" type="float" nodename="lifted" />
    <output name="normal_out" type="vector3" nodename="mapped" />
  </nodegraph>
  <standard_surface name="SR_wood" type="surfaceshader">
    <input name="base_color" type="color3" nodegraph="NG_wood" output="color_out" />
    <input name="specular_roughness" type="float" nodegraph="NG_wood" output="rough_out" />
    <input name="normal" type="vector3" nodegraph="NG_wood" output="normal_out" />
    <input name="metalness" type="float" value="0.75" />
  </standard_surface>
  <surfacematerial name="M_wood" type="material">
    <input name="surfaceshader" type="surfaceshader" nodename="SR_wood" />
  </surfacematerial>
</materialx>
)";

TEST_CASE("a MaterialX document composes by reference into materials whose networks fold to images") {
    GeomStage s("def Scope \"Looks\" (\n references = @./looks/look.mtlx@</MaterialX/Materials>\n)\n{\n}\n"
        "def Mesh \"Board\" (\n prepend apiSchemas = [\"MaterialBindingAPI\"]\n)\n{\n"
        " rel material:binding = </Looks/M_wood>\n}\n", "", kLook);
    PrUsdPrimIndex material = s.prim("/Looks/M_wood");
    CHECK(prUsdGeomPrimKind(s.stage, material) == PrUsdGeomKind_Material);
    CHECK(prUsdShadeBoundMaterial(s.stage, s.prim("/Board"), s.scratch) == material);

    PrStringView id{};
    PrUsdPrimIndex shader = prUsdMtlxSurface(s.stage, material, s.scratch, &id);
    CHECK(shader == s.prim("/Looks/M_wood/SR_wood"));
    CHECK(id == "ND_standard_surface_surfaceshader");
    CHECK(prUsdMtlxNodeCategory(id) == "standard_surface");
    CHECK(prUsdMtlxNodeCategory("ND_open_pbr_surface_surfaceshader") == "open_pbr_surface");
    CHECK(prUsdMtlxNodeCategory("ND_multiply_color3FA") == "multiply");
    CHECK(prUsdMtlxNodeCategory("ND_surface_unlit") == "surface_unlit");
    CHECK(prUsdMtlxNodeCategory("UsdPreviewSurface").size == 0);

    // A value of the shader's own.
    const float gray[3] = {0.8f, 0.8f, 0.8f};
    PrUsdMtlxInput input{};
    REQUIRE(prUsdMtlxInputRead(s.stage, shader, "metalness", 1, gray, kPrF64Nan, s.scratch, &input));
    CHECK(!input.textured);
    CHECK(input.value[0] == 0.75f);
    CHECK(!prUsdMtlxInputRead(s.stage, shader, "coat", 1, gray, kPrF64Nan, s.scratch, &input));
    CHECK(input.value[0] == 0.8f);

    // An image of color, in sRGB, at the document's folder, through the node graph's output.
    REQUIRE(prUsdMtlxInputRead(s.stage, shader, "base_color", 3, gray, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(!input.approximated);
    CHECK(input.texture.colorSpace == PrUsdTextureColorSpace_Srgb);
    CHECK(input.texture.primvar == "st");
    CHECK(input.weights[0][0] == 1);
    CHECK(input.weights[1][1] == 1);
    CHECK(input.weights[2][2] == 1);
    CHECK(input.weights[0][1] == 0);
    PrRawString location = prUsdTextureLocation(s.stage, &input.texture, kPrF64Nan);
    CHECK(prRawStringView(&location) == "/assets/looks/tex/wood_color.png");
    prRawStringReset(&location);

    // The green channel of an image of data, times the node graph's input and plus a value: 0.5 g + 0.25.
    REQUIRE(prUsdMtlxInputRead(s.stage, shader, "specular_roughness", 1, gray, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(!input.approximated);
    CHECK(input.texture.colorSpace == PrUsdTextureColorSpace_Raw);
    CHECK(input.texture.wrapS == PrUsdTextureWrap_Clamp);
    CHECK(input.texture.wrapT == PrUsdTextureWrap_Repeat);
    CHECK(input.texture.primvar == "st1");
    CHECK(input.weights[0][0] == 0);
    CHECK(input.weights[0][1] == 0.5f);
    CHECK(input.offset[0] == 0.25f);

    // A normal map of scale 0.5: x and y of (2 t - 1) * 0.5, and z of 2 t - 1.
    REQUIRE(prUsdMtlxInputRead(s.stage, shader, "normal", 3, gray, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(input.normalMap);
    CHECK(input.weights[0][0] == 1);
    CHECK(input.offset[0] == -0.5f);
    CHECK(input.weights[2][2] == 2);
    CHECK(input.offset[2] == -1);
}

TEST_CASE("a place2d node is its image's transform, exactly, in either order") {
    // MaterialX's test of placements: one node graph that scales, rotates and moves about a pivot, and one that moves,
    // rotates and scales. A shader of the stage reads each graph's output.
    GeomStage s("def Scope \"Looks\" (\n references = @./looks/look.mtlx@</MaterialX>\n)\n{\n}\n"
        "def Shader \"Surface\" {\n uniform token info:id = \"ND_open_pbr_surface_surfaceshader\"\n"
        " color3f inputs:base_color.connect = </Looks/NodeGraphs/test_place2d_SRT.outputs:out>\n"
        " color3f inputs:emission_color.connect = </Looks/NodeGraphs/test_place2d_TRS.outputs:out>\n}\n", "",
        materialxDocument("image_transform.mtlx"));
    const float black[3] = {0, 0, 0};
    const double pivot[2] = {0.5, 0.5};
    const double scale[2] = {2, 1};
    PrUsdMtlxInput input{};
    REQUIRE(prUsdMtlxInputRead(s.stage, s.prim("/Surface"), "base_color", 3, black, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(!input.approximated);
    CHECK(input.texture.primvar == "st");
    const double still[2] = {0, 0};
    checkPlacement(input.texture.transform, false, pivot, scale, 30, still);
    REQUIRE(prUsdMtlxInputRead(s.stage, s.prim("/Surface"), "emission_color", 3, black, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(!input.approximated);
    const double moved[2] = {0.2, 0.2};
    checkPlacement(input.texture.transform, true, pivot, scale, 30, moved);
}

TEST_CASE("a material of MaterialX's examples places its image by a place2d node") {
    GeomStage s("def Scope \"Looks\" (\n references = @./looks/look.mtlx@</MaterialX/Materials>\n)\n{\n}\n", "",
        materialxDocument("standard_surface_greysphere_calibration.mtlx"));
    PrStringView id{};
    PrUsdPrimIndex shader = prUsdMtlxSurface(s.stage, s.prim("/Looks/Greysphere_Calibration"), s.scratch, &id);
    REQUIRE(shader != kPrUsdNone);
    const float black[3] = {0, 0, 0};
    PrUsdMtlxInput input{};
    REQUIRE(prUsdMtlxInputRead(s.stage, shader, "base_color", 3, black, kPrF64Nan, s.scratch, &input));
    REQUIRE(input.textured);
    CHECK(!input.approximated);
    CHECK(input.texture.wrapS == PrUsdTextureWrap_Clamp);
    const double pivot[2] = {0.5, 0.5};
    const double scale[2] = {0.21, 0.21};
    const double offset[2] = {-1.66, -0.49};
    checkPlacement(input.texture.transform, false, pivot, scale, 0, offset);
    // The document's file prefix is not applied, as UsdMtlx does not apply it.
    PrRawString location = prUsdTextureLocation(s.stage, &input.texture, kPrF64Nan);
    CHECK(prRawStringView(&location) == "/assets/looks/greysphere_calibration.png");
    prRawStringReset(&location);
}

TEST_CASE("a MaterialX document is a layer of materials, shaders and node graphs in UsdMtlx's layout") {
    PrBlob* file = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(kLook.data), kLook.size});
    PrUsdLayer* layer = nullptr;
    REQUIRE(prUsdLayerCreate(file, nullptr, &layer) == PrStatus_Ok);
    prBlobRelease(file);
    CHECK(prUsdLayerFormat(layer) == PrUsdFormat_MaterialX);
    CHECK(prUsdLayerVersion(layer) == "1.38");
    CHECK(prUsdLayerFindSpec(layer, "/MaterialX/Materials/M_wood") != kPrUsdNone);
    CHECK(prUsdLayerFindSpec(layer, "/MaterialX/Materials/M_wood/SR_wood") != kPrUsdNone);
    CHECK(prUsdLayerFindSpec(layer, "/MaterialX/Shaders/SR_wood.info:id") != kPrUsdNone);
    CHECK(prUsdLayerFindSpec(layer, "/MaterialX/NodeGraphs/NG_wood/packed.inputs:file") != kPrUsdNone);
    CHECK(prUsdLayerFindSpec(layer, "/MaterialX/NodeGraphs/NG_wood.outputs:rough_out") != kPrUsdNone);
    prUsdLayerDestroy(layer);

    // A document that lib/materialx refuses is no layer.
    const PrStringView broken = "<materialx><nodegraph name=\"a\"/><nodegraph name=\"a\"/></materialx>";
    file = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(broken.data), broken.size});
    CHECK(prUsdLayerCreate(file, nullptr, &layer) == PrStatus_InvalidData);
    prBlobRelease(file);
}
