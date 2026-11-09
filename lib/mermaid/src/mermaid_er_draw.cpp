// ER diagrams as drawn (mermaid_er_draw.h). An entity with attributes is a table: a header with its name, then a row
// for each attribute, in columns for the type, the name, the keys and the comment. An entity without attributes is a
// box around its name. lib/graph lays out the entities and routes the relationships, whose ends carry crow's foot
// marks.

#include "mermaid/src/mermaid_er_draw.h"

#include "pr/float.h"
#include "pr/geom.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_er.h"

namespace pr_impl {

namespace {

// Mermaid's ER diagram measurements, in px.
constexpr float kFontSize = 16;
constexpr float kLabelFontSize = 14;        // a relationship's label
constexpr float kLineHeight = 1.5f;         // a label's line height, by its font size
constexpr float kRowPadding = 9.375f;       // above and below the text of a row
constexpr float kColumnPadding = 12.5f;     // at either side of a column's text
constexpr float kNamePadding = 15;          // at either side of the name of an entity with attributes
constexpr float kBoxPaddingX = 20;          // at either side of the name of an entity without attributes
constexpr float kBoxPaddingY = 30;          // above and below it
constexpr float kWrapWidth = 120;           // the width at which the name of an entity without attributes wraps
constexpr float kTitleFontSize = 18;
constexpr float kTitleGap = 10;
constexpr float kDashed[2] = {3, 3};        // a non-identifying relationship
constexpr float kMarkLength = 40;           // the length of a route's end that stays straight for its mark

// The space between layers, which holds the marks at both ends of a relationship and its label between them.
constexpr float kLayerSpacing = 80;

// The columns of an entity's attribute rows.
enum {
    Column_Type,
    Column_Name,
    Column_Keys,
    Column_Comment,
    Column_Count,
};
typedef uint8_t Column;

// An entity as sized: its extent, and its columns' widths, zero for a column that no attribute fills.
struct EntityBox {
    PrVec2 extent;
    float rowHeight;
    float columns[Column_Count];
};

// An attribute's labels, one for each column, or kMermaidNone for an empty one.
struct AttributeLabels {
    MermaidLabelIndex columns[Column_Count];
};

struct Layout {
    MermaidRender* render;
    const MermaidErDiagram* diagram;
    RawArray<MermaidLabelIndex> names;          // by entity
    RawArray<EntityBox> boxes;                  // by entity
    RawArray<AttributeLabels> attributes;       // by attribute
    RawArray<uint32_t> rows;                    // the attributes by entity, each entity's in source order
    RawArray<uint32_t> rowStart;                // by entity: its first row, and at the next entity the end of its rows
    RawArray<PrGraphNode> graphNodes;           // by entity
    RawArray<PrGraphEdge> graphEdges;           // by relation
    RawArray<MermaidLabelIndex> edgeLabels;     // by relation, or kMermaidNone
    RawArray<PrGraphLabel> graphLabels;         // by relation, or kMermaidNone
};

void resetLayout(Layout& l) {
    l.names.reset();
    l.boxes.reset();
    l.attributes.reset();
    l.rows.reset();
    l.rowStart.reset();
    l.graphNodes.reset();
    l.graphEdges.reset();
    l.edgeLabels.reset();
    l.graphLabels.reset();
}

PrStringView textOf(const Layout& l, MermaidSpan span) {
    return mermaidErText(*l.diagram, span);
}

MermaidTextStyle styleOf(float size) {
    MermaidTextStyle style = mermaidTextStyle(size);
    style.lineHeight = size * kLineHeight;
    return style;
}

MermaidLabelIndex label(Layout& l, PrStringView text, float size) {
    if (text.size == 0)
        return kMermaidNone;
    return mermaidLabelCreate(*l.render, text, MermaidTextFormat_Plain, styleOf(size));
}

PrVec2 extentOf(const Layout& l, MermaidLabelIndex index) {
    return mermaidLabelExtent(*l.render, index);
}

// Shapes every label and sizes every entity. A column is as wide as its widest text and its padding, and the rows are
// as tall as the tallest text. An entity's name that is wider than its columns widens its last column.
void measure(Layout& l) {
    const MermaidErDiagram& d = *l.diagram;
    l.names.resize(d.entities.count);
    l.boxes.assignZeroed(d.entities.count);
    l.attributes.resize(d.attributes.count);
    for (uint32_t a = 0; a < d.attributes.count; ++a) {
        const MermaidAttribute& attribute = d.attributes.data[a];
        AttributeLabels& labels = l.attributes.data[a];
        labels.columns[Column_Type] = label(l, textOf(l, attribute.type), kFontSize);
        labels.columns[Column_Name] = label(l, textOf(l, attribute.name), kFontSize);
        labels.columns[Column_Keys] = label(l, textOf(l, attribute.keys), kFontSize);
        labels.columns[Column_Comment] = label(l, textOf(l, attribute.comment), kFontSize);
        EntityBox& box = l.boxes.data[attribute.entity];
        for (uint32_t c = 0; c < Column_Count; ++c) {
            PrVec2 extent = extentOf(l, labels.columns[c]);
            if (labels.columns[c] != kMermaidNone)
                box.columns[c] = prF32Max(box.columns[c], extent.x + 2 * kColumnPadding);
            box.rowHeight = prF32Max(box.rowHeight, extent.y + 2 * kRowPadding);
        }
    }
    // The rows go by entity, each entity's in source order.
    l.rowStart.assignZeroed(d.entities.count + 1);
    for (const MermaidAttribute& attribute : d.attributes.span())
        ++l.rowStart.data[attribute.entity + 1];
    for (uint32_t e = 0; e < d.entities.count; ++e)
        l.rowStart.data[e + 1] += l.rowStart.data[e];
    RawArray<uint32_t> next{};                  // by entity: where its next row goes
    next.append(l.rowStart.data, d.entities.count);
    l.rows.resize(d.attributes.count);
    for (uint32_t a = 0; a < d.attributes.count; ++a)
        l.rows.data[next.data[d.attributes.data[a].entity]++] = a;
    next.reset();
    for (uint32_t e = 0; e < d.entities.count; ++e) {
        EntityBox& box = l.boxes.data[e];
        uint32_t rows = l.rowStart.data[e + 1] - l.rowStart.data[e];
        if (rows == 0) {
            MermaidTextStyle style = styleOf(kFontSize);
            style.wrapWidth = kWrapWidth;
            l.names.data[e] =
                mermaidLabelCreate(*l.render, textOf(l, d.entities.data[e].label), MermaidTextFormat_Plain, style);
            PrVec2 text = extentOf(l, l.names.data[e]);
            box.extent = PrVec2{text.x + 2 * kBoxPaddingX, text.y + 2 * kBoxPaddingY};
            continue;
        }
        l.names.data[e] = label(l, textOf(l, d.entities.data[e].label), kFontSize);
        PrVec2 name = extentOf(l, l.names.data[e]);
        float width = 0;
        uint32_t last = 0;
        for (uint32_t c = 0; c < Column_Count; ++c) {
            width += box.columns[c];
            if (box.columns[c] > 0)
                last = c;
        }
        float least = name.x + 2 * kNamePadding;
        if (width < least) {
            box.columns[last] += least - width;
            width = least;
        }
        box.rowHeight = prF32Max(box.rowHeight, name.y + 2 * kRowPadding);
        box.extent = PrVec2{width, box.rowHeight * float(rows + 1)};
    }
}

void describe(Layout& l, PrGraph* graph) {
    const MermaidErDiagram& d = *l.diagram;
    l.graphNodes.resize(d.entities.count);
    for (uint32_t e = 0; e < d.entities.count; ++e) {
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.extent = l.boxes.data[e].extent;
        l.graphNodes.data[e] = prGraphAddNode(graph, &desc);
    }
    uint32_t count = d.relations.count;
    l.graphEdges.resize(count);
    l.edgeLabels.resize(count);
    l.graphLabels.resize(count);
    for (uint32_t r = 0; r < count; ++r) {
        const MermaidErRelation& relation = d.relations.data[r];
        l.graphEdges.data[r] = mermaidGraphAddEdge(graph, l.graphNodes.data[relation.source],
            l.graphNodes.data[relation.target], MermaidGraphPortFlag_Both);
        l.edgeLabels.data[r] = label(l, textOf(l, relation.label), kLabelFontSize);
        l.graphLabels.data[r] = kMermaidNone;
        if (l.edgeLabels.data[r] != kMermaidNone) {
            PrGraphLabelDesc labelDesc{
                .edge = l.graphEdges.data[r],
                .extent = extentOf(l, l.edgeLabels.data[r]),
                .placement = PrGraphLabelPlacement_Center,
            };
            l.graphLabels.data[r] = prGraphAddLabel(graph, &labelDesc);
        }
    }
}

// Records the mark at an end of a relationship, whose line meets the entity at `tip`, pointing along `direction` into
// the entity. The marks are Mermaid's: bars 18 across, circles of radius 6, and a crow's foot that spreads to 18 across
// where the line meets the entity. Each crow's foot is a closed lens that straddles the entity's outline, and the
// entity, drawn later, covers its inner half.
void drawEnd(MermaidRender& render, MermaidCardinality cardinality, PrVec2 tip, PrVec2 direction) {
    MermaidInk line = mermaidInk(MermaidRole_Line);
    PrVec2 across{-direction.y, direction.x};
    auto at = [&](float back, float side) {
        return tip - direction * back + across * side;
    };
    PrPath* path = prPathCreate();
    auto bar = [&](float back) {
        prPathMoveTo(path, at(back, -9));
        prPathLineTo(path, at(back, 9));
    };
    float circle = 0;
    switch (cardinality) {
        case MermaidCardinality_ExactlyOne:
            bar(9);
            bar(15);
            break;
        case MermaidCardinality_ZeroOrOne:
            bar(9);
            circle = 21;
            break;
        case MermaidCardinality_OneOrMore:
        case MermaidCardinality_ZeroOrMore:
            prPathMoveTo(path, at(18, 0));
            prPathQuadTo(path, at(0, -18), at(-18, 0));
            prPathQuadTo(path, at(0, 18), at(18, 0));
            if (cardinality == MermaidCardinality_OneOrMore)
                bar(24);
            else
                circle = 30;
            break;
        default:
            break;
    }
    mermaidStrokePath(render, path, line, 1, nullptr);
    if (circle > 0) {
        prPathClear(path);
        prPathAddEllipse(path, at(circle, 0), PrVec2{6, 6}, 0);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .fill = mermaidInk(MermaidRole_Background),
            .stroke = line,
            .strokeWidth = 1,
        };
        mermaidDrawPath(render, path, paint);
    }
    prPathDestroy(path);
}

void drawRelation(Layout& l, PrGraph* graph, uint32_t r, PrVec2 offset, RawArray<PrVec2>& points) {
    MermaidRender& render = *l.render;
    const MermaidErRelation& relation = l.diagram->relations.data[r];
    points.clear();
    if (!mermaidGraphRouteAppend(graph, l.graphEdges.data[r], offset, points))
        return;
    mermaidItemAddEdge(render, points.data, points.count, PrStringView{});
    // The marks lie along the route's tangent at each end, so each end's control point moves out along the tangent
    // until the curve stays near it for the length of the marks.
    if (points.count >= 4) {
        for (int side = 0; side < 2; ++side) {
            bool atStart = side == 0;
            uint32_t end = atStart ? 0 : points.count - 1;
            uint32_t control = atStart ? 1 : points.count - 2;
            PrVec2 direction = mermaidRouteEndDirection(points.data, points.count, atStart);
            PrVec2 toward = points.data[end] - points.data[control];
            float along = toward.x * direction.x + toward.y * direction.y;
            if (along < kMarkLength)
                points.data[control] = points.data[end] - direction * kMarkLength;
        }
    }
    PrPath* path = prPathCreate();
    mermaidRouteAppend(path, points.data, points.count);
    mermaidStrokePath(render, path, mermaidInk(MermaidRole_Line), 1, relation.identifying ? nullptr : kDashed);
    prPathDestroy(path);
    drawEnd(render, relation.sourceCardinality, points.data[0], mermaidRouteEndDirection(points.data, points.count, true));
    drawEnd(render, relation.targetCardinality, points.data[points.count - 1],
        mermaidRouteEndDirection(points.data, points.count, false));
}

void drawEntity(Layout& l, uint32_t e, PrRect r) {
    MermaidRender& render = *l.render;
    const MermaidErDiagram& d = *l.diagram;
    const EntityBox& box = l.boxes.data[e];
    MermaidPaint paint{
        .flags =
            MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_Color | MermaidPaintFlag_StrokeWidth,
        .fill = mermaidInk(MermaidRole_NodeFill),
        .stroke = mermaidInk(MermaidRole_NodeStroke),
        .color = mermaidInk(MermaidRole_NodeText),
        .strokeWidth = 1,
    };
    mermaidPaintMerge(paint, d.entities.data[e].style);
    PrPath* path = prPathCreate();
    prPathAddRect(path, r);
    MermaidPaint fill = paint;
    fill.flags &= MermaidPaintFlags(~MermaidPaintFlag_Stroke);
    mermaidDrawPath(render, path, fill);
    if (box.rowHeight == 0) {
        mermaidLabelDrawCentered(render, l.names.data[e], prRectCenter(r), paint.color);
    } else {
        // The header, then the rows in alternating fills with lines between them, then the columns' dividers.
        PrVec2 name = extentOf(l, l.names.data[e]);
        mermaidLabelDraw(render, l.names.data[e], PrVec2{(r.left + r.right - name.x) * 0.5f, r.top + kRowPadding},
            paint.color);
        float top = r.top + box.rowHeight;
        for (uint32_t row = 0; row < l.rowStart.data[e + 1] - l.rowStart.data[e]; ++row) {
            uint32_t a = l.rows.data[l.rowStart.data[e] + row];
            PrRect rowRect{r.left, top, r.right, top + box.rowHeight};
            mermaidFillRect(render, rowRect, mermaidInk(row % 2 == 0 ? MermaidRole_RowOdd : MermaidRole_RowEven));
            mermaidDrawLine(render, PrVec2{r.left, top}, PrVec2{r.right, top}, paint.stroke, paint.strokeWidth, nullptr);
            float x = r.left;
            for (uint32_t c = 0; c < Column_Count; ++c) {
                MermaidLabelIndex text = l.attributes.data[a].columns[c];
                if (text != kMermaidNone)
                    mermaidLabelDraw(render, text, PrVec2{x + kColumnPadding, top + kRowPadding}, paint.color);
                x += box.columns[c];
            }
            top += box.rowHeight;
        }
        float x = r.left;
        for (uint32_t c = 0; c + 1 < Column_Count; ++c) {
            x += box.columns[c];
            if (box.columns[c] > 0 && x < r.right - 0.5f) {
                mermaidDrawLine(render, PrVec2{x, r.top + box.rowHeight}, PrVec2{x, r.bottom}, paint.stroke,
                    paint.strokeWidth, nullptr);
            }
        }
    }
    MermaidPaint stroke = paint;
    stroke.flags &= MermaidPaintFlags(~MermaidPaintFlag_Fill);
    mermaidDrawPath(render, path, stroke);
    prPathDestroy(path);
    mermaidItemAdd(render, PrMermaidItemKind_Node, r, textOf(l, d.entities.data[e].id), PrStringView{});
}

} // namespace

PrStatus mermaidErDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidErDiagram diagram{};
    PrStatus status = mermaidErParse(diagram, source, error);
    if (status != PrStatus_Ok) {
        mermaidErReset(diagram);
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
    prGraphLayoutCompute(graph, &options);
    PrVec2 offset = mermaidGraphPlace(render, graph, textOf(l, diagram.title), mermaidTextStyle(kTitleFontSize),
        kTitleGap, mermaidInk(MermaidRole_Title));

    // The relationships and their labels, then the entities, which cover the inner halves of the marks.
    RawArray<PrVec2> points{};
    for (uint32_t r = 0; r < diagram.relations.count; ++r)
        drawRelation(l, graph, r, offset, points);
    points.reset();
    for (uint32_t r = 0; r < diagram.relations.count; ++r) {
        if (l.graphLabels.data[r] != kMermaidNone) {
            mermaidGraphLabelDraw(render, graph, l.graphLabels.data[r], offset, l.edgeLabels.data[r],
                mermaidInk(MermaidRole_Text), nullptr);
        }
    }
    for (uint32_t e = 0; e < diagram.entities.count; ++e)
        drawEntity(l, e, prRectOffset(prGraphNodeRect(graph, l.graphNodes.data[e]), offset));

    prGraphDestroy(graph);
    resetLayout(l);
    mermaidErReset(diagram);
    return PrStatus_Ok;
}

} // namespace pr_impl
