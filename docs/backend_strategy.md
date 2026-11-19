# Backend strategy

Prax is a toolkit, not a framework. An application takes the libraries it needs, and it may replace any interface's
implementation with its own. This plan makes the code follow that rule in three places where it does not today:
libraries that choose their own backends; the image and compression codecs, which grow as monoliths; and format
libraries that depend on the interfaces they serve.

The rule that all three follow: **a library is handed its dependencies, and composition code chooses backends.** A
program's `main` is its composition root. A composition library, such as `lib/studio/app`, puts the pieces together
for programs of a typical shape, and chooses backends as a program would. A program of another shape composes the
pieces itself.

## Problems

**Libraries choose backends.** The `pr<Interface>Create(Pr<Interface>Backend_Default, ...)` constructors are meant for
programs. Libraries call them too:

| caller | what it creates |
| --- | --- |
| `lib/studio/terminal/src/terminal.cpp` | its own text shaper, compression codec and image codec |
| `lib/canvas/src/canvas.cpp` | a draw renderer |
| `lib/draw/backends/shared/src/draw_shared_memory.cpp` | a memory GPU device, through the generic constructor |

A library that does this cannot be given another implementation. It also links every backend, because the generic
constructor names all of them.

**The built-in codecs are monoliths.** `prImageBuiltinCreate` decodes PNG, JPEG, GIF, ICO, TIFF and emjc through one
dispatcher. `prCompressBuiltinCreate` does the same for Deflate, Brotli and LZFSE. Brotli has a special option,
`PR_COMPRESS_BROTLI`, so that a program can leave it out. An application cannot take one format without the others,
and a format with an API of its own, such as PNG's text chunks (`pr/image_png.h`), has no home but the generic module.

**Format libraries depend on the interfaces they serve.** `lib/gltf` and `lib/usd` hold their model sources
(`gltf_model.cpp`, `usd_model.cpp`), so both link `lib/model`, and through it `lib/render` and `lib/gpu`: a program that
reads a glTF document to inspect it links the 3D renderer. `lib/ktx` and `lib/image` hold their texture sources the
same way. `lib/gltf` links its Draco and meshopt decoders whether or not an asset uses them.

## Selectors

Each interface with backends gets a companion library, its selector, at `lib/<module>/select/`, as
`prax::<module>_select`. Every such interface gets a selector, including those with one backend today, so that the
rule has no exceptions.

The interfaces are: app, audio output, compress, draw renderer, draw writer, fetch network, font hinter, font system,
gpu, http, image, media, model, net, process, task pool, text shaper, texture, tls, websocket and webview.

- **A selector owns the choice.** It holds the `Pr<Interface>Backend` enum, `FromName`, `Name`, `IsBuilt`, and
  `Create(backend, ...)` with `Default`. These move out of the modules.
- **A module keeps its concrete constructors**, such as `prCompressBuiltinCreate`, `prAppAppKitCreate` and
  `prGpuMemoryCreate`. A library may call one if it needs that backend in particular. The CPU renderer needs a memory
  device, so `draw_shared_memory.cpp` calls `prGpuMemoryCreate`. A library never chooses a default.
- **A selector chooses within its own interface only.** `image_select` takes a compression codec set and never creates
  one. The program connects one interface to another. A drawing program links `gpu_select`, `draw_select` and
  `text_select`, and nothing of the networking stack.
- **Composition code links selectors.** `apps/`, `examples/`, tests, suites and composition libraries such as
  `lib/studio/app` link selectors. Other libraries take their objects from their callers.
- **A module's root is its interface, and `select/` is a target beside it.** A directory under `lib/<module>/` with a
  `CMakeLists.txt` of its own is a target of its own; `backends/` holds sources of the module's target. A family of
  peer targets with no root target, as `lib/studio` is, is a group.

`lib/model/host` becomes `lib/model/select`: it builds the default model format set and texture format set from the
adapters that the build has, and takes the image and compression codec sets from the program.

### Choosing in the environment

The environment chooses backends, never a program's arguments. `lib/backend` (`prax::backend`) is the only library
that reads the choice. It defines a descriptor, `PrBackendInterface`, which holds an interface's name, the names of its
backends, and which of them the build includes. Each selector returns the descriptor of its interface from a function.
A selector's `Default` reads `PR_BACKEND_<NAME>` through `prBackendFromEnvironment`, so every program honors the
same variables without code of its own. No other library reads an environment variable to choose a backend, so a
variable such as `PR_GPU_BACKEND` becomes `PR_BACKEND_GPU`. `dev.sh` and the suites set the variables to run a program
under a matrix of backends:

```
PR_BACKEND_GPU=vulkan PR_BACKEND_RENDERER=fragment
PR_BACKEND_IMAGE=jpeg_turbo,builtin
```

Choosing a backend that the build does not include is an error, and the message lists the backends that the build
includes. The default is never a silent fallback. On the web, the page's query sets the variables.

An interface whose implementation is a format set takes a list of backends in priority order (see Format sets). A
selector for such an interface names presets, such as `builtin` for every built-in codec the build has.

### Libraries that take their dependencies

- `lib/studio/app` creates the app, the device, the renderer, the text shaper and the codecs through their selectors.
- The terminal receives its text shaper and codec sets from the studio app.
- Canvas receives the renderer that presents it from its description.

The wiring that several examples repeat goes in a shared source under `examples/`, not in a library. Applications such
as `apps/browser` keep their wiring explicit.

## Declaring backends in CMake

Today each module's `CMakeLists.txt` writes the conditions for shared sources by hand:

```cmake
if (PR_DRAW_TRIANGLES OR PR_DRAW_FRAGMENT)
    target_sources(pr_draw PRIVATE backends/shared/src/draw_shared_atlas.cpp)
```

Those lists have to be kept current by hand as backends are added. A helper declares each backend and each shared unit
instead:

```cmake
pr_backend_shared(draw atlas SOURCES backends/shared/src/draw_shared_atlas.cpp INCLUDE backends/shared/include)

pr_backend(draw triangles SHARED atlas INCLUDE backends/triangles/include SOURCES backends/triangles/src/draw_triangles.cpp)
pr_backend(draw svg SHARED writer REQUIRES cpu INCLUDE backends/svg/include SOURCES backends/svg/src/draw_svg.cpp)
pr_backend(draw canvas WHEN PR_GPU_CANVAS2D INCLUDE backends/canvas/include SOURCES backends/canvas/src/draw_canvas.cpp)

pr_select(draw INCLUDE select/include SOURCES select/src/draw_select.cpp)
```

- **Options stay in the root `CMakeLists.txt`**, declared with `pr_backend_option` along with each platform's
  default. The README's table and the options then come from one place.
- `pr_backend` builds a backend if its option is on and every variable in `WHEN` is true. It adds the backend's sources
  and headers, and defines `PR_<MODULE>_HAS_<BACKEND>` on the module's target. It also adds each shared unit
  (`pr_backend_shared`) that the backend uses, once.
- `REQUIRES` replaces the hand-written `FATAL_ERROR` checks between backends.
- `pr_select` adds the module's selector, `prax::<module>_select`. The selector links the module and `lib/backend`. It
  receives the same definitions as the module, so the two agree on which backends the build includes.
- Build steps other than compiling sources, such as compiling a backend's shaders, stay in the module's
  `CMakeLists.txt`, under the backend's option.
- `dev.sh lint` checks the README's table of backends against the declarations.

## Format sets

An interface whose implementations each read some formats is a collection of them, not one implementation that a
backend replaces whole. One pattern serves every such interface:

| set | member | holds |
| --- | --- | --- |
| `PrImageCodecSet` | `PrImageCodec` | image codecs |
| `PrCompressCodecSet` | `PrCompressCodec` | compression codecs |
| `PrTextureFormatSet` | `PrTextureFormat` | the formats that make texture sources of encoded bytes |
| `PrModelFormatSet` | `PrModelFormat` | the formats that make model sources of a resource |
| `PrGltfExtensionSet` | `PrGltfExtension` | the glTF extensions that carry code of their own |

- **A member is the backend interface.** A codec or a format reads one or more formats. Each of Prax's own reads one.
  A platform codec, such as ImageIO, WIC or Apple's Compression framework, may read many.
- **A set is a concrete type**, with no function table, like `PrFontSet`. It is built from an ordered list of members
  and owns them. It does not change once it is built, so any thread may use it.
- **The order is the priority.** To open a resource, the set asks each member in order whether it reads the media type
  or recognizes the bytes, and the first that does not refuse them reads them. If libjpeg-turbo comes before the
  built-in JPEG codec, it decodes JPEG.
- **Callers take sets.** Every function that takes one member to read with takes the set.
- **A member that depends on another interface takes it in its constructor.** The PNG and emjc image codecs take the
  compression codec set that they decompress through, and the KTX texture format takes it for supercompression.
- **A container reads a whole embedded resource through the set.** The set passes itself to a member when it opens a
  resource. An ICO entry that holds a PNG is opened as a nested image. A build with ICO and without PNG decodes bitmap
  icons, and reports NotSupported for PNG entries.

This replaces the rule in [conventions.md](conventions.md) that a host combines implementations by writing a backend
that dispatches to them.

## Format libraries

Each format is a top-level library, as `lib/gltf` and `lib/usd` are:

| kind | libraries |
| --- | --- |
| image | `png`, `jpeg`, `gif`, `ico`, `tiff`, `emjc`, `webp`, `ktx`, `basis`, `hdr`, `exr`, and later `psd` |
| compression | `deflate`, `brotli`, `lzfse`, `lz4`, `zstd` |
| archive | `zip`, from `lib/compress/src/compress_zip.cpp` |
| scene | `gltf`, `usd`, `materialx` |
| mesh compression | `draco`, `meshopt` |

- **A format library holds the format's API.** That means PNG's text chunks, PSD's layers, JPEG's abbreviated streams,
  the block transcoding of KTX and Basis that a GPU wants, a glTF document and a USD stage. A format library works with
  core's types (`pr/pixels.h`, `pr/blob.h`) and its own.
- **A format library depends on no interface that it serves.** `lib/png` does not link `lib/image`, `lib/ktx` does not
  link `lib/texture`, and `lib/gltf` and `lib/usd` do not link `lib/model`.
- **An interface's backends adapt the format libraries.** Each adapter is one small source under the interface's
  `backends/`, and a member of its set:

  | interface | adapters |
  | --- | --- |
  | `lib/image` | `backends/builtin`: one per image format library, as `prImagePngCreate` |
  | `lib/compress` | `backends/builtin`: one per compression format library |
  | `lib/texture` | `backends/ktx` and `backends/image`: KTX 2.0's texture sources, and those of any image that an image codec set decodes |
  | `lib/model` | `backends/gltf` and `backends/usd`: the model sources of glTF documents and USD stages |
  | `lib/gltf` | `backends/draco` and `backends/meshopt`: the extensions `KHR_draco_mesh_compression` and `EXT_meshopt_compression` |

  The selector's `builtin` preset lists every adapter that the build has.
- **Only an interface's backends reach a format.** The adapters are sources of the interface's target, so the target
  links the format libraries that they adapt, and a program carries only the adapters that it calls. The interface's
  own sources include no format's header: `dev.sh lint` fails if a source of `lib/image`, `lib/compress`,
  `lib/texture` or `lib/model` outside `backends/` includes one (`format-dependency`).
- **The platform backends stay under `backends/`**, such as ImageIO, WIC and Apple's Compression framework. They have no
  API of their own.

### Dependencies between formats

- **A format that needs part of another format's stream depends on its library directly.** TIFF's JPEG compression
  stores abbreviated JPEG streams, with their tables in the `JPEGTables` tag, so `lib/tiff` links `lib/jpeg`. KTX2
  carries Basis Universal payloads without a Basis file header, so `lib/ktx` links `lib/basis`. A USD stage reads a
  MaterialX document as a layer, so `lib/usd` links `lib/materialx`.
- **A format that needs a whole compressed stream reaches it through `PrCompressCodecSet`.** PNG gets zlib from the
  set, not by linking `lib/deflate`, so that an application can substitute zlib-ng or the system's zlib. KTX's
  supercompression and USD's LZ4 work this way.
- **A whole embedded image goes through `PrImageCodecSet`**, as in Format sets.

### glTF extensions

`lib/gltf` reads every extension that is data alone, such as the `KHR_materials_*` family, since those cost nothing.
An extension that carries code of its own is a member of the extension set that the options hand in
(`PrGltfOptions::extensions`): Draco's and meshopt's decoders, each in its own library, and adapted under
`lib/gltf/backends/`. lib/gltf stays strict about required extensions: it refuses an asset that requires an extension
that it neither reads nor has in its set.

### Shared coding primitives

One C++ library holds the pieces that the compression and image formats share. Like `lib/core/cpp`, it has C++ headers
only, no C API, and it is linked privately:

- bit readers: least significant bit first for Deflate, Brotli and WebP lossless, most significant bit first for JPEG,
  and backward for FSE and zstd
- canonical Huffman tables
- FSE, the tANS coder of LZFSE and zstd, implemented once
- the LZ match copy, `compress_copy.h` today

It stays one library until a part grows an API of its own. With static linking and COMDAT folding, splitting it would
not make binaries smaller.

### Build options

A format library is a target of its own. A program that calls neither it nor its adapter does not carry it, and one
that reads a format's API alone links the format library alone. Prax's own formats therefore have no `PR_` options, and
an install build compiles them all. `PR_COMPRESS_BROTLI` goes away.

Options remain for the adapters of third-party libraries, such as libjpeg-turbo and libzstd, and for the platform
backends, under the README's rule. A backend that uses the platform's API or Prax's code is on by default, and one that
depends on a third-party library is off.

## Web backends

On the web, `lib/net` has only `PR_NET_NULL`, so the backends built on it have no connections there.

- **fetch** gets `PR_FETCH_WEB`, over the browser's `fetch`. Every load is asynchronous, so a glTF asset or a USD stage
  on the web reads its resources through providers over it.
- **websocket** gets `PR_WEBSOCKET_WEB`, over the browser's `WebSocket`.

On the web, the selector's `Default` for each is the web backend.

## Plan

The work lands on the render3d branch, before its 3D restructuring, and splits into commits that come first in its
series, so each format moves once, to where it belongs.

1. **Selectors.** Add `lib/backend`, with the `PrBackendInterface` descriptor and the environment's choices. Add the
   `pr_backend` helper, and move `lib/draw` and `lib/app` to it first. Add a selector for every interface, and move the
   enums, the generic constructors and the variables that choose a backend into them. Migrate studio, the terminal,
   canvas and `draw_shared_memory.cpp`. Remove the flags that choose backends from the apps and examples.
2. **Format sets and the coding library.** Add `PrImageCodecSet`, `PrCompressCodecSet`, `PrTextureFormatSet` and
   `PrModelFormatSet`, and migrate their callers. Add the shared coding library, with one FSE.
3. **Format libraries.** Lift the compression formats and zip, then the image formats, into libraries of their own.
   `pr/image_png.h` moves to `lib/png`. The built-in backends become adapters, and ICO opens its PNG entries through the
   set. Move the texture sources to `lib/texture/backends/` and the model sources to `lib/model/backends/`. Lift
   meshopt into `lib/meshopt`, and add `PrGltfExtensionSet` with the Draco and meshopt adapters.
4. **Web backends.** Add the web backends of fetch and websocket.

[conventions.md](conventions.md) and the README change with steps 1 to 3. The section "Choosing a backend" moves to
selectors, the rule that an interface is replaced whole gives way to format sets, and the table of options lists the
new backends.

## Open questions

- **The name of the shared coding library.** `entropy_cpp` covers most of it, but not the LZ match copy.
