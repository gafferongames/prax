// Placement (graph_place.h): assigns the lanes for edges that run beside their nodes, the positions on the cross axis,
// the tracks of orthogonal routes in the gaps between layers, and the layers' positions on the main axis.
//
// The balanced placement takes the cross-axis positions from Brandes and Köpf's method (graph_place_balanced.h). The
// shortest placement takes them from one network simplex problem (Gansner et al., 1993, section 4.2). Each edge segment
// adds a node whose value is at most the attachments at both ends, so minimizing the problem minimizes the distance
// between them. Spacing between neighbors in a layer and the enclosure of groups are constraints of the same problem.

#include "graph/src/graph_place.h"

#include "pr/float.h"
#include "pr/sort.h"

#include "graph/src/graph_place_balanced.h"

namespace pr_impl {

namespace {

// The number of simplex units per unit of the caller's coordinates.
constexpr float kQuantum = 32;

// The weight that keeps a group narrow, relative to an edge's weight.
constexpr int64_t kGroupWidthWeight = 512;

int64_t quantize(float length) {
    return int64_t(prF64Ceil(double(length) * double(kQuantum)));
}

bool isVirtual(const GraphVertex& vertex) {
    return vertex.kind != GraphVertexKind_Leaf;
}

bool isLane(GraphSide side) {
    return side == GraphSide_CrossStart || side == GraphSide_CrossEnd;
}

// Assigns the side and lane of one end of an edge. `natural` is the side where the end meets its vertex if it has no
// port.
void assignEnd(GraphLayered& layered, GraphEnd& end, GraphSide natural, RawArray<uint32_t>& laneCounts) {
    end.side = natural;
    end.lane = kGraphLaneNone;
    if (end.port == kGraphNone)
        return;
    const GraphLayeredPort& port = layered.ports.data[end.port];
    end.side = port.side;
    if (port.side == natural)
        return;
    GraphSide lane = port.side;
    if (!isLane(lane)) {
        float cross = layered.vertices.data[end.vertex].cross;
        lane = port.offset < cross * 0.5f ? GraphSide_CrossStart : GraphSide_CrossEnd;
    }
    end.lane = lane;
    end.laneIndex = uint16_t(laneCounts.data[end.vertex * 2 + (lane == GraphSide_CrossEnd ? 1 : 0)]++);
}

// An item in a layer at one level of the hierarchy: a vertex, or a group whose sides are variables.
struct Side {
    uint32_t variable;
    float extent;                   // from the variable to the item's edge
    float spacing;
};

struct Emitter {
    GraphLayered* layered;
    const RawArray<uint32_t>* groupVariable; // by node: the variable of a group's left side, then its right
};

bool within(const GraphLayered& layered, const GraphVertex& vertex, uint32_t group) {
    for (uint32_t g = vertex.group; g != kGraphNone; g = layered.scope->nodes.data[g].parent) {
        if (g == group)
            return true;
    }
    return false;
}

// Adds the spacing constraints between adjacent items of `vertices` within `group`.
void emitSeparation(Emitter& emitter, const uint32_t* vertices, uint32_t count, uint32_t group) {
    GraphLayered& layered = *emitter.layered;
    bool hasPrevious = false;
    Side previous{};
    uint32_t previousComponent = kGraphNone;
    for (uint32_t i = 0; i < count;) {
        const GraphVertex& vertex = layered.vertices.data[vertices[i]];
        Side left{};
        Side right{};
        uint32_t component = graphLayeredVertexComponent(layered, vertices[i]);
        if (vertex.group == group) {
            if (vertex.kind == GraphVertexKind_Placeholder) {
                ++i;
                continue;
            }
            float spacing = graphLayeredSpacing(layered, vertices[i]);
            left = Side{vertices[i], vertex.cross * 0.5f + vertex.reserveStart, spacing};
            right = Side{vertices[i], vertex.cross * 0.5f + vertex.reserveEnd, spacing};
            ++i;
        } else {
            uint32_t child = graphLayeredChildOf(layered, vertex.group, group);
            uint32_t j = i + 1;
            while (j < count && within(layered, layered.vertices.data[vertices[j]], child))
                ++j;
            emitSeparation(emitter, vertices + i, j - i, child);
            uint32_t variable = emitter.groupVariable->data[child];
            left = Side{variable, layered.groupReserveStart.data[child], layered.options.nodeSpacing};
            right = Side{variable + 1, layered.groupReserveEnd.data[child], layered.options.nodeSpacing};
            i = j;
        }
        if (hasPrevious) {
            float spacing = (previous.spacing + left.spacing) * 0.5f;
            if (component != previousComponent)
                spacing = layered.options.componentSpacing;
            float gap = previous.extent + spacing + left.extent;
            graphSimplexAddEdge(layered.simplex, previous.variable, left.variable, quantize(gap), 0);
        }
        previous = right;
        previousComponent = component;
        hasPrevious = true;
    }
}

// Moves each vertex to the middle of the range of positions that cost the same as its own, within its constraints.
// Network simplex stops at a corner of the optimal set, so a node over two children ends up above one of them. Each
// move preserves the cost and the constraints, so the result stays optimal.
void balance(GraphLayered& layered, uint32_t auxEdgeStart) {
    GraphSimplex& simplex = layered.simplex;
    uint32_t vertexCount = layered.vertices.count;
    int64_t* x = simplex.values.data;
    const GraphSimplexEdge* edges = simplex.edges.data;
    uint32_t edgeCount = simplex.edges.count;

    // By vertex: the other ends of its segments, each with the difference between the two ends' offsets and the
    // segment's weight.
    RawArray<uint32_t>& start = layered.scratch[3];
    start.assignZeroed(vertexCount + 1);
    for (uint32_t e = auxEdgeStart; e + 1 < edgeCount; e += 2) {
        ++start.data[edges[e].head + 1];
        ++start.data[edges[e + 1].head + 1];
    }
    for (uint32_t v = 0; v < vertexCount; ++v)
        start.data[v + 1] += start.data[v];
    RawArray<uint32_t>& partner = layered.scratch[4];
    RawArray<int64_t>& shift = layered.scratchWide[0];
    RawArray<int64_t>& weight = layered.scratchWide[1];
    uint32_t total = start.data[vertexCount];
    partner.resize(total);
    shift.resize(total);
    weight.resize(total);
    RawArray<uint32_t>& fill = layered.scratch[5];
    fill.resize(vertexCount);
    for (uint32_t v = 0; v < vertexCount; ++v)
        fill.data[v] = start.data[v];
    for (uint32_t e = auxEdgeStart; e + 1 < edgeCount; e += 2) {
        const GraphSimplexEdge& a = edges[e];
        const GraphSimplexEdge& b = edges[e + 1];
        uint32_t i = fill.data[a.head]++;
        partner.data[i] = b.head;
        shift.data[i] = a.minLength - b.minLength;
        weight.data[i] = a.weight;
        uint32_t j = fill.data[b.head]++;
        partner.data[j] = a.head;
        shift.data[j] = b.minLength - a.minLength;
        weight.data[j] = b.weight;
    }

    // By vertex: the constraints that bound it from below, as their edges into it, and from above, as their edges out.
    RawArray<uint32_t> boundStart{};
    RawArray<uint32_t> bounds{};
    boundStart.assignZeroed(vertexCount + 1);
    for (uint32_t e = 0; e < auxEdgeStart; ++e) {
        if (edges[e].head < vertexCount)
            ++boundStart.data[edges[e].head + 1];
        if (edges[e].tail < vertexCount)
            ++boundStart.data[edges[e].tail + 1];
    }
    for (uint32_t v = 0; v < vertexCount; ++v)
        boundStart.data[v + 1] += boundStart.data[v];
    bounds.resize(boundStart.data[vertexCount]);
    for (uint32_t v = 0; v < vertexCount; ++v)
        fill.data[v] = boundStart.data[v];
    for (uint32_t e = 0; e < auxEdgeStart; ++e) {
        if (edges[e].head < vertexCount)
            bounds.data[fill.data[edges[e].head]++] = e;
        if (edges[e].tail < vertexCount)
            bounds.data[fill.data[edges[e].tail]++] = e;
    }

    RawArray<int64_t>& points = layered.scratchWide[2];
    RawArray<uint32_t> byPoint{};
    for (uint32_t pass = 0; pass < 4; ++pass) {
        for (uint32_t k = 0; k < vertexCount; ++k) {
            uint32_t v = pass % 2 == 0 ? k : vertexCount - 1 - k;
            uint32_t first = start.data[v];
            uint32_t count = start.data[v + 1] - first;
            if (count == 0)
                continue;
            // The cost is a weighted sum of distances to breakpoints, so it is minimal at the weighted medians.
            points.resize(count);
            byPoint.resize(count);
            int64_t sum = 0;
            for (uint32_t i = 0; i < count; ++i) {
                points.data[i] = x[partner.data[first + i]] + shift.data[first + i];
                byPoint.data[i] = i;
                sum += weight.data[first + i];
            }
            sortItems(byPoint.span(), [&](uint32_t a, uint32_t b) {
                return points.data[a] < points.data[b] || (points.data[a] == points.data[b] && a < b);
            });
            int64_t low = 0;
            int64_t high = 0;
            int64_t running = 0;
            for (uint32_t i = 0; i < count; ++i) {
                running += weight.data[first + byPoint.data[i]];
                if (2 * running > sum) {
                    low = points.data[byPoint.data[i]];
                    high = low;
                    break;
                }
                if (2 * running == sum) {
                    low = points.data[byPoint.data[i]];
                    high = i + 1 < count ? points.data[byPoint.data[i + 1]] : low;
                    break;
                }
            }
            if (x[v] < low || x[v] > high)
                continue;
            for (uint32_t i = boundStart.data[v]; i < boundStart.data[v + 1]; ++i) {
                uint32_t e = bounds.data[i];
                if (edges[e].head == v && x[edges[e].tail] + edges[e].minLength > low)
                    low = x[edges[e].tail] + edges[e].minLength;
                if (edges[e].tail == v && x[edges[e].head] - edges[e].minLength < high)
                    high = x[edges[e].head] - edges[e].minLength;
            }
            if (low <= x[v] && x[v] <= high)
                x[v] = low + (high - low) / 2;
        }
    }
    byPoint.reset();
    boundStart.reset();
    bounds.reset();
}

// The cross-axis offset from a vertex's center at which placement aligns an end. An end toward a dummy or label aligns
// at its attachment, so that the edge runs straight from its port into its chain. An end toward another node aligns at
// its fixed port or its lane, and otherwise at the node's center. Ports that the layout places spread around the
// center, so nodes joined directly line up by their centers, and a chain of nodes runs straight however many edges each
// has.
float alignedAttachment(const GraphLayered& layered, const GraphEnd& end, bool towardVirtual) {
    if (!towardVirtual && end.lane == kGraphLaneNone && end.port != kGraphNone &&
            layered.scope->ports.data[end.port].constraint != PrGraphPortConstraint_FixedPosition)
        return 0;
    return graphLayeredAttachment(layered, end);
}

// The cross-axis positions where a segment leaves its upper vertex and reaches its lower one.
void segmentEnds(const GraphLayered& layered, const GraphSegment& segment, float& upper, float& lower) {
    const GraphLayeredEdge& edge = layered.edges.data[segment.edge];
    upper = layered.vertices.data[segment.upper].x;
    if (segment.upper == edge.tail.vertex)
        upper += graphLayeredAttachment(layered, edge.tail);
    lower = layered.vertices.data[segment.lower].x;
    if (segment.lower == edge.head.vertex)
        lower += graphLayeredAttachment(layered, edge.head);
}

// A segment tree over the elementary intervals between a gap's sorted endpoints, which finds the latest track among the
// steps whose runs overlap a range. Each node holds the largest value set on its whole range, and the largest set
// anywhere within it. A value is a track plus one, so that 0 means none.
struct TrackTree {
    uint32_t size;                  // the number of leaves, a power of two
    RawArray<uint32_t> cover;       // by node
    RawArray<uint32_t> best;        // by node
};

// The largest value set on any interval of [low, high), within the node's range [nodeLow, nodeHigh).
uint32_t trackTreeQuery(const TrackTree& tree, uint32_t node, uint32_t nodeLow, uint32_t nodeHigh, uint32_t low,
        uint32_t high) {
    if (high <= nodeLow || nodeHigh <= low)
        return 0;
    if (low <= nodeLow && nodeHigh <= high)
        return tree.best.data[node];
    uint32_t middle = nodeLow + (nodeHigh - nodeLow) / 2;
    uint32_t left = trackTreeQuery(tree, 2 * node, nodeLow, middle, low, high);
    uint32_t right = trackTreeQuery(tree, 2 * node + 1, middle, nodeHigh, low, high);
    uint32_t below = left > right ? left : right;
    return below > tree.cover.data[node] ? below : tree.cover.data[node];
}

// Sets `value` on the intervals of [low, high), within the node's range [nodeLow, nodeHigh), where it is larger.
void trackTreeSet(TrackTree& tree, uint32_t node, uint32_t nodeLow, uint32_t nodeHigh, uint32_t low, uint32_t high,
        uint32_t value) {
    if (high <= nodeLow || nodeHigh <= low)
        return;
    if (tree.best.data[node] < value)
        tree.best.data[node] = value;
    if (low <= nodeLow && nodeHigh <= high) {
        if (tree.cover.data[node] < value)
            tree.cover.data[node] = value;
        return;
    }
    uint32_t middle = nodeLow + (nodeHigh - nodeLow) / 2;
    trackTreeSet(tree, 2 * node, nodeLow, middle, low, high, value);
    trackTreeSet(tree, 2 * node + 1, middle, nodeHigh, low, high, value);
}

// Assigns each segment that changes its cross-axis position its track in its gap, for orthogonal routes. A step runs
// along the main axis at its upper position to its track, across to its lower position, and on along the main axis.
// Steps whose runs across come within the edge spacing of each other take different tracks. The steps are taken in an
// order, and each takes the track after the latest track of the overlapping steps before it.
//
// The order decides the crossings. Two steps that run the same way and overlap cross twice or not at all, depending on
// which takes the earlier track. The one whose lower end lies farther the way they run must take it, so that the
// other's legs stay clear of its run. Of the steps toward higher positions, the one that ends higher comes first, and
// of those toward lower positions, the one that ends lower. Two steps that run opposite ways cross once in either
// order, so the order merges the two by the lower position of each step. It follows the gap from low positions to high
// as far as the two orders allow.
//
// The cost is O(k log k) for the gap's k steps: a sort, and one query and one update of a segment tree for each step.
void assignTracks(GraphLayered& layered) {
    float spacing = layered.options.edgeSpacing;
    layered.segmentTrack.assignZeroed(layered.segments.count);
    layered.trackCount.assignZeroed(layered.layerCount);
    // A step, with its cross-axis positions at its upper and lower ends.
    struct Step {
        uint32_t segment;
        float upper;
        float lower;
    };

    // The gap's steps toward higher positions, and those toward lower ones.
    RawArray<Step> rising{};
    RawArray<Step> falling{};

    RawArray<Step> steps{};             // the gap's steps in the order that they take tracks
    RawArray<float> ends{};             // the sorted, distinct ends of the steps' ranges
    TrackTree tree{};
    auto low = [](const Step& step) {
        return step.upper < step.lower ? step.upper : step.lower;
    };
    auto high = [](const Step& step) {
        return step.upper < step.lower ? step.lower : step.upper;
    };
    for (uint32_t gap = 0; gap + 1 < layered.layerCount; ++gap) {
        rising.clear();
        falling.clear();
        for (uint32_t i = layered.gapSegmentStart.data[gap]; i < layered.gapSegmentStart.data[gap + 1]; ++i) {
            uint32_t s = layered.gapSegments.data[i];
            const GraphSegment& segment = layered.segments.data[s];
            if (segment.chain)
                continue;
            Step step{.segment = s};
            segmentEnds(layered, segment, step.upper, step.lower);
            if (prF32Abs(step.upper - step.lower) < 0.01f)
                continue;
            (step.lower > step.upper ? rising : falling).push(step);
        }
        if (rising.count + falling.count == 0)
            continue;
        sortItems(rising.span(), [](const Step& a, const Step& b) {
            if (a.lower != b.lower)
                return a.lower > b.lower;
            if (a.upper != b.upper)
                return a.upper > b.upper;
            return a.segment < b.segment;
        });
        sortItems(falling.span(), [](const Step& a, const Step& b) {
            if (a.lower != b.lower)
                return a.lower < b.lower;
            if (a.upper != b.upper)
                return a.upper < b.upper;
            return a.segment < b.segment;
        });
        steps.clear();
        uint32_t r = 0;
        uint32_t f = 0;
        while (r < rising.count || f < falling.count) {
            bool takeRising = f == falling.count;
            if (r < rising.count && f < falling.count) {
                float a = low(rising.data[r]);
                float b = low(falling.data[f]);
                takeRising = a < b || (a == b && rising.data[r].segment < falling.data[f].segment);
            }
            steps.push(takeRising ? rising.data[r++] : falling.data[f++]);
        }

        // A step's range runs from its low end to the edge spacing past its high end. Two steps overlap if their ranges
        // do, as half-open intervals, so they overlap if they share an elementary interval between the ends.
        ends.clear();
        for (const Step& step : steps.span()) {
            ends.push(low(step));
            ends.push(high(step) + spacing);
        }
        sortItems(ends.span(), [](float a, float b) { return a < b; });
        uint32_t distinct = 0;
        for (uint32_t i = 0; i < ends.count; ++i) {
            if (i == 0 || ends.data[i] != ends.data[distinct - 1])
                ends.data[distinct++] = ends.data[i];
        }
        ends.resize(distinct);
        auto rankOf = [&](float x) {
            uint32_t first = 0;
            uint32_t last = ends.count;
            while (first < last) {
                uint32_t middle = first + (last - first) / 2;
                if (ends.data[middle] < x)
                    first = middle + 1;
                else
                    last = middle;
            }
            return first;
        };
        tree.size = 1;
        while (tree.size + 1 < ends.count)
            tree.size *= 2;
        tree.cover.assignZeroed(2 * tree.size);
        tree.best.assignZeroed(2 * tree.size);
        uint32_t tracks = 0;
        for (const Step& step : steps.span()) {
            uint32_t from = rankOf(low(step));
            uint32_t to = rankOf(high(step) + spacing);
            uint32_t track = trackTreeQuery(tree, 1, 0, tree.size, from, to);
            trackTreeSet(tree, 1, 0, tree.size, from, to, track + 1);
            layered.segmentTrack.data[step.segment] = track;
            tracks = track + 1 > tracks ? track + 1 : tracks;
        }
        layered.trackCount.data[gap] = tracks;
    }
    rising.reset();
    falling.reset();
    steps.reset();
    ends.reset();
    tree.cover.reset();
    tree.best.reset();
}

// Places the vertices and groups on the cross axis by network simplex: the positions that minimize the weighted
// distances between the ends of each segment, within the spacing and enclosure constraints.
void placeShortest(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    uint32_t n = scope.nodes.count;
    uint32_t vertexCount = layered.vertices.count;
    // The variables: one for each vertex, then each group's left and right sides, then one for each segment.
    RawArray<uint32_t>& groupVariable = layered.scratch[1];
    groupVariable.resize(n);
    uint32_t count = vertexCount;
    for (uint32_t v = 0; v < n; ++v) {
        groupVariable.data[v] = kGraphNone;
        if (layered.isGroup.data[v] != 0) {
            groupVariable.data[v] = count;
            count += 2;
        }
    }
    uint32_t segmentNodes = 0;
    for (uint32_t s = 0; s < layered.segments.count; ++s)
        segmentNodes += layered.segments.data[s].chain ? 0 : 1;
    GraphSimplex& simplex = layered.simplex;
    graphSimplexBegin(simplex, count + segmentNodes);

    Emitter emitter{.layered = &layered, .groupVariable = &groupVariable};
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        uint32_t start = layered.layerStart.data[layer];
        emitSeparation(emitter, layered.layerVertices.data + start, layered.layerStart.data[layer + 1] - start,
            kGraphNone);
    }
    for (uint32_t v = 0; v < vertexCount; ++v) {
        const GraphVertex& vertex = layered.vertices.data[v];
        if (vertex.group == kGraphNone)
            continue;
        uint32_t group = vertex.group;
        uint32_t left = groupVariable.data[group];
        const float* padding = layered.padding.data + group * GraphSide_Count;
        float half = vertex.cross * 0.5f;
        graphSimplexAddEdge(simplex, left, v, quantize(padding[GraphSide_CrossStart] + half + vertex.reserveStart), 0);
        graphSimplexAddEdge(simplex, v, left + 1, quantize(padding[GraphSide_CrossEnd] + half + vertex.reserveEnd), 0);
    }
    for (uint32_t v = 0; v < n; ++v) {
        if (layered.isGroup.data[v] == 0)
            continue;
        uint32_t left = groupVariable.data[v];
        const float* padding = layered.padding.data + v * GraphSide_Count;
        bool vertical = scope.direction == PrGraphDirection_Down || scope.direction == PrGraphDirection_Up;
        float least = vertical ? scope.nodes.data[v].extent.x : scope.nodes.data[v].extent.y;
        float pads = padding[GraphSide_CrossStart] + padding[GraphSide_CrossEnd];
        graphSimplexAddEdge(simplex, left, left + 1, quantize(least > pads ? least : pads), kGroupWidthWeight);
        uint32_t parent = scope.nodes.data[v].parent;
        if (parent == kGraphNone)
            continue;
        uint32_t outer = groupVariable.data[parent];
        const float* outerPadding = layered.padding.data + parent * GraphSide_Count;
        graphSimplexAddEdge(simplex, outer, left,
            quantize(outerPadding[GraphSide_CrossStart] + layered.groupReserveStart.data[v]), 0);
        graphSimplexAddEdge(simplex, left + 1, outer + 1,
            quantize(outerPadding[GraphSide_CrossEnd] + layered.groupReserveEnd.data[v]), 0);
    }
    uint32_t aux = count;
    uint32_t auxEdgeStart = simplex.edges.count;
    for (uint32_t s = 0; s < layered.segments.count; ++s) {
        const GraphSegment& segment = layered.segments.data[s];
        if (segment.chain)
            continue;
        float upperOffset = 0;
        float lowerOffset = 0;
        graphLayeredSegmentOffsets(layered, s, upperOffset, lowerOffset);
        // Gansner et al.'s weights favor straight long edges: 1 between nodes, 2 between a node and a dummy, and 8
        // between dummies.
        uint32_t virtualEnds = (isVirtual(layered.vertices.data[segment.upper]) ? 1 : 0) +
            (isVirtual(layered.vertices.data[segment.lower]) ? 1 : 0);
        static const int64_t kOmega[3] = {1, 2, 8};
        int64_t omega = kOmega[virtualEnds];
        int64_t weight = omega * graphLayeredWeight(segment.weight);
        graphSimplexAddEdge(simplex, aux, segment.upper, -quantize(upperOffset), weight);
        graphSimplexAddEdge(simplex, aux, segment.lower, -quantize(lowerOffset), weight);
        ++aux;
    }
    graphSimplexSolve(simplex, 16 * simplex.nodeCount + 4096);
    balance(layered, auxEdgeStart);
    for (uint32_t v = 0; v < vertexCount; ++v)
        layered.vertices.data[v].x = float(simplex.values.data[v]) / kQuantum;
    layered.groupLeft.resize(n);
    layered.groupRight.resize(n);
    for (uint32_t v = 0; v < n; ++v) {
        if (layered.isGroup.data[v] == 0)
            continue;
        layered.groupLeft.data[v] = float(simplex.values.data[groupVariable.data[v]]) / kQuantum;
        layered.groupRight.data[v] = float(simplex.values.data[groupVariable.data[v] + 1]) / kQuantum;
    }

}

} // namespace

void graphLayeredPlace(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    const PrGraphLayoutOptions& options = layered.options;
    uint32_t n = scope.nodes.count;
    uint32_t vertexCount = layered.vertices.count;

    // By vertex: the number of lanes on its cross-start side, then on its cross-end side.
    RawArray<uint32_t>& laneCounts = layered.scratch[0];
    laneCounts.assignZeroed(vertexCount * 2);
    RawArray<float>& loopLabels = layered.scratchFloat[0];
    loopLabels.assignZeroed(vertexCount * 2);
    for (uint32_t e = 0; e < layered.edges.count; ++e) {
        GraphLayeredEdge& edge = layered.edges.data[e];
        if (!edge.selfLoop) {
            assignEnd(layered, edge.tail, GraphSide_After, laneCounts);
            assignEnd(layered, edge.head, GraphSide_Before, laneCounts);
            continue;
        }
        // A self-loop runs in one lane: on the cross-start side if a port faces it, and on the cross-end side
        // otherwise.
        edge.tail.side = edge.tail.port != kGraphNone ? layered.ports.data[edge.tail.port].side : GraphSide_After;
        edge.head.side = edge.head.port != kGraphNone ? layered.ports.data[edge.head.port].side : GraphSide_Before;
        GraphSide lane = GraphSide_CrossEnd;
        if (edge.tail.side == GraphSide_CrossStart || edge.head.side == GraphSide_CrossStart)
            lane = GraphSide_CrossStart;
        uint32_t slot = edge.tail.vertex * 2 + (lane == GraphSide_CrossEnd ? 1 : 0);
        edge.tail.lane = lane;
        edge.head.lane = lane;
        edge.tail.laneIndex = uint16_t(laneCounts.data[slot]++);
        edge.head.laneIndex = edge.tail.laneIndex;
        const GraphScopeEdge& described = scope.edges.data[e];
        for (uint32_t i = 0; i < described.labelCount; ++i) {
            const GraphScopeLabel& label = scope.labels.data[described.labelStart + i];
            if (label.placement != PrGraphLabelPlacement_Center)
                continue;
            bool vertical = scope.direction == PrGraphDirection_Down || scope.direction == PrGraphDirection_Up;
            float cross = (vertical ? label.extent.x : label.extent.y) + options.labelSpacing;
            if (cross > loopLabels.data[slot])
                loopLabels.data[slot] = cross;
        }
    }
    // A group's self-loops count their lanes at its first placeholder, and run outside the group's border.
    layered.groupReserveStart.assignZeroed(n);
    layered.groupReserveEnd.assignZeroed(n);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        GraphVertex& vertex = layered.vertices.data[v];
        vertex.reserveStart = float(laneCounts.data[2 * v]) * options.edgeSpacing + loopLabels.data[2 * v];
        vertex.reserveEnd = float(laneCounts.data[2 * v + 1]) * options.edgeSpacing + loopLabels.data[2 * v + 1];
        if (vertex.kind != GraphVertexKind_Placeholder)
            continue;
        if (layered.nodeVertex.data[vertex.node] == v) {
            layered.groupReserveStart.data[vertex.node] = vertex.reserveStart;
            layered.groupReserveEnd.data[vertex.node] = vertex.reserveEnd;
        }
        vertex.reserveStart = 0;
        vertex.reserveEnd = 0;
    }

    if (options.placement == PrGraphPlacement_Shortest)
        placeShortest(layered);
    else
        graphLayeredPlaceBalanced(layered);

    // Places the layers along the main axis. Each band is as deep as its deepest vertex. Each gap makes room for the
    // padding of the groups that end before it or begin after it, and for the tracks of orthogonal routes.
    uint32_t layerCount = layered.layerCount;
    layered.bandStart.resize(layerCount);
    layered.bandExtent.assignZeroed(layerCount);
    RawArray<uint32_t>& hasNode = layered.scratch[2];
    hasNode.assignZeroed(layerCount);
    for (uint32_t v = 0; v < vertexCount; ++v) {
        const GraphVertex& vertex = layered.vertices.data[v];
        if (vertex.main > layered.bandExtent.data[vertex.layer])
            layered.bandExtent.data[vertex.layer] = vertex.main;
        if (vertex.kind == GraphVertexKind_Leaf && !scope.nodes.data[vertex.node].isProxy)
            hasNode.data[vertex.layer] = 1;
    }
    if (options.routing == PrGraphRouting_Orthogonal) {
        assignTracks(layered);
    } else {
        layered.segmentTrack.assignZeroed(layered.segments.count);
        layered.trackCount.assignZeroed(layerCount);
    }

    // By node: the padding that a group needs before its first layer and after its last. It includes the padding of
    // nested groups that share those layers. The extra padding after the last layer keeps the group at least as deep as
    // its minimum extent.
    RawArray<float>& before = layered.scratchFloat[0];
    RawArray<float>& after = layered.scratchFloat[2];
    RawArray<float> extra{};
    RawArray<float> opening{};          // by layer: the deepest padding before the groups that begin at it
    RawArray<float> closing{};          // by layer: the deepest padding after the groups that end before it
    extra.assignZeroed(n);
    layered.groupMainStart.resize(n);
    layered.groupMainEnd.resize(n);
    for (uint32_t pass = 0; pass <= n; ++pass) {
        before.assignZeroed(n);
        after.assignZeroed(n);
        for (uint32_t v = n; v-- > 0;) {
            if (layered.isGroup.data[v] == 0)
                continue;
            before.data[v] += layered.padding.data[v * GraphSide_Count + GraphSide_Before];
            after.data[v] += layered.padding.data[v * GraphSide_Count + GraphSide_After] + extra.data[v];
            uint32_t parent = scope.nodes.data[v].parent;
            if (parent == kGraphNone)
                continue;
            if (layered.rankTop.data[v] == layered.rankTop.data[parent] && before.data[v] > before.data[parent])
                before.data[parent] = before.data[v];
            if (layered.rankBottom.data[v] == layered.rankBottom.data[parent] && after.data[v] > after.data[parent])
                after.data[parent] = after.data[v];
        }
        // Computes each layer's start, past the deepest padding of the groups that end before it or begin at it.
        opening.assignZeroed(layerCount);
        closing.assignZeroed(layerCount);
        for (uint32_t v = 0; v < n; ++v) {
            if (layered.isGroup.data[v] == 0)
                continue;
            float& opens = opening.data[layered.rankTop.data[v]];
            opens = before.data[v] > opens ? before.data[v] : opens;
            uint32_t next = layered.rankBottom.data[v] + 1;
            if (next < layerCount) {
                float& closes = closing.data[next];
                closes = after.data[v] > closes ? after.data[v] : closes;
            }
        }
        float at = 0;
        for (uint32_t layer = 0; layer < layerCount; ++layer) {
            float opens = opening.data[layer];
            float closes = closing.data[layer];
            if (layer == 0) {
                at = opens;
            } else {
                bool full = hasNode.data[layer - 1] != 0 && hasNode.data[layer] != 0;
                float base = full ? options.layerSpacing : options.layerSpacing * 0.5f;
                float gap = base + closes + opens;
                float needed = closes + opens + 2 * options.edgeSpacing;
                if (options.routing == PrGraphRouting_Orthogonal)
                    needed = closes + opens + float(layered.trackCount.data[layer - 1] + 1) * options.edgeSpacing;
                at += gap > needed ? gap : needed;
            }
            layered.bandStart.data[layer] = at;
            at += layered.bandExtent.data[layer];
        }
        bool deficit = false;
        for (uint32_t v = 0; v < n; ++v) {
            if (layered.isGroup.data[v] == 0)
                continue;
            float start = layered.bandStart.data[layered.rankTop.data[v]] - before.data[v];
            uint32_t bottom = layered.rankBottom.data[v];
            float end = layered.bandStart.data[bottom] + layered.bandExtent.data[bottom] + after.data[v];
            layered.groupMainStart.data[v] = start;
            layered.groupMainEnd.data[v] = end;
            float least = scope.nodes.data[v].extent.x;
            if (scope.direction == PrGraphDirection_Down || scope.direction == PrGraphDirection_Up)
                least = scope.nodes.data[v].extent.y;
            if (end - start + 0.001f < least) {
                extra.data[v] += least - (end - start);
                deficit = true;
            }
        }
        if (!deficit)
            break;
    }
    extra.reset();
    opening.reset();
    closing.reset();
    for (uint32_t v = 0; v < vertexCount; ++v) {
        GraphVertex& vertex = layered.vertices.data[v];
        vertex.mainStart = layered.bandStart.data[vertex.layer] + (layered.bandExtent.data[vertex.layer] - vertex.main) * 0.5f;
    }

    // Each gap's free part starts past the groups that end before it and ends before the groups that begin after it.
    layered.gapStart.resize(layerCount);
    layered.gapEnd.resize(layerCount);
    for (uint32_t layer = 0; layer < layerCount; ++layer) {
        layered.gapStart.data[layer] = layered.bandStart.data[layer] + layered.bandExtent.data[layer];
        layered.gapEnd.data[layer] = layer + 1 < layerCount ? layered.bandStart.data[layer + 1] : kPrF32Infinity;
    }
    for (uint32_t v = 0; v < n; ++v) {
        if (layered.isGroup.data[v] == 0)
            continue;
        float& start = layered.gapStart.data[layered.rankBottom.data[v]];
        start = layered.groupMainEnd.data[v] > start ? layered.groupMainEnd.data[v] : start;
        uint32_t top = layered.rankTop.data[v];
        if (top > 0) {
            float& end = layered.gapEnd.data[top - 1];
            end = layered.groupMainStart.data[v] < end ? layered.groupMainStart.data[v] : end;
        }
    }
}

float graphLayeredAttachment(const GraphLayered& layered, const GraphEnd& end) {
    const GraphVertex& vertex = layered.vertices.data[end.vertex];
    float half = vertex.cross * 0.5f;
    if (end.lane != kGraphLaneNone) {
        float out = half + float(end.laneIndex + 1) * layered.options.edgeSpacing;
        return end.lane == GraphSide_CrossStart ? -out : out;
    }
    if (end.port == kGraphNone || vertex.kind != GraphVertexKind_Leaf)
        return 0;
    return layered.ports.data[end.port].offset - half;
}

float graphLayeredSpacing(const GraphLayered& layered, uint32_t vertex) {
    const GraphVertex& v = layered.vertices.data[vertex];
    if (v.kind == GraphVertexKind_Leaf && !layered.scope->nodes.data[v.node].isProxy)
        return layered.options.nodeSpacing;
    return layered.options.edgeSpacing;
}

void graphLayeredSegmentOffsets(const GraphLayered& layered, uint32_t segment, float& upper, float& lower) {
    const GraphSegment& s = layered.segments.data[segment];
    const GraphLayeredEdge& edge = layered.edges.data[s.edge];
    bool upperVirtual = isVirtual(layered.vertices.data[s.upper]);
    bool lowerVirtual = isVirtual(layered.vertices.data[s.lower]);
    upper = s.upper == edge.tail.vertex ? alignedAttachment(layered, edge.tail, lowerVirtual) : 0;
    lower = s.lower == edge.head.vertex ? alignedAttachment(layered, edge.head, upperVirtual) : 0;
}

} // namespace pr_impl
