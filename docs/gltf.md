# glTF

`lib/gltf` reads a glTF 2.0 asset into a document: the asset as its specification describes it, with every default
filled in and every rule of the specification that a reader relies on already checked. It depends on `lib/core`, on
`lib/geom` for matrices, on `lib/json` for the asset's JSON, and on `lib/draco` for the Draco streams of an asset's
meshes. Other libraries reach it through `pr/gltf.h`.

## Responsibilities

The module owns what the specification defines, so that no consumer restates it:

- The containers: glTF JSON, and binary glTF with its JSON and binary chunks.
- The objects of an asset, as records in arrays. An object names another by its index in the array of its kind, which
  is the index that the asset's JSON uses.
- The layout of binary data. `prGltfAccessorReadFloats` and `prGltfAccessorReadIntegers` unpack an accessor's
  elements, and apply stride, the padding of matrix columns, the conversion of normalized integers and sparse
  substitution.
- The compression of binary data that an extension defines as a layer under buffer views: meshopt compression, whose
  streams the module decodes when it reads the asset, so that a compressed view has bytes like any other.
- The compression of a primitive that an extension defines in place of its accessors' data: Draco compression, whose
  stream `lib/draco` decodes when the module reads the asset, so that the accessors that the stream fills read like any
  other.
- The arithmetic that the specification gives: a node's matrix from its translation, rotation and scale, the world
  matrix of a node under its parent, the triangle list of a primitive of any mode, the transform of a texture
  coordinate, and the value of an animation channel at a time under each interpolation.

It leaves to others everything that the specification does not define:

- It reads no files and no network. The host gives `prGltfCreate` the asset's bytes as a blob, and a callback that
  loads the buffers and images that the asset names by URI. A host that cannot load during the call reads the asset
  once without the callback, to learn the URIs, and again when it has their bytes. Each read parses the JSON and runs
  the checks, and neither copies the bytes of a blob: the document keeps a reference to it instead.
- It decodes no images. An image is its encoded bytes and their media type, for `lib/image`.
- It processes no geometry. Bounds, normals and tangents that an asset does not carry are `lib/mesh`'s to compute.
- It draws nothing, and it holds no state of a scene. A document is immutable, so an animation's values are returned
  to the caller, which decides what they move.

## What a document guarantees

All of an asset is data from outside the program. `prGltfCreate` checks it once, at the boundary, and returns a
document only if every check passes. A consumer indexes arrays with the indices that a document holds, and reads
accessors, without checking either again. The comment at the top of `pr/gltf.h` lists the guarantees. In outline:

- Every index names an object, and every enumeration and number is one that the specification's schema allows.
- Every accessor lies within its buffer view, and every buffer view within its buffer.
- The accessors that a mesh, a skin or an animation uses have the types and counts that the specification gives their
  use.
- The nodes form trees.
- An index of a primitive names one of its vertices, and a joint of a vertex names a joint of each skin that the
  vertex is drawn with.
- The times of an animation's keyframes increase.

The checks of the last two lines, and the check of sparse indices, read the bytes of buffers. A buffer whose URI the
host did not load has no bytes, so those checks do not run for it, and every function that would read it reports
that instead of reading.

An asset that fails a check is refused whole, with a status and a sentence that names the property and the rule. The
module does not repair an asset, because a repair would hide from a consumer which of the asset's statements it could
no longer trust. The exception is an empty array for an optional property, which reads as an absent one.

The module checks structure, not meaning. It does not check that a normal has unit length, that an accessor's stated
bounds are those of its data, that an image's bytes are an image, or that a node's matrix decomposes. A consumer
that needs one of these computes it.

## Extensions

`prGltfExtensionIsSupported` names the extensions that the module reads. Each adds to the records that the
specification defines:

| extension | adds |
| --- | --- |
| `EXT_mesh_gpu_instancing` | `PrGltfNode::instancing`, and `prGltfInstanceMatrices` |
| `EXT_meshopt_compression`, `KHR_meshopt_compression` | `PrGltfBufferView::meshopt`, `PrGltfBuffer::fallback`, and the decoded bytes of the view |
| `KHR_draco_mesh_compression` | `PrGltfPrimitive::draco`, `PrGltfAccessor::draco`, and the decoded elements of the accessors |
| `KHR_animation_pointer` | the paths of `PrGltfAnimationChannel` past the core's, with the object that holds each |
| `KHR_lights_punctual` | the document's lights, and `PrGltfNode::light` |
| `KHR_materials_anisotropy` | `PrGltfMaterial::anisotropy` |
| `KHR_materials_clearcoat` | `PrGltfMaterial::clearcoat` |
| `KHR_materials_diffuse_transmission` | `PrGltfMaterial::diffuseTransmission` |
| `KHR_materials_dispersion` | `PrGltfMaterial::dispersion` |
| `KHR_materials_emissive_strength` | `PrGltfMaterial::emissiveStrength` |
| `KHR_materials_ior` | `PrGltfMaterial::ior` |
| `KHR_materials_iridescence` | `PrGltfMaterial::iridescence` |
| `KHR_materials_pbrSpecularGlossiness` | `PrGltfMaterial::specularGlossiness`, an archived extension |
| `KHR_materials_sheen` | `PrGltfMaterial::sheen` |
| `KHR_materials_specular` | `PrGltfMaterial::specular` |
| `KHR_materials_transmission` | `PrGltfMaterial::transmission` |
| `KHR_materials_unlit` | the material's `PrGltfMaterialExtensionFlag_Unlit` |
| `KHR_materials_variants` | the document's variants, the mappings of each primitive, and `prGltfPrimitiveMaterial` |
| `KHR_materials_volume` | `PrGltfMaterial::volume` |
| `KHR_mesh_quantization` | the types of attributes that it adds, whose bounds `prGltfAccessorBounds` converts |
| `KHR_node_visibility` | `PrGltfNode::visible` |
| `KHR_texture_transform` | `PrGltfTextureInfo::transform`, and the set of texture coordinates that it replaces |
| `EXT_texture_webp`, `KHR_texture_basisu` | an image of `PrGltfTexture::sources` in WebP, or in KTX 2 with Basis Universal |

A material records which of the material model's extensions it has (`PrGltfMaterial::extensions`), and one that it
does not have holds that extension's defaults, which leave the core material as it is. So a consumer that shades an
extension reads its fields without checking for it, and one that does not can tell what it leaves out.

A texture's images are the same picture in several formats, and a consumer draws the first that it decodes. The module
decodes none of them, as it decodes no image.

A primitive's Draco stream holds its indices and those of its attributes that the extension names, in an order of
vertices that the stream's decoder makes (docs/draco.md). The module decodes the stream when it reads the asset and
keeps, for each accessor that the stream fills, the elements in the accessor's own layout, so a consumer reads a
compressed primitive as it reads any other and never sees the stream. The accessor's `bufferView`, which an asset may
give for a reader without the extension, is not read, and the buffer behind it is loaded only because the asset does
not say which buffers hold nothing else. Morph targets, the attributes that the extension leaves out, and skins stay in
their own accessors, and sparse elements replace decoded ones as they replace any others.

The extension says that the accessors "must match the decompressed data" and little more, so the module decides what
matching is:

- The stream is a triangle mesh with as many points as the attributes have elements and three indices for each face of
  the indices' `count`. `lib/draco` is given those counts as its limits, so a stream cannot make the module allocate
  for more than the asset's own JSON states.
- An attribute of the stream has the number of components of its accessor's `type` and exactly its `componentType`.
  The module converts nothing, because every conversion would be a guess at what the exporter meant. An accessor of
  normalized or quantized integers (`KHR_mesh_quantization`) is filled by a stream's attribute of the same integers.
- The stream's own statement that an attribute is normalized is not compared with the accessor's. The extension does
  not mention it, and exporters that fill a stream through Draco's bindings leave it unset.
- Indices are written in the accessor's `componentType`, and an index that it cannot hold is refused.
- The attributes that the extension names are attributes of the primitive ("must be a subset"), and each id is the id
  of an attribute of the stream.
- The primitive has indices. A stream decodes to indexed triangles, which a primitive that draws its vertices in order
  does not state.
- The primitive's `mode` is triangles. The extension also allows a triangle strip, "decoded accordingly", and does not
  say how the triangles that a stream decodes to become the indices of a strip, which the indices' `count` would have
  to match. The implementations that it names do not agree. Draco's bindings can make strips from a decoded mesh
  (`GetTriangleStripsFromMesh`), joined by degenerate triangles, in an order and a number that are the stripifier's
  own. Khronos's sample renderer does not call it: it takes three indices for each face
  (`GetTrianglesUInt32Array`) whatever the mode, and would then draw that list as a strip. Draco's own glTF reader
  refuses a primitive that is neither triangles nor points. No sample asset has such a primitive. The module refuses a
  strip as NotSupported rather than choose.
- The stream's buffer view has no `byteStride`, as an image's has none.
- Two primitives may name one stream, or streams that give an accessor the same elements. A stream that would give an
  accessor other elements than an earlier stream gave it is refused.

An animation's pointer is resolved when the asset is read, to a path, the object that holds the property, and the
element of an array or the texture of a transform. The module resolves every pointer of the Asset Object Model to a
property of the core or of an extension that it reads, and refuses one that names a property that the asset does not
define: one without a default that the asset does not state, or one of an extension or a texture that the object does
not have. A pointer into `extras`, or into an extension that the module does not read, is kept unresolved, with the
path `PrGltfPath_Pointer`, since the module cannot tell what it names. A pointer to a node's transform or weights reads
as the core's target, so a consumer animates the two forms alike, and one animation may not animate a property twice
in either form.

An asset lists the extensions that it must be read with in `extensionsRequired`. The module refuses an asset that
requires any other extension, with NotSupported and the extension's name, because the asset says that it cannot be
understood without it. An extension that an asset only uses is ignored: the document holds nothing of it, and
`prGltfUsedExtension` lists the names, so that a consumer can see what it was not shown. An animation channel whose
target only an extension that the module does not read defines is kept, with no node and no path, so that the indices
of the other channels stay those of the asset.

An asset of the `gltf` suite that breaks a rule of an extension is refused like any other, and its case is in the
suite's errata with the rule.

`extras` are not read.

## Testing

The unit tests read small assets written in the tests' own text, one feature and one refusal at a time. The `gltf`
suite reads Khronos's sample assets, every model in every variant, and then reads everything that each document
offers (docs/testing.md). It compares the bounds of each accessor as the module unpacks it with the bounds that the
asset states, which the specification requires to be exact. That checks stride, padding and sparse substitution
against the corpus's own data, except for data that a meshopt filter or a Draco stream decodes, whose stated bounds
are those of the data before it was compressed, which the decoded data only approaches. A variant that requires an
extension that the module does not read is counted as skipped, once the module has refused it.

The decoders of meshopt compression are compared with meshoptimizer's, which is fetched for the development build
alone (`tools/suite/meshopt`): `meshopt_reference_tests` decodes, with both, the streams that meshoptimizer's encoders
write of attributes in both versions and at every level, over the strides and counts that change how blocks and groups
fall, of the indices of triangles and of sequences, and of each filter. The codecs agree byte for byte, and the filters
to the unit in the last place that the extension allows, or exactly for the exponential filter, which it requires to be
exact.

`lib/gltf` reads `KHR_texture_basisu` into the document and decodes no image. `lib/ktx` reads the KTX 2.0 files that the
extension names, and `lib/basis`'s decoders of ETC1S and UASTC are compared with Basis Universal's transcoder, which is
fetched for the development build alone (`tools/suite/basis`): `basis_reference_tests` decodes, with both, the textures
of the sample assets and textures that the reference encoder writes across its options, and the pixels must be equal.
The decoders of blocks that a KTX 2.0 file may hold in place of pixels, of BC and of ETC2, are compared there with the
reference's unpackers, over blocks of any bits, and decode the same texels. The blocks that ETC1S transcodes to are
compared too: those of ETC2 RGB8 and RGBA8, of EAC R11 and RG11, of BC1 and of BC3's color must be the reference's, byte
for byte, as the blocks of BC7 and of ASTC 4x4 that UASTC transcodes to must be. Its blocks of BC4, which BC3's alpha
and BC5 are made of, come from tables of its own, and the error that they leave against the pixels must be within half a
decibel of the error of the reference's.
