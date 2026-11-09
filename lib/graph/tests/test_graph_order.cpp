#include "graph/src/graph_order.h"

#include "tools/harness/test.h"

#include <vector>

namespace pr_impl {

namespace {

// A deterministic pseudo-random sequence.
struct Sequence {
    uint32_t state;

    uint32_t next(uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % bound;
    }
};

// Describes a random scope: leaves in nested groups, free ports on some leaves, and edges between leaves, from leaves
// to groups that do not hold them, and in parallel. A dense scope has more edges, a third of them at four hubs, so that
// its gaps hold many segments and its hubs many each.
void randomScope(GraphScope& scope, Sequence& random, uint32_t leaves, bool dense) {
    graphScopeClear(scope);
    scope.direction = PrGraphDirection_Down;
    std::vector<uint32_t> containers{kGraphNone};
    std::vector<uint32_t> leafNodes;
    std::vector<uint32_t> groups;
    for (uint32_t i = 0; i < leaves; ++i) {
        uint32_t parent = containers[random.next(uint32_t(containers.size()))];
        if (random.next(4) == 0) {
            scope.nodes.push(GraphScopeNode{.extent = PrVec2{0, 0}, .parent = parent});
            parent = scope.nodes.count - 1;
            containers.push_back(parent);
            groups.push_back(parent);
        }
        scope.nodes.push(GraphScopeNode{.extent = PrVec2{40, 20}, .parent = parent});
        leafNodes.push_back(scope.nodes.count - 1);
        if (random.next(3) == 0) {
            for (uint32_t p = 0; p < 2; ++p)
                scope.ports.push(GraphScopePort{
                    .node = scope.nodes.count - 1,
                    .constraint = PrGraphPortConstraint_Free,
                });
        }
    }
    auto holds = [&](uint32_t group, uint32_t node) {
        for (uint32_t g = scope.nodes.data[node].parent; g != kGraphNone; g = scope.nodes.data[g].parent) {
            if (g == group)
                return true;
        }
        return false;
    };
    auto portOf = [&](uint32_t node) {
        for (uint32_t p = 0; p < scope.ports.count; ++p) {
            if (scope.ports.data[p].node == node && random.next(2) == 0)
                return p;
        }
        return kGraphNone;
    };
    uint32_t edges = leaves * (dense ? 5 : 2);
    for (uint32_t i = 0; i < edges; ++i) {
        uint32_t source = leafNodes[random.next(uint32_t(leafNodes.size()))];
        uint32_t target = leafNodes[random.next(uint32_t(leafNodes.size()))];
        if (dense && random.next(3) == 0)
            source = leafNodes[random.next(4)];
        if (!groups.empty() && random.next(6) == 0)
            target = groups[random.next(uint32_t(groups.size()))];
        if (source == target || holds(target, source))
            continue;
        uint32_t copies = random.next(5) == 0 ? 2 : 1;
        for (uint32_t c = 0; c < copies; ++c) {
            scope.edges.push(GraphScopeEdge{
                .source = source,
                .target = target,
                .sourcePort = portOf(source),
                .targetPort = portOf(target),
                .weight = 1,
                .minLength = 1 + random.next(2),
                .labelStart = 0,
                .labelCount = 0,
            });
        }
    }
}

bool within(const GraphLayered& layered, const GraphVertex& vertex, uint32_t group) {
    for (uint32_t g = vertex.group; g != kGraphNone; g = layered.scope->nodes.data[g].parent) {
        if (g == group)
            return true;
    }
    return false;
}

float endKey(const GraphLayered& layered, uint32_t vertex, uint32_t port) {
    return float(layered.vertices.data[vertex].order) + graphLayeredPortKey(layered, port);
}

// The weighted crossings of a gap by their definition, pair by pair.
uint64_t pairwiseGapCrossings(const GraphLayered& layered, uint32_t gap) {
    std::vector<uint32_t> ids;
    for (uint32_t i = layered.gapSegmentStart.data[gap]; i < layered.gapSegmentStart.data[gap + 1]; ++i)
        ids.push_back(layered.gapSegments.data[i]);
    auto passesThrough = [&](const GraphSegment& segment, uint32_t group) {
        const GraphVertex& upper = layered.vertices.data[segment.upper];
        uint32_t top = layered.rankTop.data[group];
        if (upper.layer < top || upper.layer + 1 > layered.rankBottom.data[group] || within(layered, upper, group) ||
                within(layered, layered.vertices.data[segment.lower], group))
            return false;
        uint32_t placeholder = layered.nodeVertex.data[group] + (upper.layer - top);
        bool upperBefore = endKey(layered, segment.upper, segment.upperPort) < endKey(layered, placeholder, kGraphNone);
        bool lowerBefore =
            endKey(layered, segment.lower, segment.lowerPort) < endKey(layered, placeholder + 1, kGraphNone);
        return upperBefore != lowerBefore;
    };
    uint64_t crossings = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        const GraphSegment& a = layered.segments.data[ids[i]];
        float ui = endKey(layered, a.upper, a.upperPort);
        float li = endKey(layered, a.lower, a.lowerPort);
        for (size_t j = i + 1; j < ids.size(); ++j) {
            const GraphSegment& b = layered.segments.data[ids[j]];
            float uj = endKey(layered, b.upper, b.upperPort);
            float lj = endKey(layered, b.lower, b.lowerPort);
            if (!((ui < uj && li > lj) || (ui > uj && li < lj)))
                continue;
            if (!a.chain && !b.chain) {
                ++crossings;
                continue;
            }
            if (a.chain && b.chain)
                continue;
            uint32_t group = layered.vertices.data[(a.chain ? a : b).upper].node;
            if (passesThrough(a.chain ? b : a, group))
                crossings += kGraphGroupCrossingWeight;
        }
    }
    return crossings;
}

// The pairwise crossings of the gaps on either side of `layer`.
int64_t pairwiseBeside(const GraphLayered& layered, uint32_t layer) {
    int64_t total = 0;
    if (layer > 0)
        total += int64_t(pairwiseGapCrossings(layered, layer - 1));
    if (layer + 1 < layered.layerCount)
        total += int64_t(pairwiseGapCrossings(layered, layer));
    return total;
}

void setOrder(GraphLayered& layered, uint32_t layer) {
    for (uint32_t i = layered.layerStart.data[layer]; i < layered.layerStart.data[layer + 1]; ++i)
        layered.vertices.data[layered.layerVertices.data[i]].order = i - layered.layerStart.data[layer];
}

// Shuffles every layer and every port's key. The orders need not keep groups contiguous, and the keys repeat, so that
// ties occur.
void shuffle(GraphLayered& layered, Sequence& random) {
    for (uint32_t layer = 0; layer < layered.layerCount; ++layer) {
        uint32_t start = layered.layerStart.data[layer];
        uint32_t count = layered.layerStart.data[layer + 1] - start;
        for (uint32_t i = count; i > 1; --i) {
            uint32_t j = random.next(i);
            uint32_t t = layered.layerVertices.data[start + i - 1];
            layered.layerVertices.data[start + i - 1] = layered.layerVertices.data[start + j];
            layered.layerVertices.data[start + j] = t;
        }
        setOrder(layered, layer);
    }
    for (uint32_t p = 0; p < layered.ports.count; ++p)
        layered.ports.data[p].key = float(random.next(4)) * 0.25f;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("graph order: a gap's crossings match the pairwise count, and a move's delta matches the counts around it") {
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    // The counts compare small inputs pair by pair and sort large ones. These count the large inputs that the test
    // reaches: gaps of many segments, and moves past one vertex with many pairs of segments.
    uint32_t moves = 0;
    uint32_t largeGaps = 0;
    uint32_t largeMoves = 0;
    for (uint32_t seed = 1; seed <= 72; ++seed) {
        Sequence random{seed};
        GraphScope scope{};
        bool dense = seed > 60;
        randomScope(scope, random, dense ? 100 : 8 + seed % 24, dense);
        GraphLayered layered{};
        graphLayeredBegin(layered, scope, options);
        graphLayeredBreakCycles(layered);
        graphLayeredRank(layered);
        graphLayeredBuild(layered);
        graphLayeredOrder(layered);
        for (uint32_t round = 0; round < 8; ++round) {
            if (round > 0)
                shuffle(layered, random);
            for (uint32_t gap = 0; gap + 1 < layered.layerCount; ++gap) {
                INFO("seed " << seed << " round " << round << " gap " << gap);
                CHECK(graphLayeredGapCrossings(layered, gap) == pairwiseGapCrossings(layered, gap));
                largeGaps += layered.gapSegmentStart.data[gap + 1] - layered.gapSegmentStart.data[gap] > 64 ? 1 : 0;
            }
            for (uint32_t trial = 0; trial < 16; ++trial) {
                uint32_t layer = random.next(layered.layerCount);
                uint32_t start = layered.layerStart.data[layer];
                uint32_t count = layered.layerStart.data[layer + 1] - start;
                uint32_t from = random.next(count);
                uint32_t to = random.next(count);
                if (random.next(2) == 0)
                    to = from + 1 < count ? from + 1 : from - (from > 0 ? 1 : 0);
                uint32_t vertex = layered.layerVertices.data[start + from];
                if (layered.vertices.data[vertex].kind == GraphVertexKind_Placeholder)
                    continue;
                if (to + 1 == from || from + 1 == to) {
                    uint32_t passed = layered.layerVertices.data[start + to];
                    uint64_t pairs =
                        uint64_t(layered.incidentStart.data[vertex + 1] - layered.incidentStart.data[vertex]) *
                        (layered.incidentStart.data[passed + 1] - layered.incidentStart.data[passed]);
                    largeMoves += pairs > 64 ? 1 : 0;
                }
                int64_t before = pairwiseBeside(layered, layer);
                int64_t delta = graphLayeredMoveDelta(layered, layer, from, to);
                std::vector<uint32_t> row(layered.layerVertices.data + start, layered.layerVertices.data + start + count);
                row.erase(row.begin() + from);
                row.insert(row.begin() + to, vertex);
                for (uint32_t i = 0; i < count; ++i)
                    layered.layerVertices.data[start + i] = row[i];
                setOrder(layered, layer);
                INFO("seed " << seed << " layer " << layer << " from " << from << " to " << to);
                CHECK(pairwiseBeside(layered, layer) - before == delta);
                ++moves;
            }
        }
        graphLayeredReset(layered);
        graphScopeReset(scope);
    }
    CHECK(moves > 1000);
    CHECK(largeGaps > 0);
    CHECK(largeMoves > 0);
}
