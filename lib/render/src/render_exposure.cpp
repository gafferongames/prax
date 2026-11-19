// The exposure of a renderer's views: a camera's EV100, and the meters that choose it.

#include "pr/render_exposure.h"

#include "pr/float.h"

float prRenderExposureFromEv100(float ev100) {
    return 1 / (1.2f * prF32Exp2(ev100));
}

float prRenderEv100FromIlluminance(float illuminance) {
    constexpr float kSpeed = 100;
    constexpr float kCalibration = 120 * kPrF32Pi;
    return prF32Log2(prF32Max(illuminance, 1e-6f) * kSpeed / kCalibration);
}

float prRenderEv100FromLuminance(float luminance) {
    constexpr float kSpeed = 100;
    constexpr float kCalibration = 12.5f;
    return prF32Log2(prF32Max(luminance, 1e-6f) * kSpeed / kCalibration);
}

float prRenderEv100Adapt(float ev100, float target, float seconds, float halfLife) {
    if (halfLife <= 0)
        return target;
    return target + (ev100 - target) * prF32Exp2(-prF32Max(seconds, 0) / halfLife);
}
