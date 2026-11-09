// Pie charts (mermaid_pie.h). The slices go clockwise from the top in source order, each in the theme's next pie color,
// with its percentage inside it and a legend to the right.

#include "mermaid/src/mermaid_pie.h"

#include "pr/float.h"
#include "pr/float_convert.h"
#include "pr/path.h"
#include "pr/string_ops.h"

#include "mermaid/src/mermaid_parse.h"

namespace pr_impl {

namespace {

// mermaid.js's pie measurements, in px.
constexpr float kHeight = 450;
constexpr float kRadius = 185;
constexpr float kOuterRadius = 186;
constexpr float kTextPosition = 0.75f;      // where a slice's percentage sits, by the radius
constexpr float kSliceFontSize = 17;
constexpr float kLegendFontSize = 17;
constexpr float kTitleFontSize = 25;
constexpr float kLegendRect = 18;
constexpr float kLegendSpacing = 4;
constexpr float kLegendX = 216;             // from the center
constexpr float kMargin = 40;               // between the pie and its legend's text, in the chart's width
constexpr float kSliceOpacity = 0.7f;
constexpr uint32_t kColorCount = 12;

struct Parser : MermaidParseState {
    MermaidPie* pie;
};

// Reads the words after `pie` on its line: showData, and a title.
void readHeader(MermaidPie& pie, PrStringView rest) {
    rest = prStringTrim(rest);
    if (prStringStartsWith(rest, "showData")) {
        pie.showData = true;
        rest = prStringTrim(mermaidAfter(rest, 8));
    }
    if (prStringStartsWith(rest, "title"))
        pie.title = mermaidTextAdd(pie.text, prStringTrim(mermaidAfter(rest, 5)));
}

// "label" : value
bool parseSlice(Parser& parser, PrStringView content) {
    MermaidPie& pie = *parser.pie;
    if (content.data[0] != '"')
        return mermaidParseFail(parser, "unknown statement");
    size_t close = prStringFindByte(mermaidAfter(content, 1), '"') + 1;
    if (close >= content.size)
        return mermaidParseFail(parser, "a slice's label has no closing quote");
    PrStringView rest = prStringTrim(mermaidAfter(content, close + 1));
    if (rest.size == 0 || rest.data[0] != ':')
        return mermaidParseFail(parser, "a slice has no value");
    PrStringView number = prStringTrim(mermaidAfter(rest, 1));
    double value = 0;
    if (number.size == 0 || mermaidNumberParse(number, &value) != number.size || value < 0)
        return mermaidParseFail(parser, "a slice's value is not a positive number");
    pie.slices.push(MermaidSlice{
        .label = mermaidTextAdd(pie.text, PrStringView{content.data + 1, close - 1}),
        .valueText = mermaidTextAdd(pie.text, number),
        .value = value,
    });
    return true;
}

} // namespace

PrStatus mermaidPieParse(MermaidPie& pie, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    pie.title = MermaidSpan{};
    pie.showData = false;
    pie.text.clear();
    pie.slices.clear();
    Parser parser{};
    parser.error = error;
    parser.pie = &pie;
    MermaidPreamble preamble = mermaidPreambleRead(source);
    PrStringView rest{};
    if (!mermaidParseHeader(parser, preamble, PrMermaidKind_Pie, &rest))
        return PrStatus_Syntax;
    pie.title = mermaidTextAdd(pie.text, preamble.title);
    readHeader(pie, rest);
    MermaidLines lines = mermaidLinesOfBody(source, preamble);
    MermaidLine line{};
    while (mermaidLinesNext(lines, &line)) {
        parser.line = line.number;
        parser.column = line.column;
        PrStringView content = line.content;
        if (prStringStartsWith(content, "title"))
            pie.title = mermaidTextAdd(pie.text, prStringTrim(mermaidAfter(content, 5)));
        else if (prStringStartsWith(content, "showData"))
            pie.showData = true;
        else if (!parseSlice(parser, content))
            return PrStatus_Syntax;
    }
    return PrStatus_Ok;
}

PrStringView mermaidPieText(const MermaidPie& pie, MermaidSpan span) {
    return mermaidTextAt(pie.text, span);
}

void mermaidPieReset(MermaidPie& pie) {
    pie.text.reset();
    pie.slices.reset();
}

PrStatus mermaidPieDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error) {
    MermaidPie pie{};
    PrStatus status = mermaidPieParse(pie, source, error);
    if (status != PrStatus_Ok) {
        mermaidPieReset(pie);
        return status;
    }
    double sum = 0;
    for (const MermaidSlice& slice : pie.slices.span())
        sum += slice.value;

    // The legend's labels, whose widest sets the chart's width.
    RawArray<MermaidLabelIndex> legend{};
    RawArray<char> text{};
    float widest = 0;
    for (const MermaidSlice& slice : pie.slices.span()) {
        text.clear();
        PrStringView name = mermaidPieText(pie, slice.label);
        text.append(name.data, uint32_t(name.size));
        if (pie.showData) {
            PrStringView value = mermaidPieText(pie, slice.valueText);
            text.append(" [", 2);
            text.append(value.data, uint32_t(value.size));
            text.push(']');
        }
        MermaidLabelIndex label =
            mermaidLabelCreate(render, PrStringView{text.data, text.count}, MermaidTextFormat_Plain,
                mermaidTextStyle(kLegendFontSize));
        legend.push(label);
        widest = prF32Max(widest, mermaidLabelExtent(render, label).x);
    }
    render.extent = PrVec2{kHeight + kMargin + kLegendRect + kLegendSpacing + widest, kHeight};
    PrVec2 center{kHeight * 0.5f, kHeight * 0.5f};
    MermaidInk black = mermaidInk(MermaidRole_StrongText);

    // The slices, from the top, clockwise.
    double angle = -kPrF64Pi * 0.5;
    for (uint32_t i = 0; i < pie.slices.count && sum > 0; ++i) {
        double sweep = pie.slices.data[i].value / sum * 2 * kPrF64Pi;
        MermaidInk ink = mermaidInk(MermaidRole(MermaidRole_Pie1 + i % kColorCount));
        PrPath* path = prPathCreate();
        prPathMoveTo(path, center);
        prPathEllipticArcTo(path, center, PrVec2{kRadius, kRadius}, 0, float(angle), float(sweep));
        prPathClose(path);
        MermaidPaint paint{
            .flags = MermaidPaintFlag_Fill | MermaidPaintFlag_Stroke | MermaidPaintFlag_StrokeWidth |
                MermaidPaintFlag_Opacity,
            .fill = ink,
            .stroke = black,
            .strokeWidth = 2,
            .opacity = kSliceOpacity,
        };
        mermaidDrawPath(render, path, paint);
        prPathDestroy(path);
        angle += sweep;
    }
    PrPath* outline = prPathCreate();
    prPathAddEllipse(outline, center, PrVec2{kOuterRadius, kOuterRadius}, 0);
    mermaidStrokePath(render, outline, black, 2, nullptr);
    prPathDestroy(outline);

    // Each slice's percentage, at the middle of its arc.
    angle = -kPrF64Pi * 0.5;
    for (uint32_t i = 0; i < pie.slices.count && sum > 0; ++i) {
        double share = pie.slices.data[i].value / sum;
        double middle = angle + share * kPrF64Pi;
        angle += share * 2 * kPrF64Pi;
        if (share <= 0)
            continue;
        text.clear();
        char digits[kPrF64TextMaxBytes];
        size_t written = prF64Format(prF64Round(share * 100), digits);
        text.append(digits, uint32_t(written));
        text.push('%');
        MermaidLabelIndex label =
            mermaidLabelCreate(render, PrStringView{text.data, text.count}, MermaidTextFormat_Plain,
                mermaidTextStyle(kSliceFontSize));
        PrVec2 at{
            center.x + float(prF64Cos(middle)) * kRadius * kTextPosition,
            center.y + float(prF64Sin(middle)) * kRadius * kTextPosition,
        };
        mermaidLabelDrawCentered(render, label, at, mermaidInk(MermaidRole_Text));
    }

    // The legend: a swatch and a label for each slice, centered on the pie's middle.
    float legendTop = center.y - float(pie.slices.count) * (kLegendRect + kLegendSpacing) * 0.5f;
    for (uint32_t i = 0; i < pie.slices.count; ++i) {
        float top = legendTop + float(i) * (kLegendRect + kLegendSpacing);
        PrRect swatch{center.x + kLegendX, top, center.x + kLegendX + kLegendRect, top + kLegendRect};
        mermaidFillRect(render, swatch, mermaidInk(MermaidRole(MermaidRole_Pie1 + i % kColorCount)));
        PrVec2 extent = mermaidLabelExtent(render, legend.data[i]);
        mermaidLabelDraw(render, legend.data[i],
            PrVec2{swatch.right + kLegendSpacing, top + kLegendRect * 0.5f - extent.y * 0.5f}, black);
    }

    if (pie.title.size > 0) {
        MermaidLabelIndex title = mermaidLabelCreate(render, mermaidPieText(pie, pie.title), MermaidTextFormat_Plain,
            mermaidTextStyle(kTitleFontSize));
        PrVec2 extent = mermaidLabelExtent(render, title);
        mermaidLabelDraw(render, title, PrVec2{center.x - extent.x * 0.5f, center.y - 200 - extent.y * 0.75f}, black);
    }
    legend.reset();
    text.reset();
    mermaidPieReset(pie);
    return PrStatus_Ok;
}

} // namespace pr_impl
