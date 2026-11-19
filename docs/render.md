# Rendering in 3D

`lib/render` is a 3D renderer as a library of parts: meshes, textures, materials and environments on a GPU device,
scenes of instances and lights, a shading model, and the parts that add the passes of a view to a render graph
([gpu.md](gpu.md#render-graph)). The headers state each call's contract. This file covers what the parts are for and
what they promise each other.

## Responsibilities

- **The application owns the frame.** It builds a graph each frame, asks the renderer to add a view's passes, adds
  passes of its own, and records the graph. The renderer owns no window, swap chain, camera controls or frame loop.
- **A scene is flat records in world space**: instances, lights and an environment. There is no hierarchy, no entity
  system and no simulation. A producer, such as a loader of glTF, a game's own storage or an editor, owns the structure
  and writes the records.
- **Meshes, textures, materials and environments are resources of the renderer**, apart from scenes. A scene refers
  to them by id, and several scenes share them.
- **A texture is a source that the host's texture cache realizes** (`PrRenderOptions::textures`,
  [texture.md](texture.md#the-texture-cache)). A loader sets a texture's source and states what the materials read of
  it: its channels, and whether as color or as data (`prRenderTextureSetSource`). The renderer asks the cache for it
  with every mip level, samples a color texture through its Srgb form and a data texture through its plain form, and
  samples the stand-in until it arrives. The cache is the host's, on the renderer's device, so an image that a material
  samples and a UI shows is one texture.
- **The records and the shaders' functions are published.** An application's own passes read the scene's records, and
  its own shaders call the renderer's materials and shading model.
- **Content that is not an instance of a mesh**, such as terrain, hair or a volume, is a pass of the application's
  that reads the same records. The scene is not a host for plugins.

The renderer does not load files, decode images or process geometry. A loader gives it vertices in its layout, texture
sources, material descriptions and an environment's source or sky.

**Space is `lib/geom`'s.** The world is right-handed with y up, and a camera looks along its negative z axis. An
instance's transform and a joint's matrix are a `PrAffine3`, a part's bounds and the sphere that a light's shadows
cover are a `PrSphere`, and a camera's matrices are the ones that `prMatrix4LookAt` and
`prMatrix4PerspectiveProjection` return: depth is reversed, 1 at the near distance and 0 infinitely far, with no far
plane. A producer that has a `PrMatrix4` takes its top three rows with `prAffine3FromMatrix4`. A view's camera is
`lib/camera`'s (`PrCamera`, [camera.md](camera.md)), which the renderer takes as it is and never moves.

## Objects and lifetimes

Everything is named by a small integer id, as `lib/gpu` names its resources. Zero is none.

| object | made by | lives until |
| --- | --- | --- |
| renderer (`PrRender`) | `prRenderCreate`, over a device that is ready, with a shader set | `prRenderDestroy` |
| shader set (`PrRenderShaderSet`) | the build, by `pr_render_shaders` | the program ends |
| pipeline | the renderer, when a material or a pass first needs it | `prRenderDestroy` |
| mesh, texture, material, environment | its `Create` call, on any thread | its `Destroy` call |
| scene | `prRenderSceneCreate` | `prRenderSceneDestroy` |
| meter | `prRenderMeterCreate`, on any thread | `prRenderMeterDestroy` |
| instance, light | an update of their scene | an update removes them, or the scene is destroyed |
| an instance's deformed vertices | the frame after an update gives the instance a pose | the instance is removed, or its mesh has no data that deforms |
| update (`PrRenderUpdate`) | `prRenderUpdateBegin` | its commit or its discard |
| a view (`PrRenderView`) and the textures that its parts write | `prRenderViewBegin` and the parts | the frame's graph is begun again |

- **An id is valid at once.** Creating a resource, and adding an instance or a light to an update, return an id that
  other calls may name before the object has data or the update is committed.
- **An id names one object at a time.** A destroyed or removed object's id is given out again only after a frame has
  applied the destruction, so no frame draws a new object under an old reference by accident of timing. An object that
  still refers to a destroyed one is the application's to remove.
- **An id is a record's index.** An instance's id indexes the scene's instance records, and a material's id indexes
  the materials' records. Record zero of the materials is the neutral material that stands in for a missing one.
- **A lost device means every object is made again.** The application destroys the renderer, makes one on the new
  device, and fills it as it did at start-up. The renderer holds no data that survives a device. The host moves its
  texture cache to the new device, and the textures' sources realize there again as the new renderer asks for them.

## The update model

**Rendering never waits.** A frame draws what has arrived and stands in for what has not:

- An instance whose mesh has no data is not drawn.
- A texture that a material names and that has no texels yet reads as one texel of a stand-in value, which the
  material's type declares for that texture. A destroyed texture reads the same. For the Khronos PBR type the stand-ins
  are a white base color, a rough dielectric (roughness 1 and metallic 0, in the channels that glTF reads), the flat
  normal, no occlusion and no emission. So a surface whose images are still loading is never a metal for want of them,
  never glows by its emissive factor alone, and its normal is the mesh's own. The unlit type's stand-in is white.
- A texture that a material does not name is another case: it reads as white, so that the material's factor shows
  alone, as glTF specifies.
- A material that is not set, or that was destroyed, draws as a rough gray.
- An environment without light, and a scene without an environment, are black.

Filling an empty renderer piece by piece is therefore one path for start-up, for streaming and for recovery from a
lost device.

**Everything takes effect at a frame boundary**, which is `prRenderFrameBegin`:

- A resource's data is copied and its GPU objects are made by the call that sets it, on the caller's thread. The frames
  that begin after the call draw it.
- A scene changes only through updates. An update is a transaction: a producer begins it, fills it, and commits it.
  A frame draws all of an update's changes or none of them. Updates that are committed before a frame begins apply in
  the order of their commits.
- Each instance keeps the transform that it had in the frame before, as its previous transform, for passes that need
  motion. An instance that deforms keeps where each of its vertices was, too.

**Threads.** Any thread creates, sets and destroys resources, and begins and commits updates. One thread at a time
fills one update, and several updates of a scene may be open at once. One thread at a time builds frames. A loader on
another thread therefore never shows half of what it loads, and it takes no lock that a frame waits on for longer than
a queue's swap.

**Bulk forms exist for the paths that run every frame.** One call sets the transforms of many instances, and one call
sets every joint of an instance's skin. A pass of the application's may also write the instance records on the GPU
(`prRenderSceneInstanceBuffer`). The renderer then does not see those transforms on the CPU, where it orders blended
draws and fits the shadow map.

## Building a frame

```c
prGpuGraphBegin(graph);
prRenderFrameBegin(render, graph);              // the frame boundary

PrRenderViewDesc desc = prRenderViewDescDefault(scene);
desc.camera = prCameraLookAt(eye, target, fov, aspect, nearDistance);    // pr/camera.h
desc.extent = extent;
PrRenderView* view = prRenderViewBegin(render, &desc);

// One of the library's frames, which calls the parts below. An application that wants another copies it.
PrRenderForwardOptions forward = prRenderForwardOptionsDefault();
PrGpuGraphTexture color = prRenderForward(view, &forward);

// The application's own pass shows the view: here, over the swap chain's texture, with lib/draw's 2D after it.
PrGpuGraphPass pass = prGpuGraphAddPass(graph, &displayPass, drawDisplay, context);
prGpuGraphUseTexture(graph, pass, color, PrGpuAccessFlag_FragmentRead);

prGpuGraphRecord(graph, commands);
```

- **A view is what every part of a frame shares** (`PrRenderView`): the scene, the camera, the extent, the visibility
  mask, the exposure and the meter, and the uniforms that the passes read. It lives until the graph is begun again.
- **A frame is plain code that calls the parts** ([Passes](#passes)). The library ships two frames, the forward one
  (`prRenderForward`) and the visibility one (`prRenderVisibility`), in `pr/render_frames.h`. They are written against
  the public parts alone, in a source that includes no header of the library's own, so each is a frame that an
  application may ship as it is, or copy and change: add an outline between the opaque and the blended surfaces, draw
  shadows its own way, or occlude indirect light before shading.

- **A view's color is scene-referred linear sRGB** times the view's exposure, in half floats, at one sample per pixel.
  The application decides what happens to it next: a pass of its own, or the display draw.
- **A frame may add several views**: two eyes, a reflection, an editor's viewports. The passes that do not depend on
  the camera, such as a scene's shadow map, are added once for all of a frame's views of that scene.
- **The graph decides what runs.** A view whose color no pass reads is not drawn: the graph skips its passes.
- **The display draw** (`prRenderDisplayDraw`) draws a view's color into a pass of the application's, scaled, tone
  mapped and encoded as `lib/draw`'s targets hold color: extended sRGB, encoded. `lib/draw`'s 2D then composes over it
  in the same pass, and the application's usual mapping to its display follows ([draw.md](draw.md)). With the display's
  headroom, highlights keep values above SDR white, and every color up to diffuse white stays where an SDR display
  shows it.

## Light and exposure

Light is photometric, in the units of glTF's `KHR_lights_punctual` and of its emission, which UsdLux shares.

- **A light's intensity is lux for a directional light, and candela for a point or a spot light.** A surface's
  radiance, an emission and an environment's radiance are luminance in nits (cd/m²), each channel of linear sRGB
  weighted as the luminance of white. A producer keeps the unit of its asset: the renderer converts nothing.
- **The sky is daylight.** Its sun's illuminance above the atmosphere is the IES's solar constant, 127.5 klx
  (`kPrRenderSunIlluminance`), so a level surface under a clear sky with the sun high receives some 100 klx and the
  sky's zenith is a few thousand nits.
- **Exposure is a camera's, in EV100.** `prRenderExposureFromEv100` is the reciprocal of the luminance that saturates
  a sensor of ISO 100 by ISO 12232's saturation-based speed, 1.2 × 2^EV100 cd/m². No one way to choose it suits every
  scene, so the renderer offers three, and the application chooses among them and adds its own compensation:
  - **Manual.** The application gives an EV100.
  - **A meter of the scene's light.** `prRenderEv100FromIlluminance` is an incident-light meter with a flat receptor,
    whose calibration constant, 120π lx·s, is within ISO 2720's range for one and is the one at which a white surface
    without gloss that faces the metered light shows as diffuse white. `prRenderSceneIlluminance` reads a scene's lights
    and its environment as the shaders do, at a point, toward what lights it most. It reads no image, so the exposure of
    a frame does not depend on the frame before: a view holds still, and a capture is the same each time. It sees no
    emission, so a scene that its emission lights reads as dark as its lights leave it.
  - **A meter of the image** (`PrRenderMeter`). A view that names a meter reads back what it shows, emission and
    background included, as the mean logarithm of the luminance over each cell of a grid of 64 by 64, divided by the
    view's exposure. The meter sorts the cells and averages the middle eight tenths of what has light, so neither a
    black background nor the sun's highlight moves it, and `prRenderEv100FromLuminance`, a reflected-light meter with
    ISO 2720's constant of 12.5 cd·s/m², turns the mean into an EV100. Sorting 4,096 cells costs microseconds and is
    exact, where a histogram would round each cell to a bin. A reading arrives once its frame's work completes, so it
    lags the frame that it reads, and the exposure is a loop through the frames: `prRenderEv100Adapt` moves an EV100
    toward a reading by a half-life, on the time steps that the application gives, since the renderer reads no clock.
    The loop stays reproducible because each reading is a function of the view's color and its exposure alone: a view
    that holds still converges to one exposure, which a host waits for before it captures, and a test renders, waits
    for its work, and reads, which is exact. Content whose values are the view's, an unlit color or an emission in the
    view's unit, divides by the exposure that shows it, so it pulls the reading toward that exposure; the loop still
    converges while anything else has light, and a view of such content alone is shown the same at any exposure.
- **A view is exposed before its color is stored** (`PrRenderViewDesc::exposure`). Daylight reaches tens of thousands
  of nits, and a smooth highlight of the sun far more than the largest half float, 65,504. Every surface and the
  background are scaled by the view's exposure as they are written, and a surface that reads the scene behind it
  divides the exposure out again. The display draw's exposure is what remains: a tone map's own calibration and a
  user's compensation.
- **An emission's unit is its material's** (`PrRenderEmission`). It is luminance, in nits, which the view's exposure
  scales as it scales light, or the view's own value, a share of diffuse white that no exposure changes: an emission of
  1 is then a value of 1 in the view's color, a fully exposed pixel. glTF's core material defines its emission in nits
  where a physical unit is needed and leaves the conversion to a pixel to the camera's exposure, and many engines read
  an emissive factor of 1 as a fully exposed pixel. An asset authored beside lights of its own wants the first, and one
  authored to glow on any screen wants the second, under a sky of 100 klx that exposes 1 nit as black. A scene may hold
  both kinds of asset at once, which a unit of the view's could not serve, so the unit is a render state of the
  material, as its alpha mode is, which a producer chooses for each asset.
- **An unlit color is the view's, not the scene's.** `KHR_materials_unlit` describes a surface that shows its color
  whatever lights it, so the unlit type emits its color in the view's unit, and a view at any exposure holds the color
  as it was authored.

## Materials and shading

Shaders are Slang. A shading model and a material type are each a structure that implements an interface
(`pr/render_surface.slang`), and the library's passes call them through generic functions that it writes once.

- **A shading model is its surface.** A shading model is a structure that implements `IRenderSurface`, or
  `IRenderSimpleSurface` if it prepares nothing for a pixel. Its fields are the model's surface: what the model reads
  at a point. Its methods take a surface to the radiance that leaves it.
- **A material type is a function** from what a point of a mesh offers (`RenderSurfaceInputs`), the attributes of the
  mesh's vertices that the type declares, a material's record and the textures that the type declares, to the surface
  of a shading model. It is a structure that implements `IRenderMaterial`, which declares the type's parameter record,
  names its shading model, and declares its textures and its attributes.
- **The shader is in charge of what it reads.** A type declares its parameters, by name with their values where a
  material does not set them, the attributes of the vertices that it reads, their formats and their values where a
  mesh lacks them, and the textures that it samples, with what each holds, the channels that it reads and what stands
  in for it. The build reflects those declarations into the shader set, and a producer fills a material and converts
  its data into them on the CPU by their names (`prRenderMaterialParameterSet`, `prRenderVertexLayoutFill`). No shader
  decodes a layout that data describes at run time, and a loader needs no C structure of a type to fill its material.
- **A technique decides where each one runs.** Every technique calls the same material types and the same shading
  models.
- **A technique reads one member of a surface: `alpha`.** The passes that draw depth test it against a masked
  material's cutoff, and the forward pass blends by it. Every other field is between a material and its model. No
  pass, shadow or background reads one, so a technique names no model and no field of a model's.
- **The material types and the shading models of a renderer are its shader set.** The build generates the files of a
  set, which hold a fragment entry point for each variant of a type and the switch on a material's variant, and nothing
  else. The passes include them, and the library's own set is built by the function that builds an application's
  ([Custom materials and shading models](#custom-materials-and-shading-models)).
- **A material samples its textures through the renderer's functions, with explicit derivatives.** A technique that
  shades a pixel away from its triangle's own draw has no implicit derivatives, so no material may depend on them.
- **A technique chooses the derivatives that it gives a material.** A draw's fragment shader takes the fine
  derivative: the difference between a pixel and its neighbor in its block of two by two. A technique that shades away
  from the draw computes the exact derivative at the pixel's center from the triangle's corners. The two numbers differ
  wherever perspective bends a triangle within one pixel, so two techniques may filter one texture at slightly
  different levels. Techniques need not agree on derivatives.
- **A material's render states are glTF's**: an alpha mode (opaque, masked at a cutoff, or blended) and double sided.
  A masked material's alpha is tested wherever its surface is drawn, in depth and shadow passes too, by the same
  material type.

### The library's models

The library ships the Khronos PBR model, `RenderPbrSurface`, and four material types that it lights, which every
shader set has. A set may ask for a second model, OpenPBR Surface, with a material type of its own.

- **`RenderPbrSurface` is the Khronos PBR material model**, which is the material model of glTF 2.0 with Khronos's
  ratified material extensions: metallic and roughness, with a Lambertian diffuse lobe under a GGX specular lobe, and
  the layers that the extensions add, lit by the scene's lights and its environment.
- **Its fields are the inputs of glTF's core material and of the extensions' layers** (`pr/render_brdf.slang`): base
  color, alpha, metallic, roughness, the strength of a dielectric's reflection, a normal, occlusion and emission, and
  each layer's own. `pr/render_shading.slang` makes the structure a shading model, with the methods that light it.
- **An extension is a layer of this one model**, by the rule that its specification gives: a clear coat and a sheen
  over the base, an index of refraction, a specular color and a thin film in its Fresnel terms, two widths of its
  specular lobe, and light that passes through it, with the volume that it encloses. A surface without layers is shaded
  by the core model's arithmetic alone: the layers are behind one test, which a type that sets none of them folds away.
- **The glTF material type**, `RenderMaterialKhronosPbr`, computes the core surface from base color, metallic-roughness,
  normal, occlusion and emissive textures, each with a transform of its coordinates and a choice of the vertex's two
  sets of coordinates, and an emissive strength. The library's types read glTF's attributes, `TEXCOORD_0`,
  `TEXCOORD_1` and `COLOR_0`, so they share one layout. It samples five textures, each in the slot of its index, and its
  shader holds no layer.
- **The layered glTF material type**, `RenderMaterialKhronosPbrLayered`, computes the same surface with the layers of
  `KHR_materials_ior`, `_specular`, `_clearcoat`, `_sheen`, `_iridescence`, `_anisotropy`, `_transmission`, `_volume`,
  `_dispersion` and `_diffuse_transmission`. It samples a layer's textures only where the material's factor turns the
  layer on. It also draws the archived specular-glossiness workflow by its own definition, as this model's dielectric
  with a colored reflection. A loader chooses the core type for a material that sets nothing that only the layered type
  holds, by the names of the two types' parameters and textures. An application that knows the library's types sets
  them from their descriptions (`prRenderMaterialSetKhronosPbr`, `prRenderMaterialSetKhronosPbrLayered`, which it
  chooses between with `prRenderKhronosPbrMaterialIsLayered`).
- **The unlit material type**, `RenderMaterialUnlit`, is a surface of the same model that reflects nothing and emits
  its color times its texture, so no light changes it.
- **The preview surface type**, `RenderMaterialUsdPreview`, is OpenUSD's UsdPreviewSurface as a surface of the same
  model: its metallic and its specular workflows, a clear coat, emission, opacity with its threshold, occlusion and a
  normal map. Its inputs are not glTF's: each reads a texture of its own, in the channel that its network reads, with
  UsdUVTexture's scale and bias, so an asset's textures draw without being packed into glTF's channels. Its emissive
  color is times an optional attribute of the vertices, which a mesh carries only where a material binds a primitive
  variable to it ([Attributes of the vertices](#attributes-of-the-vertices)).
- **OpenPBR Surface is a model that a set lists as it lists any type** (`MATERIAL OpenPbr STRUCT
  RenderMaterialOpenPbr`): a surface of its own that lights itself (`pr/render_openpbr.slang`), with a material type of
  OpenPBR's inputs, named as the specification names them, run together (`pr/render_material_openpbr.slang`). A loader
  finds it by its name (`prRenderMaterialTypeFromName`), its parameters and its textures by theirs
  (`prRenderMaterialParameterFromName`, `prRenderMaterialTextureFromName`), and needs no header of the application's. A
  set without it holds nothing of that model.
- **The OpenPBR type's inputs are what a MaterialX graph computes**: each texturable input reads a texture of its own,
  in the channels that the graph reads, scaled and biased as the graph scales them, and takes that value in place of
  its own, as the preview type's inputs do. A graph that reads roughness and metalness from two images, or one channel
  of an image of four, draws as it is written, without its images being packed into a layout of the type's. The
  textures that matter most come first, so a device with slots samples the base color, the normal, the roughness, the
  metalness and the opacity.
- **The models share their lobes** (`pr/render_lobes.slang`): Fresnel terms, the GGX lobe with one width and with two,
  the sheen and fuzz lobes, the diffuse lobes and a thin film's interference. The Khronos PBR model mixes them by
  Fresnel terms, as glTF's extensions specify, and OpenPBR by albedo scaling, as its specification mandates. The
  functions read numbers and directions alone, so an application's model may call them too.
- **A surface that transmits light reads the scene behind it** (`pr/render_behind.slang`): the camera's view of the
  surfaces that do not blend, with the background, blurred level by level, which the surface shows bent and blurred by
  its roughness. A material asks for it (`PrRenderMaterialFlag_ReadsBehind`) and is then drawn with the blended
  surfaces ([Techniques](#techniques)). The renderer makes the texture only for a view of a scene that has such a
  material. The texture holds the opaque scene alone, so such a surface does not show another behind it.
- **A shading model with a surface of its own is for a model that is a different one**, such as OpenPBR or the toon
  model of `examples/render_features`. A glTF extension grows `RenderPbrSurface`, its methods and the layered type.
- **`RenderPbrSurface` has no standing that another model lacks.** The build reads it as it reads an application's
  model, and no pass names it.

The Fresnel mix of the model's two lobes follows glTF's definition, by the half vector of each light. It conserves
energy for a metal at every angle. For a dielectric seen at a grazing angle it returns more than it receives, as that
definition does. A surface between a dielectric and a metal is glTF's mix of the two: its diffuse lobe is the
dielectric's, under the dielectric's own reflection, whatever the metallic value.

## Custom materials and shading models

An application adds material types and shading models at build time. It changes no source of the library and copies
none of its passes. A CMake function compiles the library's pass sources with the application's Slang sources into a
shader set, and the application creates its renderer with that set. The same sources serve both techniques and both
binding tiers.

### Adding a material type

1. **Write the material type** in a Slang source: a structure that implements `IRenderMaterial`. It declares its
   parameter record, each field marked with its value where a material does not set it, names the shading model that
   will light it, here the library's, declares the textures that it samples and the attributes of the vertices that it
   reads, and computes that model's surface. A record that C compiles too marks its fields with `PR_RENDER_PARAMETER`
   (`pr/render_records.h`), which C reads as nothing.

   ```slang
   #include "pr/render_material.slang"
   #include "pr/render_shading.slang"

   struct MyStripes : IRenderMaterial {
       typedef RenderPbrSurface Surface;

       // The colors of the stripes, linear, and their pairs to one unit of the distance.
       struct Parameters {
           [RenderParameter(1, 1, 1, 1)] float4 first;
           [RenderParameter(0, 0, 0, 1)] float4 second;
           [RenderParameter(8, 0, 0, 0)] float count;
           float _pad[3];
       };

       struct Textures {};

       // The distance along the stripes, which a vertex without it has at zero.
       struct Attributes {
           [RenderVertex("custom", "float", 0, 0, 0, 0)] float along;
       };

       static Surface surface(RenderSurfaceInputs inputs, Attributes attributes, Parameters parameters,
               Textures textures) {
           RenderPbrSurface surface = renderPbrSurfaceDefault(inputs.normal);
           float stripe = frac(attributes.along * parameters.count);
           surface.baseColor = stripe < 0.5 ? parameters.first.rgb : parameters.second.rgb;
           surface.specular = 1;
           return surface;
       }
   }
   ```

2. **Declare the set** in CMake, and link it. The set is a static library. `MATERIAL` gives the type its name in C,
   and `STRUCT` names the Slang structure.

   ```cmake
   pr_render_shaders(my_shaders My
       SOURCES my_materials.slang
       MATERIAL Stripes STRUCT MyStripes)
   target_link_libraries(my_app PRIVATE my_shaders)
   ```

3. **Create the renderer with the set, and materials with the type's id**, whose records a producer fills by the
   names of the type's parameters. The build generates `render_shaders/MyShaders.h`, which declares the set and an id
   for each type, with the index of each of its parameters, its textures and its attributes.

   ```c
   PrRenderOptions options = prRenderOptionsDefault();
   options.shaders = myShaderSet();
   prRenderCreate(device, &options, &render);

   const PrRenderMaterialType* stripes = &myShaderSet()->materialTypes[kMyMaterialStripes];
   uint8_t record[48];
   prRenderMaterialParametersDefault(stripes, record);
   const float count = 12;
   prRenderMaterialParameterSet(stripes, record, kMyStripesParameterCount, &count, 1);
   PrRenderMaterialDesc desc = prRenderMaterialDescDefault(kMyMaterialStripes, record, stripes->parameterBytes);
   prRenderMaterialSet(render, material, &desc);
   ```

4. **Give a mesh the type's attributes**, converted from the producer's own formats into the type's layout
   ([Attributes of the vertices](#attributes-of-the-vertices)).

   ```c
   const PrRenderShaderSet* set = prRenderShaderSet(render);
   const PrRenderVertexLayout* layout = prRenderMaterialTypeLayout(set, kMyMaterialStripes, 0);
   PrRenderAttributeSource along = {PrRenderSemantic_Custom, 0, "along",
       {distances, count, sizeof(double), {.component = PrMeshComponent_Float64, .count = 1}}};
   prRenderVertexLayoutFill(layout, &along, 1, count, bytes, NULL);
   PrRenderMeshStream stream = {layout, bytes};
   ```

That is four lines of CMake and about thirty of Slang. `tools/package` builds this set against the
installed package, and `examples/render_features` builds a larger one.

### The material type's contract

`IRenderMaterial` (`pr/render_surface.slang`) has five members, and the compiler holds a material type to them.

- **`Parameters` is the structure of the type's parameter record**, which C fills and the shaders load. Each field is
  a parameter marked `[RenderParameter(x, y, z, w)]` with its value where a material does not set it, or padding named
  `_pad`. The build reflects each parameter's name, offset, components and value (`PrRenderMaterialParameter`), so a
  loader fills a record of any type by name, starting from the declared values (`prRenderMaterialParametersDefault`).
- **`Surface` names the type's shading model**: a structure that implements `IRenderSurface`, such as
  `RenderPbrSurface`.
- **`Textures` is a structure of the type's textures**, each a `RenderTexture` marked with what it holds and its
  stand-in: `[RenderColor(r, g, b, a)]` for color, or `[RenderData(r, g, b, a)]` for values that a shader reads as
  stored. A texture's color is its transfer alone: the renderer samples a color texture through its Srgb form and a
  data texture through its plain form, and no texture carries primaries, so the shader reads its values as linear
  sRGB, the color of a view. A second mark, `[RenderReads("gb")]`, names the channels that the type reads, or
  `"chosen"` for the one that a material chooses for a scalar input, so a loader may store those alone; a texture
  without it reads all four (`PrRenderMaterialTexture::channels`). The build fills it, so a texture's index is a
  constant of the code.
- **`Attributes` is a structure of the attributes of the vertices that the type reads**, each marked
  `[RenderVertex(semantic, format, x, y, z, w)]`: `"uv"` for a set of texture coordinates, a `RenderTexCoord` with its
  derivatives, `"color"` for a color, and `"custom"` for an attribute that the type alone knows, matched by its name.
  The format is one of lib/gpu's vertex formats, and the four numbers are its value where a mesh lacks it. A second
  mark, `[RenderOptional]`, makes the attribute optional: a mesh carries it only where a material binds it.
- **`surface` receives** the point of the mesh (`RenderSurfaceInputs`: the position, the normal and the tangent in the
  world, the triangle's own normal, and the ids of the instance and the material), the attributes there, the
  material's parameter record, and the material's textures.
- **The triangle's own normal is `geometricNormal`**: the unit normal of the triangle's plane, on the side of
  `normal`. The forward technique takes it from the differences of the position between neighboring pixels, and the
  visibility technique from the triangle's corners. A material uses it to keep a mapped normal from facing away from
  the eye, or to shade a faceted surface.
- **`surface` returns the surface of the type's shading model.** `renderPbrSurfaceDefault` gives a `RenderPbrSurface`
  that reflects and emits nothing, for a type that sets only some fields.
- **A type serves one model.** An application that wants a material lit by a second model writes a second type, whose
  `surface` may call the first's and fill the other surface from what it returns.
- **It samples its textures through `renderMaterialSample`, `renderMaterialScalar` and `renderMaterialNormal`**, which
  take the derivatives from a `RenderTexCoord`, and the set of coordinates, the channel, the scale and the bias that
  the material names for the texture. It calls no `Sample` of its own and takes no `ddx`. The
  visibility technique shades a pixel away from the draw of its triangle, where no implicit derivative exists, and
  computes the derivatives there itself. The derivatives are the technique's choice, and a material must not expect
  the same numbers from every technique. A material that filters a pattern of its own, such as the checker of
  `examples/render_features`, filters it by the derivatives of its texture coordinates.
- **It declares no slots and no root data.** Every texture that it samples is one of the material's textures. A device
  without bindless binds those for each draw, and the passes have no other slot to give. A slot is a property of a
  whole pass source, so a slot of a material's would be a slot of every draw of every material. Numbers that many
  materials share are a table of the shader set ([Tables](#tables)).
- **It may read the scene's time** with `renderSceneTime`. An update sets the time (`prRenderSceneTimeSet`), so a frame
  draws materials at the time of the transforms of that update. The time is the application's number. The renderer
  has no clock, and an application wraps the time at the period of its materials, because a float loses precision as
  it grows.
- **It is the one source of the material's alpha.** A masked material's alpha is tested in the depth pass, in the
  shadow map and in the visibility pass by calling the same `surface` and reading `alpha` of what it returns. The
  compiler removes what the alpha does not depend on. There is no second function for alpha, so the pass that tests
  and the pass that shades cannot disagree.
- **It must compute the same surface in every pass.** It reads nothing but its arguments, the library's functions and
  the scene's time.

### Limits and layout

- **A parameter record has the size of its structure**, in a multiple of 16 bytes. It holds floats, 32-bit integers,
  vectors of two or four of them, and arrays of those. Each field starts at a multiple of its alignment without help:
  16 bytes for a 4-vector, 8 for a 2-vector. It has no 3-vector, matrix or boolean. Those are the rules of every record
  of `pr/gpu.slang`, under which C and each shading language read the same bytes. A color of three components is a
  4-vector whose fourth the type does not read.
- **A type declares its textures** in its `Textures`, in the order of their indices. A material reads a texture's
  stand-in while the texels have not arrived. The number of textures has no limit.
- **A material costs what its type declares.** A material's record on the GPU is 32 bytes. It names where the
  materials' buffer holds the material's textures, 96 bytes each, and its parameter record
  ([Records and the attributes](#records-and-the-attributes)). A material of a type with one texture and a
  small record does not pay for a type with thirty.
- **A device without bindless samples 5 textures of one material**, the slots that the passes declare, and one fewer
  of a material that reads the scene behind it ([Binding tiers](#binding-tiers)). A type with more textures lists first
  the ones that matter most, and a loader asks `prRenderMaterialTextureLimit` what the renderer's device samples.
- **Types are numbered in the order of their declaration**, after the library's four. An id is valid for the set that
  declares it, and a loader that does not know the set finds a type by its name. A material whose type the renderer's
  set lacks draws as the Khronos PBR type.
- **A set is fixed when the program is built.** A renderer has one set for its life.

### Attributes of the vertices

A mesh's vertices have a geometry, which every pass reads, and the attributes that its materials' types read, in the
layout of each type.

- **The geometry is the renderer's** (`PrRenderGeometryVertex`, 28 bytes): a position as floats, and a normal and a
  tangent with its handedness as signed 16-bit values. Every pass reads it, deformation writes it, and no material
  type declares it.
- **A type's attributes are a layout** (`PrRenderVertexLayout`): each attribute's semantic, its format, its offset and
  its fallback, packed in the order of the declaration. Types whose declarations are equal share a layout, so the
  library's four types are one layout, and a pipeline is made for a layout and not for a type where it reads the
  attributes alone.
- **Only bound attributes cost.** A type's optional attributes (`[RenderOptional]`, at most four) make variants of the
  type, one for each set of them (`PrRenderMaterialVariant`). A variant's layout holds the attributes that are not
  optional and those of its set, and its shaders hold the value of each other optional attribute, where a vertex lacks
  it, as a constant. A material names the set that its meshes carry (`PrRenderMaterialDesc::attributes`), which chooses
  the variant (`prRenderMaterialTypeLayout`). So the preview type's variant without its emissive color is the Khronos
  types' layout, and a mesh pays for the attribute only where a material binds a primitive variable to it. A variant
  has pipelines of its own, made only where a material draws with it, and the dispatches of the masked depth and of
  the visibility technique switch on the variant (`PrRenderMaterialRecord::variant`).
- **A mesh has a stream of each layout that its parts draw with** (`PrRenderMeshStream`). A producer converts its own
  data into a layout with `prRenderVertexLayoutFill`, from sources that it names by semantic or by name, in any of
  lib/mesh's formats (`prMeshAttributeConvert`): doubles become floats, 64-bit integers become 32-bit ones, and an
  attribute that no source gives is its fallback. The shader reads the layout that it declared, whatever the source
  held.
- **A draw of a layout that its mesh lacks reads the fallbacks**, which the renderer makes a stream of once for the mesh
  and the layout. A scene counts such draws (`prRenderSceneFallbackDrawCount`), so a producer learns of attributes
  that it did not give.
- **The forward technique interpolates attributes in hardware.** Each layout has a vertex shader that fetches the
  geometry and the attributes and passes them to the fragment shader of each type of the layout. The visibility
  technique pulls the three corners of a pixel's triangle from the draw's stream in the case of the type's dispatch,
  and interpolates them with their derivatives.

### Adding a shading model

A shading model is a structure whose fields are the model's surface, and whose methods return the radiance that
leaves such a surface toward the eye. There are two interfaces. A model that computes each light from the surface
alone implements `IRenderSimpleSurface`:

```slang
#include "pr/render_material.slang"
#include "pr/render_shading.slang"

struct MyToonSurface : IRenderSimpleSurface {
    float3 color;
    float alpha;
    float3 normal;
    float bands;

    float3 directRadiance(float3 toEye, RenderShadingLight light) {
        float level = ceil(saturate(dot(normal, light.toLight)) * bands) / bands;
        return color * level * light.irradiance * light.visibility;
    }

    float3 indirectRadiance(float3 toEye) {
        return color * renderEnvironmentIrradiance(normal);
    }
}

struct MyPaint : IRenderMaterial {
    typedef MyPaintParameters Parameters;
    typedef MyToonSurface Surface;

    static Surface surface(RenderSurfaceInputs inputs, Parameters parameters, RenderMaterialTextures textures) { ... }
}
```

```cmake
    MATERIAL Paint STRUCT MyPaint
```

A model with terms that depend on the surface and the eye alone implements `IRenderSurface`, which adds a stage that
runs once for a pixel:

```slang
struct MyGlossContext {
    float nv;           // the cosine of the normal with the eye
    float3 albedo;      // what the gloss reflects toward the eye of an even light
};

struct MyGlossSurface : IRenderSurface {
    typedef MyGlossContext Context;
    float3 color;
    float alpha;
    float3 normal;
    float roughness;

    Context prepare(float3 toEye) { ... }
    float3 directRadiance(Context context, float3 toEye, RenderShadingLight light) { ... }
    float3 indirectRadiance(Context context, float3 toEye) { ... }
    override float3 emittedRadiance(Context context, float3 toEye) { ... }
}
```

- **The library shades a pixel in two stages** (`renderShade`). It calls `prepare` once with the direction to the eye,
  and passes the context that `prepare` returns to `indirectRadiance`, to `directRadiance` for each light, and to
  `emittedRadiance`. It sums what they return.
- **The context holds what no light changes**: a lobe's albedo toward the eye, a Fresnel term at the eye's angle, the
  terms of a thin film. A model computes them in `prepare`, not in `directRadiance`, which runs for each light. The
  compilers do not move them out of the loop over lights. For the OpenPBR model's first form, the SPIR-V of the
  forward shader had 2,858 instructions with the stage, and 3,575 with the same terms computed in `directRadiance`:
  the loop's body holds a second copy of them.
- **A simple model is a model with an empty context.** The library makes every structure that implements
  `IRenderSimpleSurface` an `IRenderSurface`, so a material type names either as its `Surface`.
- **The build description does not name a model.** A model is in a set because a material type of the set names it as
  its `Surface`. One scene mixes models, since each draw's material decides.
- **The surface holds what a material gives the model, and the context what the model derives from it.** A material
  fills every field of the surface and no field of the context, so a material pays only for the fields of its own
  model.
- **A surface has a float field `alpha`**: the part of the point that the surface covers. It is the one member that
  the passes read. A model whose surfaces are always opaque sets it to 1.
- **A `RenderShadingLight` holds** the direction to the light, its irradiance on a plane that faces it, the part of it
  that the scene's shadows let through, and the angular radius of its source as the point sees it. The shadow is apart
  from the irradiance so that a model may treat a shadow in its own way. The angular radius lets a model widen a
  smooth lobe's highlight to the source's size. A directional light's is the one that softens its shadows, and a point
  or spot light's follows from the radius of its source (`PrRenderLightDesc::radius`), which is zero for a light
  without size.
- **Occlusion and emission are the model's.** A model that has occlusion dims the environment's light by it in its
  own `indirectRadiance`. A model that emits defines `emittedRadiance` with the keyword `override`, which the compiler
  requires of a method that replaces a default. The method takes the direction to the eye, since a coat over an
  emitter passes less of it toward a grazing angle.
- **The methods are named for what they return**, so that a field of a surface may be called `emission` or `direct`.
- **A structure of another header becomes a model by an extension.** `RenderPbrSurface` is plain data in
  `pr/render_brdf.slang`, which any shader may include, and `pr/render_shading.slang` extends it with the methods.
- **A shading model declares no slots either.** It reads the lights that it is given, and the scene's environment
  through `pr/render_environment.slang`.
- **The library's own model shades a light as a source without size.** `RenderPbrSurface` does not read the angular
  radius. Its lobe is never narrower than a least width (`renderBrdfAlpha`), as before the lights carried a size.

### Tables

A shading model or a material type may read lookup tables: a lobe's albedo by roughness and angle, the matrices of a
linearly transformed cosine, the terms of an energy compensation. A table is a resource of the shader set.

```cmake
pr_render_shaders(MyShaders My
    SOURCES my_materials.slang
    TABLE Sheen EXTENT 32 32
    MATERIAL Cloth STRUCT MyCloth)
```

```c
// Writes four floats for each texel, row after row.
static void fillSheen(void* context, PrUVec2 extent, float* texels) { ... }

PrRenderTableSource tables[] = {{.table = kMyTableSheen, .fill = fillSheen}};
PrRenderOptions options = prRenderOptionsDefault();
options.shaders = myShaderSet();
options.tables = tables;
options.tableCount = 1;
prRenderCreate(device, &options, &render);
```

```slang
float albedo = renderTableSample(kRenderTableSheen, float2(roughness, nv)).r;
```

- **A set declares each table** with `TABLE`: a name, and a width and a height in texels. The build defines the
  constant `kRenderTable<Name>` for the set's sources, and the table's id, `k<Name>Table<Table>`, in the set's header.
- **The application gives each table's function when it creates a renderer** (`PrRenderOptions::tables`). The function
  computes the texels or copies them from an array, and takes a context of the application's.
- **The renderer owns the tables.** `prRenderCreate` calls each table's function once and uploads the result. A table
  holds four half floats a texel.
- **A shader reads a table with `renderTableSample`** (`pr/render_table.slang`), at two coordinates from 0 to 1. The
  first texel of an axis is at 0 and the last at 1, so a function tabulated at evenly spaced values, ends included,
  reads back exactly at those values and linearly between them.
- **The shader interpolates the four texels itself**, which it reads by their indices. A device's own filter weighs
  texels with 8 bits or so, which vary by device. With it, two surfaces that differ in the last bit of a normal
  could differ by a step of the table, and a test that compares two such renders exactly failed. Both binding tiers
  and every device now read the same value.
- **Every table of a set is a region of one texture**, one under another. With bindless, the uniforms hold its handle.
  With slots, it takes one texture slot, however many tables the set has.
- **The limits that one texture imposes**: every table has the same format, four half floats. A table has two axes,
  so a function of three variables is a table for each step of the third, which the model interpolates between. The
  sum of the tables' heights, and the widest table, fit the device's largest texture, which is 8192 texels or more.
  A renderer whose tables do not fit fails to create with `PrStatus_Limit`.
- **The library's own table is every set's first**: the split sum of its specular lobe
  ([Environments](#environments)), which `renderSplitSum` reads for the library's model and for any model that shares
  its GGX lobe, as the library's OpenPBR model does, and the albedo of glTF's sheen lobe beside it, which
  `renderSheenAlbedo` reads for the sheen's layering.

A surface exists only between a material type and its model. A technique that stores surfaces, such as a deferred
one, has no layout of its own to impose: each model says how its surface is packed into the technique's targets and read
back, and the application's shader set declares the targets. The library fixes no G-buffer.

### What the build generates and checks

`pr_render_shaders(<target> <Name> ...)` adds the static library `<target>`, which is the shader set `<Name>`.

- **A set is a target that others link.** The target that creates the renderer links it, and so does every target
  that names the set's material types or tables: a library of an application's scenes, and its tests. Each gets the
  directory of the set's header, and `prax::render`, from the link.
- **The library's own set is part of `lib/render`**, since a renderer that is created with no set draws with it.

The function generates, in the build directory of the directory that calls it:

- **`render_shader_sources.slang`**, which includes the library's material types and the set's sources, and defines
  a constant for each of the set's tables.
- **`render_shader_set.slang`**, which holds what the build reflected of the types: the code that fills each type's
  `Textures`, the vertex, the varyings and the pulled values of each layout, each variant's `Attributes` from those,
  with the values of the optional attributes that it lacks, and the dispatch on a material's variant that the
  visibility technique and the masked depth call. No pass names a type or a model. The dispatch groups the variants by
  their shading model, so that a shader holds each model's shading once.
- **`render_shader_forward.slang`**, the source of the forward pass's library, with a vertex shader for each layout and
  a fragment entry point for each variant of each type. Each is one call of the library's generic fragment function with
  the type's structure.
- **`render_shader_depth_mask.slang`** and **`render_shader_visibility.slang`**, the sources of the masked depth's
  library, with a vertex and a fragment shader for each layout, and of the visibility pass's.
- **The shader libraries** of the three passes, each for bindless and for slots. The renderer's other passes are the
  same for every set and stay in the library.
- **`render_shaders/<Name>Shaders.h`** for C and C++, with the ids `k<Name>Material<Type>` and `k<Name>Table<Table>`,
  the index of each type's parameters, textures and attributes, `k<Name><Type>Parameter<Field>`,
  `k<Name><Type>Texture<Field>` and `k<Name><Type>Attribute<Field>`, and the function `<name>ShaderSet`.

The loop over the lights, the forward fragment shader and the call of a type's `surface` are generic Slang in the
library's headers. The compiler specializes them for each type, so the compiled shaders hold no generic code.

A mistake in a set fails the build, before the passes compile. The Slang compiler reports a structure that does not
implement its interface. Its message names the structure's line in the application's source, and the member of the
interface that the structure lacks or does not match:

- a material type without `Parameters`, `Surface` or `surface`, or whose `surface` takes other parameters;
- a `surface` that returns another structure than the type's `Surface`;
- a `Surface` that is not a shading model;
- a shading model without `alpha`, `directRadiance` or `indirectRadiance`, or with one of another form, and a model
  with a context that lacks `Context` or `prepare`;
- an `emittedRadiance` marked `override` that does not have the form of the interface's.

The build reports the rest, by reading the compiler's reflection of the set and its syntax tree of the sources:

- a `STRUCT` that the sources do not define, or that does not implement `IRenderMaterial`;
- a parameter record with a 3-vector, a matrix or a boolean, with a field that C would move, or of a size that is not
  a multiple of 16 bytes, and a parameter that is not marked `[RenderParameter]`;
- a source that declares a texture, a buffer or root data of its own;
- a texture that is not a `RenderTexture` marked `[RenderColor]` or `[RenderData]`, or whose `[RenderReads]` names
  other than channels in order or `chosen`, and an attribute that is not marked `[RenderVertex]`, whose semantic is
  none of uv, color and custom, whose format is not a vertex format, or whose type is not the one that its format
  reads as, and a type of more than four optional attributes;
- a member of a structure that is named `emittedRadiance` and is not marked `override`: a method of another form than
  the interface's, or a field. The compiler takes such a member for one of the structure's own and calls the
  interface's default, so the model would emit nothing without a word. The message names the member's line in the
  application's source;
- a type without a `STRUCT`, or a table without an extent, when CMake configures.

A method that an interface requires cannot be missed in this way: the compiler reports a `directRadiance` or a
`prepare` of another form as missing. Only a method with a default can stand unused beside it, and `emittedRadiance`
is the one such method. The check reads the syntax tree that slangc dumps, in the form that the pinned version of
slangc writes, and fails if a tree does not show the library's own methods.

The size of each type's record, as the shaders load it, and each parameter's offset are in the set.
`prRenderMaterialSet` asserts that a description's record has that size, which catches a C structure that has drifted
from its Slang twin.

The installed package has the function too. `find_package(prax COMPONENTS render)` defines `pr_render_shaders`, which
compiles the installed pass sources for the backends that the package was built with, with the Slang compiler that
the package's configuration fetches.

### Where a material's type is chosen

- **The forward technique draws each material type with a pipeline of its own.** The fragment shader of a type's
  pipeline holds that type's material and its shading model alone. It is smaller than a shader of every
  type, and the compiler folds what the type holds constant.
- **The visibility technique chooses in the shader.** Its shading pass shades whatever each pixel shows, so one shader
  must shade any material, and it switches on the material's variant. A switch in one shader is one way to shade many
  materials there. There are others, such as a pass for each type that a stencil or depth test selects.
- **The masked depth chooses in the shader too**, among the types of a layout: a pipeline for each layout fetches its
  attributes, and its fragment shader switches on the types that share it. It evaluates a material for its alpha
  alone, so its shader is small whatever the set holds. A pipeline for each type would multiply the depth pipelines by
  the types, in the view and in the shadow map, to remove one switch.
- **The cost of a pipeline for each type is the forward library's size.** It holds a shader for each type, so it grows
  with the set, where the shader that switches shared the code of each model among its types.

## Environments

An environment is the light that arrives from every direction around a scene, as scene-referred linear sRGB. A scene
has one, turned and scaled for it by a color (`PrRenderSceneEnvironment::scale`), so a light's tint and intensity
apply without changing the environment. It lights the scene's surfaces, and it is the background of the scene's views.

- **Three sources, one form.** An environment is set from a texture source (`prRenderEnvironmentSetSource`), whose
  texture is an equirectangular image of a 2D texture or the six faces of a cube, or from an analytic sky. A source of
  any format serves: the renderer reads it as linear floats, decoding a Srgb form's values, so an HDR or EXR file that
  a texture format set opens, a KTX 2.0 cube, or an application's own pixels light a scene alike. Each becomes the
  first level of a cube of half floats. An equirectangular image is averaged down to the cube's texels, so that a small
  bright light keeps its energy.
- **Setting an environment never waits.** The source's read completes later, on its provider's thread, and the cube
  is made and filtered then, as a texture's realization is. Until it arrives, the environment keeps the light that it
  had, or none (`prRenderEnvironmentIsReady`). A model's environment and an application's are set the same way: no
  caller decodes an image to light a scene.
- **The specular lobe reads a filtered cube.** Each level after the first holds the environment as the GGX lobe of a
  rougher surface gathers it, up to a roughness of 1 at the last level, which is 8 texels wide. The device filters the
  levels by importance sampling when the environment is set, in a command buffer of its own that the call submits. No
  frame builds or waits for it, and the frames that begin after the call read the result.
- **The diffuse lobe reads nine colors.** The environment's diffuse light is its spherical harmonics of three bands,
  which the calling thread sums from the cube's first level and the uniforms carry. They hold a light that varies
  slowly exactly, and a light with one bright spot roughly.
- **The two lobes are combined by the split sum**, with the light that bounces between microfacets more than once added
  back (Fdez-Aguera, JCGT 2019), so that a white surface in an even environment returns all that it receives at any
  roughness and any metallic value.
- **The scale and bias of the split sum are a table** of 32 by 32 entries, by the roughness and the square root of
  the cosine of the eye's angle (`renderSplitSum`). Each entry is the lobe's integral over the directions of light,
  with the sheen lobe's albedo by the same axes beside it, which `tools/gen/gen_render.py` computes and the library
  holds as 6 KB of constants. Read between its entries, the
  table is within 0.0064 of the integral for the scale and 0.017 for the bias, and within 0.0007 and 0.0002 on
  average. The largest errors lie where a smooth surface is seen at a grazing angle.
- **An environment costs one texture slot.** The cube is the only texture of an environment: the harmonics need none.
  With the shadow map, the five textures of a material and the shader set's tables, a device without bindless binds 8
  textures for a draw, which is all that the smallest device has, and both binding tiers run the same code.
- **Occlusion dims the environment's light alone.** A material's occlusion scales what the environment gives a surface,
  diffuse and specular, and nothing of a light's.

### The sky

The analytic sky is the light of a clear day as a viewer on the ground sees it, over a ground that reflects evenly.

- **The light has the magnitude of daylight.** Air scatters sunlight (Rayleigh), haze scatters it forward (Mie), and
  ozone absorbs it. The light that reaches the viewer after one scattering is integrated along each direction (Nishita
  et al., 1993). The light that scatters more than once, and the ground's light that the air scatters back, are added
  by Hillaire's approximation (EGSR 2020). Together they are a third or more of a clear sky's light, so a sky without
  them lights a shadow too little, and a scene needs no factor to make up for it.
- **A haze of 1 is a clear day** as the Illuminating Engineering Society's daylight tables state one (IES RP-21): the
  sun's illuminance on a plane that faces it is 127.5 klx times exp(-0.21 m) at an air mass of m. The haze's amount
  was chosen for the model's sun to be that. The sky's share of a level surface's light then follows: 15% with the sun
  60 degrees up, 22% at 30 degrees, and about half at 10 degrees.
- **Red, green and blue are three wavelengths**, and sunlight is white above the atmosphere. On the ground the sun is
  then warmer than the display's white and the sky bluer, as they are, and the two together are close to white.
- **The sun lights a surface once.** `prRenderSkySunIrradiance` gives the sunlight that the atmosphere lets through,
  for a directional light of the scene, which casts the shadows. The environment's cube and its harmonics hold the sky
  and the ground without the sun, so no surface gets the sun's light from both. A mirror shows the sun as the
  directional light's highlight, which carries the same light, and which a shadow hides as it hides the sun.
- **The sun's disc is drawn in the background alone**, by the function that draws the environment behind a view, at
  any resolution of the cube. Its radiance is the directional light's irradiance over the disc's solid angle, so the
  two are one light. It darkens toward its edge by the linear law of limb darkening, with one constant for every
  color.
- **Making a sky costs a few milliseconds on the calling thread**: its tables are made for each call, since the sun's
  place and the haze change them.

## Records and the attributes

`pr/render_records.h` defines the records once, for C, C++ and Slang: instances, draws, mesh parts, materials, lights,
and the root data of the library's draws. Every record lays out as `pr/gpu.slang` requires, so each language reads the
same bytes.

- **A draw is one part of one instance.** A scene's draws are a buffer of records in the order that passes issue them,
  which the renderer builds again only when an instance, a mesh or a material's render state changes.
- **A mesh's vertices are its geometry and a stream of attributes for each layout**
  ([Attributes of the vertices](#attributes-of-the-vertices)). Indices are 32 bits. A mesh has one or more parts, each
  a range of its indices with a bounding sphere, so that a glTF mesh with several primitives is one mesh.
- **A part names its geometry, and a draw its attributes.** A part's record holds the buffers of its geometry and
  indices, and the first of its vertices in that buffer. A draw's record holds the stream of its material's layout and
  the first of the part's vertices there, since one part draws with the layouts of the materials of its instances. A
  pass binds the vertex buffers at that vertex, and a shader that pulls vertices adds it, so that many meshes' vertices
  may share one buffer and no draw needs a base vertex.
- **Ids are carried in data.** A draw's index is in its push data, never in a base instance or a base vertex, and a
  shader that needs a triangle's index pulls its vertices from vertex zero. The APIs disagree about the rest.
- **A material's size is its type's.** The materials' buffer begins with a record of 32 bytes for each material id:
  the type, the render states, and two offsets into the same buffer. The first offset leads to the material's
  textures, a record of 96 bytes for each texture that the type declares, with the texture's handles, its slot, its
  transform, the set of coordinates and the channel that it reads, its scale and bias, and its stand-in value. The
  second leads to the type's parameter record. A texture's sampler is its use's (`PrRenderTextureUse::sampler`), so
  two materials may read one texture differently, and whether its values decode as sRGB is the form that the texture
  was set to be read as, color or data.
- **A pass of the application's may write a material's parameters on the GPU.** It reads the offset from the
  material's record in the same pass, because the renderer writes the whole buffer again when a material or a texture
  changes, and what the pass wrote is then replaced by the parameters that the material was set with.

## Deformation

A mesh may have a skin, morph targets, or both. An instance of it deforms once an update gives it a pose: the matrices
of its joints (`prRenderInstanceSetJoints`), the weights of its morph targets (`prRenderInstanceSetMorphWeights`), or
both.

- **A deformed mesh is content that the GPU produces.** A compute pass of the frame reads the mesh's vertices as they
  were bound, their skin and their morph targets, and the instance's pose, and writes geometry of the instance's own.
  Deformation moves the geometry alone: the attributes that a material reads do not change with a pose. The renderer
  adds the pass with a frame's first view of a scene.
- **No other pass knows of deformation.** A deforming instance has parts of its own, which name its vertices and
  bound them in its pose, and its draws name those parts. So the cull, the shadow map, both techniques and a shader
  that pulls vertices draw a deformed instance as they draw any other, and a later producer of geometry on the GPU
  takes the same road.
- **The pass runs when a pose changes**, for the instances whose pose changed, and once more in the frame after, which
  brings their previous positions up to where they stopped. An instance at rest costs no work.
- **The vertices' previous positions are kept**, in a buffer beside them, for passes that need motion
  (`prRenderSceneDeformedBuffers`). The pass writes them from the vertices that it is about to replace.
- **A vertex is morphed and then skinned**, as glTF orders them. A morph target adds its displacements of the position,
  the normal and the tangent, times its weight. Up to four joints then move the vertex, by the sum of their matrices,
  each times its weight. The normal is mapped by that sum's cofactors, which keep it perpendicular under any scale.
- **The bounds hold the pose.** A skinned vertex lies at a weighted mean of where each of its joints would put it
  alone, so it lies among the images of the spheres that bound what each joint moves. The renderer keeps those spheres
  with the mesh, widens them by the farthest that the morph targets reach at their weights, and bounds every part of
  the instance by one sphere about them. It computes the sphere on the CPU when the pose is set, from the joints
  alone.
- **An instance that was given no pose draws its mesh's own vertices**, as the mesh was bound, and costs nothing more.

A deforming instance costs 40 bytes for each vertex of its mesh, 28 for the geometry and 12 for its previous position,
and 48 bytes for each joint of its pose. The vertices of a scene's deforming instances share two buffers, which grow
and are not given back until the scene is destroyed. The mesh holds 16 bytes of skin for each vertex, and 36 for each
vertex of each morph target, once for all of its instances.

## Binding tiers

A device with `PrGpuFeatureFlag_Bindless` reaches resources by the handles in the records and the uniforms. Any other
device reaches them through slots. One source serves both tiers: it compiles twice, and a macro switches only the small
functions that load a record or sample a texture.

- **Both tiers draw the same pixels, to within rounding.** The two libraries compile the same arithmetic beside
  different code to reach a texture, and fast math lets a compiler fuse and reassociate each where its code allows, so
  a pixel may differ by the last bit of a half float. The tests allow that much. A set that needs the same bits
  compiles with `PRECISE_MATH`, which costs some speed.
- **Entry points that should not share slots are separate sources**, one for each pass, because every entry point of
  a source has every slot that the source declares.
- **A pass binds the slots that its library's source declares**, by their names, which the build reflects from the
  compiled code into the library (`prGpuShaderLibrarySlots`). The renderer holds no slot's number of its own.
- **With slots, a draw binds its material's textures**, 5 of them, the scene's
  shadow map and environment, the texture of the shader set's tables, and its mesh's buffers. The smallest device has
  8 texture slots and 4 buffer slots, and the passes use all of them. A new kind of texture for every draw would have
  to take a slot from the materials.
- **The scene behind a surface takes the last of the material's slots**, in the
  passes that draw a material that reads it. Such a material samples one texture fewer with slots, and
  `prRenderMaterialTextureLimit` says so.
- **A material type with five textures or fewer draws alike on both tiers.** Its texture of index i is in slot i, and
  the compiler knows the slot wherever a material type names a texture by a constant.
- **A larger type draws alike on both tiers while its material names five textures or fewer**, whichever five they
  are. With slots, the renderer gives the textures that a material names the draw's slots, in the order of the type's
  texture indices, and the shader reads each texture's slot from the material's data. That choice of slot in the
  shader costs time: in Sponza, drawn with slots on an Apple M5 Max, it added 0.25 ms to a frame of 2.8 ms when
  the Khronos PBR type was drawn that way, which is why a type that fits uses its indices.
- **A material that names more textures than the slots hold loses the rest on that tier, and says so.** Each texture
  past the fifth that the material names reads as the stand-in that its type declares: what the material reads while
  texels have not arrived, so the surface is never a metal, and never glows, for want of them.
  `prRenderMaterialTextureLimit` returns the number of textures that the renderer's device samples, so a loader knows
  before it sets a material. It can then pack several textures into one, or choose a type with fewer.
- **The materials are one buffer on both tiers.** A second buffer for the textures and the parameters would need a
  fifth buffer slot, which the smallest device does not have.
- **A scene's draws are in one order on both tiers**: by alpha mode and culling, then by material type, then by
  material, then by mesh. A pass therefore changes its pipeline once for each type, and within a type a device
  without bindless binds a material's textures once.

## Passes

A part is a function that adds passes to the frame's graph for a view, takes the graph's textures and buffers that it
reads, and returns those that it writes. Nothing chooses among parts but the frame that calls them, so there is no
technique to select: the forward and visibility techniques are two frames of the same parts (`pr/render_passes.h`).

| part | adds | reads | writes |
| --- | --- | --- | --- |
| `prRenderShadowMapAdd` | the scene's shadow map, once for every view of the scene in the frame | the scene | the map |
| `prRenderCullAdd` | a compute pass that tests each draw's bounds against the view's frustum and mask | the scene | the view's draws |
| `prRenderDepthAdd` | depth alone, testing a masked material's alpha | the draws | depth, at a sample count |
| `prRenderOpaqueAdd` | the opaque and masked surfaces, each material type with its own pipeline | the draws, depth, the map | color |
| `prRenderVisibilityAdd` | the draw and triangle at each pixel | the draws | the visibility target |
| `prRenderVisibilityShadeAdd` | one shading of each pixel, in one shader for every type | the visibility target, the map | color |
| `prRenderBackgroundAdd` | the environment where no surface is | depth | color |
| `prRenderBehindAdd` | a copy of what the opaque surfaces and the background drew, each level blurred from the one before | color | the scene behind |
| `prRenderBlendedAdd` | the blended surfaces and those that read the scene behind | the draws, depth, the map, the scene behind | color |
| `prRenderResolveAdd` | the resolve of a multisampled color | color | color of one sample |
| `prRenderMeterAdd` | the meter's reading of what the color shows | color | the meter's next reading |

| frame | its parts | requires |
| --- | --- | --- |
| `prRenderForward` | shadow map, cull, depth, opaque, background, behind, blended, resolve, meter | any device |
| `prRenderVisibility` | shadow map, cull, visibility, visibility shade, behind, blended, meter | bindless |

- **The forward frame** draws depth alone first, so that each pixel shades once for its nearest opaque surface, at the
  sample count of its options. It then draws the surfaces, each material type with its own pipeline.
- **The visibility frame** finds each pixel's point on its triangle from the records, with the exact derivatives of
  its texture coordinates at the pixel's center. Its first pass tests a masked material's alpha where the triangle is
  drawn, as the depth part does, so the shading pass meets no pixel that a mask cut out. It has one sample per pixel,
  and shades the background with the pixels that show nothing.
- **Blended materials are drawn by the forward path in both frames**, after the opaque and masked ones, from
  the farthest to the nearest by the center of each part's bounds. That order comes before the order of types, so the
  pipeline changes wherever a blended draw's type differs from that of the draw behind it. They cast no shadows.
- **A material that reads the scene behind it is drawn with the blended ones**, in their order, whatever its alpha
  mode, and casts no shadows. Before them, a view of a scene that has such a material copies what its opaque surfaces
  and its background drew into the texture of the scene behind, and blurs each of its levels from the one before.
- **A view issues one indirect draw for each of the scene's draws** in each of its passes, whether the cull kept it or
  not. The cost of a view therefore grows with the scene's draws, not with what the view sees.
- **A directional light casts the scene's shadows** through one shadow map, which covers a sphere of the world that
  the light names, or else the bounds of the instances that cast shadows. The map's view is a box of the light's space
  that holds them as tightly as their bounding spheres allow. Its width and its place move in steps, so that the map's
  texels stay where they were in the world while an animation changes the bounds, and a shadow's edge does not crawl.
- **A shadow's edge is the penumbra of the light's source.** A directional light has an angular size, the sun's by
  default. The shading model first looks in the map for what stands between a point and the light, and then weighs the
  part of the source's disc that it hides, over a disc of the map whose radius grows with the distance to what casts
  the shadow (Fernando's percentage-closer soft shadows). An edge is sharp where a shadow touches what casts it and
  soft far from it. It is never sharper than a texel of the map, nor than a pixel of the view at that point, which is
  what keeps it from stepping.
- **The filter reads the map's texels by their indices**, at fixed places about the point, and compares each with the
  depth of the surface's own plane there. So both binding tiers and both techniques compute the same shadow for the
  same point, and a surface that slants from the light does not shadow itself.
- **A point is placed in the shadow on the side of its surface that faces the light.** A light that reaches a surface
  from behind lights it only through a layer that transmits, and the surface does not shadow what passes through it.
- **Point and spot lights** are evaluated in a loop over the scene's first `kPrRenderMaxLights` lights, in the units
  and with the falloff of glTF's `KHR_lights_punctual` ([Light and exposure](#light-and-exposure)).
- **The background is the environment** along each pixel's direction, wherever a view shows no surface. The forward
  frame draws it at the far plane between its opaque draws and its blended ones, and the visibility frame
  shades it with the pixels that show nothing. Both call one function, so both draw the same pixels.

## Pipelines

The renderer makes a pipeline when something first needs it, and keeps it for its life.

- **A pipeline is made for a set of states**: the pass, the sample count, the culling of a double-sided material and
  of a mirrored instance, blending, and in the forward technique the material type. A renderer has the pipelines of
  the states that its content has used, not of every state that its shader set allows.
- **The call that sets a material makes its pipelines**, on the thread that calls `prRenderMaterialSet`, before the
  material is queued for a frame. It makes them for the passes and the sample counts that the renderer has drawn with.
  So a loader on another thread pays for the pipelines of the types and states that it brings, and the frame that
  first draws them makes none.
- **A pass makes what no material announced**: the pipelines of a view's first frame, of a sample count or a part
  that no view has used, and of a mirrored instance. Those are made on the thread that builds frames.
- **The first creation of a pipeline on a system compiles its shader**, which takes tens of milliseconds for a shader
  that shades. The systems that keep compiled shaders, as Metal does for each application, make it again in less than
  a millisecond.
- **`prRenderPipelineInfo` counts the pipelines and the time that making them took**, for an application that watches
  for a stall.

## The display

The display draw applies an exposure, a tone map and sRGB's encoding.

- **The display transform is replaceable.** A view's color is a scene-referred texture of the graph. The display draw
  is one pass that reads it. An application that wants another tone map, a color grade or its own encoding reads the
  texture in a pass of its own and does not call the display draw.
- **The display is a part too.** `prRenderDisplayDraw` draws into a pass of the application's, so that lib/draw's 2D
  composes over the view in the same pass. `prRenderDisplayAdd` adds that pass to the graph for an application that
  shows the view alone, into a target of the graph's, such as the swap chain's texture.
- **Three tone maps** (`PrRenderToneMap`). None is right for every scene, and each distorts something.
  - **Neutral** is Khronos's PBR Neutral tone mapper, and the default. It shows a base color under even light as it
    was authored: it changes no value below 0.8 but for the 4% that a dielectric reflects, and it compresses only the
    highlights above that. It distorts dim colors, as described below.
  - **ACES on each channel** applies the ACES filmic curve to red, green and blue alone. It keeps neutrals neutral
    and gives a scene the contrast of film. It does not keep hue: a channel is compressed whatever the other two
    hold, so a bright orange turns yellow and a bright sky turns cyan, and the curve's toe saturates dark colors.
  - **ACES on luminance** applies the same curve to a color's luminance and scales the color by the ratio of the
    mapped luminance to the luminance. It keeps hue at every exposure, and saturation while the scaled color fits
    the display. A color that does not fit is mixed with the gray of its own luminance until it does, so bright
    saturated colors turn pale and the brightest turn white, without a change of hue. A saturated color is shown as
    dark as its luminance is: under the curve's toe, a dim pure blue is darker than the other two maps show it.
- **The ACES curve is the reference's, at the reference's exposure.** The curve is Narkowicz's fit, with its input
  scaled by one half. It is then within 0.014 of the tone scale of ACES 1.0's reference rendering for an sRGB display
  in a dim surround, on the neutral axis, over 8 stops either side of middle gray, as the reference's CTL computes it.
  Narkowicz's post gives 0.6 as the factor for the original curve, but the fit scaled by 0.6 is up to 0.063 from that
  tone scale. That rendering is darker than the scene: it shows middle gray, 0.18, as 0.10, and diffuse white as 0.62,
  and the curve shows them as 0.11 and 0.62. An exposure of 1.45 shows middle gray as middle gray, and an exposure of 2
  gives the curve as Narkowicz published it.
- **Headroom is for highlights.** A tone map's curve maps scene values into what a display without headroom shows. On
  a display with headroom, the part of the curve above diffuse white is scaled about that point, in its input and its
  output alike, until its ceiling is the headroom. So every color up to diffuse white is shown exactly as a display
  without headroom shows it, the curve keeps its slope there, and a headroom of 1 changes nothing.
- **The neutral map is made for even light.** It takes a dielectric's 4% from every channel, and from a color whose
  darkest channel is below 0.08 it takes more of that channel than a dielectric put there: a channel of 0.04 is shown
  as 0.01. So what a scene lights dimly, such as a shadow that only the sky fills, is shown darker and more saturated
  than it is. A scene under a sun wants its exposure and its surfaces chosen with that in mind, or the ACES curve on
  luminance, whose toe darkens such a shadow and leaves its saturation as it is.

## Testing

`render_tests` runs on each backend that the system has a device for, with handles and with slots only, under
validation, and any validation message fails it. On the web, `render_page_tests` runs it in a page on WebGPU, with
slots. It renders small views offscreen and reads them back as floats.

- **Against the equations**: an unlit color through the display's transform and back, a white furnace over the direct
  lobes, premultiplied compositing of blended surfaces, a mask's cutoff, a shadowed and a lit point, a normal map, and
  the falloff of point and spot lights.
- **Exposure against its standards**: the saturation luminance of ISO 12232 at several EV100, the meter's reading
  and the exposure of diffuse white under illuminances from a hundredth of a lux to the sun's, a scene's meter under a
  directional light, a point light with and without its range and an even environment, against the arithmetic, and a
  view's surfaces and background scaled by its exposure, by either technique, with an unlit color left as authored.
  The reflected-light meter's reading against ISO 2720's constant, and an adaptation against its half-life. A meter of
  a view of two emissions, at three exposures, against the mean of their logarithms; of a view with a black background
  and a bright sliver, against the weighted mean of what lies between a tenth and nine tenths; and of a view of
  nothing.
  A glTF material's and a preview surface's emission at three exposures: scaled by each as luminance, and left as
  authored as the view's.
- **Environments against the equations**: a white sphere in a white furnace, a mirror sphere against the environment
  along the directions that it reflects, the levels of a filtered cube against their source and its energy, the
  harmonics' diffuse light against the light summed over the hemisphere, and the table of the split sum, read between
  its entries, against the lobe's integral, computed in the test.
- **Tables**: the tests' shader set has two tables of its own under the library's. A model shows each across a view,
  and every pixel is compared with the function that the table holds, and the library's split sum with the tests' own
  reading of the table. A table of one value shows that no sample reaches a neighboring table, and that a source's
  context reaches its function.
- **The sky against its integrals and against published skies**: a cube's texels against the model integrated in the
  test without tables, and that integral against its closed form straight up at the sun. The sun's and the sky's
  light on a level surface against the IES's daylight tables for a clear day, the zenith's luminance against the
  formula attributed to Karayel et al., as Radiance's gensky computes it, over the turbidities of clear skies, and the
  horizon against the zenith as in the CIE's standard clear sky, each with the tolerance that the test states. The
  sun's disc against the directional light's irradiance, and a white surface under the sky and its sun against what
  the two give it once.
- **Shadows against the light's size**: the lit part of a point under the edge of a square against the part of the
  light's disc that the square leaves in view, at two heights.
- **Against each other**: slots against bindless, exactly; backend against backend, within the rounding of their
  compilers; and forward against visibility, away from geometric edges, which a single-sampled forward render finds.
  The two techniques filter a minified texture by different derivatives, so a comparison of such a surface allows
  what that difference in level changes, and no other comparison does.
- **Derivatives, measured**: a texture whose every level holds its own number shows the level that a technique's
  derivatives chose. On a minified plane seen at a slant, each technique's level is compared with the level that the
  test computes from the camera for the derivative that the technique promises: the difference within a block of two
  by two for forward, and the exact derivative at the pixel's center for visibility.
- **A shader set of the tests' own**: a procedural material type with named parameters, a texture and the scene's time,
  which the library's model lights, and two shading models with surfaces of their own. Each pixel of the pattern is
  compared with its formula, in both techniques, also where its alpha masks it. A toon sphere's bands are compared with
  the bands that each pixel's normal predicts, and the pixels of one band are equal. A scene that mixes the tests' types
  with the library's is compared between tiers, backends and techniques as the library's own scene is.
- **Pipelines by type**: setting a material of a type that a forward view has not drawn makes that type's pipeline in
  the call, and the next frame makes none. The visibility technique makes none for a new type.
- **Broken shader sets**: a test for each mistake that the compiler or the build's check reports runs the check on a
  set that makes it, and reads the message. Two of the sets declare an emission that the compiler accepts and never
  calls: a method of another form without `override`, and a field of the method's name.
- **A type larger than the slots**: a material type of the tests with 8 textures and a record of 144 bytes draws the
  sum that its textures and weights predict. With bindless it samples all 8. With slots it samples the five that a
  material names, in any places, and reads the stand-ins of those named past the fifth.
- **The layers against the equations**: a white sphere with any one layer in a white furnace, a smooth coat against
  the Fresnel term of its interface, a thin film over a smooth metal against the Airy sum of its thickness, a slab that
  transmits against Beer's law, a sheet that transmits diffusely against a light behind it that casts shadows, slanted
  or not, the anisotropic lobe's normalization and the shape of its highlight, the sheen's table
  against the lobe's integral, and the specular-glossiness workflow against metallic-roughness where the two define one
  surface. A surface that transmits shows the scene behind it by either technique and with any number of samples, and
  the texture of the scene behind is made for a view that needs it and for no other. A scene of layered materials is
  compared between tiers, backends and techniques.
- **OpenPBR against Pbr and against its specification**: the two models under an environment where the specifications
  agree, and under a light by what they differ by; a white sphere in a white furnace with any coat and fuzz; a smooth
  coat against Fresnel's equations; emission under a coat; a translucent base against glTF's transmission; the rough
  diffuse lobe and the thin-walled subsurface against the specification's lobes; and a coat's own normal map.
- **Textures that have not arrived**: each texture of a glTF material without texels draws exactly as a texture of one
  texel of its stand-in does. A material whose metallic-roughness texture has not arrived draws exactly as a dielectric,
  and one whose emissive texture has not arrived exactly as a material without emission. Four textures arrive in each
  of their 24 orders, and every step draws the stand-ins of those still to come.
- **Deformation against the CPU**: the vertices that the pass writes are read back and compared with the same
  arithmetic in the test, for a bar bent by two joints, for morph targets at several weights, and for both together,
  with their previous positions and the bounds that must hold them. A bent bar is lit as a rigid bar turned the same
  way is.
- **The update model**: ids, transactions, commits from several threads while frames render, resources that have not
  arrived, and a lost device.

`examples/render_features` is the 3D analog of `examples/draw_features`: a page for each feature, as a small procedural
scene that sweeps the feature's parameters. It has a page for each of Khronos's material extensions and for each group
of OpenPBR's inputs, and pages for alpha modes, texture transforms, lights, shadows, environments from each source,
tone maps, instancing, and skinning and morphing. Its shader set adds material types and a shading model of the
example's own: a checker that the material computes and filters by the technique's derivatives, a surface whose bands
of light move with the scene's time, and a toon model with a surface of its own, each with a page. `lib/draw`'s 2D
labels each page over the view, in the pass that shows it. The pages are generated, so the example reads no file but
its font, which the page on the web packages. `--page NAME --capture FILE` writes one page at a fixed time, and
`--list` names them. `--stats` prints what the first frames' graphs recorded, and then the frames' times on the CPU and
the GPU and the pipelines that the renderer made. `render_features_tests` draws every page offscreen on each backend,
and on the web in a page on WebGPU: a device with the slots alone draws the bytes that one with handles draws, and the
two techniques agree away from geometric edges.
