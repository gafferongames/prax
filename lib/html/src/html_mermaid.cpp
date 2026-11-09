// Mermaid diagrams as images (html_mermaid.h). The painter holds the diagram, which records its drawing once, and
// scales the drawing to the size that the image is shown at.

#include "html/src/html_mermaid.h"

#include "pr/alloc.h"
#include "pr/atomic.h"
#include "pr/mermaid.h"

#include "css/src/css_presentation.h"
#include "css/src/css_presentation_hooks.h"

namespace pr_impl {

namespace {

// A diagram that painters share.
struct MermaidImage {
    mutable Atomic<uint32_t> references;
    PrMermaid* diagram;
};

void retainImage(void* PR_NULLABLE context) {
    static_cast<MermaidImage*>(context)->references.fetchAddRelaxed(1);
}

void releaseImage(void* PR_NULLABLE context) {
    MermaidImage* image = static_cast<MermaidImage*>(context);
    if (image == nullptr || image->references.fetchSub(1) != 1)
        return;
    prMermaidDestroy(image->diagram);
    prMemoryFree(image);
}

// Draws the diagram in the box's scheme, stretched over the extent, which the image's object-fit has already fitted to
// the diagram's ratio.
void paintImage(void* PR_NULLABLE context, const PrLayoutPaintDesc* desc, PrDrawList* list) {
    PrVec2 extent = desc->extent;
    const MermaidImage* image = static_cast<const MermaidImage*>(context);
    PrVec2 natural = prMermaidExtent(image->diagram);
    if (natural.x <= 0 || natural.y <= 0)
        return;
    prDrawListPushTransformAffine2(list, prAffine2Scale(PrVec2{extent.x / natural.x, extent.y / natural.y}));
    prMermaidDraw(image->diagram, list, PrVec2{0, 0}, desc->scheme);
    prDrawListPopTransform(list);
}

} // namespace

PrLayoutPainter htmlMermaidPainter(const PrDomElement* element, PrStringView source, PrVec2* extent) {
    const PrCssPresentation* presentation = cssPresentationOf(element->document);
    if (presentation == nullptr)
        return PrLayoutPainter{};
    PrMermaidDesc desc{.source = source, .fonts = presentation->fonts, .shaper = presentation->shaper};
    PrMermaid* diagram = nullptr;
    PrMermaidError error{};
    if (prMermaidCreate(&desc, nullptr, &diagram, &error) != PrStatus_Ok)
        diagram = prMermaidCreateError(&desc, nullptr, &error);
    MermaidImage* image = allocZeroed<MermaidImage>();
    image->references.storeRelaxed(1);
    image->diagram = diagram;
    *extent = prMermaidExtent(diagram);
    return PrLayoutPainter{.paint = paintImage, .context = image, .retain = retainImage, .release = releaseImage};
}

} // namespace pr_impl
