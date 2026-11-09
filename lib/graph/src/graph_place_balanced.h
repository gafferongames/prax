// Balanced placement: the cross-axis placement of the layered pipeline's placement phase (graph_place.h) when the
// layout asks for PrGraphPlacement_Balanced.

#pragma once

#include "graph/src/graph_layered.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Places the vertices and groups on the cross axis by Brandes and Köpf's method, with the lanes beside the vertices
// already reserved.
void graphLayeredPlaceBalanced(GraphLayered& layered);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
