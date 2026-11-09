// Pie charts: the model of a Mermaid pie chart, the parser that builds it from the source of `pie`, and its drawing,
// laid out as mermaid.js lays it out.

#pragma once

#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// A slice, in source order.
struct MermaidSlice {
    MermaidSpan label;
    MermaidSpan valueText;              // the value as the source writes it
    double value;
};

// A pie chart, as its source describes it.
struct MermaidPie {
    MermaidSpan title;
    bool showData;                      // the legend shows each slice's value
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidSlice> slices;
};

// Parses the source of a pie chart into `pie`, emptying it first. If the source does not parse, returns Syntax and
// fills a non-null `error`.
PrStatus mermaidPieParse(MermaidPie& pie, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the chart.
PrStringView mermaidPieText(const MermaidPie& pie, MermaidSpan span);

// Frees the chart's storage.
void mermaidPieReset(MermaidPie& pie);

// Parses the pie chart `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error`
// if the source does not parse.
PrStatus mermaidPieDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
