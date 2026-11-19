// Stages: the prims that a root layer composes (usd-core/#stage-population), and the values that their opinions resolve
// to (usd-core/#value-resolution).
//
// A stage is composed once, depth-first from the pseudo-root, and is immutable afterwards. A prim keeps the opinions
// that its graph gives it, each a spec of a layer with the mapping and the retiming of its site, and the graph itself
// lives only while the prim's children compose. A value resolves by walking those opinions, and points into the layer
// that holds it unless resolving had to make it, in which case a scratch of the caller's holds it.

#include "pr/usd_stage.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/float.h"
#include "pr/float16.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "usd/src/usd_compose.h"
#include "usd/src/usd_layer.h"

namespace pr_impl {

namespace {

constexpr PrStringView kPrototypePrefix = "__Prototype_";

// The most relationships that forwarding one relationship's targets follows.
constexpr uint32_t kMaxForwarding = 1024;

// The flags of an opinion.
enum {
    OpinionFlag_DirectInherit = 1,      // an inherit of the prim itself is on the way to the opinion's site
    OpinionFlag_SiteStart = 2,          // the strongest opinion of its site: the first of its layer stack's layers
};
typedef uint8_t OpinionFlags;

// An opinion of a prim: a prim spec, with what its site's node gives it.
struct StageOpinion {
    uint32_t layer;
    PrUsdSpecIndex spec;
    uint32_t chain;             // of the composer's chains of mappings: from the site's namespace to the stage's
    uint32_t offset;            // in UsdStage::offsets: from the layer's times to the stage's
    uint8_t arc;
    OpinionFlags flags;
};

// A variant that a prim's composition selected.
struct StageSelection {
    PrUsdPrimIndex prim;
    Atom variantSet;
    Atom variant;
};

// A prototype: the children that the instances with one key share (usd-core/#scene-graph-instancing).
struct StagePrototype {
    PrUsdPrimIndex root;
    UsdPath source;             // the path of the instance that the prototype's prims are composed under
    uint32_t firstKey;          // in UsdStage::prototypeKeys
    uint32_t keyCount;
    uint32_t nextOfHash;        // the next prototype whose key has the same hash, or kPrUsdNone
    UsdGraph graph;             // the graph of the source instance, until the prototype is populated
};

struct UsdStage {
    UsdComposer composer;
    uint32_t rootStack;
    RawArray<PrUsdPrim> prims;
    RawArray<UsdPath> primPaths;        // by prim: the path that it composed at, under a prototype's source if in one
    RawArray<uint32_t> primPrototype;   // by prim: the prototype that it is in, or kPrUsdNone
    UsdKeyMap primOfPath;               // by prototype and path
    RawArray<StageOpinion> opinions;
    RawArray<PrUsdLayerOffset> offsets;
    RawArray<PrUsdProperty> properties;
    RawArray<uint32_t> propertiesByName;    // beside `properties`: each prim's, in the order of their names' bytes
    RawArray<StageSelection> selections;
    RawArray<StagePrototype> prototypes;
    RawArray<uint64_t> prototypeKeys;
    UsdKeyMap prototypeOfHash;          // by the hash of a key: the first prototype with that hash
    RawArray<PrUsdError> errors;
};

struct UsdScratch {
    Arena arena;                // of pages up to kUsdArenaLargestPage
    AtomTable keys;             // the texts that list ops are combined by
};

// A prim whose children are being composed.
struct Work {
    UsdGraph graph;
    PrUsdPrimIndex prim;
    RawArray<Atom> names;
    uint32_t next;
    PrUsdPrimIndex lastChild;
};

uint64_t primKey(uint32_t prototype, UsdPath path) {
    return uint64_t(prototype + 1) << 32 | path;
}

uint32_t internOffset(UsdStage* stage, PrUsdLayerOffset offset) {
    // Few retimings are distinct, and the one sought is most often the last one made.
    for (uint32_t i = stage->offsets.count; i-- > 0 && stage->offsets.count - i <= 16;) {
        if (stage->offsets.data[i].offset == offset.offset && stage->offsets.data[i].scale == offset.scale)
            return i;
    }
    stage->offsets.push(offset);
    return stage->offsets.count - 1;
}

const PrUsdField* PR_NULLABLE opinionField(const UsdStage* stage, const StageOpinion& opinion, PrStringView name) {
    return prUsdLayerFindField(stage->composer.layers.data[opinion.layer].layer, opinion.spec, name);
}

// ---------------------------------------------------------------------------------------------------------------------
// Population

// Appends what identifies the arcs that an instance's children take their opinions from. Instances whose keys are equal
// share a prototype: the same sites, by the same arcs, at the same retimings, in the same order.
void instanceKey(const UsdStage* stage, const UsdGraph* graph, RawArray<uint64_t>* key) {
    const UsdComposer* c = &stage->composer;
    for (uint32_t i = 0; i < graph->nodes.count; ++i) {
        const UsdNode& n = graph->nodes.data[i];
        if (!usdGraphNodeIsShared(c, graph, i) || !usdNodeContributes(n))
            continue;
        PrUsdLayerOffset offset = c->maps.maps.data[n.mapToRoot].offset;
        uint64_t bits[2] = {};
        prMemCopy(bits, &offset, sizeof(bits));
        key->push(uint64_t(n.arc) << 32 | n.layerStack);
        key->push(n.path);
        key->push(bits[0]);
        key->push(bits[1]);
    }
}

// Returns the prototype of an instance whose graph is `graph`, adding it if no instance had its key.
uint32_t findPrototype(UsdStage* stage, const UsdGraph* graph) {
    RawArray<uint64_t> key{};
    instanceKey(stage, graph, &key);
    // The prototypes are found by the hash of their keys, and the prototypes of one hash are chained.
    uint64_t hash = prHashBytes(key.data, size_t(key.count) * sizeof(uint64_t), 0);
    uint32_t first = stage->prototypeOfHash.find(hash);
    uint32_t found = kPrUsdNone;
    for (uint32_t i = first; found == kPrUsdNone && i != kPrUsdNone; i = stage->prototypes.data[i].nextOfHash) {
        const StagePrototype& prototype = stage->prototypes.data[i];
        if (prototype.keyCount == key.count && prMemCompare(stage->prototypeKeys.data + prototype.firstKey, key.data,
                size_t(key.count) * sizeof(uint64_t)) == 0)
            found = i;
    }
    if (found == kPrUsdNone) {
        StagePrototype prototype{};
        prototype.root = kPrUsdNone;
        prototype.source = graph->nodes.data[0].path;
        prototype.firstKey = stage->prototypeKeys.count;
        prototype.keyCount = key.count;
        prototype.graph.nodes.append(graph->nodes.data, graph->nodes.count);
        prototype.graph.instanceable = true;
        stage->prototypeKeys.append(key.data, key.count);
        found = stage->prototypes.count;
        // A prototype joins its hash's chain behind the first, which the map holds.
        prototype.nextOfHash = kPrUsdNone;
        if (first == kPrUsdNone) {
            stage->prototypeOfHash.add(hash, found);
        } else {
            prototype.nextOfHash = stage->prototypes.data[first].nextOfHash;
            stage->prototypes.data[first].nextOfHash = found;
        }
        stage->prototypes.push(prototype);
    }
    key.reset();
    return found;
}

// Adds the properties of a prim: the names that its opinions' specs hold, in the order of names, with the strongest
// `propertyOrder` applied (usd-core/#ordered-property-children).
void addProperties(UsdStage* stage, PrUsdPrim* prim, UsdPath path) {
    UsdComposer* c = &stage->composer;
    const UsdPathTable* paths = &c->paths;
    RawArray<uint64_t> names{};
    RawArray<PrUsdSpecForm> forms{};
    UsdKeyMap seen{};
    const PrUsdField* order = nullptr;
    for (uint32_t i = 0; i < prim->opinionCount; ++i) {
        const StageOpinion& opinion = stage->opinions.data[prim->firstOpinion + i];
        const UsdStageLayer& layer = c->layers.data[opinion.layer];
        const PrUsdSpec& spec = prUsdLayerSpecs(layer.layer)[opinion.spec];
        const PrUsdSpecIndex* children = prUsdLayerList(layer.layer, spec.properties);
        for (uint32_t j = 0; j < spec.properties.count; ++j) {
            Atom name = usdPathNode(paths, layer.specPaths.data[children[j]]).name;
            PrUsdSpecForm form = prUsdLayerSpecs(layer.layer)[children[j]].form;
            uint32_t at = seen.find(name);
            if (at == kPrUsdNone) {
                seen.add(name, names.count);
                names.push(name);
                forms.push(form);
            } else if (forms.data[at] != form) {
                // The strongest spec says what a property is, and a weaker spec of the other form gives it nothing.
                PrTextBuffer text = {};
                prTextBufferAppend(&text, "the spec of the property `");
                prTextBufferAppend(&text, atomTableText(&paths->names, name));
                prTextBufferAppend(&text, "` in @");
                prTextBufferAppend(&text, layer.name);
                prTextBufferAppend(&text, "@ is ignored: it is not of the form of the property's strongest spec");
                usdComposerError(c, PrUsdErrorKind_InconsistentProperty, path, prTextBufferView(&text));
                prTextBufferReset(&text);
            }
        }
        if (order == nullptr) {
            const PrUsdField* field = opinionField(stage, opinion, "propertyOrder");
            if (field != nullptr && field->value.type == PrUsdType_Token && field->value.isArray)
                order = field;
        }
    }
    // The names sort with their forms, which the sort reaches through the names' first positions.
    RawArray<uint64_t> sorted{};
    sorted.append(names.data, names.count);
    sortItems(Span<uint64_t>(sorted.data, sorted.count), [&](uint64_t a, uint64_t b) {
        return usdNameCompare(atomTableText(&paths->names, Atom(a)), atomTableText(&paths->names, Atom(b))) < 0;
    });
    if (order != nullptr) {
        // The names of the order come first, in its order, and the others follow in theirs. This is the reorder of the
        // specification, and not the one that a prim's children take.
        RawArray<uint64_t> ordered{};
        UsdKeyMap moved{};
        for (uint32_t i = 0; i < order->value.count; ++i) {
            Atom name = atomTableFind(&paths->names, static_cast<const PrStringView*>(order->value.data)[i]);
            if (name != kAtomNone && seen.find(name) != kPrUsdNone && moved.find(name) == kPrUsdNone) {
                moved.add(name, i);
                ordered.push(name);
            }
        }
        for (uint32_t i = 0; i < sorted.count; ++i) {
            if (moved.find(sorted.data[i]) == kPrUsdNone)
                ordered.push(sorted.data[i]);
        }
        sorted.reset();
        sorted = ordered;
        moved.reset();
    }
    prim->firstProperty = stage->properties.count;
    prim->propertyCount = sorted.count;
    for (uint32_t i = 0; i < sorted.count; ++i) {
        PrUsdProperty property{};
        property.name = atomTableText(&paths->names, Atom(sorted.data[i]));
        property.form = forms.data[seen.find(sorted.data[i])];
        stage->properties.push(property);
    }
    // The prim's properties in the order of their names' bytes, which a search by name or by a prefix of one takes. The
    // names are nearly in that order already, so each is put in its place among those before it.
    for (uint32_t i = 0; i < sorted.count; ++i) {
        uint32_t property = prim->firstProperty + i;
        uint32_t at = stage->propertiesByName.count;
        stage->propertiesByName.push(property);
        uint32_t* byName = stage->propertiesByName.data;
        PrStringView name = stage->properties.data[property].name;
        for (; at > prim->firstProperty && prStringCompare(name, stage->properties.data[byName[at - 1]].name) < 0; --at)
            byName[at] = byName[at - 1];
        byName[at] = property;
    }
    names.reset();
    forms.reset();
    seen.reset();
    sorted.reset();
}

// Adds the prim of `graph` under `parent`, with its opinions and what they resolve to of the prim itself.
PrUsdPrimIndex addPrim(UsdStage* stage, const UsdGraph* graph, PrUsdPrimIndex parent, uint32_t prototype) {
    UsdComposer* c = &stage->composer;
    PrUsdPrimIndex index = stage->prims.count;
    UsdPath path = graph->nodes.data[0].path;
    PrUsdPrim prim{};
    prim.name = path == kUsdPathRoot ? PrStringView{} : usdPathName(&c->paths, path);
    prim.parent = parent;
    prim.firstChild = kPrUsdNone;
    prim.nextSibling = kPrUsdNone;
    prim.end = index + 1;
    prim.prototype = kPrUsdNone;
    prim.firstOpinion = stage->opinions.count;

    // The opinions: the specs of each node's site, strongest node first, and under a node its stack's layers.
    bool active = true;
    bool activeFound = false;
    uint32_t specifierStrength = 0;
    prim.specifier = PrUsdSpecifier_Over;
    RawArray<bool> inherited{};
    inherited.resize(graph->nodes.count);
    for (uint32_t i = 0; i < graph->nodes.count; ++i) {
        const UsdNode& n = graph->nodes.data[i];
        // A graph's nodes are in strength order, so a node's parent comes before it.
        bool direct = n.arc == PrUsdArc_Inherit && n.parent != kPrUsdNone &&
            usdPathNode(&c->paths, graph->nodes.data[n.parent].path).primDepth == n.namespaceDepth;
        inherited.data[i] = direct || (n.parent != kPrUsdNone && inherited.data[n.parent]);
        if (n.arc == PrUsdArc_Variant && n.parent != kPrUsdNone &&
                usdPathNode(&c->paths, graph->nodes.data[n.parent].path).primDepth == n.namespaceDepth) {
            const UsdPathNode& variant = usdPathNode(&c->paths, n.path);
            stage->selections.push(StageSelection{index, variant.name, variant.variant});
        }
        if (!usdNodeContributes(n) || (n.flags & UsdNodeFlag_HasSpecs) == 0)
            continue;
        const UsdLayerStack& stack = c->layerStacks.data[n.layerStack];
        PrUsdLayerOffset nodeOffset = c->maps.maps.data[n.mapToRoot].offset;
        bool siteStarted = false;
        for (uint32_t j = 0; j < stack.layerCount; ++j) {
            const UsdStackLayer& stackLayer = c->stackLayers.data[stack.firstLayer + j];
            PrUsdSpecIndex spec = usdComposerSpec(c, stackLayer.layer, n.path);
            if (spec == kPrUsdNone)
                continue;
            StageOpinion opinion{};
            opinion.layer = stackLayer.layer;
            opinion.spec = spec;
            opinion.chain = n.chain;
            opinion.offset = internOffset(stage, usdOffsetCompose(nodeOffset, stackLayer.offset));
            opinion.arc = n.arc;
            opinion.flags = inherited.data[i] ? OpinionFlag_DirectInherit : 0;
            if (!siteStarted)
                opinion.flags |= OpinionFlag_SiteStart;
            siteStarted = true;
            stage->opinions.push(opinion);
            if (path == kUsdPathRoot)
                continue;
            const PrUsdLayer* layer = c->layers.data[stackLayer.layer].layer;
            // A defining specifier is stronger than `over`, and a class that the prim inherits is weaker than any other
            // defining specifier, so that inheriting a class does not make a prim one.
            const PrUsdField* specifier = prUsdLayerFindField(layer, spec, "specifier");
            if (specifier != nullptr && specifier->value.type == PrUsdType_Specifier) {
                PrUsdSpecifier value = *static_cast<const PrUsdSpecifier*>(specifier->value.data);
                uint32_t strength = value == PrUsdSpecifier_Over ? 1 : 3;
                if (value == PrUsdSpecifier_Class && inherited.data[i])
                    strength = 2;
                if (strength > specifierStrength) {
                    specifierStrength = strength;
                    prim.specifier = value;
                }
            }
            const PrUsdField* typeName = prUsdLayerFindField(layer, spec, "typeName");
            if (prim.typeName.size == 0 && typeName != nullptr && typeName->value.type == PrUsdType_Token) {
                PrStringView value = *static_cast<const PrStringView*>(typeName->value.data);
                if (value != "__AnyType__")
                    prim.typeName = value;
            }
            const PrUsdField* activeField = prUsdLayerFindField(layer, spec, "active");
            if (!activeFound && activeField != nullptr && activeField->value.type == PrUsdType_Bool) {
                activeFound = true;
                active = *static_cast<const bool*>(activeField->value.data);
            }
        }
    }
    inherited.reset();
    prim.opinionCount = stage->opinions.count - prim.firstOpinion;

    uint8_t above = PrUsdPrimFlag_Active | PrUsdPrimFlag_Loaded | PrUsdPrimFlag_Defined;
    if (parent != kPrUsdNone)
        above = stage->prims.data[parent].flags;
    bool defined = path == kUsdPathRoot || prim.specifier != PrUsdSpecifier_Over;
    bool loaded = !graph->hasPayloads || c->load == PrUsdLoad_All;
    for (uint32_t i = 0; !loaded && i < c->loadPaths.count; ++i) {
        loaded = usdPathHasPrefix(&c->paths, path, c->loadPaths.data[i]) ||
            usdPathHasPrefix(&c->paths, c->loadPaths.data[i], path);
    }
    if (path == kUsdPathRoot)
        prim.specifier = PrUsdSpecifier_Def;
    prim.flags = 0;
    if (active && (above & PrUsdPrimFlag_Active) != 0)
        prim.flags |= PrUsdPrimFlag_Active;
    // A prim that is not active composes nothing under it, and is not loaded either.
    if (loaded && (above & PrUsdPrimFlag_Loaded) != 0 && (prim.flags & PrUsdPrimFlag_Active) != 0)
        prim.flags |= PrUsdPrimFlag_Loaded;
    if (defined && (above & PrUsdPrimFlag_Defined) != 0)
        prim.flags |= PrUsdPrimFlag_Defined;
    if (prim.specifier == PrUsdSpecifier_Class || (above & PrUsdPrimFlag_Abstract) != 0)
        prim.flags |= PrUsdPrimFlag_Abstract;
    if (graph->hasPayloads)
        prim.flags |= PrUsdPrimFlag_HasPayload;
    if (prototype != kPrUsdNone)
        prim.flags |= PrUsdPrimFlag_InPrototype;
    if (graph->instanceable && !c->expandInstances && (prim.flags & PrUsdPrimFlag_Active) != 0)
        prim.flags |= PrUsdPrimFlag_Instance;
    if (path != kUsdPathRoot)
        addProperties(stage, &prim, path);
    stage->prims.push(prim);
    stage->primPaths.push(path);
    stage->primPrototype.push(prototype);
    stage->primOfPath.add(primKey(prototype, path), index);
    return index;
}

// Composes the prims under the prim at the top of `work`, depth-first.
void populate(UsdStage* stage, RawArray<Work>* work, uint32_t prototype) {
    UsdComposer* c = &stage->composer;
    while (work->count != 0) {
        Work* top = &work->data[work->count - 1];
        if (top->next == top->names.count) {
            stage->prims.data[top->prim].end = stage->prims.count;
            top->graph.nodes.reset();
            top->names.reset();
            --work->count;
            continue;
        }
        Atom name = top->names.data[top->next++];
        Work child{};
        usdComposeChild(c, &top->graph, name, &child.graph);
        child.prim = addPrim(stage, &child.graph, top->prim, prototype);
        child.lastChild = kPrUsdNone;
        if (top->lastChild == kPrUsdNone)
            stage->prims.data[top->prim].firstChild = child.prim;
        else
            stage->prims.data[top->lastChild].nextSibling = child.prim;
        top->lastChild = child.prim;
        uint8_t flags = stage->prims.data[child.prim].flags;
        if ((flags & PrUsdPrimFlag_Instance) != 0) {
            // An instance's children are its prototype's, which is recorded by its number until it has a root.
            stage->prims.data[child.prim].prototype = findPrototype(stage, &child.graph);
        } else if ((flags & PrUsdPrimFlag_Active) != 0) {
            RawArray<Atom> prohibited{};
            usdGraphChildNames(c, &child.graph, &child.names, &prohibited);
            prohibited.reset();
        }
        // `top` is not used again: pushing may move the work.
        work->push(child);
    }
}

void populateStage(UsdStage* stage) {
    UsdComposer* c = &stage->composer;
    RawArray<Work> work{};
    Work root{};
    usdComposeRoot(c, stage->rootStack, &root.graph);
    root.prim = addPrim(stage, &root.graph, kPrUsdNone, kPrUsdNone);
    root.lastChild = kPrUsdNone;
    RawArray<Atom> prohibited{};
    usdGraphChildNames(c, &root.graph, &root.names, &prohibited);
    prohibited.reset();
    work.push(root);
    populate(stage, &work, kPrUsdNone);

    // The prototypes follow, each from the graph of its first instance. Populating one may add others.
    for (uint32_t i = 0; i < stage->prototypes.count; ++i) {
        PrTextBuffer text = {};
        prTextBufferAppend(&text, kPrototypePrefix);
        char digits[12];
        uint32_t count = 0;
        for (uint32_t number = i + 1; number != 0; number /= 10)
            digits[count++] = char('0' + number % 10);
        while (count != 0)
            prTextBufferAppendChar(&text, digits[--count]);
        Atom name = atomTableAdd(&c->paths.names, prTextBufferView(&text));
        prTextBufferReset(&text);

        Work top{};
        top.graph = stage->prototypes.data[i].graph;
        stage->prototypes.data[i].graph = UsdGraph{};
        PrUsdPrim prim{};
        prim.name = atomTableText(&c->paths.names, name);
        prim.parent = kPrUsdNone;
        prim.firstChild = kPrUsdNone;
        prim.nextSibling = kPrUsdNone;
        prim.prototype = kPrUsdNone;
        prim.specifier = PrUsdSpecifier_Def;
        prim.flags = PrUsdPrimFlag_Active | PrUsdPrimFlag_Loaded | PrUsdPrimFlag_Defined | PrUsdPrimFlag_InPrototype;
        top.prim = stage->prims.count;
        top.lastChild = kPrUsdNone;
        prim.end = top.prim + 1;
        stage->prototypes.data[i].root = top.prim;
        stage->prims.push(prim);
        stage->primPaths.push(stage->prototypes.data[i].source);
        stage->primPrototype.push(i);
        stage->primOfPath.add(primKey(i, stage->prototypes.data[i].source), top.prim);
        RawArray<Atom> banned{};
        usdGraphChildNames(c, &top.graph, &top.names, &banned);
        banned.reset();
        work.push(top);
        populate(stage, &work, i);
    }
    work.reset();
    // An instance records its prototype by number until every prototype has a root.
    for (uint32_t i = 0; i < stage->prims.count; ++i) {
        PrUsdPrim& prim = stage->prims.data[i];
        if ((prim.flags & PrUsdPrimFlag_Instance) != 0)
            prim.prototype = stage->prototypes.data[prim.prototype].root;
    }
    for (uint32_t i = 0; i < c->errors.count; ++i) {
        const UsdComposeError& error = c->errors.data[i];
        PrUsdError out{};
        out.kind = error.kind;
        out.message = error.message;
        if (error.prim != kUsdPathNone) {
            PrTextBuffer text = {};
            usdPathText(&c->paths, error.prim, &text);
            out.prim = atomTableText(&c->strings, atomTableAdd(&c->strings, prTextBufferView(&text)));
            prTextBufferReset(&text);
        }
        stage->errors.push(out);
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Value resolution

// Appends the specs of a property of a prim, strongest first, as opinions whose spec is the property's.
void propertyOpinions(const UsdStage* stage, PrUsdPrimIndex prim, uint32_t property, RawArray<StageOpinion>* out) {
    const UsdComposer* c = &stage->composer;
    const PrUsdPrim& p = stage->prims.data[prim];
    PrStringView name = stage->properties.data[property].name;
    for (uint32_t i = 0; i < p.opinionCount; ++i) {
        StageOpinion opinion = stage->opinions.data[p.firstOpinion + i];
        UsdPath path = usdPathFindProperty(&c->paths, c->layers.data[opinion.layer].specPaths.data[opinion.spec], name);
        if (path == kUsdPathNone)
            continue;
        opinion.spec = usdComposerSpec(c, opinion.layer, path);
        // A spec of the other form than the property's strongest is no opinion of the property.
        if (opinion.spec != kPrUsdNone && prUsdLayerSpecs(c->layers.data[opinion.layer].layer)[opinion.spec].form ==
                stage->properties.data[property].form)
            out->push(opinion);
    }
}

// Returns the dictionary that the entries of `strong` make over those of `weak`: a key of both keeps the stronger
// value, and two dictionaries under one key combine (usd-core/#dictionaries).
PrUsdDictionary mergeDictionaries(UsdScratch* scratch, const PrUsdDictionary& strong, const PrUsdDictionary& weak) {
    PrUsdDictionaryEntry* entries = scratch->arena.allocZeroed<PrUsdDictionaryEntry>(strong.count + weak.count);
    uint32_t count = 0;
    uint32_t i = 0;
    uint32_t j = 0;
    while (i < strong.count || j < weak.count) {
        int order =
            i == strong.count ? 1 : j == weak.count ? -1 : prStringCompare(strong.entries[i].key, weak.entries[j].key);
        if (order < 0) {
            entries[count++] = strong.entries[i++];
        } else if (order > 0) {
            entries[count++] = weak.entries[j++];
        } else {
            PrUsdDictionaryEntry entry = strong.entries[i];
            const PrUsdValue& other = weak.entries[j].value;
            if (entry.value.type == PrUsdType_Dictionary && other.type == PrUsdType_Dictionary && !entry.value.isArray &&
                    !other.isArray) {
                PrUsdDictionary* merged = scratch->arena.allocZeroed<PrUsdDictionary>();
                *merged = mergeDictionaries(scratch, *static_cast<const PrUsdDictionary*>(entry.value.data),
                    *static_cast<const PrUsdDictionary*>(other.data));
                entry.value.data = merged;
            }
            entries[count++] = entry;
            ++i;
            ++j;
        }
    }
    return PrUsdDictionary{count, entries};
}

// The keys that the items of list ops combine by, and an item of each key.
struct ListKeys {
    UsdScratch* scratch;
    PrUsdType itemType;
    UsdKeyMap index;                    // by key: its position in `items`
    RawArray<const void*> items;

    uint64_t add(uint64_t key, const void* item) {
        uint32_t at = index.find(key);
        if (at == kPrUsdNone) {
            at = items.count;
            index.add(key, at);
            items.push(item);
        }
        return at;
    }

    // Returns the key of the item at `index` of `data`, whose elements have the type `type`.
    uint64_t keyOf(PrUsdType type, const void* data, uint32_t at) {
        switch (type) {
            case PrUsdType_Int: return add(uint64_t(int64_t(static_cast<const int32_t*>(data)[at])),
                static_cast<const int32_t*>(data) + at);
            case PrUsdType_UInt: return add(static_cast<const uint32_t*>(data)[at], static_cast<const uint32_t*>(data) + at);
            case PrUsdType_Int64: return add(uint64_t(static_cast<const int64_t*>(data)[at]),
                static_cast<const int64_t*>(data) + at);
            case PrUsdType_UInt64: return add(static_cast<const uint64_t*>(data)[at], static_cast<const uint64_t*>(data) + at);
            case PrUsdType_Reference:
            case PrUsdType_Payload: {
                // A reference is its asset, its prim and its retiming.
                const PrUsdReference* item = static_cast<const PrUsdReference*>(data) + at;
                PrTextBuffer text = {};
                prTextBufferAppend(&text, item->assetPath);
                prTextBufferAppendChar(&text, '\n');
                prTextBufferAppend(&text, item->primPath);
                prTextBufferAppendChar(&text, '\n');
                prTextBufferAppend(&text,
                    PrStringView{reinterpret_cast<const char*>(&item->layerOffset), sizeof(item->layerOffset)});
                uint64_t key = add(atomTableAdd(&scratch->keys, prTextBufferView(&text)), item);
                prTextBufferReset(&text);
                return key;
            }
            default: return text(static_cast<const PrStringView*>(data)[at]);
        }
    }

    // Returns the key of a string, a token or a path. The item of the key is a view of the scratch's own copy.
    uint64_t text(PrStringView value) {
        Atom atom = atomTableAdd(&scratch->keys, value);
        uint32_t at = index.find(uint64_t(atom) | uint64_t(1) << 40);
        if (at == kPrUsdNone) {
            PrStringView* view = scratch->arena.allocZeroed<PrStringView>();
            *view = atomTableText(&scratch->keys, atom);
            at = items.count;
            index.add(uint64_t(atom) | uint64_t(1) << 40, at);
            items.push(view);
        }
        return at;
    }

    void reset() {
        index.reset();
        items.reset();
    }
};

// Combines list ops, strongest first, into the list that they make of an empty list, as an array of items in the
// scratch (usd-core/#list-op-resolution). `mapOf` gives the mapping of each op's paths into the stage's namespace, for
// targets and connections, and a path that does not map is left out.
template <typename MapOf>
PrUsdValue combineListOps(const UsdStage* stage, UsdScratch* scratch, const PrUsdListOp* const* ops, uint32_t count,
        bool mapPaths, MapOf mapOf) {
    ListKeys keys{scratch, ops[0]->itemType, {}, {}};
    RawArray<uint64_t> list{};
    PrTextBuffer mapped = {};
    for (uint32_t i = count; i-- > 0;) {
        const PrUsdListOp* op = ops[i];
        if (op->itemType != keys.itemType)
            continue;
        RawArray<uint64_t> lists[PrUsdListOpList_Count] = {};
        for (uint32_t which = 0; which < PrUsdListOpList_Count; ++which) {
            for (uint32_t j = 0; j < op->items[which].count; ++j) {
                if (!mapPaths) {
                    lists[which].push(keys.keyOf(op->itemType, op->items[which].data, j));
                    continue;
                }
                prTextBufferClear(&mapped);
                if (mapOf(i, static_cast<const PrStringView*>(op->items[which].data)[j], &mapped))
                    lists[which].push(keys.text(prTextBufferView(&mapped)));
            }
        }
        usdListApply(&list, lists, op->isExplicit);
        for (RawArray<uint64_t>& each : lists)
            each.reset();
    }
    prTextBufferReset(&mapped);
    PrUsdValue value{};
    value.type = keys.itemType;
    value.isArray = true;
    value.count = list.count;
    uint32_t size = prUsdTypeSize(keys.itemType);
    uint8_t* data = static_cast<uint8_t*>(scratch->arena.allocBytes(size_t(size) * list.count, 8));
    for (uint32_t i = 0; i < list.count; ++i)
        prMemCopy(data + size_t(i) * size, keys.items.data[list.data[i]], size);
    value.data = data;
    (void)stage;
    list.reset();
    keys.reset();
    return value;
}

// Resolves the field `name` across `opinions`, strongest first, by the rules of metadata: the strongest value, with
// dictionaries combined and list ops made into the explicit list op of what they combine to.
bool resolveField(const UsdStage* stage, const StageOpinion* opinions, uint32_t count, PrStringView name,
        UsdScratch* scratch, PrUsdValue* out) {
    RawArray<const PrUsdField*> fields{};
    for (uint32_t i = 0; i < count; ++i) {
        const PrUsdField* field = opinionField(stage, opinions[i], name);
        if (field != nullptr)
            fields.push(field);
    }
    if (fields.count == 0)
        return false;
    *out = fields.data[0]->value;
    if (out->type == PrUsdType_Dictionary && !out->isArray) {
        PrUsdDictionary merged = *static_cast<const PrUsdDictionary*>(out->data);
        bool made = false;
        for (uint32_t i = 1; i < fields.count; ++i) {
            const PrUsdValue& weaker = fields.data[i]->value;
            if (weaker.type == PrUsdType_Dictionary && !weaker.isArray) {
                merged = mergeDictionaries(scratch, merged, *static_cast<const PrUsdDictionary*>(weaker.data));
                made = true;
            }
        }
        if (made) {
            PrUsdDictionary* held = scratch->arena.allocZeroed<PrUsdDictionary>();
            *held = merged;
            out->data = held;
        }
    } else if (out->type == PrUsdType_ListOp) {
        RawArray<const PrUsdListOp*> ops{};
        for (uint32_t i = 0; i < fields.count; ++i) {
            if (fields.data[i]->value.type == PrUsdType_ListOp)
                ops.push(static_cast<const PrUsdListOp*>(fields.data[i]->value.data));
        }
        PrUsdListOp* made = scratch->arena.allocZeroed<PrUsdListOp>();
        made->itemType = ops.data[0]->itemType;
        made->isExplicit = true;
        made->items[PrUsdListOpList_Explicit] = combineListOps(stage, scratch, ops.data, ops.count, false,
            [](uint32_t, PrStringView, PrTextBuffer*) { return false; });
        for (uint32_t which = PrUsdListOpList_Explicit + 1; which < PrUsdListOpList_Count; ++which) {
            made->items[which].type = made->itemType;
            made->items[which].isArray = true;
        }
        out->data = made;
        ops.reset();
    }
    fields.reset();
    return true;
}

// Appends the path of the stage that a path of where the prims compose is: a path under the source of a prototype is
// the path under the prototype. `own` is the prototype of the prim that asks, which is tried first.
void appendStagePath(const UsdStage* stage, uint32_t own, PrStringView path, PrTextBuffer* out) {
    const UsdComposer* c = &stage->composer;
    // A prim outside every prototype refers to the paths that it composes.
    if (own == kPrUsdNone) {
        prTextBufferAppend(out, path);
        return;
    }
    PrTextBuffer source = {};
    for (uint32_t round = 0; round < stage->prototypes.count + 1; ++round) {
        uint32_t prototype = round == 0 ? own : round - 1;
        if (prototype == kPrUsdNone || (round != 0 && prototype == own))
            continue;
        prTextBufferClear(&source);
        usdPathText(&c->paths, stage->prototypes.data[prototype].source, &source);
        PrStringView prefix = prTextBufferView(&source);
        bool under = path.size >= prefix.size && prMemCompare(path.data, prefix.data, prefix.size) == 0 &&
            (path.size == prefix.size || path.data[prefix.size] == '/' || path.data[prefix.size] == '.');
        if (under) {
            prTextBufferAppendChar(out, '/');
            prTextBufferAppend(out, stage->prims.data[stage->prototypes.data[prototype].root].name);
            prTextBufferAppend(out, PrStringView{path.data + prefix.size, path.size - prefix.size});
            prTextBufferReset(&source);
            return;
        }
    }
    prTextBufferReset(&source);
    prTextBufferAppend(out, path);
}

// Returns the element size and the number of floating-point components of a type that interpolates linearly
// (usd-core/#linear), or zero components for a type that is held.
uint32_t lerpComponents(PrUsdType type, uint32_t* scalarSize) {
    *scalarSize = 0;
    switch (type) {
        case PrUsdType_Half: *scalarSize = 2; return 1;
        case PrUsdType_Half2: *scalarSize = 2; return 2;
        case PrUsdType_Half3: *scalarSize = 2; return 3;
        case PrUsdType_Half4: *scalarSize = 2; return 4;
        case PrUsdType_Float: *scalarSize = 4; return 1;
        case PrUsdType_Float2: *scalarSize = 4; return 2;
        case PrUsdType_Float3: *scalarSize = 4; return 3;
        case PrUsdType_Float4: *scalarSize = 4; return 4;
        case PrUsdType_Double:
        case PrUsdType_TimeCode: *scalarSize = 8; return 1;
        case PrUsdType_Double2: *scalarSize = 8; return 2;
        case PrUsdType_Double3: *scalarSize = 8; return 3;
        case PrUsdType_Double4: *scalarSize = 8; return 4;
        case PrUsdType_Matrix2d: *scalarSize = 8; return 4;
        case PrUsdType_Matrix3d: *scalarSize = 8; return 9;
        case PrUsdType_Matrix4d: *scalarSize = 8; return 16;
        case PrUsdType_Quath: *scalarSize = 2; return 4;
        case PrUsdType_Quatf: *scalarSize = 4; return 4;
        case PrUsdType_Quatd: *scalarSize = 8; return 4;
        default: return 0;
    }
}

double readScalar(const uint8_t* data, uint32_t size) {
    if (size == 8) {
        double value = 0;
        prMemCopy(&value, data, 8);
        return value;
    }
    if (size == 4) {
        float value = 0;
        prMemCopy(&value, data, 4);
        return double(value);
    }
    PrFloat16 value{};
    prMemCopy(&value, data, 2);
    return double(prFloat16ToFloat(value));
}

void writeScalar(uint8_t* data, uint32_t size, double value) {
    if (size == 8) {
        prMemCopy(data, &value, 8);
    } else if (size == 4) {
        float narrow = float(value);
        prMemCopy(data, &narrow, 4);
    } else {
        PrFloat16 narrow = prFloat16FromFloat(float(value));
        prMemCopy(data, &narrow, 2);
    }
}

// Interpolates between two samples of one type and one length, into the scratch. A quaternion turns along the shorter
// arc, and every other type interpolates each of its numbers.
PrUsdValue interpolate(UsdScratch* scratch, const PrUsdValue& a, const PrUsdValue& b, double alpha) {
    uint32_t scalarSize = 0;
    uint32_t components = lerpComponents(a.type, &scalarSize);
    uint32_t elementSize = components * scalarSize;
    uint8_t* data = static_cast<uint8_t*>(scratch->arena.allocBytes(size_t(elementSize) * a.count, 8));
    const uint8_t* from = static_cast<const uint8_t*>(a.data);
    const uint8_t* to = static_cast<const uint8_t*>(b.data);
    bool quaternion = a.type == PrUsdType_Quath || a.type == PrUsdType_Quatf || a.type == PrUsdType_Quatd;
    for (uint32_t i = 0; i < a.count; ++i) {
        const uint8_t* p = from + size_t(i) * elementSize;
        const uint8_t* q = to + size_t(i) * elementSize;
        uint8_t* r = data + size_t(i) * elementSize;
        double weightA = 1.0 - alpha;
        double weightB = alpha;
        if (quaternion) {
            double cosine = 0;
            for (uint32_t k = 0; k < 4; ++k)
                cosine += readScalar(p + k * scalarSize, scalarSize) * readScalar(q + k * scalarSize, scalarSize);
            bool flip = cosine < 0;
            if (flip)
                cosine = -cosine;
            if (1.0 - cosine > 0.00001) {
                double theta = prF64Acos(cosine);
                double sine = prF64Sin(theta);
                weightA = prF64Sin((1.0 - alpha) * theta) / sine;
                weightB = prF64Sin(alpha * theta) / sine;
            }
            if (flip)
                weightB = -weightB;
        }
        for (uint32_t k = 0; k < components; ++k) {
            double x = readScalar(p + k * scalarSize, scalarSize);
            double y = readScalar(q + k * scalarSize, scalarSize);
            writeScalar(r + k * scalarSize, scalarSize, weightA * x + weightB * y);
        }
    }
    PrUsdValue value = a;
    value.data = data;
    return value;
}

// Evaluates time samples at a time of the stage, which `offset` maps the samples' times to. Returns false if the value
// there is a block.
//
// The time is compared with the samples' times as the stage has them, and not mapped back to the layer's: the retiming
// rounds, and a sample is the value at the time that the stage lists it at.
bool sampleAt(UsdScratch* scratch, const PrUsdTimeSamples& samples, PrUsdLayerOffset offset, double time, PrUsdValue* out) {
    auto staged = [&](uint32_t index) { return samples.times[index] * offset.scale + offset.offset; };
    // The last sample at or before the time, by bisection.
    uint32_t low = 0;
    uint32_t high = samples.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (staged(middle) <= time)
            low = middle + 1;
        else
            high = middle;
    }
    uint32_t lower = low == 0 ? 0 : low - 1;
    const PrUsdValue& held = samples.values[lower];
    if (held.type == PrUsdType_Block)
        return false;
    *out = held;
    if (low == 0 || low == samples.count || staged(lower) == time)
        return true;
    const PrUsdValue& next = samples.values[low];
    uint32_t scalarSize = 0;
    if (next.type != held.type || next.isArray != held.isArray || next.count != held.count ||
            lerpComponents(held.type, &scalarSize) == 0)
        return true;
    double alpha = (time - staged(lower)) / (staged(low) - staged(lower));
    *out = interpolate(scratch, held, next, alpha);
    return true;
}

// Finds the opinion that decides an attribute's value at a time, or at the default time for a NaN: the strongest with
// time samples, at a time, or with a default. Returns its samples in `*samples`, or its default in `*value` with null
// samples. Returns false if no opinion has either.
bool decidingOpinion(const UsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time, StageOpinion* decides,
        const PrUsdTimeSamples* PR_NULLABLE* PR_NONNULL samples, PrUsdValue* value) {
    RawArray<StageOpinion> opinions{};
    propertyOpinions(stage, prim, property, &opinions);
    bool found = false;
    *samples = nullptr;
    for (uint32_t i = 0; !found && i < opinions.count; ++i) {
        *decides = opinions.data[i];
        if (!prF64IsNan(time)) {
            const PrUsdField* field = opinionField(stage, *decides, "timeSamples");
            if (field != nullptr && field->value.type == PrUsdType_TimeSamples &&
                    static_cast<const PrUsdTimeSamples*>(field->value.data)->count != 0) {
                *samples = static_cast<const PrUsdTimeSamples*>(field->value.data);
                found = true;
                continue;
            }
        }
        const PrUsdField* field = opinionField(stage, *decides, "default");
        if (field != nullptr) {
            *value = field->value;
            found = true;
        }
    }
    opinions.reset();
    return found;
}

bool attributeValue(const UsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time, UsdScratch* scratch,
        PrUsdValue* out) {
    StageOpinion decides{};
    const PrUsdTimeSamples* samples = nullptr;
    if (!decidingOpinion(stage, prim, property, time, &decides, &samples, out))
        return false;
    PrUsdLayerOffset offset = stage->offsets.data[decides.offset];
    if (samples != nullptr ? !sampleAt(scratch, *samples, offset, time, out) : out->type == PrUsdType_Block)
        return false;
    if (out->type == PrUsdType_TimeCode && !usdOffsetIsIdentity(offset)) {
        // A time code is a time of its layer, and resolves to the stage's.
        double* retimed = scratch->arena.allocZeroed<double>(out->count);
        for (uint32_t i = 0; i < out->count; ++i)
            retimed[i] = static_cast<const double*>(out->data)[i] * offset.scale + offset.offset;
        out->data = retimed;
    }
    return true;
}

// Returns the prim at a path of the stage, or kPrUsdNone.
PrUsdPrimIndex findPrim(const UsdStage* stage, PrStringView path) {
    const UsdComposer* c = &stage->composer;
    uint32_t prototype = kPrUsdNone;
    UsdPath found = kUsdPathNone;
    if (path.size > kPrototypePrefix.size + 1 &&
            prMemCompare(path.data + 1, kPrototypePrefix.data, kPrototypePrefix.size) == 0) {
        // A path under a prototype is the rest of the path under the prototype's source.
        size_t end = 1;
        while (end < path.size && path.data[end] != '/')
            ++end;
        PrStringView name{path.data + 1, end - 1};
        for (uint32_t i = 0; prototype == kPrUsdNone && i < stage->prototypes.count; ++i) {
            if (stage->prims.data[stage->prototypes.data[i].root].name == name)
                prototype = i;
        }
        if (prototype == kPrUsdNone)
            return kPrUsdNone;
        PrTextBuffer text = {};
        usdPathText(&c->paths, stage->prototypes.data[prototype].source, &text);
        prTextBufferAppend(&text, PrStringView{path.data + end, path.size - end});
        found = usdPathLookup(&c->paths, prTextBufferView(&text));
        prTextBufferReset(&text);
    } else {
        found = usdPathLookup(&c->paths, path);
    }
    if (found == kUsdPathNone)
        return kPrUsdNone;
    return stage->primOfPath.find(primKey(prototype, found));
}

// Returns the position, among a prim's properties in the order of their names' bytes, of the first whose name is not
// before `name`, as an index of UsdStage::propertiesByName.
uint32_t propertyLowerBound(const UsdStage* stage, PrUsdPrimIndex prim, PrStringView name) {
    const PrUsdPrim& p = stage->prims.data[prim];
    uint32_t low = p.firstProperty;
    uint32_t high = p.firstProperty + p.propertyCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (prStringCompare(stage->properties.data[stage->propertiesByName.data[middle]].name, name) < 0)
            low = middle + 1;
        else
            high = middle;
    }
    return low;
}

// Returns the property of a prim that has the name, as an index of the stage's properties, or kPrUsdNone.
uint32_t findProperty(const UsdStage* stage, PrUsdPrimIndex prim, PrStringView name) {
    const PrUsdPrim& p = stage->prims.data[prim];
    uint32_t at = propertyLowerBound(stage, prim, name);
    if (at == p.firstProperty + p.propertyCount)
        return kPrUsdNone;
    uint32_t property = stage->propertiesByName.data[at];
    return stage->properties.data[property].name == name ? property : kPrUsdNone;
}

// Resolves the targets of a property into `list`, as texts in the scratch, following relationships if `forward`.
void collectTargets(const UsdStage* stage, PrUsdPrimIndex prim, uint32_t property, bool forward, UsdScratch* scratch,
        RawArray<PrStringView>* list, bool* authored, uint32_t* invalid, RawArray<uint32_t>* visited) {
    // A relationship is followed once, so that a chain that returns to it ends.
    if (visited->find(property) != kArrayMissing)
        return;
    visited->push(property);
    RawArray<StageOpinion> opinions{};
    propertyOpinions(stage, prim, property, &opinions);
    PrStringView name = "connectionPaths";
    if (stage->properties.data[property].form == PrUsdSpecForm_Relationship)
        name = "targetPaths";
    RawArray<const PrUsdListOp*> ops{};
    RawArray<uint32_t> owners{};
    for (uint32_t i = 0; i < opinions.count; ++i) {
        const PrUsdField* field = opinionField(stage, opinions.data[i], name);
        if (field != nullptr && field->value.type == PrUsdType_ListOp &&
                static_cast<const PrUsdListOp*>(field->value.data)->itemType == PrUsdType_Path) {
            ops.push(static_cast<const PrUsdListOp*>(field->value.data));
            owners.push(i);
            // An explicit list replaces what the weaker opinions list, so their paths are not mapped, nor counted where
            // they do not map.
            if (ops.data[ops.count - 1]->isExplicit)
                break;
        }
    }
    if (ops.count != 0) {
        *authored = true;
        uint32_t prototype = stage->primPrototype.data[prim];
        PrTextBuffer composed = {};
        PrUsdValue targets = combineListOps(stage, scratch, ops.data, ops.count, true,
            [&](uint32_t op, PrStringView path, PrTextBuffer* mapped) {
                prTextBufferClear(&composed);
                if (!usdMapChainTextToTarget(&stage->composer.maps, opinions.data[owners.data[op]].chain, path,
                        &composed)) {
                    ++*invalid;
                    return false;
                }
                appendStagePath(stage, prototype, prTextBufferView(&composed), mapped);
                return true;
            });
        prTextBufferReset(&composed);
        const PrStringView* paths = static_cast<const PrStringView*>(targets.data);
        for (uint32_t i = 0; i < targets.count; ++i) {
            // A target that is a relationship stands for that relationship's targets.
            size_t period = prStringFindByte(paths[i], '.');
            PrUsdPrimIndex owner = kPrUsdNone;
            uint32_t found = kPrUsdNone;
            if (forward && period != paths[i].size && visited->count < kMaxForwarding) {
                owner = findPrim(stage, PrStringView{paths[i].data, period});
                if (owner != kPrUsdNone)
                    found = findProperty(stage, owner, PrStringView{paths[i].data + period + 1, paths[i].size - period - 1});
            }
            if (found != kPrUsdNone && stage->properties.data[found].form == PrUsdSpecForm_Relationship) {
                bool unused = false;
                collectTargets(stage, owner, found, true, scratch, list, &unused, invalid, visited);
            } else if (list->find(paths[i]) == kArrayMissing) {
                list->push(paths[i]);
            }
        }
    }
    opinions.reset();
    ops.reset();
    owners.reset();
}

// Returns true if a variant of `variantSet` named `variant` is authored at one of the prim's opinions.
bool hasVariant(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView variantSet, PrStringView variant) {
    const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
    PrTextBuffer path = {};
    bool found = false;
    for (uint32_t i = 0; i < record.opinionCount && !found; ++i) {
        PrUsdOpinion opinion = prUsdStageOpinion(stage, record.firstOpinion + i);
        const PrUsdLayer* layer = prUsdStageLayer(stage, opinion.layer);
        prTextBufferClear(&path);
        prTextBufferAppend(&path, prUsdLayerSpecs(layer)[opinion.spec].path);
        prTextBufferAppendChar(&path, '{');
        prTextBufferAppend(&path, variantSet);
        prTextBufferAppendChar(&path, '=');
        prTextBufferAppend(&path, variant);
        prTextBufferAppendChar(&path, '}');
        found = prUsdLayerFindSpec(layer, prTextBufferView(&path)) != kPrUsdNone;
    }
    prTextBufferReset(&path);
    return found;
}

// Returns true if the prim has a variant set named `variantSet`.
bool hasVariantSet(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView variantSet) {
    PrUsdScratch* scratch = prUsdScratchCreate();
    PrUsdValue sets{};
    bool found = false;
    if (prUsdStagePrimVariantSets(stage, prim, scratch, &sets)) {
        const PrStringView* names = static_cast<const PrStringView*>(sets.data);
        for (uint32_t i = 0; i < sets.count && !found; ++i)
            found = names[i] == variantSet;
    }
    prUsdScratchDestroy(scratch);
    return found;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrUsdStage : UsdStage {};
struct PrUsdScratch : UsdScratch {};

PrUsdStageOptions prUsdStageOptionsDefault(void) {
    return PrUsdStageOptions{};
}

PrStatus prUsdStageCreate(PrStringView location, const PrUsdStageOptions* PR_NULLABLE options,
        PrUsdStage* PR_NULLABLE* PR_NONNULL out) {
    *out = nullptr;
    PrUsdStage* stage = allocZeroed<PrUsdStage>();
    usdComposerInit(&stage->composer, options);
    PrStatus status = PrStatus_Ok;
    stage->rootStack = usdComposerOpen(&stage->composer, location, &status);
    if (stage->rootStack == kPrUsdNone) {
        prUsdStageDestroy(stage);
        return status == PrStatus_Ok ? PrStatus_Io : status;
    }
    populateStage(stage);
    *out = stage;
    return PrStatus_Ok;
}

void prUsdStageDestroy(PrUsdStage* PR_NULLABLE stage) {
    if (stage == nullptr)
        return;
    for (uint32_t i = 0; i < stage->prototypes.count; ++i)
        stage->prototypes.data[i].graph.nodes.reset();
    usdComposerReset(&stage->composer);
    stage->prims.reset();
    stage->primPaths.reset();
    stage->primPrototype.reset();
    stage->primOfPath.reset();
    stage->opinions.reset();
    stage->offsets.reset();
    stage->properties.reset();
    stage->propertiesByName.reset();
    stage->selections.reset();
    stage->prototypes.reset();
    stage->prototypeKeys.reset();
    stage->prototypeOfHash.reset();
    stage->errors.reset();
    prMemoryFree(stage);
}

uint32_t prUsdStageErrorCount(const PrUsdStage* stage) {
    return stage->errors.count;
}

const PrUsdError* PR_NULLABLE prUsdStageErrors(const PrUsdStage* stage) {
    return stage->errors.data;
}

uint32_t prUsdStageLayerCount(const PrUsdStage* stage) {
    return stage->composer.layers.count;
}

const PrUsdLayer* PR_NULLABLE prUsdStageLayer(const PrUsdStage* stage, uint32_t layer) {
    PR_ASSERT(layer < stage->composer.layers.count);
    return stage->composer.layers.data[layer].layer;
}

PrStringView prUsdStageLayerLocation(const PrUsdStage* stage, uint32_t layer) {
    PR_ASSERT(layer < stage->composer.layers.count);
    return stage->composer.layers.data[layer].location;
}

uint32_t prUsdStagePrimCount(const PrUsdStage* stage) {
    return stage->prims.count;
}

const PrUsdPrim* prUsdStagePrims(const PrUsdStage* stage) {
    return stage->prims.data;
}

uint32_t prUsdStagePrototypeCount(const PrUsdStage* stage) {
    return stage->prototypes.count;
}

PrUsdPrimIndex prUsdStagePrototype(const PrUsdStage* stage, uint32_t prototype) {
    PR_ASSERT(prototype < stage->prototypes.count);
    return stage->prototypes.data[prototype].root;
}

PrUsdPrimIndex prUsdStageFindPrim(const PrUsdStage* stage, PrStringView path) {
    return findPrim(stage, path);
}

void prUsdStagePrimPath(const PrUsdStage* stage, PrUsdPrimIndex prim, PrTextBuffer* out) {
    PR_ASSERT(prim < stage->prims.count);
    PrTextBuffer text = {};
    usdPathText(&stage->composer.paths, stage->primPaths.data[prim], &text);
    appendStagePath(stage, stage->primPrototype.data[prim], prTextBufferView(&text), out);
    prTextBufferReset(&text);
}

PrUsdOpinion prUsdStageOpinion(const PrUsdStage* stage, uint32_t index) {
    PR_ASSERT(index < stage->opinions.count);
    const StageOpinion& opinion = stage->opinions.data[index];
    return PrUsdOpinion{opinion.layer, opinion.spec, opinion.arc, stage->offsets.data[opinion.offset]};
}

const PrUsdProperty* PR_NULLABLE prUsdStageProperties(const PrUsdStage* stage) {
    return stage->properties.data;
}

uint32_t prUsdStageFindProperty(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name) {
    PR_ASSERT(prim < stage->prims.count);
    return findProperty(stage, prim, name);
}

uint32_t prUsdStagePropertiesWithPrefix(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView prefix,
        const uint32_t* PR_NULLABLE* PR_NONNULL indices) {
    PR_ASSERT(prim < stage->prims.count);
    *indices = nullptr;
    // The names that begin with the prefix follow one another in the order of bytes, from the first that is not before
    // the prefix to the first that does not begin with it, which a second search finds among the names after.
    const PrUsdPrim& p = stage->prims.data[prim];
    uint32_t first = propertyLowerBound(stage, prim, prefix);
    uint32_t low = first;
    uint32_t high = p.firstProperty + p.propertyCount;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (prStringStartsWith(stage->properties.data[stage->propertiesByName.data[middle]].name, prefix))
            low = middle + 1;
        else
            high = middle;
    }
    if (low != first)
        *indices = stage->propertiesByName.data + first;
    return low - first;
}

uint32_t prUsdStagePropertyOpinions(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property,
        PrUsdOpinion* PR_NULLABLE out, uint32_t capacity) {
    RawArray<StageOpinion> opinions{};
    propertyOpinions(stage, prim, property, &opinions);
    for (uint32_t i = 0; i < opinions.count && i < capacity; ++i) {
        const StageOpinion& opinion = opinions.data[i];
        out[i] = PrUsdOpinion{opinion.layer, opinion.spec, opinion.arc, stage->offsets.data[opinion.offset]};
    }
    uint32_t count = opinions.count;
    opinions.reset();
    return count;
}

PrUsdScratch* prUsdScratchCreate(void) {
    PrUsdScratch* scratch = allocZeroed<PrUsdScratch>();
    scratch->arena.largestPage = kUsdArenaLargestPage;
    return scratch;
}

void prUsdScratchClear(PrUsdScratch* scratch) {
    scratch->arena.clear();
    atomTableClear(&scratch->keys);
}

void prUsdScratchDestroy(PrUsdScratch* PR_NULLABLE scratch) {
    if (scratch == nullptr)
        return;
    scratch->arena.reset();
    atomTableReset(&scratch->keys);
    prMemoryFree(scratch);
}

void* prUsdScratchAlloc(PrUsdScratch* scratch, size_t size, size_t alignment) {
    return scratch->arena.allocBytesZeroed(size, alignment);
}

bool prUsdStageField(const PrUsdStage* stage, PrStringView name, PrUsdValue* out) {
    const UsdComposer* c = &stage->composer;
    uint32_t root = c->layerStacks.data[stage->rootStack].root;
    const PrUsdField* field = prUsdLayerFindField(c->layers.data[root].layer, kPrUsdLayerSpec, name);
    if (field == nullptr)
        return false;
    *out = field->value;
    return true;
}

double prUsdStageTimeCodesPerSecond(const PrUsdStage* stage) {
    return stage->composer.layerStacks.data[stage->rootStack].timeCodesPerSecond;
}

bool prUsdStagePrimField(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView name, PrUsdScratch* scratch,
        PrUsdValue* out) {
    PR_ASSERT(prim < stage->prims.count);
    const PrUsdPrim& p = stage->prims.data[prim];
    if (name == "specifier" || name == "typeName") {
        // The two fields that a prim resolves by rules of their own, which composing the prim applied.
        bool specifier = name == "specifier";
        if (p.opinionCount == 0 || (!specifier && p.typeName.size == 0))
            return false;
        out->type = specifier ? PrUsdType_Specifier : PrUsdType_Token;
        out->isArray = false;
        out->count = 1;
        out->data = specifier ? static_cast<const void*>(&p.specifier) : static_cast<const void*>(&p.typeName);
        return true;
    }
    return resolveField(stage, stage->opinions.data + p.firstOpinion, p.opinionCount, name, scratch, out);
}

bool prUsdStagePropertyField(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, PrStringView name,
        PrUsdScratch* scratch, PrUsdValue* out) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    RawArray<StageOpinion> opinions{};
    propertyOpinions(stage, prim, property, &opinions);
    bool found = false;
    if (name == "variability" && opinions.count != 0) {
        // An attribute's variability is its weakest opinion (usd-core/#variability), and the weakest spec of its name
        // decides even if it is a relationship's, which is uniform.
        const UsdComposer* c = &stage->composer;
        const PrUsdPrim& p = stage->prims.data[prim];
        static const PrUsdVariability kUniform = PrUsdVariability_Uniform;
        for (uint32_t i = p.opinionCount; !found && i-- > 0;) {
            const StageOpinion& opinion = stage->opinions.data[p.firstOpinion + i];
            UsdPath path = usdPathFindProperty(&c->paths, c->layers.data[opinion.layer].specPaths.data[opinion.spec],
                stage->properties.data[property].name);
            PrUsdSpecIndex spec = path == kUsdPathNone ? kPrUsdNone : usdComposerSpec(c, opinion.layer, path);
            if (spec == kPrUsdNone)
                continue;
            const PrUsdLayer* layer = c->layers.data[opinion.layer].layer;
            const PrUsdField* field = prUsdLayerFindField(layer, spec, name);
            found = true;
            if (prUsdLayerSpecs(layer)[spec].form == PrUsdSpecForm_Relationship || field == nullptr)
                *out = PrUsdValue{PrUsdType_Variability, false, 1, &kUniform};
            else
                *out = field->value;
        }
    } else if (name == "custom") {
        // A property is custom if any of its specs says so.
        for (uint32_t i = 0; i < opinions.count; ++i) {
            const PrUsdField* field = opinionField(stage, opinions.data[i], name);
            if (field == nullptr || field->value.type != PrUsdType_Bool)
                continue;
            if (!found || *static_cast<const bool*>(field->value.data))
                *out = field->value;
            found = true;
        }
    } else {
        found = resolveField(stage, opinions.data, opinions.count, name, scratch, out);
    }
    opinions.reset();
    return found;
}

bool prUsdStageAttributeValue(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
        PrUsdScratch* scratch, PrUsdValue* out) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    return attributeValue(stage, prim, property, time, scratch, out);
}

bool prUsdStageAttributeRead(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
        PrUsdType type, void* PR_NULLABLE out, uint32_t capacity, uint32_t* count) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    *count = 0;
    UsdScratch scratch{};
    PrUsdValue value{};
    bool found = attributeValue(stage, prim, property, time, &scratch, &value) && value.type == type;
    if (found) {
        *count = value.count;
        found = value.count <= capacity;
        if (found && value.count != 0)
            prMemCopy(out, value.data, size_t(value.count) * prUsdTypeSize(type));
    }
    scratch.arena.reset();
    atomTableReset(&scratch.keys);
    return found;
}

uint32_t prUsdStageAttributeTimes(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double start,
        double end, PrUsdScratch* scratch, const double* PR_NULLABLE* PR_NONNULL times) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    *times = nullptr;
    RawArray<StageOpinion> opinions{};
    propertyOpinions(stage, prim, property, &opinions);
    uint32_t count = 0;
    for (uint32_t i = 0; i < opinions.count; ++i) {
        const PrUsdField* field = opinionField(stage, opinions.data[i], "timeSamples");
        if (field == nullptr || field->value.type != PrUsdType_TimeSamples)
            continue;
        const PrUsdTimeSamples& samples = *static_cast<const PrUsdTimeSamples*>(field->value.data);
        if (samples.count == 0)
            continue;
        PrUsdLayerOffset offset = stage->offsets.data[opinions.data[i].offset];
        double* mapped = scratch->arena.allocZeroed<double>(samples.count);
        for (uint32_t j = 0; j < samples.count; ++j) {
            double time = samples.times[j] * offset.scale + offset.offset;
            if (time >= start && time <= end)
                mapped[count++] = time;
        }
        *times = mapped;
        break;
    }
    opinions.reset();
    return count;
}

bool prUsdStageTargets(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, bool forward,
        PrUsdScratch* scratch, PrUsdValue* out, uint32_t* PR_NULLABLE invalid) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    RawArray<PrStringView> list{};
    bool authored = false;
    RawArray<uint32_t> visited{};
    uint32_t leftOut = 0;
    collectTargets(stage, prim, property, forward, scratch, &list, &authored, &leftOut, &visited);
    if (invalid != nullptr)
        *invalid = leftOut;
    visited.reset();
    PrStringView* paths = scratch->arena.allocZeroed<PrStringView>(list.count);
    if (list.count != 0)
        prMemCopy(paths, list.data, size_t(list.count) * sizeof(PrStringView));
    out->type = PrUsdType_Path;
    out->isArray = true;
    out->count = list.count;
    out->data = paths;
    list.reset();
    return authored;
}

bool prUsdStagePrimVariantSets(const PrUsdStage* stage, PrUsdPrimIndex prim, PrUsdScratch* scratch, PrUsdValue* out) {
    PR_ASSERT(prim < stage->prims.count);
    const PrUsdPrim& p = stage->prims.data[prim];
    // The names that each site's list ops make across the layers of its stack, strongest site first, each name once.
    RawArray<PrStringView> names{};
    RawArray<const PrUsdListOp*> ops{};
    for (uint32_t i = 0; i <= p.opinionCount; ++i) {
        bool ends = i == p.opinionCount || (stage->opinions.data[p.firstOpinion + i].flags & OpinionFlag_SiteStart) != 0;
        if (ends && ops.count != 0) {
            PrUsdValue made = combineListOps(stage, scratch, ops.data, ops.count, false,
                [](uint32_t, PrStringView, PrTextBuffer*) { return false; });
            for (uint32_t j = 0; j < made.count && made.type != PrUsdType_Reference; ++j) {
                PrStringView name = static_cast<const PrStringView*>(made.data)[j];
                if (names.find(name) == kArrayMissing)
                    names.push(name);
            }
            ops.count = 0;
        }
        if (i == p.opinionCount)
            break;
        const PrUsdField* field = opinionField(stage, stage->opinions.data[p.firstOpinion + i], "variantSetNames");
        if (field != nullptr && field->value.type == PrUsdType_ListOp)
            ops.push(static_cast<const PrUsdListOp*>(field->value.data));
    }
    ops.reset();
    PrStringView* held = scratch->arena.allocZeroed<PrStringView>(names.count);
    if (names.count != 0)
        prMemCopy(held, names.data, size_t(names.count) * sizeof(PrStringView));
    out->type = PrUsdType_String;
    out->isArray = true;
    out->count = names.count;
    out->data = held;
    names.reset();
    return out->count != 0;
}

PrStringView prUsdStagePrimVariantSelection(const PrUsdStage* stage, PrUsdPrimIndex prim, PrStringView variantSet) {
    const AtomTable* names = &stage->composer.paths.names;
    // The selections are in the order of their prims.
    uint32_t low = 0;
    uint32_t high = stage->selections.count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (stage->selections.data[middle].prim < prim)
            low = middle + 1;
        else
            high = middle;
    }
    for (; low < stage->selections.count && stage->selections.data[low].prim == prim; ++low) {
        if (atomTableText(names, stage->selections.data[low].variantSet) == variantSet)
            return atomTableText(names, stage->selections.data[low].variant);
    }
    return PrStringView{};
}

PrRawString prUsdStageAttributeAssetResolve(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
        PrStringView assetPath) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    StageOpinion decides{};
    const PrUsdTimeSamples* samples = nullptr;
    PrUsdValue value{};
    if (!decidingOpinion(stage, prim, property, time, &decides, &samples, &value))
        return prRawStringCopy(assetPath);
    return prUsdAssetResolve(stage->composer.layers.data[decides.layer].location, assetPath);
}

bool prUsdStageAttributeBracket(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property, double time,
        double* lower, double* upper) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    StageOpinion decides{};
    const PrUsdTimeSamples* samples = nullptr;
    PrUsdValue value{};
    if (prF64IsNan(time) || !decidingOpinion(stage, prim, property, time, &decides, &samples, &value) || samples == nullptr)
        return false;
    PrUsdLayerOffset offset = stage->offsets.data[decides.offset];
    auto staged = [&](uint32_t index) { return samples->times[index] * offset.scale + offset.offset; };
    // The first sample after the time, by bisection.
    uint32_t low = 0;
    uint32_t high = samples->count;
    while (low < high) {
        uint32_t middle = low + (high - low) / 2;
        if (staged(middle) <= time)
            low = middle + 1;
        else
            high = middle;
    }
    uint32_t before = low == 0 ? 0 : low - 1;
    *lower = staged(before);
    *upper = low == samples->count || staged(before) == time ? *lower : staged(low);
    if (low == 0)
        *upper = *lower;
    return true;
}

bool prUsdStageAttributeVaries(const PrUsdStage* stage, PrUsdPrimIndex prim, uint32_t property) {
    PR_ASSERT(prim < stage->prims.count && property < stage->properties.count);
    StageOpinion decides{};
    const PrUsdTimeSamples* samples = nullptr;
    PrUsdValue value{};
    return decidingOpinion(stage, prim, property, 0.0, &decides, &samples, &value) && samples != nullptr &&
        samples->count > 1;
}

PrUsdPrimIndex prUsdStageFindProxy(const PrUsdStage* stage, PrStringView path) {
    const UsdComposer* c = &stage->composer;
    if (path.size == 0 || path.data[0] != '/')
        return kPrUsdNone;
    // Each name is looked up under the prim before it, or under that prim's prototype if it is an instance.
    PrUsdPrimIndex prim = kPrUsdPseudoRoot;
    size_t at = 1;
    while (at < path.size && prim != kPrUsdNone) {
        size_t end = at;
        while (end < path.size && path.data[end] != '/')
            ++end;
        PrStringView name{path.data + at, end - at};
        if (prim == kPrUsdPseudoRoot && name.size > kPrototypePrefix.size &&
                prMemEquals(name.data, kPrototypePrefix.data, kPrototypePrefix.size)) {
            PrUsdPrimIndex found = kPrUsdNone;
            for (uint32_t i = 0; found == kPrUsdNone && i < stage->prototypes.count; ++i) {
                if (stage->prims.data[stage->prototypes.data[i].root].name == name)
                    found = stage->prototypes.data[i].root;
            }
            prim = found;
        } else {
            if (stage->prims.data[prim].prototype != kPrUsdNone)
                prim = stage->prims.data[prim].prototype;
            Atom atom = atomTableFind(&c->paths.names, name);
            UsdPath child = usdPathFind(&c->paths, stage->primPaths.data[prim], UsdPathPart_Prim, atom, kAtomNone);
            prim = child == kUsdPathNone ? kPrUsdNone : stage->primOfPath.find(primKey(stage->primPrototype.data[prim], child));
        }
        at = end + 1;
    }
    return prim;
}

PrStatus prUsdStageFindPath(const PrUsdStage* stage, PrStringView path, PrUsdPrimIndex* prim) {
    *prim = kPrUsdNone;
    UsdPathTable table{};
    usdPathTableInit(&table);
    UsdPath parsed = usdPathParse(&table, path);
    PrStatus status = PrStatus_Syntax;
    if (parsed != kUsdPathNone) {
        const UsdPathNode& node = usdPathNode(&table, parsed);
        UsdPath primPath = node.part == UsdPathPart_Prim || node.part == UsdPathPart_Root ? parsed : node.parent;
        PrTextBuffer text = {};
        usdPathText(&table, usdPathNode(&table, primPath).stripped, &text);
        PrUsdPrimIndex found = prUsdStageFindProxy(stage, prTextBufferView(&text));
        prTextBufferReset(&text);
        status = found != kPrUsdNone ? PrStatus_Ok : PrStatus_NotFound;
        if (status == PrStatus_Ok && node.part == UsdPathPart_Property &&
                prUsdStageFindProperty(stage, found, usdPathName(&table, parsed)) == kPrUsdNone)
            status = PrStatus_NotFound;
        if (status == PrStatus_Ok && node.part == UsdPathPart_Variant) {
            PrStringView variantSet = usdPathName(&table, parsed);
            PrStringView variant = atomTableText(&table.names, node.variant);
            bool exists = false;
            if (variant.size == 0)
                exists = hasVariantSet(stage, found, variantSet);
            else
                exists = hasVariant(stage, found, variantSet, variant);
            status = exists ? PrStatus_Ok : PrStatus_NotFound;
        }
        if (status == PrStatus_Ok)
            *prim = found;
    }
    usdPathTableReset(&table);
    return status;
}
