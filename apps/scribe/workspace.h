// Scribe's workspace: the project panel over a folder, the tabs of the resources that it opens by their addresses, the
// view of each tab, and the outline panel of the tab that shows.

#pragma once

#include "pr/array.h"
#include "pr/compress.h"
#include "pr/fetch_intercept.h"
#include "pr/gltf_view.h"
#include "pr/image.h"
#include "pr/image_view.h"
#include "pr/kit_collection.h"
#include "pr/kit_dock.h"
#include "pr/kit_history.h"
#include "pr/mime_sniff.h"
#include "pr/model_host.h"
#include "pr/model_view.h"
#include "pr/mutex.h"
#include "pr/net.h"
#include "pr/page_view.h"
#include "pr/string.h"
#include "pr/task.h"
#include "pr/text_editor.h"
#include "pr/texture.h"
#include "pr/usd_view.h"

#include "apps/scribe/address.h"
#include "apps/scribe/source_link.h"

PR_ASSUME_NONNULL_BEGIN

namespace scribe {

using namespace pr;

struct Scribe;

// The index of no node of the project's tree.
constexpr uint32_t kProjectNone = 0xffffffffu;

// The panels of the workspace's dock: the project, the outline, the editor's own documents, and the tabs of resources,
// whose ids start at kFirstTab.
constexpr PrKitDockPanelId kPanelProject = 1;
constexpr PrKitDockPanelId kPanelOutline = 2;
constexpr PrKitDockPanelId kPanelEditor = 3;
constexpr PrKitDockPanelId kFirstTab = 16;

// A row of the project's tree: a folder or a file on disk, or a folder or a file of a container.
struct ProjectNode {
    String name;
    String path;                    // the file or folder on disk, or empty inside a container
    String address;                 // a file's address, or empty for a folder
    uint32_t parent = kProjectNone;
    Array<uint32_t> children;
    bool folder = false;
    bool container = false;         // a file that expands into the resources that it holds
    bool open = false;
    bool listed = false;                       // the children are known
    AddressNode* PR_NULLABLE held = nullptr;   // the container, open while it shows its children
    String failure;                            // why a container did not expand
};

// What a tab shows its resource as.
enum {
    ViewMode_Source,                // the text, in the editor
    ViewMode_Preview,               // a rendering of Markdown or HTML, or a glTF asset or a USD stage drawn in 3D
    ViewMode_Structure,             // what a USD stage composes, or the items of a glTF asset
    ViewMode_Image,
    ViewMode_Info,                  // what the classifier found, and what a format's module reports
};
typedef uint8_t ViewMode;

// An item of the outline: a location of the tab's resource, as a step.
struct OutlineItem {
    String label;
    String step;
    uint32_t depth = 0;
    uint32_t value = 0;             // the prim of a stage, or the line of a text, that the step names
};

// A tab: a resource, by its address, which follows its outermost file. Its id is its panel in the dock.
struct ViewTab {
    uint64_t id = 0;
    String address;
    String title;
    String tooltip;                             // where the resource is: in the project, and its whole address
    AddressNode* PR_NULLABLE node = nullptr;    // held while the address resolves

    // While the address does not resolve, the resource of the steps before the one that failed, held so that its file's
    // changes are followed. Null if the outermost resource is what failed.
    AddressNode* PR_NULLABLE partial = nullptr;

    PrStatus status = PrStatus_Ok;              // of the last resolution
    uint32_t failedStep = kAddressStepNone;
    PrMimeFormat format = PrMimeFormat_None;
    ViewMode mode = ViewMode_Info;
    String path;                                // a file on disk, whose document the editor owns, or empty
    PrTextEditorDocument* PR_NULLABLE document = nullptr;
    PrImageView* PR_NULLABLE image = nullptr;
    PrUsdView* PR_NULLABLE usd = nullptr;
    PrGltfView* PR_NULLABLE gltf = nullptr;     // the structure of a glTF asset
    PrPageView* PR_NULLABLE page = nullptr;     // the preview of Markdown or HTML
    PrModelView* PR_NULLABLE model = nullptr;   // the preview of a glTF asset or a USD stage
    uint32_t property = 0xffffffffu;            // the property of the stage that the structure view chose
    String selection;                           // a location under the resource, as a step, or empty
    SourceLinks links;                          // between the text and the selection, for a .gltf or .usda resource
    Array<OutlineItem> outline;
    PrKitSelection outlineCursor{};             // the outline's cursor, which is the tab's own
};

// The project, the tabs and the services that resolve their addresses.
struct Workspace {
    PrCompressCodecSet* PR_NULLABLE compress = nullptr;
    PrImageCodecSet* PR_NULLABLE images = nullptr;
    PrTaskPool* PR_NULLABLE pool = nullptr;          // loads the meshes and images of the models that previews draw
    PrModelHost* PR_NULLABLE models = nullptr;       // the formats of the models' textures, and their options
    Array<AddressHandler*> handlers;
    AddressResolver* PR_NULLABLE resolver = nullptr;

    // The fetcher of the previews, whose requests arrive on the network's loop, wait under the mutex, and are answered
    // through the resolver in the next build.
    PrNet* PR_NULLABLE net = nullptr;
    PrFetchNetwork* PR_NULLABLE fetcher = nullptr;
    PrMutex mutex{};
    Array<PrFetchInterceptRequest*> requests;
    double polled = 0;              // seconds on the monotonic clock of the last poll

    String root;                    // the project's folder, or empty if no project is open
    Array<ProjectNode> project;     // node 0 is the folder, if a project is open

    // The project's visible rows in display order, as the round's build flattened them for the tree, which declares its
    // rows after the round's layout: each row's node, and its place in the tree. The outline's are the current tab's.
    Array<uint32_t> projectRows;
    Array<PrKitTreeItem> projectItems;
    Array<PrKitTreeItem> outlineItems;

    PrKitSelection projectCursor{};
    uint32_t projectSelected = kProjectNone;
    bool projectReveals = false;    // the project's next build scrolls its cursor's row into view

    // The panels in a dock: the project and the outline at its sides, and the tabs, and the editor's own documents, in
    // its primary leaf, which the user can rearrange.
    PrKitDock* PR_NULLABLE dock = nullptr;
    Array<ViewTab*> tabs;           // each in storage of its own, which stays in place through a round
    uint64_t nextTab = kFirstTab;
    uint64_t current = 0;           // the tab that the outline and the status line follow: the last that opened
    Array<PrKitDockPanelId> shown;  // the tabs whose panes the last build declared
    bool focusView = false;         // the current tab's view takes focus in the next build

    // The places that the window showed, each a tab's address with its selection as a last step, which Back and Forward
    // walk. The window has one history across its tabs, as a tab's identity is its resource. The tab and the selection
    // that the history last saw tell a new place from the one it holds without composing an address each build.
    PrKitHistory* PR_NULLABLE history = nullptr;
    uint64_t visitedTab = 0;
    String visitedSelection;

    // What a context menu's commands act on while the menu creates its items: a panel's tab, or a node of the project.
    // Outside a menu, they act on the current tab.
    PrKitDockPanelId menuTab = 0;
    uint32_t menuNode = kProjectNone;
};

// Makes the workspace with no project open, and adds its commands: opening a folder or a file through the system's
// dialog, and showing or hiding each panel. openEditor calls it.
void openWorkspace(Scribe& s);

// Opens the folder at `root` as the project, and remembers it as the project that a launch without a path opens
// (Scribe::statePath). The tabs stay open. Returns false if `root` is not a folder.
bool openProject(Scribe& s, PrStringView root);

// Opens what `path` names on disk: a folder as the project, or a file in the view that its format calls for, in the
// project of its folder unless the project holds it. Returns false if nothing is at `path`.
bool openPath(Scribe& s, PrStringView path);

// Returns the project that the last run opened, as Scribe::statePath remembers it, or empty if there is none or it is
// no longer a folder.
String lastProject(const Scribe& s);

// Opens or closes the node of the project at `index`, as its disclosure does. A folder lists its files, and a container
// its resources, which it holds open while it shows them.
void setProjectOpen(Scribe& s, uint32_t index, bool open);

// How a tab opens: kept until the user closes it, or as the preview of its leaf, which the next preview takes over
// until the user keeps it, by a double click on its tab, an edit, or Keep Tab Open.
enum {
    OpenMode_Keep,
    OpenMode_Preview,
    OpenMode_Side,                  // kept, in a leaf of its own beside the current tab's
};
typedef uint8_t OpenMode;

// Opens the resource that the address `text` names in a tab, or shows the tab that shows it, which `mode` then keeps if
// it keeps tabs. An address that ends in a location opens its resource and selects the location. Returns the status of
// resolving the address, and opens a tab that shows what is missing if a step does not resolve.
PrStatus openAddress(Scribe& s, PrStringView text, OpenMode mode = OpenMode_Keep);

// Steps `delta` places through the history, back if negative, and shows the place: its tab, which opens as the preview
// of its leaf if no tab shows it, with the place's selection or none. Returns false, and shows nothing new, if the
// history has no place there.
bool stepHistory(Scribe& s, int32_t delta);

// Frees the workspace: its tabs, its project, the resolver and its handlers.
void closeWorkspace(Scribe& s);

// Declares the workspace's dock under `parent`, a flex column, in the room that its other children leave: the project
// panel, the tabs and their views, and the outline. Follows the changes of the files that the tabs show, and closes the
// tabs that the user closed, which the round's end frees.
void buildWorkspace(Scribe& s, PrLayoutNode parent);

// Returns `address` as a person reads it: relative to the project's folder, and decoded, for a resource in the project,
// and as it is otherwise.
String displayAddress(const Scribe& s, PrStringView address);

// Returns the URL that a preview loads the document at `address` at, from which its relative URLs resolve: the file's
// own URL, or for a nested resource the URL of its outermost file with the steps after it as a path.
String pageUrl(PrStringView text);

// Returns the address of what a preview's URL names (pageUrl), or empty if it names nothing in a file on disk.
String pageAddress(PrStringView url);

// The current tab, which the outline and the status line follow, or null if there is none.
ViewTab* PR_NULLABLE shownTab(Scribe& s);

} // namespace scribe

PR_ASSUME_NONNULL_END
