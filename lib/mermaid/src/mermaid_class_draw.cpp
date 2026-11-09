// Class diagrams as drawn (mermaid_class_draw.h). Each class is a box of three compartments: its annotation and name,
// its attributes, and its methods, sized as Mermaid sizes them. lib/graph lays out the classes, notes and namespaces,
// and routes the relations, whose marks are Mermaid's.

#include "mermaid/src/mermaid_class_draw.h"

#include "pr/float.h"
#include "pr/geom.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_class.h"

namespace pr_impl {

namespace {

// Mermaid's class diagram measurements, in px.
constexpr float kFontSize = 16;
constexpr float kLineHeight = 1.5f;         // a label's line height, by its font size
constexpr float kPadding = 12;              // class.padding: around and between a class's compartments
constexpr float kNotePadding = 6;           // between a note's text and its box
constexpr float kCardinalitySize = 11;
constexpr float kEdgeLabelPadding = 4;
constexpr float kClusterPadding = 12;
constexpr float kTitleFontSize = 18;
constexpr float kTitleGap = 10;
constexpr float kDashed[2] = {3, 3};        // a relation drawn with ..
constexpr float kDotted[2] = {2, 2};        // the link from a note to its class

// A class's box as sized: its extent, and the offsets from its top to its dividers and to its methods.
struct ClassBox {
    PrVec2 extent;
    float firstDivider;
    float secondDivider;
    float methodsTop;
};

// A row of a class's box: an attribute or a method.
struct Row {
    uint32_t node;
    MermaidLabelIndex label;
    bool method;
};

struct Layout {
    MermaidRender* render;
    const MermaidClassDiagram* diagram;
    RawArray<MermaidLabelIndex> names;          // by node: a class's name, or the text of an interface or note
    RawArray<MermaidLabelIndex> annotations;    // by node, or kMermaidNone
    RawArray<ClassBox> boxes;                   // by node
    RawArray<Row> rows;                         // by class, each class's attributes before its methods
    RawArray<uint32_t> rowStart;                // by node: its first row, and at the next node the end of its rows
    RawArray<PrGraphNode> graphNodes;           // by node
    RawArray<PrGraphNode> graphSpaces;          // by namespace
    RawArray<MermaidLabelIndex> spaceLabels;    // by namespace
    RawArray<PrGraphEdge> graphEdges;           // by relation
    RawArray<MermaidLabelIndex> edgeLabels;     // by relation
    RawArray<PrGraphLabel> graphLabels;         // by relation, or kMermaidNone

    // By relation: the cardinalities at its source and its target, and their places in the graph, or kMermaidNone.
    RawArray<MermaidLabelIndex> sourceLabels;
    RawArray<MermaidLabelIndex> targetLabels;
    RawArray<PrGraphLabel> sourcePlaces;
    RawArray<PrGraphLabel> targetPlaces;
};

void resetLayout(Layout& l) {
    l.names.reset();
    l.annotations.reset();
    l.boxes.reset();
    l.rows.reset();
    l.rowStart.reset();
    l.graphNodes.reset();
    l.graphSpaces.reset();
    l.spaceLabels.reset();
    l.graphEdges.reset();
    l.edgeLabels.reset();
    l.graphLabels.reset();
    l.sourceLabels.reset();
    l.targetLabels.reset();
    l.sourcePlaces.reset();
    l.targetPlaces.reset();
}

PrStringView textOf(const Layout& l, MermaidSpan span) {
    return mermaidClassText(*l.diagram, span);
}

MermaidTextStyle styleOf(float size) {
    MermaidTextStyle style = mermaidTextStyle(size);
    style.lineHeight = size * kLineHeight;
    return style;
}

MermaidLabelIndex plainLabel(Layout& l, PrStringView text, float size) {
    return mermaidLabelCreate(*l.render, text, MermaidTextFormat_Plain, styleOf(size));
}

// Appends `text` with the characters that HTML reads as markup escaped.
void appendEscaped(RawArray<char>& out, PrStringView text) {
    for (size_t i = 0; i < text.size; ++i) {
        char c = text.data[i];
        if (c == '<')
            out.append("&lt;", 4);
        else if (c == '>')
            out.append("&gt;", 4);
        else if (c == '&')
            out.append("&amp;", 5);
        else
            out.push(c);
    }
}

// A member's label: underlined if it is static, and italic if it is abstract.
MermaidLabelIndex memberLabel(Layout& l, const MermaidClassMember& member) {
    RawArray<char> html{};
    bool underline = member.classifier == MermaidClassifier_Static;
    if (underline)
        html.append("<u>", 3);
    appendEscaped(html, textOf(l, member.text));
    if (underline)
        html.append("</u>", 4);
    MermaidTextStyle style = styleOf(kFontSize);
    style.italic = member.classifier == MermaidClassifier_Abstract;
    style.align = PrTextAlign_Left;
    MermaidLabelIndex label = mermaidLabelCreate(*l.render, PrStringView{html.data, html.count}, MermaidTextFormat_Html, style);
    html.reset();
    return label;
}

PrVec2 extentOf(const Layout& l, MermaidLabelIndex label) {
    return mermaidLabelExtent(*l.render, label);
}

// Sizes a class's box as Mermaid does. The name is centered, and the rows start at the padding from the left, so the
// box is as wide as half the name, plus the wider of half the name and the widest row, plus the padding at either side.
// An empty compartment is the padding twice over, and a class with no rows has two shallower compartments.
ClassBox measureClass(const Layout& l, uint32_t node) {
    PrVec2 name = extentOf(l, l.names.data[node]);
    PrVec2 annotation = extentOf(l, l.annotations.data[node]);
    float half = prF32Max(name.x, annotation.x) * 0.5f;
    float widest = 0;
    float attributes = 0;
    float methods = 0;
    uint32_t attributeCount = 0;
    uint32_t methodCount = 0;
    for (uint32_t i = l.rowStart.data[node]; i < l.rowStart.data[node + 1]; ++i) {
        const Row& row = l.rows.data[i];
        PrVec2 extent = extentOf(l, row.label);
        widest = prF32Max(widest, extent.x);
        if (row.method) {
            methods += extent.y;
            ++methodCount;
        } else {
            attributes += extent.y;
            ++attributeCount;
        }
    }
    ClassBox box{};
    box.extent.x = half + prF32Max(half, widest) + 2 * kPadding;
    box.firstDivider = 2 * kPadding + annotation.y + name.y;
    float bottom = 0;
    if (attributeCount == 0 && methodCount == 0) {
        box.secondDivider = box.firstDivider + 1.5f * kPadding;
        bottom = box.secondDivider + 1.5f * kPadding;
    } else {
        box.secondDivider = box.firstDivider + 2 * kPadding + attributes;
        if (methodCount > 0) {
            box.methodsTop = box.secondDivider + kPadding + (attributeCount == 0 ? kPadding * 0.5f : 0);
            bottom = box.methodsTop + methods + kPadding;
        } else {
            bottom = box.secondDivider + 2 * kPadding;
        }
    }
    box.extent.y = bottom;
    return box;
}

// Shapes every label and sizes every node.
void measure(Layout& l) {
    const MermaidClassDiagram& d = *l.diagram;
    uint32_t count = d.nodes.count;
    l.names.resize(count);
    l.annotations.resize(count);
    l.boxes.resize(count);
    for (uint32_t n = 0; n < count; ++n) {
        const MermaidClassNode& node = d.nodes.data[n];
        l.annotations.data[n] = kMermaidNone;
        if (node.kind == MermaidClassNodeKind_Class) {
            MermaidTextStyle bold = styleOf(kFontSize);
            bold.weight = 700;
            l.names.data[n] = mermaidLabelCreate(*l.render, textOf(l, node.label), MermaidTextFormat_Plain, bold);
            if (node.annotation.size > 0) {
                RawArray<char> text{};
                text.append("\xC2\xAB", 2);
                PrStringView annotation = textOf(l, node.annotation);
                text.append(annotation.data, uint32_t(annotation.size));
                text.append("\xC2\xBB", 2);
                l.annotations.data[n] = plainLabel(l, PrStringView{text.data, text.count}, kFontSize);
                text.reset();
            }
        } else {
            l.names.data[n] = plainLabel(l, textOf(l, node.label), kFontSize);
        }
    }
    // The rows go by node, each node's attributes before its methods, in source order.
    l.rowStart.assignZeroed(count + 1);
    for (const MermaidClassMember& member : d.members.span())
        ++l.rowStart.data[member.node + 1];
    for (uint32_t n = 0; n < count; ++n)
        l.rowStart.data[n + 1] += l.rowStart.data[n];
    RawArray<uint32_t> next{};                  // by node: where its next row goes
    next.append(l.rowStart.data, count);
    l.rows.resize(d.members.count);
    for (int pass = 0; pass < 2; ++pass) {
        for (const MermaidClassMember& member : d.members.span()) {
            if (member.method == (pass == 1))
                l.rows.data[next.data[member.node]++] = Row{member.node, memberLabel(l, member), member.method};
        }
    }
    next.reset();
    for (uint32_t n = 0; n < count; ++n) {
        const MermaidClassNode& node = d.nodes.data[n];
        PrVec2 text = extentOf(l, l.names.data[n]);
        if (node.kind == MermaidClassNodeKind_Class)
            l.boxes.data[n] = measureClass(l, n);
        else if (node.kind == MermaidClassNodeKind_Note)
            l.boxes.data[n] = ClassBox{.extent = PrVec2{text.x + 2 * kNotePadding, text.y + 2 * kNotePadding}};
        else
            l.boxes.data[n] = ClassBox{.extent = text};
    }
}

// Sets `crossed`, by namespace, to 1 for each namespace that a relation crosses: one that holds one end of the
// relation, at any depth, and not the other.
void markCrossed(const MermaidClassDiagram& d, RawArray<uint8_t>& crossed) {
    // A namespace's depth, counted from one, so that the diagram's own scope is zero. A namespace's parent comes before
    // it.
    RawArray<uint32_t> depths{};                // by namespace
    depths.resize(d.namespaces.count);
    for (uint32_t s = 0; s < d.namespaces.count; ++s) {
        uint32_t parent = d.namespaces.data[s].parent;
        depths.data[s] = parent != kMermaidNone ? depths.data[parent] + 1 : 1;
    }
    crossed.assignZeroed(d.namespaces.count);
    for (const MermaidRelation& relation : d.relations.span()) {
        // The ends climb to the namespace that holds both, and the namespaces that they leave on the way are crossed.
        uint32_t a = d.nodes.data[relation.source].space;
        uint32_t b = d.nodes.data[relation.target].space;
        while (a != b) {
            uint32_t depthA = a != kMermaidNone ? depths.data[a] : 0;
            uint32_t depthB = b != kMermaidNone ? depths.data[b] : 0;
            uint32_t& deeper = depthA >= depthB ? a : b;
            crossed.data[deeper] = 1;
            deeper = d.namespaces.data[deeper].parent;
        }
    }
    depths.reset();
}

// Describes the diagram to `graph`: namespaces as groups, nodes inside them, and each relation with its label and
// cardinalities.
void describe(Layout& l, PrGraph* graph) {
    const MermaidClassDiagram& d = *l.diagram;
    l.graphSpaces.resize(d.namespaces.count);
    l.spaceLabels.resize(d.namespaces.count);
    RawArray<uint8_t> crossed{};                // by namespace
    markCrossed(d, crossed);
    for (uint32_t s = 0; s < d.namespaces.count; ++s) {
        const MermaidNamespace& space = d.namespaces.data[s];
        l.spaceLabels.data[s] = plainLabel(l, textOf(l, space.name), kFontSize);
        PrVec2 text = extentOf(l, l.spaceLabels.data[s]);
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.parent = space.parent != kMermaidNone ? l.graphSpaces.data[space.parent] : kPrGraphNodeNone;
        desc.extent = PrVec2{text.x + 2 * kClusterPadding, 0};
        for (float& side : desc.padding)
            side = kClusterPadding;
        desc.padding[PrRectSide_Top] = kClusterPadding + text.y;
        // As in mermaid.js, a namespace that no relation crosses turns across its parent: a diagram that runs down lays
        // out such a namespace's classes in a row, and any other diagram lays them out in a column. A namespace that
        // relations cross follows its parent.
        if (crossed.data[s] != 0)
            desc.direction = PrGraphDirection_Inherit;
        else if (d.direction == PrGraphDirection_Down)
            desc.direction = PrGraphDirection_Right;
        else
            desc.direction = PrGraphDirection_Down;
        l.graphSpaces.data[s] = prGraphAddNode(graph, &desc);
    }
    crossed.reset();
    l.graphNodes.resize(d.nodes.count);
    for (uint32_t n = 0; n < d.nodes.count; ++n) {
        const MermaidClassNode& node = d.nodes.data[n];
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.extent = l.boxes.data[n].extent;
        desc.parent = node.space != kMermaidNone ? l.graphSpaces.data[node.space] : kPrGraphNodeNone;
        l.graphNodes.data[n] = prGraphAddNode(graph, &desc);
    }
    uint32_t count = d.relations.count;
    l.graphEdges.resize(count);
    l.edgeLabels.resize(count);
    l.graphLabels.resize(count);
    l.sourceLabels.resize(count);
    l.targetLabels.resize(count);
    l.sourcePlaces.resize(count);
    l.targetPlaces.resize(count);
    for (uint32_t r = 0; r < count; ++r) {
        // Relations join classes, interfaces and notes, which are never groups.
        const MermaidRelation& relation = d.relations.data[r];
        PrGraphEdge edge = mermaidGraphAddEdge(graph, l.graphNodes.data[relation.source],
            l.graphNodes.data[relation.target], MermaidGraphPortFlag_Both);
        l.graphEdges.data[r] = edge;
        auto addLabel = [&](PrStringView text, float size, float padding, PrGraphLabelPlacement placement,
                MermaidLabelIndex* label, PrGraphLabel* place) {
            *label = kMermaidNone;
            *place = kMermaidNone;
            if (text.size == 0)
                return;
            *label = plainLabel(l, text, size);
            PrVec2 extent = extentOf(l, *label);
            PrGraphLabelDesc labelDesc{
                .edge = edge,
                .extent = PrVec2{extent.x + 2 * padding, extent.y + padding},
                .placement = placement,
            };
            *place = prGraphAddLabel(graph, &labelDesc);
        };
        addLabel(textOf(l, relation.label), kFontSize, kEdgeLabelPadding, PrGraphLabelPlacement_Center,
            &l.edgeLabels.data[r], &l.graphLabels.data[r]);
        addLabel(textOf(l, relation.sourceCardinality), kCardinalitySize, 0, PrGraphLabelPlacement_Tail,
            &l.sourceLabels.data[r], &l.sourcePlaces.data[r]);
        addLabel(textOf(l, relation.targetCardinality), kCardinalitySize, 0, PrGraphLabelPlacement_Head,
            &l.targetLabels.data[r], &l.targetPlaces.data[r]);
    }
}

// Records the mark at an end of a relation, whose tip meets the node at `tip`, pointing along `direction` into the
// node. The marks are Mermaid's markers, 17 long and 12 wide. Returns how far back from the tip the line stops.
float drawEnd(MermaidRender& render, MermaidRelationEnd end, PrVec2 tip, PrVec2 direction) {
    if (end == MermaidRelationEnd_None)
        return 0;
    MermaidInk line = mermaidInk(MermaidRole_Line);
    PrVec2 across{-direction.y, direction.x};
    auto at = [&](float along, float side) {
        return tip + direction * along + across * side;
    };
    PrPath* path = prPathCreate();
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .stroke = line,
        .strokeWidth = 1,
    };
    float back = 17;
    switch (end) {
        case MermaidRelationEnd_Extension:
            prPathMoveTo(path, tip);
            prPathLineTo(path, at(-17, 6));
            prPathLineTo(path, at(-17, -6));
            prPathClose(path);
            break;
        case MermaidRelationEnd_Composition:
        case MermaidRelationEnd_Aggregation:
            prPathMoveTo(path, tip);
            prPathLineTo(path, at(-8, 6));
            prPathLineTo(path, at(-17, 0));
            prPathLineTo(path, at(-8, -6));
            prPathClose(path);
            if (end == MermaidRelationEnd_Composition) {
                paint.flags |= MermaidPaintFlag_Fill;
                paint.fill = line;
            }
            break;
        case MermaidRelationEnd_Dependency:
            prPathMoveTo(path, tip);
            prPathLineTo(path, at(-8, 6));
            prPathLineTo(path, at(-4, 0));
            prPathLineTo(path, at(-8, -6));
            prPathClose(path);
            paint.flags |= MermaidPaintFlag_Fill;
            paint.fill = line;
            back = 4;
            break;
        case MermaidRelationEnd_Lollipop:
            prPathAddEllipse(path, at(-7, 0), PrVec2{6, 6}, 0);
            paint.flags |= MermaidPaintFlag_Fill;
            paint.fill = mermaidInk(MermaidRole_NodeFill);
            back = 13;
            break;
        default:
            break;
    }
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
    return back;
}

void drawRelation(Layout& l, PrGraph* graph, uint32_t r, PrVec2 offset, RawArray<PrVec2>& points) {
    MermaidRender& render = *l.render;
    const MermaidRelation& relation = l.diagram->relations.data[r];
    points.clear();
    if (!mermaidGraphRouteAppend(graph, l.graphEdges.data[r], offset, points))
        return;
    mermaidItemAddEdge(render, points.data, points.count, PrStringView{});
    for (int side = 0; side < 2; ++side) {
        bool atStart = side == 0;
        MermaidRelationEnd end = atStart ? relation.start : relation.end;
        PrVec2 tip = points.data[atStart ? 0 : points.count - 1];
        PrVec2 direction = mermaidRouteEndDirection(points.data, points.count, atStart);
        float back = drawEnd(render, end, tip, direction);
        mermaidRouteMoveEnd(points.data, points.count, atStart, direction * -back);
    }
    const float* dash = nullptr;
    if (relation.note)
        dash = kDotted;
    else if (relation.dashed)
        dash = kDashed;
    PrPath* path = prPathCreate();
    mermaidRouteAppend(path, points.data, points.count);
    mermaidStrokePath(render, path, mermaidInk(MermaidRole_Line), 1, dash);
    prPathDestroy(path);
}

// The paint of a node: the theme's for its kind, with its style on top.
MermaidPaint nodePaint(const MermaidClassNode& node) {
    bool note = node.kind == MermaidClassNodeKind_Note;
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

void drawClass(Layout& l, uint32_t n, PrRect r) {
    MermaidRender& render = *l.render;
    const MermaidClassNode& node = l.diagram->nodes.data[n];
    const ClassBox& box = l.boxes.data[n];
    MermaidPaint paint = nodePaint(node);
    PrPath* path = prPathCreate();
    prPathAddRect(path, r);
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
    const float dividers[] = {box.firstDivider, box.secondDivider};
    for (float y : dividers)
        mermaidDrawLine(render, PrVec2{r.left, r.top + y}, PrVec2{r.right, r.top + y}, paint.stroke, paint.strokeWidth, nullptr);
    float center = (r.left + r.right) * 0.5f;
    float y = r.top + kPadding;
    MermaidLabelIndex annotation = l.annotations.data[n];
    if (annotation != kMermaidNone) {
        PrVec2 extent = extentOf(l, annotation);
        mermaidLabelDraw(render, annotation, PrVec2{center - extent.x * 0.5f, y}, paint.color);
        y += extent.y;
    }
    PrVec2 name = extentOf(l, l.names.data[n]);
    mermaidLabelDraw(render, l.names.data[n], PrVec2{center - name.x * 0.5f, y}, paint.color);
    float attributes = r.top + box.firstDivider + kPadding;
    float methods = r.top + box.methodsTop;
    for (uint32_t i = l.rowStart.data[n]; i < l.rowStart.data[n + 1]; ++i) {
        const Row& row = l.rows.data[i];
        float& top = row.method ? methods : attributes;
        mermaidLabelDraw(render, row.label, PrVec2{r.left + kPadding, top}, paint.color);
        top += extentOf(l, row.label).y;
    }
    mermaidItemAdd(render, PrMermaidItemKind_Node, r, textOf(l, node.id), textOf(l, node.link));
}

void drawNode(Layout& l, uint32_t n, PrRect r) {
    MermaidRender& render = *l.render;
    const MermaidClassNode& node = l.diagram->nodes.data[n];
    switch (node.kind) {
        case MermaidClassNodeKind_Class:
            drawClass(l, n, r);
            return;
        case MermaidClassNodeKind_Note: {
            MermaidPaint paint = nodePaint(node);
            PrPath* path = prPathCreate();
            prPathAddRect(path, r);
            mermaidDrawPath(render, path, paint);
            prPathDestroy(path);
            mermaidLabelDrawCentered(render, l.names.data[n], prRectCenter(r), paint.color);
            mermaidItemAdd(render, PrMermaidItemKind_Label, r, PrStringView{}, PrStringView{});
            return;
        }
        default:
            mermaidLabelDrawCentered(render, l.names.data[n], prRectCenter(r), nodePaint(node).color);
            mermaidItemAdd(render, PrMermaidItemKind_Node, r, textOf(l, node.label), PrStringView{});
            return;
    }
}

} // namespace

PrStatus mermaidClassDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidClassDiagram diagram{};
    PrStatus status = mermaidClassParse(diagram, source, error);
    if (status != PrStatus_Ok) {
        mermaidClassReset(diagram);
        return status;
    }
    Layout l{.render = &render, .diagram = &diagram};
    measure(l);
    PrGraph* graph = prGraphCreate();
    describe(l, graph);
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    options.direction = diagram.direction;
    options.routing = PrGraphRouting_Spline;
    prGraphLayoutCompute(graph, &options);
    PrVec2 offset = mermaidGraphPlace(render, graph, textOf(l, diagram.title), mermaidTextStyle(kTitleFontSize),
        kTitleGap, mermaidInk(MermaidRole_Title));

    // Namespaces, outermost first, then the relations, their labels, and the nodes on top.
    for (uint32_t s = 0; s < diagram.namespaces.count; ++s) {
        PrRect r = prRectOffset(prGraphNodeRect(graph, l.graphSpaces.data[s]), offset);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke,
            .fill = mermaidInk(MermaidRole_ClusterFill),
            .stroke = mermaidInk(MermaidRole_ClusterStroke),
        };
        PrPath* path = prPathCreate();
        prPathAddRect(path, r);
        mermaidDrawPath(render, path, paint);
        prPathDestroy(path);
        PrVec2 extent = extentOf(l, l.spaceLabels.data[s]);
        mermaidLabelDraw(render, l.spaceLabels.data[s], PrVec2{
            (r.left + r.right - extent.x) * 0.5f, r.top + kClusterPadding * 0.5f,
        }, mermaidInk(MermaidRole_Title));
        mermaidItemAdd(render, PrMermaidItemKind_Group, r, textOf(l, diagram.namespaces.data[s].name), PrStringView{});
    }
    RawArray<PrVec2> points{};
    for (uint32_t r = 0; r < diagram.relations.count; ++r)
        drawRelation(l, graph, r, offset, points);
    points.reset();
    MermaidInk text = mermaidInk(MermaidRole_Text);
    MermaidInk background = mermaidInk(MermaidRole_EdgeLabelBackground);
    for (uint32_t r = 0; r < diagram.relations.count; ++r) {
        if (l.graphLabels.data[r] != kMermaidNone) {
            mermaidGraphLabelDraw(render, graph, l.graphLabels.data[r], offset, l.edgeLabels.data[r], text,
                &background);
        }
        // The cardinalities have no background, and no items.
        if (l.sourcePlaces.data[r] != kMermaidNone) {
            PrRect rect = prRectOffset(prGraphLabelRect(graph, l.sourcePlaces.data[r]), offset);
            mermaidLabelDrawCentered(render, l.sourceLabels.data[r], prRectCenter(rect), text);
        }
        if (l.targetPlaces.data[r] != kMermaidNone) {
            PrRect rect = prRectOffset(prGraphLabelRect(graph, l.targetPlaces.data[r]), offset);
            mermaidLabelDrawCentered(render, l.targetLabels.data[r], prRectCenter(rect), text);
        }
    }
    for (uint32_t n = 0; n < diagram.nodes.count; ++n)
        drawNode(l, n, prRectOffset(prGraphNodeRect(graph, l.graphNodes.data[n]), offset));

    prGraphDestroy(graph);
    resetLayout(l);
    mermaidClassReset(diagram);
    return PrStatus_Ok;
}

} // namespace pr_impl
