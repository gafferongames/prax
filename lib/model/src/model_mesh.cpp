// A model's meshes: the parts of a source's mesh prepared as the vertices, indices and parts of a renderer's mesh, with
// what deforms them.

#include "model/src/model_mesh.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/mem.h"
#include "pr/memory.h"
#include "pr/mesh.h"
#include "pr/mesh_cache.h"
#include "pr/mesh_normals.h"
#include "pr/mesh_tangents.h"
#include "pr/vec.h"

namespace pr_impl {

namespace {

float* allocFloats(uint64_t count) {
    return static_cast<float*>(prMemoryAlloc(size_t(count != 0 ? count : 1) * sizeof(float)));
}

// Returns a value of a source, or `fallback` if it is not finite.
float finiteOr(float value, float fallback) {
    return prF32IsFinite(value) ? value : fallback;
}

// Returns the unit vector along three floats, or `fallback` if they have no direction.
PrVec3 unitOr(const float* v, PrVec3 fallback) {
    float length2 = v[0] * v[0] + v[1] * v[1] + v[2] * v[2];
    if (!prF32IsFinite(length2) || length2 < 1e-20f)
        return fallback;
    float scale = 1 / prF32Sqrt(length2);
    return PrVec3{v[0] * scale, v[1] * scale, v[2] * scale};
}

PrVec3 vectorAt(const float* PR_NULLABLE values, size_t index) {
    if (values == nullptr)
        return PrVec3{0, 0, 0};
    return PrVec3{finiteOr(values[index * 3], 0), finiteOr(values[index * 3 + 1], 0), finiteOr(values[index * 3 + 2], 0)};
}

// One part of a mesh's data as its preparation reads it: the arrays that its vertices have, null for those that they
// lack, and its triangles.
struct PartView {
    const PrModelMeshData* data;
    const float* PR_NULLABLE normals;
    const float* PR_NULLABLE tangents;
    const float* PR_NULLABLE uv;        // the texture coordinates that tangents follow
    uint32_t uvWidth;
    uint32_t streams;                   // the request's streams that the part has, as bits
    const uint32_t* triangles;
    uint32_t indexCount;
};

// How a part's corners are shared again after they were unshared.
enum {
    Sharing_Equal,      // corners whose vertices are equal share one
    Sharing_Source,     // only if they also came from one vertex of the source, which deforms them alike
    Sharing_None,       // no corners share a vertex, so each can follow its own triangle
};
typedef uint8_t Sharing;

// Where each vertex that a part appended came from: the vertex of the source, and, for a part whose triangles share no
// vertex, the place of its corner in the part's triangles.
struct Sources {
    RawArray<uint32_t> vertex;
    RawArray<uint32_t> corner;      // empty unless no two triangles share a vertex
};

// Writes a vertex of `out`'s layout at `at`: its geometry, and the streams of the source's vertex `source` that the
// part has, or zeros for those that it lacks.
void writeVertex(const ModelMeshPrepared& out, const PartView& part, uint32_t source, PrVec3 position, PrVec3 normal,
        PrVec3 tangent, float handedness, uint8_t* at) {
    PrRenderGeometryVertex geometry = prRenderGeometryVertexPack(position, normal, tangent, handedness);
    prMemCopy(at, &geometry, sizeof(geometry));
    float* values = reinterpret_cast<float*>(at + sizeof(PrRenderGeometryVertex));
    const PrModelMeshData& data = *part.data;
    for (uint32_t i = 0; i < out.streamWidths.count; ++i) {
        uint32_t width = out.streamWidths.data[i];
        float* value = values + out.streamOffsets.data[i];
        const float* given = data.streams != nullptr ? data.streams[i] : nullptr;
        bool has = given != nullptr && (part.streams & (1u << i)) != 0;
        for (uint32_t k = 0; k < width; ++k)
            value[k] = has ? finiteOr(given[size_t(source) * width + k], 0) : 0;
    }
}

// Returns the geometry's values of the source's vertex `i`: its position, its normal and its tangent where the part has
// them.
void valuesAt(const PartView& part, uint32_t i, PrVec3* position, PrVec3* normal, PrVec3* tangent, float* handedness) {
    const float* p = part.data->positions + size_t(i) * 3;
    *position = PrVec3{p[0], p[1], p[2]};
    *normal = part.normals != nullptr ? unitOr(part.normals + size_t(i) * 3, PrVec3{0, 0, 1}) : PrVec3{0, 0, 1};
    *tangent = PrVec3{0, 0, 0};
    *handedness = 1;
    if (part.tangents != nullptr) {
        *tangent = unitOr(part.tangents + size_t(i) * 4, PrVec3{0, 0, 0});
        *handedness = part.tangents[size_t(i) * 4 + 3] < 0 ? -1.0f : 1.0f;
    }
}

// Appends the source's vertices that the part's triangles use, in the order of their first use, and the triangles as
// indices of them.
void appendShared(const PartView& part, ModelMeshPrepared* out, Sources* sources) {
    uint32_t base = out->vertexCount;
    uint32_t* map = static_cast<uint32_t*>(prMemoryAlloc(size_t(part.data->vertexCount) * sizeof(uint32_t) + 4));
    prMemFill(map, 0xff, size_t(part.data->vertexCount) * sizeof(uint32_t));
    uint32_t first = out->indices.count;
    out->indices.resize(first + part.indexCount);
    for (uint32_t k = 0; k < part.indexCount; ++k) {
        uint32_t source = part.triangles[k];
        if (map[source] == 0xffffffffu) {
            map[source] = sources->vertex.count;
            sources->vertex.push(source);
        }
        out->indices.data[first + k] = base + map[source];
    }
    prMemoryFree(map);
    out->vertices.resize((base + sources->vertex.count) * out->stride);
    for (uint32_t v = 0; v < sources->vertex.count; ++v) {
        PrVec3 position, normal, tangent;
        float handedness = 1;
        valuesAt(part, sources->vertex.data[v], &position, &normal, &tangent, &handedness);
        writeVertex(*out, part, sources->vertex.data[v], position, normal, tangent, handedness,
            out->vertices.data + size_t(base + v) * out->stride);
    }
    out->vertexCount = base + sources->vertex.count;
}

// Appends the part with a vertex for each corner of each triangle, so that a corner can have the normal of its face and
// the tangent that MikkTSpace gives it, and then shares the corners again as `sharing` says. With `generates`, the
// corners get MikkTSpace's tangents from the part's texture coordinates.
PrStatus appendUnshared(const PartView& part, bool generates, Sharing sharing, ModelMeshPrepared* out, Sources* sources) {
    uint32_t indexCount = part.indexCount;
    const float* points = part.data->positions;
    float* positions = allocFloats(uint64_t(indexCount) * 3);
    float* normals = allocFloats(uint64_t(indexCount) * 3);
    float* texcoords = allocFloats(uint64_t(indexCount) * 2);
    float* tangents = allocFloats(uint64_t(indexCount) * 4);
    uint32_t recordSize = out->stride + uint32_t(sizeof(uint32_t));
    uint8_t* corners = static_cast<uint8_t*>(prMemoryAllocZeroed(size_t(indexCount) * recordSize + 1));
    uint8_t* welded = static_cast<uint8_t*>(prMemoryAlloc(size_t(indexCount) * recordSize + 1));
    uint32_t* remap = static_cast<uint32_t*>(prMemoryAlloc(size_t(indexCount) * sizeof(uint32_t) + 4));
    for (uint32_t k = 0; k < indexCount; ++k) {
        uint32_t i = part.triangles[k];
        prMemCopy(positions + size_t(k) * 3, points + size_t(i) * 3, 3 * sizeof(float));
        if (part.normals != nullptr) {
            PrVec3 normal = unitOr(part.normals + size_t(i) * 3, PrVec3{0, 0, 1});
            prMemCopy(normals + size_t(k) * 3, &normal, 3 * sizeof(float));
        }
        if (generates) {
            texcoords[k * 2] = finiteOr(part.uv[size_t(i) * part.uvWidth], 0);
            texcoords[k * 2 + 1] = finiteOr(part.uv[size_t(i) * part.uvWidth + 1], 0);
        }
    }
    // The corners are in the order of the indices, so they need none.
    PrMeshIndexView sequence{nullptr, indexCount, PrMeshIndexType_U32};
    PrMeshAttributeView positionView{positions, indexCount, 3 * sizeof(float), prMeshFormatFloat32(3)};
    PrMeshAttributeView normalView{normals, indexCount, 3 * sizeof(float), prMeshFormatFloat32(3)};
    PrStatus status = PrStatus_Ok;
    if (part.normals == nullptr)
        status = prMeshFlatNormals(positionView, sequence,
            PrMeshAttribute{normals, indexCount, 3 * sizeof(float), normalView.format});
    if (status == PrStatus_Ok && generates) {
        status = prMeshTangents(positionView, normalView,
            PrMeshAttributeView{texcoords, indexCount, 2 * sizeof(float), prMeshFormatFloat32(2)}, sequence,
            PrMeshAttribute{tangents, indexCount, 4 * sizeof(float), prMeshFormatFloat32(4)});
    }
    for (uint32_t k = 0; status == PrStatus_Ok && k < indexCount; ++k) {
        uint32_t source = part.triangles[k];
        PrVec3 position, normal, tangent;
        float handedness = 1;
        valuesAt(part, source, &position, &normal, &tangent, &handedness);
        normal = PrVec3{normals[k * 3], normals[k * 3 + 1], normals[k * 3 + 2]};
        if (generates) {
            tangent = PrVec3{tangents[k * 4], tangents[k * 4 + 1], tangents[k * 4 + 2]};
            // lib/mesh's bitangent points where v increases, and the source's up the image, where v decreases.
            handedness = -tangents[k * 4 + 3];
        } else if (part.normals == nullptr) {
            // Tangents that came with no normals are ignored.
            tangent = PrVec3{0, 0, 0};
            handedness = 1;
        }
        uint8_t* record = corners + size_t(k) * recordSize;
        writeVertex(*out, part, source, position, normal, tangent, handedness, record);
        uint32_t key = 0;
        if (sharing == Sharing_Source)
            key = source;
        else if (sharing == Sharing_None)
            key = k;
        prMemCopy(record + out->stride, &key, sizeof(key));
    }
    uint32_t weldedCount = 0;
    if (status == PrStatus_Ok)
        status = prMeshWeldRemap(corners, indexCount, recordSize, remap, &weldedCount);
    if (status == PrStatus_Ok)
        status = prMeshVerticesRemap(corners, indexCount, recordSize, remap, welded, weldedCount);
    if (status == PrStatus_Ok) {
        uint32_t base = out->vertexCount;
        out->vertices.resize((base + weldedCount) * out->stride);
        for (uint32_t i = 0; i < weldedCount; ++i)
            prMemCopy(out->vertices.data + size_t(base + i) * out->stride, welded + size_t(i) * recordSize,
                out->stride);
        out->vertexCount = base + weldedCount;
        uint32_t first = out->indices.count;
        out->indices.resize(first + indexCount);
        sources->vertex.resize(weldedCount);
        if (sharing == Sharing_None)
            sources->corner.resize(weldedCount);
        for (uint32_t k = 0; k < indexCount; ++k) {
            out->indices.data[first + k] = base + remap[k];
            sources->vertex.data[remap[k]] = part.triangles[k];
            if (sharing == Sharing_None)
                sources->corner.data[remap[k]] = k;
        }
    }
    prMemoryFree(positions);
    prMemoryFree(normals);
    prMemoryFree(texcoords);
    prMemoryFree(tangents);
    prMemoryFree(corners);
    prMemoryFree(welded);
    prMemoryFree(remap);
    return status;
}

// Grows an array to hold `count` items, with zeroed bytes in the new ones.
template <typename T>
void growZeroed(RawArray<T>* array, uint32_t count) {
    uint32_t old = array->count;
    if (count <= old)
        return;
    array->resize(count);
    prMemZero(array->data + old, size_t(count - old) * sizeof(T));
}

// Gives each of the `count` vertices from `base` the skin of the vertex that it came from: of its influences, the four
// of the greatest weight.
void appendSkin(const PrModelMeshData& data, const Sources& sources, uint32_t base, uint32_t count, ModelMeshPrepared* out) {
    growZeroed(&out->skin, base + count);
    uint32_t influences = data.influenceCount < 16 ? data.influenceCount : 16;
    for (uint32_t v = 0; v < count; ++v) {
        uint32_t source = sources.vertex.data[v];
        uint16_t joints[16] = {};
        float weights[16] = {};
        uint32_t found = 0;
        for (uint32_t k = 0; data.joints != nullptr && data.weights != nullptr && k < influences; ++k) {
            float weight = data.weights[size_t(source) * data.influenceCount + k];
            uint32_t joint = data.joints[size_t(source) * data.influenceCount + k];
            if (!(weight > 0) || !prF32IsFinite(weight) || joint > 0xffffu)
                continue;
            // The influences are kept in the order of their weights, the greatest first.
            uint32_t at = found++;
            for (; at != 0 && weights[at - 1] < weight; --at) {
                weights[at] = weights[at - 1];
                joints[at] = joints[at - 1];
            }
            weights[at] = weight;
            joints[at] = uint16_t(joint);
        }
        out->skin.data[base + v] = prRenderSkinVertexPack(joints, weights);
    }
}

// Returns the unit normal of a triangle, or (0, 0, 1) for a triangle without area.
PrVec3 faceNormal(PrVec3 p0, PrVec3 p1, PrVec3 p2) {
    PrVec3 normal = prVec3Cross(p1 - p0, p2 - p0);
    const float n[3] = {normal.x, normal.y, normal.z};
    return unitOr(n, PrVec3{0, 0, 1});
}

// Gives each of the `count` vertices from `base` what each morph target adds to the vertex that it came from. A part
// whose triangles share no vertex has its faces' normals, which follow the displaced positions.
void appendMorphs(const PartView& part, const Sources& sources, uint32_t base, uint32_t count, uint32_t targetCount,
        ModelMeshPrepared* out) {
    const PrModelMeshData& data = *part.data;
    growZeroed(&out->morphs, (base + count) * targetCount);
    uint32_t given = data.morphTargetCount;
    for (uint32_t t = 0; t < targetCount && t < given; ++t) {
        for (uint32_t v = 0; v < count; ++v) {
            uint32_t source = sources.vertex.data[v];
            size_t at = size_t(source) * given + t;
            PrVec3 position = vectorAt(data.morphPositions, at);
            PrVec3 normal = vectorAt(data.morphNormals, at);
            PrVec3 tangent = vectorAt(data.morphTangents, at);
            if (sources.corner.count != 0 && data.morphPositions != nullptr) {
                const uint32_t* triangle = part.triangles + sources.corner.data[v] / 3 * 3;
                PrVec3 p[3];
                PrVec3 moved[3];
                for (uint32_t c = 0; c < 3; ++c) {
                    p[c] = vectorAt(data.positions, triangle[c]);
                    moved[c] = p[c] + vectorAt(data.morphPositions, size_t(triangle[c]) * given + t);
                }
                normal = faceNormal(moved[0], moved[1], moved[2]) - faceNormal(p[0], p[1], p[2]);
            }
            out->morphs.data[size_t(base + v) * targetCount + t] = PrRenderMorphDelta{
                {position.x, position.y, position.z},
                {normal.x, normal.y, normal.z},
                {tangent.x, tangent.y, tangent.z},
            };
        }
    }
}

// Moves the `count` values of `valueSize` bytes at `values` to the places that `remap` gives them, of which there are
// `usedCount`.
PrStatus reorderValues(void* values, uint32_t count, uint32_t valueSize, const uint32_t* remap, uint32_t usedCount) {
    size_t bytes = size_t(count) * valueSize;
    void* old = prMemoryAlloc(bytes + 1);
    prMemCopy(old, values, bytes);
    PrStatus status = prMeshVerticesRemap(old, count, valueSize, remap, values, usedCount);
    prMemoryFree(old);
    return status;
}

// Orders a part for a GPU's caches: its triangles so that each reuses vertices that were shaded lately, unless it keeps
// their order, and then its vertices in the order in which the triangles first use them. The part is the indices from
// `firstIndex` and the vertices from `firstVertex`, with their skin and their morph deltas, which are the last of
// `out`. A vertex that no triangle uses is dropped. Every triangle keeps its vertices and their order.
PrStatus orderForCaches(ModelMeshPrepared* out, uint32_t firstVertex, uint32_t firstIndex, bool keepsOrder,
        bool skinned, uint32_t targets) {
    uint32_t count = out->vertexCount - firstVertex;
    uint32_t indexCount = out->indices.count - firstIndex;
    uint32_t* indices = out->indices.data + firstIndex;
    uint32_t* remap = static_cast<uint32_t*>(prMemoryAlloc(size_t(count) * sizeof(uint32_t) + 4));
    // The functions of lib/mesh number a part's vertices from zero.
    for (uint32_t k = 0; k < indexCount; ++k)
        indices[k] -= firstVertex;
    PrMeshIndices part{indices, indexCount, PrMeshIndexType_U32};
    PrMeshIndexView view{indices, indexCount, PrMeshIndexType_U32};
    PrStatus status = PrStatus_Ok;
    if (!keepsOrder)
        status = prMeshIndicesOptimizeCache(view, count, part);
    uint32_t used = 0;
    if (status == PrStatus_Ok)
        status = prMeshFetchRemap(view, count, remap, &used);
    if (status == PrStatus_Ok)
        status = prMeshIndicesRemap(view, remap, count, part);
    if (status == PrStatus_Ok)
        status = reorderValues(out->vertices.data + size_t(firstVertex) * out->stride, count, out->stride, remap, used);
    if (status == PrStatus_Ok && skinned)
        status = reorderValues(out->skin.data + firstVertex, count, sizeof(PrRenderSkinVertex), remap, used);
    if (status == PrStatus_Ok && targets != 0) {
        status = reorderValues(out->morphs.data + size_t(firstVertex) * targets, count,
            targets * uint32_t(sizeof(PrRenderMorphDelta)), remap, used);
    }
    if (status == PrStatus_Ok) {
        out->vertexCount = firstVertex + used;
        out->vertices.resize(out->vertexCount * out->stride);
        if (skinned)
            out->skin.resize(firstVertex + used);
        if (targets != 0)
            out->morphs.resize((firstVertex + used) * targets);
    }
    for (uint32_t k = 0; k < indexCount; ++k)
        indices[k] += firstVertex;
    prMemoryFree(remap);
    return status;
}

// Prepares one part of the data and appends it to `out`, or leaves `out` as it was if it fails.
PrStatus appendPart(const PrModelMeshData& data, uint32_t index, const ModelMeshOptions& options, ModelMeshPrepared* out) {
    const PrModelMeshDataPart& given = data.parts[index];
    const ModelPartOptions& partOptions = options.parts[index];
    PartView part{
        .data = &data,
        .normals = (given.flags & PrModelMeshPartFlag_Normals) != 0 ? data.normals : nullptr,
        .tangents = (given.flags & PrModelMeshPartFlag_Tangents) != 0 ? data.tangents : nullptr,
        .streams = given.streams,
        .triangles = data.triangles + size_t(given.firstTriangle) * 3,
        .indexCount = given.triangleCount * 3,
    };
    if (part.normals == nullptr)
        part.tangents = nullptr;
    uint32_t t = partOptions.tangentStream;
    if (t != kModelStreamNone && t < out->streamWidths.count && out->streamWidths.data[t] >= 2 &&
            data.streams != nullptr && data.streams[t] != nullptr && (given.streams & (1u << t)) != 0) {
        part.uv = data.streams[t];
        part.uvWidth = out->streamWidths.data[t];
    }
    uint64_t most = uint64_t(out->vertexCount) + part.indexCount;
    uint64_t morphs = most * (options.morphTargetCount != 0 ? options.morphTargetCount : 1);
    if (morphs > 0xffffffffu || uint64_t(out->indices.count) + part.indexCount > 0xffffffffu)
        return PrStatus_Limit;

    uint32_t firstVertex = out->vertexCount;
    uint32_t firstIndex = out->indices.count;
    uint32_t firstSkin = out->skin.count;
    uint32_t firstMorph = out->morphs.count;
    bool corners = (given.flags & PrModelMeshPartFlag_Corners) != 0;
    bool morphed = options.morphTargetCount != 0 && data.morphPositions != nullptr;
    Sharing sharing = Sharing_Equal;
    if (!corners && part.normals == nullptr && morphed)
        sharing = Sharing_None;
    else if (!corners && (options.skinned || morphed))
        sharing = Sharing_Source;
    bool lacksTangents = part.tangents == nullptr || part.normals == nullptr;
    bool generates = partOptions.wantsTangents && lacksTangents && part.uv != nullptr;
    Sources sources{};
    PrStatus status = PrStatus_Ok;
    if (part.normals == nullptr || generates || corners)
        status = appendUnshared(part, generates, sharing, out, &sources);
    else
        appendShared(part, out, &sources);
    if (status == PrStatus_Ok) {
        uint32_t count = out->vertexCount - firstVertex;
        if (options.skinned)
            appendSkin(data, sources, firstVertex, count, out);
        if (options.morphTargetCount != 0)
            appendMorphs(part, sources, firstVertex, count, options.morphTargetCount, out);
    }
    // The sphere holds every vertex of the part, as the bounds that a source states do: also a vertex that no triangle
    // uses, which the ordering leaves out.
    PrSphere sphere{};
    if (status == PrStatus_Ok && out->vertexCount > firstVertex) {
        PrMeshAttributeView positions{
            out->vertices.data + size_t(firstVertex) * out->stride,
            out->vertexCount - firstVertex,
            out->stride,
            prMeshFormatFloat32(3),
        };
        prMeshSphere(positions, &sphere);
        status = orderForCaches(out, firstVertex, firstIndex, partOptions.keepsTriangleOrder, options.skinned,
            options.morphTargetCount);
    }
    sources.vertex.reset();
    sources.corner.reset();
    if (status != PrStatus_Ok) {
        out->vertexCount = firstVertex;
        out->vertices.resize(firstVertex * out->stride);
        out->indices.resize(firstIndex);
        out->skin.resize(firstSkin);
        out->morphs.resize(firstMorph);
        return status;
    }
    out->parts.push(PrRenderMeshPart{
        .firstIndex = firstIndex,
        .indexCount = part.indexCount,
        // A radius of zero would ask the renderer to compute the sphere, and a part of one point has that radius.
        .bounds = {sphere.center, sphere.radius > 0 ? sphere.radius : 1e-6f},
    });
    out->partFirstVertex.push(firstVertex);
    out->partVertexCount.push(out->vertexCount - firstVertex);
    out->partStreams.push(given.streams);
    return PrStatus_Ok;
}

} // namespace

PrStatus modelMeshPrepare(const PrModelMeshData& data, const ModelMeshOptions& options, ModelMeshPrepared* out) {
    if (data.positions == nullptr || data.triangles == nullptr)
        return data.partCount == 0 ? PrStatus_Ok : PrStatus_InvalidData;
    for (uint32_t i = 0; i < data.triangleCount * 3; ++i) {
        if (data.triangles[i] >= data.vertexCount)
            return PrStatus_InvalidData;
    }
    PR_ASSERT(out->stride != 0);
    for (uint32_t k = 0; k < data.partCount; ++k) {
        PrStatus status = appendPart(data, k, options, out);
        if (status != PrStatus_Ok)
            return status;
    }
    return PrStatus_Ok;
}

void modelMeshPreparedInit(ModelMeshPrepared* mesh, const uint32_t* PR_NULLABLE streamWidths, uint32_t streamCount) {
    uint32_t floats = 0;
    for (uint32_t i = 0; i < streamCount; ++i) {
        mesh->streamOffsets.push(floats);
        mesh->streamWidths.push(streamWidths[i]);
        floats += streamWidths[i];
    }
    mesh->stride = uint32_t(sizeof(PrRenderGeometryVertex)) + floats * uint32_t(sizeof(float));
}

void modelMeshPreparedReset(ModelMeshPrepared* mesh) {
    mesh->vertices.reset();
    mesh->streamOffsets.reset();
    mesh->streamWidths.reset();
    mesh->indices.reset();
    mesh->parts.reset();
    mesh->partFirstVertex.reset();
    mesh->partVertexCount.reset();
    mesh->partStreams.reset();
    mesh->skin.reset();
    mesh->morphs.reset();
}

} // namespace pr_impl
