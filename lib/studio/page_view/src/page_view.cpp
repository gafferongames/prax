// Studio's page view: a document of the web rendered by lib/html in a Studio Kit tree.

#include "pr/page_view.h"

#include "pr/alloc.h"
#include "pr/float.h"
#include "pr/html_page.h"
#include "pr/html_view.h"
#include "pr/kit_box.h"
#include "pr/layout_query.h"
#include "pr/layout_tree.h"
#include "pr/time.h"
#include "pr/ui.h"

namespace pr_impl {

namespace {

// The most time that a frame runs the document's tasks for, in milliseconds.
constexpr double kTaskBudget = 8;

// How much of a page a page's scroll keeps in view, in px.
constexpr float kLine = 40;

double nowMillis(void*) {
    return prTimeMonotonicMilliseconds();
}

struct PageView {
    PrKit* kit;
    PrTextFont* font;
    PrTextShaper* shaper;
    const PrImageCodecSet* PR_NULLABLE images;
    const PrCompressCodecSet* PR_NULLABLE compress;
    PrFetchNetwork* fetcher;
    PrHtmlPage* PR_NULLABLE page;
    PrHtmlView* PR_NULLABLE view;
    uint64_t revision;
    PrAppColorScheme scheme;
    bool schemed;                   // the document has the user's color scheme
    bool loading;
};

void closePage(PageView& v) {
    prHtmlViewDestroy(v.view);
    v.view = nullptr;
    prHtmlPageDestroy(v.page);
    v.page = nullptr;
}

void openPage(PageView& v) {
    PrHtmlHost host = prHtmlHostDefault();
    host.context = &v;
    host.fetcher = v.fetcher;
    host.font = v.font;
    host.shaper = v.shaper;
    host.images = v.images;
    host.textures = v.kit->textures;
    host.compress = v.compress;
    host.now = nowMillis;
    host.loaded = [](void* context, PrHtmlPage*, PrStatus) { static_cast<PageView*>(context)->loading = false; };
    v.page = prHtmlPageCreate(&host);
    PrHtmlViewDesc desc{.page = v.page};
    v.view = prHtmlViewCreate(&desc);
    v.schemed = false;
}

// Scrolls `scroller`, the box that shows the document, by what the document's last event asked of the containers around
// it and none of its own took (prHtmlViewOverscrolled). The box scrolls within its range as the last layout gave it,
// which is the layout that the user acted on.
void overscroll(PageView& v, PrLayoutNode scroller) {
    PrHtmlViewOverscroll rest = prHtmlViewOverscrolled(v.view);
    if (rest.amount.x == 0 && rest.amount.y == 0)
        return;
    PrLayoutTree* tree = v.kit->tree;
    PrLayoutScrollInfo info{};
    if (!prLayoutNodeScrollInfo(tree, scroller, &info) || !info.scrollContainer)
        return;
    PrVec2 offset = prLayoutVecToPx(info.offset);
    PrRect range = prLayoutRectToPx(info.range);
    PrVec2 port = prLayoutVecToPx(info.portExtent);
    PrVec2 by = rest.amount;
    if (rest.unit == PrHtmlViewOverscrollUnit_Pages) {
        by = PrVec2{rest.amount.x * prF32Max(port.x - kLine, kLine), rest.amount.y * prF32Max(port.y - kLine, kLine)};
    } else if (rest.unit == PrHtmlViewOverscrollUnit_End) {
        by = PrVec2{rest.amount.x * (range.right - range.left), rest.amount.y * (range.bottom - range.top)};
    }
    PrVec2 to{
        prF32Clamp(offset.x + by.x, range.left, range.right),
        prF32Clamp(offset.y + by.y, range.top, range.bottom),
    };
    if (to.x != offset.x || to.y != offset.y)
        prLayoutNodeSetScroll(tree, scroller, prLayoutVecFromPx(to));
}

// Routes the round's input to the document: the pointer's over the document's element, in the document's own
// coordinates, and the keys and the text while the element has focus. The wheel goes to the document's own scroll
// containers first, and what they do not take scrolls `scroller`, as do the keys that scroll.
void route(PageView& v, PrLayoutNode node, PrLayoutNode scroller) {
    PrUi* ui = v.kit->ui;
    PrUiEvents events = prUiEvents(ui, node);
    for (uint32_t i = 0; i < events.count; ++i) {
        PrAppEvent event = events.events[i];
        switch (event.kind) {
            case PrAppEventKind_PointerDown:
            case PrAppEventKind_PointerUp:
            case PrAppEventKind_PointerMove: {
                PrVec2 at{};
                if (!prUiLocalPoint(ui, node, event.pointer.position, &at))
                    break;
                event.pointer.position = at;
                prHtmlViewEvent(v.view, &event, 0);
                break;
            }
            case PrAppEventKind_KeyDown:
            case PrAppEventKind_KeyUp:
            case PrAppEventKind_Text:
            case PrAppEventKind_CompositionUpdate:
            case PrAppEventKind_CompositionEnd:
                prHtmlViewEvent(v.view, &event, 0);
                overscroll(v, scroller);
                break;
            default:
                break;
        }
    }
    PrVec2 wheel{};
    PrVec2 pointer{};
    PrVec2 at{};
    if (prUiWheel(ui, node, &wheel) && prUiPointer(ui, nullptr, &pointer) && prUiLocalPoint(ui, node, pointer, &at)) {
        PrAppEvent scroll{.kind = PrAppEventKind_Scroll, .time = prUiCurrentRound(ui)->time};
        scroll.scroll = PrAppScrollEvent{.position = at, .delta = wheel};
        prHtmlViewEvent(v.view, &scroll, 0);
        overscroll(v, scroller);
    }
}

} // namespace

} // namespace pr_impl

using namespace pr_impl;

// C opaque types
struct PrPageView : PageView {};

PrPageView* prPageViewCreate(const PrPageViewDesc* desc) {
    PrPageView* v = allocZeroed<PrPageView>();
    v->kit = desc->kit;
    v->font = desc->font;
    prTextFontRetain(v->font);
    v->shaper = desc->shaper;
    v->images = desc->images;
    v->compress = desc->compress;
    v->fetcher = desc->fetcher;
    openPage(*v);
    return v;
}

void prPageViewDestroy(PrPageView* PR_NULLABLE view) {
    if (view == nullptr)
        return;
    closePage(*view);
    prTextFontRelease(view->font);
    prMemoryFree(view);
}

void prPageViewLoad(PrPageView* view, PrStringView url, PrStringView contentType, PrByteView bytes) {
    PageView& v = *view;
    closePage(v);
    openPage(v);
    PrHtmlResponseHeaders headers{.contentType = contentType};
    v.loading = prHtmlPageBeginLoad(v.page, url, &headers) == PrStatus_Ok;
    if (!v.loading)
        return;
    prHtmlPageAppendLoad(v.page, PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size});
    prHtmlPageEndLoad(v.page);
    ++v.revision;
}

void prPageViewFrame(PrPageView* view, PrLayoutNode parent) {
    PageView& v = *view;
    PrKit* kit = v.kit;
    PrUi* ui = kit->ui;
    PrDomAgent* agent = prHtmlPageAgent(v.page);
    double deadline = prTimeMonotonicMilliseconds() + kTaskBudget;
    bool ran = false;
    while (prTimeMonotonicMilliseconds() < deadline && prDomAgentNextTaskDelay(agent) == 0 && prDomAgentRunTask(agent))
        ran = true;
    PrAppUserPreferences preferences = prUiUserPreferences(ui);
    if (!v.schemed || preferences.colorScheme != v.scheme) {
        prHtmlViewSetUserPreferences(v.view, &preferences);
        v.scheme = preferences.colorScheme;
        v.schemed = true;
        ++v.revision;
    }
    // While the document loads or its tasks run, what it shows changes, and the next frame shows it again.
    bool busy = v.loading || prHtmlPageIsFetching(v.page) || prDomAgentNextTaskDelay(agent) >= 0;
    if (busy || ran) {
        ++v.revision;
        prUiRequestFrame(ui);
    }

    PrKitBoxStyle box{
        .overflow = PrKitOverflow_ScrollY,
        .place = {.width = prKitPercent(100), .height = prKitPercent(100), .flex = PrKitFlex_Fixed},
    };
    PrLayoutNode scroller = prKitBox(kit, parent, {}, box);
    PrLayoutTree* tree = kit->tree;
    PrLayoutNode document = prLayoutTreeFrameReplaced(tree, scroller, 0, 0);
    // The document fills the view at least, so that its canvas does, as a browser's viewport shows it.
    prKitPlace(kit, document, {.width = prKitPercent(100), .minHeight = prKitPercent(100), .flex = PrKitFlex_Fixed});
    // The document takes focus, and the keys while it has it, and the wheel over it.
    prUiSetFlags(ui, document, PrUiFlag_Pressable | PrUiFlag_Focusable | PrUiFlag_Wheel);
    PrLayoutContent content{.kind = PrLayoutContentKind_Painter, .revision = v.revision};
    content.painter = prHtmlViewPainter(v.view);
    prLayoutTreeFrameSetContent(tree, document, &content);
    route(v, document, scroller);
}
