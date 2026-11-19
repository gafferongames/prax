# Scribe

Scribe (`apps/scribe`) is Studio's editor: the text editor of lib/studio in a window, with the panels and views of a
code editor that also shows what code works with, such as images, 3D assets and USD stages. It is one app. Each view is
a component of lib/studio, which any app can host: Scribe, `examples/model_viewer`, or a tool of its own. Scribe reads
assets and edits text. It does not edit scenes.

## Addresses

Everything that Scribe shows has a resource address: a file, an entry of an archive, an image that a glTF asset
embeds, a prim of a stage. An address names a resource, or a location inside one, however deeply it is nested. One
resolver serves the window, so every view under one archive shares the archive's open state.

Addresses are Scribe's own (`apps/scribe/address.h`), and so are the handlers of the formats that they step into. The
libraries know nothing of them: each handler is built on its format library's public API, and a Studio view that
links to a part of what it shows reports the part in its format's own terms, as the glTF structure view reports a JSON
Pointer.

- **Tabs.** A tab shows one resource, and its identity is the resource's address. Opening an address that a tab shows
  activates that tab. A tab of a nested resource is read only, and follows its parent: when the outermost file
  changes, every tab under it resolves again, and a step that no longer resolves shows as missing in the tab, which
  stays open.
- **Selection.** The selection is an address under the tab's resource that ends in a location, as
  `car.usdz#/World/Car`. Every view of the tab and the outline show it, and selecting in any of them sets it.
- **History.** Back and Forward move through the places that the window showed: each a tab's address with its
  selection, in the order that the user went to them, by a link, the project, a tab or a selection. The window has one
  history across its tabs, as a tab is its resource. A step shows its place's tab, opening it again as the leaf's
  preview if it closed, and selects what the place selected; it records nothing, and a new place after a step back
  drops the places ahead. A tab keeps its own scroll and caret while it is open. Back and Forward are in the Go menu
  and the menu bar, on the platform's chords, and on a mouse's back and forward buttons and a swipe between pages.
- **Links.** An address is text, which can be copied and opened again. Opening an address that ends in a location opens
  its resource's tab and selects the location.

### Grammar

An address is a URL whose fragment holds a chain of steps:

```
address = url [ "#" step *( "!" step ) ]
```

- The URL names the outermost resource, as `file:///work/car.usdz`, and has no fragment of its own. Every step after
  it applies to the resource that the steps before it reached.
- A step is percent-encoded (url/#percent-encode). It encodes `%`, `!` and `#`, and what the fragment percent-encode
  set holds (url/#fragment-percent-encode-set). A parser splits the fragment at each `!` and then decodes each step, so
  a step holds any byte, including a `!` of its own.
- An address without a fragment, or with an empty one, has no steps and names the outermost resource.

The fragment holds the chain, not the query. A query belongs to the server that the URL names, and each tool that
reads a file: URL handles its query differently. Only the reader of a URL interprets its fragment, and each step is
interpreted by its reader.

`!` separates steps. The jar: URL of Java is the precedent (`jar:file:/a.jar!/b/c.class`), and its nested form chains
the same way. `!` is a sub-delimiter that a fragment holds unencoded (RFC 3986 section 3.5), so an address survives the
URL serializer and a copy into any tool unchanged. No step grammar below uses it, and it is rare in file names. Each
alternative fails one of those tests: `/` is part of entry paths, JSON Pointers and scene paths, a URL does not allow
a second `#`, and some serializers encode `|` and `^`.

### Steps

The format of the resource that a step applies to interprets the step. The classifier (Routing) identifies that format
by the resource's bytes, which its name refines only for text. A step resolves either to a nested resource, whose bytes
are classified in turn and may take another step, or to a location inside the resource, which ends the chain.

| format | step | resolves to |
| --- | --- | --- |
| ZIP | an entry's name as the archive gives it, with `/` separators: `textures/wood.png` | the entry's bytes |
| USD package (.usdz) | an entry's name: `geom.usdc` | the entry's bytes |
| | a scene path, which begins with `/`: `/World/Car` | a location of the stage that the package's default layer composes |
| USD layer (.usda, .usdc) | a scene path (usd-core/#paths): a prim `/World/Car`, a property `/World/Car.points`, a variant selection `/World/Car{color=red}` | a location of the stage that the layer composes as its root layer |
| glTF (.gltf, .glb) | a JSON Pointer (RFC 6901) into the asset's JSON: `/images/0`, `/bufferViews/3`, `/buffers/0` | the bytes of the image, buffer view or buffer |
| | any other JSON Pointer that names a value: `/nodes/3`, `/meshes/0/primitives/1` | a location |
| image | `frame=N`, `level=N`, or both as `frame=N&level=N` | a location: a frame, or a mip level of a frame |

- glTF steps use JSON Pointer because KHR_animation_pointer addresses an asset's properties with it, so an animation's
  target and a link are written the same way. JSON Pointer is lib/json's (`prJsonValueFromPointer`).
- An image's step follows Media Fragments URI 1.0: named dimensions with `=`, joined by `&`. A frame is a frame of an
  animation or a layer or face of a texture, as lib/image counts them.
- A scene path in a package composes the package's default layer. A step that names a layer of the package and then
  a scene path composes that layer alone as the root layer: `car.usdz#geom.usdc!/Body` is a location of geom.usdc's
  own stage. lib/usd finds what a scene path names (`prUsdStageFindPath`).
- lib/usd's package syntax, `car.usdz[geom.usdc]`, stays lib/usd's input syntax for asset paths. The USD handler
  converts at the boundary: it reads what lib/usd asks for through the resolver, as a sibling (Relative reads).

Examples:

- An image of a binary glTF asset in an archive: `file:///assets/models.zip#Box/glTF-Binary/Box.glb!/images/0`.
- A property of a prim in a package: `file:///assets/car.usdz#/World/Car.points`.
- A deep chain: `file:///assets/outer.zip#inner.zip!scene.usdz!geom.usdc!/World/Car{color=red}`.
- A frame of an animated image in an archive: `file:///assets/icons.zip#spinner.webp!frame=3`.

### Resolution

A resolver resolves addresses. It is created with the handlers of the formats, a reader of outermost URLs, and limits.
The resolver depends on no format, and Scribe gives it a handler for each format above.

- The reader reads the outermost resource's bytes by its URL. Scribe's reads file: URLs.
- The resolver classifies each resource's bytes, and the first handler that opens the format opens them. The handler
  parses what it needs to take a step, such as a ZIP archive's central directory or a composed stage. A resource opens
  only when a step, its children or its handler's state is asked for, so the resolver never parses an image that a tab
  shows.
- A step on an opened resource gives a nested resource or a location. A handler gives a nested resource's bytes as a
  blob. A blob that shares the bytes of its parent keeps its parent open, as a stored ZIP entry or an image in a glb's
  binary chunk does. A blob of bytes that the step decoded is the nested resource's own.
- Resolution fails with the index of the step that failed and its status: NotFound for a step that names nothing,
  Syntax for a step that its format's grammar does not accept, NotSupported for a step on a resource that no handler
  opens, InvalidData for bytes that do not parse, Io if the outermost resource cannot be read, and Limit past a limit.

A handler also lists the nested resources that an opened container holds, each with the step that reaches it: the
files of an archive, the entries of a package, and the images that a glTF asset embeds. The project panel expands a
container by this list.

#### Relative reads

A resource may name others relative to itself: a glTF asset's external buffers and images, a USD layer's sublayers,
references and payloads. A handler reads them through the resolver, which resolves the reference against the
resource's own address. In a container whose steps are paths, as an archive's are, the reference is joined with the
directory of the resource's step: `models.zip#Car/scene.gltf` reads `textures/a.png` as
`models.zip#Car/textures/a.png`. At the outermost level, it resolves against the URL (url/#concept-basic-url-parser),
and the reader reads it. A reference that leaves the container's root, or a container whose steps are not paths, finds
nothing. A relative read takes from the budget of the chain that reads it.

#### Limits

- **Depth.** An address has at most a fixed number of steps, 16 by default.
- **Bytes.** Every byte that a chain's steps decode, as a deflated entry's, and every byte that its relative reads
  read, counts against one budget per chain, 1 GiB by default. A handler refuses a step whose declared size exceeds
  what remains before it decodes anything, and a decoder that writes past the declared size fails. So an archive that
  claims less than it holds does not exceed the budget either. Bytes that a step shares with its parent cost nothing.
- **Cycles** cannot occur: each step reaches bytes strictly inside its parent's, or a location, which ends the chain.
  A relative read reaches a sibling, not a step below the reader, and lib/usd ends its own composition cycles.

#### Caching

The resolver holds each live resource in a table by its address. Resolving reuses the longest prefix that the table
holds, so two tabs under one archive open it once. A resource holds a reference to its parent, so a container stays
open while anything under it is held: a tab, an expanded node of the project panel, or the blob of a nested resource.
Once nothing holds a resource, it closes and leaves the table.

#### Following changes

Everything under an address is read only, and follows the outermost resource. The resolver records a stamp of each
outermost resource that it reads, from a function that the host gives: a file's modification time and size, for files
on disk. A poll compares the stamps. Each resource under an outermost resource whose stamp changed becomes stale and
leaves the table, and the resolver's generation advances. A resource that read another relatively becomes stale with
it too, so a composed stage follows each of its layers. A holder that sees a new generation resolves its address
again. If a step no longer resolves, the holder shows what failed and where, and keeps the address. A tab of an entry
that was removed shows as missing, not closed, and shows the entry again if it comes back.

## Panels

The panels and the tabs are panels of Kit's dock, which the user resizes and rearranges; the tabs fill its primary leaf.
The panels show whether or not a project is open, and the View menu shows or hides each. File > Open Folder opens a
folder as the project, and File > Open File opens a file in the project that holds it, or else in the project of its
folder, as the command line does. A launch without a path opens the last project again.

Choosing a file in the project, by a click or the arrow keys, shows it in its leaf's preview tab, whose title is in
italics, and leaves focus in the tree; the next file chosen takes the preview over. Opening a file, by a double click or
Enter, keeps its tab and gives its view focus, and so do a double click on the preview's tab, an edit, and Keep Open.

A tab's tooltip says where its resource is: its place in the project, each step of a nested resource, and the whole
address. The context menus of a tab and of a row of the project run Scribe's commands on what they were opened on:
closing tabs, keeping a preview, copying a path or an address, revealing a file in the system's file manager or in the
project, opening a file beside the current tab, and collapsing a folder.

- **Project.** A tree of the files of a folder on disk. A click opens a file in the view that its format calls for
  (Routing). A container expands like a folder: a ZIP archive's entries, split into folders at each `/`, a USD
  package's files, and the images that a glb embeds. An expanded container stays open while it shows. An entry of a
  container opens in a tab of its own address.
- **Outline.** The navigation of the tab that shows, whichever view of it shows: a Markdown document's headings, a
  source file's symbols, a glTF asset's scenes and nodes, a stage's prims. Each item is a location, and choosing one
  sets the selection. The outline is generic: a view gives it items, and it knows no format.

## Views

A tab shows one of the views that its resource's format offers, and has buttons to switch between them:

- **Source.** The text editor, for text: code, Markdown, HTML, JSON, .gltf and .usda. Its language is the language of the
  media type that the classifier gives the resource from its bytes and its name (Routing), so a .gltf reads as JSON, a
  .mtlx as XML and a .php page as PHP, in a file or in an archive.
- **Preview.** A rendering of a text document, Markdown or HTML, by lib/html in Studio's page view
  (`pr/page_view.h`): Markdown as the HTML that lib/markdown makes of it, in lib/html's style sheet for Markdown, in the
  window's color scheme. What the document names, as an image, resolves through the document's address, so a document
  in an archive shows the archive's images, and a link that the user follows opens what it names in a tab.
- **Structure.** For a format with a lot to explain. A USD stage's structure view shows the composed prims, each prim's
  resolved properties, the opinions of the prim and of a property strongest first with the layer and the arc of each
  (their provenance), the stage's layers, and each prim's variant sets and selections. A glTF asset's structure view
  (`pr/gltf_view.h`) shows the selected item, an element of one of the asset's arrays or of an array that an extension
  defines: its properties, its extensions and whether the engine draws each, the items that it names and those that
  name it. A mesh shows each primitive's attributes, indices, mode, material and morph targets, and an accessor its
  elements, the byte ranges of the buffer views and buffers that hold them, and their values. With no item selected it
  shows the asset and its collections, and a collection lists its items. Each item and collection is a link: another
  selects it, and an image, a buffer view or a buffer opens in a tab of its own.
- **Image.** Every image that lib/image decodes, KTX2, WebP and HDR included through the format libraries' adapters,
  with its frames and mip levels, and an animation playing.
- **Preview of a glTF asset or a USD stage.** The asset drawn by lib/render through lib/model, in Studio's model view
  (`pr/model_view.h`): what the asset names relative to its address read through the resolver, an orbit camera and
  the asset's own cameras, its animations, and the controls of its tone map, its lighting and exposure, and its
  ground. The lighting starts from what the asset brings, and the command `model.lighting` sets it, as `--run` does
  for a capture. The node that the tab selects draws highlighted, with the nodes under it, and a click on the asset
  selects the node under the pointer, or nothing beside the asset. A stage's node is a prim at its path through
  instances, so the outline, the structure view, the source and the preview select one prim alike, and the history
  records it as it records a glTF node. The view renders only when what it shows changes.

### Routing

The classifier (lib/mime) identifies a file by its bytes, with its name refining text, and the format decides the views
and the one that opens first: text in Source, an image in Image, and a glTF asset or a USD layer or package in
Preview. A format that no view shows opens in a view that reports what the classifier found: the format, its media type
and the size.

lib/mime's classifier (`pr/mime_sniff.h`) identifies a resource's format from its first bytes and its name, as the
Unix `file` command does. It gives a format id, a media type, and the kind of the format. It only identifies, and each
format's own library inspects a header, as lib/image reports an image's extent.

- It reads at most the first 4 KiB, so a listing of a folder classifies each file by reading only that much.
- A signature decides. A format that shares another's signature is told apart by what its header holds: a USD package
  is a ZIP archive whose first entry is a layer, and an ISO base media file is HEIF, AVIF or MP4 by its brands. Text
  has signatures too: a USD layer's `#usda` header, and a shebang line that names an interpreter, as
  `#!/usr/bin/env python3` makes a script Python.
- The format of other text is a guess from its content. HTML and XML follow mimesniff's patterns
  (mimesniff/#identifying-a-resource-with-an-unknown-mime-type), JSON its first byte, and glTF an `asset` member.
  These patterns match the start of many formats: an HTML comment begins Markdown, and a doctype begins a PHP page.
- A name that implies a text format replaces the guess. The name's last part implies a format by a conventional file
  name, such as `Makefile`, `Dockerfile` or `CMakeLists.txt`, or else by its extension: `.md` makes text Markdown,
  `.gltf` makes it glTF, and `.tsx` makes it TypeScript with JSX. A name never overrides a signature, and does not
  make bytes that are not text into text.
- Each language of text that Scribe treats as its own is a format of the classifier, and a programming language's kind
  is code. So Scribe chooses how to read text by the format's media type alone, as its editor chooses a grammar. The
  media type is the one that IANA registers if there is one, such as `text/javascript` or `application/toml`, and
  otherwise the conventional unregistered type, such as `text/x-python`.

### Source and structure

The text of a .gltf asset or a .usda layer is linked to the tab's selection. Selecting an item, in the outline or a
structure view, selects its text in Source, and moving the caret into another item's text selects that item. A glTF
item is an element of the asset's top-level arrays, by its JSON Pointer, and a USD item is a prim or a property, by its
path on the stage. The links read the editor's text, as lib/json's spans (`PrJsonParseFlag_Spans`) and the text spans
of a layer's specs (`prUsdLayerSpecSpan`) give it, so an edit moves them with the text.

## Follow-ups

- Writing into containers.
- Text locations: RFC 5147's `line=` and `char=` steps for text resources.
- Single-stream compression (gzip, zstd) as a container of one resource.
- Keeping a container open for a while after its last holder releases it, for tabs that close and open again.
