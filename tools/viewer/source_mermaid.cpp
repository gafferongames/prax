// Mermaid diagrams as sources (source_mermaid.h).

#include "tools/viewer/source_mermaid.h"

#include "pr/font_bundle.h"
#include "pr/font_set.h"
#include "pr/geom.h"
#include "pr/mermaid.h"

#include "tools/viewer/documents.h"

#include <cstdio>

using namespace pr;

namespace viewer {

namespace fs = std::filesystem;

namespace {

constexpr const char* kThirdParty = PR_THIRD_PARTY_DIR;

// Returns the system's fonts, which diagrams use for their text, as a browser does, with the generic families bound as
// the viewer's pages bind them (prFontBundleInstall). The set is created when the first diagram loads, and reused after
// that.
PrFontSet* systemFonts() {
    static PrFontSet* fonts = nullptr;
    if (fonts == nullptr) {
        fonts = prFontSetCreate(nullptr);
        prFontSetSetSystem(fonts, prFontSystemCreate(PrFontSystemBackend_Default));
        PrFontBundle bundle = PrFontBundle_Standard14;
        prFontBundleInstall(fonts, &bundle, 1, PR_FONT_BUNDLE_DIR);
    }
    return fonts;
}

} // namespace

struct MermaidSource {
    PrMermaid* PR_NULLABLE diagram = nullptr;

    // A font set over the system's fonts, hinted as the source asks. It outlives the diagram.
    PrFontSet* PR_NULLABLE fonts = nullptr;

    MermaidSource() = default;
    MermaidSource(const MermaidSource&) = delete;
    MermaidSource& operator=(const MermaidSource&) = delete;
    ~MermaidSource() {
        prMermaidDestroy(diagram);
        prFontSetDestroy(fonts);
    }
};

std::shared_ptr<MermaidSource> loadMermaid(const fs::path& path, PrTextShaper* shaper, PrFontHinting hinting,
        PrFontHinter* PR_NULLABLE hinter) {
    std::string text = readFile(path);
    std::shared_ptr<MermaidSource> source = std::make_shared<MermaidSource>();
    source->fonts = prFontSetCreate(systemFonts());
    prFontSetSetHinting(source->fonts, hinting, hinter);
    PrMermaidDesc desc{.source = text, .fonts = source->fonts, .shaper = shaper};
    PrMermaidError error{};
    PrStatus status = prMermaidCreate(&desc, nullptr, &source->diagram, &error);
    // A source that does not draw shows why, as mermaid.js shows it.
    if (status != PrStatus_Ok) {
        std::fprintf(stderr, "%s:%u:%u: %.*s\n", path.string().c_str(), error.line, error.column,
            int(error.message.size), error.message.data);
        source->diagram = prMermaidCreateError(&desc, nullptr, &error);
    }
    return source;
}

void drawMermaid(MermaidSource& diagram, PrDrawList* list, PrVec2 viewport, PrColorScheme scheme) {
    prDrawListRect(list, PrRect{0, 0, viewport.x, viewport.y}, {.color = prMermaidBackground(diagram.diagram, scheme)});
    PrVec2 extent = prMermaidExtent(diagram.diagram);
    PrRect fitted = fittedIn(extent, viewport);
    float scale = extent.x > 0 ? prRectWidth(fitted) / extent.x : 1;
    PrAffine2 placed = prAffine2Concat(prAffine2Scale(PrVec2{scale, scale}),
        prAffine2Translate(PrVec2{fitted.left, fitted.top}));
    prDrawListPushTransformAffine2(list, placed);
    prMermaidDraw(diagram.diagram, list, PrVec2{0, 0}, scheme);
    prDrawListPopTransform(list);
}

std::string mermaidPageUrl(const fs::path& diagram) {
    fs::path page = fs::path(kThirdParty).parent_path() / "tools" / "suite" / "mermaid" / "mermaid.html";
    return fileUrl(page) + "?src=" + queryEscaped(fileUrl(diagram));
}

} // namespace viewer
