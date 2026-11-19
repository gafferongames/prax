// Composition: layers, layer stacks, and the graph of the sites that give one prim its opinions.
//
// The specification gives composition as rules and an outline (usd-core/#composition), and names OpenUSD's behaviour as
// the one to match where it is silent (usd-core/#compliance-ambiguities). The graph is built here as a queue of tasks
// over its nodes, in a fixed order of kinds: relocates, references, payloads, inherits, specializes, the arcs that
// inherits and specializes imply in the layer stacks above them, and last the variants, whose selection may come from
// any site that the others brought. A task that adds a node queues the tasks of that node.
//
// An arc to a prim below a root prim composes that prim and its ancestors in a nested composition, which selects no
// variant. The composition that nothing nests selects the variants of those ancestors once the nested graph has joined
// its own, so that a selection may come from any site of the prim's whole graph.

#include "usd/src/usd_compose.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/file.h"
#include "pr/float.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/raw_string.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "usd/src/usd_layer.h"

namespace pr_impl {

namespace {

constexpr PrUsdLayerOffset kNoOffset{0.0, 1.0};

// The deepest that one prim's composition nests: an arc to a prim below a root prim composes that prim and its
// ancestors before it joins the graph. Layers from outside the program decide the depth, so it has a bound.
constexpr uint32_t kMaxNesting = 256;

// The deepest that a graph's nodes nest: the longest chain of arcs from a prim to a site that gives it opinions.
constexpr uint32_t kMaxGraphDepth = 1024;

// The kinds of task, in the order that they run.
enum {
    Task_Relocations,
    Task_ImpliedRelocations,
    Task_References,
    Task_Payloads,
    Task_Inherits,
    Task_Specializes,
    Task_ImpliedClasses,
    Task_AncestralVariantSets,
    Task_VariantSets,
    Task_Unresolved,
};
typedef uint32_t Task;

// The kinds of variant task, which run after every other task that is not Task_Unresolved.
enum {
    VariantTaskKind_Authored,
    VariantTaskKind_Fallback,
    VariantTaskKind_NoneFound,
};
typedef uint8_t VariantTaskKind;

struct VariantTask {
    VariantTaskKind kind;
    bool ancestral;             // the variant set is of an ancestor of the node's site
    uint16_t number;            // the variant set's position among its site's variant sets
    uint32_t node;
    Atom variantSet;
    UsdPath site;               // the path that lists the variant set: the node's, or an ancestor of it
};

// A reference or a payload as a site authors it, with the layer that authors it.
struct RefArc {
    Atom location;              // of `strings`: the resolved asset path, or kAtomNone for the same layer stack
    UsdPath prim;               // kUsdPathNone for the default prim
    PrUsdLayerOffset authored;  // the offset that the item states, which is part of what the item is
    PrUsdLayerOffset offset;    // from the times of the target to those of the site's layer stack
    double timeCodesPerSecond;  // of the layer that authors the item
    bool invalidPath;
    bool invalidOffset;
};

struct Frame;

// What the composition of a graph keeps by node and by key, so that a node with thousands of arcs takes each without a
// walk of the others.
struct IndexerKept {
    RawArray<uint32_t> lastLinked;  // by node: the child that was linked under it last, or kPrUsdNone
    RawArray<uint32_t> ranks;       // by node: its number among its parent's children, where `ranked` says so
    RawArray<bool> ranked;          // by node: whether its children are numbered in their order
    UsdKeyMap sites;                // by layer stack and path: the sites of the first `sitesCount` nodes that give
                                    // opinions
    uint32_t sitesCount;
    UsdKeyMap classOfOrigin;        // by parent, origin and mapping: an inherits or specializes node of the first
                                    // `classCount` nodes
    UsdKeyMap classOfSite;          // by parent, layer stack, path and arc: the same
    UsdKeyMap copyOfPlaceholder;    // by a specializes placeholder: its copy at the root, of the same nodes
    uint32_t classCount;
};

// The composition of one graph.
struct Indexer {
    UsdComposer* c;
    UsdGraph* g;
    uint32_t rootStack;
    UsdPath errorPrim;          // the prim of the stage whose composition this is part of
    uint32_t nesting;
    Frame* PR_NULLABLE previous;
    RawArray<uint64_t> heap;
    RawArray<VariantTask> variantTasks;
    uint32_t linking;               // the node that is being linked into its parent's children, or kPrUsdNone
    IndexerKept* PR_NULLABLE kept;  // made when an arc is first added: most prims add none
};

// Returns what the composition keeps by node and by key, made at its first use.
IndexerKept* keptOf(Indexer* ix) {
    if (ix->kept == nullptr)
        ix->kept = allocZeroed<IndexerKept>();
    return ix->kept;
}

// The arc that a nested composition will join its caller's graph by.
struct Frame {
    Indexer* caller;
    uint32_t parentNode;        // in the caller's graph
    UsdPath requestedPath;
    uint8_t arc;
    UsdMapId mapToParent;
    uint32_t origin;
    uint32_t namespaceDepth;
    uint16_t siblingNum;
    uint32_t layerStack;
    bool skipDuplicates;        // every arc of the nested composition is added only if no node gives its opinions
};

// What decides an arc's strength among its siblings.
struct ArcKey {
    uint8_t arc;
    uint16_t siblingNum;
    uint32_t parent;
    uint32_t origin;
    uint32_t namespaceDepth;
    uint32_t layerStack;
    UsdPath path;
    uint32_t self;              // the node, or kPrUsdNone for an arc that is not in the graph yet
};

// A sentence of an error, built from text and paths.
struct Sentence {
    UsdComposer* c;
    PrTextBuffer text = {};

    Sentence& add(PrStringView words) {
        prTextBufferAppend(&text, words);
        return *this;
    }
    Sentence& path(UsdPath value) {
        prTextBufferAppendChar(&text, '<');
        usdPathText(&c->paths, value, &text);
        prTextBufferAppendChar(&text, '>');
        return *this;
    }
    Sentence& site(uint32_t layerStack, UsdPath value) {
        prTextBufferAppendChar(&text, '@');
        prTextBufferAppend(&text, c->layers.data[c->layerStacks.data[layerStack].root].name);
        prTextBufferAppendChar(&text, '@');
        return path(value);
    }
    void report(PrUsdErrorKind kind, UsdPath prim) {
        usdComposerError(c, kind, prim, prTextBufferView(&text));
        prTextBufferReset(&text);
    }
};

bool isClassArc(uint8_t arc) {
    return arc == PrUsdArc_Inherit || arc == PrUsdArc_Specialize;
}

uint32_t primDepth(const UsdComposer* c, UsdPath path) {
    return usdPathNode(&c->paths, path).primDepth;
}

UsdPath stripped(const UsdComposer* c, UsdPath path) {
    return usdPathNode(&c->paths, path).stripped;
}

// Returns true if `path` is a prim's: it has a prim part, and no property or variant part.
bool isPrimPath(const UsdComposer* c, UsdPath path) {
    if (path == kUsdPathNone || path == kUsdPathRoot)
        return false;
    const UsdPathNode& node = usdPathNode(&c->paths, path);
    return node.part == UsdPathPart_Prim && node.stripped == path;
}

const PrUsdField* PR_NULLABLE fieldAt(const UsdComposer* c, uint32_t layer, UsdPath path, PrStringView name) {
    PrUsdSpecIndex spec = usdComposerSpec(c, layer, path);
    if (spec == kPrUsdNone)
        return nullptr;
    return prUsdLayerFindField(c->layers.data[layer].layer, spec, name);
}

bool hasSpecs(const UsdComposer* c, uint32_t layerStack, UsdPath path) {
    const UsdLayerStack& stack = c->layerStacks.data[layerStack];
    for (uint32_t i = 0; i < stack.layerCount; ++i) {
        if (usdComposerSpec(c, c->stackLayers.data[stack.firstLayer + i].layer, path) != kPrUsdNone)
            return true;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers and layer stacks

// Returns the layer at `location`, reading it if the composer has not tried to, or kPrUsdNone.
uint32_t openLayer(UsdComposer* c, PrStringView location, PrStatus* status) {
    Atom atom = atomTableAdd(&c->strings, location);
    while (c->layerOfLocation.count <= atom)
        c->layerOfLocation.push(kPrUsdNone);
    uint32_t index = c->layerOfLocation.data[atom];
    if (index != kPrUsdNone) {
        *status = c->layers.data[index].layer != nullptr ? PrStatus_Ok : PrStatus_Io;
        return c->layers.data[index].layer != nullptr ? index : kPrUsdNone;
    }
    UsdStageLayer entry{};
    *status = prUsdLayerRead(location, c->open, c->openContext, &c->layerOptions, &entry.layer);
    entry.location = atomTableText(&c->strings, atom);
    entry.name = prFilePathName(entry.location);
    entry.timeCodesPerSecond = 24.0;
    index = c->layers.count;
    if (entry.layer != nullptr) {
        uint32_t count = prUsdLayerSpecCount(entry.layer);
        const PrUsdSpec* specs = prUsdLayerSpecs(entry.layer);
        entry.specPaths.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            UsdPath path = i == kPrUsdLayerSpec ? kUsdPathRoot : usdPathParse(&c->paths, specs[i].path);
            entry.specPaths.data[i] = path;
            if (path != kUsdPathNone && usdComposerSpec(c, index, path) == kPrUsdNone)
                c->siteSpecs.add(uint64_t(index) << 32 | path, i);
        }
        // A layer's time scale is its `timeCodesPerSecond`, else its `framesPerSecond`, else 24.
        const PrStringView rates[2] = {"framesPerSecond", "timeCodesPerSecond"};
        for (PrStringView name : rates) {
            const PrUsdField* rate = prUsdLayerFindField(entry.layer, kPrUsdLayerSpec, name);
            if (rate != nullptr && rate->value.type == PrUsdType_Double && !rate->value.isArray)
                entry.timeCodesPerSecond = *static_cast<const double*>(rate->value.data);
        }
    }
    c->layers.push(entry);
    c->stackOfLayer.push(kPrUsdNone);
    c->layerOfLocation.data[atom] = index;
    return entry.layer != nullptr ? index : kPrUsdNone;
}

// Returns `offset` if its scale is a positive number, and otherwise the identity with `*valid` false.
PrUsdLayerOffset checkedOffset(PrUsdLayerOffset offset, bool* valid) {
    *valid = offset.scale > 0.0 && prF64IsFinite(offset.scale) && prF64IsFinite(offset.offset);
    return *valid ? offset : kNoOffset;
}

// Adds a layer and its sublayers to the stack being built. `chain` holds the layers that lead to it.
void addStackLayer(UsdComposer* c, uint32_t layer, PrUsdLayerOffset offset, RawArray<uint32_t>* chain) {
    c->stackLayers.push(UsdStackLayer{layer, offset});
    chain->push(layer);
    uint32_t count = prUsdLayerSublayerCount(c->layers.data[layer].layer);
    for (uint32_t i = 0; i < count; ++i) {
        PrUsdSublayer sublayer = prUsdLayerSublayers(c->layers.data[layer].layer)[i];
        PrRawString location = prUsdAssetResolve(c->layers.data[layer].location, sublayer.assetPath);
        PrStatus status = PrStatus_Ok;
        uint32_t sub = openLayer(c, prRawStringView(&location), &status);
        prRawStringReset(&location);
        Sentence sentence{c};
        if (sub == kPrUsdNone) {
            sentence.add("the sublayer @").add(sublayer.assetPath).add("@ of @").add(c->layers.data[layer].name)
                .add("@ cannot be read").report(PrUsdErrorKind_SublayerUnreadable, kUsdPathNone);
            continue;
        }
        if (chain->find(sub) != kArrayMissing || chain->count >= kMaxNesting) {
            sentence.add("the sublayer @").add(sublayer.assetPath).add("@ of @").add(c->layers.data[layer].name)
                .add("@ is a layer that it is a sublayer of").report(PrUsdErrorKind_SublayerCycle, kUsdPathNone);
            continue;
        }
        bool valid = true;
        PrUsdLayerOffset retiming = checkedOffset(sublayer.layerOffset, &valid);
        if (!valid) {
            sentence.add("the offset of the sublayer @").add(sublayer.assetPath).add("@ of @")
                .add(c->layers.data[layer].name).add("@ has a scale that is not positive")
                .report(PrUsdErrorKind_InvalidOffset, kUsdPathNone);
        }
        // A sublayer with another time scale is scaled to its parent's (usd-core/#sublayer-offset-adjustment).
        double factor = c->layers.data[layer].timeCodesPerSecond / c->layers.data[sub].timeCodesPerSecond;
        if (!(factor > 0.0) || !prF64IsFinite(factor)) {
            factor = 1.0;
            Sentence scale{c};
            scale.add("the time codes per second of @").add(c->layers.data[layer].name).add("@ and of its sublayer @")
                .add(sublayer.assetPath).add("@ are not both positive").report(PrUsdErrorKind_InvalidOffset,
                    kUsdPathNone);
        }
        retiming.scale *= factor;
        addStackLayer(c, sub, usdOffsetCompose(offset, retiming), chain);
    }
    --chain->count;
}

// Reads the relocates of a layer stack's layers, and keeps those that the rules allow (usd-core/#relocates).
void addRelocates(UsdComposer* c, UsdLayerStack* stack) {
    const UsdPathTable* paths = &c->paths;
    struct Authored {
        UsdPath source;
        UsdPath target;
        uint32_t layer;
        bool valid;
    };
    RawArray<Authored> authored{};
    for (uint32_t i = 0; i < stack->layerCount; ++i) {
        uint32_t layer = c->stackLayers.data[stack->firstLayer + i].layer;
        const PrUsdField* field = prUsdLayerFindField(c->layers.data[layer].layer, kPrUsdLayerSpec, "layerRelocates");
        if (field == nullptr || field->value.type != PrUsdType_Relocate)
            continue;
        const PrUsdRelocate* items = static_cast<const PrUsdRelocate*>(field->value.data);
        for (uint32_t j = 0; j < field->value.count; ++j) {
            Authored entry{usdPathParse(&c->paths, items[j].source), usdPathParse(&c->paths, items[j].target), layer, true};
            Sentence sentence{c};
            sentence.add("the relocate from <").add(items[j].source).add("> to <").add(items[j].target).add("> in @")
                .add(c->layers.data[layer].name).add("@ is ignored: ");
            PrStringView why{};
            if (!isPrimPath(c, entry.source) || (items[j].target.size != 0 && !isPrimPath(c, entry.target)))
                why = "its paths are not the paths of prims";
            else if (entry.source == entry.target)
                why = "its target is its source";
            else if (usdPathHasPrefix(paths, entry.source, entry.target))
                why = "its target is an ancestor of its source";
            else if (usdPathHasPrefix(paths, entry.target, entry.source))
                why = "its target is a descendant of its source";
            else if (primDepth(c, entry.source) == 1)
                why = "its source is a root prim";
            bool repeats = false;
            for (uint32_t k = 0; why.size == 0 && !repeats && k < authored.count; ++k)
                repeats = authored.data[k].source == entry.source;
            if (why.size != 0)
                sentence.add(why).report(PrUsdErrorKind_InvalidRelocate, kUsdPathNone);
            else
                prTextBufferReset(&sentence.text);
            // A stronger layer's relocate of a source replaces a weaker layer's.
            if (why.size == 0 && !repeats)
                authored.push(entry);
        }
    }
    // The relocates of a layer stack may not conflict with one another.
    for (uint32_t i = 0; i < authored.count; ++i) {
        const Authored& a = authored.data[i];
        PrStringView why{};
        for (uint32_t j = 0; why.size == 0 && j < authored.count; ++j) {
            const Authored& b = authored.data[j];
            if (j == i)
                continue;
            if (a.target != kUsdPathNone && a.target == b.target)
                why = "another relocate has its target";
            else if (a.target != kUsdPathNone && a.target == b.source)
                why = "its target is the source of another relocate";
            else if (a.source == b.target)
                why = "its source is the target of another relocate";
            else if (usdPathHasPrefix(paths, a.source, b.source))
                why = "its source is a descendant of the source of another relocate";
            else if (usdPathHasPrefix(paths, a.target, b.source))
                why = "its target is a descendant of the source of another relocate";
        }
        if (why.size != 0) {
            authored.data[i].valid = false;
            Sentence sentence{c};
            sentence.add("the relocate from ").path(a.source).add(" to ").path(a.target).add(" in @")
                .add(c->layers.data[a.layer].name).add("@ is ignored: ").add(why)
                .report(PrUsdErrorKind_InvalidRelocate, kUsdPathNone);
        }
    }
    stack->firstRelocate = c->relocates.count;
    for (uint32_t i = 0; i < authored.count; ++i) {
        if (authored.data[i].valid)
            c->relocates.push(UsdStackRelocate{
                authored.data[i].source, authored.data[i].target,
                authored.data[i].source,
            });
    }
    stack->relocateCount = c->relocates.count - stack->firstRelocate;
    authored.reset();
    // A relocate's source is in the namespace that the relocates of its ancestors made, so its origin undoes them.
    UsdStackRelocate* relocates = c->relocates.data + stack->firstRelocate;
    for (uint32_t i = 0; i < stack->relocateCount; ++i) {
        for (uint32_t round = 0; round < stack->relocateCount; ++round) {
            uint32_t above = kPrUsdNone;
            for (uint32_t j = 0; j < stack->relocateCount; ++j) {
                if (j != i && relocates[j].target != relocates[i].origin &&
                        usdPathHasPrefix(paths, relocates[i].origin, relocates[j].target) && (above == kPrUsdNone ||
                            usdPathNode(paths, relocates[j].target).depth >
                            usdPathNode(paths, relocates[above].target).depth))
                    above = j;
            }
            if (above == kPrUsdNone)
                break;
            relocates[i].origin =
                usdPathReplacePrefix(&c->paths, relocates[i].origin, relocates[above].target, relocates[above].source);
        }
    }
}

// Returns the layer stack whose root is `layer`.
uint32_t layerStackOf(UsdComposer* c, uint32_t layer) {
    if (c->stackOfLayer.data[layer] != kPrUsdNone)
        return c->stackOfLayer.data[layer];
    UsdLayerStack stack{};
    stack.root = layer;
    stack.firstLayer = c->stackLayers.count;
    stack.timeCodesPerSecond = c->layers.data[layer].timeCodesPerSecond;
    RawArray<uint32_t> chain{};
    addStackLayer(c, layer, kNoOffset, &chain);
    chain.reset();
    stack.layerCount = c->stackLayers.count - stack.firstLayer;
    addRelocates(c, &stack);
    uint32_t index = c->layerStacks.count;
    c->layerStacks.push(stack);
    c->stackOfLayer.data[layer] = index;
    return index;
}

// Returns the relocate of a layer stack whose target, or whose source, is `path`, or null.
const UsdStackRelocate* PR_NULLABLE findRelocate(const UsdComposer* c, uint32_t layerStack, UsdPath path, bool byTarget) {
    const UsdLayerStack& stack = c->layerStacks.data[layerStack];
    for (uint32_t i = 0; i < stack.relocateCount; ++i) {
        const UsdStackRelocate& relocate = c->relocates.data[stack.firstRelocate + i];
        if ((byTarget ? relocate.target : relocate.source) == path)
            return &relocate;
    }
    return nullptr;
}

// Returns the mapping of an arc from `source` to `target`, a path of `targetStack`, with the relocates of that stack at
// and under the target (usd-core/#relocates).
UsdMapId arcMap(UsdComposer* c, uint32_t targetStack, UsdPath source, UsdPath target, PrUsdLayerOffset offset) {
    UsdMapPair pair{source, target};
    UsdMapId map = usdMapMake(&c->maps, &pair, 1, offset);
    const UsdLayerStack& stack = c->layerStacks.data[targetStack];
    RawArray<UsdMapPair> pairs{};
    for (uint32_t i = 0; i < stack.relocateCount; ++i) {
        const UsdStackRelocate& relocate = c->relocates.data[stack.firstRelocate + i];
        // The target is a path of the namespace that the relocates of its ancestors made, and what the arc brings under
        // it has the paths from before any relocate. A relocate applies by whichever of the two its source is under the
        // target in.
        if (relocate.target == kUsdPathNone)
            continue;
        if (usdPathHasPrefix(&c->paths, relocate.origin, target))
            pairs.push(UsdMapPair{relocate.origin, relocate.target});
        else if (usdPathHasPrefix(&c->paths, relocate.source, target))
            pairs.push(UsdMapPair{relocate.source, relocate.target});
    }
    if (pairs.count != 0) {
        pairs.push(UsdMapPair{kUsdPathRoot, kUsdPathRoot});
        map = usdMapCompose(&c->maps, usdMapMake(&c->maps, pairs.data, pairs.count, kNoOffset), map);
    }
    pairs.reset();
    return map;
}

// ---------------------------------------------------------------------------------------------------------------------
// List ops (usd-core/#list-operations)

// Removes from `list` the keys that are in `keys`.
void removeKeys(RawArray<uint64_t>* list, const RawArray<uint64_t>& keys) {
    if (keys.count == 0 || list->count == 0)
        return;
    UsdKeyMap set{};
    for (uint32_t i = 0; i < keys.count; ++i) {
        if (set.find(keys.data[i]) == kPrUsdNone)
            set.add(keys.data[i], i);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; ++i) {
        if (set.find(list->data[i]) == kPrUsdNone)
            list->data[kept++] = list->data[i];
    }
    list->count = kept;
    set.reset();
}

// Removes the keys of `list` that repeat an earlier key.
void uniqueKeys(RawArray<uint64_t>* list) {
    UsdKeyMap set{};
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; ++i) {
        if (set.find(list->data[i]) == kPrUsdNone) {
            set.add(list->data[i], i);
            list->data[kept++] = list->data[i];
        }
    }
    list->count = kept;
    set.reset();
}

// Composes the list op field `name` of a site, weakest layer first, into `list`. `keyOf` gives the key of an item of a
// layer of the stack, or kPrUsdNone to leave the item out.
template <typename KeyOf>
void composeListOp(const UsdComposer* c, uint32_t layerStack, UsdPath path, PrStringView name, RawArray<uint64_t>* list,
        KeyOf keyOf) {
    const UsdLayerStack& stack = c->layerStacks.data[layerStack];
    for (uint32_t i = stack.layerCount; i-- > 0;) {
        const UsdStackLayer& stackLayer = c->stackLayers.data[stack.firstLayer + i];
        const PrUsdField* field = fieldAt(c, stackLayer.layer, path, name);
        if (field == nullptr || field->value.type != PrUsdType_ListOp)
            continue;
        const PrUsdListOp* op = static_cast<const PrUsdListOp*>(field->value.data);
        RawArray<uint64_t> lists[PrUsdListOpList_Count] = {};
        for (uint32_t which = 0; which < PrUsdListOpList_Count; ++which) {
            for (uint32_t j = 0; j < op->items[which].count; ++j) {
                uint64_t key = keyOf(stackLayer, op->itemType, op->items[which].data, j);
                if (key != kPrUsdNone)
                    lists[which].push(key);
            }
        }
        usdListApply(list, lists, op->isExplicit);
        for (RawArray<uint64_t>& each : lists)
            each.reset();
    }
}

// Composes a list op of paths at a site, as inherits and specializes are, into paths.
void composePaths(UsdComposer* c, uint32_t layerStack, UsdPath path, PrStringView name, RawArray<uint64_t>* list) {
    composeListOp(c, layerStack, path, name, list,
        [&](const UsdStackLayer&, PrUsdType type, const void* items, uint32_t index) -> uint64_t {
            if (type != PrUsdType_Path)
                return kPrUsdNone;
            UsdPath item = usdPathParse(&c->paths, static_cast<const PrStringView*>(items)[index]);
            return item == kUsdPathNone ? kPrUsdNone : stripped(c, item);
        });
}

// Composes a list op of strings at a site, as the names of variant sets are, into atoms of the path table.
void composeNames(UsdComposer* c, uint32_t layerStack, UsdPath path, PrStringView name, RawArray<uint64_t>* list) {
    composeListOp(c, layerStack, path, name, list,
        [&](const UsdStackLayer&, PrUsdType type, const void* items, uint32_t index) -> uint64_t {
            if (type != PrUsdType_String && type != PrUsdType_Token)
                return kPrUsdNone;
            return atomTableAdd(&c->paths.names, static_cast<const PrStringView*>(items)[index]);
        });
}

// Composes the references or the payloads of a site into `arcs`, strongest first.
void composeRefs(UsdComposer* c, uint32_t layerStack, UsdPath path, PrStringView name, RawArray<RefArc>* arcs) {
    RawArray<RefArc> seen{};
    UsdKeyMap seenOf{};
    RawArray<uint64_t> list{};
    composeListOp(c, layerStack, path, name, &list,
        [&](const UsdStackLayer& stackLayer, PrUsdType type, const void* items, uint32_t index) -> uint64_t {
            if (type != PrUsdType_Reference && type != PrUsdType_Payload)
                return kPrUsdNone;
            const PrUsdReference& item = static_cast<const PrUsdReference*>(items)[index];
            RefArc arc{};
            arc.location = kAtomNone;
            if (item.assetPath.size != 0) {
                PrRawString location = prUsdAssetResolve(c->layers.data[stackLayer.layer].location, item.assetPath);
                arc.location = atomTableAdd(&c->strings, prRawStringView(&location));
                prRawStringReset(&location);
            }
            if (item.primPath.size != 0) {
                arc.prim = usdPathParse(&c->paths, item.primPath);
                arc.invalidPath = !isPrimPath(c, arc.prim);
            }
            bool valid = true;
            arc.authored = item.layerOffset;
            arc.offset = usdOffsetCompose(stackLayer.offset, checkedOffset(item.layerOffset, &valid));
            arc.invalidOffset = !valid;
            arc.timeCodesPerSecond = c->layers.data[stackLayer.layer].timeCodesPerSecond;
            // An item is the asset, the prim and the offset that it states. The retiming of the layer that states it is
            // no part of it, and the strongest layer that lists the item gives its own.
            auto same = [&](const RefArc& other) {
                return other.location == arc.location && other.prim == arc.prim && other.invalidPath == arc.invalidPath &&
                    prMemEquals(&other.authored, &arc.authored, sizeof(arc.authored));
            };
            uint64_t bits[2] = {};
            prMemCopy(bits, &arc.authored, sizeof(bits));
            uint64_t key = prHashMixKeys(prHashMixKeys(uint64_t(arc.location) << 32 | arc.prim, arc.invalidPath),
                prHashMixKeys(bits[0], bits[1]));
            uint32_t found = seenOf.find(key);
            // Two items with one key are told apart by a search of them all, which no layer is expected to need.
            for (uint32_t i = 0; found != kPrUsdNone && !same(seen.data[found]) && i < seen.count; ++i) {
                if (same(seen.data[i]))
                    found = i;
            }
            if (found != kPrUsdNone && same(seen.data[found])) {
                seen.data[found].offset = arc.offset;
                seen.data[found].timeCodesPerSecond = arc.timeCodesPerSecond;
                return found;
            }
            if (found == kPrUsdNone)
                seenOf.add(key, seen.count);
            seen.push(arc);
            return seen.count - 1;
        });
    for (uint32_t i = 0; i < list.count; ++i)
        arcs->push(seen.data[list.data[i]]);
    if (seen.count != 0)
        seenOf.reset();
    seen.reset();
    list.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Graphs

uint32_t depthBelowIntroduction(const UsdComposer* c, const UsdGraph* g, uint32_t node) {
    const UsdNode& n = g->nodes.data[node];
    if (n.parent == kPrUsdNone)
        return 0;
    uint32_t depth = primDepth(c, g->nodes.data[n.parent].path);
    return depth > n.namespaceDepth ? depth - n.namespaceDepth : 0;
}

// Returns the node's path where its arc was introduced: without the prim parts that descending to this prim added.
UsdPath pathAtIntroduction(const UsdComposer* c, const UsdGraph* g, uint32_t node) {
    UsdPath path = g->nodes.data[node].path;
    for (uint32_t depth = depthBelowIntroduction(c, g, node); depth != 0 && path != kUsdPathRoot;) {
        if (usdPathNode(&c->paths, path).part == UsdPathPart_Prim)
            --depth;
        path = usdPathNode(&c->paths, path).parent;
    }
    return path;
}

bool hasClassChild(const UsdGraph* g, uint32_t node) {
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if (isClassArc(g->nodes.data[child].arc))
            return true;
    }
    return false;
}

// Returns true if the node is the copy at the root of a specializes arc that was authored or implied below the root,
// where a placeholder stands for it (usd-core/#specializes).
bool isSpecializeCopy(const UsdGraph* g, uint32_t node) {
    const UsdNode& n = g->nodes.data[node];
    if (n.arc != PrUsdArc_Specialize || n.parent != 0 || n.origin == n.parent)
        return false;
    const UsdNode& origin = g->nodes.data[n.origin];
    return origin.layerStack == n.layerStack && origin.path == n.path;
}

// Returns the copy at the root of a specializes placeholder, or the node itself if it has none.
uint32_t specializeCopy(const UsdGraph* g, uint32_t node) {
    const UsdNode& n = g->nodes.data[node];
    if (n.arc != PrUsdArc_Specialize || n.parent == 0)
        return node;
    for (uint32_t child = g->nodes.data[0].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if (g->nodes.data[child].origin == node && isSpecializeCopy(g, child))
            return child;
    }
    return node;
}

ArcKey keyOf(const UsdGraph* g, uint32_t node) {
    const UsdNode& n = g->nodes.data[node];
    return ArcKey{n.arc, n.siblingNum, n.parent, n.origin, n.namespaceDepth, n.layerStack, n.path, node};
}

int compareSiblings(Indexer* ix, const ArcKey& a, const ArcKey& b);

// Orders two children of one node of the composition's graph: negative if `a` comes first. A node's children are
// numbered in their order when two of them are first compared, and the numbers hold until a node joins them anywhere
// but at their end.
int compareLinked(Indexer* ix, uint32_t a, uint32_t b) {
    const UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    // A node that is not among its parent's children yet comes after those that are.
    if (a == ix->linking || b == ix->linking)
        return a == ix->linking ? 1 : -1;
    uint32_t parent = g->nodes.data[a].parent;
    while (kept->ranks.count < g->nodes.count) {
        kept->ranks.push(0);
        kept->ranked.push(false);
    }
    if (!kept->ranked.data[parent]) {
        uint32_t rank = 0;
        for (uint32_t at = g->nodes.data[parent].firstChild; at != kPrUsdNone; at = g->nodes.data[at].nextSibling)
            kept->ranks.data[at] = rank++;
        kept->ranked.data[parent] = true;
    }
    return kept->ranks.data[a] < kept->ranks.data[b] ? -1 : 1;
}

// Orders two nodes of a graph by strength: negative if `a` is stronger. A key whose node is not in the graph yet stands
// for the node that it will be.
int comparePlaces(Indexer* ix, const ArcKey& a, const ArcKey& b) {
    const UsdGraph* g = ix->g;
    if (a.self == b.self && a.self != kPrUsdNone)
        return 0;
    if (a.self != kPrUsdNone && b.self != kPrUsdNone && a.parent == b.parent && a.parent != kPrUsdNone)
        return compareLinked(ix, a.self, b.self);
    // The chains of each from the root. A key that is not in the graph is the last of its chain.
    RawArray<uint32_t> chainA{};
    for (uint32_t at = a.self != kPrUsdNone ? a.self : a.parent; at != kPrUsdNone; at = g->nodes.data[at].parent)
        chainA.push(at);
    RawArray<uint32_t> chainB{};
    for (uint32_t at = b.self != kPrUsdNone ? b.self : b.parent; at != kPrUsdNone; at = g->nodes.data[at].parent)
        chainB.push(at);
    uint32_t i = chainA.count;
    uint32_t j = chainB.count;
    while (i != 0 && j != 0 && chainA.data[i - 1] == chainB.data[j - 1]) {
        --i;
        --j;
    }
    // Where the chains part, the stronger sibling decides, and an ancestor is stronger than its descendants.
    bool endA = i == 0 && a.self != kPrUsdNone;
    bool endB = j == 0 && b.self != kPrUsdNone;
    int order = 0;
    if (endA || endB) {
        order = endA ? -1 : 1;
    } else {
        ArcKey first = i == 0 ? a : keyOf(g, chainA.data[i - 1]);
        ArcKey second = j == 0 ? b : keyOf(g, chainB.data[j - 1]);
        // Two nodes of the graph are in their parent's children, in order.
        if (first.self != kPrUsdNone && second.self != kPrUsdNone)
            order = compareLinked(ix, first.self, second.self);
        else
            order = compareSiblings(ix, first, second);
    }
    chainA.reset();
    chainB.reset();
    return order;
}

int compareNodes(Indexer* ix, uint32_t a, uint32_t b) {
    return a == b ? 0 : comparePlaces(ix, keyOf(ix->g, a), keyOf(ix->g, b));
}

// Orders two arcs under one parent by the rules that follow the kind of arc (usd-core/#strength-ordering): the arc
// introduced deeper in namespace, then the arc whose origin is stronger, which puts an authored arc before an implied
// one, then the arc that its site listed first.
int compareByOrigin(Indexer* ix, const ArcKey& a, const ArcKey& b) {
    if (a.namespaceDepth != b.namespaceDepth)
        return a.namespaceDepth > b.namespaceDepth ? -1 : 1;
    if (a.origin != b.origin)
        return compareNodes(ix, a.origin, b.origin);
    if (a.siblingNum != b.siblingNum)
        return a.siblingNum < b.siblingNum ? -1 : 1;
    return 0;
}

int compareSiblings(Indexer* ix, const ArcKey& a, const ArcKey& b) {
    const UsdGraph* g = ix->g;
    if (a.arc != b.arc)
        return a.arc < b.arc ? -1 : 1;
    if (a.arc == PrUsdArc_Specialize && a.parent == 0) {
        // The specializes arcs of the whole graph are children of the root, so that they are weaker than everything
        // else. They order as the arcs that were authored do where they were authored, and the arcs that one authored
        // arc implies or is copied to follow it together, in the order of the nodes that they are implied under.
        auto authored = [&](ArcKey key) {
            while (key.origin != key.parent)
                key = keyOf(g, key.origin);
            return key;
        };
        ArcKey rootA = authored(a);
        ArcKey rootB = authored(b);
        bool same = rootA.self == rootB.self && rootA.self != kPrUsdNone;
        bool both = rootA.self == a.self && rootB.self == b.self;
        if (!same && !both) {
            int order = comparePlaces(ix, rootA, rootB);
            if (order != 0)
                return order;
        }
        auto home = [&](const ArcKey& key) {
            if (key.origin == key.parent)
                return key;
            const UsdNode& origin = g->nodes.data[key.origin];
            return origin.layerStack == key.layerStack && origin.path == key.path ? keyOf(g, key.origin) : key;
        };
        ArcKey homeA = home(a);
        ArcKey homeB = home(b);
        if (homeA.parent != homeB.parent)
            return compareNodes(ix, homeA.parent, homeB.parent);
        return compareByOrigin(ix, homeA, homeB);
    }
    return compareByOrigin(ix, a, b);
}

// Links a node, whose fields are set, into its parent's children at its strength. A site's arcs are added strongest
// first, and so are the arcs that another node's imply, so most nodes follow the node that their parent took last. The
// search for a node's place begins there if the node is no stronger, so that a node with thousands of children takes
// each without a walk of them all.
void linkNode(Indexer* ix, uint32_t node) {
    UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    ArcKey key = keyOf(g, node);
    uint32_t parent = g->nodes.data[node].parent;
    while (kept->lastLinked.count < g->nodes.count)
        kept->lastLinked.push(kPrUsdNone);
    ix->linking = node;
    uint32_t before = kPrUsdNone;
    uint32_t at = g->nodes.data[parent].firstChild;
    uint32_t last = kept->lastLinked.data[parent];
    if (last != kPrUsdNone && last != node && compareSiblings(ix, key, keyOf(g, last)) >= 0) {
        before = last;
        at = g->nodes.data[last].nextSibling;
    }
    while (at != kPrUsdNone && compareSiblings(ix, key, keyOf(g, at)) >= 0) {
        before = at;
        at = g->nodes.data[at].nextSibling;
    }
    g->nodes.data[node].nextSibling = at;
    if (before == kPrUsdNone)
        g->nodes.data[parent].firstChild = node;
    else
        g->nodes.data[before].nextSibling = node;
    kept->lastLinked.data[parent] = node;
    ix->linking = kPrUsdNone;
    // The numbers of the parent's children hold if the node is their last, and are made again if it is not.
    if (parent < kept->ranked.count && kept->ranked.data[parent]) {
        while (kept->ranks.count < g->nodes.count) {
            kept->ranks.push(0);
            kept->ranked.push(false);
        }
        if (at == kPrUsdNone && before != kPrUsdNone)
            kept->ranks.data[node] = kept->ranks.data[before] + 1;
        else
            kept->ranked.data[parent] = false;
    }
}

// Takes a node out of its parent's children.
void unlinkNode(Indexer* ix, uint32_t node) {
    UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    uint32_t parent = g->nodes.data[node].parent;
    uint32_t* link = &g->nodes.data[parent].firstChild;
    while (*link != node)
        link = &g->nodes.data[*link].nextSibling;
    *link = g->nodes.data[node].nextSibling;
    g->nodes.data[node].nextSibling = kPrUsdNone;
    if (parent < kept->lastLinked.count && kept->lastLinked.data[parent] == node)
        kept->lastLinked.data[parent] = kPrUsdNone;
}

// Returns true if a node of the composition's graph that gives opinions has the site. The sites are kept by their keys,
// and the nodes that the graph has gained since the last question are added first. A node does not become a placeholder
// while its graph composes, so a site that was added stays one.
bool graphHasSite(Indexer* ix, uint32_t layerStack, UsdPath path) {
    const UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    for (; kept->sitesCount < g->nodes.count; ++kept->sitesCount) {
        const UsdNode& n = g->nodes.data[kept->sitesCount];
        uint64_t key = uint64_t(n.layerStack) << 32 | n.path;
        if ((n.flags & UsdNodeFlag_Inert) == 0 && kept->sites.find(key) == kPrUsdNone)
            kept->sites.add(key, kept->sitesCount);
    }
    return kept->sites.find(uint64_t(layerStack) << 32 | path) != kPrUsdNone;
}

// Returns true if a node gives no opinions and nothing under it does, so that the prims below this one never need it.
// `copies` gives the copy at the root of each specializes placeholder, by node, or kPrUsdNone, for a caller that asks
// of many nodes. Without it the copy of a placeholder is searched for.
bool canCull(const UsdComposer* c, const UsdGraph* g, uint32_t node, uint32_t rootStack,
        const RawArray<uint32_t>* PR_NULLABLE copies) {
    const UsdNode& n = g->nodes.data[node];
    if ((n.flags & UsdNodeFlag_Culled) != 0)
        return true;
    // The node of an arc is kept where the arc was introduced, so that the arc can be found.
    if (n.parent == kPrUsdNone || depthBelowIntroduction(c, g, node) == 0)
        return false;
    // An inherit of a prim below a root prim is a prim of the stage, in the root layer stack.
    if (n.arc == PrUsdArc_Inherit && n.layerStack == rootStack) {
        uint32_t origin = node;
        while (g->nodes.data[origin].origin != g->nodes.data[origin].parent)
            origin = g->nodes.data[origin].origin;
        if (primDepth(c, pathAtIntroduction(c, g, origin)) != 1)
            return false;
    }
    for (uint32_t child = n.firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if ((g->nodes.data[child].flags & UsdNodeFlag_Culled) == 0)
            return false;
    }
    // The placeholder of a specializes arc says where the classes under the arc's copy at the root are implied, so it
    // stays while the copy does.
    if (n.arc == PrUsdArc_Specialize && n.parent != 0 && (n.flags & UsdNodeFlag_Inert) != 0) {
        uint32_t copy = copies != nullptr ? copies->data[node] : specializeCopy(g, node);
        if (copy != kPrUsdNone && copy != node && (g->nodes.data[copy].flags & UsdNodeFlag_Culled) == 0)
            return false;
    }
    return !((n.flags & UsdNodeFlag_HasSpecs) != 0 && usdNodeContributes(n));
}

void cullSubtrees(const UsdComposer* c, UsdGraph* g, uint32_t node, uint32_t rootStack,
        const RawArray<uint32_t>* PR_NULLABLE copies) {
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling)
        cullSubtrees(c, g, child, rootStack, copies);
    if (canCull(c, g, node, rootStack, copies))
        g->nodes.data[node].flags |= UsdNodeFlag_Culled;
}

void elideSubtree(UsdGraph* g, uint32_t node) {
    g->nodes.data[node].flags |= UsdNodeFlag_Culled | UsdNodeFlag_Elided;
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling)
        elideSubtree(g, child);
}

// Removes the culled nodes of a graph, and puts the others in strength order.
void finalizeGraph(UsdGraph* g) {
    RawArray<uint32_t> order{};
    RawArray<uint32_t> newIndex{};
    newIndex.resize(g->nodes.count);
    for (uint32_t i = 0; i < g->nodes.count; ++i)
        newIndex.data[i] = kPrUsdNone;
    RawArray<uint32_t> pending{};
    pending.push(0);
    while (pending.count != 0) {
        uint32_t node = pending.data[--pending.count];
        newIndex.data[node] = order.count;
        order.push(node);
        // The children are pushed weakest first, so that the strongest is next.
        uint32_t first = pending.count;
        for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child =
                g->nodes.data[child].nextSibling) {
            if ((g->nodes.data[child].flags & UsdNodeFlag_Culled) == 0)
                pending.push(child);
        }
        reverseItems(Span<uint32_t>(pending.data + first, pending.count - first));
    }
    RawArray<UsdNode> nodes{};
    nodes.resize(order.count);
    for (uint32_t i = 0; i < order.count; ++i) {
        UsdNode n = g->nodes.data[order.data[i]];
        n.parent = n.parent == kPrUsdNone ? kPrUsdNone : newIndex.data[n.parent];
        // An origin that was culled leaves the arc as one of its parent's own.
        n.origin = n.origin == kPrUsdNone ? kPrUsdNone : newIndex.data[n.origin];
        if (n.origin == kPrUsdNone)
            n.origin = n.parent;
        uint32_t child = n.firstChild;
        while (child != kPrUsdNone && newIndex.data[child] == kPrUsdNone)
            child = g->nodes.data[child].nextSibling;
        n.firstChild = child == kPrUsdNone ? kPrUsdNone : newIndex.data[child];
        uint32_t next = n.nextSibling;
        while (next != kPrUsdNone && newIndex.data[next] == kPrUsdNone)
            next = g->nodes.data[next].nextSibling;
        n.nextSibling = next == kPrUsdNone ? kPrUsdNone : newIndex.data[next];
        nodes.data[i] = n;
    }
    g->nodes.reset();
    g->nodes = nodes;
    order.reset();
    newIndex.reset();
    pending.reset();
}

// Visits the nodes of a graph, strongest first, with whether a prim under an instance may take opinions from each
// (usd-core/#instancing). `visit` returns false to pass over a node's children.
template <typename Visit>
void walkShared(const UsdComposer* c, const UsdGraph* g, uint32_t node, bool direct, Visit& visit) {
    const UsdNode& n = g->nodes.data[node];
    direct = direct || (n.parent != kPrUsdNone && depthBelowIntroduction(c, g, node) == 0);
    bool shared = n.parent != kPrUsdNone && direct && (n.flags & UsdNodeFlag_HasSpecs) != 0;
    if (!visit(node, shared))
        return;
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling)
        walkShared(c, g, child, direct, visit);
}

// ---------------------------------------------------------------------------------------------------------------------
// Tasks

void heapPush(RawArray<uint64_t>* heap, uint64_t key) {
    uint32_t at = heap->count;
    heap->push(key);
    while (at != 0 && heap->data[(at - 1) / 2] > key) {
        heap->data[at] = heap->data[(at - 1) / 2];
        at = (at - 1) / 2;
    }
    heap->data[at] = key;
}

uint64_t heapPop(RawArray<uint64_t>* heap) {
    uint64_t top = heap->data[0];
    uint64_t last = heap->data[--heap->count];
    uint32_t at = 0;
    for (;;) {
        uint32_t child = at * 2 + 1;
        if (child >= heap->count)
            break;
        if (child + 1 < heap->count && heap->data[child + 1] < heap->data[child])
            ++child;
        if (heap->data[child] >= last)
            break;
        heap->data[at] = heap->data[child];
        at = child;
    }
    if (heap->count != 0)
        heap->data[at] = last;
    return top;
}

// Queues a task of a node. The tasks of one kind run in the order of their nodes, and those that imply classes in the
// reverse order, so that a node's classes are implied before its ancestors'.
void pushTask(Indexer* ix, uint32_t kind, uint32_t node) {
    uint32_t order = kind == Task_ImpliedClasses ? 0xffffffffu - node : node;
    heapPush(&ix->heap, uint64_t(kind) << 32 | order);
}

// Returns the node where implying the classes that `node` is part of begins: the node that inherits the hierarchy of
// classes, or the node that inherits the hierarchy that this one is inherited in turn through.
uint32_t impliedClassStart(const UsdComposer* c, const UsdGraph* g, uint32_t node) {
    uint32_t start = node;
    while (isClassArc(g->nodes.data[start].arc)) {
        uint32_t depth = depthBelowIntroduction(c, g, start);
        uint32_t instance = start;
        uint32_t classNode = start;
        while (isClassArc(g->nodes.data[instance].arc) && depthBelowIntroduction(c, g, instance) == depth) {
            classNode = instance;
            instance = g->nodes.data[instance].parent;
        }
        start = instance;
        if (isClassArc(g->nodes.data[instance].arc) &&
                usdPathHasPrefix(&c->paths, g->nodes.data[classNode].path, pathAtIntroduction(c, g, instance)))
            break;
    }
    return start;
}

// Queues the tasks of a node and of the nodes under it. `composed` says that a nested composition made them, which
// evaluated their arcs and left their variants, and implied their classes as far as the node itself. Variants are
// selected in the graph that no composition nests, where every site that may author a selection is.
void addTasks(Indexer* ix, uint32_t node, bool composed, bool top = true) {
    const UsdGraph* g = ix->g;
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling)
        addTasks(ix, child, composed, false);
    if (!composed) {
        pushTask(ix, Task_Relocations, node);
        if (g->nodes.data[node].arc == PrUsdArc_Relocate)
            pushTask(ix, Task_ImpliedRelocations, node);
        pushTask(ix, Task_References, node);
        pushTask(ix, Task_Payloads, node);
        pushTask(ix, Task_Inherits, node);
        pushTask(ix, Task_Specializes, node);
    }
    if (ix->previous == nullptr)
        pushTask(ix, Task_VariantSets, node);
    if (composed && !top)
        return;
    if (isClassArc(g->nodes.data[node].arc))
        pushTask(ix, Task_ImpliedClasses, impliedClassStart(ix->c, g, node));
    else if (hasClassChild(g, node))
        pushTask(ix, Task_ImpliedClasses, node);
}

// What adding an arc does besides adding its node.
struct ArcOptions {
    bool contributes = true;        // false for a placeholder, whose own site gives no opinions
    bool ancestral = false;         // the site's ancestors compose too, as for an arc to a prim below a root prim
    bool skipDuplicates = false;    // add nothing if a node already gives the site's opinions
};

void buildGraph(UsdComposer* c, uint32_t layerStack, UsdPath path, Frame* frame, bool rootContributes, uint32_t nesting,
    UsdPath errorPrim, UsdGraph* g);

// Adds the node of an arc to the graph, with the nodes of the site's ancestors if the options ask, and queues its
// tasks. Returns the node, or kPrUsdNone if the arc is a cycle or a duplicate.
uint32_t addArc(Indexer* ix, uint8_t arc, uint32_t parent, uint32_t origin, uint32_t layerStack, UsdPath path,
        UsdMapId map, uint16_t siblingNum, uint32_t namespaceDepth, ArcOptions options) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    const UsdPathTable* paths = &c->paths;
    // An arc to a site that the arc's own site was reached from, or to an ancestor or a descendant of one, never ends.
    // The nodes of a nested composition continue in the graph of the composition that nests it, where the site is the
    // prim that was asked for, with the site in place of the prim that is being composed. A placeholder under a
    // relocate's node is at the path before the relocate, where no site repeats. A graph also has a bound on its depth,
    // since layers from outside the program decide it.
    {
        bool check = g->nodes.data[parent].arc != PrUsdArc_Relocate && arc != PrUsdArc_Variant;
        bool cycle = false;
        uint32_t depth = 0;
        UsdPath site = stripped(c, path);
        const Indexer* at = ix;
        for (uint32_t node = parent; !cycle && site != kUsdPathNone;) {
            for (; !cycle && node != kPrUsdNone; node = at->g->nodes.data[node].parent) {
                const UsdNode& above = at->g->nodes.data[node];
                UsdPath other = stripped(c, above.path);
                cycle = ++depth > kMaxGraphDepth || (check && above.layerStack == layerStack &&
                    (usdPathHasPrefix(paths, other, site) || usdPathHasPrefix(paths, site, other)));
            }
            if (at->previous == nullptr)
                break;
            site = usdPathReplacePrefix(&c->paths, at->previous->requestedPath, at->g->nodes.data[0].path, site);
            node = at->previous->parentNode;
            at = at->previous->caller;
        }
        if (cycle) {
            Sentence sentence{c};
            sentence.add("the arc from ").site(g->nodes.data[parent].layerStack, g->nodes.data[parent].path).add(" to ")
                .site(layerStack, path).add(depth > kMaxGraphDepth ? " is deeper than a stage follows" : " is a cycle")
                .report(PrUsdErrorKind_ArcCycle, ix->errorPrim);
            return kPrUsdNone;
        }
    }
    // A path at or under the source of a relocate is no prim's in its layer stack: the prim is at the target. Only the
    // relocate itself reaches it.
    if (arc != PrUsdArc_Relocate && arc != PrUsdArc_Variant) {
        const UsdLayerStack& stack = c->layerStacks.data[layerStack];
        for (uint32_t i = 0; i < stack.relocateCount; ++i) {
            if (!usdPathHasPrefix(paths, path, c->relocates.data[stack.firstRelocate + i].source))
                continue;
            Sentence sentence{c};
            sentence.add("the arc from ").site(g->nodes.data[parent].layerStack, g->nodes.data[parent].path).add(" to ")
                .site(layerStack, path).add(" is ignored: a relocate moved the prim from that path")
                .report(PrUsdErrorKind_ArcInvalidPath, ix->errorPrim);
            return kPrUsdNone;
        }
    }
    bool skipDuplicates = options.skipDuplicates || (ix->previous != nullptr && ix->previous->skipDuplicates);
    if (skipDuplicates) {
        UsdPath site = path;
        for (Indexer* at = ix; site != kUsdPathNone;) {
            if (graphHasSite(at, layerStack, site))
                return kPrUsdNone;
            if (at->previous == nullptr)
                break;
            site = usdPathReplacePrefix(&c->paths, at->previous->requestedPath, at->g->nodes.data[0].path, site);
            at = at->previous->caller;
        }
    }

    UsdNode n{};
    n.path = path;
    n.layerStack = layerStack;
    n.mapToParent = map;
    n.mapToRoot = usdMapCompose(&c->maps, g->nodes.data[parent].mapToRoot, map);
    n.chain = usdMapChainAdd(&c->maps, map, g->nodes.data[parent].chain);
    n.parent = parent;
    n.origin = origin;
    n.firstChild = kPrUsdNone;
    n.nextSibling = kPrUsdNone;
    n.namespaceDepth = namespaceDepth;
    n.siblingNum = siblingNum;
    n.arc = arc;
    uint32_t node = g->nodes.count;
    if (!options.ancestral) {
        n.flags = uint8_t((options.contributes ? 0 : UsdNodeFlag_Inert) |
            (hasSpecs(c, layerStack, path) ? UsdNodeFlag_HasSpecs : 0));
        g->nodes.push(n);
    } else {
        if (ix->nesting >= kMaxNesting) {
            Sentence sentence{c};
            sentence.add("the arc to ").site(layerStack, path).add(" nests deeper than a stage follows")
                .report(PrUsdErrorKind_ArcCycle, ix->errorPrim);
            return kPrUsdNone;
        }
        Frame frame{ix, parent, path, arc, map, origin, namespaceDepth, siblingNum, layerStack, skipDuplicates};
        UsdGraph nested{};
        buildGraph(c, layerStack, path, &frame, options.contributes, ix->nesting + 1, ix->errorPrim, &nested);
        // The nested graph's root takes the arc, and its nodes follow this graph's.
        for (uint32_t i = 0; i < nested.nodes.count; ++i) {
            UsdNode moved = nested.nodes.data[i];
            auto shift = [&](uint32_t index) { return index == kPrUsdNone ? kPrUsdNone : index + node; };
            moved.parent = shift(moved.parent);
            moved.origin = shift(moved.origin);
            moved.firstChild = shift(moved.firstChild);
            moved.nextSibling = shift(moved.nextSibling);
            if (i == 0) {
                n.path = moved.path;
                n.flags = moved.flags;
                n.restrictedDepth = moved.restrictedDepth;
                n.firstChild = moved.firstChild;
                moved = n;
            } else {
                moved.mapToRoot = usdMapCompose(&c->maps, g->nodes.data[moved.parent].mapToRoot, moved.mapToParent);
                moved.chain = usdMapChainAdd(&c->maps, moved.mapToParent, g->nodes.data[moved.parent].chain);
            }
            g->nodes.push(moved);
        }
        g->hasPayloads = g->hasPayloads || nested.hasPayloads;
        nested.nodes.reset();
    }
    linkNode(ix, node);
    if (canCull(c, g, node, ix->rootStack, nullptr)) {
        g->nodes.data[node].flags |= UsdNodeFlag_Culled;
    } else {
        for (uint32_t up = parent; up != kPrUsdNone && (g->nodes.data[up].flags & UsdNodeFlag_Culled) != 0;
                up = g->nodes.data[up].parent)
            g->nodes.data[up].flags &= uint8_t(~UsdNodeFlag_Culled);
    }
    addTasks(ix, node, options.ancestral);
    if (arc == PrUsdArc_Relocate)
        pushTask(ix, Task_ImpliedRelocations, node);
    if (options.ancestral && ix->previous == nullptr) {
        // A nested composition selects no variant. The variant sets of the ancestors of each site that it brought are
        // selected here, where the arcs that the layer stacks above imply are too.
        for (uint32_t at = node; at < g->nodes.count; ++at) {
            if (skipDuplicates)
                g->nodes.data[at].flags |= UsdNodeFlag_Unique;
            pushTask(ix, Task_AncestralVariantSets, at);
        }
        // A nested composition leaves the specializes arcs that it finds as placeholders. Each has its copy at the root
        // of the graph that no composition nests, which is this one.
        uint32_t end = g->nodes.count;
        for (uint32_t at = node + 1; at < end; ++at) {
            UsdNode held = g->nodes.data[at];
            if (held.arc != PrUsdArc_Specialize)
                continue;
            ArcOptions copy;
            copy.ancestral = primDepth(c, held.path) > 1;
            copy.skipDuplicates = true;
            addArc(ix, PrUsdArc_Specialize, 0, at, held.layerStack, held.path, held.mapToRoot, held.siblingNum,
                primDepth(c, g->nodes.data[0].path), copy);
        }
    }
    return node;
}

// Evaluates the relocate whose target is the node's site (usd-core/#relocates): the opinions of the source take the
// place of those that the node's ancestors brought.
void evalRelocations(Indexer* ix, uint32_t node) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    UsdNode n = g->nodes.data[node];
    if (!usdNodeContributes(n) && depthBelowIntroduction(c, g, node) == 0)
        return;
    // A node that a relocate above it replaced gives nothing, and neither does a relocate to its site.
    if ((n.flags & UsdNodeFlag_Elided) != 0)
        return;
    const UsdStackRelocate* relocate = findRelocate(c, n.layerStack, n.path, true);
    if (relocate == nullptr)
        return;
    UsdPath source = relocate->source;
    // What ancestors brought to the target is replaced, except what variants hold, which may override a relocated prim.
    for (uint32_t child = n.firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if (g->nodes.data[child].arc != PrUsdArc_Variant)
            elideSubtree(g, child);
    }
    ArcOptions options;
    options.contributes = false;
    options.ancestral = true;
    uint32_t added = addArc(ix, PrUsdArc_Relocate, node, node, n.layerStack, source, kUsdMapIdentity, 0,
        primDepth(c, n.path), options);
    if (added == kPrUsdNone)
        return;
    const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
    for (uint32_t i = 0; i < stack.layerCount; ++i) {
        uint32_t layer = c->stackLayers.data[stack.firstLayer + i].layer;
        if (usdComposerSpec(c, layer, source) != kPrUsdNone) {
            Sentence sentence{c};
            sentence.add("the spec at ").path(source).add(" in @").add(c->layers.data[layer].name)
                .add("@ is ignored: the path is the source of a relocate")
                .report(PrUsdErrorKind_OpinionAtRelocateSource, ix->errorPrim);
        }
    }
}

// Implies a relocate in the layer stack above the one that authors it, as a placeholder that classes are implied
// through.
void evalImpliedRelocations(Indexer* ix, uint32_t node) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    UsdNode n = g->nodes.data[node];
    if (n.arc != PrUsdArc_Relocate || depthBelowIntroduction(c, g, node) != 0)
        return;
    UsdNode parent = g->nodes.data[n.parent];
    if (parent.parent == kPrUsdNone)
        return;
    UsdPath source = usdMapToTarget(&c->maps, parent.mapToParent, stripped(c, n.path));
    if (source == kUsdPathNone)
        return;
    UsdNode above = g->nodes.data[parent.parent];
    for (uint32_t child = above.firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if (g->nodes.data[child].path == source && g->nodes.data[child].arc == PrUsdArc_Relocate)
            return;
    }
    ArcOptions options;
    options.contributes = false;
    addArc(ix, PrUsdArc_Relocate, parent.parent, node, above.layerStack, source, kUsdMapIdentity, 0,
        primDepth(c, above.path), options);
}

// Returns true if the payloads of the prim being composed are loaded (usd-core/#payloads).
bool payloadsLoad(const Indexer* ix) {
    const UsdComposer* c = ix->c;
    // The payloads of the ancestors of an arc's target always load.
    if (ix->previous != nullptr || c->load == PrUsdLoad_All)
        return true;
    UsdPath prim = ix->g->nodes.data[0].path;
    for (uint32_t i = 0; i < c->loadPaths.count; ++i) {
        if (usdPathHasPrefix(&c->paths, prim, c->loadPaths.data[i]) || usdPathHasPrefix(&c->paths, c->loadPaths.data[i], prim))
            return true;
    }
    return false;
}

// Evaluates the references or the payloads that the node's site authors (usd-core/#references).
void evalRefs(Indexer* ix, uint32_t node, uint8_t arc) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    UsdNode n = g->nodes.data[node];
    if (!usdNodeContributes(n) || (n.flags & UsdNodeFlag_HasSpecs) == 0)
        return;
    RawArray<RefArc> refs{};
    PrStringView field = "references";
    PrStringView what = "the reference of ";
    if (arc == PrUsdArc_Payload) {
        field = "payload";
        what = "the payload of ";
    }
    composeRefs(c, n.layerStack, n.path, field, &refs);
    if (arc == PrUsdArc_Payload && refs.count != 0) {
        g->hasPayloads = true;
        if (!payloadsLoad(ix))
            refs.count = 0;
    }
    for (uint32_t i = 0; i < refs.count; ++i) {
        RefArc ref = refs.data[i];
        Sentence sentence{c};
        sentence.add(what).site(n.layerStack, n.path);
        if (ref.invalidOffset) {
            Sentence scale{c};
            scale.add(what).site(n.layerStack, n.path).add(" has an offset whose scale is not positive")
                .report(PrUsdErrorKind_InvalidOffset, ix->errorPrim);
        }
        uint32_t layerStack = n.layerStack;
        PrUsdLayerOffset offset = ref.offset;
        if (ref.location != kAtomNone) {
            PrStatus status = PrStatus_Ok;
            uint32_t layer = openLayer(c, atomTableText(&c->strings, ref.location), &status);
            if (layer == kPrUsdNone) {
                sentence.add(" names the layer @").add(atomTableText(&c->strings, ref.location))
                    .add("@, which cannot be read").report(PrUsdErrorKind_ArcUnreadable, ix->errorPrim);
                continue;
            }
            layerStack = layerStackOf(c, layer);
        }
        // The times of the arc's target are scaled from the time scale of its layer stack to that of the layer that
        // authors the arc, whose own retiming takes them on to its layer stack's
        // (usd-core/#reference-and-payload-offset-adjustment). An arc within the layer stack is scaled too.
        double factor = ref.timeCodesPerSecond / c->layerStacks.data[layerStack].timeCodesPerSecond;
        if (!(factor > 0.0) || !prF64IsFinite(factor)) {
            factor = 1.0;
            Sentence scale{c};
            scale.add(what).site(n.layerStack, n.path).add(" is to a layer stack whose time codes per second, or ")
                .add("those of the layer that authors it, are not positive")
                .report(PrUsdErrorKind_InvalidOffset, ix->errorPrim);
        }
        offset.scale *= factor;
        UsdPath prim = ref.prim;
        if (ref.invalidPath) {
            sentence.add(" names a path that is not a prim's").report(PrUsdErrorKind_ArcInvalidPath, ix->errorPrim);
            continue;
        }
        if (prim == kUsdPathNone) {
            // The default prim of the layer, which names a root prim or gives a path.
            uint32_t root = c->layerStacks.data[layerStack].root;
            const PrUsdField* named = prUsdLayerFindField(c->layers.data[root].layer, kPrUsdLayerSpec, "defaultPrim");
            PrStringView name{};
            if (named != nullptr && (named->value.type == PrUsdType_Token || named->value.type == PrUsdType_String) &&
                    !named->value.isArray)
                name = *static_cast<const PrStringView*>(named->value.data);
            if (name.size != 0 && name.data[0] == '/')
                prim = usdPathParse(&c->paths, name);
            else if (name.size != 0 && usdNameIsIdentifier(name))
                prim = usdPathChild(&c->paths, kUsdPathRoot, name);
            if (!isPrimPath(c, prim)) {
                sentence.add(" names no prim, and the layer @").add(c->layers.data[root].name)
                    .add("@ has no default prim").report(PrUsdErrorKind_ArcInvalidPath, ix->errorPrim);
                continue;
            }
        }
        prTextBufferReset(&sentence.text);
        UsdMapId map = arcMap(c, n.layerStack, prim, stripped(c, n.path), offset);
        if (ref.location == kAtomNone)
            map = usdMapAddRootIdentity(&c->maps, map);
        ArcOptions options;
        options.ancestral = primDepth(c, prim) > 1;
        uint32_t added = addArc(ix, arc, node, node, layerStack, prim, map, uint16_t(i < 0xffffu ? i : 0xffffu),
            primDepth(c, n.path), options);
        if (added != kPrUsdNone)
            pushTask(ix, Task_Unresolved, added);
    }
    refs.reset();
}

// Reports a reference or a payload whose target has no spec, at its site or by any arc of its own.
void evalUnresolved(Indexer* ix, uint32_t node) {
    const UsdGraph* g = ix->g;
    RawArray<uint32_t> pending{};
    pending.push(node);
    bool found = false;
    while (!found && pending.count != 0) {
        uint32_t at = pending.data[--pending.count];
        found = (g->nodes.data[at].flags & UsdNodeFlag_HasSpecs) != 0;
        for (uint32_t child = g->nodes.data[at].firstChild; child != kPrUsdNone; child =
                g->nodes.data[child].nextSibling)
            pending.push(child);
    }
    pending.reset();
    if (found)
        return;
    const UsdNode& n = g->nodes.data[node];
    const UsdNode& parent = g->nodes.data[n.parent];
    Sentence sentence{ix->c};
    PrStringView what = "the reference of ";
    if (n.arc == PrUsdArc_Payload)
        what = "the payload of ";
    sentence.add(what).site(parent.layerStack, parent.path).add(" names the prim ").site(n.layerStack, n.path)
        .add(", which no layer has a spec for").report(PrUsdErrorKind_ArcUnresolved, ix->errorPrim);
}

uint64_t classOriginKey(uint32_t parent, uint32_t origin, UsdMapId map) {
    return prHashMixKeys(uint64_t(parent) << 32 | origin, map);
}

uint64_t classSiteKey(uint32_t parent, uint32_t layerStack, UsdPath path, uint8_t arc) {
    return prHashMixKeys(uint64_t(parent) << 32 | path, uint64_t(layerStack) << 8 | arc);
}

// Brings the composition's keys of its inherits and specializes nodes up to the nodes that its graph has. A key keeps
// the first node that has it, and the searches that use the keys fall back on a walk where that node is not the one.
void indexClasses(Indexer* ix) {
    const UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    for (; kept->classCount < g->nodes.count; ++kept->classCount) {
        const UsdNode& n = g->nodes.data[kept->classCount];
        if (!isClassArc(n.arc) || n.parent == kPrUsdNone)
            continue;
        uint64_t byOrigin = classOriginKey(n.parent, n.origin, n.mapToParent);
        if (kept->classOfOrigin.find(byOrigin) == kPrUsdNone)
            kept->classOfOrigin.add(byOrigin, kept->classCount);
        if (isSpecializeCopy(g, kept->classCount) && kept->copyOfPlaceholder.find(n.origin) == kPrUsdNone)
            kept->copyOfPlaceholder.add(n.origin, kept->classCount);
        uint64_t bySite = classSiteKey(n.parent, n.layerStack, n.path, n.arc);
        if (kept->classOfSite.find(bySite) == kPrUsdNone)
            kept->classOfSite.add(bySite, kept->classCount);
    }
}

// Returns the copy at the root of a specializes placeholder, or the node itself if it has none, by the composition's
// keys.
uint32_t specializeCopyOf(Indexer* ix, uint32_t node) {
    const UsdGraph* g = ix->g;
    IndexerKept* kept = keptOf(ix);
    const UsdNode& n = g->nodes.data[node];
    if (n.arc != PrUsdArc_Specialize || n.parent == 0)
        return node;
    indexClasses(ix);
    uint32_t found = kept->copyOfPlaceholder.find(node);
    if (found == kPrUsdNone)
        return node;
    if (g->nodes.data[found].origin == node && isSpecializeCopy(g, found))
        return found;
    return specializeCopy(g, node);
}

// Adds an inherits or a specializes arc from `parent` to the site that `map` gives its path, unless the parent has it.
// `origin` is the node that implies the arc, or the parent for an authored arc. Returns the arc's node, or kPrUsdNone.
uint32_t addClassArc(Indexer* ix, uint8_t arc, uint32_t parent, uint32_t origin, UsdMapId map, uint16_t siblingNum,
        uint32_t ignoreStack, UsdPath ignorePath) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    UsdNode p = g->nodes.data[parent];
    // The path of the class in the parent's namespace. A class outside the namespace that the arcs above map has no
    // site here, which is not an error. The mappings of arcs lead to where relocates put a prim, so a parent at the
    // source of a relocate maps by its target.
    UsdPath mapped = stripped(c, p.path);
    const UsdLayerStack& stack = c->layerStacks.data[p.layerStack];
    for (uint32_t i = 0; i < stack.relocateCount; ++i) {
        const UsdStackRelocate& relocate = c->relocates.data[stack.firstRelocate + i];
        if (relocate.target != kUsdPathNone && usdPathHasPrefix(&c->paths, mapped, relocate.source)) {
            mapped = usdPathReplacePrefix(&c->paths, mapped, relocate.source, relocate.target);
            break;
        }
    }
    UsdPath path = usdMapToSource(&c->maps, map, mapped);
    if (path == kUsdPathNone || !isPrimPath(c, path))
        return kPrUsdNone;
    uint32_t below = depthBelowIntroduction(c, g, origin);
    auto same = [&](uint32_t child) {
        const UsdNode& other = g->nodes.data[child];
        return other.parent == parent && other.layerStack == p.layerStack && other.path == path && other.arc == arc &&
            other.mapToParent == map && depthBelowIntroduction(c, g, other.origin) == below;
    };
    // The parent's node of the class is found by its key. Where the key's node is another arc to the same path, the
    // parent's children are searched.
    indexClasses(ix);
    IndexerKept* kept = keptOf(ix);
    uint32_t existing = kept->classOfSite.find(classSiteKey(parent, p.layerStack, path, arc));
    if (existing != kPrUsdNone && !same(existing)) {
        existing = kPrUsdNone;
        for (uint32_t child = p.firstChild; existing == kPrUsdNone && child != kPrUsdNone;
                child = g->nodes.data[child].nextSibling) {
            if (same(child))
                existing = child;
        }
    }
    if (existing != kPrUsdNone) {
        // Two arcs may imply one class here. It is implied from the stronger of them, whichever was met first.
        const UsdNode& other = g->nodes.data[existing];
        if (other.origin != parent && origin != other.origin && compareNodes(ix, origin, other.origin) < 0) {
            unlinkNode(ix, existing);
            g->nodes.data[existing].origin = origin;
            g->nodes.data[existing].siblingNum = siblingNum;
            linkNode(ix, existing);
            if (kept->classOfOrigin.find(classOriginKey(parent, origin, map)) == kPrUsdNone)
                kept->classOfOrigin.add(classOriginKey(parent, origin, map), existing);
        }
        return existing;
    }
    uint32_t namespaceDepth = primDepth(c, p.path);
    if (ignoreStack == p.layerStack && ignorePath == path) {
        // A class implied onto its own site gives its opinions where it is, and here it is a placeholder that the layer
        // stacks above imply it through.
        ArcOptions placeholder;
        placeholder.contributes = false;
        return addArc(ix, arc, parent, origin, p.layerStack, path, map, siblingNum, namespaceDepth, placeholder);
    }
    ArcOptions options;
    options.ancestral = primDepth(c, path) > 1;
    bool nested = ix->previous != nullptr;
    if (arc == PrUsdArc_Specialize && (parent != 0 || nested)) {
        // A specializes arc below the root leaves a placeholder where it is authored or implied, and the arc itself is
        // a child of the root, where it is weaker than every other arc (usd-core/#specializes). The root is that of the
        // graph that no composition nests, which makes the copies of a nested graph's placeholders when the graph joins
        // it.
        ArcOptions placeholder;
        placeholder.contributes = false;
        uint32_t held =
            addArc(ix, arc, parent, origin, p.layerStack, path, map, siblingNum, namespaceDepth, placeholder);
        if (held == kPrUsdNone || nested)
            return held;
        options.skipDuplicates = true;
        addArc(ix, arc, 0, held, p.layerStack, path, usdMapCompose(&c->maps, p.mapToRoot, map), siblingNum,
            primDepth(c, g->nodes.data[0].path), options);
        return held;
    }
    options.skipDuplicates = true;
    return addArc(ix, arc, parent, origin, p.layerStack, path, map, siblingNum, namespaceDepth, options);
}

// Evaluates the inherits or the specializes that the node's site authors (usd-core/#inherits).
void evalClasses(Indexer* ix, uint32_t node, uint8_t arc) {
    UsdComposer* c = ix->c;
    UsdNode n = ix->g->nodes.data[node];
    if (!usdNodeContributes(n) || (n.flags & UsdNodeFlag_HasSpecs) == 0)
        return;
    RawArray<uint64_t> classes{};
    PrStringView field = "inheritPaths";
    if (arc == PrUsdArc_Specialize)
        field = "specializes";
    composePaths(c, n.layerStack, n.path, field, &classes);
    for (uint32_t i = 0; i < classes.count; ++i) {
        UsdPath path = UsdPath(classes.data[i]);
        if (!isPrimPath(c, path)) {
            Sentence sentence{c};
            sentence.add("an inherits or specializes of ").site(n.layerStack, n.path).add(" names the path ").path(path)
                .add(", which is not a prim's").report(PrUsdErrorKind_ArcInvalidPath, ix->errorPrim);
            continue;
        }
        // The class maps to the prim, and every path outside them to itself.
        UsdMapId map = usdMapAddRootIdentity(&c->maps, arcMap(c, n.layerStack, path, stripped(c, n.path), kNoOffset));
        addClassArc(ix, arc, node, node, map, uint16_t(i < 0xffffu ? i : 0xffffu), kPrUsdNone, kUsdPathNone);
    }
    classes.reset();
}

// Implies the classes under `source` in `target`, a node whose namespace `transfer` maps the source's to: each class
// that the source inherits or specializes is one that the target does, at the path that the mapping gives it
// (usd-core/#inherits).
void implyClasses(Indexer* ix, uint32_t target, uint32_t source, UsdMapId transfer) {
    UsdComposer* c = ix->c;
    UsdGraph* g = ix->g;
    if (g->nodes.data[target].arc == PrUsdArc_Relocate) {
        // A relocate's node is at the path before the relocate, where the mappings of the arcs under it do not lead, so
        // the classes are implied in the node that was relocated.
        UsdMapId above = usdMapCompose(&c->maps, usdMapAddRootIdentity(&c->maps, g->nodes.data[target].mapToParent), transfer);
        implyClasses(ix, g->nodes.data[target].parent, source, above);
        pushTask(ix, Task_ImpliedClasses, target);
        return;
    }
    RawArray<uint32_t> children{};
    for (uint32_t child = g->nodes.data[source].firstChild; child != kPrUsdNone; child =
            g->nodes.data[child].nextSibling) {
        if (isClassArc(g->nodes.data[child].arc))
            children.push(child);
    }
    for (uint32_t i = 0; i < children.count; ++i) {
        uint32_t child = children.data[i];
        UsdNode s = g->nodes.data[child];
        // The class's mapping in the target's namespace.
        UsdMapId implied = s.mapToParent;
        if (transfer != kUsdMapIdentity) {
            implied = usdMapAddRootIdentity(&c->maps, usdMapCompose(&c->maps, transfer,
                usdMapCompose(&c->maps, s.mapToParent, usdMapInverse(&c->maps, transfer))));
        }
        auto isImplied = [&](uint32_t at) {
            const UsdNode& other = g->nodes.data[at];
            return other.parent == target && other.origin == child && other.mapToParent == implied;
        };
        // The target's node that the class implied is found by its key, or by a search where the key's node is not it.
        indexClasses(ix);
        uint32_t found = keptOf(ix)->classOfOrigin.find(classOriginKey(target, child, implied));
        if (found != kPrUsdNone && !isImplied(found)) {
            found = kPrUsdNone;
            for (uint32_t at = g->nodes.data[target].firstChild; found == kPrUsdNone && at != kPrUsdNone;
                    at = g->nodes.data[at].nextSibling) {
                if (isImplied(at))
                    found = at;
            }
        }
        if (found == kPrUsdNone) {
            // A class in the target's own layer stack is not implied onto itself.
            bool same = s.layerStack == g->nodes.data[target].layerStack;
            found = addClassArc(ix, s.arc, target, child, implied, s.siblingNum, same ? s.layerStack : kPrUsdNone,
                same ? s.path : kUsdPathNone);
        }
        if (found == kPrUsdNone)
            continue;
        // The classes of a specializes arc are under its copy at the root.
        uint32_t nextSource = specializeCopyOf(ix, child);
        uint32_t nextTarget = specializeCopyOf(ix, found);
        if (hasClassChild(g, nextSource)) {
            UsdMapId next = usdMapCompose(&c->maps, usdMapInverse(&c->maps, implied),
                usdMapCompose(&c->maps, transfer, s.mapToParent));
            implyClasses(ix, nextTarget, nextSource, next);
        }
    }
    children.reset();
}

void evalImpliedClasses(Indexer* ix, uint32_t node) {
    const UsdGraph* g = ix->g;
    const UsdNode& n = g->nodes.data[node];
    if (n.parent == kPrUsdNone || !hasClassChild(g, node))
        return;
    if (isSpecializeCopy(g, node)) {
        // The classes under a specializes arc at the root are implied through its placeholder, not through the root:
        // the node above the placeholder has them, as it would if the arc were where it was authored.
        const UsdNode& held = g->nodes.data[n.origin];
        if ((held.flags & UsdNodeFlag_Elided) == 0)
            implyClasses(ix, held.parent, node, usdMapAddRootIdentity(&ix->c->maps, held.mapToParent));
        return;
    }
    UsdMapId transfer = usdMapAddRootIdentity(&ix->c->maps, n.mapToParent);
    implyClasses(ix, n.parent, node, transfer);
}

// Finds the selection of a variant set that a node's site authors for `path`, a path of the node's namespace.
bool selectionAtNode(const Indexer* ix, uint32_t node, UsdPath path, Atom variantSet, Atom* selection) {
    UsdComposer* c = ix->c;
    const UsdNode& n = ix->g->nodes.data[node];
    // A node that was culled for want of specs at the prim's path may still author a selection of an ancestor's.
    if ((n.flags & (UsdNodeFlag_Inert | UsdNodeFlag_Elided)) != 0)
        return false;
    UsdPath site = path;
    if (n.arc == PrUsdArc_Variant) {
        // A variant's specs are at the path with its selection.
        UsdPath introduced = pathAtIntroduction(c, ix->g, node);
        UsdPath inVariant = usdPathReplacePrefix(&c->paths, path, stripped(c, introduced), introduced);
        if (inVariant != kUsdPathNone)
            site = inVariant;
    }
    PrStringView name = atomTableText(&c->paths.names, variantSet);
    const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
    for (uint32_t i = 0; i < stack.layerCount; ++i) {
        const PrUsdField* field = fieldAt(c, c->stackLayers.data[stack.firstLayer + i].layer, site, "variantSelection");
        if (field == nullptr || field->value.type != PrUsdType_VariantSelection)
            continue;
        const PrUsdVariantSelection* items = static_cast<const PrUsdVariantSelection*>(field->value.data);
        for (uint32_t j = 0; j < field->value.count; ++j) {
            if (items[j].variantSet == name) {
                *selection = atomTableAdd(&c->paths.names, items[j].variant);
                return true;
            }
        }
    }
    return false;
}

// Searches a node and the nodes under it, strongest first, for a selection of a variant set of `path`, a path of the
// node's namespace.
bool searchSelection(const Indexer* ix, uint32_t node, UsdPath path, Atom variantSet, Atom* selection) {
    UsdComposer* c = ix->c;
    const UsdGraph* g = ix->g;
    if (selectionAtNode(ix, node, path, variantSet, selection))
        return true;
    for (uint32_t child = g->nodes.data[node].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        const UsdNode& n = g->nodes.data[child];
        UsdPath inner = n.arc == PrUsdArc_Variant ? path : usdMapToSource(&c->maps, n.mapToParent, path);
        if (inner != kUsdPathNone && searchSelection(ix, child, inner, variantSet, selection))
            return true;
    }
    return false;
}

// Searches a node and the nodes under it for the variant that the graph already has of a variant set of `path`, a path
// of the node's namespace: a variant's node whose site, where it was introduced, is the path.
bool searchPrior(const Indexer* ix, uint32_t node, UsdPath path, Atom variantSet, Atom* selection) {
    UsdComposer* c = ix->c;
    const UsdGraph* g = ix->g;
    const UsdNode& n = g->nodes.data[node];
    if (n.arc == PrUsdArc_Variant && (n.flags & UsdNodeFlag_Elided) == 0) {
        UsdPath introducedPath = pathAtIntroduction(c, g, node);
        const UsdPathNode& introduced = usdPathNode(&c->paths, introducedPath);
        if (introduced.part == UsdPathPart_Variant && introduced.name == variantSet && stripped(c, introducedPath) == path) {
            *selection = introduced.variant;
            return true;
        }
    }
    for (uint32_t child = n.firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        const UsdNode& below = g->nodes.data[child];
        UsdPath inner = below.arc == PrUsdArc_Variant ? path : usdMapToSource(&c->maps, below.mapToParent, path);
        if (inner != kUsdPathNone && searchPrior(ix, child, inner, variantSet, selection))
            return true;
    }
    return false;
}

// Computes the selection of a variant set that `path` lists, a path of the node's namespace without its variant parts
// (usd-core/#computing-variant-selection): the variant that the graph already has of the set for the prim, else the
// strongest opinion of any site of the prim.
bool composeSelection(Indexer* ix, uint32_t node, UsdPath path, Atom variantSet, Atom* selection) {
    UsdComposer* c = ix->c;
    const UsdGraph* g = ix->g;
    // The search begins at the strongest node that the path maps to.
    for (;;) {
        const UsdNode& n = g->nodes.data[node];
        if (n.parent == kPrUsdNone)
            break;
        UsdPath above = n.arc == PrUsdArc_Variant ? path : usdMapToTarget(&c->maps, n.mapToParent, path);
        if (above == kUsdPathNone)
            break;
        path = above;
        node = n.parent;
    }
    if (searchPrior(ix, node, path, variantSet, selection))
        return true;
    // The session is the strongest layer of the root layer stack, so its selection is the strongest of a prim whose
    // path maps to the stage's root node.
    const UsdNode& root = g->nodes.data[node];
    for (uint32_t i = 0; root.parent == kPrUsdNone && root.layerStack == c->rootStack && i < c->sessionSelections.count; ++i) {
        const UsdSessionSelection& session = c->sessionSelections.data[i];
        if (session.prim == path && session.variantSet == variantSet) {
            *selection = session.variant;
            return true;
        }
    }
    return searchSelection(ix, node, path, variantSet, selection);
}

// Queues a task for each variant set that `site` lists, a path of the node's layer stack.
void addVariantTasks(Indexer* ix, uint32_t node, UsdPath site, bool ancestral) {
    RawArray<uint64_t> names{};
    composeNames(ix->c, ix->g->nodes.data[node].layerStack, site, "variantSetNames", &names);
    for (uint32_t i = 0; i < names.count; ++i)
        ix->variantTasks.push(VariantTask{
            VariantTaskKind_Authored, ancestral, uint16_t(i < 0xffffu ? i : 0xffffu), node,
            Atom(names.data[i]), site,
        });
    names.reset();
}

void evalVariantSets(Indexer* ix, uint32_t node) {
    UsdNode n = ix->g->nodes.data[node];
    if (!usdNodeContributes(n) || (n.flags & UsdNodeFlag_HasSpecs) == 0)
        return;
    addVariantTasks(ix, node, n.path, false);
}

// Queues the variant sets of the ancestors of a node's site, which a nested composition brought without them. A
// placeholder has those of the ancestors above where it became one. A variant's site has those of its ancestors as far
// as the variant, and the others are its parent's.
void evalAncestralVariantSets(Indexer* ix, uint32_t node) {
    UsdComposer* c = ix->c;
    UsdNode n = ix->g->nodes.data[node];
    if ((n.flags & UsdNodeFlag_Elided) != 0)
        return;
    for (UsdPath path = usdPathNode(&c->paths, n.path).parent; path != kUsdPathRoot;
            path = usdPathNode(&c->paths, path).parent) {
        const UsdPathNode& p = usdPathNode(&c->paths, path);
        bool restricted = (n.flags & UsdNodeFlag_Inert) != 0 && p.primDepth >= n.restrictedDepth;
        if (!restricted && hasSpecs(c, n.layerStack, path))
            addVariantTasks(ix, node, path, true);
        if (p.part == UsdPathPart_Variant)
            break;
    }
}

// Runs a variant task: adds the arc to the variant that the set's selection names, or to the fallback that the stage's
// options give the set.
void evalVariant(Indexer* ix, VariantTask task) {
    UsdComposer* c = ix->c;
    UsdNode n = ix->g->nodes.data[task.node];
    bool dead = task.ancestral ? (n.flags & UsdNodeFlag_Elided) != 0 : !usdNodeContributes(n);
    if (dead || task.kind == VariantTaskKind_NoneFound)
        return;
    Atom selection = kAtomNone;
    if (task.kind == VariantTaskKind_Authored) {
        if (!composeSelection(ix, task.node, stripped(c, task.site), task.variantSet, &selection)) {
            task.kind = VariantTaskKind_Fallback;
            ix->variantTasks.push(task);
            return;
        }
    } else {
        // The first fallback of the set that one of the stack's layers has as a variant.
        PrStringView name = atomTableText(&c->paths.names, task.variantSet);
        for (uint32_t i = 0; selection == kAtomNone && i < c->variantFallbacks.count; ++i) {
            const PrUsdVariantSelection& fallback = c->variantFallbacks.data[i];
            if (fallback.variantSet != name)
                continue;
            Atom variant = atomTableAdd(&c->paths.names, fallback.variant);
            UsdPath path = usdPathAppend(&c->paths, task.site, UsdPathPart_Variant, task.variantSet, variant);
            if (hasSpecs(c, n.layerStack, path))
                selection = variant;
        }
        if (selection == kAtomNone) {
            task.kind = VariantTaskKind_NoneFound;
            ix->variantTasks.push(task);
            return;
        }
    }
    // A selection of the empty name selects no variant.
    if (atomTableText(&c->paths.names, selection).size == 0)
        return;
    // The variant of an ancestor composes with what the variant brings the prims under it, as any arc to a prim below
    // another does.
    UsdPath variant = usdPathAppend(&c->paths, task.site, UsdPathPart_Variant, task.variantSet, selection);
    UsdPath path = usdPathReplacePrefix(&c->paths, n.path, task.site, variant);
    ArcOptions options;
    options.ancestral = task.ancestral;
    options.skipDuplicates = task.ancestral && (n.flags & UsdNodeFlag_Unique) != 0;
    uint32_t added = addArc(ix, PrUsdArc_Variant, task.node, task.node, n.layerStack, path, kUsdMapIdentity,
        task.number, primDepth(c, task.site), options);
    if (added == kPrUsdNone)
        return;
    // A variant may author the selection of a set that had none, so those sets are tried again.
    for (uint32_t i = 0; i < ix->variantTasks.count; ++i)
        ix->variantTasks.data[i].kind = VariantTaskKind_Authored;
}

// Takes the variant task to run next: the variant sets of ancestors before those of the sites themselves, an authored
// selection before a fallback, the strongest node's first, the nearest ancestor to the root first, and a site's variant
// sets in their order.
VariantTask popVariantTask(Indexer* ix) {
    uint32_t best = 0;
    for (uint32_t i = 1; i < ix->variantTasks.count; ++i) {
        const VariantTask& a = ix->variantTasks.data[i];
        const VariantTask& b = ix->variantTasks.data[best];
        bool first = false;
        if (a.ancestral != b.ancestral)
            first = a.ancestral;
        else if (a.kind != b.kind)
            first = a.kind < b.kind;
        else if (a.node != b.node)
            first = a.kind == VariantTaskKind_NoneFound ? a.node < b.node : compareNodes(ix, a.node, b.node) < 0;
        else if (a.site != b.site)
            first = usdPathNode(&ix->c->paths, a.site).depth < usdPathNode(&ix->c->paths, b.site).depth;
        else
            first = a.number < b.number;
        if (first)
            best = i;
    }
    VariantTask task = ix->variantTasks.data[best];
    ix->variantTasks.erase(best);
    return task;
}

void runTasks(Indexer* ix) {
    ix->linking = kPrUsdNone;
    addTasks(ix, 0, false);
    uint64_t last = ~uint64_t(0);
    for (;;) {
        bool variantsNext = ix->variantTasks.count != 0 &&
            (ix->heap.count == 0 || uint32_t(ix->heap.data[0] >> 32) == Task_Unresolved);
        if (variantsNext) {
            evalVariant(ix, popVariantTask(ix));
            last = ~uint64_t(0);
            continue;
        }
        if (ix->heap.count == 0)
            break;
        uint64_t key = heapPop(&ix->heap);
        if (key == last)
            continue;
        last = key;
        uint32_t kind = uint32_t(key >> 32);
        uint32_t node = kind == Task_ImpliedClasses ? 0xffffffffu - uint32_t(key) : uint32_t(key);
        switch (kind) {
            case Task_Relocations: evalRelocations(ix, node); break;
            case Task_ImpliedRelocations: evalImpliedRelocations(ix, node); break;
            case Task_References: evalRefs(ix, node, PrUsdArc_Reference); break;
            case Task_Payloads: evalRefs(ix, node, PrUsdArc_Payload); break;
            case Task_Inherits: evalClasses(ix, node, PrUsdArc_Inherit); break;
            case Task_Specializes: evalClasses(ix, node, PrUsdArc_Specialize); break;
            case Task_ImpliedClasses: evalImpliedClasses(ix, node); break;
            case Task_AncestralVariantSets: evalAncestralVariantSets(ix, node); break;
            case Task_VariantSets: evalVariantSets(ix, node); break;
            default: evalUnresolved(ix, node); break;
        }
    }
    ix->heap.reset();
    ix->variantTasks.reset();
    if (ix->kept != nullptr) {
        ix->kept->lastLinked.reset();
        ix->kept->ranks.reset();
        ix->kept->ranked.reset();
        ix->kept->sites.reset();
        ix->kept->classOfOrigin.reset();
        ix->kept->classOfSite.reset();
        ix->kept->copyOfPlaceholder.reset();
        prMemoryFree(ix->kept);
        ix->kept = nullptr;
    }
}

// Returns true if the prim of a graph is an instance: its strongest `instanceable` is true, and an arc of its own
// brings specs (usd-core/#instancing).
bool isInstanceable(const UsdComposer* c, const UsdGraph* g) {
    bool any = false;
    auto visit = [&](uint32_t, bool shared) {
        any = any || shared;
        return !any;
    };
    walkShared(c, g, 0, false, visit);
    if (!any)
        return false;
    RawArray<uint32_t> pending{};
    pending.push(0);
    bool decided = false;
    bool instanceable = false;
    while (!decided && pending.count != 0) {
        uint32_t node = pending.data[--pending.count];
        const UsdNode& n = g->nodes.data[node];
        if (usdNodeContributes(n) && (n.flags & UsdNodeFlag_HasSpecs) != 0) {
            const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
            for (uint32_t i = 0; !decided && i < stack.layerCount; ++i) {
                const PrUsdField* field = fieldAt(c, c->stackLayers.data[stack.firstLayer + i].layer, n.path, "instanceable");
                if (field != nullptr && field->value.type == PrUsdType_Bool && !field->value.isArray) {
                    decided = true;
                    instanceable = *static_cast<const bool*>(field->value.data);
                }
            }
        }
        uint32_t first = pending.count;
        for (uint32_t child = n.firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling)
            pending.push(child);
        reverseItems(Span<uint32_t>(pending.data + first, pending.count - first));
    }
    pending.reset();
    return instanceable;
}

// Makes `g`, which holds the graph of a prim, the graph that its child named `name` begins from.
void descend(UsdComposer* c, UsdGraph* g, Atom name, uint32_t rootStack, bool expandInstances, UsdPath errorPrim) {
    if (g->instanceable && !expandInstances) {
        // A prim under an instance takes opinions only from the arcs of the instance.
        auto visit = [&](uint32_t node, bool shared) {
            UsdNode& n = g->nodes.data[node];
            if (!shared && (n.flags & UsdNodeFlag_Inert) == 0) {
                n.flags |= UsdNodeFlag_Inert;
                n.restrictedDepth = primDepth(c, n.path);
            }
            return !shared;
        };
        walkShared(c, g, 0, false, visit);
    }
    for (uint32_t i = 0; i < g->nodes.count; ++i) {
        UsdNode& n = g->nodes.data[i];
        n.path = usdPathAppend(&c->paths, n.path, UsdPathPart_Prim, name, kAtomNone);
        if ((n.flags & UsdNodeFlag_HasSpecs) != 0 && !hasSpecs(c, n.layerStack, n.path))
            n.flags &= uint8_t(~UsdNodeFlag_HasSpecs);
    }
    // A site that an ancestor's arc brought, and that a relocate of its own layer stack moved the prim from, is no
    // prim's. The arc of the prim that leads to it gives the prim nothing.
    for (uint32_t i = 1; i < g->nodes.count; ++i) {
        const UsdNode& n = g->nodes.data[i];
        // A variant holds what it says of a prim at the prim's path before a relocate, which is where its specs are.
        if (n.arc == PrUsdArc_Relocate || (n.flags & UsdNodeFlag_Culled) != 0 || stripped(c, n.path) != n.path ||
                c->layerStacks.data[n.layerStack].relocateCount == 0 ||
                findRelocate(c, n.layerStack, stripped(c, n.path), false) == nullptr)
            continue;
        uint32_t top = i;
        while (g->nodes.data[top].parent != 0)
            top = g->nodes.data[top].parent;
        Sentence sentence{c};
        sentence.add("the arc to ").site(g->nodes.data[top].layerStack, g->nodes.data[top].path)
            .add(" is ignored: it leads to ").site(n.layerStack, n.path).add(", which a relocate moved the prim from")
            .report(PrUsdErrorKind_ArcInvalidPath, errorPrim);
        elideSubtree(g, top);
    }
    g->hasPayloads = false;
    g->instanceable = false;
    // The copy at the root of each specializes placeholder, which says whether the placeholder stays. Few graphs have
    // one.
    RawArray<uint32_t> copies{};
    for (uint32_t child = g->nodes.data[0].firstChild; child != kPrUsdNone; child = g->nodes.data[child].nextSibling) {
        if (!isSpecializeCopy(g, child))
            continue;
        while (copies.count < g->nodes.count)
            copies.push(kPrUsdNone);
        if (copies.data[g->nodes.data[child].origin] == kPrUsdNone)
            copies.data[g->nodes.data[child].origin] = child;
    }
    cullSubtrees(c, g, 0, rootStack, copies.count != 0 ? &copies : nullptr);
    copies.reset();
    g->nodes.data[0].flags &= uint8_t(~UsdNodeFlag_Culled);
}

void rootGraph(UsdGraph* g, uint32_t layerStack) {
    UsdNode root{};
    root.path = kUsdPathRoot;
    root.layerStack = layerStack;
    root.mapToParent = kUsdMapIdentity;
    root.mapToRoot = kUsdMapIdentity;
    root.chain = kUsdMapChainNone;
    root.parent = kPrUsdNone;
    root.origin = kPrUsdNone;
    root.firstChild = kPrUsdNone;
    root.nextSibling = kPrUsdNone;
    root.flags = UsdNodeFlag_HasSpecs;
    g->nodes.count = 0;
    g->nodes.push(root);
    g->hasPayloads = false;
    g->instanceable = false;
}

// Composes the graph of a site with the graphs of its ancestors, for an arc of the composition that `frame` nests it
// in. No variant is selected: the composition that nothing nests selects them, when the graph has joined it. The
// ancestors of a variant's site are those within the variant, since the others are the ancestors of the site that the
// variant is of.
void buildGraph(UsdComposer* c, uint32_t layerStack, UsdPath path, Frame* frame, bool rootContributes, uint32_t nesting,
        UsdPath errorPrim, UsdGraph* g) {
    RawArray<UsdPath> chain{};
    bool inVariant = false;
    for (UsdPath at = path; at != kUsdPathRoot && !inVariant; at = usdPathNode(&c->paths, at).parent) {
        chain.push(at);
        inVariant = usdPathNode(&c->paths, at).part == UsdPathPart_Variant;
    }
    rootGraph(g, layerStack);
    for (uint32_t above = chain.count; above-- > 0;) {
        if (inVariant && above + 1 == chain.count) {
            g->nodes.data[0].path = chain.data[above];
            g->nodes.data[0].flags = hasSpecs(c, layerStack, chain.data[above]) ? UsdNodeFlag_HasSpecs : 0;
        } else {
            // Whether a prim is an instance is for the graph of the prim to say, which a variant's alone is not.
            g->instanceable = !inVariant && isInstanceable(c, g);
            descend(c, g, usdPathNode(&c->paths, chain.data[above]).name, layerStack, c->expandInstances, errorPrim);
        }
        if (above == 0 && !rootContributes && (g->nodes.data[0].flags & UsdNodeFlag_Inert) == 0) {
            g->nodes.data[0].flags |= UsdNodeFlag_Inert;
            g->nodes.data[0].restrictedDepth = primDepth(c, path);
        }
        Indexer ix{};
        ix.c = c;
        ix.g = g;
        ix.rootStack = layerStack;
        ix.errorPrim = errorPrim;
        ix.nesting = nesting;
        ix.previous = frame;
        runTasks(&ix);
    }
    chain.reset();
}

} // namespace

void usdListReorder(RawArray<uint64_t>* list, RawArray<uint64_t>* order) {
    uniqueKeys(order);
    if (order->count == 0 || list->count == 0)
        return;
    UsdKeyMap ordered{};
    for (uint32_t i = 0; i < order->count; ++i)
        ordered.add(order->data[i], i);
    UsdKeyMap position{};
    for (uint32_t i = 0; i < list->count; ++i) {
        if (position.find(list->data[i]) == kPrUsdNone)
            position.add(list->data[i], i);
    }
    // Each key of the order takes the keys that follow it in the list, up to the next key of the order. The keys that
    // none takes stay first.
    RawArray<bool> taken{};
    taken.resize(list->count);
    prMemZero(taken.data, list->count);
    RawArray<uint64_t> moved{};
    for (uint32_t i = 0; i < order->count; ++i) {
        uint32_t at = position.find(order->data[i]);
        if (at == kPrUsdNone || taken.data[at])
            continue;
        do {
            moved.push(list->data[at]);
            taken.data[at] = true;
            ++at;
        } while (at < list->count && !taken.data[at] && ordered.find(list->data[at]) == kPrUsdNone);
    }
    uint32_t kept = 0;
    for (uint32_t i = 0; i < list->count; ++i) {
        if (!taken.data[i])
            list->data[kept++] = list->data[i];
    }
    prMemCopy(list->data + kept, moved.data, size_t(moved.count) * sizeof(uint64_t));
    ordered.reset();
    position.reset();
    taken.reset();
    moved.reset();
}

void usdListApply(RawArray<uint64_t>* list, RawArray<uint64_t>* lists, bool isExplicit) {
    for (uint32_t i = 0; i < PrUsdListOpList_Count; ++i)
        uniqueKeys(&lists[i]);
    if (isExplicit) {
        list->count = 0;
        list->append(lists[PrUsdListOpList_Explicit].data, lists[PrUsdListOpList_Explicit].count);
        return;
    }
    removeKeys(list, lists[PrUsdListOpList_Deleted]);
    const RawArray<uint64_t>& added = lists[PrUsdListOpList_Added];
    if (added.count != 0) {
        UsdKeyMap set{};
        for (uint32_t i = 0; i < list->count; ++i)
            set.add(list->data[i], i);
        for (uint32_t i = 0; i < added.count; ++i) {
            if (set.find(added.data[i]) == kPrUsdNone)
                list->push(added.data[i]);
        }
        set.reset();
    }
    const RawArray<uint64_t>& prepended = lists[PrUsdListOpList_Prepended];
    if (prepended.count != 0) {
        removeKeys(list, prepended);
        RawArray<uint64_t> joined{};
        joined.append(prepended.data, prepended.count);
        joined.append(list->data, list->count);
        list->reset();
        *list = joined;
    }
    removeKeys(list, lists[PrUsdListOpList_Appended]);
    list->append(lists[PrUsdListOpList_Appended].data, lists[PrUsdListOpList_Appended].count);
    usdListReorder(list, &lists[PrUsdListOpList_Ordered]);
}

void usdComposerInit(UsdComposer* composer, const PrUsdStageOptions* PR_NULLABLE options) {
    PrUsdStageOptions defaults = prUsdStageOptionsDefault();
    const PrUsdStageOptions& o = options != nullptr ? *options : defaults;
    composer->layerOptions = o.layer;
    composer->open = o.open;
    composer->openContext = o.openContext;
    composer->load = o.load;
    composer->expandInstances = o.expandInstances;
    usdPathTableInit(&composer->paths);
    usdMapTableInit(&composer->maps, &composer->paths);
    for (uint32_t i = 0; i < o.loadPathCount; ++i) {
        UsdPath path = usdPathParse(&composer->paths, o.loadPaths[i]);
        if (path != kUsdPathNone)
            composer->loadPaths.push(path);
    }
    for (uint32_t i = 0; i < o.variantFallbackCount; ++i) {
        PrUsdVariantSelection fallback = o.variantFallbacks[i];
        fallback.variantSet = atomTableText(&composer->strings, atomTableAdd(&composer->strings, fallback.variantSet));
        fallback.variant = atomTableText(&composer->strings, atomTableAdd(&composer->strings, fallback.variant));
        composer->variantFallbacks.push(fallback);
    }
    for (uint32_t i = 0; i < o.variantSelectionCount; ++i) {
        const PrUsdPrimVariantSelection& given = o.variantSelections[i];
        UsdPath prim = usdPathParse(&composer->paths, given.prim);
        if (prim == kUsdPathNone)
            continue;
        composer->sessionSelections.push(UsdSessionSelection{
            .prim = prim,
            .variantSet = atomTableAdd(&composer->paths.names, given.variantSet),
            .variant = atomTableAdd(&composer->paths.names, given.variant),
        });
    }
    composer->rootStack = kPrUsdNone;
}

void usdComposerReset(UsdComposer* composer) {
    for (uint32_t i = 0; i < composer->layers.count; ++i) {
        prUsdLayerDestroy(composer->layers.data[i].layer);
        composer->layers.data[i].specPaths.reset();
    }
    composer->layers.reset();
    composer->loadPaths.reset();
    composer->variantFallbacks.reset();
    composer->sessionSelections.reset();
    usdMapTableReset(&composer->maps);
    usdPathTableReset(&composer->paths);
    atomTableReset(&composer->strings);
    composer->layerOfLocation.reset();
    composer->siteSpecs.reset();
    composer->layerStacks.reset();
    composer->stackOfLayer.reset();
    composer->stackLayers.reset();
    composer->relocates.reset();
    composer->errors.reset();
}

uint32_t usdComposerOpen(UsdComposer* composer, PrStringView location, PrStatus* status) {
    uint32_t layer = openLayer(composer, location, status);
    uint32_t stack = layer == kPrUsdNone ? kPrUsdNone : layerStackOf(composer, layer);
    if (composer->rootStack == kPrUsdNone)
        composer->rootStack = stack;
    return stack;
}

void usdComposeRoot(UsdComposer* composer, uint32_t rootStack, UsdGraph* graph) {
    (void)composer;
    rootGraph(graph, rootStack);
}

void usdComposeChild(UsdComposer* composer, const UsdGraph* parent, Atom name, UsdGraph* graph) {
    graph->nodes.count = 0;
    graph->nodes.append(parent->nodes.data, parent->nodes.count);
    graph->instanceable = parent->instanceable;
    uint32_t rootStack = graph->nodes.data[0].layerStack;
    UsdPath prim = usdPathAppend(&composer->paths, graph->nodes.data[0].path, UsdPathPart_Prim, name, kAtomNone);
    descend(composer, graph, name, rootStack, composer->expandInstances, prim);
    Indexer ix{};
    ix.c = composer;
    ix.g = graph;
    ix.rootStack = rootStack;
    ix.errorPrim = graph->nodes.data[0].path;
    runTasks(&ix);
    graph->instanceable = isInstanceable(composer, graph);
    finalizeGraph(graph);
}

void usdGraphChildNames(UsdComposer* composer, const UsdGraph* graph, RawArray<Atom>* names, RawArray<Atom>* prohibited) {
    UsdComposer* c = composer;
    const UsdPathTable* paths = &c->paths;
    // The nodes of a composed graph are in strength order, and the names compose weakest first.
    RawArray<uint64_t> list{};
    RawArray<uint64_t> banned{};
    // The names that the list holds, which is made again after a relocate takes a name out of the list.
    UsdKeyMap have{};
    bool haveAll = true;
    bool instance = graph->instanceable && !c->expandInstances;
    for (uint32_t i = graph->nodes.count; i-- > 0;) {
        const UsdNode& n = graph->nodes.data[i];
        // An instance's children are those of the arcs that it shares.
        if (instance && !usdGraphNodeIsShared(c, graph, i))
            continue;
        // The relocates of the node's layer stack rename, remove and add children (usd-core/#ordered-prim-children).
        const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
        RawArray<uint64_t> added{};
        for (uint32_t j = 0; j < stack.relocateCount; ++j) {
            const UsdStackRelocate& relocate = c->relocates.data[stack.firstRelocate + j];
            bool fromHere = usdPathNode(paths, relocate.source).parent == n.path;
            bool toHere = relocate.target != kUsdPathNone && usdPathNode(paths, relocate.target).parent == n.path;
            if (fromHere) {
                uint64_t oldName = usdPathNode(paths, relocate.source).name;
                uint32_t at = list.find(oldName);
                if (toHere && at != kArrayMissing) {
                    uint64_t newName = usdPathNode(paths, relocate.target).name;
                    if (list.find(newName) == kArrayMissing)
                        list.data[at] = newName;
                    else
                        list.erase(at);
                } else if (at != kArrayMissing) {
                    list.erase(at);
                }
                haveAll = haveAll && at == kArrayMissing;
                if (banned.find(oldName) == kArrayMissing)
                    banned.push(oldName);
            } else if (toHere) {
                uint64_t newName = usdPathNode(paths, relocate.target).name;
                if (list.find(newName) == kArrayMissing && added.find(newName) == kArrayMissing)
                    added.push(newName);
            }
        }
        sortItems(Span<uint64_t>(added.data, added.count), [&](uint64_t a, uint64_t b) {
            return prStringCompare(atomTableText(&paths->names, Atom(a)), atomTableText(&paths->names, Atom(b))) < 0;
        });
        list.append(added.data, added.count);
        haveAll = haveAll && added.count == 0;
        added.reset();
        if (!usdNodeContributes(n) || (n.flags & UsdNodeFlag_HasSpecs) == 0)
            continue;
        for (uint32_t j = stack.layerCount; j-- > 0;) {
            uint32_t layer = c->stackLayers.data[stack.firstLayer + j].layer;
            PrUsdSpecIndex spec = usdComposerSpec(c, layer, n.path);
            if (spec == kPrUsdNone)
                continue;
            const PrUsdLayer* data = c->layers.data[layer].layer;
            const PrUsdSpec& s = prUsdLayerSpecs(data)[spec];
            const PrUsdSpecIndex* children = prUsdLayerList(data, s.prims);
            if (s.prims.count != 0) {
                if (!haveAll) {
                    have.reset();
                    for (uint32_t k = 0; k < list.count; ++k)
                        have.add(list.data[k], k);
                    haveAll = true;
                }
                for (uint32_t k = 0; k < s.prims.count; ++k) {
                    uint64_t name = usdPathNode(paths, c->layers.data[layer].specPaths.data[children[k]]).name;
                    if (have.find(name) == kPrUsdNone) {
                        have.add(name, list.count);
                        list.push(name);
                    }
                }
            }
            const PrUsdField* reorder = prUsdLayerFindField(data, spec, "primOrder");
            if (reorder != nullptr && reorder->value.type == PrUsdType_Token && reorder->value.isArray) {
                RawArray<uint64_t> wanted{};
                for (uint32_t k = 0; k < reorder->value.count; ++k) {
                    Atom name = atomTableFind(&paths->names, static_cast<const PrStringView*>(reorder->value.data)[k]);
                    if (name != kAtomNone)
                        wanted.push(name);
                }
                usdListReorder(&list, &wanted);
                wanted.reset();
            }
        }
    }
    for (uint32_t i = 0; i < list.count; ++i) {
        if (banned.find(list.data[i]) == kArrayMissing)
            names->push(Atom(list.data[i]));
    }
    for (uint32_t i = 0; i < banned.count; ++i)
        prohibited->push(Atom(banned.data[i]));
    list.reset();
    banned.reset();
    have.reset();
}

bool usdGraphNodeIsShared(const UsdComposer* composer, const UsdGraph* graph, uint32_t node) {
    if (node == 0 || (graph->nodes.data[node].flags & UsdNodeFlag_HasSpecs) == 0)
        return false;
    for (uint32_t at = node; graph->nodes.data[at].parent != kPrUsdNone; at = graph->nodes.data[at].parent) {
        if (depthBelowIntroduction(composer, graph, at) == 0)
            return true;
    }
    return false;
}

void usdGraphText(const UsdComposer* composer, const UsdGraph* graph, PrTextBuffer* out) {
    static const char* const kArcs[] = {"root", "inherit", "variant", "relocate", "reference", "payload", "specialize"};
    RawArray<uint32_t> pending{};
    RawArray<uint32_t> depths{};
    pending.push(0);
    depths.push(0);
    while (pending.count != 0) {
        uint32_t node = pending.data[--pending.count];
        uint32_t depth = depths.data[--depths.count];
        const UsdNode& n = graph->nodes.data[node];
        for (uint32_t i = 0; i < depth; ++i)
            prTextBufferAppend(out, "  ");
        prTextBufferAppend(out, kArcs[n.arc]);
        prTextBufferAppendChar(out, ' ');
        prTextBufferAppend(out, composer->layers.data[composer->layerStacks.data[n.layerStack].root].name);
        prTextBufferAppendChar(out, ' ');
        usdPathText(&composer->paths, n.path, out);
        if ((n.flags & UsdNodeFlag_Inert) != 0)
            prTextBufferAppend(out, " inert");
        if ((n.flags & UsdNodeFlag_Culled) != 0)
            prTextBufferAppend(out, " culled");
        if ((n.flags & UsdNodeFlag_HasSpecs) != 0)
            prTextBufferAppend(out, " specs");
        if (n.origin != kPrUsdNone && n.origin != n.parent) {
            prTextBufferAppend(out, " from ");
            prTextBufferAppend(out,
                composer->layers.data[composer->layerStacks.data[graph->nodes.data[n.origin].layerStack].root].name);
            prTextBufferAppendChar(out, ' ');
            usdPathText(&composer->paths, graph->nodes.data[n.origin].path, out);
        }
        if (n.mapToParent > kUsdMapIdentity) {
            // The pairs of the arc's mapping, source to target.
            const UsdMap& map = composer->maps.maps.data[n.mapToParent];
            prTextBufferAppend(out, " maps");
            for (uint32_t i = 0; i < map.count; ++i) {
                prTextBufferAppendChar(out, ' ');
                usdPathText(&composer->paths, composer->maps.pairs.data[map.first + i].source, out);
                prTextBufferAppendChar(out, '>');
                usdPathText(&composer->paths, composer->maps.pairs.data[map.first + i].target, out);
            }
        }
        prTextBufferAppendChar(out, '\n');
        uint32_t first = pending.count;
        for (uint32_t child = n.firstChild; child != kPrUsdNone; child = graph->nodes.data[child].nextSibling) {
            pending.push(child);
            depths.push(depth + 1);
        }
        reverseItems(Span<uint32_t>(pending.data + first, pending.count - first));
    }
    pending.reset();
    depths.reset();
}

void usdComposerError(UsdComposer* composer, PrUsdErrorKind kind, UsdPath prim, PrStringView message) {
    // A message's text is kept apart from the strings that name layers, which the message may be one of.
    Atom atom = atomTableAdd(&composer->strings, message);
    composer->errors.push(UsdComposeError{kind, prim, atomTableText(&composer->strings, atom)});
}

} // namespace pr_impl
