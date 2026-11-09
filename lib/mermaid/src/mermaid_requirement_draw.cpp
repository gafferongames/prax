// Requirement diagrams as drawn (mermaid_requirement_draw.h). A block is a box with its kind between double angle
// brackets and its name in bold, centered, then a divider and its fields, one to a line. lib/graph lays out the blocks
// and routes the relations, which carry their kind as a label. A contains relation is solid, with a circled cross at
// its source. The others are dashed, with an open arrowhead at their target.

#include "mermaid/src/mermaid_requirement_draw.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_requirement.h"

namespace pr_impl {

namespace {

// Mermaid's requirement diagram measurements, in px.
constexpr float kFontSize = 16;
constexpr float kLineHeight = 1.5f;         // a label's line height, by its font size
constexpr float kPaddingX = 10;             // at either side of a block's widest label
constexpr float kPaddingTop = 8;            // above a block's kind
constexpr float kPaddingBottom = 12;        // below a block's last label
constexpr float kFieldGap = 20;             // between a block's name and its first field
constexpr float kDividerGap = 8;            // between the divider and the first field
constexpr float kStrokeWidth = 1.3f;        // a block's outline and divider
constexpr float kArrowSize = 20;            // an arrowhead's length, and its width
constexpr float kContainsRadius = 9;        // the circle of a contains relation
constexpr float kDashed[2] = {10, 7};       // every relation but contains
constexpr float kTitleGap = 30;             // between the title and the blocks

// mermaid.js's spacing between nodes and between ranks. The layout adds the height of the relations' labels to the
// space between layers, as dagre does.
constexpr float kNodeSpacing = 50;
constexpr float kLayerSpacing = 50;

// The longest number of fields that a block has: a requirement's ID, text, risk and verification method.
constexpr uint32_t kMaxFields = 4;

// By MermaidRequirementKind.
constexpr PrStringView kKindNames[] = {
    "<<Requirement>>",
    "<<Functional Requirement>>",
    "<<Interface Requirement>>",
    "<<Performance Requirement>>",
    "<<Physical Requirement>>",
    "<<Design Constraint>>",
    "<<Element>>",
};
static_assert(countOf(kKindNames) == MermaidRequirementKind_Element + 1, "a name for each kind");

// By MermaidRisk and MermaidVerifyMethod, with None empty.
constexpr PrStringView kRiskNames[] = {"", "Low", "Medium", "High"};
constexpr PrStringView kVerifyNames[] = {"", "Analysis", "Demonstration", "Inspection", "Test"};

// A block's labels and extent. A label is kMermaidNone if its text is empty.
struct BlockBox {
    MermaidLabelIndex kind;
    MermaidLabelIndex name;
    MermaidLabelIndex fields[kMaxFields];
    uint32_t fieldCount;
    PrVec2 extent;
};

struct Layout {
    MermaidRender* render;
    const MermaidRequirementDiagram* diagram;
    RawArray<BlockBox> boxes;                   // by block
    RawArray<PrGraphNode> graphNodes;           // by block
    RawArray<PrGraphEdge> graphEdges;           // by link
    RawArray<MermaidLabelIndex> edgeLabels;     // by link
    RawArray<PrGraphLabel> graphLabels;         // by link
};

void resetLayout(Layout& l) {
    l.boxes.reset();
    l.graphNodes.reset();
    l.graphEdges.reset();
    l.edgeLabels.reset();
    l.graphLabels.reset();
}

PrStringView textOf(const Layout& l, MermaidSpan span) {
    return mermaidRequirementText(*l.diagram, span);
}

MermaidTextStyle textStyle(float weight) {
    MermaidTextStyle style = mermaidTextStyle(kFontSize);
    style.lineHeight = kFontSize * kLineHeight;
    style.weight = weight;
    return style;
}

PrVec2 extentOf(const Layout& l, MermaidLabelIndex index) {
    return mermaidLabelExtent(*l.render, index);
}

// Adds a field's label, "prefix: value", to `box` if the value is not empty.
void addField(Layout& l, BlockBox& box, PrStringView prefix, PrStringView value, RawArray<char>& text) {
    if (value.size == 0)
        return;
    text.clear();
    text.append(prefix.data, uint32_t(prefix.size));
    text.append(value.data, uint32_t(value.size));
    box.fields[box.fieldCount++] = mermaidLabelCreate(*l.render, PrStringView{text.data, text.count},
        MermaidTextFormat_Markdown, textStyle(400));
}

// Shapes every block's labels and sizes the block as mermaid.js's requirementBox does: as wide as its widest label and
// its padding, and as tall as its labels stacked, with the gap before the fields if it has any.
void measure(Layout& l) {
    const MermaidRequirementDiagram& d = *l.diagram;
    MermaidRender& render = *l.render;
    l.boxes.assignZeroed(d.blocks.count);
    RawArray<char> text{};
    for (uint32_t b = 0; b < d.blocks.count; ++b) {
        const MermaidRequirementBlock& block = d.blocks.data[b];
        BlockBox& box = l.boxes.data[b];
        box.kind = mermaidLabelCreate(render, kKindNames[block.kind], MermaidTextFormat_Plain, textStyle(400));
        box.name = mermaidLabelCreate(render, textOf(l, block.name), MermaidTextFormat_Markdown, textStyle(700));
        if (block.kind == MermaidRequirementKind_Element) {
            addField(l, box, "Type: ", textOf(l, block.type), text);
            addField(l, box, "Doc Ref: ", textOf(l, block.docRef), text);
        } else {
            addField(l, box, "ID: ", textOf(l, block.id), text);
            addField(l, box, "Text: ", textOf(l, block.text), text);
            addField(l, box, "Risk: ", kRiskNames[block.risk], text);
            addField(l, box, "Verification: ", kVerifyNames[block.verifyMethod], text);
        }
        PrVec2 kind = extentOf(l, box.kind);
        PrVec2 name = extentOf(l, box.name);
        float width = prF32Max(kind.x, name.x);
        float height = kind.y + name.y;
        if (box.fieldCount > 0)
            height += kFieldGap;
        for (uint32_t f = 0; f < box.fieldCount; ++f) {
            PrVec2 field = extentOf(l, box.fields[f]);
            width = prF32Max(width, field.x);
            height += field.y;
        }
        box.extent = PrVec2{width + 2 * kPaddingX, height + kPaddingTop + kPaddingBottom};
    }
    text.reset();
}

// Describes the blocks to the graph, the requirements before the elements as mermaid.js orders its nodes, and the
// relations in source order.
void describe(Layout& l, PrGraph* graph) {
    const MermaidRequirementDiagram& d = *l.diagram;
    l.graphNodes.resize(d.blocks.count);
    for (int pass = 0; pass < 2; ++pass) {
        bool elements = pass == 1;
        for (uint32_t b = 0; b < d.blocks.count; ++b) {
            if ((d.blocks.data[b].kind == MermaidRequirementKind_Element) != elements)
                continue;
            PrGraphNodeDesc desc = prGraphNodeDescDefault();
            desc.extent = l.boxes.data[b].extent;
            l.graphNodes.data[b] = prGraphAddNode(graph, &desc);
        }
    }
    uint32_t count = d.links.count;
    l.graphEdges.resize(count);
    l.edgeLabels.resize(count);
    l.graphLabels.resize(count);
    RawArray<char> text{};
    for (uint32_t r = 0; r < count; ++r) {
        const MermaidRequirementLink& link = d.links.data[r];
        l.graphEdges.data[r] = mermaidGraphAddEdge(graph, l.graphNodes.data[link.source],
            l.graphNodes.data[link.target], MermaidGraphPortFlag_Both);
        PrStringView name = mermaidRequirementRelationName(link.relation);
        text.clear();
        text.append("<<", 2);
        text.append(name.data, uint32_t(name.size));
        text.append(">>", 2);
        l.edgeLabels.data[r] = mermaidLabelCreate(*l.render, PrStringView{text.data, text.count},
            MermaidTextFormat_Plain, textStyle(400));
        PrGraphLabelDesc labelDesc{
            .edge = l.graphEdges.data[r],
            .extent = extentOf(l, l.edgeLabels.data[r]),
            .placement = PrGraphLabelPlacement_Center,
        };
        l.graphLabels.data[r] = prGraphAddLabel(graph, &labelDesc);
    }
    text.reset();
}

// Records a contains relation's mark at its source: a circle that the line enters at `start`, with a cross through it
// along the line and across it. `direction` points from the line toward the source.
void drawContains(MermaidRender& render, PrVec2 start, PrVec2 direction) {
    MermaidInk line = mermaidInk(MermaidRole_Line);
    PrVec2 across{-direction.y, direction.x};
    PrVec2 center = start - direction * (kContainsRadius + 1);
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, center, PrVec2{kContainsRadius, kContainsRadius}, 0);
    prPathMoveTo(path, center - direction * kContainsRadius);
    prPathLineTo(path, center + direction * kContainsRadius);
    prPathMoveTo(path, center - across * kContainsRadius);
    prPathLineTo(path, center + across * kContainsRadius);
    mermaidStrokePath(render, path, line, 1, nullptr);
    prPathDestroy(path);
}

void drawLink(Layout& l, PrGraph* graph, uint32_t r, PrVec2 offset, RawArray<PrVec2>& points) {
    MermaidRender& render = *l.render;
    const MermaidRequirementLink& link = l.diagram->links.data[r];
    points.clear();
    if (!mermaidGraphRouteAppend(graph, l.graphEdges.data[r], offset, points))
        return;
    mermaidItemAddEdge(render, points.data, points.count, PrStringView{});
    bool contains = link.relation == MermaidRequirementRelation_Contains;
    MermaidInk line = mermaidInk(MermaidRole_Line);
    PrPath* path = prPathCreate();
    mermaidRouteAppend(path, points.data, points.count);
    mermaidStrokePath(render, path, line, 1, contains ? nullptr : kDashed);
    prPathDestroy(path);
    if (contains) {
        drawContains(render, points.data[0], mermaidRouteEndDirection(points.data, points.count, true));
    } else {
        mermaidDrawMarker(render, MermaidMarker_OpenArrow, points.data[points.count - 1],
            mermaidRouteEndDirection(points.data, points.count, false), line, kArrowSize);
    }
}

void drawBlock(Layout& l, uint32_t b, PrRect r) {
    MermaidRender& render = *l.render;
    const MermaidRequirementBlock& block = l.diagram->blocks.data[b];
    const BlockBox& box = l.boxes.data[b];
    MermaidPaint paint{
        .flags =
            MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_Color | MermaidPaintFlag_StrokeWidth,
        .fill = mermaidInk(MermaidRole_NodeFill),
        .stroke = mermaidInk(MermaidRole_NodeStroke),
        .color = mermaidInk(MermaidRole_NodeText),
        .strokeWidth = kStrokeWidth,
    };
    mermaidPaintMerge(paint, block.style);
    PrPath* path = prPathCreate();
    prPathAddRect(path, r);
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);

    // The kind and the name, centered, then the fields at the left below the divider.
    float center = (r.left + r.right) * 0.5f;
    float top = r.top + kPaddingTop;
    const MermaidLabelIndex heading[] = {box.kind, box.name};
    for (MermaidLabelIndex label : heading) {
        PrVec2 extent = extentOf(l, label);
        if (label != kMermaidNone)
            mermaidLabelDraw(render, label, PrVec2{center - extent.x * 0.5f, top}, paint.color);
        top += extent.y;
    }
    if (box.fieldCount > 0) {
        top += kFieldGap;
        float divider = top - kDividerGap;
        mermaidDrawLine(render, PrVec2{r.left, divider}, PrVec2{r.right, divider}, paint.stroke, paint.strokeWidth, nullptr);
    }
    for (uint32_t f = 0; f < box.fieldCount; ++f) {
        mermaidLabelDraw(render, box.fields[f], PrVec2{r.left + kPaddingX, top}, paint.color);
        top += extentOf(l, box.fields[f]).y;
    }
    mermaidItemAdd(render, PrMermaidItemKind_Node, r, textOf(l, block.name), PrStringView{});
}

} // namespace

PrStatus mermaidRequirementDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidRequirementDiagram diagram{};
    PrStatus status = mermaidRequirementParse(diagram, source, error);
    if (status != PrStatus_Ok) {
        mermaidRequirementReset(diagram);
        return status;
    }
    Layout l{.render = &render, .diagram = &diagram};
    measure(l);
    PrGraph* graph = prGraphCreate();
    describe(l, graph);
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    options.direction = diagram.direction;
    options.routing = PrGraphRouting_Spline;
    options.layerSpacing = kLayerSpacing;
    options.nodeSpacing = kNodeSpacing;
    prGraphLayoutCompute(graph, &options);
    PrVec2 offset = mermaidGraphPlace(render, graph, textOf(l, diagram.title), mermaidTextStyle(kFontSize), kTitleGap,
        mermaidInk(MermaidRole_Text));

    // The relations, then their labels on their backgrounds, then the blocks.
    RawArray<PrVec2> points{};
    for (uint32_t r = 0; r < diagram.links.count; ++r)
        drawLink(l, graph, r, offset, points);
    points.reset();
    MermaidInk background = mermaidInk(MermaidRole_EdgeLabelBackground);
    for (uint32_t r = 0; r < diagram.links.count; ++r) {
        mermaidGraphLabelDraw(render, graph, l.graphLabels.data[r], offset, l.edgeLabels.data[r],
            mermaidInk(MermaidRole_NodeText), &background);
    }
    for (uint32_t b = 0; b < diagram.blocks.count; ++b)
        drawBlock(l, b, prRectOffset(prGraphNodeRect(graph, l.graphNodes.data[b]), offset));

    prGraphDestroy(graph);
    resetLayout(l);
    mermaidRequirementReset(diagram);
    return PrStatus_Ok;
}

} // namespace pr_impl
