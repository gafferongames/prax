// Routing: a phase of the layered pipeline (graph_layered.h).

#pragma once

#include "graph/src/graph_layered.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Routes the edges, places the labels, and writes the scope's results in its direction, with its box at the origin.
void graphLayeredRoute(GraphLayered& layered, GraphScope& scope);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
