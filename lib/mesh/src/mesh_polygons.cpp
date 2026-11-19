// Mesh polygons: faces of any number of vertices cut into triangles.

#include "pr/mesh_polygons.h"

#include "pr/float.h"
#include "pr/raw_array.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

// A corner of a polygon in the plane that it is cut in.
struct PlanePoint {
    double x;
    double y;
};

// Returns twice the signed area of the triangle (a, b, c): positive if it winds counterclockwise.
double turn(PlanePoint a, PlanePoint b, PlanePoint c) {
    return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

bool samePoint(PlanePoint a, PlanePoint b) {
    return a.x == b.x && a.y == b.y;
}

// Returns true if `p` is inside the counterclockwise triangle (a, b, c) or on its boundary.
bool inTriangle(PlanePoint p, PlanePoint a, PlanePoint b, PlanePoint c) {
    return turn(a, b, p) >= 0 && turn(b, c, p) >= 0 && turn(c, a, p) >= 0;
}

// Writes the `count` corners of a polygon from `first`, in the plane of the largest axis of the polygon's Newell
// normal, to `out`, mirrored where that keeps the polygon counterclockwise about its normal. Returns false if a
// position is not finite or the polygon has no area.
bool project(PrMeshAttributeView positions, PrMeshIndexView corners, uint32_t first, uint32_t count,
        RawArray<PlanePoint>* out) {
    double n[3] = {0, 0, 0};
    for (uint32_t i = 0; i < count; ++i) {
        PrVec3 p = meshReadVec3(positions, meshIndex(corners, first + i));
        PrVec3 q = meshReadVec3(positions, meshIndex(corners, first + (i + 1) % count));
        if (!meshIsFinite(p))
            return false;
        n[0] += (double(p.y) - double(q.y)) * (double(p.z) + double(q.z));
        n[1] += (double(p.z) - double(q.z)) * (double(p.x) + double(q.x));
        n[2] += (double(p.x) - double(q.x)) * (double(p.y) + double(q.y));
    }
    double a[3] = {n[0] < 0 ? -n[0] : n[0], n[1] < 0 ? -n[1] : n[1], n[2] < 0 ? -n[2] : n[2]};
    uint32_t axis = a[2] >= a[0] && a[2] >= a[1] ? 2 : (a[0] >= a[1] ? 0 : 1);
    if (!(a[axis] > 0))
        return false;
    // The plane's axes follow the dropped one cyclically, (x, y) for z, (y, z) for x and (z, x) for y, so that their
    // cross product is the dropped axis.
    double sign = n[axis] < 0 ? -1 : 1;
    out->clear();
    for (uint32_t i = 0; i < count; ++i) {
        PrVec3 p = meshReadVec3(positions, meshIndex(corners, first + i));
        double c[3] = {double(p.x), double(p.y), double(p.z)};
        out->push(PlanePoint{sign * c[(axis + 1) % 3], c[(axis + 2) % 3]});
    }
    return true;
}

// Writes the fan of the polygon's corners `remaining` from its first, as positions of `corners` from `first`.
uint32_t* writeFan(const RawArray<uint32_t>& remaining, uint32_t first, uint32_t* out) {
    for (uint32_t i = 1; i + 1 < remaining.count; ++i) {
        *out++ = first + remaining.data[0];
        *out++ = first + remaining.data[i];
        *out++ = first + remaining.data[i + 1];
    }
    return out;
}

// Returns true if the corner `at` of `remaining` is an ear: it turns counterclockwise, and no other corner lies in the
// triangle that it makes with its neighbors, except at that triangle's own points.
bool isEar(const RawArray<PlanePoint>& points, const RawArray<uint32_t>& remaining, uint32_t at) {
    uint32_t count = remaining.count;
    PlanePoint a = points.data[remaining.data[(at + count - 1) % count]];
    PlanePoint b = points.data[remaining.data[at]];
    PlanePoint c = points.data[remaining.data[(at + 1) % count]];
    if (!(turn(a, b, c) > 0))
        return false;
    for (uint32_t i = 0; i < count; ++i) {
        PlanePoint p = points.data[remaining.data[i]];
        if (samePoint(p, a) || samePoint(p, b) || samePoint(p, c))
            continue;
        if (inTriangle(p, a, b, c))
            return false;
    }
    return true;
}

// Cuts one polygon into triangles, written as positions of `corners` from `first`. Returns past the last written.
uint32_t* cutPolygon(PrMeshAttributeView positions, PrMeshIndexView corners, uint32_t first, uint32_t count,
        RawArray<PlanePoint>* points, RawArray<uint32_t>* remaining, uint32_t* out) {
    remaining->clear();
    for (uint32_t i = 0; i < count; ++i)
        remaining->push(i);
    if (count == 3 || !project(positions, corners, first, count, points))
        return writeFan(*remaining, first, out);
    bool convex = true;
    for (uint32_t i = 0; convex && i < count; ++i)
        convex = turn(points->data[(i + count - 1) % count], points->data[i], points->data[(i + 1) % count]) >= 0;
    if (convex)
        return writeFan(*remaining, first, out);
    // Ear clipping: cut off a corner whose triangle holds no other corner, until three remain. The search for the next
    // ear begins where the last was cut, so that a polygon's ears are found in one pass about it where it has them.
    uint32_t at = 0;
    while (remaining->count > 3) {
        uint32_t tried = 0;
        while (tried < remaining->count && !isEar(*points, *remaining, at)) {
            at = (at + 1) % remaining->count;
            ++tried;
        }
        if (tried == remaining->count)
            return writeFan(*remaining, first, out);
        uint32_t n = remaining->count;
        *out++ = first + remaining->data[(at + n - 1) % n];
        *out++ = first + remaining->data[at];
        *out++ = first + remaining->data[(at + 1) % n];
        for (uint32_t i = at; i + 1 < n; ++i)
            remaining->data[i] = remaining->data[i + 1];
        remaining->pop();
        if (at >= remaining->count)
            at = 0;
        else if (at != 0)
            --at;
    }
    return writeFan(*remaining, first, out);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

uint32_t prMeshPolygonTriangleCount(const uint32_t* PR_NULLABLE counts, uint32_t polygonCount) {
    uint64_t total = 0;
    for (uint32_t i = 0; i < polygonCount; ++i) {
        if (counts[i] >= 3)
            total += counts[i] - 2;
    }
    return total > UINT32_MAX ? UINT32_MAX : uint32_t(total);
}

PrStatus prMeshPolygonsTriangulate(PrMeshAttributeView positions, PrMeshIndexView corners,
        const uint32_t* PR_NULLABLE counts, uint32_t polygonCount, uint32_t* PR_NULLABLE triangles,
        uint32_t* PR_NULLABLE polygons) {
    if (!meshAttributeIsReadable(positions, 3) || meshIndexSize(corners.type) == 0)
        return PrStatus_InvalidData;
    uint64_t total = 0;
    for (uint32_t i = 0; i < polygonCount; ++i)
        total += counts[i];
    if (total != corners.count)
        return PrStatus_InvalidData;
    for (uint32_t i = 0; i < corners.count; ++i) {
        if (meshIndex(corners, i) >= positions.count)
            return PrStatus_OutOfRange;
    }
    RawArray<PlanePoint> points{};
    RawArray<uint32_t> remaining{};
    uint32_t* out = triangles;
    uint32_t first = 0;
    for (uint32_t polygon = 0; polygon < polygonCount; ++polygon) {
        uint32_t count = counts[polygon];
        if (count >= 3) {
            uint32_t* end = cutPolygon(positions, corners, first, count, &points, &remaining, out);
            for (uint32_t* t = out; polygons != nullptr && t < end; t += 3)
                polygons[(t - triangles) / 3] = polygon;
            out = end;
        }
        first += count;
    }
    points.reset();
    remaining.reset();
    return PrStatus_Ok;
}
