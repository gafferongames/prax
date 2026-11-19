// Composition (usd-core/#composition): the layers that a stage reaches, their layer stacks, and the graph of the sites
// that give one prim its opinions.
//
// A prim's graph is a tree of nodes. A node is a site, a path in a layer stack, and the arc that brought it: the node
// that the arc was authored on, its namespace mapping and its strength among its siblings. The root node is the prim in
// the root layer stack. A node's children are in strength order, so the opinions of a prim are its nodes depth-first,
// and under each node the layers of its stack. A child prim's graph begins as its parent's with the child's name
// appended to every site, which is how the arcs of ancestors reach it, and composing it adds the arcs that its own
// specs author.

#pragma once

#include "pr/usd_stage.h"

#include "usd/src/usd_path.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A layer that a stage reached. `layer` is null for a location that could not be read, which is kept so that it is
// tried once.
struct UsdStageLayer {
    PrUsdLayer* PR_NULLABLE layer;
    PrStringView location;
    PrStringView name;                  // the last part of the location, for messages
    RawArray<UsdPath> specPaths;        // by spec
    double timeCodesPerSecond;
};

// A layer of a layer stack, and the retiming from its times to the times of the stack's root layer.
struct UsdStackLayer {
    uint32_t layer;
    PrUsdLayerOffset offset;
};

// A relocate of a layer stack. `origin` is the source before the relocates of its ancestors moved it.
struct UsdStackRelocate {
    UsdPath source;
    UsdPath target;                     // kUsdPathNone to remove the prim
    UsdPath origin;
};

// A layer stack (usd-core/#sublayers): a root layer and the layers that it reaches by sublayers, strongest first.
struct UsdLayerStack {
    uint32_t root;
    uint32_t firstLayer;                // in UsdComposer::stackLayers
    uint32_t layerCount;
    uint32_t firstRelocate;             // in UsdComposer::relocates
    uint32_t relocateCount;
    double timeCodesPerSecond;
};

// The flags of a node.
enum {
    UsdNodeFlag_Inert = 1,              // a placeholder, whose site gives no opinions
    UsdNodeFlag_Culled = 2,             // gives no opinions, and nothing under it does
    UsdNodeFlag_HasSpecs = 4,           // a layer of the stack has a spec at the path
    UsdNodeFlag_Elided = 8,             // culled for good: another arc gives what this one would
    UsdNodeFlag_Unique = 16,            // an arc under it is added only if no node already gives its opinions
};
typedef uint8_t UsdNodeFlags;

// A node of a prim's graph.
struct UsdNode {
    UsdPath path;
    uint32_t layerStack;
    UsdMapId mapToParent;
    UsdMapId mapToRoot;
    uint32_t chain;                     // of the composer's chains of mappings: those of the arcs from here to the root
    uint32_t parent;                    // kPrUsdNone for the root node
    uint32_t origin;                    // the node that an implied arc was implied from, and otherwise the parent
    uint32_t firstChild;                // strongest first, or kPrUsdNone
    uint32_t nextSibling;
    uint32_t namespaceDepth;            // the number of prim parts of the parent's path where the arc was introduced
    uint32_t restrictedDepth;           // of a placeholder: the number of prim parts of the ancestors that composed
                                        // with it before it became one, whose variants it still takes
    uint16_t siblingNum;                // the arc's position in the list that authored it
    PrUsdArc arc;
    UsdNodeFlags flags;
};

// The graph of one prim.
struct UsdGraph {
    RawArray<UsdNode> nodes;            // node 0 is the root node
    bool hasPayloads;                   // a contributing site authors a payload, loaded or not
    bool instanceable;
};

// A composition error (usd-core/#composition-errors).
struct UsdComposeError {
    PrUsdErrorKind kind;
    UsdPath prim;                       // the prim whose composition found it, or kUsdPathNone for a layer stack's
    PrStringView message;
};

// A selection of the stage's session: a prim's path, and a variant set's name and the variant's, as atoms of the path
// table's names.
struct UsdSessionSelection {
    UsdPath prim;
    Atom variantSet;
    Atom variant;
};

// What composes the prims of one stage: the layers, the layer stacks, the tables and the errors.
struct UsdComposer {
    PrUsdOptions layerOptions;
    PrUsdOpenFn PR_NULLABLE open;
    void* PR_NULLABLE openContext;
    PrUsdLoad load;
    bool expandInstances;
    RawArray<UsdPath> loadPaths;
    RawArray<PrUsdVariantSelection> variantFallbacks;
    RawArray<UsdSessionSelection> sessionSelections;
    uint32_t rootStack;                 // the stage's root layer stack, which the first opening makes

    UsdPathTable paths;
    UsdMapTable maps;
    AtomTable strings;                  // locations and messages
    RawArray<UsdStageLayer> layers;
    RawArray<uint32_t> layerOfLocation; // by atom of `strings`: the layer, or kPrUsdNone
    UsdKeyMap siteSpecs;                // by layer and path: the spec
    RawArray<UsdLayerStack> layerStacks;
    RawArray<uint32_t> stackOfLayer;    // by layer: the stack that it is the root of, or kPrUsdNone
    RawArray<UsdStackLayer> stackLayers;
    RawArray<UsdStackRelocate> relocates;
    RawArray<UsdComposeError> errors;
};

// Moves the keys of `list` that are in `order` into the order that it gives them, each with the keys that follow it up
// to the next key of the order, after the keys that precede them all. This is how OpenUSD applies `primOrder`,
// `propertyOrder` and the ordered list of a list op. `order` loses its repeated keys.
void usdListReorder(RawArray<uint64_t>* list, RawArray<uint64_t>* order);

// Applies a list op to `list` (usd-core/#list-operations). `lists` are the keys of the op's lists by PrUsdListOpList,
// which lose their repeated keys.
void usdListApply(RawArray<uint64_t>* list, RawArray<uint64_t>* lists, bool isExplicit);

// Start a composer with the options of a stage, and free what a composer holds.
void usdComposerInit(UsdComposer* composer, const PrUsdStageOptions* PR_NULLABLE options);
void usdComposerReset(UsdComposer* composer);

// Returns the layer stack whose root layer is at `location`, or kPrUsdNone with the status of reading the layer.
uint32_t usdComposerOpen(UsdComposer* composer, PrStringView location, PrStatus* status);

// Returns the spec of a layer at a path, or kPrUsdNone.
inline PrUsdSpecIndex usdComposerSpec(const UsdComposer* composer, uint32_t layer, UsdPath path) {
    return composer->siteSpecs.find(uint64_t(layer) << 32 | path);
}

// Makes `graph` the graph of the absolute root in the layer stack `rootStack`.
void usdComposeRoot(UsdComposer* composer, uint32_t rootStack, UsdGraph* graph);

// Makes `graph` the graph of the child named `name` of the prim whose graph is `parent`.
void usdComposeChild(UsdComposer* composer, const UsdGraph* parent, Atom name, UsdGraph* graph);

// Appends the names of the children of the prim of `graph`, in their composed order (usd-core/#ordered-prim-children),
// and the names that relocates forbid its children, as atoms of the path table.
void usdGraphChildNames(UsdComposer* composer, const UsdGraph* graph, RawArray<Atom>* names,
    RawArray<Atom>* prohibited);

// Returns true if the node gives opinions: it is neither a placeholder nor culled.
inline bool usdNodeContributes(const UsdNode& node) {
    return (node.flags & (UsdNodeFlag_Inert | UsdNodeFlag_Culled)) == 0;
}

// Returns true if a prim under the prim of `graph` may take opinions from the node, where the prim is an instance: the
// node came by an arc of the instance itself and has specs (usd-core/#instancing).
bool usdGraphNodeIsShared(const UsdComposer* composer, const UsdGraph* graph, uint32_t node);

// Appends the nodes of a graph, strongest first, a line each and indented under their parents: the arc, the root layer
// of the layer stack, the path, the flags, and the node that implied the arc if one did. It is how a test states the
// graph that it expects.
void usdGraphText(const UsdComposer* composer, const UsdGraph* graph, PrTextBuffer* out);

// Appends a sentence to the composer's errors.
void usdComposerError(UsdComposer* composer, PrUsdErrorKind kind, UsdPath prim, PrStringView message);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
