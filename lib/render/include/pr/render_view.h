// The views of a renderer (pr/render.h): the techniques that add the passes of one view of a scene to the frame's
// graph, and the cameras that they see by. A view's color is shown by the display draw (pr/render_display.h), at an
// exposure that pr/render_exposure.h computes.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/gpu.h"
#include "pr/gpu_graph.h"
#include "pr/render.h"
#include "pr/render_meter.h"
#include "pr/render_scene.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Views

// How a view is drawn. Every technique calls the same materials and the same shading models.
enum {
    // Shades each surface as it is drawn, after a pass that draws depth alone, into multisampled targets that resolve
    // into the view's color. Each material type is drawn with a pipeline of its own. Every device draws with it.
    PrRenderTechnique_Forward,

    // Draws which triangle each pixel shows, and then shades each pixel once, in one shader for every material type.
    // Blended materials are drawn over the result as the forward technique draws them. It needs a device with
    // PrGpuFeatureFlag_Bindless, and has one sample per pixel.
    PrRenderTechnique_Visibility,
};
typedef uint8_t PrRenderTechnique;

// Returns true if the renderer's device draws with `technique`.
bool prRenderSupportsTechnique(const PrRender* render, PrRenderTechnique technique);

// A camera: the matrix that maps the world to the camera's space, and the matrix that maps that space to clip space.
// The camera looks along its negative z axis, with x to its right and y up. The view matrix turns and moves the world
// and does not scale it, so that the camera's place in the world follows from it. Clip space holds a point whose x and
// y lie within its w of zero, with y up. Depth is reversed: z / w is 1 at the near plane and falls toward 0 far away.
typedef struct PrRenderCamera {
    PrMatrix4 view;
    PrMatrix4 projection;
} PrRenderCamera;

// Returns a camera at `eye` that looks toward `target`, with the world's y axis up, and a perspective of `verticalFov`
// radians from top to bottom. `aspect` is the target's width over its height. The camera sees from `nearDistance` on,
// and has no far plane.
PrRenderCamera prRenderCameraLookAt(PrVec3 eye, PrVec3 target, float verticalFov, float aspect, float nearDistance);

// A view of a scene.
typedef struct PrRenderViewDesc {
    PrRenderScene scene;
    PrRenderCamera camera;
    PrUVec2 extent;             // of the view's color and depth, in pixels
    PrRenderTechnique technique;

    // The forward technique's samples per pixel: 1, 2, 4 or 8. A device that lacks the number draws with the most that
    // it has below it (PrGpuDeviceInfo::sampleCounts).
    uint8_t samples;

    // The view sees the instances whose visibility mask shares a bit with this one. kPrRenderVisibilityShadow is not a
    // bit of a view: an instance with that bit alone casts shadows and is seen by no view.
    uint32_t mask;

    // The factor that scales the scene's luminance, in nits, into the values of the view's color, which are half
    // floats: a photometric scene, whose daylight reaches tens of thousands of nits and whose sun's highlights reach
    // far more than a half float holds, is exposed before it is stored (prRenderExposureFromEv100). 1 keeps the scene's
    // values. Every surface and the background are scaled alike, and a surface that reads the scene behind it reads the
    // scene's own values.
    float exposure;

    // The meter that reads what the view's color shows, or none. A view with a meter is drawn whether or not a pass
    // reads its color.
    PrRenderMeter meter;

    bool shadows;               // surfaces are shadowed by the scene's shadow map
    bool keepsDepth;            // the view's depth is kept for the application's later passes
} PrRenderViewDesc;

// Returns a view of `scene` by the forward technique with 4 samples, that sees every instance, with an exposure of 1
// and shadows. The caller sets the camera and the extent.
PrRenderViewDesc prRenderViewDescDefault(PrRenderScene scene);

// The textures of the frame that a view's passes write.
typedef struct PrRenderViewTextures {
    // The view's color: scene-referred linear sRGB times the view's exposure, in Rgba16F, at one sample per pixel.
    PrGpuGraphTexture color;

    // The view's depth in D32F, reversed, at the samples that the technique draws with, or none unless the view keeps
    // its depth.
    PrGpuGraphTexture depth;
} PrRenderViewTextures;

// The maximum number of views of one scene in one frame.
enum { kPrRenderMaxViews = 8 };

// Adds the passes that draw a view to the frame's graph, and returns the textures that they write. The passes that do
// not depend on the camera, such as the scene's shadow map, are added once for all of a frame's views of one scene. A
// view by a technique that the device does not draw with is drawn by the forward technique. Returns no textures for a
// scene that does not exist or an empty extent. A frame adds at most kPrRenderMaxViews views of one scene.
PrRenderViewTextures prRenderViewAdd(PrRender* render, const PrRenderViewDesc* desc);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
