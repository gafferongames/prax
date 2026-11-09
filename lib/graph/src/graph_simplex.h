// Network simplex: assigns integer values to the nodes of a constraint graph to minimize the weighted sum of edge
// lengths, subject to each edge's minimum length (Gansner, Koutsofios, North and Vo, "A Technique for Drawing Directed
// Graphs", 1993). Layering uses it to assign layers, and placement uses it to assign positions on the cross axis.

#pragma once

#include "pr/base.h"
#include "pr/raw_array.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A constraint that `head`'s value is at least `tail`'s plus `minLength`. The edge's length, `head`'s value minus
// `tail`'s, costs `weight` per unit.
struct GraphSimplexEdge {
    uint32_t tail;
    uint32_t head;
    int64_t minLength;
    int64_t weight;
};

// A problem and the solver's working storage, which later problems reuse. The edges must not form a cycle, and no edge
// may join a node to itself. Solving is deterministic: the same problem gives the same values.
struct GraphSimplex {
    uint32_t nodeCount;
    RawArray<GraphSimplexEdge> edges;
    RawArray<int64_t> values;               // by node: the result, with a minimum of zero

    // By node, with the solver's root last: the spanning tree and its postorder numbering. `postorder` lists the nodes
    // by their numbers.
    RawArray<uint32_t> parentEdge;
    RawArray<uint32_t> low;
    RawArray<uint32_t> lim;
    RawArray<uint32_t> postorder;
    RawArray<uint64_t> negative;            // a bit by node: set if the cut value of its tree edge is negative

    // By edge: whether it is in the tree, and a tree edge's cut value.
    RawArray<uint8_t> inTree;
    RawArray<int64_t> cut;

    // Each node's tree edges, as a list of slots: an edge's slot at its tail is twice its index, and at its head one
    // more. `treeFirst` and `treeLast` are by node, and `treeNext` and `treePrev` are by slot.
    RawArray<uint32_t> treeFirst;
    RawArray<uint32_t> treeLast;
    RawArray<uint32_t> treeNext;
    RawArray<uint32_t> treePrev;

    RawArray<uint32_t> incidentStart;       // by node: where its edges start in `incident`, with the total last
    RawArray<uint32_t> incident;
    RawArray<uint32_t> stack;
    RawArray<uint32_t> cursor;              // by node: the next slot that numbering visits
    RawArray<uint32_t> path;                // the nodes on the cycle that an exchange closes
};

// Begins a problem of `nodeCount` nodes and no edges.
void graphSimplexBegin(GraphSimplex& simplex, uint32_t nodeCount);

// Adds the constraint that `head`'s value is at least `tail`'s plus `minLength`. The edge's length costs `weight` per
// unit.
void graphSimplexAddEdge(GraphSimplex& simplex, uint32_t tail, uint32_t head, int64_t minLength, int64_t weight);

// Solves the problem and writes `values`. Each pivot improves the solution. The solver stops after `maxPivots` pivots,
// and the values then meet every constraint even if they are not optimal.
void graphSimplexSolve(GraphSimplex& simplex, uint32_t maxPivots);

// Frees the storage.
void graphSimplexReset(GraphSimplex& simplex);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
