// Mermaid diagrams (mermaid.h). Creating a diagram finds its kind and theme, lays it out with the code of its kind, and
// keeps the scene that the layout recorded and the items. A draw records the scene in a scheme.

#include "pr/mermaid.h"

#include "pr/alloc.h"
#include "pr/ascii.h"
#include "pr/float.h"
#include "pr/path_data.h"
#include "pr/raw_array.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_class_draw.h"
#include "mermaid/src/mermaid_er_draw.h"
#include "mermaid/src/mermaid_flowchart_draw.h"
#include "mermaid/src/mermaid_gantt_draw.h"
#include "mermaid/src/mermaid_gitgraph.h"
#include "mermaid/src/mermaid_journey.h"
#include "mermaid/src/mermaid_mindmap.h"
#include "mermaid/src/mermaid_parse.h"
#include "mermaid/src/mermaid_pie.h"
#include "mermaid/src/mermaid_quadrant.h"
#include "mermaid/src/mermaid_render.h"
#include "mermaid/src/mermaid_requirement_draw.h"
#include "mermaid/src/mermaid_sequence_draw.h"
#include "mermaid/src/mermaid_state_draw.h"
#include "mermaid/src/mermaid_timeline.h"
#include "mermaid/src/mermaid_xychart.h"

namespace pr_impl {

// A diagram, as mermaid.h describes it.
struct Mermaid {
    MermaidScene scene;
    PrMermaidKind kind;
    PrVec2 extent;
    bool hasScheme;                 // the source names a theme, whose scheme is `scheme`
    PrColorScheme scheme;
    RawArray<MermaidItemRecord> items;
    RawArray<char> itemText;
    RawArray<PrVec2> itemPoints;
    RawArray<char> accessibleTitle;
    RawArray<char> accessibleDescription;
};

namespace {

// The names by kind, as prMermaidKindName gives them.
constexpr PrStringView kKindNamesByKind[PrMermaidKind_Count] = {
    "flowchart", "sequenceDiagram", "classDiagram", "stateDiagram", "erDiagram", "gantt", "pie", "gitGraph",
    "mindmap", "timeline", "journey", "quadrantChart", "xychart", "sankey", "radar-beta", "treemap", "block",
    "packet", "kanban", "requirementDiagram", "C4Context", "architecture", "venn-beta", "usecase-beta",
    "swimlane-beta", "agentflow-beta", "treeView-beta", "ishikawa", "wardley-beta", "cynefin-beta", "eventmodeling",
    "railroad-beta", "zenuml", "info", "error",
};

// Reads the accessible title and description: accTitle: text, accDescr: text and accDescr { lines }, falling back to
// the front matter's title.
void readAccessibility(PrStringView source, const MermaidPreamble& p, Mermaid& diagram) {
    MermaidLines lines = mermaidLinesOfBody(source, p);
    MermaidLine line{};
    bool inBlock = false;
    while (mermaidLinesNextRaw(lines, &line)) {
        PrStringView content = line.content;
        if (inBlock) {
            size_t close = prStringFindByte(content, '}');
            PrStringView part = mermaidBefore(content, close);
            if (diagram.accessibleDescription.count > 0 && part.size > 0)
                diagram.accessibleDescription.push('\n');
            diagram.accessibleDescription.append(part.data, uint32_t(part.size));
            inBlock = close >= content.size;
            continue;
        }
        size_t colon = prStringFindByte(content, ':');
        if (prStringStartsWith(content, "accTitle")) {
            if (colon < content.size) {
                PrStringView text = prStringTrim(mermaidAfter(content, colon + 1));
                diagram.accessibleTitle.clear();
                diagram.accessibleTitle.append(text.data, uint32_t(text.size));
            }
        } else if (prStringStartsWith(content, "accDescr")) {
            size_t brace = prStringFindByte(content, '{');
            if (brace < colon) {
                PrStringView rest = prStringTrim(mermaidAfter(content, brace + 1));
                size_t close = prStringFindByte(rest, '}');
                PrStringView part = mermaidBefore(rest, close);
                diagram.accessibleDescription.append(part.data, uint32_t(part.size));
                inBlock = close >= rest.size;
            } else if (colon < content.size) {
                PrStringView text = prStringTrim(mermaidAfter(content, colon + 1));
                diagram.accessibleDescription.append(text.data, uint32_t(text.size));
            }
        }
    }
    if (diagram.accessibleTitle.count == 0)
        mermaidTextDecode(p.title, MermaidTextFormat_Plain, diagram.accessibleTitle);
}

// Moves what a render recorded into `diagram`, and frees the rest of the render.
void finish(MermaidRender& render, const MermaidPreamble& p, Mermaid& diagram) {
    diagram.scene = render.scene;
    diagram.kind = render.kind;
    diagram.extent = render.extent;
    diagram.hasScheme = p.hasScheme;
    diagram.scheme = p.scheme;
    diagram.items = render.items;
    diagram.itemText = render.itemText;
    diagram.itemPoints = render.itemPoints;
    render.scene = MermaidScene{};
    render.items = RawArray<MermaidItemRecord>{};
    render.itemText = RawArray<char>{};
    render.itemPoints = RawArray<PrVec2>{};
    mermaidRenderReset(render);
}

// The scheme that a diagram draws in when `scheme` is asked for: the scheme of its source's theme, if it names one.
PrColorScheme schemeOf(const Mermaid& diagram, PrColorScheme scheme) {
    return diagram.hasScheme ? diagram.scheme : scheme;
}

// The error diagram's measurements: mermaid.js's, at a quarter of its size.
constexpr float kErrorScale = 0.25f;

// The paths of mermaid.js's error icon, a bomb, in its units.
constexpr PrStringView kErrorIcon[] = {
    "m411.313,123.313c6.25-6.25 6.25-16.375 0-22.625s-16.375-6.25-22.625,0l-32,32-9.375,9.375-20.688-20.688"
        "c-12.484-12.5-32.766-12.5-45.25,0l-16,16c-1.261,1.261-2.304,2.648-3.31,4.051-21.739-8.561-45.324-13.426-70.065"
        "-13.426-105.867,0-192,86.133-192,192s86.133,192 192,192 192-86.133 192-192c0-24.741-4.864-48.327-13.426-70.065 "
        "1.402-1.007 2.79-2.049 4.051-3.31l16-16c12.5-12.492 12.5-32.758 0-45.25l-20.688-20.688 9.375-9.375 "
        "32.001-31.999zm-219.313,100.687c-52.938,0-96,43.063-96,96 0,8.836-7.164,16-16,16s-16-7.164-16-16c0-70.578 "
        "57.422-128 128-128 8.836,0 16,7.164 16,16s-7.164,16-16,16z",
    "m459.02,148.98c-6.25-6.25-16.375-6.25-22.625,0s-6.25,16.375 0,22.625l16,16c3.125,3.125 7.219,4.688 11.313,4.688 "
        "4.094,0 8.188-1.563 11.313-4.688 6.25-6.25 6.25-16.375 0-22.625l-16.001-16z",
    "m340.395,75.605c3.125,3.125 7.219,4.688 11.313,4.688 4.094,0 8.188-1.563 11.313-4.688 6.25-6.25 6.25-16.375 "
        "0-22.625l-16-16c-6.25-6.25-16.375-6.25-22.625,0s-6.25,16.375 0,22.625l15.999,16z",
    "m400,64c8.844,0 16-7.164 16-16v-32c0-8.836-7.156-16-16-16-8.844,0-16,7.164-16,16v32c0,8.836 7.156,16 16,16z",
    "m496,96.586h-32c-8.844,0-16,7.164-16,16 0,8.836 7.156,16 16,16h32c8.844,0 16-7.164 16-16 0-8.836-7.156-16-16-16z",
    "m436.98,75.605c3.125,3.125 7.219,4.688 11.313,4.688 4.094,0 8.188-1.563 11.313-4.688l32-32c6.25-6.25 6.25-16.375 "
        "0-22.625s-16.375-6.25-22.625,0l-32,32c-6.251,6.25-6.251,16.375-0.001,22.625z",
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrMermaid : Mermaid {};

bool prMermaidKindFromSource(PrStringView source, PrMermaidKind* out) {
    MermaidPreamble p = mermaidPreambleRead(source);
    return p.header.number != 0 && mermaidKindOfHeader(p.header.content, out, nullptr);
}

PrStringView prMermaidKindName(PrMermaidKind kind) {
    PR_ASSERT(kind < PrMermaidKind_Count);
    return kKindNamesByKind[kind];
}

PrMermaidOptions prMermaidOptionsDefault(void) {
    return PrMermaidOptions{};
}

PrStatus prMermaidCreate(const PrMermaidDesc* desc, const PrMermaidOptions* PR_NULLABLE options,
        PrMermaid* PR_NULLABLE* PR_NONNULL out, PrMermaidError* PR_NULLABLE error) {
    *out = nullptr;
    PrMermaidOptions resolved = options != nullptr ? *options : prMermaidOptionsDefault();
    MermaidPreamble p = mermaidPreambleRead(desc->source);
    PrMermaidKind kind = PrMermaidKind_Flowchart;
    if (p.header.number == 0 || !mermaidKindOfHeader(p.header.content, &kind, nullptr)) {
        if (error != nullptr && p.header.number != 0)
            *error = PrMermaidError{p.header.number, p.header.column + 1, "unknown kind of diagram"};
        else if (error != nullptr)
            *error = PrMermaidError{1, 1, "the source names no diagram"};
        return PrStatus_Syntax;
    }
    MermaidRender render{.kind = kind};
    mermaidRenderInit(render, *desc, resolved);
    PrStatus status;
    switch (kind) {
        case PrMermaidKind_Flowchart:
            status = mermaidFlowchartDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Sequence:
            status = mermaidSequenceDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Class:
            status = mermaidClassDraw(render, desc->source, error);
            break;
        case PrMermaidKind_State:
            status = mermaidStateDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Er:
            status = mermaidErDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Gantt:
            status = mermaidGanttDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Pie:
            status = mermaidPieDraw(render, desc->source, error);
            break;
        case PrMermaidKind_GitGraph:
            status = mermaidGitGraphDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Mindmap:
            status = mermaidMindmapDraw(render, desc->source, error);
            break;
        case PrMermaidKind_XyChart:
            status = mermaidXyChartDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Journey:
            status = mermaidJourneyDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Timeline:
            status = mermaidTimelineDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Quadrant:
            status = mermaidQuadrantDraw(render, desc->source, error);
            break;
        case PrMermaidKind_Requirement:
            status = mermaidRequirementDraw(render, desc->source, error);
            break;
        default:
            if (error != nullptr)
                *error = PrMermaidError{p.header.number, p.header.column + 1, "the engine does not draw this kind of diagram"};
            status = PrStatus_NotSupported;
            break;
    }
    if (status != PrStatus_Ok) {
        mermaidRenderReset(render);
        return status;
    }
    PrMermaid* diagram = allocZeroed<PrMermaid>();
    finish(render, p, *diagram);
    readAccessibility(desc->source, p, *diagram);
    *out = diagram;
    return PrStatus_Ok;
}

PrMermaid* prMermaidCreateError(const PrMermaidDesc* desc, const PrMermaidOptions* PR_NULLABLE options,
        const PrMermaidError* error) {
    PrMermaidOptions resolved = options != nullptr ? *options : prMermaidOptionsDefault();
    MermaidRender render{.kind = PrMermaidKind_Error};
    mermaidRenderInit(render, *desc, resolved);
    // mermaid.js's layout, in its units: the icon at the left, then the heading, then the details, which name where the
    // source failed in place of mermaid.js's version.
    RawArray<char> details{};
    details.append("line ", 5);
    PrRawString digits = prRawStringNull();
    prRawStringAppendU64(&digits, error->line, 10);
    details.append(prRawStringView(&digits).data, uint32_t(prRawStringView(&digits).size));
    prRawStringReset(&digits);
    details.append(", column ", 9);
    prRawStringAppendU64(&digits, error->column, 10);
    details.append(prRawStringView(&digits).data, uint32_t(prRawStringView(&digits).size));
    prRawStringReset(&digits);
    details.append(": ", 2);
    details.append(error->message.data, uint32_t(error->message.size));
    MermaidTextStyle heading = mermaidTextStyle(150 * kErrorScale);
    MermaidTextStyle detail = mermaidTextStyle(100 * kErrorScale);
    detail.wrapWidth = 1500 * kErrorScale;
    MermaidLabelIndex headingLabel = mermaidLabelCreate(render, "Syntax error in text", MermaidTextFormat_Plain, heading);
    MermaidLabelIndex detailLabel =
        mermaidLabelCreate(render, PrStringView{details.data, details.count}, MermaidTextFormat_Plain, detail);
    details.reset();
    PrVec2 detailExtent = mermaidLabelExtent(render, detailLabel);
    float height = prF32Max(512 * kErrorScale, 400 * kErrorScale + detailExtent.y + 40 * kErrorScale);
    render.extent = PrVec2{2412 * kErrorScale, height};
    mermaidPushTransform(render, prAffine2Scale(PrVec2{kErrorScale, kErrorScale}));
    PrPath* path = prPathCreate();
    for (PrStringView data : kErrorIcon)
        prPathAppendData(path, data);
    mermaidFillPath(render, path, mermaidInk(MermaidRole_ErrorIcon));
    prPathDestroy(path);
    mermaidPopTransform(render);
    PrVec2 headingExtent = mermaidLabelExtent(render, headingLabel);
    MermaidInk text = mermaidInk(MermaidRole_ErrorText);
    mermaidLabelDraw(render, headingLabel, PrVec2{1440 * kErrorScale - headingExtent.x * 0.5f, 120 * kErrorScale}, text);
    mermaidLabelDraw(render, detailLabel, PrVec2{1440 * kErrorScale - detailExtent.x * 0.5f, 300 * kErrorScale}, text);
    PrMermaid* diagram = allocZeroed<PrMermaid>();
    finish(render, mermaidPreambleRead(desc->source), *diagram);
    return diagram;
}

void prMermaidDestroy(PrMermaid* PR_NULLABLE diagram) {
    if (diagram == nullptr)
        return;
    mermaidSceneReset(diagram->scene);
    diagram->items.reset();
    diagram->itemText.reset();
    diagram->itemPoints.reset();
    diagram->accessibleTitle.reset();
    diagram->accessibleDescription.reset();
    prMemoryFree(diagram);
}

PrMermaidKind prMermaidKind(const PrMermaid* diagram) {
    return diagram->kind;
}

PrVec2 prMermaidExtent(const PrMermaid* diagram) {
    return diagram->extent;
}

bool prMermaidScheme(const PrMermaid* diagram, PrColorScheme* out) {
    if (diagram->hasScheme)
        *out = diagram->scheme;
    return diagram->hasScheme;
}

PrColor prMermaidBackground(const PrMermaid* diagram, PrColorScheme scheme) {
    return prColorFromColor8(mermaidInkColor(mermaidInk(MermaidRole_Background), schemeOf(*diagram, scheme)));
}

PrStringView prMermaidAccessibleTitle(const PrMermaid* diagram) {
    return PrStringView{diagram->accessibleTitle.data, diagram->accessibleTitle.count};
}

PrStringView prMermaidAccessibleDescription(const PrMermaid* diagram) {
    return PrStringView{diagram->accessibleDescription.data, diagram->accessibleDescription.count};
}

void prMermaidDraw(const PrMermaid* diagram, PrDrawList* list, PrVec2 origin, PrColorScheme scheme) {
    prDrawListPushTransformAffine2(list, prAffine2Translate(origin));
    mermaidSceneDraw(diagram->scene, list, schemeOf(*diagram, scheme));
    prDrawListPopTransform(list);
}

uint32_t prMermaidItemCount(const PrMermaid* diagram) {
    return diagram->items.count;
}

PrMermaidItem prMermaidItem(const PrMermaid* diagram, uint32_t index) {
    PR_ASSERT(index < diagram->items.count);
    const MermaidItemRecord& item = diagram->items.data[index];
    return PrMermaidItem{
        .kind = item.kind,
        .rect = item.rect,
        .id = PrStringView{diagram->itemText.data + item.id.start, item.id.size},
        .link = PrStringView{diagram->itemText.data + item.link.start, item.link.size},
        .points = item.pointCount > 0 ? diagram->itemPoints.data + item.pointStart : nullptr,
        .pointCount = item.pointCount,
    };
}
