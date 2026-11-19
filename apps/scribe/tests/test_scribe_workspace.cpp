// Scribe's workspace: the project panel over a folder, its containers expanding as folders do, the tabs that open
// resources by their addresses and follow their files, and the outline of the tab that shows.

#include "pr/align.h"
#include "pr/file.h"
#include "pr/layout_query.h"
#include "pr/string.h"
#include "pr/string_ops.h"
#include "pr/text_edit.h"
#include "pr/text_font.h"
#include "pr/text_view.h"
#include "pr/thread.h"
#include "pr/url_file.h"

#include "apps/scribe/scribe.h"
#include "apps/scribe/workspace.h"
#include "tools/harness/kit_fixture.h"
#include "tools/harness/zip_fixture.h"

#include "tools/harness/test.h"

using namespace harness;
using namespace scribe;

namespace {

// A layer of a prim with a property.
constexpr PrStringView kLayer = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
    def Mesh "Car"
    {
        float size = 2
    }
}
)";

// The same layer without the prim.
constexpr PrStringView kEmptied = R"(#usda 1.0
(
    defaultPrim = "World"
)

def Xform "World"
{
}
)";

constexpr PrStringView kNotes = "# Notes\n\nSome text.\n\n## Details\n\nMore text.\n";

// Binary glTF of `json` and the binary chunk `bin`.
String glb(PrStringView json, PrStringView bin) {
    auto put32 = [](String& out, uint32_t value) {
        for (uint32_t i = 0; i < 4; ++i)
            out.append(1, char((value >> (8 * i)) & 0xff));
    };
    uint32_t jsonSize = alignUp(uint32_t(json.size), 4u);
    uint32_t binSize = alignUp(uint32_t(bin.size), 4u);
    String out("glTF");
    put32(out, 2);
    put32(out, 12 + 8 + jsonSize + 8 + binSize);
    put32(out, jsonSize);
    out.append("JSON");
    out.append(json);
    out.append(jsonSize - uint32_t(json.size), ' ');
    put32(out, binSize);
    out.append({"BIN\0", 4});
    out.append(bin);
    out.append(binSize - uint32_t(bin.size), '\0');
    return out;
}

// Scribe with a project over a folder of its own.
struct Project {
    KitFixture f;
    Scribe s;
    String folder;

    Project() {
        PrFontFamily mono{.generic = PrFontGeneric_Monospace};
        PrFontStyle style = prFontStyleDefault();
        style.size = 13;
        PrTextFontDesc desc{.set = f.fonts, .families = &mono, .familyCount = 1, .style = style};
        s.font = prTextFontCreate(&desc);
        s.fonts = f.fonts;
        s.shaper = f.shaper;
        s.kit = f.kit;
        s.ui = f.ui;
        openEditor(s);
        // Each project has a folder of its own, so that the files of one test are not in another's.
        static uint32_t projects = 0;
        folder = format("{}/project{}/project", PrStringView(testFolder()), ++projects);
        REQUIRE(prFileCreateDirectory(folder));
        f.build = [this](PrLayoutNode root) { buildScribe(s, root); };
    }

    ~Project() { closeEditor(s); }

    void settle() {
        f.frame();
        for (uint32_t i = 0; i < 32 && afterLayout(s); ++i)
            f.frame();
        afterLayout(s);
    }

    String path(PrStringView name) { return format("{}/{}", folder, name); }

    // Returns the innermost node under `node` whose text, as the last layout shows it, is `text`, or none.
    PrLayoutNode find(PrStringView text, PrLayoutNode node = kPrLayoutNodeNone) {
        node = node == kPrLayoutNodeNone ? f.root : node;
        for (PrLayoutNode child = prLayoutNodeFirstChild(f.tree, node); child != kPrLayoutNodeNone;
                child = prLayoutNodeNextSibling(f.tree, child)) {
            if (PrLayoutNode found = find(text, child); found != kPrLayoutNodeNone)
                return found;
        }
        String shown = String::adopt(prLayoutNodeRenderedText(f.tree, node, nullptr));
        return shown == text ? node : kPrLayoutNodeNone;
    }

    // Clicks the node that shows `text`, and settles.
    void click(PrStringView text, uint32_t clicks = 1) {
        PrLayoutNode node = find(text);
        REQUIRE(node != kPrLayoutNodeNone);
        f.click(f.center(node), 0, clicks);
        settle();
    }

    // Returns true if the last layout shows `text`.
    bool shows(PrStringView text) {
        String shown = String::adopt(prLayoutNodeRenderedText(f.tree, f.root, nullptr));
        return prStringFind(shown, text) != shown.count();
    }

    String url(PrStringView name) {
        String absolute = String::adopt(prFilePathAbsolute(path(name), ""));
        return String::adopt(prUrlFromFilePath(absolute));
    }

    void write(PrStringView name, PrStringView bytes) { REQUIRE(prFileWrite(path(name), prStringBytes(bytes))); }

    // Returns the node of the project named `name` under `parent`, or kProjectNone.
    uint32_t child(uint32_t parent, PrStringView name) {
        for (uint32_t index : s.workspace->project[parent].children) {
            if (s.workspace->project[index].name == name)
                return index;
        }
        return kProjectNone;
    }

    // Reads the stamps of the files again in the next frame, as a second passing does.
    void poll() {
        s.workspace->polled = 0;
        prThreadSleep(20);
        settle();
    }
};

// The text that the primary selection of a tab's document holds.
String selectedText(ViewTab& tab) {
    PrTextEditSelection s = prTextEditViewPrimary(prTextViewEdit(prTextEditorDocumentView(tab.document)));
    PrStringView text = prTextEditBufferText(prTextEditorDocumentBuffer(tab.document));
    uint32_t low = s.anchor < s.caret ? s.anchor : s.caret;
    uint32_t high = s.anchor < s.caret ? s.caret : s.anchor;
    return String(prStringSlice(text, low, high - low));
}

// Puts the caret of a tab's document at the first `needle` of its text.
void putCaret(ViewTab& tab, PrStringView needle) {
    PrStringView text = prTextEditBufferText(prTextEditorDocumentBuffer(tab.document));
    uint32_t at = uint32_t(prStringFind(text, needle));
    PrTextEditSelection caret{.anchor = at, .caret = at};
    prTextEditViewSetSelections(prTextViewEdit(prTextEditorDocumentView(tab.document)), &caret, 1);
}

} // namespace

TEST_CASE("without a project the panel offers to open a folder, and an opened project is remembered") {
    Project p;
    p.s.statePath = format("{}/state/Last Project", PrStringView(testFolder()));
    p.settle();
    Workspace& w = *p.s.workspace;
    CHECK(w.project.isEmpty());
    CHECK(p.shows("Open Folder…"));
    CHECK(lastProject(p.s).isEmpty());

    // A file opens in the project of its folder, which the next launch opens again.
    p.write("notes.md", kNotes);
    REQUIRE(openPath(p.s, p.path("notes.md")));
    p.settle();
    REQUIRE(!w.project.isEmpty());
    CHECK(prStringEndsWith(w.root, "/project"));
    REQUIRE(shownTab(p.s) != nullptr);
    CHECK(shownTab(p.s)->title == "notes.md");
    CHECK(lastProject(p.s) == w.root);
    CHECK_FALSE(p.shows("Open Folder…"));

    // A file in a folder of the project opens in that project.
    REQUIRE(prFileCreateDirectory(p.path("docs")));
    p.write("docs/more.md", kNotes);
    REQUIRE(openPath(p.s, p.path("docs/more.md")));
    CHECK(prStringEndsWith(w.root, "/project"));

    // The View menu's commands hide and show each panel.
    CHECK(p.shows("Project"));
    REQUIRE(prCommandRegistryRun(p.s.commands, "view.toggleProject", nullptr, nullptr) == PrStatus_Ok);
    REQUIRE(prCommandRegistryRun(p.s.commands, "view.toggleOutline", nullptr, nullptr) == PrStatus_Ok);
    p.settle();
    CHECK_FALSE(p.shows("Project"));
    CHECK_FALSE(p.shows("Outline"));
    REQUIRE(prCommandRegistryRun(p.s.commands, "view.toggleProject", nullptr, nullptr) == PrStatus_Ok);
    p.settle();
    CHECK(p.shows("Project"));
}

TEST_CASE("the project's keys move through its rows without folding folders, and a click folds a folder") {
    Project p;
    REQUIRE(prFileCreateDirectory(p.path("alpha")));
    REQUIRE(prFileCreateDirectory(p.path("beta")));
    p.write("alpha/a.md", kNotes);
    p.write("notes.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    p.settle();
    Workspace& w = *p.s.workspace;
    uint32_t alpha = p.child(0, "alpha");
    uint32_t beta = p.child(0, "beta");
    p.click("alpha");
    CHECK(w.project[alpha].open);
    CHECK(w.projectSelected == alpha);
    p.click("alpha");
    CHECK_FALSE(w.project[alpha].open);
    // Down walks alpha, beta and notes.md, and folds neither folder.
    for (uint32_t i = 0; i < 2; ++i) {
        p.f.key(PrAppKey_Down);
        p.settle();    }
    CHECK(w.projectSelected == p.child(0, "notes.md"));
    CHECK_FALSE(w.project[alpha].open);
    CHECK_FALSE(w.project[beta].open);
    // The file shows, and focus stays in the tree.
    REQUIRE(shownTab(p.s) != nullptr);
    CHECK(shownTab(p.s)->title == "notes.md");
    CHECK(prUiFocus(p.f.ui) != prTextEditorTextNode(p.s.editor));
    // Right opens a folder, as Kit's tree walks it.
    p.f.key(PrAppKey_Up);
    p.settle();
    p.f.key(PrAppKey_Right);
    p.settle();
    CHECK(w.project[beta].open);
}

TEST_CASE("choosing files previews them in one tab while the tree keeps focus, and opening one keeps it") {
    Project p;
    p.write("a.md", kNotes);
    p.write("b.md", kNotes);
    p.write("c.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    p.settle();
    Workspace& w = *p.s.workspace;
    p.click("a.md");
    PrLayoutNode tree = prUiFocus(p.f.ui);
    REQUIRE(w.tabs.count() == 1);
    CHECK(prKitDockIsPreview(w.dock, w.tabs[0]->id));
    // Down previews the next file in the same tab, and focus stays in the tree.
    p.f.key(PrAppKey_Down);
    p.settle();
    REQUIRE(w.tabs.count() == 1);
    CHECK(shownTab(p.s)->title == "b.md");
    CHECK(prUiFocus(p.f.ui) == tree);
    // A double click keeps the tab, and the next preview has a tab of its own.
    p.f.click(p.f.center(p.find("c.md")), 0, 1);
    p.settle();
    p.click("c.md", 2);
    REQUIRE(w.tabs.count() == 1);
    CHECK_FALSE(prKitDockIsPreview(w.dock, w.tabs[0]->id));
    CHECK(shownTab(p.s)->title == "c.md");
    p.click("a.md");
    CHECK(w.tabs.count() == 2);
    CHECK(prKitDockIsPreview(w.dock, shownTab(p.s)->id));
    // Opening a file that a kept tab shows shows that tab.
    p.click("c.md");
    CHECK(w.tabs.count() == 2);
    CHECK(shownTab(p.s)->title == "c.md");
    // Enter keeps the preview and gives its text focus.
    p.click("b.md");
    p.f.key(PrAppKey_Enter);
    p.settle();
    CHECK_FALSE(prKitDockIsPreview(w.dock, shownTab(p.s)->id));
    CHECK(prUiFocus(p.f.ui) == prTextEditorTextNode(p.s.editor));
}

TEST_CASE("an address in the project reads relative to its folder, in the status line and the views") {
    Project p;
    p.write("with space.bin", "data");
    REQUIRE(openProject(p.s, p.folder));
    CHECK(displayAddress(p.s, format("{}#a/b.md", p.url("models.zip"))) == "models.zip#a/b.md");
    CHECK(displayAddress(p.s, p.url("with space.bin")) == "with space.bin");
    CHECK(displayAddress(p.s, "https://example.com/a") == "https://example.com/a");
    REQUIRE(openAddress(p.s, p.url("with space.bin")) == PrStatus_Ok);
    p.settle();
    CHECK(p.shows("with space.bin"));
    CHECK_FALSE(p.shows("file://"));
}

TEST_CASE("the project lists a folder, and a container expands as a folder does") {
    Project p;
    String json = R"({"asset":{"version":"2.0"},"buffers":[{"byteLength":8}],)"
        R"("bufferViews":[{"buffer":0,"byteLength":8}],"images":[{"bufferView":0,"mimeType":"image/png"}],)"
        R"("nodes":[{"name":"root"}],"scenes":[{"nodes":[0]}]})";
    String box = glb(json, {"\x89PNG\r\n\x1a\n", 8});
    ZipFixtureFile files[] = {{"Box/glTF-Binary/Box.glb", box}, {"readme.md", kNotes}};
    p.write("models.zip", zipFixture(files));
    p.write("notes.md", kNotes);
    ZipFixtureFile layers[] = {{"scene.usda", kLayer}};
    p.write("car.usdz", zipFixture(layers));
    REQUIRE(openProject(p.s, p.folder));
    p.settle();
    Workspace& w = *p.s.workspace;
    uint32_t models = p.child(0, "models.zip");
    REQUIRE(models != kProjectNone);
    CHECK(w.project[models].container);
    CHECK(!w.project[p.child(0, "notes.md")].container);
    CHECK(w.project[p.child(0, "car.usdz")].container);

    // An archive expands into its folders and files, and holds itself open while it does.
    setProjectOpen(p.s, models, true);
    p.settle();
    CHECK(w.project[models].held != nullptr);
    uint32_t folder = p.child(models, "Box");
    REQUIRE(folder != kProjectNone);
    CHECK(w.project[folder].folder);
    CHECK(p.child(models, "readme.md") != kProjectNone);
    uint32_t variant = p.child(folder, "glTF-Binary");
    REQUIRE(variant != kProjectNone);
    uint32_t asset = p.child(variant, "Box.glb");
    REQUIRE(asset != kProjectNone);
    CHECK(w.project[asset].container);
    CHECK(w.project[asset].address == format("{}#Box/glTF-Binary/Box.glb", p.url("models.zip")));

    // A glb in it expands into its images.
    setProjectOpen(p.s, asset, true);
    p.settle();
    uint32_t images = p.child(asset, "images");
    REQUIRE(images != kProjectNone);
    uint32_t image = p.child(images, "image0.png");
    REQUIRE(image != kProjectNone);
    CHECK(w.project[image].address == format("{}#Box/glTF-Binary/Box.glb!/images/0", p.url("models.zip")));

    // Closing the archive releases what it held.
    setProjectOpen(p.s, models, false);
    p.settle();
    CHECK(w.project[models].held == nullptr);
    CHECK(w.project[asset].held == nullptr);
}

TEST_CASE("a tab opens a resource by its address, and the outline and the views share its selection") {
    Project p;
    ZipFixtureFile layers[] = {{"scene.usda", kLayer}};
    p.write("car.usdz", zipFixture(layers));
    p.write("notes.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    String address = format("{}#/World/Car", p.url("car.usdz"));
    REQUIRE(openAddress(p.s, address) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->address == p.url("car.usdz"));
    CHECK(tab->mode == ViewMode_Preview);
    CHECK(tab->selection == "/World/Car");
    // The preview draws the stage, whose node at the selection's path is the one that it highlights.
    REQUIRE(tab->model != nullptr);
    CHECK(prModelNodeFromAddress(prModelViewModel(tab->model), tab->selection) != kPrModelNodeNone);
    CHECK(tab->title == "car.usdz");
    // The outline holds the stage's prims, depth first.
    REQUIRE(tab->outline.count() == 3);
    CHECK(tab->outline[2].step == "/World/Car");
    CHECK(tab->outline[2].depth == 2);

    // A layer of the package opens in a tab of its own, which has its source too.
    REQUIRE(openAddress(p.s, format("{}#scene.usda", p.url("car.usdz"))) == PrStatus_Ok);
    p.settle();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->format == PrMimeFormat_Usda);
    CHECK(tab->mode == ViewMode_Preview);
    CHECK(tab->document != nullptr);
    CHECK(p.s.workspace->tabs.count() == 2);

    // Opening an address that a tab shows shows that tab.
    REQUIRE(openAddress(p.s, p.url("car.usdz")) == PrStatus_Ok);
    CHECK(p.s.workspace->tabs.count() == 2);
    CHECK(shownTab(p.s)->format == PrMimeFormat_Usdz);

    // Markdown opens as its source, with its headings in the outline.
    REQUIRE(openAddress(p.s, p.url("notes.md")) == PrStatus_Ok);
    p.settle();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->mode == ViewMode_Source);
    REQUIRE(tab->outline.count() == 2);
    CHECK(tab->outline[1].label == "Details");
    CHECK(tab->outline[1].step == "line=4");
    tab->mode = ViewMode_Preview;
    p.settle();
}

TEST_CASE("Back and Forward walk the places that showed and the selections, reopening a place whose tab closed") {
    Project p;
    p.write("a.md", kNotes);
    p.write("b.md", kNotes);
    ZipFixtureFile layers[] = {{"scene.usda", kLayer}};
    p.write("car.usdz", zipFixture(layers));
    REQUIRE(openProject(p.s, p.folder));
    p.settle();
    Workspace& w = *p.s.workspace;
    auto run = [&](const char* id) {
        REQUIRE(prCommandRegistryRun(p.s.commands, id, nullptr, nullptr) == PrStatus_Ok);
        p.settle();
    };
    auto enabled = [&](const char* id) {
        PrCommandIndex index = prCommandRegistryFind(p.s.commands, id);
        return (prCommandRegistryState(p.s.commands, index) & PrCommandStateFlag_Disabled) == 0;
    };
    CHECK_FALSE(enabled("go.back"));
    // Two previews, a kept tab with a selection, and another selection in it.
    p.click("a.md");
    p.f.key(PrAppKey_Down);
    p.settle();
    REQUIRE(openAddress(p.s, format("{}#/World/Car", p.url("car.usdz"))) == PrStatus_Ok);
    p.settle();
    shownTab(p.s)->selection = "/World";
    p.settle();
    REQUIRE(prKitHistoryCount(w.history) == 4);
    CHECK(enabled("go.back"));
    CHECK_FALSE(enabled("go.forward"));

    // Back restores the selection, then shows the preview that is still open.
    run("go.back");
    CHECK(shownTab(p.s)->selection == "/World/Car");
    run("go.back");
    CHECK(shownTab(p.s)->title == "b.md");
    // The next preview took over a.md's tab, so Back opens it again as the preview.
    run("go.back");
    CHECK(shownTab(p.s)->title == "a.md");
    CHECK(prKitDockIsPreview(w.dock, shownTab(p.s)->id));
    CHECK(w.tabs.count() == 2);
    CHECK_FALSE(enabled("go.back"));
    CHECK(prKitHistoryCount(w.history) == 4);

    // Forward walks the same places, and a place that names the resource alone shows it with nothing selected.
    run("go.forward");
    run("go.forward");
    ViewTab* car = shownTab(p.s);
    REQUIRE(car != nullptr);
    CHECK(car->selection == "/World/Car");
    run("go.forward");
    CHECK(car->selection == "/World");
    CHECK_FALSE(enabled("go.forward"));

    // A mouse's back button and a swipe step too, and a new place after a step back drops the places ahead.
    PrAppEvent back{.kind = PrAppEventKind_PointerDown, .pointer = {.button = kPrAppPointerButtonBack}};
    CHECK(handleWindowEvent(p.s, back));
    p.settle();
    CHECK(car->selection == "/World/Car");
    PrAppEvent swipe{.kind = PrAppEventKind_Gesture};
    swipe.gesture = {.kind = PrAppGestureKind_Swipe, .phase = PrAppGesturePhase_Begin, .swipe = PrVec2{-1, 0}};
    CHECK(handleWindowEvent(p.s, swipe));
    p.settle();
    CHECK(car->selection == "/World");
    run("go.back");
    car->selection = String();
    p.settle();
    CHECK_FALSE(enabled("go.forward"));
    CHECK(prKitHistoryPlace(w.history, prKitHistoryCurrent(w.history)) == p.url("car.usdz"));
    PrAppEvent key{.kind = PrAppEventKind_KeyDown};
    CHECK_FALSE(handleWindowEvent(p.s, key));

    // Each address that opens is a place, though several open in one round.
    uint32_t count = prKitHistoryCount(w.history);
    REQUIRE(openAddress(p.s, p.url("a.md")) == PrStatus_Ok);
    REQUIRE(openAddress(p.s, p.url("b.md")) == PrStatus_Ok);
    p.settle();
    CHECK(prKitHistoryCount(w.history) == count + 2);
    run("go.back");
    CHECK(shownTab(p.s)->title == "a.md");
}

TEST_CASE("a tab's tooltip says where its resource is, and the tab menu's commands close and reveal its tabs") {
    Project p;
    ZipFixtureFile layers[] = {{"scene.usda", kLayer}};
    p.write("car.usdz", zipFixture(layers));
    p.write("notes.md", kNotes);
    p.write("other.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    String nested = format("{}#scene.usda", p.url("car.usdz"));
    REQUIRE(openAddress(p.s, p.url("notes.md")) == PrStatus_Ok);
    REQUIRE(openAddress(p.s, nested) == PrStatus_Ok);
    REQUIRE(openAddress(p.s, p.url("other.md")) == PrStatus_Ok);
    p.settle();
    Workspace& w = *p.s.workspace;
    REQUIRE(w.tabs.count() == 3);
    // A nested resource's tooltip names its container in the project, then its step, then its whole address.
    CHECK(w.tabs[1]->tooltip == format("car.usdz  ›  scene.usda\n{}", nested));
    CHECK(w.tabs[0]->tooltip == format("notes.md\n{}", p.url("notes.md")));

    // Close to the Right closes the tabs after the target in its leaf, and Close Others the rest.
    w.current = w.tabs[1]->id;
    REQUIRE(prCommandRegistryRun(p.s.commands, "tab.closeRight", nullptr, nullptr) == PrStatus_Ok);
    p.settle();
    CHECK(w.tabs.count() == 2);
    REQUIRE(prCommandRegistryRun(p.s.commands, "tab.closeOthers", nullptr, nullptr) == PrStatus_Ok);
    p.settle();
    REQUIRE(w.tabs.count() == 1);
    CHECK(w.tabs[0]->address == nested);

    // Reveal in Project opens the container and selects the entry.
    REQUIRE(prCommandRegistryRun(p.s.commands, "path.revealProject", nullptr, nullptr) == PrStatus_Ok);
    uint32_t car = p.child(0, "car.usdz");
    REQUIRE(car != kProjectNone);
    CHECK(w.project[car].open);
    CHECK(w.projectSelected == p.child(car, "scene.usda"));
    // A nested resource has no path of its own on disk.
    CHECK(prCommandRegistryRun(p.s.commands, "path.copy", nullptr, nullptr) == PrStatus_InvalidState);
    REQUIRE(prCommandRegistryRun(p.s.commands, "tab.closeAll", nullptr, nullptr) == PrStatus_Ok);
    p.settle();
    CHECK(w.tabs.isEmpty());
}

TEST_CASE("a project row's context menu opens its file to the side and collapses its folder") {
    Project p;
    REQUIRE(prFileCreateDirectory(p.path("docs")));
    p.write("docs/a.md", kNotes);
    p.write("notes.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, p.url("notes.md")) == PrStatus_Ok);
    p.settle();
    uint32_t docs = p.child(0, "docs");
    setProjectOpen(p.s, docs, true);
    p.settle();
    auto secondaryClick = [&](PrStringView text) {
        PrLayoutNode row = p.find(text);
        REQUIRE(row != kPrLayoutNodeNone);
        PrVec2 at = p.f.center(row);
        p.f.push(PrAppEvent{.kind = PrAppEventKind_PointerDown, .pointer = {.position = at, .button = 1}});
        p.f.push(PrAppEvent{.kind = PrAppEventKind_PointerUp, .pointer = {.position = at, .button = 1}});
        p.settle();
    };
    secondaryClick("a.md");
    p.click("Open to the Side");
    Workspace& w = *p.s.workspace;
    REQUIRE(w.tabs.count() == 2);
    // The file opens in a leaf of its own beside the first tab's.
    CHECK(prKitDockPanelLeaf(w.dock, w.tabs[0]->id) != prKitDockPanelLeaf(w.dock, w.tabs[1]->id));
    CHECK(w.tabs[1]->address == p.url("docs/a.md"));
    secondaryClick("a.md");
    p.click("Collapse All");
    CHECK_FALSE(w.project[docs].open);
}

TEST_CASE("a preview's URL takes a nested resource's steps as a path, which its relative URLs resolve back through") {
    Project p;
    ZipFixtureFile files[] = {{"docs/a b.md", "# A\n"}, {"img/x.png", "x"}};
    p.write("bundle.zip", zipFixture(files));
    REQUIRE(openProject(p.s, p.folder));
    String zip = p.url("bundle.zip");
    String url = pageUrl(format("{}#docs/a%20b.md", zip));
    CHECK(url == format("{}/docs/a%20b.md", zip));
    // A sibling up a folder in the archive, the archive's own file, and a file beside it.
    CHECK(pageAddress(format("{}/img/x.png", zip)) == format("{}#img/x.png", zip));
    CHECK(pageAddress(zip) == zip);
    p.write("notes.md", kNotes);
    CHECK(pageAddress(p.url("notes.md")) == p.url("notes.md"));
    CHECK(pageAddress("https://example.com/").isEmpty());
}

TEST_CASE("Markdown previews as a page of lib/html, and Toggle Preview switches its tab between it and the source") {
    Project p;
    p.write("notes.md", kNotes);
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, p.url("notes.md")) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->page != nullptr);
    REQUIRE(prCommandRegistryRun(p.s.commands, "view.togglePreview", nullptr, nullptr) == PrStatus_Ok);
    CHECK(tab->mode == ViewMode_Preview);
    p.settle();
    REQUIRE(prCommandRegistryRun(p.s.commands, "view.togglePreview", nullptr, nullptr) == PrStatus_Ok);
    CHECK(tab->mode == ViewMode_Source);
}

TEST_CASE("Look Through Camera shows a model's preview through an asset's camera, by its name or its path") {
    Project p;
    p.write("shot.usda", "#usda 1.0\ndef Xform \"World\" {\n def Camera \"Wide\" {\n }\n def Camera \"Close\" {\n }\n}\n");
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, p.url("shot.usda")) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    REQUIRE(tab->model != nullptr);
    auto look = [&](PrStringView arguments) {
        PrJson* document = prJsonCreate();
        REQUIRE(prJsonParse(document, arguments, 0, nullptr));
        PrCommandArgs args{.document = document};
        PrStatus status = prCommandRegistryRun(p.s.commands, "model.camera", &args, nullptr);
        prJsonDestroy(document);
        return status;
    };
    REQUIRE(look(R"({"camera": "Close"})") == PrStatus_Ok);
    CHECK(prModelViewCamera(tab->model) == 2);
    CHECK(tab->mode == ViewMode_Preview);
    REQUIRE(look(R"({"camera": "/World/Wide"})") == PrStatus_Ok);
    CHECK(prModelViewCamera(tab->model) == 1);
    // A name that no camera has is the orbit camera, and the camera is a required argument.
    REQUIRE(look(R"({"camera": "Missing"})") == PrStatus_Ok);
    CHECK(prModelViewCamera(tab->model) == 0);
    CHECK(prCommandRegistryRun(p.s.commands, "model.camera", nullptr, nullptr) == PrStatus_InvalidData);
}

TEST_CASE("a tab of a nested resource follows its file, and shows what is missing without closing") {
    Project p;
    ZipFixtureFile layers[] = {{"scene.usda", kLayer}};
    p.write("car.usdz", zipFixture(layers));
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, format("{}#scene.usda", p.url("car.usdz"))) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->outline.count() == 3);

    // The package changes: its layer loses the prim, and the tab's outline follows.
    ZipFixtureFile emptied[] = {{"scene.usda", kEmptied}};
    p.write("car.usdz", zipFixture(emptied));
    p.poll();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->status == PrStatus_Ok);
    CHECK(tab->outline.count() == 2);

    // The layer leaves the package: the tab stays, and says which step is missing.
    ZipFixtureFile other[] = {{"other.usda", kLayer}};
    p.write("car.usdz", zipFixture(other));
    p.poll();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->status == PrStatus_NotFound);
    CHECK(tab->failedStep == 0);
    CHECK(tab->node == nullptr);

    // It comes back, and the tab shows it again.
    p.write("car.usdz", zipFixture(layers));
    p.poll();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->status == PrStatus_Ok);
    CHECK(tab->outline.count() == 3);
}

TEST_CASE("a glb's image in an archive opens in the image view") {
    Project p;
    uint8_t pixels[4] = {255, 0, 0, 255};
    size_t size = 0;
    PrPixelView red{pixels, 4, {1, 1}};
    REQUIRE(prImageCodecSetEncode(p.f.images, "image/png", red, nullptr, 0, &size) == PrStatus_Ok);
    const uint32_t capacity = uint32_t(size);
    Array<uint8_t> encoded(capacity);
    REQUIRE(prImageCodecSetEncode(p.f.images, "image/png", red, encoded.data(), encoded.count(), &size) == PrStatus_Ok);
    const String png(PrStringView{reinterpret_cast<const char*>(encoded.data()), size});
    String json = format(R"({{"asset":{{"version":"2.0"}},"buffers":[{{"byteLength":{}}}],)"
        R"("bufferViews":[{{"buffer":0,"byteLength":{}}}],"images":[{{"bufferView":0,"mimeType":"image/png"}}]}})",
        size, size);
    String asset = glb(json, png);
    ZipFixtureFile files[] = {{"Box.glb", asset}};
    p.write("models.zip", zipFixture(files));
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, format("{}#Box.glb!/images/0", p.url("models.zip"))) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->format == PrMimeFormat_Png);
    CHECK(tab->mode == ViewMode_Image);
    REQUIRE(tab->image != nullptr);
    REQUIRE(prImageViewInfo(tab->image) != nullptr);
    CHECK(prImageViewInfo(tab->image)->extent.x == 1);
    CHECK(tab->title == "image0.png");

    // The glb opens in its preview, the model view, with its scenes and nodes in the outline.
    REQUIRE(openAddress(p.s, format("{}#Box.glb", p.url("models.zip"))) == PrStatus_Ok);
    p.settle();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    CHECK(tab->mode == ViewMode_Preview);
    CHECK(tab->model != nullptr);
}

TEST_CASE("the selection of a .gltf or .usda tab selects its text in the source, and the caret selects its item") {
    Project p;
    PrStringView gltf = "{\n  \"asset\": {\"version\": \"2.0\"},\n  \"scenes\": [{\"nodes\": [0, 1]}],\n"
        "  \"nodes\": [\n    {\"name\": \"Body\"},\n    {\"name\": \"Wheel\", \"translation\": [1, 0, 0]}\n  ]\n}\n";
    p.write("car.gltf", gltf);
    p.write("scene.usda", kLayer);
    REQUIRE(openProject(p.s, p.folder));
    REQUIRE(openAddress(p.s, format("{}#/nodes/1", p.url("car.gltf"))) == PrStatus_Ok);
    p.settle();
    ViewTab* tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    REQUIRE(tab->document != nullptr);
    // The selection shows as its text once the tab shows its source.
    tab->mode = ViewMode_Source;
    p.settle();
    CHECK(selectedText(*tab) == "{\"name\": \"Wheel\", \"translation\": [1, 0, 0]}");

    // The caret in the other node's text selects that node, whose row the outline marks.
    putCaret(*tab, "Body");
    p.settle();
    CHECK(tab->selection == "/nodes/0");
    // A caret outside every item leaves the selection as it is.
    putCaret(*tab, "asset");
    p.settle();
    CHECK(tab->selection == "/nodes/0");

    // The structure view shows the selected node, and following the link to the scene that names it selects the scene.
    tab->mode = ViewMode_Structure;
    p.settle();
    CHECK(p.shows("NAMED BY"));
    p.click("/scenes/0");
    CHECK(tab->selection == "/scenes/0");

    // A layer selects a prim's text, and the caret in a property selects the property.
    REQUIRE(openAddress(p.s, format("{}#/World/Car", p.url("scene.usda"))) == PrStatus_Ok);
    p.settle();
    tab = shownTab(p.s);
    REQUIRE(tab != nullptr);
    tab->mode = ViewMode_Source;
    p.settle();
    CHECK(prStringStartsWith(selectedText(*tab), "def Mesh \"Car\""));
    putCaret(*tab, "size");
    p.settle();
    CHECK(tab->selection == "/World/Car.size");
}
