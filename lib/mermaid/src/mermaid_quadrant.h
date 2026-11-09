// Quadrant charts: the model of a Mermaid quadrant chart, the parser that builds it from the source of `quadrantChart`,
// and its drawing, laid out as mermaid.js lays it out.

#pragma once

#include "pr/color.h"
#include "pr/mermaid.h"
#include "pr/raw_array.h"

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The properties of a point's style that are set.
enum {
    MermaidQuadrantStyleFlag_Radius = 1u << 0,
    MermaidQuadrantStyleFlag_Color = 1u << 1,
    MermaidQuadrantStyleFlag_StrokeColor = 1u << 2,
    MermaidQuadrantStyleFlag_StrokeWidth = 1u << 3,
};
typedef uint8_t MermaidQuadrantStyleFlags;

// The style of a point, from its class and then its own styles. `flags` says which properties are set.
struct MermaidQuadrantStyle {
    MermaidQuadrantStyleFlags flags;
    float radius;                       // px
    PrColor8 color;                     // the fill
    PrColor8 strokeColor;
    float strokeWidth;                  // px
};

// A point, in source order. Its coordinates run from 0 to 1, from the left and from the bottom.
struct MermaidQuadrantPoint {
    MermaidSpan label;
    double x;
    double y;
    MermaidQuadrantStyle style;
};

// A quadrant chart, as its source describes it. Every text may be empty.
struct MermaidQuadrantChart {
    MermaidSpan title;
    MermaidSpan quadrants[4];           // quadrant-1 to quadrant-4: top right, top left, bottom left, bottom right
    MermaidSpan xAxis[2];               // the left text, then the right one
    MermaidSpan yAxis[2];               // the bottom text, then the top one
    RawArray<char> text;                // the characters of every span
    RawArray<MermaidQuadrantPoint> points;
};

// Parses the source of a quadrant chart into `chart`, emptying it first. If the source does not parse, returns Syntax
// and fills a non-null `error`.
PrStatus mermaidQuadrantParse(MermaidQuadrantChart& chart, PrStringView source, PrMermaidError* PR_NULLABLE error);

// The text of a span of the chart.
PrStringView mermaidQuadrantText(const MermaidQuadrantChart& chart, MermaidSpan span);

// Frees the chart's storage.
void mermaidQuadrantReset(MermaidQuadrantChart& chart);

// Parses the quadrant chart `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null
// `error` if the source does not parse.
PrStatus mermaidQuadrantDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
