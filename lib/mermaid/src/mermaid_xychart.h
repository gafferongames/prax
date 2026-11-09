// XY charts: the model of a Mermaid XY chart, the parser that builds it from the source of `xychart` or `xychart-beta`,
// and its drawing, laid out as mermaid.js lays it out.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// An axis of an XY chart. The x-axis is a band of categories or a linear range, and the y-axis is a linear range. An
// x-axis without categories or a range of its own spans the first series's values from 1, and a y-axis without a range
// spans the values of every series.
struct MermaidXyAxis {
    MermaidSpan title;
    bool band;                          // the axis places categories, in MermaidXyChart::categories
    bool hasRange;                      // `min` and `max` are set, by the source or from the x-axis's first series
    double min;
    double max;
    uint32_t categoryStart;
    uint32_t categoryCount;
};

// A bar or line series, in source order. Its values follow the x-axis's categories, or spread evenly over its range.
struct MermaidXySeries {
    MermaidSpan title;
    bool line;                          // drawn as a line through its values, not as bars
    uint32_t valueStart;                // in MermaidXyChart::values
    uint32_t valueCount;
};

// An XY chart, as its source describes it.
struct MermaidXyChart {
    MermaidSpan title;
    bool horizontal;                    // the x-axis runs down the left, and the y-axis along the top
    MermaidXyAxis x;
    MermaidXyAxis y;
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidSpan> categories;
    RawArray<double> values;
    RawArray<MermaidXySeries> series;
};

// Parses the source of an XY chart into `chart`, emptying it first. If the source does not parse, or has no series,
// returns Syntax and fills a non-null `error`.
PrStatus mermaidXyChartParse(MermaidXyChart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the chart.
PrStringView mermaidXyChartText(const MermaidXyChart& chart, MermaidSpan span);

// Frees the chart's storage.
void mermaidXyChartReset(MermaidXyChart& chart);

// Parses the XY chart `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error`
// if the source does not parse.
PrStatus mermaidXyChartDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
