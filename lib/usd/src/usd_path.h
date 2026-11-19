// The paths of a stage as numbers, and the namespace mappings of composition arcs over them.
//
// A layer holds its paths as text. Composition maps paths between namespaces at every arc of every prim, so a stage
// interns each path that it meets into a table, where a path is its parent and its last part. Two equal paths are one
// number, a prefix is found by walking parents, and replacing a prefix makes a new path from an old one without text.

#pragma once

#include "pr/atom_table.h"
#include "pr/base.h"
#include "pr/hash_index.h"
#include "pr/raw_array.h"
#include "pr/text_buffer.h"
#include "pr/usd.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A map from 64-bit keys to numbers, which only grows.
struct UsdKeyMap {
    RawArray<uint64_t> keys;
    RawArray<uint32_t> values;
    KeyIndex index;

    // Returns the value of `key`, or kPrUsdNone.
    uint32_t find(uint64_t key) const {
        uint32_t at = index.find(key, [&](uint32_t i) { return keys.data[i]; });
        return at == kHashMissing ? kPrUsdNone : values.data[at];
    }

    // Sets the value of `key`, which the map must not hold.
    void add(uint64_t key, uint32_t value) {
        index.insert(key, keys.count);
        keys.push(key);
        values.push(value);
    }

    void reset() {
        keys.reset();
        values.reset();
        index.reset();
    }
};

// A path of a table. Zero is no path, and one is the absolute root, `/`.
typedef uint32_t UsdPath;
constexpr UsdPath kUsdPathNone = 0;
constexpr UsdPath kUsdPathRoot = 1;

// What the last part of a path names.
enum {
    UsdPathPart_Root,
    UsdPathPart_Prim,           // `/name`
    UsdPathPart_Variant,        // `{name=variant}`, where an empty variant names the variant set
    UsdPathPart_Property,       // `.name`, with everything that follows the first period
};
typedef uint8_t UsdPathPart;

// A path: its parent and its last part.
struct UsdPathNode {
    UsdPath parent;
    Atom name;
    Atom variant;               // of a variant part, and otherwise kAtomNone
    UsdPath stripped;           // the path without its variant parts
    uint32_t depth;             // the number of parts
    uint32_t primDepth : 28;    // the number of prim parts
    uint32_t part : 4;
};

// The paths that a stage has met. A path stays valid for the life of the table, and the table only grows.
struct UsdPathTable {
    AtomTable names;
    RawArray<UsdPathNode> nodes;
    HashIndex index;
};

// Start a table, which holds the root path, and free what a table holds.
void usdPathTableInit(UsdPathTable* table);
void usdPathTableReset(UsdPathTable* table);

// Return the path that `parent` and a part make if the table holds it, and otherwise kUsdPathNone. They change nothing,
// so any number of threads may call them on a table that no thread adds to.
UsdPath usdPathFind(const UsdPathTable* table, UsdPath parent, UsdPathPart part, Atom name, Atom variant);
UsdPath usdPathFindProperty(const UsdPathTable* table, UsdPath parent, PrStringView name);

// Return the path that `parent` and a part make, adding it if the table does not hold it.
UsdPath usdPathAppend(UsdPathTable* table, UsdPath parent, UsdPathPart part, Atom name, Atom variant);
UsdPath usdPathChild(UsdPathTable* table, UsdPath parent, PrStringView name);
UsdPath usdPathVariant(UsdPathTable* table, UsdPath parent, PrStringView variantSet, PrStringView variant);
UsdPath usdPathProperty(UsdPathTable* table, UsdPath parent, PrStringView name);

// Returns the path that `text` writes, which is an absolute path in the form that a layer gives its paths, as
// `/A{v=x}B.size`. Returns kUsdPathNone for text that is not such a path.
UsdPath usdPathParse(UsdPathTable* table, PrStringView text);

// As usdPathParse, for a table that no thread adds to: returns kUsdPathNone if the table does not hold the path.
UsdPath usdPathLookup(const UsdPathTable* table, PrStringView text);

// Appends the text of `path`, and nothing for kUsdPathNone.
void usdPathText(const UsdPathTable* table, UsdPath path, PrTextBuffer* out);

// Returns the node of `path`, which the table must hold.
inline const UsdPathNode& usdPathNode(const UsdPathTable* table, UsdPath path) {
    return table->nodes.data[path];
}

// Returns the name of the last part of `path`.
inline PrStringView usdPathName(const UsdPathTable* table, UsdPath path) {
    return atomTableText(&table->names, table->nodes.data[path].name);
}

// Returns the ancestor of `path` that has `depth` parts, which is `path` if it has no more.
UsdPath usdPathAncestor(const UsdPathTable* table, UsdPath path, uint32_t depth);

// Returns true if `prefix` is `path` or an ancestor of it.
bool usdPathHasPrefix(const UsdPathTable* table, UsdPath path, UsdPath prefix);

// Returns `path` with its prefix `from` replaced by `to`, or kUsdPathNone if `from` is not a prefix of it.
UsdPath usdPathReplacePrefix(UsdPathTable* table, UsdPath path, UsdPath from, UsdPath to);

// Returns the path of the prim or the variant that holds `path`: `path` itself unless it is a property's.
UsdPath usdPathPrim(const UsdPathTable* table, UsdPath path);

// Orders two names as the specification orders the elements of a path (usd-core/#element-ordering): negative if `a`
// comes first, positive if `b` does, and zero for equal names.
int usdNameCompare(PrStringView a, PrStringView b);

// ---------------------------------------------------------------------------------------------------------------------
// Namespace mappings (usd-core/#namespace-mappings)

// A namespace mapping of a table. Zero maps nothing, and one is the identity.
typedef uint32_t UsdMapId;
constexpr UsdMapId kUsdMapNull = 0;
constexpr UsdMapId kUsdMapIdentity = 1;

// Return the retimings that apply `inner` and then `outer`, and that undo `offset`.
PrUsdLayerOffset usdOffsetCompose(PrUsdLayerOffset outer, PrUsdLayerOffset inner);
PrUsdLayerOffset usdOffsetInverse(PrUsdLayerOffset offset);

// Returns true if `offset` changes no time.
inline bool usdOffsetIsIdentity(PrUsdLayerOffset offset) {
    return offset.offset == 0.0 && offset.scale == 1.0;
}

// A pair of a mapping: the paths at and under `source` map to those at and under `target`. A pair whose target is
// kUsdPathNone is a block: the paths at and under its source map to nothing, whatever a pair above them says.
struct UsdMapPair {
    UsdPath source;
    UsdPath target;
};

// A mapping: pairs of a source prefix and the target prefix that replaces it, and the retiming that takes a time of the
// source to a time of the target. A path maps by the pair whose prefix is its longest, and only if the result maps back
// by the same pair, so that a mapping is a bijection between the paths that it maps.
struct UsdMap {
    uint32_t first;             // in UsdMapTable::pairs
    uint32_t count;
    PrUsdLayerOffset offset;
};

// A chain of mappings: a path maps by `map` and then by the chain `above`, and only if each of them maps it. One
// mapping composed of a chain's would map a path that a mapping in the middle does not map back, so the path of a site
// maps to the namespace of its graph's root by the chain of its node's arcs. Zero is the chain of no mapping.
struct UsdMapChain {
    UsdMapId map;
    uint32_t above;
};
constexpr uint32_t kUsdMapChainNone = 0;

// The mappings that a stage has made. Equal mappings are one number, and so are equal chains.
struct UsdMapTable {
    UsdPathTable* paths;
    RawArray<UsdMapPair> pairs;
    RawArray<UsdMap> maps;
    HashIndex index;
    UsdKeyMap composed;                     // by the two mappings composed
    RawArray<UsdMapId> inverses;            // by mapping, or kPrUsdNone where it is not yet made
    RawArray<UsdMapChain> chains;
    UsdKeyMap chainOf;                      // by a chain's mapping and the chain above it
};

// Start a table over the paths of `paths`, and free what a table holds.
void usdMapTableInit(UsdMapTable* table, UsdPathTable* paths);
void usdMapTableReset(UsdMapTable* table);

// Returns the mapping of `pairs` and `offset`, without the pairs that the others imply.
UsdMapId usdMapMake(UsdMapTable* table, const UsdMapPair* PR_NULLABLE pairs, uint32_t count, PrUsdLayerOffset offset);

// Return `path` mapped from source to target and from target to source, or kUsdPathNone if the mapping does not map it.
// A variant part is no part of a namespace, so the path must have none.
UsdPath usdMapToTarget(UsdMapTable* table, UsdMapId map, UsdPath path);
UsdPath usdMapToSource(UsdMapTable* table, UsdMapId map, UsdPath path);

// Returns the mapping that applies `inner` and then `outer`.
UsdMapId usdMapCompose(UsdMapTable* table, UsdMapId outer, UsdMapId inner);

// Returns the mapping from the target of `map` to its source.
UsdMapId usdMapInverse(UsdMapTable* table, UsdMapId map);

// Returns `map` with the pair that maps every path that no other pair maps to itself.
UsdMapId usdMapAddRootIdentity(UsdMapTable* table, UsdMapId map);

// Maps the text of a path from source to target without adding to any table, so any number of threads may call it once
// the tables are complete. Appends the mapped path to `out` and returns true, or returns false if the mapping does not
// map the path.
bool usdMapTextToTarget(const UsdMapTable* table, UsdMapId map, PrStringView path, PrTextBuffer* out);

// Returns the chain of `map` and then the chain `above`, which is `above` itself for the identity.
uint32_t usdMapChainAdd(UsdMapTable* table, UsdMapId map, uint32_t above);

// Maps the text of a path by each mapping of a chain in turn, as usdMapTextToTarget maps by one.
bool usdMapChainTextToTarget(const UsdMapTable* table, uint32_t chain, PrStringView path, PrTextBuffer* out);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
