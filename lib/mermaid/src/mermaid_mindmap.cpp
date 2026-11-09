// Mindmaps (mermaid_mindmap.h). The parser reads one node from each line, and a node's indentation makes it a child of
// the nearest node before it that is indented less. The drawing lays the tree out down the page with lib/graph, as
// mermaid.js's classic look lays it out with dagre. Each of the root's children starts a section in its own color, and
// the lines thin with depth.

#include "mermaid/src/mermaid_mindmap.h"

#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/graph.h"
#include "pr/path.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's mindmap measurements, in px.
constexpr float kFontSize = 16;
constexpr float kLineHeight = 1.5f;
constexpr float kWrapWidth = 200;           // maxNodeWidth
constexpr float kRadius = 5;
constexpr float kUnderline = 3;
constexpr float kDiagramPadding = 8;
constexpr float kTitleFontSize = 18;

// The colors of the root, then of each section in turn: its fill, its underline, and whether its text is white on the
// light theme's fill. The dark theme's fills are dark, and its text is white.
struct SectionColors {
    PrColor8 fill;
    PrColor8 darkFill;
    PrColor8 line;
    PrColor8 darkLine;
    bool whiteText;
};

constexpr SectionColors kSectionColors[12] = {
    {{0x00, 0x00, 0xec, 0xff}, {0x3d, 0x3d, 0xff, 0xff}, {0xff, 0xff, 0xb9, 0xff}, {0xff, 0xff, 0x33, 0xff}, true},
    {{0xff, 0xff, 0x78, 0xff}, {0xa3, 0xa3, 0x00, 0xff}, {0xb9, 0xb9, 0xff, 0xff}, {0x33, 0x33, 0xff, 0xff}, false},
    {{0xd7, 0xff, 0x86, 0xff}, {0x6d, 0xa3, 0x00, 0xff}, {0xd0, 0xb9, 0xff, 0xff}, {0x77, 0x33, 0xff, 0xff}, false},
    {{0xc2, 0x86, 0xff, 0xff}, {0x52, 0x00, 0xa3, 0xff}, {0xdc, 0xff, 0xb9, 0xff}, {0x99, 0xff, 0x33, 0xff}, true},
    {{0xff, 0x86, 0xff, 0xff}, {0xa3, 0x00, 0xa3, 0xff}, {0xb9, 0xff, 0xb9, 0xff}, {0x33, 0xff, 0x33, 0xff}, false},
    {{0xff, 0x86, 0xc2, 0xff}, {0xa3, 0x00, 0x52, 0xff}, {0xb9, 0xff, 0xdc, 0xff}, {0x33, 0xff, 0x99, 0xff}, false},
    {{0xff, 0x86, 0x86, 0xff}, {0xa3, 0x00, 0x00, 0xff}, {0xb9, 0xff, 0xff, 0xff}, {0x33, 0xff, 0xff, 0xff}, false},
    {{0xff, 0xc2, 0x86, 0xff}, {0xa3, 0x52, 0x00, 0xff}, {0xb9, 0xdc, 0xff, 0xff}, {0x33, 0x99, 0xff, 0xff}, false},
    {{0xc2, 0xff, 0x86, 0xff}, {0x52, 0xa3, 0x00, 0xff}, {0xdc, 0xb9, 0xff, 0xff}, {0x99, 0x33, 0xff, 0xff}, false},
    {{0x86, 0xff, 0xc2, 0xff}, {0x00, 0xa3, 0x52, 0xff}, {0xff, 0xb9, 0xdc, 0xff}, {0xff, 0x33, 0x99, 0xff}, false},
    {{0x86, 0xff, 0xff, 0xff}, {0x00, 0xa3, 0xa3, 0xff}, {0xff, 0xb9, 0xb9, 0xff}, {0xff, 0x33, 0x33, 0xff}, false},
    {{0x86, 0xc2, 0xff, 0xff}, {0x00, 0x52, 0xa3, 0xff}, {0xff, 0xdc, 0xb9, 0xff}, {0xff, 0x99, 0x33, 0xff}, false},
};

const SectionColors& colorsOf(const MermaidMindmapNode& node) {
    return node.parent == kMermaidNone ? kSectionColors[0] : kSectionColors[1 + node.section % 11];
}

// The brackets of each shape, longest first, so that the first match is the right shape.
struct ShapeSyntax {
    PrStringView open;
    PrStringView close;
    MermaidMindmapShape shape;
};

constexpr ShapeSyntax kShapes[] = {
    {"((", "))", MermaidMindmapShape_Circle}, {"))", "((", MermaidMindmapShape_Bang},
    {"{{", "}}", MermaidMindmapShape_Hexagon}, {"(", ")", MermaidMindmapShape_Rounded},
    {")", "(", MermaidMindmapShape_Cloud}, {"[", "]", MermaidMindmapShape_Square},
};

// Reads a node: text alone, or an ID and text in a shape's brackets. Text may be quoted, and a Markdown string is in
// backticks inside the quotes.
void readNode(MermaidMindmap& mindmap, PrStringView text, MermaidMindmapNode* node) {
    node->shape = MermaidMindmapShape_Default;
    node->format = MermaidTextFormat_Html;
    // A ::: class at the end of the line is not drawn.
    size_t classAt = prStringFind(text, ":::");
    if (classAt < text.size)
        text = prStringTrim(PrStringView{text.data, classAt});
    PrStringView label = text;
    for (size_t at = 0; at < text.size && label.data == text.data; ++at) {
        for (const ShapeSyntax& syntax : kShapes) {
            if (prStringStartsWith(mermaidAfter(text, at), syntax.open) && prStringEndsWith(text, syntax.close) &&
                    text.size >= at + syntax.open.size + syntax.close.size) {
                node->shape = syntax.shape;
                label = PrStringView{
                    text.data + at + syntax.open.size,
                    text.size - at - syntax.open.size - syntax.close.size,
                };
                break;
            }
        }
        if (text.data[at] == '"' || prAsciiIsSpace(text.data[at]))
            break;
    }
    label = prStringTrim(label);
    if (label.size >= 2 && label.data[0] == '"' && label.data[label.size - 1] == '"') {
        label = PrStringView{label.data + 1, label.size - 2};
        if (label.size >= 2 && label.data[0] == '`' && label.data[label.size - 1] == '`') {
            label = PrStringView{label.data + 1, label.size - 2};
            node->format = MermaidTextFormat_Markdown;
        }
    }
    node->label = mermaidTextAdd(mindmap.text, label);
}

// Returns the extent of a node's shape around text of extent `text`, as mermaid.js pads each shape.
PrVec2 shapeExtent(MermaidMindmapShape shape, PrVec2 text) {
    switch (shape) {
        case MermaidMindmapShape_Square:
            return PrVec2{text.x + 40, text.y + 20};
        case MermaidMindmapShape_Rounded:
            return PrVec2{text.x + 30, text.y + 30};
        case MermaidMindmapShape_Circle: {
            float diameter = text.x + 24;
            return PrVec2{diameter, diameter};
        }
        case MermaidMindmapShape_Hexagon:
            return PrVec2{text.x + 42, text.y + 20};
        case MermaidMindmapShape_Bang:
            return PrVec2{text.x + 40, text.y + 40};
        case MermaidMindmapShape_Cloud:
            return PrVec2{text.x + 30, text.y + 30};
        default:
            return PrVec2{text.x + 40, text.y + 10};
    }
}

// Builds a node's outline within `r`.
void buildShape(MermaidMindmapShape shape, PrRect r, PrPath* path) {
    PrVec2 c{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f};
    PrVec2 half{(r.right - r.left) * 0.5f, (r.bottom - r.top) * 0.5f};
    switch (shape) {
        case MermaidMindmapShape_Square:
            prPathAddRect(path, r);
            return;
        case MermaidMindmapShape_Circle:
            prPathAddEllipse(path, c, half, 0);
            return;
        case MermaidMindmapShape_Hexagon: {
            float m = 11;
            prPathMoveTo(path, PrVec2{r.left + m, r.top});
            prPathLineTo(path, PrVec2{r.right - m, r.top});
            prPathLineTo(path, PrVec2{r.right, c.y});
            prPathLineTo(path, PrVec2{r.right - m, r.bottom});
            prPathLineTo(path, PrVec2{r.left + m, r.bottom});
            prPathLineTo(path, PrVec2{r.left, c.y});
            prPathClose(path);
            return;
        }
        case MermaidMindmapShape_Bang: {
            // A burst: points around the ellipse, alternately on it and inside it.
            constexpr uint32_t kPoints = 24;
            for (uint32_t i = 0; i < kPoints; ++i) {
                float angle = float(i) * 2 * kPrF32Pi / float(kPoints);
                float scale = i % 2 == 0 ? 1.0f : 0.82f;
                PrVec2 p{c.x + prF32Cos(angle) * half.x * scale, c.y + prF32Sin(angle) * half.y * scale};
                if (i == 0)
                    prPathMoveTo(path, p);
                else
                    prPathLineTo(path, p);
            }
            prPathClose(path);
            return;
        }
        case MermaidMindmapShape_Cloud: {
            // Bumps around the ellipse, each a circle centered on it.
            constexpr uint32_t kBumps = 10;
            float bump = prF32Min(half.x, half.y) * 0.45f;
            for (uint32_t i = 0; i < kBumps; ++i) {
                float angle = float(i) * 2 * kPrF32Pi / float(kBumps);
                PrVec2 p{c.x + prF32Cos(angle) * (half.x - bump), c.y + prF32Sin(angle) * (half.y - bump)};
                prPathAddEllipse(path, p, PrVec2{bump, bump}, 0);
            }
            prPathAddEllipse(path, c, PrVec2{half.x - bump, half.y - bump}, 0);
            return;
        }
        default: {
            PrCornerRect rounded{.rect = r};
            for (PrCorner& corner : rounded.corners)
                corner = PrCorner{.radii = PrVec2{kRadius, kRadius}, .shape = 1};
            prPathAddCornerRect(path, rounded);
            return;
        }
    }
}

// The width of the line into a node at `depth`, which thins as the tree deepens.
float lineWidth(uint32_t depth) {
    if (depth <= 1)
        return 11;
    if (depth == 2)
        return 5;
    return 2;
}

} // namespace

PrStatus mermaidMindmapParse(MermaidMindmap& mindmap, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    mindmap.title = MermaidSpan{};
    mindmap.text.clear();
    mindmap.nodes.clear();
    MermaidParseState parser{.error = error};
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_Mindmap, nullptr);
    mindmap.title = mermaidTextAdd(mindmap.text, preamble.title);
    // A node's indentation is the column of its line's content.
    RawArray<uint32_t> indents{};           // by node
    uint32_t sections = 0;                  // the root's children so far
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        PrStringView content = line.content;
        // An icon or a class for the node before applies to nothing that the engine draws.
        if (prStringStartsWith(content, "::icon(") || prStringStartsWith(content, ":::"))
            continue;
        MermaidMindmapNode node{.parent = kMermaidNone};
        if (mindmap.nodes.count > 0) {
            // The walk up from the last node passes only nodes that no later line can be a child of, so the walks of
            // all the lines together pass each node at most once.
            uint32_t parent = mindmap.nodes.count - 1;
            while (parent != kMermaidNone && indents.data[parent] >= line.column)
                parent = mindmap.nodes.data[parent].parent;
            if (parent == kMermaidNone) {
                ok = mermaidParseFail(parser, "a mindmap has only one root");
                break;
            }
            const MermaidMindmapNode& above = mindmap.nodes.data[parent];
            node.parent = parent;
            node.depth = above.depth + 1;
            node.section = above.parent == kMermaidNone ? sections++ : above.section;
        }
        readNode(mindmap, content, &node);
        mindmap.nodes.push(node);
        indents.push(line.column);
    }
    indents.reset();
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidMindmapText(const MermaidMindmap& mindmap, MermaidSpan span) {
    return mermaidTextAt(mindmap.text, span);
}

void mermaidMindmapReset(MermaidMindmap& mindmap) {
    mindmap.text.reset();
    mindmap.nodes.reset();
}

PrStatus mermaidMindmapDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidMindmap mindmap{};
    PrStatus status = mermaidMindmapParse(mindmap, source, error);
    if (status != PrStatus_Ok) {
        mermaidMindmapReset(mindmap);
        return status;
    }
    uint32_t count = mindmap.nodes.count;
    RawArray<MermaidLabelIndex> labels{};
    RawArray<PrGraphNode> graphNodes{};
    RawArray<PrGraphEdge> graphEdges{};     // by node: the edge from its parent, or kMermaidNone
    PrGraph* graph = prGraphCreate();
    for (const MermaidMindmapNode& node : mindmap.nodes.span()) {
        MermaidTextStyle style = mermaidTextStyle(kFontSize);
        style.lineHeight = kFontSize * kLineHeight;
        style.wrapWidth = kWrapWidth;
        MermaidLabelIndex label = mermaidLabelCreate(render, mermaidMindmapText(mindmap, node.label), node.format, style);
        labels.push(label);
        PrGraphNodeDesc desc = prGraphNodeDescDefault();
        desc.extent = shapeExtent(node.shape, mermaidLabelExtent(render, label));
        graphNodes.push(prGraphAddNode(graph, &desc));
    }
    for (uint32_t n = 0; n < count; ++n) {
        uint32_t parent = mindmap.nodes.data[n].parent;
        if (parent == kMermaidNone) {
            graphEdges.push(kMermaidNone);
            continue;
        }
        PrGraphEdgeDesc desc = prGraphEdgeDescDefault(graphNodes.data[parent], graphNodes.data[n]);
        graphEdges.push(prGraphAddEdge(graph, &desc));
    }
    PrGraphLayoutOptions options = prGraphLayoutOptionsDefault();
    options.direction = PrGraphDirection_Down;
    options.routing = PrGraphRouting_Spline;
    prGraphLayoutCompute(graph, &options);

    MermaidLabelIndex title = mermaidLabelCreate(render, mermaidMindmapText(mindmap, mindmap.title),
        MermaidTextFormat_Plain, mermaidTextStyle(kTitleFontSize));
    PrVec2 titleExtent = mermaidLabelExtent(render, title);
    float titleSpace = titleExtent.y > 0 ? titleExtent.y + 10 : 0;
    PrRect bounds = prGraphBounds(graph);
    float width = prF32Max(bounds.right, titleExtent.x);
    render.extent = PrVec2{width + 2 * kDiagramPadding, bounds.bottom + titleSpace + 2 * kDiagramPadding};
    PrVec2 offset{kDiagramPadding + (width - bounds.right) * 0.5f, kDiagramPadding + titleSpace};
    mermaidLabelDraw(render, title, PrVec2{(render.extent.x - titleExtent.x) * 0.5f, kDiagramPadding},
        mermaidInk(MermaidRole_Title));
    auto rectOf = [&](uint32_t n) {
        PrRect r = prGraphNodeRect(graph, graphNodes.data[n]);
        return PrRect{r.left + offset.x, r.top + offset.y, r.right + offset.x, r.bottom + offset.y};
    };

    // The lines from each node to its children, in the children's section colors, under the nodes.
    RawArray<PrVec2> points{};
    for (uint32_t n = 0; n < count; ++n) {
        if (graphEdges.data[n] == kMermaidNone)
            continue;
        const MermaidMindmapNode& node = mindmap.nodes.data[n];
        PrGraphRoute route = prGraphEdgeRoute(graph, graphEdges.data[n]);
        points.clear();
        for (uint32_t i = 0; i < route.pointCount; ++i)
            points.push(route.points[i] + offset);
        if (points.count < 2)
            continue;
        // A line from a circle starts at its outline.
        const MermaidMindmapNode& parent = mindmap.nodes.data[node.parent];
        if (parent.shape == MermaidMindmapShape_Circle) {
            PrRect r = rectOf(node.parent);
            PrVec2 center{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f};
            PrVec2 direction = mermaidRouteEndDirection(points.data, points.count, true);
            mermaidRouteMoveEnd(points.data, points.count, true,
                center - direction * ((r.right - r.left) * 0.5f) - points.data[0]);
        }
        const SectionColors& colors = colorsOf(node);
        PrPath* path = prPathCreate();
        mermaidRouteAppend(path, points.data, points.count);
        mermaidStrokePath(render, path, mermaidInkOfColors(colors.fill, colors.darkFill), lineWidth(node.depth), nullptr);
        prPathDestroy(path);
        mermaidItemAddEdge(render, points.data, points.count, PrStringView{});
    }
    points.reset();

    for (uint32_t n = 0; n < count; ++n) {
        const MermaidMindmapNode& node = mindmap.nodes.data[n];
        const SectionColors& colors = colorsOf(node);
        PrRect r = rectOf(n);
        PrPath* path = prPathCreate();
        buildShape(node.shape, r, path);
        mermaidFillPath(render, path, mermaidInkOfColors(colors.fill, colors.darkFill));
        prPathDestroy(path);
        if (node.shape == MermaidMindmapShape_Default) {
            mermaidDrawLine(render, PrVec2{r.left, r.bottom}, PrVec2{r.right, r.bottom},
                mermaidInkOfColors(colors.line, colors.darkLine), kUnderline, nullptr);
        }
        PrColor8 white{0xff, 0xff, 0xff, 0xff};
        PrColor8 text = colors.whiteText ? white : PrColor8{0x00, 0x00, 0x00, 0xff};
        PrVec2 center{(r.left + r.right) * 0.5f, (r.top + r.bottom) * 0.5f};
        mermaidLabelDrawCentered(render, labels.data[n], center, mermaidInkOfColors(text, white));
        mermaidItemAdd(render, PrMermaidItemKind_Node, r, mermaidMindmapText(mindmap, node.label), PrStringView{});
    }

    prGraphDestroy(graph);
    labels.reset();
    graphNodes.reset();
    graphEdges.reset();
    mermaidMindmapReset(mindmap);
    return PrStatus_Ok;
}

} // namespace pr_impl
