// Mesh tangents: the tangent space that a normal map is sampled in.
//
// The algorithm is MikkTSpace's, in five steps. A corner is one index of the triangle list, and corner 3t + i is vertex
// i of triangle t.
// 1. Corners are welded by exact equality of position, normal and texture coordinate, so the result does not depend
//    on which vertices the caller's indices share.
// 2. Each triangle gets the directions in which its two texture coordinates increase, and whether its texture mapping
//    preserves orientation. A triangle with two equal positions is set aside until step 5.
// 3. Triangles are paired across the edges they share.
// 4. The corners at a welded vertex are divided into groups. A group is the corners reached through shared edges at
//    the vertex, between triangles of one orientation, from a corner of a triangle whose texture mapping has area.
// 5. Each corner's tangent is the angle-weighted sum of the tangents of its group's triangles, projected onto the
//    plane of the normal. A corner that no group reached gets a replacement, and a triangle that was set aside copies
//    the tangents of a neighbor.

#include "pr/mesh_tangents.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"
#include "pr/raw_array.h"
#include "pr/sort.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

constexpr uint32_t kNone = 0xffffffffu;

// The smallest magnitude that counts as different from zero: the smallest normal float.
constexpr float kTiny = 1.17549435e-38f;

// What corners are welded by. Every byte is a float, so the bytes can be hashed and compared.
struct WeldKey {
    float values[8];                // position, normal, texture coordinate
};

// What the algorithm knows about a triangle.
struct Triangle {
    // Unit vectors in the directions in which the first and the second texture coordinate increase, or zero if the
    // triangle's texture mapping has no area.
    PrVec3 os;
    PrVec3 ot;

    uint32_t neighbors[3];          // by edge: the triangle across the edge from vertex i to the next, or kNone
    uint32_t groups[3];             // by vertex: the corner's group, or kNone
    bool isDegenerate;              // two of its positions are equal
    bool joinsAnyGroup;             // its texture mapping has no area, so it takes the orientation of its neighbors
    bool preservesOrientation;      // its texture mapping is not mirrored
};

// One edge of one triangle, by the welded vertices at its ends in increasing order.
struct Edge {
    uint32_t low;
    uint32_t high;
    uint32_t triangle;
    uint8_t edge;                   // the edge runs from this vertex of the triangle to the next
    bool isForward;                 // the triangle winds from `low` to `high`
};

// The corners of one group, as a range of the list of grouped corners.
struct Group {
    uint32_t first;
    uint32_t count;
    bool preservesOrientation;
};

// The tangent directions of one corner of a group, projected onto the plane of the normal.
struct Projected {
    PrVec3 os;
    PrVec3 ot;
    bool joinsAnyGroup;
};

// The input, and the working state that the steps share.
struct Tangents {
    PrMeshAttributeView positions;
    PrMeshAttributeView normals;
    PrMeshAttributeView texcoords;
    RawArray<uint32_t> indices;     // by corner: the caller's vertex
    RawArray<uint32_t> welded;      // by corner: the welded vertex
    RawArray<Triangle> triangles;
    RawArray<Group> groups;
    RawArray<uint32_t> grouped;     // corners, each group's together in increasing order
};

bool isNotZero(float value) {
    return prF32Abs(value) > kTiny;
}

bool isNotZero(PrVec3 v) {
    return isNotZero(v.x) || isNotZero(v.y) || isNotZero(v.z);
}

// Scales by the reciprocal of the length, as MikkTSpace does. prVec3Normalize divides by the length, which rounds
// otherwise.
PrVec3 normalize(PrVec3 v) {
    return v * (1.0f / prVec3Length(v));
}

// Returns `v` projected onto the plane of `normal` and scaled to unit length, or as it is if the projection is zero.
PrVec3 projectUnit(PrVec3 v, PrVec3 normal) {
    PrVec3 projected = v - normal * prVec3Dot(normal, v);
    return isNotZero(projected) ? normalize(projected) : projected;
}

uint32_t next(uint32_t vertex) {
    return vertex < 2 ? vertex + 1 : 0;
}

uint32_t previous(uint32_t vertex) {
    return vertex > 0 ? vertex - 1 : 2;
}

// Writes to `out` the key of the caller's vertex `vertex`. Returns false if the key holds a NaN, which equals nothing.
bool weldKey(const Tangents& state, uint32_t vertex, WeldKey* out) {
    PrVec3 position = meshReadVec3(state.positions, vertex);
    PrVec3 normal = meshReadVec3(state.normals, vertex);
    PrVec2 texcoord = meshReadVec2(state.texcoords, vertex);
    float values[8] = {position.x, position.y, position.z, normal.x, normal.y, normal.z, texcoord.x, texcoord.y};
    bool comparable = true;
    for (uint32_t i = 0; i < 8; ++i) {
        // Zero and negative zero are equal, so they get the same bytes.
        out->values[i] = values[i] == 0 ? 0.0f : values[i];
        comparable = comparable && !prF32IsNan(values[i]);
    }
    return comparable;
}

// Step 1: gives each corner its welded vertex.
void weldCorners(Tangents* state) {
    uint32_t vertexCount = state->positions.count;
    RawArray<WeldKey> keys;          // by the caller's vertex
    RawArray<uint32_t> weldedVertex; // by the caller's vertex
    keys.resize(vertexCount);
    weldedVertex.resize(vertexCount);
    HashIndex firsts;
    firsts.reserve(vertexCount);
    uint32_t count = 0;
    for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
        WeldKey& key = keys.data[vertex];
        if (!weldKey(*state, vertex, &key)) {
            weldedVertex.data[vertex] = count++;
            continue;
        }
        uint32_t hash = uint32_t(prHashBytes(&key, sizeof(key), 0));
        uint32_t first = firsts.find(hash, [&](uint32_t other) {
            return prMemEquals(&key, &keys.data[other], sizeof(key));
        });
        if (first != kHashMissing) {
            weldedVertex.data[vertex] = weldedVertex.data[first];
            continue;
        }
        firsts.insert(hash, vertex);
        weldedVertex.data[vertex] = count++;
    }
    state->welded.resize(state->indices.count);
    for (uint32_t corner = 0; corner < state->indices.count; ++corner)
        state->welded.data[corner] = weldedVertex.data[state->indices.data[corner]];
    firsts.reset();
    weldedVertex.reset();
    keys.reset();
}

// Step 2: gives each triangle its tangent directions and flags.
void initTriangles(Tangents* state) {
    uint32_t triangleCount = state->indices.count / 3;
    state->triangles.resize(triangleCount);
    for (uint32_t t = 0; t < triangleCount; ++t) {
        const uint32_t* vertices = state->indices.data + t * 3;
        Triangle& triangle = state->triangles.data[t];
        triangle = Triangle{};
        for (uint32_t i = 0; i < 3; ++i) {
            triangle.neighbors[i] = kNone;
            triangle.groups[i] = kNone;
        }
        triangle.joinsAnyGroup = true;

        PrVec3 p0 = meshReadVec3(state->positions, vertices[0]);
        PrVec3 p1 = meshReadVec3(state->positions, vertices[1]);
        PrVec3 p2 = meshReadVec3(state->positions, vertices[2]);
        triangle.isDegenerate = p0 == p1 || p0 == p2 || p1 == p2;
        if (triangle.isDegenerate)
            continue;

        PrVec2 t0 = meshReadVec2(state->texcoords, vertices[0]);
        PrVec2 t1 = meshReadVec2(state->texcoords, vertices[1]);
        PrVec2 t2 = meshReadVec2(state->texcoords, vertices[2]);
        float t21x = t1.x - t0.x;
        float t21y = t1.y - t0.y;
        float t31x = t2.x - t0.x;
        float t31y = t2.y - t0.y;
        PrVec3 d1 = p1 - p0;
        PrVec3 d2 = p2 - p0;
        float signedArea = t21x * t31y - t21y * t31x;
        PrVec3 os = d1 * t31y - d2 * t21y;
        PrVec3 ot = d1 * -t31x + d2 * t21x;
        triangle.preservesOrientation = signedArea > 0;
        if (!isNotZero(signedArea))
            continue;

        float area = prF32Abs(signedArea);
        float lengthOs = prVec3Length(os);
        float lengthOt = prVec3Length(ot);
        float sign = triangle.preservesOrientation ? 1.0f : -1.0f;
        if (isNotZero(lengthOs))
            triangle.os = os * (sign / lengthOs);
        if (isNotZero(lengthOt))
            triangle.ot = ot * (sign / lengthOt);
        if (isNotZero(lengthOs / area) && isNotZero(lengthOt / area))
            triangle.joinsAnyGroup = false;
    }
}

// Step 3: pairs the triangles that share an edge and wind across it in opposite directions. If more than two triangles
// share an edge, they pair in the order of their indices.
void linkNeighbors(Tangents* state) {
    RawArray<Edge> edges;
    edges.reserve(state->indices.count);
    for (uint32_t t = 0; t < state->triangles.count; ++t) {
        if (state->triangles.data[t].isDegenerate)
            continue;
        for (uint32_t i = 0; i < 3; ++i) {
            uint32_t from = state->welded.data[t * 3 + i];
            uint32_t to = state->welded.data[t * 3 + next(i)];
            edges.push(Edge{
                .low = from < to ? from : to,
                .high = from < to ? to : from,
                .triangle = t,
                .edge = uint8_t(i),
                .isForward = from < to,
            });
        }
    }
    sortItems(edges.span(), [](const Edge& a, const Edge& b) {
        if (a.low != b.low)
            return a.low < b.low;
        if (a.high != b.high)
            return a.high < b.high;
        if (a.triangle != b.triangle)
            return a.triangle < b.triangle;
        return a.edge < b.edge;
    });

    for (uint32_t start = 0; start < edges.count;) {
        uint32_t end = start + 1;
        while (end < edges.count && edges.data[end].low == edges.data[start].low &&
                edges.data[end].high == edges.data[start].high)
            ++end;
        // Within the edges between one pair of vertices, the nth forward edge pairs with the nth backward edge.
        uint32_t forward = start;
        uint32_t backward = start;
        for (;;) {
            while (forward < end && !edges.data[forward].isForward)
                ++forward;
            while (backward < end && edges.data[backward].isForward)
                ++backward;
            if (forward == end || backward == end)
                break;
            const Edge& a = edges.data[forward++];
            const Edge& b = edges.data[backward++];
            state->triangles.data[a.triangle].neighbors[a.edge] = b.triangle;
            state->triangles.data[b.triangle].neighbors[b.edge] = a.triangle;
        }
        start = end;
    }
    edges.reset();
}

// Step 4: divides the corners into groups. A group starts at a corner of a triangle with texture area, and the search
// from it is depth first, taking the triangle across the edge that leaves the vertex before the one across the edge
// that arrives. That order decides which group a triangle without texture area joins, and the first group to reach such
// a triangle gives it its orientation. A corner of such a triangle that no group reaches stays ungrouped.
void buildGroups(Tangents* state) {
    RawArray<uint32_t> stack;       // triangles to try for the current group
    state->grouped.reserve(state->indices.count);
    for (uint32_t t = 0; t < state->triangles.count; ++t) {
        if (state->triangles.data[t].isDegenerate || state->triangles.data[t].joinsAnyGroup)
            continue;
        for (uint32_t i = 0; i < 3; ++i) {
            if (state->triangles.data[t].groups[i] != kNone)
                continue;
            uint32_t vertex = state->welded.data[t * 3 + i];
            uint32_t groupIndex = state->groups.count;
            Group group{
                .first = state->grouped.count,
                .preservesOrientation = state->triangles.data[t].preservesOrientation,
            };
            state->triangles.data[t].groups[i] = groupIndex;
            state->grouped.push(t * 3 + i);
            stack.push(state->triangles.data[t].neighbors[previous(i)]);
            stack.push(state->triangles.data[t].neighbors[i]);
            while (stack.count != 0) {
                uint32_t candidate = stack.last();
                stack.pop();
                if (candidate == kNone)
                    continue;
                Triangle& triangle = state->triangles.data[candidate];
                uint32_t at = 0;
                while (at < 3 && state->welded.data[candidate * 3 + at] != vertex)
                    ++at;
                if (at == 3 || triangle.groups[at] != kNone)
                    continue;
                bool isUngrouped = triangle.groups[0] == kNone && triangle.groups[1] == kNone && triangle.groups[2] == kNone;
                if (triangle.joinsAnyGroup && isUngrouped)
                    triangle.preservesOrientation = group.preservesOrientation;
                if (triangle.preservesOrientation != group.preservesOrientation)
                    continue;
                triangle.groups[at] = groupIndex;
                state->grouped.push(candidate * 3 + at);
                stack.push(triangle.neighbors[previous(at)]);
                stack.push(triangle.neighbors[at]);
            }
            group.count = state->grouped.count - group.first;
            sortItems(Span<uint32_t>(state->grouped.data + group.first, group.count), [](uint32_t a, uint32_t b) {
                return a < b;
            });
            state->groups.push(group);
        }
    }
    stack.reset();
}

PrVec3 cornerNormal(const Tangents& state, uint32_t corner) {
    return meshReadVec3(state.normals, state.indices.data[corner]);
}

PrVec3 cornerPosition(const Tangents& state, uint32_t corner) {
    return meshReadVec3(state.positions, state.indices.data[corner]);
}

// Returns the tangent of the corners `members` of one group: the sum of their triangles' tangents in the plane of the
// normal, each weighted by the triangle's angle at the corner in that plane. Returns zero if no member has a tangent.
PrVec3 groupTangent(const Tangents& state, Span<const uint32_t> members) {
    PrVec3 sum{};
    for (uint32_t corner : members) {
        const Triangle& triangle = state.triangles.data[corner / 3];
        if (triangle.joinsAnyGroup)
            continue;
        uint32_t base = corner - corner % 3;
        PrVec3 normal = cornerNormal(state, corner);
        PrVec3 os = projectUnit(triangle.os, normal);
        PrVec3 at = cornerPosition(state, corner);
        PrVec3 toPrevious = projectUnit(cornerPosition(state, base + previous(corner % 3)) - at, normal);
        PrVec3 toNext = projectUnit(cornerPosition(state, base + next(corner % 3)) - at, normal);
        float cosine = prVec3Dot(toPrevious, toNext);
        if (cosine > 1)
            cosine = 1;
        if (cosine < -1)
            cosine = -1;
        float angle = float(meshAcos(double(cosine)));
        sum = sum + os * angle;
    }
    return isNotZero(sum) ? normalize(sum) : sum;
}

// Writes a corner's tangent and handedness. A tangent that is not a unit vector is replaced by a unit vector
// perpendicular to the corner's normal, with a handedness of 1.
void writeTangent(const Tangents& state, PrMeshAttribute tangents, uint32_t corner, PrVec3 tangent, bool isPositive) {
    float lengthSquared = prVec3Dot(tangent, tangent);
    float values[4] = {tangent.x, tangent.y, tangent.z, isPositive ? 1.0f : -1.0f};
    if (!(lengthSquared > 0.5f && lengthSquared < 2.0f)) {
        PrVec3 fallback = meshPerpendicular(cornerNormal(state, corner));
        values[0] = fallback.x;
        values[1] = fallback.y;
        values[2] = fallback.z;
        values[3] = 1.0f;
    }
    // Negative zero is written as zero, so that equal tangents have equal bytes and weld.
    for (uint32_t i = 0; i < 3; ++i)
        values[i] = values[i] == 0 ? 0.0f : values[i];
    meshWriteFloats(tangents, corner, values, 4);
}

// The cosine at or below which two tangent directions count as opposite: that of half a turn.
constexpr float kOppositeCosine = -1.0f;

// Returns true if no two corners of a group can have opposite tangent directions, found without comparing every pair.
// It holds if every corner's directions are within a cone around the first corner's that is narrower than a quarter
// turn: two directions in the cone are then less than half a turn apart. A false result says nothing.
bool allAgree(Span<const Projected> projected) {
    constexpr float kConeCosine = 0.1f;
    const Projected* reference = nullptr;
    for (const Projected& corner : projected) {
        if (corner.joinsAnyGroup)
            continue;
        if (reference == nullptr)
            reference = &corner;
        if (!(prVec3Dot(corner.os, reference->os) > kConeCosine && prVec3Dot(corner.ot, reference->ot) > kConeCosine))
            return false;
    }
    return true;
}

// Step 5 for one group. Two corners of a group share a tangent unless their triangles' tangent directions point exactly
// opposite ways in the plane of the normal, so each corner sums over the corners that agree with it.
void writeGroup(const Tangents& state, const Group& group, PrMeshAttribute tangents, RawArray<Projected>* projected,
        RawArray<uint32_t>* members) {
    Span<const uint32_t> corners(state.grouped.data + group.first, group.count);
    projected->resize(group.count);
    for (uint32_t i = 0; i < group.count; ++i) {
        const Triangle& triangle = state.triangles.data[corners[i] / 3];
        PrVec3 normal = cornerNormal(state, corners[i]);
        projected->data[i] = Projected{
            .os = projectUnit(triangle.os, normal),
            .ot = projectUnit(triangle.ot, normal),
            .joinsAnyGroup = triangle.joinsAnyGroup,
        };
    }

    // Most groups agree as a whole, and then every corner takes the tangent of all the corners together.
    if (allAgree(projected->span())) {
        PrVec3 whole = groupTangent(state, corners);
        for (uint32_t corner : corners)
            writeTangent(state, tangents, corner, whole, group.preservesOrientation);
        return;
    }
    for (uint32_t i = 0; i < group.count; ++i) {
        const Projected& a = projected->data[i];
        members->clear();
        for (uint32_t j = 0; j < group.count; ++j) {
            const Projected& b = projected->data[j];
            bool agrees = prVec3Dot(a.os, b.os) > kOppositeCosine && prVec3Dot(a.ot, b.ot) > kOppositeCosine;
            if (i == j || a.joinsAnyGroup || b.joinsAnyGroup || agrees)
                members->push(corners[j]);
        }
        PrVec3 tangent = groupTangent(state, members->span());
        writeTangent(state, tangents, corners[i], tangent, group.preservesOrientation);
    }
}

// Step 5 for the corners that no group reached, of triangles without texture area: they get the replacement tangent.
void writeUngrouped(const Tangents& state, PrMeshAttribute tangents) {
    for (uint32_t corner = 0; corner < state.indices.count; ++corner) {
        const Triangle& triangle = state.triangles.data[corner / 3];
        if (!triangle.isDegenerate && triangle.groups[corner % 3] == kNone)
            writeTangent(state, tangents, corner, PrVec3{}, true);
    }
}

// Step 5 for the triangles that were set aside: each corner copies the tangent of the first corner of a triangle that
// was not set aside at the same welded vertex. A corner without one gets the replacement tangent.
void writeDegenerate(const Tangents& state, PrMeshAttribute tangents) {
    RawArray<uint32_t> firstCorner; // by welded vertex: its first corner that was not set aside, or kNone
    firstCorner.resize(state.positions.count);
    for (uint32_t i = 0; i < firstCorner.count; ++i)
        firstCorner.data[i] = kNone;
    for (uint32_t corner = state.indices.count; corner-- > 0;) {
        if (!state.triangles.data[corner / 3].isDegenerate)
            firstCorner.data[state.welded.data[corner]] = corner;
    }
    for (uint32_t corner = 0; corner < state.indices.count; ++corner) {
        if (!state.triangles.data[corner / 3].isDegenerate)
            continue;
        uint32_t source = firstCorner.data[state.welded.data[corner]];
        if (source == kNone) {
            writeTangent(state, tangents, corner, PrVec3{}, true);
            continue;
        }
        float values[4];
        prMemCopy(values, static_cast<const uint8_t*>(tangents.data) + size_t(source) * tangents.stride,
            sizeof(values));
        meshWriteFloats(tangents, corner, values, 4);
    }
    firstCorner.reset();
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prMeshTangents(PrMeshAttributeView positions, PrMeshAttributeView normals, PrMeshAttributeView texcoords,
        PrMeshIndexView indices, PrMeshAttribute tangents) {
    if (!meshAttributeIsReadable(positions, 3) || !meshAttributeIsReadable(normals, 3) ||
            !meshAttributeIsReadable(texcoords, 2) || !meshAttributeIsReadable(meshAttributeView(tangents), 4) ||
            normals.count != positions.count || texcoords.count != positions.count)
        return PrStatus_InvalidData;
    PR_ASSERT(tangents.count >= indices.count);

    Tangents state{.positions = positions, .normals = normals, .texcoords = texcoords};
    PrStatus status = meshTrianglesLoad(indices, positions.count, &state.indices);
    if (status == PrStatus_Ok) {
        weldCorners(&state);
        initTriangles(&state);
        linkNeighbors(&state);
        buildGroups(&state);
        RawArray<Projected> projected;
        RawArray<uint32_t> members;
        for (uint32_t i = 0; i < state.groups.count; ++i)
            writeGroup(state, state.groups.data[i], tangents, &projected, &members);
        writeUngrouped(state, tangents);
        writeDegenerate(state, tangents);
        members.reset();
        projected.reset();
    }
    state.grouped.reset();
    state.groups.reset();
    state.triangles.reset();
    state.welded.reset();
    state.indices.reset();
    return status;
}
