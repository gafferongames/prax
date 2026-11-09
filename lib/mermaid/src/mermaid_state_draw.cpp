// State diagrams as drawn (mermaid_state_draw.h). States, pseudostates and notes are sized as Mermaid sizes them.
// Composite states and their concurrent regions are lib/graph's groups. Transitions end in Mermaid's barbed arrowhead,
// and meet a round or diamond node at its outline.

#include "mermaid/src/mermaid_state_draw.h"

#include "pr/float.h"
#include "pr/geom.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/raw_array.h"
#include "pr/sort.h"

#include "mermaid/src/mermaid_state.h"

namespace pr_impl {

namespace {

// Mermaid's state diagram measurements, in px.
constexpr float kFontSize = 16;
constexpr float kLineHeight = 1.5f;         // a label's line height, by its font size
constexpr float kStatePadding = 8;          // around a state's name
constexpr float kMinLabelWidth = 120;       // the least width of a state's or note's name
constexpr float kNotePadding = 15;
constexpr float kRadius = 5;                // the corners of a state and of a composite state
constexpr float kDotRadius = 7;             // a start or an end
constexpr float kEndDotRadius = 2.5f;       // the dot inside an end
constexpr float kBarLength = 70;            // a fork or a join
constexpr float kBarWidth = 10;
constexpr float kChoiceSize = 28;
constexpr float kTitleBand = 26;            // the band of a composite state's name, above its inside
constexpr float kBottomBand = 4;            // below a composite state's inside
constexpr float kGroupPadding = 12;         // between a composite state's inside and what it holds
constexpr float kRegionPadding = 16;
constexpr float kEdgeLabelPadding = 4;
constexpr float kTitleFontSize = 18;
constexpr float kTitleGap = 10;
constexpr float kNoteDash[2] = {5, 5};
constexpr float kRegionDash[2] = {10, 10};

// A state with a description: the offsets from its top to its divider and to its description.
struct Described {
    float divider;
    float description;
};

struct Layout {
    MermaidRender* render;
    const MermaidStateDiagram* diagram;
    RawArray<MermaidLabelIndex> labels;         // by node: its name, a note's text, or kMermaidNone
    RawArray<MermaidLabelIndex> descriptions;   // by node, or kMermaidNone
    RawArray<Described> described;              // by node
    RawArray<uint8_t> groups;                   // by node: 1 for a composite state or a region
    RawArray<PrGraphNode> graphNodes;           // by node
    RawArray<PrRect> rects;                     // by node, once laid out
    RawArray<uint32_t> order;                   // the nodes by depth, then in source order
    RawArray<PrGraphEdge> graphEdges;           // by transition
    RawArray<MermaidLabelIndex> edgeLabels;     // by transition, or kMermaidNone
    RawArray<PrGraphLabel> graphLabels;         // by transition, or kMermaidNone
};

void resetLayout(Layout& l) {
    l.labels.reset();
    l.descriptions.reset();
    l.described.reset();
    l.groups.reset();
    l.graphNodes.reset();
    l.rects.reset();
    l.order.reset();
    l.graphEdges.reset();
    l.edgeLabels.reset();
    l.graphLabels.reset();
}

PrStringView textOf(const Layout& l, MermaidSpan span) {
    return mermaidStateText(*l.diagram, span);
}

MermaidLabelIndex label(Layout& l, PrStringView text, float size) {
    MermaidTextStyle style = mermaidTextStyle(size);
    style.lineHeight = size * kLineHeight;
    return mermaidLabelCreate(*l.render, text, MermaidTextFormat_Plain, style);
}

PrVec2 extentOf(const Layout& l, MermaidLabelIndex index) {
    return mermaidLabelExtent(*l.render, index);
}

uint32_t depthOf(const MermaidStateDiagram& d, uint32_t node) {
    uint32_t depth = 0;
    for (uint32_t n = d.nodes.data[node].parent; n != kMermaidNone; n = d.nodes.data[n].parent)
        ++depth;
    return depth;
}

// Returns true if the composite state `group` holds `node`, at any depth. The parser bounds the depth.
bool holds(const MermaidStateDiagram& d, uint32_t group, uint32_t node) {
    for (uint32_t n = d.nodes.data[node].parent; n != kMermaidNone; n = d.nodes.data[n].parent) {
        if (n == group)
            return true;
    }
    return false;
}

// Returns true if a node lays out in a row: its nearest direction, set by the composite states around it or by the
// diagram, runs left or right.
bool runsAcross(const MermaidStateDiagram& d, uint32_t node) {
    PrGraphDirection direction = d.direction;
    for (uint32_t n = d.nodes.data[node].parent; n != kMermaidNone; n = d.nodes.data[n].parent) {
        if (d.nodes.data[n].direction != PrGraphDirection_Inherit) {
            direction = d.nodes.data[n].direction;
            break;
        }
    }
    return direction == PrGraphDirection_Right || direction == PrGraphDirection_Left;
}

// Returns the extent of a node that is not a group, as Mermaid sizes it.
PrVec2 extentOfNode(Layout& l, uint32_t n) {
    const MermaidStateNode& node = l.diagram->nodes.data[n];
    PrVec2 text = extentOf(l, l.labels.data[n]);
    switch (node.kind) {
        case MermaidStateKind_Start:
        case MermaidStateKind_End:
            return PrVec2{2 * kDotRadius, 2 * kDotRadius};
        case MermaidStateKind_Fork:
        case MermaidStateKind_Join:
            if (runsAcross(*l.diagram, n))
                return PrVec2{kBarWidth, kBarLength};
            return PrVec2{kBarLength, kBarWidth};
        case MermaidStateKind_Choice:
            return PrVec2{kChoiceSize, kChoiceSize};
        case MermaidStateKind_Note:
            return PrVec2{prF32Max(text.x, kMinLabelWidth) + 2 * kNotePadding, text.y + 2 * kNotePadding};
        default:
            break;
    }
    if (l.descriptions.data[n] == kMermaidNone)
        return PrVec2{prF32Max(text.x, kMinLabelWidth) + 2 * kStatePadding, text.y + 2 * kStatePadding};
    // A state with a description has its name above a divider and the description below it.
    PrVec2 description = extentOf(l, l.descriptions.data[n]);
    Described& described = l.described.data[n];
    described.divider = 3 + text.y + 1;
    described.description = described.divider + kStatePadding;
    return PrVec2{prF32Max(text.x, description.x) + kStatePadding, described.description + description.y + 5};
}

// Describes the diagram to `graph`: composite states and regions as groups, the other nodes inside them, and each
// transition with its label.
void describe(Layout& l, PrGraph* graph) {
    const MermaidStateDiagram& d = *l.diagram;
    uint32_t count = d.nodes.count;
    l.labels.resize(count);
    l.descriptions.resize(count);
    l.described.assignZeroed(count);
    l.groups.assignZeroed(count);
    l.graphNodes.resize(count);
    l.rects.resize(count);
    for (uint32_t n = 0; n < count; ++n) {
        const MermaidStateNode& node = d.nodes.data[n];
        if (node.parent != kMermaidNone)
            l.groups.data[node.parent] = 1;
        bool named = node.kind == MermaidStateKind_State || node.kind == MermaidStateKind_Note;
        l.labels.data[n] = named ? label(l, textOf(l, node.label), kFontSize) : kMermaidNone;
        l.descriptions.data[n] =
            node.description.size > 0 ? label(l, textOf(l, node.description), kFontSize) : kMermaidNone;
    }
    // The nodes go by depth, so that each group comes before what it holds.
    RawArray<uint32_t> depths{};                // by node
    depths.resize(count);
    l.order.resize(count);
    for (uint32_t n = 0; n < count; ++n) {
        depths.data[n] = depthOf(d, n);
        l.order.data[n] = n;
    }
    sortItems(l.order.span(), [&](uint32_t a, uint32_t b) {
        return depths.data[a] < depths.data[b] || (depths.data[a] == depths.data[b] && a < b);
    });
    depths.reset();
    for (uint32_t n : l.order.span()) {
        const MermaidStateNode& node = d.nodes.data[n];
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.parent = node.parent != kMermaidNone ? l.graphNodes.data[node.parent] : kPrGraphNodeNone;
        if (l.groups.data[n] != 0) {
            desc.direction = node.direction;
            if (node.kind == MermaidStateKind_Region) {
                for (float& side : desc.padding)
                    side = kRegionPadding;
            } else {
                PrVec2 text = extentOf(l, l.labels.data[n]);
                desc.extent = PrVec2{text.x + 2 * kGroupPadding, 0};
                for (float& side : desc.padding)
                    side = kGroupPadding;
                desc.padding[PrRectSide_Top] = kTitleBand + kGroupPadding;
                desc.padding[PrRectSide_Bottom] = kBottomBand + kGroupPadding;
            }
        } else {
            desc.extent = extentOfNode(l, n);
        }
        l.graphNodes.data[n] = prGraphAddNode(graph, &desc);
    }
    uint32_t edges = d.transitions.count;
    l.graphEdges.resize(edges);
    l.edgeLabels.resize(edges);
    l.graphLabels.resize(edges);
    for (uint32_t t = 0; t < edges; ++t) {
        const MermaidTransition& transition = d.transitions.data[t];
        l.graphEdges.data[t] = kMermaidNone;
        l.edgeLabels.data[t] = kMermaidNone;
        l.graphLabels.data[t] = kMermaidNone;
        // As in a flowchart, a transition between a composite state and what it holds has no direction, so the layout
        // does not join them, and it is not drawn.
        if (holds(d, transition.source, transition.target) || holds(d, transition.target, transition.source))
            continue;
        // A box's transitions meet it at separate points. A small node's meet at its center, and are clipped to its
        // outline. A group has no ports.
        auto hasPort = [&](uint32_t node) {
            const MermaidStateNode& end = d.nodes.data[node];
            bool box = end.kind == MermaidStateKind_State || end.kind == MermaidStateKind_Note;
            return box && l.groups.data[node] == 0;
        };
        MermaidGraphPortFlags ports = 0;
        if (hasPort(transition.source))
            ports |= MermaidGraphPortFlag_Source;
        if (hasPort(transition.target))
            ports |= MermaidGraphPortFlag_Target;
        l.graphEdges.data[t] = mermaidGraphAddEdge(graph, l.graphNodes.data[transition.source],
            l.graphNodes.data[transition.target], ports);
        if (transition.label.size > 0) {
            l.edgeLabels.data[t] = label(l, textOf(l, transition.label), kFontSize);
            PrVec2 text = extentOf(l, l.edgeLabels.data[t]);
            PrGraphLabelDesc labelDesc{
                .edge = l.graphEdges.data[t],
                .extent = PrVec2{text.x + 2 * kEdgeLabelPadding, text.y + kEdgeLabelPadding},
                .placement = PrGraphLabelPlacement_Center,
            };
            l.graphLabels.data[t] = prGraphAddLabel(graph, &labelDesc);
        }
    }
}

// Returns the distance from a node's center to its outline along the unit direction `u`, for a node whose outline is
// not its box: a circle or a diamond. Returns a negative value for a box.
float reach(const Layout& l, uint32_t n, PrVec2 u) {
    const MermaidStateNode& node = l.diagram->nodes.data[n];
    if (node.kind == MermaidStateKind_Start || node.kind == MermaidStateKind_End)
        return kDotRadius;
    if (node.kind == MermaidStateKind_Choice) {
        float half = kChoiceSize * 0.5f;
        return 1 / (prF32Abs(u.x) / half + prF32Abs(u.y) / half);
    }
    return -1;
}

void drawTransition(Layout& l, PrGraph* graph, uint32_t t, PrVec2 offset, RawArray<PrVec2>& points) {
    MermaidRender& render = *l.render;
    const MermaidTransition& transition = l.diagram->transitions.data[t];
    if (l.graphEdges.data[t] == kMermaidNone)
        return;
    points.clear();
    if (!mermaidGraphRouteAppend(graph, l.graphEdges.data[t], offset, points))
        return;
    // An end at a circle or a diamond moves from its box to its outline.
    if (transition.source != transition.target) {
        for (int side = 0; side < 2; ++side) {
            bool atStart = side == 0;
            uint32_t node = atStart ? transition.source : transition.target;
            PrVec2 direction = mermaidRouteEndDirection(points.data, points.count, atStart);
            float distance = reach(l, node, direction);
            if (distance < 0)
                continue;
            PrVec2 end = points.data[atStart ? 0 : points.count - 1];
            PrVec2 outline = prRectCenter(l.rects.data[node]) - direction * distance;
            mermaidRouteMoveEnd(points.data, points.count, atStart, outline - end);
        }
    }
    MermaidInk line = mermaidInk(MermaidRole_Line);
    mermaidItemAddEdge(render, points.data, points.count, PrStringView{});
    if (!transition.note) {
        // Mermaid's barbed arrowhead, M 19,7 L9,13 L14,7 L9,1 Z, with its tip at the target.
        PrVec2 tip = points.data[points.count - 1];
        PrVec2 direction = mermaidRouteEndDirection(points.data, points.count, false);
        PrVec2 across{-direction.y, direction.x};
        PrPath* head = prPathCreate();
        prPathMoveTo(head, tip);
        prPathLineTo(head, tip - direction * 10 + across * 6);
        prPathLineTo(head, tip - direction * 5);
        prPathLineTo(head, tip - direction * 10 - across * 6);
        prPathClose(head);
        mermaidFillPath(render, head, line);
        prPathDestroy(head);
        mermaidRouteMoveEnd(points.data, points.count, false, direction * -5);
    }
    PrPath* path = prPathCreate();
    mermaidRouteAppend(path, points.data, points.count);
    mermaidStrokePath(render, path, line, 1, transition.note ? kNoteDash : nullptr);
    prPathDestroy(path);
}

void fillAndStroke(MermaidRender& render, const PrPath* path, MermaidInk fill, MermaidInk stroke, float width) {
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .fill = fill,
        .stroke = stroke,
        .strokeWidth = width,
    };
    mermaidDrawPath(render, path, paint);
}

void addRounded(PrPath* path, PrRect r, float radius) {
    PrCornerRect rounded{.rect = r};
    for (PrCorner& corner : rounded.corners)
        corner = PrCorner{.radii = PrVec2{radius, radius}, .shape = 1};
    prPathAddCornerRect(path, rounded);
}

// The paint of a state: the theme's, with its style on top.
MermaidPaint statePaint(const MermaidStateNode& node) {
    bool note = node.kind == MermaidStateKind_Note;
    MermaidPaint paint{
        .flags =
            MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_Color | MermaidPaintFlag_StrokeWidth,
        .fill = mermaidInk(note ? MermaidRole_NoteFill : MermaidRole_NodeFill),
        .stroke = mermaidInk(note ? MermaidRole_NoteStroke : MermaidRole_NodeStroke),
        .color = mermaidInk(note ? MermaidRole_StrongText : MermaidRole_NodeText),
        .strokeWidth = 1,
    };
    mermaidPaintMerge(paint, node.style);
    return paint;
}

// Records a composite state's box, its name in the band above its inside, or a region's dashed box.
void drawGroup(Layout& l, uint32_t n) {
    MermaidRender& render = *l.render;
    const MermaidStateNode& node = l.diagram->nodes.data[n];
    PrRect r = l.rects.data[n];
    PrPath* path = prPathCreate();
    if (node.kind == MermaidStateKind_Region) {
        prPathAddRect(path, r);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_Dash,
            .fill = mermaidInk(MermaidRole_AltBackground),
            .stroke = mermaidInk(MermaidRole_NodeStroke),
            .dash = {kRegionDash[0], kRegionDash[1]},
        };
        mermaidDrawPath(render, path, paint);
        prPathDestroy(path);
        return;
    }
    MermaidPaint paint = statePaint(node);
    addRounded(path, r, kRadius);
    mermaidDrawPath(render, path, paint);
    prPathClear(path);
    prPathAddRect(path, PrRect{r.left, r.top + kTitleBand, r.right, r.bottom - kBottomBand});
    fillAndStroke(render, path, mermaidInk(MermaidRole_Background), paint.stroke, paint.strokeWidth);
    prPathDestroy(path);
    PrVec2 text = extentOf(l, l.labels.data[n]);
    mermaidLabelDraw(render, l.labels.data[n], PrVec2{(r.left + r.right - text.x) * 0.5f, r.top + 1}, paint.color);
    mermaidItemAdd(render, PrMermaidItemKind_Group, r, textOf(l, node.id), textOf(l, node.link));
}

void drawNode(Layout& l, uint32_t n) {
    MermaidRender& render = *l.render;
    const MermaidStateNode& node = l.diagram->nodes.data[n];
    PrRect r = l.rects.data[n];
    PrVec2 c = prRectCenter(r);
    MermaidInk line = mermaidInk(MermaidRole_Line);
    PrPath* path = prPathCreate();
    switch (node.kind) {
        case MermaidStateKind_Start:
            prPathAddEllipse(path, c, PrVec2{kDotRadius, kDotRadius}, 0);
            mermaidFillPath(render, path, line);
            break;
        case MermaidStateKind_End:
            prPathAddEllipse(path, c, PrVec2{kDotRadius, kDotRadius}, 0);
            fillAndStroke(render, path, mermaidInk(MermaidRole_NodeFill), line, 2);
            prPathClear(path);
            prPathAddEllipse(path, c, PrVec2{kEndDotRadius, kEndDotRadius}, 0);
            mermaidFillPath(render, path, mermaidInk(MermaidRole_NodeStroke));
            break;
        case MermaidStateKind_Fork:
        case MermaidStateKind_Join:
            prPathAddRect(path, r);
            mermaidFillPath(render, path, line);
            break;
        case MermaidStateKind_Choice: {
            prPathMoveTo(path, PrVec2{c.x, r.top});
            prPathLineTo(path, PrVec2{r.right, c.y});
            prPathLineTo(path, PrVec2{c.x, r.bottom});
            prPathLineTo(path, PrVec2{r.left, c.y});
            prPathClose(path);
            MermaidPaint paint = statePaint(node);
            mermaidDrawPath(render, path, paint);
            break;
        }
        case MermaidStateKind_Note: {
            prPathAddRect(path, r);
            MermaidPaint paint = statePaint(node);
            mermaidDrawPath(render, path, paint);
            mermaidLabelDrawCentered(render, l.labels.data[n], c, paint.color);
            prPathDestroy(path);
            mermaidItemAdd(render, PrMermaidItemKind_Label, r, PrStringView{}, PrStringView{});
            return;
        }
        default: {
            MermaidPaint paint = statePaint(node);
            addRounded(path, r, kRadius);
            mermaidDrawPath(render, path, paint);
            MermaidLabelIndex description = l.descriptions.data[n];
            if (description == kMermaidNone) {
                mermaidLabelDrawCentered(render, l.labels.data[n], c, paint.color);
            } else {
                const Described& described = l.described.data[n];
                PrVec2 text = extentOf(l, l.labels.data[n]);
                mermaidLabelDraw(render, l.labels.data[n], PrVec2{c.x - text.x * 0.5f, r.top + 3}, paint.color);
                float y = r.top + described.divider;
                mermaidDrawLine(render, PrVec2{r.left, y}, PrVec2{r.right, y}, paint.stroke, paint.strokeWidth, nullptr);
                PrVec2 below = extentOf(l, description);
                mermaidLabelDraw(render, description, PrVec2{c.x - below.x * 0.5f, r.top + described.description},
                    paint.color);
            }
            break;
        }
    }
    prPathDestroy(path);
    mermaidItemAdd(render, PrMermaidItemKind_Node, r, textOf(l, node.id), textOf(l, node.link));
}

} // namespace

PrStatus mermaidStateDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidStateDiagram diagram{};
    PrStatus status = mermaidStateParse(diagram, source, error);
    if (status != PrStatus_Ok) {
        mermaidStateReset(diagram);
        return status;
    }
    Layout l{.render = &render, .diagram = &diagram};
    PrGraph* graph = prGraphCreate();
    describe(l, graph);
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    options.direction = diagram.direction;
    options.routing = PrGraphRouting_Spline;
    prGraphLayoutCompute(graph, &options);
    PrVec2 offset = mermaidGraphPlace(render, graph, textOf(l, diagram.title), mermaidTextStyle(kTitleFontSize),
        kTitleGap, mermaidInk(MermaidRole_Title));
    for (uint32_t n = 0; n < diagram.nodes.count; ++n)
        l.rects.data[n] = prRectOffset(prGraphNodeRect(graph, l.graphNodes.data[n]), offset);

    // Composite states and regions, outermost first, then the transitions, their labels, and the nodes on top.
    for (uint32_t n : l.order.span()) {
        if (l.groups.data[n] != 0)
            drawGroup(l, n);
    }
    RawArray<PrVec2> points{};
    for (uint32_t t = 0; t < diagram.transitions.count; ++t)
        drawTransition(l, graph, t, offset, points);
    points.reset();
    MermaidInk background = mermaidInk(MermaidRole_EdgeLabelBackground);
    for (uint32_t t = 0; t < diagram.transitions.count; ++t) {
        if (l.graphLabels.data[t] != kMermaidNone) {
            mermaidGraphLabelDraw(render, graph, l.graphLabels.data[t], offset, l.edgeLabels.data[t],
                mermaidInk(MermaidRole_Text), &background);
        }
    }
    for (uint32_t n = 0; n < diagram.nodes.count; ++n) {
        if (l.groups.data[n] == 0)
            drawNode(l, n);
    }

    prGraphDestroy(graph);
    resetLayout(l);
    mermaidStateReset(diagram);
    return PrStatus_Ok;
}

} // namespace pr_impl
