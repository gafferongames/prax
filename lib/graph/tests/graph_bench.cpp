// The cost of laying out graphs at the scales that diagrams reach and beyond: whole layouts through the API, by each
// placement, and each phase of the layered pipeline alone.
//
// ./dev.sh bench graph runs this and prints its rows. The graphs are generated the way diagrams tend to be: most edges
// run a few nodes forward in the order of description, a few run back and close cycles, and in some graphs runs of
// nodes sit in groups, nested up to two deep. The cost of a layout should grow little faster than the graph does, so a
// row that grows with the square of the size points at a defect.
//
// A phase is measured by running it again on a scope that the pipeline has laid out. Each phase rebuilds what it
// writes, so a run alone costs what it costs in the pipeline.
//
// Usage: graph_bench [--digest | case]
//
// If a case is named, only that case runs, repeatedly, for five minutes (harnessRepeat). This is for attaching a
// profiler. --digest prints a hash of each layout of smaller generated graphs instead, which a change that should not
// move anything leaves as it was.

#include "pr/graph.h"
#include "pr/string_view.h"

#include "graph/src/graph_layered.h"
#include "graph/src/graph_order.h"
#include "graph/src/graph_place.h"
#include "graph/src/graph_route.h"
#include "tools/harness/harness.h"

#include <stdio.h>
#include <string>
#include <vector>

using namespace harness;
using namespace pr_impl;

namespace {

// The numbers of leaves in the graphs.
constexpr uint32_t kSizes[] = {100, 1000, 5000};

// A deterministic pseudo-random sequence.
struct Sequence {
    uint32_t state;

    uint32_t next(uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % bound;
    }
};

// What a generated graph holds besides its leaves and edges.
struct Shape {
    bool groups;
    bool labels;
};

// A generated graph's description, in the order that the API takes it. Nodes come after their parents, and each edge's
// labels are together.
struct Description {
    std::vector<PrGraphNodeDesc> nodes;
    std::vector<PrGraphEdgeDesc> edges;
    std::vector<PrGraphLabelDesc> labels;
};

Description generate(uint32_t leaves, Shape shape) {
    Description d;
    Sequence random{leaves * 2 + (shape.groups ? 1 : 0)};
    std::vector<PrGraphNode> leafNodes;
    PrGraphNode outer = kPrGraphNodeNone;
    PrGraphNode inner = kPrGraphNodeNone;
    for (uint32_t i = 0; i < leaves; ++i) {
        if (shape.groups && i % 12 == 0) {
            // Every twelve leaves, a group closes and another opens, which is nested in an outer group a third of the
            // time.
            bool nested = random.next(3) == 0 && outer != kPrGraphNodeNone;
            if (!nested) {
                PrGraphNodeDesc group = prGraphNodeDescDefault();
                for (float& side : group.padding)
                    side = 8;
                group.padding[PrRectSide_Top] = 24;
                d.nodes.push_back(group);
                outer = PrGraphNode(d.nodes.size() - 1);
            }
            PrGraphNodeDesc group = prGraphNodeDescDefault();
            group.parent = outer;
            for (float& side : group.padding)
                side = 8;
            group.padding[PrRectSide_Top] = 24;
            d.nodes.push_back(group);
            inner = PrGraphNode(d.nodes.size() - 1);
        }
        PrGraphNodeDesc leaf = prGraphNodeDescDefault();
        leaf.extent = PrVec2{float(40 + random.next(80)), float(20 + random.next(30))};
        leaf.parent = shape.groups ? inner : kPrGraphNodeNone;
        d.nodes.push_back(leaf);
        leafNodes.push_back(PrGraphNode(d.nodes.size() - 1));
    }
    for (uint32_t i = 0; i < leaves; ++i) {
        uint32_t forward = 1 + random.next(2);
        for (uint32_t k = 0; k < forward && i + 1 < leaves; ++k) {
            uint32_t reach = leaves - 1 - i < 30 ? leaves - 1 - i : 30;
            uint32_t j = i + 1 + random.next(reach);
            d.edges.push_back(prGraphEdgeDescDefault(leafNodes[i], leafNodes[j]));
        }
        if (i > 0 && random.next(20) == 0) {
            uint32_t reach = i < 30 ? i : 30;
            d.edges.push_back(prGraphEdgeDescDefault(leafNodes[i], leafNodes[i - 1 - random.next(reach)]));
        }
    }
    if (shape.labels) {
        for (uint32_t e = 0; e < d.edges.size(); ++e) {
            if (e % 4 == 0)
                d.labels.push_back(PrGraphLabelDesc{.edge = e, .extent = PrVec2{60, 14}});
            if (e % 8 == 0) {
                d.labels.push_back(PrGraphLabelDesc{
                    .edge = e,
                    .extent = PrVec2{24, 12},
                    .placement = PrGraphLabelPlacement_Tail,
                });
                d.labels.push_back(PrGraphLabelDesc{
                    .edge = e,
                    .extent = PrVec2{24, 12},
                    .placement = PrGraphLabelPlacement_Head,
                });
            }
        }
    }
    return d;
}

void describe(PrGraph* graph, const Description& d) {
    prGraphClear(graph);
    for (const PrGraphNodeDesc& node : d.nodes)
        prGraphAddNode(graph, &node);
    for (const PrGraphEdgeDesc& edge : d.edges)
        prGraphAddEdge(graph, &edge);
    for (const PrGraphLabelDesc& label : d.labels)
        prGraphAddLabel(graph, &label);
}

// The scope that the driver builds for a graph whose groups share its direction: every node, edge and label.
void buildScope(GraphScope& scope, const Description& d, PrGraphDirection direction) {
    graphScopeClear(scope);
    scope.direction = direction;
    for (const PrGraphNodeDesc& node : d.nodes) {
        GraphScopeNode n{
            .extent = node.extent,
            .position = node.position,
            .parent = node.parent == kPrGraphNodeNone ? kGraphNone : node.parent,
            .layer = node.layer,
            .isProxy = false,
        };
        for (uint32_t s = 0; s < PrRectSide_Count; ++s)
            n.padding[s] = node.padding[s];
        scope.nodes.push(n);
    }
    for (uint32_t e = 0; e < d.edges.size(); ++e) {
        const PrGraphEdgeDesc& edge = d.edges[e];
        GraphScopeEdge scoped{
            .source = edge.source,
            .target = edge.target,
            .sourcePort = kGraphNone,
            .targetPort = kGraphNone,
            .weight = edge.weight,
            .minLength = edge.minLength,
            .labelStart = scope.labels.count,
            .labelCount = 0,
        };
        for (const PrGraphLabelDesc& label : d.labels) {
            if (label.edge != e)
                continue;
            scope.labels.push(GraphScopeLabel{.extent = label.extent, .placement = label.placement});
            ++scoped.labelCount;
        }
        scope.edges.push(scoped);
    }
    scope.nodeRects.resize(scope.nodes.count);
    scope.portPositions.resize(0);
    scope.labelRects.resize(scope.labels.count);
    scope.routeStart.resize(scope.edges.count + 1);
}

struct Graph {
    const char* name;
    Shape shape;
};

constexpr Graph kGraphs[] = {
    {"flat", {.groups = false, .labels = false}},
    {"grouped", {.groups = true, .labels = false}},
    {"labeled", {.groups = false, .labels = true}},
};

struct Placement {
    const char* name;
    PrGraphPlacement placement;
};

constexpr Placement kPlacements[] = {{"balanced", PrGraphPlacement_Balanced}, {"shortest", PrGraphPlacement_Shortest}};

struct Routing {
    const char* name;
    PrGraphRouting routing;
};

constexpr Routing kRoutings[] = {
    {"spline", PrGraphRouting_Spline},
    {"polyline", PrGraphRouting_Polyline},
    {"orthogonal", PrGraphRouting_Orthogonal},
};

// The numbers of leaves in the graphs whose layouts --digest prints.
constexpr uint32_t kDigestSizes[] = {50, 200, 1000};

// Folds `size` bytes into an FNV-1a hash.
uint64_t fold(uint64_t hash, const void* data, size_t size) {
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i)
        hash = (hash ^ bytes[i]) * 0x100000001b3ull;
    return hash;
}

// The hash of every result of the graph's layout.
uint64_t layoutHash(const PrGraph* graph, const Description& d) {
    uint64_t hash = 0xcbf29ce484222325ull;
    PrRect bounds = prGraphBounds(graph);
    hash = fold(hash, &bounds, sizeof(bounds));
    for (uint32_t v = 0; v < d.nodes.size(); ++v) {
        PrRect rect = prGraphNodeRect(graph, v);
        hash = fold(hash, &rect, sizeof(rect));
    }
    for (uint32_t l = 0; l < d.labels.size(); ++l) {
        PrRect rect = prGraphLabelRect(graph, l);
        hash = fold(hash, &rect, sizeof(rect));
    }
    for (uint32_t e = 0; e < d.edges.size(); ++e) {
        PrGraphRoute route = prGraphEdgeRoute(graph, e);
        hash = fold(hash, &route.pointCount, sizeof(route.pointCount));
        if (route.pointCount > 0)
            hash = fold(hash, route.points, sizeof(PrVec2) * route.pointCount);
    }
    return hash;
}

// Prints a hash of each layout of the generated graphs, by every placement and routing, fresh and interactive. A change
// that should not move anything leaves every hash as it was.
void printDigests() {
    PrGraph* graph = prGraphCreate();
    for (const Placement& p : kPlacements) {
        for (uint32_t size : kDigestSizes) {
            for (const Graph& g : kGraphs) {
                Description d = generate(size, g.shape);
                for (const Routing& r : kRoutings) {
                    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
                    options.routing = r.routing;
                    options.placement = p.placement;
                    describe(graph, d);
                    prGraphLayoutCompute(graph, &options);
                    printf("%s %u %s %s fresh %016llx\n", p.name, size, g.name, r.name,
                        static_cast<unsigned long long>(layoutHash(graph, d)));
                // An interactive layout that follows the fresh one, with every other node nudged across.
                    Description moved = d;
                    for (uint32_t v = 0; v < moved.nodes.size(); ++v) {
                        PrRect rect = prGraphNodeRect(graph, v);
                        moved.nodes[v].position = PrVec2{rect.left + float(v % 2) * 40, rect.top};
                    }
                    options.mode = PrGraphLayoutMode_Interactive;
                    describe(graph, moved);
                    prGraphLayoutCompute(graph, &options);
                    printf("%s %u %s %s interactive %016llx\n", p.name, size, g.name, r.name,
                        static_cast<unsigned long long>(layoutHash(graph, moved)));
                }
            }
        }
    }
    prGraphDestroy(graph);
}

} // namespace

int main(int argc, char** argv) {
    prCrashHandler();
    if (argc > 2) {
        fprintf(stderr, "usage: graph_bench [--digest | case]\n");
        return 2;
    }
    PrStringView only = argc == 2 ? argv[1] : PrStringView{};
    if (only == "--digest") {
        printDigests();
        return 0;
    }
    bool first = true;
    if (only.size == 0)
        printf("[");
    // Measures a case, or repeats it if it is the one named.
    auto measure = [&](const std::string& name, auto run) {
        if (only.size != 0) {
            if (only == PrStringView{name})
                harnessRepeat(run);
            return;
        }
        harnessWriteCase(first, name.c_str(), harnessMeasure(run));
        first = false;
        fflush(stdout);
    };
    PrGraph* graph = prGraphCreate();
    for (uint32_t size : kSizes) {
        for (const Graph& g : kGraphs) {
            Description d = generate(size, g.shape);
            std::string prefix = std::to_string(size) + " " + g.name + " ";
            describe(graph, d);
            for (const Routing& r : kRoutings) {
                // Only the flat graph varies the routing. The others measure what groups and labels add to splines.
                bool flat = !g.shape.groups && !g.shape.labels;
                if (r.routing != PrGraphRouting_Spline && !flat)
                    continue;
                PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
                options.routing = r.routing;
                measure(prefix + "layout " + r.name, [&] { prGraphLayoutCompute(graph, &options); });
            }
            PrGraphLayoutOptions shortest = prGraphLayoutOptionsDefault();
            shortest.placement = PrGraphPlacement_Shortest;
            measure(prefix + "layout spline shortest", [&] { prGraphLayoutCompute(graph, &shortest); });

            // The phases of the spline layout.
            PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
            GraphScope scope{};
            buildScope(scope, d, options.direction);
            GraphLayered layered{};
            graphLayeredBegin(layered, scope, options);
            graphLayeredBreakCycles(layered);
            graphLayeredRank(layered);
            graphLayeredBuild(layered);
            graphLayeredOrder(layered);
            graphLayeredPlace(layered);
            graphLayeredRoute(layered, scope);
            measure(prefix + "rank", [&] {
                graphLayeredBegin(layered, scope, options);
                graphLayeredBreakCycles(layered);
                graphLayeredRank(layered);
            });
            measure(prefix + "order", [&] {
                graphLayeredBuild(layered);
                graphLayeredOrder(layered);
            });
            measure(prefix + "place", [&] { graphLayeredPlace(layered); });
            layered.options.placement = PrGraphPlacement_Shortest;
            measure(prefix + "place shortest", [&] { graphLayeredPlace(layered); });
            layered.options.placement = options.placement;
            graphLayeredPlace(layered);
            measure(prefix + "route", [&] { graphLayeredRoute(layered, scope); });
            graphLayeredReset(layered);
            graphScopeReset(scope);
        }
    }
    prGraphDestroy(graph);
    if (only.size == 0)
        printf("\n]\n");
    return 0;
}
