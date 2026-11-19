// OpenUSD's schemas of geometry, shading and light, read from a composed stage (pr/usd_stage.h): UsdGeom's transforms,
// meshes, primitive variables, subsets, cameras and point instancers, UsdShade's material bindings and its preview
// surface (UsdPreviewSurface with UsdUVTexture, UsdTransform2d and UsdPrimvarReader), and UsdLux's lights.
//
// The module follows OpenUSD's definitions of these schemas (its schema documentation and the "UsdPreviewSurface
// Specification"), which are the reference until the Alliance for OpenUSD publishes specifications of their domains. It
// applies the fallback of every attribute that a schema declares, which a stage does not: a stage knows no schema, so
// an attribute that a prim does not author has no value there.
//
// The module reads what a schema says of one prim, and composes nothing across prims. What a consumer walks, it walks:
// a transform is local to its prim, a binding is the prim's own, and a primitive variable is the prim's own. What the
// schemas say of a walk, such as a transform that resets the stack of its ancestors, a binding that is stronger than
// its descendants', or a prim that its ancestor makes invisible, the reading reports and the consumer applies.
//
// What a reading guarantees. A stage's values are data from outside the program, and the schemas ask more of them than
// a stage checks. A call that returns success has checked what it returns against the schema: every index names an
// element of the array that it indexes, every count agrees with its interpolation, and every value has its type. An
// attribute of another type than the schema's is converted where the conversion keeps its meaning, as a double3 of
// points is to floats, and is otherwise an attribute that fails its read.
//
// Threads: the calls read the stage, which is immutable, and write only the caller's scratch.

#pragma once

#include "pr/geom.h"
#include "pr/raw_string.h"
#include "pr/usd_stage.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// The stage

// The axis that points up in a stage's world.
enum {
    PrUsdUpAxis_Y,
    PrUsdUpAxis_Z,
};
typedef uint8_t PrUsdUpAxis;

// What a stage's metadata says of its world and its time, with the fallbacks that OpenUSD applies.
typedef struct PrUsdGeomStageInfo {
    PrUsdUpAxis upAxis;         // `upAxis`, or Y
    double metersPerUnit;       // `metersPerUnit`, or 0.01: a stage is in centimeters unless it says otherwise
    bool hasTimeRange;          // the stage authors `startTimeCode` and `endTimeCode`
    double startTimeCode;       // or 0
    double endTimeCode;         // or 0
    double timeCodesPerSecond;  // prUsdStageTimeCodesPerSecond
} PrUsdGeomStageInfo;

// Returns what the stage's metadata says of its world and its time.
PrUsdGeomStageInfo prUsdGeomStageInfo(const PrUsdStage* stage);

// ---------------------------------------------------------------------------------------------------------------------
// Prims by schema

// What a prim's type makes it, of the schemas that this module reads. A type that is none of these, or that is empty,
// is Other: a consumer walks under it, since its children may be any of these.
enum {
    PrUsdGeomKind_Other,
    PrUsdGeomKind_Scope,            // imageable, with no transform
    PrUsdGeomKind_Xform,            // an Xform, or another transformable type that draws nothing itself, as SkelRoot
    PrUsdGeomKind_Mesh,
    PrUsdGeomKind_Shape,            // an implicit surface (prUsdGeomShapeRead), as Cube or Sphere
    PrUsdGeomKind_Gprim,            // another geometric primitive, as Points or BasisCurves
    PrUsdGeomKind_Camera,
    PrUsdGeomKind_PointInstancer,
    PrUsdGeomKind_Light,            // a light of UsdLux (prUsdLuxLightRead)
    PrUsdGeomKind_Material,
    PrUsdGeomKind_Shader,           // a Shader or a NodeGraph, which a material's network holds
    PrUsdGeomKind_Subset,           // a GeomSubset
};
typedef uint8_t PrUsdGeomKind;

// Returns what the prim's type makes it.
PrUsdGeomKind prUsdGeomPrimKind(const PrUsdStage* stage, PrUsdPrimIndex prim);

// Returns true if a prim of `kind` has a transform of its own (UsdGeomXformable).
bool prUsdGeomKindIsXformable(PrUsdGeomKind kind);

// What a prim is drawn for (UsdGeomImageable's `purpose`).
enum {
    PrUsdGeomPurpose_Default,
    PrUsdGeomPurpose_Render,
    PrUsdGeomPurpose_Proxy,
    PrUsdGeomPurpose_Guide,
};
typedef uint8_t PrUsdGeomPurpose;

// Returns the purpose that the prim authors, and sets `*authored`. A prim that authors none takes its nearest
// ancestor's, which is the consumer's walk, and Default at the root.
PrUsdGeomPurpose prUsdGeomPurpose(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch, bool* authored);

// Returns true if the prim's `visibility` at `time` is `invisible`, which hides the prim and every prim under it.
bool prUsdGeomIsInvisible(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch);

// ---------------------------------------------------------------------------------------------------------------------
// Transforms (UsdGeomXformable)

// Writes to `*out` the prim's local transform at `time`: its operations in the order of `xformOpOrder`, each the
// transform that its attribute's value at the time gives, so that a point maps through the last first. A matrix maps
// column vectors (pr/geom.h). `*resetsStack` is set if the order begins with `!resetXformStack!`, which makes the
// transform the prim's whole transform to the world, ignoring its ancestors'. A prim without an order has the identity.
//
// Returns false, with the identity, if the order names an operation that the prim does not author, an operation of no
// type that the schema knows, or a value of a type that the operation does not take.
bool prUsdGeomLocalTransform(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
    PrDMatrix4* out, bool* resetsStack);

// Returns true if the prim's local transform may differ between two times: an operation of its order has more than one
// time sample.
bool prUsdGeomTransformVaries(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch);

// Writes to `times` the times, from `start` to `end` inclusive, of the samples of the operations of the prim's order,
// each once and in increasing order, up to `capacity` of them. Returns the number that there are.
uint32_t prUsdGeomTransformTimes(const PrUsdStage* stage, PrUsdPrimIndex prim, double start, double end,
    PrUsdScratch* scratch, double* PR_NULLABLE times, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Primitive variables (UsdGeomPrimvar)

// How a primitive variable's elements spread over a surface.
enum {
    PrUsdGeomInterpolation_Constant,    // one element for the whole surface
    PrUsdGeomInterpolation_Uniform,     // one element for each face
    PrUsdGeomInterpolation_Varying,     // one element for each point, interpolated linearly
    PrUsdGeomInterpolation_Vertex,      // one element for each point, interpolated as the surface is
    PrUsdGeomInterpolation_FaceVarying, // one element for each corner of each face
};
typedef uint8_t PrUsdGeomInterpolation;

// A primitive variable's value. `value` is an array, or one element for a value that is not. With `indices`, element i
// of the surface is `value`'s element `indices[i]`, and otherwise its element i. `elementSize` elements of the array
// make one element of the surface.
typedef struct PrUsdGeomPrimvar {
    PrUsdValue value;
    const int32_t* PR_NULLABLE indices;
    uint32_t indexCount;
    PrUsdGeomInterpolation interpolation;
    uint32_t elementSize;
} PrUsdGeomPrimvar;

// Reads the primitive variable `primvars:<name>` of the prim at `time`, with its `primvars:<name>:indices`, its
// interpolation (Constant unless it authors one) and its element size (1 unless it authors one). Returns false if the
// prim authors no value for it, if its indices are not ints, or if an index is negative or past its value's elements.
// The variable's own prim is read alone: a constant variable that an ancestor authors is the consumer's to inherit.
bool prUsdGeomPrimvarRead(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, double time,
    PrUsdScratch* scratch, PrUsdGeomPrimvar* out);

// ---------------------------------------------------------------------------------------------------------------------
// Meshes (UsdGeomMesh)

// A mesh's subdivision scheme. A mesh of any scheme but None is a control cage, which the scheme refines into a smooth
// surface.
enum {
    PrUsdGeomSubdivision_CatmullClark,
    PrUsdGeomSubdivision_Loop,
    PrUsdGeomSubdivision_Bilinear,
    PrUsdGeomSubdivision_None,
};
typedef uint8_t PrUsdGeomSubdivision;

// A mesh at a time: its points and its faces, each face a run of corners in `faceVertexIndices`, which name points.
// `holeIndices` names the faces that are holes and draw nothing. A mesh's faces wind counterclockwise seen from their
// front unless it is `leftHanded`. `normals` is `primvars:normals` if the mesh authors it, and else `normals`, whose
// interpolation is Vertex unless it authors one, and has a value of no elements if the mesh authors neither.
typedef struct PrUsdGeomMesh {
    const PrVec3* PR_NULLABLE points;
    uint32_t pointCount;
    const int32_t* PR_NULLABLE faceVertexCounts;
    uint32_t faceCount;
    const int32_t* PR_NULLABLE faceVertexIndices;
    uint32_t cornerCount;
    const int32_t* PR_NULLABLE holeIndices;
    uint32_t holeCount;
    bool leftHanded;
    bool doubleSided;
    PrUsdGeomSubdivision subdivision;
    PrUsdGeomPrimvar normals;
} PrUsdGeomMesh;

// Reads the mesh at `time`. Returns NotFound if the mesh authors no points, and InvalidData if its topology does not
// hold together: a face of fewer than three corners or a negative count, counts that do not sum to its corners, a
// corner or a hole that names nothing, or normals whose number does not match their interpolation.
PrStatus prUsdGeomMeshRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
    PrUsdGeomMesh* out);

// Returns the number of elements that a primitive variable of `interpolation` has on `mesh`, as its indices or its
// value count them, before `elementSize` multiplies it.
uint32_t prUsdGeomMeshElementCount(const PrUsdGeomMesh* mesh, PrUsdGeomInterpolation interpolation);

// Returns true if `primvar`, read from a prim of `mesh`, has as many elements as its interpolation asks of the mesh.
bool prUsdGeomPrimvarFits(const PrUsdGeomMesh* mesh, const PrUsdGeomPrimvar* primvar);

// A subset of a mesh's faces (UsdGeomSubset), which carries a material binding of its own in the family `materialBind`.
typedef struct PrUsdGeomSubset {
    PrUsdPrimIndex prim;
    const int32_t* PR_NULLABLE indices; // faces of the mesh
    uint32_t indexCount;
} PrUsdGeomSubset;

// Writes to `out` the subsets of faces among the children of `mesh` whose `familyName` is `family`, up to `capacity` of
// them, at `time`, and returns the number that there are. A subset whose element type is not `face`, or that names a
// face that the mesh of `faceCount` faces lacks, is left out.
uint32_t prUsdGeomSubsets(const PrUsdStage* stage, PrUsdPrimIndex mesh, uint32_t faceCount, PrStringView family,
    double time, PrUsdScratch* scratch, PrUsdGeomSubset* PR_NULLABLE out, uint32_t capacity);

// ---------------------------------------------------------------------------------------------------------------------
// Shapes (UsdGeomSphere, UsdGeomCube, UsdGeomCylinder, UsdGeomCylinder_1, UsdGeomCone, UsdGeomCapsule, UsdGeomPlane)

// The kinds of implicit surface. Each is centered on its prim's origin.
enum {
    PrUsdGeomShapeKind_Sphere,      // of `radius`
    PrUsdGeomShapeKind_Cube,        // of edges of `size`
    PrUsdGeomShapeKind_Cylinder,    // of `height` along its axis, from `radiusBottom` to `radiusTop`, capped
    PrUsdGeomShapeKind_Cone,        // of `height` along its axis, from a base of `radius` to its apex at its top
    PrUsdGeomShapeKind_Capsule,     // a cylinder of `radius` and `height`, with a hemisphere on each end
    PrUsdGeomShapeKind_Plane,       // of `width` by `length`, facing along its axis
};
typedef uint8_t PrUsdGeomShapeKind;

// An implicit surface at a time, with its schema's fallbacks where it authors nothing. `axis` is 0, 1 or 2 for x, y or
// z. A plane's `width` is along x for an axis of z or y, and along z for x, and its `length` along y for an axis of z
// or x, and along z for y. A cylinder of UsdGeomCylinder has one `radius`, which is both of its radii. A shape's
// surface faces outward unless it is `leftHanded`.
typedef struct PrUsdGeomShape {
    PrUsdGeomShapeKind kind;
    uint8_t axis;
    bool leftHanded;
    bool doubleSided;
    double radius;
    double radiusBottom;
    double radiusTop;
    double height;
    double size;
    double width;
    double length;
} PrUsdGeomShape;

// Reads the prim's shape at `time`. Returns false if the prim is none of the shapes, or if a dimension is negative or
// not finite.
bool prUsdGeomShapeRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
    PrUsdGeomShape* out);

// ---------------------------------------------------------------------------------------------------------------------
// Cameras (UsdGeomCamera)

// A camera at a time, with the schema's fallbacks. The lens's measures are in tenths of the stage's unit, so a focal
// length over an aperture is the tangent of half the field of view, and an orthographic camera's apertures are the
// view's extent. The camera looks down its local -z with +y up.
typedef struct PrUsdGeomCamera {
    bool orthographic;
    float focalLength;
    float horizontalAperture;
    float verticalAperture;
    float horizontalApertureOffset;
    float verticalApertureOffset;
    float nearClip;             // in the stage's unit
    float farClip;
} PrUsdGeomCamera;

// Reads the camera at `time`. Returns false if a value has a type that the schema does not give it.
bool prUsdGeomCameraRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
    PrUsdGeomCamera* out);

// ---------------------------------------------------------------------------------------------------------------------
// Point instancers (UsdGeomPointInstancer)

// A point instancer at a time: for each instance, its prototype, an index of `prototypes`, and its placement within the
// instancer, which each of `positions`, `orientations` and `scales` gives if the instancer authors it. An instance that
// `invisibleIds` names, by its id, is not drawn. `ids` is null for an instancer whose ids are the instances' indices.
typedef struct PrUsdGeomPointInstancer {
    const PrUsdPrimIndex* PR_NULLABLE prototypes;   // kPrUsdNone for a target that names no prim
    uint32_t prototypeCount;
    const int32_t* PR_NULLABLE protoIndices;
    uint32_t instanceCount;
    const PrVec3* PR_NULLABLE positions;
    const PrQuat* PR_NULLABLE orientations;
    const PrVec3* PR_NULLABLE scales;
    const int64_t* PR_NULLABLE ids;
    const int64_t* PR_NULLABLE invisibleIds;
    uint32_t invisibleIdCount;
} PrUsdGeomPointInstancer;

// Reads the instancer at `time`. Returns NotFound if it authors no `protoIndices`, and InvalidData if an index names no
// prototype, or if `positions`, `orientations`, `scales` or `ids` is authored with another count than the instances'.
PrStatus prUsdGeomPointInstancerRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch,
    PrUsdGeomPointInstancer* out);

// Returns the transform of an instance within its instancer: its scale, then its orientation, then its position.
PrDMatrix4 prUsdGeomInstanceTransform(const PrUsdGeomPointInstancer* instancer, uint32_t instance);

// Returns true if the instance is drawn: `invisibleIds` does not name its id.
bool prUsdGeomInstanceIsVisible(const PrUsdGeomPointInstancer* instancer, uint32_t instance);

// ---------------------------------------------------------------------------------------------------------------------
// Material bindings (UsdShadeMaterialBindingAPI)

// The material that a prim binds itself to, for the preview purpose: `material:binding:preview` if the prim authors it,
// and else `material:binding`. `strongerThanDescendants` is the binding's `bindMaterialAs`, which makes it win over the
// bindings of the prims under the prim. `material` is kPrUsdNone if the prim binds nothing, or binds a path that names
// no material.
typedef struct PrUsdShadeBinding {
    PrUsdPrimIndex material;
    bool authored;
    bool strongerThanDescendants;
} PrUsdShadeBinding;

// Returns the prim's own binding. Collection-based bindings are not read.
PrUsdShadeBinding prUsdShadeDirectBinding(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch);

// Returns the material that binds the prim for the preview purpose, or kPrUsdNone: the binding of the prim or of its
// nearest ancestor that authors one, unless an ancestor's binding is stronger than its descendants', and then the
// binding of the highest such ancestor (UsdShadeMaterialBindingAPI::ComputeBoundMaterial, without collections). A prim
// of a prototype has the ancestors of the prototype alone, so the binding that an instance inherits from above it is
// the consumer's to apply. Takes time proportional to the prim's depth.
PrUsdPrimIndex prUsdShadeBoundMaterial(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch);

// ---------------------------------------------------------------------------------------------------------------------
// The preview surface (UsdPreviewSurface)

// The inputs of UsdPreviewSurface.
enum {
    PrUsdPreviewInput_DiffuseColor,
    PrUsdPreviewInput_EmissiveColor,
    PrUsdPreviewInput_SpecularColor,
    PrUsdPreviewInput_Metallic,
    PrUsdPreviewInput_Roughness,
    PrUsdPreviewInput_Clearcoat,
    PrUsdPreviewInput_ClearcoatRoughness,
    PrUsdPreviewInput_Opacity,
    PrUsdPreviewInput_OpacityThreshold,
    PrUsdPreviewInput_Ior,
    PrUsdPreviewInput_Normal,
    PrUsdPreviewInput_Displacement,
    PrUsdPreviewInput_Occlusion,
    PrUsdPreviewInput_Count,
};
typedef uint8_t PrUsdPreviewInput;

// The channels of a texture that an input reads (UsdUVTexture's outputs).
enum {
    PrUsdTextureChannel_R,
    PrUsdTextureChannel_G,
    PrUsdTextureChannel_B,
    PrUsdTextureChannel_A,
    PrUsdTextureChannel_Rgb,
};
typedef uint8_t PrUsdTextureChannel;

// What a texture does outside [0, 1] (UsdUVTexture's `wrapS` and `wrapT`). `useMetadata` asks for what the image file
// says, and no image format that the module knows says anything, so it is Repeat, as OpenUSD's Storm draws it.
enum {
    PrUsdTextureWrap_Repeat,
    PrUsdTextureWrap_Mirror,
    PrUsdTextureWrap_Clamp,
    PrUsdTextureWrap_Black,
};
typedef uint8_t PrUsdTextureWrap;

// How a texture's values are encoded: UsdUVTexture's `sourceColorSpace`, or the color space that the file's attribute
// names, as the transfer function that it implies. Auto leaves it to the file and the input, Raw is values as they are,
// which a linear space such as `lin_rec709`, `acescg` or `lin_ap1` names, and Srgb is sRGB's encoding. A space's
// primaries are not a texture's concern.
enum {
    PrUsdTextureColorSpace_Auto,
    PrUsdTextureColorSpace_Raw,
    PrUsdTextureColorSpace_Srgb,
};
typedef uint8_t PrUsdTextureColorSpace;

// A UsdUVTexture that an input reads: its file, the channels that the input reads, how it samples, and the value that
// each texel becomes, `texel * scale + bias`. The coordinates are those of the primitive variable `primvar`, which a
// UsdPrimvarReader_float2 reads, or empty if no reader gives them, and `transform` takes them to the image's, in USD's
// texture space, with t up the image: a UsdTransform2d's, which scales, rotates counterclockwise by its degrees and
// then translates, or a MaterialX network's placement of the image, or the identity.
typedef struct PrUsdTexture {
    PrUsdPrimIndex shader;
    uint32_t fileProperty;      // the property of `shader` whose value is the file, an index of the stage's properties
    PrStringView file;          // the asset path as authored
    PrUsdTextureChannel channel;
    PrUsdTextureWrap wrapS;
    PrUsdTextureWrap wrapT;
    PrUsdTextureColorSpace colorSpace;
    float scale[4];
    float bias[4];
    float fallback[4];          // the value where the file does not read
    PrStringView primvar;
    PrAffine2 transform;
} PrUsdTexture;

// An input of the preview surface: its value, which is the schema's fallback where the network authors none, and the
// texture that it reads, if it is connected to one, or the primitive variable that a UsdPrimvarReader reads for it, if
// it is connected to one of those, whose `fallback` is then the value. A scalar input's value is `value[0]`.
typedef struct PrUsdPreviewInputValue {
    float value[3];
    bool textured;
    PrUsdTexture texture;
    PrStringView primvar;       // empty unless a primitive variable gives the input
} PrUsdPreviewInputValue;

// A material's preview surface: the UsdPreviewSurface shader that the material's universal surface output reaches, and
// each of its inputs, followed through the connections of the network, its node graphs and the material's own inputs.
typedef struct PrUsdPreviewSurface {
    PrUsdPrimIndex shader;
    bool useSpecularWorkflow;
    PrUsdPreviewInputValue inputs[PrUsdPreviewInput_Count];
} PrUsdPreviewSurface;

// Reads the preview surface of `material` at `time`. Returns false if its `outputs:surface` reaches no shader whose
// `info:id` is UsdPreviewSurface. An input that a connection takes to anything but a value, a UsdUVTexture or an input
// of a node graph or of the material has its fallback.
bool prUsdPreviewSurfaceRead(const PrUsdStage* stage, PrUsdPrimIndex material, double time, PrUsdScratch* scratch,
    PrUsdPreviewSurface* out);

// Resolves the file of a texture to a location, against the layer that authors it (prUsdStageAttributeAssetResolve).
// The caller releases the location with prRawStringReset.
PrRawString prUsdTextureLocation(const PrUsdStage* stage, const PrUsdTexture* texture, double time);

// ---------------------------------------------------------------------------------------------------------------------
// MaterialX networks (UsdMtlx)
//
// A material's MaterialX network is the shader that its `outputs:mtlx:surface` reaches, whose `info:id` names a node
// definition of MaterialX's standard library, as `ND_standard_surface_surfaceshader`, and the node graphs that its
// inputs connect to. A layer of a MaterialX document holds one (pr/usd.h). The reading folds the graph behind an input
// into what a material of fixed inputs draws: a value, or one image read through an affine map of its channels, which
// is what the image, constant and channel nodes and the arithmetic of a node by a value compose to.

// The node definition of a MaterialX shader: `info:id` without its `ND_` and its type, as `standard_surface`,
// `open_pbr_surface`, `UsdPreviewSurface` or `surface_unlit`.
PrStringView prUsdMtlxNodeCategory(PrStringView id);

// Returns the shader that the material's `outputs:mtlx:surface` reaches, or kPrUsdNone, and writes its `info:id` to
// `*id`.
PrUsdPrimIndex prUsdMtlxSurface(const PrUsdStage* stage, PrUsdPrimIndex material, PrUsdScratch* scratch, PrStringView* id);

// An input of a MaterialX shader, folded. An input whose graph reads a texture or a primitive variable varies over a
// surface, and its source is the texture's texel, or else the value of the primitive variable `primvar`, which a
// `geompropvalue` node reads, `primvarFallback` where the surface has none. Element i of such an input is the sum over
// the source's channels j of `weights[i][j]` times channel j, plus `offset[i]`. An input without a source is `value`,
// which for a primitive variable's input is its value where the surface lacks the variable. The texture's `channel`,
// `scale` and `bias` are UsdUVTexture's and unused here. A `normalmap` node makes the input a tangent-space normal of
// the texture, which `normalMap` says. `tint` is a primitive variable that multiplies the whole input element by
// element, as a `multiply` node of a texture's value and the variable's makes it, with `tintFallback` where the surface
// has none.
typedef struct PrUsdMtlxInput {
    float value[4];
    bool textured;
    bool normalMap;
    PrUsdTexture texture;
    float weights[4][4];
    float offset[4];
    PrStringView primvar;
    float primvarFallback[4];
    PrStringView tint;
    float tintFallback[4];

    // The graph had a node that the folding does not compute, which it read as the first of its inputs, or a node of
    // two textures, of which it kept the first.
    bool approximated;
} PrUsdMtlxInput;

// Reads the input `name`, without its `inputs:` namespace, of the MaterialX shader `shader`, of `width` elements, at
// `time`: its value, or the value that the graph behind its connection folds to. `fallback` is the value where nothing
// decides it. A texture's coordinates are the primitive variable of a `texcoord` node's index, `st` for index 0 and
// `st1` for index 1, or a `geompropvalue` node's, and `st` where nothing connects them; a tiled image's tiling and
// offset are its transform, and coordinates that another node computes are approximated as `st`. A texture's color
// space is its file's `colorSpace`, as the transfer function that it implies: sRGB for `srgb_texture` and the spaces of
// sRGB's transfer, and raw for any other, such as the linear `lin_rec709` and `acescg`. Returns false, with the
// fallback, if the shader has no such input.
bool prUsdMtlxInputRead(const PrUsdStage* stage, PrUsdPrimIndex shader, PrStringView name, uint32_t width,
    const float* fallback, double time, PrUsdScratch* scratch, PrUsdMtlxInput* out);

// ---------------------------------------------------------------------------------------------------------------------
// Lights (UsdLux)

// The kinds of light.
enum {
    PrUsdLightKind_Distant,
    PrUsdLightKind_Sphere,
    PrUsdLightKind_Disk,
    PrUsdLightKind_Rect,
    PrUsdLightKind_Cylinder,
    PrUsdLightKind_Dome,
    PrUsdLightKind_Other,       // a light of another type, as a GeometryLight or a PortalLight
};
typedef uint8_t PrUsdLightKind;

// How a dome light's texture maps directions (UsdLuxDomeLight's `inputs:texture:format`). Automatic takes the layout
// from the file itself.
enum {
    PrUsdLuxTextureFormat_Automatic,
    PrUsdLuxTextureFormat_Latlong,
    PrUsdLuxTextureFormat_MirroredBall,
    PrUsdLuxTextureFormat_Angular,
    PrUsdLuxTextureFormat_CubeMapVerticalCross,
};
typedef uint8_t PrUsdLuxTextureFormat;

// The axis that a dome's top pole starts along (UsdLuxDomeLight_1's `poleAxis`). Scene is the stage's up axis.
enum {
    PrUsdLuxPoleAxis_Y,
    PrUsdLuxPoleAxis_Z,
    PrUsdLuxPoleAxis_Scene,
};
typedef uint8_t PrUsdLuxPoleAxis;

// A light at a time, with the schema's fallbacks. A light emits along its local -z. Its radiance is `color` times
// `intensity` times 2 to the `exposure`, and with `normalize`, divided by its area, so that its size does not change
// how much it emits. A distant light's `intensity` is its illuminance at a surface that faces it.
//
// A dome light's radiance arrives from every direction, times its texture's where it has one. A texture of the latlong
// format is OpenEXR's latitude-longitude map in the dome's frame: its columns run in longitude from +pi at the left
// edge to -pi at the right, its rows in latitude from +pi/2 at the top to -pi/2 at the bottom, and the direction at
// latitude a and longitude b is (sin b cos a, sin a, cos b cos a), so that the middle of the image is +z and its top
// +y. A DomeLight's top pole is +y, and a DomeLight_1's is its `poleAxis`.
typedef struct PrUsdLuxLight {
    PrUsdLightKind kind;
    PrVec3 color;               // linear
    float intensity;
    float exposure;
    bool normalize;
    bool enableColorTemperature;
    float colorTemperature;     // in kelvin, which tints `color` if it is enabled
    float radius;               // of a sphere, a disk or a cylinder
    float width;                // of a rectangle
    float height;
    float length;               // of a cylinder
    float angle;                // of a distant light: the angular diameter of its source, in degrees
    bool treatAsPoint;          // a sphere light that lights as a point
    float coneAngle;            // ShapingAPI: the half angle of the cone that it lights, in degrees, 90 for none
    float coneSoftness;

    // A dome's or a rectangle's `inputs:texture:file`, as authored, and the property of the light that holds it, an
    // index of the stage's properties, or kPrUsdNone without one.
    PrStringView textureFile;
    uint32_t textureFileProperty;
    PrUsdLuxTextureFormat textureFormat;
    PrUsdLuxPoleAxis poleAxis;
} PrUsdLuxLight;

// Reads the light at `time`, whose inputs are in the `inputs:` namespace, as OpenUSD's UsdLux reads them: a light that
// authors the names that came before the namespace has the fallbacks. Returns false for a prim that is not a light.
bool prUsdLuxLightRead(const PrUsdStage* stage, PrUsdPrimIndex prim, double time, PrUsdScratch* scratch, PrUsdLuxLight* out);

// Resolves the texture of the light `prim`, which `light` read, to a location against the layer that authors it, or
// returns the empty string for a light without one. The caller releases the location with prRawStringReset.
PrRawString prUsdLuxTextureLocation(const PrUsdStage* stage, PrUsdPrimIndex prim, const PrUsdLuxLight* light, double time);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
