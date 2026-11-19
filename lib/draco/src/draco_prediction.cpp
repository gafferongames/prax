// Undoing the prediction of an attribute's integers: the prediction schemes and their transforms.
//
// A value is coded as its correction from a prediction, which a scheme makes from the values already decoded and a
// transform combines with the correction. The bitstream specification describes the schemes in its sections Prediction
// Decoder to Normal Prediction Decoder. The arithmetic is the reference decoder's: 32-bit integers that wrap, and
// 64-bit integers for the predictions of texture coordinates and of normals.

#include "draco/src/draco_prediction.h"

namespace pr_impl {

namespace {

// The prediction schemes (the bitstream's seq_att_dec_prediction_scheme). The multi-parallelogram scheme and the first
// scheme for texture coordinates are from versions of the bitstream before the specification's, which the reference
// decoder still reads in a stream of the specification's version.
enum {
    Scheme_Difference = 0,
    Scheme_Parallelogram = 1,
    Scheme_MultiParallelogram = 2,
    Scheme_TexCoordsDeprecated = 3,
    Scheme_ConstrainedMultiParallelogram = 4,
    Scheme_TexCoordsPortable = 5,
    Scheme_GeometricNormal = 6,
    Scheme_Count,
};
typedef uint8_t Scheme;

// The transforms (the bitstream's seq_att_dec_prediction_transform_type).
enum {
    TransformType_Wrap = 1,
    TransformType_Octahedron = 2,
    TransformType_OctahedronCanonicalized = 3,
};
typedef uint8_t TransformType;

// The most parallelograms that the constrained multi-parallelogram scheme averages.
constexpr uint32_t kMaxParallelograms = 4;

// How a prediction and a correction give a value.
struct Transform {
    TransformType type;
    uint32_t components;

    // Wrap: the bounds of the values, and the number of values between them.
    int32_t minimum;
    int32_t maximum;
    int32_t span;

    // Octahedron: the greatest coordinate plus one, and the coordinate of the octahedron's center.
    int32_t maxQuantized;
    int32_t center;
};

inline int32_t addWrapped(int32_t a, int32_t b) {
    return int32_t(uint32_t(a) + uint32_t(b));
}

inline uint32_t magnitude(int32_t value) {
    return value < 0 ? 0u - uint32_t(value) : uint32_t(value);
}

// Reads a transform's parameters (the bitstream's DecodeTransformData).
bool readTransform(DracoReader* reader, Transform* transform) {
    uint32_t first = 0;
    if (!dracoReadU32(reader, &first))
        return false;
    if (transform->type == TransformType_Wrap) {
        uint32_t second = 0;
        if (!dracoReadU32(reader, &second))
            return false;
        transform->minimum = int32_t(first);
        transform->maximum = int32_t(second);
        int64_t difference = int64_t(transform->maximum) - int64_t(transform->minimum);
        if (difference < 0 || difference >= 0x7fffffff)
            return false;
        transform->span = int32_t(difference) + 1;
        return true;
    }
    // The canonicalized transform also stores the center, which the greatest coordinate gives.
    uint32_t center = 0;
    if (transform->type == TransformType_OctahedronCanonicalized && !dracoReadU32(reader, &center))
        return false;
    // The greatest coordinate is 2^bits - 1 for 2 to 30 bits, of which only the number of bits is taken.
    if (int32_t(first) < 0 || first % 2 == 0)
        return false;
    uint32_t bits = 0;
    while (first >> bits != 0)
        ++bits;
    if (bits < 2 || bits > 30)
        return false;
    transform->maxQuantized = int32_t((1u << bits) - 1);
    transform->center = (transform->maxQuantized - 1) / 2;
    return true;
}

// Reflects a point outside the diamond |s| + |t| <= center into it, and back (the bitstream's InvertDiamond).
inline void invertDiamond(const Transform& transform, int32_t* s, int32_t* t) {
    int32_t signS = 0;
    int32_t signT = 0;
    if (*s >= 0 && *t >= 0) {
        signS = 1;
        signT = 1;
    } else if (*s <= 0 && *t <= 0) {
        signS = -1;
        signT = -1;
    } else {
        signS = *s > 0 ? 1 : -1;
        signT = *t > 0 ? 1 : -1;
    }
    uint32_t cornerS = uint32_t(signS * transform.center);
    uint32_t cornerT = uint32_t(signT * transform.center);
    uint32_t us = uint32_t(*s) * 2 - cornerS;
    uint32_t ut = uint32_t(*t) * 2 - cornerT;
    if (signS * signT >= 0) {
        uint32_t temp = us;
        us = 0u - ut;
        ut = 0u - temp;
    } else {
        uint32_t temp = us;
        us = ut;
        ut = temp;
    }
    *s = int32_t(us + cornerS) / 2;
    *t = int32_t(ut + cornerT) / 2;
}

inline int32_t modMax(const Transform& transform, int32_t value) {
    if (value > transform.center)
        return value - transform.maxQuantized;
    if (value < -transform.center)
        return value + transform.maxQuantized;
    return value;
}

// Turns a point by a number of quarter turns.
inline void rotate(int32_t* s, int32_t* t, int32_t count) {
    int32_t x = *s;
    int32_t y = *t;
    int32_t negX = int32_t(0u - uint32_t(x));
    int32_t negY = int32_t(0u - uint32_t(y));
    switch (count) {
        case 1:
            *s = y;
            *t = negX;
            break;
        case 2:
            *s = negX;
            *t = negY;
            break;
        case 3:
            *s = negY;
            *t = x;
            break;
        default:
            break;
    }
}

// Gives the value that `correction` makes of `prediction`, each of the transform's components.
inline void applyTransform(const Transform& transform, const int32_t* prediction, const int32_t* correction, int32_t* out) {
    if (transform.type == TransformType_Wrap) {
        // The prediction is clamped to the bounds, and a sum outside them wraps around.
        for (uint32_t c = 0; c < transform.components; ++c) {
            int32_t clamped = prediction[c];
            if (clamped > transform.maximum)
                clamped = transform.maximum;
            else if (clamped < transform.minimum)
                clamped = transform.minimum;
            int32_t value = addWrapped(clamped, correction[c]);
            if (value > transform.maximum)
                value -= transform.span;
            else if (value < transform.minimum)
                value += transform.span;
            out[c] = value;
        }
        return;
    }
    // Octahedral coordinates, about the center. A prediction outside the diamond is brought into it, and for the
    // canonicalized transform turned into the bottom left quadrant, and the value is taken back the same way.
    int32_t s = int32_t(uint32_t(prediction[0]) - uint32_t(transform.center));
    int32_t t = int32_t(uint32_t(prediction[1]) - uint32_t(transform.center));
    bool inDiamond = magnitude(s) + magnitude(t) <= uint32_t(transform.center);
    if (!inDiamond)
        invertDiamond(transform, &s, &t);
    bool turned = false;
    int32_t turns = 0;
    if (transform.type == TransformType_OctahedronCanonicalized) {
        turned = !((s == 0 && t == 0) || (s < 0 && t <= 0));
        if (s == 0)
            turns = t == 0 ? 0 : t > 0 ? 3 : 1;
        else if (s > 0)
            turns = t >= 0 ? 2 : 1;
        else
            turns = t <= 0 ? 0 : 3;
        if (turned)
            rotate(&s, &t, turns);
    }
    s = modMax(transform, addWrapped(s, correction[0]));
    t = modMax(transform, addWrapped(t, correction[1]));
    if (turned)
        rotate(&s, &t, (4 - turns) % 4);
    if (!inDiamond)
        invertDiamond(transform, &s, &t);
    out[0] = addWrapped(s, transform.center);
    out[1] = addWrapped(t, transform.center);
}

// ---------------------------------------------------------------------------------------------------------------------
// Schemes

// What a scheme reads: the attribute's sequence, and for the schemes that predict from positions, the positions.
struct Prediction {
    const DracoSequence* sequence;
    Transform transform;
    uint32_t components;
    const int32_t* PR_NULLABLE positions;       // by value of the position attribute: three integers
    const uint32_t* PR_NULLABLE positionValue;  // by point: its value of the position attribute, or null for itself
};

// Each value from the one before, and the first from zero (the bitstream's
// PredictionSchemeDifference_ComputeOriginalValues).
void decodeDifference(const Prediction& prediction, int32_t* values) {
    uint32_t components = prediction.components;
    int32_t zero[2] = {0, 0};
    RawArray<int32_t> zeros;
    const int32_t* previous = zero;
    if (components > 2) {
        zeros.resizeZeroed(components);
        previous = zeros.data;
    }
    uint32_t count = prediction.sequence->valueCount;
    for (uint32_t i = 0; i < count; ++i) {
        int32_t* value = values + size_t(i) * components;
        applyTransform(prediction.transform, previous, value, value);
        previous = value;
    }
    zeros.reset();
}

// Predicts value `id`, at `corner`, as the fourth vertex of the parallelogram that the face across from the corner
// makes with it, if that face's three values are decoded (the bitstream's ComputeParallelogramPrediction).
inline bool predictParallelogram(const Prediction& prediction, uint32_t id, uint32_t corner, const int32_t* values,
        int32_t* out) {
    const DracoConnectivity& connectivity = prediction.sequence->connectivity;
    uint32_t across = connectivity.opposite(corner);
    if (across == kDracoInvalid)
        return false;
    const uint32_t* vertexValue = prediction.sequence->vertexValue.data;
    uint32_t tip = vertexValue[connectivity.vertex(across)];
    uint32_t next = vertexValue[connectivity.vertex(DracoConnectivity::next(across))];
    uint32_t previous = vertexValue[connectivity.vertex(DracoConnectivity::previous(across))];
    if (tip >= id || next >= id || previous >= id)
        return false;
    uint32_t components = prediction.components;
    const int32_t* tipValue = values + size_t(tip) * components;
    const int32_t* nextValue = values + size_t(next) * components;
    const int32_t* previousValue = values + size_t(previous) * components;
    for (uint32_t c = 0; c < components; ++c)
        out[c] = int32_t(int64_t(nextValue[c]) + int64_t(previousValue[c]) - int64_t(tipValue[c]));
    return true;
}

// The parallelogram schemes: one parallelogram, the mean of all of them around the vertex, or the mean of those that a
// flag for each does not call a crease (the bitstream's MeshPredictionSchemeParallelogramDecoder_ComputeOriginalValues
// and MeshPredictionSchemeConstrainedMultiParallelogramDecoder_ComputeOriginalValues).
bool decodeParallelogram(const Prediction& prediction, int scheme, const RawArray<uint8_t>* creases, int32_t* values) {
    const DracoConnectivity& connectivity = prediction.sequence->connectivity;
    uint32_t components = prediction.components;
    uint32_t count = prediction.sequence->valueCount;
    // The predictions of up to four parallelograms, and then their mean.
    RawArray<int32_t> scratch;
    scratch.resizeZeroed(components * (kMaxParallelograms + 1));
    int32_t* mean = scratch.data + size_t(kMaxParallelograms) * components;
    uint32_t creaseAt[kMaxParallelograms] = {};
    bool ok = true;
    applyTransform(prediction.transform, scratch.data, values, values);
    for (uint32_t p = 1; p < count && ok; ++p) {
        uint32_t start = prediction.sequence->valueCorner.data[p];
        int32_t* value = values + size_t(p) * components;
        const int32_t* previous = value - components;
        if (scheme == Scheme_Parallelogram) {
            bool predicted = predictParallelogram(prediction, p, start, values, mean);
            applyTransform(prediction.transform, predicted ? mean : previous, value, value);
            continue;
        }
        uint32_t used = 0;
        for (uint32_t c = 0; c < components; ++c)
            mean[c] = 0;
        if (scheme == Scheme_MultiParallelogram) {
            // Every parallelogram to the right around the vertex, to a boundary or to the start.
            for (uint32_t corner = start; corner != kDracoInvalid;) {
                if (predictParallelogram(prediction, p, corner, values, scratch.data)) {
                    for (uint32_t c = 0; c < components; ++c)
                        mean[c] = addWrapped(mean[c], scratch.data[c]);
                    ++used;
                }
                corner = connectivity.swingRight(corner);
                if (corner == start)
                    break;
            }
        } else {
            // Up to four parallelograms, to the left around the vertex and then, from a boundary, to the right.
            uint32_t found = 0;
            bool leftward = true;
            for (uint32_t corner = start; corner != kDracoInvalid;) {
                if (predictParallelogram(prediction, p, corner, values, scratch.data + size_t(found) * components) &&
                        ++found == kMaxParallelograms) {
                    break;
                }
                corner = leftward ? connectivity.swingLeft(corner) : connectivity.swingRight(corner);
                if (corner == start)
                    break;
                if (corner == kDracoInvalid && leftward) {
                    leftward = false;
                    corner = connectivity.swingRight(start);
                }
            }
            // The flags are in an array for each number of parallelograms found, in order.
            for (uint32_t i = 0; i < found; ++i) {
                const RawArray<uint8_t>& flags = creases[found - 1];
                uint32_t at = creaseAt[found - 1]++;
                if (at >= flags.count) {
                    ok = false;
                    break;
                }
                if (flags.data[at] != 0)
                    continue;
                ++used;
                for (uint32_t c = 0; c < components; ++c)
                    mean[c] = addWrapped(mean[c], scratch.data[size_t(i) * components + c]);
            }
        }
        if (used == 0) {
            applyTransform(prediction.transform, previous, value, value);
        } else {
            for (uint32_t c = 0; c < components; ++c)
                mean[c] /= int32_t(used);
            applyTransform(prediction.transform, mean, value, value);
        }
    }
    scratch.reset();
    return ok;
}

// Returns the integers of the position of the point that has `value` of the attribute being decoded.
inline const int32_t* positionOf(const Prediction& prediction, uint32_t value) {
    uint32_t point = prediction.sequence->valuePoint.data[value];
    uint32_t position = prediction.positionValue != nullptr ? prediction.positionValue[point] : point;
    return prediction.positions + size_t(position) * 3;
}

// The integer square root that the reference decoder computes.
uint64_t integerSqrt(uint64_t number) {
    if (number == 0)
        return 0;
    uint64_t remaining = number;
    uint64_t root = 1;
    while (remaining >= 2) {
        root *= 2;
        remaining /= 4;
    }
    do {
        root = (root + number / root) / 2;
    } while (root * root > number);
    return root;
}

inline uint64_t magnitude64(int64_t value) {
    return value < 0 ? 0u - uint64_t(value) : uint64_t(value);
}

inline uint64_t max64(uint64_t a, uint64_t b) {
    return a > b ? a : b;
}

// Divides as the reference decoder's signed division does, where it is defined.
inline bool divide(int64_t numerator, int64_t denominator, int64_t* out) {
    if (denominator == 0 || (denominator == -1 && numerator == INT64_MIN))
        return false;
    *out = numerator / denominator;
    return true;
}

// Predicts the texture coordinate of value `id`, at `corner`, from those of the two other vertices of the corner's
// face, by placing it over their edge as the position is placed over the edge's positions. Which side of the edge it is
// on is a bit of `orientations`, read from the last (the bitstream's
// MeshPredictionSchemeTexCoordsPortablePredictor_ComputePredictedValue). The products wrap as two's complement, and a
// stream whose integers overflow what the reference decoder checks fails.
bool predictTexCoord(const Prediction& prediction, uint32_t id, uint32_t corner, const int32_t* values,
        RawArray<uint8_t>* orientations, int32_t* out) {
    const DracoConnectivity& connectivity = prediction.sequence->connectivity;
    const uint32_t* vertexValue = prediction.sequence->vertexValue.data;
    uint32_t next = vertexValue[connectivity.vertex(DracoConnectivity::next(corner))];
    uint32_t previous = vertexValue[connectivity.vertex(DracoConnectivity::previous(corner))];
    if (previous < id && next < id) {
        int64_t nextUv[2] = {values[size_t(next) * 2], values[size_t(next) * 2 + 1]};
        int64_t previousUv[2] = {values[size_t(previous) * 2], values[size_t(previous) * 2 + 1]};
        if (previousUv[0] == nextUv[0] && previousUv[1] == nextUv[1]) {
            out[0] = int32_t(previousUv[0]);
            out[1] = int32_t(previousUv[1]);
            return true;
        }
        const int32_t* tipPosition = positionOf(prediction, id);
        const int32_t* nextPosition = positionOf(prediction, next);
        const int32_t* previousPosition = positionOf(prediction, previous);
        // The edge, and the tip from the edge's start. Differences and products of 32-bit integers fit in 64 bits.
        int64_t edge[3];
        int64_t tip[3];
        uint64_t edgeSquared = 0;
        int64_t tipDotEdge = 0;
        int64_t edgeMax = 0;
        for (uint32_t i = 0; i < 3; ++i) {
            edge[i] = int64_t(previousPosition[i]) - int64_t(nextPosition[i]);
            tip[i] = int64_t(tipPosition[i]) - int64_t(nextPosition[i]);
            edgeSquared += uint64_t(edge[i]) * uint64_t(edge[i]);
            tipDotEdge = int64_t(uint64_t(tipDotEdge) + uint64_t(edge[i]) * uint64_t(tip[i]));
            int64_t size = int64_t(magnitude64(edge[i]));
            edgeMax = size > edgeMax ? size : edgeMax;
        }
        if (edgeSquared != 0) {
            int64_t edgeUv[2] = {previousUv[0] - nextUv[0], previousUv[1] - nextUv[1]};
            int64_t nextUvMax = int64_t(max64(magnitude64(nextUv[0]), magnitude64(nextUv[1])));
            int64_t edgeUvMax = int64_t(max64(magnitude64(edgeUv[0]), magnitude64(edgeUv[1])));
            uint64_t dotSize = magnitude64(tipDotEdge);
            // The reference decoder's checks that the products below fit.
            if (uint64_t(nextUvMax) > uint64_t(INT64_MAX) / edgeSquared || dotSize > uint64_t(INT64_MAX / edgeUvMax) ||
                    dotSize > uint64_t(INT64_MAX / edgeMax)) {
                return false;
            }
            // The foot of the tip on the edge, in texture coordinates scaled by the edge's squared length, and in
            // positions.
            uint64_t footUv[2];
            int64_t offset[3];
            uint64_t offsetSquared = 0;
            for (uint32_t i = 0; i < 2; ++i)
                footUv[i] = uint64_t(nextUv[i]) * edgeSquared + uint64_t(tipDotEdge) * uint64_t(edgeUv[i]);
            for (uint32_t i = 0; i < 3; ++i) {
                int64_t along = 0;
                if (!divide(int64_t(uint64_t(tipDotEdge) * uint64_t(edge[i])), int64_t(edgeSquared), &along))
                    return false;
                offset[i] = int64_t(uint64_t(tipPosition[i]) - (uint64_t(nextPosition[i]) + uint64_t(along)));
                offsetSquared += uint64_t(offset[i]) * uint64_t(offset[i]);
            }
            // The offset from the foot is the edge turned a quarter, as long as the tip is far from the edge.
            uint64_t length = integerSqrt(offsetSquared * edgeSquared);
            uint64_t turned[2] = {uint64_t(edgeUv[1]) * length, (0u - uint64_t(edgeUv[0])) * length};
            if (orientations->count == 0)
                return false;
            bool orientation = orientations->last() != 0;
            orientations->pop();
            for (uint32_t i = 0; i < 2; ++i) {
                int64_t predicted = 0;
                uint64_t sum = orientation ? footUv[i] + turned[i] : footUv[i] - turned[i];
                if (!divide(int64_t(sum), int64_t(edgeSquared), &predicted))
                    return false;
                out[i] = int32_t(predicted);
            }
            return true;
        }
    }
    // Without both neighbours and an edge between them, the value is predicted as one of them, or as the value before.
    const int32_t* source = nullptr;
    if (next < id)
        source = values + size_t(next) * 2;
    else if (id > 0)
        source = values + size_t(id - 1) * 2;
    out[0] = source != nullptr ? source[0] : 0;
    out[1] = source != nullptr ? source[1] : 0;
    return true;
}

// Predicts a normal as the sum of the cross products of the edges of the faces around the vertex of `corner`, which
// weights each face's normal by its area (the bitstream's
// MeshPredictionSchemeGeometricNormalPredictorArea_ComputePredictedValue).
void predictNormal(const Prediction& prediction, uint32_t corner, int32_t* out) {
    const DracoConnectivity& connectivity = prediction.sequence->connectivity;
    const uint32_t* vertexValue = prediction.sequence->vertexValue.data;
    auto positionAt = [&](uint32_t at) { return positionOf(prediction, vertexValue[connectivity.vertex(at)]); };
    const int32_t* center = positionAt(corner);
    uint64_t normal[3] = {0, 0, 0};
    bool leftward = true;
    for (uint32_t at = corner; at != kDracoInvalid;) {
        const int32_t* nextPosition = positionAt(DracoConnectivity::next(at));
        const int32_t* previousPosition = positionAt(DracoConnectivity::previous(at));
        int64_t a[3];
        int64_t b[3];
        for (uint32_t i = 0; i < 3; ++i) {
            a[i] = int64_t(nextPosition[i]) - int64_t(center[i]);
            b[i] = int64_t(previousPosition[i]) - int64_t(center[i]);
        }
        // The differences are of 33 bits, so a product is of 66: it wraps, as the reference decoder's does.
        normal[0] += uint64_t(a[1]) * uint64_t(b[2]) - uint64_t(a[2]) * uint64_t(b[1]);
        normal[1] += uint64_t(a[2]) * uint64_t(b[0]) - uint64_t(a[0]) * uint64_t(b[2]);
        normal[2] += uint64_t(a[0]) * uint64_t(b[1]) - uint64_t(a[1]) * uint64_t(b[0]);
        // Every corner of the vertex: to the left to a boundary or to the start, and then from the start to the right.
        if (leftward) {
            at = connectivity.swingLeft(at);
            if (at == kDracoInvalid) {
                at = connectivity.swingRight(corner);
                leftward = false;
            } else if (at == corner) {
                break;
            }
        } else {
            at = connectivity.swingRight(at);
        }
    }
    // The sum is scaled down until its components fit in 32 bits with room to spare.
    constexpr int64_t kUpperBound = 1 << 29;
    int64_t sum = 0;
    for (uint32_t i = 0; i < 3; ++i) {
        uint64_t size = magnitude64(int64_t(normal[i]));
        sum = size > uint64_t(INT64_MAX - sum) ? INT64_MAX : sum + int64_t(size);
    }
    for (uint32_t i = 0; i < 3; ++i) {
        int64_t value = int64_t(normal[i]);
        if (sum > kUpperBound)
            value /= sum / kUpperBound;
        out[i] = int32_t(value);
    }
}

// Scales an integer vector so that the sum of its components' sizes is the transform's center, and gives its octahedral
// coordinates (the bitstream's CanonicalizeIntegerVector and IntegerVectorToQuantizedOctahedralCoords).
void normalToOctahedral(const Transform& transform, int32_t* vector, bool flip, int32_t* out) {
    int64_t center = transform.center;
    int64_t sum = int64_t(magnitude(vector[0])) + int64_t(magnitude(vector[1])) + int64_t(magnitude(vector[2]));
    if (sum == 0) {
        vector[0] = transform.center;
    } else {
        vector[0] = int32_t(int64_t(vector[0]) * center / sum);
        vector[1] = int32_t(int64_t(vector[1]) * center / sum);
        int32_t rest = int32_t(center - int64_t(magnitude(vector[0])) - int64_t(magnitude(vector[1])));
        vector[2] = vector[2] >= 0 ? rest : -rest;
    }
    if (flip) {
        for (uint32_t i = 0; i < 3; ++i)
            vector[i] = -vector[i];
    }
    int32_t maxValue = transform.maxQuantized - 1;
    int32_t s = 0;
    int32_t t = 0;
    if (vector[0] >= 0) {
        s = vector[1] + transform.center;
        t = vector[2] + transform.center;
    } else {
        s = vector[1] < 0 ? int32_t(magnitude(vector[2])) : maxValue - int32_t(magnitude(vector[2]));
        t = vector[2] < 0 ? int32_t(magnitude(vector[1])) : maxValue - int32_t(magnitude(vector[1]));
    }
    // The points on the octahedron's seam have two coordinates each, of which one is kept.
    if ((s == 0 && t == 0) || (s == 0 && t == maxValue) || (s == maxValue && t == 0)) {
        s = maxValue;
        t = maxValue;
    } else if (s == 0 && t > transform.center) {
        t = transform.center - (t - transform.center);
    } else if (s == maxValue && t < transform.center) {
        t = transform.center + (transform.center - t);
    } else if (t == maxValue && s < transform.center) {
        s = transform.center + (transform.center - s);
    } else if (t == 0 && s > transform.center) {
        s = transform.center - (s - transform.center);
    }
    out[0] = s;
    out[1] = t;
}

// Reads `count` flags coded with rANS, or none for a count of zero.
bool readFlags(DracoReader* reader, uint32_t count, RawArray<uint8_t>* out) {
    DracoAnsBits bits = {};
    if (!dracoAnsBitsStart(reader, &bits))
        return false;
    out->resize(count);
    for (uint32_t i = 0; i < count; ++i)
        out->data[i] = dracoAnsBit(&bits) ? 1 : 0;
    return true;
}

} // namespace

bool dracoDecodePrediction(Draco* draco, DracoAttributeState* attribute, int method, int transform, int32_t* values) {
    DracoReader* reader = &draco->reader;
    const DracoSequence& sequence = draco->sequences.data[attribute->sequence];
    if (method < Scheme_Difference || method >= Scheme_Count)
        return false;
    bool normals = attribute->coding == DracoCoding_Normals;
    bool octahedral = transform == TransformType_Octahedron || transform == TransformType_OctahedronCanonicalized;
    if (normals ? !octahedral : transform != TransformType_Wrap)
        return false;
    if (method == Scheme_TexCoordsDeprecated)
        return dracoFail(draco, PrStatus_NotSupported);

    // A scheme that reads the mesh is the difference where there is no mesh to read, and so is every scheme but the
    // geometric one for a normal, as the reference decoder has it. Only a normal is predicted geometrically.
    int scheme = method;
    bool hasMesh = sequence.connectivity.opposites != nullptr;
    if (!hasMesh || (normals && method != Scheme_GeometricNormal))
        scheme = Scheme_Difference;
    else if (!normals && method == Scheme_GeometricNormal)
        return false;

    Prediction prediction = {};
    prediction.sequence = &sequence;
    prediction.components = attribute->portableComponents;
    prediction.transform.type = TransformType(transform);
    prediction.transform.components = attribute->portableComponents;
    if (scheme == Scheme_TexCoordsPortable || scheme == Scheme_GeometricNormal) {
        // The positions are those of the first position attribute, which is decoded already and has three integers.
        const DracoAttributeState* positions = nullptr;
        for (uint32_t i = 0; i < draco->attributes.count && positions == nullptr; ++i) {
            if (draco->attributes.data[i].info.kind == PrDracoAttributeKind_Position)
                positions = &draco->attributes.data[i];
        }
        if (positions == nullptr || positions == attribute || !positions->hasPortable || positions->portableComponents != 3) {
            return false;
        }
        const DracoSequence& positionSequence = draco->sequences.data[positions->sequence];
        prediction.positions = positions->portable.data;
        prediction.positionValue = positionSequence.pointValue.count != 0 ? positionSequence.pointValue.data : nullptr;
        if (scheme == Scheme_TexCoordsPortable && prediction.components != 2)
            return false;
    }

    uint32_t count = sequence.valueCount;
    RawArray<uint8_t> flags[kMaxParallelograms];
    bool ok = true;
    switch (scheme) {
        case Scheme_Difference:
        case Scheme_Parallelogram:
        case Scheme_MultiParallelogram:
            ok = readTransform(reader, &prediction.transform);
            if (ok && count != 0 && scheme == Scheme_Difference)
                decodeDifference(prediction, values);
            else if (ok && count != 0)
                ok = decodeParallelogram(prediction, scheme, nullptr, values);
            break;
        case Scheme_ConstrainedMultiParallelogram: {
            // The crease flags, an array for each number of parallelograms that a vertex can have.
            for (uint32_t i = 0; i < kMaxParallelograms && ok; ++i) {
                uint32_t flagCount = 0;
                ok = dracoReadVarint32(reader, &flagCount) && flagCount <= draco->table.faceCount * 3;
                if (ok && flagCount != 0)
                    ok = readFlags(reader, flagCount, &flags[i]);
            }
            ok = ok && readTransform(reader, &prediction.transform);
            if (ok && count != 0)
                ok = decodeParallelogram(prediction, scheme, flags, values);
            break;
        }
        case Scheme_TexCoordsPortable: {
            // The orientations, each coded as whether it is the one before.
            uint32_t orientationCount = 0;
            ok = dracoReadU32(reader, &orientationCount) && orientationCount <= count;
            if (ok)
                ok = readFlags(reader, orientationCount, &flags[0]);
            uint8_t last = 1;
            for (uint32_t i = 0; ok && i < orientationCount; ++i) {
                if (flags[0].data[i] == 0)
                    last = last != 0 ? 0 : 1;
                flags[0].data[i] = last;
            }
            ok = ok && readTransform(reader, &prediction.transform);
            for (uint32_t p = 0; ok && p < count; ++p) {
                int32_t predicted[2] = {0, 0};
                int32_t* value = values + size_t(p) * 2;
                ok = predictTexCoord(prediction, p, sequence.valueCorner.data[p], values, &flags[0], predicted);
                if (ok)
                    applyTransform(prediction.transform, predicted, value, value);
            }
            break;
        }
        default: {
            // A bit for each normal says whether the prediction is turned around.
            DracoAnsBits flips = {};
            ok = readTransform(reader, &prediction.transform) && dracoAnsBitsStart(reader, &flips);
            for (uint32_t p = 0; ok && p < count; ++p) {
                int32_t normal[3] = {0, 0, 0};
                int32_t predicted[2] = {0, 0};
                int32_t* value = values + size_t(p) * 2;
                predictNormal(prediction, sequence.valueCorner.data[p], normal);
                normalToOctahedral(prediction.transform, normal, dracoAnsBit(&flips), predicted);
                applyTransform(prediction.transform, predicted, value, value);
            }
            break;
        }
    }
    for (RawArray<uint8_t>& array : flags)
        array.reset();
    return ok;
}

} // namespace pr_impl
