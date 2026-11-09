// Mermaid diagrams as images: the painter of an image whose type is text/vnd.mermaid (HtmlImageKind_Diagram), which
// holds a diagram's source, as Markdown writes its diagrams (pr/markdown_html.h).

#pragma once

#include "pr/dom.h"
#include "pr/layout_style.h"
#include "pr/string_view.h"

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// Returns a painter of the diagram `source`, with a reference for the caller, and writes its natural size in px. The
// painter draws in the color scheme of the box that shows it. The diagram's text is set in the fonts of the
// presentation of `element`'s document. A source that does not draw gives the diagram that shows why. Returns a painter
// with no paint function if the document has no presentation.
PrLayoutPainter htmlMermaidPainter(const PrDomElement* element, PrStringView source, PrVec2* extent);

} // namespace pr_impl

PR_ASSUME_NONNULL_END
