// The paths of a stage as numbers, and the namespace mappings of composition arcs over them.

#include "usd/src/usd_path.h"

#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/hash.h"
#include "pr/mem.h"
#include "pr/sort.h"

#include "usd/src/usd_layer.h"

namespace pr_impl {

namespace {

uint32_t hashNode(UsdPath parent, UsdPathPart part, Atom name, Atom variant) {
    return uint32_t(prHashMixKeys(prHashMixKeys(parent, part), prHashMixKeys(name, variant)));
}

// Returns `text` without the spaces at its ends.
PrStringView trimSpaces(PrStringView text) {
    while (text.size != 0 && text.data[0] == ' ')
        text = PrStringView{text.data + 1, text.size - 1};
    while (text.size != 0 && text.data[text.size - 1] == ' ')
        --text.size;
    return text;
}

// The parts of a path's text, in order, which `visit` receives as (part, name, variant) and stops by returning false.
// Returns false if the text is not an absolute path of the grammar (usd-core/#path-grammar) or `visit` stopped. The
// names of a variant selection are without the spaces that the text format allows about them.
template <typename Visit>
bool walkText(PrStringView text, Visit visit) {
    if (text.size == 0 || text.data[0] != '/' || !usdPathIsValid(text, UsdPathKind_Any))
        return false;
    size_t at = 1;
    while (at < text.size) {
        char c = text.data[at];
        if (c == '/') {
            ++at;
        } else if (c == '{') {
            size_t equals = at + 1;
            while (equals < text.size && text.data[equals] != '=' && text.data[equals] != '}')
                ++equals;
            size_t close = equals;
            while (close < text.size && text.data[close] != '}')
                ++close;
            if (equals >= text.size || text.data[equals] != '=' || close >= text.size)
                return false;
            if (!visit(UsdPathPart_Variant, trimSpaces(PrStringView{text.data + at + 1, equals - at - 1}),
                    trimSpaces(PrStringView{text.data + equals + 1, close - equals - 1})))
                return false;
            at = close + 1;
        } else if (c == '.') {
            return at + 1 < text.size &&
                visit(UsdPathPart_Property, PrStringView{text.data + at + 1, text.size - at - 1}, PrStringView{});
        } else {
            size_t end = at;
            while (end < text.size && text.data[end] != '/' && text.data[end] != '{' && text.data[end] != '.')
                ++end;
            if (!visit(UsdPathPart_Prim, PrStringView{text.data + at, end - at}, PrStringView{}))
                return false;
            at = end;
        }
    }
    return true;
}

// Returns true if the text `prefix` of a path is the text `path` or of an ancestor of it.
bool textHasPrefix(PrStringView path, PrStringView prefix) {
    if (prefix.size == 1)
        return path.size != 0;
    if (path.size < prefix.size || prMemCompare(path.data, prefix.data, prefix.size) != 0)
        return false;
    if (path.size == prefix.size)
        return true;
    char next = path.data[prefix.size];
    return next == '/' || next == '.' || next == '{' || prefix.data[prefix.size - 1] == '}';
}

uint64_t offsetBits(double value) {
    uint64_t bits = 0;
    prMemCopy(&bits, &value, sizeof(bits));
    return bits;
}

uint32_t hashMap(const UsdMapPair* PR_NULLABLE pairs, uint32_t count, PrUsdLayerOffset offset) {
    uint64_t hash = prHashMixKeys(offsetBits(offset.offset), offsetBits(offset.scale));
    for (uint32_t i = 0; i < count; ++i)
        hash = prHashMixKeys(hash, prHashMixKeys(pairs[i].source, pairs[i].target));
    return uint32_t(hash ^ (hash >> 32));
}

// Returns the pair of `map` whose source, or whose target, is the longest prefix of `path`, or null.
const UsdMapPair* PR_NULLABLE bestPair(const UsdMapTable* table, const UsdMap& map, UsdPath path, bool bySource) {
    const UsdMapPair* best = nullptr;
    uint32_t bestDepth = 0;
    for (uint32_t i = 0; i < map.count; ++i) {
        const UsdMapPair& pair = table->pairs.data[map.first + i];
        UsdPath key = bySource ? pair.source : pair.target;
        uint32_t depth = usdPathNode(table->paths, key).depth;
        if ((best == nullptr || depth > bestDepth) && usdPathHasPrefix(table->paths, path, key)) {
            best = &pair;
            bestDepth = depth;
        }
    }
    return best;
}

UsdPath mapPath(UsdMapTable* table, UsdMapId id, UsdPath path, bool toTarget) {
    if (path == kUsdPathNone || id == kUsdMapNull)
        return kUsdPathNone;
    if (id == kUsdMapIdentity)
        return path;
    UsdMap map = table->maps.data[id];
    const UsdMapPair* best = bestPair(table, map, path, toTarget);
    if (best == nullptr)
        return kUsdPathNone;
    UsdPath from = toTarget ? best->source : best->target;
    UsdPath to = toTarget ? best->target : best->source;
    UsdPath result = usdPathReplacePrefix(table->paths, path, from, to);
    // The result maps back by the same pair, or the mapping is not a bijection at this path.
    if (bestPair(table, map, result, !toTarget) != best)
        return kUsdPathNone;
    return result;
}

} // namespace

void usdPathTableInit(UsdPathTable* table) {
    UsdPathNode none{};
    table->nodes.push(none);
    UsdPathNode root{};
    root.name = kAtomNone;
    root.variant = kAtomNone;
    root.stripped = kUsdPathRoot;
    root.part = UsdPathPart_Root;
    table->nodes.push(root);
}

void usdPathTableReset(UsdPathTable* table) {
    atomTableReset(&table->names);
    table->nodes.reset();
    table->index.reset();
}

UsdPath usdPathFind(const UsdPathTable* table, UsdPath parent, UsdPathPart part, Atom name, Atom variant) {
    if (parent == kUsdPathNone || name == kAtomNone)
        return kUsdPathNone;
    uint32_t found = table->index.find(hashNode(parent, part, name, variant), [&](uint32_t at) {
        const UsdPathNode& node = table->nodes.data[at];
        return node.parent == parent && node.part == part && node.name == name && node.variant == variant;
    });
    return found == kHashMissing ? kUsdPathNone : found;
}

UsdPath usdPathFindProperty(const UsdPathTable* table, UsdPath parent, PrStringView name) {
    return usdPathFind(table, parent, UsdPathPart_Property, atomTableFind(&table->names, name), kAtomNone);
}

UsdPath usdPathAppend(UsdPathTable* table, UsdPath parent, UsdPathPart part, Atom name, Atom variant) {
    if (parent == kUsdPathNone)
        return kUsdPathNone;
    UsdPath found = usdPathFind(table, parent, part, name, variant);
    if (found != kUsdPathNone)
        return found;
    UsdPathNode up = table->nodes.data[parent];
    // A path without variant parts is its own stripped path. Adding the stripped path of another adds to the table.
    UsdPath stripped = kUsdPathNone;
    if (part == UsdPathPart_Variant)
        stripped = up.stripped;
    else if (up.stripped != parent)
        stripped = usdPathAppend(table, up.stripped, part, name, variant);
    UsdPath path = table->nodes.count;
    UsdPathNode node{};
    node.parent = parent;
    node.name = name;
    node.variant = variant;
    node.stripped = stripped == kUsdPathNone ? path : stripped;
    node.depth = up.depth + 1;
    node.primDepth = (up.primDepth + (part == UsdPathPart_Prim ? 1 : 0)) & 0xfffffffu;
    node.part = part & 15;
    table->nodes.push(node);
    table->index.insert(hashNode(parent, part, name, variant), path);
    return path;
}

UsdPath usdPathChild(UsdPathTable* table, UsdPath parent, PrStringView name) {
    return usdPathAppend(table, parent, UsdPathPart_Prim, atomTableAdd(&table->names, name), kAtomNone);
}

UsdPath usdPathVariant(UsdPathTable* table, UsdPath parent, PrStringView variantSet, PrStringView variant) {
    return usdPathAppend(table, parent, UsdPathPart_Variant, atomTableAdd(&table->names, variantSet),
        atomTableAdd(&table->names, variant));
}

UsdPath usdPathProperty(UsdPathTable* table, UsdPath parent, PrStringView name) {
    return usdPathAppend(table, parent, UsdPathPart_Property, atomTableAdd(&table->names, name), kAtomNone);
}

UsdPath usdPathParse(UsdPathTable* table, PrStringView text) {
    UsdPath path = kUsdPathRoot;
    bool ok = walkText(text, [&](UsdPathPart part, PrStringView name, PrStringView variant) {
        Atom second = part == UsdPathPart_Variant ? atomTableAdd(&table->names, variant) : kAtomNone;
        path = usdPathAppend(table, path, part, atomTableAdd(&table->names, name), second);
        return true;
    });
    return ok ? path : kUsdPathNone;
}

UsdPath usdPathLookup(const UsdPathTable* table, PrStringView text) {
    UsdPath path = kUsdPathRoot;
    bool ok = walkText(text, [&](UsdPathPart part, PrStringView name, PrStringView variant) {
        Atom second = part == UsdPathPart_Variant ? atomTableFind(&table->names, variant) : kAtomNone;
        if (part == UsdPathPart_Variant && second == kAtomNone)
            return false;
        path = usdPathFind(table, path, part, atomTableFind(&table->names, name), second);
        return path != kUsdPathNone;
    });
    return ok ? path : kUsdPathNone;
}

void usdPathText(const UsdPathTable* table, UsdPath path, PrTextBuffer* out) {
    if (path == kUsdPathNone)
        return;
    if (path == kUsdPathRoot) {
        prTextBufferAppendChar(out, '/');
        return;
    }
    RawArray<UsdPath> chain{};
    for (UsdPath at = path; at != kUsdPathRoot; at = table->nodes.data[at].parent)
        chain.push(at);
    for (uint32_t i = chain.count; i-- > 0;) {
        const UsdPathNode& node = table->nodes.data[chain.data[i]];
        PrStringView name = atomTableText(&table->names, node.name);
        if (node.part == UsdPathPart_Variant) {
            prTextBufferAppendChar(out, '{');
            prTextBufferAppend(out, name);
            prTextBufferAppendChar(out, '=');
            prTextBufferAppend(out, atomTableText(&table->names, node.variant));
            prTextBufferAppendChar(out, '}');
        } else if (node.part == UsdPathPart_Property) {
            prTextBufferAppendChar(out, '.');
            prTextBufferAppend(out, name);
        } else {
            if (table->nodes.data[node.parent].part != UsdPathPart_Variant)
                prTextBufferAppendChar(out, '/');
            prTextBufferAppend(out, name);
        }
    }
    chain.reset();
}

UsdPath usdPathAncestor(const UsdPathTable* table, UsdPath path, uint32_t depth) {
    while (path != kUsdPathNone && table->nodes.data[path].depth > depth)
        path = table->nodes.data[path].parent;
    return path;
}

bool usdPathHasPrefix(const UsdPathTable* table, UsdPath path, UsdPath prefix) {
    if (path == kUsdPathNone || prefix == kUsdPathNone)
        return false;
    return usdPathAncestor(table, path, table->nodes.data[prefix].depth) == prefix;
}

UsdPath usdPathReplacePrefix(UsdPathTable* table, UsdPath path, UsdPath from, UsdPath to) {
    if (path == kUsdPathNone || from == kUsdPathNone || to == kUsdPathNone)
        return kUsdPathNone;
    if (!usdPathHasPrefix(table, path, from))
        return kUsdPathNone;
    if (from == to)
        return path;
    RawArray<UsdPath> chain{};
    for (UsdPath at = path; at != from; at = table->nodes.data[at].parent)
        chain.push(at);
    UsdPath result = to;
    for (uint32_t i = chain.count; i-- > 0;) {
        UsdPathNode node = table->nodes.data[chain.data[i]];
        result = usdPathAppend(table, result, UsdPathPart(node.part), node.name, node.variant);
    }
    chain.reset();
    return result;
}

UsdPath usdPathPrim(const UsdPathTable* table, UsdPath path) {
    if (path != kUsdPathNone && table->nodes.data[path].part == UsdPathPart_Property)
        return table->nodes.data[path].parent;
    return path;
}

int usdNameCompare(PrStringView a, PrStringView b) {
    // A letter orders by its lower case, so the digits come first, then an underscore, then the letters.
    auto rank = [](uint8_t c) -> uint32_t { return prAsciiIsUpper(char(c)) ? uint32_t(c) + 32 : c; };
    size_t i = 0;
    size_t j = 0;
    int tie = 0;
    while (i < a.size && j < b.size) {
        uint8_t ca = uint8_t(a.data[i]);
        uint8_t cb = uint8_t(b.data[j]);
        if (prAsciiIsDigit(char(ca)) && prAsciiIsDigit(char(cb))) {
            // Two numbers compare by their values, and then by their leading zeros.
            size_t zerosA = i;
            while (i < a.size && a.data[i] == '0')
                ++i;
            size_t zerosB = j;
            while (j < b.size && b.data[j] == '0')
                ++j;
            zerosA = i - zerosA;
            zerosB = j - zerosB;
            size_t endA = i;
            while (endA < a.size && prAsciiIsDigit(a.data[endA]))
                ++endA;
            size_t endB = j;
            while (endB < b.size && prAsciiIsDigit(b.data[endB]))
                ++endB;
            if (endA - i != endB - j)
                return endA - i < endB - j ? -1 : 1;
            int order = prMemCompare(a.data + i, b.data + j, endA - i);
            if (order != 0)
                return order < 0 ? -1 : 1;
            if (tie == 0 && zerosA != zerosB)
                tie = zerosA < zerosB ? -1 : 1;
            i = endA;
            j = endB;
            continue;
        }
        if (rank(ca) != rank(cb))
            return rank(ca) < rank(cb) ? -1 : 1;
        if (tie == 0 && ca != cb)
            tie = ca < cb ? -1 : 1;
        ++i;
        ++j;
    }
    if (i < a.size)
        return 1;
    if (j < b.size)
        return -1;
    return tie;
}

// ---------------------------------------------------------------------------------------------------------------------
// Namespace mappings

PrUsdLayerOffset usdOffsetCompose(PrUsdLayerOffset outer, PrUsdLayerOffset inner) {
    return PrUsdLayerOffset{outer.scale * inner.offset + outer.offset, outer.scale * inner.scale};
}

PrUsdLayerOffset usdOffsetInverse(PrUsdLayerOffset offset) {
    double scale = 1.0 / offset.scale;
    return PrUsdLayerOffset{-offset.offset * scale, scale};
}

void usdMapTableInit(UsdMapTable* table, UsdPathTable* paths) {
    table->paths = paths;
    table->maps.push(UsdMap{0, 0, PrUsdLayerOffset{0.0, 1.0}});
    table->inverses.push(kUsdMapNull);
    UsdMapPair identity{kUsdPathRoot, kUsdPathRoot};
    UsdMapId made = usdMapMake(table, &identity, 1, PrUsdLayerOffset{0.0, 1.0});
    PR_ASSERT(made == kUsdMapIdentity);
    (void)made;
    table->chains.push(UsdMapChain{kUsdMapIdentity, kUsdMapChainNone});
}

void usdMapTableReset(UsdMapTable* table) {
    table->pairs.reset();
    table->maps.reset();
    table->index.reset();
    table->composed.reset();
    table->inverses.reset();
    table->chains.reset();
    table->chainOf.reset();
}

UsdMapId usdMapMake(UsdMapTable* table, const UsdMapPair* PR_NULLABLE pairs, uint32_t count, PrUsdLayerOffset offset) {
    const UsdPathTable* paths = table->paths;
    // The pairs in the order of their sources, with the first of the pairs that share a source.
    RawArray<UsdMapPair> sorted{};
    for (uint32_t i = 0; i < count; ++i) {
        bool repeats = pairs[i].source == kUsdPathNone;
        for (uint32_t j = 0; !repeats && j < sorted.count; ++j)
            repeats = sorted.data[j].source == pairs[i].source;
        if (!repeats)
            sorted.push(pairs[i]);
    }
    // A pair that the pair above it implies, in both directions, says nothing.
    RawArray<UsdMapPair> kept{};
    for (uint32_t i = 0; i < sorted.count; ++i) {
        const UsdMapPair& pair = sorted.data[i];
        uint32_t above = kPrUsdNone;
        uint32_t aboveTarget = kPrUsdNone;
        for (uint32_t j = 0; j < sorted.count; ++j) {
            const UsdMapPair& other = sorted.data[j];
            if (j == i)
                continue;
            if (usdPathHasPrefix(paths, pair.source, other.source) && (above == kPrUsdNone ||
                    usdPathNode(paths, other.source).depth > usdPathNode(paths, sorted.data[above].source).depth))
                above = j;
            if (usdPathHasPrefix(paths, pair.target, other.target) && (aboveTarget == kPrUsdNone ||
                    usdPathNode(paths, other.target).depth > usdPathNode(paths, sorted.data[aboveTarget].target).depth))
                aboveTarget = j;
        }
        bool implied = above != kPrUsdNone && above == aboveTarget && pair.target ==
            usdPathReplacePrefix(table->paths, pair.source, sorted.data[above].source, sorted.data[above].target);
        // A block under no pair, or under another block, blocks nothing that would map.
        if (pair.target == kUsdPathNone && (above == kPrUsdNone || sorted.data[above].target == kUsdPathNone))
            implied = true;
        if (pair.target == kUsdPathNone && !implied) {
            // So does a block of a path whose image another pair's target claims, which does not map back.
            UsdPath image =
                usdPathReplacePrefix(table->paths, pair.source, sorted.data[above].source, sorted.data[above].target);
            uint32_t depth = usdPathNode(paths, sorted.data[above].target).depth;
            for (uint32_t j = 0; !implied && j < sorted.count; ++j) {
                UsdPath other = sorted.data[j].target;
                implied = other != kUsdPathNone && usdPathNode(paths, other).depth > depth &&
                    usdPathHasPrefix(paths, image, other);
            }
        }
        if (!implied)
            kept.push(pair);
    }
    sorted.reset();
    if (kept.count == 0)
        return kUsdMapNull;
    sortItems(Span<UsdMapPair>(kept.data, kept.count),
        [](const UsdMapPair& a, const UsdMapPair& b) { return a.source < b.source; });

    uint32_t hash = hashMap(kept.data, kept.count, offset);
    uint32_t found = table->index.find(hash, [&](uint32_t at) {
        const UsdMap& map = table->maps.data[at];
        return map.count == kept.count && offsetBits(map.offset.offset) == offsetBits(offset.offset) &&
            offsetBits(map.offset.scale) == offsetBits(offset.scale) &&
            prMemCompare(table->pairs.data + map.first, kept.data, kept.count * sizeof(UsdMapPair)) == 0;
    });
    if (found == kHashMissing) {
        found = table->maps.count;
        table->maps.push(UsdMap{table->pairs.count, kept.count, offset});
        table->pairs.append(kept.data, kept.count);
        table->inverses.push(kPrUsdNone);
        table->index.insert(hash, found);
    }
    kept.reset();
    return found;
}

UsdPath usdMapToTarget(UsdMapTable* table, UsdMapId map, UsdPath path) {
    return mapPath(table, map, path, true);
}

UsdPath usdMapToSource(UsdMapTable* table, UsdMapId map, UsdPath path) {
    return mapPath(table, map, path, false);
}

UsdMapId usdMapCompose(UsdMapTable* table, UsdMapId outer, UsdMapId inner) {
    if (outer == kUsdMapNull || inner == kUsdMapNull)
        return kUsdMapNull;
    if (outer == kUsdMapIdentity)
        return inner;
    if (inner == kUsdMapIdentity)
        return outer;
    uint64_t key = uint64_t(outer) << 32 | inner;
    uint32_t memo = table->composed.find(key);
    if (memo != kPrUsdNone)
        return memo;
    UsdMap f = table->maps.data[outer];
    UsdMap g = table->maps.data[inner];
    RawArray<UsdMapPair> pairs{};
    // A pair of the inner mapping whose target the outer maps, and a pair of the outer whose source the inner maps
    // back, are the pairs of the composition.
    for (uint32_t i = 0; i < g.count; ++i) {
        // A pair of the inner mapping whose target the outer does not map becomes a block, so that the paths under it
        // do not map by a pair above it.
        UsdMapPair pair = table->pairs.data[g.first + i];
        pairs.push(UsdMapPair{pair.source, mapPath(table, outer, pair.target, true)});
    }
    for (uint32_t i = 0; i < f.count; ++i) {
        UsdMapPair pair = table->pairs.data[f.first + i];
        UsdPath source = mapPath(table, inner, pair.source, false);
        if (source != kUsdPathNone)
            pairs.push(UsdMapPair{source, pair.target});
    }
    UsdMapId made = usdMapMake(table, pairs.data, pairs.count, usdOffsetCompose(f.offset, g.offset));
    pairs.reset();
    table->composed.add(key, made);
    return made;
}

UsdMapId usdMapInverse(UsdMapTable* table, UsdMapId map) {
    if (map <= kUsdMapIdentity)
        return map;
    if (table->inverses.data[map] != kPrUsdNone)
        return table->inverses.data[map];
    UsdMap f = table->maps.data[map];
    RawArray<UsdMapPair> pairs{};
    for (uint32_t i = 0; i < f.count; ++i) {
        UsdMapPair pair = table->pairs.data[f.first + i];
        if (pair.target != kUsdPathNone)
            pairs.push(UsdMapPair{pair.target, pair.source});
    }
    UsdMapId made = usdMapMake(table, pairs.data, pairs.count, usdOffsetInverse(f.offset));
    pairs.reset();
    table->inverses.data[map] = made;
    return made;
}

UsdMapId usdMapAddRootIdentity(UsdMapTable* table, UsdMapId map) {
    if (map == kUsdMapNull)
        return kUsdMapIdentity;
    UsdMap f = table->maps.data[map];
    RawArray<UsdMapPair> pairs{};
    pairs.append(table->pairs.data + f.first, f.count);
    pairs.push(UsdMapPair{kUsdPathRoot, kUsdPathRoot});
    UsdMapId made = usdMapMake(table, pairs.data, pairs.count, f.offset);
    pairs.reset();
    return made;
}

bool usdMapTextToTarget(const UsdMapTable* table, UsdMapId map, PrStringView path, PrTextBuffer* out) {
    if (map == kUsdMapNull || path.size == 0)
        return false;
    if (map == kUsdMapIdentity) {
        prTextBufferAppend(out, path);
        return true;
    }
    const UsdMap& f = table->maps.data[map];
    PrTextBuffer text = {};
    uint32_t best = kPrUsdNone;
    size_t bestSize = 0;
    for (uint32_t i = 0; i < f.count; ++i) {
        prTextBufferClear(&text);
        usdPathText(table->paths, table->pairs.data[f.first + i].source, &text);
        PrStringView source = prTextBufferView(&text);
        if (textHasPrefix(path, source) && (best == kPrUsdNone || source.size > bestSize)) {
            best = i;
            bestSize = source.size;
        }
    }
    if (best == kPrUsdNone) {
        prTextBufferReset(&text);
        return false;
    }
    if (table->pairs.data[f.first + best].target == kUsdPathNone) {
        prTextBufferReset(&text);
        return false;
    }
    // The rest of the path follows the target, with one separator between them.
    PrStringView rest{path.data + bestSize, path.size - bestSize};
    if (bestSize == 1)
        rest = path.size == 1 ? PrStringView{} : path;
    PrTextBuffer result = {};
    usdPathText(table->paths, table->pairs.data[f.first + best].target, &result);
    size_t targetSize = prTextBufferView(&result).size;
    if (targetSize == 1 && rest.size != 0 && rest.data[0] == '/')
        prTextBufferClear(&result);
    prTextBufferAppend(&result, rest);
    // The result maps back by the same pair, or the mapping is not a bijection at this path.
    bool maps = true;
    for (uint32_t i = 0; maps && i < f.count; ++i) {
        prTextBufferClear(&text);
        usdPathText(table->paths, table->pairs.data[f.first + i].target, &text);
        PrStringView target = prTextBufferView(&text);
        maps = i == best || target.size <= targetSize || target.size == 0 || !textHasPrefix(prTextBufferView(&result), target);
    }
    if (maps)
        prTextBufferAppend(out, prTextBufferView(&result));
    prTextBufferReset(&text);
    prTextBufferReset(&result);
    return maps;
}

uint32_t usdMapChainAdd(UsdMapTable* table, UsdMapId map, uint32_t above) {
    if (map == kUsdMapIdentity)
        return above;
    uint64_t key = uint64_t(map) << 32 | above;
    uint32_t found = table->chainOf.find(key);
    if (found != kPrUsdNone)
        return found;
    found = table->chains.count;
    table->chains.push(UsdMapChain{map, above});
    table->chainOf.add(key, found);
    return found;
}

bool usdMapChainTextToTarget(const UsdMapTable* table, uint32_t chain, PrStringView path, PrTextBuffer* out) {
    if (chain == kUsdMapChainNone) {
        prTextBufferAppend(out, path);
        return path.size != 0;
    }
    PrTextBuffer texts[2] = {};
    PrStringView current = path;
    bool maps = true;
    for (uint32_t which = 0; maps && chain != kUsdMapChainNone; chain = table->chains.data[chain].above, which ^= 1) {
        prTextBufferClear(&texts[which]);
        maps = usdMapTextToTarget(table, table->chains.data[chain].map, current, &texts[which]);
        current = prTextBufferView(&texts[which]);
    }
    if (maps)
        prTextBufferAppend(out, current);
    prTextBufferReset(&texts[0]);
    prTextBufferReset(&texts[1]);
    return maps;
}

} // namespace pr_impl
