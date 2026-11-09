// Ordering (graph_order.h): orders the vertices in each layer, and the ports along their nodes' sides, to reduce
// crossings. Each layer keeps every group's vertices contiguous, and sorts each group among its siblings as one unit,
// by the barycenter of its vertices. Sibling groups keep the same order in every layer. Each connected component's
// vertices stay together, in the order of the components.
//
// An edge that passes through a group that holds neither of its ends runs over the group's contents. Ordering counts
// that as several crossings, and moves vertices across groups to avoid it.

#include "graph/src/graph_order.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/sort.h"

namespace pr_impl {

namespace {

// The maximum number of sweeps, and the number of sweeps without an improvement after which ordering stops.
constexpr uint32_t kMaxSweeps = 24;
constexpr uint32_t kPatience = 4;

// A weighted sum of positions. The barycenter is the sum divided by the weight. A weight of zero means no position.
struct Barycenter {
    float sum;
    float weight;
};

// An item of one context in one layer: a vertex directly in the context, or a child group.
struct Item {
    uint32_t index;                 // the position of the item's first vertex in the layer's current order
    uint32_t id;                    // the vertex or group
    bool isGroup;
    uint32_t start;                 // the first of the item's sorted vertices in the sorter's `moved`
    uint32_t count;
    Barycenter barycenter;
};

struct Sorter {
    GraphLayered* layered;
    const Barycenter* barycenters;  // by vertex
    bool biasRight;
    RawArray<uint32_t> output;
    RawArray<Item> items;
    RawArray<uint32_t> moved;
    RawArray<uint32_t> sortable;
    RawArray<uint32_t> unsortable;
};

void sorterReset(Sorter& sorter) {
    sorter.output.reset();
    sorter.items.reset();
    sorter.moved.reset();
    sorter.sortable.reset();
    sorter.unsortable.reset();
}

// Returns true if the vertex lies within `group`, or anywhere if `group` is kGraphNone.
bool within(const GraphLayered& layered, const GraphVertex& vertex, uint32_t group) {
    if (group == kGraphNone)
        return true;
    for (uint32_t g = vertex.group; g != kGraphNone; g = layered.scope->nodes.data[g].parent) {
        if (g == group)
            return true;
    }
    return false;
}

// Sorts the layer's vertices that lie within `group` by barycenter, and appends them to the sorter's output with each
// child group contiguous. Items without a barycenter keep their positions among their siblings. Returns the combined
// barycenter of the sorted items.
Barycenter sortContext(Sorter& sorter, const uint32_t* layer, uint32_t layerCount, uint32_t group) {
    GraphLayered& layered = *sorter.layered;
    uint32_t itemStart = sorter.items.count;
    for (uint32_t i = 0; i < layerCount; ++i) {
        const GraphVertex& vertex = layered.vertices.data[layer[i]];
        if (!within(layered, vertex, group))
            continue;
        if (vertex.group == group) {
            sorter.items.push(Item{.index = i, .id = layer[i], .isGroup = false});
            continue;
        }
        uint32_t child = graphLayeredChildOf(layered, vertex.group, group);
        bool seen = false;
        for (uint32_t k = itemStart; k < sorter.items.count && !seen; ++k)
            seen = sorter.items.data[k].isGroup && sorter.items.data[k].id == child;
        if (!seen)
            sorter.items.push(Item{.index = i, .id = child, .isGroup = true});
    }
    uint32_t itemCount = sorter.items.count - itemStart;

    uint32_t movedStart = sorter.moved.count;
    for (uint32_t k = 0; k < itemCount; ++k) {
        uint32_t item = itemStart + k;
        if (!sorter.items.data[item].isGroup) {
            uint32_t vertex = sorter.items.data[item].id;
            sorter.items.data[item].barycenter = sorter.barycenters[vertex];
            sorter.items.data[item].start = sorter.moved.count;
            sorter.items.data[item].count = 1;
            sorter.moved.push(vertex);
            continue;
        }
        uint32_t outputStart = sorter.output.count;
        Barycenter barycenter = sortContext(sorter, layer, layerCount, sorter.items.data[item].id);
        sorter.items.data[item].barycenter = barycenter;
        sorter.items.data[item].start = sorter.moved.count;
        sorter.items.data[item].count = sorter.output.count - outputStart;
        for (uint32_t i = outputStart; i < sorter.output.count; ++i)
            sorter.moved.push(sorter.output.data[i]);
        sorter.output.resize(outputStart);
    }

    uint32_t sortableStart = sorter.sortable.count;
    uint32_t unsortableStart = sorter.unsortable.count;
    for (uint32_t k = 0; k < itemCount; ++k) {
        if (sorter.items.data[itemStart + k].barycenter.weight > 0)
            sorter.sortable.push(k);
        else
            sorter.unsortable.push(k);
    }
    const Item* items = sorter.items.data + itemStart;
    bool biasRight = sorter.biasRight;
    Span<uint32_t> sortable(sorter.sortable.data + sortableStart, sorter.sortable.count - sortableStart);
    sortItems(sortable, [&](uint32_t a, uint32_t b) {
        float ba = items[a].barycenter.sum / items[a].barycenter.weight;
        float bb = items[b].barycenter.sum / items[b].barycenter.weight;
        if (ba != bb)
            return ba < bb;
        return biasRight ? a > b : a < b;
    });

    Barycenter total{0, 0};
    uint32_t placed = 0;
    uint32_t next = unsortableStart;
    auto emit = [&](uint32_t k) {
        for (uint32_t i = 0; i < items[k].count; ++i)
            sorter.output.push(sorter.moved.data[items[k].start + i]);
        ++placed;
    };
    auto consume = [&]() {
        while (next < sorter.unsortable.count && sorter.unsortable.data[next] <= placed)
            emit(sorter.unsortable.data[next++]);
    };
    consume();
    for (uint32_t s = 0; s < sortable.count(); ++s) {
        uint32_t k = sortable[s];
        emit(k);
        total.sum += items[k].barycenter.sum;
        total.weight += items[k].barycenter.weight;
        consume();
    }
    while (next < sorter.unsortable.count)
        emit(sorter.unsortable.data[next++]);

    sorter.sortable.resize(sortableStart);
    sorter.unsortable.resize(unsortableStart);
    sorter.moved.resize(movedStart);
    sorter.items.resize(itemStart);
    return total;
}

void sortLayer(GraphLayered& layered, Sorter& sorter, uint32_t layer) {
    uint32_t start = layered.layerStart.data[layer];
    uint32_t count = layered.layerStart.data[layer + 1] - start;
    uint32_t* vertices = layered.layerVertices.data + start;
    sorter.output.clear();
    sortContext(sorter, vertices, count, kGraphNone);
    PR_ASSERT(sorter.output.count == count);
    for (uint32_t i = 0; i < count; ++i) {
        vertices[i] = sorter.output.data[i];
        layered.vertices.data[vertices[i]].order = i;
    }
}

void renumber(GraphLayered& layered) {
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        for (uint32_t i = layered.layerStart.data[layer]; i < layered.layerStart.data[layer + 1]; ++i)
            layered.vertices.data[layered.layerVertices.data[i]].order = i - layered.layerStart.data[layer];
    }
}

// The sort key of a segment's end: its vertex's order plus its port's key along the side.
float endKey(const GraphLayered& layered, uint32_t vertex, uint32_t port) {
    return float(layered.vertices.data[vertex].order) + graphLayeredPortKey(layered, port);
}

// Computes the barycenters of the vertices of `layer` against the adjacent layer above (`fromAbove`) or below.
void computeBarycenters(GraphLayered& layered, uint32_t layer, bool fromAbove, Barycenter* out) {
    for (uint32_t i = layered.layerStart.data[layer]; i < layered.layerStart.data[layer + 1]; ++i)
        out[layered.layerVertices.data[i]] = Barycenter{0, 0};
    if (fromAbove && layer == 0)
        return;
    uint32_t gap = fromAbove ? layer - 1 : layer;
    for (uint32_t i = layered.gapSegmentStart.data[gap]; i < layered.gapSegmentStart.data[gap + 1]; ++i) {
        const GraphSegment& segment = layered.segments.data[layered.gapSegments.data[i]];
        uint32_t own = fromAbove ? segment.lower : segment.upper;
        float key = endKey(layered, segment.lower, segment.lowerPort);
        if (fromAbove)
            key = endKey(layered, segment.upper, segment.upperPort);
        out[own].sum += key * segment.weight;
        out[own].weight += segment.weight;
    }
}

// Returns true if the segment of an edge passes through `group` from one side to the other, rather than entering or
// leaving it. A group passes through only the gaps between the layers it spans.
bool passesThrough(const GraphLayered& layered, const GraphSegment& segment, uint32_t group) {
    const GraphVertex& upper = layered.vertices.data[segment.upper];
    uint32_t top = layered.rankTop.data[group];
    if (upper.layer < top || upper.layer + 1 > layered.rankBottom.data[group] ||
            within(layered, upper, group) || within(layered, layered.vertices.data[segment.lower], group))
        return false;
    uint32_t placeholder = layered.nodeVertex.data[group] + (upper.layer - top);
    bool upperBefore = endKey(layered, segment.upper, segment.upperPort) < endKey(layered, placeholder, kGraphNone);
    bool lowerBefore = endKey(layered, segment.lower, segment.lowerPort) < endKey(layered, placeholder + 1, kGraphNone);
    return upperBefore != lowerBefore;
}

// Returns true if a segment whose ends have the keys `upper` and `lower` crosses one whose ends have `otherUpper` and
// `otherLower`: their ends are in opposite orders in the two layers.
bool keysCross(float upper, float lower, float otherUpper, float otherLower) {
    return (upper < otherUpper && lower > otherLower) || (upper > otherUpper && lower < otherLower);
}

// The sort key of a group's placeholder in `layer`, which the group spans.
float placeholderKey(const GraphLayered& layered, uint32_t group, uint32_t layer) {
    return endKey(layered, layered.nodeVertex.data[group] + (layer - layered.rankTop.data[group]), kGraphNone);
}

// Returns true if the group spans the gap after `layer`, and so has a chain segment across it.
bool spansGap(const GraphLayered& layered, uint32_t group, uint32_t layer) {
    return layered.rankTop.data[group] <= layer && layer < layered.rankBottom.data[group];
}

// The innermost group that is or holds both `a` and `b`, each a group or kGraphNone for the top level. Returns
// kGraphNone if only the top level holds both.
uint32_t commonGroup(const GraphLayered& layered, uint32_t a, uint32_t b) {
    while (a != b) {
        if (a == kGraphNone || b == kGraphNone)
            return kGraphNone;
        if (layered.depth.data[a] >= layered.depth.data[b])
            a = layered.scope->nodes.data[a].parent;
        else
            b = layered.scope->nodes.data[b].parent;
    }
    return a;
}

// Adds an item at `rank` to an accumulator tree (Barth, Jünger and Mutzel, "Simple and Efficient Bilayer Cross
// Counting", 2002). The tree is a complete binary tree in an array. Its leaves, from `firstIndex`, count the items at
// each rank, and its inner nodes sum their children.
void treeInsert(int64_t* tree, uint32_t firstIndex, uint32_t rank) {
    uint32_t index = firstIndex + rank;
    ++tree[index];
    while (index > 0) {
        index = (index - 1) / 2;
        ++tree[index];
    }
}

// The number of items in the tree at ranks above `rank`: the sum of the right siblings along the path from its leaf.
int64_t treeAbove(const int64_t* tree, uint32_t firstIndex, uint32_t rank) {
    int64_t sum = 0;
    for (uint32_t index = firstIndex + rank; index > 0; index = (index - 1) / 2) {
        if (index % 2 == 1)
            sum += tree[index + 1];
    }
    return sum;
}

// The weighted number of crossings in every gap.
uint64_t countCrossings(GraphLayered& layered) {
    uint64_t crossings = 0;
    for (uint32_t layer = 0; layer + 1 < layered.layerCount; ++layer)
        crossings += graphLayeredGapCrossings(layered, layer);
    return crossings;
}

bool isAlongMain(GraphSide side) {
    return side == GraphSide_CrossStart || side == GraphSide_CrossEnd;
}

// Assigns each port its offset and key along its side. Of the ports that the layout positions on the same side of a
// node, `slots` holds each port's index and `slotCounts` holds the number.
void assignPortKeys(GraphLayered& layered, const RawArray<uint32_t>& slots, const RawArray<uint32_t>& slotCounts) {
    const GraphScope& scope = *layered.scope;
    for (uint32_t p = 0; p < scope.ports.count; ++p) {
        const GraphScopePort& port = scope.ports.data[p];
        GraphLayeredPort& placed = layered.ports.data[p];
        float length = isAlongMain(placed.side) ? layered.nodeMain.data[port.node] : layered.nodeCross.data[port.node];
        if (port.constraint != PrGraphPortConstraint_FixedPosition)
            placed.offset = float(slots.data[p] + 1) / float(slotCounts.data[p] + 1) * length;
        if (isAlongMain(placed.side)) {
            placed.key = placed.side == GraphSide_CrossStart ? 0.0f : 0.999f;
        } else {
            float key = length > 0 ? placed.offset / length : 0.5f;
            placed.key = key < 0 ? 0 : key > 0.999f ? 0.999f : key;
        }
    }
}

// Sorts the ports that the layout orders on each side of a node by the positions of their edges' far ends. The ports
// are rearranged only among the slots that they already hold.
void reorderPorts(GraphLayered& layered, RawArray<uint32_t>& slots, RawArray<Barycenter>& far, RawArray<uint32_t>& byPlace) {
    const GraphScope& scope = *layered.scope;
    uint32_t portCount = scope.ports.count;
    far.resize(portCount);
    for (uint32_t p = 0; p < portCount; ++p)
        far.data[p] = Barycenter{0, 0};
    for (uint32_t s = 0; s < layered.segments.count; ++s) {
        const GraphSegment& segment = layered.segments.data[s];
        if (segment.upperPort != kGraphNone) {
            far.data[segment.upperPort].sum += endKey(layered, segment.lower, segment.lowerPort) * segment.weight;
            far.data[segment.upperPort].weight += segment.weight;
        }
        if (segment.lowerPort != kGraphNone) {
            far.data[segment.lowerPort].sum += endKey(layered, segment.upper, segment.upperPort) * segment.weight;
            far.data[segment.lowerPort].weight += segment.weight;
        }
    }
    auto isReorderable = [&](uint32_t p) {
        PrGraphPortConstraint constraint = scope.ports.data[p].constraint;
        return (constraint == PrGraphPortConstraint_Free || constraint == PrGraphPortConstraint_FixedSide) &&
            !isAlongMain(layered.ports.data[p].side) && far.data[p].weight > 0;
    };
    // Sorts the reorderable ports by node, side and slot, so that each side's ports form a run that is sorted within
    // its own slots.
    byPlace.clear();
    for (uint32_t p = 0; p < portCount; ++p) {
        if (isReorderable(p))
            byPlace.push(p);
    }
    sortItems(byPlace.span(), [&](uint32_t a, uint32_t b) {
        const GraphScopePort& pa = scope.ports.data[a];
        const GraphScopePort& pb = scope.ports.data[b];
        if (pa.node != pb.node)
            return pa.node < pb.node;
        if (layered.ports.data[a].side != layered.ports.data[b].side)
            return layered.ports.data[a].side < layered.ports.data[b].side;
        return slots.data[a] < slots.data[b];
    });
    RawArray<uint32_t> runSlots{};
    for (uint32_t i = 0; i < byPlace.count;) {
        uint32_t j = i + 1;
        while (j < byPlace.count && scope.ports.data[byPlace.data[j]].node == scope.ports.data[byPlace.data[i]].node &&
                layered.ports.data[byPlace.data[j]].side == layered.ports.data[byPlace.data[i]].side)
            ++j;
        runSlots.clear();
        for (uint32_t k = i; k < j; ++k)
            runSlots.push(slots.data[byPlace.data[k]]);
        Span<uint32_t> run(byPlace.data + i, j - i);
        sortItems(run, [&](uint32_t a, uint32_t b) {
            float fa = far.data[a].sum / far.data[a].weight;
            float fb = far.data[b].sum / far.data[b].weight;
            return fa < fb || (fa == fb && slots.data[a] < slots.data[b]);
        });
        for (uint32_t k = i; k < j; ++k)
            slots.data[byPlace.data[k]] = runSlots.data[k - i];
        i = j;
    }
    runSlots.reset();
}

void sortKeys(RawArray<float>& keys) {
    sortItems(keys.span(), [](float a, float b) { return a < b; });
}

// The most pairs of segments that a move's change in crossings compares one by one, and the most segments in a gap
// whose crossings are counted pair by pair. A comparison of keys costs a few instructions, and sorting costs tens for
// each key, so small counts cost less without sorting.
constexpr uint64_t kMaxPairwise = 64;
constexpr uint32_t kMaxPairwiseGap = 64;

// The sum, over every pair of an `x` and a `y`, of 1 if x < y and -1 if x > y. Few pairs are compared one by one.
// Otherwise both arrays are sorted and merged, in O(n log n) for their n keys.
int64_t orderBalance(RawArray<float>& x, RawArray<float>& y) {
    int64_t total = 0;
    if (uint64_t(x.count) * y.count <= kMaxPairwise) {
        for (float a : x.span()) {
            for (float b : y.span())
                total += a < b ? 1 : a > b ? -1 : 0;
        }
        return total;
    }
    sortKeys(x);
    sortKeys(y);
    uint32_t below = 0;             // the number of y less than the current x
    uint32_t through = 0;           // the number of y at most the current x
    for (uint32_t i = 0; i < x.count; ++i) {
        while (below < y.count && y.data[below] < x.data[i])
            ++below;
        through = through > below ? through : below;
        while (through < y.count && y.data[through] <= x.data[i])
            ++through;
        total += int64_t(y.count - through) - int64_t(below);
    }
    return total;
}

// Moves the vertex at position `from` in its layer to `to`, shifting the vertices between.
void moveVertex(GraphLayered& layered, uint32_t layer, uint32_t from, uint32_t to) {
    uint32_t* vertices = layered.layerVertices.data + layered.layerStart.data[layer];
    uint32_t moved = vertices[from];
    if (from < to) {
        for (uint32_t i = from; i < to; ++i)
            vertices[i] = vertices[i + 1];
    } else {
        for (uint32_t i = from; i > to; --i)
            vertices[i] = vertices[i - 1];
    }
    vertices[to] = moved;
    uint32_t low = from < to ? from : to;
    uint32_t high = from < to ? to : from;
    for (uint32_t i = low; i <= high; ++i)
        layered.vertices.data[vertices[i]].order = i;
}

// Moves the vertex at position `i` of `layer` across the adjacent run of a sibling group if that reduces the crossings,
// counting those through the group. Returns true if it moved. `step` is +1 to try the run after the vertex or -1 for
// the run before it.
bool transposeAcrossGroup(GraphLayered& layered, uint32_t layer, uint32_t i, int step) {
    const uint32_t* row = layered.layerVertices.data + layered.layerStart.data[layer];
    int64_t count = layered.layerStart.data[layer + 1] - layered.layerStart.data[layer];
    const GraphVertex& a = layered.vertices.data[row[i]];
    int64_t near = int64_t(i) + step;
    if (near < 0 || near >= count)
        return false;
    const GraphVertex& b = layered.vertices.data[row[near]];
    if (b.group == a.group || !within(layered, b, a.group))
        return false;
    uint32_t group = graphLayeredChildOf(layered, b.group, a.group);
    int64_t far = near;
    while (far + step >= 0 && far + step < count && within(layered, layered.vertices.data[row[far + step]], group))
        far += step;
    if (graphLayeredMoveDelta(layered, layer, i, uint32_t(far)) >= 0)
        return false;
    moveVertex(layered, layer, i, uint32_t(far));
    return true;
}

// Swaps adjacent vertices of the same group while a swap reduces the crossings on either side of their layer (Gansner
// et al., 1993, section 3.2). A swap keeps every group contiguous, since both vertices lie directly in it. A vertex
// next to a sibling group's run may also move across the whole run.
void transpose(GraphLayered& layered) {
    constexpr uint32_t kMaxPasses = 8;
    bool improved = true;
    for (uint32_t pass = 0; pass < kMaxPasses && improved; ++pass) {
        improved = false;
        for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
            const uint32_t* row = layered.layerVertices.data + layered.layerStart.data[layer];
            uint32_t count = layered.layerStart.data[layer + 1] - layered.layerStart.data[layer];
            for (uint32_t i = 0; i < count; ++i) {
                if (layered.vertices.data[row[i]].kind == GraphVertexKind_Placeholder)
                    continue;
                if (transposeAcrossGroup(layered, layer, i, 1) || transposeAcrossGroup(layered, layer, i, -1))
                    improved = true;
            }
            for (uint32_t i = 0; i + 1 < count; ++i) {
                const GraphVertex& a = layered.vertices.data[row[i]];
                const GraphVertex& b = layered.vertices.data[row[i + 1]];
                if (a.group != b.group || a.kind == GraphVertexKind_Placeholder ||
                        b.kind == GraphVertexKind_Placeholder)
                    continue;
                if (graphLayeredMoveDelta(layered, layer, i, i + 1) >= 0)
                    continue;
                moveVertex(layered, layer, i, i + 1);
                improved = true;
            }
        }
    }
}

// Appends the vertices of `vertices` that lie within `group` to `out`. The child groups' runs are reordered by their
// keys, within the places that the child groups held. Each child group's vertices are contiguous in `vertices`.
void alignContext(const GraphLayered& layered, const uint32_t* vertices, uint32_t count, uint32_t group,
        const float* keys, RawArray<uint32_t>& out) {
    // An item of the context: a vertex directly in it, or a child group with the run of its vertices.
    struct Run {
        uint32_t child;             // kGraphNone for a vertex
        uint32_t start;
        uint32_t count;
    };
    RawArray<Run> runs{};
    for (uint32_t i = 0; i < count;) {
        const GraphVertex& vertex = layered.vertices.data[vertices[i]];
        if (vertex.group == group) {
            runs.push(Run{kGraphNone, i, 1});
            ++i;
            continue;
        }
        uint32_t child = graphLayeredChildOf(layered, vertex.group, group);
        uint32_t j = i + 1;
        while (j < count && within(layered, layered.vertices.data[vertices[j]], child))
            ++j;
        runs.push(Run{child, i, j - i});
        i = j;
    }
    RawArray<uint32_t> groups{};
    for (uint32_t r = 0; r < runs.count; ++r) {
        if (runs.data[r].child != kGraphNone)
            groups.push(r);
    }
    RawArray<uint32_t> sorted{};
    sorted.append(groups.data, groups.count);
    sortItems(sorted.span(), [&](uint32_t a, uint32_t b) {
        uint32_t ga = runs.data[a].child;
        uint32_t gb = runs.data[b].child;
        return keys[ga] < keys[gb] || (keys[ga] == keys[gb] && ga < gb);
    });
    uint32_t nextGroup = 0;
    for (uint32_t r = 0; r < runs.count; ++r) {
        Run run = runs.data[r];
        if (run.child == kGraphNone) {
            out.push(vertices[run.start]);
            continue;
        }
        Run placed = runs.data[sorted.data[nextGroup++]];
        alignContext(layered, vertices + placed.start, placed.count, placed.child, keys, out);
    }
    runs.reset();
    groups.reset();
    sorted.reset();
}

// A vertex's move from one position in its layer to another.
struct Move {
    uint32_t layer;
    uint32_t from;
    uint32_t to;
};

// A trial reordering: the moves that it made, in order, so that it can be undone, and the change in the weighted
// crossings that they made.
struct Trial {
    RawArray<Move> moves;
    int64_t delta;
};

// Moves a vertex as part of the trial, and adds the move's change in the weighted crossings to the trial's.
void trialMove(GraphLayered& layered, Trial& trial, uint32_t layer, uint32_t from, uint32_t to) {
    trial.delta += graphLayeredMoveDelta(layered, layer, from, to);
    moveVertex(layered, layer, from, to);
    trial.moves.push(Move{.layer = layer, .from = from, .to = to});
}

// Keeps the trial's moves, and empties it.
void trialKeep(Trial& trial) {
    trial.moves.clear();
    trial.delta = 0;
}

// Undoes the trial's moves, and empties it.
void trialUndo(GraphLayered& layered, Trial& trial) {
    for (uint32_t k = trial.moves.count; k-- > 0;) {
        const Move& move = trial.moves.data[k];
        moveVertex(layered, move.layer, move.to, move.from);
    }
    trialKeep(trial);
}

// Moves a vertex to the other side of the run of the item of its context that holds `group`. Returns false if it
// cannot: the vertex is a placeholder or lies in the group, or its context does not hold the group in its layer. The
// item's vertices are contiguous in the layer, around its placeholder.
bool jumpAcross(GraphLayered& layered, Trial& trial, uint32_t vertex, uint32_t group) {
    const GraphVertex& v = layered.vertices.data[vertex];
    if (v.kind == GraphVertexKind_Placeholder || within(layered, v, group))
        return false;
    bool held = v.group == kGraphNone;
    for (uint32_t g = layered.scope->nodes.data[group].parent; !held && g != kGraphNone;
            g = layered.scope->nodes.data[g].parent)
        held = g == v.group;
    if (!held)
        return false;
    uint32_t item = graphLayeredChildOf(layered, group, v.group);
    uint32_t layer = v.layer;
    if (layer < layered.rankTop.data[item] || layer > layered.rankBottom.data[item])
        return false;
    const uint32_t* row = layered.layerVertices.data + layered.layerStart.data[layer];
    uint32_t count = layered.layerStart.data[layer + 1] - layered.layerStart.data[layer];
    uint32_t placeholder = layered.nodeVertex.data[item] + (layer - layered.rankTop.data[item]);
    uint32_t runFirst = layered.vertices.data[placeholder].order;
    uint32_t runLast = runFirst;
    while (runFirst > 0 && within(layered, layered.vertices.data[row[runFirst - 1]], item))
        --runFirst;
    while (runLast + 1 < count && within(layered, layered.vertices.data[row[runLast + 1]], item))
        ++runLast;
    uint32_t at = v.order;
    trialMove(layered, trial, layer, at, at < runFirst ? runLast : runFirst);
    return true;
}

// Moves a vertex across `group` as jumpAcross does, then moves each neighbor in an adjacent layer whose segment with
// the vertex now passes through the group: first those in the layer before, then those in the layer after, each in the
// order of the segments. Returns false if the vertex cannot move.
bool jumpAcrossWithNeighbors(GraphLayered& layered, Trial& trial, uint32_t vertex, uint32_t group) {
    if (!jumpAcross(layered, trial, vertex, group))
        return false;
    for (int upward = 1; upward >= 0; --upward) {
        for (uint32_t i = layered.incidentStart.data[vertex]; i < layered.incidentStart.data[vertex + 1]; ++i) {
            const GraphSegment& segment = layered.segments.data[layered.incident.data[i]];
            if ((segment.lower == vertex) != (upward != 0) || !passesThrough(layered, segment, group))
                continue;
            jumpAcross(layered, trial, segment.upper == vertex ? segment.lower : segment.upper, group);
        }
    }
    return true;
}

// Tries to take edges out of the groups that they pass through. For each segment that passes through a group, moves its
// upper end, its lower end, or both across the group, and keeps the first move that reduces the weighted crossings.
// Moving one end at a time can leave an edge in a group when its ends' other edges hold them in place. Each move's
// change in the crossings is counted from the segments of the vertices that it reorders, so a trial costs what its
// moves pass over rather than what its layers hold.
void untangleGroups(GraphLayered& layered) {
    constexpr uint32_t kMaxPasses = 4;
    if (countCrossings(layered) < kGraphGroupCrossingWeight)
        return;
    // The chains' segments by the layer of their upper ends.
    RawArray<uint32_t> chainStart{};
    RawArray<uint32_t> chains{};
    chainStart.assignZeroed(layered.layerCount + 1);
    for (uint32_t c = 0; c < layered.segments.count; ++c) {
        if (layered.segments.data[c].chain)
            ++chainStart.data[layered.vertices.data[layered.segments.data[c].upper].layer + 1];
    }
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer)
        chainStart.data[layer + 1] += chainStart.data[layer];
    chains.resize(chainStart.data[layered.layerCount]);
    RawArray<uint32_t> fill{};
    fill.append(chainStart.data, layered.layerCount);
    for (uint32_t c = 0; c < layered.segments.count; ++c) {
        if (layered.segments.data[c].chain)
            chains.data[fill.data[layered.vertices.data[layered.segments.data[c].upper].layer]++] = c;
    }
    fill.reset();
    Trial trial{};
    bool improved = true;
    for (uint32_t pass = 0; pass < kMaxPasses && improved; ++pass) {
        improved = false;
        for (uint32_t s = 0; s < layered.segments.count; ++s) {
            if (layered.segments.data[s].chain)
                continue;
            uint32_t layer = layered.vertices.data[layered.segments.data[s].upper].layer;
            for (uint32_t k = chainStart.data[layer]; k < chainStart.data[layer + 1]; ++k) {
                const GraphSegment& segment = layered.segments.data[s];
                uint32_t group = layered.vertices.data[layered.segments.data[chains.data[k]].upper].node;
                if (!passesThrough(layered, segment, group))
                    continue;
                // Moves the upper end, the lower end, both, or one end with the neighbors that it would pull through.
                for (uint32_t option = 0; option < 5; ++option) {
                    bool moved = true;
                    if (option == 0 || option == 2)
                        moved = jumpAcross(layered, trial, segment.upper, group) && moved;
                    if (option == 1 || option == 2)
                        moved = jumpAcross(layered, trial, segment.lower, group) && moved;
                    if (option == 3)
                        moved = jumpAcrossWithNeighbors(layered, trial, segment.upper, group);
                    if (option == 4)
                        moved = jumpAcrossWithNeighbors(layered, trial, segment.lower, group);
                    if (moved && trial.delta < 0) {
                        trialKeep(trial);
                        improved = true;
                        break;
                    }
                    trialUndo(layered, trial);
                }
            }
        }
    }
    trial.moves.reset();
    chainStart.reset();
    chains.reset();
}

// Puts sibling groups in the same order in every layer they share, by the mean position of their vertices. A group is
// one rectangle across its layers, so its order cannot change from layer to layer. The sweeps keep each group
// contiguous within a layer, but they may order two groups differently in adjacent layers.
void alignGroups(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    uint32_t n = scope.nodes.count;
    RawArray<float> keys{};
    RawArray<float> counts{};
    keys.assignZeroed(n);
    counts.assignZeroed(n);
    for (uint32_t v = 0; v < layered.vertices.count; ++v) {
        const GraphVertex& vertex = layered.vertices.data[v];
        uint32_t layerSize = layered.layerStart.data[vertex.layer + 1] - layered.layerStart.data[vertex.layer];
        float position = layerSize > 1 ? float(vertex.order) / float(layerSize - 1) : 0.5f;
        for (uint32_t g = vertex.group; g != kGraphNone; g = scope.nodes.data[g].parent) {
            keys.data[g] += position;
            counts.data[g] += 1;
        }
    }
    for (uint32_t g = 0; g < n; ++g) {
        if (counts.data[g] > 0)
            keys.data[g] /= counts.data[g];
    }
    RawArray<uint32_t> out{};
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        uint32_t start = layered.layerStart.data[layer];
        uint32_t count = layered.layerStart.data[layer + 1] - start;
        out.clear();
        alignContext(layered, layered.layerVertices.data + start, count, kGraphNone, keys.data, out);
        PR_ASSERT(out.count == count);
        prMemCopy(layered.layerVertices.data + start, out.data, sizeof(uint32_t) * count);
    }
    renumber(layered);
    keys.reset();
    counts.reset();
    out.reset();
}

// Keeps the vertices of each connected component together in each layer, in the order of the components, so that
// components lie side by side across their shared layers. No edge joins two components, so this adds no crossing.
void separateComponents(GraphLayered& layered) {
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        uint32_t start = layered.layerStart.data[layer];
        Span<uint32_t> vertices(layered.layerVertices.data + start, layered.layerStart.data[layer + 1] - start);
        sortItems(vertices, [&](uint32_t a, uint32_t b) {
            float ka = layered.componentKey.data[graphLayeredVertexComponent(layered, a)];
            float kb = layered.componentKey.data[graphLayeredVertexComponent(layered, b)];
            if (ka != kb)
                return ka < kb;
            return layered.vertices.data[a].order < layered.vertices.data[b].order;
        });
    }
    renumber(layered);
}

} // namespace

void graphLayeredOrder(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    PrGraphDirection direction = scope.direction;
    uint32_t vertexCount = layered.vertices.count;
    uint32_t layerCount = layered.layerCount;

    // Fills each layer in the order that a depth-first walk down the segments visits its vertices. An interactive
    // layout uses the arrangement's order on the cross axis instead.
    layered.layerStart.assignZeroed(layerCount + 1);
    for (uint32_t v = 0; v < vertexCount; ++v)
        ++layered.layerStart.data[layered.vertices.data[v].layer + 1];
    for (uint32_t layer = 0; layer < layerCount; ++layer)
        layered.layerStart.data[layer + 1] += layered.layerStart.data[layer];
    layered.layerVertices.resize(vertexCount);
    RawArray<uint32_t>& cursor = layered.scratch[0];
    cursor.resize(layerCount);
    for (uint32_t layer = 0; layer < layerCount; ++layer)
        cursor.data[layer] = layered.layerStart.data[layer];

    if (layered.options.mode == PrGraphLayoutMode_Interactive) {
        RawArray<float>& key = layered.scratchFloat[0];
        key.resize(vertexCount);
        auto crossCenter = [&](uint32_t node) {
            const GraphScopeNode& n = scope.nodes.data[node];
            bool vertical = direction == PrGraphDirection_Down || direction == PrGraphDirection_Up;
            return vertical ? n.position.x + n.extent.x * 0.5f : n.position.y + n.extent.y * 0.5f;
        };
        for (uint32_t v = 0; v < vertexCount; ++v) {
            const GraphVertex& vertex = layered.vertices.data[v];
            if (vertex.kind == GraphVertexKind_Leaf || vertex.kind == GraphVertexKind_Placeholder)
                key.data[v] = crossCenter(vertex.node);
        }
        for (uint32_t e = 0; e < layered.edges.count; ++e) {
            const GraphLayeredEdge& edge = layered.edges.data[e];
            if (edge.selfLoop)
                continue;
            float tailKey = key.data[edge.tail.vertex];
            float headKey = key.data[edge.head.vertex];
            float tailLayer = float(layered.vertices.data[edge.tail.vertex].layer);
            float span = float(layered.vertices.data[edge.head.vertex].layer) - tailLayer;
            for (uint32_t s = 0; s + 1 < edge.segmentCount; ++s) {
                uint32_t v = layered.segments.data[edge.segmentStart + s].lower;
                float t = (float(layered.vertices.data[v].layer) - tailLayer) / span;
                key.data[v] = tailKey + (headKey - tailKey) * t;
            }
        }
        for (uint32_t v = 0; v < vertexCount; ++v)
            layered.layerVertices.data[cursor.data[layered.vertices.data[v].layer]++] = v;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            Span<uint32_t> vertices(layered.layerVertices.data + layered.layerStart.data[layer],
                layered.layerStart.data[layer + 1] - layered.layerStart.data[layer]);
            sortItems(vertices, [&](uint32_t a, uint32_t b) {
                return key.data[a] < key.data[b] || (key.data[a] == key.data[b] && a < b);
            });
        }
    } else {
        // The segments down from each vertex.
        RawArray<uint32_t>& downStart = layered.scratch[1];
        RawArray<uint32_t>& down = layered.scratch[2];
        downStart.assignZeroed(vertexCount + 1);
        for (uint32_t s = 0; s < layered.segments.count; ++s)
            ++downStart.data[layered.segments.data[s].upper + 1];
        for (uint32_t v = 0; v < vertexCount; ++v)
            downStart.data[v + 1] += downStart.data[v];
        down.resize(layered.segments.count);
        RawArray<uint32_t>& fill = layered.scratch[3];
        fill.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v)
            fill.data[v] = downStart.data[v];
        for (uint32_t s = 0; s < layered.segments.count; ++s)
            down.data[fill.data[layered.segments.data[s].upper]++] = layered.segments.data[s].lower;
        // The walk's roots, sorted by layer and then by creation order.
        RawArray<uint32_t>& roots = layered.scratch[4];
        roots.resize(vertexCount);
        for (uint32_t v = 0; v < vertexCount; ++v)
            roots.data[v] = v;
        sortItems(roots.span(), [&](uint32_t a, uint32_t b) {
            uint32_t la = layered.vertices.data[a].layer;
            uint32_t lb = layered.vertices.data[b].layer;
            return la < lb || (la == lb && a < b);
        });
        RawArray<uint32_t>& visited = layered.scratch[5];
        visited.assignZeroed(vertexCount);
        RawArray<uint32_t> stack{};
        for (uint32_t r = 0; r < vertexCount; ++r) {
            stack.clear();
            stack.push(roots.data[r]);
            while (stack.count > 0) {
                uint32_t v = stack.last();
                stack.pop();
                if (visited.data[v] != 0)
                    continue;
                visited.data[v] = 1;
                layered.layerVertices.data[cursor.data[layered.vertices.data[v].layer]++] = v;
                for (uint32_t i = downStart.data[v + 1]; i-- > downStart.data[v];) {
                    if (visited.data[down.data[i]] == 0)
                        stack.push(down.data[i]);
                }
            }
        }
        stack.reset();
    }
    renumber(layered);

    // Assigns the ports' slots along their sides, in the order of description.
    uint32_t portCount = scope.ports.count;
    RawArray<uint32_t> slots{};
    RawArray<uint32_t> slotCounts{};
    slots.resize(portCount);
    slotCounts.assignZeroed(portCount);
    // Each run of the ports that the layout positions on one side of one node, in the order of description.
    RawArray<uint32_t> byPlace{};
    for (uint32_t p = 0; p < portCount; ++p) {
        slots.data[p] = 0;
        if (scope.ports.data[p].constraint != PrGraphPortConstraint_FixedPosition)
            byPlace.push(p);
    }
    sortItems(byPlace.span(), [&](uint32_t a, uint32_t b) {
        const GraphScopePort& pa = scope.ports.data[a];
        const GraphScopePort& pb = scope.ports.data[b];
        if (pa.node != pb.node)
            return pa.node < pb.node;
        if (layered.ports.data[a].side != layered.ports.data[b].side)
            return layered.ports.data[a].side < layered.ports.data[b].side;
        return a < b;
    });
    for (uint32_t i = 0; i < byPlace.count;) {
        uint32_t j = i + 1;
        while (j < byPlace.count && scope.ports.data[byPlace.data[j]].node == scope.ports.data[byPlace.data[i]].node &&
                layered.ports.data[byPlace.data[j]].side == layered.ports.data[byPlace.data[i]].side)
            ++j;
        for (uint32_t k = i; k < j; ++k) {
            slots.data[byPlace.data[k]] = k - i;
            slotCounts.data[byPlace.data[k]] = j - i;
        }
        i = j;
    }
    assignPortKeys(layered, slots, slotCounts);

    RawArray<Barycenter> centers{};
    centers.resize(vertexCount);
    RawArray<Barycenter> far{};
    Sorter sorter{.layered = &layered, .barycenters = centers.data, .biasRight = false};

    // Makes each group contiguous in each layer, keeping the order otherwise.
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        for (uint32_t i = layered.layerStart.data[layer]; i < layered.layerStart.data[layer + 1]; ++i) {
            uint32_t v = layered.layerVertices.data[i];
            centers.data[v] = Barycenter{float(layered.vertices.data[v].order), 1};
        }
        sortLayer(layered, sorter, layer);
    }

    RawArray<uint32_t>& incidentStart = layered.incidentStart;
    RawArray<uint32_t>& incident = layered.incident;
    incidentStart.assignZeroed(vertexCount + 1);
    for (uint32_t s = 0; s < layered.segments.count; ++s) {
        const GraphSegment& segment = layered.segments.data[s];
        if (segment.chain)
            continue;
        ++incidentStart.data[segment.upper + 1];
        ++incidentStart.data[segment.lower + 1];
    }
    for (uint32_t v = 0; v < vertexCount; ++v)
        incidentStart.data[v + 1] += incidentStart.data[v];
    incident.resize(incidentStart.data[vertexCount]);
    RawArray<uint32_t> fillAt{};
    fillAt.append(incidentStart.data, vertexCount);
    for (uint32_t s = 0; s < layered.segments.count; ++s) {
        const GraphSegment& segment = layered.segments.data[s];
        if (segment.chain)
            continue;
        incident.data[fillAt.data[segment.upper]++] = s;
        incident.data[fillAt.data[segment.lower]++] = s;
    }
    fillAt.reset();

    if (layered.options.mode != PrGraphLayoutMode_Interactive) {
        transpose(layered);
        uint64_t best = countCrossings(layered);
        RawArray<uint32_t>& bestSlots = layered.scratch[1];
        layered.bestOrder.clear();
        layered.bestOrder.append(layered.layerVertices.data, layered.layerVertices.count);
        bestSlots.clear();
        bestSlots.append(slots.data, slots.count);
        uint32_t lastImproved = 0;
        for (uint32_t sweep = 0; sweep < kMaxSweeps && best > 0; ++sweep) {
            sorter.biasRight = sweep % 4 >= 2;
            if (sweep % 2 == 0) {
                for (uint32_t layer = 1; layer < layerCount; ++layer) {
                    computeBarycenters(layered, layer, true, centers.data);
                    sortLayer(layered, sorter, layer);
                }
            } else {
                for (uint32_t layer = layerCount - 1; layer-- > 0;) {
                    computeBarycenters(layered, layer, false, centers.data);
                    sortLayer(layered, sorter, layer);
                }
            }
            transpose(layered);
            reorderPorts(layered, slots, far, byPlace);
            assignPortKeys(layered, slots, slotCounts);
            uint64_t crossings = countCrossings(layered);
            if (crossings < best) {
                best = crossings;
                lastImproved = sweep;
                layered.bestOrder.clear();
                layered.bestOrder.append(layered.layerVertices.data, layered.layerVertices.count);
                bestSlots.clear();
                bestSlots.append(slots.data, slots.count);
            } else if (sweep - lastImproved >= kPatience) {
                break;
            }
        }
        // Untangling counts crossings by the ports' keys, so the best order's slots give them their keys first.
        prMemCopy(layered.layerVertices.data, layered.bestOrder.data, sizeof(uint32_t) * layered.bestOrder.count);
        if (portCount > 0)
            prMemCopy(slots.data, bestSlots.data, sizeof(uint32_t) * portCount);
        assignPortKeys(layered, slots, slotCounts);
        renumber(layered);
        untangleGroups(layered);
    } else {
        reorderPorts(layered, slots, far, byPlace);
        assignPortKeys(layered, slots, slotCounts);
    }
    alignGroups(layered);
    separateComponents(layered);

    sorterReset(sorter);
    centers.reset();
    far.reset();
    byPlace.reset();
    slots.reset();
    slotCounts.reset();
}

uint64_t graphLayeredGapCrossings(GraphLayered& layered, uint32_t layer) {
    uint32_t first = layered.gapSegmentStart.data[layer];
    uint32_t count = layered.gapSegmentStart.data[layer + 1] - first;
    if (count < 2)
        return 0;
    // By the gap's segments: the keys of their upper and lower ends, and the ranks of their lower keys among the
    // distinct lower keys.
    RawArray<float>& keys = layered.scratchFloat[1];
    RawArray<uint32_t>& byUpper = layered.scratch[2];
    RawArray<uint32_t>& byLower = layered.scratch[3];
    RawArray<uint32_t>& rank = layered.scratch[4];
    keys.resize(2 * count);
    byUpper.resize(count);
    byLower.resize(count);
    rank.resize(count);
    for (uint32_t i = 0; i < count; ++i) {
        const GraphSegment& segment = layered.segments.data[layered.gapSegments.data[first + i]];
        keys.data[2 * i] = endKey(layered, segment.upper, segment.upperPort);
        keys.data[2 * i + 1] = endKey(layered, segment.lower, segment.lowerPort);
        byUpper.data[i] = i;
        byLower.data[i] = i;
    }
    if (count <= kMaxPairwiseGap) {
        int64_t crossings = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const GraphSegment& a = layered.segments.data[layered.gapSegments.data[first + i]];
            for (uint32_t j = i + 1; j < count; ++j) {
                if (!keysCross(keys.data[2 * i], keys.data[2 * i + 1], keys.data[2 * j], keys.data[2 * j + 1]))
                    continue;
                const GraphSegment& b = layered.segments.data[layered.gapSegments.data[first + j]];
                if (a.chain == b.chain) {
                    crossings += a.chain ? 0 : 1;
                    continue;
                }
                const GraphSegment& edge = a.chain ? b : a;
                uint32_t group = layered.vertices.data[(a.chain ? a : b).upper].node;
                if (!within(layered, layered.vertices.data[edge.upper], group) &&
                        !within(layered, layered.vertices.data[edge.lower], group))
                    crossings += kGraphGroupCrossingWeight;
            }
        }
        return uint64_t(crossings);
    }
    // Segments that share an upper key do not cross. Visiting them in the order of their lower keys keeps each from
    // counting against the next.
    sortItems(byUpper.span(), [&](uint32_t a, uint32_t b) {
        float ua = keys.data[2 * a];
        float ub = keys.data[2 * b];
        return ua < ub || (ua == ub && keys.data[2 * a + 1] < keys.data[2 * b + 1]);
    });
    sortItems(byLower.span(), [&](uint32_t a, uint32_t b) { return keys.data[2 * a + 1] < keys.data[2 * b + 1]; });
    uint32_t ranks = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t s = byLower.data[i];
        if (i > 0 && keys.data[2 * s + 1] != keys.data[2 * byLower.data[i - 1] + 1])
            ++ranks;
        rank.data[s] = ranks;
    }
    ++ranks;

    // Each segment crosses the segments visited before it whose lower ends rank above its own. The edges' segments and
    // the chains are counted in separate trees, so that each pair takes its weight.
    uint32_t firstIndex = 1;
    while (firstIndex < ranks)
        firstIndex *= 2;
    --firstIndex;
    RawArray<int64_t>& edgeTree = layered.scratchWide[0];
    RawArray<int64_t>& chainTree = layered.scratchWide[1];
    edgeTree.assignZeroed(2 * firstIndex + 1);
    chainTree.assignZeroed(2 * firstIndex + 1);
    int64_t crossings = 0;
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t s = byUpper.data[i];
        uint32_t r = rank.data[s];
        int64_t edges = treeAbove(edgeTree.data, firstIndex, r);
        if (layered.segments.data[layered.gapSegments.data[first + s]].chain) {
            crossings += edges * kGraphGroupCrossingWeight;
            treeInsert(chainTree.data, firstIndex, r);
        } else {
            crossings += edges + treeAbove(chainTree.data, firstIndex, r) * kGraphGroupCrossingWeight;
            treeInsert(edgeTree.data, firstIndex, r);
        }
    }

    // An edge that crosses the chain of a group that holds one of its ends enters or leaves the group rather than
    // passing through it, so its crossing with that chain is taken back. Only the groups that hold an end can hold one.
    for (uint32_t i = 0; i < count; ++i) {
        const GraphSegment& segment = layered.segments.data[layered.gapSegments.data[first + i]];
        if (segment.chain)
            continue;
        uint32_t upperGroup = layered.vertices.data[segment.upper].group;
        uint32_t lowerGroup = layered.vertices.data[segment.lower].group;
        uint32_t common = commonGroup(layered, upperGroup, lowerGroup);
        auto takeBack = [&](uint32_t group) {
            if (spansGap(layered, group, layer) &&
                    keysCross(keys.data[2 * i], keys.data[2 * i + 1], placeholderKey(layered, group, layer),
                        placeholderKey(layered, group, layer + 1)))
                crossings -= kGraphGroupCrossingWeight;
        };
        for (uint32_t g = upperGroup; g != kGraphNone; g = layered.scope->nodes.data[g].parent)
            takeBack(g);
        for (uint32_t g = lowerGroup; g != common; g = layered.scope->nodes.data[g].parent)
            takeBack(g);
    }
    PR_ASSERT(crossings >= 0);
    return uint64_t(crossings);
}

int64_t graphLayeredMoveDelta(GraphLayered& layered, uint32_t layer, uint32_t from, uint32_t to) {
    if (from == to)
        return 0;
    const uint32_t* row = layered.layerVertices.data + layered.layerStart.data[layer];
    uint32_t vertex = row[from];
    const GraphVertex& moved = layered.vertices.data[vertex];
    PR_ASSERT(moved.kind != GraphVertexKind_Placeholder);
    // The positions that the vertex passes. Only its order against theirs changes, so only the pairs of its segments
    // and theirs can change whether they cross.
    uint32_t low = from < to ? from + 1 : to;
    uint32_t high = from < to ? to : from - 1;
    auto farKey = [&](const GraphSegment& segment, uint32_t near) {
        if (segment.upper == near)
            return endKey(layered, segment.lower, segment.lowerPort);
        return endKey(layered, segment.upper, segment.upperPort);
    };
    // A move past one vertex that is not a placeholder, with few pairs of segments, compares the pairs one by one.
    if (low == high && layered.vertices.data[row[low]].kind != GraphVertexKind_Placeholder) {
        uint32_t passed = row[low];
        uint32_t ownFirst = layered.incidentStart.data[vertex];
        uint32_t ownEnd = layered.incidentStart.data[vertex + 1];
        uint32_t passedFirst = layered.incidentStart.data[passed];
        uint32_t passedEnd = layered.incidentStart.data[passed + 1];
        if (uint64_t(ownEnd - ownFirst) * (passedEnd - passedFirst) <= kMaxPairwise) {
            int64_t total = 0;
            for (uint32_t i = ownFirst; i < ownEnd; ++i) {
                const GraphSegment& a = layered.segments.data[layered.incident.data[i]];
                bool upward = a.lower == vertex;
                float key = farKey(a, vertex);
                for (uint32_t j = passedFirst; j < passedEnd; ++j) {
                    const GraphSegment& b = layered.segments.data[layered.incident.data[j]];
                    if ((b.lower == passed) != upward)
                        continue;
                    float other = farKey(b, passed);
                    total += key < other ? 1 : key > other ? -1 : 0;
                }
            }
            return from < to ? total : -total;
        }
    }
    // The far keys of the vertex's segments toward one adjacent layer, and those of the passed vertices' segments and
    // chains toward it.
    RawArray<float>& own = layered.scratchFloat[0];
    RawArray<float>& others = layered.scratchFloat[1];
    RawArray<float>& chains = layered.scratchFloat[2];
    // Returns true if the group's chain toward `farLayer` changes order against the vertex and weighs against its
    // segments: the group's placeholder is among the passed vertices, and the group does not hold the vertex.
    auto weighsChain = [&](uint32_t group, uint32_t farLayer) {
        uint32_t top = layered.rankTop.data[group];
        uint32_t bottom = layered.rankBottom.data[group];
        if (layer < top || layer > bottom || farLayer < top || farLayer > bottom)
            return false;
        uint32_t at = layered.vertices.data[layered.nodeVertex.data[group] + (layer - top)].order;
        return at >= low && at <= high && !within(layered, moved, group);
    };
    int64_t total = 0;
    for (int upward = 0; upward < 2; ++upward) {
        if (upward != 0 ? layer == 0 : layer + 1 >= layered.layerCount)
            continue;
        uint32_t farLayer = upward != 0 ? layer - 1 : layer + 1;
        others.clear();
        chains.clear();
        for (uint32_t k = low; k <= high; ++k) {
            uint32_t w = row[k];
            for (uint32_t i = layered.incidentStart.data[w]; i < layered.incidentStart.data[w + 1]; ++i) {
                const GraphSegment& segment = layered.segments.data[layered.incident.data[i]];
                if ((segment.lower == w) == (upward != 0))
                    others.push(farKey(segment, w));
            }
            const GraphVertex& passed = layered.vertices.data[w];
            if (passed.kind == GraphVertexKind_Placeholder && weighsChain(passed.node, farLayer))
                chains.push(placeholderKey(layered, passed.node, farLayer));
        }
        // A segment whose far end lies in a group enters the group, so its pair with the group's chain is excluded.
        own.clear();
        int64_t excluded = 0;
        for (uint32_t i = layered.incidentStart.data[vertex]; i < layered.incidentStart.data[vertex + 1]; ++i) {
            const GraphSegment& segment = layered.segments.data[layered.incident.data[i]];
            if ((segment.lower == vertex) != (upward != 0))
                continue;
            float key = farKey(segment, vertex);
            own.push(key);
            uint32_t far = segment.upper == vertex ? segment.lower : segment.upper;
            for (uint32_t g = layered.vertices.data[far].group; g != kGraphNone; g =
                    layered.scope->nodes.data[g].parent) {
                if (!weighsChain(g, farLayer))
                    continue;
                float chain = placeholderKey(layered, g, farLayer);
                excluded += key < chain ? 1 : key > chain ? -1 : 0;
            }
        }
        total +=orderBalance(own, others) + (orderBalance(own, chains) - excluded) * kGraphGroupCrossingWeight;
    }
    // Before a move toward later positions, a pair crosses if the vertex's far end lies past the other's, and after it,
    // if the far end lies before.
    return from < to ? total : -total;
}

} // namespace pr_impl
