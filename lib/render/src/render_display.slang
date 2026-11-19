// lib/render's display: a draw over a whole target that shows a view's color, tone mapped for the display's headroom
// and encoded as extended sRGB.

#include "pr/gpu.slang"
#include "pr/render_records.h"
#include "pr/render_whole.slang"

PR_GPU_PUSH(PrRenderDisplayPush, push);

#if defined(PR_RENDER_SLOTS)

PR_GPU_TEXTURE(Texture2D<float4>, renderSlotColor, 0);

// Returns the view's color at a texel, as scene-referred linear sRGB.
float3 viewColor(int2 texel) {
    return renderSlotColor.Load(int3(texel, 0)).rgb;
}

#else

float3 viewColor(int2 texel) {
    return prGpuGet<Texture2D<float4>>(push.color).Load(int3(texel, 0)).rgb;
}

#endif

// A tone map has two parts. Its curve maps scene-referred values into [0, 1] for a display without headroom. On a
// display with headroom, the part of the curve above diffuse white, a scene-referred value of 1, is scaled about the
// point where the curve puts diffuse white, in its input and in its output alike, by the factor that moves the curve's
// ceiling from 1 to the headroom. So every value up to diffuse white is shown where a display without headroom shows
// it, the curve keeps its slope at diffuse white, and only the highlights above it use the headroom. With a headroom of
// 1, the factor is 1 and the curve is unchanged.

// Returns the factor that scales the part of a curve above diffuse white, which the curve maps to `white`, for a
// display of `headroom`.
float highlightScale(float white, float headroom) {
    return (headroom - white) / (1 - white);
}

// The constants of Khronos's PBR Neutral tone mapper: the value from which it compresses, and how fast a compressed
// color loses its saturation.
static const float kNeutralStart = 0.8 - 0.04;
static const float kNeutralDesaturation = 0.15;

// Returns where the PBR Neutral curve puts the largest channel of a color, without headroom: unchanged below the start
// of the compression, and toward 1 above it.
float neutralCompress(float peak) {
    float d = 1 - kNeutralStart;
    return peak < kNeutralStart ? peak : 1 - d * d / (peak + d - kNeutralStart);
}

// Returns scene-referred linear sRGB mapped by Khronos's PBR Neutral tone mapper, for a display of `headroom`, in
// linear light with SDR white at 1. The mapper keeps a color's hue and saturation up to the start of its compression,
// so that a base color under even light is shown as it was authored. It first takes away the 4% that a dielectric
// reflects, then compresses the largest channel, and moves a compressed color toward white.
float3 toneMapNeutral(float3 color, float headroom) {
    float low = min(color.r, min(color.g, color.b));
    float offset = low < 0.08 ? low - 6.25 * low * low : 0.04;
    color -= offset;
    float peak = max(color.r, max(color.g, color.b));
    if (peak < kNeutralStart)
        return color;
    // Diffuse white is 1 less the offset.
    float whiteIn = 1 - 0.04;
    float white = neutralCompress(whiteIn);
    float scale = highlightScale(white, headroom);
    float compressed = neutralCompress(min(peak, whiteIn) + max(peak - whiteIn, 0) / scale);
    float newPeak = compressed + (scale - 1) * max(compressed - white, 0);
    color *= newPeak / peak;
    float g = 1 - 1 / (kNeutralDesaturation * (peak - newPeak) + 1);
    return lerp(color, float3(newPeak, newPeak, newPeak), g);
}

// The factor that takes a scene-referred value to the input of Narkowicz's fit. Narkowicz fitted the curve to data
// exposed in advance ("ACES Filmic Tone Mapping Curve", 2016). With this factor the fit is within 0.014 of the tone
// scale of ACES 1.0's reference rendering for an sRGB display in a dim surround, on the neutral axis, from 8 stops
// under middle gray to 8 stops over it, by the reference's CTL (aces-dev, RRT.ctl and
// ODT.Academy.sRGB_100nits_dim.ctl). His post gives 0.6 as the factor for the original curve, but at 0.6 the fit is
// 0.063 from that tone scale at most, and the factor that fits it best is within a hundredth of one half.
static const float kAcesExposure = 0.5;

// Returns where the ACES filmic curve puts a scene-referred value, in [0, 1]: Narkowicz's fit of the tone scale of
// ACES's reference rendering.
float3 acesCurve(float3 value) {
    float3 x = value * kAcesExposure;
    return saturate(x * (2.51 * x + 0.03) / (x * (2.43 * x + 0.59) + 0.14));
}

// Returns where the ACES filmic curve puts a scene-referred value on a display of `headroom`.
float3 acesMapped(float3 value, float headroom) {
    float white = acesCurve(float3(1, 1, 1)).r;
    float scale = highlightScale(white, headroom);
    float3 mapped = acesCurve(min(value, 1) + max(value - 1, 0) / scale);
    return mapped + (scale - 1) * max(mapped - white, 0);
}

// Returns scene-referred linear sRGB mapped by the ACES filmic curve, each channel alone, for a display of `headroom`.
// A channel is mapped whatever the other two hold, so a color's hue and saturation change with its exposure.
float3 toneMapAcesChannels(float3 color, float headroom) {
    return acesMapped(color, headroom);
}

// The share of the room between a color's luminance and the display's ceiling that the color's largest channel may take
// before the color loses saturation.
static const float kGamutKnee = 0.75;

// Returns scene-referred linear sRGB mapped by the ACES filmic curve applied to its luminance, for a display of
// `headroom`. The color is scaled by the ratio of its mapped luminance to its luminance, which keeps its hue and its
// saturation. A scaled color whose largest channel nears the display's ceiling is then mixed with the gray of its own
// luminance, by as much as keeps that channel below the ceiling. So a color that leaves the display's gamut loses
// saturation and keeps its hue and its luminance, and the brightest colors are white. Diffuse white is a luminance of
// 1: the ceiling is 1 up to it, and rises from there to the headroom as the mapped luminance does.
float3 toneMapAcesLuminance(float3 color, float headroom) {
    float luminance = dot(color, float3(0.2126, 0.7152, 0.0722));
    if (luminance <= 0)
        return float3(0, 0, 0);
    float mapped = acesMapped(float3(luminance, luminance, luminance), headroom).r;
    color *= mapped / luminance;
    float white = acesCurve(float3(1, 1, 1)).r;
    float ceiling = 1 + (max(mapped, white) - white) * (headroom - 1) / (headroom - white);
    float room = max(ceiling - mapped, 0);
    float excess = max(color.r, max(color.g, color.b)) - mapped;
    if (excess <= kGamutKnee * room)
        return color;
    // The excess is compressed toward the room by a curve that leaves the knee with a slope of 1.
    float d = (1 - kGamutKnee) * room;
    float compressed = room - d * d / (excess + room - 2 * kGamutKnee * room);
    return lerp(float3(mapped, mapped, mapped), color, compressed / excess);
}

// Returns linear sRGB in sRGB's encoding. A value above 1 is encoded by the same curve, as extended sRGB encodes it.
float3 srgbEncode(float3 color) {
    float3 low = color * 12.92;
    float3 high = 1.055 * pow(color, 1 / 2.4) - 0.055;
    return float3(color.r <= 0.0031308 ? low.r : high.r, color.g <= 0.0031308 ? low.g : high.g,
        color.b <= 0.0031308 ? low.b : high.b);
}

[shader("vertex")]
float4 displayVertex(uint id : SV_VertexID) : SV_Position {
    return renderWholeTriangle(id);
}

// Returns the view's color at the pixel, exposed, tone mapped and encoded.
[shader("fragment")]
float4 displayPixel(float4 position : SV_Position) : SV_Target {
    float3 color = max(viewColor(int2(position.xy)) * push.exposure, 0);
    float headroom = max(push.headroom, 1);
    float3 mapped;
    if (push.toneMap == kPrRenderToneMapAcesChannels)
        mapped = toneMapAcesChannels(color, headroom);
    else if (push.toneMap == kPrRenderToneMapAcesLuminance)
        mapped = toneMapAcesLuminance(color, headroom);
    else
        mapped = toneMapNeutral(color, headroom);
    return float4(srgbEncode(max(mapped, 0)), 1);
}
