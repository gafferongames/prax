// Gantt charts as drawn: a parsed gantt chart laid out on a time axis as mermaid.js lays it out, and recorded into a
// render.

#pragma once

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses the gantt chart `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null
// `error` if the source does not parse.
PrStatus mermaidGanttDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
