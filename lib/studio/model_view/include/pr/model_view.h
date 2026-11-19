// Studio's model view: a model's asset drawn by lib/render through lib/model, in a GPU view of Studio Kit (prKitEmbed),
// with an orbit camera and the asset's own cameras, its animations, and the controls of its environment and display.
//
// A host gives the view a model (pr/model.h), of any format that the host's formats read, and declares the view in each
// round. A model that the host opens fills in as its steps complete, and the view shows it from the round after its
// description arrives. The view is a GPU view of its Studio window (prStudioWindowGpuViewCreate): it renders the asset
// on the device that renders the window, into a texture of its own, in the frame's graph, at its laid-out size in
// target pixels. It renders a frame only when something that it shows changed: the camera, the size, the selection, the
// controls, a part of the asset that loaded, or an animation that plays. Otherwise the window draws the texture of its
// last render, so a view that holds still costs no render.
//
// Dragging in the view turns the orbit camera about the asset, and the wheel moves it nearer or farther. The bar above
// the asset chooses the camera, the animation, whether it plays, the tone map, how the asset is lit and exposed, and
// whether a ground under the asset receives its shadow. The node that the host selects draws with a highlight, with the
// nodes under it.
//
// Light is photometric, as lib/render's is. No one way of lighting and exposing suits every asset, so the view offers
// the renderer's options (PrModelViewLighting), and takes its defaults for each asset from what the asset brings
// (prModelLighting, prModelViewLightingDefault). The environment is the asset's own, a stage's dome light, or the
// view's sky, a clear day's daylight with its sun, or none: a scene has one. The asset's own lights shine with it, each
// scaled by the view's factor of the asset's lights. The exposure is a meter's of the scene's light at the asset's
// center (prRenderSceneIlluminance), so that a white surface without gloss that faces the brightest of that light shows
// as diffuse white, or a meter's of the image, which follows what the view shows over a few frames, or an EV100 of the
// user's, with a compensation over each.
//
// One thread uses a view: the thread that runs the window, on which the window's frame renders it.

#pragma once

#include "pr/gpu_graph.h"
#include "pr/kit.h"
#include "pr/model.h"
#include "pr/string_view.h"
#include "pr/studio_window.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// An asset, the renderer that draws it, the camera and the controls.
typedef struct PrModelView PrModelView;

// What a view draws with. The view is a GPU view of `window`, whose kit `kit` is. Without a window, the view shows its
// controls, and its host renders it (prModelViewRender). Each outlives the view.
typedef struct PrModelViewDesc {
    PrKit* kit;
    PrStudioWindow* PR_NULLABLE window;
} PrModelViewDesc;

// Creates a view that shows nothing, or frees one.
PrModelView* prModelViewCreate(const PrModelViewDesc* desc);
void prModelViewDestroy(PrModelView* PR_NULLABLE view);

// Shows a model, which the view takes and destroys, or nothing for null: once its description arrives, with the orbit
// camera framing it, its first animation playing and its default lighting. The view populates its renderer with it, and
// updates it in each round. A model whose opening failed shows why, without naming a format.
void prModelViewSetModel(PrModelView* view, PrModel* PR_NULLABLE model);

// The model that the view shows, or null.
const PrModel* PR_NULLABLE prModelViewModel(const PrModelView* view);

// Draws the meshes of `node`, a node of the model (prModelNodeFromAddress finds one by its address), and of the nodes
// under it with the highlight, or none with kPrModelNodeNone.
void prModelViewSetSelection(PrModelView* view, PrModelNodeIndex node);

// The camera that the view looks through: 0 for the orbit camera, or 1 more than a camera of the model
// (prModelCameraCount). Setting a camera that the model lacks chooses the orbit camera.
void prModelViewSetCamera(PrModelView* view, uint32_t camera);
uint32_t prModelViewCamera(const PrModelView* view);

// Plays an animation of the asset from its start, or none with kPrModelAnimationNone, and pauses or plays it.
void prModelViewSetAnimation(PrModelView* view, PrModelAnimationIndex animation);
PrModelAnimationIndex prModelViewAnimation(const PrModelView* view);
void prModelViewSetPlaying(PrModelView* view, bool playing);
bool prModelViewPlaying(const PrModelView* view);

// Where the orbit camera is: its turn about the asset's vertical axis and its tilt above the asset's center, in
// radians, and its distance as a multiple of the distance at which the asset fills the view. The tilt is within 1.5
// radians of level, and the zoom between 0.02 and 8.
typedef struct PrModelViewOrbit {
    float turn;
    float tilt;
    float zoom;
} PrModelViewOrbit;

// The environment that lights the asset and is its background.
enum {
    PrModelViewEnvironment_Asset,   // the asset's own (prModelEnvironmentNode), or none for an asset without one
    PrModelViewEnvironment_Sky,     // the view's sky, with its sun, which casts the shadows
    PrModelViewEnvironment_None,
    PrModelViewEnvironment_Count,
};
typedef uint8_t PrModelViewEnvironment;

// How the view chooses its exposure.
enum {
    PrModelViewExposure_Scene,      // an incident-light meter of the scene's light at the asset's center
    PrModelViewExposure_Image,      // a meter of what the view shows (PrRenderMeter), which adapts over a few frames
    PrModelViewExposure_Manual,     // the lighting's EV100
    PrModelViewExposure_Count,
};
typedef uint8_t PrModelViewExposure;

// How the view lights and exposes its asset. A factor of light is in stops: a scale of 2 to the power of the value.
typedef struct PrModelViewLighting {
    PrModelViewEnvironment environment;
    float environmentStops;     // scales the environment, and the sky's sun with the sky
    float lightStops;           // scales each of the asset's lights (prModelPlacementSetLightScale)
    PrRenderEmission emission;  // the unit of the asset's emission (prModelSetEmission)
    PrModelViewExposure exposure;
    float ev100;                // the exposure of PrModelViewExposure_Manual
    float compensation;         // stops that brighten the image over the exposure that the mode chooses
} PrModelViewLighting;

// Returns the lighting that the view gives `model` when it shows it, from what the model brings (prModelLighting):
// - the asset's environment if it has one, the sky for an asset that brings neither lights nor an environment, and
//   otherwise none, so that an asset authored with its own light is seen in it alone;
// - emission in nits for an asset that brings light of its own, whose emission was authored beside it, and otherwise
//   in the view's unit, where an emission of 1 is a fully exposed pixel, so that an asset that glows does not vanish
//   under a sky of 100,000 lux;
// - the meter of the scene's light, with no scale and no compensation, and the EV100 of a clear day for the manual
//   exposure.
//
// A null model gives the lighting of an asset that brings nothing.
PrModelViewLighting prModelViewLightingDefault(const PrModel* PR_NULLABLE model);

// The view's lighting, which the view sets to its model's default once the model is described. Setting it renders the
// view again.
PrModelViewLighting prModelViewLighting(const PrModelView* view);
void prModelViewSetLighting(PrModelView* view, const PrModelViewLighting* lighting);

// Returns the EV100 that the view's last render was exposed at, before its compensation: the meter's or the user's.
float prModelViewEv100(const PrModelView* view);

// Return the choice that `name` names, as the matching name function spells it, ignoring case, or Count if it names
// none, and a choice's name in lowercase: "asset", "sky" and "none", and "scene", "image" and "manual".
PrModelViewEnvironment prModelViewEnvironmentFromName(PrStringView name);
PrStringView prModelViewEnvironmentName(PrModelViewEnvironment environment);
PrModelViewExposure prModelViewExposureFromName(PrStringView name);
PrStringView prModelViewExposureName(PrModelViewExposure exposure);

// The orbit camera, which a drag in the view turns and the wheel moves nearer or farther.
PrModelViewOrbit prModelViewOrbit(const PrModelView* view);
void prModelViewSetOrbit(PrModelView* view, PrModelViewOrbit orbit);

// Writes what a click in the round of the last prModelViewFrame picked to `*out`, and returns true, or returns false if
// the round had no click. A click picks the nearest mesh under it (prModelPlacementPick), as the asset showed when it
// was clicked, and selects the node that draws the mesh, or no node if nothing of the asset is under it. The host takes
// the pick as the selection that its other views share.
bool prModelViewPicked(const PrModelView* view, PrModelPick* out);

// The number of frames that the view has rendered, which a frame that holds still does not change.
uint64_t prModelViewRenders(const PrModelView* view);

// Frees the renderer and the texture that the view made on its device, which is lost and not yet freed. The next render
// makes them again on the device that it gives. The view's window calls it as its device goes.
void prModelViewDeviceLost(PrModelView* view);

// Renders the view into a frame's graph at the extent that `render` gives, into the view's own texture, which it makes
// again at another extent. Returns the texture imported into the graph, which the graph leaves in the accesses of
// shader reads, opaque, or no texture if the view shows no asset or the device draws nothing of lib/render. The view's
// window calls it once the round is laid out. The device outlives the view's use of it.
PrStudioWindowGpuOutput prModelViewRender(PrModelView* view, const PrStudioWindowRender* render);

// Declares the view in a round under `parent`, which it fills: the bar of its controls, and the asset below it, which
// renders at the size that layout gives it. Renders again while the asset loads or an animation plays.
void prModelViewFrame(PrModelView* view, PrLayoutNode parent);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
