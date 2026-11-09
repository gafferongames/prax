// Layered layout of a scope (graph_layered.h): the layout of a scope as a whole, and the phases before ordering. Those
// phases set up the abstract frame and the connected components, break cycles, assign layers, and build the layered
// graph.

#include "graph/src/graph_layered.h"

#include "pr/assert.h"
#include "pr/float.h"
#include "pr/sort.h"

#include "graph/src/graph_order.h"
#include "graph/src/graph_place.h"
#include "graph/src/graph_route.h"

namespace pr_impl {

namespace {

bool isVertical(PrGraphDirection direction) {
    return direction == PrGraphDirection_Down || direction == PrGraphDirection_Up;
}

bool isReversed(PrGraphDirection direction) {
    return direction == PrGraphDirection_Up || direction == PrGraphDirection_Left;
}

// The abstract side that corresponds to `side` of the result, in a layout in `direction`.
GraphSide abstractSide(PrGraphDirection direction, PrRectSide side) {
    // By direction, starting from Down, then by PrRectSide.
    static const GraphSide kSides[4][PrRectSide_Count] = {
        {GraphSide_Before, GraphSide_CrossEnd, GraphSide_After, GraphSide_CrossStart},
        {GraphSide_After, GraphSide_CrossEnd, GraphSide_Before, GraphSide_CrossStart},
        {GraphSide_CrossStart, GraphSide_After, GraphSide_CrossEnd, GraphSide_Before},
        {GraphSide_CrossStart, GraphSide_Before, GraphSide_CrossEnd, GraphSide_After},
    };
    return kSides[direction - PrGraphDirection_Down][side];
}

float mainOf(PrGraphDirection direction, PrVec2 v) {
    return isVertical(direction) ? v.y : v.x;
}

float crossOf(PrGraphDirection direction, PrVec2 v) {
    return isVertical(direction) ? v.x : v.y;
}

// The main-axis coordinate of a point in the caller's arrangement, increasing in the direction of flow.
float flowOf(PrGraphDirection direction, PrVec2 v) {
    float main = mainOf(direction, v);
    return isReversed(direction) ? -main : main;
}

PrVec2 centerOf(const GraphScopeNode& node) {
    return PrVec2{node.position.x + node.extent.x * 0.5f, node.position.y + node.extent.y * 0.5f};
}

uint32_t findRoot(RawArray<uint32_t>& parents, uint32_t v) {
    while (parents.data[v] != v) {
        parents.data[v] = parents.data[parents.data[v]];
        v = parents.data[v];
    }
    return v;
}

bool hasCenterLabel(const GraphScope& scope, const GraphScopeEdge& edge) {
    for (uint32_t i = 0; i < edge.labelCount; ++i) {
        if (scope.labels.data[edge.labelStart + i].placement == PrGraphLabelPlacement_Center)
            return true;
    }
    return false;
}

// Lays out a scope without padding, with its box at the origin.
void layoutScope(GraphLayered& layered, GraphScope& scope, const PrGraphLayoutOptions& options) {
    graphLayeredBegin(layered, scope, options);
    graphLayeredBreakCycles(layered);
    graphLayeredRank(layered);
    graphLayeredBuild(layered);
    graphLayeredOrder(layered);
    graphLayeredPlace(layered);
    graphLayeredRoute(layered, scope);
}

void resizeResults(GraphScope& scope) {
    scope.nodeRects.resize(scope.nodes.count);
    scope.portPositions.resize(scope.ports.count);
    scope.labelRects.resize(scope.labels.count);
    scope.routeStart.resize(scope.edges.count + 1);
    for (uint32_t i = 0; i <= scope.edges.count; ++i)
        scope.routeStart.data[i] = 0;
    scope.routePoints.clear();
}

PrRect offsetRect(PrRect rect, PrVec2 by) {
    return PrRect{rect.left + by.x, rect.top + by.y, rect.right + by.x, rect.bottom + by.y};
}

// Moves every result of the scope by `by`.
void offsetResults(GraphScope& scope, PrVec2 by) {
    for (uint32_t i = 0; i < scope.nodeRects.count; ++i)
        scope.nodeRects.data[i] = offsetRect(scope.nodeRects.data[i], by);
    for (uint32_t i = 0; i < scope.portPositions.count; ++i)
        scope.portPositions.data[i] = scope.portPositions.data[i] + by;
    for (uint32_t i = 0; i < scope.labelRects.count; ++i)
        scope.labelRects.data[i] = offsetRect(scope.labelRects.data[i], by);
    for (uint32_t i = 0; i < scope.routePoints.count; ++i)
        scope.routePoints.data[i] = scope.routePoints.data[i] + by;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Scopes

void graphScopeClear(GraphScope& scope) {
    scope.nodes.clear();
    scope.ports.clear();
    scope.edges.clear();
    scope.labels.clear();
    scope.nodeRects.clear();
    scope.portPositions.clear();
    scope.labelRects.clear();
    scope.routeStart.clear();
    scope.routePoints.clear();
    scope.extent = PrVec2{0, 0};
    scope.minExtent = PrVec2{0, 0};
    for (uint32_t s = 0; s < PrRectSide_Count; ++s)
        scope.padding[s] = 0;
}

void graphScopeReset(GraphScope& scope) {
    scope.nodes.reset();
    scope.ports.reset();
    scope.edges.reset();
    scope.labels.reset();
    scope.nodeRects.reset();
    scope.portPositions.reset();
    scope.labelRects.reset();
    scope.routeStart.reset();
    scope.routePoints.reset();
}

void graphScopeLayout(GraphScope& scope, const PrGraphLayoutOptions& options) {
    resizeResults(scope);
    uint32_t n = scope.nodes.count;
    GraphLayered layered{};

    if (n > 0)
        layoutScope(layered, scope, options);
    else
        scope.extent = PrVec2{0, 0};

    // Adds the scope's padding, and applies its minimum extent.
    offsetResults(scope, PrVec2{scope.padding[PrRectSide_Left], scope.padding[PrRectSide_Top]});
    scope.extent.x += scope.padding[PrRectSide_Left] + scope.padding[PrRectSide_Right];
    scope.extent.y += scope.padding[PrRectSide_Top] + scope.padding[PrRectSide_Bottom];
    if (scope.extent.x < scope.minExtent.x)
        scope.extent.x = scope.minExtent.x;
    if (scope.extent.y < scope.minExtent.y)
        scope.extent.y = scope.minExtent.y;

    graphLayeredReset(layered);
}

// ---------------------------------------------------------------------------------------------------------------------
// The pipeline's helpers

void graphLayeredReset(GraphLayered& layered) {
    layered.nodeMain.reset();
    layered.nodeCross.reset();
    layered.padding.reset();
    layered.isGroup.reset();
    layered.depth.reset();
    layered.unitPosition.reset();
    layered.component.reset();
    layered.componentKey.reset();
    layered.rankTop.reset();
    layered.rankBottom.reset();
    layered.nodeVertex.reset();
    layered.groupLeft.reset();
    layered.groupRight.reset();
    layered.groupMainStart.reset();
    layered.groupMainEnd.reset();
    layered.groupReserveStart.reset();
    layered.groupReserveEnd.reset();
    layered.ports.reset();
    layered.edges.reset();
    layered.vertices.reset();
    layered.segments.reset();
    layered.gapSegmentStart.reset();
    layered.gapSegments.reset();
    layered.incidentStart.reset();
    layered.incident.reset();
    layered.layerStart.reset();
    layered.layerVertices.reset();
    layered.bandStart.reset();
    layered.bandExtent.reset();
    layered.gapStart.reset();
    layered.gapEnd.reset();
    layered.trackCount.reset();
    layered.segmentTrack.reset();
    graphSimplexReset(layered.simplex);
    for (RawArray<uint32_t>& array : layered.scratch)
        array.reset();
    for (RawArray<float>& array : layered.scratchFloat)
        array.reset();
    for (RawArray<int64_t>& array : layered.scratchWide)
        array.reset();
    layered.bestOrder.reset();
}

uint32_t graphLayeredCommonGroup(const GraphLayered& layered, uint32_t a, uint32_t b) {
    const GraphScope& scope = *layered.scope;
    uint32_t x = scope.nodes.data[a].parent;
    uint32_t y = scope.nodes.data[b].parent;
    while (x != y) {
        if (x == kGraphNone || y == kGraphNone)
            return kGraphNone;
        if (layered.depth.data[x] >= layered.depth.data[y])
            x = scope.nodes.data[x].parent;
        else
            y = scope.nodes.data[y].parent;
    }
    return x;
}

uint32_t graphLayeredChildOf(const GraphLayered& layered, uint32_t node, uint32_t group) {
    const GraphScope& scope = *layered.scope;
    while (scope.nodes.data[node].parent != group) {
        node = scope.nodes.data[node].parent;
        PR_ASSERT(node != kGraphNone);
    }
    return node;
}

uint32_t graphLayeredVertexComponent(const GraphLayered& layered, uint32_t vertex) {
    const GraphVertex& v = layered.vertices.data[vertex];
    uint32_t node = v.node;
    if (v.kind == GraphVertexKind_Dummy || v.kind == GraphVertexKind_Label)
        node = layered.scope->edges.data[v.edge].source;
    return layered.component.data[node];
}

void graphLayeredDepthFirstOrder(uint32_t count, const RawArray<uint32_t>& edges, RawArray<uint32_t>& order,
        RawArray<uint32_t>& scratch) {
    uint32_t edgeCount = edges.count / 2;
    // The scratch storage holds three arrays in turn. The first is by item: where its edges start in the adjacency,
    // with the total last. The second is by item: 0 before the walk reaches it, 1 while it is open, and 2 once it is
    // finished. The third is the adjacency. The walk's stack holds an item and the position of its next edge for each
    // open item.
    scratch.assignZeroed(2 * count + 1 + edgeCount);
    uint32_t* start = scratch.data;
    uint32_t* state = scratch.data + count + 1;
    uint32_t* adjacency = scratch.data + 2 * count + 1;
    for (uint32_t e = 0; e < edgeCount; ++e)
        ++start[edges.data[2 * e] + 1];
    for (uint32_t u = 0; u < count; ++u)
        start[u + 1] += start[u];
    RawArray<uint32_t> fill{};
    fill.append(start, count);
    for (uint32_t e = 0; e < edgeCount; ++e)
        adjacency[fill.data[edges.data[2 * e]]++] = edges.data[2 * e + 1];
    fill.reset();

    RawArray<uint32_t> stack{};
    order.resize(count);
    uint32_t finished = count;
    for (uint32_t root = 0; root < count; ++root) {
        if (state[root] != 0)
            continue;
        state[root] = 1;
        stack.push(root);
        stack.push(start[root]);
        while (stack.count > 0) {
            uint32_t u = stack.data[stack.count - 2];
            uint32_t at = stack.data[stack.count - 1];
            if (at < start[u + 1]) {
                stack.data[stack.count - 1] = at + 1;
                uint32_t v = adjacency[at];
                if (state[v] == 0) {
                    state[v] = 1;
                    stack.push(v);
                    stack.push(start[v]);
                }
                continue;
            }
            state[u] = 2;
            order.data[--finished] = u;
            stack.resize(stack.count - 2);
        }
    }
    stack.reset();
}

int64_t graphLayeredWeight(float weight) {
    double scaled = prF64Round(double(weight) * 256.0);
    return scaled < 1 ? 1 : int64_t(scaled);
}

// ---------------------------------------------------------------------------------------------------------------------
// The abstract frame

void graphLayeredBegin(GraphLayered& layered, const GraphScope& scope, const PrGraphLayoutOptions& options) {
    layered.scope = &scope;
    layered.options = options;
    layered.options.direction = scope.direction;
    PrGraphDirection direction = scope.direction;
    uint32_t n = scope.nodes.count;
    layered.nodeMain.resize(n);
    layered.nodeCross.resize(n);
    layered.padding.resize(n * GraphSide_Count);
    layered.isGroup.assignZeroed(n);
    layered.depth.resize(n);
    for (uint32_t v = 0; v < n; ++v) {
        const GraphScopeNode& node = scope.nodes.data[v];
        PR_ASSERT(node.parent == kGraphNone || node.parent < v);
        if (node.parent != kGraphNone)
            layered.isGroup.data[node.parent] = 1;
        layered.depth.data[v] = node.parent == kGraphNone ? 0 : layered.depth.data[node.parent] + 1;
        layered.nodeMain.data[v] = mainOf(direction, node.extent);
        layered.nodeCross.data[v] = crossOf(direction, node.extent);
        for (uint32_t s = 0; s < PrRectSide_Count; ++s)
            layered.padding.data[v * GraphSide_Count + abstractSide(direction, PrRectSide(s))] = node.padding[s];
    }
    // Finds the connected components of the top-level units, joined by edges and by group membership. The components
    // are ordered on the cross axis by their order of description, or by the arrangement in an interactive layout.
    RawArray<uint32_t>& top = layered.scratch[0];
    RawArray<uint32_t>& sets = layered.scratch[1];
    top.resize(n);
    sets.resize(n);
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t parent = scope.nodes.data[v].parent;
        top.data[v] = parent == kGraphNone ? v : top.data[parent];
        sets.data[v] = v;
    }
    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        uint32_t a = findRoot(sets, top.data[scope.edges.data[e].source]);
        uint32_t b = findRoot(sets, top.data[scope.edges.data[e].target]);
        if (a != b)
            sets.data[a > b ? a : b] = a < b ? a : b;
    }
    // By node: the component whose root it is, numbered in the order that the components' first nodes were described.
    RawArray<uint32_t>& rootComponent = layered.scratch[2];
    rootComponent.resize(n);
    for (uint32_t v = 0; v < n; ++v)
        rootComponent.data[v] = kGraphNone;
    uint32_t componentCount = 0;
    layered.component.resize(n);
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t root = findRoot(sets, top.data[v]);
        if (rootComponent.data[root] == kGraphNone)
            rootComponent.data[root] = componentCount++;
        layered.component.data[v] = rootComponent.data[root];
    }
    layered.componentKey.resize(componentCount);
    for (uint32_t c = 0; c < componentCount; ++c)
        layered.componentKey.data[c] = float(c);
    if (options.mode == PrGraphLayoutMode_Interactive) {
        for (uint32_t c = 0; c < componentCount; ++c)
            layered.componentKey.data[c] = kPrF32Infinity;
        for (uint32_t v = 0; v < n; ++v) {
            float cross = crossOf(direction, centerOf(scope.nodes.data[v]));
            float& key = layered.componentKey.data[layered.component.data[v]];
            key = cross < key ? cross : key;
        }
    }

    layered.ports.resize(scope.ports.count);
    for (uint32_t p = 0; p < scope.ports.count; ++p) {
        const GraphScopePort& port = scope.ports.data[p];
        PR_ASSERT(layered.isGroup.data[port.node] == 0);
        GraphLayeredPort& placed = layered.ports.data[p];
        placed.side =
            port.constraint == PrGraphPortConstraint_Free ? GraphSide_After : abstractSide(direction, port.side);
        placed.offset = 0;
        placed.key = 0.5f;
        if (port.constraint == PrGraphPortConstraint_FixedPosition) {
            bool along = placed.side == GraphSide_CrossStart || placed.side == GraphSide_CrossEnd;
            placed.offset =
                along && isReversed(direction) ? layered.nodeMain.data[port.node] - port.offset : port.offset;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Cycles

void graphLayeredBreakCycles(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    PrGraphDirection direction = scope.direction;
    bool interactive = layered.options.mode == PrGraphLayoutMode_Interactive;
    uint32_t n = scope.nodes.count;

    // The children of each context. The top level is context `n`, after the nodes.
    RawArray<uint32_t>& childStart = layered.scratch[0];
    RawArray<uint32_t>& children = layered.scratch[1];
    childStart.assignZeroed(n + 2);
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t parent = scope.nodes.data[v].parent;
        ++childStart.data[(parent == kGraphNone ? n : parent) + 1];
    }
    for (uint32_t c = 0; c <= n; ++c)
        childStart.data[c + 1] += childStart.data[c];
    children.resize(n);
    RawArray<uint32_t>& fill = layered.scratch[2];
    fill.resize(n + 1);
    for (uint32_t c = 0; c <= n; ++c)
        fill.data[c] = childStart.data[c];
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t parent = scope.nodes.data[v].parent;
        children.data[fill.data[parent == kGraphNone ? n : parent]++] = v;
    }

    // Lifts each edge to the children of the innermost context that holds both of its ends. By edge: the two units and
    // the context.
    RawArray<uint32_t>& lifted = layered.scratch[3];
    lifted.resize(scope.edges.count * 3);
    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        uint32_t a = scope.edges.data[e].source;
        uint32_t b = scope.edges.data[e].target;
        if (a == b) {
            lifted.data[3 * e] = kGraphNone;
            continue;
        }
        while (layered.depth.data[a] > layered.depth.data[b])
            a = scope.nodes.data[a].parent;
        while (layered.depth.data[b] > layered.depth.data[a])
            b = scope.nodes.data[b].parent;
        PR_ASSERT(a != b);
        while (scope.nodes.data[a].parent != scope.nodes.data[b].parent) {
            a = scope.nodes.data[a].parent;
            b = scope.nodes.data[b].parent;
        }
        uint32_t context = scope.nodes.data[a].parent;
        lifted.data[3 * e] = a;
        lifted.data[3 * e + 1] = b;
        lifted.data[3 * e + 2] = context == kGraphNone ? n : context;
    }

    layered.unitPosition.resize(n);
    RawArray<uint32_t>& local = layered.scratch[4];
    local.resize(n);
    RawArray<uint32_t> localEdges{};
    RawArray<uint32_t> order{};
    RawArray<uint32_t> middle{};
    for (uint32_t context = 0; context <= n; ++context) {
        uint32_t first = childStart.data[context];
        uint32_t last = childStart.data[context + 1];
        if (first == last)
            continue;
        // Sorts the context's units into three classes: constrained to the first layer, unconstrained, and constrained
        // to the last layer.
        middle.clear();
        for (uint32_t i = first; i < last; ++i) {
            uint32_t u = children.data[i];
            if (scope.nodes.data[u].layer == PrGraphLayerConstraint_None)
                middle.push(u);
        }
        order.clear();
        if (interactive) {
            for (uint32_t i = 0; i < middle.count; ++i)
                order.push(i);
            sortItems(order.span(), [&](uint32_t a, uint32_t b) {
                float fa = flowOf(direction, centerOf(scope.nodes.data[middle.data[a]]));
                float fb = flowOf(direction, centerOf(scope.nodes.data[middle.data[b]]));
                return fa < fb || (fa == fb && a < b);
            });
        } else {
            for (uint32_t i = 0; i < middle.count; ++i)
                local.data[middle.data[i]] = i;
            localEdges.clear();
            for (uint32_t e = 0; e < scope.edges.count; ++e) {
                if (lifted.data[3 * e] == kGraphNone || lifted.data[3 * e + 2] != context)
                    continue;
                uint32_t a = lifted.data[3 * e];
                uint32_t b = lifted.data[3 * e + 1];
                if (scope.nodes.data[a].layer != PrGraphLayerConstraint_None ||
                        scope.nodes.data[b].layer != PrGraphLayerConstraint_None)
                    continue;
                localEdges.push(local.data[a]);
                localEdges.push(local.data[b]);
            }
            // The walk starts from the units in the order of description, so that, as a reader of a diagram expects,
            // the first unit described starts the flow.
            graphLayeredDepthFirstOrder(middle.count, localEdges, order, layered.scratch[5]);
        }
        uint32_t position = 0;
        for (uint32_t i = first; i < last; ++i) {
            uint32_t u = children.data[i];
            if (scope.nodes.data[u].layer == PrGraphLayerConstraint_First)
                layered.unitPosition.data[u] = position++;
        }
        for (uint32_t i = 0; i < order.count; ++i)
            layered.unitPosition.data[middle.data[order.data[i]]] = position++;
        for (uint32_t i = first; i < last; ++i) {
            uint32_t u = children.data[i];
            if (scope.nodes.data[u].layer == PrGraphLayerConstraint_Last)
                layered.unitPosition.data[u] = position++;
        }
    }

    layered.edges.resize(scope.edges.count);
    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        GraphLayeredEdge& edge = layered.edges.data[e];
        edge = GraphLayeredEdge{};
        edge.selfLoop = lifted.data[3 * e] == kGraphNone;
        edge.labelVertex = kGraphNone;
        if (!edge.selfLoop) {
            uint32_t a = lifted.data[3 * e];
            uint32_t b = lifted.data[3 * e + 1];
            edge.reversed = layered.unitPosition.data[a] > layered.unitPosition.data[b];
        }
    }
    localEdges.reset();
    order.reset();
    middle.reset();
}

// ---------------------------------------------------------------------------------------------------------------------
// Layers

void graphLayeredRank(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    uint32_t n = scope.nodes.count;
    bool interactive = layered.options.mode == PrGraphLayoutMode_Interactive;
    PrGraphDirection direction = scope.direction;

    // The simplex's nodes: one for each leaf, a top and a bottom for each group, and separators for each context that
    // has constrained units.
    RawArray<uint32_t>& topNode = layered.scratch[0];
    RawArray<uint32_t>& bottomNode = layered.scratch[1];
    topNode.resize(n);
    bottomNode.resize(n);
    uint32_t count = 0;
    for (uint32_t v = 0; v < n; ++v) {
        topNode.data[v] = count++;
        bottomNode.data[v] = layered.isGroup.data[v] != 0 ? count++ : topNode.data[v];
    }
    // By context, with the top level last: the separator after the units on the first layer, and the separator before
    // the units on the last layer.
    RawArray<uint32_t>& firstSeparator = layered.scratch[2];
    RawArray<uint32_t>& lastSeparator = layered.scratch[3];
    firstSeparator.resize(n + 1);
    lastSeparator.resize(n + 1);
    for (uint32_t c = 0; c <= n; ++c) {
        firstSeparator.data[c] = kGraphNone;
        lastSeparator.data[c] = kGraphNone;
    }
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t parent = scope.nodes.data[v].parent;
        uint32_t context = parent == kGraphNone ? n : parent;
        if (scope.nodes.data[v].layer == PrGraphLayerConstraint_First && firstSeparator.data[context] == kGraphNone)
            firstSeparator.data[context] = count++;
        if (scope.nodes.data[v].layer == PrGraphLayerConstraint_Last && lastSeparator.data[context] == kGraphNone)
            lastSeparator.data[context] = count++;
    }

    GraphSimplex& simplex = layered.simplex;
    graphSimplexBegin(simplex, count);
    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        const GraphScopeEdge& edge = scope.edges.data[e];
        const GraphLayeredEdge& placed = layered.edges.data[e];
        if (placed.selfLoop)
            continue;
        uint32_t tail = placed.reversed ? edge.target : edge.source;
        uint32_t head = placed.reversed ? edge.source : edge.target;
        uint32_t minLength = edge.minLength < 1 ? 1 : edge.minLength;
        if (minLength < 2 && hasCenterLabel(scope, edge))
            minLength = 2;
        graphSimplexAddEdge(simplex, bottomNode.data[tail], topNode.data[head], minLength,
            graphLayeredWeight(edge.weight));
    }
    constexpr int64_t kNestingWeight = 64;
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t parent = scope.nodes.data[v].parent;
        if (parent != kGraphNone) {
            if (topNode.data[parent] != topNode.data[v])
                graphSimplexAddEdge(simplex, topNode.data[parent], topNode.data[v], 0, kNestingWeight);
            graphSimplexAddEdge(simplex, bottomNode.data[v], bottomNode.data[parent], 0, kNestingWeight);
        }
        uint32_t context = parent == kGraphNone ? n : parent;
        PrGraphLayerConstraint layer = scope.nodes.data[v].layer;
        uint32_t first = firstSeparator.data[context];
        uint32_t last = lastSeparator.data[context];
        if (first != kGraphNone) {
            if (layer == PrGraphLayerConstraint_First)
                graphSimplexAddEdge(simplex, bottomNode.data[v], first, 0, 0);
            else
                graphSimplexAddEdge(simplex, first, topNode.data[v], 1, 0);
        }
        if (last != kGraphNone) {
            if (layer == PrGraphLayerConstraint_Last)
                graphSimplexAddEdge(simplex, last, topNode.data[v], 0, 0);
            else
                graphSimplexAddEdge(simplex, bottomNode.data[v], last, 1, 0);
        }
    }

    RawArray<int64_t>& values = layered.scratchWide[0];
    if (!interactive) {
        graphSimplexSolve(simplex, 64 * count + 1024);
        values.resize(count);
        for (uint32_t i = 0; i < count; ++i)
            values.data[i] = simplex.values.data[i];
    } else {
        // Takes the layers from the arrangement: leaves whose extents overlap along the flow share a layer.
        values.assignZeroed(count);
        RawArray<uint32_t>& leaves = layered.scratch[4];
        leaves.clear();
        for (uint32_t v = 0; v < n; ++v) {
            if (layered.isGroup.data[v] == 0)
                leaves.push(v);
        }
        auto flowStart = [&](uint32_t v) {
            const GraphScopeNode& node = scope.nodes.data[v];
            float a = flowOf(direction, node.position);
            float b = flowOf(direction, node.position + node.extent);
            return a < b ? a : b;
        };
        sortItems(leaves.span(), [&](uint32_t a, uint32_t b) {
            float fa = flowStart(a);
            float fb = flowStart(b);
            return fa < fb || (fa == fb && a < b);
        });
        int64_t layer = -1;
        float layerEnd = -kPrF32Infinity;
        for (uint32_t i = 0; i < leaves.count; ++i) {
            uint32_t v = leaves.data[i];
            float start = flowStart(v);
            if (layer < 0 || start >= layerEnd) {
                ++layer;
                layerEnd = start + layered.nodeMain.data[v];
            } else if (start + layered.nodeMain.data[v] > layerEnd) {
                layerEnd = start + layered.nodeMain.data[v];
            }
            values.data[topNode.data[v]] = layer;
        }
        // Pushes each node forward as far as its constraints require, in topological order.
        RawArray<uint32_t>& indegree = layered.scratch[5];
        indegree.assignZeroed(count);
        for (uint32_t e = 0; e < simplex.edges.count; ++e)
            ++indegree.data[simplex.edges.data[e].head];
        // By simplex node: its edges out, in their order.
        RawArray<uint32_t> outStart{};
        RawArray<uint32_t> outEdges{};
        outStart.assignZeroed(count + 1);
        for (uint32_t e = 0; e < simplex.edges.count; ++e)
            ++outStart.data[simplex.edges.data[e].tail + 1];
        for (uint32_t i = 0; i < count; ++i)
            outStart.data[i + 1] += outStart.data[i];
        outEdges.resize(simplex.edges.count);
        RawArray<uint32_t> fill{};
        fill.append(outStart.data, count);
        for (uint32_t e = 0; e < simplex.edges.count; ++e)
            outEdges.data[fill.data[simplex.edges.data[e].tail]++] = e;
        fill.reset();
        RawArray<uint32_t>& stack = layered.scratch[4];
        stack.clear();
        for (uint32_t i = 0; i < count; ++i) {
            if (indegree.data[i] == 0)
                stack.push(i);
        }
        while (stack.count > 0) {
            uint32_t v = stack.last();
            stack.pop();
            for (uint32_t i = outStart.data[v]; i < outStart.data[v + 1]; ++i) {
                const GraphSimplexEdge& edge = simplex.edges.data[outEdges.data[i]];
                if (values.data[v] + edge.minLength > values.data[edge.head])
                    values.data[edge.head] = values.data[v] + edge.minLength;
                if (--indegree.data[edge.head] == 0)
                    stack.push(edge.head);
            }
        }
        outStart.reset();
        outEdges.reset();
        // Tightens each group to its children, innermost first. Children come after their parents, so a reverse walk
        // finishes each group's children before the group, and hands each node's range to its parent.
        RawArray<int64_t> least{};
        RawArray<int64_t> most{};
        least.resize(n);
        most.resize(n);
        for (uint32_t v = 0; v < n; ++v) {
            least.data[v] = INT64_MAX;
            most.data[v] = INT64_MIN;
        }
        for (uint32_t v = n; v-- > 0;) {
            if (layered.isGroup.data[v] != 0) {
                values.data[topNode.data[v]] = least.data[v];
                values.data[bottomNode.data[v]] = most.data[v];
            }
            uint32_t parent = scope.nodes.data[v].parent;
            if (parent == kGraphNone)
                continue;
            int64_t a = values.data[topNode.data[v]];
            int64_t b = values.data[bottomNode.data[v]];
            least.data[parent] = a < least.data[parent] ? a : least.data[parent];
            most.data[parent] = b > most.data[parent] ? b : most.data[parent];
        }
        least.reset();
        most.reset();
    }

    // Shifts each component to start at the first layer, so that the components' layers line up. If the top level has
    // layer constraints, they hold across components, so all components shift together.
    uint32_t componentCount = layered.componentKey.count;
    bool together = firstSeparator.data[n] != kGraphNone || lastSeparator.data[n] != kGraphNone;
    RawArray<int64_t>& least = layered.scratchWide[1];
    least.resize(componentCount);
    for (uint32_t c = 0; c < componentCount; ++c)
        least.data[c] = INT64_MAX;
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t c = together ? 0 : layered.component.data[v];
        int64_t value = values.data[topNode.data[v]];
        least.data[c] = value < least.data[c] ? value : least.data[c];
    }
    layered.rankTop.resize(n);
    layered.rankBottom.resize(n);
    uint32_t layerCount = 0;
    for (uint32_t v = 0; v < n; ++v) {
        int64_t from = least.data[together ? 0 : layered.component.data[v]];
        layered.rankTop.data[v] = uint32_t(values.data[topNode.data[v]] - from);
        layered.rankBottom.data[v] = uint32_t(values.data[bottomNode.data[v]] - from);
        if (layered.rankBottom.data[v] + 1 > layerCount)
            layerCount = layered.rankBottom.data[v] + 1;
    }
    layered.layerCount = layerCount;
}

// ---------------------------------------------------------------------------------------------------------------------
// The layered graph

void graphLayeredBuild(GraphLayered& layered) {
    const GraphScope& scope = *layered.scope;
    PrGraphDirection direction = scope.direction;
    uint32_t n = scope.nodes.count;
    layered.vertices.clear();
    layered.segments.clear();
    layered.nodeVertex.resize(n);

    auto addVertex = [&](GraphVertexKind kind, uint32_t layer, uint32_t group, uint32_t node, uint32_t edge, float main,
            float cross) {
        layered.vertices.push(GraphVertex{
            .layer = layer,
            .group = group,
            .node = node,
            .edge = edge,
            .kind = kind,
            .main = main,
            .cross = cross,
        });
        return layered.vertices.count - 1;
    };
    for (uint32_t v = 0; v < n; ++v) {
        if (layered.isGroup.data[v] != 0)
            continue;
        layered.nodeVertex.data[v] = addVertex(GraphVertexKind_Leaf, layered.rankTop.data[v],
            scope.nodes.data[v].parent, v, kGraphNone, layered.nodeMain.data[v], layered.nodeCross.data[v]);
    }
    for (uint32_t v = 0; v < n; ++v) {
        if (layered.isGroup.data[v] == 0)
            continue;
        layered.nodeVertex.data[v] = layered.vertices.count;
        for (uint32_t layer = layered.rankTop.data[v]; layer <= layered.rankBottom.data[v]; ++layer) {
            uint32_t vertex = addVertex(GraphVertexKind_Placeholder, layer, v, v, kGraphNone, 0, 0);
            if (layer > layered.rankTop.data[v]) {
                layered.segments.push(GraphSegment{
                    .upper = vertex - 1,
                    .lower = vertex,
                    .edge = kGraphNone,
                    .upperPort = kGraphNone,
                    .lowerPort = kGraphNone,
                    .weight = 1,
                    .chain = true,
                });
            }
        }
    }

    // Puts each free port on the side that faces the layers its edges reach, weighted by the edges' weights.
    RawArray<float>& toward = layered.scratchFloat[0];
    toward.assignZeroed(scope.ports.count);
    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        const GraphScopeEdge& edge = scope.edges.data[e];
        bool reversed = layered.edges.data[e].reversed;
        if (edge.sourcePort != kGraphNone)
            toward.data[edge.sourcePort] += reversed ? -edge.weight : edge.weight;
        if (edge.targetPort != kGraphNone)
            toward.data[edge.targetPort] += reversed ? edge.weight : -edge.weight;
    }
    for (uint32_t p = 0; p < scope.ports.count; ++p) {
        if (scope.ports.data[p].constraint == PrGraphPortConstraint_Free)
            layered.ports.data[p].side = toward.data[p] >= 0 ? GraphSide_After : GraphSide_Before;
    }

    for (uint32_t e = 0; e < scope.edges.count; ++e) {
        const GraphScopeEdge& edge = scope.edges.data[e];
        GraphLayeredEdge& placed = layered.edges.data[e];
        uint32_t tail = placed.reversed ? edge.target : edge.source;
        uint32_t head = placed.reversed ? edge.source : edge.target;
        placed.tail.port = placed.reversed ? edge.targetPort : edge.sourcePort;
        placed.head.port = placed.reversed ? edge.sourcePort : edge.targetPort;
        placed.tail.lane = kGraphLaneNone;
        placed.head.lane = kGraphLaneNone;
        placed.segmentStart = layered.segments.count;
        placed.segmentCount = 0;
        if (placed.selfLoop) {
            placed.tail.vertex = layered.nodeVertex.data[tail];
            placed.head.vertex = placed.tail.vertex;
            continue;
        }
        uint32_t tailRank = layered.rankBottom.data[tail];
        uint32_t headRank = layered.rankTop.data[head];
        PR_ASSERT(headRank > tailRank);
        // A group's edges leave from its placeholder in its last layer.
        placed.tail.vertex = layered.nodeVertex.data[tail];
        if (layered.isGroup.data[tail] != 0)
            placed.tail.vertex += tailRank - layered.rankTop.data[tail];
        placed.head.vertex = layered.nodeVertex.data[head];
        uint32_t common = graphLayeredCommonGroup(layered, tail, head);
        uint32_t group = common;

        // Stacks the center labels along the main axis, in the layer midway along the edge.
        float labelMain = 0;
        float labelCross = 0;
        uint32_t labelCount = 0;
        for (uint32_t i = 0; i < edge.labelCount; ++i) {
            const GraphScopeLabel& label = scope.labels.data[edge.labelStart + i];
            if (label.placement != PrGraphLabelPlacement_Center)
                continue;
            labelMain += mainOf(direction, label.extent) + (labelCount > 0 ? layered.options.labelSpacing : 0);
            float cross = crossOf(direction, label.extent);
            labelCross = cross > labelCross ? cross : labelCross;
            ++labelCount;
        }
        uint32_t labelLayer = labelCount > 0 ? tailRank + (headRank - tailRank) / 2 : kGraphNone;
        uint32_t previous = placed.tail.vertex;
        for (uint32_t layer = tailRank + 1; layer <= headRank; ++layer) {
            uint32_t vertex = placed.head.vertex;
            if (layer < headRank) {
                // The edge stays in each group of its tail's until it passes the group's last layer, and enters each
                // group of its head's at the group's first layer, so that it crosses their borders at their ends rather
                // than at their sides. A layer that both reach goes to the tail's.
                group = common;
                for (uint32_t g = scope.nodes.data[tail].parent; g != common; g = scope.nodes.data[g].parent) {
                    if (layered.rankBottom.data[g] >= layer) {
                        group = g;
                        break;
                    }
                }
                for (uint32_t g = scope.nodes.data[head].parent; group == common && g != common;
                        g = scope.nodes.data[g].parent) {
                    if (layered.rankTop.data[g] <= layer) {
                        group = g;
                        break;
                    }
                }
                if (layer == labelLayer) {
                    vertex = addVertex(GraphVertexKind_Label, layer, group, kGraphNone, e, labelMain, labelCross);
                    placed.labelVertex = vertex;
                } else {
                    vertex = addVertex(GraphVertexKind_Dummy, layer, group, kGraphNone, e, 0, 0);
                }
            }
            layered.segments.push(GraphSegment{
                .upper = previous,
                .lower = vertex,
                .edge = e,
                .upperPort = layer == tailRank + 1 ? placed.tail.port : kGraphNone,
                .lowerPort = layer == headRank ? placed.head.port : kGraphNone,
                .weight = edge.weight,
                .chain = false,
            });
            ++placed.segmentCount;
            previous = vertex;
        }
    }

    uint32_t layerCount = layered.layerCount;
    layered.gapSegmentStart.assignZeroed(layerCount + 1);
    for (uint32_t s = 0; s < layered.segments.count; ++s)
        ++layered.gapSegmentStart.data[layered.vertices.data[layered.segments.data[s].upper].layer + 1];
    for (uint32_t layer = 0; layer < layerCount; ++layer)
        layered.gapSegmentStart.data[layer + 1] += layered.gapSegmentStart.data[layer];
    layered.gapSegments.resize(layered.segments.count);
    RawArray<uint32_t>& fill = layered.scratch[0];
    fill.clear();
    fill.append(layered.gapSegmentStart.data, layerCount);
    for (uint32_t s = 0; s < layered.segments.count; ++s)
        layered.gapSegments.data[fill.data[layered.vertices.data[layered.segments.data[s].upper].layer]++] = s;
}

} // namespace pr_impl
