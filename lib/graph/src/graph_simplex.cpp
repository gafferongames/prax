// Network simplex (graph_simplex.h). A root joins every node by an edge of zero weight, so that every problem has a
// spanning tree. The root's value anchors the others.

#include "graph/src/graph_simplex.h"

#include "pr/assert.h"
#include "pr/integer.h"

namespace pr_impl {

namespace {

constexpr uint32_t kNone = 0xffffffffu;

int64_t slack(const GraphSimplex& simplex, uint32_t edge) {
    const GraphSimplexEdge& e = simplex.edges.data[edge];
    return simplex.values.data[e.head] - simplex.values.data[e.tail] - e.minLength;
}

uint32_t otherEnd(const GraphSimplex& simplex, uint32_t edge, uint32_t node) {
    const GraphSimplexEdge& e = simplex.edges.data[edge];
    return e.tail == node ? e.head : e.tail;
}

// Assigns each node its longest path from the root, in a topological order. These values meet every constraint.
void initValues(GraphSimplex& simplex) {
    uint32_t total = simplex.nodeCount + 1;
    RawArray<uint32_t>& indegree = simplex.lim;
    indegree.assignZeroed(total);
    for (uint32_t e = 0; e < simplex.edges.count; ++e)
        ++indegree.data[simplex.edges.data[e].head];
    simplex.values.resize(total);
    for (uint32_t v = 0; v < total; ++v)
        simplex.values.data[v] = 0;
    simplex.stack.clear();
    simplex.stack.push(simplex.nodeCount);
    uint32_t visited = 0;
    while (simplex.stack.count > 0) {
        uint32_t v = simplex.stack.last();
        simplex.stack.pop();
        ++visited;
        for (uint32_t i = simplex.incidentStart.data[v]; i < simplex.incidentStart.data[v + 1]; ++i) {
            const GraphSimplexEdge& e = simplex.edges.data[simplex.incident.data[i]];
            if (e.tail != v)
                continue;
            int64_t reach = simplex.values.data[v] + e.minLength;
            if (reach > simplex.values.data[e.head])
                simplex.values.data[e.head] = reach;
            if (--indegree.data[e.head] == 0)
                simplex.stack.push(e.head);
        }
    }
    PR_ASSERT(visited == total);
}

// Grows a tree of tight edges from the root until it spans every node. Each time the tree stops growing, its values
// shift toward the nearest node outside it.
void feasibleTree(GraphSimplex& simplex) {
    uint32_t total = simplex.nodeCount + 1;
    RawArray<uint32_t>& inTreeNode = simplex.lim;
    inTreeNode.assignZeroed(total);
    simplex.inTree.assignZeroed(simplex.edges.count);
    inTreeNode.data[simplex.nodeCount] = 1;
    uint32_t treeSize = 1;
    for (;;) {
        simplex.stack.clear();
        for (uint32_t v = 0; v < total; ++v) {
            if (inTreeNode.data[v] != 0)
                simplex.stack.push(v);
        }
        while (simplex.stack.count > 0) {
            uint32_t v = simplex.stack.last();
            simplex.stack.pop();
            for (uint32_t i = simplex.incidentStart.data[v]; i < simplex.incidentStart.data[v + 1]; ++i) {
                uint32_t e = simplex.incident.data[i];
                uint32_t w = otherEnd(simplex, e, v);
                if (inTreeNode.data[w] != 0 || slack(simplex, e) != 0)
                    continue;
                inTreeNode.data[w] = 1;
                simplex.inTree.data[e] = 1;
                ++treeSize;
                simplex.stack.push(w);
            }
        }
        if (treeSize == total)
            return;
        uint32_t best = kNone;
        int64_t bestSlack = 0;
        for (uint32_t e = 0; e < simplex.edges.count; ++e) {
            const GraphSimplexEdge& edge = simplex.edges.data[e];
            if (inTreeNode.data[edge.tail] == inTreeNode.data[edge.head])
                continue;
            int64_t s = slack(simplex, e);
            if (best == kNone || s < bestSlack) {
                best = e;
                bestSlack = s;
            }
        }
        PR_ASSERT(best != kNone);
        int64_t delta = inTreeNode.data[simplex.edges.data[best].tail] != 0 ? bestSlack : -bestSlack;
        for (uint32_t v = 0; v < total; ++v) {
            if (inTreeNode.data[v] != 0)
                simplex.values.data[v] += delta;
        }
    }
}

// Adds a tree edge to the ends of the lists of its ends, or removes it.
void treeLink(GraphSimplex& simplex, uint32_t edge) {
    for (uint32_t end = 0; end < 2; ++end) {
        uint32_t slot = 2 * edge + end;
        uint32_t node = end == 0 ? simplex.edges.data[edge].tail : simplex.edges.data[edge].head;
        uint32_t last = simplex.treeLast.data[node];
        simplex.treePrev.data[slot] = last;
        simplex.treeNext.data[slot] = kNone;
        if (last != kNone)
            simplex.treeNext.data[last] = slot;
        else
            simplex.treeFirst.data[node] = slot;
        simplex.treeLast.data[node] = slot;
    }
}

void treeUnlink(GraphSimplex& simplex, uint32_t edge) {
    for (uint32_t end = 0; end < 2; ++end) {
        uint32_t slot = 2 * edge + end;
        uint32_t node = end == 0 ? simplex.edges.data[edge].tail : simplex.edges.data[edge].head;
        uint32_t next = simplex.treeNext.data[slot];
        uint32_t prev = simplex.treePrev.data[slot];
        if (prev != kNone)
            simplex.treeNext.data[prev] = next;
        else
            simplex.treeFirst.data[node] = next;
        if (next != kNone)
            simplex.treePrev.data[next] = prev;
        else
            simplex.treeLast.data[node] = prev;
    }
}

// Numbers the subtree of `top`, whose tree edge to its parent is `parent`, in postorder from `first`. A node's `lim` is
// its number, and its `low` is the smallest number in its subtree, so a node is in another node's subtree if its `lim`
// is within the other node's range. Sets the parent edges of the nodes below `top`.
//
// A subtree whose root keeps its parent edge and its first number keeps its numbering, so the walk skips it. A node
// whose subtree changed must have its `low` set to kNone first (Gansner et al. renumber the whole subtree; Graphviz
// skips the unchanged parts in the same way).
void numberSubtree(GraphSimplex& simplex, uint32_t top, uint32_t parent, uint32_t first) {
    auto unchanged = [&](uint32_t v, uint32_t edge, uint32_t low) {
        return simplex.low.data[v] != kNone && simplex.parentEdge.data[v] == edge && simplex.low.data[v] == low;
    };
    if (unchanged(top, parent, first))
        return;
    uint32_t next = first;
    simplex.stack.clear();
    simplex.stack.push(top);
    simplex.parentEdge.data[top] = parent;
    simplex.low.data[top] = next;
    simplex.cursor.data[top] = simplex.treeFirst.data[top];
    while (simplex.stack.count > 0) {
        uint32_t v = simplex.stack.last();
        bool descended = false;
        while (simplex.cursor.data[v] != kNone) {
            uint32_t slot = simplex.cursor.data[v];
            simplex.cursor.data[v] = simplex.treeNext.data[slot];
            uint32_t e = slot / 2;
            if (e == simplex.parentEdge.data[v])
                continue;
            uint32_t w = otherEnd(simplex, e, v);
            if (unchanged(w, e, next)) {
                next = simplex.lim.data[w] + 1;
                continue;
            }
            simplex.parentEdge.data[w] = e;
            simplex.low.data[w] = next;
            simplex.cursor.data[w] = simplex.treeFirst.data[w];
            simplex.stack.push(w);
            descended = true;
            break;
        }
        if (descended)
            continue;
        simplex.lim.data[v] = next;
        simplex.postorder.data[next++] = v;
        simplex.stack.pop();
    }
}

// The cut value of the tree edge from `child` to its parent: the weight of the edges from the side of the cut that
// holds the edge's tail to the side that holds its head, minus the weight of the edges the other way. The cut values of
// the child's own tree edges must already be known.
int64_t cutValue(const GraphSimplex& simplex, uint32_t child) {
    uint32_t treeEdge = simplex.parentEdge.data[child];
    bool childIsTail = simplex.edges.data[treeEdge].tail == child;
    int64_t value = simplex.edges.data[treeEdge].weight;
    for (uint32_t i = simplex.incidentStart.data[child]; i < simplex.incidentStart.data[child + 1]; ++i) {
        uint32_t e = simplex.incident.data[i];
        if (e == treeEdge)
            continue;
        const GraphSimplexEdge& edge = simplex.edges.data[e];
        bool isOut = edge.tail == child;
        bool pointsToHead = isOut == childIsTail;
        value += pointsToHead ? edge.weight : -edge.weight;
        if (simplex.inTree.data[e] != 0)
            value += pointsToHead ? -simplex.cut.data[e] : simplex.cut.data[e];
    }
    return value;
}

void initCutValues(GraphSimplex& simplex) {
    for (uint32_t i = 0; i < simplex.postorder.count; ++i) {
        uint32_t v = simplex.postorder.data[i];
        if (v != simplex.nodeCount)
            simplex.cut.data[simplex.parentEdge.data[v]] = cutValue(simplex, v);
    }
}

// Sets or clears the node's bit by the sign of its tree edge's cut value.
void markNegative(GraphSimplex& simplex, uint32_t v) {
    if (v == simplex.nodeCount)
        return;
    uint64_t bit = uint64_t(1) << (v % 64);
    if (simplex.cut.data[simplex.parentEdge.data[v]] < 0)
        simplex.negative.data[v / 64] |= bit;
    else
        simplex.negative.data[v / 64] &= ~bit;
}

// The first node, from `from` on and then from the start, whose tree edge has a negative cut value, or kNone.
uint32_t nextNegative(const GraphSimplex& simplex, uint32_t from) {
    uint32_t n = simplex.nodeCount;
    uint32_t words = simplex.negative.count;
    for (uint32_t pass = 0; pass < 2; ++pass) {
        uint32_t start = pass == 0 ? from : 0;
        uint32_t end = pass == 0 ? n : from;
        for (uint32_t word = start / 64; word < words && word * 64 < end; ++word) {
            uint64_t bits = simplex.negative.data[word];
            if (word == start / 64)
                bits &= ~uint64_t(0) << (start % 64);
            if (bits == 0)
                continue;
            uint32_t v = word * 64 + prU64LowestBit(bits);
            if (v < end)
                return v;
        }
    }
    return kNone;
}

bool isDescendant(const GraphSimplex& simplex, uint32_t node, uint32_t root) {
    uint32_t lim = simplex.lim.data[node];
    return simplex.low.data[root] <= lim && lim <= simplex.lim.data[root];
}

// Calls `visit` with each node of the subtree of `top` if `inside`, or else with each node outside it.
template <typename Visit>
void forSide(const GraphSimplex& simplex, uint32_t top, bool inside, Visit visit) {
    uint32_t low = simplex.low.data[top];
    uint32_t lim = simplex.lim.data[top];
    if (inside) {
        for (uint32_t i = low; i <= lim; ++i)
            visit(simplex.postorder.data[i]);
        return;
    }
    for (uint32_t i = 0; i < low; ++i)
        visit(simplex.postorder.data[i]);
    for (uint32_t i = lim + 1; i < simplex.postorder.count; ++i)
        visit(simplex.postorder.data[i]);
}

// Returns true if the subtree of `top` holds fewer nodes than the rest of the tree.
bool isSmallerSide(const GraphSimplex& simplex, uint32_t top) {
    return simplex.lim.data[top] - simplex.low.data[top] + 1 <= simplex.postorder.count / 2;
}

// The edge that replaces `treeEdge` in the tree: the edge with the least slack among those that cross its cut in the
// opposite direction, the first in the order of the edges on a tie. Every such edge has one end on each side of the
// cut, so the search reads the edges of the side with fewer nodes.
uint32_t enterEdge(const GraphSimplex& simplex, uint32_t treeEdge) {
    const GraphSimplexEdge& leaving = simplex.edges.data[treeEdge];
    uint32_t side = leaving.tail;
    bool flip = false;
    if (simplex.lim.data[leaving.tail] > simplex.lim.data[leaving.head]) {
        side = leaving.head;
        flip = true;
    }
    uint32_t best = kNone;
    int64_t bestSlack = 0;
    forSide(simplex, side, isSmallerSide(simplex, side), [&](uint32_t v) {
        for (uint32_t i = simplex.incidentStart.data[v]; i < simplex.incidentStart.data[v + 1]; ++i) {
            uint32_t e = simplex.incident.data[i];
            const GraphSimplexEdge& edge = simplex.edges.data[e];
            if (flip != isDescendant(simplex, edge.tail, side) || flip == isDescendant(simplex, edge.head, side))
                continue;
            int64_t s = slack(simplex, e);
            if (best == kNone || s < bestSlack || (s == bestSlack && e < best)) {
                best = e;
                bestSlack = s;
            }
        }
    });
    return best;
}

// Adds `delta` to the cut values of the tree edges on the path up from `v` to the first node whose subtree holds `w`,
// and returns that node. An edge whose tail is on the walk's side takes `delta` if `forward` is set, and its negation
// otherwise.
uint32_t treeUpdate(GraphSimplex& simplex, uint32_t v, uint32_t w, int64_t delta, bool forward) {
    while (!isDescendant(simplex, w, v)) {
        uint32_t e = simplex.parentEdge.data[v];
        bool add = simplex.edges.data[e].tail == v ? forward : !forward;
        simplex.cut.data[e] += add ? delta : -delta;
        v = otherEnd(simplex, e, v);
    }
    return v;
}

// Appends the nodes on the path up from `v` to `top`, without `top`, to the exchange's path.
void collectPath(GraphSimplex& simplex, uint32_t v, uint32_t top) {
    for (; v != top; v = otherEnd(simplex, simplex.parentEdge.data[v], v))
        simplex.path.push(v);
}

// Replaces the tree edge `leaving`, whose child is `child`, with `entering` (Gansner et al., 1993, section 2.4). The
// two sides of the cut move apart so that `entering` becomes tight; only differences between values matter, so the side
// with fewer nodes moves. Only the tree edges on the cycle that `entering` closes change their cut values, and only the
// nodes on that cycle, and the subtrees whose numbers they shift, change their numbering.
void exchange(GraphSimplex& simplex, uint32_t leaving, uint32_t child, uint32_t entering) {
    const GraphSimplexEdge& edge = simplex.edges.data[entering];
    int64_t s = slack(simplex, entering);
    int64_t delta = isDescendant(simplex, edge.tail, child) ? s : -s;
    bool inside = isSmallerSide(simplex, child);
    forSide(simplex, child, inside, [&](uint32_t v) {
        simplex.values.data[v] += inside ? delta : -delta;
    });

    int64_t cut = simplex.cut.data[leaving];
    uint32_t top = treeUpdate(simplex, edge.tail, edge.head, cut, true);
    uint32_t other = treeUpdate(simplex, edge.head, edge.tail, cut, false);
    PR_ASSERT(top == other);
    (void)other;
    simplex.cut.data[entering] = -cut;
    simplex.cut.data[leaving] = 0;

    // The cycle's nodes, from each end of `entering` up to the top. Their subtrees change, and numbering skips the
    // others unless their numbers shift.
    simplex.path.clear();
    collectPath(simplex, edge.tail, top);
    collectPath(simplex, edge.head, top);
    simplex.path.push(top);
    uint32_t first = simplex.low.data[top];
    for (uint32_t i = 0; i < simplex.path.count; ++i)
        simplex.low.data[simplex.path.data[i]] = kNone;
    simplex.inTree.data[leaving] = 0;
    simplex.inTree.data[entering] = 1;
    treeUnlink(simplex, leaving);
    // Last in its ends' lists, the new edge shifts the numbers of no sibling after it.
    treeLink(simplex, entering);
    numberSubtree(simplex, top, simplex.parentEdge.data[top], first);
    for (uint32_t i = 0; i < simplex.path.count; ++i)
        markNegative(simplex, simplex.path.data[i]);
}

} // namespace

void graphSimplexBegin(GraphSimplex& simplex, uint32_t nodeCount) {
    simplex.nodeCount = nodeCount;
    simplex.edges.clear();
}

void graphSimplexAddEdge(GraphSimplex& simplex, uint32_t tail, uint32_t head, int64_t minLength, int64_t weight) {
    PR_ASSERT(tail < simplex.nodeCount && head < simplex.nodeCount && tail != head);
    PR_ASSERT(weight >= 0);
    simplex.edges.push(GraphSimplexEdge{.tail = tail, .head = head, .minLength = minLength, .weight = weight});
}

void graphSimplexSolve(GraphSimplex& simplex, uint32_t maxPivots) {
    uint32_t n = simplex.nodeCount;
    uint32_t total = n + 1;
    uint32_t userEdges = simplex.edges.count;
    for (uint32_t v = 0; v < n; ++v)
        simplex.edges.push(GraphSimplexEdge{.tail = n, .head = v, .minLength = 0, .weight = 0});
    uint32_t edgeCount = simplex.edges.count;

    simplex.incidentStart.assignZeroed(total + 1);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        ++simplex.incidentStart.data[simplex.edges.data[e].tail + 1];
        ++simplex.incidentStart.data[simplex.edges.data[e].head + 1];
    }
    for (uint32_t v = 0; v < total; ++v)
        simplex.incidentStart.data[v + 1] += simplex.incidentStart.data[v];
    simplex.incident.resize(simplex.incidentStart.data[total]);
    simplex.stack.resize(total);
    for (uint32_t v = 0; v < total; ++v)
        simplex.stack.data[v] = simplex.incidentStart.data[v];
    for (uint32_t e = 0; e < edgeCount; ++e) {
        simplex.incident.data[simplex.stack.data[simplex.edges.data[e].tail]++] = e;
        simplex.incident.data[simplex.stack.data[simplex.edges.data[e].head]++] = e;
    }

    initValues(simplex);
    feasibleTree(simplex);
    simplex.treeFirst.resize(total);
    simplex.treeLast.resize(total);
    for (uint32_t v = 0; v < total; ++v) {
        simplex.treeFirst.data[v] = kNone;
        simplex.treeLast.data[v] = kNone;
    }
    simplex.treeNext.resize(2 * edgeCount);
    simplex.treePrev.resize(2 * edgeCount);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        if (simplex.inTree.data[e] != 0)
            treeLink(simplex, e);
    }
    simplex.low.resize(total);
    simplex.lim.resize(total);
    simplex.parentEdge.resize(total);
    simplex.postorder.resize(total);
    simplex.cursor.resize(total);
    for (uint32_t v = 0; v < total; ++v)
        simplex.low.data[v] = kNone;
    numberSubtree(simplex, n, kNone, 0);
    simplex.cut.assignZeroed(edgeCount);
    initCutValues(simplex);
    simplex.negative.assignZeroed((n + 63) / 64);
    for (uint32_t v = 0; v < n; ++v)
        markNegative(simplex, v);
    uint32_t searchFrom = 0;
    for (uint32_t pivot = 0; pivot < maxPivots && n > 0; ++pivot) {
        uint32_t leaving = nextNegative(simplex, searchFrom);
        if (leaving == kNone)
            break;
        searchFrom = leaving + 1 < n ? leaving + 1 : 0;
        uint32_t treeEdge = simplex.parentEdge.data[leaving];
        uint32_t entering = enterEdge(simplex, treeEdge);
        if (entering == kNone)
            break;
        exchange(simplex, treeEdge, leaving, entering);
    }

    int64_t least = 0;
    for (uint32_t v = 0; v < n; ++v) {
        if (v == 0 || simplex.values.data[v] < least)
            least = simplex.values.data[v];
    }
    for (uint32_t v = 0; v < n; ++v)
        simplex.values.data[v] -= least;
    simplex.values.resize(n);
    simplex.edges.resize(userEdges);
}

void graphSimplexReset(GraphSimplex& simplex) {
    simplex.edges.reset();
    simplex.values.reset();
    simplex.parentEdge.reset();
    simplex.low.reset();
    simplex.lim.reset();
    simplex.postorder.reset();
    simplex.negative.reset();
    simplex.inTree.reset();
    simplex.cut.reset();
    simplex.treeFirst.reset();
    simplex.treeLast.reset();
    simplex.treeNext.reset();
    simplex.treePrev.reset();
    simplex.incidentStart.reset();
    simplex.incident.reset();
    simplex.stack.reset();
    simplex.cursor.reset();
    simplex.path.reset();
}

} // namespace pr_impl
