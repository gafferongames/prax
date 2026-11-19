# Meshes

`lib/mesh` is geometry processing for triangle meshes. It depends on `lib/core`, and on `lib/geom` for the box and the
sphere that bound a mesh. It has no GPU and reads no file format. A loader or an application calls it to bring a mesh
into the form that a renderer wants: triangle lists, normals, tangents, shared vertices, an order that suits a GPU's
caches, bounds, and a hierarchy for ray queries.

## The shape of the interface

A mesh is not an object. Every function is a plain function over the caller's arrays:

- An attribute is a pointer, a count, a stride and a format (`PrMeshAttributeView`, and `PrMeshAttribute` for one that a
  function writes). It can be an array of its own or a field of interleaved vertices. A format is one to four
  components of a float or an integer type, and an integer may be normalized (`PrMeshFormat`). The geometric functions
  read and write 32-bit floats, and say how many of each value. A function leaves the other bytes of a stride alone.
  Values are read at any alignment.
- **Conversion is one function** (`prMeshAttributeConvert`): it reads an attribute in any format, doubles and 64-bit
  integers included, and writes it in another, normalized, rounded and clamped, with a fallback for what the source
  lacks. A producer that lays out vertices for a GPU calls it once for each attribute of the layout, at the attribute's
  offset in the vertex. It is where flexible source data meets a fixed layout, so no shader decodes a layout.
- Indices are 16 or 32 bits (`PrMeshIndexView`, `PrMeshIndices`). A view with a null pointer is the sequence 0, 1, 2
  and so on, so every function also takes vertices that are not indexed.
- A function that works on whole vertices, such as welding, takes them as an array of `vertexSize` bytes each. A mesh
  that keeps its attributes in separate arrays calls such a function once for each array.
- A result goes to storage that the caller provides. A function allocates only working memory and frees it before it
  returns.

Each function stands alone. No function registers anything or keeps state between calls, so a program links only the
functions that it calls, and an offline tool and a running application call the same code. Every function may run on
any thread.

New kinds of processing, such as meshlets, simplification and surface sampling, are further functions over the same
views.

## Input from files

Attributes and indices usually come from an asset file, so a function checks everything that could make it read out of
bounds, and returns a status instead:

- `InvalidData` for a stride smaller than the value, an index type that is not one, a count that is not a whole number
  of triangles, or attributes of one mesh with different counts.
- `OutOfRange` for an index that names no vertex, or that does not fit the type of the indices being written.
- `Limit` for a result that a 32-bit count cannot hold.

The size of the storage that receives a result is the caller's contract, which a function asserts.

A coordinate that is not finite never stops a function. Each function says what it does with one: bounds ignore the
point, normals and the hierarchy leave the triangle out, and tangents give the corners around it a unit vector.

## Determinism

The same input gives the same output on every platform and in every build. Three rules keep this true:

- The sources are compiled without floating-point contraction (`lib/mesh/CMakeLists.txt`), and use only the operations
  that IEEE 754 rounds exactly: arithmetic and the square root. Angles come from the module's own arccosine
  (`meshAcos`), and a shape's sines and cosines from its own series (`meshTurnSinCos`), because a C library's differ
  between platforms.
- No result depends on the order of a hash table's slots. Welding numbers vertices in the order of the input, and a
  table only finds equal ones.
- Sorting compares complete keys, so that an unstable sort has one result.

A test compares the bytes of each kind of result with hashes recorded once (`test_mesh_access.cpp`). A change that
alters a result on purpose updates the hashes.

## Functions

### Bounds (`pr/mesh.h`)

`prMeshBox` is the smallest axis-aligned box of the finite points, a `PrBox3`. `prMeshSphere` is a sphere that contains
them, a `PrSphere`. The sphere is not the smallest: it is the best of the sphere around the box's center, Ritter's
sphere, and a fixed number of refinement passes that step the center toward the farthest point. Its radius is at most
the square root of three times the smallest radius, and usually within a few percent of it. Both take a fixed number of
passes over the points.

### Triangle lists (`pr/mesh.h`)

`prMeshTriangleList` converts a strip or a fan to a list, with or without indices. Each triangle keeps the winding and
the first vertex that glTF and the graphics APIs give it. Triangle i of the list is triangle i of the strip, so
triangles that repeat a vertex are kept. A list converts to itself, which validates its indices and changes their type.

### Polygons (`pr/mesh_polygons.h`)

`prMeshPolygonsTriangulate` cuts faces of any number of corners, as OpenUSD and OBJ list them, into triangles that name
corners, so that what a file gives each corner of a face follows its triangles. A convex polygon is a fan from its first
corner. Any other is cut by ear clipping in the plane of its Newell normal, which keeps each triangle inside it and the
polygon's winding. A polygon that crosses itself or has no area has no ears to clip, and is a fan.

### Shapes (`pr/mesh_shapes.h`)

`prMeshShapeWrite` writes the polygons of a sphere, a box, a cylinder whose ends may differ in radius (a cone's top is
a point), a capsule or a plane, about an axis, with the normal of the surface at each corner. A round shape's points lie
on its surface. Its faces are quads between rings about the axis, and triangles at a point on the axis, whose normal is
the surface's in the middle of the face. An edge where the surface folds, as a cylinder's rim, has a normal on each
side, so it shades sharp, and every other edge shades smooth. Faces wind counterclockwise seen from outside.

### Vertices (`pr/mesh.h`)

A remap is an array with a new index for each vertex, or `kPrMeshIndexNone` for a vertex to leave out.
`prMeshVerticesRemap` and `prMeshIndicesRemap` apply one.

- `prMeshVerticesUnindex` writes one vertex for each index, so that no two triangles share a vertex.
- `prMeshWeldRemap` welds vertices whose bytes are all equal. It is exact: it has no tolerance, and it compares padding
  and the sign of zero. The functions of this module that write normals and tangents never write negative zero, so
  their results weld.
- `prMeshFetchRemap` numbers the vertices in the order that the indices first use them and leaves out the vertices
  that no index uses. It serves two purposes: removing unused vertices, and storing vertices in the order in which
  they are drawn.

### Normals (`pr/mesh_normals.h`)

`prMeshNormals` gives each vertex the sum of the normals of its triangles, weighted by each triangle's angle at the
vertex. The weights make the result independent of how a polygon was cut into triangles. Triangles without area
contribute nothing. `prMeshFlatNormals` gives each index its triangle's normal, for vertices unshared with
`prMeshVerticesUnindex`. A vertex without a normal gets (0, 0, 1) from either function, so every normal has unit
length.

### Tangents (`pr/mesh_tangents.h`)

`prMeshTangents` computes the tangent space that glTF asks for if a mesh has a normal map and no tangents: MikkTSpace's
(glTF-2.0/#meshes-overview). Tools that bake normal maps use the same algorithm, and a normal map shades correctly only
in the tangent space that it was baked in.

The result is one tangent for each index, not for each vertex, because the algorithm decides which corners share a
tangent. Corners at one position share a tangent only if their normals and texture coordinates are equal, their
triangles are joined around the vertex by shared edges, and the triangles map the texture with one orientation. So a
seam where the texture is mirrored splits its vertices. To get an indexed mesh, unshare the vertices, add the tangents,
and weld:

```
prMeshTangents(positions, normals, texcoords, indices, tangents)     one tangent for each index
prMeshVerticesUnindex(vertices, ..., indices, corners)               one vertex for each index
copy tangent k into corner k
prMeshWeldRemap(corners, ..., remap, &count)                         share what is still equal
prMeshVerticesRemap(corners, ..., remap, welded, count)
prMeshIndicesRemap(unindexed, remap, ..., newIndices)
```

The handedness is 1 if cross(normal, tangent) points where the second texture coordinate increases. glTF stores the
opposite, because its texture coordinates run down the image and its normal maps point up it. A glTF loader negates the
handedness.

The header lists how the result relates to the reference implementation of MikkTSpace. In short: the steps and the
arithmetic are the same, so every corner at which the reference computes a tangent gets the reference's tangent and
handedness to the last bit. Two differences remain. A corner that the reference leaves without a tangent, zero or its
default of (1, 0, 0), gets a unit tangent perpendicular to the normal with a handedness of 1, so a renderer can
normalize and orthogonalize without a special case. And the reference leaves the last run of its edge sort unsorted,
which leaves a few triangles near the end of a mesh unpaired; this module pairs them.

The `tangents` suite (`tools/suite/tangents.py`) holds the module to that over Khronos's glTF sample assets, with
the reference and meshoptimizer's generator fetched for the development build alone, and `tangents_reference_tests`
over the shapes of the module's own tangent tests. meshoptimizer's `meshopt_generateTangents` follows the same
algorithm with a different grouping, an approximate arccosine and, by default, weights of its own. At the pinned
release it runs several times faster than this module, and its tangents are close to MikkTSpace's but not equal: within
a few ten-thousandths of a radian at most corners with `meshopt_TangentCompatible`, and further apart around
degenerate texture mappings. The suite's scoreboard has the numbers.

### Cache order (`pr/mesh_cache.h`)

`prMeshIndicesOptimizeCache` reorders the triangles of a list so that a GPU reuses recently shaded vertices. The order
is Forsyth's: greedy, by a score for each vertex that favors recent use and few remaining triangles. It suits caches
of any size. The search reads a bounded number of triangles for each step, so the time is linear even if thousands of
triangles share one vertex. Follow it with `prMeshFetchRemap`, so that the vertices are stored in the order in which
the triangles use them. Every array that holds a value for each vertex takes the same remap, as `lib/model` gives it to
a part's vertices, skin and morph deltas ([model.md](model.md#preparing-meshes)).

`prMeshCacheMissRatio` and `prMeshFetchMissRatio` measure the two orders against simple models of the caches: the
vertices shaded for each triangle, and the cache lines of vertex data loaded for each line used. They exist so that a
tool or a test can state an improvement as a number.

### Ray queries (`pr/mesh_bvh.h`)

`prMeshBvhBuild` builds a bounding volume hierarchy over a mesh's triangles into two arrays of the caller's: nodes of
32 bytes, and the triangle order that the leaves index. Nodes refer to children and triangles by index, so the arrays
can be copied, written to a file, or uploaded for a shader to traverse. The build splits by a binned surface area
heuristic, and no path is longer than `kPrMeshBvhMaxDepth`, so a traversal needs a stack of that size and no more.

`prMeshBvhIntersect` returns the nearest hit of a ray: the triangle, the distance and the barycentric weights. It
allocates nothing. `prMeshBvhIsValid` checks the structure of a hierarchy that was read from a file. A query assumes a
valid hierarchy, and reads nothing outside the arrays for one.

A mesh that deforms keeps one tree. `prMeshBvhRefit` recomputes its boxes for the vertices' new positions in time
proportional to its nodes, where a build sorts the triangles again. The tree's shape stays the one that the build chose,
so a refitted tree answers every query correctly but slows as its boxes overlap. `prMeshBvhCost` measures a tree by the
surface area heuristic, and its owner builds again when a refit's cost outgrows the build's by the factor it chooses.
