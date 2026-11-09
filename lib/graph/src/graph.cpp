// Graphs (graph.h): the description and the layout driver. The driver splits the graph into scopes: one for the top
// level, and one for each group whose direction differs from its parent's. It lays out the innermost scopes first, so
// that each such group enters its parent's scope as a single node with its laid-out extent.
//
// An edge that crosses a scope's border is split into pieces. Inside the scope, its piece ends at a proxy node on the
// first or last layer. Outside, its piece ends at a port on the group's side, at the proxy's position. The edge's route
// is the routes of its pieces, joined. A center label goes with the piece in the scope that holds both ends, a tail
// label with the piece at the source, and a head label with the piece at the target.
//
// The driver's cost is linear in the size of the graph and of its layouts, plus the nesting depth of scopes and of
// groups for each edge.

#include "pr/graph.h"

#include "pr/alloc.h"
#include "pr/assert.h"
#include "pr/raw_array.h"
#include "pr/sort.h"

#include "graph/src/graph_layered.h"

namespace pr_impl {

struct Graph {
    RawArray<PrGraphNodeDesc> nodes;
    RawArray<PrGraphPortDesc> ports;
    RawArray<PrGraphEdgeDesc> edges;
    RawArray<PrGraphLabelDesc> labels;

    bool hasLayout;
    PrRect bounds;
    RawArray<PrRect> nodeRects;         // by node
    RawArray<PrVec2> portPositions;     // by port
    RawArray<PrRect> labelRects;        // by label
    RawArray<uint32_t> routeStart;      // by edge: where its route starts in `routePoints`, with the total last
    RawArray<PrVec2> routePoints;
};

namespace {

// The role of an edge's piece in one scope.
enum {
    PieceRole_Whole,                // the scope holds both of the edge's ends, or the groups that hold them
    PieceRole_Exit,                 // the edge leaves the scope, from its source to a proxy on the last layer
    PieceRole_Entry,                // the edge enters the scope, from a proxy on the first layer to its target
};
typedef uint8_t PieceRole;

// The part of an edge in one scope.
struct Piece {
    uint32_t edge;
    uint32_t scope;
    uint32_t localEdge;             // the edge's index in the scope
    uint32_t proxy;                 // the proxy's local node, for an exit or entry
    PieceRole role;

    // Where an exit or entry crosses its scope's border: the side of the scope's box, and the distance along it.
    PrRectSide side;
    float offset;
};

struct ScopeInfo {
    uint32_t group;                 // kGraphNone for the top level
    uint32_t parent;                // the enclosing scope, or kGraphNone for the top level
    uint32_t depth;
    PrGraphDirection direction;
    PrVec2 origin;                  // the box's top left corner in the result
};

void clearLayout(Graph& graph) {
    graph.hasLayout = false;
    graph.nodeRects.clear();
    graph.portPositions.clear();
    graph.labelRects.clear();
    graph.routeStart.clear();
    graph.routePoints.clear();
}

// Lists items by a key, in the order of the items: `start` by key holds where each key's items start in `items`, with
// the total last.
struct Index {
    RawArray<uint32_t> start;
    RawArray<uint32_t> items;
};

// Fills the index with `count` items under `keyCount` keys. `keyOf` gives an item's key, or kGraphNone to leave the
// item out.
template <typename KeyFn>
void indexBuild(Index& index, uint32_t keyCount, uint32_t count, KeyFn keyOf) {
    index.start.assignZeroed(keyCount + 1);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key = keyOf(i);
        if (key != kGraphNone)
            ++index.start.data[key + 1];
    }
    for (uint32_t k = 0; k < keyCount; ++k)
        index.start.data[k + 1] += index.start.data[k];
    index.items.resize(index.start.data[keyCount]);
    RawArray<uint32_t> fill{};
    fill.append(index.start.data, keyCount);
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t key = keyOf(i);
        if (key != kGraphNone)
            index.items.data[fill.data[key]++] = i;
    }
    fill.reset();
}

void indexReset(Index& index) {
    index.start.reset();
    index.items.reset();
}

// The layout's working state.
struct Driver {
    const Graph* graph;
    PrGraphLayoutOptions options;
    RawArray<uint8_t> isGroup;            // by node
    RawArray<PrGraphDirection> direction; // by node
    RawArray<uint32_t> nodeScope;         // by node: the scope that lays it out
    RawArray<uint32_t> groupScope;        // by node: the scope of a group laid out separately, or kGraphNone
    RawArray<uint32_t> local;             // by node: its index in its scope
    RawArray<uint32_t> portLocal;         // by port: its index in its scope
    RawArray<ScopeInfo> scopes;
    RawArray<GraphScope> problems;        // by scope

    Index scopeNodes;                     // the nodes by the scopes that lay them out
    Index scopePorts;                     // the ports by the scopes of their nodes
    Index edgeLabels;                     // the labels by their edges

    // By edge: where its pieces start in `pieces`, with the total last. An edge's pieces run from its source to its
    // target: the exits from the source's scope outward, the whole piece, then the entries inward. An edge that the
    // layout ignores has none.
    RawArray<uint32_t> pieceStart;
    RawArray<Piece> pieces;
    Index scopePieces;                    // the pieces by their scopes, in the order of their edges
};

uint32_t commonScope(const Driver& driver, uint32_t a, uint32_t b) {
    while (a != b) {
        if (driver.scopes.data[a].depth >= driver.scopes.data[b].depth)
            a = driver.scopes.data[a].parent;
        else
            b = driver.scopes.data[b].parent;
    }
    return a;
}

// Returns true if `ancestor` holds `node`, at any depth.
bool holds(const Graph& graph, uint32_t ancestor, uint32_t node) {
    for (uint32_t a = graph.nodes.data[node].parent; a != kGraphNone; a = graph.nodes.data[a].parent) {
        if (a == ancestor)
            return true;
    }
    return false;
}

// Returns true if the label goes with the piece at `index` of its edge's pieces. A center label needs the layer midway
// between the edge's ends, which only the whole piece has. A tail or head label goes beside the edge's end.
bool carriesLabel(const Driver& driver, uint32_t edge, uint32_t index, const PrGraphLabelDesc& label) {
    switch (label.placement) {
        case PrGraphLabelPlacement_Tail:
            return index == driver.pieceStart.data[edge];
        case PrGraphLabelPlacement_Head:
            return index + 1 == driver.pieceStart.data[edge + 1];
        default:
            return driver.pieces.data[index].role == PieceRole_Whole;
    }
}

// The side of a scope's box that faces later layers, where its edges leave.
PrRectSide afterSide(PrGraphDirection direction) {
    switch (direction) {
        case PrGraphDirection_Up:
            return PrRectSide_Top;
        case PrGraphDirection_Right:
            return PrRectSide_Right;
        case PrGraphDirection_Left:
            return PrRectSide_Left;
        default:
            return PrRectSide_Bottom;
    }
}

PrRectSide beforeSide(PrGraphDirection direction) {
    switch (direction) {
        case PrGraphDirection_Up:
            return PrRectSide_Bottom;
        case PrGraphDirection_Right:
            return PrRectSide_Left;
        case PrGraphDirection_Left:
            return PrRectSide_Right;
        default:
            return PrRectSide_Top;
    }
}

// The position of a port on `side` at `offset`, on a box of `extent`.
PrVec2 sidePoint(PrVec2 extent, PrRectSide side, float offset) {
    switch (side) {
        case PrRectSide_Top:
            return PrVec2{offset, 0};
        case PrRectSide_Bottom:
            return PrVec2{offset, extent.y};
        case PrRectSide_Left:
            return PrVec2{0, offset};
        default:
            return PrVec2{extent.x, offset};
    }
}

// Finds the node and port that stand for an edge's end in `scope`. An end in an inner scope maps to the group of that
// scope, at a new port where the edge's piece in that scope crosses the group's border. That piece is at `crossing` in
// `pieces`, next to the edge's piece in `scope`.
void endIn(Driver& driver, GraphScope& problem, uint32_t scope, uint32_t node, uint32_t port, uint32_t crossing,
        uint32_t& outNode, uint32_t& outPort) {
    if (driver.nodeScope.data[node] == scope) {
        outNode = driver.local.data[node];
        outPort = port == kGraphNone ? kGraphNone : driver.portLocal.data[port];
        return;
    }
    const Piece& inner = driver.pieces.data[crossing];
    outNode = driver.local.data[driver.scopes.data[inner.scope].group];
    outPort = problem.ports.count;
    problem.ports.push(GraphScopePort{
        .node = outNode,
        .constraint = PrGraphPortConstraint_FixedPosition,
        .side = inner.side,
        .offset = inner.offset,
    });
}

// Builds and lays out one scope, after every scope inside it.
void layoutScope(Driver& driver, uint32_t scope) {
    const Graph& graph = *driver.graph;
    const ScopeInfo& info = driver.scopes.data[scope];
    GraphScope& problem = driver.problems.data[scope];
    graphScopeClear(problem);
    problem.direction = info.direction;
    PrVec2 groupPosition{0, 0};
    if (info.group != kGraphNone) {
        const PrGraphNodeDesc& group = graph.nodes.data[info.group];
        for (uint32_t s = 0; s < PrRectSide_Count; ++s)
            problem.padding[s] = group.padding[s];
        problem.minExtent = group.extent;
        groupPosition = group.position;
    }

    for (uint32_t i = driver.scopeNodes.start.data[scope]; i < driver.scopeNodes.start.data[scope + 1]; ++i) {
        uint32_t v = driver.scopeNodes.items.data[i];
        const PrGraphNodeDesc& desc = graph.nodes.data[v];
        driver.local.data[v] = problem.nodes.count;
        GraphScopeNode node{
            .extent = desc.extent,
            .position = desc.position,
            .parent = kGraphNone,
            .layer = desc.layer,
            .isProxy = false,
        };
        if (desc.parent != kGraphNone && driver.nodeScope.data[desc.parent] == scope)
            node.parent = driver.local.data[desc.parent];
        uint32_t inner = driver.groupScope.data[v];
        if (inner != kGraphNone) {
            node.extent = driver.problems.data[inner].extent;
        } else {
            for (uint32_t s = 0; s < PrRectSide_Count; ++s)
                node.padding[s] = desc.padding[s];
        }
        problem.nodes.push(node);
    }
    for (uint32_t i = driver.scopePorts.start.data[scope]; i < driver.scopePorts.start.data[scope + 1]; ++i) {
        uint32_t p = driver.scopePorts.items.data[i];
        const PrGraphPortDesc& desc = graph.ports.data[p];
        driver.portLocal.data[p] = problem.ports.count;
        problem.ports.push(GraphScopePort{
            .node = driver.local.data[desc.node],
            .constraint = desc.constraint,
            .side = desc.side,
            .offset = desc.offset,
        });
    }

    for (uint32_t i = driver.scopePieces.start.data[scope]; i < driver.scopePieces.start.data[scope + 1]; ++i) {
        uint32_t index = driver.scopePieces.items.data[i];
        Piece& piece = driver.pieces.data[index];
        uint32_t e = piece.edge;
        const PrGraphEdgeDesc& desc = graph.edges.data[e];
        GraphScopeEdge edge{
            .weight = desc.weight,
            .minLength = piece.role == PieceRole_Whole ? desc.minLength : 1,
            .labelStart = problem.labels.count,
            .labelCount = 0,
        };
        piece.proxy = kGraphNone;
        if (piece.role != PieceRole_Whole) {
            PrGraphLayerConstraint layer = PrGraphLayerConstraint_First;
            if (piece.role == PieceRole_Exit)
                layer = PrGraphLayerConstraint_Last;
            piece.proxy = problem.nodes.count;
            problem.nodes.push(GraphScopeNode{
                .extent = PrVec2{0, 0},
                .position = groupPosition,
                .parent = kGraphNone,
                .layer = layer,
                .isProxy = true,
            });
        }
        // The pieces before and after this one, in inner scopes, give the ports where the edge crosses their borders.
        if (piece.role == PieceRole_Entry) {
            edge.source = piece.proxy;
            edge.sourcePort = kGraphNone;
        } else {
            endIn(driver, problem, scope, desc.source, desc.sourcePort, index - 1, edge.source, edge.sourcePort);
        }
        if (piece.role == PieceRole_Exit) {
            edge.target = piece.proxy;
            edge.targetPort = kGraphNone;
        } else {
            endIn(driver, problem, scope, desc.target, desc.targetPort, index + 1, edge.target, edge.targetPort);
        }
        for (uint32_t k = driver.edgeLabels.start.data[e]; k < driver.edgeLabels.start.data[e + 1]; ++k) {
            const PrGraphLabelDesc& label = graph.labels.data[driver.edgeLabels.items.data[k]];
            if (!carriesLabel(driver, e, index, label))
                continue;
            problem.labels.push(GraphScopeLabel{.extent = label.extent, .placement = label.placement});
            ++edge.labelCount;
        }
        piece.localEdge = problem.edges.count;
        problem.edges.push(edge);
    }

    graphScopeLayout(problem, driver.options);

    // Records where each exit and entry crosses the group's border.
    for (uint32_t i = driver.scopePieces.start.data[scope]; i < driver.scopePieces.start.data[scope + 1]; ++i) {
        Piece& piece = driver.pieces.data[driver.scopePieces.items.data[i]];
        if (piece.role == PieceRole_Whole)
            continue;
        PrRect at = problem.nodeRects.data[piece.proxy];
        piece.side = piece.role == PieceRole_Exit ? afterSide(info.direction) : beforeSide(info.direction);
        piece.offset = piece.side == PrRectSide_Top || piece.side == PrRectSide_Bottom ? at.left : at.top;
    }
}

// Appends a piece's route to `out`, in the frame of the whole graph. An exit or entry runs on from its proxy to its
// scope's border, along a straight segment or a straight cubic. If `joined` is set, the route leaves out its first
// point, where the previous piece ends.
void appendPiece(const Driver& driver, const Piece& piece, bool joined, RawArray<PrVec2>& out) {
    const GraphScope& problem = driver.problems.data[piece.scope];
    PrVec2 origin = driver.scopes.data[piece.scope].origin;
    uint32_t start = problem.routeStart.data[piece.localEdge];
    uint32_t end = problem.routeStart.data[piece.localEdge + 1];
    bool skip = joined;
    auto push = [&](PrVec2 local) {
        if (skip)
            skip = false;
        else
            out.push(local + origin);
    };
    if (piece.role == PieceRole_Whole || end == start) {
        for (uint32_t i = start; i < end; ++i)
            push(problem.routePoints.data[i]);
        return;
    }
    PrVec2 border = sidePoint(problem.extent, piece.side, piece.offset);
    bool exit = piece.role == PieceRole_Exit;
    PrVec2 inner = exit ? problem.routePoints.data[end - 1] : problem.routePoints.data[start];
    PrVec2 from = exit ? inner : border;
    PrVec2 to = exit ? border : inner;
    PrVec2 step = (to - from) * (1.0f / 3.0f);
    bool spline = driver.options.routing == PrGraphRouting_Spline;
    if (!exit) {
        push(border);
        if (spline) {
            push(from + step);
            push(from + step * 2.0f);
        }
    }
    for (uint32_t i = start; i < end; ++i)
        push(problem.routePoints.data[i]);
    if (exit) {
        if (spline) {
            push(from + step);
            push(from + step * 2.0f);
        }
        push(border);
    }
}

PrRect offsetRect(PrRect rect, PrVec2 by) {
    return PrRect{rect.left + by.x, rect.top + by.y, rect.right + by.x, rect.bottom + by.y};
}

void driverReset(Driver& driver) {
    driver.isGroup.reset();
    driver.direction.reset();
    driver.nodeScope.reset();
    driver.groupScope.reset();
    driver.local.reset();
    driver.portLocal.reset();
    for (uint32_t s = 0; s < driver.problems.count; ++s)
        graphScopeReset(driver.problems.data[s]);
    driver.problems.reset();
    driver.scopes.reset();
    indexReset(driver.scopeNodes);
    indexReset(driver.scopePorts);
    indexReset(driver.edgeLabels);
    driver.pieceStart.reset();
    driver.pieces.reset();
    indexReset(driver.scopePieces);
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrGraph : Graph {};

PrGraphNodeDesc prGraphNodeDescDefault(void) {
    return PrGraphNodeDesc{
        .parent = kPrGraphNodeNone,
        .layer = PrGraphLayerConstraint_None,
        .direction = PrGraphDirection_Inherit,
    };
}

PrGraphEdgeDesc prGraphEdgeDescDefault(PrGraphNode source, PrGraphNode target) {
    return PrGraphEdgeDesc{
        .source = source,
        .target = target,
        .sourcePort = kPrGraphPortNone,
        .targetPort = kPrGraphPortNone,
        .weight = 1,
        .minLength = 1,
    };
}

PrGraph* prGraphCreate(void) {
    return allocZeroed<PrGraph>();
}

void prGraphDestroy(PrGraph* PR_NULLABLE graph) {
    if (graph == nullptr)
        return;
    graph->nodes.reset();
    graph->ports.reset();
    graph->edges.reset();
    graph->labels.reset();
    graph->nodeRects.reset();
    graph->portPositions.reset();
    graph->labelRects.reset();
    graph->routeStart.reset();
    graph->routePoints.reset();
    prMemoryFree(graph);
}

void prGraphClear(PrGraph* graph) {
    graph->nodes.clear();
    graph->ports.clear();
    graph->edges.clear();
    graph->labels.clear();
    clearLayout(*graph);
}

PrGraphNode prGraphAddNode(PrGraph* graph, const PrGraphNodeDesc* desc) {
    PR_ASSERT(desc->parent == kPrGraphNodeNone || desc->parent < graph->nodes.count);
    clearLayout(*graph);
    graph->nodes.push(*desc);
    return graph->nodes.count - 1;
}

PrGraphPort prGraphAddPort(PrGraph* graph, const PrGraphPortDesc* desc) {
    PR_ASSERT(desc->node < graph->nodes.count);
    clearLayout(*graph);
    graph->ports.push(*desc);
    return graph->ports.count - 1;
}

PrGraphEdge prGraphAddEdge(PrGraph* graph, const PrGraphEdgeDesc* desc) {
    PR_ASSERT(desc->source < graph->nodes.count && desc->target < graph->nodes.count);
    PR_ASSERT(desc->sourcePort == kPrGraphPortNone ||
        (desc->sourcePort < graph->ports.count && graph->ports.data[desc->sourcePort].node == desc->source));
    PR_ASSERT(desc->targetPort == kPrGraphPortNone ||
        (desc->targetPort < graph->ports.count && graph->ports.data[desc->targetPort].node == desc->target));
    PR_ASSERT(desc->weight > 0 && desc->minLength >= 1);
    clearLayout(*graph);
    graph->edges.push(*desc);
    return graph->edges.count - 1;
}

PrGraphLabel prGraphAddLabel(PrGraph* graph, const PrGraphLabelDesc* desc) {
    PR_ASSERT(desc->edge < graph->edges.count);
    clearLayout(*graph);
    graph->labels.push(*desc);
    return graph->labels.count - 1;
}

PrGraphLayoutOptions prGraphLayoutOptionsDefault(void) {
    return PrGraphLayoutOptions{
        .direction = PrGraphDirection_Down,
        .routing = PrGraphRouting_Spline,
        .placement = PrGraphPlacement_Balanced,
        .mode = PrGraphLayoutMode_Fresh,
        .layerSpacing = 50,
        .nodeSpacing = 50,
        .edgeSpacing = 10,
        .labelSpacing = 4,
        .componentSpacing = 50,
    };
}

void prGraphLayoutCompute(PrGraph* graph, const PrGraphLayoutOptions* PR_NULLABLE options) {
    Driver driver{.graph = graph, .options = options != nullptr ? *options : prGraphLayoutOptionsDefault()};
    PR_ASSERT(driver.options.direction != PrGraphDirection_Inherit);
    uint32_t n = graph->nodes.count;
    uint32_t edgeCount = graph->edges.count;

    driver.isGroup.assignZeroed(n);
    for (uint32_t v = 0; v < n; ++v) {
        if (graph->nodes.data[v].parent != kGraphNone)
            driver.isGroup.data[graph->nodes.data[v].parent] = 1;
    }
    for (uint32_t p = 0; p < graph->ports.count; ++p)
        PR_ASSERT(driver.isGroup.data[graph->ports.data[p].node] == 0);

    // Creates the scopes: the top level, then each group whose direction differs from its parent's, outermost first.
    driver.direction.resize(n);
    driver.nodeScope.resize(n);
    driver.groupScope.resize(n);
    driver.local.resize(n);
    driver.portLocal.resize(graph->ports.count);
    driver.scopes.push(ScopeInfo{
        .group = kGraphNone,
        .parent = kGraphNone,
        .depth = 0,
        .direction = driver.options.direction,
    });
    for (uint32_t v = 0; v < n; ++v) {
        const PrGraphNodeDesc& desc = graph->nodes.data[v];
        uint32_t parent = desc.parent;
        PrGraphDirection outer = parent == kGraphNone ? driver.options.direction : driver.direction.data[parent];
        driver.direction.data[v] = desc.direction == PrGraphDirection_Inherit ? outer : desc.direction;
        driver.nodeScope.data[v] = 0;
        if (parent != kGraphNone && driver.groupScope.data[parent] != kGraphNone)
            driver.nodeScope.data[v] = driver.groupScope.data[parent];
        else if (parent != kGraphNone)
            driver.nodeScope.data[v] = driver.nodeScope.data[parent];
        driver.groupScope.data[v] = kGraphNone;
        if (driver.isGroup.data[v] != 0 && driver.direction.data[v] != outer) {
            driver.groupScope.data[v] = driver.scopes.count;
            uint32_t enclosing = driver.nodeScope.data[v];
            driver.scopes.push(ScopeInfo{
                .group = v,
                .parent = enclosing,
                .depth = driver.scopes.data[enclosing].depth + 1,
                .direction = driver.direction.data[v],
            });
        }
    }
    uint32_t scopeCount = driver.scopes.count;
    indexBuild(driver.scopeNodes, scopeCount, n, [&](uint32_t v) { return driver.nodeScope.data[v]; });
    indexBuild(driver.scopePorts, scopeCount, graph->ports.count,
        [&](uint32_t p) { return driver.nodeScope.data[graph->ports.data[p].node]; });
    indexBuild(driver.edgeLabels, edgeCount, graph->labels.count, [&](uint32_t l) { return graph->labels.data[l].edge; });

    // Splits each edge into its pieces. An edge between a group and one of its descendants breaks the precondition of
    // prGraphLayoutCompute, and has no direction to flow in. The layout ignores it.
    driver.pieceStart.resize(edgeCount + 1);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const PrGraphEdgeDesc& edge = graph->edges.data[e];
        driver.pieceStart.data[e] = driver.pieces.count;
        bool nested = holds(*graph, edge.target, edge.source) || holds(*graph, edge.source, edge.target);
        PR_ASSERT(!nested);
        if (nested)
            continue;
        uint32_t sourceScope = driver.nodeScope.data[edge.source];
        uint32_t targetScope = driver.nodeScope.data[edge.target];
        uint32_t whole = commonScope(driver, sourceScope, targetScope);
        for (uint32_t s = sourceScope; s != whole; s = driver.scopes.data[s].parent)
            driver.pieces.push(Piece{.edge = e, .scope = s, .role = PieceRole_Exit});
        driver.pieces.push(Piece{.edge = e, .scope = whole, .role = PieceRole_Whole});
        uint32_t entries = driver.pieces.count;
        for (uint32_t s = targetScope; s != whole; s = driver.scopes.data[s].parent)
            driver.pieces.push(Piece{.edge = e, .scope = s, .role = PieceRole_Entry});
        reverseItems(Span<Piece>(driver.pieces.data + entries, driver.pieces.count - entries));
    }
    driver.pieceStart.data[edgeCount] = driver.pieces.count;
    indexBuild(driver.scopePieces, scopeCount, driver.pieces.count,
        [&](uint32_t i) { return driver.pieces.data[i].scope; });

    driver.problems.assignZeroed(scopeCount);
    // Scopes were created outermost first, so laying them out in reverse order handles inner scopes first.
    for (uint32_t s = scopeCount; s-- > 0;)
        layoutScope(driver, s);
    for (uint32_t s = 1; s < scopeCount; ++s) {
        ScopeInfo& info = driver.scopes.data[s];
        const ScopeInfo& parent = driver.scopes.data[info.parent];
        PrRect rect = driver.problems.data[info.parent].nodeRects.data[driver.local.data[info.group]];
        info.origin = PrVec2{parent.origin.x + rect.left, parent.origin.y + rect.top};
    }

    // Maps each scope's results into the frame of the whole graph.
    clearLayout(*graph);
    graph->nodeRects.resize(n);
    for (uint32_t v = 0; v < n; ++v) {
        uint32_t scope = driver.nodeScope.data[v];
        PrRect rect = driver.problems.data[scope].nodeRects.data[driver.local.data[v]];
        graph->nodeRects.data[v] = offsetRect(rect, driver.scopes.data[scope].origin);
    }
    graph->portPositions.resize(graph->ports.count);
    for (uint32_t p = 0; p < graph->ports.count; ++p) {
        uint32_t scope = driver.nodeScope.data[graph->ports.data[p].node];
        graph->portPositions.data[p] =
            driver.problems.data[scope].portPositions.data[driver.portLocal.data[p]] + driver.scopes.data[scope].origin;
    }
    graph->labelRects.assignZeroed(graph->labels.count);
    graph->routeStart.resize(edgeCount + 1);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        graph->routeStart.data[e] = graph->routePoints.count;
        for (uint32_t i = driver.pieceStart.data[e]; i < driver.pieceStart.data[e + 1]; ++i) {
            const Piece& piece = driver.pieces.data[i];
            appendPiece(driver, piece, i > driver.pieceStart.data[e], graph->routePoints);
            const GraphScope& problem = driver.problems.data[piece.scope];
            uint32_t labelAt = problem.edges.data[piece.localEdge].labelStart;
            for (uint32_t k = driver.edgeLabels.start.data[e]; k < driver.edgeLabels.start.data[e + 1]; ++k) {
                uint32_t l = driver.edgeLabels.items.data[k];
                if (carriesLabel(driver, e, i, graph->labels.data[l]))
                    graph->labelRects.data[l] =
                        offsetRect(problem.labelRects.data[labelAt++], driver.scopes.data[piece.scope].origin);
            }
        }
    }
    graph->routeStart.data[edgeCount] = graph->routePoints.count;
    PrVec2 extent = driver.problems.data[0].extent;
    graph->bounds = PrRect{0, 0, extent.x, extent.y};
    graph->hasLayout = true;
    driverReset(driver);
}

bool prGraphHasLayout(const PrGraph* graph) {
    return graph->hasLayout;
}

PrRect prGraphBounds(const PrGraph* graph) {
    PR_ASSERT(graph->hasLayout);
    return graph->bounds;
}

PrRect prGraphNodeRect(const PrGraph* graph, PrGraphNode node) {
    PR_ASSERT(graph->hasLayout && node < graph->nodeRects.count);
    return graph->nodeRects.data[node];
}

PrVec2 prGraphPortPosition(const PrGraph* graph, PrGraphPort port) {
    PR_ASSERT(graph->hasLayout && port < graph->portPositions.count);
    return graph->portPositions.data[port];
}

PrRect prGraphLabelRect(const PrGraph* graph, PrGraphLabel label) {
    PR_ASSERT(graph->hasLayout && label < graph->labelRects.count);
    return graph->labelRects.data[label];
}

PrGraphRoute prGraphEdgeRoute(const PrGraph* graph, PrGraphEdge edge) {
    PR_ASSERT(graph->hasLayout && edge < graph->edges.count);
    uint32_t start = graph->routeStart.data[edge];
    return PrGraphRoute{
        .pointCount = graph->routeStart.data[edge + 1] - start,
        .points = graph->routePoints.data + start,
    };
}
