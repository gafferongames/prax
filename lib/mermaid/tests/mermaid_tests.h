// Mermaid unit tests
//
// The fixtures that the Mermaid unit tests share: the fonts that diagrams' text is set in, and a diagram drawn in them.

#pragma once

#include "pr/font.h"
#include "pr/font_set.h"
#include "pr/mermaid.h"
#include "pr/text.h"

#include "tools/harness/test.h"

#include <string>
#include <vector>

PR_ASSUME_NONNULL_BEGIN

namespace pr_impl {

// The fonts and the shaper of diagrams' text: Noto Sans, which every family falls back to.
struct MermaidTestFonts {
    PrFontSet* set = prFontSetCreate(nullptr);
    PrTextShaper* shaper = prTextShaperCreate(PrTextShaperBackend_Default);

    MermaidTestFonts() {
        std::string path = std::string(PR_FONTS_DIR) + "/NotoSans.ttf";
        const PrFont* PR_NULLABLE face = nullptr;
        REQUIRE(prFontCreateFromFile(path, 0, nullptr, &face) == PrStatus_Ok);
        PrFontFaceDescriptor descriptor = prFontFaceDescriptorDefault();
        prFontSetAdd(set, &descriptor, face);
        prFontRelease(face);
        prFontSetSetDefaultFamily(set, prFontFamilyFromName("Noto Sans"));
    }
    MermaidTestFonts(const MermaidTestFonts&) = delete;
    MermaidTestFonts& operator=(const MermaidTestFonts&) = delete;
    ~MermaidTestFonts() {
        prTextShaperDestroy(shaper);
        prFontSetDestroy(set);
    }

    // Creates the diagram of `source` in these fonts.
    PrStatus create(const std::string& source, PrMermaid* PR_NULLABLE* PR_NONNULL out,
            PrMermaidError* PR_NULLABLE error = nullptr) {
        PrMermaidDesc desc{.source = PrStringView(source), .fonts = set, .shaper = shaper};
        return prMermaidCreate(&desc, nullptr, out, error);
    }
};

// A diagram drawn in Noto Sans, with its items, freed when it goes out of scope.
struct MermaidDrawn {
    MermaidTestFonts fonts;
    PrMermaid* PR_NULLABLE diagram = nullptr;
    PrStatus status = PrStatus_Ok;
    std::vector<PrMermaidItem> items;

    explicit MermaidDrawn(const std::string& source) {
        status = fonts.create(source, &diagram);
        for (uint32_t i = 0; diagram != nullptr && i < prMermaidItemCount(diagram); ++i)
            items.push_back(prMermaidItem(diagram, i));
    }
    MermaidDrawn(const MermaidDrawn&) = delete;
    MermaidDrawn& operator=(const MermaidDrawn&) = delete;
    ~MermaidDrawn() {
        prMermaidDestroy(diagram);
    }

    // The items of one kind, in order.
    std::vector<PrMermaidItem> itemsOf(PrMermaidItemKind kind) const {
        std::vector<PrMermaidItem> out;
        for (const PrMermaidItem& item : items) {
            if (item.kind == kind)
                out.push_back(item);
        }
        return out;
    }

    // The number of items of one kind.
    uint32_t count(PrMermaidItemKind kind) const {
        return uint32_t(itemsOf(kind).size());
    }
};

} // namespace pr_impl

PR_ASSUME_NONNULL_END
