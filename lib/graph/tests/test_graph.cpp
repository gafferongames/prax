#include "pr/float.h"
#include "pr/graph.h"
#include "pr/mem.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

extern "C" int prTestGraphCApi(void);

namespace pr_impl {

namespace {

constexpr float kTolerance = 0.01f;

// A graph under test, built and laid out through the public API.
struct TestGraph {
    PrGraph* graph = prGraphCreate();
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();

    TestGraph() = default;
    TestGraph(const TestGraph&) = delete;
    TestGraph& operator=(const TestGraph&) = delete;
    ~TestGraph() {
        prGraphDestroy(graph);
    }

    PrGraphNode node(float width = 40, float height = 20, PrGraphNode parent = kPrGraphNodeNone) {
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.extent = PrVec2{width, height};
        desc.parent = parent;
        return prGraphAddNode(graph, &desc);
    }

    PrGraphNode group(PrGraphNode parent = kPrGraphNodeNone, float padding = 10,
            PrGraphDirection direction = PrGraphDirection_Inherit) {
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.parent = parent;
        desc.direction = direction;
        for (float& side : desc.padding)
            side = padding;
        return prGraphAddNode(graph, &desc);
    }

    PrGraphEdge edge(PrGraphNode source, PrGraphNode target) {
        PrGraphEdgeDesc desc = prGraphEdgeDescDefault(source, target);
        return prGraphAddEdge(graph, &desc);
    }

    PrGraphEdge edge(PrGraphNode source, PrGraphPort sourcePort, PrGraphNode target, PrGraphPort targetPort) {
        PrGraphEdgeDesc desc = prGraphEdgeDescDefault(source, target);
        desc.sourcePort = sourcePort;
        desc.targetPort = targetPort;
        return prGraphAddEdge(graph, &desc);
    }

    PrGraphPort port(PrGraphNode node, PrGraphPortConstraint constraint, PrRectSide side, float offset = 0) {
        PrGraphPortDesc desc{.node = node, .constraint = constraint, .side = side, .offset = offset};
        return prGraphAddPort(graph, &desc);
    }

    PrGraphLabel label(PrGraphEdge edge, float width, float height,
            PrGraphLabelPlacement placement = PrGraphLabelPlacement_Center) {
        PrGraphLabelDesc desc{.edge = edge, .extent = PrVec2{width, height}, .placement = placement};
        return prGraphAddLabel(graph, &desc);
    }

    void layout() {
        prGraphLayoutCompute(graph, &options);
    }

    PrRect rect(PrGraphNode node) const {
        return prGraphNodeRect(graph, node);
    }

    std::vector<PrVec2> route(PrGraphEdge edge) const {
        PrGraphRoute route = prGraphEdgeRoute(graph, edge);
        return std::vector<PrVec2>(route.points, route.points + route.pointCount);
    }
};

PrVec2 center(PrRect rect) {
    return PrVec2{(rect.left + rect.right) * 0.5f, (rect.top + rect.bottom) * 0.5f};
}

bool overlaps(PrRect a, PrRect b) {
    return a.left < b.right - kTolerance && b.left < a.right - kTolerance && a.top < b.bottom - kTolerance &&
        b.top < a.bottom - kTolerance;
}

bool contains(PrRect outer, PrRect inner) {
    return inner.left >= outer.left - kTolerance && inner.right <= outer.right + kTolerance &&
        inner.top >= outer.top - kTolerance && inner.bottom <= outer.bottom + kTolerance;
}

bool near(PrVec2 a, PrVec2 b, float tolerance = 0.05f) {
    return prF32Abs(a.x - b.x) <= tolerance && prF32Abs(a.y - b.y) <= tolerance;
}

// Returns true if `end` lies on the rectangle's boundary, and the route's step from `end` to `next` runs along the line
// through the rectangle's center, away from the center.
bool meetsTowardCenter(PrVec2 end, PrVec2 next, PrRect rect) {
    bool onSide = prF32Abs(end.y - rect.bottom) < kTolerance || prF32Abs(end.y - rect.top) < kTolerance ||
        prF32Abs(end.x - rect.left) < kTolerance || prF32Abs(end.x - rect.right) < kTolerance;
    PrVec2 out = next - end;
    PrVec2 in = center(rect) - end;
    float cross = out.x * in.y - out.y * in.x;
    float dot = out.x * in.x + out.y * in.y;
    return onSide && prF32Abs(cross) < 0.01f && dot < 0;
}

// The points along a route, with each cubic segment of a spline sampled.
std::vector<PrVec2> sampled(const std::vector<PrVec2>& route, PrGraphRouting routing) {
    if (routing != PrGraphRouting_Spline || route.size() < 4)
        return route;
    std::vector<PrVec2> out{route[0]};
    for (size_t i = 0; i + 3 < route.size(); i += 3) {
        for (int k = 1; k <= 16; ++k) {
            float t = float(k) / 16;
            float u = 1 - t;
            PrVec2 p = route[i] * (u * u * u) + route[i + 1] * (3 * u * u * t) + route[i + 2] * (3 * u * t * t) +
                route[i + 3] * (t * t * t);
            out.push_back(p);
        }
    }
    return out;
}

// Returns true if the segment from `a` to `b` passes through the inside of `rect`, by sampling it.
bool crossesInside(PrVec2 a, PrVec2 b, PrRect rect) {
    PrRect inside{rect.left + 0.5f, rect.top + 0.5f, rect.right - 0.5f, rect.bottom - 0.5f};
    if (inside.right <= inside.left || inside.bottom <= inside.top)
        return false;
    for (int k = 0; k <= 32; ++k) {
        float t = float(k) / 32;
        PrVec2 p = a + (b - a) * t;
        if (p.x > inside.left && p.x < inside.right && p.y > inside.top && p.y < inside.bottom)
            return true;
    }
    return false;
}

// The graph's description, read back for checks: each node's parent, and each edge's ends.
struct Shape {
    std::vector<PrGraphNode> parents;
    std::vector<std::pair<PrGraphNode, PrGraphNode>> edges;

    bool isGroup(PrGraphNode node) const {
        for (PrGraphNode parent : parents) {
            if (parent == node)
                return true;
        }
        return false;
    }

    bool isAncestor(PrGraphNode ancestor, PrGraphNode node) const {
        for (PrGraphNode a = parents[node]; a != kPrGraphNodeNone; a = parents[a]) {
            if (a == ancestor)
                return true;
        }
        return false;
    }
};

// Checks what every layout promises. Nodes do not overlap unless one holds the other. Groups hold their descendants and
// no other leaf. Routes avoid every leaf except their own ends, and start and end at those ends.
void checkInvariants(const TestGraph& test, const Shape& shape) {
    uint32_t n = uint32_t(shape.parents.size());
    for (uint32_t a = 0; a < n; ++a) {
        for (uint32_t b = a + 1; b < n; ++b) {
            if (shape.isAncestor(a, b) || shape.isAncestor(b, a))
                continue;
            INFO("nodes " << a << " and " << b);
            CHECK_FALSE(overlaps(test.rect(a), test.rect(b)));
        }
    }
    for (uint32_t g = 0; g < n; ++g) {
        if (!shape.isGroup(g))
            continue;
        for (uint32_t v = 0; v < n; ++v) {
            if (shape.isAncestor(g, v)) {
                INFO("group " << g << " holds " << v);
                CHECK(contains(test.rect(g), test.rect(v)));
            }
        }
    }
    PrRect bounds = prGraphBounds(test.graph);
    for (uint32_t e = 0; e < shape.edges.size(); ++e) {
        std::vector<PrVec2> route = test.route(e);
        REQUIRE(route.size() >= 2);
        std::vector<PrVec2> points = sampled(route, test.options.routing);
        PrGraphNode source = shape.edges[e].first;
        PrGraphNode target = shape.edges[e].second;
        for (uint32_t v = 0; v < n; ++v) {
            if (shape.isGroup(v) || v == source || v == target)
                continue;
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                INFO("edge " << e << " step " << i << " and node " << v);
                CHECK_FALSE(crossesInside(points[i], points[i + 1], test.rect(v)));
            }
        }
        for (PrVec2 p : route) {
            CHECK(p.x >= bounds.left - kTolerance);
            CHECK(p.y >= bounds.top - kTolerance);
            CHECK(p.x <= bounds.right + kTolerance);
            CHECK(p.y <= bounds.bottom + kTolerance);
        }
    }
}

// A deterministic pseudo-random sequence.
struct Sequence {
    uint32_t state;

    uint32_t next(uint32_t bound) {
        state = state * 1664525u + 1013904223u;
        return (state >> 8) % bound;
    }
};

// Describes a random graph of `nodes` leaves, some in groups, with forward and backward edges.
Shape randomGraph(TestGraph& test, Sequence& random, uint32_t nodes, bool groups) {
    Shape shape;
    std::vector<PrGraphNode> leaves;
    std::vector<PrGraphNode> containers{kPrGraphNodeNone};
    for (uint32_t i = 0; i < nodes; ++i) {
        PrGraphNode parent = containers[random.next(uint32_t(containers.size()))];
        if (groups && random.next(5) == 0) {
            PrGraphNode group = test.group(parent, float(4 + random.next(12)));
            shape.parents.push_back(parent);
            containers.push_back(group);
            parent = group;
        }
        PrGraphNode leaf = test.node(float(20 + random.next(60)), float(10 + random.next(30)), parent);
        shape.parents.push_back(parent);
        leaves.push_back(leaf);
    }
    uint32_t edges = nodes + nodes / 2;
    for (uint32_t i = 0; i < edges; ++i) {
        PrGraphNode a = leaves[random.next(uint32_t(leaves.size()))];
        PrGraphNode b = leaves[random.next(uint32_t(leaves.size()))];
        test.edge(a, b);
        shape.edges.emplace_back(a, b);
    }
    return shape;
}

// Returns true if the segments `a`-`b` and `c`-`d` cross at a point inside both.
bool segmentsCross(PrVec2 a, PrVec2 b, PrVec2 c, PrVec2 d) {
    auto side = [](PrVec2 p, PrVec2 q, PrVec2 r) {
        return (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
    };
    float d1 = side(c, d, a);
    float d2 = side(c, d, b);
    float d3 = side(a, b, c);
    float d4 = side(a, b, d);
    return ((d1 > 0.01f && d2 < -0.01f) || (d1 < -0.01f && d2 > 0.01f)) &&
        ((d3 > 0.01f && d4 < -0.01f) || (d3 < -0.01f && d4 > 0.01f));
}

// The number of points where the routes of two different edges cross.
uint32_t routeCrossings(const TestGraph& test, uint32_t edgeCount) {
    std::vector<std::vector<PrVec2>> routes;
    for (uint32_t e = 0; e < edgeCount; ++e)
        routes.push_back(sampled(test.route(e), test.options.routing));
    uint32_t crossings = 0;
    for (uint32_t e = 0; e < edgeCount; ++e) {
        for (uint32_t f = e + 1; f < edgeCount; ++f) {
            for (size_t i = 0; i + 1 < routes[e].size(); ++i) {
                for (size_t j = 0; j + 1 < routes[f].size(); ++j) {
                    if (segmentsCross(routes[e][i], routes[e][i + 1], routes[f][j], routes[f][j + 1]))
                        ++crossings;
                }
            }
        }
    }
    return crossings;
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("graph: the public header compiles as C") {
    CHECK(prTestGraphCApi() == 0);
}

TEST_CASE("graph: an edge runs in the layout's direction, from its source's side to its target's") {
    const PrGraphDirection directions[] = {
        PrGraphDirection_Down, PrGraphDirection_Up, PrGraphDirection_Right, PrGraphDirection_Left,
    };
    for (PrGraphDirection direction : directions) {
        TestGraph test;
        test.options.direction = direction;
        test.options.routing = PrGraphRouting_Polyline;
        PrGraphNode a = test.node();
        PrGraphNode b = test.node();
        PrGraphEdge e = test.edge(a, b);
        test.layout();
        PrVec2 ca = center(test.rect(a));
        PrVec2 cb = center(test.rect(b));
        INFO("direction " << int(direction));
        switch (direction) {
            case PrGraphDirection_Down:
                CHECK(cb.y > ca.y);
                break;
            case PrGraphDirection_Up:
                CHECK(cb.y < ca.y);
                break;
            case PrGraphDirection_Right:
                CHECK(cb.x > ca.x);
                break;
            default:
                CHECK(cb.x < ca.x);
                break;
        }
        std::vector<PrVec2> route = test.route(e);
        REQUIRE(route.size() >= 2);
        // Without ports, the ends meet the rectangles on lines toward the centers.
        CHECK(meetsTowardCenter(route[0], route[1], test.rect(a)));
        CHECK(meetsTowardCenter(route[route.size() - 1], route[route.size() - 2], test.rect(b)));
        PrRect bounds = prGraphBounds(test.graph);
        CHECK(bounds.left == 0);
        CHECK(bounds.top == 0);
    }
}

TEST_CASE("graph: the layer spacing separates layers, and the node spacing separates neighbors") {
    TestGraph test;
    test.options.layerSpacing = 30;
    test.options.nodeSpacing = 20;
    PrGraphNode a = test.node(40, 20);
    PrGraphNode b = test.node(40, 20);
    PrGraphNode c = test.node(40, 20);
    test.edge(a, b);
    test.edge(a, c);
    test.layout();
    CHECK(test.rect(b).top - test.rect(a).bottom == doctest::Approx(30));
    CHECK(test.rect(b).top == doctest::Approx(test.rect(c).top));
    PrRect left = test.rect(b).left < test.rect(c).left ? test.rect(b) : test.rect(c);
    PrRect right = test.rect(b).left < test.rect(c).left ? test.rect(c) : test.rect(b);
    CHECK(right.left - left.right == doctest::Approx(20).epsilon(0.01));
    // The source sits centered over its two targets.
    CHECK(center(test.rect(a)).x == doctest::Approx((center(left).x + center(right).x) * 0.5f).epsilon(0.01));
}

TEST_CASE("graph: a long edge runs straight past a node in its way") {
    TestGraph test;
    test.options.routing = PrGraphRouting_Polyline;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphNode c = test.node();
    test.edge(a, b);
    test.edge(b, c);
    PrGraphEdge longEdge = test.edge(a, c);
    test.layout();
    Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone}, {{a, b}, {b, c}, {a, c}}};
    checkInvariants(test, shape);
    CHECK(test.route(longEdge).size() >= 2);
}

TEST_CASE("graph: a cycle is laid out with one edge reversed, still routed from source to target") {
    TestGraph test;
    test.options.routing = PrGraphRouting_Polyline;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphNode c = test.node();
    test.edge(a, b);
    test.edge(b, c);
    PrGraphEdge back = test.edge(c, a);
    test.layout();
    Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone}, {{a, b}, {b, c}, {c, a}}};
    checkInvariants(test, shape);
    CHECK(center(test.rect(a)).y < center(test.rect(b)).y);
    CHECK(center(test.rect(b)).y < center(test.rect(c)).y);
    std::vector<PrVec2> route = test.route(back);
    CHECK(near(route.front(), center(test.rect(c)), 30));
    CHECK(near(route.back(), center(test.rect(a)), 30));
}

TEST_CASE("graph: fixed ports keep their positions, and routes meet them") {
    TestGraph test;
    test.options.direction = PrGraphDirection_Right;
    PrGraphNode a = test.node(60, 40);
    PrGraphNode b = test.node(60, 40);
    PrGraphPort out = test.port(a, PrGraphPortConstraint_FixedPosition, PrRectSide_Right, 10);
    PrGraphPort in = test.port(b, PrGraphPortConstraint_FixedPosition, PrRectSide_Left, 30);
    PrGraphEdge e = test.edge(a, out, b, in);
    test.layout();
    PrRect ra = test.rect(a);
    PrRect rb = test.rect(b);
    PrVec2 outAt = prGraphPortPosition(test.graph, out);
    PrVec2 inAt = prGraphPortPosition(test.graph, in);
    CHECK(near(outAt, PrVec2{ra.right, ra.top + 10}));
    CHECK(near(inAt, PrVec2{rb.left, rb.top + 30}));
    std::vector<PrVec2> route = test.route(e);
    CHECK(near(route.front(), outAt));
    CHECK(near(route.back(), inAt));
    // Placement lines the ports up, so that the edge runs straight.
    CHECK(outAt.y == doctest::Approx(inAt.y));
}

TEST_CASE("graph: ports that the layout places spread along their sides in the order that avoids crossings") {
    TestGraph test;
    PrGraphNode hub = test.node(120, 20);
    PrGraphNode left = test.node();
    PrGraphNode right = test.node();
    PrGraphNode under = test.node();
    // The ports are described in the order that would cross.
    PrGraphPort p0 = test.port(hub, PrGraphPortConstraint_FixedSide, PrRectSide_Bottom);
    PrGraphPort p1 = test.port(hub, PrGraphPortConstraint_FixedSide, PrRectSide_Bottom);
    test.edge(left, under);
    test.edge(right, under);
    test.edge(hub, p0, right, kPrGraphPortNone);
    test.edge(hub, p1, left, kPrGraphPortNone);
    test.layout();
    PrVec2 a = prGraphPortPosition(test.graph, p0);
    PrVec2 b = prGraphPortPosition(test.graph, p1);
    CHECK(a.y == doctest::Approx(test.rect(hub).bottom));
    CHECK(b.y == doctest::Approx(test.rect(hub).bottom));
    bool rightIsRight = center(test.rect(right)).x > center(test.rect(left)).x;
    CHECK((a.x > b.x) == rightIsRight);
}

TEST_CASE("graph: a free port takes the side that faces its edges") {
    TestGraph test;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphPort in = test.port(b, PrGraphPortConstraint_Free, PrRectSide_Bottom);
    test.edge(a, kPrGraphPortNone, b, in);
    test.layout();
    CHECK(prGraphPortPosition(test.graph, in).y == doctest::Approx(test.rect(b).top));
}

TEST_CASE("graph: a port that faces away from its edge routes around its node") {
    for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Orthogonal, PrGraphRouting_Spline}) {
        TestGraph test;
        test.options.routing = routing;
        test.options.direction = PrGraphDirection_Right;
        PrGraphNode a = test.node(60, 40);
        PrGraphNode b = test.node(60, 40);
        PrGraphNode c = test.node(60, 40);
        // An output on the right feeding back to an input on the left: the classic feedback loop of an editor.
        PrGraphPort out = test.port(c, PrGraphPortConstraint_FixedPosition, PrRectSide_Right, 20);
        PrGraphPort in = test.port(a, PrGraphPortConstraint_FixedPosition, PrRectSide_Left, 20);
        test.edge(a, b);
        test.edge(b, c);
        PrGraphEdge back = test.edge(c, out, a, in);
        test.layout();
        Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone}, {{a, b}, {b, c}, {c, a}}};
        checkInvariants(test, shape);
        std::vector<PrVec2> route = test.route(back);
        CHECK(near(route.front(), prGraphPortPosition(test.graph, out)));
        CHECK(near(route.back(), prGraphPortPosition(test.graph, in)));
    }
}

TEST_CASE("graph: a self-loop runs beside its node, clear of its neighbors") {
    for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Orthogonal, PrGraphRouting_Spline}) {
        TestGraph test;
        test.options.routing = routing;
        PrGraphNode a = test.node();
        PrGraphNode b = test.node();
        PrGraphNode c = test.node();
        test.edge(a, b);
        test.edge(a, c);
        PrGraphEdge loop = test.edge(b, b);
        test.layout();
        Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone}, {{a, b}, {a, c}, {b, b}}};
        checkInvariants(test, shape);
        std::vector<PrVec2> route = test.route(loop);
        CHECK(route.size() >= 4);
    }
}

TEST_CASE("graph: a center label sits on its edge in space of its own") {
    TestGraph test;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphNode c = test.node();
    PrGraphEdge e = test.edge(a, b);
    test.edge(a, c);
    PrGraphLabel label = test.label(e, 50, 14);
    test.layout();
    PrRect rect = prGraphLabelRect(test.graph, label);
    CHECK(rect.right - rect.left == doctest::Approx(50));
    CHECK(rect.bottom - rect.top == doctest::Approx(14));
    for (PrGraphNode v : {a, b, c})
        CHECK_FALSE(overlaps(rect, test.rect(v)));
    // The edge runs through the label's center.
    std::vector<PrVec2> points = sampled(test.route(e), test.options.routing);
    bool through = false;
    for (PrVec2 p : points)
        through = through || near(p, center(rect), 8);
    CHECK(through);
}

TEST_CASE("graph: a group holds its children, and keeps other nodes out") {
    for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Spline}) {
        TestGraph test;
        test.options.routing = routing;
        PrGraphNode outside = test.node();
        PrGraphNode g = test.group(kPrGraphNodeNone, 12);
        PrGraphNode a = test.node(40, 20, g);
        PrGraphNode b = test.node(40, 20, g);
        PrGraphNode after = test.node();
        test.edge(outside, a);
        test.edge(a, b);
        test.edge(outside, after);
        test.edge(b, after);
        test.layout();
        Shape shape{
            {kPrGraphNodeNone, kPrGraphNodeNone, g, g, kPrGraphNodeNone},
            {{outside, a}, {a, b}, {outside, after}, {b, after}},
        };
        checkInvariants(test, shape);
        PrRect group = test.rect(g);
        CHECK(test.rect(a).top - group.top >= 12 - kTolerance);
        CHECK(group.bottom - test.rect(b).bottom >= 12 - kTolerance);
    }
}

TEST_CASE("graph: an edge may join a group itself") {
    TestGraph test;
    PrGraphNode a = test.node();
    PrGraphNode g = test.group();
    PrGraphNode b = test.node(40, 20, g);
    PrGraphNode c = test.node(40, 20, g);
    PrGraphEdge into = test.edge(a, g);
    test.edge(b, c);
    test.layout();
    Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, g, g}, {{a, g}, {b, c}}};
    checkInvariants(test, shape);
    CHECK(test.rect(g).top > test.rect(a).bottom);
    std::vector<PrVec2> route = test.route(into);
    CHECK(route.back().y == doctest::Approx(test.rect(g).top));
}

TEST_CASE("graph: a group with a direction of its own is laid out in it, and edges cross its border") {
    TestGraph test;
    test.options.routing = PrGraphRouting_Polyline;
    PrGraphNode top = test.node();
    PrGraphNode g = test.group(kPrGraphNodeNone, 8, PrGraphDirection_Right);
    PrGraphNode a = test.node(40, 20, g);
    PrGraphNode b = test.node(40, 20, g);
    PrGraphNode c = test.node(40, 20, g);
    PrGraphNode bottom = test.node();
    PrGraphEdge in = test.edge(top, a);
    test.edge(a, b);
    test.edge(b, c);
    PrGraphEdge out = test.edge(c, bottom);
    test.layout();
    Shape shape{
        {kPrGraphNodeNone, kPrGraphNodeNone, g, g, g, kPrGraphNodeNone},
        {{top, a}, {a, b}, {b, c}, {c, bottom}},
    };
    checkInvariants(test, shape);
    // The flow runs right inside the group and down outside it.
    CHECK(test.rect(b).left > test.rect(a).right);
    CHECK(test.rect(c).left > test.rect(b).right);
    CHECK(test.rect(g).top > test.rect(top).bottom);
    CHECK(test.rect(bottom).top > test.rect(g).bottom);
    std::vector<PrVec2> inRoute = test.route(in);
    std::vector<PrVec2> outRoute = test.route(out);
    CHECK(near(inRoute.front(), center(test.rect(top)), 30));
    CHECK(near(inRoute.back(), center(test.rect(a)), 30));
    CHECK(near(outRoute.front(), center(test.rect(c)), 30));
    CHECK(near(outRoute.back(), center(test.rect(bottom)), 30));
}

TEST_CASE("graph: layer constraints put nodes first and last") {
    TestGraph test;
    PrGraphNodeDesc desc = prGraphNodeDescDefault();
    desc.extent = PrVec2{40, 20};
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    desc.layer = PrGraphLayerConstraint_First;
    PrGraphNode first = prGraphAddNode(test.graph, &desc);
    desc.layer = PrGraphLayerConstraint_Last;
    PrGraphNode last = prGraphAddNode(test.graph, &desc);
    test.edge(a, b);
    test.edge(b, first);
    test.edge(last, a);
    test.layout();
    for (PrGraphNode v : {a, b, last})
        CHECK(test.rect(first).bottom < test.rect(v).top);
    for (PrGraphNode v : {a, b, first})
        CHECK(test.rect(last).top > test.rect(v).bottom);
}

TEST_CASE("graph: an interactive layout keeps the arrangement's order") {
    for (bool swapped : {false, true}) {
        TestGraph test;
        test.options.mode = PrGraphLayoutMode_Interactive;
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.extent = PrVec2{40, 20};
        desc.position = PrVec2{100, 0};
        PrGraphNode root = prGraphAddNode(test.graph, &desc);
        desc.position = PrVec2{swapped ? 300.0f : 0.0f, 100};
        PrGraphNode x = prGraphAddNode(test.graph, &desc);
        desc.position = PrVec2{swapped ? 0.0f : 300.0f, 100};
        PrGraphNode y = prGraphAddNode(test.graph, &desc);
        test.edge(root, x);
        test.edge(root, y);
        test.layout();
        CHECK((test.rect(x).left < test.rect(y).left) == !swapped);
    }
}

TEST_CASE("graph: disconnected components are packed side by side") {
    TestGraph test;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphNode c = test.node();
    PrGraphNode d = test.node();
    test.edge(a, b);
    test.edge(c, d);
    test.layout();
    Shape shape{{kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone, kPrGraphNodeNone}, {{a, b}, {c, d}}};
    checkInvariants(test, shape);
    CHECK(test.rect(c).left >= test.rect(a).right + test.options.componentSpacing - kTolerance);
}

TEST_CASE("graph: the same graph lays out the same, and random graphs keep the invariants") {
    for (uint32_t seed = 1; seed <= 128; ++seed) {
        for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Orthogonal, PrGraphRouting_Spline}) {
            PrGraphDirection direction = PrGraphDirection(PrGraphDirection_Down + seed % 4);
            PrGraphPlacement placement = seed % 8 < 4 ? PrGraphPlacement_Balanced : PrGraphPlacement_Shortest;
            std::vector<PrVec2> first;
            for (int run = 0; run < 2; ++run) {
                TestGraph test;
                test.options.routing = routing;
                test.options.direction = direction;
                test.options.placement = placement;
                Sequence random{seed};
                Shape shape = randomGraph(test, random, 6 + seed % 10, seed % 2 == 0);
                test.layout();
                INFO("seed " << seed << " routing " << int(routing) << " placement " << int(placement));
                checkInvariants(test, shape);
                std::vector<PrVec2> all;
                for (uint32_t v = 0; v < shape.parents.size(); ++v) {
                    PrRect r = test.rect(v);
                    all.push_back(PrVec2{r.left, r.top});
                    all.push_back(PrVec2{r.right, r.bottom});
                }
                for (uint32_t e = 0; e < shape.edges.size(); ++e) {
                    std::vector<PrVec2> route = test.route(e);
                    all.insert(all.end(), route.begin(), route.end());
                }
                if (run == 0) {
                    first = all;
                } else {
                    REQUIRE(first.size() == all.size());
                    CHECK(prMemEquals(first.data(), all.data(), first.size() * sizeof(PrVec2)));
                }
            }
        }
    }
}

TEST_CASE("graph: parallel edges without ports take routes of their own") {
    TestGraph test;
    PrGraphNode a = test.node();
    PrGraphNode b = test.node();
    PrGraphEdge first = test.edge(a, b);
    PrGraphEdge second = test.edge(a, b);
    test.layout();
    std::vector<PrVec2> one = sampled(test.route(first), test.options.routing);
    std::vector<PrVec2> two = sampled(test.route(second), test.options.routing);
    bool apart = one.size() != two.size();
    for (size_t i = 0; !apart && i < one.size(); ++i)
        apart = !near(one[i], two[i], 1);
    CHECK(apart);
}

TEST_CASE("graph: an edge between nodes outside a group stays out of the group") {
    for (uint32_t seed = 1; seed <= 24; ++seed) {
        TestGraph test;
        test.options.routing = PrGraphRouting_Polyline;
        Sequence random{seed};
        Shape shape = randomGraph(test, random, 8 + seed % 8, true);
        test.layout();
        for (uint32_t g = 0; g < shape.parents.size(); ++g) {
            if (!shape.isGroup(g))
                continue;
            for (uint32_t e = 0; e < shape.edges.size(); ++e) {
                auto [source, target] = shape.edges[e];
                if (shape.isAncestor(g, source) || shape.isAncestor(g, target))
                    continue;
                std::vector<PrVec2> route = test.route(e);
                for (size_t i = 0; i + 1 < route.size(); ++i) {
                    INFO("seed " << seed << " group " << g << " edge " << e);
                    CHECK_FALSE(crossesInside(route[i], route[i + 1], test.rect(g)));
                }
            }
        }
    }
}

TEST_CASE("graph: a spline turns smoothly where it runs along a lane") {
    TestGraph test;
    test.options.direction = PrGraphDirection_Right;
    PrGraphNode a = test.node(60, 40);
    PrGraphNode b = test.node(60, 40);
    PrGraphPort out = test.port(b, PrGraphPortConstraint_FixedPosition, PrRectSide_Right, 20);
    PrGraphPort in = test.port(a, PrGraphPortConstraint_FixedPosition, PrRectSide_Left, 20);
    test.edge(a, b);
    PrGraphEdge back = test.edge(b, out, a, in);
    test.layout();
    // At each join of two cubic segments, the incoming and outgoing tangents point the same way.
    std::vector<PrVec2> route = test.route(back);
    for (size_t i = 3; i + 1 < route.size(); i += 3) {
        PrVec2 incoming = route[i] - route[i - 1];
        PrVec2 outgoing = route[i + 1] - route[i];
        float cross = incoming.x * outgoing.y - incoming.y * outgoing.x;
        float dot = incoming.x * outgoing.x + incoming.y * outgoing.y;
        INFO("join " << i);
        CHECK(prF32Abs(cross) < 0.01f);
        CHECK(dot >= 0);
    }
}

TEST_CASE("graph: orthogonal routes cross no more than straight ones") {
    uint32_t orthogonal = 0;
    uint32_t straight = 0;
    for (uint32_t seed = 1; seed <= 24; ++seed) {
        for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Orthogonal}) {
            TestGraph test;
            test.options.routing = routing;
            Sequence random{seed};
            Shape shape = randomGraph(test, random, 8 + seed % 8, false);
            test.layout();
            uint32_t crossings = routeCrossings(test, uint32_t(shape.edges.size()));
            if (routing == PrGraphRouting_Orthogonal)
                orthogonal += crossings;
            else
                straight += crossings;
        }
    }
    CHECK(orthogonal <= straight);
}

TEST_CASE("graph: head and tail labels overlap no node and no other label") {
    for (uint32_t seed = 1; seed <= 24; ++seed) {
        TestGraph test;
        Sequence random{seed};
        Shape shape = randomGraph(test, random, 8 + seed % 8, false);
        std::vector<PrGraphLabel> labels;
        for (uint32_t e = 0; e < shape.edges.size(); ++e) {
            labels.push_back(test.label(e, 30, 12, PrGraphLabelPlacement_Tail));
            labels.push_back(test.label(e, 30, 12, PrGraphLabelPlacement_Head));
        }
        test.layout();
        for (PrGraphLabel label : labels) {
            for (uint32_t v = 0; v < shape.parents.size(); ++v) {
                INFO("seed " << seed << " label " << label << " node " << v);
                CHECK_FALSE(overlaps(prGraphLabelRect(test.graph, label), test.rect(v)));
            }
            for (PrGraphLabel other : labels) {
                INFO("seed " << seed << " labels " << label << " and " << other);
                if (other != label)
                    CHECK_FALSE(overlaps(prGraphLabelRect(test.graph, label), prGraphLabelRect(test.graph, other)));
            }
        }
    }
}

TEST_CASE("graph: a group's self-loop runs around the group, outside its border") {
    for (PrGraphRouting routing : {PrGraphRouting_Polyline, PrGraphRouting_Orthogonal, PrGraphRouting_Spline}) {
        for (PrGraphPlacement placement : {PrGraphPlacement_Balanced, PrGraphPlacement_Shortest}) {
            TestGraph test;
            test.options.routing = routing;
            test.options.placement = placement;
            PrGraphNode outer = test.group(kPrGraphNodeNone, 10);
            PrGraphNode g = test.group(outer, 10);
            PrGraphNode a = test.node(40, 20, g);
            PrGraphNode b = test.node(40, 20, g);
            PrGraphNode beside = test.node(40, 20, outer);
            test.edge(a, b);
            PrGraphEdge loop = test.edge(g, g);
            test.layout();
            INFO("routing " << int(routing) << " placement " << int(placement));
            Shape shape{{kPrGraphNodeNone, outer, g, g, outer}, {{a, b}, {g, g}}};
            checkInvariants(test, shape);
            PrRect rect = test.rect(g);
            std::vector<PrVec2> route = test.route(loop);
            REQUIRE(route.size() >= 4);
            // The loop leaves and returns at the group's border, and nowhere enters the group.
            auto onBorder = [&](PrVec2 p) {
                return contains(rect, PrRect{p.x, p.y, p.x, p.y}) &&
                    (prF32Abs(p.x - rect.left) < kTolerance || prF32Abs(p.x - rect.right) < kTolerance ||
                        prF32Abs(p.y - rect.top) < kTolerance || prF32Abs(p.y - rect.bottom) < kTolerance);
            };
            CHECK(onBorder(route.front()));
            CHECK(onBorder(route.back()));
            std::vector<PrVec2> points = sampled(route, routing);
            for (size_t i = 0; i + 1 < points.size(); ++i) {
                INFO("step " << i);
                CHECK_FALSE(crossesInside(points[i], points[i + 1], rect));
                CHECK_FALSE(crossesInside(points[i], points[i + 1], test.rect(beside)));
                CHECK(contains(test.rect(outer), PrRect{points[i].x, points[i].y, points[i].x, points[i].y}));
            }
        }
    }
}

TEST_CASE("graph: tail and head labels stay at their ends when an edge crosses a group laid out on its own") {
    TestGraph test;
    PrGraphNode before = test.node();
    PrGraphNode g = test.group(kPrGraphNodeNone, 8, PrGraphDirection_Right);
    PrGraphNode a = test.node(40, 20, g);
    PrGraphNode b = test.node(40, 20, g);
    PrGraphNode c = test.node(40, 20, g);
    PrGraphNode after = test.node();
    test.edge(a, b);
    test.edge(b, c);
    // The edge out of the group leaves by its far side, past `c`, and the edge in enters by its near side, before `a`.
    PrGraphEdge out = test.edge(a, after);
    PrGraphEdge in = test.edge(before, c);
    PrGraphLabel outTail = test.label(out, 20, 10, PrGraphLabelPlacement_Tail);
    PrGraphLabel outHead = test.label(out, 20, 10, PrGraphLabelPlacement_Head);
    PrGraphLabel inTail = test.label(in, 20, 10, PrGraphLabelPlacement_Tail);
    PrGraphLabel inHead = test.label(in, 20, 10, PrGraphLabelPlacement_Head);
    test.layout();
    Shape shape{
        {kPrGraphNodeNone, kPrGraphNodeNone, g, g, g, kPrGraphNodeNone},
        {{a, b}, {b, c}, {a, after}, {before, c}},
    };
    checkInvariants(test, shape);
    // The distance between two rectangles, or 0 if they touch or overlap.
    auto distance = [](PrRect p, PrRect q) {
        float dx = p.left - q.right > q.left - p.right ? p.left - q.right : q.left - p.right;
        float dy = p.top - q.bottom > q.top - p.bottom ? p.top - q.bottom : q.top - p.bottom;
        float d = dx > dy ? dx : dy;
        return d > 0 ? d : 0;
    };
    // Each label lies within its own extent of its end's node, and a label at an end inside the group lies inside it.
    float reach = 20 + test.options.labelSpacing;
    CHECK(distance(prGraphLabelRect(test.graph, outTail), test.rect(a)) <= reach);
    CHECK(distance(prGraphLabelRect(test.graph, outHead), test.rect(after)) <= reach);
    CHECK(distance(prGraphLabelRect(test.graph, inTail), test.rect(before)) <= reach);
    CHECK(distance(prGraphLabelRect(test.graph, inHead), test.rect(c)) <= reach);
    CHECK(contains(test.rect(g), prGraphLabelRect(test.graph, outTail)));
    CHECK(contains(test.rect(g), prGraphLabelRect(test.graph, inHead)));
    for (PrGraphLabel label : {outTail, outHead, inTail, inHead}) {
        for (PrGraphNode v : {before, a, b, c, after})
            CHECK_FALSE(overlaps(prGraphLabelRect(test.graph, label), test.rect(v)));
    }
}

#if defined(NDEBUG)
TEST_CASE("graph: a release build ignores an edge between a group and its descendant") {
    for (PrGraphDirection inner : {PrGraphDirection_Inherit, PrGraphDirection_Right}) {
        TestGraph test;
        PrGraphNode g = test.group(kPrGraphNodeNone, 10, inner);
        PrGraphNode a = test.node(40, 20, g);
        PrGraphNode b = test.node(40, 20, g);
        PrGraphNode c = test.node();
        test.edge(a, b);
        PrGraphEdge down = test.edge(g, a);
        PrGraphEdge up = test.edge(b, g);
        PrGraphLabel label = test.label(down, 20, 10);
        test.edge(b, c);
        test.layout();
        INFO("direction " << int(inner));
        CHECK(test.route(down).empty());
        CHECK(test.route(up).empty());
        PrRect rect = prGraphLabelRect(test.graph, label);
        CHECK(rect.right - rect.left == 0);
        CHECK(contains(test.rect(g), test.rect(a)));
        CHECK(contains(test.rect(g), test.rect(b)));
        CHECK_FALSE(overlaps(test.rect(g), test.rect(c)));
    }
}
#endif

TEST_CASE("graph: orthogonal steps that fan out and in take tracks of their own and do not cross") {
    for (PrGraphPlacement placement : {PrGraphPlacement_Balanced, PrGraphPlacement_Shortest}) {
        TestGraph test;
        test.options.routing = PrGraphRouting_Orthogonal;
        test.options.placement = placement;
        PrGraphNode hub = test.node(40, 20);
        std::vector<PrGraphEdge> edges;
        for (int i = 0; i < 6; ++i)
            edges.push_back(test.edge(hub, test.node(40, 20)));
        PrGraphNode sink = test.node(40, 20);
        for (int i = 0; i < 6; ++i)
            edges.push_back(test.edge(PrGraphNode(1 + i), sink));
        test.layout();
        INFO("placement " << int(placement));
        CHECK(routeCrossings(test, uint32_t(edges.size())) == 0);
        // Runs across a gap at the same depth stay at least the edge spacing apart.
        struct Run {
            float y;
            float left;
            float right;
            PrGraphEdge edge;
        };
        std::vector<Run> runs;
        for (PrGraphEdge e : edges) {
            std::vector<PrVec2> route = test.route(e);
            for (size_t i = 0; i + 1 < route.size(); ++i) {
                PrVec2 p = route[i];
                PrVec2 q = route[i + 1];
                if (prF32Abs(p.y - q.y) < kTolerance && prF32Abs(p.x - q.x) > 1)
                    runs.push_back(Run{p.y, p.x < q.x ? p.x : q.x, p.x < q.x ? q.x : p.x, e});
            }
        }
        CHECK(runs.size() >= 8);
        for (size_t i = 0; i < runs.size(); ++i) {
            for (size_t j = i + 1; j < runs.size(); ++j) {
                if (runs[i].edge == runs[j].edge || prF32Abs(runs[i].y - runs[j].y) > kTolerance)
                    continue;
                INFO("runs " << i << " and " << j);
                bool apart = runs[i].right + test.options.edgeSpacing <= runs[j].left + kTolerance ||
                    runs[j].right + test.options.edgeSpacing <= runs[i].left + kTolerance;
                CHECK(apart);
            }
        }
    }
}
