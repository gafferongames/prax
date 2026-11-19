// Draco: decoding the compressed geometry of the Draco bitstream.
//
// A stream holds one geometry: a triangle mesh or a point cloud, as points that each have one value of every attribute,
// and, for a mesh, faces that each name three points. prDracoCreate decodes a whole stream into a geometry, and the
// functions after it return what the geometry holds. The points, their order and the values of their attributes are
// exactly those that the reference decoder gives, so an index that a stream's producer wrote beside the stream names
// the same point here.
//
// A stream is data from outside the program. Every count, index and offset in it is checked, so a stream that is not
// one that the bitstream defines fails with InvalidData and reads nothing past its end. A valid stream of a few bytes
// can describe any number of points, since a run of equal values costs nothing to code, so the caller states the most
// points and faces that it accepts (PrDracoLimits), and decoding allocates in proportion to those and to the size of
// the stream, and takes time in proportion to what it allocates.
//
// A geometry is immutable, so any number of threads may read it.

#pragma once

#include "pr/base.h"
#include "pr/byte_view.h"
#include "pr/status.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A decoded geometry.
typedef struct PrDraco PrDraco;

// The index of an attribute that a geometry does not have.
PR_CONSTANT uint32_t kPrDracoNone = 0xffffffffu;

// What a stream holds.
enum {
    PrDracoGeometry_PointCloud,
    PrDracoGeometry_TriangleMesh,
};
typedef uint8_t PrDracoGeometry;

// What an attribute is for, as the stream names it. The bitstream gives the values of no kind a meaning, and a stream's
// container, such as a glTF primitive, says which attribute is which by its id instead.
enum {
    PrDracoAttributeKind_Position,
    PrDracoAttributeKind_Normal,
    PrDracoAttributeKind_Color,
    PrDracoAttributeKind_TexCoord,
    PrDracoAttributeKind_Generic,
};
typedef uint8_t PrDracoAttributeKind;

// The type of an attribute's components.
enum {
    PrDracoComponentType_Int8,
    PrDracoComponentType_Uint8,
    PrDracoComponentType_Int16,
    PrDracoComponentType_Uint16,
    PrDracoComponentType_Int32,
    PrDracoComponentType_Uint32,
    PrDracoComponentType_Int64,
    PrDracoComponentType_Uint64,
    PrDracoComponentType_Float32,
    PrDracoComponentType_Float64,
    PrDracoComponentType_Bool,          // one byte
};
typedef uint8_t PrDracoComponentType;

// One attribute of a geometry's points.
typedef struct PrDracoAttribute {
    uint32_t id;                        // the id that the stream gives the attribute, which its container names it by
    PrDracoAttributeKind kind;
    PrDracoComponentType componentType;
    uint8_t componentCount;             // the number of components in a value, at least one
    bool normalized;                    // the stream's statement that integer components stand for [0, 1] or [-1, 1]
} PrDracoAttribute;

// The most that a stream may describe. A stream that describes more fails with Limit before anything is allocated for
// it.
typedef struct PrDracoLimits {
    uint32_t maxPoints;
    uint32_t maxFaces;
} PrDracoLimits;

// Returns limits of 2^24 points and 2^24 faces.
PrDracoLimits prDracoLimitsDefault(void);

// Decodes `stream`, which is used only during the call. Null limits mean the defaults.
//
// Returns InvalidData for a stream that is not one that the bitstream defines, Limit for one that describes more than
// the limits, and NotSupported for a stream of another version of the bitstream than 2.2, or 2.3 for a point cloud, for
// a point cloud coded as a k-d tree, and for a stream that uses a prediction scheme that the bitstream has withdrawn.
// On failure, `*out` is null.
PrStatus prDracoCreate(PrByteView stream, const PrDracoLimits* PR_NULLABLE limits, PrDraco* PR_NULLABLE* PR_NONNULL out);

// Frees the geometry.
void prDracoDestroy(PrDraco* PR_NULLABLE draco);

// Returns what the stream holds.
PrDracoGeometry prDracoGeometry(const PrDraco* draco);

// Returns the number of points.
uint32_t prDracoPointCount(const PrDraco* draco);

// Returns the number of faces, and the points of each: three indices for a face, each less than the number of points. A
// point cloud has no faces.
uint32_t prDracoFaceCount(const PrDraco* draco);
const uint32_t* PR_NULLABLE prDracoFaces(const PrDraco* draco);

// Return the number of attributes, and the array of them, in the order of the stream.
uint32_t prDracoAttributeCount(const PrDraco* draco);
const PrDracoAttribute* PR_NULLABLE prDracoAttributes(const PrDraco* draco);

// Returns the index of the first attribute whose id is `id`, or kPrDracoNone.
uint32_t prDracoFindAttribute(const PrDraco* draco, uint32_t id);

// Returns the size of a component in bytes.
uint32_t prDracoComponentTypeSize(PrDracoComponentType type);

// Returns the values of an attribute: one value for each point, in the order of the points, each `componentCount`
// components that are tightly packed and little-endian.
PrByteView prDracoAttributeValues(const PrDraco* draco, uint32_t attribute);

#if defined(__cplusplus)
} // extern "C"
#endif

PR_ASSUME_NONNULL_END
