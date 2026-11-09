// Quadrant charts (mermaid_quadrant.h). The chart is 500 px square, as mermaid.js's default configuration makes it. The
// title is at the top, and the axes' labels are at the left and, if the chart has points, at the bottom. The four
// quadrants fill the rest, each in its own shade of the theme's primary color, with its label at its top or, if the
// chart has no points, at its middle.

#include "mermaid/src/mermaid_quadrant.h"

#include "pr/ascii.h"
#include "pr/color_css.h"
#include "pr/float.h"
#include "pr/geom.h"
#include "pr/mem.h"
#include "pr/path.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's quadrant chart configuration, in px.
constexpr float kChartSize = 500;
constexpr float kTitleFontSize = 20;
constexpr float kTitlePadding = 10;             // above and below the title
constexpr float kQuadrantPadding = 5;           // around the quadrants
constexpr float kAxisFontSize = 16;
constexpr float kAxisPadding = 5;               // on either side of an axis's labels
constexpr float kQuadrantFontSize = 16;
constexpr float kQuadrantTextPadding = 5;       // above a quadrant's label, if the chart has points
constexpr float kPointFontSize = 12;
constexpr float kPointTextPadding = 5;          // between a point's center and its label
constexpr float kPointRadius = 5;
constexpr float kExternalBorder = 2;
constexpr float kInternalBorder = 1;

// A color in the light scheme and in the dark one.
struct Colors {
    PrColor8 light;
    PrColor8 dark;
};

// The theme's colors: mermaid.js's default theme in the light scheme, and the engine's dark counterpart, whose
// quadrants are the dark theme's node fill. A quadrant's fill and its label's color step away from the primary color by
// 5 in each channel, from quadrant-1 to quadrant-4.
constexpr Colors kQuadrantFills[4] = {
    {{0xec, 0xec, 0xff, 0xff}, {0x2e, 0x2a, 0x4e, 0xff}},
    {{0xf1, 0xf1, 0xff, 0xff}, {0x33, 0x2f, 0x53, 0xff}},
    {{0xf6, 0xf6, 0xff, 0xff}, {0x38, 0x34, 0x58, 0xff}},
    {{0xfb, 0xfb, 0xff, 0xff}, {0x3d, 0x39, 0x5d, 0xff}},
};
constexpr Colors kQuadrantTexts[4] = {
    {{0x13, 0x13, 0x00, 0xff}, {0xe8, 0xe6, 0xf5, 0xff}},
    {{0x0e, 0x0e, 0x00, 0xff}, {0xe3, 0xe1, 0xf0, 0xff}},
    {{0x09, 0x09, 0x00, 0xff}, {0xde, 0xdc, 0xeb, 0xff}},
    {{0x04, 0x04, 0x00, 0xff}, {0xd9, 0xd7, 0xe6, 0xff}},
};
constexpr Colors kText = {{0x13, 0x13, 0x00, 0xff}, {0xe8, 0xe6, 0xf5, 0xff}};      // the title, axes and points
constexpr Colors kBorder = {{0xc7, 0xc7, 0xf1, 0xff}, {0x5c, 0x54, 0x90, 0xff}};

struct Parser : MermaidParseState {
    MermaidQuadrantChart* chart;

    // The classes that classDef statements define, by their names, which view the source. A class defined more than
    // once has its last definition.
    MermaidNames classNames;            // the index in `classes` of each class's name, which views the source
    RawArray<MermaidQuadrantStyle> classes;

    RawArray<PrStringView> pointClasses; // by point: the class that ::: gives it, or empty
};

// Removes the quotes of a string, and the backticks within them of a Markdown string, which the chart draws as plain
// text.
PrStringView plainText(PrStringView text) {
    text = prStringTrim(text);
    PrStringView inner = mermaidUnquoted(text);
    if (inner.size < text.size && inner.size >= 2 && inner.data[0] == '`' && inner.data[inner.size - 1] == '`')
        return PrStringView{inner.data + 1, inner.size - 2};
    return inner;
}

// Reads an axis's texts: `first --> second`, with any number of dashes of two or more, or `first` alone.
void readAxis(Parser& parser, PrStringView text, MermaidSpan (&axis)[2]) {
    MermaidQuadrantChart& chart = *parser.chart;
    for (size_t i = 0; i < text.size; ++i) {
        size_t end = i;
        while (end < text.size && text.data[end] == '-')
            ++end;
        if (end - i >= 2 && end < text.size && text.data[end] == '>') {
            axis[0] = mermaidTextAdd(chart.text, plainText(mermaidBefore(text, i)));
            axis[1] = mermaidTextAdd(chart.text, plainText(mermaidAfter(text, end + 1)));
            return;
        }
        i = end > i ? end - 1 : i;
    }
    axis[0] = mermaidTextAdd(chart.text, plainText(text));
    axis[1] = MermaidSpan{};
}

bool isHexColor(PrStringView text) {
    if (prStringStartsWith(text, "#"))
        text = mermaidAfter(text, 1);
    if (text.size != 3 && text.size != 6)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsHexDigit(text.data[i]))
            return false;
    }
    return true;
}

bool readColor(PrStringView text, PrColor8* out) {
    if (!isHexColor(text))
        return false;
    char hex[8] = {'#'};
    PrStringView digits = prStringStartsWith(text, "#") ? mermaidAfter(text, 1) : text;
    prMemCopy(hex + 1, digits.data, digits.size);
    return prColor8FromCss(PrStringView{hex, digits.size + 1}, out);
}

bool allDigits(PrStringView text) {
    if (text.size == 0)
        return false;
    for (size_t i = 0; i < text.size; ++i) {
        if (!prAsciiIsDigit(text.data[i]))
            return false;
    }
    return true;
}

// Reads styles separated by commas, each `name: value`, into `style`: radius, color, stroke-color and stroke-width,
// with mermaid.js's checks of their values.
bool readStyles(Parser& parser, PrStringView text, MermaidQuadrantStyle& style) {
    size_t start = 0;
    for (size_t i = 0; i <= text.size; ++i) {
        if (i < text.size && text.data[i] != ',')
            continue;
        PrStringView item = prStringTrim(PrStringView{text.data + start, i - start});
        start = i + 1;
        if (item.size == 0)
            continue;
        size_t colon = prStringFindByte(item, ':');
        PrStringView name = prStringTrim(mermaidBefore(item, colon));
        PrStringView value = prStringTrim(mermaidAfter(item, colon + 1));
        double number = 0;
        if (name == "radius") {
            if (!allDigits(value) || mermaidNumberParse(value, &number) == 0)
                return mermaidParseFail(parser, "a point's radius is not a number");
            style.radius = float(number);
            style.flags |= MermaidQuadrantStyleFlag_Radius;
        } else if (name == "color") {
            if (!readColor(value, &style.color))
                return mermaidParseFail(parser, "a point's color is not a hex code");
            style.flags |= MermaidQuadrantStyleFlag_Color;
        } else if (name == "stroke-color") {
            if (!readColor(value, &style.strokeColor))
                return mermaidParseFail(parser, "a point's stroke color is not a hex code");
            style.flags |= MermaidQuadrantStyleFlag_StrokeColor;
        } else if (name == "stroke-width") {
            bool pixels = value.size >= 3 && prStringEndsWith(value, "px");
            if (!pixels || !allDigits(mermaidBefore(value, value.size - 2)) || mermaidNumberParse(value, &number) == 0)
                return mermaidParseFail(parser, "a point's stroke width is not a number of pixels");
            style.strokeWidth = float(number);
            style.flags |= MermaidQuadrantStyleFlag_StrokeWidth;
        } else {
            return mermaidParseFail(parser, "unknown style of a point");
        }
    }
    return true;
}

bool readCoordinate(PrStringView text, double* out) {
    text = prStringTrim(text);
    return text.size > 0 && mermaidNumberParse(text, out) == text.size && *out >= 0 && *out <= 1;
}

// name[:::class]: [x, y] styles
bool parsePoint(Parser& parser, PrStringView statement) {
    MermaidQuadrantChart& chart = *parser.chart;
    size_t open = prStringFindByte(statement, '[');
    PrStringView head = prStringTrim(mermaidBefore(statement, open));
    if (open >= statement.size || head.size == 0 || head.data[head.size - 1] != ':')
        return mermaidParseFail(parser, "unknown statement");
    head = prStringTrim(mermaidBefore(head, head.size - 1));
    size_t classAt = prStringFind(head, ":::");
    PrStringView name = plainText(mermaidBefore(head, classAt));
    PrStringView className = classAt < head.size ? prStringTrim(mermaidAfter(head, classAt + 3)) : PrStringView{};
    if (name.size == 0)
        return mermaidParseFail(parser, "a point has no name");
    PrStringView rest = mermaidAfter(statement, open + 1);
    size_t close = prStringFindByte(rest, ']');
    size_t comma = prStringFindByte(rest, ',');
    MermaidQuadrantPoint point{.label = mermaidTextAdd(chart.text, name)};
    if (close >= rest.size || comma >= close || !readCoordinate(mermaidBefore(rest, comma), &point.x) ||
            !readCoordinate(PrStringView{rest.data + comma + 1, close - comma - 1}, &point.y))
        return mermaidParseFail(parser, "a point's coordinates are not two numbers from 0 to 1");
    if (!readStyles(parser, mermaidAfter(rest, close + 1), point.style))
        return false;
    chart.points.push(point);
    parser.pointClasses.push(className);
    return true;
}

bool parseStatement(Parser& parser, PrStringView statement) {
    MermaidQuadrantChart& chart = *parser.chart;
    PrStringView word{};
    PrStringView rest{};
    mermaidSplitWord(statement, &word, &rest);
    if (prAsciiEqualsFold(word, "title")) {
        chart.title = mermaidTextAdd(chart.text, rest);
        return true;
    }
    if (prAsciiEqualsFold(word, "x-axis")) {
        readAxis(parser, rest, chart.xAxis);
        return true;
    }
    if (prAsciiEqualsFold(word, "y-axis")) {
        readAxis(parser, rest, chart.yAxis);
        return true;
    }
    if (word.size == 10 && prAsciiStartsWithFold(word, "quadrant-") && word.data[9] >= '1' && word.data[9] <= '4') {
        chart.quadrants[word.data[9] - '1'] = mermaidTextAdd(chart.text, plainText(rest));
        return true;
    }
    if (prAsciiEqualsFold(word, "classDef")) {
        PrStringView name{};
        PrStringView styles{};
        mermaidSplitWord(rest, &name, &styles);
        MermaidQuadrantStyle style{};
        if (!readStyles(parser, styles, style))
            return false;
        uint32_t found = mermaidNamesFind(parser.classNames, name);
        if (found == kMermaidNone) {
            mermaidNamesSet(parser.classNames, name, parser.classes.count);
            parser.classes.push(style);
        } else {
            parser.classes.data[found] = style;
        }
        return true;
    }
    return parsePoint(parser, statement);
}

// Gives each point the style of its class, under the point's own styles.
void resolveStyles(Parser& parser) {
    MermaidQuadrantChart& chart = *parser.chart;
    for (uint32_t p = 0; p < chart.points.count; ++p) {
        PrStringView name = parser.pointClasses.data[p];
        uint32_t found = name.size > 0 ? mermaidNamesFind(parser.classNames, name) : kMermaidNone;
        if (found == kMermaidNone)
            continue;
        MermaidQuadrantStyle& own = chart.points.data[p].style;
        MermaidQuadrantStyle merged = parser.classes.data[found];
        if ((own.flags & MermaidQuadrantStyleFlag_Radius) != 0)
            merged.radius = own.radius;
        if ((own.flags & MermaidQuadrantStyleFlag_Color) != 0)
            merged.color = own.color;
        if ((own.flags & MermaidQuadrantStyleFlag_StrokeColor) != 0)
            merged.strokeColor = own.strokeColor;
        if ((own.flags & MermaidQuadrantStyleFlag_StrokeWidth) != 0)
            merged.strokeWidth = own.strokeWidth;
        merged.flags |= own.flags;
        own = merged;
    }
}

MermaidInk inkOf(Colors colors) {
    return mermaidInkOfColors(colors.light, colors.dark);
}

// Records `text` as mermaid.js's quadrant renderer places it: centered on `at` or starting there, with its top or its
// middle at `at`, and turned a quarter counterclockwise if `turned`.
void drawText(MermaidRender& render, PrStringView text, float size, PrVec2 at, bool centered, bool middle, bool turned,
        MermaidInk ink) {
    if (text.size == 0)
        return;
    MermaidTextStyle style = mermaidTextStyle(size);
    style.lineHeight = size;
    MermaidLabelIndex label = mermaidLabelCreate(render, text, MermaidTextFormat_Plain, style);
    PrVec2 extent = mermaidLabelExtent(render, label);
    PrVec2 offset{centered ? -extent.x * 0.5f : 0, middle ? -extent.y * 0.5f : 0};
    if (!turned) {
        mermaidLabelDraw(render, label, at + offset, ink);
        return;
    }
    mermaidPushTransform(render, prAffine2Concat(prAffine2Rotate(prF32DegreesToRadians(-90)), prAffine2Translate(at)));
    mermaidLabelDraw(render, label, offset, ink);
    mermaidPopTransform(render);
}

} // namespace

PrStatus mermaidQuadrantParse(MermaidQuadrantChart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    chart.title = MermaidSpan{};
    for (MermaidSpan& span : chart.quadrants)
        span = MermaidSpan{};
    chart.xAxis[0] = chart.xAxis[1] = MermaidSpan{};
    chart.yAxis[0] = chart.yAxis[1] = MermaidSpan{};
    chart.text.clear();
    chart.points.clear();
    Parser parser{};
    parser.error = error;
    parser.chart = &chart;
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = mermaidParseHeader(parser, preamble, PrMermaidKind_Quadrant, nullptr);
    chart.title = mermaidTextAdd(chart.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        ok = parseStatement(parser, line.content);
    }
    if (ok)
        resolveStyles(parser);
    parser.pointClasses.reset();
    mermaidNamesReset(parser.classNames);
    parser.classes.reset();
    return ok ? PrStatus_Ok : PrStatus_Syntax;
}

PrStringView mermaidQuadrantText(const MermaidQuadrantChart& chart, MermaidSpan span) {
    return mermaidTextAt(chart.text, span);
}

void mermaidQuadrantReset(MermaidQuadrantChart& chart) {
    chart.text.reset();
    chart.points.reset();
}

PrStatus mermaidQuadrantDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidQuadrantChart chart{};
    PrStatus status = mermaidQuadrantParse(chart, source, error);
    if (status != PrStatus_Ok) {
        mermaidQuadrantReset(chart);
        return status;
    }
    auto textOf = [&](MermaidSpan span) {
        return mermaidQuadrantText(chart, span);
    };
    render.extent = PrVec2{kChartSize, kChartSize};

    // The space of the title and the axes, and the quadrants in the rest. The x axis is at the top if the chart has no
    // points.
    bool hasPoints = chart.points.count > 0;
    bool hasX = chart.xAxis[0].size > 0 || chart.xAxis[1].size > 0;
    bool hasY = chart.yAxis[0].size > 0 || chart.yAxis[1].size > 0;
    float xSpace = hasX ? kAxisPadding * 2 + kAxisFontSize : 0;
    float ySpace = hasY ? kAxisPadding * 2 + kAxisFontSize : 0;
    float titleSpace = chart.title.size > 0 ? kTitleFontSize + kTitlePadding * 2 : 0;
    float xTop = hasPoints ? 0 : xSpace;
    float xBottom = hasPoints ? xSpace : 0;
    float left = kQuadrantPadding + ySpace;
    float top = kQuadrantPadding + xTop + titleSpace;
    float width = kChartSize - kQuadrantPadding * 2 - ySpace;
    float height = kChartSize - kQuadrantPadding * 2 - xTop - xBottom - titleSpace;
    float halfWidth = width * 0.5f;
    float halfHeight = height * 0.5f;

    // The quadrants, each with its label.
    const PrVec2 corners[4] = {
        {left + halfWidth, top},
        {left, top},
        {left, top + halfHeight},
        {left + halfWidth, top + halfHeight},
    };
    for (uint32_t q = 0; q < 4; ++q) {
        PrRect rect{corners[q].x, corners[q].y, corners[q].x + halfWidth, corners[q].y + halfHeight};
        mermaidFillRect(render, rect, inkOf(kQuadrantFills[q]));
        float y = hasPoints ? rect.top + kQuadrantTextPadding : rect.top + halfHeight * 0.5f;
        drawText(render, textOf(chart.quadrants[q]), kQuadrantFontSize, PrVec2{rect.left + halfWidth * 0.5f, y}, true,
            !hasPoints, false, inkOf(kQuadrantTexts[q]));
    }

    // The border, and the lines between the quadrants.
    MermaidInk border = inkOf(kBorder);
    float a = kExternalBorder * 0.5f;
    float right = left + width;
    float bottom = top + height;
    mermaidDrawLine(render, PrVec2{left - a, top}, PrVec2{right + a, top}, border, kExternalBorder, nullptr);
    mermaidDrawLine(render, PrVec2{right, top + a}, PrVec2{right, bottom - a}, border, kExternalBorder, nullptr);
    mermaidDrawLine(render, PrVec2{left - a, bottom}, PrVec2{right + a, bottom}, border, kExternalBorder, nullptr);
    mermaidDrawLine(render, PrVec2{left, top + a}, PrVec2{left, bottom - a}, border, kExternalBorder, nullptr);
    mermaidDrawLine(render, PrVec2{left + halfWidth, top + a}, PrVec2{left + halfWidth, bottom - a}, border,
        kInternalBorder, nullptr);
    mermaidDrawLine(render, PrVec2{left + a, top + halfHeight}, PrVec2{right - a, top + halfHeight}, border,
        kInternalBorder, nullptr);

    // The points, the last first as mermaid.js draws them, each with its label below it. A point without a color is
    // drawn in the text's color: mermaid.js's theme gives it no valid color, and browsers draw it black.
    MermaidInk text = inkOf(kText);
    PrPath* path = prPathCreate();
    for (uint32_t i = chart.points.count; i-- > 0;) {
        const MermaidQuadrantPoint& point = chart.points.data[i];
        const MermaidQuadrantStyle& style = point.style;
        PrVec2 center{left + float(point.x) * width, bottom - float(point.y) * height};
        float radius = (style.flags & MermaidQuadrantStyleFlag_Radius) != 0 ? style.radius : kPointRadius;
        bool stroked = (style.flags & MermaidQuadrantStyleFlag_StrokeColor) != 0 && style.strokeWidth > 0;
        MermaidPaint paint{
            .flags = MermaidPaintFlags(MermaidPaintFlag_Fill | (stroked ? MermaidPaintFlag_Stroke : 0) |
                MermaidPaintFlag_StrokeWidth),
            .fill = (style.flags & MermaidQuadrantStyleFlag_Color) != 0 ? mermaidInkOfColor(style.color) : text,
            .stroke = mermaidInkOfColor(style.strokeColor),
            .strokeWidth = style.strokeWidth,
        };
        prPathClear(path);
        prPathAddEllipse(path, center, PrVec2{radius, radius}, 0);
        mermaidDrawPath(render, path, paint);
        drawText(render, textOf(point.label), kPointFontSize, center + PrVec2{0, kPointTextPadding}, true, false, false, text);
        PrRect rect{center.x - radius, center.y - radius, center.x + radius, center.y + radius};
        mermaidItemAdd(render, PrMermaidItemKind_Node, rect, textOf(point.label), PrStringView{});
    }
    prPathDestroy(path);

    // The axes' labels. An axis with two labels centers each on its half, and one with one label starts it at the
    // axis's start.
    bool xBoth = chart.xAxis[1].size > 0;
    float xLabelY = hasPoints ? kAxisPadding + bottom + kQuadrantPadding : kAxisPadding + titleSpace;
    for (uint32_t side = 0; side < 2; ++side) {
        float x = left + float(side) * halfWidth + (xBoth ? halfWidth * 0.5f : 0);
        drawText(render, textOf(chart.xAxis[side]), kAxisFontSize, PrVec2{x, xLabelY}, xBoth, false, false, text);
    }
    bool yBoth = chart.yAxis[1].size > 0;
    for (uint32_t side = 0; side < 2; ++side) {
        float y = (side == 0 ? bottom : top + halfHeight) - (yBoth ? halfHeight * 0.5f : 0);
        drawText(render, textOf(chart.yAxis[side]), kAxisFontSize, PrVec2{kAxisPadding, y}, yBoth, false, true, text);
    }

    drawText(render, textOf(chart.title), kTitleFontSize, PrVec2{kChartSize * 0.5f, kTitlePadding}, true, false, false, text);
    mermaidQuadrantReset(chart);
    return PrStatus_Ok;
}

} // namespace pr_impl
