// Flowcharts as drawn: a parsed flowchart laid out with lib/graph and recorded into a render.

#pragma once

#include "mermaid/src/mermaid_flowchart.h"
#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses the flowchart `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null `error`
// if the source does not parse.
PrStatus mermaidFlowchartDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
