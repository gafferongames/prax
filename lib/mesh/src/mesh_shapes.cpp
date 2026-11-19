// Mesh shapes: polygon meshes that approximate the quadric surfaces and the solids of planes.

#include "pr/mesh_shapes.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/raw_array.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

// A point of a round shape's profile, in the half plane of its axis: its distance from the axis, its height along the
// axis, and the surface's normal there, as its parts away from the axis and along it. A point on the axis is one point
// of the mesh, and any other is a ring of them.
struct ProfilePoint {
    double radius;
    double height;
    double normalOut;
    double normalUp;
};

// Writes the profile of a round shape, from its top, the positive end of its axis, to its bottom. Two points in a row
// at one place are an edge where the surface folds, with the normal of each side.
void profileOf(const PrMeshShapeDesc& desc, RawArray<ProfilePoint>* out) {
    uint32_t stacks = desc.segments / 2 > 2 ? desc.segments / 2 : 2;
    double radius = double(desc.radius);
    double half = 0.5 * double(desc.height);
    switch (desc.kind) {
        case PrMeshShapeKind_Sphere:
            for (uint32_t i = 0; i <= stacks; ++i) {
                double s = 0;
                double c = 0;
                meshTurnSinCos(0.5 * double(i) / stacks, &s, &c);
                out->push(ProfilePoint{radius * s, radius * c, s, c});
            }
            break;
        case PrMeshShapeKind_Capsule: {
            // Each hemisphere has half of a sphere's stacks, and the cylinder between them joins their equators.
            uint32_t hemisphere = stacks / 2 > 1 ? stacks / 2 : 1;
            for (uint32_t end = 0; end < 2; ++end) {
                double center = end == 0 ? half : -half;
                for (uint32_t i = 0; i <= hemisphere; ++i) {
                    double s = 0;
                    double c = 0;
                    meshTurnSinCos(0.25 * double(end * hemisphere + i) / hemisphere, &s, &c);
                    out->push(ProfilePoint{radius * s, center + radius * c, s, c});
                }
            }
            break;
        }
        case PrMeshShapeKind_Cylinder: {
            double top = double(desc.radiusTop);
            double bottom = double(desc.radiusBottom);
            // The side's normal leans toward the narrower end.
            double out2 = 2 * half;
            double up = bottom - top;
            double length = prF64Sqrt(out2 * out2 + up * up);
            double sideOut = length > 0 ? out2 / length : 1;
            double sideUp = length > 0 ? up / length : 0;
            if (top > 0) {
                out->push(ProfilePoint{0, half, 0, 1});
                out->push(ProfilePoint{top, half, 0, 1});
            }
            out->push(ProfilePoint{top, half, sideOut, sideUp});
            out->push(ProfilePoint{bottom, -half, sideOut, sideUp});
            if (bottom > 0) {
                out->push(ProfilePoint{bottom, -half, 0, -1});
                out->push(ProfilePoint{0, -half, 0, -1});
            }
            break;
        }
        default:
            break;
    }
}

// Returns true if two points of a profile are at one place, an edge where the surface folds.
bool samePlace(const ProfilePoint& a, const ProfilePoint& b) {
    return a.radius == b.radius && a.height == b.height;
}

uint32_t segmentsOf(const PrMeshShapeDesc& desc) {
    return desc.segments >= 3 ? desc.segments : 3;
}

// Returns a point of the shape about z turned so that z is the shape's axis.
PrVec3 turned(const PrMeshShapeDesc& desc, double x, double y, double z) {
    float out[3];
    out[desc.axis % 3] = float(z);
    out[(desc.axis + 1) % 3] = float(x);
    out[(desc.axis + 2) % 3] = float(y);
    // No coordinate is negative zero, so equal points have equal bytes.
    return PrVec3{out[0] + 0.0f, out[1] + 0.0f, out[2] + 0.0f};
}

PrMeshShapeCounts roundCounts(const PrMeshShapeDesc& desc, const RawArray<ProfilePoint>& profile) {
    uint32_t segments = segmentsOf(desc);
    PrMeshShapeCounts counts{};
    for (uint32_t i = 0; i < profile.count; ++i) {
        const ProfilePoint& p = profile.data[i];
        if (i == 0 || !samePlace(profile.data[i - 1], p))
            counts.pointCount += p.radius > 0 ? segments : 1;
        if (i == 0 || samePlace(profile.data[i - 1], p))
            continue;
        bool pole = profile.data[i - 1].radius == 0 || p.radius == 0;
        counts.faceCount += segments;
        counts.cornerCount += segments * (pole ? 3 : 4);
    }
    return counts;
}

// A corner of a face of a round shape: the place of the profile that it is at, its point, and the fraction of a turn
// about the axis at which its normal is taken.
struct FaceCorner {
    const ProfilePoint* place;
    uint32_t point;
    double turn;
};

void writeRound(const PrMeshShapeDesc& desc, const RawArray<ProfilePoint>& profile, PrVec3* points,
        uint32_t* faceCounts, uint32_t* corners, PrVec3* normals) {
    uint32_t segments = segmentsOf(desc);
    uint32_t pointCount = 0;
    uint32_t faceCount = 0;
    uint32_t cornerCount = 0;
    uint32_t above = 0;         // the first point of the previous place of the profile
    for (uint32_t i = 0; i < profile.count; ++i) {
        const ProfilePoint& p = profile.data[i];
        bool newPlace = i == 0 || !samePlace(profile.data[i - 1], p);
        uint32_t first = pointCount;
        if (newPlace) {
            uint32_t ring = p.radius > 0 ? segments : 1;
            for (uint32_t k = 0; k < ring; ++k) {
                double s = 0;
                double c = 0;
                meshTurnSinCos(double(k) / segments, &s, &c);
                points[pointCount++] = turned(desc, p.radius * c, p.radius * s, p.height);
            }
        } else {
            first = above;
        }
        if (i != 0 && newPlace) {
            const ProfilePoint& q = profile.data[i - 1];
            for (uint32_t k = 0; k < segments; ++k) {
                uint32_t next = (k + 1) % segments;
                // The corners of the face, counterclockwise seen from outside: down the profile, across and back up. A
                // place on the axis is one corner, whose normal is the surface's in the middle of the face.
                double middle = (double(k) + 0.5) / segments;
                FaceCorner faceCorners[4];
                uint32_t count = 0;
                if (q.radius == 0)
                    faceCorners[count++] = FaceCorner{&q, above, middle};
                else
                    faceCorners[count++] = FaceCorner{&q, above + k, double(k) / segments};
                if (p.radius == 0) {
                    faceCorners[count++] = FaceCorner{&p, first, middle};
                } else {
                    faceCorners[count++] = FaceCorner{&p, first + k, double(k) / segments};
                    faceCorners[count++] = FaceCorner{&p, first + next, double(next) / segments};
                }
                if (q.radius != 0)
                    faceCorners[count++] = FaceCorner{&q, above + next, double(next) / segments};
                for (uint32_t j = 0; j < count; ++j) {
                    double s = 0;
                    double c = 0;
                    meshTurnSinCos(faceCorners[j].turn, &s, &c);
                    const ProfilePoint& place = *faceCorners[j].place;
                    corners[cornerCount + j] = faceCorners[j].point;
                    normals[cornerCount + j] = turned(desc, place.normalOut * c, place.normalOut * s, place.normalUp);
                }
                faceCounts[faceCount++] = count;
                cornerCount += count;
            }
        }
        if (newPlace)
            above = first;
    }
}

// The corners of a box's faces, by the signs of their coordinates, counterclockwise seen from outside, with each face's
// normal.
struct BoxFace {
    uint8_t corners[4];         // bit 0 for +x, bit 1 for +y, bit 2 for +z
    int8_t normal[3];
};

constexpr BoxFace kBoxFaces[6] = {
    {{1, 3, 7, 5}, {1, 0, 0}},
    {{0, 4, 6, 2}, {-1, 0, 0}},
    {{2, 6, 7, 3}, {0, 1, 0}},
    {{0, 1, 5, 4}, {0, -1, 0}},
    {{4, 5, 7, 6}, {0, 0, 1}},
    {{0, 2, 3, 1}, {0, 0, -1}},
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMeshShapeCounts prMeshShapeCounts(const PrMeshShapeDesc* desc) {
    switch (desc->kind) {
        case PrMeshShapeKind_Box:
            return PrMeshShapeCounts{.pointCount = 8, .faceCount = 6, .cornerCount = 24};
        case PrMeshShapeKind_Plane:
            return PrMeshShapeCounts{.pointCount = 4, .faceCount = 1, .cornerCount = 4};
        default:
            break;
    }
    RawArray<ProfilePoint> profile{};
    profileOf(*desc, &profile);
    PrMeshShapeCounts counts = roundCounts(*desc, profile);
    profile.reset();
    return counts;
}

void prMeshShapeWrite(const PrMeshShapeDesc* desc, PrVec3* points, uint32_t* faceCounts, uint32_t* corners, PrVec3* normals) {
    if (desc->kind == PrMeshShapeKind_Box) {
        PrDVec3 half = prDVec3FromVec3(desc->size) * 0.5;
        for (uint32_t i = 0; i < 8; ++i)
            points[i] = turned(*desc, i & 1 ? half.x : -half.x, i & 2 ? half.y : -half.y, i & 4 ? half.z : -half.z);
        for (uint32_t f = 0; f < 6; ++f) {
            faceCounts[f] = 4;
            const BoxFace& face = kBoxFaces[f];
            for (uint32_t j = 0; j < 4; ++j) {
                corners[f * 4 + j] = face.corners[j];
                normals[f * 4 + j] = turned(*desc, face.normal[0], face.normal[1], face.normal[2]);
            }
        }
        return;
    }
    if (desc->kind == PrMeshShapeKind_Plane) {
        double x = 0.5 * double(desc->size.x);
        double y = 0.5 * double(desc->size.y);
        points[0] = turned(*desc, -x, -y, 0);
        points[1] = turned(*desc, x, -y, 0);
        points[2] = turned(*desc, x, y, 0);
        points[3] = turned(*desc, -x, y, 0);
        faceCounts[0] = 4;
        for (uint32_t j = 0; j < 4; ++j) {
            corners[j] = j;
            normals[j] = turned(*desc, 0, 0, 1);
        }
        return;
    }
    RawArray<ProfilePoint> profile{};
    profileOf(*desc, &profile);
    writeRound(*desc, profile, points, faceCounts, corners, normals);
    profile.reset();
}
