// The state of decoding one Draco stream, and the geometry that it leaves.

#pragma once

#include "pr/draco.h"
#include "pr/status.h"

#include "draco/src/draco_stream.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The corner, vertex, face, point or value that an index does not name.
constexpr uint32_t kDracoInvalid = 0xffffffffu;

// How a stream codes its geometry (the bitstream's encoder_method).
enum {
    DracoMethod_Sequential,
    DracoMethod_Edgebreaker,            // for a point cloud, a k-d tree
};
typedef uint8_t DracoMethod;

// How an attribute's values are coded (the bitstream's seq_att_dec_decoder_type).
enum {
    DracoCoding_Generic,                // the values, whole
    DracoCoding_Integer,                // integers, predicted
    DracoCoding_Quantization,           // floats quantized to integers, predicted
    DracoCoding_Normals,                // unit vectors as octahedral coordinates, predicted
    DracoCoding_Count,
};
typedef uint8_t DracoCoding;

// The connectivity of a mesh as a corner table. A face has three corners, 3f to 3f + 2, each at one of its vertices.
// The corner opposite a corner is the one that faces it across the edge that the corner does not touch, in the face on
// the other side of that edge.
struct DracoCornerTable {
    uint32_t faceCount;
    uint32_t vertexCount;
    RawArray<uint32_t> cornerVertex;    // by corner: its vertex
    RawArray<uint32_t> opposite;        // by corner: the opposite corner, or kDracoInvalid on a boundary
    RawArray<uint32_t> vertexCorner;    // by vertex: its left-most corner, or kDracoInvalid for an isolated vertex
};

// The connectivity of the attributes that share seams: the corner table cut along the edges across which the
// attributes' values differ. A vertex of the corner table that seams divide is one vertex here for each part.
struct DracoSeamTable {
    RawArray<uint8_t> cornerOnSeam;     // by corner: nonzero if the edge opposite it is a seam
    RawArray<uint8_t> vertexOnSeam;     // by vertex of the corner table: nonzero if a seam meets it
    RawArray<uint32_t> opposite;        // by corner: the opposite corner, or kDracoInvalid across a seam
    RawArray<uint32_t> cornerVertex;    // by corner: its vertex here
    RawArray<uint32_t> vertexCorner;    // by vertex here: its left-most corner
};

// A corner table, whole or cut along seams, as the traversals and the predictions read it: the arrays of one or of the
// other, so that a walk reads the same memory whichever it walks.
struct DracoConnectivity {
    const uint32_t* PR_NULLABLE opposites;      // null where there is no mesh
    const uint32_t* PR_NULLABLE cornerVertices;
    const uint32_t* PR_NULLABLE vertexCorners;
    uint32_t vertices;

    static uint32_t next(uint32_t corner) {
        return corner == kDracoInvalid ? corner : corner % 3 == 2 ? corner - 2 : corner + 1;
    }
    static uint32_t previous(uint32_t corner) {
        return corner == kDracoInvalid ? corner : corner % 3 == 0 ? corner + 2 : corner - 1;
    }
    uint32_t opposite(uint32_t corner) const { return corner == kDracoInvalid ? corner : opposites[corner]; }
    uint32_t vertex(uint32_t corner) const { return corner == kDracoInvalid ? corner : cornerVertices[corner]; }
    uint32_t vertexCount() const { return vertices; }
    uint32_t leftMostCorner(uint32_t vertex) const { return vertexCorners[vertex]; }
    // The corners of the faces to the left and to the right of a corner's face, which face it.
    uint32_t leftCorner(uint32_t corner) const { return opposite(previous(corner)); }
    uint32_t rightCorner(uint32_t corner) const { return opposite(next(corner)); }
    // The corner at the same vertex in the next face around it, in either direction.
    uint32_t swingLeft(uint32_t corner) const { return next(opposite(next(corner))); }
    uint32_t swingRight(uint32_t corner) const { return previous(opposite(previous(corner))); }
};

// Return the corner table whole, and cut along the seams of a seam table.
inline DracoConnectivity dracoWhole(const DracoCornerTable* table) {
    return {table->opposite.data, table->cornerVertex.data, table->vertexCorner.data, table->vertexCount};
}
inline DracoConnectivity dracoCut(const DracoSeamTable* seams) {
    return {seams->opposite.data, seams->cornerVertex.data, seams->vertexCorner.data, seams->vertexCorner.count};
}

// The order in which the values of one group of attributes are coded, which a traversal of the mesh gives.
struct DracoSequence {
    DracoConnectivity connectivity;     // what the traversal walked; no table for a sequence in the order of the points
    RawArray<uint32_t> valueCorner;     // by value: the corner at which the traversal reached its vertex
    RawArray<uint32_t> vertexValue;     // by vertex of the connectivity: its value
    RawArray<uint32_t> valuePoint;      // by value: a point that has it
    RawArray<uint32_t> pointValue;      // by point: its value; empty if each point has the value of its own index
    uint32_t valueCount;
};

// One attribute while it is decoded, and its values once it is.
struct DracoAttributeState {
    PrDracoAttribute info;
    DracoCoding coding;
    uint32_t sequence;                  // the group of attributes that it is coded with
    uint32_t portableComponents;        // the components of a value as integers: two for a normal
    RawArray<int32_t> portable;         // by value: its integers, before the attribute's transform is undone
    bool hasPortable;

    // What undoes the attribute's transform: the bounds of quantization, and the number of bits of either transform.
    RawArray<float> minimums;
    float range;
    uint8_t quantizationBits;

    RawArray<uint8_t> values;           // by point: its value, as prDracoAttributeValues gives it
};

// A geometry, and the state of decoding it.
struct Draco {
    PrDracoGeometry geometry;
    DracoMethod method;
    PrDracoLimits limits;
    PrStatus status;                    // why decoding failed, if it is not InvalidData
    DracoReader reader;

    uint32_t pointCount;
    uint32_t faceCount;
    RawArray<uint32_t> faces;           // by corner: its point
    RawArray<DracoAttributeState> attributes;
    RawArray<PrDracoAttribute> attributeInfos;

    DracoCornerTable table;
    RawArray<DracoSeamTable> seamTables;
    RawArray<DracoSequence> sequences;
};

// Records why decoding fails, and returns false.
inline bool dracoFail(Draco* draco, PrStatus status) {
    draco->status = status;
    return false;
}

} // namespace pr_impl

PR_ASSUME_NONNULL_END
