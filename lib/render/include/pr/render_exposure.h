// The exposure of a renderer's views (pr/render_view.h): a camera's exposure in EV100, and the meters that choose it,
// in the photometric units of the scene's light.

#pragma once

#include "pr/base.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// Exposure. A view's color is photometric: the luminance of each channel in candela per square meter (nits), of
// surfaces lit by lights in lux and candela (PrRenderLightDesc). A camera's exposure is its exposure value at ISO 100,
// EV100, which is log2(N^2 / t) for an f-number N and a shutter time t in seconds.
//
// Returns the factor that shows a view's color as a camera at `ev100` records it, for PrRenderDisplayOptions::exposure:
// the reciprocal of the luminance that saturates the camera's sensor, which ISO 12232's saturation-based speed puts at
// 78 / (S q) N^2 / t for a speed S of 100 and a lens's transmission and vignetting q of 0.65, so 1.2 * 2^EV100 cd/m2.
float prRenderExposureFromEv100(float ev100);

// Returns the EV100 that an incident-light meter with a flat receptor reads under `illuminance` lux: log2(E S / C) for
// a speed S of 100. Its calibration constant C is 120 pi lx s, within the range of 240 to 400 that ISO 2720 gives for a
// flat receptor, and the one at which a white surface without gloss that faces the meter, and is lit by what lights the
// meter, is exposed to saturation: prRenderExposureFromEv100 of the reading is pi / E, and shows that surface's
// luminance, E / pi, as diffuse white, a value of 1. An illuminance below a millionth of a lux reads as a millionth.
float prRenderEv100FromIlluminance(float illuminance);

// Returns the EV100 that a reflected-light meter reads for a surface of `luminance` nits: log2(L S / K) for a speed S
// of 100. Its calibration constant K is 12.5 cd s/m2, within the range of 10.6 to 13.4 that ISO 2720 gives, so
// prRenderExposureFromEv100 of the reading shows the luminance as 12.5 / 120, a value of about 0.104, which is how a
// meter that averages a scene places its average. A luminance below a millionth of a nit reads as a millionth.
float prRenderEv100FromLuminance(float luminance);

// Returns the EV100 that adapts from `ev100` toward `target` over `seconds`: the difference shrinks by half in each
// `halfLife` seconds, and a half-life that is not above zero reaches the target at once. The function reads no clock:
// the same steps give the same exposures.
float prRenderEv100Adapt(float ev100, float target, float seconds, float halfLife);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
