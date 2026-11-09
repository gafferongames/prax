// Routing (graph_route.h): routes the edges, places the labels, and maps the scope's results to its direction.
//
// A polyline or orthogonal route is a path of waypoints in the abstract frame. Within each layer's band, it runs
// straight along the main axis at its vertex's cross-axis position. It changes position only in the free part of a gap
// between layers, where there are no nodes and no group's padding. A polyline crosses a gap diagonally, and an
// orthogonal route crosses it along the track that placement assigned.
//
// A spline is instead one smooth curve through its dummies and labels. It runs straight through a band only if the
// curve would come too close to a neighbor there. Edges that run along a lane, and self-loops, follow their paths in
// every routing style, and a spline rounds the corners of such a path.
//
// Edges between the same two nodes in adjacent layers, without ports, bow apart. Head and tail labels go beside their
// routes near their ends, clear of nodes and of other labels.

#include "graph/src/graph_route.h"

#include "pr/float.h"
#include "pr/sort.h"

#include "graph/src/graph_place.h"

namespace pr_impl {

namespace {

// A point of the abstract frame.
struct Point {
    float main;
    float cross;
};

// A path of waypoints. Each step to the next waypoint either runs along an axis, or crosses the free part of the gap
// after the layer of a segment's upper vertex, changing its cross-axis position.
struct Path {
    RawArray<Point> points;
    RawArray<uint32_t> segments;    // by step: the segment whose gap the step crosses, or kGraphNone
};

void pathAdd(Path& path, Point point, uint32_t segment) {
    if (path.points.count > 0) {
        Point last = path.points.last();
        if (last.main == point.main && last.cross == point.cross)
            return;
        path.segments.push(segment);
    }
    path.points.push(point);
}

float bandEnd(const GraphLayered& layered, uint32_t layer) {
    return layered.bandStart.data[layer] + layered.bandExtent.data[layer];
}

float nodeEnd(const GraphVertex& vertex) {
    return vertex.mainStart + vertex.main;
}

uint32_t segmentGap(const GraphLayered& layered, uint32_t segment) {
    return layered.vertices.data[layered.segments.data[segment].upper].layer;
}

// Appends the waypoints from the path's last waypoint across the gap of `segment` to `to`. The path keeps its
// cross-axis position through the padding of groups at either side of the gap, so that it changes position only where
// no group reaches.
void crossGap(const GraphLayered& layered, Path& path, uint32_t segment, Point to) {
    uint32_t gap = segmentGap(layered, segment);
    pathAdd(path, Point{layered.gapStart.data[gap], path.points.last().cross}, kGraphNone);
    pathAdd(path, Point{layered.gapEnd.data[gap], to.cross}, segment);
    pathAdd(path, to, kGraphNone);
}

// The point on the vertex's side where the port lies.
Point portPoint(const GraphLayered& layered, const GraphVertex& vertex, uint32_t port) {
    const GraphLayeredPort& placed = layered.ports.data[port];
    float left = vertex.x - vertex.cross * 0.5f;
    switch (placed.side) {
        case GraphSide_Before:
            return Point{vertex.mainStart, left + placed.offset};
        case GraphSide_After:
            return Point{nodeEnd(vertex), left + placed.offset};
        case GraphSide_CrossStart:
            return Point{vertex.mainStart + placed.offset, left};
        default:
            return Point{vertex.mainStart + placed.offset, left + vertex.cross};
    }
}

// Appends the waypoints from an edge's tail to the end of the tail's band.
void startPath(const GraphLayered& layered, const GraphLayeredEdge& edge, Path& path) {
    const GraphVertex& vertex = layered.vertices.data[edge.tail.vertex];
    float stub = layered.options.edgeSpacing;
    if (vertex.kind == GraphVertexKind_Placeholder) {
        pathAdd(path, Point{layered.groupMainEnd.data[vertex.node], vertex.x}, kGraphNone);
        return;
    }
    float end = bandEnd(layered, vertex.layer);
    float at = vertex.x + graphLayeredAttachment(layered, edge.tail);
    if (edge.tail.lane == kGraphLaneNone) {
        pathAdd(path, Point{nodeEnd(vertex), at}, kGraphNone);
        pathAdd(path, Point{end, at}, kGraphNone);
        return;
    }
    Point port = portPoint(layered, vertex, edge.tail.port);
    pathAdd(path, port, kGraphNone);
    if (edge.tail.side == GraphSide_Before) {
        float out = layered.bandStart.data[vertex.layer] - stub;
        pathAdd(path, Point{out, port.cross}, kGraphNone);
        pathAdd(path, Point{out, at}, kGraphNone);
    } else if (edge.tail.side == GraphSide_After) {
        float out = end + stub;
        pathAdd(path, Point{out, port.cross}, kGraphNone);
        pathAdd(path, Point{out, at}, kGraphNone);
    } else {
        pathAdd(path, Point{port.main, at}, kGraphNone);
    }
    pathAdd(path, Point{end, at}, kGraphNone);
}

// Appends the waypoints across the gap of the edge's last segment to the head.
void endPath(const GraphLayered& layered, const GraphLayeredEdge& edge, uint32_t segment, Path& path) {
    const GraphVertex& vertex = layered.vertices.data[edge.head.vertex];
    float stub = layered.options.edgeSpacing;
    if (vertex.kind == GraphVertexKind_Placeholder) {
        crossGap(layered, path, segment, Point{layered.groupMainStart.data[vertex.node], vertex.x});
        return;
    }
    float start = layered.bandStart.data[vertex.layer];
    float at = vertex.x + graphLayeredAttachment(layered, edge.head);
    crossGap(layered, path, segment, Point{start, at});
    if (edge.head.lane == kGraphLaneNone) {
        pathAdd(path, Point{vertex.mainStart, at}, kGraphNone);
        return;
    }
    Point port = portPoint(layered, vertex, edge.head.port);
    if (edge.head.side == GraphSide_After) {
        float out = bandEnd(layered, vertex.layer) + stub;
        pathAdd(path, Point{out, at}, kGraphNone);
        pathAdd(path, Point{out, port.cross}, kGraphNone);
    } else if (edge.head.side == GraphSide_Before) {
        float out = start - stub;
        pathAdd(path, Point{out, at}, kGraphNone);
        pathAdd(path, Point{out, port.cross}, kGraphNone);
    } else {
        pathAdd(path, Point{port.main, at}, kGraphNone);
    }
    pathAdd(path, port, kGraphNone);
}

// What a self-loop runs around, in the abstract frame: its leaf's box, or its group's rectangle.
struct LoopBody {
    float mainStart;
    float main;                     // the extent along the main axis
    float crossStart;
    float crossEnd;
};

LoopBody loopBody(const GraphLayered& layered, const GraphVertex& vertex) {
    if (vertex.kind == GraphVertexKind_Placeholder) {
        uint32_t group = vertex.node;
        float start = layered.groupMainStart.data[group];
        return LoopBody{
            .mainStart = start,
            .main = layered.groupMainEnd.data[group] - start,
            .crossStart = layered.groupLeft.data[group],
            .crossEnd = layered.groupRight.data[group],
        };
    }
    return LoopBody{
        .mainStart = vertex.mainStart,
        .main = vertex.main,
        .crossStart = vertex.x - vertex.cross * 0.5f,
        .crossEnd = vertex.x + vertex.cross * 0.5f,
    };
}

// The cross-axis position of a self-loop's lane, outside its body. A group's self-loop has no ports, and its lane lies
// outside the group's border.
float loopLane(const GraphLayered& layered, const GraphLayeredEdge& edge, const LoopBody& body) {
    const GraphVertex& vertex = layered.vertices.data[edge.tail.vertex];
    if (vertex.kind != GraphVertexKind_Placeholder)
        return vertex.x + graphLayeredAttachment(layered, edge.tail);
    float out = float(edge.tail.laneIndex + 1) * layered.options.edgeSpacing;
    return edge.tail.lane == GraphSide_CrossStart ? body.crossStart - out : body.crossEnd + out;
}

// Computes a self-loop's end (`at`) and the point just outside it (`outer`), from which the loop runs to its lane.
void loopEnd(const GraphLayered& layered, const GraphVertex& vertex, const LoopBody& body, const GraphEnd& end,
        float laneAt, bool isTail, Point& outer, Point& at) {
    float stub = layered.options.edgeSpacing;
    float sideAt = end.lane == GraphSide_CrossStart ? body.crossStart : body.crossEnd;
    if (end.port == kGraphNone) {
        float quarter = body.main > 0 ? body.main * 0.25f : stub * 0.5f;
        float main = body.mainStart + body.main * 0.5f + (isTail ? quarter : -quarter);
        at = Point{main, sideAt};
        outer = Point{main, laneAt};
        return;
    }
    at = portPoint(layered, vertex, end.port);
    switch (end.side) {
        case GraphSide_Before:
            outer = Point{layered.bandStart.data[vertex.layer] - stub, at.cross};
            break;
        case GraphSide_After:
            outer = Point{bandEnd(layered, vertex.layer) + stub, at.cross};
            break;
        default:
            outer = Point{at.main, laneAt};
            break;
    }
}

void loopPath(const GraphLayered& layered, const GraphLayeredEdge& edge, Path& path) {
    const GraphVertex& vertex = layered.vertices.data[edge.tail.vertex];
    LoopBody body = loopBody(layered, vertex);
    float laneAt = loopLane(layered, edge, body);
    Point tailOuter{};
    Point tailAt{};
    Point headOuter{};
    Point headAt{};
    loopEnd(layered, vertex, body, edge.tail, laneAt, true, tailOuter, tailAt);
    loopEnd(layered, vertex, body, edge.head, laneAt, false, headOuter, headAt);
    pathAdd(path, tailAt, kGraphNone);
    pathAdd(path, tailOuter, kGraphNone);
    pathAdd(path, Point{tailOuter.main, laneAt}, kGraphNone);
    pathAdd(path, Point{headOuter.main, laneAt}, kGraphNone);
    pathAdd(path, headOuter, kGraphNone);
    pathAdd(path, headAt, kGraphNone);
}

bool isCollinear(Point a, Point b, Point c) {
    return (a.main == b.main && b.main == c.main) || (a.cross == b.cross && b.cross == c.cross);
}

Point lerp(Point a, Point b, float t) {
    return Point{a.main + (b.main - a.main) * t, a.cross + (b.cross - a.cross) * t};
}

float distance(Point a, Point b) {
    return prF32Sqrt((b.main - a.main) * (b.main - a.main) + (b.cross - a.cross) * (b.cross - a.cross));
}

// Appends a straight cubic segment from the last point of `out` to `to`.
void appendLine(RawArray<Point>& out, Point to) {
    Point from = out.last();
    out.push(lerp(from, to, 1.0f / 3));
    out.push(lerp(from, to, 2.0f / 3));
    out.push(to);
}

// Appends a spline through a path of straight runs, as cubic segments after its first point. A run marked in `bends`
// crosses a gap as an S-curve that leaves and arrives along the main axis. Each corner between two straight runs is
// rounded by a curve that leaves the first run and joins the second along their directions, so the spline turns without
// a kink. A corner's curve starts and ends within `radius` of it, and within half of either run.
void appendRounded(const RawArray<Point>& points, const RawArray<uint8_t>& bends, float radius, RawArray<Point>& out) {
    out.push(points.data[0]);
    for (uint32_t i = 0; i + 1 < points.count; ++i) {
        Point a = points.data[i];
        Point b = points.data[i + 1];
        if (bends.data[i] != 0) {
            float middle = (a.main + b.main) * 0.5f;
            out.push(Point{middle, a.cross});
            out.push(Point{middle, b.cross});
            out.push(b);
            continue;
        }
        bool corner = i + 2 < points.count && bends.data[i + 1] == 0;
        if (!corner) {
            if (distance(out.last(), b) > 1e-4f)
                appendLine(out, b);
            continue;
        }
        Point c = points.data[i + 2];
        float in = distance(a, b);
        float outgoing = distance(b, c);
        float r = radius;
        r = in * 0.5f < r ? in * 0.5f : r;
        r = outgoing * 0.5f < r ? outgoing * 0.5f : r;
        Point enter = lerp(b, a, r / in);
        Point leave = lerp(b, c, r / outgoing);
        if (distance(out.last(), enter) > 1e-4f)
            appendLine(out, enter);
        out.push(lerp(enter, b, 2.0f / 3));
        out.push(lerp(leave, b, 2.0f / 3));
        out.push(leave);
    }
}

// Appends the route of a path in the routing's style, in the abstract frame.
void shapeRoute(const GraphLayered& layered, const Path& path, bool loopCurve, RawArray<Point>& out) {
    PrGraphRouting routing = layered.options.routing;
    uint32_t count = path.points.count;
    if (count == 0)
        return;
    if (routing == PrGraphRouting_Spline && loopCurve && count == 4) {
        // A self-loop without ports is one curve whose farthest point reaches its lane.
        Point a = path.points.data[0];
        Point d = path.points.data[3];
        float reach = path.points.data[1].cross - a.cross;
        float control = a.cross + reach * (4.0f / 3.0f);
        out.push(a);
        out.push(Point{a.main, control});
        out.push(Point{d.main, control});
        out.push(d);
        return;
    }
    // Splits each orthogonal step across a gap at its track.
    RawArray<Point> points{};
    RawArray<uint8_t> bends{};
    points.push(path.points.data[0]);
    for (uint32_t i = 0; i + 1 < count; ++i) {
        Point a = path.points.data[i];
        Point b = path.points.data[i + 1];
        uint32_t segment = path.segments.data[i];
        bool transition = segment != kGraphNone && a.cross != b.cross;
        if (transition && routing == PrGraphRouting_Orthogonal) {
            uint32_t track = layered.segmentTrack.data[segment];
            uint32_t tracks = layered.trackCount.data[segmentGap(layered, segment)];
            tracks = tracks > track ? tracks : track + 1;
            float main = a.main + (b.main - a.main) * float(track + 1) / float(tracks + 1);
            bends.push(0);
            points.push(Point{main, a.cross});
            bends.push(0);
            points.push(Point{main, b.cross});
            bends.push(0);
            points.push(b);
            continue;
        }
        bends.push(transition ? 1 : 0);
        points.push(b);
    }
    // Drops the waypoints that lie on a straight run.
    RawArray<Point> kept{};
    RawArray<uint8_t> keptBends{};
    kept.push(points.data[0]);
    for (uint32_t i = 1; i < points.count; ++i) {
        if (kept.count >= 2 && keptBends.last() == 0 && bends.data[i - 1] == 0 &&
                isCollinear(kept.data[kept.count - 2], kept.last(), points.data[i])) {
            kept.last() = points.data[i];
            continue;
        }
        keptBends.push(bends.data[i - 1]);
        kept.push(points.data[i]);
    }
    if (routing != PrGraphRouting_Spline)
        out.append(kept.data, kept.count);
    else
        appendRounded(kept, keptBends, layered.options.edgeSpacing, out);
    points.reset();
    bends.reset();
    kept.reset();
    keptBends.reset();
}

struct Frame {
    PrGraphDirection direction;
    float minMain;
    float minCross;
    float mainExtent;
};

PrVec2 toWorld(const Frame& frame, Point point) {
    float m = point.main - frame.minMain;
    float c = point.cross - frame.minCross;
    switch (frame.direction) {
        case PrGraphDirection_Up:
            return PrVec2{c, frame.mainExtent - m};
        case PrGraphDirection_Right:
            return PrVec2{m, c};
        case PrGraphDirection_Left:
            return PrVec2{frame.mainExtent - m, c};
        default:
            return PrVec2{c, m};
    }
}

// A rectangle in the abstract frame, as its ranges on the main and cross axes.
struct Box {
    float mainStart;
    float mainEnd;
    float crossStart;
    float crossEnd;
};

PrRect toWorldRect(const Frame& frame, Box box) {
    PrVec2 a = toWorld(frame, Point{box.mainStart, box.crossStart});
    PrVec2 b = toWorld(frame, Point{box.mainEnd, box.crossEnd});
    return PrRect{a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y, a.x < b.x ? b.x : a.x, a.y < b.y ? b.y : a.y};
}

void include(Box& bounds, bool& any, Box box) {
    if (!any) {
        bounds = box;
        any = true;
        return;
    }
    bounds.mainStart = box.mainStart < bounds.mainStart ? box.mainStart : bounds.mainStart;
    bounds.mainEnd = box.mainEnd > bounds.mainEnd ? box.mainEnd : bounds.mainEnd;
    bounds.crossStart = box.crossStart < bounds.crossStart ? box.crossStart : bounds.crossStart;
    bounds.crossEnd = box.crossEnd > bounds.crossEnd ? box.crossEnd : bounds.crossEnd;
}

Box vertexBox(const GraphVertex& vertex) {
    float half = vertex.cross * 0.5f;
    return Box{vertex.mainStart, vertex.mainStart + vertex.main, vertex.x - half, vertex.x + half};
}

// How far a route through a dummy or label may stray on the cross axis within its band. It is the distance from the
// vertex's center to its nearest neighbor in the layer, minus half the edge spacing, or infinite if it has no
// neighbors.
float clearance(const GraphLayered& layered, uint32_t vertex) {
    const GraphVertex& v = layered.vertices.data[vertex];
    uint32_t start = layered.layerStart.data[v.layer];
    uint32_t end = layered.layerStart.data[v.layer + 1];
    float least = kPrF32Infinity;
    for (int step = -1; step <= 1; step += 2) {
        for (int64_t i = int64_t(start + v.order) + step; i >= int64_t(start) && i < int64_t(end); i += step) {
            const GraphVertex& other = layered.vertices.data[layered.layerVertices.data[i]];
            if (other.kind == GraphVertexKind_Placeholder)
                continue;
            float distance = other.x - other.cross * 0.5f - other.reserveStart - v.x;
            if (step < 0)
                distance = v.x - (other.x + other.cross * 0.5f + other.reserveEnd);
            least = distance < least ? distance : least;
            break;
        }
    }
    float margin = layered.options.edgeSpacing * 0.5f;
    return least > margin ? least - margin : 0;
}

// A point that a smooth route passes through, and the dummy or label at that point, if any.
struct Waypoint {
    Point point;
    uint32_t vertex;                // kGraphNone at an end, or at either end of a straight run through a band
    uint32_t dummy;                 // the index of its dummy along the edge, or kGraphNone at an end
    bool alongMain;                 // true if the curve passes the point moving along the main axis
};

// Appends the cubic segments of a curve through the waypoints. The curve's cross-axis position is a monotone cubic
// function of its main-axis position (Fritsch and Carlson, "Monotone Piecewise Cubic Interpolation", 1980). The
// waypoints must advance along the main axis. Between two waypoints, the curve stays between their cross-axis
// positions, so it never overshoots a waypoint and comes back. The slope is zero at a waypoint where the route stops
// moving across or turns back, and at a waypoint with `alongMain` set.
void appendCurve(const RawArray<Waypoint>& waypoints, RawArray<Point>& out) {
    uint32_t count = waypoints.count;
    // The cross-axis slope, per unit along the main axis, of each interval and at each waypoint.
    auto secant = [&](uint32_t i) {
        Point a = waypoints.data[i].point;
        Point b = waypoints.data[i + 1].point;
        float run = b.main - a.main;
        return run > 1e-4f ? (b.cross - a.cross) / run : 0.0f;
    };
    auto slope = [&](uint32_t i) {
        if (waypoints.data[i].alongMain || i == 0 || i + 1 == count)
            return 0.0f;
        float before = secant(i - 1);
        float after = secant(i);
        if (before * after <= 0)
            return 0.0f;
        // The harmonic mean lies between the two slopes and keeps each interval monotone.
        return 2 / (1 / before + 1 / after);
    };
    out.push(waypoints.data[0].point);
    for (uint32_t i = 0; i + 1 < count; ++i) {
        Point a = waypoints.data[i].point;
        Point b = waypoints.data[i + 1].point;
        float third = (b.main - a.main) / 3;
        out.push(Point{a.main + third, a.cross + slope(i) * third});
        out.push(Point{b.main - third, b.cross - slope(i + 1) * third});
        out.push(b);
    }
}

// Returns the point at `t` of the cubic segment from `p[0]` through `p[3]`.
Point cubicAt(const Point* p, float t) {
    float u = 1 - t;
    float a = u * u * u;
    float b = 3 * u * u * t;
    float c = 3 * u * t * t;
    float d = t * t * t;
    return Point{
        a * p[0].main + b * p[1].main + c * p[2].main + d * p[3].main,
        a * p[0].cross + b * p[1].cross + c * p[2].cross + d * p[3].cross,
    };
}

// Appends a spline route for an edge that meets both of its vertices without a lane. The route is one smooth curve
// through the centers of the edge's dummies and labels, and it leaves and meets its ends along the main axis. If the
// curve would stray toward a neighbor within a dummy's band by more than the dummy's clearance, the route runs straight
// through that band instead, as a path does. Returns false for any other edge, which is routed as a path.
bool smoothRoute(const GraphLayered& layered, const GraphLayeredEdge& edge, RawArray<Point>& out) {
    if (layered.options.routing != PrGraphRouting_Spline || edge.selfLoop || edge.tail.lane != kGraphLaneNone ||
            edge.head.lane != kGraphLaneNone)
        return false;
    // By dummy along the edge: true once the route runs straight through the dummy's band.
    RawArray<uint8_t> straight{};
    straight.assignZeroed(edge.segmentCount);
    RawArray<Waypoint> waypoints{};
    RawArray<Point> curve{};
    const GraphVertex& tail = layered.vertices.data[edge.tail.vertex];
    const GraphVertex& head = layered.vertices.data[edge.head.vertex];
    Point first{nodeEnd(tail), tail.x + graphLayeredAttachment(layered, edge.tail)};
    if (tail.kind == GraphVertexKind_Placeholder)
        first = Point{layered.groupMainEnd.data[tail.node], tail.x};
    Point last{head.mainStart, head.x + graphLayeredAttachment(layered, edge.head)};
    if (head.kind == GraphVertexKind_Placeholder)
        last = Point{layered.groupMainStart.data[head.node], head.x};
    for (uint32_t pass = 0; pass <= edge.segmentCount; ++pass) {
        waypoints.clear();
        waypoints.push(Waypoint{first, kGraphNone, kGraphNone, true});
        for (uint32_t s = 0; s < edge.segmentCount; ++s) {
            uint32_t segment = edge.segmentStart + s;
            uint32_t v = layered.segments.data[segment].lower;
            const GraphVertex& vertex = layered.vertices.data[v];
            float bandStart = layered.bandStart.data[vertex.layer];
            float band = layered.bandExtent.data[vertex.layer];
            Point next = s + 1 < edge.segmentCount ? Point{bandStart, vertex.x} : last;
            // Keeps the curve at its cross-axis positions through the padding of groups at either side of the gap, and
            // through the rest of its ends' bands, where other nodes may be deeper than its ends.
            uint32_t gap = segmentGap(layered, segment);
            float freeStart = layered.gapStart.data[gap];
            float freeEnd = layered.gapEnd.data[gap];
            bool fromEnd = s == 0;
            bool toEnd = s + 1 == edge.segmentCount;
            if (freeStart > waypoints.last().point.main + 0.01f && (fromEnd || freeStart > bandEnd(layered, gap) + 0.01f))
                waypoints.push(Waypoint{Point{freeStart, waypoints.last().point.cross}, kGraphNone, kGraphNone, true});
            if (freeEnd < next.main - 0.01f && (toEnd || freeEnd < layered.bandStart.data[gap + 1] - 0.01f))
                waypoints.push(Waypoint{Point{freeEnd, next.cross}, kGraphNone, kGraphNone, true});
            if (s + 1 == edge.segmentCount)
                break;
            if (straight.data[s] != 0) {
                waypoints.push(Waypoint{Point{bandStart, vertex.x}, kGraphNone, s, true});
                waypoints.push(Waypoint{Point{bandStart + band, vertex.x}, kGraphNone, s, true});
            } else {
                waypoints.push(Waypoint{Point{bandStart + band * 0.5f, vertex.x}, v, s, false});
            }
        }
        waypoints.push(Waypoint{last, kGraphNone, kGraphNone, true});
        curve.clear();
        appendCurve(waypoints, curve);

        // Samples the curve in each smooth dummy's band, on the segments on either side of the dummy.
        bool strayed = false;
        for (uint32_t i = 1; i + 1 < waypoints.count && !strayed; ++i) {
            const Waypoint& w = waypoints.data[i];
            if (w.vertex == kGraphNone)
                continue;
            const GraphVertex& vertex = layered.vertices.data[w.vertex];
            float bandStart = layered.bandStart.data[vertex.layer];
            float bandEnd = bandStart + layered.bandExtent.data[vertex.layer];
            float allowed = clearance(layered, w.vertex);
            for (uint32_t segment = i - 1; segment <= i && !strayed; ++segment) {
                const Point* p = curve.data + 3 * segment;
                for (int k = 0; k <= 16; ++k) {
                    Point at = cubicAt(p, float(k) / 16);
                    if (at.main >= bandStart && at.main <= bandEnd && prF32Abs(at.cross - vertex.x) > allowed) {
                        strayed = true;
                        break;
                    }
                }
            }
            if (strayed)
                straight.data[w.dummy] = 1;
        }
        if (!strayed)
            break;
    }
    out.append(curve.data, curve.count);
    straight.reset();
    waypoints.reset();
    curve.reset();
    return true;
}

// The point where the ray from the vertex's center toward `toward` leaves the vertex's box.
Point boundaryToward(const GraphVertex& vertex, Point toward) {
    Point center{vertex.mainStart + vertex.main * 0.5f, vertex.x};
    float dm = toward.main - center.main;
    float dc = toward.cross - center.cross;
    float scale = kPrF32Infinity;
    if (prF32Abs(dm) > 1e-6f)
        scale = vertex.main * 0.5f / prF32Abs(dm);
    if (prF32Abs(dc) > 1e-6f) {
        float across = vertex.cross * 0.5f / prF32Abs(dc);
        scale = across < scale ? across : scale;
    }
    if (scale == kPrF32Infinity)
        return center;
    return Point{center.main + dm * scale, center.cross + dc * scale};
}

// By edge: how far the middle of the edge's route bows across from the line between its ends. Edges that join the same
// two leaves in adjacent layers without ports would otherwise take the same route. They bow apart symmetrically, by the
// edge spacing, or by less if that would spread them wider than the node spacing.
void assignBows(const GraphLayered& layered, RawArray<float>& bows) {
    uint32_t edgeCount = layered.edges.count;
    bows.assignZeroed(edgeCount);
    RawArray<uint32_t> candidates{};
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const GraphLayeredEdge& edge = layered.edges.data[e];
        if (edge.selfLoop || edge.segmentCount != 1 || edge.tail.port != kGraphNone || edge.head.port != kGraphNone ||
                layered.vertices.data[edge.tail.vertex].kind != GraphVertexKind_Leaf ||
                layered.vertices.data[edge.head.vertex].kind != GraphVertexKind_Leaf)
            continue;
        candidates.push(e);
    }
    auto ends = [&](uint32_t e) {
        const GraphLayeredEdge& edge = layered.edges.data[e];
        return (uint64_t(edge.tail.vertex) << 32) | edge.head.vertex;
    };
    sortItems(candidates.span(), [&](uint32_t a, uint32_t b) {
        return ends(a) < ends(b) || (ends(a) == ends(b) && a < b);
    });
    for (uint32_t i = 0; i < candidates.count;) {
        uint32_t j = i + 1;
        while (j < candidates.count && ends(candidates.data[j]) == ends(candidates.data[i]))
            ++j;
        uint32_t count = j - i;
        if (count > 1) {
            float spacing = layered.options.edgeSpacing;
            float widest = layered.options.nodeSpacing / float(count - 1);
            spacing = widest < spacing ? widest : spacing;
            for (uint32_t k = 0; k < count; ++k)
                bows.data[candidates.data[i + k]] = (float(k) - float(count - 1) * 0.5f) * spacing;
        }
        i = j;
    }
    candidates.reset();
}

// Appends the route of an edge between leaves in adjacent layers that bows across by `bow` from the line between its
// ends. It changes its cross-axis position only in the free part of the gap, and an end that leaves the main axis still
// points at its node's center. Returns false if the edge needs no bow, or if orthogonal tracks already keep it apart.
bool bowedRoute(const GraphLayered& layered, const GraphLayeredEdge& edge, float bow, RawArray<Point>& out) {
    if (bow == 0)
        return false;
    const GraphVertex& tail = layered.vertices.data[edge.tail.vertex];
    const GraphVertex& head = layered.vertices.data[edge.head.vertex];
    float freeStart = layered.gapStart.data[tail.layer];
    float freeEnd = layered.gapEnd.data[tail.layer];
    float free = freeEnd - freeStart;
    switch (layered.options.routing) {
        case PrGraphRouting_Polyline: {
            Point a{freeStart + free * 0.25f, tail.x + bow};
            Point b{freeEnd - free * 0.25f, head.x + bow};
            out.push(boundaryToward(tail, a));
            out.push(a);
            out.push(b);
            out.push(boundaryToward(head, b));
            return true;
        }
        case PrGraphRouting_Spline: {
            // A cubic whose control points bow by 4/3 of `bow` bows by `bow` at its middle.
            Point start{nodeEnd(tail), tail.x};
            Point end{head.mainStart, head.x};
            Point a = lerp(start, end, 1.0f / 3);
            Point b = lerp(start, end, 2.0f / 3);
            a.cross += bow * (4.0f / 3);
            b.cross += bow * (4.0f / 3);
            out.push(boundaryToward(tail, a));
            out.push(a);
            out.push(b);
            out.push(boundaryToward(head, b));
            return true;
        }
        default: {
            if (tail.x != head.x)
                return false;
            float a = freeStart + free / 3;
            float b = freeEnd - free / 3;
            out.push(Point{nodeEnd(tail), tail.x});
            out.push(Point{a, tail.x});
            out.push(Point{a, tail.x + bow});
            out.push(Point{b, tail.x + bow});
            out.push(Point{b, head.x});
            out.push(Point{head.mainStart, head.x});
            return true;
        }
    }
}

// Returns true if the boxes share more than an edge.
bool boxesOverlap(Box a, Box b) {
    constexpr float kTouch = 0.01f;
    return a.mainStart < b.mainEnd - kTouch && b.mainStart < a.mainEnd - kTouch &&
        a.crossStart < b.crossEnd - kTouch && b.crossStart < a.crossEnd - kTouch;
}

// Returns true if the segment from `a` to `b` passes through the inside of the box (Liang and Barsky, 1984).
bool segmentEntersBox(Point a, Point b, Box box) {
    constexpr float kTouch = 0.01f;
    float t0 = 0;
    float t1 = 1;
    float d[2] = {b.main - a.main, b.cross - a.cross};
    float start[2] = {a.main, a.cross};
    float low[2] = {box.mainStart + kTouch, box.crossStart + kTouch};
    float high[2] = {box.mainEnd - kTouch, box.crossEnd - kTouch};
    for (int axis = 0; axis < 2; ++axis) {
        if (low[axis] >= high[axis])
            return false;
        if (prF32Abs(d[axis]) < 1e-6f) {
            if (start[axis] <= low[axis] || start[axis] >= high[axis])
                return false;
            continue;
        }
        float ta = (low[axis] - start[axis]) / d[axis];
        float tb = (high[axis] - start[axis]) / d[axis];
        float enter = ta < tb ? ta : tb;
        float leave = ta < tb ? tb : ta;
        t0 = enter > t0 ? enter : t0;
        t1 = leave < t1 ? leave : t1;
        if (t0 >= t1)
            return false;
    }
    return true;
}

// Writes the route's points to `out` as a polyline, with each cubic segment of a spline sampled.
void flatten(const Point* route, uint32_t count, bool spline, RawArray<Point>& out) {
    out.clear();
    if (count == 0)
        return;
    out.push(route[0]);
    if (!spline) {
        out.append(route + 1, count - 1);
        return;
    }
    for (uint32_t i = 0; i + 3 < count; i += 3) {
        for (int k = 1; k <= 8; ++k)
            out.push(cubicAt(route + i, float(k) / 8));
    }
}

// A uniform grid over boxes, which finds the boxes that may overlap a query box. Each box is listed in every cell that
// it reaches. A position outside the grid falls in the nearest border cell, so two boxes that overlap share a cell
// wherever they lie. The grid has about as many cells as the boxes that it starts with, which do not overlap one
// another, so a query looks at the boxes near it rather than at all of them.
struct BoxGrid {
    RawArray<Box> boxes;
    float mainStart;
    float crossStart;
    float cell;                     // the side of a cell
    uint32_t mainCells;
    uint32_t crossCells;
    RawArray<uint32_t> head;        // by cell: the first of its entries, or kGraphNone
    RawArray<uint32_t> entryBox;    // by entry: its box
    RawArray<uint32_t> entryNext;   // by entry: the next entry of its cell, or kGraphNone
    RawArray<uint32_t> seen;        // by box: the last query that tested it, so that a query tests each box once
    uint32_t query;
};

// The cell of a position along one axis, within [0, cells).
uint32_t gridCell(float at, float start, float cell, uint32_t cells) {
    float index = (at - start) / cell;
    if (!(index > 0))
        return 0;
    if (index >= float(cells))
        return cells - 1;
    return uint32_t(index);
}

void boxGridAdd(BoxGrid& grid, Box box) {
    uint32_t index = grid.boxes.count;
    grid.boxes.push(box);
    grid.seen.push(0);
    uint32_t m0 = gridCell(box.mainStart, grid.mainStart, grid.cell, grid.mainCells);
    uint32_t m1 = gridCell(box.mainEnd, grid.mainStart, grid.cell, grid.mainCells);
    uint32_t c0 = gridCell(box.crossStart, grid.crossStart, grid.cell, grid.crossCells);
    uint32_t c1 = gridCell(box.crossEnd, grid.crossStart, grid.cell, grid.crossCells);
    for (uint32_t m = m0; m <= m1; ++m) {
        for (uint32_t c = c0; c <= c1; ++c) {
            uint32_t& head = grid.head.data[m * grid.crossCells + c];
            grid.entryBox.push(index);
            grid.entryNext.push(head);
            head = grid.entryBox.count - 1;
        }
    }
}

// Sizes the grid to the bounds of `boxes` and adds them.
void boxGridBuild(BoxGrid& grid, const RawArray<Box>& boxes) {
    Box bounds{0, 0, 0, 0};
    bool any = false;
    for (const Box& box : boxes.span())
        include(bounds, any, box);
    float mainExtent = bounds.mainEnd - bounds.mainStart;
    float crossExtent = bounds.crossEnd - bounds.crossStart;
    float area = mainExtent * crossExtent;
    float cell = boxes.count > 0 && area > 0 ? prF32Sqrt(area / float(boxes.count)) : 1;
    // A cell is at least the longer extent divided by the number of boxes, so that neither axis has more cells than
    // boxes.
    float least = mainExtent > crossExtent ? mainExtent : crossExtent;
    least /= float(boxes.count > 0 ? boxes.count : 1);
    cell = cell > least ? cell : least;
    cell = cell > 1 ? cell : 1;
    grid.mainStart = bounds.mainStart;
    grid.crossStart = bounds.crossStart;
    grid.cell = cell;
    grid.mainCells = uint32_t(prF32Ceil(mainExtent / cell)) + 1;
    grid.crossCells = uint32_t(prF32Ceil(crossExtent / cell)) + 1;
    grid.head.resize(grid.mainCells * grid.crossCells);
    for (uint32_t i = 0; i < grid.head.count; ++i)
        grid.head.data[i] = kGraphNone;
    grid.boxes.clear();
    grid.seen.clear();
    grid.entryBox.clear();
    grid.entryNext.clear();
    grid.query = 0;
    for (const Box& box : boxes.span())
        boxGridAdd(grid, box);
}

// Returns true if no box of the grid overlaps `box`.
bool boxGridIsClear(BoxGrid& grid, Box box) {
    uint32_t query = ++grid.query;
    uint32_t m0 = gridCell(box.mainStart, grid.mainStart, grid.cell, grid.mainCells);
    uint32_t m1 = gridCell(box.mainEnd, grid.mainStart, grid.cell, grid.mainCells);
    uint32_t c0 = gridCell(box.crossStart, grid.crossStart, grid.cell, grid.crossCells);
    uint32_t c1 = gridCell(box.crossEnd, grid.crossStart, grid.cell, grid.crossCells);
    for (uint32_t m = m0; m <= m1; ++m) {
        for (uint32_t c = c0; c <= c1; ++c) {
            for (uint32_t e = grid.head.data[m * grid.crossCells + c]; e != kGraphNone; e = grid.entryNext.data[e]) {
                uint32_t index = grid.entryBox.data[e];
                if (grid.seen.data[index] == query)
                    continue;
                grid.seen.data[index] = query;
                if (boxesOverlap(box, grid.boxes.data[index]))
                    return false;
            }
        }
    }
    return true;
}

void boxGridReset(BoxGrid& grid) {
    grid.boxes.reset();
    grid.head.reset();
    grid.entryBox.reset();
    grid.entryNext.reset();
    grid.seen.reset();
}

// Places a label of the given extents beside a polyline that starts at the label's end. The candidates lie along the
// polyline at increasing distances from its start, on either side, with the label's near side `spacing` from the line.
// Returns the first candidate that overlaps no obstacle and that the line does not cross, or else the first that
// overlaps no obstacle, or else the first.
Box placeEndLabel(const RawArray<Point>& line, float main, float cross, float spacing, BoxGrid& obstacles) {
    constexpr uint32_t kMaxCandidates = 256;
    float step = spacing > 2 ? spacing : 2;
    bool hasFallback = false;
    Box fallback{};
    bool hasFirst = false;
    Box first{};
    uint32_t candidates = 0;
    float along = spacing;      // the distance of the next candidate from the start
    float walked = 0;           // the distance from the start to the current step's start
    for (uint32_t i = 0; i + 1 < line.count && candidates < kMaxCandidates; ++i) {
        Point a = line.data[i];
        Point b = line.data[i + 1];
        float length = distance(a, b);
        if (length < 1e-4f)
            continue;
        float dm = (b.main - a.main) / length;
        float dc = (b.cross - a.cross) / length;
        for (; along <= walked + length && candidates < kMaxCandidates; along += step, ++candidates) {
            Point p = lerp(a, b, (along - walked) / length);
            for (int sideIndex = 0; sideIndex < 2; ++sideIndex) {
                float side = sideIndex == 0 ? 1.0f : -1.0f;
                Box box{};
                if (prF32Abs(dm) >= prF32Abs(dc)) {
                    box.mainStart = dm >= 0 ? p.main : p.main - main;
                    box.crossStart = side > 0 ? p.cross + spacing : p.cross - spacing - cross;
                } else {
                    box.crossStart = dc >= 0 ? p.cross : p.cross - cross;
                    box.mainStart = side > 0 ? p.main + spacing : p.main - spacing - main;
                }
                box.mainEnd = box.mainStart + main;
                box.crossEnd = box.crossStart + cross;
                if (!hasFirst) {
                    first = box;
                    hasFirst = true;
                }
                if (!boxGridIsClear(obstacles, box))
                    continue;
                bool crossed = false;
                for (uint32_t k = 0; k + 1 < line.count && !crossed; ++k)
                    crossed = segmentEntersBox(line.data[k], line.data[k + 1], box);
                if (!crossed)
                    return box;
                if (!hasFallback) {
                    fallback = box;
                    hasFallback = true;
                }
            }
        }
        walked += length;
    }
    if (hasFallback)
        return fallback;
    // Nothing beside the line is clear, as on a short line among crowded labels. Searches rings around the end.
    Point at = line.count > 0 ? line.data[0] : Point{0, 0};
    constexpr uint32_t kRings = 64;
    constexpr uint32_t kAngles = 16;
    for (uint32_t ring = 1; ring <= kRings; ++ring) {
        float radius = float(ring) * step;
        for (uint32_t k = 0; k < kAngles; ++k) {
            float angle = float(k) * (2 * kPrF32Pi / float(kAngles));
            float m = at.main + radius * prF32Sin(angle);
            float c = at.cross + radius * prF32Cos(angle);
            Box box{m - main * 0.5f, m + main * 0.5f, c - cross * 0.5f, c + cross * 0.5f};
            if (boxGridIsClear(obstacles, box))
                return box;
        }
    }
    if (hasFirst)
        return first;
    return Box{at.main + spacing, at.main + spacing + main, at.cross + spacing, at.cross + spacing + cross};
}

} // namespace

void graphLayeredRoute(GraphLayered& layered, GraphScope& scope) {
    const PrGraphLayoutOptions& options = layered.options;
    PrGraphDirection direction = scope.direction;
    bool vertical = direction == PrGraphDirection_Down || direction == PrGraphDirection_Up;
    uint32_t edgeCount = scope.edges.count;

    // Routes every edge from tail to head, then turns the reversed edges around.
    RawArray<float> bows{};
    assignBows(layered, bows);
    Path path{};
    RawArray<Point> routes{};
    RawArray<uint32_t> routeStart{};
    routeStart.resize(edgeCount + 1);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const GraphLayeredEdge& edge = layered.edges.data[e];
        routeStart.data[e] = routes.count;
        if (!bowedRoute(layered, edge, bows.data[e], routes) && !smoothRoute(layered, edge, routes)) {
            path.points.clear();
            path.segments.clear();
            if (edge.selfLoop) {
                loopPath(layered, edge, path);
            } else {
                startPath(layered, edge, path);
                for (uint32_t s = 0; s < edge.segmentCount; ++s) {
                    uint32_t segment = edge.segmentStart + s;
                    if (s + 1 == edge.segmentCount) {
                        endPath(layered, edge, segment, path);
                        break;
                    }
                    const GraphVertex& vertex = layered.vertices.data[layered.segments.data[segment].lower];
                    crossGap(layered, path, segment, Point{layered.bandStart.data[vertex.layer], vertex.x});
                    pathAdd(path, Point{bandEnd(layered, vertex.layer), vertex.x}, kGraphNone);
                }
            }
            shapeRoute(layered, path, edge.selfLoop && edge.tail.port == kGraphNone && edge.head.port == kGraphNone, routes);
        }
        if (edge.reversed) {
            Span<Point> route(routes.data + routeStart.data[e], routes.count - routeStart.data[e]);
            reverseItems(route);
        }
    }
    routeStart.data[edgeCount] = routes.count;

    // Places the center labels, in the abstract frame.
    RawArray<Box> labelBoxes{};
    labelBoxes.resize(scope.labels.count);
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const GraphScopeEdge& described = scope.edges.data[e];
        const GraphLayeredEdge& edge = layered.edges.data[e];
        float centerMain = 0;
        float centerCross = 0;
        if (edge.labelVertex != kGraphNone) {
            const GraphVertex& vertex = layered.vertices.data[edge.labelVertex];
            centerMain = vertex.mainStart;
            centerCross = vertex.x;
        }
        for (uint32_t i = 0; i < described.labelCount; ++i) {
            const GraphScopeLabel& label = scope.labels.data[described.labelStart + i];
            float main = vertical ? label.extent.y : label.extent.x;
            float cross = vertical ? label.extent.x : label.extent.y;
            Box& box = labelBoxes.data[described.labelStart + i];
            if (label.placement == PrGraphLabelPlacement_Center && edge.selfLoop) {
                LoopBody body = loopBody(layered, layered.vertices.data[edge.tail.vertex]);
                float laneAt = loopLane(layered, edge, body);
                float middle = body.mainStart + body.main * 0.5f;
                float side = edge.tail.lane == GraphSide_CrossStart ? -1.0f : 1.0f;
                float near = laneAt + side * options.labelSpacing;
                float far = near + side * cross;
                box = Box{middle - main * 0.5f, middle + main * 0.5f, near < far ? near : far, near < far ? far : near};
            } else if (label.placement == PrGraphLabelPlacement_Center) {
                box = Box{centerMain, centerMain + main, centerCross - cross * 0.5f, centerCross + cross * 0.5f};
                centerMain += main + options.labelSpacing;
            }
        }
    }

    // Places the head and tail labels, each beside its route near its end, clear of the leaves and of the labels placed
    // before it.
    bool hasEndLabels = false;
    for (uint32_t i = 0; i < scope.labels.count && !hasEndLabels; ++i)
        hasEndLabels = scope.labels.data[i].placement != PrGraphLabelPlacement_Center;
    BoxGrid obstacles{};
    if (hasEndLabels) {
        RawArray<Box> boxes{};
        for (uint32_t v = 0; v < layered.vertices.count; ++v) {
            if (layered.vertices.data[v].kind == GraphVertexKind_Leaf)
                boxes.push(vertexBox(layered.vertices.data[v]));
        }
        for (uint32_t e = 0; e < edgeCount; ++e) {
            const GraphScopeEdge& described = scope.edges.data[e];
            for (uint32_t i = 0; i < described.labelCount; ++i) {
                if (scope.labels.data[described.labelStart + i].placement == PrGraphLabelPlacement_Center)
                    boxes.push(labelBoxes.data[described.labelStart + i]);
            }
        }
        boxGridBuild(obstacles, boxes);
        boxes.reset();
    }
    RawArray<Point> line{};
    for (uint32_t e = 0; e < edgeCount; ++e) {
        const GraphScopeEdge& described = scope.edges.data[e];
        uint32_t first = routeStart.data[e];
        uint32_t count = routeStart.data[e + 1] - first;
        for (uint32_t i = 0; i < described.labelCount; ++i) {
            const GraphScopeLabel& label = scope.labels.data[described.labelStart + i];
            if (label.placement == PrGraphLabelPlacement_Center)
                continue;
            float main = vertical ? label.extent.y : label.extent.x;
            float cross = vertical ? label.extent.x : label.extent.y;
            Box& box = labelBoxes.data[described.labelStart + i];
            flatten(routes.data + first, count, options.routing == PrGraphRouting_Spline, line);
            if (label.placement == PrGraphLabelPlacement_Head)
                reverseItems(line.span());
            box = placeEndLabel(line, main, cross, options.labelSpacing, obstacles);
            boxGridAdd(obstacles, box);
        }
    }
    boxGridReset(obstacles);
    line.reset();

    // The bounds of everything, which become the scope's box.
    Box bounds{};
    bool any = false;
    for (uint32_t v = 0; v < layered.vertices.count; ++v) {
        const GraphVertex& vertex = layered.vertices.data[v];
        if (vertex.kind == GraphVertexKind_Leaf || vertex.kind == GraphVertexKind_Label)
            include(bounds, any, vertexBox(vertex));
    }
    for (uint32_t v = 0; v < scope.nodes.count; ++v) {
        if (layered.isGroup.data[v] != 0) {
            include(bounds, any, Box{
                layered.groupMainStart.data[v], layered.groupMainEnd.data[v],
                layered.groupLeft.data[v], layered.groupRight.data[v],
            });
        }
    }
    for (uint32_t i = 0; i < labelBoxes.count; ++i)
        include(bounds, any, labelBoxes.data[i]);
    for (uint32_t i = 0; i < routes.count; ++i) {
        Point p = routes.data[i];
        include(bounds, any, Box{p.main, p.main, p.cross, p.cross});
    }
    Frame frame{
        .direction = direction,
        .minMain = bounds.mainStart,
        .minCross = bounds.crossStart,
        .mainExtent = bounds.mainEnd - bounds.mainStart,
    };
    float crossExtent = bounds.crossEnd - bounds.crossStart;
    scope.extent = vertical ? PrVec2{crossExtent, frame.mainExtent} : PrVec2{frame.mainExtent, crossExtent};

    for (uint32_t v = 0; v < scope.nodes.count; ++v) {
        if (layered.isGroup.data[v] != 0) {
            scope.nodeRects.data[v] = toWorldRect(frame, Box{
                layered.groupMainStart.data[v],
                layered.groupMainEnd.data[v], layered.groupLeft.data[v], layered.groupRight.data[v],
            });
        } else {
            scope.nodeRects.data[v] = toWorldRect(frame, vertexBox(layered.vertices.data[layered.nodeVertex.data[v]]));
        }
    }
    for (uint32_t p = 0; p < scope.ports.count; ++p) {
        const GraphVertex& vertex = layered.vertices.data[layered.nodeVertex.data[scope.ports.data[p].node]];
        scope.portPositions.data[p] = toWorld(frame, portPoint(layered, vertex, p));
    }
    for (uint32_t i = 0; i < scope.labels.count; ++i)
        scope.labelRects.data[i] = toWorldRect(frame, labelBoxes.data[i]);
    scope.routePoints.clear();
    for (uint32_t e = 0; e < edgeCount; ++e) {
        scope.routeStart.data[e] = scope.routePoints.count;
        for (uint32_t i = routeStart.data[e]; i < routeStart.data[e + 1]; ++i)
            scope.routePoints.push(toWorld(frame, routes.data[i]));
    }
    scope.routeStart.data[edgeCount] = scope.routePoints.count;

    bows.reset();
    path.points.reset();
    path.segments.reset();
    routes.reset();
    routeStart.reset();
    labelBoxes.reset();
}

} // namespace pr_impl
