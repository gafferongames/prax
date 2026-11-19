# USD

`lib/usd` reads OpenUSD layers into the document data model of the USD Core Specification, composes them into stages,
reads the schemas that drawing a stage needs, and reifies a stage for a model. It links nothing of Pixar's OpenUSD.
Everything that it depends on is prax's own (`lib/usd/CMakeLists.txt`): the core library, the matrices and
quaternions of geometry, the archives of packages and the compressed blocks of crate files, MaterialX's documents and
the meaning of its surfaces, the faces and shapes that a model draws, and the model source that it implements.

Other libraries reach it through four headers: `pr/usd.h` for layers, `pr/usd_stage.h` for stages, `pr/usd_geom.h`
for the schemas, MaterialX networks among them, and `pr/usd_model.h` for the model source. MaterialX's own headers,
`pr/materialx.h` and `pr/materialx_openpbr.h`, are lib/materialx's.

Each part follows its own reference. Layers and stages follow the Alliance for OpenUSD's Core Specification, which
covers the data model, the file formats, composition, stage population and value resolution. The schema reader
(UsdGeom, UsdShade, UsdLux) follows OpenUSD's schema definitions, which are the reference until AOUSD publishes
specifications of those domains. Where the Core Specification is silent, it names OpenUSD's behaviour as the one to
match (usd-core/#compliance-ambiguities), so the `usd` suite's reference decides those cases, and this document
records each.

The text that the module is written from, and that `usd-core/#…` references name, is the USD Core Specification
v1.1.0 (e8f44c0): `core/1.1.0/core_spec.md` of aousd/specifications-public, at the commit that
`tools/fetch/checkouts.py` pins (`USD_SPEC_COMMIT`), which lint checks each reference against. It revises 1.0, the
release that AOUSD ratified.

## prax's relationship to USD

prax takes the terms of Nick Porcino's "Reification, Oracles, and Evolution"
(https://nickporcino.com/papers/reification-oracles-evolution/):

- **Reification** makes a concrete, resolved representation of a composed description. It is eager, and lossy by
  design. It is idempotent: reifying twice gives the same result.
- **An oracle** answers questions about composed state on demand. It is authoritative within its context (the layer
  stack, the variant selections, the purpose), and it keeps nothing.
- **Authored change** belongs to USD's layers. **Emergent change**, such as simulation or interaction, belongs to the
  runtime.

Each subsystem chooses one posture toward USD. A second representation kept in sync with a stage, "USD-adjacent", is a
debt that grows with every feature, and prax has none:

- **`lib/render` never sees USD.** It consumes flat records ([render.md](render.md)).
- **`lib/model` is fed by USD, through an honest reifier** (`pr/usd_model.h`, [Models](#models)). The adapter reifies a
  stage once for each composition context: the variant selections, the purposes, and the range of time codes, whose
  time samples become tracks. It reifies again only when the stage or the context changes. The stage owns that
  invalidation: a stage is immutable, so a changed stage is a new stage, and a new source of it. No stage query happens
  on the frame's path, and reifying the same stage in the same context twice gives the same description.
- **Inspection is an oracle.** Scribe's views of a stage, its outline and its structure view ([scribe.md](scribe.md)),
  query the live composed stage when they show it, and keep nothing of it.
- **Authoring is hosted by USD**, in a tool whose product is the layer stack. An edit is a sparse opinion written to an
  edit target's layer. View state, such as a variant selection, is the session's: a stage has the session's variant
  selections (`PrUsdStageOptions::variantSelections`) and no other opinions of a session
  ([The session](#decisions-that-the-specification-leaves-open-for-stages)).
- **No library is USD-adjacent.** No generic document model over USD, kept in sync with a stage, lives in `lib/`.
- **`lib/usd` is an independent implementation** of the AOUSD specification, beside Pixar's reference and vendors'
  builds. The `usd` suite is its differential test against `usd-core`, a wheel at a version that
  `tools/fetch/checkouts.py` pins, over corpora pinned there too. The model source's diagnostics are its manifest of
  what it does not reify ([Diagnostics of the model source](#diagnostics-of-the-model-source)).
- **prax's own formats**, when they come (prax_texture, prax_material, prax_model), are reification persisted: a bake
  for the GPU behind an explicit boundary that converges, as the source's description does.

## Responsibilities

The module owns what the specification says of one layer, so that no consumer restates it:

- The three formats: text (.usda), crate (.usdc) and packages (.usdz). A file is read as what its first bytes say, so
  a .usd file is either of the first two.
- The specs of a layer, as records in an array: the layer spec, prims, attributes, relationships, variant sets and
  variants. A spec names its parent and its children by their indices, and a path finds a spec.
- The fields of each spec, by name, and their values in every type of the specification: scalars, vectors, matrices,
  quaternions, strings, tokens, asset paths, arrays, dictionaries, the value block, time samples, and the records of
  composition. A list op is the lists that the file authors, explicit, added, prepended, appended, deleted and
  ordered, and nothing is combined.
- The conversions that a format hides from its reader: the types of a text layer's values, relative paths made
  absolute, a crate file's value representations and its codings of integers and floats, and a matrix as `lib/geom`
  holds one.
- The location of an asset that a layer names, resolved against the layer that names it, and a layer read from a
  location, in a package or not.

A layer needs nothing but its own file:

- It composes nothing. Sublayers, references, payloads, inherits, specializes, variants and relocates are fields that a
  layer returns as authored. It opens no layer that a layer names.
- It resolves no value. A field that a spec does not author is absent, and its fallback is the consumer's to apply.
- It reads no file by itself except through `prUsdLayerRead`, whose opener the host provides, and `prUsdFileOpen`,
  which is the opener of a host that keeps layers in files. An opener gives a byte provider of a location
  (`PrBlobSource`, `pr/blob.h`). A location in a package is a provider of the package's file over the package's
  provider (`prUsdResourceSource`), so a package nested in a package is a provider over a provider; its first read
  finds the file in the package's directory, and the reads after it read the file's range of the package alone.

A stage owns what the specification says of many layers:

- The layers that composition reaches from a root layer, each read once through the host's opener, and their layer
  stacks, with the retiming of each layer.
- The graph of each prim: the sites that give it opinions, by local specs, inherits, variants, relocates, references,
  payloads and specializes, with the arcs that inherits and specializes imply and the mapping of each site's
  namespace. The graph lives while the prim's children compose. The stage keeps what it gives: the prim's opinions in
  strength order.
- The prims, in their composed order, with their specifiers, types, flags and properties, and the prototypes that
  instances share.
- A prim's properties by name, and by the prefix that a namespace gives its names, as `primvars:`. A stage keeps each
  prim's properties in the order of their names' bytes beside their composed order, so either search takes steps that
  grow with the logarithm of the prim's properties, and those of a namespace are listed together.
- Value resolution: metadata, attribute values at a time, sample times, targets and connections.

Neither knows a schema. A prim's type is a token, an attribute is its declared type and its values, and a property
that only a schema would give a prim is not one of its properties.

The schemas that drawing a stage needs are a third part, over a stage (`pr/usd_geom.h`): UsdGeom's transforms,
meshes, implicit surfaces, primitive variables, subsets, cameras and point instancers, UsdShade's bindings and its
preview surface, and UsdLux's lights. It follows OpenUSD's schema definitions, and OpenUSD's reading of them is the
reference. The part owns what a schema says of one prim:

- A schema's fallbacks. A stage gives no value where nothing is authored, and the schema's reading gives the fallback.
- The checks that a schema asks of values, which a stage does not make: a mesh's counts sum to its corners and its
  corners name its points, a primitive variable has as many elements as its interpolation asks, and an instancer's
  indices name its prototypes. A call that succeeds returns values that hold together.
- The walks of a network: a material's surface output to its shader, and each input through node graphs and the
  material's own inputs to a value or a texture, which itself reads a primitive variable through a reader and a 2D
  transform.

It composes nothing across prims. A transform is local, a binding and a purpose are the prim's own, and what one prim's
schema says of another, as a transform that resets the stack of its ancestors, is reported for the consumer's walk.
`prUsdShadeBoundMaterial` is the one walk that it makes, up a prim's ancestors, for a consumer that asks of one prim.

## MaterialX

MaterialX reaches a stage in two ways, which meet in one network: a layer authors a network of MaterialX's nodes in
UsdShade, with `outputs:mtlx:surface` and shaders whose `info:id` names a node definition of MaterialX's standard
library, or it references a MaterialX document, `@look.mtlx@</MaterialX/Materials>`, whose layer holds such a network.

- **A document is read by `lib/materialx`** (`pr/materialx.h`), a module of its own: MaterialX's document is a format
  of its own, with a specification of its own, which a renderer's material compiler or a tool may read without USD. It
  reads the XML into a tree of elements and their attributes, and interprets no node.
- **A document is a layer** (`PrUsdFormat_MaterialX`). `prUsdLayerCreate` reads a file that begins with `<` as a
  MaterialX document and translates it in the layout of OpenUSD's UsdMtlx file format: `/MaterialX/Materials` holds a
  Material for each `surfacematerial`, `/MaterialX/Shaders` a Shader for each shader node, and `/MaterialX/NodeGraphs`
  the node graphs and the nodes outside them, which each material references beneath itself so that its connections
  name paths under it. The translation is in `lib/usd` because a layer is what composition reads: an arc to a document
  is then an arc like any other, with its prim paths, and a file that the document names resolves against the
  document's location, as any asset path of a layer does, through the host's reader. A document's `fileprefix` is not
  applied, as UsdMtlx does not apply it: OpenUSD's flattening of the working group's `standard_surface_brass_tiled.mtlx`
  resolves `./textures/brass_color.jpg` under a prefix of `./textures/` to `textures/brass_color.jpg`.
- **What a surface means is lib/materialx's** (`pr/materialx_openpbr.h`): the inputs of OpenPBR Surface as an
  `open_pbr_surface`, a `standard_surface` or a `UsdPreviewSurface` has them, each by the input of the surface's that
  means the same, its default and the factor of its unit, and the translation of a surface's folded inputs into
  OpenPBR's: Standard Surface's specular weight, which scales its dielectric alone, becomes OpenPBR's on a metal, and its
  film is present wherever it has a thickness. A renderer's loader of MaterialX without USD reads the same mapping.
- **A network is folded, not compiled, and what folding means is lib/materialx's.** Which nodes fold, and to what, is
  MaterialX's semantics, as a surface's mapping is, so that a loader of MaterialX without USD folds the same way. What
  is USD's is the network as a stage holds it: the shaders and node graphs that UsdShade connects. An input of a
  MaterialX shader is a value, or the graph behind it folded into what a material of fixed inputs draws: one image,
  read through an affine map of its channels. Images and tiled images, constants, the arithmetic of a node by a value
  (`multiply`, `divide`, `add`, `subtract`, `mix`, `invert`, `remap`), the channel nodes (`extract`, `separate`,
  `swizzle`, `luminance`, `convert`), `switch` by a constant, and `normalmap` compose to that form exactly. A
  `geompropvalue` node's primitive variable takes an image's place as the source that the map reads, and a product of
  an image's value and a variable's value as it is keeps the variable as a tint of the whole. An image's coordinates
  pass through an affine transform, which `place2d` nodes, in either order of their operations, and a tiled image's
  tiling and offset compose exactly, as a UsdTransform2d's does for a preview surface. A tiled image's real-world
  sizes, whose units the translation does not carry, are not read. A node of another kind, a product of two images, a
  sum of two sources, and coordinates that another node computes are read approximately, and the reading says so. A
  texture's color space is its file's `colorSpace`, read as the transfer that it implies: the spaces of sRGB's
  transfer decode as sRGB, and every other space, such as the linear `acescg` and `lin_ap1`, as it is. A space's
  primaries are not a texture's concern ([model.md](model.md#textures-of-the-material-system)). The folding is the
  scope of a loader that draws MaterialX at run time without compiling a shader. A graph that needs more needs a
  material function of its own, which a translator at build time would write.

## Models

`pr/usd_model.h` is USD's model source (`pr/model_source.h`, [model.md](model.md)): a composed stage reified for a
model, and the model format of USD's layers and packages.

- **A source is one composition context's reification** (`PrUsdModelContext`): the session's variant selections, the
  purposes drawn beside `default`, and the range of time codes, which is the stage's unless the context gives one. The
  context's selections are the stage's composition, so the format composes each stage with them, and a source of a
  stage that a caller composed reports a selection that the stage does not make (`variant-selections`). A source keeps
  nothing of its context, and nothing that it read in one context serves another: another context is another source.
- **Reifying converges.** Two sources of one stage in one context have the same description, bit for bit, and their
  meshes read the same vertices. The `usd` suite checks it of every stage of its corpora.
- **Nodes are the stage's prims, with instances expanded.** Every instance of a prototype has nodes of its own, so a
  node's address is the path of the prim that it shows through instances, and a tool selects a prim of one instance
  alone. A prim that is not active, not loaded or not defined, a class, a material and its network, and a prim of a
  purpose that the context does not draw, with what is under it, are no nodes. A context draws `render`, or `proxy` in
  its place, and `guide` beside either if it asks.
- **The source's scene is in meters with +y up.** The roots take the stage's `metersPerUnit` and `upAxis`, and a
  transform that resets the stack takes them alone.
- **A mesh is a Mesh prim with the binding that its ancestors give it**, so the instances of a prototype that bind
  alike share one mesh. Its parts are its subsets of the `materialBind` family, then the faces that no subset holds.
- **An implicit surface is a mesh of its shape** (`prUsdGeomShapeRead`, `pr/mesh_shapes.h`), and **faces become
  triangles in lib/mesh** (`pr/mesh_polygons.h`). A left-handed mesh's triangles are turned to the renderer's winding.
- **Normals are the stage's for a mesh that no scheme subdivides**; one of a subdivision scheme has its cage's smooth
  normals, and its faces are not refined (`subdivision`).
- **A vertex is a corner of a face**, with what each primitive variable that a request names gives that corner at its
  interpolation, and texture coordinates with t turned to v down the image. Each primitive variable that a texture
  reads is a set of texture coordinates, in the order in which the materials name them, up to two sets: a texture
  whose variable would be a third set reads the first (`texture-coordinate-sets`).
- **A material is its MaterialX network or its preview surface.** A network that reaches a surface that lib/materialx
  maps is a material of OpenPBR's inputs, each folded into a value, an image through a map of its channels, a primitive
  variable through a map, or an image tinted by a variable. Otherwise the material is UsdPreviewSurface's inputs, each a
  value, a texture of its UsdUVTexture's scale, bias and channel, or a primitive variable that a reader gives. A prim that
  binds no material draws its display color and opacity, which are inputs that the vertices give.
- **A texture is a file that the stage resolves against the layer that names it**, whose provider the source opens when
  it is made, through its options' opener; a texture of `auto` color space is sRGB for a color input and data for the
  others, and a texture of a linear space is data for any input, whatever primaries the space names.
- **A point instancer draws each mesh of its prototypes at every visible instance**, as instances of one draw at the
  instancer's node.
- **The context's time codes are one animation**, which starts at the rest pose. Each value that varies and that a
  track can hold is a track, keyed at the time samples that decide it, at the range's start and end, and at each time
  code between: a node's matrix and its visibility, a light's color, intensity and cone, a camera's field of view,
  aspect, magnification and clipping range, and a material's input of a value. A frame evaluates the tracks and
  resolves no value of the stage.
- **What varies and no track holds is reified at the start**, and a diagnostic names it: a mesh's points, normals,
  primitive variables, shape or subsets, an instancer's instances, the environment, a material's textures, primitive
  variables or alpha, a light's kind or shape, and a camera's kind of projection. A stage whose values vary in a context
  of no time codes is reified at its start too.
- **A light is what UsdLux describes of it**: a distant light is directional, and a sphere, a disk, a rectangle or a
  cylinder is a light of that area, with its sizes in meters, its normalization and its shaping cone. Its photometric
  intensity is its intensity times 2 to its exposure: the luminance of its surface in nits, or a distant light's
  illuminance in lux. A light of another kind lights nothing (`lights`). How a model draws a light is lib/model's
  ([model.md](model.md#sources)).
- **The first dome light is the environment**: OpenEXR's latitude-longitude map in the dome's frame, top pole along +y,
  or along a DomeLight_1's `poleAxis`, tinted by its color and scaled by its intensity times 2 to its exposure. A
  texture of another layout is not read (`environment-format`).

### Diagnostics of the model source

The features that a source names (`PrModelDiagnostic`) are what the adapter does not reify as the stage means it. The
list is the adapter's capability manifest: a feature that the adapter learns leaves it.

| Feature | What the source does instead |
|---|---|
| `subdivision` | draws a subdivision surface's cage, with its smooth normals |
| `primitives` | draws nothing of a geometric primitive that is neither a mesh nor an implicit surface |
| `texture-coordinate-sets` | reads the first set of texture coordinates for a texture whose primitive variable would be a third set |
| `skeletons` | draws the meshes that UsdSkel's skeletons and blend shapes bind as they are authored, without skinning or blend shapes |
| `lights` | lights nothing for a light of a kind other than UsdLux's distant, sphere, disk, rectangle, cylinder and dome lights |
| `material-networks` | draws a MaterialX node that is not folded as its first input, or a partly metal Standard Surface with its specular weight |
| `environment-format` | lights evenly for a dome texture that is not a latitude-longitude map |
| `variant-selections` | shows the stage's selection where the context's differs |
| `time-range` | shows values that vary at the start, in a context of no time codes |
| `mesh-animation` | draws a mesh as it is at the start |
| `instancer-animation` | draws an instancer's instances as they are at the start |
| `environment-animation` | lights the scene with the environment as it is at the start |
| `material-animation` | draws a material whose textures, primitive variables or alpha vary as it is at the start |
| `light-animation` | keeps a light's kind and shape at the start |
| `camera-animation` | keeps a camera's kind of projection at the start |

## What a layer guarantees

All of a file is data from outside the program. `prUsdLayerCreate` checks it as it reads, and returns a layer only if
every check passes. A consumer follows indices, reads values as the types that they state, and treats paths as paths,
without checking any of it again. The comment at the top of `pr/usd.h` lists the guarantees. In outline:

- The specs form one tree, each under a parent of a form that may hold it, and each at a path that no other spec has.
- A spec's lists of children hold exactly its children, in the order of the file.
- Every value has a type of the module's, and as many elements of that type as it states.
- Every path is a path of the specification's grammar, and is absolute.
- The times of time samples increase, and the keys of dictionaries are distinct and sorted.
- The fields that the specification requires of a spec are present, with their fallbacks if the file leaves them out.

A file that fails a check is refused whole, with a status and a sentence that says what failed. A crate file is
binary, so every offset, count and index in it is checked against the file before it is used, and a file that states
more than it holds is refused. The module does not repair a file. A layer that holds a spline, in either format, is
refused as NotSupported, so composition treats it as a layer that cannot be read
(`PrUsdErrorKind_SublayerUnreadable`, `PrUsdErrorKind_ArcUnreadable`).

The module checks structure, not meaning. It does not check that a path names a spec, that an asset path names a
file, that a prim's type is one that a schema knows, or that a quaternion has unit length. These are composition's and
the schemas' to decide.

## Decisions that the specification leaves open

- **The types of a text layer's fields.** The text grammar gives a field's value no type, and the specification
  leaves the registry of fields to implementations and schemas. The module keeps a table of the fields of the data
  model and of the fields that OpenUSD's schemas register, as `upAxis` and `apiSchemas`. A field outside the table is
  kept as its text (`PrUsdType_Unregistered`), or as a dictionary if it is written as one.
- **Children.** The fields that list a spec's children are the layer's lists of specs, and are not fields of the
  spec. In a crate file, a child that a list names without a spec, and a spec that no list names, refuse the file.
- **Matrices.** A matrix value is the transpose of the rows that a file holds. USD multiplies a row vector by a
  matrix, and `lib/geom`'s matrices map column vectors, so the transpose is the same transform.
- **List ops.** The explicit list of a list op and its lists that edit do not coexist. In a text layer, the statement
  that comes last decides which the list op is. An explicit list of variant sets, and a list of the deprecated `add`,
  also declares a spec for each of its variant sets.
- **Identifiers.** A character outside ASCII is accepted wherever the grammar takes a letter, which is wider than the
  Unicode classes that the grammar names.
- **Crate versions.** Versions 0.4.0 to 0.12.0 are read. The specification asks for 0.8.0 and later.
- **Legacy specs.** The specs of a crate file that are not specs of the specification, such as a relationship's
  target, are passed over.
- **Packages.** A package whose files are not aligned, or whose archive has a comment, is read, and
  `prUsdPackageIsConforming` reports it. A package that compresses a file is refused.

## What a stage guarantees

Everything that a layer tells composition to do is data from outside the program too. `prUsdStageCreate` checks it as
it composes, and the comment at the top of `pr/usd_stage.h` lists what a consumer may then rely on. In outline:

- Composition ends. A layer that is its own sublayer, an arc to a site that the arc's own site comes from, and arcs
  nested or chained past a fixed bound are composition errors, and what is wrong is ignored.
- A composition error is never fatal (usd-core/#composition-errors). The stage records a kind, the prim and a
  sentence, and composes around it. Only a root layer that cannot be read fails `prUsdStageCreate`.
- Every index that a stage gives is in range, and every opinion is a spec of a layer that was read.
- The prims are in depth-first order, with children in their composed order, and a prim's opinions are in strength
  order.

A stage is immutable once composed. Any number of threads may read it, each with a `PrUsdScratch` of its own for the
values that resolving has to make. To compose with other payloads, other variant fallbacks or other selections of the
session, a host makes another stage. Composition has no state that an edit could update in place, so a change is a new
stage, and what reified the old one reifies the new one.

## How a stage composes

The specification gives composition as rules and an outline, not an algorithm. A prim's graph is built by a queue of
tasks over its nodes, in a fixed order of kinds: relocates, references, payloads, inherits, specializes, the arcs that
inherits and specializes imply in the layer stacks above them, and last the variants, whose selection may come from
any site that the others brought. A task that adds a node queues that node's tasks.

- A child prim's graph begins as its parent's, with the child's name appended to every site. This is how the arcs of
  ancestors reach a prim, and why composing a prim costs what its own graph holds and not what its ancestors' did.
- An arc to a prim below a root prim composes that prim with its ancestors, in a nested composition, before it joins
  the graph, so that the ancestors' arcs come with it. A nested composition selects no variant. When its graph has
  joined the graph that nothing nests, the variant sets of the ancestors of each of its sites are selected there, each
  variant as an arc under the site's node to the site's path within the variant, composed with what the variant
  brings the prims under it. The selection may then come from any site of the whole graph, an implied class too.
- A node that gives no opinions and has nothing under it that does is culled when the prim is done, so the prims below
  never carry it.
- A specializes arc below the root leaves a placeholder where it is authored or implied, and the arc itself is a child
  of the root, which makes its opinions weaker than every other arc's (usd-core/#specializes).
- A prim under an instance takes opinions only from the arcs of the instance itself, so every instance with the same
  arcs composes the same prims below it. The stage composes them once, as a prototype, from the first such instance
  that it meets (usd-core/#scene-graph-instancing).

Composing a stage is linear in what it holds: each prim costs the nodes of its graph, the specs at their sites, and
the names of its children and properties, through hashed lookups of paths, of specs by layer and path, and of
prototypes by key. A prim with thousands of arcs is linear in them too. The arcs of a site, and the arcs that another
node's imply, are added strongest first, so an arc takes its place among its siblings after the one that its parent
took last, without a walk of the others. Two siblings compare by their numbers in their parent's children, which are
made when two are first compared and hold while arcs join at the end. A site that some node already has, the class
that an arc already implied, and the copy at the root of a specializes arc are found by key. What a composition keeps
for this it makes when it first adds an arc, so a prim that adds none pays nothing. Paths are numbers in a table
(`usd_path.h`), where replacing a prefix walks the parts that follow it, and the mappings of arcs are interned, so
that equal mappings are one number and composing two is looked up after the first time.

## Decisions that the specification leaves open, for stages

Each is what OpenUSD does at the version that the suite pins, found by the suite, unless it says otherwise.

- **The direction of a layer offset.** A time of the layer that is brought in, scaled and then offset, is a time of
  the layer that brings it in. The specification's sublayer example has it this way (usd-core/#sublayer-time-offsets).
  Its value resolution example has it the other way, and is wrong (see the errata below).
- **Where a sample is.** A sample is the value at the time that the stage lists it at: its layer's time, retimed. A
  query compares its time with those times, and interpolates between them, so the rounding of a retiming never turns a
  sample's own time into a time just beside it.
- **The time scale of a layer** is its `timeCodesPerSecond`, else its `framesPerSecond`, else 24
  (usd-core/#effective-time-codes-per-second). A reference's or a payload's offset is scaled by the time scale of the
  layer that authors the arc over that of the layer stack that it brings in
  (usd-core/#reference-and-payload-offset-adjustment), and the layer's own retiming, which holds its time scale against
  its stack's, applies after. An arc to a prim of its own layer stack is scaled the same way, which the specification
  does not ask and OpenUSD does. A scale that is not positive, from an offset or from time scales, is a composition
  error and is taken as 1.
- **The identity of a reference in a list op** is its asset path, resolved against the layer that authors it, its prim
  path and the offset that it states. The retiming of the layer that states it is no part of it, so two layers of a
  stack that list the same reference list one reference, with the retiming of the stronger layer.
- **Implied arcs to a site that the arc's own node has** leave a placeholder, which gives no opinions, so that the
  layer stacks above still have the arc implied through it.
- **The order of specializes arcs** is the order in which they would be met as ordinary arcs where they were authored,
  and the arcs that one authored arc implies follow it together, strongest layer stack first.
- **The classes under a specializes arc** are implied in the node above it wherever the arc is authored, at the root
  too. The classes under the copy at the root of an arc from below are implied in the node above the copy's
  placeholder, as they would be if the arc were where it was authored, and the placeholder stays in the graphs of
  the prims below for as long as the copy does.
- **A class that two arcs imply at one site** is one node, implied from the stronger of the two, whichever composition
  met first. Its place among its siblings follows from that.
- **A cycle** is an arc to a site of the same layer stack as a node on the way from the root to the arc's node, at
  that node's path, above it or below it. For a nested composition the site is taken where it will be in the graph
  that nests it.
- **Relocates.** The rules of the specification (usd-core/#relocates), with the conflicts that OpenUSD rejects: two
  relocates to one target, a target that is the source of another, a source that is the target of another, and a
  source or a target under the source of another. An arc to a path at or under a relocate's source is a composition
  error and is ignored. A relocate with an empty target removes the prim.
- **The order of children** (usd-core/#ordered-prim-children) applies each layer's `primOrder` as OpenUSD's list
  ordering does: each name of the order takes the names that follow it up to the next name of the order, and the names
  before them all stay first. This is not the `reorder` of the specification's pseudocode.
- **The order of properties** is the strongest `propertyOrder`'s names first, then the others by name, which is the
  specification's `reorder` (usd-core/#ordered-property-children). Names order as OpenUSD orders them, which is not
  the specification's example (see the errata).
- **Specifiers.** A defining specifier is stronger than `over`, and a `class` that reaches the prim through an inherit
  of the prim itself is weaker than any other defining specifier (usd-core/#specifier).
- **A prim's type** is the strongest opinion that is not empty and not `__AnyType__`.
- **Variability** is the weakest opinion (usd-core/#variability), and `custom` is true if any opinion is.
- **Variant selection** (usd-core/#computing-variant-selection): a variant set that an earlier arc of the prim
  selected takes that selection in every layer stack. Otherwise the session's selection decides, for a prim whose path
  the root layer stack holds, as a session layer's opinion would (see the session, below). Otherwise
  the strongest opinion of any site decides, weaker sites than the variant set's own included. A selection of the
  empty name selects nothing. A set without a selection takes the first of the stage's fallbacks that the set has as
  a variant.
- **The variant sets of a prim** are the names that each site's `variantSetNames` make across the layers of its
  stack, strongest site first, each name once.
- **A variant selection that an earlier arc made** is taken again for the same prim: the search maps the path that
  lists the variant set as far up the graph as the arcs map it, and then down every arc, and a variant's node whose
  path, where it was introduced and without the selection, is the mapped path decides.
- **The order of variant sets.** Those of the ancestors of the sites that an arc brought are selected before those of
  the sites themselves, the strongest node's first, and for one node the ancestor nearest the root first. A fallback
  is taken only when no set of its group has an authored selection left to try.
- **The variants of a placeholder's ancestors.** A node that gives no opinions because its prim is under an instance
  takes the variants of its site's ancestors above the instance, and those do give opinions. The variants that the
  instance's own node lists are not composed for an arc to a prim under the instance. A relocate's source takes the
  variants of all its ancestors, and a placeholder that was added without its ancestors takes none.
- **A duplicate under a variant of an ancestor.** Where an arc is added only if no node gives its site's opinions, as
  an implied class is, so are the variants of its ancestors and the arcs under them.
- **Payloads.** A stage loads all or none, and with none it loads the payloads of the prims that the options name,
  with their ancestors and descendants. The payloads on the ancestors of an arc's target always load.
- **Instances.** A prim is an instance if its strongest `instanceable` is true and an arc of its own brings specs
  (usd-core/#instancing). The specification does not say when two instances share. They share when their keys are
  equal: for each node of the graph that came by an arc of the instance itself and has specs, in strength order, the
  kind of arc, the layer stack, the path and the retiming. A variant of the instance is such an arc, so instances
  that select different variants do not share. An instance keeps its own opinions and properties, and its children
  are its prototype's. A prototype is named `__Prototype_` and a number, in the order that the stage met them, and
  a path of the instance's namespace under the first instance resolves to a path under the prototype.
- **A prim that is not active** has no children, and is not loaded.
- **Dictionaries** combine key by key down the opinions, and a dictionary under a key of two combines in turn
  (usd-core/#dictionaries). **A list op** resolves to the explicit list that the opinions' list ops make, applied
  weakest first to an empty list (usd-core/#list-op-resolution).
- **Attribute values** (usd-core/#attribute-resolution): at a time, the strongest opinion with time samples or a
  default decides, samples before a default in one spec. A block is no value, whatever is weaker. Between two samples
  a value interpolates if both have a type that does and the same length, and the sample before is held otherwise,
  as it is if the sample after is a block. A stage knows no fallback: without an opinion there is no value.
- **Targets and connections** combine as list ops, weakest opinion first, each path mapped into the stage's namespace
  by the mapping of each arc from its site to the root, in turn. A mapping is a bijection
  (usd-core/#namespace-mappings), so an arc does not map a path that it would not map back, and one mapping composed
  of the arcs' mappings would: a connection that a class makes to the prim of one of its instances is no connection
  of that instance, whatever references it. A path that an arc does not map is left out, and `prUsdStageTargets`
  counts those. Forwarding follows each relationship once. An explicit list replaces what weaker opinions list, so
  their paths are neither mapped nor counted.
- **What OpenUSD's composition can check of a target, and its stage does not.** A connection that a class makes to a
  prim of one of its instances, and a target that a permission denies, are errors of OpenUSD's composition when it is
  asked for a property's targets, and its stage asks without that check. lib/usd resolves targets as the stage does.
- **A site that a relocate emptied.** Where the arcs of a prim's ancestors bring a site whose path is the source of a
  relocate of the site's own layer stack, the prim is not there, and the arc of the prim that leads to that site is a
  composition error that gives the prim nothing, with everything else that the arc brought. A variant's site is
  exempt: a variant says what it says of a relocated prim at the path before the relocate.
- **A node that a relocate replaced.** A relocate's target takes the opinions of its source in place of what the arcs
  of the target's ancestors brought it. If one of those arcs brought a site that is itself the target of a relocate,
  that relocate is not followed: the site gives the prim nothing, and neither does its source.
- **Mappings block.** Where two mappings compose and the outer does not map the target of a pair of the inner, the
  pair stays as a block, so that the paths under it map to nothing and not by a pair above it. A prim that a relocate
  moved out of what a reference maps is then no target in the stage that references it.
- **The relocates in an arc's mapping** are those whose source is at or under the arc's site, where the source is
  taken as authored, in the namespace that the relocates of its ancestors made, or else as it was before any relocate.
- **A property's opinions** are the specs of its name that have the form, attribute or relationship, of the strongest.
  A spec of the other form is a composition error and gives the property nothing, except that the weakest spec of the
  name still decides the variability, as in OpenUSD's stage, where a relationship's is uniform.
- **A time code** that an attribute holds is a time of its layer, and resolves through its opinion's retiming.
- **Permissions.** The `permission` field is deprecated, and the specification does not require it supported
  (usd-core/#permission). OpenUSD's stage does not consult it, and the results that the specification's composition
  tests expect of the layers that author it have every opinion that it would deny. A stage reads it as any other
  field, and it restricts nothing.
- **A target that maps to nothing is no composition error.** A stage resolves targets when it is asked for them, as
  OpenUSD's does, and `prUsdStageTargets` counts the paths that do not map with the answer.
- **The session.** The specification has no session layer. A stage has the session's variant selections
  (`PrUsdStageOptions::variantSelections`), which decide where OpenUSD's session layer would (see variant selection,
  above), and no other opinions of a session.

## Errata of the Core Specification v1.1.0 (e8f44c0)

Where the text of the Core Specification that the module is written from (`USD_SPEC_COMMIT`) or its examples differ
from what OpenUSD does, which the specification's own rule makes the expected behaviour:

- **Layer Offset and Scale** (usd-core/#layer-offset-and-scale). The example maps stage time 12 through an offset of
  10 and a scale of 0.5 to layer time 16, as 12 * 0.5 + 10. OpenUSD maps a layer's time to the stage's by that
  formula, so stage time 12 is layer time (12 - 10) / 0.5 = 4, as the specification's own sublayer example has it.
- **Element Ordering** (usd-core/#element-ordering). The rules put `_` before digits and letters, and the example
  orders `foo_bar` before `Foobar` and `foobar` before `foo00001bar`. OpenUSD orders a digit before `_`, and `_`
  before a letter: `foo00001bar`, `foo001bar001abc`, `foo001bar002abc`, `foo0001bar0002xyz`, `foo_bar`, `Foobar`,
  `foobar`.
- **Ordered Prim Children** (usd-core/#ordered-prim-children). `reorder` puts the names of the order first. OpenUSD's
  ordering of prim children keeps the names that precede every ordered name first, and moves each ordered name with
  the names that follow it. For properties OpenUSD does what `reorder` says.
- **Inherits** (usd-core/#inherits). "If none of the layers ... contain specs for the inherit prim path, it is a
  composition error." OpenUSD reports nothing for an inherit or a specializes of a prim without specs, and its own
  tests name that as intended (BasicSpecializes).
- **Variants** (usd-core/#variants) says nothing of fallbacks, which OpenUSD's composition takes as an input and the
  specification's composition tests rely on (`standin` falls back to `render`).
- **Strength Ordering** (usd-core/#specializes) does not order two specializes arcs of different sites against each
  other, nor a prim's own specializes against those of what it references. In OpenUSD the specializes that a
  referenced prim brings are stronger than the referencing prim's own.
- **Composition Arcs.** The inherits example's layer lacks the closing parenthesis of `Inherit_1`'s metadata, and the
  relocates passage writes the mapping as **\[(`/Ref`, `/A`), (`/Ref/B,`/A/C\`)\]**.
- **Variable expressions** are in the supplement's composition tests (ExpressionsInReferences, ExpressionsInPayloads)
  and not in the specification, which lists them as not yet covered.

## Testing

The unit tests read layers written in the tests' own text and bytes: each production of the text grammar, each type
of value in both formats, and each refusal. A test writes a crate file with a writer of its own, so that it can state
any offset, count and index, and one test changes every byte of a file in turn. The stage's tests compose small sets
of layers held in memory: each arc, each rule of strength, each composition error, the load options, instancing, and
each rule of value resolution.

The `usd` suite reads the USD working group's assets and the layers of the specification's supplement, and compares
each layer with what Pixar's OpenUSD reads of it, spec for spec and bit for bit (docs/testing.md). It then composes
every layer and package of both as the root of a stage, and compares the stage with the one that OpenUSD composes:
the prims with their specifiers, types and flags, every prim's opinions in strength order, resolved metadata,
properties, defaults, values at and between samples, targets and connections. Last, it composes the supplement's
composition tests against the results that they expect: each prim's opinions, variant selections, children, the
names that relocates forbid its children, properties, targets and connections, the retiming of each node of its
graph, and the kinds of composition error that each prim has. It reifies every stage for a model three times, twice
from one stage and once from the layers composed again, and checks that the three descriptions and their meshes are
equal. OpenUSD is the reference only: the suite runs its published
wheel in a Python environment of its own under `third_party`, and builds and links none of it.
