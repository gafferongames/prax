// XY charts (mermaid_xychart.h). The layout is mermaid.js's: a chart of a fixed size, whose plot takes what the title
// and the axes leave, with the series drawn over it in source order and then the axes.

#include "mermaid/src/mermaid_xychart.h"

#include "pr/ascii.h"
#include "pr/count_of.h"
#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/geom.h"
#include "pr/min_max.h"
#include "pr/path.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's xyChart configuration, in px.
constexpr float kWidth = 700;
constexpr float kHeight = 500;
constexpr float kPlotReserved = 0.5f;       // the share of each dimension that the plot keeps before the axes
constexpr float kTitleFontSize = 20;
constexpr float kTitlePadding = 10;
constexpr float kLabelFontSize = 14;
constexpr float kLabelPadding = 5;
constexpr float kAxisTitleFontSize = 16;
constexpr float kAxisTitlePadding = 5;
constexpr float kTickLength = 5;
constexpr float kTickWidth = 2;
constexpr float kAxisLineWidth = 2;
constexpr float kLineWidth = 2;
constexpr float kBarShare = 0.95f;          // of the space that a bar may take
constexpr float kBarSpace = 0.7f;           // of the distance between categories, which bars widen the padding to
constexpr double kTickCount = 10;           // the number of ticks that a linear axis aims for

// The theme's primaryTextColor, which the title and the axes are drawn in.
constexpr PrColor8 kText{0x13, 0x13, 0x00, 0xff};
constexpr PrColor8 kDarkText{0xe0, 0xdf, 0xdf, 0xff};

// The themes' plotColorPalette, which the series take in turn.
constexpr PrColor8 kPalette[] = {
    {0xec, 0xec, 0xff, 0xff}, {0x84, 0x93, 0xa6, 0xff}, {0xff, 0xc3, 0xa0, 0xff}, {0xdc, 0xdd, 0xe1, 0xff},
    {0xb8, 0xe9, 0x94, 0xff}, {0xd1, 0xa3, 0x6f, 0xff}, {0xc3, 0xcd, 0xe6, 0xff}, {0xff, 0xb6, 0xc1, 0xff},
    {0x49, 0x60, 0x78, 0xff}, {0xf8, 0xf3, 0xe3, 0xff},
};
constexpr PrColor8 kDarkPalette[] = {
    {0x34, 0x98, 0xdb, 0xff}, {0x2e, 0xcc, 0x71, 0xff}, {0xe7, 0x4c, 0x3c, 0xff}, {0xf1, 0xc4, 0x0f, 0xff},
    {0xbd, 0xc3, 0xc7, 0xff}, {0xff, 0xff, 0xff, 0xff}, {0x34, 0x49, 0x5e, 0xff}, {0x9b, 0x59, 0xb6, 0xff},
    {0x1a, 0xbc, 0x9c, 0xff}, {0xe6, 0x7e, 0x22, 0xff},
};
static_assert(countOf(kPalette) == countOf(kDarkPalette), "each theme's palette has a color for each series");

// ---------------------------------------------------------------------------------------------------------------------
// Parsing

// Returns true if `text` starts with a range, `min --> max`.
bool startsRange(PrStringView text) {
    double value = 0;
    size_t length = mermaidNumberParse(text, &value);
    return length > 0 && prStringStartsWith(prStringTrim(mermaidAfter(text, length)), "-->");
}

// Reads the text at the start of `rest`: a quoted string, or the words up to a list or a range. Advances `rest` past
// it. Returns false if a quote is not closed.
bool readText(PrStringView& rest, PrStringView& text) {
    rest = prStringTrim(rest);
    if (rest.size > 0 && rest.data[0] == '"') {
        size_t close = 1;
        while (close < rest.size && rest.data[close] != '"')
            ++close;
        if (close >= rest.size)
            return false;
        text = PrStringView{rest.data + 1, close - 1};
        rest = prStringTrim(mermaidAfter(rest, close + 1));
        return true;
    }
    size_t end = 0;
    while (end < rest.size && rest.data[end] != '[' &&
            !((end == 0 || prAsciiIsSpace(rest.data[end - 1])) && startsRange(mermaidAfter(rest, end))))
        ++end;
    text = prStringTrim(PrStringView{rest.data, end});
    rest = mermaidAfter(rest, end);
    return true;
}

// Reads a range, `min --> max`, which must be all of `text`.
bool readRange(PrStringView text, double& min, double& max) {
    size_t length = mermaidNumberParse(text, &min);
    if (length == 0)
        return false;
    text = prStringTrim(mermaidAfter(text, length));
    if (!prStringStartsWith(text, "-->"))
        return false;
    text = prStringTrim(mermaidAfter(text, 3));
    length = mermaidNumberParse(text, &max);
    return length > 0 && length == text.size;
}

// Reads a list, `[a, "b c", d]`, which must be all of `text`, and calls `item` with each of its items, unquoted.
// Returns false if the list is malformed or `item` returns false.
template <typename Fn>
bool readList(PrStringView text, Fn item) {
    if (text.size < 2 || text.data[0] != '[' || text.data[text.size - 1] != ']')
        return false;
    PrStringView rest = prStringTrim(PrStringView{text.data + 1, text.size - 2});
    while (rest.size > 0) {
        PrStringView value{};
        if (rest.data[0] == '"') {
            size_t close = 1;
            while (close < rest.size && rest.data[close] != '"')
                ++close;
            if (close >= rest.size)
                return false;
            value = PrStringView{rest.data + 1, close - 1};
            rest = prStringTrim(mermaidAfter(rest, close + 1));
            if (rest.size > 0 && rest.data[0] != ',')
                return false;
        } else {
            size_t end = 0;
            while (end < rest.size && rest.data[end] != ',')
                ++end;
            value = prStringTrim(PrStringView{rest.data, end});
            rest = mermaidAfter(rest, end);
        }
        if (!item(value))
            return false;
        if (rest.size > 0)
            rest = prStringTrim(mermaidAfter(rest, 1));
    }
    return true;
}

// Reads the values of a series into `chart`. Returns false if one is not a number, or there are none.
bool readValues(MermaidXyChart& chart, PrStringView text, MermaidXySeries& series) {
    series.valueStart = chart.values.count;
    bool ok = readList(text, [&](PrStringView value) {
        double number = 0;
        if (value.size == 0 || mermaidNumberParse(value, &number) != value.size)
            return false;
        chart.values.push(number);
        return true;
    });
    series.valueCount = chart.values.count - series.valueStart;
    return ok && series.valueCount > 0;
}

// Reads an axis statement, after its keyword. Only the x-axis may have categories.
const char* PR_NULLABLE readAxis(MermaidXyChart& chart, MermaidXyAxis& axis, PrStringView rest, bool allowBand) {
    PrStringView title{};
    if (!readText(rest, title))
        return "an axis's title has no closing quote";
    if (title.size > 0)
        axis.title = mermaidTextAdd(chart.text, title);
    rest = prStringTrim(rest);
    if (rest.size == 0)
        return nullptr;
    if (rest.data[0] == '[') {
        if (!allowBand)
            return "only the x-axis has categories";
        axis.band = true;
        axis.hasRange = false;
        axis.categoryStart = chart.categories.count;
        bool ok = readList(rest, [&](PrStringView value) {
            chart.categories.push(mermaidTextAdd(chart.text, value));
            return true;
        });
        axis.categoryCount = chart.categories.count - axis.categoryStart;
        return ok ? nullptr : "an axis's categories are not a list";
    }
    double min = 0;
    double max = 0;
    if (!readRange(rest, min, max))
        return "an axis's range is not `min --> max`";
    axis.band = false;
    axis.hasRange = true;
    axis.min = min;
    axis.max = max;
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------------
// Layout

// d3's choice of ticks for a linear axis: the integers from `first` to `last`, times `increment`, or divided by its
// negation when it is negative.
struct TickSpec {
    double first;
    double last;
    double increment;
};

// JavaScript's Math.round, which rounds halves up.
double jsRound(double value) {
    return prF64Floor(value + 0.5);
}

// d3's tickSpec: a step of 1, 2 or 5 times a power of ten that gives about `count` ticks from `start` to `stop`.
TickSpec tickSpec(double start, double stop, double count) {
    double step = (stop - start) / prF64Max(0, count);
    double power = prF64Floor(prF64Log10(step));
    double error = step / prF64Pow(10, power);
    double factor = 1;
    if (error >= prF64Sqrt(50))
        factor = 10;
    else if (error >= prF64Sqrt(10))
        factor = 5;
    else if (error >= prF64Sqrt(2))
        factor = 2;
    TickSpec spec{};
    if (power < 0) {
        double increment = prF64Pow(10, -power) / factor;
        spec.first = jsRound(start * increment);
        spec.last = jsRound(stop * increment);
        if (spec.first / increment < start)
            ++spec.first;
        if (spec.last / increment > stop)
            --spec.last;
        spec.increment = -increment;
    } else {
        double increment = prF64Pow(10, power) * factor;
        spec.first = jsRound(start / increment);
        spec.last = jsRound(stop / increment);
        if (spec.first * increment < start)
            ++spec.first;
        if (spec.last * increment > stop)
            --spec.last;
        spec.increment = increment;
    }
    if (spec.last < spec.first && 0.5 <= count && count < 2)
        return tickSpec(start, stop, count * 2);
    return spec;
}

// d3's ticks: about `count` round values between `start` and `stop`, in increasing order.
void linearTicks(double start, double stop, double count, RawArray<double>& out) {
    if (start == stop) {
        out.push(start);
        return;
    }
    TickSpec spec = stop < start ? tickSpec(stop, start, count) : tickSpec(start, stop, count);
    // A range too wide or too fine for doubles to count its ticks gives none, rather than ticks that never end or are
    // not finite.
    constexpr double kMaxTicks = 1000;
    constexpr double kMaxExact = 9007199254740992.0; // 2^53, past which adding one may not change a double
    if (!(spec.last >= spec.first) || !(spec.last - spec.first < kMaxTicks) || !(prF64Abs(spec.first) < kMaxExact) ||
            !(prF64Abs(spec.last) < kMaxExact) || !prF64IsFinite(spec.increment) || spec.increment == 0)
        return;
    uint32_t ticks = uint32_t(spec.last - spec.first) + 1;
    for (uint32_t k = 0; k < ticks; ++k) {
        double i = spec.first + k;
        double tick = spec.increment < 0 ? i / -spec.increment : i * spec.increment;
        if (prF64IsFinite(tick))
            out.push(tick);
    }
}

// Where an axis is drawn, beside the plot.
enum {
    AxisSide_Left,
    AxisSide_Bottom,
    AxisSide_Top,
};
typedef uint8_t AxisSide;

// An axis as mermaid.js lays it out: the space that it takes beside the plot, and the range of the plot's edge that its
// ticks spread over, less its outer padding at each end.
struct Axis {
    bool band;
    AxisSide side;
    double domain[2];                       // a linear axis's least and greatest values
    RawArray<double> ticks;                 // a linear axis's tick values
    RawArray<MermaidLabelIndex> labels;     // by tick
    MermaidLabelIndex title;                // or kMermaidNone
    PrVec2 labelExtent;                     // the largest of the labels
    PrVec2 titleExtent;
    float range[2];
    float outerPadding;
    PrRect box;
    bool showLine;
    bool showLabel;
    bool showTick;
    bool showTitle;
};

bool isVertical(const Axis& axis) {
    return axis.side == AxisSide_Left;
}

void axisSetRange(Axis& axis, float start, float end) {
    axis.range[0] = start;
    axis.range[1] = end;
    if (isVertical(axis))
        axis.box.bottom = axis.box.top + (end - start);
    else
        axis.box.right = axis.box.left + (end - start);
}

void axisSetOrigin(Axis& axis, PrVec2 origin) {
    axis.box = PrRect{
        origin.x, origin.y, origin.x + (axis.box.right - axis.box.left),
        origin.y + (axis.box.bottom - axis.box.top),
    };
}

// The distance between ticks, along the range without its outer padding.
float tickDistance(const Axis& axis) {
    float length = prF32Abs(axis.range[1] - axis.range[0] - 2 * axis.outerPadding);
    return axis.labels.count > 0 ? length / float(axis.labels.count) : length;
}

// The position along the axis of a band axis's category `index`. The categories spread evenly from one end of the range
// to the other, or sit at its middle if there is one.
float bandPosition(const Axis& axis, uint32_t index) {
    float start = axis.range[0] + axis.outerPadding;
    float end = axis.range[1] - axis.outerPadding;
    uint32_t count = axis.labels.count;
    float step = (end - start) / float(count > 1 ? count - 1 : 1);
    float first = start + (end - start - step * float(count > 0 ? count - 1 : 0)) * 0.5f;
    return first + step * float(index);
}

// The position along a linear axis of `value`. A vertical axis has its least value at the bottom.
float linearPosition(const Axis& axis, double value) {
    double start = double(axis.range[0] + axis.outerPadding);
    double end = double(axis.range[1] - axis.outerPadding);
    double from = axis.domain[0];
    double to = axis.domain[1];
    if (isVertical(axis)) {
        from = axis.domain[1];
        to = axis.domain[0];
    }
    double t = to != from ? (value - from) / (to - from) : 0.5;
    return float(start + t * (end - start));
}

// Takes from `space` what the axis needs: its line, labels, ticks and title, each if it fits in what remains.
void axisMeasure(Axis& axis, PrVec2 space) {
    bool vertical = isVertical(axis);
    float rest = vertical ? space.x : space.y;
    if (rest > kAxisLineWidth) {
        rest -= kAxisLineWidth;
        axis.showLine = true;
    }
    float labelSpace = 0;
    if (vertical) {
        axis.outerPadding = prF32Min(axis.labelExtent.y * 0.5f, 0.2f * space.y);
        labelSpace = axis.labelExtent.x + kLabelPadding * 2;
    } else {
        axis.outerPadding = prF32Min(axis.labelExtent.x * 0.5f, 0.2f * space.x);
        labelSpace = axis.labelExtent.y + kLabelPadding * 2;
    }
    if (labelSpace <= rest) {
        rest -= labelSpace;
        axis.showLabel = true;
    }
    if (rest >= kTickLength) {
        rest -= kTickLength;
        axis.showTick = true;
    }
    if (axis.title != kMermaidNone) {
        float titleSpace = axis.titleExtent.y + kAxisTitlePadding * 2;
        if (titleSpace <= rest) {
            rest -= titleSpace;
            axis.showTitle = true;
        }
    }
    if (vertical)
        axis.box = PrRect{0, 0, space.x - rest, space.y};
    else
        axis.box = PrRect{0, 0, space.x, space.y - rest};
}

// The position along the axis of tick `index`.
float tickPosition(const Axis& axis, uint32_t index) {
    return axis.band ? bandPosition(axis, index) : linearPosition(axis, axis.ticks.data[index]);
}

// Records a label with `at` at its left, center or right as `align` is 0, 0.5 or 1, and at its top, or at its middle if
// `middle`.
void drawText(MermaidRender& render, MermaidLabelIndex label, PrVec2 at, float align, bool middle, MermaidInk ink) {
    PrVec2 extent = mermaidLabelExtent(render, label);
    mermaidLabelDraw(render, label, PrVec2{at.x - extent.x * align, middle ? at.y - extent.y * 0.5f : at.y}, ink);
}

void axisDraw(MermaidRender& render, const Axis& axis, MermaidInk ink) {
    const PrRect& box = axis.box;
    float line = axis.showLine ? kAxisLineWidth : 0;
    float tick = axis.showTick ? kTickLength : 0;
    if (axis.side == AxisSide_Left) {
        float x = box.right - kAxisLineWidth * 0.5f;
        if (axis.showLine)
            mermaidDrawLine(render, PrVec2{x, box.top}, PrVec2{x, box.bottom}, ink, kAxisLineWidth, nullptr);
        for (uint32_t i = 0; i < axis.labels.count && axis.showLabel; ++i) {
            PrVec2 at{box.right - kLabelPadding - tick - line, tickPosition(axis, i)};
            drawText(render, axis.labels.data[i], at, 1, true, ink);
        }
        for (uint32_t i = 0; i < axis.labels.count && axis.showTick; ++i) {
            float y = tickPosition(axis, i);
            mermaidDrawLine(render, PrVec2{box.right - line, y}, PrVec2{box.right - line - kTickLength, y}, ink,
                kTickWidth, nullptr);
        }
        if (axis.showTitle) {
            // The title reads upward, with its top toward the left edge.
            PrAffine2 transform = prAffine2Concat(prAffine2Rotate(prF32DegreesToRadians(270)),
                prAffine2Translate(PrVec2{box.left + kAxisTitlePadding, (box.top + box.bottom) * 0.5f}));
            mermaidPushTransform(render, transform);
            drawText(render, axis.title, PrVec2{0, 0}, 0.5f, false, ink);
            mermaidPopTransform(render);
        }
        return;
    }
    bool bottom = axis.side == AxisSide_Bottom;
    float y = bottom ? box.top + kAxisLineWidth * 0.5f : box.bottom - kAxisLineWidth * 0.5f;
    if (axis.showLine)
        mermaidDrawLine(render, PrVec2{box.left, y}, PrVec2{box.right, y}, ink, kAxisLineWidth, nullptr);
    float titleSpace = axis.showTitle ? axis.titleExtent.y + kAxisTitlePadding * 2 : 0;
    float labelTop = bottom ? box.top + kLabelPadding + tick + line : box.top + titleSpace + kLabelPadding;
    for (uint32_t i = 0; i < axis.labels.count && axis.showLabel; ++i)
        drawText(render, axis.labels.data[i], PrVec2{tickPosition(axis, i), labelTop}, 0.5f, false, ink);
    float tickStart = bottom ? box.top + line : box.bottom - line;
    float tickEnd = bottom ? tickStart + kTickLength : tickStart - kTickLength;
    for (uint32_t i = 0; i < axis.labels.count && axis.showTick; ++i) {
        float x = tickPosition(axis, i);
        mermaidDrawLine(render, PrVec2{x, tickStart}, PrVec2{x, tickEnd}, ink, kTickWidth, nullptr);
    }
    if (axis.showTitle) {
        PrVec2 at{(box.left + box.right) * 0.5f, box.top + kAxisTitlePadding};
        if (bottom)
            at = PrVec2{(axis.range[0] + axis.range[1]) * 0.5f, box.bottom - kAxisTitlePadding - axis.titleExtent.y};
        drawText(render, axis.title, at, 0.5f, false, ink);
    }
}

// Sets up the labels of an axis of the chart.
void axisInit(MermaidRender& render, const MermaidXyChart& chart, const MermaidXyAxis& source, Axis& axis) {
    axis.band = source.band;
    axis.title = kMermaidNone;
    if (source.title.size > 0) {
        axis.title = mermaidLabelCreate(render, mermaidXyChartText(chart, source.title), MermaidTextFormat_Plain,
            mermaidTextStyle(kAxisTitleFontSize));
        axis.titleExtent = mermaidLabelExtent(render, axis.title);
    }
    MermaidTextStyle style = mermaidTextStyle(kLabelFontSize);
    if (source.band) {
        for (uint32_t i = 0; i < source.categoryCount; ++i) {
            MermaidSpan category = chart.categories.data[source.categoryStart + i];
            axis.labels.push(mermaidLabelCreate(render, mermaidXyChartText(chart, category), MermaidTextFormat_Plain, style));
        }
    } else {
        axis.domain[0] = source.min;
        axis.domain[1] = source.max;
        linearTicks(source.min, source.max, kTickCount, axis.ticks);
        for (double value : axis.ticks.span()) {
            char digits[kPrF64TextMaxBytes];
            size_t written = prF64Format(value, digits);
            axis.labels.push(mermaidLabelCreate(render, PrStringView{digits, written}, MermaidTextFormat_Plain, style));
        }
    }
    for (MermaidLabelIndex label : axis.labels.span()) {
        PrVec2 extent = mermaidLabelExtent(render, label);
        axis.labelExtent = PrVec2{prF32Max(axis.labelExtent.x, extent.x), prF32Max(axis.labelExtent.y, extent.y)};
    }
}

void axisReset(Axis& axis) {
    axis.ticks.reset();
    axis.labels.reset();
}

// The number of a series's values that the chart places: all of them, or one for each category of a band axis.
uint32_t placedCount(const MermaidXyChart& chart, const MermaidXySeries& series) {
    return chart.x.band ? min(series.valueCount, chart.x.categoryCount) : series.valueCount;
}

// The position along the x-axis of a series's value `index`. On a linear axis, the values spread evenly over its range.
float xPosition(const MermaidXyChart& chart, const Axis& x, const MermaidXySeries& series, uint32_t index) {
    if (x.band)
        return bandPosition(x, index);
    double value = chart.x.min;
    if (series.valueCount > 1)
        value += double(index) * (chart.x.max - chart.x.min) / double(series.valueCount - 1);
    return linearPosition(x, value);
}

// Reads a statement of the chart's body.
bool parseStatement(MermaidParseState& parser, MermaidXyChart& chart, PrStringView content) {
    if (mermaidStartsWithWord(content, "title")) {
        PrStringView rest = mermaidAfter(content, 5);
        PrStringView title = prStringTrim(rest);
        if (title.size > 0 && title.data[0] == '"' && !readText(rest, title))
            return mermaidParseFail(parser, "the title has no closing quote");
        chart.title = mermaidTextAdd(chart.text, title);
        return true;
    }
    if (mermaidStartsWithWord(content, "x-axis") || mermaidStartsWithWord(content, "y-axis")) {
        bool x = content.data[0] == 'x';
        const char* message = readAxis(chart, x ? chart.x : chart.y, mermaidAfter(content, 6), x);
        return message == nullptr || mermaidParseFail(parser, message);
    }
    if (mermaidStartsWithWord(content, "bar") || mermaidStartsWithWord(content, "line")) {
        bool isLine = content.data[0] == 'l';
        PrStringView rest = mermaidAfter(content, isLine ? 4 : 3);
        PrStringView title{};
        if (!readText(rest, title))
            return mermaidParseFail(parser, "a series's title has no closing quote");
        MermaidXySeries series{.title = mermaidTextAdd(chart.text, title), .line = isLine};
        if (!readValues(chart, prStringTrim(rest), series))
            return mermaidParseFail(parser, "a series's values are not a list of numbers");
        chart.series.push(series);
        return true;
    }
    return mermaidParseFail(parser, "unknown statement");
}

// Reads the header, `xychart` or `xychart-beta` with an optional orientation.
bool parseHeader(MermaidParseState& parser, MermaidXyChart& chart, const MermaidPreamble& preamble) {
    PrStringView orientation{};
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_XyChart, &orientation))
        return false;
    if (orientation == "horizontal")
        chart.horizontal = true;
    else if (orientation.size > 0 && orientation != "vertical")
        return mermaidParseFail(parser, "an XY chart's orientation is horizontal or vertical");
    return true;
}

} // namespace

PrStatus mermaidXyChartParse(MermaidXyChart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    chart.title = MermaidSpan{};
    chart.horizontal = false;
    chart.x = MermaidXyAxis{};
    chart.y = MermaidXyAxis{};
    chart.text.clear();
    chart.categories.clear();
    chart.values.clear();
    chart.series.clear();
    MermaidParseState parser{.error = error};
    MermaidPreamble preamble = mermaidPreambleRead(source);
    bool ok = parseHeader(parser, chart, preamble);
    chart.title = mermaidTextAdd(chart.text, preamble.title);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (ok && mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        ok = parseStatement(parser, chart, line.content);
    }
    if (ok && chart.series.count == 0) {
        parser.line = lines.number;
        parser.column = 0;
        ok = mermaidParseFail(parser, "an XY chart has no bar or line");
    }
    if (!ok)
        return PrStatus_Syntax;
    if (!chart.x.band && !chart.x.hasRange) {
        chart.x.hasRange = true;
        chart.x.min = 1;
        chart.x.max = double(chart.series.data[0].valueCount);
    }
    if (!chart.y.hasRange) {
        chart.y.min = kPrF64Infinity;
        chart.y.max = -kPrF64Infinity;
        for (const MermaidXySeries& series : chart.series.span()) {
            for (uint32_t i = 0; i < placedCount(chart, series); ++i) {
                chart.y.min = prF64Min(chart.y.min, chart.values.data[series.valueStart + i]);
                chart.y.max = prF64Max(chart.y.max, chart.values.data[series.valueStart + i]);
            }
        }
        if (chart.y.min > chart.y.max) {
            chart.y.min = 0;
            chart.y.max = 0;
        }
        chart.y.hasRange = true;
    }
    return PrStatus_Ok;
}

PrStringView mermaidXyChartText(const MermaidXyChart& chart, MermaidSpan span) {
    return mermaidTextAt(chart.text, span);
}

void mermaidXyChartReset(MermaidXyChart& chart) {
    chart.text.reset();
    chart.categories.reset();
    chart.values.reset();
    chart.series.reset();
}

PrStatus mermaidXyChartDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidXyChart chart{};
    PrStatus status = mermaidXyChartParse(chart, source, error);
    if (status != PrStatus_Ok) {
        mermaidXyChartReset(chart);
        return status;
    }
    render.extent = PrVec2{kWidth, kHeight};
    Axis x{};
    Axis y{};
    axisInit(render, chart, chart.x, x);
    axisInit(render, chart, chart.y, y);
    MermaidLabelIndex title = kMermaidNone;
    PrVec2 titleExtent{};
    if (chart.title.size > 0) {
        title = mermaidLabelCreate(render, mermaidXyChartText(chart, chart.title), MermaidTextFormat_Plain,
            mermaidTextStyle(kTitleFontSize));
        titleExtent = mermaidLabelExtent(render, title);
    }

    // mermaid.js's orchestration: the plot keeps half of each dimension, the title and then the axes take what they
    // need from the rest, and the plot takes what remains.
    PrVec2 plotExtent{prF32Floor(kWidth * kPlotReserved), prF32Floor(kHeight * kPlotReserved)};
    PrVec2 rest{kWidth - plotExtent.x, kHeight - plotExtent.y};
    float titleHeight = 0;
    if (title != kMermaidNone && titleExtent.y + kTitlePadding * 2 <= rest.y)
        titleHeight = titleExtent.y + kTitlePadding * 2;
    rest.y -= titleHeight;
    PrVec2 plotOrigin{};
    if (chart.horizontal) {
        x.side = AxisSide_Left;
        axisMeasure(x, rest);
        float xWidth = x.box.right - x.box.left;
        rest.x -= xWidth;
        y.side = AxisSide_Top;
        axisMeasure(y, rest);
        float yHeight = y.box.bottom - y.box.top;
        rest.y -= yHeight;
        plotExtent = plotExtent + PrVec2{prF32Max(rest.x, 0), prF32Max(rest.y, 0)};
        plotOrigin = PrVec2{xWidth, titleHeight + yHeight};
        axisSetOrigin(y, PrVec2{plotOrigin.x, titleHeight});
        axisSetRange(y, plotOrigin.x, plotOrigin.x + plotExtent.x);
        axisSetOrigin(x, PrVec2{0, plotOrigin.y});
        axisSetRange(x, plotOrigin.y, plotOrigin.y + plotExtent.y);
    } else {
        x.side = AxisSide_Bottom;
        axisMeasure(x, rest);
        rest.y -= x.box.bottom - x.box.top;
        y.side = AxisSide_Left;
        axisMeasure(y, rest);
        float yWidth = y.box.right - y.box.left;
        rest.x -= yWidth;
        plotExtent = plotExtent + PrVec2{prF32Max(rest.x, 0), prF32Max(rest.y, 0)};
        plotOrigin = PrVec2{yWidth, titleHeight};
        axisSetOrigin(x, PrVec2{plotOrigin.x, plotOrigin.y + plotExtent.y});
        axisSetRange(x, plotOrigin.x, plotOrigin.x + plotExtent.x);
        axisSetOrigin(y, PrVec2{0, plotOrigin.y});
        axisSetRange(y, plotOrigin.y, plotOrigin.y + plotExtent.y);
    }
    PrRect plot{plotOrigin.x, plotOrigin.y, plotOrigin.x + plotExtent.x, plotOrigin.y + plotExtent.y};

    // Bars widen the x-axis's outer padding so that the first and last bars fit inside the plot.
    bool anyBar = false;
    for (const MermaidXySeries& series : chart.series.span())
        anyBar = anyBar || !series.line;
    if (anyBar && kBarSpace * tickDistance(x) > x.outerPadding * 2)
        x.outerPadding = prF32Floor(kBarSpace * tickDistance(x) * 0.5f);

    MermaidInk text = mermaidInkOfColors(kText, kDarkText);
    if (title != kMermaidNone) {
        float half = prF32Min(titleExtent.x, kWidth) * 0.5f;
        float center = prF32Min(prF32Max((plot.left + plot.right) * 0.5f, half), kWidth - half);
        drawText(render, title, PrVec2{center, titleHeight * 0.5f}, 0.5f, true, text);
    }

    // The series, each in the next color of the palette.
    float barSize = prF32Min(x.outerPadding * 2, tickDistance(x)) * kBarShare;
    for (uint32_t s = 0; s < chart.series.count; ++s) {
        const MermaidXySeries& series = chart.series.data[s];
        MermaidInk ink = mermaidInkOfColors(kPalette[s % countOf(kPalette)], kDarkPalette[s % countOf(kPalette)]);
        uint32_t count = placedCount(chart, series);
        if (series.line) {
            PrPath* path = prPathCreate();
            for (uint32_t i = 0; i < count; ++i) {
                float along = xPosition(chart, x, series, i);
                float value = linearPosition(y, chart.values.data[series.valueStart + i]);
                PrVec2 point = chart.horizontal ? PrVec2{value, along} : PrVec2{along, value};
                if (i == 0)
                    prPathMoveTo(path, point);
                else
                    prPathLineTo(path, point);
            }
            mermaidStrokePath(render, path, ink, kLineWidth, nullptr);
            prPathDestroy(path);
            continue;
        }
        for (uint32_t i = 0; i < count; ++i) {
            float along = xPosition(chart, x, series, i);
            float value = linearPosition(y, chart.values.data[series.valueStart + i]);
            PrRect bar{along - barSize * 0.5f, value, along + barSize * 0.5f, plot.bottom};
            if (chart.horizontal)
                bar = PrRect{plot.left, along - barSize * 0.5f, value, along + barSize * 0.5f};
            if (bar.right <= bar.left || bar.bottom <= bar.top)
                continue;
            mermaidFillRect(render, bar, ink);
            mermaidItemAdd(render, PrMermaidItemKind_Node, bar, PrStringView{}, PrStringView{});
        }
    }
    axisDraw(render, x, text);
    axisDraw(render, y, text);

    axisReset(x);
    axisReset(y);
    mermaidXyChartReset(chart);
    return PrStatus_Ok;
}

} // namespace pr_impl
