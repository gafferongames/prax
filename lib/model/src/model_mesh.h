// A model's meshes: the parts of a source's mesh prepared as the vertices, indices and parts of a renderer's mesh, with
// what deforms them.

#pragma once

#include "pr/base.h"
#include "pr/model_source.h"
#include "pr/raw_array.h"
#include "pr/render_resource.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// How a part is prepared.
struct ModelPartOptions {
    bool wantsTangents;         // its material reads a normal map or a direction of its surface
    uint32_t tangentStream;     // the stream of the request that the tangents follow, or kModelStreamNone
    bool keepsTriangleOrder;    // its material blends, so the order of its triangles shows and stays the source's
};

// No stream of a request.
constexpr uint32_t kModelStreamNone = 0xffffffffu;

// How a mesh is prepared: each part's options, whether a draw has a skin for it, so each vertex takes its joints and
// weights, and its morph targets, which every part has, a part of fewer having displacements of zero for the rest.
struct ModelMeshOptions {
    const ModelPartOptions* parts;
    bool skinned;
    uint32_t morphTargetCount;
};

// A mesh as a model prepares it. A vertex is its geometry, as the renderer stores it, and then the values of the
// request's streams, `streamWidths[i]` floats of stream i from `streamOffsets[i]` floats past the geometry, or zeros
// where its part lacks the stream: `stride` bytes, which equal vertices share by, so nothing is padded. `skin` has a
// value for each vertex of a skinned mesh, and `morphs` has `morphTargetCount` values for each vertex, by vertex and
// then by target. `partSources` is the source's part of each part, and `partStreams` its streams, as
// PrModelMeshDataPart::streams has them.
struct ModelMeshPrepared {
    RawArray<uint8_t> vertices;
    uint32_t stride;
    uint32_t vertexCount;
    RawArray<uint32_t> streamOffsets;
    RawArray<uint32_t> streamWidths;
    RawArray<uint32_t> indices;
    RawArray<PrRenderMeshPart> parts;
    RawArray<uint32_t> partFirstVertex;
    RawArray<uint32_t> partVertexCount;
    RawArray<uint32_t> partStreams;
    RawArray<PrRenderSkinVertex> skin;
    RawArray<PrRenderMorphDelta> morphs;
};

// Returns the geometry of vertex `i` of a prepared mesh, and the floats of its streams after it.
inline const PrRenderGeometryVertex* modelMeshVertex(const ModelMeshPrepared& mesh, uint32_t i) {
    return reinterpret_cast<const PrRenderGeometryVertex*>(mesh.vertices.data + size_t(i) * mesh.stride);
}

// Prepares the parts of a mesh that a source read, each as one part, in their order.
//
// A part's triangles become a list with 32-bit indices. A part without normals gets the normals of its faces, and loses
// its tangents. A part that wants tangents and is left without them gets MikkTSpace's, from its stream of texture
// coordinates, if it has it. Vertices that the preparation unshares are shared again where they are still equal, and,
// in a part that deforms and whose vertices are the source's, came from one vertex of the source.
//
// The part is then ordered for a GPU's caches (pr/mesh_cache.h): its triangles so that each reuses vertices that were
// shaded lately, unless it keeps their order, and its vertices in the order in which the triangles first use them. A
// vertex that no triangle uses is left out. Every triangle keeps its vertices and their order.
//
// A skinned vertex takes the four joints of the greatest weight among its influences, with their weights scaled to sum
// to one. A morph target gives a vertex the displacements of its position, its normal and its tangent. A part without
// normals has the normals of its faces, so each target gives a vertex the change of its face's normal under the
// target's displacements, and no two triangles share a vertex.
//
// Returns Limit if the mesh would have more vertices or indices than a 32-bit count holds, and the status of lib/mesh's
// refusal. On failure, `out` is as it was.
PrStatus modelMeshPrepare(const PrModelMeshData& data, const ModelMeshOptions& options, ModelMeshPrepared* out);

// Gives an empty prepared mesh the layout of its vertices: the geometry, and then `streamCount` streams of the widths
// that `streamWidths` gives, which are the request's.
void modelMeshPreparedInit(ModelMeshPrepared* mesh, const uint32_t* PR_NULLABLE streamWidths, uint32_t streamCount);

// Frees the prepared mesh's storage.
void modelMeshPreparedReset(ModelMeshPrepared* mesh);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
