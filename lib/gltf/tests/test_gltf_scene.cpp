// Meshes, materials, textures, cameras, lights, nodes, skins and scenes.

#include "pr/float.h"

#include "gltf/tests/gltf_fixture.h"

namespace pr_impl {

namespace {

// Returns the buffer of gltfQuad with indices that draw its four vertices as two triangles, and again as a run of four.
GltfBin quadBin() {
    GltfBin bin;
    bin.put<float>(0, 0, 0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 0);
    bin.put<uint16_t>(64, 0, 1, 2, 2, 1, 3);
    return bin;
}

// Returns the JSON of the quad with one mesh of one primitive, whose members are `primitive`.
String onePrimitive(PrStringView primitive) {
    String members = R"("meshes":[{"primitives":[{)";
    return gltfQuad(members.append(primitive).append("}]}]"));
}

// Returns the triangle list of primitive `index`.
Array<uint32_t> trianglesOf(const PrGltf* gltf, uint32_t index) {
    Array<uint32_t> out(prGltfPrimitiveTriangles(gltf, index, nullptr, 0));
    CHECK(prGltfPrimitiveTriangles(gltf, index, out.data(), out.count()) == out.count());
    return out;
}

// Returns the JSON of an asset with one material, whose members are `material`.
String oneMaterial(PrStringView material) {
    String members = R"("images":[{"uri":"a.png"}],"textures":[{"source":0},{"source":0}],"materials":[{)";
    return gltfAsset(members.append(material).append("}]"));
}

// Returns the JSON of an asset with the nodes `nodes`, and the members `others` after them.
String withNodes(PrStringView nodes, PrStringView others = "") {
    String members = R"("nodes":[)";
    members.append(nodes).append("]");
    if (others.size != 0)
        members.append(",").append(others);
    return gltfAsset(members);
}

// The accessors of a skinned triangle over "bin": positions at byte 0, joints of unsigned bytes at byte 36, weights at
// byte 48, and two inverse bind matrices at byte 96.
constexpr PrStringView kSkinAccessors =
    R"("accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":36,"componentType":5121,"count":3,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":3,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5126,"count":2,"type":"MAT4"}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":1,"WEIGHTS_0":2}}]},)"
    R"({"primitives":[{"attributes":{"POSITION":0}}]}])";

// Returns the JSON of the skinned triangle with the nodes `nodes` and the skins `skins`.
String skinned(PrStringView nodes, PrStringView skins) {
    String members = kSkinAccessors;
    members.append(R"(,"nodes":[)").append(nodes).append(R"(],"skins":[)").append(skins).append("]");
    return gltfBinAsset(members);
}

// Returns true if `a` and `b` are equal to within the error of a few float operations.
bool near(float a, float b) {
    return prF32Abs(a - b) < 1e-5f;
}

// The accessors of the instancing tests over "bin": a triangle's positions, and for three instances, translations at
// byte 48, rotations of normalized shorts at byte 96, scales at byte 128, and a custom attribute at byte 176, then two
// accessors of other types.
constexpr PrStringView kInstanceAccessors =
    R"("accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":0,"byteOffset":48,"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5122,"normalized":true,"count":3,"type":"VEC4"},)"
    R"({"bufferView":0,"byteOffset":128,"componentType":5126,"count":3,"type":"VEC3"},)"
    R"({"bufferView":0,"byteOffset":176,"componentType":5126,"count":3,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":176,"componentType":5126,"count":2,"type":"SCALAR"},)"
    R"({"bufferView":0,"byteOffset":96,"componentType":5122,"count":3,"type":"VEC4"}],)"
    R"("meshes":[{"primitives":[{"attributes":{"POSITION":0}}]}])";

// Returns the JSON of the instancing tests' asset whose node has `instancing` as its extension.
String instanced(PrStringView instancing, PrStringView node = R"("mesh":0,)") {
    String members = kInstanceAccessors;
    members.append(R"(,"nodes":[{)").append(node).append(R"("extensions":{"EXT_mesh_gpu_instancing":)");
    return gltfBinAsset(members.append(instancing).append("}}]"));
}

// Returns the JSON of an asset with three materials, the variants of KHR_materials_variants that `variants` names, and
// a mesh of one primitive with material 0 whose extension's mappings are `mappings`.
String withVariants(PrStringView variants, PrStringView mappings) {
    String members = R"("materials":[{},{},{}],"extensions":{"KHR_materials_variants":{"variants":[)";
    members.append(variants).append(R"(]}},"accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"material":0,)"
        R"("extensions":{"KHR_materials_variants":{"mappings":[)");
    return gltfBinAsset(members.append(mappings).append("]}}}]}]"));
}

constexpr PrStringView kThreeVariants = R"({"name":"red"},{"name":"green"},{"name":"blue"})";

// The accessors of the quantization tests, three elements each over "bin", with a stride of four bytes where the
// elements are smaller, as an attribute's elements are aligned:
//
//   0  VEC3 floats at byte 0, with bounds
//   1  VEC3 shorts at byte 48, with bounds
//   2  VEC3 normalized unsigned bytes at byte 72, with bounds
//   3  VEC3 normalized bytes at byte 72
//   4  VEC4 normalized shorts at byte 96
//   5  VEC2 unsigned bytes at byte 72
//   6  VEC2 normalized bytes at byte 72
//   7  VEC3 unsigned shorts at byte 48
//   8  VEC3 unsigned ints at byte 0
//   9  VEC4 normalized unsigned bytes at byte 72
//  10  VEC2 normalized shorts at byte 48
constexpr PrStringView kQuantizedAccessors =
    R"("bufferViews":[{"buffer":0,"byteLength":256},{"buffer":0,"byteLength":256,"byteStride":8},)"
    R"({"buffer":0,"byteLength":256,"byteStride":4}],)"
    R"("accessors":[)"
    R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":1,"byteOffset":48,"componentType":5122,"count":3,"type":"VEC3","min":[-4,0,0],"max":[4,8,0]},)"
    R"({"bufferView":2,"byteOffset":72,"componentType":5121,"normalized":true,"count":3,"type":"VEC3",)"
    R"("min":[0,51,0],"max":[255,255,0]},)"
    R"({"bufferView":2,"byteOffset":72,"componentType":5120,"normalized":true,"count":3,"type":"VEC3"},)"
    R"({"bufferView":1,"byteOffset":96,"componentType":5122,"normalized":true,"count":3,"type":"VEC4"},)"
    R"({"bufferView":2,"byteOffset":72,"componentType":5121,"count":3,"type":"VEC2"},)"
    R"({"bufferView":2,"byteOffset":72,"componentType":5120,"normalized":true,"count":3,"type":"VEC2"},)"
    R"({"bufferView":1,"byteOffset":48,"componentType":5123,"count":3,"type":"VEC3","min":[0,0,0],"max":[4,8,0]},)"
    R"({"bufferView":0,"componentType":5125,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},)"
    R"({"bufferView":2,"byteOffset":72,"componentType":5121,"normalized":true,"count":3,"type":"VEC4"},)"
    R"({"bufferView":1,"byteOffset":48,"componentType":5122,"normalized":true,"count":3,"type":"VEC2"}])";

// Returns the JSON of an asset with the quantization tests' accessors and a mesh of one primitive with `attributes` and
// the morph targets `targets`, that uses KHR_mesh_quantization if `quantized`.
String quantizedAsset(PrStringView attributes, PrStringView targets = "", bool quantized = true) {
    String members = quantized ? R"("extensionsUsed":["KHR_mesh_quantization"],)" : "";
    members.append(R"("buffers":[{"byteLength":256,"uri":"bin"}],)").append(kQuantizedAccessors);
    members.append(R"(,"meshes":[{"primitives":[{"attributes":{)").append(attributes).append("}");
    if (targets.size != 0)
        members.append(R"(,"targets":[{)").append(targets).append("}]");
    return gltfAsset(members.append("}]}]"));
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// ---------------------------------------------------------------------------------------------------------------------
// Meshes

TEST_CASE("a primitive names its attributes, its indices, its material and its mode") {
    GltfBin bin = quadBin();
    GltfRead read(gltfQuad(R"("materials":[{}],"meshes":[{"name":"quad","primitives":[)"
        R"({"attributes":{"POSITION":0,"NORMAL":2,"TANGENT":5,"TEXCOORD_0":3,"TEXCOORD_1":3,"COLOR_0":5,)"
        R"("JOINTS_0":4,"WEIGHTS_0":5,"_CUSTOM":3},"indices":1,"material":0},)"
        R"({"attributes":{"POSITION":8},"mode":0}]}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfMeshCount(read.gltf) == 1);
    const PrGltfMesh& mesh = prGltfMeshes(read.gltf)[0];
    CHECK(mesh.name == "quad");
    CHECK(mesh.primitiveFirst == 0);
    CHECK(mesh.primitiveCount == 2);
    CHECK(mesh.weights.count == 0);
    REQUIRE(prGltfPrimitiveCount(read.gltf) == 2);

    const PrGltfPrimitive& quad = prGltfPrimitives(read.gltf)[0];
    CHECK(quad.vertexCount == 4);
    CHECK(quad.indices == 1);
    CHECK(quad.material == 0);
    CHECK(quad.mode == PrGltfMode_Triangles);
    CHECK(quad.morphTargetCount == 0);
    REQUIRE(quad.attributeCount == 9);
    const PrGltfAttribute* attributes = prGltfAttributes(read.gltf) + quad.attributeFirst;
    CHECK(attributes[0].name == "POSITION");
    CHECK(attributes[0].semantic == PrGltfSemantic_Position);
    CHECK(attributes[0].accessor == 0);
    CHECK(attributes[4].name == "TEXCOORD_1");
    CHECK(attributes[4].semantic == PrGltfSemantic_TexCoord);
    CHECK(attributes[4].set == 1);
    CHECK(attributes[8].name == "_CUSTOM");
    CHECK(attributes[8].semantic == PrGltfSemantic_Custom);
    CHECK(prGltfAttributeCount(read.gltf) == 10);

    CHECK(prGltfPrimitiveAttribute(read.gltf, 0, PrGltfSemantic_Normal, 0) == 2);
    CHECK(prGltfPrimitiveAttribute(read.gltf, 0, PrGltfSemantic_TexCoord, 1) == 3);
    CHECK(prGltfPrimitiveAttribute(read.gltf, 0, PrGltfSemantic_TexCoord, 2) == kPrGltfNone);
    CHECK(prGltfPrimitiveAttribute(read.gltf, 0, PrGltfSemantic_Joints, 0) == 4);
    CHECK(prGltfPrimitiveAttribute(read.gltf, 1, PrGltfSemantic_Normal, 0) == kPrGltfNone);

    const PrGltfPrimitive& points = prGltfPrimitives(read.gltf)[1];
    CHECK(points.vertexCount == 3);
    CHECK(points.indices == kPrGltfNone);
    CHECK(points.material == kPrGltfNone);
    CHECK(points.mode == PrGltfMode_Points);
}

TEST_CASE("every mode unrolls into the triangle list that the specification gives it") {
    GltfBin bin = quadBin();
    GltfRead read(gltfQuad(R"("meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"indices":1},)"
        R"({"attributes":{"POSITION":0},"indices":7,"mode":5},)"
        R"({"attributes":{"POSITION":0},"indices":7,"mode":6},)"
        R"({"attributes":{"POSITION":0},"mode":5},)"
        R"({"attributes":{"POSITION":0},"mode":6},)"
        R"({"attributes":{"POSITION":8}},)"
        R"({"attributes":{"POSITION":0},"mode":0},)"
        R"({"attributes":{"POSITION":0},"mode":1},)"
        R"({"attributes":{"POSITION":0},"mode":2},)"
        R"({"attributes":{"POSITION":0},"indices":7,"mode":3}]}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);

    // The indices at byte 64 are 0 1 2 2 1 3, and as a run of four, 0 1 2 2.
    const uint32_t list[] = {0, 1, 2, 2, 1, 3};
    CHECK(trianglesOf(read.gltf, 0) == Array<uint32_t>(list));
    // The second triangle of a strip swaps its last two corners, which keeps its winding.
    const uint32_t indexedStrip[] = {0, 1, 2, 1, 2, 2};
    CHECK(trianglesOf(read.gltf, 1) == Array<uint32_t>(indexedStrip));
    // Each triangle of a fan ends at the first vertex.
    const uint32_t indexedFan[] = {1, 2, 0, 2, 2, 0};
    CHECK(trianglesOf(read.gltf, 2) == Array<uint32_t>(indexedFan));
    const uint32_t strip[] = {0, 1, 2, 1, 3, 2};
    CHECK(trianglesOf(read.gltf, 3) == Array<uint32_t>(strip));
    const uint32_t fan[] = {1, 2, 0, 2, 3, 0};
    CHECK(trianglesOf(read.gltf, 4) == Array<uint32_t>(fan));
    const uint32_t ordered[] = {0, 1, 2};
    CHECK(trianglesOf(read.gltf, 5) == Array<uint32_t>(ordered));
    // Points and lines have no triangles.
    for (uint32_t primitive = 6; primitive < 10; ++primitive)
        CHECK(prGltfPrimitiveTriangles(read.gltf, primitive, nullptr, 0) == 0);

    // A capacity short of the list takes its whole triangles, and the call still returns the list's length.
    uint32_t some[5] = {9, 9, 9, 9, 9};
    CHECK(prGltfPrimitiveTriangles(read.gltf, 0, some, 5) == 6);
    CHECK(some[2] == 2);
    CHECK(some[3] == 9);
}

TEST_CASE("a strip longer than one read of its indices unrolls whole") {
    // 300 vertices of zeros, drawn by 300 indices that count down, in a buffer of the asset's own.
    Array<uint8_t> bytes(uint32_t(300 * 12 + 300 * 2));
    for (uint32_t i = 0; i < 300; ++i) {
        uint16_t index = uint16_t(299 - i);
        prMemCopy(&bytes[300 * 12 + i * 2], &index, sizeof(index));
    }
    PrGltfLoadFn load = [](void* context, PrGltfExternal, PrStringView) -> PrBlob* {
        return prBlobCopy(*static_cast<Array<uint8_t>*>(context));
    };
    String json = gltfAsset(R"("buffers":[{"byteLength":4200,"uri":"long"}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":4200}],"accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":300,"type":"VEC3","min":[0,0,0],"max":[0,0,0]},)"
        R"({"bufferView":0,"byteOffset":3600,"componentType":5123,"count":300,"type":"SCALAR"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"mode":5},)"
        R"({"attributes":{"POSITION":0},"indices":1,"mode":6}]}])");
    PrBlob* file = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(json.view().data), json.count()});
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = load;
    options.loadContext = &bytes;
    PrGltf* gltf = nullptr;
    REQUIRE(prGltfCreate(file, &options, &gltf) == PrStatus_Ok);
    prBlobRelease(file);

    Array<uint32_t> strip = trianglesOf(gltf, 0);
    REQUIRE(strip.count() == 298 * 3);
    // Triangle 297 is odd, so it is vertices 297, 299 and 298 of the run, which are indices 2, 0 and 1.
    CHECK(strip[297 * 3] == 2);
    CHECK(strip[297 * 3 + 1] == 0);
    CHECK(strip[297 * 3 + 2] == 1);
    Array<uint32_t> fan = trianglesOf(gltf, 1);
    REQUIRE(fan.count() == 298 * 3);
    CHECK(fan[297 * 3] == 1);
    CHECK(fan[297 * 3 + 1] == 0);
    CHECK(fan[297 * 3 + 2] == 299);
    prGltfDestroy(gltf);
}

TEST_CASE("morph targets displace a primitive's attributes, and a mesh gives their default weights") {
    GltfBin bin = quadBin();
    GltfRead read(gltfQuad(R"("meshes":[{"weights":[0.25,0.75],"primitives":[)"
        R"({"attributes":{"POSITION":0,"NORMAL":2,"TANGENT":5,"TEXCOORD_0":3},"indices":1,)"
        R"("targets":[{"POSITION":0,"NORMAL":2},{"POSITION":0,"TANGENT":2,"TEXCOORD_0":3}]},)"
        R"({"attributes":{"POSITION":0},"indices":1}]}],)"
        R"("nodes":[{"mesh":0,"weights":[1,0]},{"mesh":0}])"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfMesh& mesh = prGltfMeshes(read.gltf)[0];
    REQUIRE(mesh.weights.count == 2);
    CHECK(prGltfListFloats(read.gltf, mesh.weights)[1] == 0.75f);

    const PrGltfPrimitive& primitive = prGltfPrimitives(read.gltf)[0];
    REQUIRE(primitive.morphTargetCount == 2);
    REQUIRE(prGltfMorphTargetCount(read.gltf) == 2);
    const PrGltfMorphTarget* targets = prGltfMorphTargets(read.gltf) + primitive.morphTargetFirst;
    CHECK(targets[0].attributeCount == 2);
    REQUIRE(targets[1].attributeCount == 3);
    const PrGltfAttribute& tangent = prGltfAttributes(read.gltf)[targets[1].attributeFirst + 1];
    CHECK(tangent.semantic == PrGltfSemantic_Tangent);
    CHECK(tangent.accessor == 2);
    // A primitive of the mesh may have no morph targets.
    CHECK(prGltfPrimitives(read.gltf)[1].morphTargetCount == 0);

    const PrGltfNode* nodes = prGltfNodes(read.gltf);
    REQUIRE(nodes[0].weights.count == 2);
    CHECK(prGltfListFloats(read.gltf, nodes[0].weights)[0] == 1);
    CHECK(nodes[1].weights.count == 0);
}

TEST_CASE("a primitive's attributes are checked against the specification's table") {
    GltfBin bin = quadBin();
    auto refusal = [&](PrStringView primitive) { return gltfRefusal(onePrimitive(primitive), &bin); };
    CHECK(refusal(R"("indices":1)") == "meshes[0].primitives[0].attributes must hold at least one attribute");
    CHECK(refusal(R"("attributes":{})") == "meshes[0].primitives[0].attributes must hold at least one attribute");
    CHECK(refusal(R"("attributes":[0])") == "meshes[0].primitives[0].attributes must be an object");
    CHECK(refusal(R"("attributes":{"POSITION":12})") == "meshes[0].primitives[0].attributes is not the index of an object");
    CHECK(refusal(R"("attributes":{"POSITION":{}})") == "meshes[0].primitives[0].attributes must be a number");
    CHECK(refusal(R"("attributes":{"POSITION":0,"NORMAL":8})") ==
        "meshes[0].primitives[0].attributes has accessors of different counts");
    CHECK(refusal(R"("attributes":{"POSITION":2})") ==
        "meshes[0].primitives[0].attributes has a POSITION accessor without min and max");

    const char* mistyped = "meshes[0].primitives[0].attributes has an accessor of a type that its semantic does not allow";
    CHECK(refusal(R"("attributes":{"POSITION":3})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"NORMAL":3})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"TANGENT":2})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_0":2})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"COLOR_0":3})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"JOINTS_0":5,"WEIGHTS_0":5})") == mistyped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"JOINTS_0":4,"WEIGHTS_0":4})") == mistyped);

    const char* misnumbered = "meshes[0].primitives[0].attributes has a numbered semantic with a malformed number";
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_01":3})") == misnumbered);
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_":3})") == misnumbered);
    CHECK(refusal(R"("attributes":{"POSITION":0,"COLOR_x":5})") == misnumbered);
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_1234567890":3})") == misnumbered);
    const char* gapped = "meshes[0].primitives[0].attributes repeats a semantic or numbers its sets with a gap";
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_1":3})") == gapped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"TEXCOORD_0":3,"TEXCOORD_2":3})") == gapped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"POSITION":0})") == gapped);
    CHECK(refusal(R"("attributes":{"POSITION":0,"JOINTS_0":4,"WEIGHTS_1":5})") == gapped);
    const char* unpaired = "meshes[0].primitives[0].attributes has joints without weights or weights without joints";
    CHECK(refusal(R"("attributes":{"POSITION":0,"JOINTS_0":4})") == unpaired);
    CHECK(refusal(R"("attributes":{"POSITION":0,"WEIGHTS_0":5})") == unpaired);
}

TEST_CASE("a primitive's indices and mode are checked against its vertices") {
    GltfBin bin = quadBin();
    auto refusal = [&](PrStringView primitive) { return gltfRefusal(onePrimitive(primitive), &bin); };
    const char* notIndices = "meshes[0].primitives[0].indices must be a SCALAR accessor of unsigned integers";
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":6)") == notIndices);
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":4)") == notIndices);
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":12)") ==
        "meshes[0].primitives[0].indices is not the index of an object");
    CHECK(refusal(R"("attributes":{"POSITION":0},"material":0)") ==
        "meshes[0].primitives[0].material is not the index of an object");
    CHECK(refusal(R"("attributes":{"POSITION":0},"mode":7)") ==
        "meshes[0].primitives[0].mode is not a value of its enumeration");

    // Four vertices, or four indices, are not whole triangles, and three vertices are not whole lines.
    const char* unassembled = "meshes[0].primitives[0].mode cannot assemble the primitive's number of vertices";
    CHECK(refusal(R"("attributes":{"POSITION":0})") == unassembled);
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":7)") == unassembled);
    CHECK(refusal(R"("attributes":{"POSITION":8},"mode":1)") == unassembled);

    // An index names a vertex.
    bin.put<uint16_t>(64, 0, 1, 2, 2, 1, 4);
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":1)") ==
        "meshes[0].primitives[0].indices has an index that names no vertex");
    bin.put<uint16_t>(64, 0, 1, 65535, 2, 1, 3);
    CHECK(refusal(R"("attributes":{"POSITION":0},"indices":1)") ==
        "meshes[0].primitives[0].indices has an index that names no vertex");
}

TEST_CASE("indices over a buffer view with a stride are refused") {
    GltfBin bin;
    CHECK(gltfRefusal(gltfBinAsset(R"("accessors":[)"
        R"({"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[0,0,0]},)"
        R"({"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],)"
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])"), &bin) ==
        "meshes[0].primitives[0].indices reads a buffer view that has a byteStride");
}

TEST_CASE("morph targets and weights are checked against the mesh") {
    GltfBin bin = quadBin();
    auto refusal = [&](PrStringView members) { return gltfRefusal(gltfQuad(members), &bin); };
    CHECK(refusal(R"("meshes":[{}])") == "meshes[0].primitives must hold at least one primitive");
    CHECK(refusal(R"("meshes":[{"primitives":[]}])") == "meshes[0].primitives must hold at least one primitive");
    CHECK(refusal(R"("meshes":[{"primitives":[7]}])") == "meshes[0].primitives[0] must be an object");
    CHECK(refusal(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"targets":[7]}]}])") ==
        "meshes[0].primitives[0].targets must hold objects");
    CHECK(refusal(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,)"
        R"("targets":[{"NORMAL":2}]}]}])") ==
        "meshes[0].primitives[0].targets displaces an attribute that the primitive does not have");
    // The displacement of a tangent is three floats, not four.
    CHECK(refusal(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"TANGENT":5},"indices":1,)"
        R"("targets":[{"TANGENT":5}]}]}])") ==
        "meshes[0].primitives[0].targets has an accessor of a type that its semantic does not allow");
    CHECK(refusal(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,)"
        R"("targets":[{"POSITION":8}]}]}])") == "meshes[0].primitives[0].targets has accessors of different counts");
    CHECK(refusal(R"("meshes":[{"primitives":[)"
        R"({"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":0}]},)"
        R"({"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":0},{"POSITION":0}]}]}])") ==
        "meshes[0].primitives[1].targets has another number of morph targets than an earlier primitive");
    CHECK(refusal(R"("meshes":[{"weights":[1,2],"primitives":[)"
        R"({"attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":0}]}]}])") ==
        "meshes[0].weights must have a weight for each morph target");
    CHECK(refusal(R"("meshes":[{"weights":[1],"primitives":[{"attributes":{"POSITION":0},"indices":1}]}])") ==
        "meshes[0].weights must have a weight for each morph target");
    CHECK(refusal(R"("meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,)"
        R"("targets":[{"POSITION":0}]}]}],"nodes":[{"mesh":0,"weights":[1,2]}])") ==
        "nodes[0].weights must have a weight for each morph target of the node's mesh");
    CHECK(refusal(R"("nodes":[{"weights":[1]}])") ==
        "nodes[0].weights must have a weight for each morph target of the node's mesh");
}

// ---------------------------------------------------------------------------------------------------------------------
// Images, samplers and textures

TEST_CASE("an image is its encoded bytes and their media type") {
    GltfBin bin;
    bin.put<uint8_t>(8, 0xff, 0xd8, 0xff, 0xe0);
    PrGltfLoadFn load = [](void* context, PrGltfExternal kind, PrStringView uri) -> PrBlob* {
        if (kind == PrGltfExternal_Buffer)
            return gltfLoadBin(context, kind, uri);
        static const uint8_t kJpeg[] = {0xff, 0xd8, 0xff, 0xdb, 0x00};
        static const uint8_t kOther[] = {'R', 'I', 'F', 'F'};
        if (uri == "photo")
            return prBlobCopy(kJpeg);
        return uri == "other" ? prBlobCopy(kOther) : nullptr;
    };
    String json = gltfAsset(R"("buffers":[{"byteLength":256,"uri":"bin"}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":8,"byteLength":4}],"images":[)"
        R"({"bufferView":0,"mimeType":"image/jpeg","name":"embedded"},)"
        R"({"uri":"data:;base64,iVBORw0KGgo="},)"
        R"({"uri":"data:image/webp;base64,AAEC"},)"
        R"({"uri":"data:image/png;base64,AAEC","mimeType":"image/jpeg"},)"
        R"({"uri":"photo"},)"
        R"({"uri":"other"},)"
        R"({"uri":"missing.png"}])");
    PrBlob* file = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(json.view().data), json.count()});
    PrGltfOptions options = prGltfOptionsDefault();
    options.load = load;
    options.loadContext = &bin;
    PrGltf* gltf = nullptr;
    REQUIRE(prGltfCreate(file, &options, &gltf) == PrStatus_Ok);
    prBlobRelease(file);
    REQUIRE(prGltfImageCount(gltf) == 7);
    const PrGltfImage* images = prGltfImages(gltf);

    // A buffer view's bytes, with the type that the asset must state.
    CHECK(images[0].name == "embedded");
    CHECK(images[0].bufferView == 0);
    CHECK(images[0].uri.data == nullptr);
    CHECK(images[0].mimeType == "image/jpeg");
    REQUIRE(images[0].bytes.size == 4);
    CHECK(images[0].bytes.data[3] == 0xe0);

    // A data: URI's bytes. Its type is the asset's, or else the URI's, or else what the bytes begin as.
    CHECK(images[1].bufferView == kPrGltfNone);
    CHECK(images[1].uri == "");
    CHECK(images[1].bytes.size == 8);
    CHECK(images[1].mimeType == "image/png");
    CHECK(images[2].mimeType == "image/webp");
    CHECK(images[2].bytes.size == 3);
    CHECK(images[3].mimeType == "image/jpeg");

    // A URI's bytes, as the host loads them.
    CHECK(images[4].uri == "photo");
    CHECK(images[4].bytes.size == 5);
    CHECK(images[4].mimeType == "image/jpeg");
    CHECK(images[5].bytes.size == 4);
    CHECK(images[5].mimeType.data == nullptr);
    CHECK(images[6].uri == "missing.png");
    CHECK(images[6].bytes.data == nullptr);
    CHECK(images[6].mimeType.data == nullptr);
    prGltfDestroy(gltf);
}

TEST_CASE("samplers and textures read with their defaults") {
    GltfRead read(gltfAsset(R"("images":[{"uri":"a.png"}],"samplers":[)"
        R"({"magFilter":9728,"minFilter":9987,"wrapS":33071,"wrapT":33648,"name":"crisp"},)"
        R"({"magFilter":9729,"minFilter":9984},{"minFilter":9985},{"minFilter":9986},{}],)"
        R"("textures":[{"sampler":0,"source":0,"name":"albedo"},{}])"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfSamplerCount(read.gltf) == 5);
    const PrGltfSampler* samplers = prGltfSamplers(read.gltf);
    CHECK(samplers[0].name == "crisp");
    CHECK(samplers[0].magFilter == PrGltfFilter_Nearest);
    CHECK(samplers[0].minFilter == PrGltfFilter_LinearMipmapLinear);
    CHECK(samplers[0].wrapS == PrGltfWrap_ClampToEdge);
    CHECK(samplers[0].wrapT == PrGltfWrap_MirroredRepeat);
    CHECK(samplers[1].magFilter == PrGltfFilter_Linear);
    CHECK(samplers[1].minFilter == PrGltfFilter_NearestMipmapNearest);
    CHECK(samplers[2].minFilter == PrGltfFilter_LinearMipmapNearest);
    CHECK(samplers[3].minFilter == PrGltfFilter_NearestMipmapLinear);
    CHECK(samplers[4].magFilter == PrGltfFilter_None);
    CHECK(samplers[4].minFilter == PrGltfFilter_None);
    CHECK(samplers[4].wrapS == PrGltfWrap_Repeat);
    CHECK(samplers[4].wrapT == PrGltfWrap_Repeat);

    REQUIRE(prGltfTextureCount(read.gltf) == 2);
    const PrGltfTexture* textures = prGltfTextures(read.gltf);
    CHECK(textures[0].name == "albedo");
    CHECK(textures[0].sampler == 0);
    CHECK(textures[0].source == 0);
    CHECK(textures[1].sampler == kPrGltfNone);
    CHECK(textures[1].source == kPrGltfNone);
}

TEST_CASE("images, samplers and textures are checked") {
    GltfBin bin;
    CHECK(gltfRefusal(gltfAsset(R"("images":[{}])")) == "images[0] must have either a uri or a bufferView");
    CHECK(gltfRefusal(gltfBinAsset(R"("images":[{"uri":"a.png","bufferView":0,"mimeType":"image/png"}])"), &bin) ==
        "images[0] must have either a uri or a bufferView");
    CHECK(gltfRefusal(gltfBinAsset(R"("images":[{"bufferView":0}])"), &bin) ==
        "images[0].mimeType is required with a bufferView");
    CHECK(gltfRefusal(gltfBinAsset(R"("images":[{"bufferView":1,"mimeType":"image/png"}])"), &bin) ==
        "images[0].bufferView must not have a byteStride");
    CHECK(gltfRefusal(gltfBinAsset(R"("images":[{"bufferView":2,"mimeType":"image/png"}])"), &bin) ==
        "images[0].bufferView is not the index of an object");
    CHECK(gltfRefusal(gltfAsset(R"("images":[{"uri":7}])")) == "images[0].uri must be a string");
    CHECK(gltfRefusal(gltfAsset(R"("images":[{"uri":"data:image/png;base64,!"}])")) ==
        "images[0].uri is a malformed data: URI");
    CHECK(gltfRefusal(gltfAsset(R"("samplers":[{"magFilter":9984}])")) ==
        "samplers[0].magFilter is not a value of its enumeration");
    CHECK(gltfRefusal(gltfAsset(R"("samplers":[{"minFilter":1}])")) ==
        "samplers[0].minFilter is not a value of its enumeration");
    CHECK(gltfRefusal(gltfAsset(R"("samplers":[{"wrapS":1}])")) ==
        "samplers[0].wrapS is not a value of its enumeration");
    CHECK(gltfRefusal(gltfAsset(R"("samplers":[{"wrapT":"repeat"}])")) == "samplers[0].wrapT must be a number");
    CHECK(gltfRefusal(gltfAsset(R"("textures":[{"source":0}])")) == "textures[0].source is not the index of an object");
    CHECK(gltfRefusal(gltfAsset(R"("textures":[{"sampler":0}])")) == "textures[0].sampler is not the index of an object");
}

// ---------------------------------------------------------------------------------------------------------------------
// Materials

TEST_CASE("a material with nothing stated has the specification's defaults") {
    GltfRead read(oneMaterial(""));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfMaterialCount(read.gltf) == 1);
    const PrGltfMaterial& material = prGltfMaterials(read.gltf)[0];
    CHECK(material.name.data == nullptr);
    for (float factor : material.baseColorFactor)
        CHECK(factor == 1);
    CHECK(material.metallicFactor == 1);
    CHECK(material.roughnessFactor == 1);
    CHECK(material.baseColorTexture.texture == kPrGltfNone);
    CHECK(material.metallicRoughnessTexture.texture == kPrGltfNone);
    CHECK(material.normalTexture.texture == kPrGltfNone);
    CHECK(material.normalScale == 1);
    CHECK(material.occlusionTexture.texture == kPrGltfNone);
    CHECK(material.occlusionStrength == 1);
    CHECK(material.emissiveTexture.texture == kPrGltfNone);
    for (float factor : material.emissiveFactor)
        CHECK(factor == 0);
    CHECK(material.emissiveStrength == 1);
    CHECK(material.alphaMode == PrGltfAlphaMode_Opaque);
    CHECK(material.alphaCutoff == 0.5f);
    CHECK(!material.doubleSided);
    CHECK(material.extensions == 0);
    // A texture that is absent still has the identity transform.
    CHECK(material.baseColorTexture.transform.scale[0] == 1);
    CHECK(material.baseColorTexture.transform.scale[1] == 1);
}

TEST_CASE("a material reads its factors, its textures and its extensions") {
    GltfRead read(oneMaterial(R"("name":"painted","pbrMetallicRoughness":{)"
        R"("baseColorFactor":[0.5,0.25,0.125,0.75],"metallicFactor":0.25,"roughnessFactor":0.5,)"
        R"("baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":1,"texCoord":1}},)"
        R"("normalTexture":{"index":1,"scale":2},"occlusionTexture":{"index":0,"texCoord":2,"strength":0.5},)"
        R"("emissiveTexture":{"index":1},"emissiveFactor":[1,0.5,0],"alphaMode":"MASK","alphaCutoff":0.25,)"
        R"("doubleSided":true,"extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":4},)"
        R"("KHR_materials_unlit":{},"VENDOR_other":{"anything":[1,2]}})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfMaterial& material = prGltfMaterials(read.gltf)[0];
    CHECK(material.name == "painted");
    CHECK(material.baseColorFactor[0] == 0.5f);
    CHECK(material.baseColorFactor[3] == 0.75f);
    CHECK(material.metallicFactor == 0.25f);
    CHECK(material.roughnessFactor == 0.5f);
    CHECK(material.baseColorTexture.texture == 0);
    CHECK(material.baseColorTexture.texCoord == 0);
    CHECK(material.metallicRoughnessTexture.texture == 1);
    CHECK(material.metallicRoughnessTexture.texCoord == 1);
    CHECK(material.normalTexture.texture == 1);
    CHECK(material.normalScale == 2);
    CHECK(material.occlusionTexture.texture == 0);
    CHECK(material.occlusionTexture.texCoord == 2);
    CHECK(material.occlusionStrength == 0.5f);
    CHECK(material.emissiveTexture.texture == 1);
    CHECK(material.emissiveFactor[1] == 0.5f);
    CHECK(material.emissiveStrength == 4);
    CHECK(material.alphaMode == PrGltfAlphaMode_Mask);
    CHECK(material.alphaCutoff == 0.25f);
    CHECK(material.doubleSided);
    CHECK(material.extensions == (PrGltfMaterialExtensionFlag_EmissiveStrength | PrGltfMaterialExtensionFlag_Unlit));

    GltfRead blend(oneMaterial(R"("alphaMode":"BLEND")"));
    REQUIRE(blend.status == PrStatus_Ok);
    CHECK(prGltfMaterials(blend.gltf)[0].alphaMode == PrGltfAlphaMode_Blend);
}

TEST_CASE("a texture transform scales, rotates and offsets a texture coordinate") {
    GltfRead read(oneMaterial(R"("pbrMetallicRoughness":{"baseColorTexture":{"index":0,"texCoord":1,)"
        R"("extensions":{"KHR_texture_transform":{"offset":[0.5,0.25],"rotation":1.5707963267948966,)"
        R"("scale":[2,3],"texCoord":4}}}},)"
        R"("emissiveTexture":{"index":0,"texCoord":1,"extensions":{"KHR_texture_transform":{"scale":[2,2]}}})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfTextureInfo& info = prGltfMaterials(read.gltf)[0].baseColorTexture;
    // The extension's set of texture coordinates replaces the texture's.
    CHECK(info.texCoord == 4);
    CHECK(info.transform.offset[0] == 0.5f);
    CHECK(info.transform.offset[1] == 0.25f);
    CHECK(info.transform.scale[0] == 2);
    CHECK(info.transform.scale[1] == 3);

    // A quarter turn after a scale of (2, 3): (u, v) goes to (3v, -2u), and then is offset.
    PrAffine2 affine = prGltfTextureTransformAffine(&info.transform);
    CHECK(near(affine.a * 1 + affine.c * 0 + affine.e, 0.5f));
    CHECK(near(affine.b * 1 + affine.d * 0 + affine.f, -1.75f));
    CHECK(near(affine.a * 0 + affine.c * 1 + affine.e, 3.5f));
    CHECK(near(affine.b * 0 + affine.d * 1 + affine.f, 0.25f));

    const PrGltfTextureInfo& emissive = prGltfMaterials(read.gltf)[0].emissiveTexture;
    CHECK(emissive.texCoord == 1);
    PrAffine2 scaled = prGltfTextureTransformAffine(&emissive.transform);
    CHECK(scaled.a == 2);
    CHECK(scaled.b == 0);
    CHECK(scaled.c == 0);
    CHECK(scaled.d == 2);
    CHECK(scaled.e == 0);
    CHECK(scaled.f == 0);
}

TEST_CASE("a material's values are checked against their ranges") {
    auto refusal = [&](PrStringView material) { return gltfRefusal(oneMaterial(material)); };
    CHECK(refusal(R"("pbrMetallicRoughness":{"metallicFactor":1.5})") ==
        "materials[0].pbrMetallicRoughness.metallicFactor is out of range");
    CHECK(refusal(R"("pbrMetallicRoughness":{"roughnessFactor":-0.5})") ==
        "materials[0].pbrMetallicRoughness.roughnessFactor is out of range");
    CHECK(refusal(R"("pbrMetallicRoughness":{"baseColorFactor":[1,1,1]})") ==
        "materials[0].pbrMetallicRoughness.baseColorFactor has the wrong number of elements");
    CHECK(refusal(R"("pbrMetallicRoughness":{"baseColorFactor":[1,1,1,2]})") ==
        "materials[0].pbrMetallicRoughness.baseColorFactor is out of range");
    CHECK(refusal(R"("pbrMetallicRoughness":{"baseColorFactor":[1,1,"1",1]})") ==
        "materials[0].pbrMetallicRoughness.baseColorFactor must be a number");
    CHECK(refusal(R"("pbrMetallicRoughness":7)") == "materials[0].pbrMetallicRoughness must be an object");
    CHECK(refusal(R"("pbrMetallicRoughness":{"baseColorTexture":{}})") ==
        "materials[0].baseColorTexture.index is required");
    CHECK(refusal(R"("normalTexture":{"index":2})") == "materials[0].normalTexture.index is not the index of an object");
    CHECK(refusal(R"("normalTexture":{"index":0,"texCoord":-1})") ==
        "materials[0].normalTexture.texCoord must be a non-negative integer");
    CHECK(refusal(R"("occlusionTexture":{"index":0,"strength":2})") == "materials[0].occlusionTexture.strength is out of range");
    CHECK(refusal(R"("emissiveTexture":{"index":0,"extensions":{"KHR_texture_transform":{"scale":[1]}}})") ==
        "materials[0].emissiveTexture.scale has the wrong number of elements");
    CHECK(refusal(R"("emissiveFactor":[0,0,1.5])") == "materials[0].emissiveFactor is out of range");
    CHECK(refusal(R"("alphaMode":"CUTOUT")") == "materials[0].alphaMode is not a value of its enumeration");
    CHECK(refusal(R"("alphaCutoff":-1)") == "materials[0].alphaCutoff is out of range");
    CHECK(refusal(R"("doubleSided":1)") == "materials[0].doubleSided must be true or false");
    CHECK(refusal(R"("extensions":{"KHR_materials_emissive_strength":{"emissiveStrength":-1}})") ==
        "materials[0].KHR_materials_emissive_strength.emissiveStrength is out of range");
    CHECK(refusal(R"("extensions":[])") == "materials[0].extensions must be an object");
}

TEST_CASE("a material without the extensions of the material model has their defaults") {
    GltfRead read(oneMaterial(""));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfMaterial& m = prGltfMaterials(read.gltf)[0];
    CHECK(m.ior == 1.5f);
    CHECK(m.dispersion == 0);
    CHECK(m.clearcoat.factor == 0);
    CHECK(m.clearcoat.roughnessFactor == 0);
    CHECK(m.clearcoat.normalScale == 1);
    CHECK(m.clearcoat.texture.texture == kPrGltfNone);
    CHECK(m.clearcoat.normalTexture.texture == kPrGltfNone);
    for (float c : m.sheen.colorFactor)
        CHECK(c == 0);
    CHECK(m.sheen.roughnessFactor == 0);
    CHECK(m.transmission.factor == 0);
    CHECK(m.volume.thicknessFactor == 0);
    CHECK(m.volume.attenuationDistance == kPrF32Infinity);
    for (float c : m.volume.attenuationColor)
        CHECK(c == 1);
    CHECK(m.specular.factor == 1);
    for (float c : m.specular.colorFactor)
        CHECK(c == 1);
    CHECK(m.iridescence.factor == 0);
    CHECK(m.iridescence.ior == 1.3f);
    CHECK(m.iridescence.thicknessMinimum == 100);
    CHECK(m.iridescence.thicknessMaximum == 400);
    CHECK(m.anisotropy.strength == 0);
    CHECK(m.anisotropy.rotation == 0);
    CHECK(m.diffuseTransmission.factor == 0);
    for (float c : m.diffuseTransmission.colorFactor)
        CHECK(c == 1);
    for (float c : m.specularGlossiness.diffuseFactor)
        CHECK(c == 1);
    for (float c : m.specularGlossiness.specularFactor)
        CHECK(c == 1);
    CHECK(m.specularGlossiness.glossinessFactor == 1);
    CHECK(m.specularGlossiness.diffuseTexture.texture == kPrGltfNone);
    CHECK(!m.baseColorTexture.hasTransform);
}

TEST_CASE("an extension of the material model reads with its defaults where it states nothing") {
    GltfRead read(oneMaterial(R"("extensions":{"KHR_materials_clearcoat":{},"KHR_materials_sheen":{},)"
        R"("KHR_materials_transmission":{},"KHR_materials_volume":{},"KHR_materials_ior":{},)"
        R"("KHR_materials_specular":{},"KHR_materials_iridescence":{},"KHR_materials_anisotropy":{},)"
        R"("KHR_materials_dispersion":{},"KHR_materials_diffuse_transmission":{},)"
        R"("KHR_materials_pbrSpecularGlossiness":{}})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfMaterial& m = prGltfMaterials(read.gltf)[0];
    CHECK(m.extensions == (PrGltfMaterialExtensionFlag_Clearcoat | PrGltfMaterialExtensionFlag_Sheen |
        PrGltfMaterialExtensionFlag_Transmission | PrGltfMaterialExtensionFlag_Volume | PrGltfMaterialExtensionFlag_Ior |
        PrGltfMaterialExtensionFlag_Specular | PrGltfMaterialExtensionFlag_Iridescence |
        PrGltfMaterialExtensionFlag_Anisotropy |
        PrGltfMaterialExtensionFlag_Dispersion | PrGltfMaterialExtensionFlag_DiffuseTransmission |
        PrGltfMaterialExtensionFlag_SpecularGlossiness));
    CHECK(m.ior == 1.5f);
    CHECK(m.specular.factor == 1);
    CHECK(m.iridescence.ior == 1.3f);
    CHECK(m.volume.attenuationDistance == kPrF32Infinity);
    CHECK(m.specularGlossiness.glossinessFactor == 1);
}

TEST_CASE("the extensions of the material model read each factor and texture") {
    GltfRead read(oneMaterial(R"("extensions":{)"
        R"("KHR_materials_clearcoat":{"clearcoatFactor":0.5,"clearcoatTexture":{"index":0},)"
        R"("clearcoatRoughnessFactor":0.25,"clearcoatRoughnessTexture":{"index":1,"texCoord":1},)"
        R"("clearcoatNormalTexture":{"index":1,"scale":2}},)"
        R"("KHR_materials_sheen":{"sheenColorFactor":[1,0.5,0.25],"sheenColorTexture":{"index":0},)"
        R"("sheenRoughnessFactor":0.75,"sheenRoughnessTexture":{"index":1}},)"
        R"("KHR_materials_transmission":{"transmissionFactor":0.5,"transmissionTexture":{"index":1}},)"
        R"("KHR_materials_volume":{"thicknessFactor":2,"thicknessTexture":{"index":0},)"
        R"("attenuationDistance":0.5,"attenuationColor":[0.5,0.25,1]},)"
        R"("KHR_materials_ior":{"ior":1.25},)"
        R"("KHR_materials_specular":{"specularFactor":0.5,"specularTexture":{"index":0},)"
        R"("specularColorFactor":[2,1,0.5],"specularColorTexture":{"index":1,)"
        R"("extensions":{"KHR_texture_transform":{"offset":[0.5,0]}}}},)"
        R"("KHR_materials_iridescence":{"iridescenceFactor":1,"iridescenceTexture":{"index":0},)"
        R"("iridescenceIor":1.8,"iridescenceThicknessMinimum":200,"iridescenceThicknessMaximum":800,)"
        R"("iridescenceThicknessTexture":{"index":1}},)"
        R"("KHR_materials_anisotropy":{"anisotropyStrength":0.5,"anisotropyRotation":-1.5,)"
        R"("anisotropyTexture":{"index":0}},)"
        R"("KHR_materials_dispersion":{"dispersion":0.25},)"
        R"("KHR_materials_diffuse_transmission":{"diffuseTransmissionFactor":0.25,)"
        R"("diffuseTransmissionTexture":{"index":0},"diffuseTransmissionColorFactor":[0.5,1,0],)"
        R"("diffuseTransmissionColorTexture":{"index":1}},)"
        R"("KHR_materials_pbrSpecularGlossiness":{"diffuseFactor":[0.5,0.5,0.5,0.25],"diffuseTexture":{"index":0},)"
        R"("specularFactor":[0,0.5,1],"glossinessFactor":0.75,"specularGlossinessTexture":{"index":1}}})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfMaterial& m = prGltfMaterials(read.gltf)[0];
    CHECK(m.clearcoat.factor == 0.5f);
    CHECK(m.clearcoat.texture.texture == 0);
    CHECK(m.clearcoat.roughnessFactor == 0.25f);
    CHECK(m.clearcoat.roughnessTexture.texture == 1);
    CHECK(m.clearcoat.roughnessTexture.texCoord == 1);
    CHECK(m.clearcoat.normalTexture.texture == 1);
    CHECK(m.clearcoat.normalScale == 2);
    CHECK(m.sheen.colorFactor[1] == 0.5f);
    CHECK(m.sheen.colorTexture.texture == 0);
    CHECK(m.sheen.roughnessFactor == 0.75f);
    CHECK(m.sheen.roughnessTexture.texture == 1);
    CHECK(m.transmission.factor == 0.5f);
    CHECK(m.transmission.texture.texture == 1);
    CHECK(m.volume.thicknessFactor == 2);
    CHECK(m.volume.thicknessTexture.texture == 0);
    CHECK(m.volume.attenuationDistance == 0.5f);
    CHECK(m.volume.attenuationColor[1] == 0.25f);
    CHECK(m.ior == 1.25f);
    CHECK(m.specular.factor == 0.5f);
    CHECK(m.specular.texture.texture == 0);
    CHECK(m.specular.colorFactor[0] == 2);
    CHECK(m.specular.colorTexture.texture == 1);
    CHECK(m.specular.colorTexture.hasTransform);
    CHECK(m.specular.colorTexture.transform.offset[0] == 0.5f);
    CHECK(m.iridescence.factor == 1);
    CHECK(m.iridescence.texture.texture == 0);
    CHECK(m.iridescence.ior == 1.8f);
    CHECK(m.iridescence.thicknessMinimum == 200);
    CHECK(m.iridescence.thicknessMaximum == 800);
    CHECK(m.iridescence.thicknessTexture.texture == 1);
    CHECK(m.anisotropy.strength == 0.5f);
    CHECK(m.anisotropy.rotation == -1.5f);
    CHECK(m.anisotropy.texture.texture == 0);
    CHECK(m.dispersion == 0.25f);
    CHECK(m.diffuseTransmission.factor == 0.25f);
    CHECK(m.diffuseTransmission.texture.texture == 0);
    CHECK(m.diffuseTransmission.colorFactor[2] == 0);
    CHECK(m.diffuseTransmission.colorTexture.texture == 1);
    CHECK(m.specularGlossiness.diffuseFactor[3] == 0.25f);
    CHECK(m.specularGlossiness.diffuseTexture.texture == 0);
    CHECK(m.specularGlossiness.specularFactor[2] == 1);
    CHECK(m.specularGlossiness.glossinessFactor == 0.75f);
    CHECK(m.specularGlossiness.specularGlossinessTexture.texture == 1);

    // An index of refraction of zero stands for an infinite one.
    GltfRead infinite(oneMaterial(R"("extensions":{"KHR_materials_ior":{"ior":0}})"));
    REQUIRE(infinite.status == PrStatus_Ok);
    CHECK(prGltfMaterials(infinite.gltf)[0].ior == 0);
}

TEST_CASE("the values of the extensions of the material model are checked against their ranges") {
    auto refusal = [&](PrStringView extensions) {
        String material = R"("extensions":{)";
        return gltfRefusal(oneMaterial(material.append(extensions).append("}")));
    };
    CHECK(refusal(R"("KHR_materials_clearcoat":{"clearcoatFactor":1.5})") ==
        "materials[0].KHR_materials_clearcoat.clearcoatFactor is out of range");
    CHECK(refusal(R"("KHR_materials_clearcoat":{"clearcoatRoughnessFactor":-1})") ==
        "materials[0].KHR_materials_clearcoat.clearcoatRoughnessFactor is out of range");
    CHECK(refusal(R"("KHR_materials_clearcoat":{"clearcoatNormalTexture":{"index":5}})") ==
        "materials[0].clearcoatNormalTexture.index is not the index of an object");
    CHECK(refusal(R"("KHR_materials_sheen":{"sheenColorFactor":[1,1]})") ==
        "materials[0].KHR_materials_sheen.sheenColorFactor has the wrong number of elements");
    CHECK(refusal(R"("KHR_materials_sheen":{"sheenColorFactor":[1,1,2]})") ==
        "materials[0].KHR_materials_sheen.sheenColorFactor is out of range");
    CHECK(refusal(R"("KHR_materials_sheen":{"sheenRoughnessFactor":2})") ==
        "materials[0].KHR_materials_sheen.sheenRoughnessFactor is out of range");
    CHECK(refusal(R"("KHR_materials_transmission":{"transmissionFactor":-0.5})") ==
        "materials[0].KHR_materials_transmission.transmissionFactor is out of range");
    CHECK(refusal(R"("KHR_materials_transmission":{"transmissionTexture":{}})") ==
        "materials[0].transmissionTexture.index is required");
    CHECK(refusal(R"("KHR_materials_volume":{"thicknessFactor":-1})") ==
        "materials[0].KHR_materials_volume.thicknessFactor is out of range");
    CHECK(refusal(R"("KHR_materials_volume":{"attenuationDistance":0})") ==
        "materials[0].KHR_materials_volume.attenuationDistance must be greater than zero");
    CHECK(refusal(R"("KHR_materials_volume":{"attenuationColor":[1,1,1.5]})") ==
        "materials[0].KHR_materials_volume.attenuationColor is out of range");
    CHECK(refusal(R"("KHR_materials_ior":{"ior":0.5})") == "materials[0].KHR_materials_ior.ior must be zero or at least 1");
    CHECK(refusal(R"("KHR_materials_ior":{"ior":-1})") == "materials[0].KHR_materials_ior.ior is out of range");
    CHECK(refusal(R"("KHR_materials_specular":{"specularFactor":2})") ==
        "materials[0].KHR_materials_specular.specularFactor is out of range");
    CHECK(refusal(R"("KHR_materials_specular":{"specularColorFactor":[1,-1,1]})") ==
        "materials[0].KHR_materials_specular.specularColorFactor is out of range");
    CHECK(refusal(R"("KHR_materials_iridescence":{"iridescenceFactor":2})") ==
        "materials[0].KHR_materials_iridescence.iridescenceFactor is out of range");
    CHECK(refusal(R"("KHR_materials_iridescence":{"iridescenceIor":0.5})") ==
        "materials[0].KHR_materials_iridescence.iridescenceIor is out of range");
    CHECK(refusal(R"("KHR_materials_iridescence":{"iridescenceThicknessMinimum":-1})") ==
        "materials[0].KHR_materials_iridescence.iridescenceThicknessMinimum is out of range");
    CHECK(refusal(R"("KHR_materials_iridescence":{"iridescenceThicknessMaximum":-1})") ==
        "materials[0].KHR_materials_iridescence.iridescenceThicknessMaximum is out of range");
    CHECK(refusal(R"("KHR_materials_anisotropy":{"anisotropyStrength":1.5})") ==
        "materials[0].KHR_materials_anisotropy.anisotropyStrength is out of range");
    CHECK(refusal(R"("KHR_materials_anisotropy":{"anisotropyRotation":"up"})") ==
        "materials[0].KHR_materials_anisotropy.anisotropyRotation must be a number");
    CHECK(refusal(R"("KHR_materials_dispersion":{"dispersion":-0.5})") ==
        "materials[0].KHR_materials_dispersion.dispersion is out of range");
    CHECK(refusal(R"("KHR_materials_diffuse_transmission":{"diffuseTransmissionFactor":2})") ==
        "materials[0].KHR_materials_diffuse_transmission.diffuseTransmissionFactor is out of range");
    CHECK(refusal(R"("KHR_materials_diffuse_transmission":{"diffuseTransmissionColorFactor":[2,0,0]})") ==
        "materials[0].KHR_materials_diffuse_transmission.diffuseTransmissionColorFactor is out of range");
    CHECK(refusal(R"("KHR_materials_pbrSpecularGlossiness":{"diffuseFactor":[1,1,1]})") ==
        "materials[0].KHR_materials_pbrSpecularGlossiness.diffuseFactor has the wrong number of elements");
    CHECK(refusal(R"("KHR_materials_pbrSpecularGlossiness":{"specularFactor":[1,1,2]})") ==
        "materials[0].KHR_materials_pbrSpecularGlossiness.specularFactor is out of range");
    CHECK(refusal(R"("KHR_materials_pbrSpecularGlossiness":{"glossinessFactor":-1})") ==
        "materials[0].KHR_materials_pbrSpecularGlossiness.glossinessFactor is out of range");
    CHECK(refusal(R"("KHR_materials_clearcoat":true)") == "materials[0].KHR_materials_clearcoat must be an object");
}

TEST_CASE("an image without a media type is known by its first bytes as WebP or KTX 2") {
    GltfRead read(gltfAsset(R"("images":[{"uri":"data:;base64,UklGRgAAAABXRUJQ"},)"
        R"({"uri":"data:;base64,q0tUWCAyMLsNChoK"},{"uri":"data:;base64,UklGRgAAAABXQVZF"}])"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfImage* images = prGltfImages(read.gltf);
    CHECK(images[0].mimeType == "image/webp");
    CHECK(images[1].mimeType == "image/ktx2");
    CHECK(images[2].mimeType.data == nullptr);
}

TEST_CASE("a texture offers its images in each format that its extensions give, and then the core's") {
    GltfRead read(gltfAsset(R"("extensionsUsed":["KHR_texture_basisu","EXT_texture_webp"],)"
        R"("extensionsRequired":["KHR_texture_basisu","EXT_texture_webp"],)"
        R"("images":[{"uri":"a.png"},{"uri":"a.webp"},{"uri":"a.ktx2"}],"textures":[)"
        R"({"source":0,"extensions":{"EXT_texture_webp":{"source":1},"KHR_texture_basisu":{"source":2}}},)"
        R"({"extensions":{"EXT_texture_webp":{"source":1}}},{"source":0},{}])"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfTexture* textures = prGltfTextures(read.gltf);
    REQUIRE(textures[0].sources.count == 3);
    const uint32_t* first = prGltfListIndices(read.gltf, textures[0].sources);
    CHECK(first[0] == 2);
    CHECK(first[1] == 1);
    CHECK(first[2] == 0);
    CHECK(textures[0].source == 0);
    REQUIRE(textures[1].sources.count == 1);
    CHECK(prGltfListIndices(read.gltf, textures[1].sources)[0] == 1);
    CHECK(textures[1].source == kPrGltfNone);
    REQUIRE(textures[2].sources.count == 1);
    CHECK(prGltfListIndices(read.gltf, textures[2].sources)[0] == 0);
    CHECK(textures[3].sources.count == 0);

    CHECK(gltfRefusal(gltfAsset(R"("images":[{"uri":"a.webp"}],"textures":[{"extensions":{"EXT_texture_webp":{}}}])")) ==
        "textures[0].EXT_texture_webp.source is required");
    CHECK(gltfRefusal(gltfAsset(
        R"("images":[{"uri":"a.webp"}],"textures":[{"extensions":{"KHR_texture_basisu":{"source":1}}}])")) ==
        "textures[0].KHR_texture_basisu.source is not the index of an object");
}

// ---------------------------------------------------------------------------------------------------------------------
// Cameras and lights

TEST_CASE("cameras are perspective, with or without a far plane, or orthographic") {
    GltfRead read(gltfAsset(R"("cameras":[)"
        R"({"type":"perspective","name":"finite","perspective":{"yfov":0.5,"aspectRatio":1.5,"znear":0.1,"zfar":100}},)"
        R"({"type":"perspective","perspective":{"yfov":1,"znear":0.25}},)"
        R"({"type":"orthographic","orthographic":{"xmag":2,"ymag":-3,"znear":0,"zfar":10}}],)"
        R"("nodes":[{"camera":2}])"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfCameraCount(read.gltf) == 3);
    const PrGltfCamera* cameras = prGltfCameras(read.gltf);
    CHECK(cameras[0].name == "finite");
    CHECK(cameras[0].type == PrGltfCameraType_Perspective);
    CHECK(cameras[0].yfov == 0.5f);
    CHECK(cameras[0].aspectRatio == 1.5f);
    CHECK(cameras[0].znear == 0.1f);
    CHECK(cameras[0].zfar == 100);
    // An aspect ratio that is not stated is the viewport's, and a far plane that is not stated is at infinity.
    CHECK(cameras[1].aspectRatio == 0);
    CHECK(cameras[1].zfar == kPrF32Infinity);
    CHECK(cameras[2].type == PrGltfCameraType_Orthographic);
    CHECK(cameras[2].xmag == 2);
    CHECK(cameras[2].ymag == -3);
    CHECK(cameras[2].znear == 0);
    CHECK(cameras[2].zfar == 10);
    CHECK(prGltfNodes(read.gltf)[0].camera == 2);
}

TEST_CASE("a camera's projection is checked") {
    auto refusal = [&](PrStringView camera) {
        String members = R"("cameras":[{)";
        return gltfRefusal(gltfAsset(members.append(camera).append("}]")));
    };
    CHECK(refusal(R"("perspective":{"yfov":1,"znear":1})") == "cameras[0].type is required");
    CHECK(refusal(R"("type":"fisheye")") == "cameras[0].type is not a value of its enumeration");
    CHECK(refusal(R"("type":"perspective")") == "cameras[0].perspective is required");
    CHECK(refusal(R"("type":"orthographic","perspective":{"yfov":1,"znear":1})") ==
        "cameras[0].orthographic is required");
    const char* notPositive = "cameras[0].perspective must have a yfov, a znear and any aspectRatio greater than zero";
    CHECK(refusal(R"("type":"perspective","perspective":{"znear":1})") == notPositive);
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":1})") == notPositive);
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":0,"znear":1})") == notPositive);
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":1,"znear":0})") == notPositive);
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":1,"znear":1,"aspectRatio":0})") == notPositive);
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":-1,"znear":1})") ==
        "cameras[0].perspective.yfov is out of range");
    CHECK(refusal(R"("type":"perspective","perspective":{"yfov":1,"znear":2,"zfar":2})") ==
        "cameras[0].perspective.zfar must be greater than znear");
    CHECK(refusal(R"("type":"orthographic","orthographic":{"xmag":0,"ymag":1,"znear":0,"zfar":1})") ==
        "cameras[0].orthographic must have an xmag and a ymag other than zero");
    CHECK(refusal(R"("type":"orthographic","orthographic":{"xmag":1,"znear":0,"zfar":1})") ==
        "cameras[0].orthographic must have an xmag and a ymag other than zero");
    const char* planes = "cameras[0].orthographic must have a znear of zero or more and a zfar greater than it";
    CHECK(refusal(R"("type":"orthographic","orthographic":{"xmag":1,"ymag":1,"znear":1,"zfar":1})") == planes);
    CHECK(refusal(R"("type":"orthographic","orthographic":{"xmag":1,"ymag":1,"zfar":1})") == planes);
    CHECK(refusal(R"("type":"orthographic","orthographic":{"xmag":1,"ymag":1,"znear":0})") == planes);
}

TEST_CASE("punctual lights read from their extension, and a node names its light") {
    GltfRead read(gltfAsset(R"("extensionsUsed":["KHR_lights_punctual"],)"
        R"("extensions":{"KHR_lights_punctual":{"lights":[)"
        R"({"type":"directional","name":"sun","color":[1,0.5,0.25],"intensity":3},)"
        R"({"type":"point","range":12},)"
        R"({"type":"spot","spot":{"innerConeAngle":0.25,"outerConeAngle":0.5}},)"
        R"({"type":"spot","spot":{}}]}},)"
        R"("nodes":[{"extensions":{"KHR_lights_punctual":{"light":2}}},{}])"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfLightCount(read.gltf) == 4);
    const PrGltfLight* lights = prGltfLights(read.gltf);
    CHECK(lights[0].name == "sun");
    CHECK(lights[0].type == PrGltfLightType_Directional);
    CHECK(lights[0].color[1] == 0.5f);
    CHECK(lights[0].color[2] == 0.25f);
    CHECK(lights[0].intensity == 3);
    CHECK(lights[0].range == kPrF32Infinity);
    CHECK(lights[1].type == PrGltfLightType_Point);
    CHECK(lights[1].color[0] == 1);
    CHECK(lights[1].intensity == 1);
    CHECK(lights[1].range == 12);
    CHECK(lights[2].type == PrGltfLightType_Spot);
    CHECK(lights[2].innerConeAngle == 0.25f);
    CHECK(lights[2].outerConeAngle == 0.5f);
    // A spot light's cone defaults to an inner angle of zero and an outer angle of an eighth of a turn.
    CHECK(lights[3].innerConeAngle == 0);
    CHECK(near(lights[3].outerConeAngle, 0.78539816f));
    CHECK(prGltfNodes(read.gltf)[0].light == 2);
    CHECK(prGltfNodes(read.gltf)[1].light == kPrGltfNone);
}

TEST_CASE("a punctual light is checked") {
    auto refusal = [&](PrStringView light) {
        String members = R"("extensions":{"KHR_lights_punctual":{"lights":[{)";
        return gltfRefusal(gltfAsset(members.append(light).append("}]}}")));
    };
    CHECK(refusal(R"("color":[1,1,1])") == "extensions.KHR_lights_punctual.lights[0].type is required");
    CHECK(refusal(R"("type":"area")") ==
        "extensions.KHR_lights_punctual.lights[0].type is not a value of its enumeration");
    CHECK(refusal(R"("type":"spot")") == "extensions.KHR_lights_punctual.lights[0].spot is required for a spot light");
    CHECK(refusal(R"("type":"spot","spot":{"innerConeAngle":0.5,"outerConeAngle":0.5})") ==
        "extensions.KHR_lights_punctual.lights[0].spot.innerConeAngle must be less than outerConeAngle");
    CHECK(refusal(R"("type":"spot","spot":{"outerConeAngle":2})") ==
        "extensions.KHR_lights_punctual.lights[0].spot.outerConeAngle is out of range");
    CHECK(refusal(R"("type":"point","range":0)") == "extensions.KHR_lights_punctual.lights[0].range must be greater than zero");
    CHECK(refusal(R"("type":"point","intensity":-1)") == "extensions.KHR_lights_punctual.lights[0].intensity is out of range");
    CHECK(refusal(R"("type":"point","color":[1,1])") ==
        "extensions.KHR_lights_punctual.lights[0].color has the wrong number of elements");
    CHECK(gltfRefusal(gltfAsset(R"("nodes":[{"extensions":{"KHR_lights_punctual":{"light":0}}}])")) ==
        "nodes[0].light is not the index of an object");
    CHECK(gltfRefusal(gltfAsset(R"("nodes":[{"extensions":{"KHR_lights_punctual":{}}}])")) ==
        "nodes[0].light is required");
}

// ---------------------------------------------------------------------------------------------------------------------
// Nodes, skins and scenes

TEST_CASE("nodes form trees, and each knows its parent") {
    GltfRead read(withNodes(R"({"name":"root","children":[2,1]},{"children":[3]},{},{},{"name":"other"})",
        R"("scenes":[{"name":"main","nodes":[0,4]},{"nodes":[4]},{}],"scene":1)"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfNodeCount(read.gltf) == 5);
    const PrGltfNode* nodes = prGltfNodes(read.gltf);
    CHECK(nodes[0].name == "root");
    CHECK(nodes[0].parent == kPrGltfNone);
    REQUIRE(nodes[0].children.count == 2);
    CHECK(prGltfListIndices(read.gltf, nodes[0].children)[0] == 2);
    CHECK(prGltfListIndices(read.gltf, nodes[0].children)[1] == 1);
    CHECK(nodes[1].parent == 0);
    CHECK(nodes[2].parent == 0);
    CHECK(nodes[3].parent == 1);
    CHECK(nodes[3].children.count == 0);
    CHECK(nodes[4].parent == kPrGltfNone);
    CHECK(nodes[0].mesh == kPrGltfNone);
    CHECK(nodes[0].skin == kPrGltfNone);
    CHECK(nodes[0].camera == kPrGltfNone);
    CHECK(nodes[0].light == kPrGltfNone);

    REQUIRE(prGltfSceneCount(read.gltf) == 3);
    const PrGltfScene* scenes = prGltfScenes(read.gltf);
    CHECK(scenes[0].name == "main");
    REQUIRE(scenes[0].nodes.count == 2);
    CHECK(prGltfListIndices(read.gltf, scenes[0].nodes)[1] == 4);
    CHECK(scenes[2].nodes.count == 0);
    CHECK(prGltfDefaultScene(read.gltf) == 1);
}

TEST_CASE("a node's transform is a matrix, or a translation, a rotation and a scale") {
    // A quarter turn about z, as a quaternion.
    GltfRead read(withNodes(R"({"translation":[5,6,7],"rotation":[0,0,0.70710678,0.70710678],"scale":[2,3,4],)"
        R"("children":[1,2]},{"translation":[1,0,0]},)"
        R"({"matrix":[1,0,0,0, 0,1,0,0, 0,0,1,0, 8,9,10,1]},{})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfNode* nodes = prGltfNodes(read.gltf);
    CHECK(!nodes[0].hasMatrix);
    CHECK(nodes[0].translation[2] == 7);
    CHECK(nodes[0].rotation[2] == 0.70710678f);
    CHECK(nodes[0].scale[1] == 3);

    // The node scales, then turns x toward y, then translates.
    PrMatrix4 local = prGltfNodeLocalMatrix(&nodes[0]);
    CHECK(near(local.m[0][0], 0));
    CHECK(near(local.m[1][0], 2));
    CHECK(near(local.m[0][1], -3));
    CHECK(near(local.m[1][1], 0));
    CHECK(near(local.m[2][2], 4));
    CHECK(local.m[0][3] == 5);
    CHECK(local.m[1][3] == 6);
    CHECK(local.m[2][3] == 7);
    CHECK(local.m[3][3] == 1);
    CHECK(local.m[3][0] == 0);

    // The asset's matrix is in column-major order, so its last four numbers are the translation.
    CHECK(nodes[2].hasMatrix);
    PrMatrix4 matrix = prGltfNodeLocalMatrix(&nodes[2]);
    CHECK(matrix.m[0][3] == 8);
    CHECK(matrix.m[1][3] == 9);
    CHECK(matrix.m[2][3] == 10);
    CHECK(matrix.m[3][0] == 0);
    CHECK(nodes[2].rotation[3] == 1);
    CHECK(nodes[2].scale[0] == 1);

    // A node with nothing stated is the identity.
    PrMatrix4 identity = prGltfNodeLocalMatrix(&nodes[3]);
    PrMatrix4 expected = prMatrix4Identity();
    CHECK(prMemEquals(&identity, &expected, sizeof(identity)));

    // A child's world matrix applies its own transform and then its parent's: (1, 0, 0) is scaled to (2, 0, 0), turned
    // to (0, 2, 0) and translated.
    PrMatrix4 childLocal = prGltfNodeLocalMatrix(&nodes[1]);
    PrMatrix4 childWorld = prGltfNodeWorldMatrix(&childLocal, &local);
    CHECK(near(childWorld.m[0][3], 5));
    CHECK(near(childWorld.m[1][3], 8));
    CHECK(near(childWorld.m[2][3], 7));

    PrMatrix4 world[4];
    prGltfWorldMatrices(read.gltf, world);
    CHECK(prMemEquals(&world[0], &local, sizeof(local)));
    CHECK(prMemEquals(&world[1], &childWorld, sizeof(childWorld)));
    CHECK(near(world[2].m[0][3], 5 - 3 * 9));
    CHECK(near(world[2].m[1][3], 6 + 2 * 8));
    CHECK(near(world[2].m[2][3], 7 + 4 * 10));
    CHECK(prMemEquals(&world[3], &expected, sizeof(expected)));
}

TEST_CASE("the hierarchy of nodes is checked") {
    CHECK(gltfRefusal(withNodes(R"({"children":[1]})")) == "nodes[0].children is not the index of an object");
    CHECK(gltfRefusal(withNodes(R"({"children":[1,1]},{})")) == "nodes[0].children names an object twice");
    CHECK(gltfRefusal(withNodes(R"({"children":[2]},{"children":[2]},{})")) ==
        "nodes[1].children names a node that already has a parent");
    CHECK(gltfRefusal(withNodes(R"({"children":[0]})")) == "nodes has a node that is its own ancestor");
    CHECK(gltfRefusal(withNodes(R"({"children":[1]},{"children":[0]})")) ==
        "nodes has a node that is its own ancestor");
    CHECK(gltfRefusal(withNodes(R"({},{"children":[2]},{"children":[3]},{"children":[1]})")) ==
        "nodes has a node that is its own ancestor");
    CHECK(gltfRefusal(withNodes(R"({"children":7})")) == "nodes[0].children must be an array");
    CHECK(gltfRefusal(withNodes(R"({"matrix":[1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1],"scale":[1,1,1]})")) ==
        "nodes[0].matrix must not be defined with a translation, a rotation or a scale");
    CHECK(gltfRefusal(withNodes(R"({"matrix":[1,0,0,0]})")) == "nodes[0].matrix has the wrong number of elements");
    CHECK(gltfRefusal(withNodes(R"({"rotation":[0,0,0,2]})")) == "nodes[0].rotation is out of range");
    CHECK(gltfRefusal(withNodes(R"({"translation":[0,0]})")) == "nodes[0].translation has the wrong number of elements");
    CHECK(gltfRefusal(withNodes(R"({"mesh":0})")) == "nodes[0].mesh is not the index of an object");
    CHECK(gltfRefusal(withNodes(R"({"camera":0})")) == "nodes[0].camera is not the index of an object");
    CHECK(gltfRefusal(withNodes(R"({"skin":0})")) == "nodes[0].skin is not the index of an object");
    CHECK(gltfRefusal(withNodes(R"(7)")) == "nodes[0] must be an object");

    CHECK(gltfRefusal(withNodes(R"({"children":[1]},{})", R"("scenes":[{"nodes":[1]}])")) ==
        "scenes[0].nodes names a node that is not a root");
    CHECK(gltfRefusal(withNodes(R"({})", R"("scenes":[{"nodes":[0,0]}])")) == "scenes[0].nodes names an object twice");
    CHECK(gltfRefusal(withNodes(R"({})", R"("scenes":[{"nodes":[1]}])")) == "scenes[0].nodes is not the index of an object");
    CHECK(gltfRefusal(withNodes(R"({})", R"("scenes":[{}],"scene":1)")) == "scene is not the index of an object");
}

TEST_CASE("a skin names its joints, and its mesh's vertices name joints of the skin") {
    GltfBin bin;
    bin.put<uint8_t>(36, 0, 1, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1);
    GltfRead read(skinned(R"({"mesh":0,"skin":0},{"children":[2]},{},{"mesh":0,"skin":1})",
        R"({"name":"arm","joints":[1,2],"inverseBindMatrices":3,"skeleton":1},{"joints":[2,1,0]})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfSkinCount(read.gltf) == 2);
    const PrGltfSkin* skins = prGltfSkins(read.gltf);
    CHECK(skins[0].name == "arm");
    REQUIRE(skins[0].joints.count == 2);
    CHECK(prGltfListIndices(read.gltf, skins[0].joints)[0] == 1);
    CHECK(prGltfListIndices(read.gltf, skins[0].joints)[1] == 2);
    CHECK(skins[0].inverseBindMatrices == 3);
    CHECK(skins[0].skeleton == 1);
    CHECK(skins[1].joints.count == 3);
    CHECK(skins[1].inverseBindMatrices == kPrGltfNone);
    CHECK(skins[1].skeleton == kPrGltfNone);
    CHECK(prGltfNodes(read.gltf)[0].skin == 0);
    CHECK(prGltfNodes(read.gltf)[0].mesh == 0);
}

TEST_CASE("a skin is checked against its joints and the meshes drawn with it") {
    GltfBin bin;
    const char* node = R"({"mesh":0,"skin":0},{})";
    CHECK(gltfRefusal(skinned(node, R"({})"), &bin) == "skins[0].joints must name at least one node");
    CHECK(gltfRefusal(skinned(node, R"({"joints":[]})"), &bin) == "skins[0].joints must name at least one node");
    CHECK(gltfRefusal(skinned(node, R"({"joints":[1,1]})"), &bin) == "skins[0].joints names an object twice");
    CHECK(gltfRefusal(skinned(node, R"({"joints":[2]})"), &bin) == "skins[0].joints is not the index of an object");
    CHECK(gltfRefusal(skinned(node, R"({"joints":[1],"skeleton":2})"), &bin) ==
        "skins[0].skeleton is not the index of an object");
    const char* matrices = "skins[0].inverseBindMatrices must be a MAT4 accessor of floats with a matrix for each joint";
    CHECK(gltfRefusal(skinned(node, R"({"joints":[1],"inverseBindMatrices":2})"), &bin) == matrices);
    CHECK(gltfRefusal(skinned(R"({"mesh":0,"skin":0},{},{})", R"({"joints":[0,1,2],"inverseBindMatrices":3})"),
        &bin) == matrices);

    CHECK(gltfRefusal(skinned(R"({"skin":0},{})", R"({"joints":[1]})"), &bin) == "nodes[0].skin needs the node to have a mesh");
    CHECK(gltfRefusal(skinned(R"({"mesh":1,"skin":0},{})", R"({"joints":[1]})"), &bin) ==
        "nodes[0].skin is used with a mesh that has a primitive without joints");

    // The joints of the vertices are zeros, which one joint serves. A vertex that names joint 2 needs three.
    GltfRead one(skinned(node, R"({"joints":[1]})"), &bin);
    CHECK(one.status == PrStatus_Ok);
    bin.put<uint8_t>(36 + 11, 2);
    CHECK(gltfRefusal(skinned(node, R"({"joints":[0,1]})"), &bin) ==
        "nodes[0].skin has fewer joints than a vertex of the node's mesh names");
    GltfRead three(skinned(R"({"mesh":0,"skin":0},{},{})", R"({"joints":[0,1,2]})"), &bin);
    CHECK(three.status == PrStatus_Ok);
}

// ---------------------------------------------------------------------------------------------------------------------
// Visibility, instancing and variants

TEST_CASE("a node is visible unless KHR_node_visibility hides it") {
    GltfRead read(withNodes(R"({},{"extensions":{"KHR_node_visibility":{"visible":false}}},)"
        R"({"extensions":{"KHR_node_visibility":{}}})"));
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfNode* nodes = prGltfNodes(read.gltf);
    CHECK(nodes[0].visible);
    CHECK(!nodes[1].visible);
    CHECK(nodes[2].visible);
    CHECK(gltfRefusal(withNodes(R"({"extensions":{"KHR_node_visibility":{"visible":0}}})")) ==
        "nodes[0].KHR_node_visibility.visible must be true or false");
}

TEST_CASE("a node draws instances of its mesh, each at its own transform") {
    GltfBin bin;
    bin.put<float>(48, 0, 0, 0, 1, 2, 3, -1, 0, 0);
    // The identity, a half turn about z, and the identity again, as normalized shorts.
    bin.put<int16_t>(96, 0, 0, 0, 32767, 0, 0, 32767, 0, 0, 0, 0, 32767);
    bin.put<float>(128, 1, 1, 1, 2, 2, 2, 1, 1, 1);
    GltfRead read(instanced(R"({"attributes":{"TRANSLATION":1,"ROTATION":2,"SCALE":3,"_ID":4}})"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    const PrGltfInstancing& instancing = prGltfNodes(read.gltf)[0].instancing;
    CHECK(instancing.count == 3);
    CHECK(instancing.translation == 1);
    CHECK(instancing.rotation == 2);
    CHECK(instancing.scale == 3);
    REQUIRE(instancing.attributeCount == 1);
    const PrGltfAttribute& id = prGltfAttributes(read.gltf)[instancing.attributeFirst];
    CHECK(id.name == "_ID");
    CHECK(id.semantic == PrGltfSemantic_Custom);
    CHECK(id.accessor == 4);

    PrMatrix4 matrices[3];
    REQUIRE(prGltfInstanceMatrices(read.gltf, 0, 0, 3, matrices));
    CHECK(matrices[0].m[0][0] == 1);
    CHECK(matrices[0].m[0][3] == 0);
    // Scaled by 2, turned half about z, then moved to (1, 2, 3).
    CHECK(near(matrices[1].m[0][0], -2));
    CHECK(near(matrices[1].m[1][1], -2));
    CHECK(near(matrices[1].m[2][2], 2));
    CHECK(matrices[1].m[0][3] == 1);
    CHECK(matrices[1].m[1][3] == 2);
    CHECK(matrices[1].m[2][3] == 3);
    PrMatrix4 last;
    REQUIRE(prGltfInstanceMatrices(read.gltf, 0, 2, 1, &last));
    CHECK(last.m[0][3] == -1);

    // A transform that the asset does not give is the identity, and a node without the extension has no instances.
    GltfRead moved(instanced(R"({"attributes":{"TRANSLATION":1}})"), &bin);
    REQUIRE(moved.status == PrStatus_Ok);
    REQUIRE(prGltfInstanceMatrices(moved.gltf, 0, 1, 1, &last));
    CHECK(last.m[0][0] == 1);
    CHECK(last.m[2][3] == 3);
    GltfRead plain(gltfBinAsset(String(kInstanceAccessors).append(R"(,"nodes":[{"mesh":0}])")), &bin);
    REQUIRE(plain.status == PrStatus_Ok);
    CHECK(prGltfNodes(plain.gltf)[0].instancing.count == 0);
    CHECK(prGltfNodes(plain.gltf)[0].instancing.translation == kPrGltfNone);

    // Without the bytes of the buffer, every instance is at the identity.
    GltfRead absent(instanced(R"({"attributes":{"TRANSLATION":1}})"));
    REQUIRE(absent.status == PrStatus_Ok);
    CHECK(!prGltfInstanceMatrices(absent.gltf, 0, 1, 1, &last));
    CHECK(last.m[2][3] == 0);
}

TEST_CASE("a node's instances are checked against the extension's table") {
    GltfBin bin;
    const char* mistyped = "nodes[0].EXT_mesh_gpu_instancing.attributes has an accessor of a type that its attribute does "
        "not allow";
    CHECK(gltfRefusal(instanced(R"({"attributes":{"TRANSLATION":2}})"), &bin) == mistyped);
    CHECK(gltfRefusal(instanced(R"({"attributes":{"SCALE":4}})"), &bin) == mistyped);
    // A rotation of integers must be normalized and signed.
    CHECK(gltfRefusal(instanced(R"({"attributes":{"ROTATION":6}})"), &bin) == mistyped);
    CHECK(gltfRefusal(instanced(R"({"attributes":{"TRANSLATION":1,"_ID":5}})"), &bin) ==
        "nodes[0].EXT_mesh_gpu_instancing.attributes has accessors of different counts");
    CHECK(gltfRefusal(instanced(R"({"attributes":{"TRANSLATION":9}})"), &bin) ==
        "nodes[0].EXT_mesh_gpu_instancing.attributes is not the index of an object");
    CHECK(gltfRefusal(instanced(R"({"attributes":{}})"), &bin) ==
        "nodes[0].EXT_mesh_gpu_instancing.attributes must hold at least one attribute");
    CHECK(gltfRefusal(instanced(R"({})"), &bin) ==
        "nodes[0].EXT_mesh_gpu_instancing.attributes must hold at least one attribute");
    CHECK(gltfRefusal(instanced(R"({"attributes":{"TRANSLATION":1}})", ""), &bin) ==
        "nodes[0].EXT_mesh_gpu_instancing needs the node to have a mesh");
}

TEST_CASE("a primitive takes a material in each variant that a mapping names, and its own in the others") {
    GltfRead read(withVariants(kThreeVariants,
        R"({"material":1,"variants":[0,2],"name":"warm"},{"material":2,"variants":[1]})"));
    REQUIRE(read.status == PrStatus_Ok);
    REQUIRE(prGltfVariantCount(read.gltf) == 3);
    CHECK(prGltfVariantName(read.gltf, 0) == "red");
    CHECK(prGltfVariantName(read.gltf, 2) == "blue");
    const PrGltfPrimitive& primitive = prGltfPrimitives(read.gltf)[0];
    REQUIRE(primitive.mappingCount == 2);
    REQUIRE(prGltfVariantMappingCount(read.gltf) == 2);
    const PrGltfVariantMapping& warm = prGltfVariantMappings(read.gltf)[primitive.mappingFirst];
    CHECK(warm.material == 1);
    CHECK(warm.name == "warm");
    REQUIRE(warm.variants.count == 2);
    CHECK(prGltfListIndices(read.gltf, warm.variants)[1] == 2);
    CHECK(prGltfPrimitiveMaterial(read.gltf, 0, 0) == 1);
    CHECK(prGltfPrimitiveMaterial(read.gltf, 0, 1) == 2);
    CHECK(prGltfPrimitiveMaterial(read.gltf, 0, 2) == 1);
    CHECK(prGltfPrimitiveMaterial(read.gltf, 0, kPrGltfNone) == 0);

    // A variant that no mapping names keeps the primitive's own material.
    GltfRead some(withVariants(kThreeVariants, R"({"material":2,"variants":[1]})"));
    REQUIRE(some.status == PrStatus_Ok);
    CHECK(prGltfPrimitiveMaterial(some.gltf, 0, 0) == 0);
    CHECK(prGltfPrimitiveMaterial(some.gltf, 0, 1) == 2);

    GltfRead none(gltfAsset(""));
    REQUIRE(none.status == PrStatus_Ok);
    CHECK(prGltfVariantCount(none.gltf) == 0);
    CHECK(prGltfVariantMappingCount(none.gltf) == 0);
}

TEST_CASE("variants and their mappings are checked") {
    CHECK(gltfRefusal(withVariants(R"({"name":"red"},{})", "")) ==
        "extensions.KHR_materials_variants.variants[1].name is required");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"({"material":1,"variants":[0]},{"material":2,"variants":[1,0]})")) ==
        "meshes[0].primitives[0].KHR_materials_variants.variants names a variant that another mapping of the primitive names");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"({"material":1,"variants":[3]})")) ==
        "meshes[0].primitives[0].KHR_materials_variants.variants is not the index of an object");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"({"material":3,"variants":[0]})")) ==
        "meshes[0].primitives[0].KHR_materials_variants.material is not the index of an object");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"({"variants":[0]})")) ==
        "meshes[0].primitives[0].KHR_materials_variants.material is required");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"({"material":1,"variants":[]})")) ==
        "meshes[0].primitives[0].KHR_materials_variants.variants must name at least one variant");
    CHECK(gltfRefusal(withVariants(kThreeVariants, R"(7)")) ==
        "meshes[0].primitives[0].KHR_materials_variants.mappings must hold objects");
}

// ---------------------------------------------------------------------------------------------------------------------
// KHR_mesh_quantization

TEST_CASE("an asset that uses KHR_mesh_quantization stores its attributes in small integers") {
    GltfBin bin;
    bin.put<int16_t>(48, -4, 0, 0, 0, 4, 8, 0, 0, 4, 0, 0, 0);
    bin.put<uint8_t>(72, 0, 51, 0, 0, 255, 255, 0, 0, 255, 51, 0, 0);
    const PrStringView accepted[] = {
        R"("POSITION":1)",
        R"("POSITION":2)",
        R"("POSITION":7)",
        R"("POSITION":0,"NORMAL":3)",
        R"("POSITION":0,"TANGENT":4)",
        R"("POSITION":0,"TEXCOORD_0":5)",
        R"("POSITION":0,"TEXCOORD_0":6)",
        R"("POSITION":0,"TEXCOORD_0":10)",
    };
    for (PrStringView attributes : accepted) {
        CAPTURE(attributes);
        GltfRead read(quantizedAsset(attributes), &bin);
        CHECK(read.status == PrStatus_Ok);
        // The core does not allow these types.
        CHECK(gltfRefusal(quantizedAsset(attributes, "", false), &bin) ==
            "meshes[0].primitives[0].attributes has an accessor of a type that its semantic does not allow");
    }
    // Morph targets displace by signed integers.
    GltfRead target(quantizedAsset(R"("POSITION":0,"NORMAL":0,"TANGENT":4)", R"("POSITION":1,"NORMAL":3)"), &bin);
    CHECK(target.status == PrStatus_Ok);

    // Quantized positions read as the floats that their integers stand for.
    GltfRead read(quantizedAsset(R"("POSITION":2)"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    float positions[9];
    REQUIRE(prGltfAccessorReadFloats(read.gltf, 2, 0, 3, positions));
    CHECK(positions[1] == 0.2f);
    CHECK(positions[3] == 1);

    const char* mistyped = "meshes[0].primitives[0].attributes has an accessor of a type that its semantic does not allow";
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":8)"), &bin) == mistyped);
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":0,"NORMAL":2)"), &bin) == mistyped);
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":0,"TANGENT":9)"), &bin) == mistyped);
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":0,"TEXCOORD_0":1)"), &bin) == mistyped);
    const char* mistypedTarget = "meshes[0].primitives[0].targets has an accessor of a type that its semantic does not allow";
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":0)", R"("POSITION":7)"), &bin) == mistypedTarget);
    CHECK(gltfRefusal(quantizedAsset(R"("POSITION":0)", R"("POSITION":2)"), &bin) == mistypedTarget);
}

TEST_CASE("the bounds of an accessor are those of the values that its integers stand for") {
    GltfBin bin;
    bin.put<int16_t>(48, -4, 0, 0, 0, 4, 8, 0, 0, 4, 0, 0, 0);
    bin.put<uint8_t>(72, 0, 51, 0, 0, 255, 255, 0, 0, 255, 51, 0, 0);
    GltfRead read(quantizedAsset(R"("POSITION":1)"), &bin);
    REQUIRE(read.status == PrStatus_Ok);
    float low[3];
    float high[3];
    REQUIRE(prGltfAccessorBounds(read.gltf, 1, low, high));
    CHECK(low[0] == -4);
    CHECK(high[1] == 8);
    REQUIRE(prGltfAccessorBounds(read.gltf, 2, low, high));
    CHECK(low[1] == 0.2f);
    CHECK(high[0] == 1);
    CHECK(!prGltfAccessorBounds(read.gltf, 3, low, high));
}
