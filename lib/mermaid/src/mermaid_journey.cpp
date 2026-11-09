// User journeys (mermaid_journey.h). The tasks stand in a row in source order, under the headers of their sections,
// with the actors' legend to their left. Below each task, a face on a dashed line shows the task's score.

#include "mermaid/src/mermaid_journey.h"

#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/path.h"
#include "pr/sort.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's journey measurements, in px. The layout's y axis starts kTop below the top of the diagram, since
// mermaid.js's title sits above its origin.
constexpr float kTop = 25;
constexpr float kLeftMargin = 150;          // from the left of the diagram to the first task, without a wide legend
constexpr float kTaskWidth = 150;
constexpr float kTaskHeight = 50;
constexpr float kTaskStep = 200;            // from one task to the next
constexpr float kTaskReach = 100;           // from a task's left to the right of mermaid.js's bounds around it
constexpr float kSectionTop = 50;
constexpr float kTaskTop = 110;
constexpr float kTaskLineBottom = 450;
constexpr float kDiagramMarginX = 50;
constexpr float kDiagramMarginY = 10;
constexpr float kCornerRadius = 3;
constexpr float kFontSize = 16;
constexpr float kTitleFontSize = 33.5f;     // 4ex: four times the x-height of 16 px trebuchet ms
constexpr float kTitleBaseline = 25;
constexpr float kTitleSpace = 70;           // the height that a title adds to the diagram
constexpr float kArrowY = 200;
constexpr float kArrowWidth = 4;
constexpr float kTaskLineDash[2] = {4, 2};

// The face that shows a task's score.
constexpr float kFaceTop = 300;             // the center of the face of a score of 5
constexpr float kFaceStep = 30;             // how far the face moves down for each point less
constexpr float kFaceRadius = 15;
constexpr float kFaceStrokeWidth = 2;
constexpr float kEyeOffset = 5;
constexpr float kEyeRadius = 2.5f;          // a circle of 1.5 with a stroke of 2
constexpr float kMouthOuter = 7.5f;
constexpr float kMouthInner = 6.8181818f;
constexpr float kMouthWidth = 5;            // half the width of a neutral mouth
constexpr float kMouthDrop = 7;             // from the face's center to a neutral or sad mouth
constexpr float kSmileDrop = 2;             // from the face's center to a smile

// The actors' legend, and the actors' dots on their tasks.
constexpr float kActorRadius = 7;
constexpr float kActorInset = 14;           // from the left of a task to its first actor
constexpr float kActorStep = 10;
constexpr float kLegendX = 20;
constexpr float kLegendTop = 60;
constexpr float kLegendLine = 20;
constexpr float kLegendTextX = 50;
constexpr float kLegendTextDrop = 2;        // from an actor's dot to the middle of the first line of its name
constexpr float kLegendWrap = 360;          // maxLabelWidth
constexpr float kLegendHeight = 50;         // the height that each actor reserves in the diagram

constexpr PrColor8 kActorColors[] = {
    {0x8f, 0xbc, 0x8f, 0xff},
    {0x7c, 0xfc, 0x00, 0xff},
    {0x00, 0xff, 0xff, 0xff},
    {0x20, 0xb2, 0xaa, 0xff},
    {0xb0, 0xe0, 0xe6, 0xff},
    {0xff, 0xff, 0xe0, 0xff},
};

// The fills of the sections and their tasks in turn, for the light and dark schemes: mermaid.js's fillType0 to
// fillType6, the theme's primary and secondary colors and their turns of hue. The dark scheme turns the dark theme's
// node and cluster fills alike.
constexpr PrColor8 kSectionFills[][2] = {
    {{0xec, 0xec, 0xff, 0xff}, {0x2e, 0x2a, 0x4e, 0xff}},
    {{0xff, 0xff, 0xde, 0xff}, {0x33, 0x32, 0x1e, 0xff}},
    {{0xff, 0xec, 0xfe, 0xff}, {0x4e, 0x2a, 0x48, 0xff}},
    {{0xde, 0xff, 0xe0, 0xff}, {0x1e, 0x33, 0x1e, 0xff}},
    {{0xec, 0xff, 0xfe, 0xff}, {0x2a, 0x4c, 0x4e, 0xff}},
    {{0xff, 0xde, 0xe0, 0xff}, {0x33, 0x1e, 0x20, 0xff}},
    {{0xff, 0xef, 0xec, 0xff}, {0x4e, 0x33, 0x2a, 0xff}},
};

constexpr PrColor8 kBoxStroke = {0x66, 0x66, 0x66, 0xff};
constexpr PrColor8 kDarkBoxStroke = {0x9a, 0x9a, 0xaa, 0xff};
constexpr PrColor8 kFaceFill = {0xff, 0xf8, 0xdc, 0xff};
constexpr PrColor8 kFaceStroke = {0x99, 0x99, 0x99, 0xff};
constexpr PrColor8 kFeature = {0x66, 0x66, 0x66, 0xff};
constexpr PrColor8 kMouthFill = {0x33, 0x33, 0x33, 0xff};
constexpr PrColor8 kBlack = {0x00, 0x00, 0x00, 0xff};

// The style of the text in the boxes, which wraps at `width`.
MermaidTextStyle boxStyle(float width) {
    MermaidTextStyle style = mermaidTextStyle(kFontSize);
    style.wrapWidth = width;
    return style;
}

// Records a box with rounded corners and its label, centered in it. A label taller than its box overflows its bottom.
void drawBox(MermaidRender& render, PrRect box, MermaidInk fill, MermaidLabelIndex label) {
    PrPath* path = prPathCreate();
    PrCornerRect rounded{.rect = box};
    for (PrCorner& corner : rounded.corners)
        corner = PrCorner{.radii = PrVec2{kCornerRadius, kCornerRadius}, .shape = 1};
    prPathAddCornerRect(path, rounded);
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .fill = fill,
        .stroke = mermaidInkOfColors(kBoxStroke, kDarkBoxStroke),
        .strokeWidth = 1,
    };
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
    PrVec2 extent = mermaidLabelExtent(render, label);
    float top = box.top + prF32Max(0, (box.bottom - box.top - extent.y) * 0.5f);
    mermaidLabelDraw(render, label, PrVec2{(box.left + box.right - extent.x) * 0.5f, top},
        mermaidInk(MermaidRole_Text));
}

void drawActor(MermaidRender& render, PrVec2 center, uint32_t position) {
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, center, PrVec2{kActorRadius, kActorRadius}, 0);
    MermaidPaint paint{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .fill = mermaidInkOfColor(kActorColors[position % countOf(kActorColors)]),
        .stroke = mermaidInkOfColor(kBlack),
        .strokeWidth = 1,
    };
    mermaidDrawPath(render, path, paint);
    prPathDestroy(path);
}

// Records the face of `score` around `center`: it smiles above 3, frowns below 3, and is neutral at 3.
void drawFace(MermaidRender& render, PrVec2 center, double score) {
    PrPath* path = prPathCreate();
    prPathAddEllipse(path, center, PrVec2{kFaceRadius, kFaceRadius}, 0);
    MermaidPaint face{
        .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
        .fill = mermaidInkOfColor(kFaceFill),
        .stroke = mermaidInkOfColor(kFaceStroke),
        .strokeWidth = kFaceStrokeWidth,
    };
    mermaidDrawPath(render, path, face);
    prPathClear(path);
    prPathAddEllipse(path, PrVec2{center.x - kEyeOffset, center.y - kEyeOffset}, PrVec2{kEyeRadius, kEyeRadius}, 0);
    prPathAddEllipse(path, PrVec2{center.x + kEyeOffset, center.y - kEyeOffset}, PrVec2{kEyeRadius, kEyeRadius}, 0);
    mermaidFillPath(render, path, mermaidInkOfColor(kFeature));
    prPathClear(path);
    if (score > 3 || score < 3) {
        bool smile = score > 3;
        PrVec2 at{center.x, center.y + (smile ? kSmileDrop : kMouthDrop)};
        prPathAddSector(path, at, kMouthInner, kMouthOuter, smile ? 0 : float(kPrF64Pi), float(kPrF64Pi));
        MermaidPaint mouth{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth,
            .fill = mermaidInkOfColor(kMouthFill),
            .stroke = mermaidInkOfColor(kFeature),
            .strokeWidth = 1,
        };
        mermaidDrawPath(render, path, mouth);
    } else {
        float y = center.y + kMouthDrop;
        mermaidDrawLine(render, PrVec2{center.x - kMouthWidth, y}, PrVec2{center.x + kMouthWidth, y},
            mermaidInkOfColor(kFeature), 1, nullptr);
    }
    prPathDestroy(path);
}

// Records an arrow from `from` to `to`, which runs to the right, at `width`. Its head is mermaid.js's arrowhead marker,
// which scales with the line's width.
void drawArrow(MermaidRender& render, PrVec2 from, PrVec2 to, float width) {
    mermaidDrawLine(render, from, to, mermaidInk(MermaidRole_Text), width, nullptr);
    PrPath* head = prPathCreate();
    prPathMoveTo(head, PrVec2{to.x - 5 * width, to.y - 2 * width});
    prPathLineTo(head, PrVec2{to.x + width, to.y});
    prPathLineTo(head, PrVec2{to.x - 5 * width, to.y + 2 * width});
    prPathClose(head);
    mermaidFillPath(render, head, mermaidInk(MermaidRole_StrongText));
    prPathDestroy(head);
}

struct Parser : MermaidParseState {
    MermaidJourney* journey;
    MermaidSpan section;                // the name of the section that the next task is in
};

// The text of a statement before a comment, which starts with # or ;.
PrStringView beforeComment(PrStringView text) {
    size_t hash = prStringFindByte(text, '#');
    size_t semicolon = prStringFindByte(text, ';');
    return mermaidBefore(text, hash < semicolon ? hash : semicolon);
}

// Task name: score: actor, actor
bool parseTask(Parser& parser, PrStringView statement) {
    MermaidJourney& journey = *parser.journey;
    size_t colon = prStringFindByte(statement, ':');
    if (colon == statement.size)
        return mermaidParseFail(parser, "a task has no score");
    PrStringView name = prStringTrim(mermaidBefore(statement, colon));
    if (name.size == 0)
        return mermaidParseFail(parser, "a task has no name");
    PrStringView data = mermaidAfter(statement, colon + 1);
    size_t actorsAt = prStringFindByte(data, ':');
    PrStringView scoreText = prStringTrim(mermaidBefore(data, actorsAt));
    double score = 0;
    if (scoreText.size > 0 && mermaidNumberParse(scoreText, &score) != scoreText.size)
        return mermaidParseFail(parser, "a task's score is not a number");
    MermaidJourneyTask task{
        .name = mermaidTextAdd(journey.text, name),
        .section = parser.section,
        .score = score,
        .firstActor = journey.actors.count,
        .actorCount = 0,
    };
    mermaidEachName(mermaidAfter(data, actorsAt + 1), [&](PrStringView actor) {
        journey.actors.push(mermaidTextAdd(journey.text, actor));
        ++task.actorCount;
    });
    journey.tasks.push(task);
    return true;
}

bool parseStatement(Parser& parser, PrStringView content) {
    MermaidJourney& journey = *parser.journey;
    PrStringView statement = beforeComment(content);
    if (mermaidStartsWithWord(statement, "title")) {
        journey.title = mermaidTextAdd(journey.text, prStringTrim(mermaidAfter(statement, 5)));
        return true;
    }
    if (mermaidStartsWithWord(statement, "section")) {
        PrStringView rest = mermaidAfter(statement, 7);
        parser.section = mermaidTextAdd(journey.text, prStringTrim(mermaidBefore(rest, prStringFindByte(rest, ':'))));
        return true;
    }
    return parseTask(parser, statement);
}

} // namespace

PrStatus mermaidJourneyParse(MermaidJourney& journey, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    journey.title = MermaidSpan{};
    journey.text.clear();
    journey.tasks.clear();
    journey.actors.clear();
    Parser parser{};
    parser.error = error;
    parser.journey = &journey;
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_Journey, nullptr);
    journey.title = mermaidTextAdd(journey.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        ok = parseStatement(parser, line.content);
    }
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidJourneyText(const MermaidJourney& journey, MermaidSpan span) {
    return mermaidTextAt(journey.text, span);
}

void mermaidJourneyReset(MermaidJourney& journey) {
    journey.text.reset();
    journey.tasks.reset();
    journey.actors.reset();
}

PrStatus mermaidJourneyDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidJourney journey{};
    PrStatus status = mermaidJourneyParse(journey, source, error);
    if (status != PrStatus_Ok) {
        mermaidJourneyReset(journey);
        return status;
    }
    auto text = [&](MermaidSpan span) { return mermaidJourneyText(journey, span); };

    // The legend lists each actor once, sorted by name, and an actor's position in it picks the actor's color.
    RawArray<MermaidSpan> actors{};
    MermaidNames positions{};                   // by name, which views the journey's text
    for (MermaidSpan actor : journey.actors.span()) {
        if (mermaidNamesFind(positions, text(actor)) == kMermaidNone) {
            mermaidNamesSet(positions, text(actor), 0);
            actors.push(actor);
        }
    }
    sortItems(actors.span(), [&](MermaidSpan a, MermaidSpan b) { return prStringCompare(text(a), text(b)) < 0; });
    for (uint32_t i = 0; i < actors.count; ++i)
        mermaidNamesSet(positions, text(actors.data[i]), i);

    // A legend wider than half the left margin moves the tasks right by its width.
    RawArray<MermaidLabelIndex> legend{};
    MermaidTextStyle legendStyle = mermaidTextStyle(kFontSize);
    legendStyle.align = PrTextAlign_Start;
    legendStyle.wrapWidth = kLegendWrap;
    legendStyle.lineHeight = kLegendLine;
    float legendWidth = 0;
    for (MermaidSpan actor : actors.span()) {
        MermaidLabelIndex label = mermaidLabelCreate(render, text(actor), MermaidTextFormat_Plain, legendStyle);
        legend.push(label);
        float width = mermaidLabelExtent(render, label).x;
        if (width > legendWidth && width > kLeftMargin - width)
            legendWidth = width;
    }
    float left = kLeftMargin + legendWidth;

    float right = left;
    float bottom = float(actors.count) * kLegendHeight;
    if (journey.tasks.count > 0) {
        right = prF32Max(right, left + float(journey.tasks.count - 1) * kTaskStep + kTaskReach);
        bottom = prF32Max(bottom, kTaskLineBottom);
    }
    float width = left + right + 2 * kDiagramMarginX;
    float height = bottom + 2 * kDiagramMarginY + (journey.title.size > 0 ? kTitleSpace : 0);
    render.extent = PrVec2{width, height};
    mermaidPushTransform(render, prAffine2Translate(PrVec2{0, kTop}));
    PrVec2 offset{0, kTop};

    // The legend: each actor's dot, with its name to the right.
    float y = kLegendTop;
    for (uint32_t i = 0; i < actors.count; ++i) {
        drawActor(render, PrVec2{kLegendX, y}, i);
        mermaidLabelDraw(render, legend.data[i], PrVec2{kLegendTextX, y + kLegendTextDrop - kLegendLine * 0.5f},
            mermaidInk(MermaidRole_Text));
        y += prF32Max(kLegendLine, float(mermaidLabelLineCount(render, legend.data[i])) * kLegendLine);
    }

    // Each section's header spans its tasks, and the sections take the fills in turn.
    uint32_t headers = 0;
    uint32_t fill = 0;
    for (uint32_t i = 0; i < journey.tasks.count; ++i) {
        const MermaidJourneyTask& task = journey.tasks.data[i];
        float x = left + float(i) * kTaskStep;
        PrStringView previous = i > 0 ? text(journey.tasks.data[i - 1].section) : PrStringView{};
        bool starts = text(task.section) != previous;
        if (starts)
            fill = headers++;
        MermaidInk fillInk = mermaidInkOfColors(kSectionFills[fill % countOf(kSectionFills)][0],
            kSectionFills[fill % countOf(kSectionFills)][1]);
        if (starts) {
            uint32_t count = 0;
            while (i + count < journey.tasks.count && text(journey.tasks.data[i + count].section) == text(task.section))
                ++count;
            PrRect box{x, kSectionTop, x + float(count - 1) * kTaskStep + kTaskWidth, kSectionTop + kTaskHeight};
            MermaidLabelIndex label =
                mermaidLabelCreate(render, text(task.section), MermaidTextFormat_Plain, boxStyle(box.right - box.left));
            drawBox(render, box, fillInk, label);
            mermaidItemAdd(render, PrMermaidItemKind_Group, prRectOffset(box, offset), text(task.section),
                PrStringView{});
        }

        float center = x + kTaskWidth * 0.5f;
        mermaidDrawLine(render, PrVec2{center, kTaskTop}, PrVec2{center, kTaskLineBottom}, mermaidInk(MermaidRole_Text),
            1, kTaskLineDash);
        drawFace(render, PrVec2{center, kFaceTop + float(5 - task.score) * kFaceStep}, task.score);
        PrRect box{x, kTaskTop, x + kTaskWidth, kTaskTop + kTaskHeight};
        MermaidLabelIndex label =
            mermaidLabelCreate(render, text(task.name), MermaidTextFormat_Plain, boxStyle(kTaskWidth));
        drawBox(render, box, fillInk, label);
        for (uint32_t a = 0; a < task.actorCount; ++a) {
            uint32_t position = mermaidNamesFind(positions, text(journey.actors.data[task.firstActor + a]));
            drawActor(render, PrVec2{x + kActorInset + float(a) * kActorStep, kTaskTop}, position);
        }
        mermaidItemAdd(render, PrMermaidItemKind_Node, prRectOffset(box, offset), text(task.name), PrStringView{});
    }

    if (journey.title.size > 0) {
        MermaidTextStyle style = mermaidTextStyle(kTitleFontSize);
        style.weight = 700;
        MermaidLabelIndex title = mermaidLabelCreate(render, text(journey.title), MermaidTextFormat_Plain, style);
        PrVec2 extent = mermaidLabelExtent(render, title);
        mermaidLabelDraw(render, title, PrVec2{left, kTitleBaseline - extent.y * 0.75f}, mermaidInk(MermaidRole_Title));
    }
    drawArrow(render, PrVec2{left, kArrowY}, PrVec2{width - left - 4, kArrowY}, kArrowWidth);
    mermaidPopTransform(render);

    actors.reset();
    mermaidNamesReset(positions);
    legend.reset();
    mermaidJourneyReset(journey);
    return PrStatus_Ok;
}

} // namespace pr_impl
