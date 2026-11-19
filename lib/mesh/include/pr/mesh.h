// Meshes: geometry processing for triangle meshes held in the caller's arrays.
//
// A mesh is not an object here. Every function reads vertex attributes and indices where the caller keeps them, in any
// vertex layout, and writes its result to storage that the caller provides. A function allocates only working memory,
// and frees it before it returns. No function keeps state between calls, so every function is safe to call from any
// thread on data that no other thread is writing.
//
// Attributes and indices come from asset files. A function returns InvalidData for a stride, a count or an index type
// that it cannot read, and OutOfRange for an index that names no vertex. It reads nothing past the elements that a view
// describes. The size of the storage that receives a result is the caller's contract.
//
// Every function is deterministic: the same input gives the same output on every platform.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/status.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Attributes and indices

// The type of each component of an attribute's values, in the machine's byte order.
enum {
    PrMeshComponent_Float32,
    PrMeshComponent_Float64,
    PrMeshComponent_Float16,
    PrMeshComponent_Int8,
    PrMeshComponent_Uint8,
    PrMeshComponent_Int16,
    PrMeshComponent_Uint16,
    PrMeshComponent_Int32,
    PrMeshComponent_Uint32,
    PrMeshComponent_Int64,
    PrMeshComponent_Uint64,
    PrMeshComponent_Count,
};
typedef uint8_t PrMeshComponent;

// How each value of an attribute is stored: `count` components of `component`, one to four of them, one after another.
// An integer component that is `normalized` stands for a number in [0, 1], or in [-1, 1] if it is signed: its value
// over the largest value of its type, and no less than -1. A float is never normalized.
typedef struct PrMeshFormat {
    PrMeshComponent component;
    uint8_t count;
    bool normalized;
} PrMeshFormat;

// Returns the format of `count` 32-bit floats.
PrMeshFormat prMeshFormatFloat32(uint32_t count);

// Returns true if `format` is one that values are stored in: a component of the enumeration, one to four of them, and
// normalized only if they are integers.
bool prMeshFormatIsValid(PrMeshFormat format);

// Returns the size in bytes of a value of a valid `format`.
uint32_t prMeshFormatSize(PrMeshFormat format);

// One attribute of `count` vertices, read in place. The value of vertex i begins `stride` bytes after the value of
// vertex i - 1, so an attribute can be an array of its own or a field of interleaved vertices, at any alignment. A
// function says what format it reads: the geometric functions read floats, three for a position or a normal and two for
// a texture coordinate, and take a format of at least that many 32-bit floats. The stride must be at least the size of
// a value.
typedef struct PrMeshAttributeView {
    const void* PR_NULLABLE data;
    uint32_t count;
    uint32_t stride;            // in bytes
    PrMeshFormat format;
} PrMeshAttributeView;

// One attribute of `count` vertices that a function writes, laid out as a PrMeshAttributeView is. A function writes
// only the bytes of its values and leaves the other bytes of each stride alone.
typedef struct PrMeshAttribute {
    void* PR_NULLABLE data;
    uint32_t count;
    uint32_t stride;            // in bytes
    PrMeshFormat format;
} PrMeshAttribute;

// Writes each of the `out.count` values of `out` from the value of the same vertex of `source`, converted to the format
// of `out`. Interleaved vertices are written by a call for each of their attributes, with their stride and each
// attribute's offset.
//
// A component is read as a number: a float as it is, an integer as its value, and a normalized integer as the number
// that it stands for. Integers of 64 bits are exact up to 2^53 in magnitude. The number is then written: as the nearest
// float, as the nearest integer of the type clamped to its range, or, normalized, clamped to [0, 1] or [-1, 1] and
// times the type's largest value, rounded to the nearest. A component that the source lacks, of a vertex past
// `source.count`, of every vertex of a source without data, or whose number is not finite, takes the component of
// `fallback`, which is four numbers, or (0, 0, 0, 1) if it is null.
//
// Returns InvalidData for a format that is not valid, or for a stride smaller than a value of its format. `out` must
// not overlap `source`.
PrStatus prMeshAttributeConvert(PrMeshAttributeView source, const float* PR_NULLABLE fallback, PrMeshAttribute out);

// The integer type of an index.
enum {
    PrMeshIndexType_U16,
    PrMeshIndexType_U32,
};
typedef uint8_t PrMeshIndexType;

// The index that stands for no vertex in a remap.
PR_CONSTANT uint32_t kPrMeshIndexNone = 0xffffffffu;

// `count` indices, read in place, at any alignment. A null `data` is the sequence 0, 1, 2 and so on up to `count` - 1,
// which describes vertices that are not indexed. A function that takes the indices of a triangle list reads three
// indices for each triangle, and returns InvalidData if `count` is not a multiple of three.
typedef struct PrMeshIndexView {
    const void* PR_NULLABLE data;
    uint32_t count;
    PrMeshIndexType type;
} PrMeshIndexView;

// `count` indices that a function writes. A function returns OutOfRange for an index that does not fit in `type`.
typedef struct PrMeshIndices {
    void* PR_NULLABLE data;
    uint32_t count;
    PrMeshIndexType type;
} PrMeshIndices;

// ---------------------------------------------------------------------------------------------------------------------
// Bounds

// Writes to `out` the smallest box that contains the points of `positions`, three floats each. A point with a
// coordinate that is not finite is ignored. If no point remains, the box is all zeros. Takes one pass over the points.
PrStatus prMeshBox(PrMeshAttributeView positions, PrBox3* out);

// Writes to `out` a sphere that contains the points of `positions`, three floats each. A point with a coordinate that
// is not finite is ignored. If no point remains, the sphere is all zeros.
//
// The sphere is not the smallest one. It starts from Ritter's sphere and from the sphere around the center of the
// points' box, and keeps the smallest sphere that a fixed number of refinement passes finds. Its radius is therefore
// never greater than either of those, and never greater than the square root of three times the smallest radius. For
// most point sets it is within a few percent of the smallest. Every point is inside the sphere or within a rounding
// error of the radius outside it. Takes a fixed number of passes over the points.
PrStatus prMeshSphere(PrMeshAttributeView positions, PrSphere* out);

// ---------------------------------------------------------------------------------------------------------------------
// Triangle lists

// How a sequence of indices forms triangles.
enum {
    PrMeshTopology_TriangleList,    // triangle i is indices 3i, 3i + 1, 3i + 2
    PrMeshTopology_TriangleStrip,   // triangle i is indices i, i + 1, i + 2, with the last two exchanged if i is odd
    PrMeshTopology_TriangleFan,     // triangle i is indices i + 1, i + 2, 0
};
typedef uint8_t PrMeshTopology;

// Returns the number of indices in the triangle list that `indexCount` indices of `topology` form. Indices at the end
// that complete no triangle form none. Returns zero for a topology that is not a PrMeshTopology, and for a strip or a
// fan whose list would have more indices than a 32-bit count holds.
uint32_t prMeshTriangleListCount(PrMeshTopology topology, uint32_t indexCount);

// Writes to `out` the triangle list that `indices` form under `topology`. `out` holds prMeshTriangleListCount indices.
// Every triangle keeps its winding and the order of its vertices, as glTF and the graphics APIs define them
// (glTF-2.0/#meshes-overview), so the first vertex of a triangle of the list is the first vertex of the same triangle
// of the strip or fan. Triangles of a strip that repeat a vertex are kept, so that triangle i of the list is triangle i
// of the strip. No index restarts a strip or a fan.
//
// A null `indices.data` converts vertices that are not indexed. A triangle list converts to itself, which checks its
// indices and changes their type. Returns OutOfRange for an index that is not less than `vertexCount`, and Limit for a
// strip or a fan whose list would have more indices than a 32-bit count holds. `out` must not overlap `indices`.
PrStatus prMeshTriangleList(PrMeshTopology topology, PrMeshIndexView indices, uint32_t vertexCount, PrMeshIndices out);

// ---------------------------------------------------------------------------------------------------------------------
// Vertices: unsharing, welding and reordering
//
// These functions treat a vertex as `vertexSize` bytes of an array of whole vertices. A mesh that keeps its attributes
// in separate arrays calls a function once for each array with the same indices or the same remap.
//
// A remap is an array with one entry for each vertex. The entry is the vertex's new index, or kPrMeshIndexNone for a
// vertex that the new mesh leaves out. Several vertices may share a new index. prMeshVerticesRemap and
// prMeshIndicesRemap apply a remap.

// Writes to `out` one vertex for each index of `indices`, in their order, so that no two triangles of the result share
// a vertex. `out` holds `indices.count` vertices and must not overlap `vertices`.
PrStatus prMeshVerticesUnindex(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
    PrMeshIndexView indices, void* PR_NULLABLE out);

// Writes to `remap` a remap that welds vertices whose bytes are equal, and writes the number of welded vertices to
// `outCount`. `remap` holds `vertexCount` entries. Welded vertices are numbered in the order of the first vertex of
// each, so the result does not depend on how equal vertices are found.
//
// Equality is of all `vertexSize` bytes. Padding inside a vertex must therefore hold the same bytes in every vertex,
// and floats that compare equal with different bytes, such as zero and negative zero, do not weld. Takes time
// proportional to the bytes of the vertices.
PrStatus prMeshWeldRemap(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
    uint32_t* PR_NULLABLE remap, uint32_t* outCount);

// Writes to `remap` a remap that numbers the vertices in the order that `indices` first use them, and leaves out the
// vertices that no index uses. Writes the number of vertices used to `outCount`. `remap` holds `vertexCount` entries.
//
// Vertices in this order are read from memory nearly in sequence when the triangles are drawn in order, which is the
// order to store them in after prMeshIndicesOptimizeCache.
PrStatus prMeshFetchRemap(PrMeshIndexView indices, uint32_t vertexCount, uint32_t* PR_NULLABLE remap, uint32_t* outCount);

// Writes each vertex of `vertices` to `out` at the index that `remap` gives it. `out` holds as many vertices as the
// remap numbers, and must not overlap `vertices`. If several vertices share a new index, the first of them is written.
// Returns OutOfRange if the remap names an index that is not less than `outCount`.
PrStatus prMeshVerticesRemap(const void* PR_NULLABLE vertices, uint32_t vertexCount, uint32_t vertexSize,
    const uint32_t* PR_NULLABLE remap, void* PR_NULLABLE out, uint32_t outCount);

// Writes to `out` each index of `indices` replaced by what `remap` gives it. `remap` holds `vertexCount` entries, and
// `out` holds `indices.count` indices. A null `indices.data` writes the remap of each vertex in order, which is the
// index buffer of vertices that were not indexed. `out` may be the storage of `indices` if the types are the same.
// Returns OutOfRange for an index that is not less than `vertexCount` or that the remap leaves out.
PrStatus prMeshIndicesRemap(PrMeshIndexView indices, const uint32_t* PR_NULLABLE remap, uint32_t vertexCount,
    PrMeshIndices out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
