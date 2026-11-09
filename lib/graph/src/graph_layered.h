// Layered layout of one scope. A scope is the set of nodes that are laid out together in one direction, including the
// groups among them that share that direction. The driver (graph.cpp) builds a scope for the top level and one for each
// group whose direction differs from its parent's. Such a group appears in its parent's scope as a single node.
//
// The phases work in an abstract frame. The main axis runs from layer to layer, and the cross axis runs along each
// layer. At the end, the results are mapped to the scope's direction, with the scope's box at the origin.

#pragma once

#include "pr/geom.h"
#include "pr/graph.h"
#include "pr/raw_array.h"

#include "graph/src/graph_simplex.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The absent index of a node, port, vertex or group.
constexpr uint32_t kGraphNone = 0xffffffffu;

// ---------------------------------------------------------------------------------------------------------------------
// Scopes

// A node of a scope, in the result's frame.
struct GraphScopeNode {
    PrVec2 extent;                      // for a group, its minimum extent
    PrVec2 position;                    // the top left corner in the caller's arrangement
    uint32_t parent;                    // a group in the same scope, or kGraphNone
    float padding[PrRectSide_Count];    // for a group, by PrRectSide
    PrGraphLayerConstraint layer;
    bool isProxy;                       // stands for the outside end of an edge that crosses the scope's border
};

// A port on a scope's node, with the same meaning as PrGraphPortDesc.
struct GraphScopePort {
    uint32_t node;
    PrGraphPortConstraint constraint;
    PrRectSide side;
    float offset;
};

// An edge between nodes of a scope. For an edge that crosses the scope's border, the part that lies in the scope.
struct GraphScopeEdge {
    uint32_t source;
    uint32_t target;
    uint32_t sourcePort;                // kGraphNone, or a port of `source`
    uint32_t targetPort;                // kGraphNone, or a port of `target`
    float weight;
    uint32_t minLength;
    uint32_t labelStart;                // the first of the edge's labels in the scope's `labels`
    uint32_t labelCount;
};

// A label on a scope's edge.
struct GraphScopeLabel {
    PrVec2 extent;
    PrGraphLabelPlacement placement;
};

// A scope's input and its results. Nodes come after their parents. The results are in the result's frame, with the
// scope's box, including its padding, at the origin.
struct GraphScope {
    PrGraphDirection direction;
    float padding[PrRectSide_Count];
    PrVec2 minExtent;
    RawArray<GraphScopeNode> nodes;
    RawArray<GraphScopePort> ports;
    RawArray<GraphScopeEdge> edges;
    RawArray<GraphScopeLabel> labels;

    PrVec2 extent;
    RawArray<PrRect> nodeRects;         // by node
    RawArray<PrVec2> portPositions;     // by port
    RawArray<PrRect> labelRects;        // by label
    RawArray<uint32_t> routeStart;      // by edge: where its route starts in `routePoints`, with the total last
    RawArray<PrVec2> routePoints;
};

// Empties the scope, keeping its storage, or frees the storage.
void graphScopeClear(GraphScope& scope);
void graphScopeReset(GraphScope& scope);

// Lays out the scope and writes its results.
void graphScopeLayout(GraphScope& scope, const PrGraphLayoutOptions& options);

// ---------------------------------------------------------------------------------------------------------------------
// The layered pipeline over one scope

// The sides of a node in the abstract frame: toward earlier layers, toward later layers, and the two cross sides.
enum {
    GraphSide_Before,
    GraphSide_After,
    GraphSide_CrossStart,
    GraphSide_CrossEnd,
    GraphSide_Count,
};
typedef uint8_t GraphSide;

// What a vertex of the layered graph stands for.
enum {
    GraphVertexKind_Leaf,           // a node without children
    GraphVertexKind_Placeholder,    // a group, in one of the layers that it spans
    GraphVertexKind_Dummy,          // an edge, in a layer that it passes through
    GraphVertexKind_Label,          // an edge's center labels, in the layer midway along the edge
};
typedef uint8_t GraphVertexKind;

// A vertex of the layered graph, in one layer.
struct GraphVertex {
    uint32_t layer;
    uint32_t order;                 // the position within the layer
    uint32_t group;                 // the innermost group that holds the vertex, or kGraphNone
    uint32_t node;                  // for a leaf, its node; for a placeholder, its group
    uint32_t edge;                  // for a dummy or a label, its edge
    GraphVertexKind kind;

    // The extent along the main and cross axes, and the width of the lanes reserved beside it on each cross side. A
    // placeholder reserves none: its group's lanes lie outside the group's border.
    float main;
    float cross;
    float reserveStart;
    float reserveEnd;

    float x;                        // the center on the cross axis
    float mainStart;
};

// A link between vertices in adjacent layers. It is part of an edge, or, if `chain` is set, part of a group's chain of
// placeholders. Only ordering reads chain segments.
struct GraphSegment {
    uint32_t upper;
    uint32_t lower;
    uint32_t edge;
    uint32_t upperPort;             // kGraphNone, or the port at the upper end
    uint32_t lowerPort;             // kGraphNone, or the port at the lower end
    float weight;
    bool chain;
};

// The `lane` of an end that meets its vertex directly, without running along a lane.
constexpr GraphSide kGraphLaneNone = 0xff;

// One end of an edge in the layered graph. The tail is the end in the earlier layer.
struct GraphEnd {
    uint32_t vertex;
    uint32_t port;                  // kGraphNone, or a port of the vertex's node
    GraphSide side;                 // the side where the end meets its vertex
    GraphSide lane;                 // kGraphLaneNone, or the cross side whose lane the end runs in
    uint16_t laneIndex;
};

// An edge in the layered graph: its ends, and its chain of segments from tail to head.
struct GraphLayeredEdge {
    bool reversed;
    bool selfLoop;
    GraphEnd tail;
    GraphEnd head;
    uint32_t segmentStart;
    uint32_t segmentCount;
    uint32_t labelVertex;           // kGraphNone, or the vertex of the center labels
};

// A port's place on its node, in the abstract frame.
struct GraphLayeredPort {
    GraphSide side;
    float offset;                   // along the side: across for Before and After, along the main axis otherwise
    float key;                      // the fraction along the side, which ordering compares
};

// The pipeline's state for one scope.
struct GraphLayered {
    const GraphScope* PR_NULLABLE scope;
    PrGraphLayoutOptions options;

    // By node.
    RawArray<float> nodeMain;
    RawArray<float> nodeCross;
    RawArray<float> padding;            // GraphSide_Count by node
    RawArray<uint8_t> isGroup;
    RawArray<uint32_t> depth;
    RawArray<uint32_t> unitPosition;    // the position among the node's siblings, which orients edges
    RawArray<uint32_t> component;       // the connected component, joined by edges and by group membership
    RawArray<uint32_t> rankTop;
    RawArray<uint32_t> rankBottom;
    RawArray<uint32_t> nodeVertex;      // a leaf's vertex, or a group's first placeholder
    RawArray<float> groupLeft;
    RawArray<float> groupRight;
    RawArray<float> groupMainStart;
    RawArray<float> groupMainEnd;

    // By node: the width of the lanes reserved outside a group's border on each cross side, where the group's
    // self-loops run.
    RawArray<float> groupReserveStart;
    RawArray<float> groupReserveEnd;

    RawArray<float> componentKey;       // by component: its position in the cross-axis order of components

    RawArray<GraphLayeredPort> ports;   // by port
    RawArray<GraphLayeredEdge> edges;   // by edge
    RawArray<GraphVertex> vertices;
    RawArray<GraphSegment> segments;

    // By layer: where the segments whose upper ends lie in it start in `gapSegments`, with the total last. A segment
    // joins adjacent layers, so these are the segments that cross the gap after the layer, in the order of `segments`.
    RawArray<uint32_t> gapSegmentStart;
    RawArray<uint32_t> gapSegments;

    // By vertex: where its segments start in `incident`, with the total last. These are the segments toward both
    // adjacent layers, without the groups' chains, in the order of `segments`. Ordering builds them.
    RawArray<uint32_t> incidentStart;
    RawArray<uint32_t> incident;

    uint32_t layerCount;
    RawArray<uint32_t> layerStart;      // by layer: where its vertices start in `layerVertices`, with the total last
    RawArray<uint32_t> layerVertices;
    RawArray<float> bandStart;          // by layer
    RawArray<float> bandExtent;         // by layer

    // By layer, for the gap after it: the part of the gap that no group's padding reaches, where routes may change
    // their cross-axis positions, and the number of tracks of orthogonal routes in that part.
    RawArray<float> gapStart;
    RawArray<float> gapEnd;
    RawArray<uint32_t> trackCount;
    RawArray<uint32_t> segmentTrack;    // by segment: the track of an orthogonal route's step across its gap

    GraphSimplex simplex;

    // Scratch storage shared by the phases.
    RawArray<uint32_t> scratch[6];
    RawArray<float> scratchFloat[3];
    RawArray<int64_t> scratchWide[3];
    RawArray<uint32_t> bestOrder;
};

// Frees the pipeline's storage.
void graphLayeredReset(GraphLayered& layered);

// The port's position along its side, from 0 to 1, which ordering compares. Returns the middle if `port` is kGraphNone.
inline float graphLayeredPortKey(const GraphLayered& layered, uint32_t port) {
    return port == kGraphNone ? 0.5f : layered.ports.data[port].key;
}

// The innermost group that holds both nodes, or kGraphNone. A group holds its descendants, not itself.
uint32_t graphLayeredCommonGroup(const GraphLayered& layered, uint32_t a, uint32_t b);

// The child of `group` that is `node` or contains it.
uint32_t graphLayeredChildOf(const GraphLayered& layered, uint32_t node, uint32_t group);

// The connected component of a vertex's node or edge.
uint32_t graphLayeredVertexComponent(const GraphLayered& layered, uint32_t vertex);

// Orders `count` items by a depth-first walk, and writes the order to `order`. `edges` holds pairs of items, each from
// the item that should come first. The walk starts from each item in turn, and follows edges in their order in `edges`.
// The result is the reverse of the order in which the walk finishes the items. The edges that point backward in it are
// those that the walk found closing a cycle.
void graphLayeredDepthFirstOrder(uint32_t count, const RawArray<uint32_t>& edges, RawArray<uint32_t>& order,
    RawArray<uint32_t>& scratch);

// Converts a weight to an integer weight for network simplex.
int64_t graphLayeredWeight(float weight);

// The phases before ordering, in the order they run: set up the abstract frame, reverse edges to break cycles, assign
// layers, and build the layered graph with its dummies and placeholders.
void graphLayeredBegin(GraphLayered& layered, const GraphScope& scope, const PrGraphLayoutOptions& options);
void graphLayeredBreakCycles(GraphLayered& layered);
void graphLayeredRank(GraphLayered& layered);
void graphLayeredBuild(GraphLayered& layered);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
