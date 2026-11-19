// Mesh cache order: triangles and vertices ordered for the caches of a GPU.

#include "pr/mesh_cache.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/raw_array.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

constexpr uint32_t kNone = 0xffffffffu;

// The size of the cache that the order is scored against. The three vertices of the newest triangle sit in front of it,
// so the modeled list holds three more.
constexpr uint32_t kModelCacheSize = 32;
constexpr uint32_t kModelListSize = kModelCacheSize + 3;
constexpr uint8_t kNotCached = 0xff;

// The score of a vertex of the newest triangle. It is lower than that of the next positions, so that the order does not
// turn back on itself along a strip's last edge.
constexpr float kLastTriangleScore = 0.75f;

// The scale of the score that a vertex gets for having few triangles left. It makes the order finish off the triangles
// around a vertex instead of leaving single triangles behind.
constexpr float kValenceBoostScale = 2.0f;

// The most triangles of one vertex that a step considers. An ordinary vertex has fewer. The bound keeps the time linear
// for a vertex that thousands of triangles share.
constexpr uint32_t kMaxCandidates = 32;

// The state of the search for an order.
struct CacheOrder {
    RawArray<uint32_t> indices;     // the caller's triangle list
    RawArray<uint32_t> remaining;   // by vertex: the number of its triangles not yet in the order
    RawArray<float> scores;         // by vertex
    RawArray<uint8_t> positions;    // by vertex: its position in the modeled list, or kNotCached
    RawArray<bool> isOrdered;       // by triangle

    // By vertex: the range of `vertexTriangles` that holds the vertex's triangles. A triangle leaves the range some
    // time after it joins the order, when a search next reads the range.
    RawArray<uint32_t> listStarts;
    RawArray<uint32_t> listCounts;
    RawArray<uint32_t> vertexTriangles;

    uint32_t list[kModelListSize];  // the modeled list of recently used vertices, newest first
    uint32_t listCount;
};

// Returns the score of a vertex at `position` of the modeled list with `remaining` triangles left.
float scoreOf(uint8_t position, uint32_t remaining) {
    if (remaining == 0)
        return 0;
    float score = kValenceBoostScale / prF32Sqrt(float(remaining));
    if (position < 3)
        return score + kLastTriangleScore;
    if (position < kModelCacheSize) {
        float nearness = 1.0f - float(position - 3) / float(kModelCacheSize - 3);
        return score + nearness * prF32Sqrt(nearness);
    }
    return score;
}

// Returns true if vertex `corner` of the triangle at `vertices` is the first of that vertex in the triangle.
bool isFirstUse(const uint32_t* vertices, uint32_t corner) {
    for (uint32_t earlier = 0; earlier < corner; ++earlier) {
        if (vertices[earlier] == vertices[corner])
            return false;
    }
    return true;
}

void initOrder(CacheOrder* order, uint32_t vertexCount) {
    uint32_t triangleCount = order->indices.count / 3;
    order->remaining.resizeZeroed(vertexCount);
    order->listStarts.resize(vertexCount);
    order->listCounts.resizeZeroed(vertexCount);
    order->scores.resize(vertexCount);
    order->positions.resize(vertexCount);
    order->isOrdered.resizeZeroed(triangleCount);
    order->vertexTriangles.resize(order->indices.count);
    order->listCount = 0;

    for (uint32_t t = 0; t < triangleCount; ++t) {
        const uint32_t* vertices = order->indices.data + t * 3;
        for (uint32_t corner = 0; corner < 3; ++corner) {
            if (isFirstUse(vertices, corner))
                ++order->remaining.data[vertices[corner]];
        }
    }
    uint32_t start = 0;
    for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
        order->listStarts.data[vertex] = start;
        start += order->remaining.data[vertex];
        order->positions.data[vertex] = kNotCached;
        order->scores.data[vertex] = scoreOf(kNotCached, order->remaining.data[vertex]);
    }
    for (uint32_t t = 0; t < triangleCount; ++t) {
        const uint32_t* vertices = order->indices.data + t * 3;
        for (uint32_t corner = 0; corner < 3; ++corner) {
            if (!isFirstUse(vertices, corner))
                continue;
            uint32_t vertex = vertices[corner];
            order->vertexTriangles.data[order->listStarts.data[vertex] + order->listCounts.data[vertex]++] = t;
        }
    }
}

// Moves the vertices of triangle `t` to the front of the modeled list, and rescores every vertex of the list. The
// vertices that fall off the end leave the cache.
void useTriangle(CacheOrder* order, uint32_t t) {
    const uint32_t* vertices = order->indices.data + t * 3;
    uint32_t updated[kModelListSize + 3];
    uint32_t updatedCount = 0;
    for (uint32_t corner = 0; corner < 3; ++corner) {
        if (!isFirstUse(vertices, corner))
            continue;
        --order->remaining.data[vertices[corner]];
        updated[updatedCount++] = vertices[corner];
    }
    for (uint32_t i = 0; i < order->listCount; ++i) {
        uint32_t vertex = order->list[i];
        if (vertex != vertices[0] && vertex != vertices[1] && vertex != vertices[2])
            updated[updatedCount++] = vertex;
    }
    order->listCount = updatedCount < kModelListSize ? updatedCount : kModelListSize;
    for (uint32_t i = 0; i < updatedCount; ++i) {
        uint32_t vertex = updated[i];
        uint8_t position = i < kModelCacheSize ? uint8_t(i) : kNotCached;
        order->positions.data[vertex] = position;
        order->scores.data[vertex] = scoreOf(position, order->remaining.data[vertex]);
        if (i < kModelListSize)
            order->list[i] = vertex;
    }
}

// Returns the triangle with the highest score among the triangles of the vertices in the modeled list, or kNone if
// those vertices have no triangles left. The first of several triangles with the same score wins.
uint32_t bestTriangle(CacheOrder* order) {
    uint32_t best = kNone;
    float bestScore = -1;
    for (uint32_t i = 0; i < order->listCount; ++i) {
        uint32_t vertex = order->list[i];
        uint32_t* triangles = order->vertexTriangles.data + order->listStarts.data[vertex];
        uint32_t& count = order->listCounts.data[vertex];
        uint32_t considered = 0;
        for (uint32_t at = 0; at < count && considered < kMaxCandidates;) {
            uint32_t t = triangles[at];
            if (order->isOrdered.data[t]) {
                triangles[at] = triangles[--count];
                continue;
            }
            const uint32_t* vertices = order->indices.data + t * 3;
            float score = order->scores.data[vertices[0]] + order->scores.data[vertices[1]] +
                order->scores.data[vertices[2]];
            if (score > bestScore) {
                bestScore = score;
                best = t;
            }
            ++considered;
            ++at;
        }
    }
    return best;
}

void resetOrder(CacheOrder* order) {
    order->indices.reset();
    order->remaining.reset();
    order->scores.reset();
    order->positions.reset();
    order->isOrdered.reset();
    order->listStarts.reset();
    order->listCounts.reset();
    order->vertexTriangles.reset();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prMeshIndicesOptimizeCache(PrMeshIndexView indices, uint32_t vertexCount, PrMeshIndices out) {
    if (meshIndexSize(out.type) == 0)
        return PrStatus_InvalidData;
    CacheOrder order{};
    PrStatus status = meshTrianglesLoad(indices, vertexCount, &order.indices);
    if (status != PrStatus_Ok) {
        order.indices.reset();
        return status;
    }
    PR_ASSERT(out.count >= indices.count && (out.data != nullptr || indices.count == 0));
    initOrder(&order, vertexCount);

    uint32_t triangleCount = order.indices.count / 3;
    uint32_t unordered = 0;         // every triangle before this one is in the order
    uint32_t best = kNone;
    for (uint32_t written = 0; written < triangleCount && status == PrStatus_Ok; ++written) {
        // When the recently used vertices have no triangles left, the order restarts at the first triangle that is not
        // yet in it.
        if (best == kNone) {
            while (order.isOrdered.data[unordered])
                ++unordered;
            best = unordered;
        }
        order.isOrdered.data[best] = true;
        for (uint32_t corner = 0; corner < 3; ++corner) {
            if (!meshWriteIndex(out, written * 3 + corner, order.indices.data[best * 3 + corner]))
                status = PrStatus_OutOfRange;
        }
        useTriangle(&order, best);
        best = bestTriangle(&order);
    }
    resetOrder(&order);
    return status;
}

PrStatus prMeshCacheMissRatio(PrMeshIndexView indices, uint32_t vertexCount, uint32_t cacheSize, float* out) {
    *out = 0;
    if (meshIndexSize(indices.type) == 0 || indices.count % 3 != 0)
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;
    PR_ASSERT(cacheSize >= 3);
    if (indices.count == 0)
        return PrStatus_Ok;

    // A vertex is in the cache from the miss that loads it until `cacheSize` more misses have pushed it out.
    RawArray<uint32_t> loadedAt;    // by vertex: the number of the miss that last loaded it, from 1, or zero
    loadedAt.resizeZeroed(vertexCount);
    uint32_t misses = 0;
    PrStatus status = PrStatus_Ok;
    for (uint32_t i = 0; i < indices.count; ++i) {
        uint32_t index = meshIndex(indices, i);
        if (index >= vertexCount) {
            status = PrStatus_OutOfRange;
            break;
        }
        uint32_t loaded = loadedAt.data[index];
        if (loaded == 0 || misses - loaded >= cacheSize)
            loadedAt.data[index] = ++misses;
    }
    loadedAt.reset();
    if (status == PrStatus_Ok)
        *out = float(double(misses) / double(indices.count / 3));
    return status;
}

PrStatus prMeshFetchMissRatio(PrMeshIndexView indices, uint32_t vertexCount, uint32_t vertexSize, float* out) {
    constexpr uint64_t kLineSize = 64;
    constexpr uint64_t kLineCount = 256;
    *out = 0;
    if (meshIndexSize(indices.type) == 0 || vertexSize == 0)
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;
    uint64_t lineCount = (uint64_t(vertexCount) * vertexSize + kLineSize - 1) / kLineSize;
    if (lineCount > 0xffffffffu)
        return PrStatus_Limit;

    RawArray<uint64_t> loadedAt;    // by line: the number of the load that last loaded it, from 1, or zero
    loadedAt.resizeZeroed(uint32_t(lineCount));
    uint64_t loads = 0;
    uint32_t linesUsed = 0;
    PrStatus status = PrStatus_Ok;
    for (uint32_t i = 0; i < indices.count; ++i) {
        uint32_t index = meshIndex(indices, i);
        if (index >= vertexCount) {
            status = PrStatus_OutOfRange;
            break;
        }
        uint64_t firstLine = uint64_t(index) * vertexSize / kLineSize;
        uint64_t lastLine = (uint64_t(index) * vertexSize + vertexSize - 1) / kLineSize;
        for (uint64_t line = firstLine; line <= lastLine; ++line) {
            uint64_t loaded = loadedAt.data[line];
            if (loaded == 0)
                ++linesUsed;
            if (loaded == 0 || loads - loaded >= kLineCount)
                loadedAt.data[line] = ++loads;
        }
    }
    loadedAt.reset();
    if (status == PrStatus_Ok && linesUsed != 0)
        *out = float(double(loads) / double(linesUsed));
    return status;
}
