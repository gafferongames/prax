// Mesh normals: normals computed from the positions of a triangle list.

#include "pr/mesh_normals.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/raw_array.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

// Normals are summed in doubles, in which the products of float coordinates neither overflow nor lose the small edges
// of a thin triangle.
PrDVec3 difference(PrVec3 a, PrVec3 b) {
    return prDVec3FromVec3(a) - prDVec3FromVec3(b);
}

// A triangle's unit normal and the angles at its three vertices.
struct Face {
    PrDVec3 normal;
    double angles[3];
};

// Returns the angle between the edges `a` and `b`, which have length.
double angleBetween(PrDVec3 a, PrDVec3 b) {
    double cosine = prDVec3Dot(a, b) / prF64Sqrt(prDVec3Dot(a, a) * prDVec3Dot(b, b));
    if (cosine > 1)
        cosine = 1;
    if (cosine < -1)
        cosine = -1;
    return meshAcos(cosine);
}

// Writes the face of the triangle with corners `p` to `out`. Returns false for a triangle without area or with a
// coordinate that is not finite.
bool faceOf(const PrVec3 p[PR_NONNULL 3], Face* out) {
    if (!meshIsFinite(p[0]) || !meshIsFinite(p[1]) || !meshIsFinite(p[2]))
        return false;
    PrDVec3 edges[3] = {difference(p[1], p[0]), difference(p[2], p[1]), difference(p[0], p[2])};
    PrDVec3 normal = prDVec3Cross(edges[0], -edges[2]);
    double lengthSquared = prDVec3Dot(normal, normal);
    if (!(lengthSquared > 0))
        return false;
    double scale = 1.0 / prF64Sqrt(lengthSquared);
    out->normal = normal * scale;
    for (uint32_t corner = 0; corner < 3; ++corner) {
        PrDVec3 leaving = edges[corner];
        PrDVec3 arriving = edges[(corner + 2) % 3];
        out->angles[corner] = angleBetween(leaving, -arriving);
    }
    return true;
}

// Writes `normal` scaled to unit length as value `index` of `normals`, or (0, 0, 1) if it has no direction.
void writeUnit(PrMeshAttribute normals, uint32_t index, PrDVec3 normal) {
    float unit[3] = {0, 0, 1};
    double lengthSquared = prDVec3Dot(normal, normal);
    if (lengthSquared > 0) {
        double scale = 1.0 / prF64Sqrt(lengthSquared);
        double scaled[3] = {normal.x * scale, normal.y * scale, normal.z * scale};
        // Negative zero is written as zero, so that equal normals have equal bytes and weld.
        for (uint32_t i = 0; i < 3; ++i)
            unit[i] = scaled[i] == 0 ? 0.0f : float(scaled[i]);
    }
    meshWriteFloats(normals, index, unit, 3);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrStatus prMeshNormals(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshAttribute normals) {
    if (!meshAttributeIsReadable(positions, 3) || !meshAttributeIsReadable(meshAttributeView(normals), 3))
        return PrStatus_InvalidData;
    PR_ASSERT(normals.count >= positions.count);
    RawArray<uint32_t> triangles;
    PrStatus status = meshTrianglesLoad(indices, positions.count, &triangles);
    if (status != PrStatus_Ok) {
        triangles.reset();
        return status;
    }

    RawArray<PrDVec3> sums;    // by vertex
    sums.resizeZeroed(positions.count);
    for (uint32_t i = 0; i < triangles.count; i += 3) {
        const uint32_t* corners = triangles.data + i;
        PrVec3 p[3] = {
            meshReadVec3(positions, corners[0]),
            meshReadVec3(positions, corners[1]),
            meshReadVec3(positions, corners[2]),
        };
        Face face;
        if (!faceOf(p, &face))
            continue;
        for (uint32_t corner = 0; corner < 3; ++corner) {
            PrDVec3& sum = sums.data[corners[corner]];
            sum.x += face.normal.x * face.angles[corner];
            sum.y += face.normal.y * face.angles[corner];
            sum.z += face.normal.z * face.angles[corner];
        }
    }
    for (uint32_t vertex = 0; vertex < positions.count; ++vertex)
        writeUnit(normals, vertex, sums.data[vertex]);
    sums.reset();
    triangles.reset();
    return PrStatus_Ok;
}

PrStatus prMeshFlatNormals(PrMeshAttributeView positions, PrMeshIndexView indices, PrMeshAttribute normals) {
    if (!meshAttributeIsReadable(positions, 3) || !meshAttributeIsReadable(meshAttributeView(normals), 3) ||
            meshIndexSize(indices.type) == 0 || indices.count % 3 != 0)
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > positions.count)
        return PrStatus_OutOfRange;
    PR_ASSERT(normals.count >= indices.count);
    for (uint32_t i = 0; i < indices.count; i += 3) {
        PrVec3 p[3];
        for (uint32_t corner = 0; corner < 3; ++corner) {
            uint32_t index = meshIndex(indices, i + corner);
            if (index >= positions.count)
                return PrStatus_OutOfRange;
            p[corner] = meshReadVec3(positions, index);
        }
        Face face;
        PrDVec3 normal{};
        if (faceOf(p, &face))
            normal = face.normal;
        for (uint32_t corner = 0; corner < 3; ++corner)
            writeUnit(normals, i + corner, normal);
    }
    return PrStatus_Ok;
}
