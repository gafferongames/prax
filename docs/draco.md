# Draco

`lib/draco` decodes a stream of the Draco bitstream into a geometry: points, the values of their attributes, and, for a
mesh, faces of three points. It depends on `lib/core` alone. Other libraries reach it through `pr/draco.h`.

The decoder is written from the Draco bitstream specification. It holds no code of the reference implementation, which
is fetched for the development build alone, to compare the two.

## Responsibilities

The module owns the bitstream: the header and the metadata of a stream, the connectivity of a mesh, the entropy coding,
the prediction of attributes, and the transforms that give floats back from integers. It decodes a stream whole and
returns the geometry as arrays.

It leaves to its callers what a stream's container defines. A glTF primitive names the attributes of its stream by
their ids and says which is the position and which the second set of texture coordinates (`lib/gltf`), as a USD mesh or
a tile of 3D Tiles does in its own way. The bitstream gives an attribute a kind and a type and no meaning past them, so
the module returns the values as the stream types them, and converts nothing.

This is why the decoder is a library and not a part of `lib/gltf`, where the decoder of meshopt compression is. Meshopt
compression is defined by a glTF extension, as a layer under glTF's buffer views. A Draco stream is a format of its own,
which names its own attributes and counts its own points, and which other containers than glTF carry.

## The order of points

A stream does not store the order of a mesh's points. A decoder makes it, by walking the mesh as the encoder did, and a
container that stores anything beside the stream by point, as glTF stores morph targets, relies on every decoder making
the same order. So the module's points, faces and values are exactly the reference decoder's, including where the
specification's description of a step differs from what the reference decoder does.

## What is read

Of the bitstream that the specification describes, version 2.2:

- The header, and the metadata of a stream and of its attributes, which is checked and not kept.
- Sequential connectivity, with indices stored whole in each of their four sizes or coded as differences.
- Edgebreaker connectivity with the standard and the valence traversal, topology splits, start faces, and the seams of
  attributes, from which the points are made.
- The sequences of attribute values that the depth-first and the prediction-degree traversal give, over the mesh whole
  and over the mesh cut along an attribute's seams.
- Attributes stored whole, as integers, as quantized floats and as normals in octahedral coordinates.
- The prediction schemes: difference, parallelogram, constrained multi-parallelogram, the portable scheme for texture
  coordinates, and the geometric scheme for normals, with the wrap transform and the canonicalized octahedral transform.
- Entropy coding: rANS for bits and for symbols, and symbols coded tagged and raw.

The reference decoder reads more than the specification describes in a stream of version 2.2, and the module follows
it where that costs little:

- The predictive traversal of Edgebreaker, which the specification names and does not describe.
- The multi-parallelogram scheme, and the octahedral transform that is not canonicalized.
- A point cloud whose points are coded in order, in version 2.2 or 2.3.

It does not read, and reports as NotSupported:

- A stream of another version. The reference decoder reads back to version 1.0 through paths of its own for each
  version, which the reference encoder has not written since its release 1.3.
- A point cloud coded as a k-d tree, which no mesh uses. The direct and the adaptive decoder of bits serve only that
  coding.
- The first prediction scheme for texture coordinates, which the bitstream withdrew, and which computes in floating
  point.

## Data from outside the program

A stream is checked as it is read. Every read is checked against the end of the bytes that it reads from, every count
against what was allocated for it, and every index against the array that it indexes, so a stream that is not one that
the bitstream defines fails with InvalidData.

Two things bound what a stream can cost, since neither follows from its size:

- A valid stream of a few bytes can describe any number of points, because a run of equal symbols costs nothing to
  code. The caller states the most points and faces that it accepts (`PrDracoLimits`), and a stream that describes more
  fails with Limit before anything is allocated for it. `lib/gltf` passes the counts of the accessors that the stream
  fills.
- Symbols that describe no mesh can still build a corner table. The module checks the table once it is built: each
  corner is at a vertex that has corners, and the two faces across an edge agree on the edge's vertices. Everything
  that later walks around a vertex then visits each corner once, so decoding takes time in proportion to the mesh.
  While the table is built, the corners that merging vertices may visit are counted against a budget of 64 visits for
  each corner.

The reference decoder makes neither check, so a stream that fails one of them here is a stream that it would decode to
something. No stream of the reference encoder does.

## Where the specification is unclear

The specification is pseudocode of the reference decoder as it once was, and the two differ. Each difference below is
decided as the reference decoder has it, except where a stream that no encoder writes is refused instead.

| specification | what the module does |
| --- | --- |
| Conventions: the bits of `f[n]` are "read from high to low order" | A sequence of bits begins at the least significant bit of its first byte, and the first bit of a number is its least significant. |
| Metadata Decoder, `ParseMetadataElement`: the size of a value is a byte | The size is a variable-length integer, and a value of no bytes is refused. |
| Sequential Connectivity Decoder: no checks | A stream has three bytes at least for each face, a difference does not take an index below zero, and an index names a point. |
| EdgeBreaker Decoder, `NewActiveCornerReached` and `ProcessInteriorEdges`: the corners of a new face are found by walking opposite corners, and merged vertices are replaced in every face | The corners are found from each vertex's left-most corner, and only the fan of the merged vertex is renamed. In a mesh with no seam tables, the last vertex is then moved into the place of each vertex that a merge emptied, which the specification does not have and which changes the order of the points. |
| EdgeBreaker Decoder: `edgebreaker_traversal_type` 1 is named and not described | The predictive traversal: after a C or an R, a bit coded with rANS says whether the next symbol is the one that the valence of the vertex ahead predicts. |
| EdgeBreaker Traversal, `EdgeBreakerAttributeTraverser_ProcessCorner`: the listing has lost a line and does not parse | The same depth-first traversal as the one before it, over the mesh cut along the attribute's seams. |
| Rans Decoding, `DecodeSymbols`: the scheme is read for any number of symbols | Nothing is read for no symbols. |
| Rans Decoding, `RansInitDecoder`: four sizes of the first state | The decoder of bits takes three, and refuses the fourth. Both decoders refuse a first state past their range. |
| Rans Decoding, `RabsDescRead`: a byte is read whenever the state is low; the probability of a one is 256 less that of a zero | No byte is read past the first, and the probability of a one is kept in eight bits, so a probability of zero of 0 gives a probability of one of 0. |
| Rans Decoding, `BuildSymbolTables`: no checks | The probabilities sum to the precision, and a run of symbols of no probability ends within the alphabet. |
| Prediction Decoder, `ParsePredictionData`: `seq_int_att_dec_compressed` is read only with a prediction scheme, and only then are integers decoded | It is read for every attribute that is not stored whole, and the integers are decoded with or without a scheme. If it is zero, a byte gives the size of an integer and the integers follow, which the specification does not have. |
| Prediction Decoder: schemes 0, 1, 4, 5 and 6, transforms 1 and 3 | Scheme 2 and transform 2 are read too, scheme 3 is not supported, and any other number is refused. A scheme that reads the mesh is the difference in a stream with no corner table, and so is any scheme but 6 for a normal. Scheme 6 for anything but a normal, and a transform that the attribute's coding does not take, are refused. |
| TexCoords Prediction Decoder and Normal Prediction Decoder: positions are `seq_int_att_dec_original_values[0][0]` | The positions are the integers of the first attribute whose kind is the position, which is decoded before the attribute that it predicts, and has three components. |
| TexCoords Prediction Decoder: no checks | The products that the reference decoder checks for overflow fail the stream, and the others wrap. |
| Sequential Quantization Attribute Decoder, `DequantizeFloat`: the integer times the reciprocal of the greatest integer, times the range | The integer times a step, which is the range over the greatest integer, and then the least value, each rounded on its own. |
| Sequential Normal Attribute Decoder, `OctaherdalCoordsToUnitVector` | The reference decoder's later arithmetic, which folds the octahedron by the sign of each coordinate, in single precision with each operation rounded on its own. |

## Testing

The unit tests decode streams that they write byte by byte, for the parts of the bitstream that are bytes, and streams
of the reference encoder that they hold as arrays, with a hash of what the reference decoder gives for each. They
refuse a stream for each reason that the header lists.

The decoder is compared with the reference decoder, which is fetched for the development build alone
(`tools/suite/draco`). `draco_reference_tests` encodes meshes with the reference encoder and decodes each stream with
both decoders, which must give the same faces, the same points in the same order, and the same value of every attribute
to the bit. The meshes are sheets with and without holes, a torus, a Klein bottle, a cube, fans, several components in
one mesh, triangles between vertices chosen at random, and shapes that are not surfaces: three faces at an edge, fans
that touch at a vertex, faces with no area and faces that repeat. Each is encoded with each set of attributes, with
seams in its texture coordinates and its normals, sequentially and by Edgebreaker with each of its three traversals, at
every speed, with each prediction scheme forced on each kind of attribute, and with quantization from 1 bit to 30. The
reference is built without fused multiplies and adds, as the module is, so that the two round alike.

The `draco` suite compares the decoders over the streams of Khronos's glTF sample assets, with the same rule
(docs/testing.md). `./dev.sh suite draco --time` also measures what each decoder takes for each asset's streams.

Two parts of the decoder are not reached by any stream of the reference encoder, and are written from the reference
decoder alone: the octahedral transform that is not canonicalized, and sequential indices of 32 bits, which a mesh of
two million points takes.

The same tests cut each of a set of streams short at every length and change each of its bytes in turn, and decode the
result, which must fail or succeed with every face naming a point. Under the address sanitizer, that is a check that
nothing outside the stream is read.
