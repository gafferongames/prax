// Ordering: a phase of the layered pipeline (graph_layered.h).

#pragma once

#include "graph/src/graph_layered.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Orders the vertices within each layer, and the ports along each side of their nodes, to reduce crossings. Each layer
// keeps every group's vertices contiguous, and each connected component's together.
void graphLayeredOrder(GraphLayered& layered);

// What an edge that passes through a group costs, in crossings. The edge runs over the group's contents, which is worse
// than crossing another edge.
constexpr uint32_t kGraphGroupCrossingWeight = 16;

// The weighted number of crossings between the segments of the gap after `layer`, which ordering reduces. Two edges'
// segments cross if their ends are in opposite orders in the two layers, and each crossing counts 1. A segment of an
// edge that passes through a group's chain, from one side of the group to the other, counts kGraphGroupCrossingWeight.
// Group chains count nothing against one another. Ends compare by their vertices' orders and their ports' keys. The
// cost is O(k log k) for the gap's k segments, with the nesting depth of groups as a factor on the segments' share.
uint64_t graphLayeredGapCrossings(GraphLayered& layered, uint32_t layer);

// The change in the weighted crossings of the gaps on either side of `layer` if the vertex at position `from` moves to
// position `to`, past the vertices between. The vertex must not be a placeholder, and `incident` must be built. The
// cost is O(d log d) for the d segments of the vertex and of the vertices that it passes, with the nesting depth of
// groups as a factor on the vertex's share.
int64_t graphLayeredMoveDelta(GraphLayered& layered, uint32_t layer, uint32_t from, uint32_t to);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
