// Evaluating glTF animation channels.

#include "gltf/src/gltf_animation.h"

#include "pr/float.h"
#include "pr/mem.h"

#include "gltf/src/gltf_accessor.h"

namespace pr_impl {

namespace {

// The number of floats of a value that are interpolated at a time, which bounds the storage that a value of many morph
// target weights needs.
constexpr uint32_t kBatch = 64;

// How close to 1 the dot product of two rotations is before they are interpolated linearly instead of spherically
// (glTF-2.0/#interpolation-slerp).
constexpr float kSlerpEpsilon = 1e-6f;

// The values of a cubic spline keyframe, in the order that the output accessor holds them
// (glTF-2.0/#interpolation-cubic).
constexpr uint32_t kInTangent = 0;
constexpr uint32_t kValue = 1;
constexpr uint32_t kOutTangent = 2;

// The keyframes of one channel.
struct Keyframes {
    const Gltf* gltf;
    const PrGltfAccessor* input;
    const PrGltfAccessor* output;
    uint32_t width;             // the number of floats in a value
    uint32_t partCount;         // the values that a keyframe holds: 3 for a cubic spline, and otherwise 1
    bool scalars;               // true if a value is `width` scalar elements, and false if it is one element
};

float timeOf(const Keyframes& keyframes, uint32_t keyframe) {
    float time = 0;
    gltfAccessorRead(keyframes.gltf, *keyframes.input, keyframe, 1, GltfNumber_Float, &time);
    return time;
}

// Reads `count` floats, from float `first`, of value `part` of a keyframe.
void readValue(const Keyframes& keyframes, uint32_t keyframe, uint32_t part, uint32_t first, uint32_t count, float* out) {
    uint32_t value = keyframe * keyframes.partCount + part;
    if (keyframes.scalars)
        gltfAccessorRead(keyframes.gltf, *keyframes.output, value * keyframes.width + first, count, GltfNumber_Float, out);
    else
        gltfAccessorRead(keyframes.gltf, *keyframes.output, value, 1, GltfNumber_Float, out);
}

// Scales a quaternion to unit length. A quaternion of no length becomes the identity.
void normalize(float* quaternion) {
    float length = prF32Sqrt(quaternion[0] * quaternion[0] + quaternion[1] * quaternion[1] +
        quaternion[2] * quaternion[2] + quaternion[3] * quaternion[3]);
    if (!(length > 0) || !prF32IsFinite(length)) {
        quaternion[0] = quaternion[1] = quaternion[2] = 0;
        quaternion[3] = 1;
        return;
    }
    for (uint32_t i = 0; i < 4; ++i)
        quaternion[i] /= length;
}

// Writes the rotation `t` of the way from `a` to `b` along the shorter arc (glTF-2.0/#interpolation-slerp).
void slerp(const float* a, const float* b, float t, float* out) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float sign = 1;
    if (dot < 0) {
        dot = -dot;
        sign = -1;
    }
    dot = prF32Min(dot, 1.0f);
    float fromWeight = 1 - t;
    float toWeight = t;
    if (1 - dot > kSlerpEpsilon) {
        float angle = prF32Acos(dot);
        float sine = prF32Sin(angle);
        fromWeight = prF32Sin(angle * (1 - t)) / sine;
        toWeight = prF32Sin(angle * t) / sine;
    }
    for (uint32_t i = 0; i < 4; ++i)
        out[i] = fromWeight * a[i] + toWeight * sign * b[i];
}

// Writes floats `first` to `first + count` of the value between keyframes `from` and `from + 1`, `t` of the way along a
// segment of `duration` seconds.
void interpolate(const Keyframes& keyframes, const PrGltfAnimationChannel& channel,
        PrGltfInterpolation interpolation, uint32_t from, float t, float duration, uint32_t first, uint32_t count,
        float* out) {
    float a[kBatch];
    float b[kBatch];
    if (interpolation != PrGltfInterpolation_CubicSpline) {
        readValue(keyframes, from, 0, first, count, a);
        readValue(keyframes, from + 1, 0, first, count, b);
        if (channel.path == PrGltfPath_Rotation) {
            slerp(a, b, t, out);
            return;
        }
        for (uint32_t i = 0; i < count; ++i)
            out[i] = (1 - t) * a[i] + t * b[i];
        return;
    }

    // The Hermite spline through the two values, with the out-tangent of the first and the in-tangent of the second.
    float outTangent[kBatch];
    float inTangent[kBatch];
    readValue(keyframes, from, kValue, first, count, a);
    readValue(keyframes, from, kOutTangent, first, count, outTangent);
    readValue(keyframes, from + 1, kValue, first, count, b);
    readValue(keyframes, from + 1, kInTangent, first, count, inTangent);
    float t2 = t * t;
    float t3 = t2 * t;
    for (uint32_t i = 0; i < count; ++i) {
        out[i] = (2 * t3 - 3 * t2 + 1) * a[i] + (t3 - 2 * t2 + t) * outTangent[i] * duration +
            (-2 * t3 + 3 * t2) * b[i] + (t3 - t2) * inTangent[i] * duration;
    }
}

} // namespace

bool gltfAnimationChannelEvaluate(const Gltf* gltf, const PrGltfAnimationChannel& channel, float seconds, float* out) {
    if (channel.width == 0)
        return true;
    const PrGltfAnimationSampler& sampler = gltf->animationSamplers.data[channel.sampler];
    Keyframes keyframes = {};
    keyframes.gltf = gltf;
    keyframes.input = &gltf->accessors.data[sampler.input];
    keyframes.output = &gltf->accessors.data[sampler.output];
    keyframes.width = channel.width;
    keyframes.partCount = sampler.interpolation == PrGltfInterpolation_CubicSpline ? 3 : 1;
    keyframes.scalars = channel.path == PrGltfPath_Weights || channel.path == PrGltfPath_MeshWeights;
    if (!gltfAccessorHasBytes(gltf, *keyframes.input) || !gltfAccessorHasBytes(gltf, *keyframes.output)) {
        prMemZero(out, size_t(channel.width) * sizeof(float));
        return false;
    }

    // Find the last keyframe at or before `seconds`. Outside the keyframes, the value is that of the nearest one. A
    // time that is not a number reads as before the first keyframe.
    uint32_t last = keyframes.input->count - 1;
    uint32_t from = 0;
    float fromTime = timeOf(keyframes, 0);
    float toTime = fromTime;
    bool between = false;
    if (last != 0 && seconds > fromTime) {
        toTime = timeOf(keyframes, last);
        if (seconds >= toTime) {
            from = last;
        } else {
            uint32_t to = last;
            while (to - from > 1) {
                uint32_t middle = from + (to - from) / 2;
                float time = timeOf(keyframes, middle);
                if (time <= seconds) {
                    from = middle;
                    fromTime = time;
                } else {
                    to = middle;
                    toTime = time;
                }
            }
            between = seconds > fromTime && sampler.interpolation != PrGltfInterpolation_Step;
        }
    }

    uint32_t valuePart = sampler.interpolation == PrGltfInterpolation_CubicSpline ? kValue : 0;
    for (uint32_t first = 0; first < channel.width; first += kBatch) {
        uint32_t count = channel.width - first < kBatch ? channel.width - first : kBatch;
        if (!between) {
            readValue(keyframes, from, valuePart, first, count, out + first);
            continue;
        }
        float duration = toTime - fromTime;
        interpolate(keyframes, channel, sampler.interpolation, from, (seconds - fromTime) / duration, duration, first,
            count, out + first);
    }
    if (channel.path == PrGltfPath_Rotation)
        normalize(out);
    return true;
}

} // namespace pr_impl
