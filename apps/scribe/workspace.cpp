// Scribe's workspace: the project panel, the tabs of resources by their addresses, their views, and the outline.

#include "apps/scribe/workspace.h"

#include "pr/alloc.h"
#include "pr/app_data.h"
#include "pr/app_shell.h"
#include "pr/ascii.h"
#include "pr/assert.h"
#include "pr/blob.h"
#include "pr/command_kit.h"
#include "pr/compress_select.h"
#include "pr/construct.h"
#include "pr/count_of.h"
#include "pr/file.h"
#include "pr/gltf_model.h"
#include "pr/image_select.h"
#include "pr/integer.h"
#include "pr/kit_box.h"
#include "pr/kit_button.h"
#include "pr/kit_choice.h"
#include "pr/kit_icon.h"
#include "pr/kit_menu.h"
#include "pr/kit_text.h"
#include "pr/layout_query.h"
#include "pr/sort.h"
#include "pr/string_ops.h"
#include "pr/text_view.h"
#include "pr/time.h"
#include "pr/url_file.h"
#include "pr/url_percent.h"
#include "pr/usd_model.h"

#include "apps/scribe/gltf_address.h"
#include "apps/scribe/image_address.h"
#include "apps/scribe/scribe.h"
#include "apps/scribe/usd_address.h"
#include "apps/scribe/zip_address.h"

namespace scribe {

using namespace pr;

namespace {

// The heights of the line of a tab's views, and of a row of a panel, in px.
constexpr float kModeHeight = 30;
constexpr float kRowHeight = 22;

// How often the workspace reads the stamps of the files that its tabs and its project hold open, in seconds.
constexpr double kPollSeconds = 1;

double nowSeconds() {
    return prTimeMonotonicMilliseconds() / 1000;
}

// The most places that the history keeps.
constexpr uint32_t kHistoryPlaces = 100;

// Returns true if a file of this name may hold resources that the project expands, which its bytes then decide.
bool mayContain(PrStringView name) {
    return prAsciiEndsWithFold(name, ".zip") || prAsciiEndsWithFold(name, ".usdz") || prAsciiEndsWithFold(name, ".glb");
}

// Returns true if bytes of `format` hold resources that the project expands.
bool isContainer(PrMimeFormat format) {
    return format == PrMimeFormat_Zip || format == PrMimeFormat_Usdz || format == PrMimeFormat_Glb;
}

// ---------------------------------------------------------------------------------------------------------------------
// The project

uint32_t addNode(Workspace& w, uint32_t parent, ProjectNode node) {
    node.parent = parent;
    w.project.push(static_cast<ProjectNode&&>(node));
    uint32_t index = w.project.count() - 1;
    if (parent != kProjectNone)
        w.project[parent].children.push(index);
    return index;
}

// The entries of a folder on disk.
struct FolderEntry {
    String name;
    bool folder = false;
};

void listFolder(Workspace& w, uint32_t index) {
    Array<FolderEntry> entries;
    prFileEachEntry(w.project[index].path, [](void* context, const PrFileEntry* entry) {
        if (prStringStartsWith(entry->name, "."))
            return;
        FolderEntry found{entry->name, entry->info.kind == PrFileKind_Directory};
        static_cast<Array<FolderEntry>*>(context)->push(static_cast<FolderEntry&&>(found));
    }, &entries);
    sortItems(Span<FolderEntry>(entries.data(), entries.count()), [](const FolderEntry& a, const FolderEntry& b) {
        if (a.folder != b.folder)
            return a.folder;
        return prStringCompare(a.name, b.name) < 0;
    });
    String folder = w.project[index].path;
    for (const FolderEntry& entry : entries) {
        ProjectNode node;
        node.name = entry.name;
        PrTextBuffer path = {};
        prTextBufferAppend(&path, folder);
        prFilePathAppend(&path, entry.name);
        node.path = prTextBufferView(&path);
        prTextBufferReset(&path);
        node.folder = entry.folder;
        if (!entry.folder) {
            node.address = String::adopt(prUrlFromFilePath(node.path));
            // A file's first bytes say whether it holds resources.
            if (PrBlob* blob = prFileOpen(node.path)) {
                node.container = isContainer(prMimeSniff(prBlobBytes(blob), node.name));
                prBlobRelease(blob);
            }
        }
        addNode(w, index, static_cast<ProjectNode&&>(node));
    }
    w.project[index].listed = true;
}

// Returns the child of `parent` named `name` that is a folder, adding it if there is none.
uint32_t folderChild(Workspace& w, uint32_t parent, PrStringView name) {
    for (uint32_t child : w.project[parent].children) {
        if (w.project[child].folder && w.project[child].name == name)
            return child;
    }
    ProjectNode node;
    node.name = name;
    node.folder = true;
    return addNode(w, parent, static_cast<ProjectNode&&>(node));
}

// Lists the resources of a container, with folders where their paths have them, and holds the container open.
void listContainer(Workspace& w, uint32_t index) {
    w.project[index].listed = true;
    AddressNode* node = nullptr;
    PrStatus status = addressResolveText(w.resolver, w.project[index].address, &node);
    if (status == PrStatus_Ok)
        status = addressNodeOpen(node);
    if (status != PrStatus_Ok) {
        w.project[index].failure = format("does not open: {}", prStatusName(status));
        addressNodeRelease(node);
        return;
    }
    w.project[index].failure = String();
    w.project[index].held = node;
    String container = w.project[index].address;
    for (uint32_t i = 0; i < addressNodeChildCount(node); ++i) {
        AddressChild child = addressNodeChild(node, i);
        uint32_t parent = index;
        PrStringView path = child.path;
        for (size_t slash = prStringFindByte(path, '/'); slash != path.size; slash = prStringFindByte(path, '/')) {
            if (slash != 0)
                parent = folderChild(w, parent, prStringHead(path, slash));
            path = prStringFrom(path, slash + 1);
        }
        ProjectNode file;
        file.name = path;
        file.address = addressWithStep(container, child.step);
        file.container = mayContain(path);
        addNode(w, parent, static_cast<ProjectNode&&>(file));
    }
}

// Closes a node of the project: forgets its children, which are listed again when it opens, and releases what it held.
void forget(Workspace& w, uint32_t index) {
    for (uint32_t child : w.project[index].children)
        forget(w, child);
    addressNodeRelease(w.project[index].held);
    w.project[index].held = nullptr;
}

void setOpen(Workspace& w, uint32_t index, bool open) {
    ProjectNode& node = w.project[index];
    node.open = open;
    // A container that closes releases what it holds, and lists its resources again when it opens.
    if (!open && node.container) {
        forget(w, index);
        node.children.clear();
        node.listed = false;
    }
    if (!open || node.listed)
        return;
    if (node.folder && !node.path.isEmpty())
        listFolder(w, index);
    else if (node.container)
        listContainer(w, index);
}

// The rows that the project shows, in display order: the open nodes' descendants under the folder.
void visibleRows(const Workspace& w, uint32_t index, uint32_t depth, Array<uint32_t>& rows, Array<PrKitTreeItem>& items) {
    for (uint32_t child : w.project[index].children) {
        const ProjectNode& node = w.project[child];
        rows.push(child);
        bool expands = node.folder || node.container;
        items.push(PrKitTreeItem{.depth = depth, .hasChildren = expands, .open = node.open});
        if (node.open)
            visibleRows(w, child, depth + 1, rows, items);
    }
}

// A container that holds a stale node lists its children again.
void refreshProject(Workspace& w) {
    for (uint32_t i = 0; i < w.project.count(); ++i) {
        ProjectNode& node = w.project[i];
        if (node.held == nullptr || !addressNodeIsStale(node.held))
            continue;
        forget(w, i);
        node.children.clear();
        node.listed = false;
        if (node.open)
            setOpen(w, i, true);
    }
}

PrKitTextStyle bodyStyle(PrKitTone tone = PrKitTone_Secondary) {
    return PrKitTextStyle{.role = PrKitTextRole_Body, .tone = tone, .wrap = PrKitWrap_Wrap};
}

// Asks the system for a folder, or a file, and opens what the user chooses as openPath does.
void chooseProject(Scribe& s, bool folder) {
    if (s.app == nullptr)
        return;
    PrAppFileDialogDesc desc{
        .kind = PrAppFileDialogKind_Open,
        .flags = folder ? PrAppFileDialogFlags(PrAppFileDialogFlag_Directories) : PrAppFileDialogFlags(0),
        .parent = prUiWindow(s.ui),
        .title = folder ? "Open Folder" : "Open File",
        .directory = s.workspace->root,
    };
    prAppFileDialog(s.app, &desc, [](void* context, PrStatus status, PrAppDataSource source) {
        Scribe& s = *static_cast<Scribe*>(context);
        if (status != PrStatus_Ok || source == kPrAppDataSourceNone)
            return;
        prAppDataReadFiles(s.app, source, [](void* context, PrStatus status, const PrAppDataFile* files, uint32_t count) {
            if (status == PrStatus_Ok && files != nullptr && count != 0 && !files[0].path.isEmpty())
                openPath(*static_cast<Scribe*>(context), files[0].path);
        }, context);
    }, &s);
}

void projectRows(void* context, PrKit* kit, PrLayoutNode view, PrKitWindow window);

// Declares the project panel in its pane, `box`: the tree of the project's files, or an offer to open a folder.
void buildProject(Scribe& s, PrLayoutNode box) {
    Workspace& w = *s.workspace;
    PrKit* kit = s.kit;
    if (w.project.isEmpty()) {
        // With no project, the panel offers to open one.
        PrKitBoxStyle empty{.gap = 8, .padding = prKitEdgesAll(12)};
        PrLayoutNode column = prKitBox(kit, box, {}, empty);
        prKitLabel(kit, column, {.text = "No folder is open."}, bodyStyle(PrKitTone_Muted));
        PrKitButton open{.text = "Open Folder…"};
        if (prKitActivated(kit, prKitButton(kit, column, open, {.variant = PrKitButtonVariant_Primary})))
            chooseProject(s, true);
        return;
    }
    w.projectRows.clear();
    w.projectItems.clear();
    visibleRows(w, 0, 0, w.projectRows, w.projectItems);
    PrKitTreeView content{
        .count = w.projectRows.count(),
        .selection = &w.projectCursor,
        .items = w.projectItems.data(),
        .opens = true,
        .revealsLead = w.projectReveals,
        .clickFolds = true,
    };
    w.projectReveals = false;
    PrKitListViewStyle style{.rowHeight = kRowHeight, .place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode view = prKitTreeView(kit, box, content, style);
    Scribe* scribe = &s;
    projectRows(&scribe, kit, view, prKitListWindow(kit, view));
}

// Declares the project tree's rows in its window, and applies what the round's input did to the tree. `context` holds
// the application.
void projectRows(void* context, PrKit* kit, PrLayoutNode view, PrKitWindow window) {
    Scribe& s = **static_cast<Scribe* const*>(context);
    Workspace& w = *s.workspace;
    const Array<uint32_t>& rows = w.projectRows;
    for (uint32_t i = window.first; i < window.first + window.count && i < rows.count(); ++i) {
        const ProjectNode& node = w.project[rows[i]];
        PrLayoutNode row = prKitTreeRow(kit, view, {.index = i, .item = rows[i], .selected = w.projectSelected == rows[i]});
        PrStudioIconKind icon = PrStudioIconKind_Script;
        if (node.folder)
            icon = node.open ? PrStudioIconKind_FolderOpen : PrStudioIconKind_Folder;
        else if (node.container)
            icon = PrStudioIconKind_Atlas;
        prKitRowIcon(kit, row, {.icon = icon});
        prKitRowLabel(kit, row, {.text = node.name}, PrKitTone_Primary);
        if (!node.failure.isEmpty())
            prKitRowMeta(kit, row, {.text = node.failure});
        // The row's context menu acts on its node.
        PrLayoutNode menu = prKitContextMenu(kit, row, {}, {});
        if (menu != kPrLayoutNodeNone) {
            w.menuNode = rows[i];
            PrStringView ids[] = {
                "project.open", "project.openSide", "", "path.copy", "path.copyRelative", "path.copyAddress", "",
                "path.reveal", "", "project.collapseAll",
            };
            for (PrStringView id : ids) {
                if (id.isEmpty())
                    prKitMenuSeparator(kit, menu, {});
                else
                    prCommandMenuItem(kit, menu, s.commands, s.keymap, id, {});
            }
            w.menuNode = kProjectNone;
        }
    }
    PrKitTreeFold fold{};
    if (prKitFolded(kit, view, &fold) && fold.row < rows.count())
        setOpen(w, rows[fold.row], fold.open);
    // Choosing a row selects it, and a file shows in the preview while focus stays in the tree; a folder folds on a
    // click, which the tree reports as a fold. Opening a row, by Enter or a double click, keeps its tab and gives its
    // view focus.
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && w.projectCursor.lead < rows.count()) {
        uint32_t chosen = rows[w.projectCursor.lead];
        w.projectSelected = chosen;
        const ProjectNode& node = w.project[chosen];
        if (!node.address.isEmpty())
            openAddress(s, String(node.address), OpenMode_Preview);
    }
    uint32_t opened = kPrKitRowNone;
    if (prKitRowOpened(kit, view, &opened) && opened < rows.count()) {
        const ProjectNode& node = w.project[rows[opened]];
        if (!node.address.isEmpty()) {
            openAddress(s, String(node.address), OpenMode_Keep);
            w.focusView = true;
        }
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Tabs

ViewTab* PR_NULLABLE tabWithId(Workspace& w, uint64_t id) {
    for (ViewTab* tab : w.tabs) {
        if (tab->id == id)
            return tab;
    }
    return nullptr;
}

// Returns a new tab, in storage of its own.
ViewTab* newTab() {
    return create<ViewTab>();
}

// Frees a tab that has left the workspace, with its views, its document and the resources that it holds.
void freeTab(ViewTab* tab) {
    if (tab->document != nullptr)
        prTextEditorDocumentDestroy(tab->document);
    prImageViewDestroy(tab->image);
    prUsdViewDestroy(tab->usd);
    prGltfViewDestroy(tab->gltf);
    prPageViewDestroy(tab->page);
    prModelViewDestroy(tab->model);
    sourceLinksReset(tab->links);
    addressNodeRelease(tab->node);
    addressNodeRelease(tab->partial);
    destroy(tab);
}

// Frees a tab's views, and its document if it is not a file's.
void clearViews(ViewTab& tab) {
    ViewTab* views = newTab();
    if (tab.path.isEmpty()) {
        views->document = tab.document;
        tab.document = nullptr;
    }
    views->image = tab.image;
    views->usd = tab.usd;
    views->gltf = tab.gltf;
    views->page = tab.page;
    views->model = tab.model;
    tab.image = nullptr;
    tab.usd = nullptr;
    tab.gltf = nullptr;
    tab.page = nullptr;
    tab.model = nullptr;
    sourceLinksReset(tab.links);
    freeTab(views);
}

// Returns the text of a node's bytes.
PrStringView textOf(const AddressNode* node) {
    PrByteView bytes = prBlobBytes(addressNodeBytes(node));
    return PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size};
}

// The outline of a stage: its prims, depth first.
void stageOutline(ViewTab& tab, const PrUsdStage* stage) {
    const PrUsdPrim* prims = prUsdStagePrims(stage);
    uint32_t end = prims[kPrUsdPseudoRoot].end;
    Array<uint32_t> depths;
    for (uint32_t i = 0; i < end; ++i) {
        uint32_t depth = prims[i].parent == kPrUsdNone ? 0 : depths[prims[i].parent] + 1;
        depths.push(depth);
        PrTextBuffer label = {};
        prUsdViewPrimLabel(stage, i, &label);
        PrTextBuffer path = {};
        prUsdStagePrimPath(stage, i, &path);
        tab.outline.push(OutlineItem{
            .label = prTextBufferView(&label),
            .step = prTextBufferView(&path),
            .depth = depth,
            .value = i,
        });
        prTextBufferReset(&label);
        prTextBufferReset(&path);
    }
}

// The outline of Markdown: its ATX headings, each by its line (RFC 5147's `line=`).
void markdownOutline(ViewTab& tab, PrStringView text) {
    uint32_t line = 0;
    bool fenced = false;
    for (size_t at = 0; at < text.size; ++line) {
        size_t end = prStringFindByte(prStringFrom(text, at), '\n') + at;
        PrStringView current = prStringSlice(text, at, end - at);
        at = end + 1;
        if (prStringStartsWith(current, "```"))
            fenced = !fenced;
        if (fenced || !prStringStartsWith(current, "#"))
            continue;
        uint32_t level = 0;
        while (level < current.size && current.data[level] == '#')
            ++level;
        if (level > 6 || (level < current.size && current.data[level] != ' '))
            continue;
        tab.outline.push(OutlineItem{
            .label = prStringTrim(prStringFrom(current, level)),
            .step = format("line={}", line),
            .depth = level - 1,
            .value = line,
        });
    }
}

// The outline of a glTF asset: its scenes and their nodes.
void gltfOutline(ViewTab& tab, const PrGltf* gltf) {
    const PrGltfNode* nodes = prGltfNodes(gltf);
    auto addNode = [&](auto& visit, uint32_t index, uint32_t depth) -> void {
        const PrGltfNode& node = nodes[index];
        String label = node.name.size != 0 ? String(node.name) : format("node {}", index);
        tab.outline.push(OutlineItem{.label = label, .step = format("/nodes/{}", index), .depth = depth, .value = index});
        const uint32_t* children = prGltfListIndices(gltf, node.children);
        for (uint32_t i = 0; i < node.children.count && depth < 64; ++i)
            visit(visit, children[i], depth + 1);
    };
    for (uint32_t i = 0; i < prGltfSceneCount(gltf); ++i) {
        const PrGltfScene& scene = prGltfScenes(gltf)[i];
        String label = scene.name.size != 0 ? String(scene.name) : format("scene {}", i);
        tab.outline.push(OutlineItem{.label = label, .step = format("/scenes/{}", i), .depth = 0, .value = i});
        const uint32_t* roots = prGltfListIndices(gltf, scene.nodes);
        for (uint32_t j = 0; j < scene.nodes.count; ++j)
            addNode(addNode, roots[j], 1);
    }
}

// The views that a format offers, the first of which a tab opens in.
Array<ViewMode> modesOf(PrMimeFormat format) {
    Array<ViewMode> modes;
    PrMimeFormatInfo info = prMimeFormatInfo(format);
    bool gltf = format == PrMimeFormat_Gltf || format == PrMimeFormat_Glb;
    bool usd = format == PrMimeFormat_Usda || format == PrMimeFormat_Usdc || format == PrMimeFormat_Usdz;
    if (gltf || usd) {
        modes.push(ViewMode_Preview);
        modes.push(ViewMode_Structure);
    }
    if (info.kind == PrMimeKind_Image && format != PrMimeFormat_Svg)
        modes.push(ViewMode_Image);
    if (info.text)
        modes.push(ViewMode_Source);
    if (format == PrMimeFormat_Markdown || format == PrMimeFormat_Html)
        modes.push(ViewMode_Preview);
    if (modes.isEmpty() || format == PrMimeFormat_Gltf || format == PrMimeFormat_Glb)
        modes.push(ViewMode_Info);
    return modes;
}

// Returns the bytes of what a preview's URL names, through the workspace's resolver, or null.
PrBlob* PR_NULLABLE readPreviewed(Workspace& w, PrStringView url) {
    String address = pageAddress(url);
    AddressNode* node = nullptr;
    if (address.isEmpty() || addressResolveText(w.resolver, address, &node) != PrStatus_Ok)
        return nullptr;
    PrBlob* blob = addressNodeBytes(node);
    if (blob != nullptr)
        prBlobRetain(blob);
    addressNodeRelease(node);
    return blob;
}

// A request of a preview and its answer, which the network's loop gives: the bytes of a resource and their type, or
// none, or a navigation's refusal.
struct PreviewAnswer {
    PrFetchInterceptRequest* request;
    PrBlob* PR_NULLABLE blob;
    String type;
    bool refused;
};

void respondPreview(void* context) {
    PreviewAnswer* answer = static_cast<PreviewAnswer*>(context);
    if (answer->refused) {
        prFetchInterceptRequestFail(answer->request, PrNetError_Blocked);
    } else if (answer->blob == nullptr) {
        PrFetchInterceptResponse missing{.status = 404, .type = PrFetchResponseType_Basic};
        prFetchInterceptRequestRespond(answer->request, &missing);
    } else {
        PrFetchHeader type{"Content-Type", answer->type};
        PrFetchInterceptResponse response{
            .status = 200,
            .type = PrFetchResponseType_Basic,
            .headers = &type,
            .headerCount = answer->type.isEmpty() ? 0u : 1u,
            .body = prBlobBytes(answer->blob),
        };
        prFetchInterceptRequestRespond(answer->request, &response);
    }
    prBlobRelease(answer->blob);
    destroy(answer);
}

// Answers the requests of the previews that wait: a resource by its bytes, and a navigation, a link that the user
// followed, by opening what it names in a tab, or in the system's handler for a URL outside the files on disk.
void answerPreviews(Scribe& s) {
    Workspace& w = *s.workspace;
    prMutexLock(&w.mutex);
    Array<PrFetchInterceptRequest*> requests = static_cast<Array<PrFetchInterceptRequest*>&&>(w.requests);
    w.requests = Array<PrFetchInterceptRequest*>();
    prMutexUnlock(&w.mutex);
    for (PrFetchInterceptRequest* request : requests) {
        PreviewAnswer* answer = create<PreviewAnswer>();
        answer->request = request;
        answer->blob = nullptr;
        answer->refused = request->desc.mode == PrFetchMode_Navigate;
        String url(request->desc.url);
        if (!answer->refused) {
            answer->blob = readPreviewed(w, url);
            if (answer->blob != nullptr)
                answer->type = prMimeFormatInfo(prMimeSniff(prBlobBytes(answer->blob), url)).type;
        }
        prNetPost(w.net, respondPreview, answer);
        if (!answer->refused)
            continue;
        String address = pageAddress(url);
        if (!address.isEmpty())
            openAddress(s, address, OpenMode_Keep);
        else if (s.app != nullptr)
            prAppOpenUrl(s.app, url);
    }
}

// Makes the preview of a document of Markdown or HTML at `address`, in the window's sans-serif faces.
PrPageView* previewOf(Scribe& s, PrStringView address, PrMimeFormat format, PrByteView bytes) {
    PrFontFamily sans{.generic = PrFontGeneric_SansSerif};
    PrTextFontDesc fontDesc{.set = s.fonts, .families = &sans, .familyCount = 1, .style = prFontStyleDefault()};
    PrTextFont* font = prTextFontCreate(&fontDesc);
    PrPageViewDesc desc{
        .kit = s.kit,
        .font = font,
        .shaper = s.shaper,
        .images = s.workspace->images,
        .compress = s.workspace->compress,
        .fetcher = s.workspace->fetcher,
    };
    PrPageView* page = prPageViewCreate(&desc);
    prTextFontRelease(font);
    PrStringView type = format == PrMimeFormat_Markdown ? "text/markdown; charset=utf-8" : "text/html";
    prPageViewLoad(page, pageUrl(address), type, bytes);
    return page;
}

// Resolves a tab's address again, and makes its views of what it reaches. A step that does not resolve leaves the tab
// open, showing what is missing.
void resolveTab(Scribe& s, ViewTab& tab) {
    Workspace& w = *s.workspace;
    addressNodeRelease(tab.node);
    tab.node = nullptr;
    addressNodeRelease(tab.partial);
    tab.partial = nullptr;
    tab.outline.clear();
    Address address;
    tab.status = addressParse(tab.address, &address);
    tab.failedStep = kAddressStepNone;
    if (tab.status == PrStatus_Ok)
        tab.status = addressResolve(w.resolver, address, &tab.node, &tab.failedStep);
    if (tab.status != PrStatus_Ok) {
        clearViews(tab);
        if (tab.failedStep != kAddressStepNone) {
            address.steps.resize(tab.failedStep);
            addressResolve(w.resolver, address, &tab.partial);
        }
        return;
    }
    AddressNode* node = tab.node;
    if (addressNodeParent(node) != nullptr)
        tab.title = addressNodeName(node);
    bool first = tab.format == PrMimeFormat_None;
    tab.format = addressNodeFormat(node);
    Array<ViewMode> modes = modesOf(tab.format);
    if (first || modes.find(tab.mode) == kArrayMissing)
        tab.mode = modes[0];
    clearViews(tab);
    PrMimeFormatInfo info = prMimeFormatInfo(tab.format);
    if (info.text && tab.document == nullptr) {
        // A file on disk is the editor's to edit and save, and a nested resource is read only.
        PrTextEditorDocumentDesc desc{.mediaType = info.type, .keepFocus = true};
        if (!tab.path.isEmpty()) {
            desc.path = tab.path;
            tab.document = prTextEditorDocumentFromPath(s.editor, tab.path);
        } else {
            desc.text = textOf(node);
            desc.readOnly = true;
        }
        if (tab.document == nullptr)
            prTextEditorDocumentCreate(s.editor, &desc, &tab.document);
    }
    if (info.kind == PrMimeKind_Image) {
        PrImageViewDesc desc{.kit = s.kit, .codec = w.images};
        tab.image = prImageViewCreate(&desc);
        prImageViewSetBytes(tab.image, addressNodeBytes(node));
    }
    if (const PrUsdStage* stage = usdAddressStage(node)) {
        tab.usd = prUsdViewCreate(s.kit);
        stageOutline(tab, stage);
        PrModelViewDesc desc{.kit = s.kit, .window = s.window};
        tab.model = prModelViewCreate(&desc);
        // The node's stage is the source, which opens the images that its layers name through the node's resolver.
        PrUsdModelOptions source{.open = usdAddressOpen, .openContext = node, .codec = w.compress};
        PrModelOptions options = prModelHostModelOptions(w.models);
        prModelViewSetModel(tab.model, prModelCreate(prUsdModelSourceCreate(stage, &source), &options));
    } else if (const PrGltf* gltf = gltfAddressDocument(node)) {
        gltfOutline(tab, gltf);
        tab.gltf = prGltfViewCreate(s.kit);
        PrModelViewDesc desc{.kit = s.kit, .window = s.window};
        tab.model = prModelViewCreate(&desc);
        PrModelOptions options = prModelHostModelOptions(w.models);
        prModelViewSetModel(tab.model, prModelCreate(prGltfModelSourceCreate(gltf, nullptr), &options));
    } else if (tab.format == PrMimeFormat_Markdown) {
        markdownOutline(tab, textOf(node));
    }
    if ((tab.format == PrMimeFormat_Markdown || tab.format == PrMimeFormat_Html) && s.shaper != nullptr)
        tab.page = previewOf(s, tab.address, tab.format, prBlobBytes(addressNodeBytes(node)));
}

// Closes a tab, and frees it.
void closeTab(Scribe& s, uint64_t id) {
    Workspace& w = *s.workspace;
    for (uint32_t i = 0; i < w.tabs.count(); ++i) {
        if (w.tabs[i]->id != id)
            continue;
        freeTab(w.tabs[i]);
        prKitDockRemovePanel(w.dock, id);
        w.tabs.erase(i);
        if (w.current == id)
            w.current = 0;
        return;
    }
}

// Tells the history the place that the current tab shows, if the tab or its selection changed since it last did.
void visit(Workspace& w) {
    const ViewTab* tab = tabWithId(w, w.current);
    if (tab == nullptr || (tab->id == w.visitedTab && tab->selection == w.visitedSelection))
        return;
    w.visitedTab = tab->id;
    w.visitedSelection = tab->selection;
    String place = tab->selection.isEmpty() ? tab->address : addressWithStep(tab->address, tab->selection);
    prKitHistoryVisit(w.history, place);
}

// Follows the changes of the files that the tabs and the project hold: each tab whose resource is stale resolves again,
// except a file on disk whose document the editor follows itself.
void follow(Scribe& s) {
    Workspace& w = *s.workspace;
    double now = nowSeconds();
    prUiRequestFrameAt(s.ui, now + kPollSeconds);
    if (now - w.polled < kPollSeconds)
        return;
    w.polled = now;
    addressResolverPoll(w.resolver);
    for (ViewTab* each : w.tabs) {
        ViewTab& tab = *each;
        // A missing resource resolves again once what holds it changes, or at each poll if its file is what is missing.
        bool stale = tab.node != nullptr && addressNodeIsStale(tab.node);
        bool missing = tab.node == nullptr && (tab.partial == nullptr || addressNodeIsStale(tab.partial));
        if ((stale || missing) && (tab.path.isEmpty() || !prMimeFormatInfo(tab.format).text || missing))
            resolveTab(s, tab);
    }
    refreshProject(w);
}

// ---------------------------------------------------------------------------------------------------------------------
// Views

// An address shown in a view: mono, wrapping anywhere so that a long one stays in its pane.
PrKitTextStyle addressStyle() {
    return PrKitTextStyle{.role = PrKitTextRole_Mono, .tone = PrKitTone_Muted, .wrap = PrKitWrap_Wrap};
}

// What is missing of an address that does not resolve: the step that failed and why.
void buildMissing(Scribe& s, PrLayoutNode parent, const ViewTab& tab) {
    PrKitBoxStyle column{.gap = 6, .padding = prKitEdgesAll(16)};
    PrLayoutNode box = prKitBox(s.kit, parent, {}, column);
    prKitLabel(s.kit, box, {.text = "Missing"}, {.role = PrKitTextRole_Heading});
    String why;
    Address address;
    if (tab.failedStep == kAddressStepNone || addressParse(tab.address, &address) != PrStatus_Ok ||
            tab.failedStep >= address.steps.count()) {
        why = format("The file cannot be read ({}).", prStatusName(tab.status));
    } else {
        why = format("Step {} of the address, {}, does not resolve ({}).", tab.failedStep + 1,
            address.steps[tab.failedStep], prStatusName(tab.status));
    }
    prKitLabel(s.kit, box, {.text = why}, bodyStyle());
    prKitLabel(s.kit, box, {.text = displayAddress(s, tab.address)}, addressStyle());
    prKitLabel(s.kit, box, {.text = "The tab shows the resource again once its file holds it."}, bodyStyle());
}

// What the classifier found, and what a glTF asset holds.
void buildInfo(Scribe& s, PrLayoutNode parent, const ViewTab& tab) {
    PrKitBoxStyle column{.gap = 6, .padding = prKitEdgesAll(16)};
    PrLayoutNode box = prKitBox(s.kit, parent, {}, column);
    PrMimeFormatInfo info = prMimeFormatInfo(tab.format);
    prKitLabel(s.kit, box, {.text = addressNodeName(tab.node)}, {.role = PrKitTextRole_Heading});
    PrByteView bytes = prBlobBytes(addressNodeBytes(tab.node));
    String line = format("{} ({}), {} bytes", info.name, info.type, uint64_t(bytes.size));
    prKitLabel(s.kit, box, {.text = line}, bodyStyle());
    if (const PrGltf* gltf = gltfAddressDocument(tab.node)) {
        String counts = format("{} scenes, {} nodes, {} meshes, {} materials, {} images, {} animations",
            prGltfSceneCount(gltf), prGltfNodeCount(gltf), prGltfMeshCount(gltf), prGltfMaterialCount(gltf),
            prGltfImageCount(gltf), prGltfAnimationCount(gltf));
        prKitLabel(s.kit, box, {.text = counts}, bodyStyle());
    }
    if (!tab.selection.isEmpty()) {
        String selected = format("Selected: {}", tab.selection);
        prKitLabel(s.kit, box, {.text = selected}, {.role = PrKitTextRole_Mono, .tone = PrKitTone_Secondary});
    }
    prKitLabel(s.kit, box, {.text = displayAddress(s, tab.address)}, addressStyle());
}

// Returns the node of the model at the address that a tab's selection names, or kPrModelNodeNone: the node at the
// address itself, or the node of the address of a property that it names, as a USD prim's `/World/Car.points`.
PrModelNodeIndex selectedNode(const ViewTab& tab) {
    const PrModel* model = tab.model != nullptr ? prModelViewModel(tab.model) : nullptr;
    if (model == nullptr)
        return kPrModelNodeNone;
    PrModelNodeIndex node = prModelNodeFromAddress(model, tab.selection);
    if (node != kPrModelNodeNone)
        return node;
    PrStringView step = tab.selection;
    size_t slash = prStringFindLastByte(step, '/');
    size_t dot = prStringFindLastByte(step, '.');
    if (dot != step.size && (slash == step.size || dot > slash))
        step = PrStringView{step.data, dot};
    return prModelNodeFromAddress(model, step);
}

// The preview of Markdown or HTML, the document as lib/html renders it, or of a model's asset, drawn in 3D with the
// node that the tab selects highlighted. A click on the asset selects the node under it, by its address in the asset,
// or nothing.
void buildPreview(Scribe& s, PrLayoutNode parent, ViewTab& tab) {
    (void)s;
    if (tab.page != nullptr)
        prPageViewFrame(tab.page, parent);
    if (tab.model != nullptr) {
        prModelViewSetSelection(tab.model, selectedNode(tab));
        prModelViewFrame(tab.model, parent);
        PrModelPick pick{};
        const PrModel* model = prModelViewModel(tab.model);
        if (prModelViewPicked(tab.model, &pick)) {
            tab.selection = String();
            if (pick.node != kPrModelNodeNone && model != nullptr) {
                PrTextBuffer address = {};
                prModelNodeAddress(model, pick.node, &address);
                tab.selection = String(prTextBufferView(&address));
                prTextBufferReset(&address);
            }
        }
    }
}

PrStringView modeName(ViewMode mode) {
    switch (mode) {
        case ViewMode_Source:
            return "Source";
        case ViewMode_Preview:
            return "Preview";
        case ViewMode_Structure:
            return "Structure";
        case ViewMode_Image:
            return "Image";
        case ViewMode_Info:
            return "Info";
        default:
            break;
    }
    return "";
}

// Returns the prim of a stage that a tab's selection names, or the pseudo-root.
PrUsdPrimIndex selectedPrim(const ViewTab& tab, const PrUsdStage* stage) {
    if (tab.selection.isEmpty())
        return kPrUsdPseudoRoot;
    PrStringView step = tab.selection;
    size_t dot = prStringFindLastByte(step, '.');
    size_t slash = prStringFindLastByte(step, '/');
    if (dot != step.size && (slash == step.size || dot > slash))
        step = prStringHead(step, dot);
    PrUsdPrimIndex prim = prUsdStageFindProxy(stage, step);
    return prim != kPrUsdNone ? prim : kPrUsdPseudoRoot;
}

// Returns the property of `prim` that a tab's selection names after the prim's path, as a link to a property does, or
// kPrUsdNone.
uint32_t selectedProperty(const ViewTab& tab, const PrUsdStage* stage, PrUsdPrimIndex prim) {
    PrStringView step = tab.selection;
    size_t dot = prStringFindLastByte(step, '.');
    size_t slash = prStringFindLastByte(step, '/');
    if (dot == step.size || (slash != step.size && dot < slash))
        return kPrUsdNone;
    PrStringView name = prStringFrom(step, dot + 1);
    const PrUsdPrim& record = prUsdStagePrims(stage)[prim];
    for (uint32_t i = 0; i < record.propertyCount; ++i) {
        if (prUsdStageProperties(stage)[record.firstProperty + i].name == name)
            return record.firstProperty + i;
    }
    return kPrUsdNone;
}

// Returns the extensions of Khronos's material model that the model reports it does not draw (prModelDiagnostic).
PrGltfMaterialExtensionFlags unshadedExtensions(const PrModel* PR_NULLABLE model) {
    PrGltfMaterialExtensionFlags flags = 0;
    for (uint32_t i = 0; model != nullptr && i < prModelDiagnosticCount(model); ++i)
        flags |= prGltfMaterialExtensionFromName(prModelDiagnostic(model, i).feature);
    return flags;
}

// The structure view of a glTF asset, which shows the tab's selection. A link that the user follows opens the asset's
// address with its pointer as the last step: another item selects it, and an image, a buffer view or a buffer opens in
// a tab of its own.
void buildGltfStructure(Scribe& s, PrLayoutNode parent, ViewTab& tab, const PrJson* json) {
    const PrModel* model = tab.model != nullptr ? prModelViewModel(tab.model) : nullptr;
    PrGltfViewContent content{
        .json = json,
        .gltf = gltfAddressDocument(tab.node),
        .selection = tab.selection,
        .unshaded = unshadedExtensions(model),
    };
    prGltfViewFrame(tab.gltf, parent, content);
    PrRawString link = prRawStringNull();
    if (prGltfViewFollowed(tab.gltf, &link)) {
        PrStringView pointer = prRawStringView(&link);
        String address = pointer.size != 0 ? addressWithStep(tab.address, pointer) : String(tab.address);
        openAddress(s, address, OpenMode_Keep);
    }
    prRawStringReset(&link);
}

// Links the text of a .gltf or .usda tab with its selection: a caret that moves into another item of the structure
// selects that item, and a selection that the text has not shown yet is selected in the text.
void linkSource(Scribe& s, ViewTab& tab) {
    SourceLinks& links = tab.links;
    if (!sourceLinksFormat(tab.format) || prTextEditorShown(s.editor) != tab.document ||
            !sourceLinksUpdate(links, tab.format, prTextEditorDocumentBuffer(tab.document)))
        return;
    uint32_t caret = prTextEditViewPrimary(prTextViewEdit(prTextEditorDocumentView(tab.document))).caret;
    if (caret != links.caret) {
        links.caret = caret;
        String item = sourceItemAt(links, caret);
        if (!item.isEmpty() && item != tab.selection) {
            tab.selection = item;
            tab.property = kPrUsdNone;
            links.revealed = item;
        }
    }
    if (tab.selection == links.revealed)
        return;
    links.revealed = tab.selection;
    uint32_t begin = 0;
    uint32_t end = 0;
    if (!sourceItemSpan(links, tab.selection, &begin, &end))
        return;
    // The selection puts the caret at the item's start, which is in the item, so the caret names it again.
    prTextEditorShowRange(s.editor, tab.document, begin, end, false);
    links.caret = begin;
    prUiRequestFrame(s.ui);
}

void buildView(Scribe& s, PrLayoutNode parent, ViewTab& tab) {
    Workspace& w = *s.workspace;
    PrKit* kit = s.kit;
    if (tab.node == nullptr) {
        buildMissing(s, parent, tab);
        return;
    }
    Array<ViewMode> modes = modesOf(tab.format);
    if (modes.count() > 1) {
        PrKitBoxStyle bar{
            .direction = PrKitDirection_Row,
            .align = PrKitAlign_Center,
            .padding = prKitEdgesAll(3),
            .place = {.height = prKitPx(kModeHeight), .flex = PrKitFlex_Fixed},
        };
        PrLayoutNode line = prKitBox(kit, parent, {}, bar);
        PrLayoutNode segmented = prKitSegmented(kit, line, {}, {.size = PrKitControlSize_Sm});
        for (ViewMode mode : modes) {
            PrLayoutNode segment = prKitSegment(kit, segmented, {.label = modeName(mode), .selected = tab.mode == mode});
            if (prKitActivated(kit, segment))
                tab.mode = mode;
        }
    }
    // The view takes the room under the bar of its modes, which layout gives it.
    PrKitBoxStyle holder{.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .flex = PrKitFlex_Fill}};
    PrLayoutNode view = prKitBox(kit, parent, {}, holder);
    switch (tab.mode) {
        case ViewMode_Source:
            if (tab.document != nullptr) {
                if (prTextEditorShown(s.editor) != tab.document || w.focusView)
                    prTextEditorShow(s.editor, tab.document, w.focusView);
                PrVec2 extent{};
                prUiExtent(s.ui, view, PrUiAxisFlag_Both, &extent);
                prTextEditorFrame(s.editor, view, extent);
                linkSource(s, tab);
            }
            break;
        case ViewMode_Preview:
            buildPreview(s, view, tab);
            break;
        case ViewMode_Structure:
            if (const PrUsdStage* stage = usdAddressStage(tab.node)) {
                PrUsdPrimIndex prim = selectedPrim(tab, stage);
                if (tab.property == kPrUsdNone)
                    tab.property = selectedProperty(tab, stage, prim);
                uint32_t property = tab.property;
                prUsdViewFrame(tab.usd, view, {.stage = stage, .prim = prim, .property = &property});
                if (property != tab.property) {
                    tab.property = property;
                    // A chosen property is the selection, a location under the prim.
                    PrTextBuffer path = {};
                    prUsdStagePrimPath(stage, prim, &path);
                    if (property != kPrUsdNone) {
                        prTextBufferAppendChar(&path, '.');
                        prTextBufferAppend(&path, prUsdStageProperties(stage)[property].name);
                    }
                    tab.selection = prTextBufferView(&path);
                    prTextBufferReset(&path);
                }
            } else if (const PrJson* json = gltfAddressJson(tab.node); json != nullptr && tab.gltf != nullptr) {
                buildGltfStructure(s, view, tab, json);
            }
            break;
        case ViewMode_Image:
            prImageViewFrame(tab.image, view);
            break;
        case ViewMode_Info:
        default:
            buildInfo(s, view, tab);
            break;
    }
    w.focusView = false;
}

// What the outline declares its rows with: the application, and the tab whose outline it shows.
struct OutlineRows {
    Scribe* scribe;
    uint64_t tab;
};

void outlineRows(void* context, PrKit* kit, PrLayoutNode view, PrKitWindow window);

// Declares the outline panel in its pane, `box`: the outline of the current tab.
void buildOutline(Scribe& s, PrLayoutNode box) {
    Workspace& w = *s.workspace;
    PrKit* kit = s.kit;
    ViewTab* tab = shownTab(s);
    if (tab == nullptr || tab->outline.isEmpty()) {
        PrKitEmptyState empty{.title = tab == nullptr ? "No tab is open." : "The tab has no outline."};
        prKitEmptyState(kit, box, empty);
        return;
    }
    Array<PrKitTreeItem>& items = w.outlineItems;
    items.clear();
    for (uint32_t i = 0; i < tab->outline.count(); ++i) {
        bool parentOfNext = i + 1 < tab->outline.count() && tab->outline[i + 1].depth > tab->outline[i].depth;
        items.push(PrKitTreeItem{.depth = tab->outline[i].depth, .hasChildren = parentOfNext, .open = true});
    }
    // The outline of each tab is a view of its own, so that its scroll and its rows never pass to another tab's.
    PrKitTreeView content{
        .key = PrUiKey(tab->id),
        .count = items.count(),
        .selection = &tab->outlineCursor,
        .items = items.data(),
        .opens = true,
    };
    PrKitListViewStyle style{.rowHeight = kRowHeight, .place = {.flex = PrKitFlex_Fill}};
    PrLayoutNode view = prKitTreeView(kit, box, content, style);
    OutlineRows rows{.scribe = &s, .tab = tab->id};
    outlineRows(&rows, kit, view, prKitListWindow(kit, view));
}

// Declares the outline's rows in its window, and applies what the round's input did to the outline. `context` holds the
// OutlineRows of the tab that the outline shows.
void outlineRows(void* context, PrKit* kit, PrLayoutNode view, PrKitWindow window) {
    const OutlineRows& rows = *static_cast<const OutlineRows*>(context);
    Scribe& s = *rows.scribe;
    Workspace& w = *s.workspace;
    ViewTab* tab = tabWithId(w, rows.tab);
    if (tab == nullptr)
        return;
    for (uint32_t i = window.first; i < window.first + window.count && i < tab->outline.count(); ++i) {
        const OutlineItem& item = tab->outline[i];
        // The selection of the tab marks its item, as a property marks its prim.
        bool selected = tab->selection == item.step ||
            (prStringStartsWith(tab->selection, item.step) && tab->selection.count() > item.step.count() &&
                tab->selection.data()[item.step.count()] == '.');
        PrLayoutNode row = prKitTreeRow(kit, view, {.index = i, .item = i, .selected = selected});
        prKitRowLabel(kit, row, {.text = item.label}, PrKitTone_Primary);
    }
    PrKitSelectionEdit edit{};
    if (prKitSelectionEdited(kit, view, &edit) && tab->outlineCursor.lead < tab->outline.count()) {
        const OutlineItem& item = tab->outline[tab->outlineCursor.lead];
        tab->selection = item.step;
        tab->property = kPrUsdNone;
        if (tab->document != nullptr && prStringStartsWith(item.step, "line="))
            prTextEditorShowAt(s.editor, tab->document, item.value, 0, false);
    }
    // Opening an item gives the tab's view focus there.
    uint32_t opened = kPrKitRowNone;
    if (prKitRowOpened(kit, view, &opened) && opened < tab->outline.count()) {
        const OutlineItem& item = tab->outline[opened];
        if (tab->document != nullptr && prStringStartsWith(item.step, "line="))
            prTextEditorShowAt(s.editor, tab->document, item.value, 0, true);
        w.focusView = true;
    }
}

// What a panel's tab says: the project's and the outline's names, and a resource's title.
PrKitDockTab describePanel(void* context, PrKitDockPanelId panel) {
    Scribe& s = *static_cast<Scribe*>(context);
    if (panel == kPanelProject)
        return PrKitDockTab{.title = "Project"};
    if (panel == kPanelOutline)
        return PrKitDockTab{.title = "Outline"};
    if (panel == kPanelEditor) {
        PrTextEditorDocument* document = prTextEditorShown(s.editor);
        PrStringView path = document != nullptr ? prTextEditorDocumentPath(document) : PrStringView{};
        bool edited = document != nullptr && prTextEditorDocumentInfo(document).edited;
        return PrKitDockTab{.title = path.isEmpty() ? PrStringView("untitled") : prFilePathName(path), .dirty = edited};
    }
    ViewTab* tab = tabWithId(*s.workspace, panel);
    if (tab == nullptr)
        return PrKitDockTab{};
    bool edited = tab->document != nullptr && prTextEditorDocumentInfo(tab->document).edited;
    return PrKitDockTab{.title = tab->title, .tooltip = tab->tooltip, .dirty = edited};
}

// Declares the pane of a tab. The editor shows one document at a time, so a text tab that is not current says where its
// text is.
void buildTabPane(Scribe& s, PrLayoutNode content, ViewTab& tab) {
    Workspace& w = *s.workspace;
    if (tab.id != w.current && tab.mode == ViewMode_Source && tab.document != nullptr) {
        PrKitEmptyState note{.title = tab.title, .hint = "Its text shows in the current tab's pane. Click to show it here."};
        PrLayoutNode box = prKitEmptyState(s.kit, content, note);
        prUiSetFlags(s.ui, box, PrUiFlag_Pressable);
        if (prUiClicked(s.ui, box, 0))
            w.current = tab.id;
        return;
    }
    buildView(s, content, tab);
}

// The title of a tab: the last part of the resource's address, and its container's name for a nested resource.
String titleOf(PrStringView text) {
    Address address;
    if (addressParse(text, &address) != PrStatus_Ok)
        return String(text);
    bool nested = !address.steps.isEmpty();
    PrStringView last = nested ? PrStringView(address.steps.last()) : PrStringView(address.url);
    size_t slash = prStringFindLastByte(last, '/');
    String title = slash == last.size || slash + 1 == last.size ? String(last) : String(prStringFrom(last, slash + 1));
    if (!nested) {
        PrRawString path = prRawStringNull();
        if (prUrlToFilePath(address.url, &path))
            title = prFilePathName(prRawStringView(&path));
        prRawStringReset(&path);
    }
    return title;
}

// The tooltip of a tab: where the resource is, as the project names it, with each step of a nested resource after its
// container's, and the whole address below.
String tooltipOf(const Scribe& s, PrStringView text) {
    Address address;
    if (addressParse(text, &address) != PrStatus_Ok)
        return String(text);
    String tooltip = displayAddress(s, address.url);
    for (const String& step : address.steps)
        formatTo(tooltip, "  \u203a  {}", step);
    if (tooltip != text)
        formatTo(tooltip, "\n{}", text);
    return tooltip;
}

// Opens the resource that `text` names in a tab, or shows the tab that shows it, as openAddress does, without telling
// the history.
PrStatus openTab(Scribe& s, PrStringView text, OpenMode mode) {
    PR_ASSERT(s.workspace != nullptr);
    Workspace& w = *s.workspace;
    Address address;
    PrStatus status = addressParse(text, &address);
    if (status != PrStatus_Ok)
        return status;
    // An address that ends in a location opens its resource, and selects the location.
    AddressNode* node = nullptr;
    status = addressResolve(w.resolver, address, &node);
    String selection;
    if (status == PrStatus_Ok && addressNodeKind(node) == AddressNodeKind_Location) {
        selection = address.steps.last();
        address.steps.pop();
    }
    addressNodeRelease(node);
    String resource = addressText(address);
    PrRawString path = prRawStringNull();
    bool onDisk = address.steps.isEmpty() && prUrlToFilePath(address.url, &path);
    for (ViewTab* open : w.tabs) {
        if (open->address != resource)
            continue;
        prKitDockOpenPanel(w.dock, open->id);
        if (mode != OpenMode_Preview)
            prKitDockKeep(w.dock, open->id);
        w.current = open->id;
        if (!selection.isEmpty())
            open->selection = selection;
        prRawStringReset(&path);
        return status;
    }
    ViewTab* tab = newTab();
    tab->id = w.nextTab++;
    tab->address = resource;
    tab->title = titleOf(resource);
    tab->tooltip = tooltipOf(s, resource);
    tab->selection = selection;
    if (onDisk)
        tab->path = prRawStringView(&path);
    prRawStringReset(&path);
    w.tabs.push(tab);
    resolveTab(s, *tab);
    uint64_t id = tab->id;
    PrStatus resolved = tab->status;
    // A tab opens in the leaf of the current tab, or in the primary leaf. A preview takes over the leaf's preview.
    PrKitDockNode leaf = prKitDockPanelLeaf(w.dock, w.current);
    if (leaf == kPrKitDockNodeNone)
        leaf = prKitDockPrimary(w.dock);
    if (mode == OpenMode_Preview) {
        PrKitDockPanelId replaced = prKitDockAddPreview(w.dock, leaf, id);
        if (replaced != 0)
            closeTab(s, replaced);
    } else if (mode == OpenMode_Side) {
        prKitDockSplit(w.dock, leaf, PrKitDockEdge_Right, id, 0.5f);
    } else {
        prKitDockAddPanel(w.dock, leaf, id);
    }
    w.current = id;
    return resolved;
}

Scribe& scribeOf(void* context) {
    return *static_cast<Scribe*>(context);
}

// Returns the camera of a model view that `name` names: one more than the index of the model's camera whose node has
// that name or address, or 0, the orbit camera, for a name that no camera's node has.
uint32_t cameraNamed(const PrModel* model, PrStringView name) {
    PrTextBuffer path = {};
    uint32_t found = 0;
    for (uint32_t i = 0; found == 0 && name.size != 0 && i < prModelCameraCount(model); ++i) {
        PrModelNodeIndex node = prModelCameraNode(model, i);
        prTextBufferClear(&path);
        prModelNodeAddress(model, node, &path);
        if (prModelNodeName(model, node) == name || prTextBufferView(&path) == name)
            found = i + 1;
    }
    prTextBufferReset(&path);
    return found;
}

// Removes a panel from the dock, or adds it back at `edge` of the tabs.
void togglePanel(Workspace& w, PrKitDockPanelId panel, PrKitDockEdge edge) {
    if (!prKitDockRemovePanel(w.dock, panel))
        prKitDockSplit(w.dock, prKitDockPrimary(w.dock), edge, panel, 0.2f);
}

// Adds one of the workspace's commands, bound to `chord` if it is not empty, whose state `state` reports if it is set.
void addCommand(Scribe& s, const char* id, const char* title, const char* chord, PrCommandRunFn run,
        PrCommandStateFn PR_NULLABLE state = nullptr) {
    PrCommandDesc desc{.id = id, .title = title, .category = "Scribe", .run = run, .state = state, .context = &s};
    prCommandRegistryAdd(s.commands, &desc);
    PrCommandChord parsed{};
    if (chord[0] != 0 && prCommandChordParse(chord, &parsed))
        prCommandKeymapBind(s.keymap, parsed, id, "", PrCommandBindingKind_Normal);
}

// ---------------------------------------------------------------------------------------------------------------------
// The commands of the context menus

// The tab that a command acts on: the tab of the open context menu, or else the current tab.
PrKitDockPanelId targetTab(const Workspace& w) {
    return w.menuTab != 0 ? w.menuTab : w.current;
}

// What a command on a resource acts on: the node of the project whose menu is open, or else the target tab's resource.
// `address` is empty for a folder, and `path` for a resource that is not a file on disk. `outer` is the file on disk
// that holds the resource, or the resource itself.
struct Target {
    String address;
    String path;
    String outer;
};

Target targetOf(Scribe& s) {
    Workspace& w = *s.workspace;
    Target t;
    if (w.menuNode != kProjectNone && w.menuNode < w.project.count()) {
        const ProjectNode& node = w.project[w.menuNode];
        t.address = node.address;
        t.path = node.path;
    } else if (ViewTab* tab = tabWithId(w, targetTab(w))) {
        t.address = tab->address;
        t.path = tab->path;
    }
    t.outer = t.path;
    Address address;
    if (t.outer.isEmpty() && !t.address.isEmpty() && addressParse(t.address, &address) == PrStatus_Ok) {
        PrRawString path = prRawStringNull();
        if (prUrlToFilePath(address.url, &path))
            t.outer = prRawStringView(&path);
        prRawStringReset(&path);
    }
    return t;
}

void copyText(Scribe& s, PrStringView text) {
    if (s.app != nullptr && !text.isEmpty())
        prAppDataWriteText(s.app, kPrAppDataSourceClipboard, text);
}

// Closes the tabs of the target tab's leaf that `keep` does not keep, by their index in the leaf and whether the leaf's
// target tab is before them. Their panels leave the dock at its next build.
template <typename Keep>
void closeTabs(Scribe& s, Keep keep) {
    Workspace& w = *s.workspace;
    PrKitDockPanelId target = targetTab(w);
    PrKitDockNode leaf = prKitDockPanelLeaf(w.dock, target);
    bool after = false;
    for (uint32_t i = 0; i < prKitDockLeafTabCount(w.dock, leaf); ++i) {
        PrKitDockPanelId panel = prKitDockLeafTab(w.dock, leaf, i);
        if (!keep(panel, after))
            prKitDockClosePanel(w.dock, panel);
        after = after || panel == target;
    }
    prUiRequestFrame(s.ui);
}

// Returns true if the panel's tab shows a resource whose text has unsaved changes.
bool unsaved(Workspace& w, PrKitDockPanelId panel) {
    ViewTab* tab = tabWithId(w, panel);
    return tab != nullptr && tab->document != nullptr && prTextEditorDocumentInfo(tab->document).edited;
}

// Shows the target's file or folder in the project: opens the folders and containers that hold it, and selects it.
void revealInProject(Scribe& s) {
    Workspace& w = *s.workspace;
    Target t = targetOf(s);
    if (w.project.isEmpty() || t.outer.isEmpty())
        return;
    String root = w.root;
    root.append('/');
    if (!prStringStartsWith(t.outer, root))
        return;
    // The path from the project's folder, then the steps of a nested resource, which name its entries by path.
    String steps(prStringFrom(t.outer, root.count()));
    Address address;
    if (t.path.isEmpty() && addressParse(t.address, &address) == PrStatus_Ok) {
        for (const String& step : address.steps)
            formatTo(steps, "/{}", step);
    }
    uint32_t node = 0;
    for (PrStringView rest = steps; rest.size != 0 && node != kProjectNone;) {
        size_t slash = prStringFindByte(rest, '/');
        PrStringView name = prStringHead(rest, slash);
        rest = slash < rest.size ? prStringFrom(rest, slash + 1) : PrStringView{};
        if (!w.project[node].open)
            setOpen(w, node, true);
        uint32_t found = kProjectNone;
        for (uint32_t child : w.project[node].children) {
            if (w.project[child].name == name)
                found = child;
        }
        if (found == kProjectNone)
            break;
        node = found;
    }
    w.projectSelected = node;
    // The project's cursor follows the node's row, so that the keys move on from it, and the tree scrolls it into view.
    Array<uint32_t> rows;
    Array<PrKitTreeItem> items;
    visibleRows(w, 0, 0, rows, items);
    uint32_t row = rows.find(node);
    if (row != kArrayMissing) {
        w.projectCursor = PrKitSelection{.anchor = row, .lead = row};
        w.projectReveals = true;
    }
    if (prKitDockPanelLeaf(w.dock, kPanelProject) != kPrKitDockNodeNone)
        prKitDockOpenPanel(w.dock, kPanelProject);
}

// Closes a node of the project and every node under it.
void collapse(Workspace& w, uint32_t index) {
    for (uint32_t child : w.project[index].children)
        collapse(w, child);
    if (w.project[index].open)
        setOpen(w, index, false);
}

PrCommandStateFlags whenOnDisk(void* context) {
    return targetOf(scribeOf(context)).outer.isEmpty() ? PrCommandStateFlag_Disabled : 0;
}

PrCommandStateFlags whenPath(void* context) {
    return targetOf(scribeOf(context)).path.isEmpty() ? PrCommandStateFlag_Disabled : 0;
}

PrCommandStateFlags whenNamed(void* context) {
    Target t = targetOf(scribeOf(context));
    return t.path.isEmpty() && t.address.isEmpty() ? PrCommandStateFlag_Disabled : 0;
}

PrCommandStateFlags whenAddressed(void* context) {
    return targetOf(scribeOf(context)).address.isEmpty() ? PrCommandStateFlag_Disabled : 0;
}

// Adds the commands that the context menus of the tabs and of the project run, which also run from the palette on the
// current tab.
void addMenuCommands(Scribe& s) {
    addCommand(s, "tab.close", "Close Tab", "Cmd W", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        prKitDockClosePanel(s.workspace->dock, targetTab(*s.workspace));
        prUiRequestFrame(s.ui);
    });
    addCommand(s, "tab.closeOthers", "Close Others", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        PrKitDockPanelId target = targetTab(*s.workspace);
        closeTabs(s, [&](PrKitDockPanelId panel, bool) { return panel == target; });
    });
    addCommand(s, "tab.closeRight", "Close to the Right", "", [](void* c, const PrCommandArgs*) {
        closeTabs(scribeOf(c), [](PrKitDockPanelId, bool after) { return !after; });
    });
    addCommand(s, "tab.closeSaved", "Close Saved", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        closeTabs(s, [&](PrKitDockPanelId panel, bool) { return unsaved(*s.workspace, panel); });
    });
    addCommand(s, "tab.closeAll", "Close All", "", [](void* c, const PrCommandArgs*) {
        closeTabs(scribeOf(c), [](PrKitDockPanelId, bool) { return false; });
    });
    addCommand(s, "view.keepTab", "Keep Open", "", [](void* c, const PrCommandArgs*) {
        Workspace& w = *scribeOf(c).workspace;
        prKitDockKeep(w.dock, targetTab(w));
    }, [](void* c) -> PrCommandStateFlags {
        Workspace& w = *scribeOf(c).workspace;
        return prKitDockIsPreview(w.dock, targetTab(w)) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "path.copy", "Copy Path", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        copyText(s, targetOf(s).path);
    }, whenPath);
    addCommand(s, "path.copyRelative", "Copy Relative Path", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        Target t = targetOf(s);
        String root = s.workspace->root;
        root.append('/');
        if (!t.path.isEmpty() && prStringStartsWith(t.path, root))
            copyText(s, prStringFrom(t.path, root.count()));
        else
            copyText(s, displayAddress(s, t.address));
    }, whenNamed);
    addCommand(s, "path.copyAddress", "Copy Address", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        copyText(s, targetOf(s).address);
    }, whenAddressed);
    addCommand(s, "path.reveal", "Reveal in Finder", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        Target t = targetOf(s);
        if (s.app != nullptr && !t.outer.isEmpty())
            prAppRevealPath(s.app, t.outer);
    }, whenOnDisk);
    addCommand(s, "path.revealProject", "Reveal in Project", "", [](void* c, const PrCommandArgs*) {
        revealInProject(scribeOf(c));
    }, whenOnDisk);
    addCommand(s, "project.open", "Open", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        Target t = targetOf(s);
        if (!t.address.isEmpty()) {
            openAddress(s, t.address, OpenMode_Keep);
            s.workspace->focusView = true;
        }
    }, whenAddressed);
    addCommand(s, "project.openSide", "Open to the Side", "", [](void* c, const PrCommandArgs*) {
        Scribe& s = scribeOf(c);
        Target t = targetOf(s);
        if (!t.address.isEmpty())
            openAddress(s, t.address, OpenMode_Side);
    }, whenAddressed);
    addCommand(s, "project.collapseAll", "Collapse All", "", [](void* c, const PrCommandArgs*) {
        Workspace& w = *scribeOf(c).workspace;
        uint32_t node = w.menuNode != kProjectNone ? w.menuNode : 0;
        if (node >= w.project.count())
            return;
        // A file collapses what its folder holds.
        if (w.project[node].children.isEmpty() && w.project[node].parent != kProjectNone)
            node = w.project[node].parent;
        collapse(w, node);
        if (node == 0)
            setOpen(w, 0, true);
    });
}

// Creates the items of a resource's tab's context menu.
void tabMenu(void* context, PrKitDockPanelId panel, PrLayoutNode menu) {
    Scribe& s = scribeOf(context);
    Workspace& w = *s.workspace;
    w.menuTab = panel;
    PrStringView ids[] = {
        "tab.close", "tab.closeOthers", "tab.closeRight", "tab.closeSaved", "tab.closeAll", "", "view.keepTab", "",
        "path.copy", "path.copyRelative", "path.copyAddress", "", "path.reveal", "path.revealProject",
    };
    // A panel of the workspace's own closes, and has no resource.
    uint32_t count = panel >= kFirstTab ? uint32_t(countOf(ids)) : 1;
    for (uint32_t i = 0; i < count; ++i) {
        if (ids[i].isEmpty())
            prKitMenuSeparator(s.kit, menu, {});
        else
            prCommandMenuItem(s.kit, menu, s.commands, s.keymap, ids[i], {});
    }
    w.menuTab = 0;
}

} // namespace

void openWorkspace(Scribe& s) {
    PR_ASSERT(s.workspace == nullptr);
    Workspace* w = create<Workspace>();
    s.workspace = w;
    w->compress = prCompressCodecSetCreateDefault();
    w->images = prImageCodecSetCreateDefault(w->compress);
    // The model host gives the previews the formats of the models' textures: KTX 2.0 through lib/ktx, which transcodes
    // Basis Universal's blocks for the device, and every other image through the codec.
    w->pool = prTaskPoolCreate(PrTaskPoolBackend_Default, 3);
    PrModelHostDesc models{.images = w->images, .compress = w->compress, .pool = w->pool};
    w->models = prModelHostCreate(&models);
    w->handlers.push(zipAddressHandlerCreate(w->compress));
    w->handlers.push(usdAddressHandlerCreate(w->compress));
    w->handlers.push(gltfAddressHandlerCreate());
    if (w->images != nullptr)
        w->handlers.push(imageAddressHandlerCreate(w->images));
    // The resolver reads files on disk.
    AddressResolverDesc desc{.handlers = w->handlers.span()};
    w->resolver = addressResolverCreate(desc);
    prMutexInit(&w->mutex);
    w->net = prNetCreate(PrNetBackend_Default);
    PrFetchInterceptDesc fetch{
        .net = w->net,
        .request = [](void* context, PrFetchInterceptRequest* request) {
            Workspace& w = *static_cast<Workspace*>(context);
            prMutexLock(&w.mutex);
            w.requests.push(request);
            prMutexUnlock(&w.mutex);
        },
        .context = w,
    };
    w->fetcher = prFetchInterceptCreate(&fetch);
    // The tabs fill the dock's primary leaf, between the project and the outline.
    w->dock = prKitDockCreate();
    w->history = prKitHistoryCreate(kHistoryPlaces);
    PrKitDockNode tabs = prKitDockPrimary(w->dock);
    prKitDockSplit(w->dock, tabs, PrKitDockEdge_Left, kPanelProject, 0.2f);
    prKitDockSplit(w->dock, tabs, PrKitDockEdge_Right, kPanelOutline, 0.22f);
    addCommand(s, "file.openFolder", "Open Folder…", "Cmd Shift O", [](void* c, const PrCommandArgs*) {
        chooseProject(scribeOf(c), true);
    });
    addCommand(s, "file.openFile", "Open File…", "Cmd O", [](void* c, const PrCommandArgs*) {
        chooseProject(scribeOf(c), false);
    });
    addCommand(s, "view.toggleProject", "Toggle Project Panel", "Cmd B", [](void* c, const PrCommandArgs*) {
        togglePanel(*scribeOf(c).workspace, kPanelProject, PrKitDockEdge_Left);
    });
    addCommand(s, "view.toggleOutline", "Toggle Outline Panel", "", [](void* c, const PrCommandArgs*) {
        togglePanel(*scribeOf(c).workspace, kPanelOutline, PrKitDockEdge_Right);
    });
    addMenuCommands(s);
    addCommand(s, "go.back", "Back", "Cmd [", [](void* c, const PrCommandArgs*) {
        stepHistory(scribeOf(c), -1);
    }, [](void* c) -> PrCommandStateFlags {
        return prKitHistoryCanStep(scribeOf(c).workspace->history, -1) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "go.forward", "Forward", "Cmd ]", [](void* c, const PrCommandArgs*) {
        stepHistory(scribeOf(c), 1);
    }, [](void* c) -> PrCommandStateFlags {
        return prKitHistoryCanStep(scribeOf(c).workspace->history, 1) ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "view.structure", "Show Structure", "", [](void* c, const PrCommandArgs*) {
        ViewTab* tab = shownTab(scribeOf(c));
        if (tab != nullptr && modesOf(tab->format).find(ViewMode_Structure) != kArrayMissing)
            tab->mode = ViewMode_Structure;
    }, [](void* c) -> PrCommandStateFlags {
        ViewTab* tab = shownTab(scribeOf(c));
        bool shows = tab != nullptr && modesOf(tab->format).find(ViewMode_Structure) != kArrayMissing;
        return shows ? 0 : PrCommandStateFlag_Disabled;
    });
    addCommand(s, "view.source", "Show Source", "", [](void* c, const PrCommandArgs*) {
        ViewTab* tab = shownTab(scribeOf(c));
        if (tab != nullptr && modesOf(tab->format).find(ViewMode_Source) != kArrayMissing)
            tab->mode = ViewMode_Source;
    }, [](void* c) -> PrCommandStateFlags {
        ViewTab* tab = shownTab(scribeOf(c));
        bool shows = tab != nullptr && modesOf(tab->format).find(ViewMode_Source) != kArrayMissing;
        return shows ? 0 : PrCommandStateFlag_Disabled;
    });
    // The camera of a model's preview: the orbit camera, or one of the asset's, by its node's name or path.
    const PrCommandParam cameraParams[] = {
        {
            .name = "camera",
            .kind = PrCommandParamKind_String,
            .required = true,
            .description = "the name or the path of a camera's node of the asset, or empty for the orbit camera",
        },
    };
    PrCommandDesc camera{
        .id = "model.camera",
        .title = "Look Through Camera",
        .category = "Scribe",
        .description = "Shows the model of the current tab through one of its asset's cameras, or the orbit camera.",
        .params = cameraParams,
        .paramCount = 1,
        .run = [](void* c, const PrCommandArgs* args) {
            ViewTab* tab = shownTab(scribeOf(c));
            if (tab == nullptr || tab->model == nullptr || prModelViewModel(tab->model) == nullptr)
                return;
            prModelViewSetCamera(tab->model, cameraNamed(prModelViewModel(tab->model),
                prCommandArgsString(args, "camera", "")));
            tab->mode = ViewMode_Preview;
        },
        .state = [](void* c) -> PrCommandStateFlags {
            ViewTab* tab = shownTab(scribeOf(c));
            return tab != nullptr && tab->model != nullptr ? 0 : PrCommandStateFlag_Disabled;
        },
        .context = &s,
    };
    prCommandRegistryAdd(s.commands, &camera);
    // The lighting of a model's preview: each argument that is given replaces that part of the view's lighting.
    const PrCommandParam lightingParams[] = {
        {
            .name = "environment",
            .kind = PrCommandParamKind_String,
            .description = "the environment: \"asset\", \"sky\" or \"none\"",
        },
        {.name = "environmentStops", .kind = PrCommandParamKind_Number, .description = "the environment's scale, in stops"},
        {.name = "lightStops", .kind = PrCommandParamKind_Number, .description = "the asset's lights' scale, in stops"},
        {
            .name = "emission",
            .kind = PrCommandParamKind_String,
            .description = "the unit of the asset's emission: \"luminance\" or \"display\"",
        },
        {
            .name = "exposure",
            .kind = PrCommandParamKind_String,
            .description = "how the exposure is chosen: \"scene\", \"image\" or \"manual\"",
        },
        {.name = "ev100", .kind = PrCommandParamKind_Number, .description = "the manual exposure's EV100"},
        {.name = "compensation", .kind = PrCommandParamKind_Number, .description = "stops that brighten the image"},
    };
    PrCommandDesc lighting{
        .id = "model.lighting",
        .title = "Set Model Lighting",
        .category = "Scribe",
        .description = "Sets how the model of the current tab is lit and exposed.",
        .params = lightingParams,
        .paramCount = countOf(lightingParams),
        .run = [](void* c, const PrCommandArgs* args) {
            ViewTab* tab = shownTab(scribeOf(c));
            if (tab == nullptr || tab->model == nullptr || prModelViewModel(tab->model) == nullptr)
                return;
            PrModelViewLighting l = prModelViewLighting(tab->model);
            PrModelViewEnvironment environment = prModelViewEnvironmentFromName(prCommandArgsString(args, "environment", ""));
            if (environment != PrModelViewEnvironment_Count)
                l.environment = environment;
            PrRenderEmission emission = prRenderEmissionFromName(prCommandArgsString(args, "emission", ""));
            if (emission != PrRenderEmission_Count)
                l.emission = emission;
            PrModelViewExposure exposure = prModelViewExposureFromName(prCommandArgsString(args, "exposure", ""));
            if (exposure != PrModelViewExposure_Count)
                l.exposure = exposure;
            l.environmentStops = float(prCommandArgsNumber(args, "environmentStops", double(l.environmentStops)));
            l.lightStops = float(prCommandArgsNumber(args, "lightStops", double(l.lightStops)));
            l.ev100 = float(prCommandArgsNumber(args, "ev100", double(l.ev100)));
            l.compensation = float(prCommandArgsNumber(args, "compensation", double(l.compensation)));
            prModelViewSetLighting(tab->model, &l);
            tab->mode = ViewMode_Preview;
        },
        .state = [](void* c) -> PrCommandStateFlags {
            ViewTab* tab = shownTab(scribeOf(c));
            return tab != nullptr && tab->model != nullptr ? 0 : PrCommandStateFlag_Disabled;
        },
        .context = &s,
    };
    prCommandRegistryAdd(s.commands, &lighting);
    addCommand(s, "view.togglePreview", "Toggle Preview", "Cmd Shift V", [](void* c, const PrCommandArgs*) {
        ViewTab* tab = shownTab(scribeOf(c));
        if (tab == nullptr || (tab->page == nullptr && tab->model == nullptr) || tab->document == nullptr)
            return;
        tab->mode = tab->mode == ViewMode_Preview ? ViewMode_Source : ViewMode_Preview;
    }, [](void* c) -> PrCommandStateFlags {
        ViewTab* tab = shownTab(scribeOf(c));
        bool previews = tab != nullptr && (tab->page != nullptr || tab->model != nullptr) && tab->document != nullptr;
        return previews ? 0 : PrCommandStateFlag_Disabled;
    });
}

bool openProject(Scribe& s, PrStringView root) {
    PrFileInfo info{};
    if (!prFileInfo(root, &info) || info.kind != PrFileKind_Directory)
        return false;
    Workspace& w = *s.workspace;
    for (uint32_t i = 0; i < w.project.count(); ++i)
        addressNodeRelease(w.project[i].held);
    w.project.clear();
    w.projectSelected = kProjectNone;
    w.projectCursor = {};
    PrRawString absolute = prFilePathAbsolute(root, "");
    w.root = String::adopt(absolute);
    ProjectNode folder;
    folder.name = prFilePathName(w.root);
    folder.path = w.root;
    folder.folder = true;
    addNode(w, kProjectNone, static_cast<ProjectNode&&>(folder));
    setOpen(w, 0, true);
    if (prKitDockPanelLeaf(w.dock, kPanelProject) == kPrKitDockNodeNone)
        togglePanel(w, kPanelProject, PrKitDockEdge_Left);
    if (!s.statePath.isEmpty()) {
        prFileCreateDirectory(prFilePathParent(s.statePath));
        prFileWrite(s.statePath, prStringBytes(w.root));
    }
    return true;
}

bool openPath(Scribe& s, PrStringView path) {
    PrFileInfo info{};
    if (path.isEmpty() || !prFileInfo(path, &info))
        return false;
    if (info.kind == PrFileKind_Directory)
        return openProject(s, path);
    // A file opens in the project that holds it, or else in the project of its folder.
    String absolute = String::adopt(prFilePathAbsolute(path, ""));
    Workspace& w = *s.workspace;
    bool held = !w.root.isEmpty() && prStringStartsWith(absolute, w.root) && absolute.count() > w.root.count() &&
        absolute.data()[w.root.count()] == '/';
    if (!held && !openProject(s, prFilePathParent(absolute)))
        return false;
    String url = String::adopt(prUrlFromFilePath(absolute));
    return openAddress(s, url) == PrStatus_Ok;
}

String lastProject(const Scribe& s) {
    if (s.statePath.isEmpty())
        return String();
    PrBlob* blob = prFileOpen(s.statePath);
    if (blob == nullptr)
        return String();
    PrByteView bytes = prBlobBytes(blob);
    String root(prStringTrim(PrStringView{reinterpret_cast<const char*>(bytes.data), bytes.size}));
    prBlobRelease(blob);
    PrFileInfo info{};
    if (root.isEmpty() || !prFileInfo(root, &info) || info.kind != PrFileKind_Directory)
        return String();
    return root;
}

void setProjectOpen(Scribe& s, uint32_t index, bool open) {
    setOpen(*s.workspace, index, open);
}

PrStatus openAddress(Scribe& s, PrStringView text, OpenMode mode) {
    // The history sees the place that the user leaves, which may have changed since the last build, and then the one
    // that opens, before another opens in the same round.
    visit(*s.workspace);
    PrStatus status = openTab(s, text, mode);
    visit(*s.workspace);
    return status;
}

bool stepHistory(Scribe& s, int32_t delta) {
    Workspace& w = *s.workspace;
    PrStringView stepped = prKitHistoryStep(w.history, delta);
    if (stepped.data == nullptr)
        return false;
    String place = stepped;
    openTab(s, place, OpenMode_Preview);
    // An address that names a resource shows it with nothing selected, as it showed when the history saw it.
    ViewTab* tab = shownTab(s);
    if (tab != nullptr && tab->address == place)
        tab->selection = String();
    visit(w);
    return true;
}

void closeWorkspace(Scribe& s) {
    if (s.workspace == nullptr)
        return;
    Workspace& w = *s.workspace;
    while (!w.tabs.isEmpty())
        closeTab(s, w.tabs[0]->id);
    for (uint32_t i = 0; i < w.project.count(); ++i)
        addressNodeRelease(w.project[i].held);
    w.project.clear();
    // The fetcher goes on its loop, once the requests that wait are refused.
    prNetPostBlocking(w.net, [](void* context) {
        Workspace& w = *static_cast<Workspace*>(context);
        for (PrFetchInterceptRequest* request : w.requests)
            prFetchInterceptRequestFail(request, PrNetError_Io);
        w.requests.clear();
        prFetchNetworkDestroy(w.fetcher);
    }, &w);
    prNetDestroy(w.net);
    prMutexReset(&w.mutex);
    addressResolverDestroy(w.resolver);
    for (AddressHandler* handler : w.handlers)
        addressHandlerDestroy(handler);
    prKitDockDestroy(w.dock);
    prKitHistoryDestroy(w.history);
    prModelHostDestroy(w.models);
    prTaskPoolDestroy(w.pool);
    prImageCodecSetDestroy(w.images);
    prCompressCodecSetDestroy(w.compress);
    destroy(&w);
    s.workspace = nullptr;
}

void buildWorkspace(Scribe& s, PrLayoutNode parent) {
    Workspace& w = *s.workspace;
    follow(s);
    answerPreviews(s);
    // The editor's own documents, as a comparison or a document of no file, have a panel while no tab is open.
    bool editorPanel = prKitDockPanelLeaf(w.dock, kPanelEditor) != kPrKitDockNodeNone;
    if (!editorPanel && w.tabs.isEmpty() && prTextEditorShown(s.editor) != nullptr)
        prKitDockAddPanel(w.dock, prKitDockPrimary(w.dock), kPanelEditor);
    PrKitBoxStyle box{.overflow = PrKitOverflow_Clip, .place = {.width = prKitPercent(100), .flex = PrKitFlex_Fill}};
    PrLayoutNode area = prKitBox(s.kit, parent, {}, box);
    prKitDockBuild(s.kit, area, {.dock = w.dock, .describe = describePanel, .menu = tabMenu, .context = &s});
    // The tabs that the user closed leave with what they held.
    for (uint32_t i = 0; i < prKitDockRemovedCount(w.dock); ++i) {
        PrKitDockPanelId removed = prKitDockRemoved(w.dock, i);
        if (removed >= kFirstTab)
            closeTab(s, removed);
    }
    // A tab whose pane newly shows, as by a click on its tab, becomes current, and so does any shown tab once the
    // current one has gone.
    Array<PrKitDockPanelId> shown;
    for (uint32_t i = 0; i < prKitDockPaneCount(w.dock); ++i) {
        PrKitDockPanelId panel = prKitDockPane(w.dock, i).panel;
        if (panel < kFirstTab)
            continue;
        shown.push(panel);
        if (w.shown.find(panel) == kArrayMissing || tabWithId(w, w.current) == nullptr)
            w.current = panel;
    }
    w.shown = static_cast<Array<PrKitDockPanelId>&&>(shown);
    for (uint32_t i = 0; i < prKitDockPaneCount(w.dock); ++i) {
        PrKitDockPane pane = prKitDockPane(w.dock, i);
        if (pane.panel == kPanelProject) {
            buildProject(s, pane.content);
        } else if (pane.panel == kPanelOutline) {
            buildOutline(s, pane.content);
        } else if (pane.panel == kPanelEditor) {
            PrVec2 extent{};
            prUiExtent(s.ui, pane.content, PrUiAxisFlag_Both, &extent);
            prTextEditorFrame(s.editor, pane.content, extent);
        } else if (ViewTab* tab = tabWithId(w, pane.panel)) {
            buildTabPane(s, pane.content, *tab);
        }
    }
    // After the views, whose selections the user may have changed in this round.
    visit(w);
}

String displayAddress(const Scribe& s, PrStringView address) {
    const Workspace& w = *s.workspace;
    if (w.root.isEmpty())
        return String(address);
    String root = String::adopt(prUrlFromFilePath(w.root));
    root.append('/');
    if (!prStringStartsWith(address, root))
        return String(address);
    // A file's URL is percent-encoded, and its path in the project is shown as the project panel names it.
    PrStringView relative = prStringFrom(address, root.count());
    Array<char> decoded;
    decoded.resize(uint32_t(relative.size));
    size_t size = prUrlPercentDecode(relative, decoded.data(), decoded.count());
    return String(PrStringView{decoded.data(), size});
}

String pageUrl(PrStringView text) {
    Address address;
    if (addressParse(text, &address) != PrStatus_Ok)
        return String(text);
    String url = address.url;
    // A step is a path in its container, whose bytes the URL percent-encodes where a path must.
    for (const String& step : address.steps) {
        url.append('/');
        for (uint32_t at = 0; at < step.count(); ++at) {
            uint8_t c = uint8_t(step.data()[at]);
            PrStringView marks = "-._~/!$&'()*+,;=:@";
            bool plain =
                prAsciiIsLetter(char(c)) || prAsciiIsDigit(char(c)) || prStringFindByte(marks, char(c)) != marks.size;
            if (plain && c != 0)
                url.append(char(c));
            else
                formatTo(url, "%{:02X}", c);
        }
    }
    return url;
}

String pageAddress(PrStringView url) {
    PrRawString path = prRawStringNull();
    if (!prUrlToFilePath(url, &path))
        return String();
    String file(prRawStringView(&path));
    prRawStringReset(&path);
    PrFileInfo info{};
    if (prFileInfo(file, &info))
        return info.kind == PrFileKind_Directory ? String() : String(url);
    // The longest part of the path that is a file holds the rest, as the steps of a nested resource (pageUrl).
    for (size_t slash = prStringFindLastByte(file, '/'); slash != 0 && slash < file.count();) {
        PrStringView head = prStringHead(file, slash);
        if (prFileInfo(head, &info) && info.kind != PrFileKind_Directory) {
            String outer = String::adopt(prUrlFromFilePath(head));
            return addressWithStep(outer, prStringFrom(file, slash + 1));
        }
        slash = prStringFindLastByte(head, '/');
        if (slash == head.size)
            break;
    }
    return String();
}

ViewTab* PR_NULLABLE shownTab(Scribe& s) {
    if (s.workspace == nullptr)
        return nullptr;
    return tabWithId(*s.workspace, s.workspace->current);
}

} // namespace scribe
