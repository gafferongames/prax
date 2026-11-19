// Mesh access: reading and writing the caller's attributes and indices, and the arithmetic that the mesh functions
// share.

#include "mesh/src/mesh_access.h"

namespace pr_impl {

namespace {

// The arctangents of k / 8 for k from 0 to 8, rounded to doubles.
constexpr double kAtanEighths[9] = {
    0.0,
    0.12435499454676144,
    0.24497866312686414,
    0.35877067027057225,
    0.46364760900080615,
    0.5585993153435624,
    0.6435011087932844,
    0.7188299996216245,
    0.7853981633974483,
};

// Returns the arctangent of `t`, for `t` in [0, 1]. It subtracts the nearest multiple of an eighth by the arctangent's
// difference formula, which leaves an argument of at most a sixteenth, and sums the series for that.
double atanUnit(double t) {
    uint32_t k = uint32_t(t * 8.0 + 0.5);
    double c = double(k) * 0.125;
    double u = (t - c) / (1.0 + t * c);
    double u2 = u * u;
    double series = 1.0 / 15.0;
    series = 1.0 / 13.0 - u2 * series;
    series = 1.0 / 11.0 - u2 * series;
    series = 1.0 / 9.0 - u2 * series;
    series = 1.0 / 7.0 - u2 * series;
    series = 1.0 / 5.0 - u2 * series;
    series = 1.0 / 3.0 - u2 * series;
    series = 1.0 - u2 * series;
    return kAtanEighths[k] + u * series;
}

} // namespace

PrStatus meshIndicesLoad(PrMeshIndexView view, uint32_t vertexCount, RawArray<uint32_t>* out) {
    if (meshIndexSize(view.type) == 0)
        return PrStatus_InvalidData;
    if (view.data == nullptr && view.count > vertexCount)
        return PrStatus_OutOfRange;
    uint32_t start = out->count;
    out->resize(start + view.count);
    for (uint32_t i = 0; i < view.count; ++i) {
        uint32_t index = meshIndex(view, i);
        if (index >= vertexCount)
            return PrStatus_OutOfRange;
        out->data[start + i] = index;
    }
    return PrStatus_Ok;
}

PrStatus meshTrianglesLoad(PrMeshIndexView view, uint32_t vertexCount, RawArray<uint32_t>* out) {
    if (view.count % 3 != 0)
        return PrStatus_InvalidData;
    return meshIndicesLoad(view, vertexCount, out);
}

double meshAcos(double x) {
    // acos(x) is twice the arctangent of sqrt((1 - x) / (1 + x)), whose argument is in [0, 1] for x in [0, 1].
    double magnitude = x < 0 ? -x : x;
    if (!(magnitude < 1.0))
        return x < 0 ? kPrF64Pi : 0.0;
    double angle = 2.0 * atanUnit(prF64Sqrt((1.0 - magnitude) / (1.0 + magnitude)));
    return x < 0 ? kPrF64Pi - angle : angle;
}

void meshTurnSinCos(double turns, double* sine, double* cosine) {
    // The nearest quarter turn is exact, and the series of the rest, of at most an eighth of a turn, converges to a
    // double's precision by its ninth term.
    double whole = turns - prF64Floor(turns);
    double quarters = prF64Floor(whole * 4.0 + 0.5);
    double r = (whole - quarters * 0.25) * (2.0 * kPrF64Pi);
    double r2 = r * r;
    double s = 1.0 / 6227020800.0;
    s = 1.0 / 39916800.0 - r2 * s;
    s = 1.0 / 362880.0 - r2 * s;
    s = 1.0 / 5040.0 - r2 * s;
    s = 1.0 / 120.0 - r2 * s;
    s = 1.0 / 6.0 - r2 * s;
    s = r - r * r2 * s;
    double c = 1.0 / 87178291200.0;
    c = 1.0 / 479001600.0 - r2 * c;
    c = 1.0 / 3628800.0 - r2 * c;
    c = 1.0 / 40320.0 - r2 * c;
    c = 1.0 / 720.0 - r2 * c;
    c = 1.0 / 24.0 - r2 * c;
    c = 0.5 - r2 * c;
    c = 1.0 - r2 * c;
    switch (uint32_t(quarters) % 4) {
        case 0:
            *sine = s;
            *cosine = c;
            break;
        case 1:
            *sine = c;
            *cosine = -s;
            break;
        case 2:
            *sine = -s;
            *cosine = -c;
            break;
        default:
            *sine = -c;
            *cosine = s;
            break;
    }
}

PrVec3 meshPerpendicular(PrVec3 normal) {
    // The axis that the normal leans toward least gives the best conditioned result.
    float ax = prF32Abs(normal.x);
    float ay = prF32Abs(normal.y);
    float az = prF32Abs(normal.z);
    PrVec3 axis{1, 0, 0};
    if (ay < ax && ay <= az)
        axis = PrVec3{0, 1, 0};
    else if (az < ax && az < ay)
        axis = PrVec3{0, 0, 1};
    float lengthSquared = prVec3Dot(normal, normal);
    if (!(lengthSquared > 0) || !prF32IsFinite(lengthSquared))
        return PrVec3{1, 0, 0};
    PrVec3 tangent = axis - normal * (prVec3Dot(normal, axis) / lengthSquared);
    float length = prVec3Length(tangent);
    if (!(length > 0) || !prF32IsFinite(length))
        return PrVec3{1, 0, 0};
    return tangent * (1.0f / length);
}

} // namespace pr_impl
