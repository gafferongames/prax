# Stages

`lib/stage` presents one or a few models: what they stand on, what lights them, how they are exposed, and what frames
them. It is the policy that a model viewer, a product page and an editor's preview share, so that showing an asset well
takes no code of the application's. It links `lib/model`, `lib/render` and `lib/geom`. The header `pr/stage.h` states
each call's contract. This file covers what the library is for, how its parts relate, and what it promises.

## What a stage is

- **A stage is a producer of a scene's records**, as a model is ([render.md](render.md#responsibilities)). It adds a
  ground's instance, a sun, and the scene's environment to a scene of the application's, and removes them when it is
  destroyed. The scene stays the application's: other content in it is drawn with what the stage adds.
- **A stage is not a scene.** A scene is the renderer's flat records, which any producer writes. A stage holds a
  policy over the placements that it presents, and the records that the policy makes.
- **A stage presents placements** (`prStageAdd`), any number, and follows them: their bounds through their animation,
  the lighting that their assets bring, and their arrival as their models load.
- **A stage knows no camera and no input.** It gives the sphere of what it presents (`prStageSphere`), which a camera
  controller fits ([camera.md](camera.md#framing)), and the exposure that its light calls for, which the frame's view
  takes.

## Light

- **The light follows what the assets bring** (`PrStageLighting_Auto`). An asset with an environment is lit by it, and
  one with lights of its own by them. One that brings neither is lit by the analytic sky of a clear day
  ([render.md](render.md#the-sky)), whose sun casts the shadows on the ground. A stage may be told which to use, or be
  given an environment of its own as a texture source (`prStageSetEnvironment`), which it reads as the renderer reads
  any environment.
- **Exposure follows the light** (`prStageExposure`): a view's exposure factor, from the EV100 that shows a clear
  day's daylight, an asset's own lights at their stated units, or an environment by its luminance, which a view's
  meter refines.
- **Emission's unit is the assets'**, as the model reports it (`prModelSetEmission`), unless the stage is told
  otherwise.

## The ground

- **The ground is a matte plane under what the stage presents**, at the lowest point of its bounds through its
  animation, with the albedo of the sky's own ground. It reaches far enough that its edge lies within a few pixels of
  the horizon, where the sky's ground takes over, so it shows no edge.
- **It receives the shadows** of the stage's sun, and is absent where the stage is told to show none.

## Framing

- **The sphere holds what the stage presents through its animation**, sampled at steps over each placement's
  animation, so a camera fit once does not cut off a limb that swings out later.
- **It changes as models arrive.** A model whose description has not arrived has no bounds; the sphere grows when it
  does, and the stage's revision tells a controller to fit again (`prStageRevision`).

## A model on screen

This is the measure of the stage and of every layer under it: a program that opens a file, presents it, orbits it, and
draws it. `examples/model_minimal` is this program, whole, in about a hundred lines with its window and its device. A
step that it cannot express in a few lines is a feature that a library is missing.

```c
// Once: the formats that the build has, and a renderer with its texture cache on the device.
PrModelFormatSet* formats = prModelSelectBuiltin(compressCodecs, gltfExtensions);
PrTextureFormatSet* textureFormats = prTextureSelectBuiltin(compressCodecs, imageCodecs);
PrTextureCache* textures = prTextureCacheCreate(&(PrTextureCacheDesc){.device = device, .tasks = pool});
PrRender* render = NULL;
prRenderCreate(device, &(PrRenderOptions){.textures = textures}, &render);
PrRenderScene scene = prRenderSceneCreate(render);

PrModelOptions options = prModelOptionsDefault();
options.textures = textureFormats;
options.pool = pool;
PrModel* model = prModelOpenFile(formats, path, &options);
prModelPopulate(model, render);
PrModelPlacement* placement = prModelPlacementCreate(model, &(PrModelPlacementDesc){.scene = scene});

PrStage* stage = prStageCreate(render, scene, NULL);
prStageAdd(stage, placement);
PrCameraOrbit orbit = prCameraOrbitDefault();

// Each frame:
prModelUpdate(model);
prModelPlacementAdvance(placement, delta);
prStageUpdate(stage);
prCameraOrbitFit(&orbit, prStageSphere(stage));        // fits again only when the sphere changed
prCameraOrbitAdvance(&orbit, &input, delta);

prGpuGraphBegin(graph);
prRenderFrameBegin(render, graph);
PrRenderViewDesc desc = prRenderViewDescDefault(scene);
desc.camera = prCameraOrbitCamera(&orbit, aspect);
desc.extent = extent;
desc.exposure = prStageExposure(stage);
PrGpuGraphTexture color = prRenderForward(prRenderViewBegin(render, &desc), NULL);
prRenderDisplayAdd(render, graph, color, swapChainTexture);
prGpuGraphRecord(graph, commands);
```
