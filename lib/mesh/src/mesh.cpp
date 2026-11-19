// Meshes: geometry processing for triangle meshes held in the caller's arrays.

#include "pr/mesh.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/hash.h"
#include "pr/hash_index.h"
#include "pr/mem.h"

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

// The sphere is fitted in doubles so that its passes do not accumulate the rounding of floats.

// The number of passes that move a sphere's center toward the farthest point.
constexpr uint32_t kSphereRefinePasses = 8;

double distanceSquared(PrDVec3 a, PrDVec3 b) {
    PrDVec3 d = a - b;
    return prDVec3Dot(d, d);
}

// Returns the point `t` of the way from `a` to `b`.
PrDVec3 lerp(PrDVec3 a, PrDVec3 b, double t) {
    return a + (b - a) * t;
}

// The finite point of `positions` that is farthest from a center, and the square of its distance.
struct Farthest {
    PrDVec3 point;
    double distanceSquared;
};

Farthest farthestFrom(PrMeshAttributeView positions, PrDVec3 center) {
    Farthest farthest{center, 0};
    for (uint32_t i = 0; i < positions.count; ++i) {
        PrVec3 position = meshReadVec3(positions, i);
        if (!meshIsFinite(position))
            continue;
        PrDVec3 point = prDVec3FromVec3(position);
        double d = distanceSquared(point, center);
        if (d > farthest.distanceSquared)
            farthest = Farthest{point, d};
    }
    return farthest;
}

// The box of the finite points of `positions`, and for each axis the first points that reach its least and greatest
// coordinates.
struct Extent {
    PrBox3 box;
    PrVec3 least[3];                // by axis
    PrVec3 greatest[3];             // by axis
    bool hasPoints;
};

float axisOf(PrVec3 v, uint32_t axis) {
    if (axis == 0)
        return v.x;
    return axis == 1 ? v.y : v.z;
}

Extent extentOf(PrMeshAttributeView positions) {
    Extent extent{};
    for (uint32_t i = 0; i < positions.count; ++i) {
        PrVec3 position = meshReadVec3(positions, i);
        if (!meshIsFinite(position))
            continue;
        if (!extent.hasPoints) {
            extent.hasPoints = true;
            for (uint32_t axis = 0; axis < 3; ++axis) {
                extent.least[axis] = position;
                extent.greatest[axis] = position;
            }
            continue;
        }
        for (uint32_t axis = 0; axis < 3; ++axis) {
            if (axisOf(position, axis) < axisOf(extent.least[axis], axis))
                extent.least[axis] = position;
            if (axisOf(position, axis) > axisOf(extent.greatest[axis], axis))
                extent.greatest[axis] = position;
        }
    }
    if (extent.hasPoints) {
        extent.box.min = PrVec3{extent.least[0].x, extent.least[1].y, extent.least[2].z};
        extent.box.max = PrVec3{extent.greatest[0].x, extent.greatest[1].y, extent.greatest[2].z};
    }
    return extent;
}

// Returns the center of Ritter's sphere: the sphere on the most distant pair of the extreme points, grown to take in
// each point outside it.
PrDVec3 ritterCenter(PrMeshAttributeView positions, const Extent& extent) {
    uint32_t widest = 0;
    double widestSquared = -1;
    for (uint32_t axis = 0; axis < 3; ++axis) {
        double d = distanceSquared(prDVec3FromVec3(extent.least[axis]), prDVec3FromVec3(extent.greatest[axis]));
        if (d > widestSquared) {
            widestSquared = d;
            widest = axis;
        }
    }
    PrDVec3 center = lerp(prDVec3FromVec3(extent.least[widest]), prDVec3FromVec3(extent.greatest[widest]), 0.5);
    double radius = prF64Sqrt(widestSquared) * 0.5;
    for (uint32_t i = 0; i < positions.count; ++i) {
        PrVec3 position = meshReadVec3(positions, i);
        if (!meshIsFinite(position))
            continue;
        PrDVec3 point = prDVec3FromVec3(position);
        double d = distanceSquared(point, center);
        if (d <= radius * radius)
            continue;
        double distance = prF64Sqrt(d);
        double grown = (radius + distance) * 0.5;
        center = lerp(center, point, (distance - grown) / distance);
        radius = grown;
    }
    return center;
}

uint32_t hashOf(const uint8_t* vertex, uint32_t vertexSize) {
    return uint32_t(prHashBytes(vertex, vertexSize, 0));
}

// ---------------------------------------------------------------------------------------------------------------------
// Formats

// The size in bytes of each component, by PrMeshComponent.
constexpr uint8_t kComponentBytes[PrMeshComponent_Count] = {4, 8, 2, 1, 1, 2, 2, 4, 4, 8, 8};

// The largest value of each integer component, by PrMeshComponent, and zero for a float.
constexpr double kComponentMax[PrMeshComponent_Count] = {
    0, 0, 0, 127.0, 255.0, 32767.0, 65535.0, 2147483647.0, 4294967295.0, 9223372036854775807.0, 18446744073709551615.0,
};

bool componentIsSigned(PrMeshComponent component) {
    return component == PrMeshComponent_Int8 || component == PrMeshComponent_Int16 ||
        component == PrMeshComponent_Int32 || component == PrMeshComponent_Int64;
}

// Returns the number that the component at `at` stands for in `format`.
double readComponent(const uint8_t* at, PrMeshFormat format) {
    double value = 0;
    switch (format.component) {
        case PrMeshComponent_Float32: {
            float v;
            prMemCopy(&v, at, sizeof(v));
            return double(v);
        }
        case PrMeshComponent_Float64:
            prMemCopy(&value, at, sizeof(value));
            return value;
        case PrMeshComponent_Float16: {
            PrFloat16 v;
            prMemCopy(&v, at, sizeof(v));
            return double(prFloat16ToFloat(v));
        }
        case PrMeshComponent_Int8: {
            int8_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        case PrMeshComponent_Uint8:
            value = double(*at);
            break;
        case PrMeshComponent_Int16: {
            int16_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        case PrMeshComponent_Uint16: {
            uint16_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        case PrMeshComponent_Int32: {
            int32_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        case PrMeshComponent_Uint32: {
            uint32_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        case PrMeshComponent_Int64: {
            int64_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
        default: {
            uint64_t v;
            prMemCopy(&v, at, sizeof(v));
            value = double(v);
            break;
        }
    }
    if (!format.normalized)
        return value;
    double scaled = value / kComponentMax[format.component];
    return scaled < -1 ? -1 : scaled;
}

// Returns `value` clamped to [low, high].
double clampTo(double value, double low, double high) {
    return value < low ? low : value > high ? high : value;
}

// Writes the number `value` as the component at `at` of `format`.
void writeComponent(uint8_t* at, PrMeshFormat format, double value) {
    PrMeshComponent component = format.component;
    if (component == PrMeshComponent_Float32) {
        float v = float(value);
        prMemCopy(at, &v, sizeof(v));
        return;
    }
    if (component == PrMeshComponent_Float64) {
        prMemCopy(at, &value, sizeof(value));
        return;
    }
    if (component == PrMeshComponent_Float16) {
        PrFloat16 v = prFloat16FromFloat(float(value));
        prMemCopy(at, &v, sizeof(v));
        return;
    }
    double most = kComponentMax[component];
    bool isSigned = componentIsSigned(component);
    if (format.normalized)
        value = clampTo(value, isSigned ? -1.0 : 0.0, 1.0) * most;
    // The largest 64-bit values are not doubles, so a value is clamped below the double that is their bound.
    double low = isSigned ? -most - 1 : 0;
    value = prF64Round(clampTo(value, low, most));
    switch (component) {
        case PrMeshComponent_Int8: {
            int8_t v = int8_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        case PrMeshComponent_Uint8:
            *at = uint8_t(value);
            break;
        case PrMeshComponent_Int16: {
            int16_t v = int16_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        case PrMeshComponent_Uint16: {
            uint16_t v = uint16_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        case PrMeshComponent_Int32: {
            int32_t v = int32_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        case PrMeshComponent_Uint32: {
            uint32_t v = uint32_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        case PrMeshComponent_Int64: {
            int64_t v = value >= 9223372036854775807.0 ? INT64_MAX : int64_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
        default: {
            uint64_t v = value >= 18446744073709551615.0 ? UINT64_MAX : uint64_t(value);
            prMemCopy(at, &v, sizeof(v));
            break;
        }
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

PrMeshFormat prMeshFormatFloat32(uint32_t count) {
    return PrMeshFormat{.component = PrMeshComponent_Float32, .count = uint8_t(count)};
}

bool prMeshFormatIsValid(PrMeshFormat format) {
    if (format.component >= PrMeshComponent_Count || format.count == 0 || format.count > 4)
        return false;
    bool isFloat = format.component <= PrMeshComponent_Float16;
    return !(isFloat && format.normalized);
}

uint32_t prMeshFormatSize(PrMeshFormat format) {
    PR_ASSERT(prMeshFormatIsValid(format));
    return uint32_t(kComponentBytes[format.component]) * format.count;
}

PrStatus prMeshAttributeConvert(PrMeshAttributeView source, const float* PR_NULLABLE fallback, PrMeshAttribute out) {
    bool sourceRead = source.data != nullptr && source.count != 0;
    if (!prMeshFormatIsValid(out.format) || out.stride < prMeshFormatSize(out.format) ||
            (sourceRead && (!prMeshFormatIsValid(source.format) || source.stride < prMeshFormatSize(source.format))))
        return PrStatus_InvalidData;
    if (out.count == 0)
        return PrStatus_Ok;
    if (out.data == nullptr)
        return PrStatus_InvalidData;
    const float kUnit[4] = {0, 0, 0, 1};
    const float* missing = fallback != nullptr ? fallback : kUnit;
    uint32_t outBytes = kComponentBytes[out.format.component];
    uint32_t sourceBytes = sourceRead ? kComponentBytes[source.format.component] : 0;
    uint32_t sourceComponents = sourceRead ? source.format.count : 0;
    for (uint32_t i = 0; i < out.count; ++i) {
        uint8_t* to = static_cast<uint8_t*>(out.data) + size_t(i) * out.stride;
        const uint8_t* from = nullptr;
        if (sourceRead && i < source.count)
            from = static_cast<const uint8_t*>(source.data) + size_t(i) * source.stride;
        for (uint32_t c = 0; c < out.format.count; ++c) {
            double value = double(missing[c]);
            if (from != nullptr && c < sourceComponents) {
                double read = readComponent(from + c * sourceBytes, source.format);
                if (prF64IsFinite(read))
                    value = read;
            }
            writeComponent(to + c * outBytes, out.format, value);
        }
    }
    return PrStatus_Ok;
}

PrStatus prMeshBox(PrMeshAttributeView positions, PrBox3* out) {
    *out = PrBox3{};
    if (!meshAttributeIsReadable(positions, 3))
        return PrStatus_InvalidData;
    *out = extentOf(positions).box;
    return PrStatus_Ok;
}

PrStatus prMeshSphere(PrMeshAttributeView positions, PrSphere* out) {
    *out = PrSphere{};
    if (!meshAttributeIsReadable(positions, 3))
        return PrStatus_InvalidData;
    Extent extent = extentOf(positions);
    if (!extent.hasPoints)
        return PrStatus_Ok;

    // The better of the box's center and Ritter's center starts the refinement.
    PrDVec3 center = lerp(prDVec3FromVec3(extent.box.min), prDVec3FromVec3(extent.box.max), 0.5);
    Farthest farthest = farthestFrom(positions, center);
    PrDVec3 ritter = ritterCenter(positions, extent);
    Farthest ritterFarthest = farthestFrom(positions, ritter);
    if (ritterFarthest.distanceSquared < farthest.distanceSquared) {
        center = ritter;
        farthest = ritterFarthest;
    }

    // Each pass moves the center a shrinking step toward the farthest point, as Badoiu and Clarkson's iteration does,
    // and the best center seen is kept.
    PrDVec3 moving = center;
    Farthest movingFarthest = farthest;
    for (uint32_t pass = 0; pass < kSphereRefinePasses; ++pass) {
        moving = lerp(moving, movingFarthest.point, 1.0 / double(pass + 8));
        movingFarthest = farthestFrom(positions, moving);
        if (movingFarthest.distanceSquared < farthest.distanceSquared) {
            center = moving;
            farthest = movingFarthest;
        }
    }

    // The radius is measured from the center as a float holds it, and rounded up.
    out->center = PrVec3{float(center.x), float(center.y), float(center.z)};
    double radius = prF64Sqrt(farthestFrom(positions, prDVec3FromVec3(out->center)).distanceSquared);
    out->radius = float(radius);
    if (double(out->radius) < radius)
        out->radius = prF32NextAfter(out->radius, kPrF32Infinity);
    return PrStatus_Ok;
}

uint32_t prMeshTriangleListCount(PrMeshTopology topology, uint32_t indexCount) {
    switch (topology) {
        case PrMeshTopology_TriangleList:
            return indexCount - indexCount % 3;
        case PrMeshTopology_TriangleStrip:
        case PrMeshTopology_TriangleFan:
            if (indexCount < 3 || indexCount - 2 > 0xffffffffu / 3)
                return 0;
            return (indexCount - 2) * 3;
    }
    return 0;
}

PrStatus prMeshTriangleList(PrMeshTopology topology, PrMeshIndexView indices, uint32_t vertexCount, PrMeshIndices out) {
    if (topology > PrMeshTopology_TriangleFan || meshIndexSize(indices.type) == 0 || meshIndexSize(out.type) == 0)
        return PrStatus_InvalidData;
    uint32_t listCount = prMeshTriangleListCount(topology, indices.count);
    if (listCount == 0)
        return indices.count >= 3 ? PrStatus_Limit : PrStatus_Ok;
    PR_ASSERT(out.count >= listCount && out.data != nullptr);
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;

    for (uint32_t triangle = 0; triangle < listCount / 3; ++triangle) {
        uint32_t corners[3];
        switch (topology) {
            case PrMeshTopology_TriangleStrip: {
                uint32_t odd = triangle & 1;
                corners[0] = triangle;
                corners[1] = triangle + 1 + odd;
                corners[2] = triangle + 2 - odd;
                break;
            }
            case PrMeshTopology_TriangleFan:
                corners[0] = triangle + 1;
                corners[1] = triangle + 2;
                corners[2] = 0;
                break;
            default:
                corners[0] = triangle * 3;
                corners[1] = triangle * 3 + 1;
                corners[2] = triangle * 3 + 2;
                break;
        }
        for (uint32_t corner = 0; corner < 3; ++corner) {
            uint32_t index = meshIndex(indices, corners[corner]);
            if (index >= vertexCount || !meshWriteIndex(out, triangle * 3 + corner, index))
                return PrStatus_OutOfRange;
        }
    }
    return PrStatus_Ok;
}

PrStatus prMeshVerticesUnindex(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
        PrMeshIndexView indices, void* PR_NULLABLE out) {
    if (vertexSize == 0 || meshIndexSize(indices.type) == 0 || (vertices == nullptr && vertexCount != 0))
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;
    PR_ASSERT(out != nullptr || indices.count == 0);
    const uint8_t* source = static_cast<const uint8_t*>(vertices);
    uint8_t* target = static_cast<uint8_t*>(out);
    for (uint32_t i = 0; i < indices.count; ++i) {
        uint32_t index = meshIndex(indices, i);
        if (index >= vertexCount)
            return PrStatus_OutOfRange;
        prMemCopy(target + size_t(i) * vertexSize, source + size_t(index) * vertexSize, vertexSize);
    }
    return PrStatus_Ok;
}

PrStatus prMeshWeldRemap(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
        uint32_t* PR_NULLABLE remap, uint32_t* outCount) {
    *outCount = 0;
    if (vertexSize == 0 || (vertices == nullptr && vertexCount != 0))
        return PrStatus_InvalidData;
    PR_ASSERT(remap != nullptr || vertexCount == 0);
    const uint8_t* bytes = static_cast<const uint8_t*>(vertices);

    // The index holds the first vertex of each welded vertex. New indices follow the order of those first vertices, so
    // the order of the index's slots never reaches the result.
    HashIndex firsts;
    firsts.reserve(vertexCount);
    uint32_t count = 0;
    for (uint32_t vertex = 0; vertex < vertexCount; ++vertex) {
        const uint8_t* at = bytes + size_t(vertex) * vertexSize;
        uint32_t hash = hashOf(at, vertexSize);
        uint32_t first = firsts.find(hash, [&](uint32_t other) {
            return prMemEquals(at, bytes + size_t(other) * vertexSize, vertexSize);
        });
        if (first != kHashMissing) {
            remap[vertex] = remap[first];
            continue;
        }
        firsts.insert(hash, vertex);
        remap[vertex] = count++;
    }
    firsts.reset();
    *outCount = count;
    return PrStatus_Ok;
}

PrStatus prMeshFetchRemap(PrMeshIndexView indices, uint32_t vertexCount, uint32_t* PR_NULLABLE remap,
        uint32_t* outCount) {
    *outCount = 0;
    if (meshIndexSize(indices.type) == 0)
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;
    PR_ASSERT(remap != nullptr || vertexCount == 0);
    for (uint32_t vertex = 0; vertex < vertexCount; ++vertex)
        remap[vertex] = kPrMeshIndexNone;
    uint32_t count = 0;
    for (uint32_t i = 0; i < indices.count; ++i) {
        uint32_t index = meshIndex(indices, i);
        if (index >= vertexCount)
            return PrStatus_OutOfRange;
        if (remap[index] == kPrMeshIndexNone)
            remap[index] = count++;
    }
    *outCount = count;
    return PrStatus_Ok;
}

PrStatus prMeshVerticesRemap(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
        const uint32_t* PR_NULLABLE remap, void* PR_NULLABLE out, uint32_t outCount) {
    if (vertexSize == 0 || (vertices == nullptr && vertexCount != 0))
        return PrStatus_InvalidData;
    PR_ASSERT((remap != nullptr || vertexCount == 0) && (out != nullptr || outCount == 0));
    const uint8_t* source = static_cast<const uint8_t*>(vertices);
    uint8_t* target = static_cast<uint8_t*>(out);

    // The vertices are written last to first, so that the first vertex of several that share an index is the one left.
    for (uint32_t vertex = vertexCount; vertex-- > 0;) {
        uint32_t index = remap[vertex];
        if (index == kPrMeshIndexNone)
            continue;
        if (index >= outCount)
            return PrStatus_OutOfRange;
        prMemCopy(target + size_t(index) * vertexSize, source + size_t(vertex) * vertexSize, vertexSize);
    }
    return PrStatus_Ok;
}

PrStatus prMeshIndicesRemap(PrMeshIndexView indices, const uint32_t* PR_NULLABLE remap, uint32_t vertexCount,
        PrMeshIndices out) {
    if (meshIndexSize(indices.type) == 0 || meshIndexSize(out.type) == 0)
        return PrStatus_InvalidData;
    if (indices.data == nullptr && indices.count > vertexCount)
        return PrStatus_OutOfRange;
    PR_ASSERT(out.count >= indices.count && (out.data != nullptr || indices.count == 0));
    PR_ASSERT(remap != nullptr || vertexCount == 0);
    for (uint32_t i = 0; i < indices.count; ++i) {
        uint32_t index = meshIndex(indices, i);
        if (index >= vertexCount || remap[index] == kPrMeshIndexNone || !meshWriteIndex(out, i, remap[index]))
            return PrStatus_OutOfRange;
    }
    return PrStatus_Ok;
}
