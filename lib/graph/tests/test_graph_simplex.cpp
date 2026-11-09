#include "graph/src/graph_simplex.h"

#include "tools/harness/test.h"

namespace pr_impl {

namespace {

// Returns true if the values meet every edge's minimum length.
bool isFeasible(const GraphSimplex& simplex) {
    for (uint32_t e = 0; e < simplex.edges.count; ++e) {
        const GraphSimplexEdge& edge = simplex.edges.data[e];
        if (simplex.values.data[edge.head] - simplex.values.data[edge.tail] < edge.minLength)
            return false;
    }
    return true;
}

int64_t cost(const GraphSimplex& simplex) {
    int64_t total = 0;
    for (uint32_t e = 0; e < simplex.edges.count; ++e) {
        const GraphSimplexEdge& edge = simplex.edges.data[e];
        total += edge.weight * (simplex.values.data[edge.head] - simplex.values.data[edge.tail]);
    }
    return total;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("graph simplex: a chain takes its minimum lengths, from zero") {
    GraphSimplex simplex{};
    graphSimplexBegin(simplex, 3);
    graphSimplexAddEdge(simplex, 0, 1, 2, 1);
    graphSimplexAddEdge(simplex, 1, 2, 3, 1);
    graphSimplexSolve(simplex, 100);
    CHECK(simplex.values.data[0] == 0);
    CHECK(simplex.values.data[1] == 2);
    CHECK(simplex.values.data[2] == 5);
    graphSimplexReset(simplex);
}

TEST_CASE("graph simplex: a short edge pulls its tail toward a later head") {
    // 0 -> 1 -> 2 -> 3, and 4 -> 3. Longest-path layering puts 4 at zero, but 4 is better placed beside 2.
    GraphSimplex simplex{};
    graphSimplexBegin(simplex, 5);
    graphSimplexAddEdge(simplex, 0, 1, 1, 1);
    graphSimplexAddEdge(simplex, 1, 2, 1, 1);
    graphSimplexAddEdge(simplex, 2, 3, 1, 1);
    graphSimplexAddEdge(simplex, 4, 3, 1, 1);
    graphSimplexSolve(simplex, 100);
    CHECK(isFeasible(simplex));
    CHECK(simplex.values.data[4] == 2);
    CHECK(cost(simplex) == 4);
    graphSimplexReset(simplex);
}

TEST_CASE("graph simplex: heavier edges win, and negative lengths are allowed") {
    // The form that placement uses: 0 is at most both 1 + 3 and 2 - 5, and 1 is at most 2.
    GraphSimplex simplex{};
    graphSimplexBegin(simplex, 3);
    graphSimplexAddEdge(simplex, 0, 1, -3, 4);
    graphSimplexAddEdge(simplex, 0, 2, 5, 1);
    graphSimplexAddEdge(simplex, 1, 2, 0, 0);
    graphSimplexSolve(simplex, 100);
    CHECK(isFeasible(simplex));
    // The heavy edge is tight: 1 sits 3 below 0, and 2 is at least 5 past 0.
    CHECK(simplex.values.data[1] - simplex.values.data[0] == -3);
    CHECK(simplex.values.data[2] - simplex.values.data[0] == 5);
    graphSimplexReset(simplex);
}

TEST_CASE("graph simplex: a solution is optimal on a diamond with a long side") {
    GraphSimplex simplex{};
    graphSimplexBegin(simplex, 6);
    graphSimplexAddEdge(simplex, 0, 1, 1, 1);
    graphSimplexAddEdge(simplex, 1, 2, 1, 1);
    graphSimplexAddEdge(simplex, 2, 3, 1, 1);
    graphSimplexAddEdge(simplex, 0, 4, 1, 1);
    graphSimplexAddEdge(simplex, 4, 3, 1, 1);
    graphSimplexAddEdge(simplex, 5, 4, 1, 3);
    graphSimplexSolve(simplex, 100);
    CHECK(isFeasible(simplex));
    // 0..3 spans three layers. The short path through 4 costs three in total wherever 4 sits, and 5 sits just before 4.
    CHECK(simplex.values.data[4] - simplex.values.data[5] == 1);
    CHECK(cost(simplex) == 9);
    graphSimplexReset(simplex);
}
