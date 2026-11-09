# Graph layout

`lib/graph` lays out directed graphs of sized nodes (`pr/graph.h`). It depends only on `lib/core` and `lib/geom`, and it
works with geometry alone: extents go in, and rectangles, positions and routes come out. The client decides what a node
looks like, what its text says and how it is drawn. The clients are text-based diagrams, such as Mermaid flowcharts,
and the auto-arrange feature of node graph editors.

## Which behavior

No standard defines how to lay out a graph, and no implementation's output is a reference. The goal is the layout
that a reader finds clearest: few crossings, short and straight edges, a consistent direction of flow, compact groups,
and no overlaps. dagre, Graphviz's dot and ELK Layered show what works and what readers expect, but the engine does
not match any of them for its own sake. In particular, a Mermaid diagram need not look the way mermaid.js draws it.

The algorithms come from the literature on layered drawing, which those implementations also build on:

- Sugiyama, Tagawa and Toda (1981) for the pipeline.
- Gansner, Koutsofios, North and Vo (1993) for network simplex, which both assigns layers and places nodes on the cross
  axis, and for crossing reduction by layer sweeps.
- Sander (1996) and Forster (2004) for groups.
- Schulze, Spönemann and von Hanxleden (2014) for ports.

## The model

- **Nodes** have an extent. A node may have a parent, which makes the parent a **group**. A group's rectangle encloses
  its children and its padding. An edge can meet a group just as it meets any other node.
- **Ports** are points on the sides of nodes where edges meet them. A port may fix its position, as the inputs and
  outputs of an editor's node do, or fix only its side, or fix nothing. The layout chooses whatever the port leaves
  open: the side that faces the port's edges, the order along the side that reduces crossings, and positions spread
  along the side. Sides refer to the final layout, so a node keeps its inputs on its left whichever way the layout
  runs. Diagram boxes use free ports, so that edges into a box spread along its side instead of meeting at its center.
- **Ports on sides parallel to the flow**, such as the top and bottom in a left-to-right layout, start or end their
  edges with a segment that runs along the layer. The ordering and routing phases place that segment beside the node,
  within its layer. These phases are also designed to handle edges that stay within one layer, which a minimum length
  of zero would allow.
- **Edges** join nodes or ports. Each edge has a weight, which says how strongly the layout keeps it short and
  straight, and a minimum length in layers. An end without a port meets its node's rectangle on a line toward the
  node's center, so that the client can clip the edge to the node's shape, such as a diamond, a circle or a cylinder.
  Self-loops and parallel edges are allowed. A self-loop runs beside its node, and a group's self-loop runs around the
  group, outside its border.
- **Labels** belong to edges. A center label takes space in the layout as a node does, in a layer midway along its
  edge. The edge runs through the label's center, so the client draws the label over the edge. A head or tail label
  sits beside its edge, next to the edge's target or source.
- **Groups and their descendants** cannot be joined by edges, because such an edge has no direction to flow in. This is
  a precondition, which assertions check. A build without assertions ignores such an edge and gives it an empty route.
- **The order of description** breaks every tie. A client describes a graph in its preferred order: the source order
  of a Mermaid diagram, or the document order of an editor.

## The pipeline

All connected components of a graph share one layered layout. Each component starts at the first layer, so that their
layers line up, like the columns of separate rows in a diagram. In each layer, a component's nodes stay together, in
the order in which the components were described, with the component spacing between components. Each phase works in
abstract coordinates, along the layers and across them, and the result is mapped to the layout's direction at the end.

1. **Cycles.** A depth-first walk finds the edges that close cycles, and the layout reverses them so that the graph is
   acyclic. The walk starts from each node in the order of description and follows edges in their order of
   description, as dagre's does. The first node described therefore starts the flow, which is where a reader of a
   diagram looks for the start. Each group is walked as one unit among its siblings, and its children are walked among
   themselves, so that the remaining edges never form a cycle through a group's border. A reversed edge's route still
   runs from its source to its target.
2. **Layers.** Network simplex assigns the layers. It minimizes the total weighted length of the edges, subject to their
   minimum lengths and to the layer constraints. Each group adds a light pull that keeps its span of layers short.
3. **Dummies.** An edge that spans more than one layer becomes a chain of dummy nodes, one in each layer between its
   ends. A center label is a dummy with the label's extent. An edge with a center label spans at least two layers, so
   that it has a middle layer.
4. **Order.** Layer sweeps order each layer by the barycenters of its neighbors' positions. Each sweep then transposes
   adjacent nodes of the same group while that reduces crossings. The layout keeps the order with the fewest crossings.
   Fixed ports count by their positions, so that edges are ordered by where their ports are on their sides. After each
   sweep, ports that are not fixed are sorted along their sides by their neighbors' positions.
5. **Coordinates.** Network simplex places nodes on the cross axis, as in dot. It minimizes the weighted offsets
   between edges' ends, and gives long edges the most weight so that they run straight. Fixed ports count by their
   positions, so that connected ports line up. The ports that the layout places spread around their nodes' centers,
   and nodes line up by their centers, so a chain of nodes runs straight however many edges each node has. Spacing,
   group padding and enclosure, and lanes beside nodes are all constraints of the same problem, so one solver handles
   them all. Ties are balanced: a node moves to the middle of the range of positions that cost the same, as a parent
   does over two children.
6. **Layer positions.** Each layer is as deep as its deepest node. The gaps between layers make room for the padding
   of the groups that begin and end there, and for the tracks of orthogonal routes.
7. **Routes.** A polyline or orthogonal route runs straight through each layer that it crosses, at its dummy's position.
   It changes position only in the gaps between layers, where there are no nodes. An orthogonal route crosses a gap on a
   track of its own. Of two routes that cross it the same way, the one that ends farther that way takes the track
   nearer the layer it leaves, so that the two do not cross. A spline is one smooth curve through its dummies and
   labels, and it leaves and meets its ends along the main axis. Its cross-axis position is a monotone cubic function of
   its main-axis position, so it never overshoots a dummy and comes back. If the curve would stray toward a neighbor in
   a dummy's layer by more than the dummy's clearance, the route runs straight through that layer instead. An edge whose
   port faces away from its route, or lies on a side parallel to the flow, runs in a lane beside its node that
   placement reserved. Self-loops also run in such a lane. A group's self-loops run in a lane that placement reserves
   outside the group's border.

## Groups

A group with the same direction as its parent takes part in its parent's layout. Its descendants are laid out with the
rest of the graph in one layered layout. The group has a placeholder in each layer that it spans, which keeps outside
nodes out of its rectangle, and every layer keeps the group's members contiguous. An edge that leaves the group leaves
it immediately: the edge's dummies belong to the innermost group that holds both of its ends. This keeps crossings low
at the group's border. That matters for an editor's frames, whose nodes usually connect to nodes outside them.

A group whose direction differs from its parent's is laid out separately, from the bottom up. It then takes part in its
parent's layout as a single node with its laid-out extent. An edge that crosses its border is split at a port on the
group's side, and each layout places that port as one of its own ends. A center label goes with the layout that holds
both of the edge's ends, and a head or tail label with the layout that holds its end, so that it sits beside its node
rather than at the group's border. The engine always honors a group's direction. mermaid.js ignores a subgraph's
direction if an edge crosses its border, and the engine does not follow it there.

## Interactive layouts

An editor that arranges a graph already has an arrangement, and its users expect a new layout to keep what they
arranged. An interactive layout follows the nodes' current positions. They decide which edges close cycles, the order
of the layers, and the order of the nodes within each layer. Coordinates and routes are computed as in a fresh layout.

To arrange a selection, a client lays out a graph of only the selected nodes. Each node outside the selection that an
edge joins to it becomes a proxy with zero extent. The proxy is constrained to the first layer if it comes before the
selection in the direction of flow, and to the last layer if it comes after. The client then places the result where
the selection was. The layout itself has no pinned nodes, because a layered layout cannot keep a node in place without
giving up the layers that make it readable.

A client that lays out again after each edit, such as an editor showing a Mermaid diagram, keeps the result stable by
passing the previous positions to an interactive layout.

## Cost

The layered graph has a segment for each gap between layers that an edge crosses, and for each gap within a group's
span of layers. With S segments:

- **Layers** come from network simplex, which is fast in practice but has no polynomial bound. On large graphs it is
  the largest cost. The shortest placement uses it too.
- **Order** runs a bounded number of sweeps, each in O(S log S). A gap's crossings are counted with an accumulator tree
  (Barth, Jünger and Mutzel, 2002), and a gap of few segments pair by pair. Transposition and the moves that take edges
  out of groups count only the change that they make, from the segments of the vertices whose order they change. Taking
  edges out of groups also checks each segment against the groups that span its gap.
- **Coordinates** by the balanced placement take O(S log S). The tracks of orthogonal routes take O(S log S): each gap's
  steps are sorted once, and a segment tree finds the latest track among the steps that overlap each one.
- **Routes** take O(S log S), except that a spline that strays toward a neighbor is curved again, up to once for each
  layer that it passes through. Its cost can therefore grow with the square of its span. A head or tail label tries a
  bounded number of positions, each against the nodes and labels near it, which a grid finds, and against its own route.
- **Scopes** cost time linear in the size of the graph and of its layout, plus the nesting depth of scopes for each
  edge.

The nesting depth of groups multiplies the cost of each check of a segment's ends against the groups that hold them.

## Testing

- **Invariants** are checked on every layout of the corpus. No two nodes overlap. No route crosses a node other than
  its ends. Every edge spans at least its minimum length in the direction of flow, unless it closes a cycle.
  Constraints and ports hold. Groups enclose their descendants and nothing else. The same graph and options give the
  same layout.
- **Quality** is measured on the corpus and recorded, so that the diff of a change shows its effect: crossings, total
  edge length, bends, and area. dagre and ELK lay out the same graphs as references. Their measurements are compared,
  not their coordinates.
