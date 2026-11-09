// Graphs: directed graphs of sized nodes, and their layered layout.
//
// A graph is described once and laid out on request. The caller adds nodes with their extents, ports on the sides of
// nodes, edges between nodes or ports, and labels on edges. A node that other nodes name as their parent is a group,
// and it encloses its children. Layout gives every node and label a rectangle, every port a position, and every edge a
// route.
//
// The layout is layered. Edges run in one direction, from layer to layer, and the nodes in each layer are ordered to
// reduce crossings. The same graph and options always give the same layout. Ties go to the order in which the graph was
// described, so the caller describes the graph in its preferred order.
//
// Coordinates are in the caller's units, with y increasing downward, and the layout's bounds start at the origin.

#pragma once

#include "pr/base.h"
#include "pr/geom.h"
#include "pr/vec.h"

PR_ASSUME_NONNULL_BEGIN

#if defined(__cplusplus)
extern "C" {
#endif

// ---------------------------------------------------------------------------------------------------------------------
// Description

// A graph's description, and its layout once it has one.
typedef struct PrGraph PrGraph;

// Nodes, ports, edges and labels, each numbered densely from zero in the order of their creation.
typedef uint32_t PrGraphNode;
typedef uint32_t PrGraphPort;
typedef uint32_t PrGraphEdge;
typedef uint32_t PrGraphLabel;

// The absent node and port.
PR_CONSTANT PrGraphNode kPrGraphNodeNone = 0xffffffffu;
PR_CONSTANT PrGraphPort kPrGraphPortNone = 0xffffffffu;

// The direction in which layers advance, and edges run unless they close a cycle.
enum {
    PrGraphDirection_Inherit,   // for a group, the direction of its parent
    PrGraphDirection_Down,
    PrGraphDirection_Up,
    PrGraphDirection_Right,
    PrGraphDirection_Left,
};
typedef uint8_t PrGraphDirection;

// A layer that a node must be placed in, within its parent. A node constrained to the first layer precedes every node
// that is not, and a node constrained to the last layer follows every node that is not.
enum {
    PrGraphLayerConstraint_None,
    PrGraphLayerConstraint_First,
    PrGraphLayerConstraint_Last,
};
typedef uint8_t PrGraphLayerConstraint;

// A node. A node with children is a group. A group's rectangle encloses its children and its padding, and is never
// smaller than its extent.
typedef struct PrGraphNodeDesc {
    PrVec2 extent;
    PrGraphNode parent;                         // kPrGraphNodeNone for a node at the top level
    PrGraphLayerConstraint layer;

    // The node's top left corner in the caller's current arrangement, which an interactive layout follows.
    PrVec2 position;

    // The space between a group's border and its children, by PrRectSide. Room for a group's header goes here.
    float padding[PrRectSide_Count];

    // A group's direction. A group whose direction differs from its parent's is laid out separately, and edges that
    // cross its border meet it at ports on its sides.
    PrGraphDirection direction;
} PrGraphNodeDesc;

// A node of zero extent at the top level, with no constraint, no padding, and its parent's direction.
PrGraphNodeDesc prGraphNodeDescDefault(void);

// How much of a port's placement the caller fixes. The layout chooses the rest.
enum {
    PrGraphPortConstraint_Free,         // the layout chooses the side, the order along it, and the position
    PrGraphPortConstraint_FixedSide,    // the layout chooses the order along the side and the position
    PrGraphPortConstraint_FixedOrder,   // the layout chooses the position, keeping the order of description
    PrGraphPortConstraint_FixedPosition,
};
typedef uint8_t PrGraphPortConstraint;

// A point on a side of a node where edges meet the node. The layout spreads the ports that it positions along their
// side, in the order that reduces crossings. Sides refer to the final layout, whatever its direction.
typedef struct PrGraphPortDesc {
    PrGraphNode node;
    PrGraphPortConstraint constraint;
    PrRectSide side;                            // ignored if the port is free

    // A fixed position's distance along its side, from the left end of the top and bottom sides and from the top end of
    // the left and right sides.
    float offset;
} PrGraphPortDesc;

// An edge from `source` to `target`. An end without a port meets its node's rectangle, and the last segment of its
// route points at the node's center, so that a caller can clip the route to the node's shape. An edge may join a node
// to itself, and several edges may join the same nodes. A self-loop runs beside its node, and a group's self-loop runs
// around the group, outside its border.
typedef struct PrGraphEdgeDesc {
    PrGraphNode source;
    PrGraphNode target;
    PrGraphPort sourcePort;                     // kPrGraphPortNone, or a port of `source`
    PrGraphPort targetPort;                     // kPrGraphPortNone, or a port of `target`
    float weight;                               // how strongly the layout keeps the edge short and straight, above 0
    uint32_t minLength;                         // the minimum number of layers that the edge spans, at least 1
} PrGraphEdgeDesc;

// An edge between two nodes without ports, of weight 1, which spans at least one layer.
PrGraphEdgeDesc prGraphEdgeDescDefault(PrGraphNode source, PrGraphNode target);

// Where a label sits along its edge.
enum {
    PrGraphLabelPlacement_Center,   // midway along the edge, in space that the layout reserves, with the edge running
                                    // through the label's center
    PrGraphLabelPlacement_Tail,     // beside the edge, as near its source as is clear of nodes and other labels
    PrGraphLabelPlacement_Head,     // beside the edge, as near its target as is clear of nodes and other labels
};
typedef uint8_t PrGraphLabelPlacement;

// A label on an edge.
typedef struct PrGraphLabelDesc {
    PrGraphEdge edge;
    PrVec2 extent;
    PrGraphLabelPlacement placement;
} PrGraphLabelDesc;

// Creates an empty graph, or frees one.
PrGraph* prGraphCreate(void);
void prGraphDestroy(PrGraph* PR_NULLABLE graph);

// Empties the graph, keeping its storage.
void prGraphClear(PrGraph* graph);

// Adds a node, port, edge or label. Everything that a description names must already be in the graph. A node's parent
// must not be the node or one of its descendants. Adding to a graph discards its layout.
//
// At layout time, no edge may join a group to one of its descendants, and no port may be on a group. In a build without
// assertions, the layout ignores an edge that joins a group to one of its descendants: its route has no points, and its
// labels' rectangles are empty.
PrGraphNode prGraphAddNode(PrGraph* graph, const PrGraphNodeDesc* desc);
PrGraphPort prGraphAddPort(PrGraph* graph, const PrGraphPortDesc* desc);
PrGraphEdge prGraphAddEdge(PrGraph* graph, const PrGraphEdgeDesc* desc);
PrGraphLabel prGraphAddLabel(PrGraph* graph, const PrGraphLabelDesc* desc);

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// The shape of edges' routes.
enum {
    PrGraphRouting_Polyline,    // straight segments
    PrGraphRouting_Spline,      // cubic Bézier curves
    PrGraphRouting_Orthogonal,  // horizontal and vertical segments
};
typedef uint8_t PrGraphRouting;

// How nodes are placed along their layers, once the layers and their orders are known.
enum {
    PrGraphPlacement_Balanced,  // each node lines up with the median of its neighbors, in time linear in the size of
                                // the graph (Brandes and Köpf, as dagre places nodes)
    PrGraphPlacement_Shortest,  // the edges' total weighted length across the layers is the least it can be (network
                                // simplex, as Graphviz places nodes), at a greater cost for large graphs
};
typedef uint8_t PrGraphPlacement;

// Whether a layout starts from scratch or follows the caller's current arrangement.
enum {
    PrGraphLayoutMode_Fresh,        // the layout depends only on the graph and its order of description
    PrGraphLayoutMode_Interactive,  // the nodes' positions decide which edges close cycles, the order of the layers,
                                    // and the order of the nodes in each layer
};
typedef uint8_t PrGraphLayoutMode;

// How a graph is laid out.
typedef struct PrGraphLayoutOptions {
    PrGraphDirection direction;     // the direction of the top level, which must not be Inherit
    PrGraphRouting routing;
    PrGraphPlacement placement;
    PrGraphLayoutMode mode;

    // The minimum space between adjacent layers, between adjacent nodes in a layer, between an edge and a parallel edge
    // or node, between a label and its edge, and between the graph's connected components.
    float layerSpacing;
    float nodeSpacing;
    float edgeSpacing;
    float labelSpacing;
    float componentSpacing;
} PrGraphLayoutOptions;

// Downward, with splines, balanced placement, fresh, with 50 between layers, nodes and components, 10 between edges,
// and 4 between labels and their edges.
PrGraphLayoutOptions prGraphLayoutOptionsDefault(void);

// Lays out the graph, replacing any previous layout. A null `options` means the defaults.
//
// The cost is counted in the S segments of the layered graph: one for each gap between layers that an edge crosses, and
// one for each gap within a group's span of layers. Ordering runs a bounded number of sweeps of O(S log S) each. The
// balanced placement, the tracks of orthogonal routes and the routes take O(S log S), except that a spline that strays
// toward a neighbor is curved again, up to once for each layer that it passes through, so its cost can grow with the
// square of its span. Each head or tail label tries a bounded number of positions, each against the nodes and labels
// near it and against the label's own route. Moving edges out of the groups that they pass through checks each segment
// against the groups that span its gap. The nesting depth of groups multiplies the cost of each check against the
// groups that hold a segment's ends. Layering, and the shortest placement, use network simplex, which is fast in
// practice but has no polynomial bound. It is the largest cost for large graphs.
void prGraphLayoutCompute(PrGraph* graph, const PrGraphLayoutOptions* PR_NULLABLE options);

// Returns true if the graph has a layout. A layout lasts from prGraphLayoutCompute until the graph next changes.
bool prGraphHasLayout(const PrGraph* graph);

// An edge's route, from its source to its target, even if the layout reversed the edge. A polyline or orthogonal route
// lists its vertices. A spline lists its start point, then three points for each cubic Bézier segment: two control
// points and the segment's end. The points are valid until the graph changes.
typedef struct PrGraphRoute {
    uint32_t pointCount;
    const PrVec2* PR_NULLABLE points;
} PrGraphRoute;

// The results of the layout. The graph must have a layout.
PrRect prGraphBounds(const PrGraph* graph);
PrRect prGraphNodeRect(const PrGraph* graph, PrGraphNode node);
PrVec2 prGraphPortPosition(const PrGraph* graph, PrGraphPort port);
PrRect prGraphLabelRect(const PrGraph* graph, PrGraphLabel label);
PrGraphRoute prGraphEdgeRoute(const PrGraph* graph, PrGraphEdge edge);

#if defined(__cplusplus)
}
#endif

PR_ASSUME_NONNULL_END
