// Sequence diagrams as drawn: a parsed sequence diagram laid out as mermaid.js lays it out, and recorded into a render.

#pragma once

#include "mermaid/src/mermaid_render.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Parses the sequence diagram `source`, lays it out and records it into `render`. Returns Syntax and fills a non-null
// `error` if the source does not parse.
PrStatus mermaidSequenceDraw(MermaidRender& render, PrStringView source, PrMermaidError* PR_NULLABLE error);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
