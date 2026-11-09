// Mermaid diagrams as viewer sources (documents.h): loading and drawing. Only the switches in documents.cpp call these
// functions. A loader returns null for a file that it cannot read or parse.

#pragma once

#include "pr/draw_list.h"
#include "pr/font.h"
#include "pr/text.h"
#include "pr/vec.h"

#include <filesystem>
#include <memory>
#include <string>

PR_ASSUME_NONNULL_BEGIN

namespace viewer {

// A Mermaid diagram. Its text uses the system's fonts, hinted as `hinting` says and shaped by `shaper`. drawMermaid
// fits it to a viewport of `viewport` CSS pixels and centers it over its background, in `scheme` (fittedIn).
// mermaidPageUrl returns the page that draws the same diagram with mermaid.js in the browser, fitted the same way, in
// the theme of the scheme that the browser prefers.
struct MermaidSource;
std::shared_ptr<MermaidSource> loadMermaid(const std::filesystem::path& path, PrTextShaper* shaper,
    PrFontHinting hinting, PrFontHinter* PR_NULLABLE hinter);
void drawMermaid(MermaidSource& diagram, PrDrawList* list, PrVec2 viewport, PrColorScheme scheme);
std::string mermaidPageUrl(const std::filesystem::path& diagram);

} // namespace viewer

PR_ASSUME_NONNULL_END
