# Models

`lib/model` makes an asset of a scene format ready to draw with `lib/render` ([render.md](render.md)). A format's
adapter describes its assets through an abstract source (`pr/model_source.h`), and is one of lib/model's backends:
`backends/gltf` over `lib/gltf`, and `backends/usd` over `lib/usd`. `lib/model` adds what none of the formats has: the
hierarchy in a pose, animation, picking, the material system that binds a source's materials to the renderer's types,
and loading that never blocks a frame. `lib/texture` is to texture formats what this is to scene formats
([texture.md](texture.md)). The headers state each call's contract: `pr/model.h` for programs, `pr/model_source.h` and
`pr/model_source_backend.h` for the formats' adapters, `pr/model_gltf.h` and `pr/model_usd.h` for the adapters' own
options, and `pr/model_select.h` for the formats that a program may take as they come. This file covers what the module
is for, how its parts relate, and what it promises.

## Dependencies

```
lib/core (PrBlobSource)  lib/geom  lib/mesh  lib/color  lib/texture  lib/render  lib/mime
                  \          |         |         |           |          |          /
                   +----------------------- lib/model --------------------------+
                                         /                 \
                              backends/gltf               backends/usd            lib/model/select
                               (lib/gltf)            (lib/usd, lib/materialx)      (prax::model_select: the
                                                                                   formats that the build has)
```

- **Only the backends reach a format.** The rest of
  lib/model includes no format's header (`format-dependency`), and `lib/gltf`, `lib/usd` and `lib/materialx` do not
  link lib/model: a program that reads a glTF document or a USD stage to inspect it links neither lib/model nor the
  renderer. Its public header `pr/model_source.h` depends on core, geom, mesh, texture and color alone.
- **Formats are handed in, never chosen.** A model opens with the model format set that its caller hands it
  (`PrModelFormatSet`, `prModelOpen`), and loads its textures with the texture format set that its options hand it
  (`PrModelOptions::textures`). The selector (`lib/model/select`, `prax::model_select`) builds the set of the formats
  that the build has, and takes the compression codec set and glTF's extension set, which are other interfaces, from
  the program.

## From a file to a frame

```c
// Once, from the program's codec sets: the formats that the build has.
PrModelFormatSet* formats = prModelSelectBuiltin(compressCodecs, gltfExtensions);
PrTextureFormatSet* textures = prTextureSelectBuiltin(compressCodecs, imageCodecs);

PrModelOptions options = prModelOptionsDefault();
options.textures = textures;
options.pool = pool;
PrModel* helmet = prModelOpenFile(formats, "helmet.glb", &options);  // returns at once
prModelPopulate(helmet, render);                                      // returns at once

PrModelPlacementDesc placed = prModelPlacementDescDefault(scene);
PrModelPlacement* placement = prModelPlacementCreate(helmet, &placed);

// Each frame, before the renderer's frame begins:
prModelPlacementAdvance(placement, delta);
prModelUpdate(helmet);                                        // takes what has arrived
```

The application owns the frame, the scene, the camera and the lights that are its own. `examples/model_viewer` is this
code in a window, for one model after another.

## Discovery

Almost nothing is known when a model is opened: not the format, not the source, not any texture's format. Each is
discovered by a step that may complete later, on any thread, and may fail alone.

1. **The bytes**: a provider of the resource (`PrBlobSource`, `pr/blob.h`), whose reads are asynchronous and ranged.
2. **Classification**: the first bytes and the resource's name, classified by lib/mime (`prMimeSniff`).
3. **The source**: the first format of the set that supports the media type and does not refuse the bytes. A format
   reads the rest of the resource, and what it names through the opening's resolver (`PrModelResolver`).
4. **The description**: `prModelUpdate` takes a source that has arrived and describes the model: its nodes, meshes,
   materials, cameras, lights and animations appear, its placements enter their scenes, and in a renderer its
   resources are created and begin loading.
5. **Each texture**: its bytes, then the texture format that reads their media type (`PrTextureFormatSet`) and makes the
   image's source, which the renderer's textures of the image are set to, and which the texture cache realizes as the
   renderer asks for it ([Textures of the material system](#textures-of-the-material-system)).

**A model exists from the first call.** Before its source arrives it is an empty asset: no nodes, no meshes, no
animations, bounds and picks of nothing, and progress whose source is pending (`PrModelProgress::source`). Its
placements are made and placed, and draw nothing until the description arrives; rendering never waits. A failure is
reported by the step that failed: an opening that no format reads leaves the model empty with NotSupported, and an
image that does not decode fails alone while the rest draws.

**A consumer learns of changes by polling a revision** (`prModelRevision`), which grows when the description arrives,
the opening fails, a resource finishes loading, or the model populates a renderer. A view polls it once a frame after
`prModelUpdate` and refreshes what it shows, as Studio's model view and Scribe's outline do.

## Sources

A source describes its asset once, and the description does not change (`PrModelSourceInfo`). Its loaders are
reentrant: the model's jobs call them from any thread at once.

- **Nodes** have a parent, children, a name, a rest transform (a translation, a rotation and a scale, or a matrix),
  visibility, and an address within the asset (`prModelSourceNodeAddress`): glTF's JSON Pointer `/nodes/3`, or a USD
  prim's path through instances, `/World/Car`. A node is found by its address (`prModelSourceNodeFromAddress`), so a
  tool selects a node in any format by the fragment that the format's addresses end with.
- **Draws** place a mesh at a node: with a skin, once at the node, or once for each of several transforms within it,
  which is instancing (EXT_mesh_gpu_instancing) and a point instancer alike.
- **Meshes** have parts, each with its material, its bounds, its morph targets' bounds, and the attribute streams that
  its vertices have, by name, semantic and source format. A mesh's reader fills the streams that a request names,
  triangulated, its vertices the source's or one for each corner (`PrModelMeshPartFlag_Corners`).
- **Materials** are a neutral description: a shading model (`PrModelShading`: the Khronos PBR model with its
  extensions as named inputs, OpenPBR, UsdPreviewSurface, unlit), alpha, sidedness, and named inputs, each bound to a
  value, a texture through its sampler, transform and map of channels, or an attribute stream through a map of channels.
  A texture input may be tinted by an attribute. A part's materials in the asset's variants are part of the
  description.
- **Textures** name the images that may show them, by preference, with their sampler and the color of their texels. An
  image opens as its bytes, which the model's texture format set reads, or as a texture source that the source makes
  itself.
- **Cameras and lights** keep what the asset states: a light of an area keeps its shape and its luminance, and the
  model draws it as the punctual light of its intensity along its axis. An **environment** is a node with an image or
  a color.
- **Skins** have joints and inverse bind matrices. **Animations** are tracks: keyed values with an interpolation, of a
  node's translation, rotation, scale or matrix, morph weights, visibility, and the properties of lights, cameras and
  materials' inputs.
- **Diagnostics** name what the source does not describe as the asset means it (`PrModelDiagnostic`), and the model adds
  what its material system loses to the renderer's types.

## Reification

A source is a reification of its asset: a concrete, resolved description of what a composed format means, made once and
eagerly, and lossy by design ([usd.md](usd.md#praxs-relationship-to-usd)). lib/model draws the description and never
asks the format anything again.

- **A source reifies one composition context.** A format whose assets compose, as USD's stages do, describes the
  asset in a context that the adapter's options state: for USD, the variant selections, the purposes and the range of
  time codes (`PrUsdModelContext`). Another context is another source, and nothing that one source read serves another.
- **Time is reified as tracks.** A format's time-varying values become an animation's tracks when the source is made,
  as glTF's animations are. Posing a placement evaluates tracks, and no format is queried on the frame's path. A value
  that varies and that no track can hold is described at its rest, and the source's diagnostics name it.
- **The format owns invalidation.** A description does not change. When the asset changes, or the context does, the
  program makes a new source of it and a new model of the source, through the same adapter. USD's stages are immutable,
  so a changed stage is a new stage, and its source a new source.
- **Reification converges.** Two sources of one asset in one context have the same description, element for element
  and bit for bit (`prModelSourceInfoWrite` writes the text that compares them), and their loaders read the same
  meshes and images. What a loader reads on demand, a mesh's vertices or an image's bytes, it reads from the same
  resolved asset, so a model that populates a new renderer draws what it drew before.

## The material system

The binder (`model_bind.cpp`) is the model's material system, and the only part of the model that uses lib/texture.
It maps a source's materials onto the renderer's types by the names that the types declare, so a new material type
needs no C description, and no code of the binder's, for a loader to fill it.

- **A material's type is chosen by its shading model**, by name in the renderer's shader set: the first of the
  shading model's types that the set has and that holds what the material sets. The Khronos PBR model draws as
  `KhronosPbr`, or as `KhronosPbrLayered` where it sets a parameter or a texture that only the layered type holds;
  OpenPBR as `OpenPbr`; UsdPreviewSurface as `UsdPreview`; and unlit as `Unlit`. A set without a shading model's types
  draws it as another's, to which the binder converts its inputs: OpenPBR as `UsdPreview`. No type is named for a
  format. `UsdPreview` keeps its name: UsdPreviewSurface is the shading model's own name in its specification, as
  OpenPBR is.
- **Inputs bind by name.** Each shading model has a vocabulary that names its inputs as the types declare them: the
  Khronos model's property is its last name without Factor or Texture (`pbrMetallicRoughness/baseColorFactor` is
  `baseColor`, `normalTexture/scale` is `normalScale`, and `extensions/KHR_materials_emissive_strength/emissiveStrength`
  is `emissiveStrength`), OpenPBR's input is its name run together (`base_color` is `baseColor`), and
  UsdPreviewSurface's is its own. A value sets the type's parameter of that name (`prRenderMaterialParameterSet`) over
  the values that the type declares, a texture sets the type's texture of that name, and an input that an attribute of
  the vertices gives sets its parameter to 1, which the attribute multiplies. An input of a name that the type lacks
  sets nothing, and a texture of one is reported (`inputs`). The specification of the archived specular-glossiness
  workflow names the parameters that its properties replace.
- **What only a shading model knows stays the binder's**: UsdPreviewSurface's opacity decides its alpha mode, a
  surface that transmits light reads the scene behind it, an unlit color is the view's, and OpenPBR drawn as the
  preview type folds its weights into its colors.
- **Attributes bind by name.** Each attribute that a type declares is filled from the stream that binds to it: a set of
  texture coordinates from the stream of that set, the color from the stream that the material's color input reads, or
  else, for the Khronos model, the vertices' first color (glTF's COLOR_0), and a custom attribute from the input of its
  name bound to a stream, or from a stream of its name (glTF's `_NAME`). So a primitive variable feeds any input that a
  type declares an attribute for: the preview type's color for its diffuse color and opacity, and its emissive color.
  Wiring past what a type declares is reported (`attribute-inputs`), and the input draws as its value where a vertex
  lacks the attribute.
- **Only bound attributes cost.** A type's optional attribute is one that the material binds: an input of its name
  that an attribute gives. The binder names the material's set of them (`PrRenderMaterialDesc::attributes`), which
  chooses the variant of the type and its layout, so a mesh of a preview surface whose emissive color is a value
  carries the Khronos types' layout alone.
- **A mesh has a stream of each layout of the variants that the model's materials draw with**, filled through
  `prRenderVertexLayoutFill` from the prepared vertices of each part.

### Textures of the material system

The binder makes the renderer's textures of an image for what its materials need of it, and sets each to the image's
source. What the texture realizes as on the device is the texture cache's choice
([texture.md](texture.md#the-texture-cache)), so the model has no texture policy of its own.

- **A texture's color is its transfer.** A texture is sampled as color, through the Srgb form of its format, which
  decodes sRGB's transfer function, or as data, through the plain form. The asset's declaration decides which: glTF's
  roles of color and data, and the color space names of USD and MaterialX, which say only the transfer that they
  imply, `srgb_texture` sRGB's and `acescg`, `lin_ap1` or `lin_rec709` linear values. An asset's working color space
  is not handled per texture.
- **A texture is a use of an image.** The materials' textures of one image that agree in the kind that their types
  declare (color or data) and in how they are sampled share one, and its channels are the union of those that each
  reads (`PrRenderMaterialTexture::channels`, or the one that a scalar input chooses). The binder sets the texture's
  source with those channels and that kind (`prRenderTextureSetSource`). An image that one material reads as color
  and another as data is two textures of the renderer, which the cache realizes as one texture unless it makes their
  levels.
- **Nothing of an image's bits is kept by the model.** The renderer's textures hold the image's source, and the cache
  realizes it. A model that populates a new renderer, as after a lost device, sets the sources again, and the cache
  reads the images' bytes again as the new renderer asks for them.

## Responsibilities

- **A model is a producer of a scene.** The renderer has no hierarchy by design. The model owns the asset's hierarchy,
  computes each node's world transform in a pose, and writes instance records through the renderer's updates: the
  transforms, the joints of the skinned instances and the weights of their morph targets.
- **A model owns what it creates in a renderer**: a mesh for each mesh of the source, a material for each material, the
  textures that the materials read, and the environment that the asset brings. The scene is the application's.
- **A placement is one use of a model in a scene.** It owns its instances, its root transform, its pose, its variant
  and its highlight. A model is placed any number of times, and each placement plays its own animation.
- **The source stays the source.** A model keeps no copy of decoded images or prepared meshes for drawing. It reads the
  source again each time it populates a renderer, so a lost device is recovered by populating a new one.
- **Picking is on demand.** A pick (`prModelPlacementPick`) tests a ray against the triangles of a placement's meshes in
  its pose, through lib/mesh's bounding volume hierarchies, prepared at the first pick that reaches a mesh. A mesh that
  deforms is tested in the shape that the renderer draws, and its hierarchy is refitted, and built again only when the
  refit's boxes overlap too much (`prMeshBvhRefit`, `prMeshBvhCost`).

## Loading

Loading never blocks a frame.

- **`prModelOpen` and `prModelPopulate` return at once.** Populating creates every resource without data and starts a
  job for each mesh and each image that a material uses, meshes first, so that a model shows its shape first. The
  renderer skips an instance whose mesh has no data, and reads a texture without texels as the stand-in that its
  material's type declares, so each frame draws what has arrived.
- **A job finishes when its last step does.** An image's job reads its bytes, opens them, and sets the source of each
  of its textures, and of the environment if the image is the asset's environment; the cache's realization and the
  environment's read the bytes again, and each read may complete later. The environment's color and intensity scale
  its radiance where the scene takes it (`prModelPlacementEnvironment`, `PrRenderSceneEnvironment::scale`), so no
  texel is changed on the CPU. `prModelWait`
  finishes the opening and the jobs, and waits for the realizations of the model's textures, for a caller that wants
  the whole model, such as a tool that renders one image.
- **Jobs run on the pool** that the options give. Without one, each `prModelUpdate` starts one job, so a model loads
  over several frames on a platform without threads.
- **Leaving a renderer waits for the jobs that are running.** `prModelDestroy` and `prModelPopulate` wait for them, and
  for the steps that they started. A placement survives a new renderer in no scene, and `prModelPlacementSetScene` puts
  it in one.
- **The bounds are known before anything loads**: a source states the bounds of every part and of every morph target.

## Preparing meshes

A mesh of the source becomes one mesh of the renderer, its parts in their order.

- **A part without normals gets flat normals**, and its tangents are ignored. **A part without tangents gets
  MikkTSpace's** where its material reads a normal map or a direction of its surface, from the set of texture
  coordinates that the texture reads. lib/mesh's bitangent points where v increases, and a source's up the image, so
  the model negates it.
- **Vertices that the preparation unshares are shared again** where they are still equal, in the geometry and in the
  streams that the bindings read. A part whose vertices are the source's and that deforms shares only corners that came
  from one vertex of the source, and one that morphs without normals shares none.
- **Each part is ordered for the GPU's caches**, unless its material blends, which keeps the order of its triangles.
- **A skinned vertex takes the four joints of the greatest weight** among its influences, and **a morph target gives a
  vertex its displacements** of position, normal and tangent.

## Animation and poses

A placement plays one animation at a time of its own, and may blend a second into it by a weight. Each track replaces
its property in the pose: translations, scales, weights and lights mix linearly in a blend, rotations along the shorter
arc, and the animation of the greater weight decides visibility. Time is the placement's input; a source's own time,
as a stage's time codes, is keyed into tracks when the source is made ([Reification](#reification)), so no value is
resolved on the frame's path.

- **A material that an animation changes is the placement's own**: a placement that plays a track of a material makes a
  material of the renderer's for itself, and frees it when it leaves its scene. A model whose animations change no
  material costs nothing more.
- **A skinned mesh follows its joints**, which are nodes like any other, and **morph weights are the node's, or else
  the mesh's**, until a track gives them.
- **The bounds follow the pose**: the box of the positions that each joint moves, under that joint, and the bounds of
  the morph targets at their weights.

## Balancing an asset's light

No one policy lights and exposes every asset well. The model gives a host the facts and the controls.

- **What the asset brings** (`prModelLighting`): lights, an environment, a surface that emits, and whether every surface
  is unlit.
- **The unit of emission** (`PrModelOptions::emission`, `prModelSetEmission`) is the model's: luminance in nits, as
  glTF's core and UsdPreviewSurface state it, or the view's own, where an emission of 1 is a fully exposed pixel.
- **Each light has a scale in each placement** (`prModelPlacementSetLightScale`). The environment needs none: the host
  sets the scene's environment from `prModelPlacementEnvironment`, or its own.

## Variants, visibility and highlight

- **A placement draws one variant of the asset's materials**, or none (`prModelPlacementSetVariant`).
- **A hidden node draws nothing, nor do its descendants**: its instances stay in the scene with no visibility, and its
  lights shine with no intensity.
- **A node and what is under it draw with a highlight** (`prModelPlacementSetHighlight`), as a tool marks a selection.

## The formats

- **glTF** ([gltf.md](gltf.md)): the adapter wraps lib/gltf's document, with its Draco and meshopt compression. A
  material's inputs are the Khronos model's properties by their paths within the material, as KHR_animation_pointer
  names them, and its animations are its channels' samplers.
- **USD** ([usd.md](usd.md#models)): the adapter wraps the schema reader. It expands instances, resolves bindings and
  subsets, triangulates faces and implicit shapes, reads preview surfaces, and reads MaterialX networks as OpenPBR's
  inputs through lib/materialx's mapping of MaterialX's surfaces. It reifies the stage in a composition context, whose
  time samples are tracks. A dome light is the environment, and an area light keeps its shape. Its resources are
  providers that it opens when it is made, each a package's file over its package's provider where the location names
  one.

## Testing

The unit tests read small assets written in the tests' own text, through the adapters, and a source of the tests' own.
They check the hierarchy, animations, bounds, cameras and lights without a GPU, and the preparation of each kind of
part. On each backend that the system has a device for, they check what a frame draws while a model loads and after a
resource fails, a model opened from bytes that arrive late, a texture whose format is known only once its bytes arrive,
an opening that no format reads, a primitive variable that drives an emissive color, an animation of a skin and a morph
target, a material that an animation changes for one placement, and a lost device.

The `model` suite renders Khronos's sample assets through the selector's formats, in every variant
([testing.md](testing.md)). It checks that every resource loads, that validation is silent, that a device with slots
alone draws what one with handles draws, that the techniques agree away from edges, and that something is drawn.
