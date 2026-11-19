// OpenUSD layers: a layer file read into its specs, their fields and their values, as the USD Core Specification's
// document data model describes them.
//
// A layer is one document: the specs that a file authors and nothing that composition would add. It opens no other
// layer, resolves no value across specs, and knows no schema. Text layers (.usda), crate layers (.usdc) and packages
// (.usdz) read into the same model, and a text layer and a crate layer of the same content read as the same specs,
// fields and values.
//
// What a layer guarantees. prUsdLayerCreate checks a file as it reads it and makes a layer only if every check passes,
// so a consumer uses a layer without checking it again:
//
// - Spec 0 is the layer spec. Every other spec has a parent of a form that may hold it: a prim under the layer spec, a
//   prim or a variant; a property or a variant set under a prim or a variant; a variant under a variant set. The specs
//   form one tree, and a parent's index is less than its children's.
// - Every spec's path is unique in the layer, and is the path that its parent's path and its own name make. A name is
//   one that the specification's grammar allows its form.
// - A spec's lists of children hold exactly the specs whose parent it is, each once, in the order of the file.
// - A spec's fields have distinct names, and none is one of the fields that list children, which the lists replace.
// - Every value has a type of PrUsdType, and `data` holds `count` elements of the C type that the type's comment
//   gives. An enumeration holds one of its members.
// - Every string is valid for the life of the layer. A path is in the specification's text form, is absolute unless
//   its comment says otherwise, and parses by the path grammar.
// - The times of time samples are numbers, which an infinity is and a NaN is not, and increase strictly. The keys of
//   a dictionary, and of a map of variant selections, are distinct and sorted by their bytes.
// - The items of each list of a list op are distinct, as the specification requires.
// - An attribute's `default` and each of its time samples is a block or has the type that the attribute's `typeName`
//   names, where the layer knows that type name.
// - Every prim and variant has a `specifier`, every property a `custom`, and every attribute a `typeName` and a
//   `variability`, which are the fields that the specification requires. Each has the type that the specification
//   gives it: a PrUsdSpecifier, a bool, a token and a PrUsdVariability.
//
// A layer does not check what composition and schemas decide: that a path names a spec, that an asset path names a
// file, that a prim's type or a field is one that a schema knows, that a quaternion or a normal has unit length, that a
// number is finite, or that a time scale is positive.
//
// A layer is immutable, so any number of threads may read it.
//
// USD Core Specification v1.1.0 (e8f44c0), core/1.1.0/core_spec.md of https://github.com/aousd/specifications-public

#pragma once

#include "pr/base.h"
#include "pr/blob.h"
#include "pr/byte_view.h"
#include "pr/compress.h"
#include "pr/status.h"
#include "pr/string_view.h"
#include "pr/text_buffer.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Values

// The type of a value's elements. Each comment gives the C type of one element. A type of the foundational data types
// is named as the specification names it, without the semantic aliases, which only an attribute's `typeName` carries.
enum {
    PrUsdType_Block,            // no element: the value block, which says that a value is authored as absent
    PrUsdType_Bool,             // bool
    PrUsdType_UChar,            // uint8_t
    PrUsdType_Int,              // int32_t
    PrUsdType_UInt,             // uint32_t
    PrUsdType_Int64,            // int64_t
    PrUsdType_UInt64,           // uint64_t
    PrUsdType_Half,             // PrFloat16
    PrUsdType_Float,            // float
    PrUsdType_Double,           // double
    PrUsdType_TimeCode,         // double
    PrUsdType_String,           // PrStringView
    PrUsdType_Token,            // PrStringView
    PrUsdType_Asset,            // PrStringView: the asset path as authored
    PrUsdType_Path,             // PrStringView: a path, or the empty view for no path
    PrUsdType_PathExpression,   // PrStringView: the expression's text
    PrUsdType_Int2,             // PrIVec2
    PrUsdType_Int3,             // PrIVec3
    PrUsdType_Int4,             // PrIVec4
    PrUsdType_Half2,            // PrFloat16[2]
    PrUsdType_Half3,            // PrFloat16[3]
    PrUsdType_Half4,            // PrFloat16[4]
    PrUsdType_Float2,           // PrVec2
    PrUsdType_Float3,           // PrVec3
    PrUsdType_Float4,           // PrVec4
    PrUsdType_Double2,          // PrDVec2
    PrUsdType_Double3,          // PrDVec3
    PrUsdType_Double4,          // PrDVec4
    PrUsdType_Quath,            // PrFloat16[4]: the imaginary parts, then the real part, as PrQuat orders them
    PrUsdType_Quatf,            // PrQuat
    PrUsdType_Quatd,            // PrDQuat
    PrUsdType_Matrix2d,         // PrDMatrix2
    PrUsdType_Matrix3d,         // PrDMatrix3
    PrUsdType_Matrix4d,         // PrDMatrix4
    PrUsdType_Dictionary,       // PrUsdDictionary
    PrUsdType_ListOp,           // PrUsdListOp
    PrUsdType_TimeSamples,      // PrUsdTimeSamples
    PrUsdType_Specifier,        // PrUsdSpecifier
    PrUsdType_Variability,      // PrUsdVariability
    PrUsdType_Permission,       // PrUsdPermission
    PrUsdType_LayerOffset,      // PrUsdLayerOffset
    PrUsdType_Reference,        // PrUsdReference
    PrUsdType_Payload,          // PrUsdReference, without custom data
    PrUsdType_Relocate,         // PrUsdRelocate
    PrUsdType_VariantSelection, // PrUsdVariantSelection
    PrUsdType_Unregistered,     // PrStringView: the text of a field that the layer knows no type for
    PrUsdType_Count,
};
typedef uint8_t PrUsdType;

// A value of a field, of a time sample or of a dictionary's entry. A value that is not an array has one element, or
// none if it is a block. An array has `count` elements, which may be none. `data` is aligned for its element type.
//
// A matrix is the transpose of the rows that the file holds. USD writes a matrix as rows and multiplies a row vector by
// it, and a matrix of lib/geom maps column vectors, so the transpose is the same transform.
typedef struct PrUsdValue {
    PrUsdType type;
    bool isArray;
    uint32_t count;
    const void* PR_NULLABLE data;
} PrUsdValue;

// Returns the specification's name of a type, as `float3` or `matrix4d`, which is how a text layer names the type of an
// attribute and of a dictionary's entry. A type that only a field holds has a name of the same style, as `listOp`.
PrStringView prUsdTypeName(PrUsdType type);

// Returns the size in bytes of one element of a type, which is zero for a block.
uint32_t prUsdTypeSize(PrUsdType type);

// Reports the type that an attribute's `typeName` names: a foundational type or a semantic alias of one, with `[]` for
// an array. Returns false if the name is neither, as it is for `opaque`, `group` and a name that is not the
// specification's.
bool prUsdTypeFromName(PrStringView typeName, PrUsdType* type, bool* isArray);

// An entry of a dictionary.
typedef struct PrUsdDictionaryEntry {
    PrStringView key;
    PrUsdValue value;
} PrUsdDictionaryEntry;

// A map from strings to values of any foundational type, to arrays of them, and to dictionaries
// (usd-core/#dictionaries). The entries are sorted by the bytes of their keys.
typedef struct PrUsdDictionary {
    uint32_t count;
    const PrUsdDictionaryEntry* PR_NULLABLE entries;
} PrUsdDictionary;

// The lists of a list op, in the order of PrUsdListOp::items.
enum {
    PrUsdListOpList_Explicit,
    PrUsdListOpList_Added,              // deprecated in the specification, and kept as authored
    PrUsdListOpList_Prepended,
    PrUsdListOpList_Appended,
    PrUsdListOpList_Deleted,
    PrUsdListOpList_Ordered,            // deprecated in the specification, and kept as authored
    PrUsdListOpList_Count,
};
typedef uint8_t PrUsdListOpList;

// A list op as authored (usd-core/#list-operations): the lists that edit a weaker list, or the explicit list that
// replaces it. Nothing is combined or reduced. Each list is an array whose elements have the type `itemType`: an
// integer type, String, Token, Path, Reference, Payload or Unregistered. An explicit list op may have no items, which
// differs from a list op that edits nothing.
typedef struct PrUsdListOp {
    PrUsdType itemType;
    bool isExplicit;
    PrUsdValue items[PrUsdListOpList_Count]; // by PrUsdListOpList
} PrUsdListOp;

// The values of an attribute at times (usd-core/#timesamples-timesamples-specialized-type). `times` increase strictly.
// A value is a block, or has the attribute's type.
typedef struct PrUsdTimeSamples {
    uint32_t count;
    const double* PR_NULLABLE times;
    const PrUsdValue* PR_NULLABLE values;
} PrUsdTimeSamples;

// Whether a prim spec defines its prim (usd-core/#specifier-enumspecifier-specialized-type).
enum {
    PrUsdSpecifier_Def,
    PrUsdSpecifier_Over,
    PrUsdSpecifier_Class,
};
typedef uint8_t PrUsdSpecifier;

// Whether an attribute's value may vary over time.
enum {
    PrUsdVariability_Varying,
    PrUsdVariability_Uniform,
};
typedef uint8_t PrUsdVariability;

// A deprecated field's values, which a layer keeps as authored.
enum {
    PrUsdPermission_Public,
    PrUsdPermission_Private,
};
typedef uint8_t PrUsdPermission;

// The retiming of a sublayer, a reference or a payload: a time of the layer that is brought in, scaled and then offset,
// is a time of the layer that brings it in.
typedef struct PrUsdLayerOffset {
    double offset;
    double scale;
} PrUsdLayerOffset;

// A reference or a payload (usd-core/#references-listopreference-specialized-type). `assetPath` is empty for a target
// in the same layer stack, and `primPath` is empty for the target layer's default prim. `customData` is null for a
// payload, and for a reference without custom data.
typedef struct PrUsdReference {
    PrStringView assetPath;
    PrStringView primPath;
    PrUsdLayerOffset layerOffset;
    const PrUsdDictionary* PR_NULLABLE customData;
} PrUsdReference;

// An entry of a layer's relocates: the prim at `source` is moved to `target`, or removed if `target` is empty.
typedef struct PrUsdRelocate {
    PrStringView source;
    PrStringView target;
} PrUsdRelocate;

// The variant that a prim selects of a variant set. An array of them is sorted by the bytes of `variantSet`, and names
// each variant set once.
typedef struct PrUsdVariantSelection {
    PrStringView variantSet;
    PrStringView variant;       // empty to select none
} PrUsdVariantSelection;

// ---------------------------------------------------------------------------------------------------------------------
// Layers

// A layer, read and checked.
typedef struct PrUsdLayer PrUsdLayer;

// A spec of a layer, by its position in the layer's array of specs.
typedef uint32_t PrUsdSpecIndex;

// The index of a spec that does not exist.
PR_CONSTANT uint32_t kPrUsdNone = 0xffffffffu;

// The layer spec's index.
enum { kPrUsdLayerSpec = 0 };

// The format that a layer was read from.
enum {
    PrUsdFormat_Text,           // .usda
    PrUsdFormat_Crate,          // .usdc
    PrUsdFormat_MaterialX,      // .mtlx: a MaterialX document, translated as OpenUSD's UsdMtlx translates one
};
typedef uint8_t PrUsdFormat;

// The services that reading uses.
typedef struct PrUsdOptions {
    // Decodes the LZ4 blocks of a crate file. Without it, a crate file is refused as NotSupported.
    const PrCompressCodecSet* PR_NULLABLE codec;

    // Receives a sentence that says what in the file failed which check, and where, if reading fails.
    PrTextBuffer* PR_NULLABLE diagnostics;
} PrUsdOptions;

// Returns options with no services.
PrUsdOptions prUsdOptionsDefault(void);

// Reads the layer in `file`, which is text, crate or a MaterialX document as its first bytes say: a file's extension is
// not consulted, so a .usd file is read as whichever it holds. A MaterialX document, which begins with `<`, is
// translated into the layer that OpenUSD's UsdMtlx makes of it, with its materials under /MaterialX/Materials. The
// layer keeps a reference to `file` if it is a crate file, whose arrays it reads in place, and otherwise keeps nothing
// of it. The options are used only during the call, and null options mean the defaults.
//
// Returns Syntax for text that the grammar does not accept, InvalidData for a file that breaks a rule of the
// specification, and NotSupported for a version of a format that the module does not read, for a package, which
// prUsdPackageCreate reads, and for a crate file without a codec. On failure, `*out` is null.
PrStatus prUsdLayerCreate(PrBlob* file, const PrUsdOptions* PR_NULLABLE options, PrUsdLayer* PR_NULLABLE* PR_NONNULL out);

// Frees the layer, and releases the blob that it holds.
void prUsdLayerDestroy(PrUsdLayer* PR_NULLABLE layer);

// Returns the format that the layer was read from.
PrUsdFormat prUsdLayerFormat(const PrUsdLayer* layer);

// Returns the version that the file states of its format, as "1.0" for a text layer and "0.8.0" for a crate layer. The
// string is valid until the layer is destroyed, as is every string and every array that a layer returns.
PrStringView prUsdLayerVersion(const PrUsdLayer* layer);

// Returns true if the module reads a crate file of this version: 0.4.0 to 0.12.0. Earlier versions lay the file out
// differently, and the specification asks no reader for them.
bool prUsdCrateVersionIsSupported(uint8_t major, uint8_t minor, uint8_t patch);

// ---------------------------------------------------------------------------------------------------------------------
// Specs (usd-core/#specs)

// The forms of a spec.
enum {
    PrUsdSpecForm_Layer,
    PrUsdSpecForm_Prim,
    PrUsdSpecForm_Attribute,
    PrUsdSpecForm_Relationship,
    PrUsdSpecForm_VariantSet,
    PrUsdSpecForm_Variant,
};
typedef uint8_t PrUsdSpecForm;

// A list of specs that a spec holds: `count` indices from `first` in the layer's storage for lists, which
// prUsdLayerList returns.
typedef struct PrUsdList {
    uint32_t first;
    uint32_t count;
} PrUsdList;

// A field of a spec: a name and its value (usd-core/#metadata-fields).
typedef struct PrUsdField {
    PrStringView name;
    PrUsdValue value;
} PrUsdField;

// A spec. Its children are in the order of the file, which is the order of the fields that the specification lists them
// in: `primChildren`, `propertyChildren`, `variantSetChildren` and `variantChildren`.
typedef struct PrUsdSpec {
    PrUsdSpecForm form;
    PrUsdSpecIndex parent;      // kPrUsdNone for the layer spec
    PrStringView name;          // empty for the layer spec
    PrStringView path;          // as "/A/B", "/A/B.size", "/A{shape=}" for a variant set and "/A{shape=round}"

    PrUsdList prims;            // of the layer spec, a prim or a variant
    PrUsdList properties;       // of a prim or a variant
    PrUsdList variantSets;      // of a prim or a variant
    PrUsdList variants;         // of a variant set

    // The spec's fields, sorted by the bytes of their names.
    uint32_t fieldCount;
    const PrUsdField* PR_NULLABLE fields;
} PrUsdSpec;

// Return the number of specs and the specs.
uint32_t prUsdLayerSpecCount(const PrUsdLayer* layer);
const PrUsdSpec* prUsdLayerSpecs(const PrUsdLayer* layer);

// Returns the indices of `list`: `list.count` of them.
const PrUsdSpecIndex* PR_NULLABLE prUsdLayerList(const PrUsdLayer* layer, PrUsdList list);

// Returns the spec at `path`, which is in the form of PrUsdSpec::path, or kPrUsdNone if the layer has none there.
PrUsdSpecIndex prUsdLayerFindSpec(const PrUsdLayer* layer, PrStringView path);

// Returns the child of `parent` that has the form and the name, or kPrUsdNone. A property is found by either of its
// forms.
PrUsdSpecIndex prUsdLayerFindChild(const PrUsdLayer* layer, PrUsdSpecIndex parent, PrUsdSpecForm form, PrStringView name);

// Returns the field of `spec` named `name`, or null if the spec does not author it. An absent field has the fallback
// that the specification gives it, which a consumer applies.
const PrUsdField* PR_NULLABLE prUsdLayerFindField(const PrUsdLayer* layer, PrUsdSpecIndex spec, PrStringView name);

// The bytes of a text layer's file that a spec was read from, as offsets into the file.
typedef struct PrUsdTextSpan {
    uint32_t begin;
    uint32_t end;               // one past the last byte
} PrUsdTextSpan;

// Returns the span of `spec`'s text: a prim's from its specifier through its closing brace, a property's from its first
// word through its value or its metadata, a variant set's from `variantSet` through its closing brace, a variant's from
// its name through its closing brace, and the whole file for the layer spec. A property that the layer declares twice
// has the text of its last declaration. The span is empty, at 0, for every spec of a crate layer, and for a variant set
// that only a prim's metadata declares.
PrUsdTextSpan prUsdLayerSpecSpan(const PrUsdLayer* layer, PrUsdSpecIndex spec);

// Returns the innermost spec whose text holds the byte at `offset`, or kPrUsdNone if no spec's text holds it, as for
// any offset of a crate layer. The lookup costs a binary search over the specs and a step up for each level of nesting.
PrUsdSpecIndex prUsdLayerSpecFromOffset(const PrUsdLayer* layer, uint32_t offset);

// ---------------------------------------------------------------------------------------------------------------------
// Sublayers (usd-core/#sublayers-asset)

// A sublayer of a layer: the asset path as authored, and its retiming, which is the identity if the layer gives none.
typedef struct PrUsdSublayer {
    PrStringView assetPath;
    PrUsdLayerOffset layerOffset;
} PrUsdSublayer;

// Return the number of the layer's sublayers and the sublayers, strongest first. They are the layer spec's `subLayers`
// and `subLayerOffsets` fields, matched by position.
uint32_t prUsdLayerSublayerCount(const PrUsdLayer* layer);
const PrUsdSublayer* PR_NULLABLE prUsdLayerSublayers(const PrUsdLayer* layer);

// ---------------------------------------------------------------------------------------------------------------------
// Packages (usd-core/#package)

// A package: a ZIP archive whose files are stored as they are, so that each is read in place.
typedef struct PrUsdPackage PrUsdPackage;

// A file of a package. `bytes` is valid until the package is destroyed.
typedef struct PrUsdPackageFile {
    PrStringView path;          // as the archive names it, with '/' separators
    PrByteView bytes;
} PrUsdPackageFile;

// Reads the package in `file`, and keeps a reference to it. The package's first file is its default layer.
//
// Returns InvalidData for a file that is not a ZIP archive, for an archive with no files, and for a file whose bytes
// lie outside the archive, and NotSupported for an archive that compresses or encrypts a file, which the specification
// forbids. A package whose layout breaks the specification's other rules, as the alignment of its files to 64 bytes, is
// read, and prUsdPackageIsConforming reports it. On failure, `*out` is null.
PrStatus prUsdPackageCreate(PrBlob* file, const PrUsdOptions* PR_NULLABLE options, PrUsdPackage* PR_NULLABLE* PR_NONNULL out);

// Frees the package. The blobs that prUsdPackageOpen returned stay valid.
void prUsdPackageDestroy(PrUsdPackage* PR_NULLABLE package);

// Return the number of files and the file at `index`, in the order of the archive's directory.
uint32_t prUsdPackageFileCount(const PrUsdPackage* package);
PrUsdPackageFile prUsdPackageFile(const PrUsdPackage* package, uint32_t index);

// Returns the index of the file at `path`, compared byte for byte, or kPrUsdNone.
uint32_t prUsdPackageFind(const PrUsdPackage* package, PrStringView path);

// Returns the bytes of a file as a blob, which the caller releases. The blob shares the package's bytes and keeps them
// alive, so it is what prUsdLayerCreate takes for a layer in the package.
PrBlob* prUsdPackageOpen(const PrUsdPackage* package, uint32_t index);

// Returns true if the package keeps the rules of the specification that reading does not need: each file's bytes begin
// at a multiple of 64 bytes, the archive is not ZIP64, and it ends with its directory's end record, without a comment.
bool prUsdPackageIsConforming(const PrUsdPackage* package);

// ---------------------------------------------------------------------------------------------------------------------
// Assets (usd-core/#resource-interface)

// Returns a provider of the bytes of the resource at `location`, which prUsdAssetResolve made, or null if the host has
// no such resource. It is called on the thread that composes a stage or reads a layer, and the provider that it returns
// may be read on any thread.
typedef const PrBlobSource* PR_NULLABLE (*PrUsdOpenFn)(void* PR_NULLABLE context, PrStringView location);

// Returns a provider of the file at `location`, a path of the file system: the opener of a host that keeps its layers
// in files.
const PrBlobSource* PR_NULLABLE prUsdFileOpen(void* PR_NULLABLE context, PrStringView location);

// Resolves an asset path that the layer at `anchor` authors to a location. A path that begins with `./` or `../` is
// anchored, and resolves against the folder of `anchor` alone (usd-core/#anchored-relative-identifiers). Any other
// relative path resolves against that folder too, since the module keeps no search path. A relative anchor's location
// is relative too, and keeps each `..` that leaves its folder. An absolute path, and a path with a URI scheme, is its
// own location. A path inside a package resolves inside that package, and the location names the file as
// `package.usdz[folder/file]`. The caller releases the location with prRawStringReset.
PrRawString prUsdAssetResolve(PrStringView anchor, PrStringView assetPath);

// Opens a provider of the resource at `location` with `open`, as a texture that a layer names, into `*out`, which the
// caller releases: the resource itself, or the file inside the packages that the location names, as
// `scene.usdz[textures/wood.png]`. The file's provider reads the range of its package that holds it from the package's
// provider, which may itself be a file of an outer package, so nested packages compose. Its first read finds the file
// in the package's directory, which it reads the package for, and the reads after it read the file's range alone. A
// null `open` is prUsdFileOpen. Returns Io if the outermost resource cannot be opened. A read of the provider fails as
// InvalidData if a package has no file of the name, and otherwise with the status of prUsdPackageCreate. On failure,
// `*out` is null.
PrStatus prUsdResourceSource(PrStringView location, PrUsdOpenFn PR_NULLABLE open, void* PR_NULLABLE openContext,
    const PrUsdOptions* PR_NULLABLE options, const PrBlobSource* PR_NULLABLE* PR_NONNULL out);

// Reads the layer at `location` with `open`, into a layer at `*out`, and waits for its bytes. A location that names a
// file in a package reads the package and then the file. A location that names a package reads its default layer. A
// null `open` is prUsdFileOpen.
//
// Returns Io if a resource cannot be read, InvalidData if a package has no file of the name, and otherwise the status
// of prUsdLayerCreate or prUsdPackageCreate. On failure, `*out` is null.
PrStatus prUsdLayerRead(PrStringView location, PrUsdOpenFn PR_NULLABLE open, void* PR_NULLABLE openContext,
    const PrUsdOptions* PR_NULLABLE options, PrUsdLayer* PR_NULLABLE* PR_NONNULL out);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
