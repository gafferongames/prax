// The display draw of a renderer (pr/render.h): a view's color (pr/render_view.h), scaled, tone mapped and encoded as
// lib/draw's targets hold color, in a pass of the application's.

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/render.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// The tone maps of the display draw. Each maps a view's scene-referred color into what a display without headroom
// shows. The two ACES maps share one curve: Narkowicz's fit of the tone scale of ACES's reference rendering, at the
// exposure of that rendering. The curve has a toe and a shoulder. It shows middle gray, a scene-referred value of 0.18,
// as 0.11 and diffuse white as 0.62, where the neutral map shows them as 0.14 and 0.87, so it shows a scene a third to
// a half of a stop darker than the neutral map does.
enum {
    // Khronos's PBR Neutral tone mapper. It shows a base color under even light as it was authored. It changes no value
    // below 0.8 but for the 4% of a dielectric's reflection, which it takes from every channel, and it compresses only
    // the highlights above that. Taking the 4% away darkens and saturates a color that the scene lights dimly.
    PrRenderToneMap_Neutral,

    // The ACES curve applied to each channel alone. It keeps a neutral color neutral, and no other color's hue: a
    // bright saturated color moves toward the nearest of red, yellow, green, cyan, blue and magenta. The toe gives each
    // channel more contrast, which saturates dark colors.
    PrRenderToneMap_AcesChannels,

    // The ACES curve applied to luminance, with the color scaled by the ratio of its mapped luminance to its luminance.
    // It keeps hue, and keeps saturation until the scaled color nears the display's gamut, where the color moves toward
    // the gray of its own luminance. The toe darkens a dark color without saturating it. A saturated color is shown as
    // dark as its luminance is: a pure blue is darker than the other maps show it.
    PrRenderToneMap_AcesLuminance,

    PrRenderToneMap_Count,
};
typedef uint8_t PrRenderToneMap;

// Returns the tone map that `name` names, as prRenderToneMapName spells it, ignoring case, or Count if it names none.
PrRenderToneMap prRenderToneMapFromName(PrStringView name);

// Returns a tone map's name in lowercase with hyphens between its words, such as "aces-luminance".
PrStringView prRenderToneMapName(PrRenderToneMap toneMap);

// How a view's color is shown.
typedef struct PrRenderDisplayOptions {
    float exposure;             // the factor that scales the view's color before the tone map
    float headroom;             // the display's headroom, as a multiple of SDR white (PrAppDisplay); 1 or less for SDR
    PrRenderToneMap toneMap;
} PrRenderDisplayOptions;

// Returns an exposure of 1, a headroom of 1, and the neutral tone map.
PrRenderDisplayOptions prRenderDisplayOptionsDefault(void);

// Draws a view's color over the whole of `pass`'s first color attachment, tone mapped and encoded as lib/draw's targets
// hold color: extended sRGB, encoded, so that lib/draw's 2D composes over it in the same pass. With a headroom of 1,
// every value lies in [0, 1]. With more, every color up to diffuse white, a scene-referred value of 1 after exposure,
// is shown exactly where a headroom of 1 shows it, and the colors above it keep values above 1, up to the headroom: the
// part of the tone map's curve above diffuse white is scaled about that point until its ceiling is the headroom. The
// draw is one way to show a view. A view's color is a texture of the graph, and an application may map it to its
// display in a pass of its own instead. `color` is the device's texture of a view's color (prGpuGraphTexture), which
// the pass reads in the fragment stage, at the extent of the pass's target. Null `options` means the defaults.
void prRenderDisplayDraw(PrRender* render, PrGpuPass* pass, PrGpuTexture color,
    const PrRenderDisplayOptions* PR_NULLABLE options);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
