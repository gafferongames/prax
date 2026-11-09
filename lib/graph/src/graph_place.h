// Placement: a phase of the layered pipeline (graph_layered.h).

#pragma once

#include "graph/src/graph_layered.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Places the vertices and groups on the cross axis, reserving lanes beside nodes for edges that run alongside them, by
// the layout's placement. Then assigns the tracks of orthogonal routes, places the layers on the main axis, and finds
// each gap's free part.
void graphLayeredPlace(GraphLayered& layered);

// The cross-axis offset from the center of the end's vertex to the point where the end attaches: its port, or its lane.
float graphLayeredAttachment(const GraphLayered& layered, const GraphEnd& end);

// The space that a vertex needs beside it: the node spacing for a node, or the edge spacing for dummies, labels and
// proxies.
float graphLayeredSpacing(const GraphLayered& layered, uint32_t vertex);

// The cross-axis offsets from the centers of a segment's vertices at which placement lines the segment up. An end
// toward a dummy or label aligns at its attachment, so that the edge runs straight from its port into its chain. An end
// toward another node aligns at its fixed port or its lane, and otherwise at the node's center.
void graphLayeredSegmentOffsets(const GraphLayered& layered, uint32_t segment, float& upper, float& lower);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
