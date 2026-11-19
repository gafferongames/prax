# The 3D plan

This document holds the direction of Prax's 3D stack: why it exists, the decisions made so far with their reasons, what
has been tried and what it showed, and the work that is planned. The other documents describe what the code does:
[gpu.md](gpu.md) for the render graph, [gltf.md](gltf.md), [mesh.md](mesh.md), [render.md](render.md) and
[model.md](model.md). This one is a plan. It will be rewritten as the work lands, and a decision that reaches the code
moves into that library's document.

## Why

- **Separable graphics.** Developers reach for a large engine because they want its graphics, and then find that the
  renderer cannot be taken without the rest or extended without changing engine source. Prax's renderer is a library
  of parts that an application assembles.
- **Ease.** Putting a 3D model on screen should be as simple as it is with a web library: load a file, place it, draw.
- **No opinions about the application.** Prax is a toolkit, not an engine. It provides no entity system and no
  gameplay simulation, at any layer. Although, navigation meshes and pathfinding may come later as libraries of their own.
- **Customizable shading.** An application defines its own materials and shading models.
- **Every platform, small binaries.** One code base serves desktop, mobile and the web, with no required third-party
  dependency, and an application links only what it calls.

## Layers

| layer | holds |
| --- | --- |
| `lib/gpu` render graph | passes, the resources they use, barriers, and textures that live for a frame |
| `lib/gltf` | the glTF 2.0 asset as its specification defines it, with no GPU |
| `lib/usd` | OpenUSD layers and stages as the USD Core Specification defines them, and the schemas that a model draws, with no GPU |
| `lib/mesh` | geometry processing over plain arrays, with no GPU |
| `lib/camera` | cameras and the controllers that move them, with no GPU |
| `lib/render` | the scene on the GPU, materials, shading models, and the parts of a frame as graph passes |
| `lib/model` | an asset of any scene format made ready to draw |
| `lib/stage` | one or a few models presented: their ground, their light and their framing |

Each layer is usable without the ones above it.

## Decisions

### The application owns the frame

A frame is a graph that the application builds again each time, in plain code. The renderer adds a view's passes to it
and returns graph textures. The library ships a small number of complete frames, which are real: an application can
ship one as it is. An application that wants a different frame copies the function that builds one and changes it.
These frames do not offer a hook for every possible change, because copying is the extension mechanism.

A technique is not chosen for the application. Forward, visibility-buffer and deferred shading suit different content
(triangle density, the number of materials and lights), so the library provides techniques as functions that add
passes, and the reusable parts as functions that the techniques call.

### The scene is flat records

Instances, meshes, materials and lights are records with a published layout, which C++ and the shaders share. An id is
a record's index. The renderer keeps no hierarchy: transforms arrive in world space, and a producer owns its own
structure. A glTF model, a game's storage and an editor are all producers.

- **Three ways to write.** A producer sets one record, sets a range in bulk, or writes records from the GPU in a compute
  pass. The published layout exists for the third.
- **Transactions.** A producer fills an update on any thread and commits it. A commit takes effect whole at a frame
  boundary.
- **Immediate content** is the same records with the lifetime of one frame.
- **Resources are apart from scenes.** Meshes, textures and material types belong to the renderer, and a scene is
  instances and lights that name them.
- **Other kinds of content** (terrain, hair, volumes, decals) are an application's own passes that read the scene's
  shared data. The scene is not a host for plugins.

### Mobility is a mask

An instance is static, dynamic or kinematic by a mask over the one array of instances, never by a partition of it, so
that an id survives a change of mobility. Destruction and level editing turn a static instance dynamic: the instance is
masked out of the structures that were built for static content, and those are rebuilt asynchronously. Physics writes
the transforms of kinematic instances in bulk where it synchronizes. No pass reads the mask yet. Cached shadow maps and
acceleration structures are its first uses.

### Starting and recovering are one path

A lost device means every GPU object is made again. The scene therefore holds demand, not durable data. Populating a
renderer from nothing, incrementally, is the one path for start-up, for recovery and for joining a world over a
network. Rendering never waits for a resource: it draws a stand-in or skips. Static data streams from storage again,
textures return at low detail and sharpen, and procedural content is rebuilt, on the GPU if that is where it is made.

### Two binding tiers

A device with bindless reaches resources by handles that data carries. Every other device binds resources to slots.
One shader source serves both, and both draw the same pixels. A technique may need bindless and say so.

Every developer platform (macOS, Windows, Linux) has a backend that runs both tiers. Direct3D 12 is bindless only: it
requires shader model 6.6, resource binding tier 3 and enhanced barriers, and a system without them falls back to
Direct3D 11, which is the tier of slots there. Direct3D 12 cannot be tested on the machine that this work is done on.

A shader source's slots belong to every entry point of the source, and slot numbers are explicit. Entry points that
should not share slots are separate sources, so each pass's entry points are a library of their own.

An id is never carried in a base instance or a base vertex, and a fragment shader does not ask the device for the
index of its triangle, because the graphics APIs disagree on each. A draw carries ids in its push data, and a shader
that needs its triangle's index pulls its vertices from a first vertex of zero.

### Materials and shading models are functions

A material is a function from what a point of a surface offers to the surface of a shading model. A shading model is
the functions that take its surface to radiance. A technique decides where each runs, and the same functions serve
every technique.

- **The contract is Slang interfaces.** The shaders are Slang, compiled when the application is built. A shading
  model is a structure that implements `IRenderSurface`, and a material type one that implements `IRenderMaterial`.
  The library writes the loop over lights, the forward fragment shader and the call of a material once, as generic
  functions. The compiler specializes them, so no backend sees a generic. A shading language of Prax's own has not
  been ruled out, and this does not constrain that choice. It only shapes what to expect of one.
- **The contract has room for real shading models.** A material's record and its textures have the size that its
  type declares. A shader set brings lookup tables. A model prepares a context once for a pixel, and its emission
  sees the eye. A light carries the angular size of its source. The decisions and what was measured for each are
  under [The contract's capacity](#the-contracts-capacity).
- **A surface belongs to its shading model.** The only thing every surface has in common is coverage, which the passes
  that test alpha read. A technique that stores surfaces, such as a deferred one, takes its layout from the models and
  from the application's shader set. The library fixes no G-buffer.
- **The shipped model is Khronos's PBR model**, glTF's metallic-roughness model, with Fdez-Agüera's compensation for
  multiple scattering under image-based light. Its extensions (clearcoat, sheen, transmission and the rest) are layers
  of that one model, and grow its surface.
- **Forward specializes a pipeline for each material type.** The visibility technique dispatches on the type in one
  shader. A pass for each type that a stencil or depth test selects is another way to do that.

### The render graph serves two goals that conflict

Barriers must be minimized, and memory must be minimized, and one costs the other. A texture of the frame that keeps a
device texture of its own, in the accesses that its passes use, never needs a barrier. A device texture that passes
from one texture of the frame to the next saves memory, and each handover needs a barrier. The order of passes feeds
both: an order that shortens lifetimes allows more sharing, and an order that groups compatible work needs fewer
barriers.

The graph exposes the choice as a priority. A barrier is recorded just before the use that needs it. A pass that
writes what an earlier pass wrote waits for it, unless the two declare byte ranges that do not overlap. The graph holds
the resources whose accesses change during a frame, and a pass binds a purely static resource without it.

## What has been tried

- **Reordering passes** in the graph by its priority: the one real frame showed no gain. The barrier counts of the test
  frame were the same with and without it, and the heuristic was quadratic in the passes. It was removed. Passes are
  recorded in the order of addition, and the priority still chooses which device texture a frame's texture takes.
- **Keeping a device texture's accesses across frames** removed two of the test frame's barriers: frame3d records 13
  in its first frame and 11 in each frame after.
- **One library of forward entry points against a library for each material type.** One library is smaller and builds
  faster. A library for each type roughly halves the time to create pipelines cold, and that time then stops growing
  with the number of types.
- **Plain structures and functions against Slang interfaces** for the shader set's contract. The plain rule cost
  three things in the generator: the loop over lights was generated text for each shading model, a function's
  signature was checked by compiling a probe after the fact, and the build description repeated eight kinds of name.
  With interfaces, the two CMake scripts went from 726 lines to 544, and the Slang that the build generates for
  frame3d's set from 484 lines in 22 files to 107 in 4. A material type is one name in the build description, and a
  shading model none. frame3d draws the same bytes on Metal and Vulkan in both techniques and both tiers. Its shader
  libraries are 0.1% smaller. Its set builds from nothing in 1.0 s where it took 3.1 s, because one compile checks
  what 19 probes did. Its pipelines are made cold in the same time, within what the runs spread: 58 ms against 55 ms
  for forward, and 96 ms against 95 ms for visibility. These are evidence for the shading language decision. What
  Slang could not do is in the list below.
  - Reflection does not list the structures that implement an interface, so the build description still names each
    material type.
  - A structure cannot say at its declaration that it means to replace a default method. A model's emission of the
    wrong form, without `override`, compiles as a method of the model's own, and the default is used silently. The
    compiler's reflection does not list a structure's methods, so the build reads them in the syntax tree that slangc
    dumps, and fails on a member of that name without the keyword. The dump's form is slangc's own, and may change
    with its version.
  - An associated type has no default. A model that needs no context for a pixel implements a second, smaller
    interface, and one generic extension of the library's makes every such structure a full model.
  - A `Surface` that is not a shading model is reported as "does not conform to `IRenderMaterial`", which does not
    name the member at fault.
  - The switch on a material's type must group the types by shading model, or each type's case holds a copy of its
    model's shading: the SPIR-V of frame3d's visibility pass is 44% larger with a case for each type. No generic
    expresses the grouping, so the build reads each type's model from the compiler's reflection and writes the switch
    when the set is built.
  - Slang's WGSL keeps `@interpolate` on the fields of a varying structure that a function takes, which WGSL refuses.
    The build removes it. The plain contract had the same fault.
- **Deformation in a compute pass**, which writes deformed vertices that every technique draws as any mesh, kept the
  techniques and the materials unaware of skinning and morph targets. Its cost is memory for each posed instance.
- **The exact texture derivative against the hardware's difference** in the visibility technique. A draw's fragment
  shader takes the difference between two pixels of a block of two by two, and the visibility technique can compute
  either that difference or the exact derivative at the pixel's center from the triangle. The exact derivative is the
  shorter shader, by 14 of 2,775 SPIR-V instructions. The time on the GPU did not differ on an Apple M5 Max. A frame of
  frame3d took 2.20 ms with the exact derivative and 2.21 ms with the difference, and a frame of Sponza in the model
  viewer 3.39 ms and 3.43 ms. Each number is the median of six runs, and the runs of one shader spread over 0.1 ms to
  0.4 ms. The two derivatives choose levels up to 0.07 apart on the tests' slanted plane, without anisotropy. The
  technique uses the exact derivative.
- **`lib/mesh`'s tangents against MikkTSpace's reference and meshoptimizer** (the `tangents` suite): they are the
  reference's to the bit wherever it computes a tangent, once groups stopped starting at triangles without texture
  area. The reference's own departure, an edge sort that leaves its last run unsorted, is in the suite's errata.
  meshoptimizer's generator is several times faster but only close to MikkTSpace, so `lib/mesh` keeps its algorithm.
- **The first blended frames of a process on Metal are not its later frames, to the bit.** With nothing changing in
  a scene, a few blended pixels change by one unit of a half float at some frame of the first three, and stay so: 10
  pixels of Khronos's CarConcept, on its glass, and 3 of ToyCar. Sponza and DamagedHelmet, which blend nothing, draw
  the same bytes from the first frame. It happens on Metal and Metal 4, with bindless and with slots, with the
  validation layer and without, and before the split sum's table as after it. On Vulkan the frames are the same
  bytes. A second renderer on the same device, with textures, an environment, a shadow map and pipelines of its own,
  draws its first frame as its later ones. So nothing of the renderer's is late: no upload, stand-in, filtered
  environment or shadow map. The backend makes its pipelines synchronously, with no option that defers work. What
  remains is the driver, which appears to replace a blended pipeline's code after the pipeline first draws. That is
  not confirmed, and no setting of pipeline creation was found that avoids it. The `model` suite compared the first
  frame of each binding tier, and the table moved one pixel of CarConcept to where that unit rounds to another of the
  picture's 8-bit levels, so the suite failed in about half of its runs. Its harness now compares each tier's fourth
  frame.
- **Composing USD stages**, measured on 2026-11-16 on an Apple M-series laptop, when `lib/usd`'s composition landed.
  One prim with 20,000 references composed in 27 ms, where an earlier composition that walked an arc's siblings took
  555 ms, and one with 20,000 inherits or specializes behind a reference in 50 to 70 ms, where 2,000 of them had
  taken 1.8 s and 4.3 s. The largest stage of the `usd` suite's corpora, teapotScene_animCycle.usd of 2,833 prims,
  composed in 51 ms at a peak of 39 MB, against 96 ms and 107 MB for OpenUSD 26.11's `Usd.Stage.Open` and a
  traversal. A generated scene of 10,000 references to an asset of 85 prims, 860,002 prims with 1.7 million opinions,
  composed in 2.5 s on one thread at a peak of 680 MB, against OpenUSD's 3.7 s of wall time on every core (55 s of
  processor time) and 1.8 GB. With its references instanced, 10,088 prims and one prototype, it composed in 15 ms
  against 49 ms.
- **The claims that were written from memory, against their sources**: the sky's coefficients and integrals, the
  tone maps, the compensation for multiple scattering and the harmonics' constants were as their sources have them.
  ACES's reference transform supports the factor of one half on Narkowicz's curve, not 0.6. Three sentences about how
  dark the ACES maps are were wrong and are corrected. The IES's, Karayel's and the CIE's formulae were confirmed
  through other publications that quote them, and the comments say so.
- **MaterialX's rotation of coordinates, against its specification.** The specification's `rotate2d` turns a vector
  counterclockwise for a positive amount, and its `place2d` says that a positive rotation turns the coordinates
  counterclockwise and the image clockwise. The GLSL of MaterialX 1.39.4's library (`mx_rotate_vector2`) turns the
  vector clockwise. The loader follows the specification, so a `place2d` with a rotation draws its image turned the
  other way from MaterialX's own viewer. Which one is to change is not settled.
- **Khronos's test assets of the material extensions, looked at** against what each README says it shows, in the
  `model` suite's images and in the model viewer from the README's angle. Sheen, iridescence, anisotropy, clear coat,
  specular, the index of refraction, transmission, volume, dispersion and diffuse transmission show what their tests
  ask, and the Compare assets show each extension against its absence. One defect was found: a surface lit from behind
  shadowed the light that it transmits (above). What differs from the READMEs' screenshots is the lighting: the suite
  lights with a clear sky and its sun, the screenshots with studio environments, and several are path traced, as the
  strong color fringes of DispersionTest and the refraction of transmissive surfaces seen through each other are. A
  transmissive surface casts a whole shadow, which AttenuationTest's path-traced screenshot shows tinted.
- **The cost of the layers on the GPU**, on an Apple M5 Max with Metal, of 48 spheres that fill a view of 1920 by
  1440, the median of three runs while other work shared the GPU: the core material 3.50 ms; the layered type with a
  clear coat, sheen, a thin film, anisotropy and diffuse transmission 4.56 ms; with transmission, volume and dispersion
  too 4.84 ms, which adds the passes of the scene behind; OpenPBR with its base alone 5.01 ms; and OpenPBR with a coat,
  fuzz, a thin film, anisotropy and subsurface 5.48 ms. A run under less load gave 1.9 to 2.1 ms for the core, 3.4 to
  4.2 ms for every layer and 3.3 to 3.7 ms for OpenPBR's base, so the ratios hold: the layers cost about a third more
  than the core, and OpenPBR about half more.
- **The extensions' cost in the default shader set**, from before they were added to now, in bytes of the libraries:
  the forward pass from 59,127 to 151,124 with handles and from 68,487 to 163,604 with slots in Metal, and from 37,736
  to 138,620 and from 38,156 to 149,384 in SPIR-V; the visibility pass from 61,852 to 96,012 in Metal and from 55,896
  to 130,164 in SPIR-V; the masked depth from 22,221 to 29,917 and from 28,941 to 37,053 in Metal. Making frame3d's
  pipelines cold, on Vulkan over MoltenVK in the first run of a build, went from 594 ms to 634 ms for the forward
  technique's 8 and from 473 ms to 857 ms for the visibility technique's 4, and from 22 ms to 25 ms and from 17 ms to
  48 ms once the system had them. Metal had the old shaders compiled from other builds, so its cold times compare
  nothing: 172 ms and 173 ms now. A frame of frame3d took 1.76 ms and 1.77 ms on the GPU by the forward technique, and
  1.86 ms and 2.17 ms by the visibility technique, whose one shader now holds the layered type. The visibility
  technique is where the layers cost a scene without them.

## Planned

Roughly in order. Each item names the existing piece that it builds on.

1. **Khronos's material extensions** on the PBR surface, with `lib/gltf` reading them: done but for the items under
   [Shading, as it stands](#shading-as-it-stands). The default is to support an
   extension. One that is left out is proposed with its reason.
2. **Shadows at scale.** Cascades, and maps cached for static instances, which is the mobility mask's first use, are
   the simpler start. A virtual shadow map is the alternative to explore.
3. **Occlusion of indirect light**, so that interiors are not lit by an unoccluded sky.
4. **Picking** through `lib/mesh`'s bounding volume hierarchy in the model viewer, and a one-pixel id buffer on the
   GPU. Tools should prefer work on the GPU to reading results back: a transform tool can run as a compute pass.
5. **Draw submission at scale, and clustered lights**, measured on a scene that needs them. A pass issues one indirect
   draw for each part of each instance, whether or not the cull kept it, and the scene's lights are a short list in
   the uniforms. Meshlets and submission that the GPU drives remove the first. The scene is to be found:
   - Zorah, with nvpro-samples' `vk_lod_clusters`, is the extreme case for clusters.
   - NVIDIA's Omniverse sample content has huge scenes, in USD, which wait for the USD item below to reach
     composition and geometry.
   - The OpenPBR shader playground exercises a second shading model.
   - Disney's Moana island would turn the work toward levels of detail for a whole world.
   - Bistro is overused and shows little.
6. **A reference path tracer**, which runs the same material and shading functions. The comparisons then test the
   approximations (prefiltering, the split sum, shadows, clustering), not a second copy of the equations.
7. **USD**, with no dependency on Pixar's OpenUSD in any library ([usd.md](usd.md)). Layers and stages are written
   from the Alliance for OpenUSD's Core Specification, which covers the data model, the file formats, composition,
   population and value resolution. The schemas of geometry, shading and lights follow OpenUSD's schema definitions
   until AOUSD publishes specifications of those domains. In stages:
   - **Layers**: text, crate and packages read into the specification's document data model. Done, in `lib/usd`. The
     `usd` suite compares every layer of two public corpora with what OpenUSD reads of it.
   - **Composition**: sublayers, references, payloads, inherits, specializes, variants, relocates, strength ordering,
     value resolution over time samples, and instancing, as a stage over layers. Done, in `lib/usd`
     (`pr/usd_stage.h`). The `usd` suite compares the stage of every layer of the corpora with OpenUSD's, and
     composes the specification's composition tests.
   - **Geometry and UsdPreviewSurface**, drawn through `lib/render` by `lib/model`, whose USD adapter reifies a stage
     in one composition context: its variant selections, purposes and range of time codes, whose samples are tracks.
   - **Skeletons, lights, cameras and animation.**
   - **MaterialX and OpenPBR materials.** Documents as layers and networks folded into the OpenPBR type, done
     ([OpenPBR and MaterialX](#openpbr-and-materialx)). MDL is on hold.

   Open items of USD:
   - **Splines.** A layer that holds one is refused as NotSupported, in either format.
   - **Value clips.** An attribute resolves from time samples and defaults alone.
   - **Variable expressions** in asset paths, which the Core Specification leaves to a later release.
   - **A population mask, the muting of layers**, and opinions of a session other than its variant selections.
   - **UsdSkel**: skeletons, their animation and blend shapes are not read, and a skinned mesh draws as it is authored
     (`skeletons`).
   - **Area lights.** The source keeps a light's shape and size, and the model draws it as the punctual light of its
     intensity along its axis ([model.md](model.md#sources)).
   - **The folding of MaterialX networks moves into `lib/materialx`.** What a network's folding means is MaterialX's,
     and the folding is in `lib/usd` (`prUsdMtlxInputRead`, `pr/usd_geom.h`), where only the walk of UsdShade's
     connections belongs.

Deferred until there is a problem to solve: **motion vectors and temporal antialiasing**. Multisampling on a dense
display shows no aliasing that matters today. Instances keep a previous transform, and deformation keeps previous
positions, for when it does.

### Settled with the owner

- **`lib/gltf` stays strict about required extensions.** Refusing an asset whose required extension is not read is
  what the mechanism is for, and most of Khronos's extensions are to be implemented.
- **The exposure of the ACES curve** stays at the factor that was verified against the reference, one half. Unreal
  likely uses 0.6, and the two would not match for other reasons.
- **The seven pixels of frame3d that forward specialization changed by one step** are accepted.
- **Nothing here merges soon.** The stack stays on its branch through this phase of cleanup and review.
- **Each subsystem takes one posture toward USD** ([usd.md](usd.md#praxs-relationship-to-usd)), after Nick Porcino's
  "Reification, Oracles, and Evolution". `lib/render` never sees USD. `lib/model` is fed by an honest reifier, which
  reifies once for each composition context and again only when the stage changes, with no stage query on the frame's
  path. Inspection in Scribe is an oracle over the live stage. Authoring, when it comes, is hosted by USD in tools
  whose product is the layer stack, as sparse opinions in an edit target and view state as the session's variant
  selections. No
  library keeps a document model in sync with a stage. prax's own formats are reification persisted, behind a boundary
  that converges.

### Producers that follow visibility

Some content attaches to other content and should exist only where its parent is seen: foliage on terrain or on
objects, scatter, fur, decals, attached particles, impostors. The plan is a kind of producer that runs after the cull.

- A record carries an expander's type, its parameters, and a second bound that holds what it will emit. The parent is
  culled against that bound.
- A compute stage runs over the parents that survive, and emits ordinary instance records with the lifetime of a
  frame, which are then culled and drawn as any others.
- Placement is a pure function of the parent's id, an index and the time. It is then stable from frame to frame, a
  previous transform comes from evaluating at the previous time, and nothing is rebuilt after a lost device.
- An expander runs once for the union of the views that saw its parent, so that a shadow view and the camera agree on
  what exists.
- Terrain offers a height field. An object offers samples of its surface, which `lib/mesh` would prepare.

### Cooked and raw assets

Loading a `.glb` at run time with no build step is what makes the stack easy. Meshlets, levels of detail and compressed
textures built ahead of time are what make it scale. Both are supported, and the concern is binary size: an application
that ships prepared data must not link the code that prepares it.

- Every prepared product is optional in the format. A mesh exists without meshlets, and a texture without compression.
- Each preparer is a unit that links on its own. The build tool links all of them, and an application links those that
  its loading needs.
- A technique states what it needs. An application that loads raw assets into a technique that needs meshlets links the
  meshlet builder, and sees that cost.
- A preparer that runs in the build must be built for the host, which is awkward when the target is a phone or the web.
  This is unsolved.

### Parameters that are interpreted

An exploratory idea, definitely for an editor and maybe at runtime to avoid compiling a shader. The plan is a small
bytecode, interpreted on the GPU, that computes a compiled material's parameters: a stage in front of a material, not
a replacement for one. An expression such as a blend of two textures by a function of time is its scale. Shading models
are not authored this way.

- Most platforms inline shaders fully, so each use of the interpreter is costly. A shader instantiates it as few
  times as it can balanced against register pressure.
- A texture sampled inside the interpreter has no implicit derivatives. The first form restricts sampled coordinates to
  affine transforms of a mesh's own, whose derivatives are cheap. An optimizer that knows exactly where derivatives are
  needed is one argument for a language of Prax's own.

### OpenPBR and MaterialX

How OpenPBR Surface, which MaterialX documents and USD assets carry, reaches the renderer. Decided for a material that
loads at run time: the library's OpenPBR type, whose every texturable input reads a texture of its own through a map of
its channels, and a loader that folds a MaterialX graph into those inputs, by a folding that is lib/materialx's
([usd.md](usd.md#materialx)).
That covers the graphs of images, constants, channels and arithmetic by a value that the working group's assets use.
Still open: a graph that computes more than that, for which the translator at build time and interpreted parameters
below remain the candidates.

- **A hand-written model and a material type of fixed parameters**, as glTF's is. `examples/openpbr` is one: the
  opaque base, the specular layer, the metal, the coat, the fuzz, the thin film and emission, which agree with `Pbr`
  where the two specifications do and pass the white furnace with every layer. It covers a document whose inputs are
  constants and images. It has 21 of OpenPBR's 41 inputs and five textures, which were all that a record held when it
  was written. A record and a type's textures now have no such limit.
- **A translator at build time** from a MaterialX node graph to a material function in Slang, which fits materials that
  compile when the application builds. It means reading MaterialX and implementing the standard nodes that assets use,
  not all of them.
- **Interpreted parameters** (above) for graphs loaded at run time that only compute OpenPBR's inputs, in front of the
  compiled model.
- **MaterialX's own generator**, whose Slang target is upstream, emits whole shaders with its own lighting and
  bindings rather than functions of this contract, and brings MaterialX's C++ libraries and data. It is a reference to
  compare against, not a dependency.

What the prototype showed: a second model with a surface of its own fits the contract, and shares the GGX lobe and the
split sum with `Pbr`. Transmission, subsurface scattering, thin-walled mode and anisotropy need the renderer first: the
scene's color behind a surface, light around a point, and an environment filtered for an anisotropic lobe. Clearcoat,
sheen, iridescence, IOR, specular and emissive strength for glTF are the same lobes on `Pbr`'s surface.

Where the prototype met the contract's limits, and what became of each:

| friction | now |
| --- | --- |
| A parameter record held 128 bytes, 21 of the model's 41 inputs. | A record has the size of its structure. |
| A material named 5 textures, for about 25 inputs that take one. | A type declares its textures. A device with slots samples 5 of a material's. |
| The model could bring no table, so its fuzz and its albedos are fits. | A set declares tables, and the library's split sum is one. |
| `direct` computed each layer's albedo toward the eye again for each light. | `prepare` computes them once for a pixel. |
| `emission` had no eye, so a coated emitter did not darken toward its edge. | `emittedRadiance` takes the eye and the context. |
| A light had no size, so a smooth lobe is clamped to a least width. | A light carries its angular radius. `Pbr` and OpenPBR do not read it yet. |
| The set was private to one target, so the example wrapped it in a library by hand. | A set is a library that targets link. |
| A field named `emission` collided with the method, and a method of the wrong form compiled silently. | The methods are `directRadiance`, `indirectRadiance` and `emittedRadiance`, and the build fails on the silent form. |

The prototype is now the library's second model: `pr/render_openpbr.slang`, with its material type and its record, which
a shader set has only if it asks for it (`pr_render_shaders`, `OPENPBR`). It reads all 41 inputs, and says in its
header what a rasterizer degrades. Still to do: a use of the light's size, the medium's scattering, and light that
bleeds under a surface.

### Shading, as it stands

Decisions that the code now holds. [render.md](render.md) is to state them once it is brought up to date.

- **One lobe library, two layering rules.** `pr/render_lobes.slang` holds the Fresnel terms, GGX with one width and
  with two, the Charlie sheen and the Zeltner fuzz, Lambert and the energy-preserving Oren-Nayar lobe, and both
  specifications' thin films. The Khronos model mixes them by Fresnel terms and OpenPBR by albedo scaling.
- **Khronos's extensions are two material types of one model.** `KhronosPbr` is the core material, with five
  textures in the slots of their indices. `KhronosPbrLayered` has 19 textures and every extension's factors, and also
  draws the archived specular-glossiness workflow, by its own definition and not by a conversion. A loader chooses
  between them (`prRenderKhronosPbrMaterialIsLayered`), so a material without extensions pays for no layer in the
  forward technique. In the shaders that dispatch, the layers are behind one test of the model's context.
- **The scene behind a surface** is a resource of the frame that a material opts into (`readsBehind`,
  `pr/render_behind.slang`). Such a material is drawn with the blended draws, after what does not blend, and the view's
  color before them is copied, or resolved, into a texture with a level for each blur. The graph has no such texture
  for a scene without such a material. With slots the texture takes the last of a material's five texture slots, so a
  material that reads it samples four.
- **Indices of a material that name one texture share a slot**, so an image that packs occlusion, roughness and
  metallic costs one of the five. One of Khronos's sample assets still names more than a device with slots samples,
  and the model says so (`prModelUnsampledTextureCount`).
- **The sheen's albedo is the third number of the split sum's table**, and OpenPBR's fuzz keeps MaterialX's fits, so
  the layers cost 2 KB of constants and no texture slot.
- **The sheen under an environment reads the cube that was filtered for GGX**, at its roughest level, mixed with the
  diffuse light. A cube filtered for the Charlie lobe would take a slot. Its error against the lobe's own integral
  over an uneven environment is not measured.
- **`lib/gpu` has a store that resolves and keeps** an attachment's samples, which the forward technique needs to
  draw blended surfaces over samples that it has already resolved once.
- **A light's size is still unused.** Adding the square of a light's angular radius to the square of a lobe's width
  would widen only lobes below a roughness of about 0.07 under the sun, where the least width stands in for it today.
  It would change every image with such a surface under a directional light, and is not done.
- **Material animation is drawn** with a material instance for each placement that animates one, made when the
  placement first plays such a channel ([model.md](model.md#animation)).
- **`examples/render_features` replaced the OpenPBR example and `examples/frame3d`.** It has pages for the example's own
  material types, its toon shading model, the scene's time, and skinning and morphing, and `lib/draw` labels each page
  over the view. Its tests draw every page and compare the binding tiers and the techniques.
- **A point lit from behind is placed in the shadow on the side that faces the light.** A sheet that transmits
  diffusely shadowed half or more of the light that passes through it, wherever the light slanted.

### The contract's capacity

Decisions that the code now holds, with what was measured. [render.md](render.md) states the contract.

- **A material's size is its type's.** The materials' buffer holds a record of 32 bytes for each material, and after
  the records each material's textures, 64 bytes each, and its parameter record. A second buffer for these would be
  the natural layout, and the smallest device has 4 buffer slots, which the passes already use. So the materials are
  one buffer, and a record names its data by an offset. The renderer writes the whole buffer when a material or a
  texture changes, as it did before. An upload of only what changed is not done.
- **Textures on a device with slots.** A draw has 5 slots for its material's textures. A type with 5 textures or
  fewer keeps each texture in the slot of its index. A larger type's material gets the slots for the textures that it
  names, and the shader reads each texture's slot from the material. Drawing the glTF type that second way cost
  0.25 ms of a 2.8 ms frame of Sponza with slots on an Apple M5 Max, so only the types that need it pay. A material
  that names more than 5 textures reads the rest as their stand-ins there, and `prRenderMaterialTextureLimit` tells a
  loader so. Packing several textures into one at import is the loader's remedy, and is not written.
- **Tables are regions of one texture**, so a set's tables cost one slot. The eighth and last texture slot of the
  smallest device is theirs. A shader interpolates a table's texels itself. With the device's filter, one pixel of
  one test differed by a step of a half float between two renders that should be equal.
- **The split sum is a table.** It replaced a fit of two polynomials of degree 4. Against the lobe's integral at the
  middles of 32 by 32 cells, the fit was off by 0.022 at most and 0.0062 on average for the scale, and by 0.030 and
  0.0027 for the bias. The table is off by 0.0064 and 0.0007, and by 0.017 and 0.0002. A frame of Sponza took 2.5 ms
  with the table and 2.9 ms with the fit, each the median of two runs of 12 reports, and frame3d's runs spread too
  widely to tell them apart. The forward shader of the glTF type has 1,187 SPIR-V instructions, down from 1,254. The
  cost is 4 KB of constants in the library, and the slot. Every image with an environment changed. In the captures,
  8% of frame3d's pixels moved, 20% of Sponza's and 22% of the OpenPBR example's, nearly all by one step of 255. The
  largest change is 3 steps in frame3d, 4 in Sponza and 13 in the OpenPBR example. No test's tolerance moved but the
  test of the split sum itself, whose bounds are now the table's.
- **The prepare stage.** The compilers do not move a term that depends on the eye alone out of the loop over lights:
  the OpenPBR forward shader is 2,858 SPIR-V instructions with the stage and 3,575 with the same terms in the light's
  method. The frame's time on the GPU did not show it in the example, with its one light or with 16: the runs of one
  build spread over 0.5 ms of 5. `Pbr` draws the same bytes as before the stage, on Metal and on Vulkan. OpenPBR's
  direct and indirect light now share one cosine of the eye's angle, clamped as the direct light's was. That changed
  about 950 pixels of the example's capture, away from the emitter below: 68 of them by more than two steps of 255,
  and the rest spread thinly over the picture, as pixels at the spheres' edges would be.
- **Emission sees the eye.** OpenPBR's coat now passes what its interface does not reflect at the eye's angle, where it
  used the angle of normal incidence. A coated emitter darkens toward its silhouette, which changed 18,000 pixels of
  the example's capture.
- **A light's size is exposed and unused.** `Pbr` shades a light as a point, as before. Widening its highlight by the
  sun's size would change every image with a smooth surface under the sun, and belongs with the extensions' work.
- **One pixel of the OpenPBR example moved on Metal** by one step when the material's record changed its layout, and
  none on Vulkan. Metal's compiler orders the same arithmetic otherwise.
- **The geometric normal** is in a material's inputs. In the forward technique it is the cross product of the
  position's differences between neighboring pixels, and in the visibility technique of the triangle's edges.
- **Primitive variables beyond the vertex are not done.** Every mesh has one vertex layout, and a material reads a
  fixed set of inputs. A material that reads a mesh's own attribute would need four things. A part names extra
  buffers of attributes, with their formats. A material type declares the attributes that it reads, by name, and the
  renderer matches them to a mesh's when a material is set. The forward technique interpolates them itself from the
  triangle's corners, as the visibility technique interpolates everything, since a varying for each attribute would
  make the vertex shader a property of the material type. That needs bindless, or buffer slots that the smallest
  device does not have. And deformation carries the attributes that move.

### Synchronization in `lib/gpu`

- Buffer barriers exist for the classic Direct3D 12 model, which the backend no longer needs. A global barrier and a
  texture barrier where a layout changes may be all that the interface needs.
- A model that attaches synchronization to a scope (a pass, or a run of dispatches or copies) matches what a graph
  knows, and maps onto Metal 4's barriers directly.
- A barrier inside a scope orders a write after a write whose result no later reader needs, which compute work does
  often. The graph assumes a barrier between writes today unless their ranges are disjoint.
- Better projections onto Metal 4 than the present one are likely, and wait for a frame complex enough to judge them.

## Not planned

An entity system, gameplay simulation, a scene graph that applications must adopt, shader compilation at run time
outside tools, and a WebGPU interface for scripts, which may come later and is a separate project.

Matching the hardware's texture derivatives across techniques. A technique gives a material the derivatives that suit
the technique, and imitates the difference that a draw's fragment shader takes only if that is measurably faster.
