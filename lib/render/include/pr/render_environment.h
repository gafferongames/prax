// The environments of a renderer (pr/render.h): the light that arrives from every direction around a scene, made from
// an image or from a sky's model. An environment lights the surfaces of the scenes that refer to it, and is the
// background of their views.
//
// Creating, setting and destroying an environment is thread-safe. An environment may be referenced as soon as it is
// created. What is set takes effect when the next frame begins (prRenderFrameBegin).

#pragma once

#include "pr/base.h"
#include "pr/gpu.h"
#include "pr/pixels.h"
#include "pr/render.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An environment: the light that arrives from every direction around a scene, in scene-referred linear sRGB. It lights
// the scene's surfaces, and is the background of the scene's views. Zero is no environment, which is black.
//
// The renderer holds an environment as a cube of half floats (PrRenderEnvironmentInfo) and nine colors. The cube's
// first level is the environment's radiance. Each level after it holds that radiance as the specular lobe of a rougher
// surface gathers it, which the device filters when the environment is set, in work of its own that no frame waits for.
// The colors are the spherical harmonics of the environment's diffuse light.
typedef struct PrRenderEnvironment {
    uint32_t id;
} PrRenderEnvironment;

// An image of radiance: rows of pixels of `channels` 32-bit floats each, `pixels.stride` bytes apart, whose first three
// floats are red, green and blue in linear sRGB. `channels` is 3 or 4. A value that is negative or not a number is read
// as zero, and a value above the largest half float as that float.
typedef struct PrRenderRadianceImage {
    PrPixelView pixels;
    uint32_t channels;
} PrRenderRadianceImage;

// The angle from the middle of the sun's disc to its edge, as the Earth sees it, in radians: 16 minutes of arc. The
// sun's nominal radius over one astronomical unit (IAU 2015 Resolution B3, IAU 2012 Resolution B2) is 0.1% less.
PR_CONSTANT float kPrRenderSunAngularRadius = 0.004654f;

// The illuminance of sunlight on a plane that faces the sun above the atmosphere, in lux: 127.5 klx, the solar
// illuminance constant of the Illuminating Engineering Society's daylight tables (IES RP-21), from which its clear day
// takes away exp(-0.21 m) at an air mass of m.
PR_CONSTANT float kPrRenderSunIlluminance = 127500.0f;

// An analytic sky: the light of a clear day as a viewer on the ground sees it, over a ground that reflects evenly.
//
// Sunlight is scattered by the air (Rayleigh's scattering) and by haze (Mie's), and absorbed by ozone, which is what
// colors the zenith and the horizon. The light that reaches the viewer after one scattering is integrated along each
// direction, as Nishita et al. do ("Display of the Earth Taking into Account Atmospheric Scattering", 1993). The light
// that scatters more than once, which is a third of a clear sky's, and the ground's light that the air scatters back,
// are added by Hillaire's approximation ("A Scalable and Production Ready Sky and Atmosphere Rendering Technique",
// 2020). Red, green and blue are three wavelengths, and sunlight is white above the atmosphere.
//
// The sun is in two places, which light nothing twice. Its light on surfaces is a directional light of the scene, which
// casts the shadows (prRenderSkySunIrradiance). Its disc is drawn in the background of a view and nowhere else: the
// environment that lights surfaces has no disc, and a mirror shows the sun as the directional light's highlight. The
// disc carries the directional light's irradiance, so the two agree in energy. It darkens toward its edge by one linear
// law for every color, to four tenths of its middle.
typedef struct PrRenderSkyDesc {
    PrVec3 sunDirection;        // toward the sun, of any length above zero
    float sunIrradiance;        // of white sunlight on a plane that faces it above the atmosphere, in lux

    // The amount of haze in the air: 0 for none, and 1 for a clear day, on which the sun straight overhead loses a
    // fifth of its illuminance to the atmosphere and a level surface in the open has a seventh of its light from the
    // sky.
    float haze;

    PrVec3 groundAlbedo;        // the part of the light that the ground reflects, linear
    float sunAngularRadius;     // the angle from the middle of the sun's disc to its edge, in radians, or 0 for no disc
} PrRenderSkyDesc;

// Returns a sky with the sun halfway up toward +x and +z, an irradiance of kPrRenderSunIlluminance, a clear day's haze,
// a ground that reflects a fifth of its light, and a disc of the sun's size: the daylight of a clear day, in lux and
// nits.
PrRenderSkyDesc prRenderSkyDescDefault(void);

// Returns the irradiance, in linear sRGB, of the sky's sun on a plane that faces it on the ground: the sunlight that
// the atmosphere lets through. A directional light of this color and an intensity of 1, shining along the reverse of
// the sun's direction, lights a scene as the sun of the sky does.
PrVec3 prRenderSkySunIrradiance(const PrRenderSkyDesc* sky);

// What the renderer holds for an environment, as of the frame that began last.
typedef struct PrRenderEnvironmentInfo {
    // The cube of the environment's radiance, in Rgba16F, or zero for an environment without data. Its faces lie as
    // lib/gpu lays out a cube's. Level i of `levels` holds the radiance filtered for a roughness of i / (levels - 1),
    // and the last level is at most 8 texels wide. The texture lives until the environment is set again or destroyed.
    PrGpuTexture texture;
    uint32_t faceSize;          // the width of a face of the first level, in texels
    uint32_t levels;
} PrRenderEnvironmentInfo;

// Creates an environment without data, which is black. Scenes may refer to it at once.
PrRenderEnvironment prRenderEnvironmentCreate(PrRender* render);

// Set or replace the environment's light, which the renderer copies before the call returns. The frames that begin
// after the call show it. The cost of a call is that of filling the cube's first level on the caller's thread, and of
// filtering the other levels on the device.
//
// An equirectangular image maps longitude to its columns and latitude to its rows: the direction toward +y is its top
// row, the direction toward +x its middle column, and the columns to the right of the middle turn toward +z. Its cube
// has faces of the renderer's environment size (PrRenderOptions).
//
// A cube is six square images of one size, at most kPrRenderEnvironmentMaxSize, which lie as lib/gpu lays out a cube's
// faces. They become the cube's first level as they are.
//
// A sky's cube has faces of the renderer's environment size. Making a sky's tables takes the calling thread a few
// milliseconds more than filling the cube does.
//
// Each returns Limit if the device has no memory or handle for the cube, NotSupported if the device does not render to
// half floats, and Lost if the device is lost. On failure the environment keeps the light that it had.
PrStatus prRenderEnvironmentSetEquirect(PrRender* render, PrRenderEnvironment environment, const PrRenderRadianceImage* image);
PrStatus prRenderEnvironmentSetCube(PrRender* render, PrRenderEnvironment environment, const PrRenderRadianceImage* faces);
PrStatus prRenderEnvironmentSetSky(PrRender* render, PrRenderEnvironment environment, const PrRenderSkyDesc* sky);

// Destroys the environment. A scene that still refers to it has a black one.
void prRenderEnvironmentDestroy(PrRender* render, PrRenderEnvironment environment);

// Returns true if the frame that began last shows the environment's light. Called on the thread that builds frames.
bool prRenderEnvironmentIsReady(const PrRender* render, PrRenderEnvironment environment);

// Returns what the renderer holds for the environment. Called on the thread that builds frames.
PrRenderEnvironmentInfo prRenderEnvironmentInfo(const PrRender* render, PrRenderEnvironment environment);

// Returns the radiance that a white surface without gloss leaves under the environment, as the shaders compute it from
// the environment's spherical harmonics: the irradiance of a plane with the unit normal `normal`, in the environment's
// own space, divided by pi. Called on the thread that builds frames.
PrVec3 prRenderEnvironmentIrradiance(const PrRender* render, PrRenderEnvironment environment, PrVec3 normal);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
