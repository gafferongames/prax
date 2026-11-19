// The layer that the readers of each format build, and what they build it with.

#pragma once

#include "pr/arena.h"
#include "pr/atom_table.h"
#include "pr/base.h"
#include "pr/raw_array.h"
#include "pr/usd.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The largest page of the storage of values, since a layer's arrays and a stage's resolved values can be large.
constexpr size_t kUsdArenaLargestPage = size_t(1) << 20;

// A field as a reader sets it, before the fields of each spec are gathered.
struct UsdBuildField {
    PrUsdField field;
    uint32_t next;              // the spec's next field, or kPrUsdNone
};

// A layer. A reader adds specs and sets fields, and usdLayerFinish gathers them into the arrays that the public
// functions return.
struct UsdLayer {
    PrUsdFormat format;
    PrBlob* PR_NULLABLE file;   // the crate file, whose arrays the values point into
    PrStringView version;       // as the file states it

    AtomTable strings;          // every string of the layer
    Arena arena;                // the data of values, of pages up to kUsdArenaLargestPage
    RawArray<PrUsdSpec> specs;
    RawArray<PrUsdSpecIndex> specOfAtom;        // by atom: the spec whose path is the atom's text, or kPrUsdNone
    RawArray<PrUsdSpecIndex> lists;
    RawArray<PrUsdField> fields;
    RawArray<PrUsdSublayer> sublayers;

    RawArray<UsdBuildField> buildFields;
    RawArray<uint32_t> fieldHead;               // by spec: its first build field, or kPrUsdNone

    // The text of each spec of a text layer, which a crate layer leaves empty, and the specs whose text is not empty in
    // the order of its beginning, which finds the spec at an offset by a binary search.
    RawArray<PrUsdTextSpan> textSpans;          // by spec
    RawArray<PrUsdSpecIndex> textOrder;
};

// Returns `text` as a string of the layer.
PrStringView usdLayerIntern(UsdLayer* layer, PrStringView text);

// Returns `count` zeroed elements of `type` in the layer's arena.
void* usdLayerAllocElements(UsdLayer* layer, PrUsdType type, uint32_t count);

// Adds a spec of `form` named `name` under `parent`, and returns its index. The name must be valid for the form, and
// the parent's form must hold the form. If the layer already has a spec at the path, returns that spec and sets
// `*existed`.
PrUsdSpecIndex usdLayerAddSpec(UsdLayer* layer, PrUsdSpecForm form, PrUsdSpecIndex parent, PrStringView name, bool* existed);

// Returns the build field of `spec` named `name`, or null.
PrUsdField* PR_NULLABLE usdLayerBuildField(UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name);

// Sets a field of a spec, replacing the field of that name if the spec has one. `name` need not be interned.
void usdLayerSetField(UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name, PrUsdValue value);

// Returns a value of `count` elements of `type`, copied from `data` into the layer's arena.
PrUsdValue usdLayerValue(UsdLayer* layer, PrUsdType type, bool isArray, uint32_t count, const void* PR_NULLABLE data);

// Removes the elements of a list that equal an earlier element, as the lists of a list op require, and returns the
// number that remain. Elements are compared as bytes, which compares the strings of a layer too, since equal strings of
// a layer are one string.
uint32_t usdListRemoveRepeats(PrUsdType type, void* PR_NULLABLE data, uint32_t count);

// Return a dictionary, the samples of an attribute, or a map of variant selections, from entries in the order that a
// file gives them. An entry whose key or time repeats an earlier one replaces it. The entries are reordered.
PrUsdValue usdLayerDictionary(UsdLayer* layer, RawArray<PrUsdDictionaryEntry>& entries);
PrUsdValue usdLayerTimeSamples(UsdLayer* layer, RawArray<double>& times, RawArray<PrUsdValue>& values);
PrUsdValue usdLayerVariantSelections(UsdLayer* layer, RawArray<PrUsdVariantSelection>& selections);

// Returns the field of `spec` named `name`, or null, once usdLayerFinish has gathered the fields.
const PrUsdField* PR_NULLABLE usdLayerFindField(const UsdLayer* layer, PrUsdSpecIndex spec, PrStringView name);

// Gathers the specs' children and fields, fills in the fields that the specification requires with their fallbacks, and
// reads the sublayers. Returns false with a sentence in `diagnostics` if the layer spec's sublayer fields are not what
// the specification gives them.
bool usdLayerFinish(UsdLayer* layer, PrTextBuffer* PR_NULLABLE diagnostics);

// Frees a layer's storage, and the layer.
void usdLayerDestroy(UsdLayer* PR_NULLABLE layer);

// ---------------------------------------------------------------------------------------------------------------------
// Names and paths

// Return true if `name` is a name that the specification's grammar allows a prim or a variant set, a property with its
// namespaces, or a variant (usd-core/#names). A character outside ASCII is accepted wherever a letter is, which is
// wider than the XID classes of the grammar.
bool usdNameIsIdentifier(PrStringView name);
bool usdNameIsProperty(PrStringView name);
bool usdNameIsVariant(PrStringView name);

// How much of the path grammar a path may use.
enum {
    UsdPathKind_Any,            // absolute or relative, to a prim, a property, a variant or a target
    UsdPathKind_Prim,           // an absolute path to a prim, without variant selections
};
typedef uint8_t UsdPathKind;

// Returns true if `path` is a path of the grammar (usd-core/#path-grammar), with the productions that the specification
// keeps for legacy content: a relationship's target in brackets, a mapper and an expression.
bool usdPathIsValid(PrStringView path, UsdPathKind kind);

// ---------------------------------------------------------------------------------------------------------------------
// Fields

// The type that the module knows a field to hold: a field of the specification's data model, or one that Pixar's
// schemas register.
struct UsdFieldType {
    PrUsdType type;
    bool isArray;
    bool isListOp;              // a list op whose items have `type`
};

// Reports the type of the field named `name`. Returns false for a field that the module does not know.
bool usdFieldTypeFromName(PrStringView name, UsdFieldType* out);

// Returns true if `name` is one of the fields that list a spec's children, which a layer gives as lists of specs.
bool usdFieldIsChildren(PrStringView name);

// Appends a sentence to `diagnostics`, if there is one.
void usdDiagnose(PrTextBuffer* PR_NULLABLE diagnostics, PrStringView sentence);

// ---------------------------------------------------------------------------------------------------------------------
// Types

// Return the name of a type, the size of one of its elements, and the type that an attribute's `typeName` names, as the
// public functions of the same names document them.
PrStringView usdTypeName(PrUsdType type);
uint32_t usdTypeSize(PrUsdType type);
bool usdTypeFromName(PrStringView typeName, PrUsdType* type, bool* isArray);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
