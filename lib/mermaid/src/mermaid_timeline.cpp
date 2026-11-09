// Timelines (mermaid_timeline.h). The periods stand in a row in source order, under the headers of their sections,
// above an arrow. Each period's events stack below it, along a dashed line.

#include "mermaid/src/mermaid_timeline.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/path.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's timeline measurements, in px.
constexpr float kLeftMargin = 150;          // from the left of the layout to the start of the arrow
constexpr float kFirstLeft = 200;           // the left of the first section or period
constexpr float kTop = 50;                  // the top of the sections, or of the periods if there are no sections
constexpr float kStep = 200;                // from one period to the next
constexpr float kTextWidth = 150;           // the width at which a period's or an event's text wraps
constexpr float kPadding = 20;              // on each side of a box's text, and in all above and below it
constexpr float kBoxWidth = kTextWidth + 2 * kPadding;
constexpr float kHeightMargin = 20;         // what the sections and periods add to the height of their tallest box
constexpr float kSectionGap = 50;           // from the bottom of the sections to the top of the periods
constexpr float kEventDrop = 200;           // from the top of a period to its first event
constexpr float kEventGap = 10;
constexpr float kMinEventHeight = 50;
constexpr float kLineReach = 200;           // how much longer a period's dashed line is than its events' heights
constexpr float kArrowDrop = 50;            // from the bottom of the periods to the arrow
constexpr float kArrowWidth = 4;
constexpr float kLineWidth = 2;             // a period's dashed line
constexpr float kLineDash[2] = {5, 5};
constexpr float kCornerRadius = 5;          // of a box's top corners
constexpr float kUnderline = 3;             // the line along a box's bottom
constexpr float kFontSize = 16;
constexpr float kLineHeight = 17.6f;        // 1.1em
constexpr float kLineBox = 19;              // the height of the box of one line of text in mermaid.js's font
constexpr float kTitleFontSize = 33.5f;     // 4ex: four times the x-height of 16 px trebuchet ms
constexpr float kTitleBaseline = 20;
constexpr float kDiagramPadding = 50;
constexpr float kEventBrightness = 1.2f;    // the brightness filter on events

// The colors of each section in turn, mermaid.js's cScale0 to cScale11: the fill, the line along the bottom, and
// whether the text is white on the light theme's fill. The dark theme's fills are dark, and its text is white.
struct SectionColors {
    PrColor8 fill;
    PrColor8 darkFill;
    PrColor8 line;
    PrColor8 darkLine;
    bool whiteText;
};

constexpr SectionColors kSectionColors[12] = {
    {{0x86, 0x86, 0xff, 0xff}, {0x00, 0x00, 0xa3, 0xff}, {0xff, 0xff, 0xb9, 0xff}, {0xff, 0xff, 0x33, 0xff}, true},
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

constexpr PrColor8 kWhite = {0xff, 0xff, 0xff, 0xff};
constexpr PrColor8 kBlack = {0x00, 0x00, 0x00, 0xff};

// True if `text` has a colon at `at` that separates events: one followed by a space, or one at the end.
bool separatesEvents(PrStringView text, size_t at) {
    return text.data[at] == ':' && (at + 1 == text.size || prAsciiIsSpace(text.data[at + 1]));
}

// Reads the events in `text`, each after a colon and a space, into the last period. Returns false if `text` does not
// start with an event.
bool readEvents(MermaidTimeline& timeline, PrStringView text) {
    MermaidTimelinePeriod& period = timeline.periods.data[timeline.periods.count - 1];
    size_t at = 0;
    while (at < text.size) {
        if (!separatesEvents(text, at) || at + 1 == text.size)
            return false;
        size_t end = at + 1;
        while (end < text.size && !separatesEvents(text, end))
            ++end;
        PrStringView event = prStringTrim(PrStringView{text.data + at + 1, end - at - 1});
        if (event.size == 0)
            return false;
        timeline.events.push(mermaidTextAdd(timeline.text, event));
        ++period.eventCount;
        at = end;
    }
    return true;
}

// Grows `rect` to contain `point`.
void include(PrRect& rect, PrVec2 point) {
    rect.left = prF32Min(rect.left, point.x);
    rect.top = prF32Min(rect.top, point.y);
    rect.right = prF32Max(rect.right, point.x);
    rect.bottom = prF32Max(rect.bottom, point.y);
}

// The brighter color that mermaid.js's brightness filter gives an event.
PrColor8 brighter(PrColor8 color) {
    auto channel = [](uint8_t value) { return uint8_t(prF32Min(255, prF32Round(float(value) * kEventBrightness))); };
    return PrColor8{channel(color.r), channel(color.g), channel(color.b), color.a};
}

// A box of the timeline: a section, a period or an event.
struct Box {
    PrRect rect;
    MermaidLabelIndex label;
    uint32_t color;                         // in kSectionColors
    bool event;
    PrMermaidItemKind kind;
    PrStringView text;
};

// The layout of a timeline, which is measured before it is drawn.
struct Layout {
    MermaidRender* render;
    RawArray<Box> boxes;
    RawArray<PrVec2> lines;                 // the ends of each period's dashed line
};

MermaidLabelIndex createLabel(MermaidRender& render, PrStringView text, float wrapWidth) {
    MermaidTextStyle style = mermaidTextStyle(kFontSize);
    style.wrapWidth = wrapWidth;
    style.lineHeight = kLineHeight;
    return mermaidLabelCreate(render, text, MermaidTextFormat_Plain, style);
}

// The height of a box around a label, before any minimum: its lines, half a line more, and its padding.
float boxHeight(const MermaidRender& render, MermaidLabelIndex label) {
    float lines = float(mermaidLabelLineCount(render, label));
    return kLineBox + (lines - 1) * kLineHeight + kFontSize * 1.1f * 0.5f + kPadding;
}

// The height of a box around `text` wrapped at the width of a period, as mermaid.js measures it before it draws.
float measure(MermaidRender& render, PrStringView text) {
    return boxHeight(render, createLabel(render, text, kTextWidth));
}

// Lays out a box with its top left at `origin`, `textWidth` wide inside its padding, and at least `minHeight` tall.
// Returns its height.
float addBox(Layout& l, PrVec2 origin, PrStringView text, float textWidth, float minHeight, uint32_t color, bool event,
        PrMermaidItemKind kind) {
    MermaidLabelIndex label = createLabel(*l.render, text, textWidth);
    float height = prF32Max(boxHeight(*l.render, label), minHeight);
    l.boxes.push(Box{
        .rect = PrRect{origin.x, origin.y, origin.x + textWidth + 2 * kPadding, origin.y + height},
        .label = label,
        .color = color,
        .event = event,
        .kind = kind,
        .text = text,
    });
    return height;
}

// Lays out a period at `origin`, its events below it, and its dashed line.
void addPeriod(Layout& l, const MermaidTimeline& timeline, const MermaidTimelinePeriod& period, PrVec2 origin,
        uint32_t color, float periodHeight, float eventsHeight) {
    PrStringView name = mermaidTimelineText(timeline, period.name);
    float height = addBox(l, origin, name, kTextWidth, periodHeight, color, false, PrMermaidItemKind_Node);
    float y = origin.y + kEventDrop;
    for (uint32_t e = 0; e < period.eventCount; ++e) {
        PrStringView event = mermaidTimelineText(timeline, timeline.events.data[period.firstEvent + e]);
        float eventHeight =
            addBox(l, PrVec2{origin.x, y}, event, kTextWidth, kMinEventHeight, color, true, PrMermaidItemKind_Node);
        y += eventHeight + kEventGap;
    }
    float x = origin.x + kBoxWidth * 0.5f;
    float top = origin.y + prF32Max(height, periodHeight);
    l.lines.push(PrVec2{x, top});
    l.lines.push(PrVec2{x, top + kLineReach + eventsHeight});
}

// Records an arrow from `from` to `to`, which runs to the right or down, at `width`. Its head is mermaid.js's arrowhead
// marker, which scales with the line's width.
void drawArrow(MermaidRender& render, PrVec2 from, PrVec2 to, float width, const float* PR_NULLABLE dash) {
    MermaidInk ink = mermaidInk(MermaidRole_StrongText);
    mermaidDrawLine(render, from, to, ink, width, dash);
    PrVec2 along = to.x > from.x ? PrVec2{1, 0} : PrVec2{0, 1};
    PrVec2 across{-along.y, along.x};
    PrVec2 base{to.x - along.x * 5 * width, to.y - along.y * 5 * width};
    PrPath* head = prPathCreate();
    prPathMoveTo(head, PrVec2{base.x + across.x * 2 * width, base.y + across.y * 2 * width});
    prPathLineTo(head, PrVec2{to.x + along.x * width, to.y + along.y * width});
    prPathLineTo(head, PrVec2{base.x - across.x * 2 * width, base.y - across.y * 2 * width});
    prPathClose(head);
    mermaidFillPath(render, head, ink);
    prPathDestroy(head);
}

void drawBox(MermaidRender& render, const Box& box) {
    const SectionColors& colors = kSectionColors[box.color % countOf(kSectionColors)];
    PrColor8 fill = box.event ? brighter(colors.fill) : colors.fill;
    PrColor8 darkFill = box.event ? brighter(colors.darkFill) : colors.darkFill;
    PrColor8 line = box.event ? brighter(colors.line) : colors.line;
    PrColor8 darkLine = box.event ? brighter(colors.darkLine) : colors.darkLine;
    PrPath* path = prPathCreate();
    PrCornerRect rounded{.rect = box.rect};
    rounded.corners[PrRectCorner_TopLeft] = PrCorner{.radii = PrVec2{kCornerRadius, kCornerRadius}, .shape = 1};
    rounded.corners[PrRectCorner_TopRight] = rounded.corners[PrRectCorner_TopLeft];
    prPathAddCornerRect(path, rounded);
    mermaidFillPath(render, path, mermaidInkOfColors(fill, darkFill));
    prPathDestroy(path);
    mermaidDrawLine(render, PrVec2{box.rect.left, box.rect.bottom}, PrVec2{box.rect.right, box.rect.bottom},
        mermaidInkOfColors(line, darkLine), kUnderline, nullptr);
    // The first line's middle is half the padding and one em below the top.
    PrVec2 extent = mermaidLabelExtent(render, box.label);
    PrVec2 origin{
        (box.rect.left + box.rect.right - extent.x) * 0.5f,
        box.rect.top + kPadding * 0.5f + kFontSize - kLineHeight * 0.5f,
    };
    mermaidLabelDraw(render, box.label, origin, mermaidInkOfColors(colors.whiteText ? kWhite : kBlack, kWhite));
}

} // namespace

PrStatus mermaidTimelineParse(MermaidTimeline& timeline, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    timeline.title = MermaidSpan{};
    timeline.text.clear();
    timeline.sections.clear();
    timeline.periods.clear();
    timeline.events.clear();
    MermaidParseState parser{.error = error};
    MermaidPreamble preamble = mermaidPreambleRead(source);
    // timeline, timeline LR or timeline TD
    PrStringView direction{};
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_Timeline, &direction);
    if (ok && direction.size > 0 && direction != "LR" && direction != "TD")
        ok = mermaidParseFail(parser, "a timeline's direction is LR or TD");
    timeline.title = mermaidTextAdd(timeline.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        PrStringView content = line.content;
        if (prStringStartsWith(content, "#"))
            continue;
        if (mermaidStartsWithWord(content, "title")) {
            timeline.title = mermaidTextAdd(timeline.text, prStringTrim(mermaidAfter(content, 5)));
            continue;
        }
        if (mermaidStartsWithWord(content, "section")) {
            PrStringView rest = mermaidAfter(content, 7);
            PrStringView name = prStringTrim(mermaidBefore(rest, prStringFindByte(rest, ':')));
            timeline.sections.push(mermaidTextAdd(timeline.text, name));
            continue;
        }
        // A period and its events (period : event : event), or more events of the last period (: event).
        size_t colon = 0;
        if (content.data[0] != ':') {
            while (colon < content.size && content.data[colon] != ':' && content.data[colon] != '#')
                ++colon;
            timeline.periods.push(MermaidTimelinePeriod{
                .name = mermaidTextAdd(timeline.text, prStringTrim(mermaidBefore(content, colon))),
                .section = timeline.sections.count > 0 ? timeline.sections.count - 1 : kMermaidNone,
                .firstEvent = timeline.events.count,
                .eventCount = 0,
            });
            if (colon < content.size && content.data[colon] == '#')
                continue;
        } else if (timeline.periods.count == 0) {
            ok = mermaidParseFail(parser, "an event comes before any period");
            break;
        }
        if (!readEvents(timeline, mermaidAfter(content, colon))) {
            parser.column += uint32_t(colon);
            ok = mermaidParseFail(parser, "an event must follow a colon and a space");
        }
    }
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidTimelineText(const MermaidTimeline& timeline, MermaidSpan span) {
    return mermaidTextAt(timeline.text, span);
}

void mermaidTimelineReset(MermaidTimeline& timeline) {
    timeline.text.reset();
    timeline.sections.reset();
    timeline.periods.reset();
    timeline.events.reset();
}

PrStatus mermaidTimelineDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidTimeline timeline{};
    PrStatus status = mermaidTimelineParse(timeline, source, error);
    if (status != PrStatus_Ok) {
        mermaidTimelineReset(timeline);
        return status;
    }
    auto text = [&](MermaidSpan span) { return mermaidTimelineText(timeline, span); };

    // Every section is as tall as the tallest, and so is every period. mermaid.js measures a section's text at the
    // width of a period, and draws it at the section's own width.
    float sectionHeight = 0;
    for (MermaidSpan section : timeline.sections.span())
        sectionHeight = prF32Max(sectionHeight, measure(render, text(section)) + kHeightMargin);
    float periodHeight = 0;
    float eventsHeight = 0;
    for (const MermaidTimelinePeriod& period : timeline.periods.span()) {
        periodHeight = prF32Max(periodHeight, measure(render, text(period.name)) + kHeightMargin);
        float height = 0;
        for (uint32_t e = 0; e < period.eventCount; ++e)
            height += measure(render, text(timeline.events.data[period.firstEvent + e]));
        if (period.eventCount > 0)
            height += float(period.eventCount - 1) * kEventGap;
        eventsHeight = prF32Max(eventsHeight, height);
    }

    // With sections, each section's header spans its periods, and they share its color. Periods before the first
    // section are not drawn. Without sections, each period takes the next color. A period is in the last section before
    // it, so each section's periods follow one another in source order.
    Layout l{.render = &render};
    float periodTop = kTop;
    if (timeline.sections.count > 0) {
        periodTop = kTop + sectionHeight + kSectionGap;
        float x = kFirstLeft;
        uint32_t p = 0;
        while (p < timeline.periods.count && timeline.periods.data[p].section == kMermaidNone)
            ++p;
        for (uint32_t s = 0; s < timeline.sections.count; ++s) {
            uint32_t first = p;
            while (p < timeline.periods.count && timeline.periods.data[p].section == s)
                ++p;
            uint32_t count = p - first;
            float width = kStep * float(count > 0 ? count : 1) - (kStep - kTextWidth);
            addBox(l, PrVec2{x, kTop}, text(timeline.sections.data[s]), width, sectionHeight, s, false,
                PrMermaidItemKind_Group);
            for (uint32_t i = first; i < p; ++i) {
                addPeriod(l, timeline, timeline.periods.data[i], PrVec2{x, periodTop}, s, periodHeight, eventsHeight);
                x += kStep;
            }
            if (count == 0)
                x += kStep;
        }
    } else {
        for (uint32_t i = 0; i < timeline.periods.count; ++i) {
            addPeriod(l, timeline, timeline.periods.data[i], PrVec2{kFirstLeft + float(i) * kStep, periodTop}, i,
                periodHeight, eventsHeight);
        }
    }

    // The arrow's length and the title's place follow from the bounds of the boxes and lines.
    PrRect content{kFirstLeft, kTop, kFirstLeft, kTop};
    for (const Box& box : l.boxes.span()) {
        include(content, PrVec2{box.rect.left, box.rect.top});
        include(content, PrVec2{box.rect.right, box.rect.bottom});
    }
    for (PrVec2 point : l.lines.span())
        include(content, point);
    float arrowY = periodTop + periodHeight + kArrowDrop;
    PrVec2 arrowFrom{kLeftMargin, arrowY};
    PrVec2 arrowTo{content.right - content.left + 3 * kLeftMargin, arrowY};
    PrRect bounds = content;
    include(bounds, arrowFrom);
    include(bounds, arrowTo);
    MermaidLabelIndex title = kMermaidNone;
    PrVec2 titleOrigin{};
    if (timeline.title.size > 0) {
        MermaidTextStyle style = mermaidTextStyle(kTitleFontSize);
        style.weight = 700;
        title = mermaidLabelCreate(render, text(timeline.title), MermaidTextFormat_Plain, style);
        PrVec2 extent = mermaidLabelExtent(render, title);
        titleOrigin = PrVec2{(content.right - content.left) * 0.5f - kLeftMargin, kTitleBaseline - extent.y * 0.75f};
        include(bounds, titleOrigin);
        include(bounds, PrVec2{titleOrigin.x + extent.x, titleOrigin.y + extent.y});
    }
    render.extent = PrVec2{
        bounds.right - bounds.left + 2 * kDiagramPadding,
        bounds.bottom - bounds.top + 2 * kDiagramPadding,
    };
    PrVec2 offset{kDiagramPadding - bounds.left, kDiagramPadding - bounds.top};
    mermaidPushTransform(render, prAffine2Translate(offset));

    // The events cover the dashed lines that run down through them.
    for (uint32_t i = 0; i + 1 < l.lines.count; i += 2)
        drawArrow(render, l.lines.data[i], l.lines.data[i + 1], kLineWidth, kLineDash);
    for (const Box& box : l.boxes.span()) {
        drawBox(render, box);
        mermaidItemAdd(render, box.kind, prRectOffset(box.rect, offset), box.text, PrStringView{});
    }
    if (title != kMermaidNone)
        mermaidLabelDraw(render, title, titleOrigin, mermaidInk(MermaidRole_Title));
    drawArrow(render, arrowFrom, arrowTo, kArrowWidth, nullptr);
    mermaidPopTransform(render);

    l.boxes.reset();
    l.lines.reset();
    mermaidTimelineReset(timeline);
    return PrStatus_Ok;
}

} // namespace pr_impl
