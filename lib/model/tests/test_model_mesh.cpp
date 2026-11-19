// How a model prepares a primitive for the renderer, and how it decodes an image.

#include "pr/array.h"
#include "pr/blob.h"
#include "pr/gltf.h"
#include "pr/gltf_model.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mesh.h"
#include "pr/mesh_cache.h"
#include "pr/model_source.h"
#include "pr/raw_array.h"
#include "pr/render_resource.h"
#include "pr/sort.h"
#include "pr/string.h"

#include "model/src/model_mesh.h"
#include "model/tests/model_fixture.h"
#include "tools/harness/render_fixture.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// A mesh prepared from the primitives of an asset's first mesh. A vertex of a prepared mesh: its geometry, its two sets
// of texture coordinates, or zeros where it has none, and its color, clamped to [0, 1], or white where it has none.
struct ModelVertex {
    PrRenderGeometryVertex geometry;
    float uv0[2];
    float uv1[2];
    float color[4];
};

// A mesh as the model prepares it: its vertices' geometry, indices, parts, skin and morph deltas.
struct PreparedMesh {
    RawArray<ModelVertex> vertices;
    RawArray<uint32_t> indices;
    RawArray<PrRenderMeshPart> parts;
    RawArray<PrRenderSkinVertex> skin;
    RawArray<PrRenderMorphDelta> morphs;
};

// The meshes of a document, read through glTF's model source and prepared one after another as one mesh, with their
// texture coordinates and colors.
struct Prepared {
    PreparedMesh data{};
    PrStatus status = PrStatus_Ok;

    Prepared(const PrGltf* gltf, bool wantsTangents, uint32_t tangentSet = 0, bool skinned = false,
            uint32_t morphTargetCount = 0, bool keepsTriangleOrder = false) {
        PrModelSource* source = prGltfModelSourceCreate(gltf, nullptr);
        const PrModelSourceInfo& info = *prModelSourceInfo(source);
        const PrStringView streams[] = {"TEXCOORD_0", "TEXCOORD_1", "COLOR_0"};
        for (uint32_t i = 0; status == PrStatus_Ok && i < info.meshCount; ++i) {
            PrModelMeshRequest request{
                .streams = streams,
                .streamCount = 3,
                .skin = skinned,
                .morphs = morphTargetCount != 0,
            };
            PrModelMeshData read{};
            status = prModelSourceMeshRead(source, i, &request, &read);
            ModelMeshPrepared prepared{};
            Array<ModelPartOptions> parts;
            for (uint32_t k = 0; k < read.partCount; ++k)
                parts.push(ModelPartOptions{
                    wantsTangents, tangentSet < 2 ? tangentSet : kModelStreamNone,
                    keepsTriangleOrder,
                });
            uint32_t widths[3] = {};
            for (uint32_t k = 0; status == PrStatus_Ok && k < 3; ++k)
                widths[k] = read.streamWidths != nullptr ? read.streamWidths[k] : 0;
            modelMeshPreparedInit(&prepared, widths, 3);
            ModelMeshOptions options{parts.data(), skinned, morphTargetCount};
            if (status == PrStatus_Ok)
                status = modelMeshPrepare(read, options, &prepared);
            uint32_t base = data.vertices.count;
            uint32_t firstIndex = data.indices.count;
            for (uint32_t k = 0; status == PrStatus_Ok && k < prepared.parts.count; ++k) {
                uint32_t has = prepared.partStreams.data[k];
                for (uint32_t v = 0; v < prepared.partVertexCount.data[k]; ++v) {
                    uint32_t at = prepared.partFirstVertex.data[k] + v;
                    const float* raw = reinterpret_cast<const float*>(modelMeshVertex(prepared, at) + 1);
                    ModelVertex vertex{.geometry = *modelMeshVertex(prepared, at), .color = {1, 1, 1, 1}};
                    for (uint32_t c = 0; (has & 1) != 0 && c < 2; ++c)
                        vertex.uv0[c] = raw[prepared.streamOffsets.data[0] + c];
                    for (uint32_t c = 0; (has & 2) != 0 && c < 2; ++c)
                        vertex.uv1[c] = raw[prepared.streamOffsets.data[1] + c];
                    for (uint32_t c = 0; (has & 4) != 0 && c < 4; ++c) {
                        float value = c < widths[2] ? raw[prepared.streamOffsets.data[2] + c] : 1;
                        vertex.color[c] = value < 0 ? 0 : (value > 1 ? 1 : value);
                    }
                    data.vertices.push(vertex);
                }
            }
            for (uint32_t k = 0; status == PrStatus_Ok && k < prepared.indices.count; ++k)
                data.indices.push(prepared.indices.data[k] + base);
            for (uint32_t k = 0; status == PrStatus_Ok && k < prepared.parts.count; ++k) {
                PrRenderMeshPart part = prepared.parts.data[k];
                part.firstIndex += firstIndex;
                data.parts.push(part);
            }
            if (status == PrStatus_Ok) {
                data.skin.append(prepared.skin.span());
                data.morphs.append(prepared.morphs.span());
            }
            modelMeshPreparedReset(&prepared);
            prModelMeshDataReset(&read);
        }
        prModelSourceDestroy(source);
    }
    Prepared(const Prepared&) = delete;
    Prepared& operator=(const Prepared&) = delete;
    ~Prepared() {
        data.vertices.reset();
        data.indices.reset();
        data.parts.reset();
        data.skin.reset();
        data.morphs.reset();
    }

    // Returns true if the mesh's indices from `first` are `expected`.
    bool hasIndices(uint32_t first, Span<const uint32_t> expected) const {
        if (first + expected.count() > data.indices.count)
            return false;
        for (uint32_t i = 0; i < expected.count(); ++i) {
            if (data.indices.data[first + i] != expected[i])
                return false;
        }
        return true;
    }

    // Returns true if the mesh's indices from `first` draw the triangles `expected`, whose corners are vertices of the
    // quad in the document's numbering: each triangle once, with its corners in their order, the triangles in any.
    bool drawsQuadTriangles(uint32_t first, Span<const uint32_t> expected) const {
        static const float kQuad[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        if (first + expected.count() > data.indices.count)
            return false;
        uint32_t triangleCount = expected.count() / 3;
        Array<bool> drawn;
        drawn.resize(triangleCount);
        for (uint32_t t = 0; t < triangleCount; ++t) {
            bool found = false;
            for (uint32_t e = 0; e < triangleCount && !found; ++e) {
                bool same = !drawn[e];
                for (uint32_t corner = 0; corner < 3; ++corner) {
                    const float* p = data.vertices.data[data.indices.data[first + t * 3 + corner]].geometry.position;
                    const float* q = kQuad[expected[e * 3 + corner]];
                    same = same && p[0] == q[0] && p[1] == q[1];
                }
                if (same)
                    drawn[e] = found = true;
            }
            if (!found)
                return false;
        }
        return true;
    }

    // Returns true if the indices from `first` name the `vertexCount` vertices from `firstVertex` and no others, in the
    // order of their numbers: the first use of each vertex follows the first use of the vertex before it.
    bool usesVerticesInOrder(uint32_t first, uint32_t count, uint32_t firstVertex, uint32_t vertexCount) const {
        uint32_t next = firstVertex;
        for (uint32_t i = 0; i < count; ++i) {
            uint32_t index = data.indices.data[first + i];
            if (index < firstVertex || index > next)
                return false;
            if (index == next)
                ++next;
        }
        return next == firstVertex + vertexCount;
    }

    // Returns the vertex at a point of the quad's plane. Fails the test if the mesh has not exactly one there.
    ModelVertex at(float x, float y) const {
        ModelVertex found{};
        uint32_t count = 0;
        for (uint32_t i = 0; i < data.vertices.count; ++i) {
            if (data.vertices.data[i].geometry.position[0] == x && data.vertices.data[i].geometry.position[1] == y) {
                found = data.vertices.data[i];
                ++count;
            }
        }
        CHECK(count == 1);
        return found;
    }
};

bool hasNormal(const ModelVertex& vertex, int16_t x, int16_t y, int16_t z) {
    return vertex.geometry.normal[0] == x && vertex.geometry.normal[1] == y && vertex.geometry.normal[2] == z;
}

bool hasTangent(const ModelVertex& vertex, int16_t x, int16_t y, int16_t z, int16_t handedness) {
    const int16_t* t = vertex.geometry.tangent;
    return t[0] == x && t[1] == y && t[2] == z && t[3] == handedness;
}

// A grid of squares in the plane z = 0, two triangles each, whose triangles the document lists in a scattered order,
// and a vertex after the grid's that no triangle uses. Vertex i of the grid is at (i % kGridSide, i / kGridSide), has
// joint i alone, and is moved by (0, 0, i + 1) by the one morph target.
constexpr uint32_t kGridSide = 9;
constexpr uint32_t kGridVertices = kGridSide * kGridSide;
constexpr uint32_t kGridTriangles = (kGridSide - 1) * (kGridSide - 1) * 2;
constexpr uint32_t kGridIndices = kGridTriangles * 3;

struct GridAsset {
    Array<uint8_t> bin;
    Array<uint16_t> indices;
    PrGltf* PR_NULLABLE gltf = nullptr;

    GridAsset() = default;
    GridAsset(const GridAsset&) = delete;
    GridAsset& operator=(const GridAsset&) = delete;
    ~GridAsset() { prGltfDestroy(gltf); }

    template <typename T>
    void put(const T& value) {
        uint32_t at = bin.count();
        bin.resize(at + uint32_t(sizeof(T)));
        prMemCopy(bin.begin() + at, &value, sizeof(T));
    }

    static PrBlob* PR_NULLABLE load(void* PR_NULLABLE context, PrGltfExternal, PrStringView uri) {
        const GridAsset* asset = static_cast<const GridAsset*>(context);
        return uri == "grid" ? prBlobCopy(PrByteView{asset->bin.begin(), asset->bin.count()}) : nullptr;
    }

    // Reads the asset, whose mesh has `primitives` primitives of the grid, with normals or without.
    void read(bool hasNormals, uint32_t primitives = 1) {
        uint32_t count = kGridVertices + 1;
        for (uint32_t i = 0; i < count; ++i) {
            const float position[3] = {float(i % kGridSide), float(i / kGridSide), 0};
            put(position);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const float normal[3] = {0, 0, 1};
            put(normal);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const uint16_t joints[4] = {uint16_t(i), 0, 0, 0};
            put(joints);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const float weights[4] = {1, 0, 0, 0};
            put(weights);
        }
        for (uint32_t i = 0; i < count; ++i) {
            const float moved[3] = {0, 0, float(i + 1)};
            put(moved);
        }
        Array<uint16_t> inOrder;
        for (uint32_t y = 0; y + 1 < kGridSide; ++y) {
            for (uint32_t x = 0; x + 1 < kGridSide; ++x) {
                uint16_t corner = uint16_t(y * kGridSide + x);
                const uint16_t square[6] = {
                    corner,
                    uint16_t(corner + 1),
                    uint16_t(corner + kGridSide),
                    uint16_t(corner + kGridSide),
                    uint16_t(corner + 1),
                    uint16_t(corner + kGridSide + 1),
                };
                for (uint16_t index : square)
                    inOrder.push(index);
            }
        }
        // 37 has no factor in common with the number of triangles, so every triangle is listed once.
        for (uint32_t t = 0; t < kGridTriangles; ++t) {
            for (uint32_t corner = 0; corner < 3; ++corner)
                indices.push(inOrder[t * 37 % kGridTriangles * 3 + corner]);
        }
        for (uint16_t index : indices)
            put(index);

        uint32_t vec3Bytes = count * 12;
        uint32_t jointsAt = 2 * vec3Bytes;
        uint32_t weightsAt = jointsAt + count * 8;
        uint32_t movedAt = weightsAt + count * 16;
        uint32_t indicesAt = movedAt + vec3Bytes;
        String text = format(
            R"({{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{},"uri":"grid"}}],)"
            R"("bufferViews":[{{"buffer":0,"byteLength":{}}}],"accessors":[)"
            R"({{"bufferView":0,"componentType":5126,"count":{},"type":"VEC3","min":[0,0,0],"max":[{},{},0]}},)"
            R"({{"bufferView":0,"byteOffset":{},"componentType":5126,"count":{},"type":"VEC3"}},)"
            R"({{"bufferView":0,"byteOffset":{},"componentType":5123,"count":{},"type":"VEC4"}},)"
            R"({{"bufferView":0,"byteOffset":{},"componentType":5126,"count":{},"type":"VEC4"}},)"
            R"({{"bufferView":0,"byteOffset":{},"componentType":5126,"count":{},"type":"VEC3","min":[0,0,1],"max":[0,0,{}]}},)"
            R"({{"bufferView":0,"byteOffset":{},"componentType":5123,"count":{},"type":"SCALAR"}}],)"
            R"("meshes":[{{"primitives":[)",
            bin.count(), bin.count(), count, kGridSide - 1, kGridSide, vec3Bytes, count, jointsAt, count, weightsAt,
            count, movedAt, count, count, indicesAt, kGridIndices);
        for (uint32_t i = 0; i < primitives; ++i) {
            text.append(i != 0 ? "," : "");
            text.append(R"({"attributes":{"POSITION":0,"JOINTS_0":2,"WEIGHTS_0":3)");
            text.append(hasNormals ? R"(,"NORMAL":1)" : "");
            text.append(R"(},"indices":5,"targets":[{"POSITION":4}]})");
        }
        text.append("]}]}");
        PrGltfOptions options = prGltfOptionsDefault();
        options.load = load;
        options.loadContext = this;
        PrStringView json = text;
        PrBlob* blob = prBlobCopy(PrByteView{reinterpret_cast<const uint8_t*>(json.data), json.size});
        PrStatus status = prGltfCreate(blob, &options, &gltf);
        prBlobRelease(blob);
        REQUIRE(status == PrStatus_Ok);
    }

    // Returns the document's triangles, in its order, as gridTriangleKeys returns a mesh's.
    Array<uint32_t> triangleKeys() const {
        Array<uint32_t> keys;
        for (uint32_t t = 0; t < kGridTriangles; ++t)
            keys.push(uint32_t(indices[t * 3]) << 16 | uint32_t(indices[t * 3 + 1]) << 8 | indices[t * 3 + 2]);
        return keys;
    }
};

// Returns the vertex of the grid, in the document's numbering, that a prepared vertex is at.
uint32_t gridVertexAt(const ModelVertex& vertex) {
    return uint32_t(vertex.geometry.position[1]) * kGridSide + uint32_t(vertex.geometry.position[0]);
}

// Returns a key for each triangle of a prepared mesh of grids, in the order in which they are drawn: the vertices of
// the grid at its three corners, in their order.
Array<uint32_t> gridTriangleKeys(const PreparedMesh& data) {
    Array<uint32_t> keys;
    for (uint32_t k = 0; k + 2 < data.indices.count; k += 3) {
        const uint32_t* t = data.indices.data + k;
        keys.push(gridVertexAt(data.vertices.data[t[0]]) << 16 | gridVertexAt(data.vertices.data[t[1]]) << 8 |
            gridVertexAt(data.vertices.data[t[2]]));
    }
    return keys;
}

void sortKeys(Array<uint32_t>* keys) {
    sortItems(keys->span(), [](uint32_t a, uint32_t b) { return a < b; });
}

// The members of an asset whose mesh has one primitive with `primitive` as its members.
String onePrimitive(PrStringView primitive) {
    String members = R"("meshes":[{"primitives":[{)";
    return members.append(primitive).append("}]}]");
}

} // namespace

} // namespace pr_impl

using namespace harness;
using namespace pr_impl;

TEST_SUITE_BEGIN("model");

TEST_CASE("every mode that forms triangles becomes a part of a triangle list with 32-bit indices") {
    ModelAsset asset;
    // A list with 8-bit indices, a strip and a fan with 16-bit indices, and a list without indices.
    Prepared mesh(asset.read(
        R"("meshes":[{"primitives":[{"attributes":{"POSITION":0,"NORMAL":5},"indices":1},)"
        R"({"attributes":{"POSITION":0,"NORMAL":5},"indices":7,"mode":5},)"
        R"({"attributes":{"POSITION":0,"NORMAL":5},"indices":8,"mode":6},)"
        R"({"attributes":{"POSITION":14,"NORMAL":15}}]}])"), false);
    REQUIRE(mesh.status == PrStatus_Ok);
    REQUIRE(mesh.data.parts.count == 4);
    CHECK(mesh.data.vertices.count == 15);
    CHECK(mesh.data.indices.count == 21);
    // Each part's indices name its own vertices, which follow the vertices of the parts before it, in the order in
    // which its triangles first use them. The fan's triangles start at vertices 1 and 3 of the quad, so its vertices
    // are stored in another order than the document's.
    const uint32_t list[] = {0, 1, 2, 2, 1, 3};
    const uint32_t strip[] = {4, 5, 6, 5, 7, 6};
    const uint32_t fan[] = {8, 9, 10, 9, 11, 10};
    const uint32_t unindexed[] = {12, 13, 14};
    CHECK(mesh.hasIndices(0, list));
    CHECK(mesh.hasIndices(6, strip));
    CHECK(mesh.hasIndices(12, fan));
    CHECK(mesh.hasIndices(18, unindexed));
    // Each part draws the triangles of its primitive, by the quad's vertices in the document's numbering.
    const uint32_t listTriangles[] = {0, 1, 2, 2, 1, 3};
    const uint32_t stripTriangles[] = {0, 1, 2, 1, 3, 2};
    const uint32_t fanTriangles[] = {1, 3, 0, 3, 2, 0};
    const uint32_t unindexedTriangles[] = {0, 1, 2};
    CHECK(mesh.drawsQuadTriangles(0, listTriangles));
    CHECK(mesh.drawsQuadTriangles(6, stripTriangles));
    CHECK(mesh.drawsQuadTriangles(12, fanTriangles));
    CHECK(mesh.drawsQuadTriangles(18, unindexedTriangles));
    for (uint32_t i = 0; i < 4; ++i) {
        const PrRenderMeshPart& part = mesh.data.parts.data[i];
        CHECK(part.firstIndex == i * 6);
        CHECK(part.indexCount == (i < 3 ? 6u : 3u));
        // The sphere holds the quad's corners, which are the square root of 2 from its center.
        CHECK(part.bounds.radius >= 1.41f);
        CHECK(part.bounds.radius < 1.5f);
    }
}

TEST_CASE("a primitive's attributes are packed as the renderer's vertex holds them") {
    ModelAsset asset;
    Prepared mesh(asset.read(onePrimitive(
        R"("attributes":{"POSITION":0,"NORMAL":5,"TANGENT":6,"TEXCOORD_0":2,"TEXCOORD_1":9,"COLOR_0":11},"indices":1)")),
        true);
    REQUIRE(mesh.status == PrStatus_Ok);
    REQUIRE(mesh.data.vertices.count == 4);
    ModelVertex corner = mesh.at(1, -1);
    CHECK(hasNormal(corner, 0, 0, 32767));
    // The asset's own tangents are kept, although the material wants tangents.
    CHECK(hasTangent(corner, 0, 32767, 0, -32767));
    CHECK(corner.uv0[0] == 1);
    CHECK(corner.uv0[1] == 1);
    CHECK(corner.uv1[0] == 0);
    CHECK(corner.uv1[1] == 1);
    // The color is (0, 1, 0, -1), clamped.
    CHECK(corner.color[0] == 0);
    CHECK(corner.color[1] == 1);
    CHECK(corner.color[2] == 0);
    CHECK(corner.color[3] == 0);

    // A vertex without a color is white, and one without the other attributes has zeros.
    ModelAsset bare;
    Prepared plain(bare.read(onePrimitive(R"("attributes":{"POSITION":0,"NORMAL":5},"indices":1)")), false);
    REQUIRE(plain.status == PrStatus_Ok);
    corner = plain.at(1, -1);
    CHECK(corner.color[0] == 1);
    CHECK(corner.color[3] == 1);
    CHECK(hasTangent(corner, 0, 0, 0, 32767));
    CHECK(corner.uv0[0] == 0);
}

TEST_CASE("a primitive without normals gets the normals of its faces, and loses its tangents") {
    ModelAsset asset;
    Prepared mesh(asset.read(onePrimitive(R"("attributes":{"POSITION":0,"TANGENT":6,"TEXCOORD_0":2},"indices":1)")), false);
    REQUIRE(mesh.status == PrStatus_Ok);
    // The quad's two faces have one normal, so the corners that they share are shared again.
    CHECK(mesh.data.vertices.count == 4);
    CHECK(mesh.data.indices.count == 6);
    for (uint32_t i = 0; i < mesh.data.vertices.count; ++i) {
        CHECK(hasNormal(mesh.data.vertices.data[i], 0, 0, 32767));
        CHECK(hasTangent(mesh.data.vertices.data[i], 0, 0, 0, 32767));
    }
    // Each triangle keeps its winding: its corners run counterclockwise about z.
    for (uint32_t t = 0; t < 2; ++t) {
        const float* a = mesh.data.vertices.data[mesh.data.indices.data[t * 3]].geometry.position;
        const float* b = mesh.data.vertices.data[mesh.data.indices.data[t * 3 + 1]].geometry.position;
        const float* c = mesh.data.vertices.data[mesh.data.indices.data[t * 3 + 2]].geometry.position;
        CHECK((b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]) > 0);
    }

    // Faces that meet at an angle keep a vertex each where they meet. The quad's second triangle is lifted at its last
    // corner, so the two faces differ.
    ModelAsset folded;
    folded.bin.put<float>(44, 1);
    Prepared creased(folded.read(onePrimitive(R"("attributes":{"POSITION":10},"indices":1)")), false);
    REQUIRE(creased.status == PrStatus_Ok);
    CHECK(creased.data.vertices.count == 6);
}

TEST_CASE("a primitive without tangents gets MikkTSpace's where its material has a normal map") {
    // The image is upright on the quad: u runs along x, and v runs down y. The tangent is along u. glTF's bitangent,
    // which is the cross product of the normal and the tangent times the handedness, points up the image, along y.
    ModelAsset asset;
    const PrGltf* gltf = asset.read(onePrimitive(R"("attributes":{"POSITION":0,"NORMAL":5,"TEXCOORD_0":2,"TEXCOORD_1":9},)"
        R"("indices":1)"));
    Prepared upright(gltf, true, 0);
    REQUIRE(upright.status == PrStatus_Ok);
    CHECK(upright.data.vertices.count == 4);
    for (uint32_t i = 0; i < upright.data.vertices.count; ++i)
        CHECK(hasTangent(upright.data.vertices.data[i], 32767, 0, 0, 32767));

    // The second set shows the image mirrored: u runs against x, and the handedness turns over so that the bitangent
    // still points up the image.
    Prepared mirrored(gltf, true, 1);
    REQUIRE(mirrored.status == PrStatus_Ok);
    for (uint32_t i = 0; i < mirrored.data.vertices.count; ++i)
        CHECK(hasTangent(mirrored.data.vertices.data[i], -32767, 0, 0, -32767));

    // Without a normal map there is nothing to compute tangents for, and without the set that the map reads there is
    // nothing to compute them from.
    Prepared unwanted(gltf, false, 0);
    CHECK(hasTangent(unwanted.data.vertices.data[0], 0, 0, 0, 32767));
    Prepared missing(gltf, true, 2);
    REQUIRE(missing.status == PrStatus_Ok);
    CHECK(hasTangent(missing.data.vertices.data[0], 0, 0, 0, 32767));

    // A primitive that has neither normals nor tangents gets both.
    ModelAsset neither;
    Prepared both(neither.read(onePrimitive(R"("attributes":{"POSITION":0,"TEXCOORD_0":2},"indices":1)")), true, 0);
    REQUIRE(both.status == PrStatus_Ok);
    CHECK(both.data.vertices.count == 4);
    CHECK(hasNormal(both.data.vertices.data[0], 0, 0, 32767));
    CHECK(hasTangent(both.data.vertices.data[0], 32767, 0, 0, 32767));
}

TEST_CASE("a skinned vertex takes its four heaviest joints, and a morph target gives it its displacements") {
    // Two sets of joints, each with one weight of 1 for joint 0, and one target that displaces each position by itself
    // and each normal by the normal.
    ModelAsset asset;
    Prepared mesh(asset.read(onePrimitive(
        R"("attributes":{"POSITION":0,"NORMAL":5,"JOINTS_0":13,"WEIGHTS_0":11,"JOINTS_1":13,"WEIGHTS_1":11},)"
        R"("indices":1,"targets":[{"POSITION":10,"NORMAL":5}])")), false, 0, true, 2);
    REQUIRE(mesh.status == PrStatus_Ok);
    REQUIRE(mesh.data.vertices.count == 4);
    REQUIRE(mesh.data.skin.count == 4);
    REQUIRE(mesh.data.morphs.count == 8);
    for (uint32_t i = 0; i < 4; ++i) {
        // The two weights of 1 become two halves, and a weight below zero is no weight.
        const PrRenderSkinVertex& skin = mesh.data.skin.data[i];
        CHECK(skin.joints[0] == 0);
        CHECK(skin.joints[1] == 0);
        CHECK(skin.weights[0] + skin.weights[1] == 65535);
        CHECK(skin.weights[0] >= 32767);
        CHECK(skin.weights[0] <= 32768);
        CHECK(skin.weights[2] == 0);
        CHECK(skin.weights[3] == 0);
        // The mesh has two targets, and the primitive only the first: the second displaces nothing.
        const PrRenderMorphDelta& first = mesh.data.morphs.data[i * 2];
        const PrRenderMorphDelta& second = mesh.data.morphs.data[i * 2 + 1];
        CHECK(first.position[0] == mesh.data.vertices.data[i].geometry.position[0]);
        CHECK(first.position[1] == mesh.data.vertices.data[i].geometry.position[1]);
        CHECK(first.normal[2] == 1);
        CHECK(first.tangent[0] == 0);
        CHECK(second.position[0] == 0);
        CHECK(second.normal[2] == 0);
    }

    // A skinned primitive without normals gets its faces' normals, and its corners are shared again only where they
    // came from one vertex: the quad's two triangles share two of its four.
    ModelAsset flat;
    Prepared skinned(flat.read(onePrimitive(
        R"("attributes":{"POSITION":0,"JOINTS_0":13,"WEIGHTS_0":11},"indices":1)")), false, 0, true, 0);
    REQUIRE(skinned.status == PrStatus_Ok);
    CHECK(skinned.data.vertices.count == 4);
    CHECK(skinned.data.skin.count == 4);

    // With a morph target, no two triangles share a vertex, since each face's normal follows its own triangle. Here the
    // target scales the quad in its plane, which leaves the normals as they are.
    ModelAsset faces;
    Prepared morphed(faces.read(onePrimitive(
        R"("attributes":{"POSITION":0},"indices":1,"targets":[{"POSITION":10}])")), false, 0, false, 1);
    REQUIRE(morphed.status == PrStatus_Ok);
    REQUIRE(morphed.data.vertices.count == 6);
    REQUIRE(morphed.data.morphs.count == 6);
    for (uint32_t i = 0; i < 6; ++i) {
        CHECK(hasNormal(morphed.data.vertices.data[i], 0, 0, 32767));
        CHECK(morphed.data.morphs.data[i].position[0] == morphed.data.vertices.data[i].geometry.position[0]);
        CHECK(morphed.data.morphs.data[i].normal[0] == 0);
        CHECK(morphed.data.morphs.data[i].normal[2] == 0);
    }
}

TEST_CASE("a part is ordered for a GPU's caches, and draws the triangles of its primitive") {
    GridAsset grid;
    grid.read(true);
    Array<uint32_t> expected = grid.triangleKeys();

    // The grid with normals, a skin and a morph target: its vertices are appended as they are.
    Prepared mesh(grid.gltf, false, 0, true, 1);
    REQUIRE(mesh.status == PrStatus_Ok);
    REQUIRE(mesh.data.parts.count == 1);
    CHECK(mesh.data.parts.data[0].firstIndex == 0);
    CHECK(mesh.data.parts.data[0].indexCount == kGridIndices);
    REQUIRE(mesh.data.indices.count == kGridIndices);
    // The vertex that no triangle uses is left out, and every stream follows the vertices.
    REQUIRE(mesh.data.vertices.count == kGridVertices);
    REQUIRE(mesh.data.skin.count == kGridVertices);
    REQUIRE(mesh.data.morphs.count == kGridVertices);
    CHECK(mesh.usesVerticesInOrder(0, kGridIndices, 0, kGridVertices));
    for (uint32_t i = 0; i < kGridVertices; ++i) {
        uint32_t source = gridVertexAt(mesh.data.vertices.data[i]);
        CHECK(mesh.data.skin.data[i].joints[0] == source);
        CHECK(mesh.data.skin.data[i].weights[0] == 65535);
        CHECK(mesh.data.morphs.data[i].position[2] == float(source + 1));
    }
    // The triangles are the document's, each with its corners in their order, in an order that shades fewer vertices
    // than the document's.
    Array<uint32_t> drawn = gridTriangleKeys(mesh.data);
    CHECK(drawn != expected);
    sortKeys(&drawn);
    Array<uint32_t> sorted = expected;
    sortKeys(&sorted);
    CHECK(drawn == sorted);
    float before = 0;
    float after = 0;
    PrMeshIndexView documentOrder{grid.indices.begin(), kGridIndices, PrMeshIndexType_U16};
    PrMeshIndexView preparedOrder{mesh.data.indices.data, kGridIndices, PrMeshIndexType_U32};
    REQUIRE(prMeshCacheMissRatio(documentOrder, kGridVertices + 1, 16, &before) == PrStatus_Ok);
    REQUIRE(prMeshCacheMissRatio(preparedOrder, kGridVertices, 16, &after) == PrStatus_Ok);
    CHECK(before > 2);
    CHECK(after < 1);

    // A part whose triangles keep their order draws them in the document's, from vertices in the order of their use.
    Prepared kept(grid.gltf, false, 0, true, 1, true);
    REQUIRE(kept.status == PrStatus_Ok);
    REQUIRE(kept.data.vertices.count == kGridVertices);
    CHECK(gridTriangleKeys(kept.data) == expected);
    CHECK(kept.usesVerticesInOrder(0, kGridIndices, 0, kGridVertices));
    for (uint32_t i = 0; i < kGridVertices; ++i) {
        uint32_t source = gridVertexAt(kept.data.vertices.data[i]);
        CHECK(kept.data.skin.data[i].joints[0] == source);
        CHECK(kept.data.morphs.data[i].position[2] == float(source + 1));
    }

    // The grid without normals: its corners get the normals of their faces and are welded where they came from one
    // vertex, and the welded vertices are ordered as the others are.
    GridAsset flat;
    flat.read(false);
    Prepared welded(flat.gltf, false, 0, true, 0);
    REQUIRE(welded.status == PrStatus_Ok);
    REQUIRE(welded.data.vertices.count == kGridVertices);
    REQUIRE(welded.data.skin.count == kGridVertices);
    CHECK(welded.usesVerticesInOrder(0, kGridIndices, 0, kGridVertices));
    for (uint32_t i = 0; i < kGridVertices; ++i) {
        CHECK(hasNormal(welded.data.vertices.data[i], 0, 0, 32767));
        CHECK(welded.data.skin.data[i].joints[0] == gridVertexAt(welded.data.vertices.data[i]));
    }
    drawn = gridTriangleKeys(welded.data);
    sortKeys(&drawn);
    CHECK(drawn == sorted);

    // A second part's vertices and indices follow the first's, and are ordered among themselves.
    GridAsset twice;
    twice.read(true, 2);
    Prepared both(twice.gltf, false, 0, true, 1);
    REQUIRE(both.status == PrStatus_Ok);
    REQUIRE(both.data.parts.count == 2);
    REQUIRE(both.data.vertices.count == 2 * kGridVertices);
    REQUIRE(both.data.skin.count == 2 * kGridVertices);
    REQUIRE(both.data.morphs.count == 2 * kGridVertices);
    CHECK(both.data.parts.data[1].firstIndex == kGridIndices);
    CHECK(both.usesVerticesInOrder(0, kGridIndices, 0, kGridVertices));
    CHECK(both.usesVerticesInOrder(kGridIndices, kGridIndices, kGridVertices, kGridVertices));
    for (uint32_t i = 0; i < 2 * kGridVertices; ++i) {
        const ModelVertex& first = mesh.data.vertices.data[i % kGridVertices];
        CHECK(prMemEquals(&both.data.vertices.data[i], &first, sizeof(ModelVertex)));
        CHECK(both.data.skin.data[i].joints[0] == gridVertexAt(both.data.vertices.data[i]));
        CHECK(both.data.morphs.data[i].position[2] == mesh.data.morphs.data[i % kGridVertices].position[2]);
    }
    for (uint32_t k = 0; k < kGridIndices; ++k)
        CHECK(both.data.indices.data[kGridIndices + k] == mesh.data.indices.data[k] + kGridVertices);
}

TEST_CASE("a primitive whose buffer has no bytes fails as Io, and leaves the mesh as it was") {
    ModelAsset asset;
    asset.hasBin = false;
    Prepared mesh(asset.read(onePrimitive(R"("attributes":{"POSITION":0,"NORMAL":5},"indices":1)")), false);
    CHECK(mesh.status == PrStatus_Io);
    CHECK(mesh.data.vertices.count == 0);
    CHECK(mesh.data.indices.count == 0);
    CHECK(mesh.data.parts.count == 0);
}

TEST_SUITE_END();
