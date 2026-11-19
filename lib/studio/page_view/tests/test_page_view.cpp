#include "pr/array.h"
#include "pr/fetch_intercept.h"
#include "pr/kit_box.h"
#include "pr/layout_query.h"
#include "pr/mutex.h"
#include "pr/net.h"
#include "pr/page_view.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/thread.h"

#include "tools/harness/kit_fixture.h"

#include "tools/harness/test.h"

using namespace harness;

namespace pr_impl {

namespace {

// A page view over a kit fixture, whose fetches are recorded and answered as not found.
struct Viewed {
    KitFixture f;
    PrNet* net = prNetCreate(PrNetBackend_Default);
    PrFetchNetwork* fetcher = nullptr;
    PrTextFont* font = nullptr;
    PrPageView* view = nullptr;
    PrMutex mutex{};
    Array<String> fetched;
    PrLayoutNode box = kPrLayoutNodeNone;

    Viewed() {
        prMutexInit(&mutex);
        PrFetchInterceptDesc fetch{
            .net = net,
            .request = [](void* context, PrFetchInterceptRequest* request) {
                Viewed& v = *static_cast<Viewed*>(context);
                prMutexLock(&v.mutex);
                v.fetched.push(String(request->desc.url));
                prMutexUnlock(&v.mutex);
                PrFetchInterceptResponse missing{.status = 404, .type = PrFetchResponseType_Basic};
                prFetchInterceptRequestRespond(request, &missing);
            },
            .context = this,
        };
        fetcher = prFetchInterceptCreate(&fetch);
        PrTextFontDesc desc{.set = f.fonts, .style = prFontStyleDefault()};
        font = prTextFontCreate(&desc);
        PrPageViewDesc page{.kit = f.kit, .font = font, .shaper = f.shaper, .fetcher = fetcher};
        view = prPageViewCreate(&page);
        f.build = [this](PrLayoutNode root) {
            // The view fills the room that layout gives it.
            PrKitBoxStyle room{.place = {.width = prKitPx(400), .height = prKitPx(300), .flex = PrKitFlex_Fixed}};
            box = prKitBox(f.kit, root, {}, room);
            prPageViewFrame(view, box);
        };
    }

    ~Viewed() {
        prPageViewDestroy(view);
        prTextFontRelease(font);
        prNetPostBlocking(net, [](void* context) { prFetchNetworkDestroy(static_cast<PrFetchNetwork*>(context)); }, fetcher);
        prNetDestroy(net);
        prMutexReset(&mutex);
    }

    // Frames for a while, so that the document's fetches are answered and it loads.
    void settle() {
        for (uint32_t i = 0; i < 100; ++i) {
            f.frame();
            prThreadSleep(2);
        }
    }

    // Returns the document's element: the replaced element in the view's scroll box.
    PrLayoutNode document() { return prLayoutNodeFirstChild(f.tree, prLayoutNodeFirstChild(f.tree, box)); }
};

} // namespace

} // namespace pr_impl

using namespace pr_impl;

TEST_CASE("a page view lays its Markdown out as tall as its content, and fetches its images relative to its URL") {
    Viewed v;
    PrStringView text = "# Title\n\nA paragraph.\n\n![x](img/x.png)\n\n| a | b |\n|---|---|\n| 1 | 2 |\n\n"
        "# Two\n\nMore.\n\n# Three\n\nMore.\n\n# Four\n\nMore.\n\n# Five\n\nMore.\n";
    prPageViewLoad(v.view, "file:///docs/README.md", "text/markdown; charset=utf-8",
        PrByteView{reinterpret_cast<const uint8_t*>(text.data), text.size});
    v.settle();
    prMutexLock(&v.mutex);
    REQUIRE(v.fetched.count() >= 1);
    CHECK(v.fetched[0] == "file:///docs/img/x.png");
    prMutexUnlock(&v.mutex);
    PrRect bounds = v.f.bounds(v.document());
    CHECK(bounds.right - bounds.left == doctest::Approx(400.0));
    // The document is taller than the view, which scrolls it.
    CHECK(bounds.bottom - bounds.top > 300.0f);
}

TEST_CASE("a page view gives the document the wheel and the keys, and scrolls by what the document does not take") {
    Viewed v;
    String text;
    for (uint32_t i = 0; i < 60; ++i)
        formatTo(text, "Paragraph {}.\n\n", i);
    prPageViewLoad(v.view, "file:///docs/long.md", "text/markdown; charset=utf-8", prStringBytes(text));
    v.settle();
    // The document is painted, as a window paints it, which lays it out in a viewport as tall as its content.
    Array<uint8_t> pixels;
    REQUIRE(v.f.render(&pixels));
    PrLayoutNode scroller = prLayoutNodeFirstChild(v.f.tree, v.box);
    auto scrolled = [&] { return prLayoutUnitToPx(prLayoutNodeScroll(v.f.tree, scroller).y); };
    REQUIRE(scrolled() == 0.0f);
    // The wheel over the document reaches the document, whose own containers do not scroll, so the view does.
    PrVec2 at = v.f.center(v.box);
    v.f.move(at);
    v.f.push(PrAppEvent{.kind = PrAppEventKind_Scroll, .scroll = {.position = at, .delta = {0, 120}}});
    v.f.frame();
    CHECK(scrolled() == doctest::Approx(120.0));

    // A click gives the document focus, and End and Home, which the document passes on, scroll the view to its ends.
    v.f.click(at);
    v.f.frame();
    CHECK(prUiFocus(v.f.ui) == v.document());
    v.f.key(PrAppKey_End);
    v.f.frame();
    PrLayoutScrollInfo info{};
    REQUIRE(prLayoutNodeScrollInfo(v.f.tree, scroller, &info));
    CHECK(scrolled() == doctest::Approx(double(prLayoutUnitToPx(info.range.bottom))));
    v.f.key(PrAppKey_Home);
    v.f.frame();
    CHECK(scrolled() == 0.0f);
}
