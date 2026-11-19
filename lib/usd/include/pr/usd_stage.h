// OpenUSD stages: the prims that a root layer composes, and the values that their opinions resolve to, as the USD Core
// Specification's composition, stage population and value resolution describe them.
//
// A stage is composed once, by prUsdStageCreate, and is immutable afterwards, so any number of threads may read it,
// each with a scratch of its own. To compose with other options, such as other payloads or variant fallbacks, make
// another stage: composition has no state that a change could update in place, and a stage's cost is its composition.
//
// What a stage guarantees. Layers are data from outside the program, and so is everything that they tell composition to
// do. prUsdStageCreate checks it as it composes, so a consumer follows a stage without checking it again:
//
// - Composition ends. A sublayer, an arc or a relocate that would make it not end is a composition error: a layer that
//   is its own sublayer, an arc to a site that the arc's site comes from, an arc nested deeper than a fixed bound.
// - A composition error is not fatal. What is wrong is ignored, as the specification has it, and recorded in the
//   stage's errors. Only a root layer that cannot be read fails prUsdStageCreate.
// - Every index that a stage gives is in range: a prim's parent, children, end, prototype, opinions and properties, and
//   an opinion's layer and spec. An opinion's layer was read, and its spec is a prim spec of that layer.
// - The prims are in depth-first order, a parent before its children, with each prim's children in their composed
//   order. The prims of the prototypes follow the pseudo-root's descendants.
// - A prim's opinions are in strength order, strongest first, and the scale of a retiming is positive.
// - A value that a stage resolves keeps the guarantees of the layer values that it is made of.
//
// A stage knows no schema: a prim's type and its applied schemas are tokens, and a property that only a schema would
// give a prim is not one of its properties.
//
// USD Core Specification v1.1.0 (e8f44c0), core/1.1.0/core_spec.md of https://github.com/aousd/specifications-public

#pragma once

#include "pr/usd.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// A stage: the prims that a root layer composes, with the layers that composition reached.
typedef struct PrUsdStage PrUsdStage;

// A prim of a stage, by its position in the stage's array of prims.
typedef uint32_t PrUsdPrimIndex;

// The index of the pseudo-root, the prim at `/` that holds the root prims.
enum { kPrUsdPseudoRoot = 0 };

// The arcs of composition, strongest first (usd-core/#strength-ordering). `Root` is no arc: it is the prim's own site
// in the root layer stack.
enum {
    PrUsdArc_Root,
    PrUsdArc_Inherit,
    PrUsdArc_Variant,
    PrUsdArc_Relocate,
    PrUsdArc_Reference,
    PrUsdArc_Payload,
    PrUsdArc_Specialize,
};
typedef uint8_t PrUsdArc;

// Which payloads a stage composes (usd-core/#payloads).
enum {
    PrUsdLoad_All,
    PrUsdLoad_None,             // none but those of PrUsdStageOptions::loadPaths
};
typedef uint8_t PrUsdLoad;

// The causes of a composition error (usd-core/#composition-errors).
enum {
    PrUsdErrorKind_SublayerUnreadable,      // a sublayer cannot be read, and is left out
    PrUsdErrorKind_SublayerCycle,           // a layer is a sublayer of itself, and is left out there
    PrUsdErrorKind_InvalidOffset,           // a retiming whose scale is not positive, and is taken as the identity
    PrUsdErrorKind_InvalidRelocate,         // a relocate that breaks a rule, and is ignored
    PrUsdErrorKind_OpinionAtRelocateSource, // a spec at the source of a relocate, and is ignored
    PrUsdErrorKind_ArcCycle,                // an arc to a site that the arc's own site comes from, and is ignored
    PrUsdErrorKind_ArcUnreadable,           // a reference or a payload to a layer that cannot be read
    PrUsdErrorKind_ArcUnresolved,           // a reference or a payload to a prim that no layer has a spec for
    PrUsdErrorKind_ArcInvalidPath,          // an arc to a path that is no prim's, or to a layer without a default prim
    PrUsdErrorKind_InconsistentProperty,    // the specs of a property disagree on its form, type or variability
    PrUsdErrorKind_Count,
};
typedef uint8_t PrUsdErrorKind;

// A composition error. Composition does not stop at one: it ignores what is wrong and records it.
typedef struct PrUsdError {
    PrUsdErrorKind kind;
    PrStringView prim;          // the path of the prim whose composition found the error, or empty for a layer stack's
    PrStringView message;       // a sentence that says what is wrong, and where
} PrUsdError;

// A variant that a stage's session selects for one prim: the strongest opinion of the prim's selection of the set, as a
// session layer above the root layer would author it (usd-core/#computing-variant-selection).
typedef struct PrUsdPrimVariantSelection {
    PrStringView prim;          // the prim's path in the stage's namespace, as `/World/Car`
    PrStringView variantSet;
    PrStringView variant;       // empty to select none
} PrUsdPrimVariantSelection;

// How a stage is composed.
typedef struct PrUsdStageOptions {
    // The services that reading each layer uses.
    PrUsdOptions layer;

    // Opens the resources of the stage's layers. Null is prUsdFileOpen.
    PrUsdOpenFn PR_NULLABLE open;
    void* PR_NULLABLE openContext;

    // The payloads to compose. With PrUsdLoad_None, a prim composes its payloads if it is one of `loadPaths`, or an
    // ancestor or a descendant of one.
    PrUsdLoad load;
    uint32_t loadPathCount;
    const PrStringView* PR_NULLABLE loadPaths;

    // The variants that a variant set without a selection takes, in order of preference where a set is named twice.
    uint32_t variantFallbackCount;
    const PrUsdVariantSelection* PR_NULLABLE variantFallbacks;

    // The session's selections, which are stronger than every layer's and than the fallbacks. A view's selections are
    // the session's: they are a stage's composition, and a stage composed with others is another stage. A selection of
    // a prim that is no prim, or of a set that the prim does not list, selects nothing.
    uint32_t variantSelectionCount;
    const PrUsdPrimVariantSelection* PR_NULLABLE variantSelections;

    // Whether an instance composes its own children, each from every opinion, instead of sharing those of a prototype.
    bool expandInstances;
} PrUsdStageOptions;

// Returns options that load every payload, with no services.
PrUsdStageOptions prUsdStageOptionsDefault(void);

// Composes the stage whose root layer is at `location`, reading every layer that composition reaches through the reader
// of the options, each once. The options are used only during the call, and null options are the defaults.
//
// Fails only if the root layer cannot be read, with the status of prUsdLayerRead, and then `*out` is null. Everything
// else that is wrong is a composition error, which the stage records and composes around.
PrStatus prUsdStageCreate(PrStringView location, const PrUsdStageOptions* PR_NULLABLE options,
    PrUsdStage* PR_NULLABLE* PR_NONNULL out);

// Frees the stage and its layers.
void prUsdStageDestroy(PrUsdStage* PR_NULLABLE stage);

// Return the number of composition errors and the errors, in the order that composition found them.
uint32_t prUsdStageErrorCount(const PrUsdStage* stage);
const PrUsdError* PR_NULLABLE prUsdStageErrors(const PrUsdStage* stage);

// Return the number of layers that composition reached, and one of them with its location. Layer 0 is the root layer. A
// layer is null if its location could not be read, which a composition error reports.
uint32_t prUsdStageLayerCount(const PrUsdStage* stage);
const PrUsdLayer* PR_NULLABLE prUsdStageLayer(const PrUsdStage* stage, uint32_t layer);
PrStringView prUsdStageLayerLocation(const PrUsdStage* stage, uint32_t layer);

// The flags of a prim (usd-core/#stage-queries), which PrUsdPrim::flags combines.
enum {
    PrUsdPrimFlag_Active = 1 << 0,          // `active` is not false on the prim or an ancestor
    PrUsdPrimFlag_Loaded = 1 << 1,          // no payload of the prim or of an ancestor is left out
    PrUsdPrimFlag_Defined = 1 << 2,         // the prim and its ancestors are `def` or `class`
    PrUsdPrimFlag_Abstract = 1 << 3,        // the prim or an ancestor is `class`
    PrUsdPrimFlag_Instance = 1 << 4,        // the prim's children are those of PrUsdPrim::prototype
    PrUsdPrimFlag_InPrototype = 1 << 5,     // the prim is a prototype or under one
    PrUsdPrimFlag_HasPayload = 1 << 6,      // an opinion of the prim authors a payload, loaded or not
};
typedef uint8_t PrUsdPrimFlags;

// A composed prim. A stage's prims are in depth-first order with children in their composed order
// (usd-core/#ordered-prim-children), so the prims from `index + 1` to `end` are a prim's descendants. The pseudo-root
// comes first, and the prototypes follow its descendants, each the root of a tree of its own.
typedef struct PrUsdPrim {
    PrStringView name;          // empty for the pseudo-root
    PrStringView typeName;      // the strongest opinion, or empty
    PrUsdPrimIndex parent;      // kPrUsdNone for the pseudo-root and for a prototype
    PrUsdPrimIndex firstChild;  // kPrUsdNone for a prim without children, and for an instance
    PrUsdPrimIndex nextSibling;
    PrUsdPrimIndex end;         // one past the prim's last descendant
    PrUsdPrimIndex prototype;   // of an instance: the prototype that holds its children, and otherwise kPrUsdNone
    uint32_t firstOpinion;      // the prim's opinions, strongest first, in the stage's array of opinions
    uint32_t opinionCount;
    uint32_t firstProperty;     // the prim's properties, in their composed order, in the stage's array of properties
    uint32_t propertyCount;
    PrUsdSpecifier specifier;
    PrUsdPrimFlags flags;
} PrUsdPrim;

// Return the number of prims and the prims.
uint32_t prUsdStagePrimCount(const PrUsdStage* stage);
const PrUsdPrim* prUsdStagePrims(const PrUsdStage* stage);

// Return the number of prototypes, and the index of the root prim of one. An instance and the prims of its prototype
// share no ancestry: a prototype's prims take their opinions from the arcs of one of its instances
// (usd-core/#scene-graph-instancing).
uint32_t prUsdStagePrototypeCount(const PrUsdStage* stage);
PrUsdPrimIndex prUsdStagePrototype(const PrUsdStage* stage, uint32_t prototype);

// Returns the prim at `path`, as `/World/Chair` or `/__Prototype_1/Geom`, or kPrUsdNone. The path of a prim under an
// instance finds nothing: the prim is in the instance's prototype.
PrUsdPrimIndex prUsdStageFindPrim(const PrUsdStage* stage, PrStringView path);

// Appends the path of a prim to `out`.
void prUsdStagePrimPath(const PrUsdStage* stage, PrUsdPrimIndex prim, PrTextBuffer* out);

// An opinion of a prim: a prim spec of a layer, with the arc that brought it and the retiming from the layer's times to
// the stage's, which scales a time of the layer and then offsets it.
typedef struct PrUsdOpinion {
    uint32_t layer;             // of prUsdStageLayer
    PrUsdSpecIndex spec;
    PrUsdArc arc;               // the arc of the opinion's site: the last arc on the way from the prim to it
    PrUsdLayerOffset offset;
} PrUsdOpinion;

// Returns the opinion at `index` of the stage's array of opinions.
PrUsdOpinion prUsdStageOpinion(const PrUsdStage* stage, uint32_t index);

// A composed property of a prim.
typedef struct PrUsdProperty {
    PrStringView name;
    PrUsdSpecForm form;         // of the strongest spec: PrUsdSpecForm_Attribute or PrUsdSpecForm_Relationship
} PrUsdProperty;

// Returns the stage's array of properties, which PrUsdPrim::firstProperty indexes. A prim's properties are those of its
// strongest `propertyOrder`, in its order, and then the others in the order of their names
// (usd-core/#ordered-property-children).
const PrUsdProperty* PR_NULLABLE prUsdStageProperties(const PrUsdStage* stage);

// Returns the index of the property of `prim` named `name` in the stage's array of properties, or kPrUsdNone. The
// search is by the names' bytes, in steps that grow with the logarithm of the prim's properties.
uint32_t prUsdStageFindProperty(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name);

// Lists the properties of `prim` whose names begin with `prefix`, as those of a namespace do: `primvars:`, `inputs:`,
// `xformOp:`. Returns their number, and sets `*indices` to their indices in the stage's array of properties, in the
// order of their names' bytes. The array is the stage's. The search is as prUsdStageFindProperty's, and an empty prefix
// lists every property of the prim.
uint32_t prUsdStagePropertiesWithPrefix(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView prefix,
    const uint32_t* PR_NULLABLE* PR_NONNULL indices);

// Writes the opinions of a property of a prim, strongest first, to `out`, up to `capacity` of them: the property's spec
// in each of the prim's opinions that has one. Returns the number that the property has.
uint32_t prUsdStagePropertyOpinions(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property,
    PrUsdOpinion* PR_NULLABLE out, uint32_t capacity);

// The storage of the values that value resolution makes: combined dictionaries and list ops, interpolated samples,
// mapped paths. A value that a stage returns through a scratch is valid until the scratch is cleared or destroyed. A
// stage is immutable, and each thread that reads it uses a scratch of its own.
typedef struct PrUsdScratch PrUsdScratch;

// Creates an empty scratch.
PrUsdScratch* prUsdScratchCreate(void);

// Invalidates the values that the scratch holds, and keeps its storage for new values.
void prUsdScratchClear(PrUsdScratch* scratch);

// Frees the scratch and the values that it holds.
void prUsdScratchDestroy(PrUsdScratch* PR_NULLABLE scratch);

// Returns `size` bytes of the scratch, zeroed, at a multiple of `alignment`, which is a power of two, for a value that
// a reader of the stage makes beside the ones that the stage resolves, as a conversion of their type. The bytes are
// valid until the scratch is cleared or destroyed.
void* prUsdScratchAlloc(PrUsdScratch* scratch, size_t size, size_t alignment);

// Resolves a field of the root layer's layer spec, which is the stage's metadata: `upAxis`, `metersPerUnit`,
// `timeCodesPerSecond`, `startTimeCode`, `endTimeCode`, `defaultPrim` and the rest (usd-core/#layer-metadata). Returns
// false if the root layer does not author the field.
bool prUsdStageField(const PrUsdStage* stage, PrStringView name, PrUsdValue* out);

// Returns the stage's time codes per second: the root layer's `timeCodesPerSecond`, else its `framesPerSecond`, else 24
// (usd-core/#effective-time-codes-per-second).
double prUsdStageTimeCodesPerSecond(const PrUsdStage* stage);

// Resolves a metadata field of a prim (usd-core/#metadata-resolution): the strongest opinion, with dictionaries
// combined across opinions, and a list op as the explicit list op of the list that the opinions' list ops make.
// `specifier` and `typeName` resolve by their own rules. Returns false if no opinion authors the field: a fallback is
// the consumer's to apply.
bool prUsdStagePrimField(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, PrUsdScratch* scratch,
    PrUsdValue* out);

// Resolves a metadata field of a property, as prUsdStagePrimField does. `custom` is true if any opinion is, and
// `variability` is the weakest opinion (usd-core/#variability). `property` is an index of the stage's array of
// properties.
bool prUsdStagePropertyField(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, PrStringView name,
    PrUsdScratch* scratch, PrUsdValue* out);

// Resolves an attribute's value (usd-core/#attribute-resolution). `time` is a time code of the stage, or a NaN for the
// default value. At a time, the strongest opinion with time samples or a default decides: samples are read at the time
// that the opinion's retiming maps the stage's time to, interpolated linearly between two samples of a type that
// interpolates and held otherwise (usd-core/#interpolation), and a default is taken as it is. A value of type
// `timecode` is a time of its layer, and resolves to the stage's. Returns false if no opinion has a value, or if the
// deciding value is a block.
//
// A value that no rule had to make points into its layer, which for an array of a crate file is the file's own bytes
// where the file stores them uncompressed: `count` is the array's length, and nothing is copied.
bool prUsdStageAttributeValue(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
    PrUsdScratch* scratch, PrUsdValue* out);

// Resolves an attribute's value as prUsdStageAttributeValue does, into the caller's storage: up to `capacity` elements
// of `type` at `out`. Returns false if there is no value, if the value is not of `type`, or if it holds more than
// `capacity` elements. `*count` is the number of elements that the value holds wherever it has that type.
bool prUsdStageAttributeRead(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
    PrUsdType type, void* PR_NULLABLE out, uint32_t capacity, uint32_t* count);

// Lists the times, as time codes of the stage, of the samples that decide an attribute's value from `start` to `end`
// inclusive: those of the strongest opinion that has samples. Returns their number.
uint32_t prUsdStageAttributeTimes(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double start,
    double end, PrUsdScratch* scratch, const double* PR_NULLABLE* PR_NONNULL times);

// Resolves the targets of a relationship or the connections of an attribute: the list that the opinions' list ops make,
// each path mapped into the stage's namespace, as an array of PrUsdType_Path. With `forward`, a target that is a
// relationship is replaced by that relationship's targets, in turn (usd-core/#relationships-and-attribute-connections).
// Returns false if no opinion authors targets.
//
// A path that its site's arcs map to nothing names nothing of the stage, and is left out. `*invalid` is the number of
// them, which a stage that is well made has none of.
bool prUsdStageTargets(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, bool forward,
    PrUsdScratch* scratch, PrUsdValue* out, uint32_t* PR_NULLABLE invalid);

// Lists the variant sets of a prim (usd-core/#variants) as an array of PrUsdType_String: the names that each site's
// `variantSetNames` make across the layers of its stack, strongest site first, each name once. Returns false for a prim
// without variant sets.
bool prUsdStagePrimVariantSets(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch, PrUsdValue* out);

// Returns the variant of `variantSet` that composing the prim selected, or the empty view if it selected none.
PrStringView prUsdStagePrimVariantSelection(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView variantSet);

// Resolves an asset path that an attribute's value holds to a location, against the layer that authors the value which
// decides the attribute at `time`, as prUsdAssetResolve does. An asset path is relative to the layer that wrote it,
// which is not the stage's root layer. The caller releases the location with prRawStringReset.
PrRawString prUsdStageAttributeAssetResolve(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
    PrStringView assetPath);

// Finds the times, as time codes of the stage, of the samples that an attribute's value at `time` lies between: the
// last at or before it and the first at or after it, which are one time at a sample and outside the samples. Returns
// false if no time samples decide the attribute's value at the time.
bool prUsdStageAttributeBracket(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
    double* lower, double* upper);

// Returns true if an attribute's value may differ between two times: more than one time sample decides it. A caller
// that reads a value once for every time reads it at any time if this is false.
bool prUsdStageAttributeVaries(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property);

// Returns the prim that holds what the prim at `path` composes, where the path may pass through instances: under an
// instance the prim is its counterpart in the instance's prototype, which every instance of that prototype shares.
// Returns kPrUsdNone if no prim is at the path. A prim that prUsdStageFindPrim finds is its own.
PrUsdPrimIndex prUsdStageFindProxy(const PrUsdStage* stage, PrStringView path);

// Finds what the scene path `path` names on the stage (usd-core/#paths), through instances as prUsdStageFindProxy does:
// a prim, as `/World/Car`, a property, as `/World/Car.points`, a variant set, as `/World/Car{color=}`, or one of its
// variants, as `/World/Car{color=red}`. A variant selection inside a path names the prim beneath it, which composition
// places at the path without the selection. Writes the prim that holds what the path names to `*prim`. Returns Syntax
// for text that is not a path, and NotFound if the stage holds nothing at the path. On failure, `*prim` is kPrUsdNone.
PrStatus prUsdStageFindPath(const PrUsdStage* stage, PrStringView path, PrUsdPrimIndex* prim);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
